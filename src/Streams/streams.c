//
// Created by victor on 10/12/25.
//
#include "stream.h"
#include "../Buffer/buffer.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Util/error.h"
#include "../RefCounter/refcounter.h"
#include <string.h>

void _readable_push_stream_piped_notify(stream_t* stream, void* payload);
void _readable_push_stream_error_notify(stream_t* stream, void* payload);
void _readable_push_stream_close_notify(stream_t* stream, void* payload);
void _writeable_push_stream_data_notify(stream_t* stream, void* data);
void _writeable_push_stream_error_notify(stream_t* stream, void* payload);
void _writeable_push_stream_close_notify(stream_t* stream, void* payload);
void _writeable_push_stream_complete_notify(stream_t* stream, void* payload);
void _writeable_push_stream_on_piped(stream_t* ws, stream_t* rs);
void _stream_purge_handlers(stream_t* stream);
void _readable_push_stream_on_pipe(stream_t* rs, stream_t* ws);
void _writeable_pull_stream_error_notify(stream_t* stream, void* payload);
void _readable_pull_stream_error_notify(stream_t* stream, void* payload);
void _readable_pull_stream_close_notify(stream_t* stream, void* payload);
void _readable_pull_stream_finish_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_close_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_complete_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_piped_notify(stream_t* stream, void* payload);
void _writeable_pull_stream_data_notify(stream_t* stream, void* data);
void _readable_pull_stream_on_piped(stream_t* rs, stream_t* ws);
void _writeable_pull_stream_on_pipe(stream_t* ws, stream_t* rs);

/* Static helpers defined later in this file — forward-declared so the public
   stream_notify / stream_dispatch can route through them. */
static uint8_t _stream_notify_dispatch(stream_t* stream, stream_event_e event, void* payload, void (*payload_destroy)(void*));
static void _stream_notify_payload_destroy(void* ptr);
static void _stream_subscribe_payload_destroy(void* ptr);

/* ---- event handler ---- */

stream_event_handler_t* stream_event_handler_create(size_t id, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*), uint8_t once) {
  stream_event_handler_t* _handler = get_clear_memory(sizeof(stream_event_handler_t));
  refcounter_init((refcounter_t*) _handler);
  _handler->handler = handler;
  _handler->ctx = ctx;
  _handler->ctx_destroy = ctx_destroy;
  _handler->once = once;
  _handler->id = id;
  return _handler;
}

void stream_event_handler_destroy(stream_event_handler_t* handler) {
  if (refcounter_dereference_is_zero((refcounter_t*) handler)) {
    refcounter_destroy_lock((refcounter_t*) handler);
    if (handler->ctx_destroy != NULL) {
      handler->ctx_destroy(handler->ctx);
    }
    free(handler);
  }
}

/* ---- event handler list ---- */

stream_event_handler_list_t* stream_event_list_create() {
  stream_event_handler_list_t* list = get_clear_memory(sizeof(stream_event_handler_list_t));
  list->first = NULL;
  list->last = NULL;
  return list;
}

void stream_event_list_destroy(stream_event_handler_list_t* list) {
  if (list == NULL) return;
  stream_event_handler_list_node_t* current = list->first;
  stream_event_handler_list_node_t* next = NULL;
  while (current != NULL) {
    next = current->next;
    stream_event_handler_destroy(current->handler);
    free(current);
    current = next;
  }
  free(list);
}

void stream_event_list_enqueue(stream_event_handler_list_t* list, stream_event_handler_t* event) {
  stream_event_handler_list_node_t* node = get_clear_memory(sizeof(stream_event_handler_list_node_t));
  node->handler = event;
  node->previous = NULL;
  node->next = NULL;
  if ((list->last == NULL) && (list->first == NULL)) {
    list->first = node;
    list->last = node;
  } else {
    node->previous = list->last;
    list->last->next = node;
    list->last = node;
  }
  list->count++;
}

stream_event_handler_t* stream_event_list_dequeue(stream_event_handler_list_t* list) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return NULL;
  } else {
    stream_event_handler_list_node_t* node = list->first;
    list->first = node->next;
    if (node->next != NULL) {
      list->first->previous = NULL;
    }
    if (list->last == node) {
      list->last = NULL;
    }
    stream_event_handler_t* event = node->handler;
    free(node);
    list->count--;
    return event;
  }
}

void stream_event_list_remove(stream_event_handler_list_t* list, stream_event_handler_list_node_t* node) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return;
  }
  if (list->last == node) {
    list->last = node->previous;
  }
  if (list->first == node) {
    list->first = node->next;
  }
  if (node->previous != NULL) {
    node->previous->next = node->next;
  }
  if (node->next != NULL) {
    node->next->previous = node->previous;
  }
  stream_event_handler_t* handler = node->handler;
  stream_event_handler_destroy(handler);
  list->count--;
  free(node);
}

void stream_event_list_remove_onces(stream_event_handler_list_t* list) {
  if ((list->last == NULL) && (list->first == NULL)) {
    return;
  }
  stream_event_handler_list_node_t* current = list->first;
  stream_event_handler_list_node_t* next = NULL;
  while (current != NULL) {
    next = current->next;
    if (current->handler->once == 1) {
      if (list->last == current) {
        list->last = current->previous;
      }
      if (list->first == current) {
        list->first = current->next;
      }
      if (current->previous != NULL) {
        current->previous->next = current->next;
      }
      if (current->next != NULL) {
        current->next->previous = current->previous;
      }

      stream_event_handler_t* handler = current->handler;
      stream_event_handler_destroy(handler);
      list->count--;
      free(current);
    }
    current = next;
  }
}

/* ---- stream lifecycle ---- */

void stream_init(stream_t* stream, stream_force_e force, stream_type_e type, uint8_t auto_push, scheduler_pool_t* pool, void (*destructor)(stream_t*)) {
  refcounter_init((refcounter_t*) stream);
  stream->force = force;
  stream->type = type;
  stream->pool = pool;
  stream->pipe_notifiers = NULL;
  stream->auto_push = auto_push;
  if (force == push) {
    stream->on_pipe = _readable_push_stream_on_pipe;
    stream->on_piped = _writeable_push_stream_on_piped;
  } else {
    stream->on_pipe = _writeable_pull_stream_on_pipe;
    stream->on_piped = _readable_pull_stream_on_piped;
  }
  stream->destructor = destructor;
  for (size_t i = 0; i < STREAM_HANDLER_COUNT; i++) {
    stream->handlers[i] = stream_event_list_create();
  }
  actor_init(&stream->actor, stream, stream_dispatch, pool);
}

void stream_deinit(stream_t* stream) {
  if (refcounter_count((refcounter_t*) stream) == 0) {
    /* Stop the actor and free its mailbox BEFORE touching the handler lists.
       The handler lists are the actor's state — subscribe / unsubscribe /
       once / notify are all routed via actor messages, so the lists are only
       mutated on the actor thread. We must quiesce the actor (no dispatch in
       flight) before tearing the lists down, or a message dispatched
       concurrently with the purge would touch a freed list. actor_destroy sets
       ACTOR_FLAG_DESTROY (actor_run stops dispatching further messages after
       the current one), waits for any in-flight dispatch to finish, and frees
       the mailbox — discarding any still-pending subscribe/unsubscribe/notify
       messages via their payload_destroy. On the self-destruct path (deinit
       called from a DEFERRED_DEREF dispatch on this actor's own thread) the
       RUNNING wait is skipped and the mailbox is freed under us; actor_run's
       post-dispatch DESTROY check keeps it from touching the freed queue. */
    actor_destroy(&stream->actor);
    _stream_purge_handlers(stream);
    if (stream->pipe_notifiers != NULL) {
      free(stream->pipe_notifiers);
      stream->pipe_notifiers = NULL;
    }
    if (stream->pullable_stream != NULL) {
      if (refcounter_dereference_is_zero((refcounter_t*) stream->pullable_stream)) {
        stream_destroy(stream->pullable_stream);
      }
      stream->pullable_stream = NULL;
    }
  }
}

void stream_destroy(stream_t* stream) {
  stream->destructor(stream);
}

void _stream_purge_handlers(stream_t* stream) {
  for (size_t i = 0; i < STREAM_HANDLER_COUNT; i++) {
    stream_event_handler_list_t* list = stream->handlers[i];
    stream->handlers[i] = NULL;
    stream_event_list_destroy(list);
  }
}

/* Synchronously deliver a caller-owned error_event to the stream's handlers.
   Unlike public stream_notify (which transfers ownership of the payload to a
   message wrapper), this is a BORROW: the caller owns the error, and
   _stream_notify_dispatch only takes a transient reference for handler safety.
   It returns 1 when it consumed the payload itself (no-handler path: it
   destroyed the error), in which case the caller must NOT free it; otherwise
   it returns 0 (has-handler path: it released only its transient ref) and the
   caller still owns the reference and must destroy it. Used for the
   fire-and-forget "no <handler> defined" errors raised directly on the actor
   thread — there is no async wrapper to own them, so the caller owns and frees
   when dispatch did not. */
static void _stream_notify_owned_error(stream_t* stream, async_error_t* error) {
  uint8_t consumed = _stream_notify_dispatch(stream, error_event, error, (void (*)(void*))error_destroy);
  if (!consumed) {
    error_destroy(error);
  }
}

/* Pipe/piped are routed through the stream actor so pipe_notifiers is only
   ever touched on the actor thread. The public pipe entry points send a
   STREAM_PIPE message; the on_pipe handler then sends STREAM_PIPED to the
   peer's actor, so each stream's pipe_notifiers is written and freed on its
   own actor thread (no cross-thread use-after-free). */
void stream_pipe_internal(stream_t* source, stream_t* dest) {
  source->on_pipe(source, dest);
}

void stream_piped_internal(stream_t* stream, stream_t* source) {
  stream->on_piped(stream, source);
}

static void _stream_pipe_payload_destroy(void* ptr) {
  stream_pipe_payload_t* p = (stream_pipe_payload_t*) ptr;
  if (p->source != NULL) {
    DEREFERENCE(p->source);
  }
  if (p->dest != NULL) {
    DEREFERENCE(p->dest);
  }
  free(p);
}

static void _stream_piped_payload_destroy(void* ptr) {
  stream_piped_payload_t* p = (stream_piped_payload_t*) ptr;
  if (p->source != NULL) {
    DEREFERENCE(p->source);
  }
  free(p);
}

