//
// Created by victor on 10/03/26.
//

/* The client-api TRANSPORTS' suite (the plan's Tasks 5 + 6 / the client-api
   spec §4): the FULL stack over real sockets — the raw client double writes
   stream_frame_encode(ca_wire_encode(...)) frames to the transport's socket
   (unix: a temp-path AF_UNIX file; tcp: loopback host:port with the bcrypt
   api-key auth); the transport's framer extracts, its bridge dispatches
   onto the handlers server, and the responses ride back encoded + framed.
   Every assertion reads a DECODED frame (the wire's bytes form), scanned by
   TYPE + REQ_ID — the handlers' suite's discipline, now over bytes on a
   socket instead of an in-proc recording vector. The TestClientApiUnix
   suite (Task 5's) is unchanged; TestClientApiTcp (Task 6) rides the SAME
   fixture machinery parameterized by transport. */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "../src/ClientApi/Unix/unix_transport.h"
#include "../src/ClientApi/Unix/unix_connection.h"
#include "../src/ClientApi/Tcp/tcp_transport.h"
#include "../src/ClientApi/Tcp/tcp_connection.h"
#include "../src/ClientApi/client_api_wire.h"
#include "../src/ClientApi/handlers.h"
#include "../src/Network/stream_framer.h"
#include "../src/Frame/frame.h"
#include "../src/Frame/model.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Streams/loop_thread.h"
#include "../src/Platform/platform.h"
#include "../src/Util/allocator.h"
#include "../src/Util/bcrypt.h"
#include "../src/Util/json.h"
}

#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include <cerrno>
#include <cstdlib>
#include <unistd.h>

static frame_config_t test_config(void) {
  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.model_base_url = NULL;
  cfg.model_api_key = NULL;
  cfg.model_name = "unused";
  cfg.max_depth = 4;
  return cfg;
}

/* --- the in-proc fixture: pool + store + loop + server + TRANSPORT ---------- */

#define FIXTURE_API_KEY "the-demo-api-key"

typedef struct {
  scheduler_pool_t* pool;
  streams_loop_thread_t* loop;
  wave_database_root_t* db;
  ca_session_server_t* server;
  unix_transport_t* transport;   /* set = the unix variant (Task 5) */
  tcp_transport_t* tcp_transport;   /* set = the TCP variant (Task 6) */
  platform_address_t tcp_addr;   /* the TCP transport's bound address */
  frame_config_t cfg;
  char socket_path[128];
  char dir_path[120];
} fixture_t;

/* 0 = set up; nonzero = which step failed. cfg's model_base_url stays NULL:
   a frame WITHOUT the shared backend builds a NULL default backend and its
   engine fails fast, deterministic and network-free. escalation_mode rides
   the fixture's frame-cfg template into every api-created frame (the
   escalation slice's ask tests carry FRAME_ESCALATION_PLAN_ASK_ACT). */
static int fixture_setup_common(fixture_t* fx, model_backend_t* shared_backend,
                                unsigned escalation_mode) {
  memset(fx, 0, sizeof(*fx));
  fx->cfg = test_config();
  fx->cfg.escalation_mode = escalation_mode;
  fx->pool = scheduler_pool_create(2);
  if (fx->pool == NULL) return -1;
  scheduler_pool_start(fx->pool);
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;         /* in-memory */
  sc.store_pool = fx->pool;   /* a pooled frame REQUIRES a pooled store */
  fx->db = wave_db_open_config(&sc);
  if (fx->db == NULL) return -2;
  fx->loop = streams_loop_create();
  if (fx->loop == NULL) return -3;
  fx->server = ca_session_server_create(fx->db, fx->pool, fx->loop, &fx->cfg,
                                        shared_backend);
  if (fx->server == NULL) return -4;

  char tmpl[] = "/tmp/sa-ca5-XXXXXX";
  char* dir = mkdtemp(tmpl);
  if (dir == NULL) return -5;
  snprintf(fx->dir_path, sizeof(fx->dir_path), "%s", dir);
  snprintf(fx->socket_path, sizeof(fx->socket_path), "%s/serve.sock", dir);
  return 0;
}

/* The UNIX variant: the socket file's permission is the auth (no key). */
static int fixture_setup(fixture_t* fx, model_backend_t* shared_backend) {
  int rc = fixture_setup_common(fx, shared_backend, FRAME_ESCALATION_FREE);
  if (rc != 0) return rc;

  fx->transport = unix_transport_create(fx->pool, fx->server,
                                        fx->socket_path);
  if (fx->transport == NULL) return -6;
  unix_transport_start(fx->transport);
  return 0;
}

/* The TCP variant: the loopback listener with a bcrypt-hashed api key (cost
   4 — the exchange runs 2^4 rounds, milliseconds, and the WRONG-key
   refusal's latency budget in the tests stays honest). The transport binds
   port 0 and reports the bound address back (create's out_addr param). */
static int fixture_setup_tcp(fixture_t* fx, model_backend_t* shared_backend) {
  int rc = fixture_setup_common(fx, shared_backend, FRAME_ESCALATION_FREE);
  if (rc != 0) return rc;

  char hash[64];
  if (bcrypt_generate(FIXTURE_API_KEY, 4, hash, sizeof(hash)) != 0) return -7;
  fx->tcp_transport = tcp_transport_create(fx->pool, fx->server,
                                           "127.0.0.1", 0, hash, &fx->tcp_addr);
  if (fx->tcp_transport == NULL) return -6;
  tcp_transport_start(fx->tcp_transport);
  return 0;
}

/* The documented teardown order (handlers.h's note, the transports' shape):
   the transport destroys FIRST — it closes every connection (each teardown
   calls ca_session_conn_closed); the server destroys next (drains its
   closures + store round trips, unwatchs, owns the frames' teardown) while
   the pool still runs; then the pool stops, the root closes, the pool dies,
   and the loop dies last. The test client's socket is closed by the test
   body BEFORE this runs (the honest disconnect path — the hangup dispatch). */
static void fixture_teardown(fixture_t* fx) {
  if (fx->transport != NULL) {
    unix_transport_destroy(fx->transport);
  }
  if (fx->tcp_transport != NULL) {
    tcp_transport_destroy(fx->tcp_transport);
  }
  ca_session_server_destroy(fx->server);
  scheduler_pool_stop(fx->pool);
  wave_db_close(fx->db);
  scheduler_pool_destroy(fx->pool);
  streams_loop_destroy(fx->loop);
  unlink(fx->socket_path);   /* the unix transport unlinked it already;
                                idempotent (and a TCP teardown is a no-op) */
  rmdir(fx->dir_path);
}

/* --- the raw client double (transport-agnostic: reads/sends/recording run
       the test thread for BOTH connect forms below) ------------------------- */

typedef struct client_frame_t {
  uint64_t type;
  uint64_t rid;   /* the decode's req_id out-param */
  std::vector<uint8_t> bytes;
} client_frame_t;

typedef struct test_client_t {
  platform_socket_t* sock;
  stream_framer_t* framer;
  std::vector<client_frame_t> frames;   /* the test thread's own recording —
                                           reads and sends both run the test
                                           thread; no lock */
  int dead;                             /* set on HANGUP (a 0-length read) */
} test_client_t;

