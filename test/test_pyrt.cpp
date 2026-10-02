//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "../src/Actor/actor.h"
#include "../src/Util/atomic_compat.h"
#include "../src/Util/allocator.h"
#include "../src/Platform/platform.h"
}
#ifdef SA_HAS_PYTHON
extern "C" {
#include "../src/Python/pyrt.h"
#include "../src/Python/pyrt_messages.h"
}
#endif

#ifdef SA_HAS_PYTHON

/* Test frame: embeds an actor (first member, per the style guide) and owns
   one pyrt runtime. Result payloads are transferred OUT of their messages
   (msg->payload = NULL) so actor_run never frees them; the frame frees them
   itself in py_frame_free (good-actors ownership rules). */
typedef struct py_frame_t {
  actor_t actor;
  pyrt_t* pyrt;
  std::vector<pyrt_result_payload_t*> results;
  std::vector<std::string> logs;
  std::vector<std::string> statuses;
  ATOMIC(uint8_t) got_result;
} py_frame_t;

static void py_frame_dispatch(void* state, message_t* msg) {
  py_frame_t* self = (py_frame_t*)state;
  switch (msg->type) {
    case PYRT_RESULT: {
      pyrt_result_payload_t* r = (pyrt_result_payload_t*)msg->payload;
      msg->payload = NULL;
      self->results.push_back(r);
      ATOMIC_STORE(&self->got_result, 1);
      break;
    }
    case PYRT_LOG: {
      pyrt_text_payload_t* t = (pyrt_text_payload_t*)msg->payload;
      if (t != NULL && t->text != NULL) {
        self->logs.push_back(t->text);
      }
      break;   /* payload freed by actor_run via payload_destroy */
    }
    case PYRT_STATUS: {
      pyrt_text_payload_t* t = (pyrt_text_payload_t*)msg->payload;
      if (t != NULL && t->text != NULL) {
        self->statuses.push_back(t->text);
      }
      break;
    }
    default:
      break;
  }
}

/* Creates the runtime WITHOUT executing a cell; used so a test can arm a
   state (e.g. a pending interrupt request) before the first cell is queued. */
static void py_frame_open(py_frame_t* self) {
  if (self->pyrt == NULL) {
    pyrt_config_t cfg;
    cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
    cfg.pool_cap = 0;
    cfg.idle_evict_ms = 0;
    self->pyrt = pyrt_create(&self->actor, &cfg);
  }
}

static py_frame_t* py_frame_create(void) {
  py_frame_t* self = (py_frame_t*)get_clear_memory(sizeof(py_frame_t));
  actor_init(&self->actor, self, py_frame_dispatch, NULL); /* NULL pool: pumped by hand */
  ATOMIC_STORE(&self->got_result, 0);
  return self;
}

static void py_frame_execute(py_frame_t* self, const char* code) {
  py_frame_open(self);
  pyrt_execute(self->pyrt, strdup(code));
}