void stream_dispatch(void* state, message_t* msg) {
  stream_t* stream = (stream_t*) state;
  switch (msg->type) {
    case READABLE_PUSH:
      if (stream->on_push != NULL) {
        stream->on_push(stream);
      } else {
        _stream_notify_owned_error(stream, OFFS_ERROR("No Push Handler Defined"));
      }
      break;
    case READABLE_READ: {
      stream_read_message_payload_t* p = (stream_read_message_payload_t*) msg->payload;
      if (stream->on_read != NULL) {
        stream->on_read(stream, p->size, p->ctx, p->cb);
      } else {
        _stream_notify_owned_error(stream, OFFS_ERROR("No Read Handler Defined"));
      }
      break;
    }
    case WRITEABLE_WRITE: {
      stream_write_payload_t* p = (stream_write_payload_t*) msg->payload;
      if (stream->on_write != NULL) {
        stream->on_write(stream, p->data);
      } else {
        _stream_notify_owned_error(stream, OFFS_ERROR("No Write Handler Defined"));
      }
      if (stream->is_pulling && stream->pullable_stream != NULL) {
        readable_pull_stream_pull(stream->pullable_stream);
      }
      break;
    }
    case CLOSE_STREAM:
      if (stream->on_close != NULL) {
        stream->on_close(stream);
      } else {
        _stream_notify_owned_error(stream, OFFS_ERROR("No Close Handler Defined"));
      }
      break;
    case READABLE_PULL:
      if (stream->on_pull != NULL) {
        stream->on_pull(stream);
      } else {
        _stream_notify_owned_error(stream, OFFS_ERROR("No Readable Pull Handler Defined"));
      }
      break;
    case DEFERRED_DEREF:
      stream->destructor(stream);
      break;
    case STREAM_SUBSCRIBE: {
      stream_subscribe_payload_t* p = (stream_subscribe_payload_t*) msg->payload;
      /* Runs on the actor thread: stream_subscribe_internal creates the
         handler (which takes ownership of ctx) and enqueues it into the
         list. Clear the payload's ctx/ctx_destroy so the message's
         _stream_subscribe_payload_destroy does not also destroy ctx — the
         handler now owns it and will release it on unsubscribe / stream
         deinit. Mirrors the STREAM_NOTIFY consumed path. */
      stream_subscribe_internal(stream, p->event, p->id, p->ctx, p->handler, p->ctx_destroy, p->once);
      p->ctx = NULL;
      p->ctx_destroy = NULL;
      break;
    }
    case STREAM_UNSUBSCRIBE: {
      stream_unsubscribe_payload_t* p = (stream_unsubscribe_payload_t*) msg->payload;
      stream_unsubscribe_internal(stream, p->event, p->id);
      break;
    }
    case STREAM_NOTIFY: {
      stream_notify_payload_t* notify_payload = (stream_notify_payload_t*) msg->payload;
      stream_event_e event = notify_payload->event;
      void* payload = notify_payload->payload;
      void (*payload_destroy)(void*) = notify_payload->payload_destroy;
      /* _stream_notify_dispatch returns 1 (consumed) in the no-handler path,
       * where it destroys the inner payload itself. In that case clear the
       * wrapper's inner pointers so the message's _stream_notify_payload_destroy
       * does not destroy the inner payload a second time (double-destroy /
       * use-after-free on the refcounted error_t). When it returns 0
       * (has-handler path) it only held a transient reference and the wrapper
       * still owns the inner payload, so leave the pointers for
       * _stream_notify_payload_destroy to release as before. */
      uint8_t consumed = _stream_notify_dispatch(stream, event, payload, payload_destroy);
      if (consumed) {
        notify_payload->payload = NULL;
        notify_payload->payload_destroy = NULL;
      }
      break;
    }
    case STREAM_PIPE: {
      stream_pipe_payload_t* p = (stream_pipe_payload_t*) msg->payload;
      stream_pipe_internal(p->source, p->dest);
      break;
    }
    case STREAM_PIPED: {
      stream_piped_payload_t* p = (stream_piped_payload_t*) msg->payload;
      stream_piped_internal(stream, p->source);
      break;
    }
    case STREAM_SET_PULLING: {
      stream_set_pulling_payload_t* p = (stream_set_pulling_payload_t*) msg->payload;
      stream->is_pulling = p->is_pulling;
      if (stream->is_pulling && stream->pullable_stream != NULL) {
        readable_pull_stream_pull(stream->pullable_stream);
      }
      break;
    }
    default:
      break;
  }
}

/* ---- stream notify ---- */

/* Public entry point: route the notification through the stream's actor so the
   handler list is only ever touched on the actor thread (subscribe /
   unsubscribe / once / notify all go via messages). The payload is owned by the
   message from here on: _stream_notify_payload_destroy releases it if the
   message is discarded (stream destroyed before dispatch), and the actor
   dispatches it via _stream_notify_dispatch when the message is processed. */
