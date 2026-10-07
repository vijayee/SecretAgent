//
// Created by victor on 10/04/26.
//

/* The typed C client's suite (the plan's Task 7): the REAL stack in-proc —
   pool + store + loop + handlers server + the REAL unix/tcp transports (the
   Task 5/6 fixture machinery) — driven by sa_client over the REAL socket
   (in-proc wiring, real bytes, real framer, real wire). The callbacks are
   recorded under a mutex and poll-waited (the transports suite's discipline:
   no sleeps racing assertions). The release-payload discipline's proofs ride
   the SUITE-LEVEL leak checks: ASan on the reader thread + payload ownership
   surface, valgrind on the binary. */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "../src/ClientLibs/c/sa_client.h"
#include "../src/ClientApi/Unix/unix_transport.h"
#include "../src/ClientApi/Tcp/tcp_transport.h"
#include "../src/ClientApi/client_api_wire.h"
#include "../src/ClientApi/handlers.h"   /* ca_session_server_frame — the
                                            parked ask's registry accessor */
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

#include <cstdlib>
#include <unistd.h>

#define FIXTURE_API_KEY "the-demo-api-key"

/* --- the in-proc fixture (test_client_api_transports.cpp's common shape) --- */

typedef struct {
  scheduler_pool_t* pool;
  streams_loop_thread_t* loop;
  wave_database_root_t* db;
  ca_session_server_t* server;
  unix_transport_t* transport;       /* the unix variant */
  tcp_transport_t* tcp_transport;    /* the TCP variant */
  platform_address_t tcp_addr;
  frame_config_t cfg;
  char socket_path[128];
  char dir_path[120];
} fixture_t;

static int fixture_setup_common(fixture_t* fx, model_backend_t* shared_backend,
                                unsigned escalation_mode) {
  memset(fx, 0, sizeof(*fx));
  memset(&fx->cfg, 0, sizeof(fx->cfg));
  fx->cfg.model_base_url = NULL;   /* a NULL-backend engine fails fast: no
                                      network, deterministic steers */
  fx->cfg.model_api_key = NULL;
  fx->cfg.model_name = "unused";
  fx->cfg.max_depth = 4;
  fx->cfg.escalation_mode = escalation_mode;
  fx->pool = scheduler_pool_create(2);
  if (fx->pool == NULL) return -1;
  scheduler_pool_start(fx->pool);
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;           /* in-memory */
  sc.store_pool = fx->pool;     /* a POOLED store for a POOLED frame */
  fx->db = wave_db_open_config(&sc);
  if (fx->db == NULL) return -2;
  fx->loop = streams_loop_create();
  if (fx->loop == NULL) return -3;
  fx->server = ca_session_server_create(fx->db, fx->pool, fx->loop, &fx->cfg,
                                        shared_backend);
  if (fx->server == NULL) return -4;
  char tmpl[] = "/tmp/sa-ca7-XXXXXX";
  char* dir = mkdtemp(tmpl);
  if (dir == NULL) return -5;
  snprintf(fx->dir_path, sizeof(fx->dir_path), "%s", dir);
  snprintf(fx->socket_path, sizeof(fx->socket_path), "%s/serve.sock", dir);
  return 0;
}

static int fixture_setup(fixture_t* fx) {
  int rc = fixture_setup_common(fx, NULL, FRAME_ESCALATION_FREE);
  if (rc != 0) return rc;
  fx->transport = unix_transport_create(fx->pool, fx->server,
                                        fx->socket_path);
  if (fx->transport == NULL) return -6;
  unix_transport_start(fx->transport);
  return 0;
}

static int fixture_setup_tcp(fixture_t* fx) {
  int rc = fixture_setup_common(fx, NULL, FRAME_ESCALATION_FREE);
  if (rc != 0) return rc;
  char hash[64];
  if (bcrypt_generate(FIXTURE_API_KEY, 4, hash, sizeof(hash)) != 0) return -7;
  fx->tcp_transport = tcp_transport_create(fx->pool, fx->server,
                                           "127.0.0.1", 0, hash, &fx->tcp_addr);
  if (fx->tcp_transport == NULL) return -6;
  tcp_transport_start(fx->tcp_transport);
  return 0;
}

/* The escalated variant (the escalation slice's ask tests): the template
   carries the plan-ask-act mode for every api-created frame, and the
   SHARED scripted backend makes the plan turn deterministic. */
static int fixture_setup_escalated(fixture_t* fx,
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

static uint16_t fixture_tcp_port(const fixture_t* fx) {
  return (fx->tcp_addr.family == PLATFORM_AF_INET6)
             ? fx->tcp_addr.inet6.port : fx->tcp_addr.inet.port;
}

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
  unlink(fx->socket_path);
  rmdir(fx->dir_path);
}

/* --- the callback recorder (mutated on the reader thread for events, the
       caller thread for the request ops; everything under one mutex) -------- */

typedef struct rec_t {
  std::mutex m;
  sa_client_t* client = nullptr;
  /* the prompt responses */
  std::vector<uint8_t> prompt_status;
  std::vector<std::string> prompt_sid;
  /* the interrupt responses */
  std::vector<uint8_t> interrupt_status;
  /* the sessions listings */
  std::vector<size_t> session_count;
  std::vector<std::vector<std::string>> session_sids;
  std::vector<std::vector<std::string>> session_status;
  /* the config responses (the GET's answer / the SET's echo): the absent
     members ride "" here (the recorder's own NULLs→"" normalisation) */
  std::vector<uint8_t> cfg_status;
  std::vector<std::string> cfg_base;
  std::vector<std::string> cfg_key;
  std::vector<std::string> cfg_model;
  int cfg_null_callbacks = 0;   /* the failure deliveries (NULL members) */
  /* the events: one slot per delivered callback (record or marker) */
  std::vector<uint64_t> ev_seq;
  std::vector<uint8_t> ev_op;
  std::vector<std::string> ev_json;   /* "" for the marker's NULL */
  /* the errors */
  std::vector<uint64_t> err_rid;
  std::vector<uint8_t> err_status;
  std::vector<std::string> err_text;
  /* the release-discipline harness: hold_payloads = keep the pointers
     (unreleased-at-destroy), release first, then double-release */
  bool hold_payloads = false;
  std::vector<void*> kept;
  std::atomic<int> attempt_reentry{0};
  std::atomic<int> reentry_attempts{0};
  std::atomic<int> reentry_rc{-2};
  std::atomic<int> reentry_ms{-1};
  /* the ask replies (the escalation slice's op): one slot per completion */
  std::vector<uint8_t> askreply_status;
  /* the ask-reply RE-ENTRY harness (armed by the roundtrip test): the
     events callback's FIRST record delivery calls sa_client_ask_reply —
     the client must refuse it immediately (return -1, no callback) */
  std::atomic<int> attempt_askreentry{0};
  std::atomic<int> askreentry_attempts{0};
  std::atomic<int> askreentry_rc{-2};
  std::atomic<int> askreentry_ms{-1};
} rec_t;