static void py_frame_pump(py_frame_t* self, int timeout_ms) {
  int waited = 0;
  while (ATOMIC_LOAD(&self->got_result) == 0 && waited < timeout_ms) {
    actor_run(&self->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
    waited++;
  }
  actor_run(&self->actor, ACTOR_BATCH_SIZE);
}

static void py_frame_free(py_frame_t* self) {
  if (self->pyrt != NULL) {
    pyrt_destroy(self->pyrt); /* join first: the mailbox must stop accepting sends */
  }
  /* Drain the mailbox: actor_destroy runs message_queue_destroy, which frees */
  /* any queued-but-undispatched payloads plus the queue's sentinel. */
  actor_destroy(&self->actor);
  for (auto* r : self->results) {
    pyrt_result_payload_destroy(r);
  }
  /* The frame is clear-allocated, so the C++ vector/string members are
     never destructed by free(): drain their buffers here or valgrind reports
     the harness vectors themselves as definitely-lost. */
  /* Assignment from the default-constructed empties frees the buffers
     (clear() alone would keep the allocator capacity alive). */
  self->results = std::vector<pyrt_result_payload_t*>();
  self->logs = std::vector<std::string>();
  self->statuses = std::vector<std::string>();
  free(self);
}

/* Frozen-API link contract: the runtime symbols must exist inside the
   library the test binary links. Referencing the addresses (not calling
   anything) keeps this safe before the runtime exists; until pyrt is
   implemented this test is the deliberate RED build of Task 3. */
TEST(TestPyrt, TestFrozenApiSymbolsResolve) {
  EXPECT_NE((void*)pyrt_create, (void*)NULL);
  EXPECT_NE((void*)pyrt_execute, (void*)NULL);
  EXPECT_NE((void*)pyrt_destroy, (void*)NULL);
}

TEST(TestPyrt, TestExecuteReturnsCorrMatchedResult) {
  py_frame_t* self = py_frame_create();

  py_frame_execute(self, "21 * 2");
  py_frame_pump(self, 10000);

  ASSERT_LT(self->results.size(), 2u);
  ASSERT_EQ(self->results.size(), 1u);
  pyrt_result_payload_t* r = self->results[0];
  EXPECT_EQ(r->status, 0);
  EXPECT_EQ(r->corr, 1u);
  EXPECT_STREQ(r->text, "42");

  py_frame_free(self);
}

TEST(TestPyrt, TestNamespacePersistsAcrossCells) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "x = 41");
  py_frame_pump(self, 10000);
  ATOMIC_STORE(&self->got_result, 0);
  py_frame_execute(self, "x + 1");
  py_frame_pump(self, 10000);

  ASSERT_LT(self->results.size(), 3u);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->status, 0);
  EXPECT_STREQ(self->results[1]->text, "42");

  py_frame_free(self);
}

TEST(TestPyrt, TestActorPreloadedInCellNamespace) {
  /* Live models emit bare `actor.*` calls (no `import actor`); the
     interpreter boot must bind the injected module so cells never see
     NameError: name 'actor' is not defined. */
  py_frame_t* self = py_frame_create();

  py_frame_execute(self, "actor.__name__");
  py_frame_pump(self, 10000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  EXPECT_STREQ(self->results[0]->text, "'actor'");

  py_frame_free(self);
}

TEST(TestPyrt, TestLogStreamsBeforeResult) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "for i in range(3):\n    import actor\n    actor.log('tick %d' % i)\n1 + 1");

  /* Stream check: narration visible while the cell is still running.
     Observed reality: the harness's own dispatch timestamps show the
     worker posts the three logs microseconds apart inside the still-
     running cell and the result right after (~7 us total) — while each
     dispatch costs ~1-5 us at -O0. So a 1 ms sleep-poll strides over the
     whole stream window, and ANY batch >=2 drain keeps swallowing
     messages the worker posts mid-drain until the result lands inside
     the same actor_run that first sees a log. Poll hot (no sleep) and
     drain exactly ONE message per call, breaking the instant a log is
     dispatched: a single-message run stops the drain dead, so the result
     cannot piggyback on the same actor_run and got_result must still be
     0 at the check below. */
  /* Time-bound instead of a pure spin budget: iteration counts have no
     wall-clock meaning (a fast host burns millions of spins; a slow host
     burns few while pyrt boot alone can cost tens of ms). Cap the stream
     window at 60 s of elapsed monotonic time — enough headroom for a slow
     harness (valgrind boots a subinterpreter in seconds). Keep the hot spin
     and the one-message-per-run drain: the FIFO order plus the one-message
     batch guarantee the result cannot piggyback on the same actor_run that
     first sees a log, whatever the polling rhythm. The only concession is a
     rare yield (every 1000 consecutive empty polls) so an emulated-slow
     worker (valgrind) is not CPU-starved by the spin. */
  uint64_t stream_started_ns = platform_monotonic_ns();
  int spins = 0;
  while (ATOMIC_LOAD(&self->got_result) == 0 &&
         (platform_monotonic_ns() - stream_started_ns) < 60000000000ULL) {
    if (message_queue_isempty(&self->actor.queue)) {
      if (++spins % 1000 == 0) {
        platform_sleep_ms(1);   /* rare yield, not a poll rhythm */
      }
      continue;   /* hot spin: no mail yet */
    }
    actor_run(&self->actor, 1);
    if (self->logs.size() > 0) {
      break;
    }
  }
  EXPECT_GT(self->logs.size(), 0u);
  EXPECT_EQ(ATOMIC_LOAD(&self->got_result), 0);   /* streaming, not buffered */

  py_frame_pump(self, 10000);
  ASSERT_LT(self->results.size(), 2u);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  EXPECT_EQ(self->logs.size(), 3u);
  EXPECT_STREQ(self->logs[0].c_str(), "tick 0");

  py_frame_free(self);
}

