//
// Created by victor on 5/6/25.
//

#include "scheduler.h"
#include "../Actor/actor.h"
#include "../RefCounter/refcounter.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Platform/platform.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define STEAL_THRESHOLD 10

static PLATFORM_THREAD_LOCAL scheduler_t* current_scheduler = NULL;

static void _inject_queue_init(inject_queue_t* queue) {
  queue->lock = platform_mutex_create();
  queue->condition = platform_condvar_create();
  queue->head = NULL;
  queue->tail = NULL;
}

static void _inject_queue_destroy(inject_queue_t* queue) {
  platform_mutex_destroy(queue->lock);
  platform_condvar_destroy(queue->condition);
  inject_node_t* node = queue->head;
  while (node != NULL) {
    inject_node_t* next = node->next;
    free(node);
    node = next;
  }
}

static void _inject_queue_push(inject_queue_t* queue, actor_t* actor) {
  inject_node_t* node = get_clear_memory(sizeof(inject_node_t));
  node->actor = actor;
  node->next = NULL;
  platform_mutex_lock(queue->lock);
  if (queue->tail != NULL) {
    queue->tail->next = node;
  } else {
    queue->head = node;
  }
  queue->tail = node;
  platform_mutex_unlock(queue->lock);
  platform_condvar_signal(queue->condition);
}

static actor_t* _inject_queue_pop(inject_queue_t* queue) {
  platform_mutex_lock(queue->lock);
  inject_node_t* node = queue->head;
  if (node == NULL) {
    platform_mutex_unlock(queue->lock);
    return NULL;
  }
  queue->head = node->next;
  if (queue->head == NULL) {
    queue->tail = NULL;
  }
  actor_t* actor = node->actor;
  free(node);
  platform_mutex_unlock(queue->lock);
  return actor;
}

static actor_t* _scheduler_try_steal(scheduler_pool_t* pool, scheduler_t* self) {
  uint32_t victim_index = atomic_load_explicit(&self->last_victim, memory_order_relaxed);
  for (uint32_t attempt = 0; attempt < pool->worker_count; attempt++) {
    victim_index = (victim_index == 0) ? (uint32_t)(pool->worker_count - 1) : victim_index - 1;
    if (victim_index == self->index) {
      continue;
    }
    void* stolen = deque_steal(&pool->workers[victim_index].local_queue);
    if (stolen != DEQUE_EMPTY && stolen != DEQUE_ABORT) {
      atomic_store_explicit(&self->last_victim, victim_index, memory_order_relaxed);
      return (actor_t*)stolen;
    }
  }
  return NULL;
}

static actor_t* _scheduler_find_work(scheduler_pool_t* pool, scheduler_t* self) {
  // Fast path: pop from local deque
  void* item = deque_pop(&self->local_queue);
  if (item != DEQUE_EMPTY && item != DEQUE_ABORT) {
    return (actor_t*)item;
  }

  // Try inject queue
  actor_t* actor = _inject_queue_pop(&pool->inject);
  if (actor != NULL) {
    return actor;
  }

  // Try stealing from other workers
  return _scheduler_try_steal(pool, self);
}

