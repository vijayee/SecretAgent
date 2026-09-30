//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
extern "C" {
#include "../src/Actor/actor.h"
#include "../src/Frame/frame_messages.h"
#include "../src/Util/allocator.h"
#include "../src/Util/atomic_compat.h"
#include "../src/Util/log.h"
#include "../src/Platform/platform.h"
}
#ifdef SA_HAS_PYTHON
extern "C" {
#include "../src/Python/pyrt.h"
#include "../src/Python/pyrt_messages.h"
#include "../src/Python/py_agent.h"
}
#endif

#ifdef SA_HAS_PYTHON

/* Scripted owning frame (the py_frame pattern from test_pyrt.cpp, with the
   recording-sink answering shape from test_frame.cpp): this frame owns the
   runtime, so the injected module's agent.* verbs post FRM_* requests into
   THIS actor's inbox. In answer mode the dispatch records each request and
   pre-answers by hand through the bridge registry's sink
   (py_agent_note_reply) — the same direct call a real frame behavior makes
   through the hook, on the same dispatch thread, with the same no-free rule
   (the reply text is borrowed and the registry copies it). In silence mode
   nothing is answered and every caller must end through the bounded wait. */
typedef struct bridge_frame_t {
  actor_t actor;
  pyrt_t* pyrt;
  std::vector<pyrt_result_payload_t*> results;
  ATOMIC(uint8_t) got_result;
  uint8_t answer;                    /* 1 = answer every bridge request; 0 = swallow it */
  /* remember → recall round-trip. NOT std::map: this struct is
     clear(AKA zero-)allocated per the py_frame harness pattern, and a zeroed
     map (unlike a zeroed vector, which is merely an empty one) has a broken
     RB-tree header — the harness keeps to vectors only. */
  std::vector<std::pair<std::string, std::string>> store;
  char* scripted_sid;   /* heap copy; no std::string member (a zeroed string
                           is not assignable — see the clear-alloc note) */
  /* Recorded requests (one entry per bridge request that arrived): */
  std::vector<uint32_t> req_types;
  std::vector<uint64_t> req_corrs;
  std::vector<std::string> req_a;    /* remember/recall key, spawn goal, report text */
  std::vector<std::string> req_b;    /* remember value, spawn context */
} bridge_frame_t;

static void bridge_frame_dispatch(void* state, message_t* msg) {
  bridge_frame_t* self = (bridge_frame_t*)state;
  switch (msg->type) {
    case PYRT_RESULT: {
      pyrt_result_payload_t* r = (pyrt_result_payload_t*)msg->payload;
      msg->payload = NULL;   /* transferred OUT; freed by the harness teardown */
      self->results.push_back(r);
      ATOMIC_STORE(&self->got_result, 1);
      break;
    }
    case FRM_REMEMBER: {
      frm_remember_payload_t* rp = (frm_remember_payload_t*)msg->payload;
      self->req_types.push_back(msg->type);
      self->req_corrs.push_back(rp ? rp->corr : 0);
      self->req_a.push_back(rp && rp->key ? rp->key : "");
      self->req_b.push_back(rp && rp->json_value ? rp->json_value : "");
      if (self->answer && rp != NULL) {
        std::string key(rp->key ? rp->key : "");
        self->store.emplace_back(key, rp->json_value ? rp->json_value : "");
        py_agent_note_reply(rp->corr, 0, "");
      }
      break;   /* payload retired by actor_run (good-actors) */
    }
    case FRM_RECALL: {
      frm_remember_payload_t* rp = (frm_remember_payload_t*)msg->payload;
      self->req_types.push_back(msg->type);
      self->req_corrs.push_back(rp ? rp->corr : 0);
      self->req_a.push_back(rp && rp->key ? rp->key : "");
      if (self->answer && rp != NULL) {
        std::string key(rp->key ? rp->key : "");
        const char* value = "";
        uint8_t found = 0;
        for (const auto& kv : self->store) {
          if (kv.first == key) {
            value = kv.second.c_str();
            found = 1;
            break;
          }
        }
        py_agent_note_reply(rp->corr, found ? 0 : 1, value);
      }
      break;
    }
    case FRM_SPAWN: {
      frm_spawn_payload_t* sp = (frm_spawn_payload_t*)msg->payload;
      self->req_types.push_back(msg->type);
      self->req_corrs.push_back(sp ? sp->corr : 0);
      self->req_a.push_back(sp && sp->goal ? sp->goal : "");
      self->req_b.push_back(sp && sp->context_json ? sp->context_json : "");
      if (self->answer && sp != NULL) {
        py_agent_note_reply(sp->corr, 0, self->scripted_sid);
      }
      break;
    }
    case FRM_REPORT: {
      frm_report_payload_t* rp = (frm_report_payload_t*)msg->payload;
      self->req_types.push_back(msg->type);
      self->req_corrs.push_back(rp ? rp->corr : 0);
      self->req_a.push_back(rp && rp->text ? rp->text : "");
      if (self->answer && rp != NULL) {
        py_agent_note_reply(rp->corr, 0, "");
      }
      break;
    }
    default:
      break;
  }
}

static bridge_frame_t* bridge_frame_create(uint8_t answer, const char* scripted_sid) {
  bridge_frame_t* self = (bridge_frame_t*)get_clear_memory(sizeof(bridge_frame_t));
  actor_init(&self->actor, self, bridge_frame_dispatch, NULL);   /* NULL pool: pumped by hand */
  self->answer = answer;
  if (scripted_sid != NULL) {
    self->scripted_sid = strdup(scripted_sid);
  }
  return self;
}

static void bridge_frame_execute(bridge_frame_t* self, const char* code) {
  if (self->pyrt == NULL) {
    pyrt_config_t cfg;
    cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
    cfg.pool_cap = 0;
    cfg.idle_evict_ms = 0;
    self->pyrt = pyrt_create(&self->actor, &cfg);
  }
  pyrt_execute(self->pyrt, strdup(code));
}

static void bridge_frame_pump(bridge_frame_t* self, int timeout_ms) {
  int waited = 0;
  while (ATOMIC_LOAD(&self->got_result) == 0 && waited < timeout_ms) {
    actor_run(&self->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
    waited++;
  }
  actor_run(&self->actor, ACTOR_BATCH_SIZE);
}

/* The frame is clear-allocated, so the C++ members are never destructed by
   free(): drain their buffers here (the py_frame_free discipline) or valgrind
   reports the harness containers themselves as definitely-lost. */
static void bridge_frame_free(bridge_frame_t* self) {
  if (self->pyrt != NULL) {
    pyrt_destroy(self->pyrt);   /* join first: the mailbox must stop accepting sends */
  }
  actor_destroy(&self->actor);
  for (auto* r : self->results) {
    pyrt_result_payload_destroy(r);
  }
  self->results = std::vector<pyrt_result_payload_t*>();
  self->store = std::vector<std::pair<std::string, std::string>>();
  free(self->scripted_sid);
  self->req_types = std::vector<uint32_t>();
  self->req_corrs = std::vector<uint64_t>();
  self->req_a = std::vector<std::string>();
  self->req_b = std::vector<std::string>();
  free(self);
}

TEST(TestPyAgent, TestFrozenApiSymbolsResolve) {
  EXPECT_NE((void*)py_agent_init, (void*)NULL);
  EXPECT_NE((void*)py_agent_note_reply, (void*)NULL);
  EXPECT_NE((void*)py_agent_methods_combined, (void*)NULL);
}

/* Runs the EXEC-form setup cell (assignments/import — the injected helper's
   SyntaxError fallback makes any such cell return '' with status 0), then,
   with the persisted namespace, runs the pure-EXPRESSION probe cell whose
   repr is the assertion surface. Single result per call, status 0. */
static void bridge_run_setup_and_probe(bridge_frame_t* self,
                                       const char* setup, const char* probe) {
  self->results.clear();
  ATOMIC_STORE(&self->got_result, 0);
  bridge_frame_execute(self, setup);
  bridge_frame_pump(self, 30000);
  ASSERT_EQ(self->results.size(), 1u);
  ASSERT_EQ(self->results[0]->status, 0);

  ATOMIC_STORE(&self->got_result, 0);
  bridge_frame_execute(self, probe);
  bridge_frame_pump(self, 30000);
  ASSERT_EQ(self->results.size(), 2u);
  ASSERT_EQ(self->results[1]->status, 0);
}

/* remember + recall: corr-matched round-trips through the bridge registry;
   the probe cell embeds all three answers (two delivered, one unresolvable).
   The value is a BARE python string — the bridge encodes it ('fast' stores
   as the JSON text "fast") and recall decodes it back, so a live model's
   natural actor.remember('word', 'wave') round trips as python values. */
TEST(TestPyAgent, TestRememberRecallCorrMatched) {
  py_agent_init();   /* idempotent; re-mounts after other suites' demounts */
  bridge_frame_t* self = bridge_frame_create(1, NULL);

  bridge_run_setup_and_probe(
      self,
      "import actor\n"
      "ok = actor.remember('mode', 'fast')\n"
      "got = actor.recall('mode')\n"
      "miss = actor.recall('absent')\n",
      "(ok, got, miss)");

  EXPECT_STREQ(self->results[1]->text, "(True, 'fast', None)");

  /* Three requests reached the owner; each carried heap-copies of the
     python-heap strings and its own correlation id. */
  ASSERT_EQ(self->req_types.size(), 3u);
  EXPECT_EQ(self->req_types[0], (uint32_t)FRM_REMEMBER);
  EXPECT_NE(self->req_corrs[0], 0u);
  EXPECT_STREQ(self->req_a[0].c_str(), "mode");
  EXPECT_STREQ(self->req_b[0].c_str(), "\"fast\"") << "JSON text verbatim";
  EXPECT_EQ(self->req_types[1], (uint32_t)FRM_RECALL);
  EXPECT_NE(self->req_corrs[1], self->req_corrs[0]) << "one corr per verb call";
  EXPECT_STREQ(self->req_a[1].c_str(), "mode");
  EXPECT_EQ(self->req_types[2], (uint32_t)FRM_RECALL);
  EXPECT_NE(self->req_corrs[2], self->req_corrs[1]);

  bridge_frame_free(self);
}

/* spawn: the scripted parent answers corr-matched with a sid; both calls
   (with and without handoff context) return that sid as a python string. */
TEST(TestPyAgent, TestSpawnReturnsScriptedSid) {
  py_agent_init();
  bridge_frame_t* self = bridge_frame_create(1, "sessions/cafebeef/frames/deadbeef");

  bridge_run_setup_and_probe(
      self,
      "import actor\n"
      "a = actor.spawn('polish', '{\"handoff\": 1}')\n"
      "b = actor.spawn('no context')\n",
      "(a, b)");

  EXPECT_STREQ(self->results[1]->text,
               "('sessions/cafebeef/frames/deadbeef', 'sessions/cafebeef/frames/deadbeef')");

  ASSERT_EQ(self->req_types.size(), 2u);
  EXPECT_EQ(self->req_types[0], (uint32_t)FRM_SPAWN);
  EXPECT_NE(self->req_corrs[0], 0u);
  EXPECT_STREQ(self->req_a[0].c_str(), "polish");
  EXPECT_STREQ(self->req_b[0].c_str(), "{\"handoff\": 1}") << "handoff context verbatim";
  EXPECT_EQ(self->req_types[1], (uint32_t)FRM_SPAWN);
  EXPECT_NE(self->req_corrs[1], self->req_corrs[0]);
  EXPECT_STREQ(self->req_a[1].c_str(), "no context");
  EXPECT_EQ(self->req_b[1], "") << "context=None posts no context key";

  bridge_frame_free(self);
}

/* report: True on the scripted success; a non-str value is coerced through
   repr (JSON text for the scalar case) rather than a TypeError, documented
   on _py_agent_report. */
TEST(TestPyAgent, TestReportReturnsTrueAndCoercesNonString) {
  py_agent_init();
  bridge_frame_t* self = bridge_frame_create(1, NULL);

  bridge_run_setup_and_probe(
      self,
      "import actor\n"
      "a = actor.report('plain')\n"
      "b = actor.report(7)\n",
      "(a, b)");

  EXPECT_STREQ(self->results[1]->text, "(True, True)");
  ASSERT_EQ(self->req_types.size(), 2u);
  EXPECT_EQ(self->req_types[0], (uint32_t)FRM_REPORT);
  EXPECT_STREQ(self->req_a[0].c_str(), "plain");
  EXPECT_EQ(self->req_types[1], (uint32_t)FRM_REPORT);
  EXPECT_STREQ(self->req_a[1].c_str(), "7") << "non-str coerced via repr";

  bridge_frame_free(self);
}

/* A silent owner (no reply ever) must answer every verb as failure through
   the bounded wait — four 500 ms waits (~2 s total), never a hang. */
TEST(TestPyAgent, TestSilentFrameTimeoutsAnswerFailure) {
  py_agent_init();
  bridge_frame_t* self = bridge_frame_create(0, NULL);

  bridge_run_setup_and_probe(
      self,
      "import actor\n",
      "(actor.remember('k', '\"v\"'), actor.recall('k'), actor.spawn('g'), actor.report('r'))");

  EXPECT_STREQ(self->results[1]->text, "(False, None, None, False)");
  ASSERT_EQ(self->req_types.size(), 4u);   /* all four requests still reached the owner */

  bridge_frame_free(self);
}

/* The no-owner observation path. 3.12 isolated subinterpreters REFUSE
   stdlib threads ("thread is not supported for isolated subinterpreters"),
   so a cell can only sit on a pyrt thread — where the TLS owner is always
   the runtime's owner. The no-owner verb guard is therefore exercised the
   direct way: a runtime created WITHOUT an owner runs the cell; every verb
   answers failure IMMEDIATELY (loud log via the recorder below — the run
   must complete in well under four 500 ms waits); the cell's result itself
   is unroutable by construction (ownerless), which is pyrt's documented
   ownerless contract. */
#define PA_LOG_RING 32
#define PA_LOG_LINE 256
static char _pa_log_ring[PA_LOG_RING][PA_LOG_LINE];
static ATOMIC(uint64_t) _pa_log_reserved = 0;
static ATOMIC(uint64_t) _pa_log_published = 0;

static void _pa_log_recorder(log_Event* ev) {
  va_list ap;
  va_copy(ap, ev->ap);
  char line[PA_LOG_LINE];
  vsnprintf(line, sizeof(line), ev->fmt, ap);
  va_end(ap);
  uint64_t slot = (uint64_t)atomic_fetch_add(&_pa_log_reserved, 1) % PA_LOG_RING;
  memcpy(_pa_log_ring[slot], line, strlen(line) + 1);
  atomic_fetch_add(&_pa_log_published, 1);
}

TEST(TestPyAgent, TestVerbsWithoutOwnerFailImmediately) {
  py_agent_init();
  log_add_callback(_pa_log_recorder, NULL, LOG_ERROR);

  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
  cfg.pool_cap = 0;
  cfg.idle_evict_ms = 0;
  pyrt_t* py = pyrt_create(NULL, &cfg);   /* NO owner */
  ASSERT_NE(py, nullptr);
  uint64_t published_before = (uint64_t)atomic_load(&_pa_log_published);
  pyrt_execute(py, strdup("import actor\n"
                          "(actor.remember('k', '\"v\"'), actor.recall('k'),\n"
                          " actor.spawn('g'), actor.report('r'))"));

  /* Wait until the four immediate-failure logs landed (bounded; the verbs
     take the no-owner path BEFORE any wait). */
  int waited = 0;
  while ((uint64_t)atomic_load(&_pa_log_published) < published_before + 4 &&
         waited < 30000) {
    platform_sleep_ms(1);
    waited++;
  }
  EXPECT_EQ((uint64_t)atomic_load(&_pa_log_published), published_before + 4u)
      << "all four verbs answered the no-owner failure loudly";

  /* Every py_agent failure line names the no-owner cause. */
  size_t verb_failures = 0;
  for (int i = 0; i < PA_LOG_RING; i++) {
    if (strstr(_pa_log_ring[i], "py_agent: verb outside a frame-owned "
                               "runtime thread") != NULL) {
      verb_failures++;
    }
  }
  EXPECT_GE(verb_failures, 4u) << "each of the four verbs logged the cause";
  pyrt_destroy(py);
}

#endif /* SA_HAS_PYTHON */

#ifndef SA_HAS_PYTHON
/* Keeps the file a valid non-empty test unit in libpython-free builds. */
TEST(TestPyAgent, TestCompiledWithoutPython) { SUCCEED(); }
#endif

/* No main() here: testsecretagent already links gtest with the runner in
   test_main.cpp; a second main would collide at the link stage. */