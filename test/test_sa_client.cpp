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
#include "../src/Frame/frame.h"
#include "../src/Frame/model.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Streams/loop_thread.h"
#include "../src/Platform/platform.h"
#include "../src/Util/allocator.h"
#include "../src/Util/bcrypt.h"
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

static int fixture_setup_common(fixture_t* fx) {
  memset(fx, 0, sizeof(*fx));
  memset(&fx->cfg, 0, sizeof(fx->cfg));
  fx->cfg.model_base_url = NULL;   /* a NULL-backend engine fails fast: no
                                      network, deterministic steers */
  fx->cfg.model_api_key = NULL;
  fx->cfg.model_name = "unused";
  fx->cfg.max_depth = 4;
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
                                        NULL);
  if (fx->server == NULL) return -4;
  char tmpl[] = "/tmp/sa-ca7-XXXXXX";
  char* dir = mkdtemp(tmpl);
  if (dir == NULL) return -5;
  snprintf(fx->dir_path, sizeof(fx->dir_path), "%s", dir);
  snprintf(fx->socket_path, sizeof(fx->socket_path), "%s/serve.sock", dir);
  return 0;
}

static int fixture_setup(fixture_t* fx) {
  int rc = fixture_setup_common(fx);
  if (rc != 0) return rc;
  fx->transport = unix_transport_create(fx->pool, fx->server,
                                        fx->socket_path);
  if (fx->transport == NULL) return -6;
  unix_transport_start(fx->transport);
  return 0;
}

static int fixture_setup_tcp(fixture_t* fx) {
  int rc = fixture_setup_common(fx);
  if (rc != 0) return rc;
  char hash[64];
  if (bcrypt_generate(FIXTURE_API_KEY, 4, hash, sizeof(hash)) != 0) return -7;
  fx->tcp_transport = tcp_transport_create(fx->pool, fx->server,
                                           "127.0.0.1", 0, hash, &fx->tcp_addr);
  if (fx->tcp_transport == NULL) return -6;
  tcp_transport_start(fx->tcp_transport);
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
  /* the re-entry harness (armed by one test): the events callback's FIRST
     record delivery calls sa_client_prompt — the client must refuse it
     immediately (return -1, no callback, the subscription keeps flowing) */
  std::atomic<int> attempt_reentry{0};
  std::atomic<int> reentry_attempts{0};
  std::atomic<int> reentry_rc{-2};
  std::atomic<int> reentry_ms{-1};
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
  ASSERT_EQ(fixture_setup_common(&fx), 0);   /* the store/server stay DOWN —
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
  ASSERT_EQ(fixture_setup_common(&fx), 0);
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

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */