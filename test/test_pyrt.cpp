//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <atomic>
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

static py_frame_t* py_frame_create(void) {
  py_frame_t* self = (py_frame_t*)get_clear_memory(sizeof(py_frame_t));
  actor_init(&self->actor, self, py_frame_dispatch, NULL); /* NULL pool: pumped by hand */
  ATOMIC_STORE(&self->got_result, 0);
  return self;
}

static void py_frame_execute(py_frame_t* self, const char* code) {
  if (self->pyrt == NULL) {
    pyrt_config_t cfg;
    cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
    cfg.pool_cap = 0;
    cfg.idle_evict_ms = 0;
    self->pyrt = pyrt_create(&self->actor, &cfg);
  }
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

#endif /* SA_HAS_PYTHON */

#ifndef SA_HAS_PYTHON
/* Keeps the file a valid non-empty test unit in libpython-free builds. */
TEST(TestPyrt, TestCompiledWithoutPython) { SUCCEED(); }
#endif

/* No main() here: testsecretagent already links gtest with the runner in
   test_main.cpp; a second main would collide at the link stage. */