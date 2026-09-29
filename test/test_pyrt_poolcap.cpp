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
#include "../src/Python/pyrt.h"
#include "../src/Python/pyrt_messages.h"
}

/* Order-isolated harness for the pool-cap contract (mirrors the py_frame_t
   harness in test_pyrt.cpp). Isolation is REQUIRED, not stylistic: the
   interpreter pool cap is fixed by the FIRST pyrt created in the process, so
   the cap=1 test must run in a binary where it is the only runtime creator —
   sharing testsecretagent means an earlier test's pool_cap=0 runtime pins the
   cap to 2x cores and the test below cannot exercise backpressure at all
   (empirically: the slot-holder cell completed inside the polling window and
   both EXPECTs fired against a pool that was never actually capped).
   No main() here: gtest_main provides the runner. */

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

static py_frame_t* py_frame_create(void) {
  py_frame_t* self = (py_frame_t*)get_clear_memory(sizeof(py_frame_t));
  actor_init(&self->actor, self, py_frame_dispatch, NULL); /* NULL pool: pumped by hand */
  ATOMIC_STORE(&self->got_result, 0);
  return self;
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
  /* The frame is plain-get_memory'd, so the C++ vector/string members are
     never destructed by free(): drain their buffers here or valgrind reports
     the harness vectors themselves as definitely-lost. */
  /* Assignment from the default-constructed empties frees the buffers
     (clear() alone would keep the allocator capacity alive). */
  self->results = std::vector<pyrt_result_payload_t*>();
  self->logs = std::vector<std::string>();
  self->statuses = std::vector<std::string>();
  free(self);
}

/* Pool-cap contract, in a binary where this is the first (and only) runtime:
   with the process-fixed pool at 1, the runtime that wins the only slot
   holds it until its interpreter is torn down; the other runtime's cell
   therefore QUEUES (backpressure, never an error) and drains with its own
   corr-matched result once the slot is released.

   Winner-agnostic BY DESIGN: whichever worker reaches the slot acquire first
   wins (their threads race after the same process-wide init — observed to
   invert under valgrind's scheduling). The test determines the winner from
   the first corr-matched RESULT that lands, then destroys the holder FIRST:
   a queued runtime's worker blocks inside _pyrt_slot_acquire, so destroying
   the waiter before the holder would deadlock pyrt_destroy's join. */
