//
// Created by victor on 10/12/25.
//

#ifndef OFFS_STREAM_H
#define OFFS_STREAM_H
#include "../RefCounter/refcounter.h"
#include "../Actor/actor.h"
#include "../Scheduler/scheduler.h"
#include "../Util/error.h"
#include "../Util/atomic_compat.h"

typedef enum {
  readable_stream = 0,
  writeable_stream = 1,
  duplex_stream = 2,
  transform_stream = 3
}  stream_type_e;

typedef enum {
  push = 0,
  pull = 1
} stream_force_e;

typedef enum {
  readable_event = 0,
  complete_event = 1,
  data_event = 2,
  overflow_event = 3,
  empty_event = 4,
  pause_event = 5,
  resume_event = 6,
  pipe_event = 7,
  unpipe_event = 8,
  close_event = 9,
  finished_event = 10,
  unpiped_event = 11,
  piped_event = 12,
  drain_event = 13,
  error_event = 14,
  load_tuple_event = 15
} stream_event_e;

typedef struct {
  refcounter_t refcounter;
  size_t id;
  void* ctx;
  void (* handler)(void*, void*);
  void (* ctx_destroy)(void*);
  uint8_t once;
} stream_event_handler_t;
stream_event_handler_t* stream_event_handler_create(size_t id, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*), uint8_t once);
void stream_event_handler_destroy(stream_event_handler_t* handler);
typedef struct stream_event_handler_list_node_t stream_event_handler_list_node_t;

struct stream_event_handler_list_node_t {
  stream_event_handler_t* handler;
  stream_event_handler_list_node_t* next;
  stream_event_handler_list_node_t* previous;
};

typedef struct {
  stream_event_handler_list_node_t *first;
  stream_event_handler_list_node_t *last;
  size_t count;
} stream_event_handler_list_t;

stream_event_handler_list_t* stream_event_list_create();
void stream_event_list_destroy(stream_event_handler_list_t* list);
void stream_event_list_enqueue(stream_event_handler_list_t* list, stream_event_handler_t* handler);
stream_event_handler_t* stream_event_list_dequeue(stream_event_handler_list_t* list);
void stream_event_list_remove(stream_event_handler_list_t* list, stream_event_handler_list_node_t* node);
void stream_event_list_remove_onces(stream_event_handler_list_t* list);

typedef struct {
  size_t size;
  void (*cb)(void*, void*);
} stream_read_payload_t;

typedef struct stream_t stream_t;

typedef struct {
  stream_event_e event;
  size_t id;
  stream_t* stream;
} stream_notifier_t;

typedef struct {
  stream_t* stream;
} stream_close_payload_t;

typedef struct {
  stream_t* stream;
  void* data;
} stream_write_payload_t;

typedef struct {
  stream_event_e event;
  void* payload;
  void (*payload_destroy)(void*);
} stream_notify_payload_t;

typedef struct {
  stream_t* stream;
  size_t size;
  void* ctx;
  void (*cb)(void*, void*);
} stream_read_message_payload_t;

/* Stream actor message payloads */
typedef struct {
  size_t id;
  stream_event_e event;
  void* ctx;
  void (*handler)(void*, void*);
  void (*ctx_destroy)(void*);
  uint8_t once;
} stream_subscribe_payload_t;

typedef struct {
  stream_event_e event;
  size_t id;
} stream_unsubscribe_payload_t;

typedef struct {
  async_error_t* error;
} stream_deactivate_payload_t;

typedef struct {
  stream_t* source;
  stream_t* dest;
} stream_pipe_payload_t;

typedef struct {
  stream_t* source;
} stream_piped_payload_t;

typedef struct {
  void (*on_close)(stream_t*);
} stream_close_handler_payload_t;

typedef struct {
  uint8_t is_pulling;
} stream_set_pulling_payload_t;

#define STREAM_HANDLER_COUNT 16
struct stream_t {
  refcounter_t refcounter;
  stream_type_e type;
  stream_force_e force;
  ATOMIC(size_t) next_handler_id;
  /* The handler lists are owned by the stream's actor: stream_subscribe,
     stream_unsubscribe, stream_once and stream_notify all route through actor
     messages, so the lists are only ever read and mutated on the actor thread
     — no lock is needed. next_handler_id is atomically incremented by the
     caller so the subscribe id can be returned synchronously. */
  stream_event_handler_list_t* handlers[STREAM_HANDLER_COUNT];
  uint8_t readable;
  uint8_t is_piped;
  uint8_t auto_push;
  uint8_t is_pulling;
  uint8_t is_deactivated;
  stream_t* pullable_stream;
  stream_notifier_t* pipe_notifiers;
  scheduler_pool_t* pool;
  actor_t actor;
  void (*on_data)(stream_t*, void*);
  void (*on_push)(stream_t*);
  void (*on_pull)(stream_t*);
  void (*on_read)(stream_t*, size_t, void*, void (*)(void*, void*));
  void (*on_write)(stream_t*, void*);
  void (*on_close)(stream_t*);
  void (*on_deactivated)(stream_t*);
  void (*on_close_read)(stream_t*);
  void (*on_close_write)(stream_t*);
  void (*on_pipe)(stream_t*, stream_t*);
  void (*on_piped)(stream_t*, stream_t*);
  void (*destructor)(stream_t*);
};


