//
// Created by victor on 10/04/26.
//

/* The client-api handlers' suite (the client-api spec §3 / the plan's Task
   4): the in-proc wiring — ONE real wave_database_root on ONE scheduler
   pool (the store actor rides it — a pooled frame requires a pooled
   store), ONE streams loop thread, and the SERVER actor; the connection
   double plays the transport through the ca_session_conn_t interface and
   records the ENCODED frames (encode→decode round-tripped per assertion —
   the handlers' composed payloads are verified the way a client sees
   them). The recorded stream is scanned by TYPE + REQ_ID (the decode's
   out-param — the wire's design carries the id OUTSIDE the payload
   struct), never by position. */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "../src/ClientApi/client_api_wire.h"
#include "../src/ClientApi/handlers.h"
#include "../src/Frame/frame.h"
#include "../src/Frame/frame_messages.h"
#include "../src/Frame/frame_internal.h"
#include "../src/Frame/model.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Streams/loop_thread.h"
#include "../src/Platform/platform_time.h"
#include "../src/Util/allocator.h"
#include "../src/Util/json.h"
}

#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include <cstdlib>
#include <ctime>

static frame_config_t test_config(void) {
  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));   /* additive fields (pool, timeout) default */
  cfg.model_base_url = NULL;
  cfg.model_api_key = NULL;
  cfg.model_name = "unused";
  cfg.max_depth = 4;
  return cfg;
}

/* --- the in-proc fixture: pool + store + loop + server ---------------------- */

typedef struct {
  scheduler_pool_t* pool;
  streams_loop_thread_t* loop;
  wave_database_root_t* db;
  ca_session_server_t* server;
  frame_config_t cfg;
} fixture_t;

/* 0 = set up; nonzero = which step failed (the test ASSERTs). cfg's
   model_base_url stays NULL: a frame WITHOUT the shared backend builds a
   NULL default backend and its engine fails fast, deterministic and
   network-free ("model-missing") — the steers/events machinery under test
   never touches the engine's model path. */
static int fixture_setup(fixture_t* fx, model_backend_t* shared_backend) {
  memset(fx, 0, sizeof(*fx));
  fx->cfg = test_config();
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
  return 0;
}

/* The documented teardown order (handlers.h): the connections are closed by
   the tests before this; the server destroys FIRST (drains its closures +
   store round trips, unwatchs, owns the frames' teardown) while the pool
   still runs; then the pool stops, the root closes, the pool dies, and the
   loop dies last. */
static void fixture_teardown(fixture_t* fx) {
  ca_session_server_destroy(fx->server);
  scheduler_pool_stop(fx->pool);
  wave_db_close(fx->db);
  scheduler_pool_destroy(fx->pool);
  streams_loop_destroy(fx->loop);
}

/* --- the connection double (the transport's stand-in) ----------------------- */

typedef struct recorded_frame_t {
  uint64_t type;
  std::vector<uint8_t> bytes;
} recorded_frame_t;

typedef struct conn_double_t {
  ca_session_conn_t iface;   /* the address the handler calls get */
  platform_mutex_t* lock;    /* the loop thread's send vs the test's read */
  std::vector<recorded_frame_t> frames;
  std::atomic<int> refs;     /* the refcounter pair's counter */
} conn_double_t;

/* send(): encode + record (the transport machinery's stand-in); the payload
   is THIS send's on entry — consumed (destroyed) either way, per the
   ca_session_conn_t contract. */
static int conn_double_send(void* conn, uint64_t type, void* payload) {
  conn_double_t* d = (conn_double_t*)conn;
  uint8_t* raw = NULL;
  size_t len = 0;

  if (ca_wire_encode(type, payload, &raw, &len) != 0) {
    ca_wire_payload_destroy(type, payload);
    return -1;
  }
  platform_mutex_lock(d->lock);
  d->frames.push_back({type, std::vector<uint8_t>(raw, raw + len)});
  platform_mutex_unlock(d->lock);
  free(raw);
  ca_wire_payload_destroy(type, payload);
  return 0;
}

static void conn_double_retain(void* conn) {
  ((conn_double_t*)conn)->refs.fetch_add(1);
}

static void conn_double_release(void* conn) {
  ((conn_double_t*)conn)->refs.fetch_sub(1);
}

static void conn_double_init(conn_double_t* d) {
  memset(d, 0, sizeof(*d));
  d->lock = platform_mutex_create();
  ASSERT_NE(d->lock, nullptr);
  d->refs.store(0);
  d->iface.conn = d;
  d->iface.send = conn_double_send;
  d->iface.retain = conn_double_retain;
  d->iface.release = conn_double_release;
}

static void conn_double_destroy(conn_double_t* d) {
  platform_mutex_destroy(d->lock);
}

static size_t conn_count(conn_double_t* d) {
  platform_mutex_lock(d->lock);
  size_t n = d->frames.size();
  platform_mutex_unlock(d->lock);
  return n;
}