static void rec_release(rec_t* r, void* p) {
  if (p == NULL) return;
  if (r->hold_payloads) {
    r->kept.push_back(p);
  } else {
    sa_client_release_payload(r->client, p);
  }
}

static void rec_prompt(void* ctx, uint8_t status, const char* sid) {
  rec_t* r = (rec_t*)ctx;
  std::lock_guard<std::mutex> g(r->m);
  r->prompt_status.push_back(status);
  r->prompt_sid.push_back(sid ? sid : "");
  rec_release(r, (void*)sid);
}

static void rec_interrupt(void* ctx, uint8_t status) {
  rec_t* r = (rec_t*)ctx;
  std::lock_guard<std::mutex> g(r->m);
  r->interrupt_status.push_back(status);
}

static void rec_askreply(void* ctx, uint8_t status) {
  rec_t* r = (rec_t*)ctx;
  std::lock_guard<std::mutex> g(r->m);
  r->askreply_status.push_back(status);
}

static void rec_sessions(void* ctx, uint8_t status,
                         const sa_client_session_row_t* rows, size_t nrows) {
  rec_t* r = (rec_t*)ctx;
  std::lock_guard<std::mutex> g(r->m);
  (void)status;   /* recorded per-row through the sids; a failure's NULL rows
                     read as an empty vector */
  std::vector<std::string> sids, statuses;
  for (size_t i = 0; i < nrows; i++) {
    sids.push_back(rows[i].sid ? rows[i].sid : "");
    statuses.push_back(rows[i].status ? rows[i].status : "");
    rec_release(r, (void*)rows[i].sid);
    rec_release(r, (void*)rows[i].status);
    rec_release(r, (void*)rows[i].goal);
  }
  rec_release(r, (void*)rows);
  r->session_count.push_back(nrows);
  r->session_sids.push_back(sids);
  r->session_status.push_back(statuses);
}

static void rec_config(void* ctx, uint8_t status, const char* base_url,
                       const char* api_key, const char* model) {
  rec_t* r = (rec_t*)ctx;
  std::lock_guard<std::mutex> g(r->m);
  if (base_url == NULL && api_key == NULL && model == NULL) {
    /* the failure delivery (NULL members — nothing to release) */
    r->cfg_status.push_back(status);
    r->cfg_base.push_back("");
    r->cfg_key.push_back("");
    r->cfg_model.push_back("");
    r->cfg_null_callbacks++;
    return;
  }
  r->cfg_status.push_back(status);
  r->cfg_base.push_back(base_url ? base_url : "");
  r->cfg_key.push_back(api_key ? api_key : "");
  r->cfg_model.push_back(model ? model : "");
  rec_release(r, (void*)base_url);
  rec_release(r, (void*)api_key);
  rec_release(r, (void*)model);
}

static void rec_events(void* ctx, const char* sid, uint64_t seq, uint8_t op,
                       const char* record_json) {
  rec_t* r = (rec_t*)ctx;
  {
    std::lock_guard<std::mutex> g(r->m);
    (void)sid;
    r->ev_seq.push_back(seq);
    r->ev_op.push_back(op);
    r->ev_json.push_back(record_json ? record_json : "");
    rec_release(r, (void*)sid);
    rec_release(r, (void*)record_json);
  }
  /* THE RE-ENTRY (armed by the re-entry test): the first RECORD delivery
     calls a blocking op from inside the callback — on the reader thread.
     The client must refuse it immediately (return -1 with no callback), so
     this measurement lands the moment the call returns. */
  if (r->attempt_reentry.load(std::memory_order_relaxed) == 1 && seq > 0 &&
      r->reentry_attempts.fetch_add(1, std::memory_order_relaxed) == 0) {
    auto t0 = std::chrono::steady_clock::now();
    int rc = sa_client_prompt(r->client, NULL, "the reentry", rec_prompt, r);
    int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    r->reentry_rc.store(rc, std::memory_order_relaxed);
    r->reentry_ms.store(ms, std::memory_order_relaxed);
  }
  /* THE ASK-REPLY's twin (the escalation slice): the re-entry rule covers
     the reply op the same way — refused -1, instantly, no callback. */
  if (r->attempt_askreentry.load(std::memory_order_relaxed) == 1 && seq > 0 &&
      r->askreentry_attempts.fetch_add(1, std::memory_order_relaxed) == 0) {
    auto t0 = std::chrono::steady_clock::now();
    int rc = sa_client_ask_reply(r->client, "sessions/nowhere", "c0ffee12",
                                 0, "too late", rec_askreply, r);
    int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    r->askreentry_rc.store(rc, std::memory_order_relaxed);
    r->askreentry_ms.store(ms, std::memory_order_relaxed);
  }
}

static void rec_error(void* ctx, uint64_t rid, uint8_t status,
                      const char* text) {
  rec_t* r = (rec_t*)ctx;
  std::lock_guard<std::mutex> g(r->m);
  r->err_rid.push_back(rid);
  r->err_status.push_back(status);
  r->err_text.push_back(text ? text : "");
  rec_release(r, (void*)text);
}

static bool wait_for(const std::function<bool()>& pred, int rounds = 1200) {
  for (int i = 0; i < rounds; i++) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

static sa_client_config_t client_config(const fixture_t* fx) {
  sa_client_config_t c = sa_client_config_default();
  c.transport = SA_CLIENT_TRANSPORT_UNIX;
  c.socket_path = fx->socket_path;
  c.error_cb = rec_error;
  return c;   /* error_ctx wired by the caller (the recorder is the ctx) */
}

/* --- the escalation slice's ask tests' machinery -------------------------- */

/* A content-only completion body (test_loop.cpp's canned_content_body
   shape): the plan-ask-act turn's content reply IS the plan — the ladder's
   runtime-authored gate ask parks on its close (NO pyrt anywhere: no cells
   run in the ask fixtures). */
static std::string sa_content_body(const std::string& text) {
  return std::string(
      R"json({"choices":[{"message":{"role":"assistant","content":")json") +
      text + std::string(R"json("}}]})json");
}

/* The scripted backend injected at the server (the handlers' suite's
   wire_scripted_t shape, renamed for this file's namespace). */
typedef struct sa_scripted_t {
  model_backend_t base;
  std::vector<std::string>* replies;
} sa_scripted_t;

static int sa_scripted_decode(const std::string& body,
                              model_reply_t** reply_out, char** error_out) {
  char* err = NULL;
  json_value_t* root = json_parse(body.c_str(), body.size(), &err);
  if (err != NULL) free(err);
  if (root == NULL) {
    *error_out = strdup("sa scripted model: body is not valid JSON");
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
    *error_out = strdup("sa scripted model: no message in choices[0]");
    return -1;
  }
  model_reply_t* r = (model_reply_t*)get_clear_memory(sizeof(model_reply_t));
  json_value_t* content = json_get(message, "content");
  if (content == NULL || json_type(content) == JSON_NULL) {
    r->content = strdup("");
  } else {
    r->content = strdup(json_as_string(content));
  }
  json_value_destroy(root);
  *reply_out = r;
  return 0;
}