void stream_notify(stream_t* stream, stream_event_e event, void* payload, void (*payload_destroy)(void*)) {
  stream_notify_payload_t* wrapper = get_clear_memory(sizeof(stream_notify_payload_t));
  wrapper->event = event;
  wrapper->payload = payload;
  wrapper->payload_destroy = payload_destroy;
  /* Ownership is the CALLER's decision, signalled by the refcounter's yield
     flag — stream_notify never presumes a transfer:
       - transfer: the caller yielded/consumed the payload before calling
         (CONSUME(var, T), YIELD, or OFFS_ERROR_TRANSFER for a freshly-made
         error). The reference arrives with yield=1; refcounter_reference
         ADOPTS that yield (no count change), so the wrapper owns exactly the
         one ref the caller released (count stays 1).
       - share:     the caller passes the payload un-yielded (yield=0) and
         RETAINS its own reference. refcounter_reference ADDS a new reference
         (count++), so the wrapper owns a fresh ref and frees only that one;
         the caller's reference survives stream_notify and stays valid.
     Adopt-when-yielded, add-when-not is precisely refcounter_reference, so it
     is the only op needed here. _stream_notify_dispatch then takes a transient
     +1 for handler-safety and releases it after dispatch, and the wrapper's
     _stream_notify_payload_destroy releases the one ref the wrapper owns (the
     no-handler path releases it directly and clears the wrapper). NULL
     payloads (close/finished/complete event signals) carry no reference. */
  if (payload != NULL) {
    refcounter_reference((refcounter_t*) payload);
  }
  message_t msg;
  msg.type = STREAM_NOTIFY;
  msg.payload = wrapper;
  msg.payload_destroy = _stream_notify_payload_destroy;
  actor_send(&stream->actor, &msg);
}

/* Synchronous handler dispatch. Runs ONLY on the stream's actor thread —
   either from the STREAM_NOTIFY message (stream_dispatch) or from an on_*
   handler invoked by stream_dispatch — so the handler list is touched by
   exactly one thread and needs no lock. Returns 1 if the payload was consumed
   (the no-handler path destroys it so the caller must not double-free); returns
   0 if the caller still owns its reference (the has-handler path takes only a
   transient reference for dispatch safety and leaves the caller's reference
   intact). */
static uint8_t _stream_notify_dispatch(stream_t* stream, stream_event_e event, void* payload, void (*payload_destroy)(void*)) {
  stream_event_handler_list_t* list = stream->handlers[event];
  if (list->count == 0) {
    if (event == error_event) {
      async_error_t* error = (async_error_t*) payload;
      if (error != NULL && error->message != NULL) {
        log_error("Unhandled stream error: %s", error->message);
      } else {
        log_error("Unhandled stream error: (null)");
      }
    }
    if (payload_destroy != NULL) {
      payload_destroy(payload);
    }
    return 1;
  }
  /* Hold a transient reference to the payload so no handler can free it
     mid-dispatch, and reference each handler across its callback — a handler
     may unsubscribe itself or the stream may close during the callback, so the
     transient ref keeps the handler (and its list node) alive until we advance
     to the next node. The list is not mutated during dispatch (any
     subscribe/unsubscribe/close the handler issues is queued to the actor and
     runs after stream_notify returns), so walking current->next is safe. */
  if (payload != NULL) {
    refcounter_reference((refcounter_t*) payload);
  }
  uint8_t has_onces = 0;
  stream_event_handler_list_node_t* current = list->first;
  while (current != NULL) {
    if (has_onces == 0) {
      has_onces = current->handler->once;
    }
    stream_event_handler_t* handler = REFERENCE(current->handler, stream_event_handler_t);
    handler->handler(handler->ctx, payload);
    DESTROY(handler, stream_event_handler);
    current = current->next;
  }
  if (has_onces == 1) {
    stream_event_list_remove_onces(list);
  }
  /* Release our transient hold on the payload; the caller's (the message
   * wrapper's) reference is still intact, so it remains the owner. */
  if (payload_destroy != NULL) {
    payload_destroy(payload);
  }
  return 0;
}

/* payload_destroy for a STREAM_SUBSCRIBE message. If the message was
   dispatched, stream_subscribe_internal created the handler (which took
   ownership of ctx) and the dispatch cleared ctx/ctx_destroy, so this just
   frees the wrapper. If the message was discarded (stream destroyed before
   dispatch), the handler was never created and ctx is still owned by the
   wrapper — call ctx_destroy to release it, then free the wrapper. */
static void _stream_subscribe_payload_destroy(void* ptr) {
  stream_subscribe_payload_t* p = (stream_subscribe_payload_t*) ptr;
  if (p->ctx_destroy != NULL && p->ctx != NULL) {
    p->ctx_destroy(p->ctx);
  }
  free(p);
}

/* ---- stream operations ---- */

static void _stream_notify_payload_destroy(void* ptr) {
  stream_notify_payload_t* payload = (stream_notify_payload_t*)ptr;
  if (payload->payload_destroy != NULL && payload->payload != NULL) {
    payload->payload_destroy(payload->payload);
  }
  free(payload);
}

void stream_deactivate(stream_t* stream, async_error_t* error) {
  stream->is_deactivated = 1;
  stream_notify_payload_t* error_payload = get_clear_memory(sizeof(stream_notify_payload_t));
  error_payload->event = error_event;
  error_payload->payload = (void*) error;
  error_payload->payload_destroy = (void (*)(void*)) error_destroy;
  message_t error_msg;
  error_msg.type = STREAM_NOTIFY;
  error_msg.payload = error_payload;
  error_msg.payload_destroy = _stream_notify_payload_destroy;
  actor_send(&stream->actor, &error_msg);

  stream_notify_payload_t* close_payload = get_clear_memory(sizeof(stream_notify_payload_t));
  close_payload->event = close_event;
  close_payload->payload = NULL;
  close_payload->payload_destroy = NULL;
  message_t close_msg;
  close_msg.type = STREAM_NOTIFY;
  close_msg.payload = close_payload;
  close_msg.payload_destroy = (void (*)(void*)) free;
  actor_send(&stream->actor, &close_msg);
}