static int client_connect(test_client_t* c, const char* path) {
  c->sock = NULL;
  c->framer = NULL;
  c->frames.clear();
  c->dead = 0;
  c->sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
  if (c->sock == NULL) return -1;
  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_LOCAL;
  strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);
  if (platform_socket_connect(c->sock, &addr) != 0) {
    platform_socket_destroy(c->sock);
    c->sock = NULL;
    return -1;
  }
  /* the client drains its socket on ITS schedule (the read loops below) —
     nonblocking keeps a quiet peer from pinning the test thread */
  platform_socket_set_nonblocking(c->sock);
  c->framer = stream_framer_create();
  return 0;
}

/* The TCP variant of the same double: a loopback TCP connection. */
static int client_connect_tcp(test_client_t* c, const char* host,
                              uint16_t port) {
  c->sock = NULL;
  c->framer = NULL;
  c->frames.clear();
  c->dead = 0;
  c->sock = platform_socket_create(PLATFORM_AF_INET, 1);
  if (c->sock == NULL) return -1;
  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  if (platform_address_parse(&addr, host, port) != 0) {
    platform_socket_destroy(c->sock);
    c->sock = NULL;
    return -1;
  }
  if (platform_socket_connect(c->sock, &addr) != 0) {
    platform_socket_destroy(c->sock);
    c->sock = NULL;
    return -1;
  }
  platform_socket_set_nonblocking(c->sock);
  c->framer = stream_framer_create();
  return 0;
}

/* The bound port the transport reports back (port 0 rides out as the real
   port in create's out_addr; the family may be the dual-stack v6). */
static uint16_t fixture_tcp_port(const fixture_t* fx) {
  return (fx->tcp_addr.family == PLATFORM_AF_INET6) ? fx->tcp_addr.inet6.port
                                                    : fx->tcp_addr.inet.port;
}

static void client_close(test_client_t* c) {
  if (c == NULL) return;
  if (c->framer != NULL) {
    stream_framer_destroy(c->framer);
    c->framer = NULL;
  }
  if (c->sock != NULL) {
    platform_socket_destroy(c->sock);
    c->sock = NULL;
  }
}

/* Encode + frame + SEND ALL (a short unix-socket send drains fully long
   before our frames approach any buffer's size — refused loud if not). */
static int client_send_frame(test_client_t* c, uint64_t type, void* payload) {
  uint8_t* raw = NULL;
  size_t len = 0;
  if (ca_wire_encode(type, payload, &raw, &len) != 0) {
    return -1;
  }
  size_t framed_len;
  uint8_t* framed = stream_frame_encode(raw, len, &framed_len);
  free(raw);
  if (framed == NULL) {
    return -1;
  }
  const uint8_t* p = framed;
  size_t off = 0;
  while (off < framed_len) {
    ssize_t n = platform_socket_send(c->sock, p + off, framed_len - off);
    if (n <= 0) {
      free(framed);
      return -1;
    }
    off += (size_t)n;
  }
  free(framed);
  return 0;
}

/* Drain whatever bytes the peer has; extract every COMPLETE frame into the
   recording (raw bytes kept; decoded on demand by the scans). Returns 1 = at
   least one frame recorded, 0 = quiet, -1 = hangup (or garbage: the server
   sent something the wire refuses — refuses loud, dead). */
static int client_pump(test_client_t* c) {
  if (c == NULL || c->sock == NULL) return -1;
  for (;;) {
    uint8_t buf[16384];
    ssize_t n = platform_socket_recv(c->sock, buf, sizeof(buf));
    if (n == 0) {
      c->dead = 1;
      return -1;
    }
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        c->dead = 1;
        return -1;
      }
      return 0;   /* quiet */
    }
    size_t consumed = (size_t)n;
    if (stream_framer_feed(c->framer, buf, consumed) != 0) {
      c->dead = 1;
      return -1;
    }
    uint8_t* fdata;
    size_t flen;
    int got = 0;
    while ((fdata = stream_framer_next(c->framer, &flen)) != NULL) {
      uint64_t type = 0, rid = 0;
      void* payload = NULL;
      uint8_t status = 0;
      /* record the RAW bytes + the decode's routing keys; the payload is
         consumed immediately (the recording stores bytes, not payloads) */
      client_frame_t f;
      if (ca_wire_decode_bytes(fdata, flen, &type, &payload, &rid, &status)
          != 0) {
        free(fdata);
        c->dead = 1;
        return -1;   /* the server itself sent garbage — refuse loud */
      }
      ca_wire_payload_destroy(type, payload);
      (void)status;
      f.type = type;
      f.rid = rid;
      f.bytes.assign(fdata, fdata + flen);
      c->frames.push_back(f);
      free(fdata);
      got = 1;
    }
    if (got) return 1;
  }
}

/* Pump until a NEW recorded frame matches the filter (type + req_id). The
   read loop is the test's ONLY reader — frames land in order, so matching by
   req_id + expected type races nothing. Returns the frame's INDEX. */