static int sa_scripted_complete(void* self, json_value_t* messages,
                                json_value_t* tools, char** raw_out,
                                model_reply_t** reply_out, char** error_out) {
  (void)messages;
  (void)tools;
  (void)raw_out;
  *reply_out = NULL;
  *error_out = NULL;
  sa_scripted_t* sm = (sa_scripted_t*)self;
  if (sm->replies->empty()) {
    *error_out = strdup("sa scripted model: queue empty");
    return -1;
  }
  std::string body = sm->replies->front();
  sm->replies->erase(sm->replies->begin());
  return sa_scripted_decode(body, reply_out, error_out) == 0 ? 0 : -1;
}

/* The parked plan gate's ask id, polled off the registry frame's event log
   (the handlers' accessor — the events-stream is the ask's delivery).
   "" when the ask never landed within the bound. */
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

/* Whether ONE read of the frame's log shows the ask.reply record resolving
   `ask_id` {answer, value}. */
static bool fixture_ask_reply_committed(fixture_t* fx, const std::string& sid,
                                        const std::string& ask_id,
                                        const char* value) {
  frame_t* f = ca_session_server_frame(fx->server, sid.c_str());
  if (f == NULL) return false;
  char* json = frame_debug_events(f);
  if (json == NULL) return false;
  char* err = NULL;
  json_value_t* events = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  free(json);
  if (events == NULL) return false;
  bool found = false;
  for (size_t j = 0; j < json_size(events); j++) {
    json_value_t* rec = json_at(events, j);
    json_value_t* t_v = json_get(rec, "type");
    if (t_v == NULL || strcmp(json_as_string(t_v), "ask.reply") != 0) continue;
    json_value_t* p = json_get(rec, "payload");
    json_value_t* id_v = (p != NULL) ? json_get(p, "askId") : NULL;
    json_value_t* val_v = (p != NULL) ? json_get(p, "value") : NULL;
    json_value_t* d_v = (p != NULL) ? json_get(p, "decision") : NULL;
    if (id_v != NULL && val_v != NULL && d_v != NULL &&
        strcmp(json_as_string(id_v), ask_id.c_str()) == 0 &&
        strcmp(json_as_string(d_v), "answer") == 0 &&
        strcmp(json_as_string(val_v), value) == 0) {
      found = true;
      break;
    }
  }
  json_value_destroy(events);
  return found;
}

