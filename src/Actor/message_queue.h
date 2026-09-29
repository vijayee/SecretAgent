//
// Created by victor on 5/6/25.
//

#ifndef SA_MESSAGE_QUEUE_H
#define SA_MESSAGE_QUEUE_H

#include "message.h"
#include "../Util/atomic_compat.h"
#include <stdbool.h>
#include <stddef.h>

#define MAILBOX_MUTE_THRESHOLD 256

typedef struct message_node_t message_node_t;
struct message_node_t {
  message_t msg;
  ATOMIC(message_node_t*) next;
};

typedef struct message_queue_t {
  ATOMIC(message_node_t*) head;
  message_node_t* tail;
  ATOMIC(size_t) size;
  /* Optional back-pointer to the owning pool's pending-messages counter.
     When non-NULL, push increments and pop decrements it, so the pool always
     knows how many messages are queued across all of its actors. This is what
     scheduler_pool_wait_for_idle waits on (in addition to all workers being
     idle) so it never returns with an undelivered message still stranded in a
     mailbox. NULL for queues not owned by a scheduler pool. */
  ATOMIC(size_t)* pending_counter;
  /* Send-side spinlock + destroyed flag: serialize actor_send vs
     message_queue_destroy so the DESTROY-check-then-push TOCTOU in
     actor_send cannot fire. The single worker that pops/markempty's is
     not affected (it runs after destroy has waited for the worker via
     actor_destroy's RUNNING spin). See docs/concurrency-pass.md F4.

     `send_lock` is an embedded spinlock (0 = unlocked, 1 = locked) rather
     than a heap-allocated platform_mutex_t: a late sender (timer pd-loop
     thread, MsQuic callback) may arrive AFTER message_queue_destroy has
     returned and the containing actor struct may already have been freed
     by the caller. With a heap mutex we would have to either (a) keep the
     mutex alive forever (a per-actor leak that breaks valgrind) or (b)
     destroy it in message_queue_destroy, which makes any late send that
     races past the destroyed check dereference a freed/NULL mutex. The
     spinlock is embedded in the queue struct, so its lifetime is the
     struct's lifetime — no separate allocation, no destruction, no leak,
     and a late send that takes it observes `destroyed` and frees its
     message without UB. It uses ATOMIC(uint8_t) (not _Atomic bool) for
     cross-platform C/C++ compatibility via the atomic_compat.h macros.

     `destroyed` is a plain bool: it is only read/written while holding
     send_lock, so the spinlock provides all the synchronization needed. */
  ATOMIC(uint8_t) send_lock;
  bool destroyed;
} message_queue_t;

void message_queue_init(message_queue_t* queue);
/* Returns true on success (message queued; *was_empty, if non-NULL, receives
   whether the queue was empty before this push), false if the queue was
   destroyed (the message node AND its payload are freed by push in that
   case — the caller must NOT free them). */
bool message_queue_push(message_queue_t* queue, message_node_t* first, message_node_t* last, bool* was_empty);
message_node_t* message_queue_pop(message_queue_t* queue);
bool message_queue_markempty(message_queue_t* queue);
bool message_queue_isempty(message_queue_t* queue);
void message_queue_destroy(message_queue_t* queue);

#endif // SA_MESSAGE_QUEUE_H