static bool conn_wait_count(conn_double_t* d, size_t want, int rounds) {
  for (int i = 0; i < rounds; i++) {
    if (conn_count(d) >= want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

/* Decode recorded frame `idx` on demand (a local COPY of its bytes — the
   recording vector is the loop thread's). The caller owns the payload; the
   type + req_id come back via the OUTS. */
static bool conn_decode(conn_double_t* d, size_t idx, uint64_t* type,
                        void** payload, uint64_t* req_id, uint8_t* status) {
  std::vector<uint8_t> bytes;

  platform_mutex_lock(d->lock);
  if (idx >= d->frames.size()) {
    platform_mutex_unlock(d->lock);
    return false;
  }
  bytes = d->frames[idx].bytes;
  uint64_t recorded = d->frames[idx].type;
  platform_mutex_unlock(d->lock);
  (void)recorded;
  return ca_wire_decode_bytes(bytes.data(), bytes.size(), type, payload,
                              req_id, status) == 0;
}

/* --- the request builders (heap payloads; ownership TRANSFERS into
   ca_session_handle) --------------------------------------------------------- */

static ca_prompt_request_t* prompt_req(uint64_t req_id, const char* sid,
                                       const char* text) {
  ca_prompt_request_t* req =
      (ca_prompt_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  req->sid = (sid != NULL) ? strdup(sid) : NULL;   /* NULL = create+start */
  req->text = strdup(text);
  return req;
}

static ca_events_request_t* events_req(uint64_t req_id, const char* sid,
                                       uint8_t op, uint64_t from_seq) {
  ca_events_request_t* req =
      (ca_events_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  /* sid NULL = the hand-crafted absent-sid shape (the CA2 provocation —
     the wire decodes "" as NULL, and the handler must refuse it); a text
     sid rides as-is. */
  req->sid = (sid != NULL) ? strdup(sid) : NULL;
  req->op = op;
  req->from_seq = from_seq;
  return req;
}

static ca_interrupt_request_t* interrupt_req(uint64_t req_id,
                                             const char* sid) {
  ca_interrupt_request_t* req =
      (ca_interrupt_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  req->sid = strdup(sid);
  return req;
}

static ca_sessions_request_t* sessions_req(uint64_t req_id) {
  ca_sessions_request_t* req =
      (ca_sessions_request_t*)get_clear_memory(sizeof(*req));
  req->req_id = req_id;
  return req;
}

/* --- the scripted model (a MINIMAL transcription of test_loop's helper:
   the shared-backend injection — ca_session_server_create's model_backend
   param — is what makes an api-created frame's turns deterministic) -------- */

typedef struct wire_scripted_t {
  model_backend_t base;
  std::vector<std::string>* replies;
} wire_scripted_t;

static int wire_scripted_decode(const std::string& body,
                                model_reply_t** reply_out, char** error_out) {
  char* err = NULL;
  json_value_t* root = json_parse(body.c_str(), body.size(), &err);
  if (err != NULL) free(err);
  if (root == NULL) {
    *error_out = strdup("wire scripted model: body is not valid JSON");
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
    *error_out = strdup("wire scripted model: no message in choices[0]");
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
      *error_out = strdup("wire scripted model: arguments must be a string");
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
      *error_out = strdup("wire scripted model: no string `code`");
      return -1;
    }
    r->tool_code = strdup(json_as_string(code));
    if (parsed_args != NULL) json_value_destroy(parsed_args);
  }
  json_value_destroy(root);
  *reply_out = r;
  return 0;
}

static int wire_scripted_complete(void* self, json_value_t* messages,
                                  json_value_t* tools, char** raw_out,
                                  model_reply_t** reply_out, char** error_out) {
  (void)messages;
  (void)tools;
  (void)raw_out;
  *reply_out = NULL;
  *error_out = NULL;
  wire_scripted_t* sm = (wire_scripted_t*)self;
  if (sm->replies->empty()) {
    *error_out = strdup("wire scripted model: queue empty");
    return -1;
  }
  std::string body = sm->replies->front();
  sm->replies->erase(sm->replies->begin());
  return wire_scripted_decode(body, reply_out, error_out) == 0 ? 0 : -1;
}

/* --- the common shapes ------------------------------------------------------ */

/* The FIRST sid a no-sid prompt answers with (decodes recorded frame 0 of
   the fresh double). Owns nothing of the caller's but the returned string. */
static std::string prompt_create_and_sid(fixture_t* fx, conn_double_t* conn,
                                         uint64_t req_id, const char* goal) {
  ca_session_handle(fx->server, CA_PROMPT_REQUEST, prompt_req(req_id, NULL,
                                                              goal),
                    &conn->iface);
  EXPECT_TRUE(conn_wait_count(conn, 1, 600)) << "the create's response";
  uint64_t type = 0, req_id_out = 0;
  void* payload = NULL;
  uint8_t status = 0;
  EXPECT_TRUE(conn_decode(conn, 0, &type, &payload, &req_id_out, &status));
  EXPECT_EQ(type, (uint64_t)CA_PROMPT_RESPONSE);
  ca_prompt_response_t* res = (ca_prompt_response_t*)payload;
  EXPECT_EQ(req_id_out, req_id);
  EXPECT_EQ(res->status, 0u);
  EXPECT_NE(res->sid, nullptr);
  std::string sid;
  if (res->sid != NULL) sid = res->sid;
  ca_wire_payload_destroy(CA_PROMPT_RESPONSE, payload);
  return sid;
}

/* --- the events-channel reader (the doubles' recorded stream, filtered to
   ONE channel's req_id — the decode's out-param is the routing key) --------- */

struct chan_ev_t {
  uint64_t seq;              /* 0 = a transition marker, not a record */
  std::string record_json;   /* "" for the marker */
  std::string steer_content; /* the msg.append user content, else "" */
};

/* The recorded events frames of ONE channel (the req_id match), decoded ONCE
   into the light shape. The caller's channel views are order-stable: the
   handlers stream ascending and never rewrite history. */
static std::vector<chan_ev_t> conn_channel(conn_double_t* d,
                                           uint64_t want_req_id) {
  std::vector<chan_ev_t> out;
  std::vector<std::vector<uint8_t>> candidates;

  platform_mutex_lock(d->lock);
  for (const recorded_frame_t& f : d->frames) {
    if (f.type == (uint64_t)CA_EVENTS_RESPONSE) {
      candidates.push_back(f.bytes);
    }
  }
  platform_mutex_unlock(d->lock);

  for (const std::vector<uint8_t>& bytes : candidates) {
    uint64_t t = 0, rid = 0;
    void* p = NULL;
    uint8_t st = 0;
    if (ca_wire_decode_bytes(bytes.data(), bytes.size(), &t, &p, &rid,
                             &st) != 0) {
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
            json_value_t* role = (rp != NULL) ? json_get(rp, "role") : NULL;
            json_value_t* content =
                (rp != NULL) ? json_get(rp, "content") : NULL;
            if (role != NULL && json_type(role) == JSON_STRING &&
                strcmp(json_as_string(role), "user") == 0 &&
                content != NULL && json_type(content) == JSON_STRING) {
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

/* The channel's highest forwarded seq (0 when nothing has arrived yet) —
   the RESUME-AT cursor's honest read (markers carry seq 0 and never
   advance it). */
static uint64_t chan_max_seq(conn_double_t* d) {
  uint64_t max_seq = 0;
  std::vector<std::vector<uint8_t>> candidates;
  platform_mutex_lock(d->lock);
  for (const recorded_frame_t& f : d->frames) {
    if (f.type == (uint64_t)CA_EVENTS_RESPONSE) candidates.push_back(f.bytes);
  }
  platform_mutex_unlock(d->lock);
  for (const std::vector<uint8_t>& bytes : candidates) {
    uint64_t t = 0, rid = 0;
    void* p = NULL;
    uint8_t st = 0;
    if (ca_wire_decode_bytes(bytes.data(), bytes.size(), &t, &p, &rid,
                             &st) != 0) {
      continue;
    }
    ca_events_response_t* ev = (ca_events_response_t*)p;
    if (ev->seq > max_seq) max_seq = ev->seq;
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
  }
  return max_seq;
}

/* Waits until the channel carries a transition marker (seq 0, `op` echoed)
   — the replay's live marker or the unsubscribe's terminal one. */
static bool chan_wait_marker(conn_double_t* d, uint64_t want_req_id,
                             uint8_t op, int rounds) {
  for (int i = 0; i < rounds; i++) {
    std::vector<std::vector<uint8_t>> candidates;
    platform_mutex_lock(d->lock);
    for (const recorded_frame_t& f : d->frames) {
      if (f.type == (uint64_t)CA_EVENTS_RESPONSE) {
        candidates.push_back(f.bytes);
      }
    }
    platform_mutex_unlock(d->lock);
    for (const std::vector<uint8_t>& bytes : candidates) {
      uint64_t t = 0, rid = 0;
      void* p = NULL;
      uint8_t st = 0;
      if (ca_wire_decode_bytes(bytes.data(), bytes.size(), &t, &p, &rid,
                               &st) != 0) {
        continue;
      }
      ca_events_response_t* ev = (ca_events_response_t*)p;
      bool hit = (rid == want_req_id && ev->seq == 0 && ev->op == op);
      ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
      if (hit) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

/* Waits until the channel delivers ONE msg.append user record whose content
   is `text` (racing steer / live tail's arrival). */
static bool chan_wait_text(conn_double_t* d, uint64_t want_req_id,
                           const char* text, int rounds) {
  for (int i = 0; i < rounds; i++) {
    for (const chan_ev_t& e : conn_channel(d, want_req_id)) {
      if (e.seq > 0 && e.steer_content == text) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

/* --- the five (plus the two decided) pins ----------------------------------- */

TEST(TestClientApiHandlers, TestPromptStartsATopFrameAndAnswersItsSid) {
  /* handlers on a no-sid prompt: the response carries {req_id, status 0,
     sid "sessions/..."}; the frame EXISTS in the root's sessions subtree
     (the SESSIONS listing through the wire proves it) + the registry holds
     it. */
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  conn_double_t conn;
  conn_double_init(&conn);

  std::string sid = prompt_create_and_sid(&fx, &conn, 7, "make the thing");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  /* The registry holds the new frame. */
  EXPECT_NE(ca_session_server_frame(fx.server, sid.c_str()), nullptr);

  /* The store-side existence: the SESSIONS listing through the wire. */
  ca_session_handle(fx.server, CA_SESSIONS_REQUEST, sessions_req(9),
                    &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, 2, 600)) << "the listing's response";
  uint64_t type = 0, req_id = 0;
  void* payload = NULL;
  uint8_t status = 0;
  ASSERT_TRUE(conn_decode(&conn, 1, &type, &payload, &req_id, &status));
  ASSERT_EQ(type, (uint64_t)CA_SESSIONS_RESPONSE);
  ASSERT_EQ(req_id, 9u);
  ca_sessions_response_t* listing = (ca_sessions_response_t*)payload;
  ASSERT_EQ(listing->nrecords, 1u) << "one born session";
  EXPECT_STREQ(listing->records[0].sid, sid.c_str());
  EXPECT_STREQ(listing->records[0].status, "running");
  EXPECT_EQ(listing->records[0].depth, (size_t)0);
  EXPECT_GT(listing->records[0].created, (uint64_t)0)
      << "the birth's ISO stamp converts to epoch seconds";
  time_t now = time(NULL);
  EXPECT_LT(listing->records[0].created, (uint64_t)now + 120);
  EXPECT_GT(listing->records[0].created, (uint64_t)now - 120);
  ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, payload);

  fixture_teardown(&fx);
  /* The ref discipline proved: the server released every connection ref. */
  EXPECT_EQ(conn.refs.load(), 0)
      << "the server's held connection references all released";
  conn_double_destroy(&conn);
}

TEST(TestClientApiHandlers, TestPromptSteerAppendsAUserMessage) {
  /* A created frame steered over the wire: status 0, the response's sid ""
     (the steer shape), and the frame's log carries the msg.append user
     record (observed through the wire's events channel). */
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  conn_double_t conn;
  conn_double_init(&conn);

  std::string sid = prompt_create_and_sid(&fx, &conn, 1, "steer me");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  /* The events channel goes live FIRST: the steer's commit arrives as a
     live record. */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(2, sid.c_str(), CA_EVENTS_LIVE_ONLY, 0),
                    &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, 2, 600)) << "the live marker";
  {
    void* p = NULL;
    uint64_t t = 0, rid = 0;
    uint8_t st = 0;
    ASSERT_TRUE(conn_decode(&conn, 1, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_EVENTS_RESPONSE);
    ca_events_response_t* marker = (ca_events_response_t*)p;
    EXPECT_EQ(marker->seq, 0u) << "the live-transition marker";
    EXPECT_EQ(marker->op, (uint8_t)CA_EVENTS_LIVE_ONLY) << "the op echoes";
    EXPECT_EQ(marker->record_json, nullptr);
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
  }

  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(3, sid.c_str(), "the steer"), &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, 3, 600)) << "the steer's QUEUED answer";
  {
    void* p = NULL;
    uint64_t t = 0, rid = 0;
    uint8_t st = 0;
    ASSERT_TRUE(conn_decode(&conn, 2, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_PROMPT_RESPONSE);
    ASSERT_EQ(rid, 3u);
    ca_prompt_response_t* res = (ca_prompt_response_t*)p;
    EXPECT_EQ(res->status, 0u) << "QUEUED — the FIFO covers the causality";
    EXPECT_STREQ(res->sid, "") << "a steer's sid is the empty sentinel";
    ca_wire_payload_destroy(CA_PROMPT_RESPONSE, p);
  }

  /* The durable record: the live channel delivers the user msg.append. */
  bool saw = false;
  for (int i = 0; i < 600 && !saw; i++) {
    size_t n = conn_count(&conn);
    for (size_t j = 2; j < n && !saw; j++) {
      void* p = NULL;
      uint64_t t = 0, rid = 0;
      uint8_t st = 0;
      if (!conn_decode(&conn, j, &t, &p, &rid, &st)) break;
      if (t != (uint64_t)CA_EVENTS_RESPONSE) {
        ca_wire_payload_destroy(t, p);
        continue;
      }
      ca_events_response_t* ev = (ca_events_response_t*)p;
      if (ev->seq > 0 && ev->record_json != NULL &&
          strstr(ev->record_json, "the steer") != NULL) {
        json_value_t* rec = json_parse(ev->record_json,
                                       strlen(ev->record_json), NULL);
        ASSERT_NE(rec, nullptr) << "the record's JSON: " << ev->record_json;
        EXPECT_STREQ(json_as_string(json_get(rec, "type")), "msg.append");
        json_value_t* rp = json_get(rec, "payload");
        ASSERT_NE(rp, nullptr);
        EXPECT_STREQ(json_as_string(json_get(rp, "role")), "user");
        EXPECT_STREQ(json_as_string(json_get(rp, "content")), "the steer");
        json_value_destroy(rec);
        saw = true;
      }
      ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
    }
    if (!saw) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(saw) << "the steer's msg.append user record arrived live";

  fixture_teardown(&fx);
  /* The ref discipline proved: the server released every connection ref. */
  EXPECT_EQ(conn.refs.load(), 0)
      << "the server's held connection references all released";
  conn_double_destroy(&conn);
}

TEST(TestClientApiHandlers, TestEventsReplaysTheCursorAndSignalsLive) {
  /* The cursor's RESUME-AT semantics + the live marker + the tail: three
     steered messages commit; a REPLAY from seq 1 delivers EXACTLY seqs
     2..3 ("two","three") — the 1-record's exclusion — then the marker; a
     FOURTH steer lands as the live tail. Pin the ordering by req_id (the
     replay frames all carry the subscribing request's id). */
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  conn_double_t conn;
  conn_double_init(&conn);

  std::string sid = prompt_create_and_sid(&fx, &conn, 1, "cursor replay");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  /* The LIVE-ONLY channel: the three steers commit through it (this makes
     the replay's precondition deterministic — they are committed BEFORE
     the replay subscription below). */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(2, sid.c_str(), CA_EVENTS_LIVE_ONLY, 0),
                    &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, 2, 600)) << "the live marker";
  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(3, sid.c_str(), "one"), &conn.iface);
  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(4, sid.c_str(), "two"), &conn.iface);
  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(5, sid.c_str(), "three"), &conn.iface);
  /* Learn the steers' actual seqs off the live tail (the engine's own
     lifecycle riders share the frame's seq space — the numbers are not
     assumed, the CONTENTS' exclusion is the pin). */
  long long seq_one = -1, seq_two = -1, seq_three = -1;
  bool saw_three_live = false;
  for (int i = 0; i < 600 && !saw_three_live; i++) {
    size_t n = conn_count(&conn);
    saw_three_live = false;
    for (size_t j = 0; j < n; j++) {
      void* p = NULL;
      uint64_t t = 0, rid = 0;
      uint8_t st = 0;
      if (!conn_decode(&conn, j, &t, &p, &rid, &st)) break;
      if (t == (uint64_t)CA_EVENTS_RESPONSE) {
        ca_events_response_t* ev = (ca_events_response_t*)p;
        if (ev->seq > 0 && ev->record_json != NULL) {
          json_value_t* rec = json_parse(ev->record_json,
                                         strlen(ev->record_json), NULL);
          if (rec != NULL) {
            std::string content =
                json_as_string(json_get(json_get(rec, "payload"), "content"));
            const char* type_v = json_as_string(json_get(rec, "type"));
            if (strcmp(type_v, "msg.append") == 0 && content == "one") {
              seq_one = (long long)ev->seq;
            } else if (strcmp(type_v, "msg.append") == 0 && content == "two") {
              seq_two = (long long)ev->seq;
            } else if (strcmp(type_v, "msg.append") == 0 && content == "three") {
              seq_three = (long long)ev->seq;
              saw_three_live = true;
            }
            json_value_destroy(rec);
          }
        }
        ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
      } else {
        ca_wire_payload_destroy(t, p);
      }
    }
    if (!saw_three_live) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ASSERT_TRUE(saw_three_live) << "the three steers committed through the tail";
  ASSERT_GT(seq_one, 0);
  ASSERT_GT(seq_two, seq_one) << "the seq space's order is the log's";
  ASSERT_GT(seq_three, seq_two);

  /* Unsubscribe (the live sub's teardown): ONE terminal marker, op echoed,
     req_id 6. */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(6, sid.c_str(), CA_EVENTS_UNSUBSCRIBE, 0),
                    &conn.iface);
  bool saw_unsub_marker = false;
  size_t unsub_at = (size_t)-1;
  for (int i = 0; i < 600 && !saw_unsub_marker; i++) {
    size_t n = conn_count(&conn);
    for (size_t j = 0; j < n && !saw_unsub_marker; j++) {
      void* p = NULL;
      uint64_t t = 0, rid = 0;
      uint8_t st = 0;
      if (!conn_decode(&conn, j, &t, &p, &rid, &st)) break;
      if (t == (uint64_t)CA_EVENTS_RESPONSE &&
          ((ca_events_response_t*)p)->seq == 0 &&
          ((ca_events_response_t*)p)->op == (uint8_t)CA_EVENTS_UNSUBSCRIBE) {
        saw_unsub_marker = true;
        unsub_at = j;
        ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
        break;
      }
      if (t == (uint64_t)CA_EVENTS_RESPONSE) {
        ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
      } else {
        ca_wire_payload_destroy(t, p);
      }
    }
    if (!saw_unsub_marker) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ASSERT_TRUE(saw_unsub_marker) << "the unsub's terminal marker";

  /* The REPLAY from the FIRST steer's seq (resume-at): EXACTLY its two
     later records then the marker, under ONE req_id (7) — everything the
     double records from here on IS this channel's. */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(7, sid.c_str(), CA_EVENTS_REPLAY_THEN_LIVE,
                               (uint64_t)seq_one),
                    &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, unsub_at + 3, 600))
      << "the replay (2 records + marker) streamed under req_id 7";
  {
    std::vector<uint64_t> seqs;
    for (size_t j = unsub_at + 1;
         j < conn_count(&conn) && seqs.size() < 3; j++) {
      void* p = NULL;
      uint64_t t = 0, rid = 0;
      uint8_t st = 0;
      ASSERT_TRUE(conn_decode(&conn, j, &t, &p, &rid, &st));
      ASSERT_EQ(t, (uint64_t)CA_EVENTS_RESPONSE);
      ASSERT_EQ(rid, 7u);
      ca_events_response_t* ev = (ca_events_response_t*)p;
      if (ev->seq == 0) {
        EXPECT_EQ(ev->op, (uint8_t)CA_EVENTS_REPLAY_THEN_LIVE)
            << "the live marker echoes the replay's op";
        EXPECT_EQ(ev->record_json, nullptr);
        seqs.push_back(0);
      } else {
        json_value_t* rec = json_parse(ev->record_json,
                                       strlen(ev->record_json), NULL);
        ASSERT_NE(rec, nullptr) << "raw: " << ev->record_json;
        std::string content =
            json_as_string(json_get(json_get(rec, "payload"), "content"));
        json_value_destroy(rec);
        if ((long long)ev->seq == seq_two) EXPECT_EQ(content, "two");
        else if ((long long)ev->seq == seq_three) EXPECT_EQ(content, "three");
        else ADD_FAILURE() << "unexpected replay seq " << ev->seq
                           << " content '" << content << "'";
        seqs.push_back(ev->seq);
      }
      ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
    }
    ASSERT_EQ(seqs.size(), 3u) << "EXACTLY the 2..3 records + the marker";
    EXPECT_EQ(seqs[0], (uint64_t)seq_two);
    EXPECT_EQ(seqs[1], (uint64_t)seq_three);
    EXPECT_EQ(seqs[2], 0u)
        << "the seq-one record is EXCLUDED (from_seq = resume-at) and the "
           "marker closes the replay";
  }

  /* The live tail after the marker: the FOURTH steer's record arrives. */
  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(8, sid.c_str(), "four"), &conn.iface);
  bool saw_four = false;
  for (int i = 0; i < 600 && !saw_four; i++) {
    size_t n = conn_count(&conn);
    for (size_t j = unsub_at; j < n && !saw_four; j++) {
      void* p = NULL;
      uint64_t t = 0, rid = 0;
      uint8_t st = 0;
      if (!conn_decode(&conn, j, &t, &p, &rid, &st)) break;
      if (t == (uint64_t)CA_EVENTS_RESPONSE) {
        ca_events_response_t* ev = (ca_events_response_t*)p;
        if (rid == 7u && ev->seq > 0 && ev->record_json != NULL &&
            strstr(ev->record_json, "\"four\"") != NULL) {
          saw_four = true;
        }
        ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
      } else {
        ca_wire_payload_destroy(t, p);
      }
    }
    if (!saw_four) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(saw_four) << "the post-marker commit arrives as the live tail";

  fixture_teardown(&fx);
  /* The ref discipline proved: the server released every connection ref. */
  EXPECT_EQ(conn.refs.load(), 0)
      << "the server's held connection references all released";
  conn_double_destroy(&conn);
}

#if defined(SA_HAS_PYTHON)

/* test_loop.cpp's same shape: py_agent_init through a bare extern —
   pulling py_agent.h would demand <Python.h> here. */
extern "C" void py_agent_init(void);

TEST(TestClientApiHandlers, TestInterruptReachesTheFrame) {
  /* A POOLED frame mid-cell (the watchdog tests' shape — a sleeping pyrt
     cell): the wire's INTERRUPT posts (status 0) and the synthesis lands —
     the durable turn.end {aborted ... interrupted}. The shared backend
     (the create's injection) makes turn 1 deterministic. */
  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import time\\nprint('started')\\ntime.sleep(3)\"}"}}]}}]})json";
  std::vector<std::string> replies = {turn1};
  wire_scripted_t sm;
  memset(&sm, 0, sizeof(sm));
  sm.base.complete = wire_scripted_complete;
  sm.replies = &replies;
  py_agent_init();

  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, &sm.base), 0);
  conn_double_t conn;
  conn_double_init(&conn);

  std::string sid = prompt_create_and_sid(&fx, &conn, 1, "interrupt me");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  /* Give the cell its run: the pyrt boot + the sleep's first phase. */
  std::this_thread::sleep_for(std::chrono::milliseconds(600));

  ca_session_handle(fx.server, CA_INTERRUPT_REQUEST,
                    interrupt_req(2, sid.c_str()), &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, 2, 600));
  {
    void* p = NULL;
    uint64_t t = 0, rid = 0;
    uint8_t st = 0;
    ASSERT_TRUE(conn_decode(&conn, 1, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_INTERRUPT_RESPONSE);
    ASSERT_EQ(rid, 2u);
    ca_interrupt_response_t* res = (ca_interrupt_response_t*)p;
    EXPECT_EQ(res->status, 0u) << "the interrupt POSTED into the mailbox";
    ca_wire_payload_destroy(CA_INTERRUPT_RESPONSE, p);
  }

  /* The synthesis's durable shape (the surface slice's contract over the
     wire): the corr-matched status-1 cell.result + the open turn's aborted
     turn.end. Scan the frame's log through the debug accessor. */
  frame_t* f = ca_session_server_frame(fx.server, sid.c_str());
  ASSERT_NE(f, nullptr);
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

  fixture_teardown(&fx);
  /* The ref discipline proved: the server released every connection ref. */
  EXPECT_EQ(conn.refs.load(), 0)
      << "the server's held connection references all released";
  conn_double_destroy(&conn);
}