static int client_wait_frame(test_client_t* c, uint64_t want_type,
                             uint64_t want_req_id, size_t* out_index,
                             int rounds) {
  for (int i = 0; i < rounds; i++) {
    client_pump(c);
    for (size_t j = 0; j < c->frames.size(); j++) {
      if (c->frames[j].type == want_type && c->frames[j].rid == want_req_id) {
        *out_index = j;
        return 1;
      }
    }
    if (c->dead) return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return -1;
}

/* Decode recorded frame `idx` (a local COPY of its bytes). */
static bool client_decode(test_client_t* c, size_t idx, uint64_t* type,
                          void** payload, uint64_t* req_id, uint8_t* status) {
  if (idx >= c->frames.size()) return false;
  std::vector<uint8_t> bytes = c->frames[idx].bytes;
  return ca_wire_decode_bytes(bytes.data(), bytes.size(), type, payload,
                              req_id, status) == 0;
}

/* --- the request builders (stack structs whose text fields are heap —
       destroyed through the wire once encoded) ------------------------------- */

static ca_prompt_request_t* prompt_req_heap(uint64_t req_id, const char* sid,
                                            const char* text) {
  ca_prompt_request_t* req =
      (ca_prompt_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  req->sid = (sid != NULL) ? strdup(sid) : NULL;
  req->text = strdup(text);
  return req;
}

static ca_events_request_t* events_req_heap(uint64_t req_id, const char* sid,
                                            uint8_t op, uint64_t from_seq) {
  ca_events_request_t* req =
      (ca_events_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  req->sid = strdup(sid);
  req->op = op;
  req->from_seq = from_seq;
  return req;
}

static ca_interrupt_request_t* interrupt_req_heap(uint64_t req_id,
                                                  const char* sid) {
  ca_interrupt_request_t* req =
      (ca_interrupt_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  req->sid = strdup(sid);
  return req;
}

static ca_sessions_request_t* sessions_req_heap(uint64_t req_id) {
  ca_sessions_request_t* req =
      (ca_sessions_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  return req;
}

static ca_auth_request_t* auth_req_heap(uint64_t req_id, const char* key) {
  ca_auth_request_t* req =
      (ca_auth_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  req->api_key = strdup(key);
  req->key_len = strlen(key);   /* the destroy's scrub rides the length */
  return req;
}

/* The TCP suite's auth exchange (the tcp_connection.c shape): send the AUTH
   pair, wait the response, return its status (0 = authenticated, 1 = bad
   key; -1 = the response never arrived or would not decode). */
static int client_authenticate(test_client_t* c, uint64_t req_id,
                               const char* key) {
  ca_auth_request_t* req = auth_req_heap(req_id, key);
  if (client_send_frame(c, CA_AUTH_REQUEST, req) != 0) {
    ca_wire_payload_destroy(CA_AUTH_REQUEST, req);
    return -1;
  }
  ca_wire_payload_destroy(CA_AUTH_REQUEST, req);

  size_t idx = 0;
  if (client_wait_frame(c, CA_AUTH_RESPONSE, req_id, &idx, 600) != 1) {
    return -1;
  }
  uint64_t t = 0, rid = 0;
  void* p = NULL;
  uint8_t st = 0;
  if (!client_decode(c, idx, &t, &p, &rid, &st)) return -1;
  ca_auth_response_t* res = (ca_auth_response_t*)p;
  int status = res->status;
  ca_wire_payload_destroy(CA_AUTH_RESPONSE, p);
  return status;
}

/* Pump until the peer closes (recv 0 / error). The graceful close's pin:
   the refused connection's socket EOFs shortly after its final frame. */
static bool client_wait_dead(test_client_t* c, int rounds) {
  for (int i = 0; i < rounds; i++) {
    client_pump(c);
    if (c->dead) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

/* A no-sid prompt's create+start over the socket; waits the response and
   returns the sid ("" on failure — the caller ASSERTs the frame first). */
static std::string prompt_create_sid(test_client_t* c, uint64_t req_id,
                                     const char* goal) {
  ca_prompt_request_t* req =
      prompt_req_heap(req_id, NULL, goal);   /* NULL sid = create+start */
  EXPECT_EQ(client_send_frame(c, CA_PROMPT_REQUEST, req), 0);
  ca_wire_payload_destroy(CA_PROMPT_REQUEST, req);

  size_t idx = 0;
  EXPECT_EQ(client_wait_frame(c, CA_PROMPT_RESPONSE, req_id, &idx, 600), 1)
      << "the create's response frame on the socket";
  uint64_t type = 0, rid = 0;
  void* payload = NULL;
  uint8_t status = 0;
  EXPECT_TRUE(client_decode(c, idx, &type, &payload, &rid, &status));
  ca_prompt_response_t* res = (ca_prompt_response_t*)payload;
  EXPECT_EQ(rid, req_id);
  EXPECT_EQ(res->status, 0u);
  EXPECT_NE(res->sid, nullptr);
  std::string sid;
  if (res->sid != NULL) sid = res->sid;
  ca_wire_payload_destroy(CA_PROMPT_RESPONSE, payload);
  return sid;
}

/* --- the events helpers (the recorded raw frames, the handlers' shapes) ----- */

struct chan_ev_t {
  uint64_t seq;              /* 0 = a transition marker, not a record */
  std::string record_json;   /* "" for the marker */
  std::string steer_content; /* the msg.append user content, else "" */
};

static std::vector<chan_ev_t> client_channel(test_client_t* c,
                                             uint64_t want_req_id) {
  std::vector<chan_ev_t> out;
  std::vector<std::vector<uint8_t>> candidates;
  for (const client_frame_t& f : c->frames) {
    if (f.type == (uint64_t)CA_EVENTS_RESPONSE) {
      candidates.push_back(f.bytes);
    }
  }
  for (const std::vector<uint8_t>& bytes : candidates) {
    uint64_t t = 0, rid = 0;
    void* p = NULL;
    uint8_t st = 0;
    if (ca_wire_decode_bytes(bytes.data(), bytes.size(), &t, &p, &rid, &st)
        != 0) {
      continue;
    }
    ca_events_response_t* ev = (ca_events_response_t*)p;
    if (rid == want_req_id) {
      chan_ev_t e;
      e.seq = ev->seq;
      if (ev->record_json != NULL) {
        e.record_json = ev->record_json;
        json_value_t* rec =
            json_parse(ev->record_json, strlen(ev->record_json), NULL);
        if (rec != NULL) {
          json_value_t* t_v = json_get(rec, "type");
          if (t_v != NULL && json_type(t_v) == JSON_STRING &&
              strcmp(json_as_string(t_v), "msg.append") == 0) {
            json_value_t* rp = json_get(rec, "payload");
            json_value_t* content =
                (rp != NULL) ? json_get(rp, "content") : NULL;
            if (content != NULL && json_type(content) == JSON_STRING) {
              e.steer_content = json_as_string(content);
            }
          }
          json_value_destroy(rec);
        }
      }
      out.push_back(e);
    }
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
  }
  return out;
}

/* The marker's op echo needs the decoded status — scan the recorded raws. */
static bool chan_marker_op_seen(test_client_t* c, uint64_t want_req_id,
                                uint8_t want_op) {
  for (size_t j = 0; j < c->frames.size(); j++) {
    if (c->frames[j].type != (uint64_t)CA_EVENTS_RESPONSE ||
        c->frames[j].rid != want_req_id) {
      continue;
    }
    uint64_t t = 0, rid = 0;
    void* p = NULL;
    uint8_t st = 0;
    if (!client_decode(c, j, &t, &p, &rid, &st)) continue;
    ca_events_response_t* ev = (ca_events_response_t*)p;
    bool hit = (ev->seq == 0 && ev->op == want_op);
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
    if (hit) return true;
  }
  return false;
}

static bool chan_wait_marker(test_client_t* c, uint64_t want_req_id,
                             uint8_t want_op, int rounds) {
  for (int i = 0; i < rounds; i++) {
    client_pump(c);
    if (chan_marker_op_seen(c, want_req_id, want_op)) return true;
    if (c->dead) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

/* Waits until the channel delivers ONE msg.append user record whose content
   is `text`. */
static bool chan_wait_text(test_client_t* c, uint64_t want_req_id,
                           const char* text, int rounds) {
  for (int i = 0; i < rounds; i++) {
    client_pump(c);
    for (const chan_ev_t& e : client_channel(c, want_req_id)) {
      if (e.seq > 0 && e.steer_content == text) return true;
    }
    if (c->dead) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

/* --- the scripted model (test_client_api_handlers.cpp's MINIMAL
       transcription verbatim; the shared-backend injection is what makes an
       api-created frame's turns deterministic) ------------------------------ */

typedef struct unix_scripted_t {
  model_backend_t base;
  std::vector<std::string>* replies;
} unix_scripted_t;

static int unix_scripted_decode(const std::string& body,
                                model_reply_t** reply_out, char** error_out) {
  char* err = NULL;
  json_value_t* root = json_parse(body.c_str(), body.size(), &err);
  if (err != NULL) free(err);
  if (root == NULL) {
    *error_out = strdup("unix scripted model: body is not valid JSON");
    return -1;
  }
  json_value_t* choices = json_get(root, "choices");
  json_value_t* choice =
      (choices != NULL && json_type(choices) == JSON_ARRAY)
          ? json_at(choices, 0) : NULL;
  json_value_t* message =
      (choice != NULL && json_type(choice) == JSON_OBJECT)
          ? json_get(choice, "message") : NULL;
  if (message == NULL) {
    json_value_destroy(root);
    *error_out = strdup("unix scripted model: no message in choices[0]");
    return -1;
  }
  model_reply_t* r = (model_reply_t*)get_clear_memory(sizeof(model_reply_t));
  json_value_t* content = json_get(message, "content");
  if (content == NULL || json_type(content) == JSON_NULL) {
    r->content = strdup("");
  } else {
    r->content = strdup(json_as_string(content));
  }
  json_value_t* calls = json_get(message, "tool_calls");
  if (calls != NULL && json_type(calls) == JSON_ARRAY && json_size(calls) > 0) {
    json_value_t* fn = json_get(json_at(calls, 0), "function");
    json_value_t* args = (fn != NULL) ? json_get(fn, "arguments") : NULL;
    if (args == NULL || json_type(args) != JSON_STRING) {
      model_reply_destroy(r);
      json_value_destroy(root);
      *error_out = strdup("unix scripted model: arguments must be a string");
      return -1;
    }
    const char* args_text = json_as_string(args);
    char* aerr = NULL;
    json_value_t* parsed_args = json_parse(args_text, strlen(args_text), &aerr);
    if (aerr != NULL) free(aerr);
    json_value_t* code =
        (parsed_args != NULL) ? json_get(parsed_args, "code") : NULL;
    if (code == NULL || json_type(code) != JSON_STRING) {
      if (parsed_args != NULL) json_value_destroy(parsed_args);
      model_reply_destroy(r);
      json_value_destroy(root);
      *error_out = strdup("unix scripted model: no string `code`");
      return -1;
    }
    r->tool_code = strdup(json_as_string(code));
    if (parsed_args != NULL) json_value_destroy(parsed_args);
  }
  json_value_destroy(root);
  *reply_out = r;
  return 0;
}

static int unix_scripted_complete(void* self, json_value_t* messages,
                                  json_value_t* tools, char** raw_out,
                                  model_reply_t** reply_out, char** error_out) {
  (void)messages;
  (void)tools;
  (void)raw_out;
  *reply_out = NULL;
  *error_out = NULL;
  unix_scripted_t* sm = (unix_scripted_t*)self;
  if (sm->replies->empty()) {
    *error_out = strdup("unix scripted model: queue empty");
    return -1;
  }
  std::string body = sm->replies->front();
  sm->replies->erase(sm->replies->begin());
  return unix_scripted_decode(body, reply_out, error_out) == 0 ? 0 : -1;
}

/* --- the pinned tests -------------------------------------------------------- */

TEST(TestClientApiUnix, TestPromptRoundTripCreatesARealFrame) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  test_client_t client;
  ASSERT_EQ(client_connect(&client, fx.socket_path), 0);

  std::string sid = prompt_create_sid(&client, 7, "make the thing");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u)
      << "the wire's response carries the frame's sid";

  /* the real frame in the real store (the fixture's root) */
  EXPECT_NE(ca_session_server_frame(fx.server, sid.c_str()), nullptr);

  /* the SESSIONS listing through the same socket: the store-side existence */
  ca_sessions_request_t* req = sessions_req_heap(9);
  ASSERT_EQ(client_send_frame(&client, CA_SESSIONS_REQUEST, req), 0);
  ca_wire_payload_destroy(CA_SESSIONS_REQUEST, req);
  size_t idx = 0;
  ASSERT_EQ(client_wait_frame(&client, CA_SESSIONS_RESPONSE, 9, &idx, 600), 1)
      << "the listing's response";
  uint64_t type = 0, rid = 0;
  void* payload = NULL;
  uint8_t status = 0;
  ASSERT_TRUE(client_decode(&client, idx, &type, &payload, &rid, &status));
  ASSERT_EQ(type, (uint64_t)CA_SESSIONS_RESPONSE);
  ASSERT_EQ(rid, 9u);
  ca_sessions_response_t* listing = (ca_sessions_response_t*)payload;
  ASSERT_EQ(listing->nrecords, 1u) << "one born session";
  EXPECT_STREQ(listing->records[0].sid, sid.c_str());
  EXPECT_STREQ(listing->records[0].status, "running");
  EXPECT_EQ(listing->records[0].depth, (size_t)0);
  ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, payload);

  client_close(&client);
  fixture_teardown(&fx);
}

TEST(TestClientApiUnix, TestSteerAndEventsStreamThroughTheSocket) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  test_client_t client;
  ASSERT_EQ(client_connect(&client, fx.socket_path), 0);

  std::string sid = prompt_create_sid(&client, 1, "steer me");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  /* the live channel first: the steer's commit arrives as a live record */
  ca_events_request_t* sub =
      events_req_heap(2, sid.c_str(), CA_EVENTS_LIVE_ONLY, 0);
  ASSERT_EQ(client_send_frame(&client, CA_EVENTS_REQUEST, sub), 0);
  ca_wire_payload_destroy(CA_EVENTS_REQUEST, sub);
  ASSERT_TRUE(chan_wait_marker(&client, 2, CA_EVENTS_LIVE_ONLY, 600))
      << "the live-transition marker (seq 0, op echoed)";
  {
    /* the marker's exact decoded shape */
    size_t idx = 0;
    ASSERT_EQ(client_wait_frame(&client, CA_EVENTS_RESPONSE, 2, &idx, 600), 1);
    uint64_t t = 0, rid = 0;
    void* p = NULL;
    uint8_t st = 0;
    ASSERT_TRUE(client_decode(&client, idx, &t, &p, &rid, &st));
    ca_events_response_t* marker = (ca_events_response_t*)p;
    EXPECT_EQ(marker->seq, 0u);
    EXPECT_EQ(marker->op, (uint8_t)CA_EVENTS_LIVE_ONLY);
    EXPECT_EQ(marker->record_json, nullptr);
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
  }

  /* the steer: QUEUED answer (status 0, sid "") */
  ca_prompt_request_t* steer =
      prompt_req_heap(3, sid.c_str(), "the steer");
  ASSERT_EQ(client_send_frame(&client, CA_PROMPT_REQUEST, steer), 0);
  ca_wire_payload_destroy(CA_PROMPT_REQUEST, steer);
  size_t idx = 0;
  ASSERT_EQ(client_wait_frame(&client, CA_PROMPT_RESPONSE, 3, &idx, 600), 1);
  {
    uint64_t t = 0, rid = 0;
    void* p = NULL;
    uint8_t st = 0;
    ASSERT_TRUE(client_decode(&client, idx, &t, &p, &rid, &st));
    ca_prompt_response_t* res = (ca_prompt_response_t*)p;
    EXPECT_EQ(res->status, 0u);
    EXPECT_STREQ(res->sid, "") << "a steer's sid is the empty sentinel";
    ca_wire_payload_destroy(CA_PROMPT_RESPONSE, p);
  }

  /* the durable record streams through the live tail */
  ASSERT_TRUE(chan_wait_text(&client, 2, "the steer", 600))
      << "the steer's msg.append user record arrived over the socket";

  /* free the (conn, sid) pair before the replay (one sub per pair — the
     duplicates' refusal): the live channel unsubscribes FIRST */
  ca_events_request_t* unsub =
      events_req_heap(5, sid.c_str(), CA_EVENTS_UNSUBSCRIBE, 0);
  ASSERT_EQ(client_send_frame(&client, CA_EVENTS_REQUEST, unsub), 0);
  ca_wire_payload_destroy(CA_EVENTS_REQUEST, unsub);
  ASSERT_TRUE(chan_wait_marker(&client, 5, CA_EVENTS_UNSUBSCRIBE, 600))
      << "the unsub's terminal marker";

  /* the REPLAY: resubscribes from seq 0, replays the records, signals the
     marker with the replay's op echo. */
  ca_events_request_t* replay =
      events_req_heap(4, sid.c_str(), CA_EVENTS_REPLAY_THEN_LIVE, 0);
  ASSERT_EQ(client_send_frame(&client, CA_EVENTS_REQUEST, replay), 0);
  ca_wire_payload_destroy(CA_EVENTS_REQUEST, replay);
  ASSERT_TRUE(chan_wait_marker(&client, 4, CA_EVENTS_REPLAY_THEN_LIVE, 600))
      << "the replay's marker closed the channel";

  client_close(&client);
  fixture_teardown(&fx);
}

TEST(TestClientApiUnix, TestUnknownSidAndMalformedFrameRefuseLoud) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  test_client_t client;
  ASSERT_EQ(client_connect(&client, fx.socket_path), 0);

  const char* ghost = "sessions/0000000000000000000deadbeef";
  ca_prompt_request_t* ghost_steer =
      prompt_req_heap(11, ghost, "a steer to nowhere");
  ASSERT_EQ(client_send_frame(&client, CA_PROMPT_REQUEST, ghost_steer), 0);
  ca_wire_payload_destroy(CA_PROMPT_REQUEST, ghost_steer);

  /* a framed-but-un-CBOR frame: the transport's bridge refuses req_id 0 */
  {
    size_t framed_len;
    uint8_t* framed =
        stream_frame_encode((const uint8_t*)"\xff\xff\xff", 3, &framed_len);
    ASSERT_NE(framed, nullptr);
    const uint8_t* p = framed;
    size_t off = 0;
    while (off < framed_len) {
      ssize_t n =
          platform_socket_send(client.sock, p + off, framed_len - off);
      ASSERT_GT(n, 0) << "the garbage frame's send";
      off += (size_t)n;
    }
    free(framed);
  }

  ca_interrupt_request_t* ghost_int = interrupt_req_heap(13, ghost);
  ASSERT_EQ(client_send_frame(&client, CA_INTERRUPT_REQUEST, ghost_int), 0);
  ca_wire_payload_destroy(CA_INTERRUPT_REQUEST, ghost_int);

  /* THREE refusals: the unknown-sid steer, the garbage frame, the
     unknown-sid interrupt — count each recorded frame ONCE (the round loop
     re-scans the recording), and wait until all THREE answers arrived. */
  {
    std::vector<size_t> seen;
    size_t saw = 0;
    for (int i = 0; i < 600 && saw < 3; i++) {
      client_pump(&client);
      for (size_t j = 0; j < client.frames.size(); j++) {
        bool counted = false;
        for (size_t k = 0; k < seen.size(); k++) {
          if (seen[k] == j) counted = true;
        }
        if (counted) continue;
        if (client.frames[j].type != (uint64_t)CA_ERROR) continue;
        uint64_t t = 0, rid = 0;
        void* p = NULL;
        uint8_t st = 0;
        ASSERT_TRUE(client_decode(&client, j, &t, &p, &rid, &st));
        ca_error_t* err = (ca_error_t*)p;
        ASSERT_NE(err->text, nullptr);
        EXPECT_FALSE(std::string(err->text).empty());
        if (rid == 11u) {
          EXPECT_EQ(err->status, 2u) << "the unknown-session status";
        } else if (rid == 13u) {
          EXPECT_EQ(err->status, 2u);
        } else if (rid == 0u) {
          EXPECT_EQ(err->status, 1u)
              << "the malformed frame's refusal status";
        }
        ca_wire_payload_destroy(CA_ERROR, p);
        seen.push_back(j);
        saw++;
      }
      if (saw < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    ASSERT_EQ(saw, 3u) << "every refused frame answered CA_ERROR exactly "
                          "once";
  }

  client_close(&client);
  fixture_teardown(&fx);
}

TEST(TestClientApiUnix, TestConnCloseMidReplayPurges) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  test_client_t a, b;
  ASSERT_EQ(client_connect(&a, fx.socket_path), 0);

  std::string sid = prompt_create_sid(&a, 1, "the replay purge");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  /* A's live channel anchors the watch; the bulk steers commit through it —
     the log B's replay will carry is loaded. */
  ca_events_request_t* live_sub =
      events_req_heap(2, sid.c_str(), CA_EVENTS_LIVE_ONLY, 0);
  ASSERT_EQ(client_send_frame(&a, CA_EVENTS_REQUEST, live_sub), 0);
  ca_wire_payload_destroy(CA_EVENTS_REQUEST, live_sub);
  ASSERT_TRUE(chan_wait_marker(&a, 2, CA_EVENTS_LIVE_ONLY, 600));
  for (int i = 0; i < 20; i++) {
    char text[32];
    snprintf(text, sizeof(text), "bulk-%02d", i);
    ca_prompt_request_t* steer = prompt_req_heap(10 + (uint64_t)i,
                                                 sid.c_str(), text);
    ASSERT_EQ(client_send_frame(&a, CA_PROMPT_REQUEST, steer), 0);
    ca_wire_payload_destroy(CA_PROMPT_REQUEST, steer);
    ASSERT_TRUE(chan_wait_text(&a, 2, text, 600)) << "steer " << text;
  }

  /* B subscribes the FULL replay and closes MID-REPLAY: the teardown's
     purge (ca_session_conn_closed, marshalled) drops the subscription and
     its pending round trip — the closed connection's replay channel dies
     quietly, and a late commit reaches nothing of B's. The B-side
     observable over a CLOSED socket is nothing — the purge's proof is the
     surviving subscriber (below) plus the suites's leak checks (ASan +
     valgrind): a leaked unpurged sub would hold a conn ref forever. */
  ASSERT_EQ(client_connect(&b, fx.socket_path), 0);
  ca_events_request_t* replay =
      events_req_heap(200, sid.c_str(), CA_EVENTS_REPLAY_THEN_LIVE, 0);
  ASSERT_EQ(client_send_frame(&b, CA_EVENTS_REQUEST, replay), 0);
  ca_wire_payload_destroy(CA_EVENTS_REQUEST, replay);
  client_close(&b);   /* IMMEDIATE: the purge races the replay's scan */

  /* The teardown settles, then the post-close commit: A keeps receiving
     (the unwatch correctly skipped — A remains subscribed), the machinery
     ran through the purge. */
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ca_prompt_request_t* post = prompt_req_heap(300, sid.c_str(), "postclose");
  ASSERT_EQ(client_send_frame(&a, CA_PROMPT_REQUEST, post), 0);
  ca_wire_payload_destroy(CA_PROMPT_REQUEST, post);
  ASSERT_TRUE(chan_wait_text(&a, 2, "postclose", 600))
      << "the surviving subscriber keeps receiving";

  client_close(&a);
  fixture_teardown(&fx);
}

/* --- the TCP suite (Task 6): the SAME stack over loopback TCP, behind the
   bcrypt api-key auth — the unix suite's fixture + client double, the AUTH
   pair opening every connection --------------------------------------------- */

/* FAIL-LOUD pin: the transport refuses a NULL or empty key hash at create —
   no unauthenticated TCP, ever (unix leans on file permissions; TCP cannot). */
TEST(TestClientApiTcp, TestTransportRefusesToStartWithoutAKey) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup_common(&fx, NULL, FRAME_ESCALATION_FREE), 0);
  EXPECT_EQ(tcp_transport_create(fx.pool, fx.server, "127.0.0.1", 0, NULL,
                                 NULL), nullptr)
      << "the NULL key hash refuses";
  EXPECT_EQ(tcp_transport_create(fx.pool, fx.server, "127.0.0.1", 0, "",
                                 NULL), nullptr)
      << "the empty key hash refuses the same way";
  fixture_teardown(&fx);
}

TEST(TestClientApiTcp, TestAuthedPromptRoundTripOverTcp) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup_tcp(&fx, NULL), 0);
  test_client_t client;
  ASSERT_EQ(client_connect_tcp(&client, "127.0.0.1", fixture_tcp_port(&fx)), 0);

  /* the AUTH pair opens the conversation; the right key authenticates */
  ASSERT_EQ(client_authenticate(&client, 1, FIXTURE_API_KEY), 0)
      << "the right key's response carries status 0";

  std::string sid = prompt_create_sid(&client, 7, "make the tcp thing");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u)
      << "the wire's response carries the frame's sid";
  EXPECT_NE(ca_session_server_frame(fx.server, sid.c_str()), nullptr)
      << "the real frame in the real store";

  client_close(&client);
  fixture_teardown(&fx);
}

/* THE AUTH STATE'S ENFORCEMENT: a non-AUTH first frame is refused loud —
   one CA_ERROR echoing the refused request's req_id — and the connection
   CLOSES. THE GRACEFUL-CLOSE PATH'S FIRST REAL CALLER: TCP_CONNECTION_CLOSE
   rides out behind the error's write and tears the connection down (the
   client's EOF is the pin); before this slice its only callers were the
   teardown passes and the socket-error paths (CA5's recorded untested
   entry point). */
TEST(TestClientApiTcp, TestPromptBeforeAuthRefusedLoudAndClosed) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup_tcp(&fx, NULL), 0);
  test_client_t client;
  ASSERT_EQ(client_connect_tcp(&client, "127.0.0.1", fixture_tcp_port(&fx)), 0);

  ca_prompt_request_t* req = prompt_req_heap(5, NULL, "sneaking in");
  ASSERT_EQ(client_send_frame(&client, CA_PROMPT_REQUEST, req), 0);
  ca_wire_payload_destroy(CA_PROMPT_REQUEST, req);

  size_t idx = 0;
  ASSERT_EQ(client_wait_frame(&client, CA_ERROR, 5, &idx, 600), 1)
      << "the pre-auth refusal answers the echoed req_id";
  uint64_t t = 0, rid = 0;
  void* p = NULL;
  uint8_t st = 0;
  ASSERT_TRUE(client_decode(&client, idx, &t, &p, &rid, &st));
  ASSERT_EQ(t, (uint64_t)CA_ERROR);
  ASSERT_EQ(rid, 5u);
  ca_error_t* err = (ca_error_t*)p;
  ASSERT_NE(err->text, nullptr);
  EXPECT_STREQ(err->text, "authentication required");
  EXPECT_EQ(err->status, 1u);
  ca_wire_payload_destroy(CA_ERROR, p);

  EXPECT_TRUE(client_wait_dead(&client, 600))
      << "the connection closes after the pre-auth refusal";

  client_close(&client);
  fixture_teardown(&fx);
}

TEST(TestClientApiTcp, TestWrongKeyAnswersStatusOneAndCloses) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup_tcp(&fx, NULL), 0);
  test_client_t client;
  ASSERT_EQ(client_connect_tcp(&client, "127.0.0.1", fixture_tcp_port(&fx)), 0);

  /* the wrong key: the AUTH RESPONSE with status 1, then the close */
  ca_auth_request_t* wrong = auth_req_heap(3, "not-the-key");
  ASSERT_EQ(client_send_frame(&client, CA_AUTH_REQUEST, wrong), 0);
  ca_wire_payload_destroy(CA_AUTH_REQUEST, wrong);

  size_t idx = 0;
  ASSERT_EQ(client_wait_frame(&client, CA_AUTH_RESPONSE, 3, &idx, 600), 1)
      << "the auth pair's response frame";
  uint64_t t = 0, rid = 0;
  void* p = NULL;
  uint8_t st = 0;
  ASSERT_TRUE(client_decode(&client, idx, &t, &p, &rid, &st));
  ASSERT_EQ(t, (uint64_t)CA_AUTH_RESPONSE);
  ASSERT_EQ(rid, 3u);
  ca_auth_response_t* res = (ca_auth_response_t*)p;
  EXPECT_EQ(res->status, 1u) << "the bad key's status";
  ca_wire_payload_destroy(CA_AUTH_RESPONSE, p);

  EXPECT_TRUE(client_wait_dead(&client, 600))
      << "the connection closes after the bad-key response";

  client_close(&client);
  fixture_teardown(&fx);
}