/* Polls fixture_ask_reply_committed until the consume lands. */
static bool fixture_wait_reply_committed(fixture_t* fx, const std::string& sid,
                                         const std::string& ask_id,
                                         const char* value, int rounds) {
  for (int i = 0; i < rounds; i++) {
    if (fixture_ask_reply_committed(fx, sid, ask_id, value)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

/* Whether ONE read of the frame's log shows the ask.reply record resolving
   `ask_id` as the REJECT (decision "reject") — the poll's existence probe. */
static bool fixture_ask_reject_recorded(fixture_t* fx, const std::string& sid,
                                        const std::string& ask_id) {
  frame_t* f = ca_session_server_frame(fx->server, sid.c_str());
  if (f == NULL) return false;
  char* json = frame_debug_events(f);
  if (json == NULL) return false;
  char* err = NULL;
  json_value_t* events = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  free(json);
  if (events == NULL) return false;
  bool found = false;
  for (size_t j = 0; j < json_size(events); j++) {
    json_value_t* rec = json_at(events, j);
    json_value_t* t_v = json_get(rec, "type");
    if (t_v == NULL || strcmp(json_as_string(t_v), "ask.reply") != 0) continue;
    json_value_t* p = json_get(rec, "payload");
    json_value_t* id_v = (p != NULL) ? json_get(p, "askId") : NULL;
    json_value_t* d_v = (p != NULL) ? json_get(p, "decision") : NULL;
    if (id_v != NULL && d_v != NULL &&
        strcmp(json_as_string(id_v), ask_id.c_str()) == 0 &&
        strcmp(json_as_string(d_v), "reject") == 0) {
      found = true;
      break;
    }
  }
  json_value_destroy(events);
  return found;
}

/* THE REJECT CONSUME'S SHAPE: the value's null is the NULL ride's terminal
   shape ("" on the wire → absent at the daemon's decode → null in the
   record); the same batch's default-wording user append rides next; and no
   plan-approved control record anywhere (a reject never transitions the
   ladder). */
static void fixture_check_reject_consumed(fixture_t* fx,
                                          const std::string& sid,
                                          const std::string& ask_id) {
  frame_t* f = ca_session_server_frame(fx->server, sid.c_str());
  ASSERT_NE(f, nullptr);
  char* json = frame_debug_events(f);
  ASSERT_NE(json, nullptr);
  char* err = NULL;
  json_value_t* events = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  free(json);
  ASSERT_NE(events, nullptr);
  bool saw_reject = false;
  for (size_t j = 0; j < json_size(events); j++) {
    json_value_t* rec = json_at(events, j);
    json_value_t* t_v = json_get(rec, "type");
    if (t_v == NULL || strcmp(json_as_string(t_v), "ask.reply") != 0) continue;
    json_value_t* p = json_get(rec, "payload");
    json_value_t* id_v = (p != NULL) ? json_get(p, "askId") : NULL;
    json_value_t* d_v = (p != NULL) ? json_get(p, "decision") : NULL;
    if (id_v == NULL || d_v == NULL ||
        strcmp(json_as_string(id_v), ask_id.c_str()) != 0 ||
        strcmp(json_as_string(d_v), "reject") != 0) {
      continue;
    }
    saw_reject = true;
    json_value_t* val_v = json_get(p, "value");
    ASSERT_NE(val_v, nullptr) << "the empty reject renders value null";
    EXPECT_EQ(json_type(val_v), JSON_NULL);
    EXPECT_EQ(json_as_string(val_v), nullptr)
        << "json_as_string on a null value answers NULL (the standing shape)";
    /* the SAME batch's user-side append rides next (ask.reply, then
       msg.append — the two-record reject shape; the plan gate's standing
       default wording on an empty refusal) */
    json_value_t* append = json_at(events, j + 1);
    ASSERT_NE(append, nullptr);
    json_value_t* at_v = json_get(append, "type");
    ASSERT_NE(at_v, nullptr);
    ASSERT_STREQ(json_as_string(at_v), "msg.append");
    json_value_t* ap = json_get(append, "payload");
    json_value_t* role = (ap != NULL) ? json_get(ap, "role") : NULL;
    json_value_t* content = (ap != NULL) ? json_get(ap, "content") : NULL;
    ASSERT_NE(role, nullptr);
    EXPECT_STREQ(json_as_string(role), "user");
    ASSERT_NE(content, nullptr);
    EXPECT_STREQ(json_as_string(content), "Plan rejected: revise and re-propose")
        << "the ladder's standing default wording (no objection text)";
    break;
  }
  ASSERT_TRUE(saw_reject) << "the reject record landed";
  /* no approval control record on a reject (the ladder revisits plan) */
  for (size_t j = 0; j < json_size(events); j++) {
    json_value_t* rec = json_at(events, j);
    json_value_t* t_v = json_get(rec, "type");
    if (t_v == NULL || strcmp(json_as_string(t_v), "control") != 0) continue;
    json_value_t* p = json_get(rec, "payload");
    json_value_t* k = (p != NULL) ? json_get(p, "kind") : NULL;
    EXPECT_TRUE(k == NULL ||
                strcmp(json_as_string(k), "plan-approved") != 0)
        << "no plan-approved control record on a reject";
  }
  json_value_destroy(events);
}

/* Polls fixture_ask_reject_recorded until the consume lands. */
static bool fixture_wait_reject_consumed(fixture_t* fx, const std::string& sid,
                                         const std::string& ask_id,
                                         int rounds) {
  for (int i = 0; i < rounds; i++) {
    if (fixture_ask_reject_recorded(fx, sid, ask_id)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

/* --- the tests (unix) -------------------------------------------------------- */

TEST(TestSaClient, TestPromptRoundTripAndSessionsListing) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);
  rec.client = cl;

  /* the goal: create + start over the socket, the sid comes back */
  ASSERT_EQ(sa_client_prompt(cl, NULL, "make the thing", rec_prompt, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.prompt_sid.empty();
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.prompt_status.back(), 0u);
    EXPECT_EQ(rec.prompt_sid.back().rfind("sessions/", 0), 0u);
    EXPECT_EQ(rec.err_status.size(), 0u);
  }

  /* the steer: queued (status 0, the "" sentinel sid) */
  std::string sid;
  {
    std::lock_guard<std::mutex> g(rec.m);
    sid = rec.prompt_sid.back();
  }
  ASSERT_EQ(sa_client_prompt(cl, sid.c_str(), "the steer", rec_prompt, &rec),
            0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return rec.prompt_status.size() == 2;
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    EXPECT_EQ(rec.prompt_status.back(), 0u);
    EXPECT_STREQ(rec.prompt_sid.back().c_str(), "")
        << "a steer's sid is the empty sentinel";
  }

  /* the listing: the ONE born session, its status the frame's */
  ASSERT_EQ(sa_client_list_sessions(cl, rec_sessions, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.session_sids.empty();
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.session_count.back(), 1u);
    EXPECT_EQ(rec.session_sids.back()[0], sid);
    EXPECT_STREQ(rec.session_status.back()[0].c_str(), "running");
  }

  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

TEST(TestSaClientConfig, TestConfigGetThenSetThenGet) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);

  /* THE GET: the fixture template's truth — base/key absent (the ""/NULL
     sentinel), model the fixture's own default. Status 0, no error channel
     noise. */
  ASSERT_EQ(sa_client_config_get(cl, rec_config, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.cfg_status.empty();
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.cfg_status.back(), 0u);
    EXPECT_STREQ(rec.cfg_base.back().c_str(), "");
    EXPECT_STREQ(rec.cfg_key.back().c_str(), "");
    EXPECT_STREQ(rec.cfg_model.back().c_str(), "unused");
    EXPECT_EQ(rec.err_status.size(), 0u);
  }

  /* THE SET, model only: the absent members unchanged (the echo's base/key
     stay absent; model rides). */
  ASSERT_EQ(sa_client_config_set(cl, NULL, NULL, "gemma4:latest",
                                 rec_config, &rec),
            0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return rec.cfg_status.size() == 2;
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.cfg_status.back(), 0u);
    EXPECT_STREQ(rec.cfg_base.back().c_str(), "");
    EXPECT_STREQ(rec.cfg_key.back().c_str(), "");
    EXPECT_STREQ(rec.cfg_model.back().c_str(), "gemma4:latest");
  }

  /* THE SET, all three members; the GET carries the whole template after. */
  ASSERT_EQ(sa_client_config_set(cl, "http://127.0.0.1:11434", "sk-the-key",
                                 "deepseek-chat", rec_config, &rec),
            0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return rec.cfg_status.size() == 3;
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.cfg_status.back(), 0u);
    EXPECT_STREQ(rec.cfg_base.back().c_str(), "http://127.0.0.1:11434");
    EXPECT_STREQ(rec.cfg_key.back().c_str(), "sk-the-key");
    EXPECT_STREQ(rec.cfg_model.back().c_str(), "deepseek-chat");
  }
  ASSERT_EQ(sa_client_config_get(cl, rec_config, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return rec.cfg_status.size() == 4;
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.cfg_status.back(), 0u);
    EXPECT_STREQ(rec.cfg_base.back().c_str(), "http://127.0.0.1:11434");
    EXPECT_STREQ(rec.cfg_key.back().c_str(), "sk-the-key");
    EXPECT_STREQ(rec.cfg_model.back().c_str(), "deepseek-chat");
  }

  /* THE "" SENTINEL = ABSENT: an empty-string member is NOT a set-to-empty;
     the GET's truth keeps every prior member (the wire's decode rule). */
  ASSERT_EQ(sa_client_config_set(cl, "", "", "gemma4:latest", rec_config, &rec),
            0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return rec.cfg_status.size() == 5;
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.cfg_status.back(), 0u);
    EXPECT_STREQ(rec.cfg_base.back().c_str(), "http://127.0.0.1:11434")
        << "the empty-string base_url decoded ABSENT, not a set-to-empty";
    EXPECT_STREQ(rec.cfg_key.back().c_str(), "sk-the-key");
    EXPECT_STREQ(rec.cfg_model.back().c_str(), "gemma4:latest");
    EXPECT_EQ(rec.cfg_null_callbacks, 0u) << "no failure delivery rode";
    EXPECT_EQ(rec.err_status.size(), 0u);
  }

  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

TEST(TestSaClientConfig, TestNullClientRefusedOutright) {
  ASSERT_EQ(sa_client_config_get(NULL, rec_config, nullptr), -1);
  ASSERT_EQ(
      sa_client_config_set(NULL, "http://x", "k", "m", rec_config, nullptr),
      -1);
}

TEST(TestSaClient, TestSteerAndEventsStreamWithMarkers) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);
  rec.client = cl;

  ASSERT_EQ(sa_client_prompt(cl, NULL, "steer me", rec_prompt, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.prompt_sid.empty();
  }));
  std::string sid;
  {
    std::lock_guard<std::mutex> g(rec.m);
    sid = rec.prompt_sid.back();
  }

  /* the subscription: REPLAY_THEN_LIVE — the create's own records (the
     goal's msg.append) replay FIRST if any, then the live marker (seq 0,
     the op echoed); subscribe BLOCKS until the marker landed */
  ASSERT_EQ(sa_client_subscribe_events(cl, sid.c_str(), rec_events, &rec), 0);
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_GE(rec.ev_seq.size(), 1u);
    EXPECT_EQ(rec.ev_seq.back(), 0u) << "the live marker closes the replay";
    EXPECT_EQ(rec.ev_op.back(), (uint8_t)CA_EVENTS_REPLAY_THEN_LIVE);
    EXPECT_EQ(rec.ev_json.back(), "");
  }

  /* the steer commits a record on the store's log — the live tail delivers
     it through the events callback */
  ASSERT_EQ(sa_client_prompt(cl, sid.c_str(), "the steer", rec_prompt, &rec),
            0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    for (size_t i = 0; i < rec.ev_seq.size(); i++) {
      if (rec.ev_seq[i] > 0 &&
          rec.ev_json[i].find("the steer") != std::string::npos) {
        return true;
      }
    }
    return false;
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    bool saw = false;
    for (size_t i = 0; i < rec.ev_seq.size(); i++) {
      if (rec.ev_seq[i] > 0 &&
          rec.ev_json[i].find("the steer") != std::string::npos) {
        saw = true;
      }
    }
    EXPECT_TRUE(saw)
        << "the store record's JSON rides VERBATIM through the callback";
  }

  /* the unsubscribe: the terminal marker (op echoed UNSUBSCRIBE) ends the
     sub — delivered through the SAME events callback */
  ASSERT_EQ(sa_client_unsubscribe_events(cl), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    for (size_t i = 0; i < rec.ev_op.size(); i++) {
      if (rec.ev_op[i] == (uint8_t)CA_EVENTS_UNSUBSCRIBE) return true;
    }
    return false;
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    bool seen = false;
    for (size_t i = 0; i < rec.ev_op.size(); i++) {
      if (rec.ev_op[i] == (uint8_t)CA_EVENTS_UNSUBSCRIBE) {
        EXPECT_EQ(rec.ev_seq[i], 0u) << "the terminal marker's seq";
        EXPECT_EQ(rec.ev_json[i], "");
        seen = true;
      }
    }
    EXPECT_TRUE(seen);
  }

  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

/* THE RE-ENTRY (the header's blocking-op contract): an events callback runs
   on the reader thread — a blocking op it calls can never see its response
   routed (a guaranteed TIMEOUT over a daemon that actually committed). The
   client refuses the call IMMEDIATELY (return -1, no callback fires) and the
   subscription keeps flowing. */
TEST(TestSaClient, TestEventsCallbackReentryRefusedImmediately) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);
  rec.client = cl;

  /* the session the watcher subscribes to */
  ASSERT_EQ(sa_client_prompt(cl, NULL, "the reentry watcher", rec_prompt,
                             &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.prompt_sid.empty();
  }));
  std::string sid;
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.prompt_status.size(), 1u);
    sid = rec.prompt_sid.back();
  }

  /* the subscription goes live; the steer's record delivery is the callback
     that calls back */
  rec.attempt_reentry.store(1);
  ASSERT_EQ(sa_client_subscribe_events(cl, sid.c_str(), rec_events, &rec), 0);

  /* the steer commits a record; its events callback calls sa_client_prompt
     — which must REFUSE immediately (~0 ms) with no prompt callback */
  ASSERT_EQ(sa_client_prompt(cl, sid.c_str(), "the steer under the reentry",
                             rec_prompt, &rec), 0);
  ASSERT_TRUE(wait_for([&] { return rec.reentry_rc.load() == -1; }))
      << "an events callback's sa_client_prompt refuses immediately";
  {
    std::lock_guard<std::mutex> g(rec.m);
    EXPECT_LT(rec.reentry_ms.load(), 100)
        << "the refusal is microseconds, not a request-timeout stall";
    ASSERT_EQ(rec.prompt_status.size(), 2u)
        << "the create + the steer only — the reentry ran NO callback";
    EXPECT_EQ(rec.prompt_status[0], 0u);
    EXPECT_EQ(rec.prompt_status[1], 0u);
    EXPECT_EQ(rec.err_status.size(), 0u)
        << "the refusal returns -1 silently (no error channel either)";
  }
  /* The trigger record's DELIVERY raced the assert under valgrind's
     slowdown: the refusal's wait returns on the first post-arm record (a
     replayed one may carry it), while the steer's msg.append commits
     milliseconds later on a slowed reader. Wait for the record itself. */
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    for (size_t i = 0; i < rec.ev_json.size(); i++) {
      if (rec.ev_seq[i] > 0 &&
          rec.ev_json[i].find("steer under the reentry") != std::string::npos) {
        return true;
      }
    }
    return false;
  }))
      << "the record whose callback attempted the reentry was delivered";

  /* THE SUBSCRIPTION CONTINUES past the refusal: the unsubscribe's terminal
     marker still rides the same events callback */
  ASSERT_EQ(sa_client_unsubscribe_events(cl), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    for (size_t i = 0; i < rec.ev_op.size(); i++) {
      if (rec.ev_op[i] == (uint8_t)CA_EVENTS_UNSUBSCRIBE) return true;
    }
    return false;
  }));

  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

TEST(TestSaClient, TestReleasePayloadDiscipline) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);
  rec.client = cl;

  /* the goal: its sid payload stays HELD (hold_payloads) — the
     unreleased-at-destroy shape */
  rec.hold_payloads = true;
  ASSERT_EQ(sa_client_prompt(cl, NULL, "hold me", rec_prompt, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.prompt_sid.empty();
  }));
  void* held_sid = NULL;
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_NE(rec.prompt_sid.back(), "");
    ASSERT_EQ(rec.kept.size(), 1u);
    held_sid = rec.kept.back();
  }

  /* the listing: its rows + strings stay HELD too */
  ASSERT_EQ(sa_client_list_sessions(cl, rec_sessions, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.session_sids.empty();
  }));
  void* held_rows = NULL;
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.session_count.back(), 1u);
    ASSERT_GE(rec.kept.size(), 3u);   /* the prompt's held sid + the row's
                                         non-NULL fields + the rows array */
    held_rows = rec.kept.back();      /* the array lands LAST (held at the
                                         build's end, after the strings) */
    ASSERT_NE(held_rows, (void*)held_sid);
  }

  /* THE DOUBLE-RELEASE / UNKNOWN-POINTER DISCIPLINE (in-holdings mode: the
     releases must be safe no-ops against the tracked table) */
  rec.hold_payloads = false;
  sa_client_release_payload(cl, held_rows);   /* released once... */
  sa_client_release_payload(cl, held_rows);   /* ...twice: a safe no-op */
  sa_client_release_payload(cl, (void*)0x1);  /* unknown: a safe no-op */
  sa_client_release_payload(cl, NULL);        /* NULL: a safe no-op */
  /* the once-released rows array is FREED here (visible only to the leak
     checks; the discipline's crash-proof is the suite running at all) */

  /* held_sid + the row strings stay UNRELEASED: destroy reclaims them —
     the valgrind/ASan runs prove the sweep */
  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

/* --- a server that never answers (the request-timeout path) ------------------ */

typedef struct silent_server_t {
  platform_socket_t* listener;
  platform_socket_t* conn;
  std::thread t;
} silent_server_t;

static void silent_server_run(platform_socket_t* listener,
                              platform_socket_t** conn_out) {
  *conn_out = platform_socket_accept(listener, NULL);
}

static void silent_server_start(silent_server_t* s, const char* path) {
  s->listener = platform_socket_create(PLATFORM_AF_LOCAL, 1);
  ASSERT_NE(s->listener, nullptr);
  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_LOCAL;
  strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);
  ASSERT_EQ(platform_socket_bind(s->listener, &addr), 0);
  ASSERT_EQ(platform_socket_listen(s->listener, 8), 0);
  s->t = std::thread(silent_server_run, s->listener, &s->conn);
}

static void silent_server_stop(silent_server_t* s) {
  if (s->t.joinable()) s->t.join();
  if (s->conn != NULL) platform_socket_destroy(s->conn);
  if (s->listener != NULL) platform_socket_destroy(s->listener);
}

TEST(TestSaClient, TestSilentServerTimesOutTheRequest) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup_common(&fx, NULL, FRAME_ESCALATION_FREE), 0);  /* the store/server stay DOWN —
                                                only the silent listener */
  silent_server_t silent;
  memset(&silent, 0, sizeof(silent));
  char path[120];
  snprintf(path, sizeof(path), "%s/silent.sock", fx.dir_path);
  silent_server_start(&silent, path);

  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.socket_path = path;
  cfg.error_ctx = &rec;
  cfg.request_timeout_ms = 300;   /* the bounded wait's tight bound */
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr) << "the connect succeeded (the socket accepted); "
                            "the SILENCE is what times out";
  rec.client = cl;

  /* the prompt: no response ever comes — the bounded wait fires the error
     callback AND completes the op callback with TIMEOUT */
  ASSERT_EQ(sa_client_prompt(cl, NULL, "nobody home", rec_prompt, &rec), 0);
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.prompt_status.size(), 1u);
    EXPECT_EQ(rec.prompt_status.back(), (uint8_t)SA_CLIENT_STATUS_TIMEOUT);
    ASSERT_EQ(rec.err_status.size(), 1u);
    EXPECT_EQ(rec.err_status.back(), (uint8_t)SA_CLIENT_STATUS_TIMEOUT);
    EXPECT_NE(rec.err_rid.back(), 0u) << "the request's req_id echoes";
    EXPECT_NE(rec.err_text.back().find("timed out"), std::string::npos);
  }

  /* the client keeps working after the timeout's slot clear (the next
     request refuses only on its own facts) — destroy lands clean */
  sa_client_destroy(cl);
  silent_server_stop(&silent);
  platform_file_unlink(path);
  fixture_teardown(&fx);
}

TEST(TestSaClient, TestServerDeathMidRequestFailsClean) {
  /* A silent server holds the channel; the request goes out and blocks;
     then its accepted connection DIES mid-request — the drop completes the
     pending slot with the disconnect sentinel: the op callback + the error
     callback both fire, and the destroy (no subscription active) lands
     clean. */
  fixture_t fx;
  ASSERT_EQ(fixture_setup_common(&fx, NULL, FRAME_ESCALATION_FREE), 0);
  silent_server_t silent;
  memset(&silent, 0, sizeof(silent));
  char path[120];
  snprintf(path, sizeof(path), "%s/silent.sock", fx.dir_path);
  silent_server_start(&silent, path);

  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.socket_path = path;
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);
  rec.client = cl;

  std::thread asker([&] {
    sa_client_prompt(cl, NULL, "into the void", rec_prompt, &rec);
  });
  /* the prompt's send settles well inside this wait; nothing will ever
     answer it */
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  platform_socket_t* conn = silent.conn;   /* grab before nulling */
  silent.conn = NULL;
  ASSERT_NE(conn, nullptr) << "the server accepted the client's socket";
  platform_socket_destroy(conn);   /* THE MID-REQUEST DEATH */
  asker.join();

  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.prompt_status.size(), 1u);
    EXPECT_EQ(rec.prompt_status.back(), (uint8_t)SA_CLIENT_STATUS_DISCONNECTED)
        << "the prompt completed with the disconnect status";
    ASSERT_EQ(rec.err_status.size(), 1u);
    EXPECT_EQ(rec.err_status.back(), (uint8_t)SA_CLIENT_STATUS_DISCONNECTED);
    EXPECT_GE(rec.err_rid.back(), 1u);
    EXPECT_NE(rec.err_text.back().find("lost"), std::string::npos);
    EXPECT_EQ(rec.prompt_sid.back(), "") << "no sid rides a failure";
  }

  sa_client_destroy(cl);
  silent_server_stop(&silent);
  platform_file_unlink(path);
  fixture_teardown(&fx);
}

/* THE RECONNECT: the events channel's backoff re-opens the socket when a
   server returns on the SAME path (the transport's destroy unlinks the
   socket file; a NEW transport binds that path again; the reader's
   reconnect re-subscribes from the resume-at cursor and the live marker +
   the post-reconnect commits deliver through the SAME callback). */
TEST(TestSaClient, TestEventsReconnectsWhenTheServerReturns) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup(&fx), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);
  rec.client = cl;

  ASSERT_EQ(sa_client_prompt(cl, NULL, "survive the restart", rec_prompt,
                             &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.prompt_sid.empty();
  }));
  std::string sid;
  {
    std::lock_guard<std::mutex> g(rec.m);
    sid = rec.prompt_sid.back();
  }
  ASSERT_EQ(sa_client_subscribe_events(cl, sid.c_str(), rec_events, &rec), 0);
  {
    std::lock_guard<std::mutex> g(rec.m);
    EXPECT_EQ(rec.ev_seq.back(), 0u)
        << "the first live marker closes the (possibly empty) replay";
    EXPECT_EQ(rec.ev_op.back(), (uint8_t)CA_EVENTS_REPLAY_THEN_LIVE);
    EXPECT_EQ(rec.ev_json.back(), "");
  }

  /* the server drops: the channel dies (the reader sees the hangup and
     begins its backoff) */
  unix_transport_destroy(fx.transport);
  fx.transport = NULL;

  /* the server RETURNS on the same path before the budget's first retry
   * settles */
  fx.transport = unix_transport_create(fx.pool, fx.server, fx.socket_path);
  ASSERT_NE(fx.transport, nullptr);
  unix_transport_start(fx.transport);

  /* the re-subscribe's NEW live marker (attempt 1's 1 s backoff) — the
     resume-at cursor makes the gap empty, so the marker follows any replay
     records directly; scan for the SECOND zero-seq REPLAY marker */
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    int markers = 0;
    for (size_t i = 0; i < rec.ev_seq.size(); i++) {
      if (rec.ev_seq[i] == 0 &&
          rec.ev_op[i] == (uint8_t)CA_EVENTS_REPLAY_THEN_LIVE) {
        markers++;
      }
    }
    return markers >= 2;
  }, 2400)) << "the second live marker = the re-subscribe went live";
  {
    std::lock_guard<std::mutex> g(rec.m);
    EXPECT_EQ(rec.err_status.size(), 0u)
        << "the reconnect's budget never exhausted";
  }

  /* the client requests THROUGH the reconnected channel */
  ASSERT_EQ(sa_client_prompt(cl, sid.c_str(), "after reconnect", rec_prompt,
                             &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    for (size_t i = 0; i < rec.ev_json.size(); i++) {
      if (rec.ev_seq[i] > 0 &&
          rec.ev_json[i].find("after reconnect") != std::string::npos) {
        return true;
      }
    }
    return false;
  })) << "the post-reconnect commit rides the live tail";

  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

/* --- the TCP suite: the auth exchange FIRST, the same callback io ------------- */

TEST(TestSaClientTcp, TestAuthedPromptRoundTripOverTcp) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup_tcp(&fx), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.transport = SA_CLIENT_TRANSPORT_TCP;
  /* the TCP config's honest shape (the demo CLI's client mode): the socket
     path rides the unix fixture's helper — NULLed here, pinning that a TCP
     connect never demands a socket_path */
  cfg.socket_path = NULL;
  cfg.host = "127.0.0.1";
  cfg.port = fixture_tcp_port(&fx);
  cfg.api_key = FIXTURE_API_KEY;
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr) << "the auth exchange ran FIRST at connect";
  rec.client = cl;

  ASSERT_EQ(sa_client_prompt(cl, NULL, "make the tcp thing", rec_prompt,
                             &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.prompt_sid.empty();
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    EXPECT_EQ(rec.prompt_status.back(), 0u);
    EXPECT_EQ(rec.prompt_sid.back().rfind("sessions/", 0), 0u);
    EXPECT_EQ(rec.err_status.size(), 0u);
  }

  /* the listing too (the authed channel carries all four ops) */
  ASSERT_EQ(sa_client_list_sessions(cl, rec_sessions, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.session_sids.empty();
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.session_count.back(), 1u);
    EXPECT_STREQ(rec.session_status.back()[0].c_str(), "running");
  }

  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

TEST(TestSaClientTcp, TestBadKeyConnectRefused) {
  fixture_t fx;
  ASSERT_EQ(fixture_setup_tcp(&fx), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.transport = SA_CLIENT_TRANSPORT_TCP;
  cfg.socket_path = NULL;   /* the honest TCP shape (the demo CLI's) */
  cfg.host = "127.0.0.1";
  cfg.port = fixture_tcp_port(&fx);
  cfg.api_key = "not-the-key";
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);

  /* the bad key closes the connection after the AUTH RESPONSE — the
     connect surfaces as NULL (and a valgrind-quiet teardown) */
  EXPECT_EQ(cl, nullptr) << "the wrong key refused at connect";
  fixture_teardown(&fx);
}

/* The FFI ABI companion's probes (the C-side half of the struct-drift
   contract; the Dart test pins the EXACT values, this suite pins the
   SHAPE: the ten members in declared order — non-decreasing offsets, all
   inside the struct's byte budget, the first member at 0 — and the
   out-of-range answer 0). A reordered member, a count change, or a member
   pushed past the recorded size flips one of these. */
TEST(TestSaClientConfigFfi, TestProbesPinTheStructShape) {
  size_t size = sa_client_config_ffi_sizeof();
  ASSERT_GT(size, (size_t)0);

  size_t prev = 0;
  for (int i = 0; i < 10; i++) {
    size_t off = sa_client_config_ffi_offset(i);
    ASSERT_LE(prev, off) << "probe index " << i
                         << " answered before its predecessor: the "
                            "declared order drifted";
    ASSERT_LT(off, size) << "probe index " << i
                         << " answered past sizeof: the struct grew "
                            "beyond the recorded budget";
    prev = off;
  }
  EXPECT_EQ(sa_client_config_ffi_offset(0), (size_t)0);
  /* the out-of-range bounds answer 0 (the probe's documented floor) */
  EXPECT_EQ(sa_client_config_ffi_offset(10), (size_t)0);
  EXPECT_EQ(sa_client_config_ffi_offset(-1), (size_t)0);
}

/* --- the escalation slice's ask tests -------------------------------------- */

TEST(TestSaClient, TestAskReplyRoundTrip) {
  /* The parked plan gate over the REAL unix stack (the handlers' shape):
     the create's PLAN_ASK_ACT frame parks at the gate; the answer through
     sa_client_ask_reply completes cb(0) — the ack's delivered member read
     off the payload — and the engine consumes durably (ask.reply + the
     answer's user msg.append). Then the re-entry rule's probe (the armed
     events callback refuses the op -1) and the one-in-flight slot's
     (an overlapping second call completes cb(BUSY), never a -1). */
  std::vector<std::string> replies = {
      sa_content_body("Plan: 1. measure 2. cut 3. report"),
      sa_content_body("the act ran quiet")};
  sa_scripted_t sm;
  memset(&sm, 0, sizeof(sm));
  sm.base.complete = sa_scripted_complete;
  sm.replies = &replies;

  fixture_t fx;
  ASSERT_EQ(fixture_setup_escalated(&fx, &sm.base), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);
  rec.client = cl;

  /* the create: the frame parks at the gate */
  ASSERT_EQ(sa_client_prompt(cl, NULL, "plan the cut", rec_prompt, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.prompt_sid.empty();
  }));
  std::string sid;
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.prompt_status.back(), 0u);
    sid = rec.prompt_sid.back();
  }
  std::string ask_id = fixture_parked_ask_id(&fx, sid);
  ASSERT_EQ(ask_id.size(), 8u) << "the gate ask parked";

  /* THE ANSWER: cb(0) — the ack reported delivered. */
  ASSERT_EQ(sa_client_ask_reply(cl, sid.c_str(), ask_id.c_str(), 0,
                                "Approve", rec_askreply, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.askreply_status.empty();
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.askreply_status.size(), 1u);
    EXPECT_EQ(rec.askreply_status.back(), 0u) << "delivered";
    EXPECT_EQ(rec.err_status.size(), 0u)
        << "the delivered ack carries no error-channel entry";
  }
  ASSERT_TRUE(fixture_wait_reply_committed(&fx, sid, ask_id, "Approve", 800))
      << "the engine consumed the answer durably";

  /* THE RE-ENTRY (the events callback's own thread): subscribing replays
     the log's records — the first record delivery calls the blocking op,
     which must refuse -1 instantly, firing NO callback. */
  rec.attempt_askreentry.store(1);
  ASSERT_EQ(sa_client_subscribe_events(cl, sid.c_str(), rec_events, &rec), 0);
  ASSERT_TRUE(wait_for([&] { return rec.askreentry_rc.load() == -1; }))
      << "an events callback's sa_client_ask_reply refuses immediately";
  {
    std::lock_guard<std::mutex> g(rec.m);
    EXPECT_LT(rec.askreentry_ms.load(), 100)
        << "the refusal is microseconds, not a timeout stall";
    ASSERT_EQ(rec.askreply_status.size(), 1u)
        << "the re-entry fired no ask-reply callback";
  }

  /* THE ONE-IN-FLIGHT SLOT: a SILENT peer holds the first call mid-flight;
     the overlapping second call completes cb(BUSY) on ITS thread (the
     op contract: rc 0 + a delivered failure status — -1 is ONLY the
     re-entry refusal). The slot's value is order-agnostic: whichever call
     won the slot times out, the other refuses busy. */
  silent_server_t silent;
  memset(&silent, 0, sizeof(silent));
  char silent_path[160];
  snprintf(silent_path, sizeof(silent_path), "%s/silent2.sock",
           fx.dir_path);
  silent_server_start(&silent, silent_path);
  rec_t recb;
  sa_client_config_t cfg2 = client_config(&fx);
  cfg2.socket_path = silent_path;
  cfg2.request_timeout_ms = 300;   /* the held call's bounded end */
  cfg2.error_ctx = &recb;
  sa_client_t* cl2 = sa_client_connect(&cfg2);
  ASSERT_NE(cl2, nullptr);
  recb.client = cl2;
  std::thread held([&] {
    sa_client_ask_reply(cl2, "sessions/0000000000000000000deadbeef",
                        "abcdef12", 0, "late answer", rec_askreply, &recb);
  });
  /* the first call's slot grab is its own thread's first act — the 20 ms
     handicap keeps the overlap honest (the held call WINS the slot) */
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_EQ(sa_client_ask_reply(cl2, "sessions/0000000000000000000deadbeef",
                                "abcdef12", 0, "the overlapping call",
                                rec_askreply, &recb), 0);
  held.join();
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(recb.m);
    return recb.askreply_status.size() == 2;
  }));
  {
    std::lock_guard<std::mutex> g(recb.m);
    ASSERT_EQ(recb.askreply_status.size(), 2u);
    bool saw_busy = false, saw_timeout = false;
    for (uint8_t st : recb.askreply_status) {
      saw_busy |= (st == (uint8_t)SA_CLIENT_STATUS_BUSY);
      saw_timeout |= (st == (uint8_t)SA_CLIENT_STATUS_TIMEOUT);
    }
    EXPECT_TRUE(saw_busy) << "the overlapping second call completed BUSY "
                             "(rc 0 — the delivery, not the refusal)";
    EXPECT_TRUE(saw_timeout) << "the held call ended at its bound";
  }
  sa_client_destroy(cl2);
  silent_server_stop(&silent);
  platform_file_unlink(silent_path);

  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