static void* _scheduler_worker_loop(void* arg) {
  platform_thread_setup_stack();
  scheduler_pool_t* pool = (scheduler_pool_t*)arg;
  // Each worker thread atomically claims its index via fetch_add
  uint32_t my_index = atomic_fetch_add(&pool->active_count, 1);
  scheduler_t* self = &pool->workers[my_index];
  current_scheduler = self;

  platform_barrier_wait(pool->barrier);

  uint32_t spin_count = 0;
  while (!atomic_load_explicit(&pool->terminate, memory_order_acquire)) {
    actor_t* actor = _scheduler_find_work(pool, self);
    if (actor != NULL) {
      spin_count = 0;
      self->current = actor;
      uint8_t flags = atomic_load(&actor->flags);
      if (flags & ACTOR_FLAG_DESTROY) {
        /* Actor has been destroyed — skip it. It is still in our deque; we
           leave it there (it will be skipped on every pop) and transition
           its queue_state to IDLE so the destroyer can proceed. */
        atomic_store(&actor->queue_state, ACTOR_QUEUE_IDLE);
        self->current = NULL;
        continue;
      }
      if (flags & ACTOR_FLAG_MUTED) {
        /* Actor is muted due to backpressure — re-queue it for later */
        self->current = NULL;
        deque_push(&self->local_queue, (void*)actor);
        continue;
      }
      if (flags & ACTOR_FLAG_RUNNING) {
        /* Another worker is already running this actor; re-queue it */
        deque_push(&self->local_queue, (void*)actor);
        continue;
      }
      if (!atomic_compare_exchange_strong(&actor->flags, &flags, flags | ACTOR_FLAG_RUNNING)) {
        deque_push(&self->local_queue, (void*)actor);
        continue;
      }
      /* We won the RUNNING flag. Record that we are running this actor so
         actor_destroy can distinguish "in a deque" from "being run". */
      atomic_store(&actor->queue_state, ACTOR_QUEUE_RUNNING);

      bool has_more = actor_run(actor, ACTOR_BATCH_SIZE);
      /* Atomically decide: re-queue (RUNNING->QUEUED) or release
         (RUNNING->IDLE), and clear RUNNING. The destroyer waits for IDLE
         before freeing, so the re-queue below cannot push a freeable
         pointer. */
      uint8_t destroy_flags = atomic_load(&actor->flags);
      bool destroy_set = (destroy_flags & ACTOR_FLAG_DESTROY) != 0;
      if (has_more && !destroy_set) {
        atomic_store(&actor->queue_state, ACTOR_QUEUE_QUEUED);
        atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_RUNNING);
        self->current = NULL;
        deque_push(&self->local_queue, (void*)actor);
        // Signal other workers that might be sleeping
        platform_mutex_lock(pool->inject.lock);
        platform_condvar_broadcast(pool->inject.condition);
        platform_mutex_unlock(pool->inject.lock);
      } else {
        atomic_store(&actor->queue_state, ACTOR_QUEUE_IDLE);
        atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_RUNNING);
        self->current = NULL;
      }
    } else {
      spin_count++;
      if (spin_count >= STEAL_THRESHOLD) {
        platform_mutex_lock(pool->inject.lock);

        /* Re-check inject queue under the lock to prevent lost wakeup.
           _inject_queue_push holds inject.lock while enqueuing work, so if
           we see an empty queue here, any future push will see our idle_count
           increment and its signal will arrive after we begin waiting. */
        if (pool->inject.head != NULL) {
          platform_mutex_unlock(pool->inject.lock);
          spin_count = 0;
          continue;
        }

        atomic_fetch_add(&pool->idle_count, 1);
        if (atomic_load(&pool->idle_count) == pool->worker_count) {
          platform_mutex_lock(pool->idle_lock);
          platform_condvar_signal(pool->idle);
          platform_mutex_unlock(pool->idle_lock);
        }

        if (!atomic_load_explicit(&pool->terminate, memory_order_acquire)) {
          platform_condvar_wait(pool->inject.condition, pool->inject.lock);
        }
        platform_mutex_unlock(pool->inject.lock);
        atomic_fetch_sub(&pool->idle_count, 1);
        spin_count = 0;
      }
    }
  }

  current_scheduler = NULL;
  return 0;
}