typedef struct {
  void* ctx;
  void* readable;
  void* complete;
  void* data;
  void* overflow;
  void* empty;
  void* pause;
  void* resume;
  void* pipe;
  void* unpipe;
  void* close;
  void* error;
} readable_push_stream_events_t;

typedef struct {
  stream_t stream;
  uint8_t readable;
  uint8_t is_piped;
  uint8_t auto_push;
  uint8_t is_destroyed;
} readable_push_stream_t;

readable_push_stream_t* readable_push_stream_create();
void readable_push_stream_subscribe(readable_push_stream_events_t events);

typedef struct {
  void* ctx;
  void* finished;
  void* unpiped;
  void* piped;
  void* drain;
  void* close;
  void* error;
} writeable_push_stream_events_t;

void stream_init(stream_t* stream, stream_force_e force, stream_type_e type, uint8_t auto_push, scheduler_pool_t* pool, void (*destructor)(stream_t*));
void stream_deinit(stream_t* stream);
void stream_destroy(stream_t* stream);
void stream_dispatch(void* state, message_t* msg);
void stream_deactivate(stream_t* stream, async_error_t* error);
void stream_close(stream_t* stream);
void stream_close_handler(stream_t* stream,  void (*on_close)(stream_t*));
size_t stream_subscribe(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*));
size_t stream_once(stream_t* stream, stream_event_e event, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*));
void stream_unsubscribe(stream_t* stream, stream_event_e event, size_t id);
void writeable_stream_data_handler(stream_t* stream, void (*on_data)(stream_t*, void*));
void readable_stream_push_handler(stream_t* stream, void (*on_push)(stream_t*));
void readable_push_stream_pipe(stream_t* rs, stream_t* ws);
/* Notifies a stream's handlers of an event with an optional payload.
 *
 * This is the PUBLIC entry point: it does NOT dispatch synchronously. It wraps
 * the payload in a STREAM_NOTIFY message and hands it to the stream's actor,
 * which dispatches it to the handlers on the actor thread. This keeps the
 * handler list touched by exactly one thread (the actor), so subscribe /
 * unsubscribe / once / notify never race on the list.
 *
 * Ownership: stream_notify TAKES OWNERSHIP of the payload (and the caller's
 * reference, if any). The message owns the payload until the actor dispatches
 * it: handlers that keep the payload take their own reference, and the
 * no-handler path destroys it. If the stream is destroyed before the message is
 * dispatched, the message's payload_destroy releases it. Callers that pass a
 * freshly-created, unowned payload (e.g. the inline OFFS_ERROR(...) "No X
 * Handler Defined" notifications) transfer that unowned payload here; callers
 * that pass a referenced payload (CONSUME(...)) transfer their reference here. */
void stream_notify(stream_t* stream, stream_event_e event, void* payload, void (*payload_destroy)(void*));
void readable_push_stream_push(stream_t* stream);
void readable_stream_read(stream_t* stream, size_t size, void* ctx, void (*cb)(void*, void*));
void writeable_stream_write_handler(stream_t* stream, void (*)(stream_t*, void*));
void writeable_stream_write(stream_t* stream, void* data);
void _writeable_push_stream_on_piped(stream_t* ws, stream_t* rs);
void stream_unsubscribe_pipe_notifiers(stream_t* stream);
void readable_stream_pull_handler(stream_t* stream, void (*on_pull)(stream_t*));
void writeable_pull_stream_pipe(stream_t* ws, stream_t* rs);
void readable_pull_stream_pull(stream_t* stream);
void stream_deferred_deref(stream_t* stream);
void stream_subscribe_internal(stream_t* stream, stream_event_e event, size_t id, void* ctx, void (* handler)(void*, void*), void (* ctx_destroy)(void*), uint8_t once);
void stream_unsubscribe_internal(stream_t* stream, stream_event_e event, size_t id);
void stream_pipe_internal(stream_t* source, stream_t* dest);
void stream_piped_internal(stream_t* stream, stream_t* source);
#endif //OFFS_STREAM_H