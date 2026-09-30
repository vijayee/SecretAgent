#ifndef SA_HTTP_CLIENT_H
#define SA_HTTP_CLIENT_H
#include <stddef.h>
#include <stdint.h>
#include "loop_thread.h"
/* Async HTTP/1.1 client on poll-dancer: ONE POST, one response — the async
   twin of the retired src/Net/http contract. Non-blocking connect/send/recv
   on watchers; the timeout is a loop timer. The completion runs ON THE LOOP
   THREAD and MUST be µs-scale. The callback MUST NOT call back into the same
   client (submit/destroy from inside on_done deadlocks). Ownership: body and
   error are heap; the CALLBACK owns them (free() or stash). status: HTTP
   code, or -1 for transport errors (error set, body NULL). Connection closed
   and request-side memory freed inside the client before the callback
   fires. */
typedef void (*http_client_completion_fn)(void* ctx, int status, char* body,
                                          size_t body_len, char* error);
typedef struct http_client_t http_client_t;
/* The client borrows the loop thread. submit COPIES url/api_key/body into
   the request (caller memory may vanish after return). Returns 0 on accept;
   nonzero = rejected BEFORE any I/O — the completion NEVER fires for a
   rejected submit, ctx untouched. */
http_client_t* http_client_create(streams_loop_thread_t* lt);
int http_client_submit(http_client_t* c, const char* url, const char* api_key,
                       const char* body_json, uint32_t timeout_ms,
                       http_client_completion_fn on_done, void* ctx);
/* Cancels in-flight requests this client owns and destroys it. Callbacks
   that have not fired will NEVER fire (the caller owns ctx cleanup). */
void http_client_destroy(http_client_t* c);
#endif // SA_HTTP_CLIENT_H