scheduler_pool_t* scheduler_pool_create(size_t worker_count) {
  scheduler_pool_t* pool = get_clear_memory(sizeof(scheduler_pool_t));
  pool->worker_count = worker_count;
  pool->workers = get_clear_memory(sizeof(scheduler_t) * worker_count);
  for (size_t index = 0; index < worker_count; index++) {
    pool->workers[index].index = index;
    deque_init(&pool->workers[index].local_queue);
    atomic_store_explicit(&pool->workers[index].last_victim, (uint32_t)((index + 1) % worker_count), memory_order_relaxed);
  }
  _inject_queue_init(&pool->inject);
  pool->barrier = platform_barrier_create(worker_count + 1);
  pool->idle_lock = platform_mutex_create();
  pool->idle = platform_condvar_create();
  pool->deref_lock = platform_mutex_create();
  pool->pending_derefs = NULL;
  pool->registry_lock = platform_mutex_create();
  pool->registry_head = NULL;
  atomic_store_explicit(&pool->idle_count, 0, memory_order_relaxed);
  atomic_store_explicit(&pool->active_count, 0, memory_order_relaxed);
  atomic_store_explicit(&pool->terminate, 0, memory_order_relaxed);
  atomic_store_explicit(&pool->stopped, 0, memory_order_relaxed);
  atomic_store_explicit(&pool->pending_messages, 0, memory_order_relaxed);
  return pool;
}

void scheduler_pool_destroy(scheduler_pool_t* pool) {
  /* Detach every still-registered actor from this pool before the pool's
     memory (registry_lock, pending_messages) is freed. Existing call sites
     routinely destroy the pool BEFORE destroying its actors
     (e.g. scheduler_pool_destroy then actor_destroy), and actor_destroy /
     message_queue_destroy touch actor->pool (to unregister from the registry
     and to decrement pending_messages). Nulling each actor's pool pointer and
     pending_counter here makes those later calls no-ops instead of
     use-after-frees. Workers are already joined (scheduler_pool_stop) and no
     actor_send can land (callers quiesce before destroy), so this is race-free. */
  platform_mutex_lock(pool->registry_lock);
  actor_t* actor = pool->registry_head;
  while (actor != NULL) {
    actor_t* next = actor->registry_next;
    actor->pool = NULL;
    actor->queue.pending_counter = NULL;
    actor->registry_prev = NULL;
    actor->registry_next = NULL;
    actor = next;
  }
  pool->registry_head = NULL;
  platform_mutex_unlock(pool->registry_lock);

  /* Drain any remaining pending derefs */
  scheduler_pool_drain_pending_derefs(pool);
  platform_mutex_destroy(pool->deref_lock);
  platform_mutex_destroy(pool->registry_lock);
  for (size_t index = 0; index < pool->worker_count; index++) {
    deque_destroy(&pool->workers[index].local_queue);
  }
  _inject_queue_destroy(&pool->inject);
  platform_barrier_destroy(pool->barrier);
  platform_mutex_destroy(pool->idle_lock);
  platform_condvar_destroy(pool->idle);
  free(pool->workers);
  free(pool);
}

void scheduler_pool_start(scheduler_pool_t* pool) {
  atomic_store_explicit(&pool->idle_count, 0, memory_order_relaxed);
  atomic_store_explicit(&pool->active_count, 0, memory_order_relaxed);
  atomic_store_explicit(&pool->terminate, 0, memory_order_release);
  atomic_store_explicit(&pool->stopped, 0, memory_order_release);
  for (size_t index = 0; index < pool->worker_count; index++) {
    pool->workers[index].thread = platform_thread_create(_scheduler_worker_loop, pool);
    if (pool->workers[index].thread == NULL) {
      log_error("scheduler_pool_start: failed to create worker thread %zu of %zu", index, pool->worker_count);
      abort();
    }
  }
  platform_barrier_wait(pool->barrier);
}

void scheduler_pool_stop(scheduler_pool_t* pool) {
  atomic_store_explicit(&pool->terminate, 1, memory_order_release);
  // Wake all sleeping workers
  platform_mutex_lock(pool->inject.lock);
  platform_condvar_broadcast(pool->inject.condition);
  platform_mutex_unlock(pool->inject.lock);
  for (size_t index = 0; index < pool->worker_count; index++) {
    platform_thread_join(pool->workers[index].thread);
  }
  /* Workers are now joined — no worker thread can touch an actor on this
     pool. actor_destroy's wait for queue_state == IDLE can break out once
     this is set, because an actor may still be QUEUED (an external thread
     like the timer loop can inject work after stop) but no live worker
     will dereference it. */
  atomic_store_explicit(&pool->stopped, 1, memory_order_release);
}

