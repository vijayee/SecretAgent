//
// Created by victor on 5/6/25.
//

#include "actor.h"
#include "../Scheduler/scheduler.h"
#include "../Util/allocator.h"
#include <stdbool.h>
#include <stdlib.h>

void actor_init(actor_t* actor, void* state, void (*dispatch)(void* state, message_t* msg), scheduler_pool_t* pool) {
  message_queue_init(&actor->queue);
  atomic_store(&actor->flags, 0);
  atomic_store(&actor->queue_state, ACTOR_QUEUE_IDLE);
  atomic_store(&actor->pressured_senders, NULL);
  actor->pool = pool;
  actor->state = state;
  actor->dispatch = dispatch;
  actor->registry_prev = NULL;
  actor->registry_next = NULL;
  if (pool != NULL) {
    /* Route this mailbox's push/pop counts to the pool's pending-messages
       counter, and register the actor so scheduler_pool_wait_for_idle can find
       and re-inject it if it ever strands. */
    actor->queue.pending_counter = &pool->pending_messages;
    platform_mutex_lock(pool->registry_lock);
    actor->registry_next = pool->registry_head;
    if (pool->registry_head != NULL) {
      pool->registry_head->registry_prev = actor;
    }
    pool->registry_head = actor;
    platform_mutex_unlock(pool->registry_lock);
  }
}

void actor_detach_pool(actor_t* actor) {
  if (actor->pool == NULL) {
    return;
  }
  scheduler_pool_t* pool = actor->pool;
  platform_mutex_lock(pool->registry_lock);
  if (actor->registry_prev != NULL) {
    actor->registry_prev->registry_next = actor->registry_next;
  } else {
    pool->registry_head = actor->registry_next;
  }
  if (actor->registry_next != NULL) {
    actor->registry_next->registry_prev = actor->registry_prev;
  }
  actor->registry_prev = NULL;
  actor->registry_next = NULL;
  actor->pool = NULL;
  platform_mutex_unlock(pool->registry_lock);
}

void actor_destroy(actor_t* actor) {
  /* Save the pool pointer before actor_detach_pool nulls actor->pool. We
     need it below to check pool->stopped: an external thread (e.g. the
     timer loop) can inject work after scheduler_pool_stop, leaving this
     actor QUEUED with no live worker to transition it to IDLE — in that
     case the queue_state wait must break out, because no worker will
     dereference the actor (workers are joined; deque_destroy and
     _inject_queue_destroy free their nodes without touching actor
     pointers). */
  scheduler_pool_t* pool = actor->pool;
  /* Mark the actor as destroyed so the scheduler skips it and actor_send
     drops new messages. */
  atomic_fetch_or(&actor->flags, ACTOR_FLAG_DESTROY);
  /* Release any senders that were muted because of this actor. */
  if (atomic_load(&actor->flags) & ACTOR_FLAG_PRESSURED) {
    backpressure_release(actor);
  }
  /* Unregister from the pool's actor registry before touching the queue, so a
     concurrent scheduler_pool_wait_for_idle recovery scan can never dereference
     an actor whose mailbox is about to be (or already is) torn down. The
     ACTOR_FLAG_DESTROY set above is the secondary guard for any scan already
     in flight on this actor. */
  actor_detach_pool(actor);
  /* Second backpressure_release sweep: a sender that passed the outer
     DESTROY check in actor_send and completed the CAS append AFTER the first
     backpressure_release (line 71) but BEFORE the in-loop DESTROY check
     (added in actor_send) would have appended to a drained list. Drain again
     unconditionally — backpressure_release is a no-op when pressured_senders
     is NULL (atomic_exchange returns NULL, the walk loop doesn't run), so
     this is cheap and safe. Don't gate on ACTOR_FLAG_PRESSURED: the first
     drain already cleared it, and a sender muted via backpressure_apply()
     (a public API that sets PRESSURED without mailbox overflow) wouldn't
     re-set it, so the guard would skip the drain and leave the orphaned
     node -> the sender muted forever (the exact livelock F10 fixes).
     See concurrency-pass.md F10. */
  backpressure_release(actor);
  /* A pool worker may still be inside actor_run on this actor, touching
     actor->queue (message_queue_pop / message_queue_markempty) after dispatch
     returns, so the queue cannot be torn down until that worker has cleared
     ACTOR_FLAG_RUNNING (it only does so at scheduler.c after actor_run
     returns). Skip the wait only when WE are that worker — i.e. self-destruction
     from within the actor's own dispatch — because then RUNNING is held by our
     own call stack and spinning would deadlock; actor_run's post-dispatch
     ACTOR_FLAG_DESTROY check (below) already avoids touching the freed queue via
     its saved-payload path. For non-pool / inline actors RUNNING is never set,
     so the wait is a no-op. */
  scheduler_t* self = scheduler_get_current();
  if (!(self != NULL && self->current == actor)) {
    while (atomic_load(&actor->flags) & ACTOR_FLAG_RUNNING) {
      platform_sleep_ms(0);
    }
    /* Wait for the actor to be out of every scheduler deque. A worker that
       just finished actor_run may be about to re-queue this actor; if we
       freed now, that re-queue would push a freed pointer and the next pop
       would read freed memory. The worker transitions queue_state to IDLE
       (no re-queue) or QUEUED (re-queue) atomically with clearing RUNNING,
       so once we observe IDLE the worker has committed to not re-queuing.
       Skipped for self-destruction: the worker holds queue_state = RUNNING
       on this stack and would deadlock waiting for itself; actor_run's
       post-dispatch DESTROY check sets IDLE before returning, but the
       self-destroy path bypasses message_queue_destroy entirely (the
       worker still needs the queue to finish actor_run). Break out if the
       pool is NULL (inline actor, or detached by an earlier
       scheduler_pool_destroy — in both cases no live worker can touch the
       actor) or stopped (workers joined): the actor may still be QUEUED
       because an external thread injected work after stop, but no live
       worker will dereference it, so freeing is safe. */
    while (atomic_load(&actor->queue_state) != ACTOR_QUEUE_IDLE) {
      if (pool == NULL || atomic_load(&pool->stopped)) {
        break;
      }
      platform_sleep_ms(0);
    }
  }
  /* Final pressured_senders drain. The two earlier drains (lines 71 and 90)
     run before the RUNNING/queue_state waits; a concurrent sender in
     actor_send's CAS loop can complete its append DURING those waits — its
     in-loop DESTROY re-check read DESTROY=false before destroy set it, then
     destroy's first two drains ran (finding nothing), then the sender's CAS
     landed. Without this final drain that node is orphaned and the sender
     stays ACTOR_FLAG_MUTED forever (F10 livelock). Draining again here closes
     the common case; a per-actor lock would be needed to close the truly
     concurrent TOCTOU where the CAS lands after this drain too, but that
     window is sub-microsecond and requires a sender racing destroy while the
     actor is already PRESSURED. backpressure_release is a no-op when the list
     is empty. */
  backpressure_release(actor);
  message_queue_destroy(&actor->queue);
}