TEST(TestPyrt, TestFailingCellReturnsTracebackNotCrash) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "1 / 0");
  py_frame_pump(self, 10000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 1);
  EXPECT_NE(strstr(self->results[0]->text, "ZeroDivisionError"), nullptr);

  /* Plan-contract correction: the harness pump idles on got_result, which
     cell 1 left set — reset it so the second pump actually waits for the
     next corr-matched result (same pattern as
     TestNamespacePersistsAcrossCells). */
  ATOMIC_STORE(&self->got_result, 0);
  py_frame_execute(self, "40 + 2");   /* interpreter survived the exception */
  py_frame_pump(self, 10000);
  ASSERT_LT(self->results.size(), 3u);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->status, 0);
  EXPECT_STREQ(self->results[1]->text, "42");

  py_frame_free(self);
}

/* Interrupt contract, COOPERATIVE-only (verified against the pinned
   CPython 3.12.13 — see the comment on pyrt_interrupt): delivery happens at
   cell boundaries only. Exactly one truth is asserted: a long cell already
   running is NOT preemptible and completes with its normal corr-matched
   result, while a cell that has not started when interrupt() fires is cut
   at its start boundary with a corr-matched status-1 RESULT; the runtime
   then keeps serving cells normally. */
TEST(TestPyrt, TestInterruptHonoredAtBoundary) {
  py_frame_t* self = py_frame_create();

  /* Phase 1: the request arrives while a long cell is mid-flight. Wait for
     the cell's own log so the interrupt is guaranteed to land AFTER the
     dispatch decision (interpreter booted, cell started) — otherwise a slow
     boot would turn this into the start-boundary cut of phase 2. */
  py_frame_execute(self, "import actor\nactor.log('start')\nsum(range(5000000))");
  {
    /* 60 s: this wait exists only to land the interrupt after dispatch; it
       must survive slow harnesses (valgrind boots a subinterpreter in
       seconds) without changing behavior on fast ones. Sleep-poll, NOT a hot
       spin: unlike the stream-window test this is a pure state-arrival wait,
       and the hot variant starves the valgrind-emulated worker thread. */
    uint64_t started_ns = platform_monotonic_ns();
    while (self->logs.empty() &&
           (platform_monotonic_ns() - started_ns) < 60000000000ULL) {
      actor_run(&self->actor, 1);
      if (!self->logs.empty()) {
        break;
      }
      platform_sleep_ms(1);
    }
  }
  ASSERT_EQ(self->logs.size(), 1u);   /* cell is provably mid-flight */
  ATOMIC_STORE(&self->got_result, 0);
  pyrt_interrupt(self->pyrt);
  py_frame_pump(self, 100000);
  ASSERT_LT(self->results.size(), 2u);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);   /* completed, not cut */
  EXPECT_STREQ(self->results[0]->text, ""); /* exec-style cell, no output */
  EXPECT_EQ(self->results[0]->corr, 1u);

  /* Phase 2: the request arms BEFORE any cell runs, so the first queued
     cell is cut at its start boundary and posts a corr-matched result. */
  py_frame_t* b = py_frame_create();
  py_frame_open(b);
  pyrt_interrupt(b->pyrt);
  py_frame_execute(b, "2 + 2");
  py_frame_pump(b, 10000);
  ASSERT_LT(b->results.size(), 2u);
  ASSERT_EQ(b->results.size(), 1u);
  EXPECT_EQ(b->results[0]->status, 1);
  EXPECT_EQ(b->results[0]->corr, 1u);
  EXPECT_STREQ(b->results[0]->text, "pyrt: interrupted at boundary");

  /* The runtime keeps serving cells normally after the interrupt. */
  ATOMIC_STORE(&b->got_result, 0);
  py_frame_execute(b, "3 * 4");
  py_frame_pump(b, 10000);
  ASSERT_LT(b->results.size(), 3u);
  ASSERT_EQ(b->results.size(), 2u);
  EXPECT_EQ(b->results[1]->status, 0);
  EXPECT_STREQ(b->results[1]->text, "12");

  py_frame_free(b);
  py_frame_free(self);
}

/* Multi-phase actor module regression: a single-phase (m_size < 0) def is
   cached process-wide by CPython and the SECOND subinterpreter raises
   "module actor does not support loading in subinterpreters". The exec
   slot form (m_size >= 0) re-initializes per interpreter. */
TEST(TestPyrt, TestActorModuleLoadsInMultipleInterpreters) {
  py_frame_t* a = py_frame_create();
  py_frame_t* b = py_frame_create();
  py_frame_execute(a, "import actor\nactor.log('a')\n3 + 3");
  py_frame_pump(a, 10000);
  ASSERT_EQ(a->results.size(), 1u);
  EXPECT_EQ(a->results[0]->status, 0);
  ATOMIC_STORE(&a->got_result, 0);
  py_frame_execute(b, "import actor\nactor.log('b')\n4 + 4");
  py_frame_pump(b, 10000);
  ASSERT_LT(b->results.size(), 2u);
  ASSERT_EQ(b->results.size(), 1u);
  EXPECT_EQ(b->results[0]->status, 0);
  py_frame_free(a);
  py_frame_free(b);
}

/* Lazy boot contract: pyrt_create() starts NOTHING (no thread, no
   interpreter — pyrt_isactive stays 0) and the interpreter is only booted by
   the pyrt thread on the first EXECUTE. The isactive probe is inside the
   wait loop on purpose: before the first cell there is provably nothing on
   the runtime, and the probe fires only once the harness has already been
   waiting long enough (>200 ms) for a boot to have started. */