void scheduler_inject(scheduler_pool_t* pool, actor_t* actor) {
  _inject_queue_push(&pool->inject, actor);
}

void scheduler_pool_defer_cleanup(scheduler_pool_t* pool, void* object, void (*destructor)(void*)) {
  if (object == NULL) return;
  // Hold a reference so the object can't be freed before the drain runs
  refcounter_reference((refcounter_t*)object);
  pending_deref_node_t* node = get_clear_memory(sizeof(pending_deref_node_t));
  node->object = object;
  node->destructor = destructor;
  platform_mutex_lock(pool->deref_lock);
  node->next = pool->pending_derefs;
  pool->pending_derefs = node;
  platform_mutex_unlock(pool->deref_lock);
}

void scheduler_pool_drain_pending_derefs(scheduler_pool_t* pool) {
  pending_deref_node_t* node;

  platform_mutex_lock(pool->deref_lock);
  node = pool->pending_derefs;
  pool->pending_derefs = NULL;
  platform_mutex_unlock(pool->deref_lock);

  while (node != NULL) {
    pending_deref_node_t* next = node->next;
    node->destructor(node->object);
    free(node);
    node = next;
  }
}

/* Recovery scan for stranded actors. Called by scheduler_pool_wait_for_idle
   when every worker is idle yet pending_messages is non-zero — the symptom of
   the elusive work-stealing strand, where a message was pushed to an actor's
   mailbox but the actor is in no worker's deque/inject queue and is not
   running (so no worker will ever pick it up on its own). Re-injecting such an
   actor wakes a worker to drain its mailbox, restoring forward progress.

   The race itself is emergent from the inject/idle/steal interaction and is
   not pinned to a single structure; this scan makes wait_for_idle correct
   regardless of how a message strands, which is the property callers actually
   depend on (every queued message is dispatched before wait_for_idle returns).

   A strand can also be a lost wakeup: an actor whose SCHEDULED flag is set but
   which is in no worker's deque and not in the inject queue (the schedule+inject
   raced with the idle machinery and was lost). Such a stale-SCHEDULED actor is
   re-injected too — when every worker is idle, a SCHEDULED flag can only be
   stale, because a genuinely-scheduled actor would occupy a worker's deque or
   the inject queue and that worker would not be idle. Re-injecting cannot
   double-run it: no worker holds it when all are idle.

   Only DESTROY-flagged actors are skipped: their owner is tearing them down
   and will drain the mailbox itself via message_queue_destroy (which decrements
   pending_messages), so re-injecting would race with that teardown.

   registry_lock is held throughout, including across scheduler_inject. No
   other path acquires registry_lock while holding inject.lock (actor_send and
   backpressure_release take inject.lock without registry_lock; actor_destroy
   takes them sequentially — backpressure_release releases inject.lock before
   actor_detach_pool takes registry_lock), so this nested registry->inject order
   is deadlock-free. Holding the lock for the whole scan also means a concurrent
   actor_destroy / actor_detach_pool (both take registry_lock) cannot free an
   actor mid-scan, so the scan never dereferences freed memory. Returns true if
   any actor was re-injected, false if none was re-injectable (meaning the
   remaining pending messages live only in DESTROY actors whose owner will
   drain them). */