#endif /* SA_HAS_PYTHON */

TEST(TestClientApiHandlers, TestUnknownSidAnswersError) {
  /* The wire's closed vocabulary at the handler: unknown sids refuse ONE
     CA_ERROR each with the req_id echo; a NULL-sid events request (the
     hand-crafted shape — "" decodes to the absent sentinel) refuses too;
     a no-op unsubscribe refuses. */
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  conn_double_t conn;
  conn_double_init(&conn);

  const char* ghost = "sessions/0000000000000000000deadbeef";
  ca_session_handle(fx.server, CA_INTERRUPT_REQUEST,
                    interrupt_req(11, ghost), &conn.iface);
  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(12, ghost, "a steer to nowhere"), &conn.iface);
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(13, ghost, CA_EVENTS_REPLAY_THEN_LIVE, 0),
                    &conn.iface);
  /* The hand-crafted NULL-sid events request (the wire decodes an absent
     sid to NULL — a handler-level refusal even then). */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(14, NULL, CA_EVENTS_LIVE_ONLY, 0), &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, 4, 600)) << "the four refusals";

  for (size_t j = 0; j < 4; j++) {
    void* p = NULL;
    uint64_t t = 0, rid = 0;
    uint8_t st = 0;
    ASSERT_TRUE(conn_decode(&conn, j, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_ERROR) << "frame j=" << j;
    ca_error_t* err = (ca_error_t*)p;
    EXPECT_EQ(rid, 11u + j) << "the req_id echoes";
    if (j < 3) {
      EXPECT_EQ(err->status, 2u) << "the unknown-session status";
    } else {
      EXPECT_EQ(err->status, 1u) << "the hand-crafted shape's refusal status";
    }
    ASSERT_NE(err->text, nullptr);
    EXPECT_FALSE(std::string(err->text).empty());
    ca_wire_payload_destroy(CA_ERROR, p);
  }

  /* The no-op unsubscribe on a REAL session: refused loud. */
  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(20, NULL, "real session"), &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, 5, 600));
  {
    void* p = NULL;
    uint64_t t = 0, rid = 0;
    uint8_t st = 0;
    ASSERT_TRUE(conn_decode(&conn, 4, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_PROMPT_RESPONSE);
    std::string sid = ((ca_prompt_response_t*)p)->sid;
    ca_wire_payload_destroy(CA_PROMPT_RESPONSE, p);
    ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                      events_req(21, sid.c_str(), CA_EVENTS_UNSUBSCRIBE, 0),
                      &conn.iface);
    ASSERT_TRUE(conn_wait_count(&conn, 6, 600));
    ASSERT_TRUE(conn_decode(&conn, 5, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_ERROR)
        << "a no-op unsubscribe is the peer's mistake — refused loud";
    ASSERT_EQ(rid, 21u);
    ca_wire_payload_destroy(CA_ERROR, p);
  }

  fixture_teardown(&fx);
  /* The ref discipline proved: the server released every connection ref. */
  EXPECT_EQ(conn.refs.load(), 0)
      << "the server's held connection references all released";
  conn_double_destroy(&conn);
}