#if defined(SA_HAS_PYTHON)

/* test_client_api_handlers.cpp's same shape: py_agent_init through a bare
   extern — pulling py_agent.h would demand <Python.h> here. */
extern "C" void py_agent_init(void);

TEST(TestClientApiUnix, TestInterruptPostsOverTheSocket) {
  /* A POOLED frame mid-cell (the handlers' interrupt shape): the wire's
     INTERRUPT posts (status 0) and the synthesis lands — the durable
     turn.end {aborted ... interrupted}. The shared backend makes turn 1
     deterministic. */
  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import time\\nprint('started')\\ntime.sleep(3)\"}"}}]}}]})json";
  std::vector<std::string> replies = {turn1};
  unix_scripted_t sm;
  memset(&sm, 0, sizeof(sm));
  sm.base.complete = unix_scripted_complete;
  sm.replies = &replies;
  py_agent_init();

  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, &sm.base), 0);
  test_client_t client;
  ASSERT_EQ(client_connect(&client, fx.socket_path), 0);

  std::string sid = prompt_create_sid(&client, 1, "interrupt me");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  frame_t* f = ca_session_server_frame(fx.server, sid.c_str());
  ASSERT_NE(f, nullptr);

  /* The interrupt must land on an OPEN turn (the poll, not a blind sleep:
     under ASan a fixed wait raced the frame's boot — an FRM_INT before
     turn.start ever hit the log is the case-3 boundary cut, which
     synthesizes nothing). Poll the frame's events until the turn's first
     lifecycle record is on the log (deadline 10 s; 5 ms between reads). */
  bool turn_open = false;
  for (int i = 0; i < 2000 && !turn_open; i++) {
    char* json = frame_debug_events(f);
    ASSERT_NE(json, nullptr);
    char* err = NULL;
    json_value_t* events = json_parse(json, strlen(json), &err);
    if (err != NULL) free(err);
    free(json);
    ASSERT_NE(events, nullptr);
    for (size_t j = 0; j < json_size(events); j++) {
      json_value_t* rec = json_at(events, j);
      json_value_t* t_v = json_get(rec, "type");
      if (t_v == NULL) continue;
      const char* t = json_as_string(t_v);
      if (strcmp(t, "turn.start") == 0 || strcmp(t, "step.start") == 0) {
        turn_open = true;
      }
    }
    json_value_destroy(events);
    if (!turn_open) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(turn_open) << "the frame's turn opened before the interrupt";

  ca_interrupt_request_t* req = interrupt_req_heap(2, sid.c_str());
  ASSERT_EQ(client_send_frame(&client, CA_INTERRUPT_REQUEST, req), 0);
  ca_wire_payload_destroy(CA_INTERRUPT_REQUEST, req);
  size_t idx = 0;
  ASSERT_EQ(client_wait_frame(&client, CA_INTERRUPT_RESPONSE, 2, &idx, 600), 1);
  {
    uint64_t t = 0, rid = 0;
    void* p = NULL;
    uint8_t st = 0;
    ASSERT_TRUE(client_decode(&client, idx, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_INTERRUPT_RESPONSE);
    ASSERT_EQ(rid, 2u);
    ca_interrupt_response_t* res = (ca_interrupt_response_t*)p;
    EXPECT_EQ(res->status, 0u) << "the interrupt POSTED into the mailbox";
    ca_wire_payload_destroy(CA_INTERRUPT_RESPONSE, p);
  }

  /* The synthesis's durable shape: the open turn's aborted turn.end. */
  bool saw_abort = false;
  for (int i = 0; i < 800 && !saw_abort; i++) {
    char* json = frame_debug_events(f);
    ASSERT_NE(json, nullptr);
    char* err = NULL;
    json_value_t* events = json_parse(json, strlen(json), &err);
    if (err != NULL) free(err);
    free(json);
    ASSERT_NE(events, nullptr);
    for (size_t j = 0; j < json_size(events); j++) {
      json_value_t* rec = json_at(events, j);
      json_value_t* t_v = json_get(rec, "type");
      if (t_v == NULL || strcmp(json_as_string(t_v), "turn.end") != 0) continue;
      json_value_t* reason = json_get(json_get(rec, "payload"), "reason");
      json_value_t* kind = (reason != NULL) ? json_get(reason, "kind") : NULL;
      json_value_t* text = (reason != NULL) ? json_get(reason, "text") : NULL;
      if (kind != NULL && strcmp(json_as_string(kind), "aborted") == 0 &&
          text != NULL &&
          strstr(json_as_string(text), "interrupted") != NULL) {
        saw_abort = true;
      }
    }
    json_value_destroy(events);
    if (!saw_abort) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(saw_abort) << "the interrupt's synthesis committed durably";

  /* Let the interrupted cell's sleep run out (the teardown's join is then
     instant — the documented bounded cost). */
  std::this_thread::sleep_for(std::chrono::milliseconds(4000));

  client_close(&client);
  fixture_teardown(&fx);
}

#endif /* SA_HAS_PYTHON */

/* --- the escalation slice's ask tests (the wire's [16]/[17] pair over the
   REAL transports; NO pyrt anywhere — the gate ask is runtime-authored
   from a content-only plan reply) ------------------------------------------ */

/* The escalated UNIX variant: the fixture's frame-cfg template carries the
   plan-ask-act mode (the server copies it at create — every api-created
   frame parks at the runtime-authored plan gate). */
static int fixture_setup_escalated_unix(fixture_t* fx,
                                        model_backend_t* shared_backend) {
  int rc = fixture_setup_common(fx, shared_backend,
                                FRAME_ESCALATION_PLAN_ASK_ACT);
  if (rc != 0) return rc;
  fx->transport = unix_transport_create(fx->pool, fx->server,
                                        fx->socket_path);
  if (fx->transport == NULL) return -6;
  unix_transport_start(fx->transport);
  return 0;
}

/* The escalated TCP twin: the auth machinery's shape, the parked gate's
   SAME template knob. */
static int fixture_setup_escalated_tcp(fixture_t* fx,
                                       model_backend_t* shared_backend) {
  int rc = fixture_setup_common(fx, shared_backend,
                                FRAME_ESCALATION_PLAN_ASK_ACT);
  if (rc != 0) return rc;
  char hash[64];
  if (bcrypt_generate(FIXTURE_API_KEY, 4, hash, sizeof(hash)) != 0) return -7;
  fx->tcp_transport = tcp_transport_create(fx->pool, fx->server,
                                           "127.0.0.1", 0, hash,
                                           &fx->tcp_addr);
  if (fx->tcp_transport == NULL) return -6;
  tcp_transport_start(fx->tcp_transport);
  return 0;
}

static ca_ask_reply_request_t* ask_reply_req_heap(uint64_t req_id,
                                                  const char* sid,
                                                  const char* ask_id,
                                                  uint8_t decision,
                                                  const char* value) {
  ca_ask_reply_request_t* req =
      (ca_ask_reply_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  req->sid = (sid != NULL) ? strdup(sid) : NULL;
  req->ask_id = (ask_id != NULL) ? strdup(ask_id) : NULL;
  req->decision = decision;
  req->value = (value != NULL) ? strdup(value) : NULL;
  return req;
}

/* The parked plan gate's ask id, polled off the registry frame's event log
   (the handlers' suite's same helper — the events-stream truth). "" when
   the ask never landed within the bound. */
static std::string fixture_parked_ask_id(fixture_t* fx,
                                         const std::string& sid) {
  frame_t* f = ca_session_server_frame(fx->server, sid.c_str());
  if (f == NULL) return "";
  for (int i = 0; i < 800; i++) {
    char* json = frame_debug_events(f);
    if (json == NULL) return "";
    char* err = NULL;
    json_value_t* events = json_parse(json, strlen(json), &err);
    if (err != NULL) free(err);
    free(json);
    if (events == NULL) return "";
    std::string found;
    for (size_t j = 0; j < json_size(events); j++) {
      json_value_t* rec = json_at(events, j);
      json_value_t* t_v = json_get(rec, "type");
      if (t_v == NULL || strcmp(json_as_string(t_v), "ask") != 0) continue;
      json_value_t* p = json_get(rec, "payload");
      json_value_t* id = (p != NULL) ? json_get(p, "askId") : NULL;
      if (id != NULL && json_type(id) == JSON_STRING) {
        found = json_as_string(id);
        break;
      }
    }
    json_value_destroy(events);
    if (!found.empty()) return found;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return "";
}

/* The gate ask's durable consumption (the ask.reply record resolving
   `ask_id` with {answer, value}) — polled on the frame's log. */
static bool fixture_ask_reply_committed(fixture_t* fx, const std::string& sid,
                                        const std::string& ask_id,
                                        const char* value, int rounds) {
  frame_t* f = ca_session_server_frame(fx->server, sid.c_str());
  if (f == NULL) return false;
  for (int i = 0; i < rounds; i++) {
    bool found = false;
    char* json = frame_debug_events(f);
    if (json == NULL) return false;
    char* err = NULL;
    json_value_t* events = json_parse(json, strlen(json), &err);
    if (err != NULL) free(err);
    free(json);
    if (events == NULL) return false;
    for (size_t j = 0; j < json_size(events); j++) {
      json_value_t* rec = json_at(events, j);
      json_value_t* t_v = json_get(rec, "type");
      if (t_v == NULL || strcmp(json_as_string(t_v), "ask.reply") != 0) {
        continue;
      }
      json_value_t* p = json_get(rec, "payload");
      json_value_t* id_v = (p != NULL) ? json_get(p, "askId") : NULL;
      json_value_t* val_v = (p != NULL) ? json_get(p, "value") : NULL;
      if (id_v != NULL && json_type(id_v) == JSON_STRING &&
          strcmp(json_as_string(id_v), ask_id.c_str()) == 0 &&
          val_v != NULL && json_type(val_v) == JSON_STRING &&
          strcmp(json_as_string(val_v), value) == 0) {
        found = true;
        break;
      }
    }
    json_value_destroy(events);
    if (found) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

TEST(TestClientApiUnix, TestAskReplyTransitsUnix) {
  /* The wire's raw bytes end-to-end (spec §3.1): the client sends [16,
     req_id, sid, ask_id, decision, value] over the REAL unix socket; the
     transport's framer extracts it, the handlers bind + post, and the [17,
     req_id, delivered] ack comes back DECODED on the socket. The consumed
     park's ask.reply record is the tail's proof that the bytes didn't just
     bounce — the engine ate the reply. */
  std::vector<std::string> replies = {
      std::string(
          R"json({"choices":[{"message":{"role":"assistant","content":)json"
          R"json("Plan: 1. measure 2. cut 3. report"}}]})json"),
      std::string(
          R"json({"choices":[{"message":{"role":"assistant","content":)json"
          R"json("the act ran quiet"}}]})json")};
  unix_scripted_t sm;
  memset(&sm, 0, sizeof(sm));
  sm.base.complete = unix_scripted_complete;
  sm.replies = &replies;

  fixture_t fx;
  ASSERT_EQ(fixture_setup_escalated_unix(&fx, &sm.base), 0);
  test_client_t client;
  ASSERT_EQ(client_connect(&client, fx.socket_path), 0);

  std::string sid = prompt_create_sid(&client, 1, "plan over the wire");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  std::string ask_id = fixture_parked_ask_id(&fx, sid);
  ASSERT_EQ(ask_id.size(), 8u) << "the gate ask parked";

  ca_ask_reply_request_t* req =
      ask_reply_req_heap(2, sid.c_str(), ask_id.c_str(), 0, "Approve");
  ASSERT_EQ(client_send_frame(&client, CA_ASK_REPLY_REQUEST, req), 0);
  ca_wire_payload_destroy(CA_ASK_REPLY_REQUEST, req);

  size_t idx = 0;
  ASSERT_EQ(client_wait_frame(&client, CA_ASK_REPLY_RESPONSE, 2, &idx, 600),
            1) << "the [17] ack came back over the socket";
  {
    uint64_t t = 0, rid = 0;
    void* p = NULL;
    uint8_t st = 0;
    ASSERT_TRUE(client_decode(&client, idx, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_ASK_REPLY_RESPONSE);
    ASSERT_EQ(rid, 2u);
    ca_ask_reply_response_t* res = (ca_ask_reply_response_t*)p;
    EXPECT_EQ(res->delivered, 1u) << "the bind/post's honest ack";
    ca_wire_payload_destroy(CA_ASK_REPLY_RESPONSE, p);
  }

  EXPECT_TRUE(
      fixture_ask_reply_committed(&fx, sid, ask_id, "Approve", 800))
      << "the consumption landed durably (the bytes reached the engine)";

  client_close(&client);
  fixture_teardown(&fx);
}

TEST(TestClientApiTcp, TestAskReplyTransitsTcp) {
  /* The TCP-auth twin: the SAME [16]/[17] exchange rides the AUTHED
     loopback channel — the api-key exchange ran FIRST at connect, and the
     reply's bytes traverse the framer either way. */
  std::vector<std::string> replies = {
      std::string(
          R"json({"choices":[{"message":{"role":"assistant","content":)json"
          R"json("Plan: 1. measure 2. cut 3. report"}}]})json"),
      std::string(
          R"json({"choices":[{"message":{"role":"assistant","content":)json"
          R"json("the act ran quiet"}}]})json")};
  unix_scripted_t sm;
  memset(&sm, 0, sizeof(sm));
  sm.base.complete = unix_scripted_complete;
  sm.replies = &replies;

  fixture_t fx;
  ASSERT_EQ(fixture_setup_escalated_tcp(&fx, &sm.base), 0);
  test_client_t client;
  ASSERT_EQ(
      client_connect_tcp(&client, "127.0.0.1", fixture_tcp_port(&fx)), 0);
  ASSERT_EQ(client_authenticate(&client, 1, FIXTURE_API_KEY), 0)
      << "the auth exchange's status 0";

  std::string sid = prompt_create_sid(&client, 2, "plan over tcp");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  std::string ask_id = fixture_parked_ask_id(&fx, sid);
  ASSERT_EQ(ask_id.size(), 8u) << "the gate ask parked";

  ca_ask_reply_request_t* req =
      ask_reply_req_heap(3, sid.c_str(), ask_id.c_str(), 0, "Approve");
  ASSERT_EQ(client_send_frame(&client, CA_ASK_REPLY_REQUEST, req), 0);
  ca_wire_payload_destroy(CA_ASK_REPLY_REQUEST, req);

  size_t idx = 0;
  ASSERT_EQ(client_wait_frame(&client, CA_ASK_REPLY_RESPONSE, 3, &idx, 600),
            1) << "the [17] ack came back over the authed channel";
  {
    uint64_t t = 0, rid = 0;
    void* p = NULL;
    uint8_t st = 0;
    ASSERT_TRUE(client_decode(&client, idx, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_ASK_REPLY_RESPONSE);
    ASSERT_EQ(rid, 3u);
    ca_ask_reply_response_t* res = (ca_ask_reply_response_t*)p;
    EXPECT_EQ(res->delivered, 1u);
    ca_wire_payload_destroy(CA_ASK_REPLY_RESPONSE, p);
  }

  client_close(&client);
  fixture_teardown(&fx);
}

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */