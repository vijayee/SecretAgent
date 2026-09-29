//
// Created by victor on 5/6/25.
//

#ifndef SA_SCHEDULER_H
#define SA_SCHEDULER_H

#include "deque.h"
#include "../Actor/actor.h"
#include "../Platform/platform.h"
#include "../Util/atomic_compat.h"
#include <stddef.h>
#include <stdint.h>

#define CACHE_LINE_SIZE 64

typedef struct pending_deref_node_t {
  struct pending_deref_node_t* next;
  void* object;
  void (*destructor)(void*);
} pending_deref_node_t;

typedef struct inject_node_t {
  actor_t* actor;
  struct inject_node_t* next;
} inject_node_t;

typedef struct inject_queue_t {
  platform_mutex_t* lock;
  platform_condvar_t* condition;
  inject_node_t* head;
  inject_node_t* tail;
} inject_queue_t;

typedef struct scheduler_t {
  size_t index;
  platform_thread_t* thread;
  deque_t local_queue;
  ATOMIC(uint32_t) last_victim;
  char _pad[CACHE_LINE_SIZE - sizeof(size_t) - sizeof(platform_thread_t*) - sizeof(deque_t) - sizeof(ATOMIC(uint32_t))];
  actor_t* current;
} scheduler_t;

typedef struct scheduler_pool_t {
  scheduler_t* workers;
  size_t worker_count;
  inject_queue_t inject;
  platform_barrier_t* barrier;
  platform_mutex_t* idle_lock;
  platform_condvar_t* idle;
  ATOMIC(size_t) idle_count;
  ATOMIC(uint32_t) active_count;
  ATOMIC(uint8_t) terminate;
  /* Set to 1 in scheduler_pool_stop AFTER the worker threads are joined.
     Distinct from terminate (which is set BEFORE the join): terminate tells
     workers to exit their loops, stopped tells actor_destroy that no worker
     is alive to transition an actor out of a deque. actor_destroy's wait for
     queue_state == IDLE breaks out when stopped is set, because the actor
     may be QUEUED (an external thread like the timer loop can inject work
     after stop) but no worker will dereference it — deque_destroy and
     _inject_queue_destroy free their nodes without touching the actor
     pointers. */
  ATOMIC(uint8_t) stopped;
  platform_mutex_t* deref_lock;
  pending_deref_node_t* pending_derefs;
  /* Total undelivered messages across every actor on this pool. Incremented by
     message_queue_push (via each actor's queue.pending_counter back-pointer)
     and decremented by message_queue_pop, so it covers both dispatch
     (actor_run) and drain (message_queue_destroy). scheduler_pool_wait_for_idle
     waits on this in addition to all workers being idle, so it can never return
     with a message still stranded in a mailbox. */
  ATOMIC(size_t) pending_messages;
  /* Registry of every actor owned by this pool (intrusive doubly-linked list
     via actor_t.registry_prev/registry_next), used by the
     scheduler_pool_wait_for_idle recovery scan to find and re-inject stranded
     actors. Protected by registry_lock. */
  platform_mutex_t* registry_lock;
  actor_t* registry_head;
} scheduler_pool_t;

scheduler_pool_t* scheduler_pool_create(size_t worker_count);
void scheduler_pool_destroy(scheduler_pool_t* pool);
void scheduler_pool_start(scheduler_pool_t* pool);
void scheduler_pool_stop(scheduler_pool_t* pool);
int scheduler_pool_wait_for_idle(scheduler_pool_t* pool);
void scheduler_pool_drain_pending_derefs(scheduler_pool_t* pool);

void scheduler_inject(scheduler_pool_t* pool, actor_t* actor);
void scheduler_pool_defer_cleanup(scheduler_pool_t* pool, void* object, void (*destructor)(void*));

scheduler_t* scheduler_get_current(void);

#endif // SA_SCHEDULER_H