TEST(TestClientApiHandlers, TestSteerOnADoneFrameRefuses) {
  /* A done session accepts no steer and no interrupt — CA_ERROR loud, the
     req_id echoing. The frame runs to done through the shared scripted
     backend's content turn. */
  std::string content_turn =
      R"json({"choices":[{"message":{"role":"assistant","content":"quiet end"}}]})json";
  std::vector<std::string> replies = {content_turn};
  wire_scripted_t sm;
  memset(&sm, 0, sizeof(sm));
  sm.base.complete = wire_scripted_complete;
  sm.replies = &replies;

  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, &sm.base), 0);
  conn_double_t conn;
  conn_double_init(&conn);

  std::string sid = prompt_create_and_sid(&fx, &conn, 1, "finish quickly");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  frame_t* f = ca_session_server_frame(fx.server, sid.c_str());
  ASSERT_NE(f, nullptr);
  for (int i = 0; i < 800 && frame_is_done(f) == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(frame_is_done(f), 1u) << "the scripted content turn completed";

  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(2, sid.c_str(), "a steer after the end"),
                    &conn.iface);
  ca_session_handle(fx.server, CA_INTERRUPT_REQUEST,
                    interrupt_req(3, sid.c_str()), &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, 3, 600));
  for (size_t j = 1; j < 3; j++) {
    void* p = NULL;
    uint64_t t = 0, rid = 0;
    uint8_t st = 0;
    ASSERT_TRUE(conn_decode(&conn, j, &t, &p, &rid, &st));
    ASSERT_EQ(t, (uint64_t)CA_ERROR)
        << "a done frame refuses loud (frame j=" << j << ")";
    ASSERT_EQ(rid, 1u + j) << "the req_id echoes";
    ca_error_t* err = (ca_error_t*)p;
    EXPECT_EQ(err->status, 2u);
    EXPECT_NE(std::string(err->text).find("done"), std::string::npos)
        << "the refusal tells the truth: " << err->text;
    ca_wire_payload_destroy(CA_ERROR, p);
  }

  fixture_teardown(&fx);
  /* The ref discipline proved: the server released every connection ref. */
  EXPECT_EQ(conn.refs.load(), 0)
      << "the server's held connection references all released";
  conn_double_destroy(&conn);
}

TEST(TestClientApiHandlers, TestServerActorSurvivesFrameDone) {
  /* The holding rule: the registry entry STAYS after frame_is_done — the
     events channel still replays a done session's records through the
     wire (and the listing keeps reporting it), while nothing re-runs. */
  std::string content_turn =
      R"json({"choices":[{"message":{"role":"assistant","content":"kept the log"}}]})json";
  std::vector<std::string> replies = {content_turn};
  wire_scripted_t sm;
  memset(&sm, 0, sizeof(sm));
  sm.base.complete = wire_scripted_complete;
  sm.replies = &replies;

  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, &sm.base), 0);
  conn_double_t conn;
  conn_double_init(&conn);

  std::string sid = prompt_create_and_sid(&fx, &conn, 1, "survive my end");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  frame_t* f = ca_session_server_frame(fx.server, sid.c_str());
  ASSERT_NE(f, nullptr);
  for (int i = 0; i < 800 && frame_is_done(f) == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(frame_is_done(f), 1u);

  /* The registry entry survives; the events channel replays the DONE
     session's records + signals the live marker — no CA_ERROR. */
  EXPECT_NE(ca_session_server_frame(fx.server, sid.c_str()), nullptr)
      << "a done frame's runtime object stays (the server owns the frames)";
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(2, sid.c_str(), CA_EVENTS_REPLAY_THEN_LIVE, 0),
                    &conn.iface);
  bool saw_marker = false;
  bool saw_record = false;
  for (int i = 0; i < 600 && !(saw_marker && saw_record); i++) {
    size_t n = conn_count(&conn);
    for (size_t j = 1; j < n; j++) {
      void* p = NULL;
      uint64_t t = 0, rid = 0;
      uint8_t st = 0;
      if (!conn_decode(&conn, j, &t, &p, &rid, &st)) break;
      ASSERT_NE(t, (uint64_t)CA_ERROR)
          << "a done session's events channel still works";
      if (t == (uint64_t)CA_EVENTS_RESPONSE) {
        ca_events_response_t* ev = (ca_events_response_t*)p;
        if (ev->seq == 0) saw_marker = true;
        else saw_record = true;
        ca_wire_payload_destroy(CA_EVENTS_RESPONSE, p);
      } else {
        ca_wire_payload_destroy(t, p);
      }
    }
    if (!(saw_marker && saw_record)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  EXPECT_TRUE(saw_record) << "the done session's records replay";
  EXPECT_TRUE(saw_marker) << "the live marker still signals (the tail is "
                             "quiet forever, but the marker is the truth)";

  /* And the SESSIONS listing reports the completed session. */
  ca_session_handle(fx.server, CA_SESSIONS_REQUEST, sessions_req(3),
                    &conn.iface);
  bool saw_listing = false;
  for (int i = 0; i < 600 && !saw_listing; i++) {
    size_t n = conn_count(&conn);
    for (size_t j = 1; j < n && !saw_listing; j++) {
      void* p = NULL;
      uint64_t t = 0, rid = 0;
      uint8_t st = 0;
      if (!conn_decode(&conn, j, &t, &p, &rid, &st)) break;
      if (t == (uint64_t)CA_SESSIONS_RESPONSE && rid == 3u) {
        ca_sessions_response_t* l = (ca_sessions_response_t*)p;
        ASSERT_EQ(l->nrecords, 1u);
        EXPECT_STREQ(l->records[0].sid, sid.c_str());
        EXPECT_STREQ(l->records[0].status, "done");
        ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, p);
        saw_listing = true;
        break;
      }
      if (t == (uint64_t)CA_SESSIONS_RESPONSE) {
        ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, p);
      } else {
        ca_wire_payload_destroy(t, p);
      }
    }
    if (!saw_listing) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(saw_listing) << "the done session's listing row";

  fixture_teardown(&fx);
  /* The ref discipline proved: the server released every connection ref. */
  EXPECT_EQ(conn.refs.load(), 0)
      << "the server's held connection references all released";
  conn_double_destroy(&conn);
}