void stream_deferred_deref(stream_t* stream) {
  // Reference first to prevent premature free, then dereference so the
  // drain's refcounter_dereference_is_zero brings the net count down by one.
  scheduler_pool_defer_cleanup(stream->pool, stream, (void (*)(void*))stream->destructor);
  refcounter_dereference((refcounter_t*)stream);
}

void stream_unsubscribe_pipe_notifiers(stream_t* stream) {
  if (stream->pipe_notifiers != NULL) {
    size_t count = 0;
    switch (stream->type) {
      case readable_stream:
        count = 3;
        break;
      case writeable_stream:
        count = stream->force == push ? 4 : 5;
        break;
      case duplex_stream:
      case transform_stream:
        count = 8;
        break;
      default:
        log_error("stream_unsubscribe_pipe_notifiers: unknown stream type");
        count = 0;
        break;
    }
    for (size_t i = 0; i < count; i++) {
      stream_notifier_t* notifier = &stream->pipe_notifiers[i];
      if (notifier->stream != NULL) {
        stream_unsubscribe(notifier->stream, notifier->event, notifier->id);
        DEREFERENCE(notifier->stream);
      }
    }
    free(stream->pipe_notifiers);
    stream->pipe_notifiers = NULL;
  }
}

void writeable_stream_data_handler(stream_t* stream, void (*on_data)(stream_t*, void*)) {
  if (stream->type == readable_stream) {
    log_error("Only Writeable Stream can set data handlers");
    return;
  }
  stream->on_data = on_data;
}

void readable_stream_push_handler(stream_t* stream, void (*on_push)(stream_t*)) {
  if (stream->type == writeable_stream || stream->force == pull) {
    log_error("Only Readable Stream can set data handlers");
    return;
  }
  stream->on_push = on_push;
}

void readable_stream_pull_handler(stream_t* stream, void (*on_pull)(stream_t*)) {
  if (stream->type == writeable_stream || stream->force == push) {
    log_error("Only Readable Stream can set data handlers");
    return;
  }
  stream->on_pull = on_pull;
}

void writeable_stream_write_handler(stream_t* stream, void (*handler)(stream_t*, void*)) {
  if (stream->type == readable_stream) {
    stream_notify(stream, error_event, OFFS_ERROR_TRANSFER("Read Stream cannot set write handlers"), (void (*)(void*))error_destroy);
  } else {
    stream->on_write = handler;
  }
}

void stream_close_handler(stream_t* stream, void (*on_close)(stream_t*)) {
  stream->on_close = on_close;
}

void readable_push_stream_push(stream_t* stream) {
  message_t msg;
  msg.type = READABLE_PUSH;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&stream->actor, &msg);
}

void readable_stream_read(stream_t* stream, size_t size, void* ctx, void (*cb)(void*, void*)) {
  stream_read_message_payload_t* payload = get_clear_memory(sizeof(stream_read_message_payload_t));
  payload->stream = stream;
  payload->size = size;
  payload->ctx = ctx;
  payload->cb = cb;

  message_t msg;
  msg.type = READABLE_READ;
  msg.payload = payload;
  msg.payload_destroy = (void (*)(void*)) free;

  actor_send(&stream->actor, &msg);
}

void stream_close(stream_t* stream) {
  message_t msg;
  msg.type = CLOSE_STREAM;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  actor_send(&stream->actor, &msg);
}

void writeable_stream_write(stream_t* stream, void* data) {
  stream_write_payload_t* payload = get_clear_memory(sizeof(stream_write_payload_t));
  payload->stream = stream;
  payload->data = data;

  message_t msg;
  msg.type = WRITEABLE_WRITE;
  msg.payload = payload;
  msg.payload_destroy = (void (*)(void*)) free;

  actor_send(&stream->actor, &msg);
}

void readable_pull_stream_pull(stream_t* stream) {
  if (stream->type == writeable_stream || stream->force == push) {
    stream_notify(stream, error_event, OFFS_ERROR_TRANSFER("Invalid Readable Pull Stream"), (void (*)(void*))error_destroy);
    return;
  }

  message_t msg;
  msg.type = READABLE_PULL;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  actor_send(&stream->actor, &msg);
}

/* ---- subscribe / unsubscribe ---- */

/* Public entry points: allocate the handler id atomically (so it can be
   returned synchronously to the caller) and route the subscription through the
   stream's actor via a STREAM_SUBSCRIBE message. The actor creates the handler
   and enqueues it on the actor thread (stream_subscribe_internal), so the
   handler list is only mutated on the actor thread — no lock, no race with a
   concurrent stream_notify. FIFO mailbox ordering guarantees the handler is
   live before any later push/notify the caller issues, and that an unsubscribe
   queued after a subscribe runs after it (finds the handler). */

size_t stream_subscribe(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*)) {
  size_t id = ++stream->next_handler_id;
  stream_subscribe_payload_t* p = get_clear_memory(sizeof(stream_subscribe_payload_t));
  p->id = id;
  p->event = event;
  p->ctx = ctx;
  p->handler = handler;
  p->ctx_destroy = ctx_destroy;
  p->once = 0;
  message_t msg;
  msg.type = STREAM_SUBSCRIBE;
  msg.payload = p;
  msg.payload_destroy = _stream_subscribe_payload_destroy;
  actor_send(&stream->actor, &msg);
  return id;
}