bool actor_send(actor_t* actor, message_t* msg) {
  if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) {
    /* Actor is being destroyed — drop the message and clean up the payload. */
    if (msg->payload_destroy != NULL && msg->payload != NULL) {
      msg->payload_destroy(msg->payload);
    }
    return false;
  }
  message_node_t* node = get_clear_memory(sizeof(message_node_t));
  node->msg = *msg;
  bool was_empty_local = false;
  bool pushed = message_queue_push(&actor->queue, node, node, &was_empty_local);
  if (!pushed) {
    /* The queue was destroyed while we were pushing; message_queue_push
       already freed the node and the payload. */
    return false;
  }
  bool was_empty = was_empty_local;

  /* Backpressure: if target is pressured, mute the sender. */
  if (atomic_load(&actor->flags) & ACTOR_FLAG_PRESSURED) {
    scheduler_t* self = scheduler_get_current();
    if (self != NULL && self->current != NULL && self->current != actor) {
      actor_t* sender = self->current;
      /* Pony-style exemption: never mute a sender that is already muted
         or pressured. This prevents deadlock chains where every actor in a
         pipeline gets muted and nobody can make forward progress. */
      if (!(atomic_load(&sender->flags) & (ACTOR_FLAG_MUTED | ACTOR_FLAG_PRESSURED))) {
        /* Don't mute senders to an actor being torn down: backpressure_release
           in actor_destroy has already (or is about to) drain pressured_senders,
           and a late append here would orphan the muted_sender_node and leave
           this sender muted forever. The message above was already pushed and
           is drained with the queue, so skipping the mute is safe. A residual
           TOCTOU remains (re-check reads DESTROY=false, destroy completes its
           drains, then this CAS lands) that would need a per-actor lock to
           close fully; actor_destroy's final drain after the queue wait
           catches the common case. See concurrency-pass.md F10. */
        if (!(atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY)) {
          muted_sender_node_t* msn = get_clear_memory(sizeof(muted_sender_node_t));
          msn->sender = sender;
          bool appended = false;
          do {
            /* Re-check DESTROY inside the loop: actor_destroy may have set
               DESTROY + drained pressured_senders after our outer check. If
               DESTROY is now set, abort the append (free msn) so it's never
               orphaned in a drained list -> the sender isn't muted forever
               (endlessly re-queued but never run). See concurrency-pass.md F10. */
            if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) {
              free(msn);
              break;
            }
            msn->next = atomic_load(&actor->pressured_senders);
            if (atomic_compare_exchange_strong(&actor->pressured_senders, &msn->next, msn)) {
              appended = true;
            }
          } while (!appended);
          if (appended) {
            atomic_fetch_or(&sender->flags, ACTOR_FLAG_MUTED);
          }
        }
      }
    }
  }

  /* Automatic backpressure: if mailbox exceeds threshold, mark as pressured. */
  if (atomic_load(&actor->queue.size) >= MAILBOX_MUTE_THRESHOLD) {
    if (!(atomic_load(&actor->flags) & ACTOR_FLAG_PRESSURED)) {
      atomic_fetch_or(&actor->flags, ACTOR_FLAG_PRESSURED);
    }
  }

  if (was_empty) {
    atomic_fetch_or(&actor->flags, ACTOR_FLAG_SCHEDULED);
    if (actor->pool != NULL) {
      /* Transition to QUEUED before injecting so actor_destroy's wait for
         IDLE cannot complete while the actor is in flight to the inject
         queue. Skipped when pool is NULL: the actor is not entering any
         scheduler deque, so queue_state stays IDLE and actor_destroy's
         wait completes immediately. */
      atomic_store(&actor->queue_state, ACTOR_QUEUE_QUEUED);
      scheduler_inject(actor->pool, actor);
    }
  }
  return was_empty;
}