TEST(TestPyrt, TestLazyBootNoInterpreterBeforeFirstExecute) {
  py_frame_t* self = py_frame_create();
  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
  cfg.pool_cap = 0;
  cfg.idle_evict_ms = 0;
  self->pyrt = pyrt_create(&self->actor, &cfg);
  EXPECT_EQ(pyrt_isactive(self->pyrt), 0);   /* nothing booted yet */

  py_frame_execute(self, "1 + 1");
  int waited = 0;
  while (ATOMIC_LOAD(&self->got_result) == 0 && waited < 10000) {
    actor_run(&self->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
    waited++;
    if (waited > 200) {
      EXPECT_EQ(pyrt_isactive(self->pyrt), 1);   /* live well before result */
    }
  }
  EXPECT_EQ(pyrt_isactive(self->pyrt), 1);   /* stays live after the cell */
  py_frame_free(self);
}

/* Idle eviction contract: with idle_evict_ms armed and the interpreter live,
   the worker's timed wait fires after the idle window and tears the
   interpreter down (pyrt_isactive returns to 0); the slot returns to the
   pool; the runtime re-boots cleanly on the next cell. */
TEST(TestPyrt, TestIdleEvictionTearsInterpreterDownAndDiscontinuesNamespace) {
  py_frame_t* self = py_frame_create();
  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
  cfg.pool_cap = 0;
  cfg.idle_evict_ms = 80;
  self->pyrt = pyrt_create(&self->actor, &cfg);
  pyrt_execute(self->pyrt, strdup("a1 = 7"));
  py_frame_pump(self, 10000);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  ATOMIC_STORE(&self->got_result, 0);

  int waited = 0;
  while (pyrt_isactive(self->pyrt) != 0 && waited < 3000) {
    actor_run(&self->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(5);
    waited++;
  }
  EXPECT_EQ(pyrt_isactive(self->pyrt), 0);   /* evicted after the idle timeout */

  /* Eviction drops the namespace BY DESIGN (opt-in memory reclamation; frames
     needing continuity must not arm idle_evict_ms — spec §Capacity). */
  pyrt_execute(self->pyrt, strdup("a1 == 7"));
  py_frame_pump(self, 10000);
  ASSERT_LT(self->results.size(), 3u);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->status, 1);   /* NameError: namespace was evicted */
  EXPECT_NE(strstr(self->results[1]->text, "NameError"), nullptr);

  py_frame_free(self);
}

/* Subprocess backend (Task 9): same EXECUTE/RESULT envelope contract as the
   subinterpreter backend, stateless per cell. The pool cap applies to the
   spawn exactly like to an interpreter; stdout IS the result text. */
TEST(TestPyrt, TestSubprocessBackendSameContract) {
  py_frame_t* self = py_frame_create();
  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBPROCESS;
  cfg.pool_cap = 0;
  cfg.idle_evict_ms = 0;
  self->pyrt = pyrt_create(&self->actor, &cfg);
  pyrt_execute(self->pyrt, strdup("print('hello subprocess')"));
  py_frame_pump(self, 20000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  EXPECT_EQ(self->results[0]->corr, 1u);
  EXPECT_NE(strstr(self->results[0]->text, "hello subprocess"), nullptr);

  /* Stateless by design: the next cell must not see the previous namespace. */
  ATOMIC_STORE(&self->got_result, 0);
  pyrt_execute(self->pyrt, strdup("print('%s' % ('boom' if 'x' in globals() else 'clean'))"));
  py_frame_pump(self, 20000);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->status, 0);
  EXPECT_EQ(self->results[1]->corr, 2u);
  EXPECT_STREQ(self->results[1]->text, "clean\n");

  py_frame_free(self);
}

/* Failing subprocess cell: exactly ONE corr-matched status-1 result whose
   text is the traceback (stderr), never a crash and never silence. */
TEST(TestPyrt, TestSubprocessFailingCellReturnsTracebackOnOneResult) {
  py_frame_t* self = py_frame_create();
  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBPROCESS;
  cfg.pool_cap = 0;
  cfg.idle_evict_ms = 0;
  self->pyrt = pyrt_create(&self->actor, &cfg);
  pyrt_execute(self->pyrt, strdup("1 / 0"));
  py_frame_pump(self, 20000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 1);
  EXPECT_EQ(self->results[0]->corr, 1u);
  EXPECT_NE(strstr(self->results[0]->text, "ZeroDivisionError"), nullptr);
  /* Subprocess stderr must also surface as a streamed STATUS post. */
  EXPECT_EQ(self->statuses.size(), 1u);
  EXPECT_NE(strstr(self->statuses[0].c_str(), "ZeroDivisionError"), nullptr);
  /* The runtime survives the failure and serves the next cell normally. */
  ATOMIC_STORE(&self->got_result, 0);
  pyrt_execute(self->pyrt, strdup("print(40 + 2)"));
  py_frame_pump(self, 20000);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->status, 0);
  EXPECT_STREQ(self->results[1]->text, "42\n");

  py_frame_free(self);
}

/* Subprocess truncation contract: per-stream output is capped at 1 MiB and
   bytes past the cap are discarded, but the drain still reaches EOF so
   waitpid cannot hang and the spawn completes cleanly. The ONLY truth this
   asserts is bounded: the pump returns, the cell exit status is 0, and the
   captured text stays inside the cap with the first MiB intact. */
TEST(TestPyrt, TestSubprocessTruncatesAndNeverHangs) {
  py_frame_t* self = py_frame_create();
  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBPROCESS;
  cfg.pool_cap = 0;
  cfg.idle_evict_ms = 0;
  self->pyrt = pyrt_create(&self->actor, &cfg);
  pyrt_execute(self->pyrt, strdup("print('x' * 3000000)"));
  py_frame_pump(self, 30000);   /* must return although stdout exceeds 1 MiB */

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  EXPECT_LE(strlen(self->results[0]->text), (size_t)(1u << 20) + 1u);
  EXPECT_GT(strlen(self->results[0]->text), 0u);   /* first MiB arrived */
  py_frame_free(self);
}

/* Source cap (budget table §4): the ONE cap inside _pyrt_post_result bounds
   every routed/durable result text for BOTH backends. A 100 KiB result through
   the embedded backend must arrive as exactly 32 KiB of text + the marker
   naming the ORIGINAL length. */
TEST(TestPyrt, TestResultTextCappedAtSourceWithMarker) {
  py_frame_t* self = py_frame_create();

  /* Single-expression cell: the subinterpreter backend returns '' (status 0)
     for a valueless multi-line exec cell, so the result-text probe must ride
     the eval path. repr() wraps a str in quotes, so 102398 x's repr to
     exactly 102400 bytes — the marker must name THAT original length. */
  py_frame_execute(self, "'x' * 102398");
  py_frame_pump(self, 10000);

  ASSERT_EQ(self->results.size(), 1u);
  pyrt_result_payload_t* r = self->results[0];
  EXPECT_EQ(r->status, 0);
  ASSERT_NE(r->text, nullptr);
  /* The bounded shape (budget.h contract): cap bytes of text, then the
     marker naming the ORIGINAL length. */
  static const char kMarker[] = "[budget: truncated at 102400 bytes]";
  EXPECT_EQ(strlen(r->text), 32u * 1024u + (sizeof(kMarker) - 1) + 1u);
  EXPECT_EQ(r->text[32u * 1024u - 1u], 'x');
  EXPECT_EQ(r->text[32u * 1024u], '\n');
  EXPECT_EQ(r->text[32u * 1024u + 1u], '[');
  EXPECT_NE(strstr(r->text, kMarker), nullptr);

  py_frame_free(self);
}

/* Embedded stdout capture (spec §5, subprocess envelope parity): printed
   stdout IS the result; the repr joins as a "=> repr" closing line only when
   the last expression is not None; a crashing cell keeps its partial stdout
   ahead of the traceback. */
TEST(TestPyrt, TestPrintedStdoutIsTheResult) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "print('hello world')");
  py_frame_pump(self, 10000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  ASSERT_NE(self->results[0]->text, nullptr);
  EXPECT_STREQ(self->results[0]->text, "hello world");
  py_frame_free(self);
}

TEST(TestPyrt, TestStdoutAndReturnValueCompose) {
  /* The composed shape needs the EVAL path: a multi-statement cell like
     "print('head')\n41 + 1" only parses as exec, where the helper's result is
     None (parity with the subprocess envelope, which reports the child's
     stdout alone — TestExecCellStdoutAlone below). The eval-path compose is
     reached by an expression that prints as a side effect and yields a value. */
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "print('head') or 42");
  py_frame_pump(self, 10000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  ASSERT_NE(self->results[0]->text, nullptr);
  EXPECT_STREQ(self->results[0]->text, "head\n=> 42");
  py_frame_free(self);
}

TEST(TestPyrt, TestExecCellStdoutAlone) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "print('head')\n41 + 1");
  py_frame_pump(self, 10000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  ASSERT_NE(self->results[0]->text, nullptr);
  EXPECT_STREQ(self->results[0]->text, "head");
  py_frame_free(self);
}

TEST(TestPyrt, TestExceptionKeepsPartialStdout) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "print('before the crash')\n1 / 0");
  py_frame_pump(self, 10000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 1);
  ASSERT_NE(self->results[0]->text, nullptr);
  const char* text = self->results[0]->text;
  const char* stdout_at = strstr(text, "before the crash");
  const char* trace_at = strstr(text, "ZeroDivisionError");
  ASSERT_NE(stdout_at, nullptr);
  ASSERT_NE(trace_at, nullptr);
  EXPECT_LT(stdout_at, trace_at) << "the partial stdout rides the traceback's head";
  py_frame_free(self);
}

/* A small result never touches the marker: the cap is a clean copy when the
   text fits. */
TEST(TestPyrt, TestSmallResultUnchangedByCap) {
  py_frame_t* self = py_frame_create();

  py_frame_execute(self, "1 + 1");
  py_frame_pump(self, 10000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  ASSERT_NE(self->results[0]->text, nullptr);
  EXPECT_STREQ(self->results[0]->text, "2");

  py_frame_free(self);
}

#endif /* SA_HAS_PYTHON */

#ifndef SA_HAS_PYTHON
/* Keeps the file a valid non-empty test unit in libpython-free builds. */
TEST(TestPyrt, TestCompiledWithoutPython) { SUCCEED(); }
#endif

/* No main() here: testsecretagent already links gtest with the runner in
   test_main.cpp; a second main would collide at the link stage. */