size_t stream_once(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*)) {
  size_t id = ++stream->next_handler_id;
  stream_subscribe_payload_t* p = get_clear_memory(sizeof(stream_subscribe_payload_t));
  p->id = id;
  p->event = event;
  p->ctx = ctx;
  p->handler = handler;
  p->ctx_destroy = ctx_destroy;
  p->once = 1;
  message_t msg;
  msg.type = STREAM_SUBSCRIBE;
  msg.payload = p;
  msg.payload_destroy = _stream_subscribe_payload_destroy;
  actor_send(&stream->actor, &msg);
  return id;
}

void stream_unsubscribe(stream_t* stream, stream_event_e event, size_t id) {
  if (stream == NULL) return;
  stream_unsubscribe_payload_t* p = get_clear_memory(sizeof(stream_unsubscribe_payload_t));
  p->event = event;
  p->id = id;
  message_t msg;
  msg.type = STREAM_UNSUBSCRIBE;
  msg.payload = p;
  msg.payload_destroy = (void (*)(void*)) free;
  actor_send(&stream->actor, &msg);
}

/* Actor-thread implementation of subscribe. Creates the handler (which takes
   ownership of ctx) and enqueues it, then auto-pushes if this is the first data
   handler so the producer starts flowing data to the new consumer. The
   once-variant drives the push when a data handler already exists, preserving
   the prior subscribe/once auto-push conditions exactly (now evaluated on the
   actor thread after enqueue). */
void stream_subscribe_internal(stream_t* stream, stream_event_e event, size_t id, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*), uint8_t once) {
  stream_event_handler_t* _handler = stream_event_handler_create(id, ctx, handler, ctx_destroy, once);
  stream_event_handler_list_t* list = stream->handlers[event];
  size_t count_before = list->count;
  stream_event_list_enqueue(list, _handler);
  uint8_t push = 0;
  if ((event == data_event) && !stream->is_piped && stream->auto_push) {
    push = once ? (count_before == 1) : (count_before == 0);
  }
  if (push) {
    readable_push_stream_push(stream);
  }
}

/* Actor-thread implementation of unsubscribe. Walks the list for the handler
   with the matching id and removes it (stream_event_list_remove unlinks the
   node, drops the list's reference on the handler — releasing ctx via
   ctx_destroy if it was the last reference — and frees the node). */
void stream_unsubscribe_internal(stream_t* stream, stream_event_e event, size_t id) {
  stream_event_handler_list_t* list = stream->handlers[event];
  if (list == NULL) return;
  stream_event_handler_list_node_t* current = list->first;
  while (current != NULL) {
    stream_event_handler_list_node_t* next = current->next;
    if (current->handler->id == id) {
      stream_event_list_remove(list, current);
      return;
    }
    current = next;
  }
}

/* ---- push stream piping ---- */

void readable_push_stream_pipe(stream_t* rs, stream_t* ws) {
  if (rs->type == writeable_stream || rs->force == pull) {
    stream_notify(rs, error_event, OFFS_ERROR_TRANSFER("Invalid read stream being piped"), (void (*)(void*))error_destroy);
  } else if (ws->type == readable_stream || ws->force == pull) {
    stream_notify(rs, error_event, OFFS_ERROR_TRANSFER("Invalid write stream being piped to"), (void (*)(void*))error_destroy);
  } else {
    stream_pipe_payload_t* payload = get_clear_memory(sizeof(stream_pipe_payload_t));
    payload->source = REFERENCE(rs, stream_t);
    payload->dest = REFERENCE(ws, stream_t);
    message_t msg;
    msg.type = STREAM_PIPE;
    msg.payload = payload;
    msg.payload_destroy = _stream_pipe_payload_destroy;
    actor_send(&rs->actor, &msg);
  }
}

void _readable_push_stream_on_pipe(stream_t* rs, stream_t* ws) {
  if (rs->is_deactivated == 1) {
    stream_notify(rs, error_event, OFFS_ERROR_TRANSFER("Stream has been destroyed"), (void (*)(void*))error_destroy);
  } else {
    if (rs->pipe_notifiers == NULL) {
      size_t size = 0;
      switch (rs->type) {
        case readable_stream:
          size = 3;
          break;
        case writeable_stream:
          size = 4;
          break;
        case duplex_stream:
        case transform_stream:
          size = 7;
          break;
      }
      rs->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    rs->pipe_notifiers[0].event = piped_event;
    rs->pipe_notifiers[0].id = stream_subscribe(ws, piped_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_push_stream_piped_notify, (void (*)(void*))rs->destructor);
    rs->pipe_notifiers[0].stream = REFERENCE(ws, stream_t);
    rs->pipe_notifiers[1].event = error_event;
    rs->pipe_notifiers[1].id = stream_subscribe(ws, error_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_push_stream_error_notify, (void (*)(void*))rs->destructor);
    rs->pipe_notifiers[1].stream = REFERENCE(ws, stream_t);
    rs->pipe_notifiers[2].event = close_event;
    rs->pipe_notifiers[2].id = stream_subscribe(ws, close_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_push_stream_close_notify, (void (*)(void*))rs->destructor);
    rs->pipe_notifiers[2].stream = REFERENCE(ws, stream_t);
    stream_piped_payload_t* piped_payload = get_clear_memory(sizeof(stream_piped_payload_t));
    piped_payload->source = REFERENCE(rs, stream_t);
    message_t piped_msg;
    piped_msg.type = STREAM_PIPED;
    piped_msg.payload = piped_payload;
    piped_msg.payload_destroy = _stream_piped_payload_destroy;
    actor_send(&ws->actor, &piped_msg);
  }
}

void _readable_push_stream_piped_notify(stream_t* stream, void* payload) {
  (void)payload;
  if (stream->on_push != NULL) {
    readable_push_stream_push(stream);
  }
}

void _readable_push_stream_error_notify(stream_t* stream, void* payload) {
  /* payload is the dispatched error, held only by _stream_notify_dispatch's
     transient reference. Take our own reference so the error_event message
     stream_deactivate builds owns a real ref — stream_deactivate takes
     ownership of one ref but does not add one, so passing a borrowed ref
     would let the upstream wrapper free the error out from under the new
     message. The close-event path stream_deactivate also queues will
     unsubscribe this handler, so the recursion terminates. */
  async_error_t* error = REFERENCE((async_error_t*) payload, async_error_t);
  stream_deactivate(stream, error);
}

void _readable_push_stream_close_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_close(stream);
}