TEST(TestPyrtPoolCap, TestPoolCapQueuesExecutesAndDrains) {
  py_frame_t* a = py_frame_create();
  py_frame_t* b = py_frame_create();

  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
  cfg.pool_cap = 1;               /* smallest pool: the FIRST create fixes it */
  cfg.idle_evict_ms = 0;
  a->pyrt = pyrt_create(&a->actor, &cfg);
  b->pyrt = pyrt_create(&b->actor, &cfg);

  /* The slot holder's cell must be a DETERMINISTIC hold, not a compute cell:
     the planned sum(range(10**7)) finishes in ~300-450 ms on this host,
     INSIDE any polling window, so the backpressure assert would race the
     cell's own completion. time.sleep(3) guarantees the slot is held until
     the cell completes (a slow/loaded harness only lengthens the hold). */
  pyrt_execute(a->pyrt, strdup("import time\ntime.sleep(3)"));
  pyrt_execute(b->pyrt, strdup("7 * 6"));

  /* Wait for the WINNER of the single slot to post its corr-matched result.
     180 s wall bound: under valgrind the process-wide CPython init plus the
     booting subinterpreter costs tens of seconds; under load the hold only
     grows. */
  uint64_t started_ns = platform_monotonic_ns();
  py_frame_t* w = NULL;
  py_frame_t* l = NULL;
  while (w == NULL && (platform_monotonic_ns() - started_ns) < 180000000000ULL) {
    actor_run(&a->actor, ACTOR_BATCH_SIZE);
    actor_run(&b->actor, ACTOR_BATCH_SIZE);
    if (ATOMIC_LOAD(&a->got_result) != 0) {
      w = a;
      l = b;
    } else if (ATOMIC_LOAD(&b->got_result) != 0) {
      w = b;
      l = a;
    } else {
      platform_sleep_ms(5);
    }
  }
  ASSERT_NE(w, (py_frame_t*)nullptr);   /* a result DID land within the bound */

  /* Backpressure, asserted over the holder's ENTIRE hold (not a fixed
     window): the loser is provably still queued — the winner retains the
     slot until its runtime is destroyed (results always post before
     teardown), so the loser cannot even boot while the winner lives. */
  EXPECT_EQ(ATOMIC_LOAD(&l->got_result), 0);
  EXPECT_EQ(l->results.size(), 0u);

  /* The winner's cell completed with its normal corr-matched result; a
     status-1 here would also surface a subinterpreter boot failure, which
     must not masquerade as passing backpressure. */
  ASSERT_EQ(w->results.size(), 1u);
  EXPECT_EQ(w->results[0]->status, 0);
  if (w == a) {
    EXPECT_STREQ(w->results[0]->text, "");   /* exec-style hold, no output */
  } else {
    EXPECT_STREQ(w->results[0]->text, "42");
  }

  /* Destroying the holder tears its interpreter down and releases the slot. */
  py_frame_free(w);
  while (ATOMIC_LOAD(&l->got_result) == 0 &&
         (platform_monotonic_ns() - started_ns) < 360000000000ULL) {
    actor_run(&l->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(5);
  }
  ASSERT_EQ(l->results.size(), 1u);
  EXPECT_EQ(l->results[0]->status, 0);
  if (l == a) {
    EXPECT_STREQ(l->results[0]->text, "");
  } else {
    EXPECT_STREQ(l->results[0]->text, "42");
  }

  py_frame_free(l);
}

/* Queued-destroy contract (reviewer-mandated regression): with the process
   pool at 1 and the holder keeping the only slot for its whole lifetime, the
   second runtime's cell is QUEUED — its worker is parked inside
   _pyrt_slot_acquire on the GLOBAL pool condvar, which pyrt_destroy used to
   never wake, so destroying the queued runtime hung at the join for as long
   as every slot stayed held. Now the worker's slot wait is shutdown-aware
   (timed, re-observes shutdown) and the stranded cell exits with a
   corr-matched status-1 refusal, so the destroy's join is bounded. */
TEST(TestPyrtPoolCap, TestDestroyOfQueuedRuntimeReturnsPromptlyWithCorrMatchedRefusal) {
  py_frame_t* a = py_frame_create();
  py_frame_t* b = py_frame_create();

  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
  cfg.pool_cap = 1;               /* the FIRST create fixes it */
  cfg.idle_evict_ms = 0;
  a->pyrt = pyrt_create(&a->actor, &cfg);
  b->pyrt = pyrt_create(&b->actor, &cfg);

  /* Holder: deterministic whole-lifetime slot hold (time.sleep(3) outlasts
     every bounded wait below; see the pool-cap test above). Queue the holder
     FIRST and let its cell start so the slot race is settled before b queues:
     a retains the slot until ITS destroy. The 3 s wall-clock hold outlives
     every bounded wait below even under valgrind's scheduling. */
  pyrt_execute(a->pyrt, strdup("import time\ntime.sleep(3)"));
  {
    uint64_t holder_ns = platform_monotonic_ns();
    while (ATOMIC_LOAD(&a->got_result) == 0 &&
           (platform_monotonic_ns() - holder_ns) < 180000000000ULL) {
      actor_run(&a->actor, ACTOR_BATCH_SIZE);
      platform_sleep_ms(5);
    }
    ASSERT_EQ(a->results.size(), 1u);   /* slot provably acquired + held */
  }
  uint64_t corr_b = pyrt_execute(b->pyrt, strdup("7 * 6"));

  /* The queued runtime must be provably queued: nothing at all lands in its
     mailbox during the observation window (the holder retains the slot until
     ITS destroy, so b cannot even boot). */
  int waited = 0;
  while (ATOMIC_LOAD(&b->got_result) == 0 && waited < 400) {
    actor_run(&b->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(5);
    waited++;
  }
  EXPECT_EQ(ATOMIC_LOAD(&b->got_result), 0);   /* still queued */
  EXPECT_EQ(b->results.size(), 0u);

  /* Destroy the QUEUED runtime: must NOT hang on the full pool. */
  uint64_t start_ns = platform_monotonic_ns();
  pyrt_destroy(b->pyrt);
  b->pyrt = NULL;
  uint64_t elapsed_ms = (platform_monotonic_ns() - start_ns) / 1000000;
  EXPECT_LT(elapsed_ms, 3000);                 /* prompt, bounded join */

  /* The stranded cell's refusal RESULT must arrive in the mailbox the
     destroy was racing: pump the still-alive actor so it is dispatched
     (b->pyrt is NULL now; py_frame_free's guard owns no runtime). */
  int pumped = 0;
  while (b->results.empty() && pumped < 2000) {
    actor_run(&b->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
    pumped++;
  }
  ASSERT_EQ(b->results.size(), 1u);
  EXPECT_EQ(b->results[0]->status, 1);          /* refusal */
  EXPECT_EQ(b->results[0]->corr, corr_b);       /* corr-matched */
  EXPECT_NE(strstr(b->results[0]->text, "destroyed while queued"), nullptr);

  /* The holder's hold cell already settled (asserted above); free cleanly. */
  py_frame_free(a);
  py_frame_free(b);
}