TEST(TestSaClient, TestAskReplyDeliveredFalseFailsTheCallback) {
  /* The delivered-0 ack → cb(1) — the PAYLOAD's delivered member read,
     NEVER the status-0-ok idiom (the codec's trap: the response's
     delivered byte rides *status OUT of the decode, so an rstatus idiom
     would invert this no into a success). The probe: an ANSWER with an
     empty value — the engine's boundary refuses it (an answer carries its
     text), the handler acks delivered 0, and the frame's park STAYS
     (no ask.reply record). No error-channel entry either. */
  std::vector<std::string> replies = {
      sa_content_body("Plan: the only plan"),
      sa_content_body("the act ran quiet")};
  sa_scripted_t sm;
  memset(&sm, 0, sizeof(sm));
  sm.base.complete = sa_scripted_complete;
  sm.replies = &replies;

  fixture_t fx;
  ASSERT_EQ(fixture_setup_escalated(&fx, &sm.base), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);
  rec.client = cl;

  ASSERT_EQ(sa_client_prompt(cl, NULL, "plan the empty answer", rec_prompt,
                             &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.prompt_sid.empty();
  }));
  std::string sid;
  {
    std::lock_guard<std::mutex> g(rec.m);
    sid = rec.prompt_sid.back();
  }
  std::string ask_id = fixture_parked_ask_id(&fx, sid);
  ASSERT_EQ(ask_id.size(), 8u);

  /* The EMPTY answer: the daemon's boundary refuses — the honest no rides
     delivered 0 → cb(1). */
  ASSERT_EQ(sa_client_ask_reply(cl, sid.c_str(), ask_id.c_str(), 0, "",
                                rec_askreply, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.askreply_status.empty();
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.askreply_status.size(), 1u);
    EXPECT_EQ(rec.askreply_status.back(), 1u)
        << "delivered 0 completes the callback 1 (the payload's member)";
    EXPECT_EQ(rec.err_status.size(), 0u)
        << "the unbound reply is the ack's no — not the error channel";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(fixture_ask_reply_committed(&fx, sid, ask_id, ""))
      << "the empty answer never entered the mailbox (no resolution record)";

  /* And the honest answer STILL consumes the standing park (the refusal
     never ate it). */
  ASSERT_EQ(sa_client_ask_reply(cl, sid.c_str(), ask_id.c_str(), 0,
                                "Approve", rec_askreply, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return rec.askreply_status.size() == 2;
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    EXPECT_EQ(rec.askreply_status.back(), 0u);
  }

  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

TEST(TestSaClient, TestAskReplyRejectNullValueRides) {
  /* THE REJECT'S NULL VALUE through sa_client (the reject-path coverage):
     a legal reject {decision 1, value NULL — the header's documented shape}
     must RIDE — the alloc check may only refuse a dup failure for a
     NON-NULL source. Before the fix the NULL value was misrouted into
     _error_local(ALLOC): no callback, no wire send, the park starved. The
     honest shape: cb(0) (the ack reported delivered — a reject posts), the
     error channel stays silent, and the engine consumes durably: the
     ask.reply record {decision "reject", value null} + the plan gate's
     default-wording user append, NO plan-approved control record. */
  std::vector<std::string> replies = {
      sa_content_body("Plan: the only plan"),
      sa_content_body("the replan ran quiet")};
  sa_scripted_t sm;
  memset(&sm, 0, sizeof(sm));
  sm.base.complete = sa_scripted_complete;
  sm.replies = &replies;

  fixture_t fx;
  ASSERT_EQ(fixture_setup_escalated(&fx, &sm.base), 0);
  rec_t rec;
  sa_client_config_t cfg = client_config(&fx);
  cfg.error_ctx = &rec;
  sa_client_t* cl = sa_client_connect(&cfg);
  ASSERT_NE(cl, nullptr);
  rec.client = cl;

  /* the create: the frame parks at the plan gate */
  ASSERT_EQ(sa_client_prompt(cl, NULL, "plan the rejection", rec_prompt, &rec),
            0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.prompt_sid.empty();
  }));
  std::string sid;
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.prompt_status.back(), 0u);
    sid = rec.prompt_sid.back();
  }
  std::string ask_id = fixture_parked_ask_id(&fx, sid);
  ASSERT_EQ(ask_id.size(), 8u) << "the gate ask parked";

  /* THE REJECT: decision 1, value NULL — the NULL value rides the wire
     (the encode renders it "", the daemon's decode absorbs it absent),
     the post succeeds → cb(0). */
  ASSERT_EQ(sa_client_ask_reply(cl, sid.c_str(), ask_id.c_str(), 1, NULL,
                                rec_askreply, &rec), 0);
  ASSERT_TRUE(wait_for([&] {
    std::lock_guard<std::mutex> g(rec.m);
    return !rec.askreply_status.empty();
  }));
  {
    std::lock_guard<std::mutex> g(rec.m);
    ASSERT_EQ(rec.askreply_status.size(), 1u);
    EXPECT_EQ(rec.askreply_status.back(), 0u)
        << "the reject posted — cb(0), never the false OOM reroute";
    EXPECT_EQ(rec.err_status.size(), 0u)
        << "a legal NULL value never rode the error channel";
  }
  ASSERT_TRUE(fixture_wait_reject_consumed(&fx, sid, ask_id, 800))
      << "the engine consumed the reject durably";
  fixture_check_reject_consumed(&fx, sid, ask_id);

  sa_client_destroy(cl);
  fixture_teardown(&fx);
}

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */