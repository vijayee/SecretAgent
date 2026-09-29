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
  free(self);
}

/* Pool-cap contract, in a binary where this is the first (and only) runtime:
   with the process-fixed pool at 1, the runtime that owns the only slot
   holds it until its interpreter is torn down; the second runtime's cell
   therefore QUEUES (backpressure, never an error) and drains with its own
   corr-matched result once the slot is released. */
TEST(TestPyrtPoolCap, TestPoolCapQueuesExecutesAndDrains) {
  py_frame_t* a = py_frame_create();
  py_frame_t* b = py_frame_create();

  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
  cfg.pool_cap = 1;               /* smallest pool: the FIRST create fixes it */
  cfg.idle_evict_ms = 0;
  a->pyrt = pyrt_create(&a->actor, &cfg);
  b->pyrt = pyrt_create(&b->actor, &cfg);

  /* The slot holder must be a DETERMINISTIC hold, not a compute cell: the
     planned sum(range(10**7)) finishes in ~300-450 ms on this host, INSIDE
     the polling window, so the backpressure assert raced the cell's own
     completion and cannot be run at all. time.sleep(3) guarantees the slot
     is held for >= 3 s (even a slow/loaded harness only lengthens the hold),
     while the backpressure window below polls for ~1 s. Also verifies
     `import time` boots in the PEP-684 subinterpreter (status-1 would mean
     the hold failed and the test cannot mean what it says). */
  pyrt_execute(a->pyrt, strdup("import time\ntime.sleep(3)"));  /* holds the only slot */
  pyrt_execute(b->pyrt, strdup("7 * 6"));               /* queues, never fails */
  int waited = 0;
  while (ATOMIC_LOAD(&b->got_result) == 0 && waited < 1000) {
    actor_run(&a->actor, ACTOR_BATCH_SIZE);
    actor_run(&b->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
    waited++;
  }
  EXPECT_EQ(ATOMIC_LOAD(&b->got_result), 0);   /* backpressure: b still queued */
  EXPECT_EQ(a->results.size(), 0u);            /* a still running its long cell */

  /* a finishes and its runtime is destroyed, releasing the slot for b. */
  py_frame_free(a);
  while (ATOMIC_LOAD(&b->got_result) == 0 && waited < 60000) {
    actor_run(&b->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(5);
    waited++;
  }
  ASSERT_LT(b->results.size(), 2u);
  ASSERT_EQ(b->results.size(), 1u);
  EXPECT_EQ(b->results[0]->status, 0);
  EXPECT_STREQ(b->results[0]->text, "42");

  py_frame_free(b);
}