TEST(TestClientApiHandlers, TestDuplicateSubscribeRefused) {
  /* The zombie's fix pinned: a same-(conn, sid) re-subscribe while one
     exists refuses ONE CA_ERROR ("already subscribed") — under the old
     shape the second sub joined the list and the scan reply's
     `_ca_sub_find` routed to the OLD head, so the new channel never went
     live while holding notices until destroy. NOTHING rides the refused
     req_id, and the ORIGINAL channel keeps working. */
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  conn_double_t conn;
  conn_double_init(&conn);

  std::string sid = prompt_create_and_sid(&fx, &conn, 1, "one channel per pair");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  /* The FIRST subscribe is mid-replay when the second arrives (back to
     back, no waits — the exact zombie window: the first sub exists, its
     scan is in flight). */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(2, sid.c_str(), CA_EVENTS_REPLAY_THEN_LIVE, 0),
                    &conn.iface);
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(3, sid.c_str(), CA_EVENTS_REPLAY_THEN_LIVE, 0),
                    &conn.iface);
  ASSERT_TRUE(conn_wait_count(&conn, 3, 600)) << "the refusal answers";
  {
    /* The refusal found BY req_id, not by position — the original's replay
       records may stream around it. */
    bool saw = false;
    for (size_t j = 1; j < conn_count(&conn) && !saw; j++) {
      void* p = NULL;
      uint64_t t = 0, rid = 0;
      uint8_t st = 0;
      ASSERT_TRUE(conn_decode(&conn, j, &t, &p, &rid, &st));
      if (t == (uint64_t)CA_ERROR && rid == 3u) {
        ca_error_t* err = (ca_error_t*)p;
        ASSERT_NE(err->text, nullptr);
        EXPECT_NE(std::string(err->text).find("already subscribed"),
                  std::string::npos)
            << "the refusal tells the truth: " << err->text;
        saw = true;
      }
      ca_wire_payload_destroy(t, p);
    }
    ASSERT_TRUE(saw) << "the re-subscribe refuses loud (one CA_ERROR, "
                        "req_id echoed)";
  }

  /* The FIRST sub's replay + marker ride ITS req_id; the refused req_id 3
     carries NOTHING — no zombie channel, no second marker ever. */
  ASSERT_TRUE(chan_wait_marker(&conn, 2, CA_EVENTS_REPLAY_THEN_LIVE, 600))
      << "the original channel went live";
  for (const chan_ev_t& e : conn_channel(&conn, 3)) {
    ADD_FAILURE() << "the refused re-subscribe delivered a frame (seq "
                  << e.seq << ") — the zombie lives";
  }

  /* The original channel's live tail: the post-refusal steer arrives under
     req_id 2 exactly once. */
  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(4, sid.c_str(), "still alive"), &conn.iface);
  ASSERT_TRUE(chan_wait_text(&conn, 2, "still alive", 600))
      << "the original channel still delivers";
  {
    size_t n = 0;
    for (const chan_ev_t& e : conn_channel(&conn, 2)) {
      if (e.steer_content == "still alive") n++;
    }
    EXPECT_EQ(n, 1u) << "exactly one delivery on the surviving channel";
  }

  /* After the unsubscribe the pair is free again: a fresh subscribe is
     LEGAL (the refusal binds only while a sub exists). */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(5, sid.c_str(), CA_EVENTS_UNSUBSCRIBE, 0),
                    &conn.iface);
  ASSERT_TRUE(chan_wait_marker(&conn, 5, CA_EVENTS_UNSUBSCRIBE, 600))
      << "the unsub's terminal marker";
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(6, sid.c_str(), CA_EVENTS_LIVE_ONLY, 0),
                    &conn.iface);
  ASSERT_TRUE(chan_wait_marker(&conn, 6, CA_EVENTS_LIVE_ONLY, 600))
      << "the fresh subscribe is legal after the pair freed";
  for (const chan_ev_t& e : conn_channel(&conn, 3)) {
    ADD_FAILURE() << "a late zombie frame (seq " << e.seq << ")";
  }

  fixture_teardown(&fx);
  /* The ref discipline proved: the refusal released its own conn ref — the
     server holds nothing for the refused channel. */
  EXPECT_EQ(conn.refs.load(), 0)
      << "the server's held connection references all released";
  conn_double_destroy(&conn);
}

TEST(TestClientApiHandlers, TestConnClosedMidReplayPurges) {
  /* A connection closing while its replay is still streaming: the purge
     drops the subscription (and any parked holds) — a subsequent live
     commit delivers NOTHING on the closed double, the refs balance, and
     the survivors' channels keep running (the unwatch correctly skips —
     another subscriber remains on the sid). */
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  conn_double_t a, b;
  conn_double_init(&a);
  conn_double_init(&b);

  std::string sid = prompt_create_and_sid(&fx, &a, 1, "the replay purge");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  /* A's live channel anchors the watch at the store; the bulk steers
     commit through it — the log B's replay will carry is LARGE (40 user
     records plus the turns' own riders). */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(2, sid.c_str(), CA_EVENTS_LIVE_ONLY, 0),
                    &a.iface);
  ASSERT_TRUE(conn_wait_count(&a, 2, 600)) << "the live marker";
  for (int i = 0; i < 40; i++) {
    char text[32];
    snprintf(text, sizeof(text), "bulk-%02d", i);
    ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                      prompt_req(10 + (uint64_t)i, sid.c_str(), text),
                      &a.iface);
    ASSERT_TRUE(chan_wait_text(&a, 2, text, 600)) << "steer " << text;
  }

  /* B subscribes the FULL replay and closes MID-REPLAY — the purge closure
     enqueues immediately after the work closure, before any post-close
     commit's notice can. Whichever way the scan reply races the purge
     (routed first or routing to a dead sub), the outcome below is the
     same shape. */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(200, sid.c_str(), CA_EVENTS_REPLAY_THEN_LIVE, 0),
                    &b.iface);
  ca_session_conn_closed(fx.server, &b.iface);

  /* The teardown settles; every trace of B is gone and every held conn ref
     released (the sub's, the pending round trip's, the closure's own). */
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ASSERT_EQ(b.refs.load(), 0)
      << "the mid-replay teardown leaves no connection ref behind";
  const size_t b_settled = conn_count(&b);

  /* The proof: a commit AFTER the close reaches A (the machinery runs; the
     purge's unwatch correctly skipped — A is still subscribed) and reaches
     B with NOTHING — the subscription is gone, no zombie, no held
     notices. */
  ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                    prompt_req(300, sid.c_str(), "postclose"), &a.iface);
  ASSERT_TRUE(chan_wait_text(&a, 2, "postclose", 600))
      << "the surviving subscriber keeps receiving";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(conn_count(&b), b_settled)
      << "a post-close commit delivers nothing on the closed connection";

  /* The log really was large (the replay's honest subject): A carried all
     forty user records. */
  {
    size_t n = 0;
    for (const chan_ev_t& e : conn_channel(&a, 2)) {
      if (e.steer_content.rfind("bulk-", 0) == 0) n++;
    }
    ASSERT_GE(n, 40u) << "the replay's subject is a loaded log";
  }

  fixture_teardown(&fx);
  /* The ref discipline proved (the fixture's existing backstop). */
  EXPECT_EQ(a.refs.load(), 0);
  EXPECT_EQ(b.refs.load(), 0);
  conn_double_destroy(&a);
  conn_double_destroy(&b);
}