void _writeable_push_stream_on_piped(stream_t* ws, stream_t* rs) {
  if (ws->type == readable_stream) {
    log_error("Invalid writeable push stream being piped");
    return;
  }
  if (ws->is_deactivated == 1) {
    stream_notify(ws, error_event, OFFS_ERROR_TRANSFER("Stream has been destroyed"), (void (*)(void*))error_destroy);
  } else {
    if (ws->pipe_notifiers == NULL) {
      size_t size = 0;
      switch (ws->type) {
        case readable_stream:
          size = 3;
          break;
        case writeable_stream:
          size = 4;
          break;
        case duplex_stream:
        case transform_stream:
          size = 7;
          break;
      }
      ws->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    if ((ws->type == duplex_stream) || (ws->type == transform_stream)) {
      ws->pipe_notifiers[3].event = error_event;
      ws->pipe_notifiers[3].id = stream_subscribe(rs, error_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_error_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[3].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[4].event = close_event;
      ws->pipe_notifiers[4].id = stream_subscribe(rs, close_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_close_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[4].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[5].event = complete_event;
      ws->pipe_notifiers[5].id = stream_subscribe(rs, complete_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_complete_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[5].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[6].event = data_event;
      ws->pipe_notifiers[6].id = stream_subscribe(rs, data_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_data_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[6].stream = REFERENCE(rs, stream_t);
    } else {
      ws->pipe_notifiers[0].event = error_event;
      ws->pipe_notifiers[0].id = stream_subscribe(rs, error_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_error_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[0].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[1].event = close_event;
      ws->pipe_notifiers[1].id = stream_subscribe(rs, close_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_close_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[1].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[2].event = complete_event;
      ws->pipe_notifiers[2].id = stream_subscribe(rs, complete_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_complete_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[2].stream = REFERENCE(rs, stream_t);
      ws->pipe_notifiers[3].event = data_event;
      ws->pipe_notifiers[3].id = stream_subscribe(rs, data_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_push_stream_data_notify, (void (*)(void*))ws->destructor);
      ws->pipe_notifiers[3].stream = REFERENCE(rs, stream_t);
    }
  }
}

void _writeable_push_stream_data_notify(stream_t* stream, void* data) {
  REFERENCE((buffer_t*)data, buffer_t);
  writeable_stream_write(stream, data);
}

void _writeable_push_stream_error_notify(stream_t* stream, void* payload) {
  async_error_t* error = REFERENCE((async_error_t*) payload, async_error_t);
  stream_deactivate(stream, error);
}

void _writeable_push_stream_close_notify(stream_t* stream, void* payload) {
  (void)payload;
  if (!stream->is_deactivated) {
    stream_close(stream);
  }
}

void _writeable_push_stream_complete_notify(stream_t* stream, void* payload) {
  (void)stream;
  (void)payload;
}

/* ---- pull stream piping ---- */

void writeable_pull_stream_pipe(stream_t* ws, stream_t* rs) {
  if (rs->type == writeable_stream || rs->force == push) {
    stream_notify(rs, error_event, OFFS_ERROR_TRANSFER("Invalid write stream being piped"), (void (*)(void*))error_destroy);
  } else if (ws->type == readable_stream || ws->force == push) {
    stream_notify(rs, error_event, OFFS_ERROR_TRANSFER("Invalid read stream being piped to"), (void (*)(void*))error_destroy);
  } else {
    stream_pipe_payload_t* payload = get_clear_memory(sizeof(stream_pipe_payload_t));
    payload->source = REFERENCE(ws, stream_t);
    payload->dest = REFERENCE(rs, stream_t);
    message_t msg;
    msg.type = STREAM_PIPE;
    msg.payload = payload;
    msg.payload_destroy = _stream_pipe_payload_destroy;
    actor_send(&ws->actor, &msg);
  }
}

void _writeable_pull_stream_on_pipe(stream_t* ws, stream_t* rs) {
  if (ws->type == readable_stream || ws->force == push) {
    log_error("Invalid readable pull stream being piped");
    return;
  }
  if (ws->is_deactivated == 1) {
    stream_notify(ws, error_event, OFFS_ERROR_TRANSFER("Stream has been destroyed"), (void (*)(void*))error_destroy);
  } else {
    if (ws->pipe_notifiers == NULL) {
      size_t size = 0;
      switch (ws->type) {
        case readable_stream:
          size = 3;
          break;
        case writeable_stream:
          size = 5;
          break;
        case duplex_stream:
        case transform_stream:
          size = 8;
          break;
      }
      ws->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    ws->pipe_notifiers[0].event = piped_event;
    ws->pipe_notifiers[0].id = stream_subscribe(rs, piped_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_piped_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[0].stream = REFERENCE(rs, stream_t);
    ws->pullable_stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[1].event = error_event;
    ws->pipe_notifiers[1].id = stream_subscribe(rs, error_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_error_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[1].stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[2].event = close_event;
    ws->pipe_notifiers[2].id = stream_subscribe(rs, close_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_close_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[2].stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[3].event = complete_event;
    ws->pipe_notifiers[3].id = stream_subscribe(rs, complete_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_complete_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[3].stream = REFERENCE(rs, stream_t);
    ws->pipe_notifiers[4].event = data_event;
    ws->pipe_notifiers[4].id = stream_subscribe(rs, data_event, REFERENCE(ws, stream_t), (void(*)(void*, void*)) _writeable_pull_stream_data_notify, (void (*)(void*))ws->destructor);
    ws->pipe_notifiers[4].stream = REFERENCE(rs, stream_t);
    ws->is_piped = 1;
    stream_piped_payload_t* piped_payload = get_clear_memory(sizeof(stream_piped_payload_t));
    piped_payload->source = REFERENCE(ws, stream_t);
    message_t piped_msg;
    piped_msg.type = STREAM_PIPED;
    piped_msg.payload = piped_payload;
    piped_msg.payload_destroy = _stream_piped_payload_destroy;
    actor_send(&rs->actor, &piped_msg);
  }
}

void _readable_pull_stream_on_piped(stream_t* rs, stream_t* ws) {
  if (rs->type == writeable_stream) {
    log_error("Invalid writeable push stream being piped");
    return;
  }
  if (rs->is_deactivated == 1) {
    stream_notify(rs, error_event, OFFS_ERROR_TRANSFER("Stream has been destroyed"), (void (*)(void*))error_destroy);
  } else {
    if (rs->pipe_notifiers == NULL) {
      size_t size = 0;
      switch (rs->type) {
        case duplex_stream:
        case transform_stream:
          size = 8;
          break;
        default:
          size = 3;
          break;
      }
      rs->pipe_notifiers = get_clear_memory(size * sizeof(stream_notifier_t));
    }
    if ((rs->type == duplex_stream) || (rs->type == transform_stream)) {
      rs->pipe_notifiers[5].event = error_event;
      rs->pipe_notifiers[5].id = stream_subscribe(ws, error_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_error_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[5].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[6].event = finished_event;
      rs->pipe_notifiers[6].id = stream_subscribe(ws, finished_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_finish_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[6].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[7].event = close_event;
      rs->pipe_notifiers[7].id = stream_subscribe(ws, close_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_close_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[7].stream = REFERENCE(ws, stream_t);
    } else {
      rs->pipe_notifiers[0].event = error_event;
      rs->pipe_notifiers[0].id = stream_subscribe(ws, error_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_error_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[0].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[1].event = finished_event;
      rs->pipe_notifiers[1].id = stream_subscribe(ws, finished_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_finish_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[1].stream = REFERENCE(ws, stream_t);
      rs->pipe_notifiers[2].event = close_event;
      rs->pipe_notifiers[2].id = stream_subscribe(ws, close_event, REFERENCE(rs, stream_t), (void(*)(void*, void*)) _readable_pull_stream_close_notify, (void (*)(void*))rs->destructor);
      rs->pipe_notifiers[2].stream = REFERENCE(ws, stream_t);
    }
    rs->is_piped = 1;
    stream_notify(rs, piped_event, NULL, NULL);
  }
}

void _readable_pull_stream_error_notify(stream_t* stream, void* payload) {
  async_error_t* error = REFERENCE((async_error_t*) payload, async_error_t);
  stream_deactivate(stream, error);
}

void _readable_pull_stream_close_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_close(stream);
}

void _readable_pull_stream_finish_notify(stream_t* stream, void* payload) {
  (void)stream;
  (void)payload;
}

void _writeable_pull_stream_error_notify(stream_t* stream, void* payload) {
  async_error_t* error = REFERENCE((async_error_t*) payload, async_error_t);
  stream_deactivate(stream, error);
}

void _writeable_pull_stream_piped_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_set_pulling_payload_t* pulling_payload = get_clear_memory(sizeof(stream_set_pulling_payload_t));
  pulling_payload->is_pulling = 1;
  message_t msg;
  msg.type = STREAM_SET_PULLING;
  msg.payload = pulling_payload;
  msg.payload_destroy = (void (*)(void*)) free;
  actor_send(&stream->actor, &msg);
}

void _writeable_pull_stream_close_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_close(stream);
}

void _writeable_pull_stream_complete_notify(stream_t* stream, void* payload) {
  (void)payload;
  stream_set_pulling_payload_t* pulling_payload = get_clear_memory(sizeof(stream_set_pulling_payload_t));
  pulling_payload->is_pulling = 0;
  message_t msg;
  msg.type = STREAM_SET_PULLING;
  msg.payload = pulling_payload;
  msg.payload_destroy = (void (*)(void*)) free;
  actor_send(&stream->actor, &msg);
}

void _writeable_pull_stream_data_notify(stream_t* stream, void* data) {
  REFERENCE((buffer_t*)data, buffer_t);
  writeable_stream_write(stream, data);
}