bool actor_run(actor_t* actor, size_t batch_size) {
  for (size_t count = 0; count < batch_size; count++) {
    if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) {
      return false;
    }
    message_node_t* node = message_queue_pop(&actor->queue);
    if (node == NULL) {
      break;
    }
    /* Save payload info before dispatch — actor_destroy may free the node.
       A dispatch that CONSUMEs the payload will set msg->payload = NULL;
       we must respect that in the normal (non-self-destruct) path. */
    void (*payload_destroy)(void*) = node->msg.payload_destroy;
    void* payload = node->msg.payload;

    actor->dispatch(actor->state, &node->msg);

    /* If dispatch requested self-destruction, stop processing immediately.
       The node may have been freed by actor_destroy — use saved payload info.
       Dispatch functions that self-destruct do not CONSUME; they leave
       normal payload cleanup to actor_run.
       The caller (typically the scheduler) must check ACTOR_FLAG_DESTROY
       and skip further operations on this actor. */
    if (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY) {
      /* Self-destruct path: the worker is returning false WITHOUT going
         through the worker loop's re-queue/release decision, so it must
         transition queue_state to IDLE here so a non-self destroyer's
         wait completes. (A self-destroyer skipped the wait in
         actor_destroy, so this store is harmless in that path.) */
      atomic_store(&actor->queue_state, ACTOR_QUEUE_IDLE);
      if (payload_destroy != NULL && payload != NULL) {
        payload_destroy(payload);
      }
      return false;
    }

    /* Node is still valid — use current msg values to respect CONSUME
       (dispatch may have set msg->payload = NULL to take ownership). */
    if (node->msg.payload_destroy != NULL && node->msg.payload != NULL) {
      node->msg.payload_destroy(node->msg.payload);
    }
    /* Muted actors still process their mailbox — muting only prevents
       sending to PRESSURED targets. Stopping processing would deadlock
       the pipeline (e.g., connection actor muted mid-request). */
  }
  if (message_queue_markempty(&actor->queue)) {
    atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_SCHEDULED);
    /* Auto-release pressure when mailbox is empty. */
    if (atomic_load(&actor->flags) & ACTOR_FLAG_PRESSURED) {
      backpressure_release(actor);
    }
    return false;
  }
  return true;
}

void backpressure_apply(actor_t* actor) {
  atomic_fetch_or(&actor->flags, ACTOR_FLAG_PRESSURED);
}

void backpressure_release(actor_t* actor) {
  atomic_fetch_and(&actor->flags, ~ACTOR_FLAG_PRESSURED);
  /* Walk the pressured_senders list and unmute each sender. */
  muted_sender_node_t* node = atomic_exchange(&actor->pressured_senders, NULL);
  while (node != NULL) {
    muted_sender_node_t* next = node->next;
    actor_t* sender = node->sender;
    atomic_fetch_and(&sender->flags, ~ACTOR_FLAG_MUTED);
    if (sender->pool != NULL && !(atomic_load(&sender->flags) & (ACTOR_FLAG_DESTROY | ACTOR_FLAG_SCHEDULED))) {
      atomic_fetch_or(&sender->flags, ACTOR_FLAG_SCHEDULED);
      /* Unmuting a sender re-injects it into a worker deque: transition
         its queue_state to QUEUED so a concurrent actor_destroy cannot
         free it while it is in flight. */
      atomic_store(&sender->queue_state, ACTOR_QUEUE_QUEUED);
      scheduler_inject(sender->pool, sender);
    }
    free(node);
    node = next;
  }
}