static bool _scheduler_pool_reinject_stranded(scheduler_pool_t* pool) {
  bool reinjected = false;
  platform_mutex_lock(pool->registry_lock);
  actor_t* actor = pool->registry_head;
  while (actor != NULL) {
    uint8_t flags = atomic_load(&actor->flags);
    if (!(flags & ACTOR_FLAG_DESTROY) && !message_queue_isempty(&actor->queue)) {
      atomic_fetch_or(&actor->flags, ACTOR_FLAG_SCHEDULED);
      /* Re-injection puts the actor into a worker deque: transition to
         QUEUED so a concurrent actor_destroy cannot free it while it is
         in flight. */
      atomic_store(&actor->queue_state, ACTOR_QUEUE_QUEUED);
      scheduler_inject(pool, actor);
      reinjected = true;
    }
    actor = actor->registry_next;
  }
  platform_mutex_unlock(pool->registry_lock);
  return reinjected;
}

int scheduler_pool_wait_for_idle(scheduler_pool_t* pool) {
  if (pool == NULL) return 0;
  if (atomic_load(&pool->terminate)) return 0;

  /* Bound on consecutive "all idle yet messages remain" recovery attempts. A
     correct run needs at most a handful (one per stranded actor batch); this
     only trips if the pending-messages counter desyncs from the mailboxes or
     the recovery scan fails to make progress — a real bug worth failing loudly
     rather than hanging silently. */
  static const uint32_t RECOVERY_LIMIT = 10000;
  uint32_t recovery_attempts = 0;

  platform_mutex_lock(pool->idle_lock);
  for (;;) {
    if (atomic_load(&pool->idle_count) == pool->worker_count &&
        atomic_load(&pool->pending_messages) == 0) {
      break;
    }
    if (atomic_load(&pool->idle_count) == pool->worker_count) {
      /* All workers idle but messages remain. Re-inject any stranded live
         actor so a worker wakes and drains its mailbox. If none is
         re-injectable, the remaining pending messages live only in
         DESTROY-flagged actors whose owner will drain them after we return
         (the established http/transport teardown pattern: set ACTOR_FLAG_DESTROY,
         wait_for_idle, then message_queue_destroy + free), so it is correct to
         return here — no dispatchable work is stranded. */
      platform_mutex_unlock(pool->idle_lock);
      bool reinjected = _scheduler_pool_reinject_stranded(pool);
      platform_mutex_lock(pool->idle_lock);
      if (!reinjected) {
        break;
      }
      if (++recovery_attempts > RECOVERY_LIMIT) {
        platform_mutex_unlock(pool->idle_lock);
        log_error("scheduler_pool_wait_for_idle: stuck with %zu pending messages "
                  "and all %zu workers idle after %u recovery attempts "
                  "(counter/registry desync or unrecoverable strand)",
                  atomic_load(&pool->pending_messages), pool->worker_count,
                  recovery_attempts);
        /* Drain deferred derefs before returning so the caller's subsequent
           pool_stop / destroy does not inherit a pending-deref backlog. */
        scheduler_pool_drain_pending_derefs(pool);
        return -1;
      }
      /* A worker woken by the re-inject may have already drained its mailbox,
         gone idle, and signalled this condvar while we ran the scan WITHOUT
         holding idle_lock. Re-check the idle+drained predicate so we don't
         condvar_wait for a signal that already fired (which would hang, since
         no further signal arrives while every worker stays idle). From this
         check through condvar_wait we hold idle_lock, and condvar_wait
         releases it atomically as it blocks — so any later idle-signal cannot
         fire until we are parked, closing the missed-signal window. */
      if (atomic_load(&pool->idle_count) == pool->worker_count &&
          atomic_load(&pool->pending_messages) == 0) {
        break;
      }
      /* Fall through to condvar_wait to await worker progress. */
    }
    platform_condvar_wait(pool->idle, pool->idle_lock);
  }
  platform_mutex_unlock(pool->idle_lock);

  /* Drain pending deferred derefs — no actors running, safe to destroy */
  scheduler_pool_drain_pending_derefs(pool);
  return 0;
}

scheduler_t* scheduler_get_current(void) {
  return current_scheduler;
}
