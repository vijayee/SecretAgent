#ifndef SA_STREAMS_LOOP_THREAD_H
#define SA_STREAMS_LOOP_THREAD_H
#include <stddef.h>
/* The runtime's one IO reactor thread: runs a poll-dancer pd_loop forever
   until destroy. Other threads reach the loop only through streams_loop_call
   (thread-safe marshalling — pd's async mechanism; adapt HERE, never at call
   sites). Fail-loud discipline: if the loop thread dies, log_error + abort
   the process (a half-dead reactor must never limp). */
typedef struct streams_loop_thread_t streams_loop_thread_t;
streams_loop_thread_t* streams_loop_create(void);
/* Runs fn(ctx) ON the loop thread. 0 ok; nonzero if the loop is gone. May
   block only on pd's async queue (µs). fn MUST be µs-scale. */
int streams_loop_call(streams_loop_thread_t* lt, void (*fn)(void*), void* ctx);
/* Underlying pd_loop_t for callers that create watchers (e.g. http_client);
   NULL if the loop thread is dead. Forward-declare struct pd_loop if types.h
   is heavy — include what the real headers make natural. */
struct pd_loop* streams_loop_raw(streams_loop_thread_t* lt);
void streams_loop_destroy(streams_loop_thread_t* lt);
#endif // SA_STREAMS_LOOP_THREAD_H