//
// Created by victor on 9/30/26.
//
// The runtime's one IO reactor thread — the loop_thread.h contract realized
// on poll-dancer's portable surface:
//
//   - The thread runs a pd_loop forever (pd_loop_run_once ticks with a 100ms
//     ceiling, the same shape http_server.c's I/O thread uses, so stop is
//     prompt and pd's async eventfd wake keeps call latency at µs).
//   - Cross-thread traffic arrives ONLY through streams_loop_call, which
//     queues a fn/ctx node under a mutex and wakes the loop with
//     pd_loop_async_send. The data pointer pd_loop_get_async_data carries is
//     deliberately unused: pd stores only the MOST RECENT async value, so a
//     fan of concurrent callers would overwrite it — the FIFO here is the
//     adaptation the header requires ("adapt HERE, never at call sites").
//   - Fail-loud: the thread's only exit paths are the stop flag and a
//     log_error + abort (a half-dead reactor must never limp).

#include "loop_thread.h"

#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include "../Util/log.h"
#include "../Platform/platform_thread.h"
#include <poll-dancer/poll-dancer.h>

#include <stdlib.h>

/* The event-loop wait ceiling (ms): an idle loop still ticks, so queued
   calls can never be stranded even without an async wake; a wake delivers
   them at µs scale. */
#define _LOOP_TICK_MS 100

typedef struct loop_call_node_t {
  void (*fn)(void*);
  void* ctx;
  struct loop_call_node_t* next;
} loop_call_node_t;

struct streams_loop_thread_t {
  pd_loop_t* loop;
  platform_thread_t* thread;
  ATOMIC(uint8_t) stop;     /* destroy request: thread exits at the next tick */
  ATOMIC(uint8_t) alive;    /* 1 from create until the thread exits for any reason */
  platform_mutex_t* call_lock;
  loop_call_node_t* call_head;   /* FIFO: callers' queued fn/ctx pairs */
  loop_call_node_t* call_tail;
};

/* Pops the whole pending-call FIFO (the caller runs the batch); the loop
   thread is the only popper, callers are the only pushers. */
static loop_call_node_t* _call_batch_take(streams_loop_thread_t* lt) {
  loop_call_node_t* batch;
  platform_mutex_lock(lt->call_lock);
  batch = lt->call_head;
  lt->call_head = NULL;
  lt->call_tail = NULL;
  platform_mutex_unlock(lt->call_lock);
  return batch;
}

static void* _loop_thread_main(void* arg) {
  streams_loop_thread_t* lt = (streams_loop_thread_t*)arg;
  platform_thread_setup_stack();

  while (!ATOMIC_LOAD(&lt->stop)) {
    int result = pd_loop_run_once(lt->loop, _LOOP_TICK_MS);
    if (result < 0) {
      ATOMIC_STORE(&lt->alive, 0);
      log_error("loop_thread: pd_loop_run_once failed (%d): a half-dead reactor "
                "must never limp", result);
      abort();
    }
    /* The async wake only interrupted the wait; queued work runs here. */
    loop_call_node_t* node = _call_batch_take(lt);
    while (node != NULL) {
      loop_call_node_t* next = node->next;
      node->fn(node->ctx);
      free(node);
      node = next;
    }
  }

  /* Normal exit (destroy): fail loud if calls were accepted but never
     delivered — dropping a caller's fn silently would strand its heap ctx. */
  loop_call_node_t* stranded = _call_batch_take(lt);
  if (stranded != NULL) {
    int count = 0;
    while (stranded != NULL) {
      loop_call_node_t* next = stranded->next;
      free(stranded);
      stranded = next;
      count++;
    }
    log_error("loop_thread: %d accepted calls stranded at destroy (loop "
              "consumers must quiesce before streams_loop_destroy)", count);
  }
  ATOMIC_STORE(&lt->alive, 0);
  return NULL;
}

streams_loop_thread_t* streams_loop_create(void) {
  streams_loop_thread_t* lt = get_clear_memory(sizeof(streams_loop_thread_t));
  lt->loop = pd_loop_create(NULL);
  if (lt->loop == NULL) {
    log_error("streams_loop_create: pd_loop_create failed");
    free(lt);
    return NULL;
  }
  lt->call_lock = platform_mutex_create();
  if (lt->call_lock == NULL) {
    log_error("streams_loop_create: call-lock creation failed");
    pd_loop_destroy(lt->loop);
    free(lt);
    return NULL;
  }
  /* Alive before the thread starts, so callers may enqueue immediately;
     the loop drains what arrived during startup at its first tick. */
  ATOMIC_STORE(&lt->alive, 1);
  lt->thread = platform_thread_create(_loop_thread_main, lt);
  if (lt->thread == NULL) {
    ATOMIC_STORE(&lt->alive, 0);
    log_error("streams_loop_create: reactor thread creation failed");
    platform_mutex_destroy(lt->call_lock);
    pd_loop_destroy(lt->loop);
    free(lt);
    return NULL;
  }
  return lt;
}

int streams_loop_call(streams_loop_thread_t* lt, void (*fn)(void*), void* ctx) {
  if (lt == NULL || fn == NULL) return -1;
  if (!ATOMIC_LOAD(&lt->alive) || ATOMIC_LOAD(&lt->stop)) {
    return -1;   /* the loop is gone */
  }
  loop_call_node_t* node = get_clear_memory(sizeof(loop_call_node_t));
  node->fn = fn;
  node->ctx = ctx;

  platform_mutex_lock(lt->call_lock);
  if (lt->call_tail != NULL) {
    lt->call_tail->next = node;
  } else {
    lt->call_head = node;
  }
  lt->call_tail = node;
  platform_mutex_unlock(lt->call_lock);

  if (pd_loop_async_send(lt->loop, NULL) != PD_OK) {
    /* The wake failed, but the queue is intact and the loop's own tick
       delivers within _LOOP_TICK_MS — the call is accepted either way. */
    log_error("streams_loop_call: async wake failed; delivery falls back to "
              "the %dms tick", _LOOP_TICK_MS);
  }
  return 0;
}

struct pd_loop* streams_loop_raw(streams_loop_thread_t* lt) {
  if (lt == NULL || !ATOMIC_LOAD(&lt->alive)) {
    return NULL;
  }
  return lt->loop;
}

void streams_loop_destroy(streams_loop_thread_t* lt) {
  if (lt == NULL) return;
  platform_thread_t* thread = lt->thread;
  ATOMIC_STORE(&lt->stop, 1);
  (void)pd_loop_async_send(lt->loop, NULL);   /* wake the tick early */
  if (thread != NULL) {
    platform_thread_join(thread);
  }
  pd_loop_destroy(lt->loop);
  platform_mutex_destroy(lt->call_lock);
  free(lt);
}