TEST(TestClientApiHandlers, TestHoldGapFlushesRacingCommitsExactlyOnce) {
  /* The replay-gap buffer's honest pin, ten rounds: commits racing a fresh
     REPLAY subscription (fired unawaited just before it joins) land in
     every position the buffer exists for — already in the store's scan
     (replayed), racing the watch→scan adjacency (HELD, flushed
     post-replay), or after the live marker (live tail) — and the channel's
     contract is exact: every covered record EXACTLY ONCE, strictly
     ascending by seq, one marker, the unsubscribe releases the pair for
     the next round.
     THE HOLD-CAP FINDING (recorded here because this suite must tell the
     truth): reaching _CA_HOLD_MAX's overflow (> 256 pre-live notices) is
     NOT honestly constructible — the hold window is bounded by the
     store's scan round trip, and the store's FIFO keeps racing commits
     either in the replay or behind the reply; a synthetic hook (an
     exported test hold-injector) was refused. The overflow branch stays
     the code's defensive floor — and a no-loss floor: any record the
     cap drops is by construction also in the replay (a held record always
     committed before the scan read), so the dedup'd channel never loses
     it. */
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx, NULL), 0);
  conn_double_t a, b;
  conn_double_init(&a);
  conn_double_init(&b);

  std::string sid = prompt_create_and_sid(&fx, &a, 1, "the hold gap");
  ASSERT_EQ(sid.rfind("sessions/", 0), 0u);

  /* A's live channel anchors the store's watch from the start — the
     notices exist for every commit that follows. */
  ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                    events_req(2, sid.c_str(), CA_EVENTS_LIVE_ONLY, 0),
                    &a.iface);
  ASSERT_TRUE(conn_wait_count(&a, 2, 600)) << "the live marker";

  for (int round = 0; round < 10; round++) {
    const uint64_t rid = 100 + (uint64_t)round;
    const uint64_t from = chan_max_seq(&b);   /* B's own cursor: resume-at */

    /* The burst fires UNAWAITED and the replay joins immediately: the three
       steers race the subscription's scan. */
    char texts[3][32];
    for (int j = 0; j < 3; j++) {
      snprintf(texts[j], sizeof(texts[j]), "race%02d-x%d", round, j);
      ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                        prompt_req(rid * 10 + (uint64_t)j, sid.c_str(),
                                   texts[j]),
                        &a.iface);
    }
    ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                      events_req(rid, sid.c_str(), CA_EVENTS_REPLAY_THEN_LIVE,
                                 from),
                      &b.iface);
    ASSERT_TRUE(chan_wait_marker(&b, rid, CA_EVENTS_REPLAY_THEN_LIVE, 600))
        << "round " << round << ": the live marker";

    /* The live tail after the marker, then the unsub frees the (conn, sid)
       pair for the next round. */
    char tail[32];
    snprintf(tail, sizeof(tail), "tail%02d", round);
    ca_session_handle(fx.server, CA_PROMPT_REQUEST,
                      prompt_req(970 + (uint64_t)round, sid.c_str(), tail),
                      &a.iface);
    ASSERT_TRUE(chan_wait_text(&b, rid, tail, 600))
        << "round " << round << ": the post-marker tail is live";

    /* The channel is final now (the unsub joins after its marker): the
       shape is [ascending records..., ONE marker, ascending tail...] under
       last_seq's gate — and each raced commit delivered EXACTLY ONCE. */
    {
      std::vector<chan_ev_t> ch = conn_channel(&b, rid);
      bool saw_marker = false;
      for (size_t i = 0; i < ch.size(); i++) {
        if (ch[i].seq != 0) {
          ASSERT_GT(ch[i].seq, from)
              << "round " << round << ": the cursor is honored (i " << i
              << ")";
          if (i > 0 && ch[i].seq <= ch[i - 1].seq) {
            ADD_FAILURE() << "round " << round
                          << ": not strictly ascending at i " << i
                          << " (replay then flush, deduplicated)";
          }
        } else {
          ASSERT_FALSE(saw_marker) << "round " << round << ": one marker";
          saw_marker = true;
        }
      }
      ASSERT_TRUE(saw_marker) << "round " << round << ": the marker";
      for (int j = 0; j < 3; j++) {
        size_t n = 0;
        for (const chan_ev_t& e : ch) {
          if (e.steer_content == texts[j]) n++;
        }
        ASSERT_EQ(n, 1u)
            << "round " << round << ": the racing commit '" << texts[j]
            << "' delivered EXACTLY ONCE (replayed, held, or live)";
      }
      size_t n_tail = 0;
      for (const chan_ev_t& e : ch) {
        if (e.steer_content == tail) n_tail++;
      }
      ASSERT_EQ(n_tail, 1u) << "round " << round
                            << ": the tail delivered exactly once";
    }

    ca_session_handle(fx.server, CA_EVENTS_REQUEST,
                      events_req(500 + (uint64_t)round, sid.c_str(),
                                 CA_EVENTS_UNSUBSCRIBE, 0),
                      &b.iface);
    ASSERT_TRUE(chan_wait_marker(&b, 500 + (uint64_t)round,
                                 CA_EVENTS_UNSUBSCRIBE, 600))
        << "round " << round << ": the unsub's terminal marker";
  }

  fixture_teardown(&fx);
  /* The ref discipline proved (the fixture's existing backstop). */
  EXPECT_EQ(a.refs.load(), 0);
  EXPECT_EQ(b.refs.load(), 0);
  conn_double_destroy(&a);
  conn_double_destroy(&b);
}

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */