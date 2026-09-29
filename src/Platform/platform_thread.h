#ifndef SA_PLATFORM_THREAD_H
#define SA_PLATFORM_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct platform_thread_t platform_thread_t;
typedef struct platform_mutex_t platform_mutex_t;
typedef struct platform_rwlock_t platform_rwlock_t;
typedef struct platform_condvar_t platform_condvar_t;
typedef struct platform_barrier_t platform_barrier_t;

typedef void* (*platform_thread_fn_t)(void* arg);

/* Thread lifecycle */
platform_thread_t* platform_thread_create(platform_thread_fn_t fn, void* arg);
void* platform_thread_join(platform_thread_t* thread);
void platform_thread_detach(platform_thread_t* thread);
uint64_t platform_thread_self(void);
int platform_thread_setup_stack(void);

/* Mutex */
platform_mutex_t* platform_mutex_create(void);
void platform_mutex_destroy(platform_mutex_t* m);
void platform_mutex_lock(platform_mutex_t* m);
void platform_mutex_unlock(platform_mutex_t* m);

/* RWLock */
platform_rwlock_t* platform_rwlock_create(void);
void platform_rwlock_destroy(platform_rwlock_t* rw);
void platform_rwlock_read_lock(platform_rwlock_t* rw);
void platform_rwlock_read_unlock(platform_rwlock_t* rw);
void platform_rwlock_write_lock(platform_rwlock_t* rw);
void platform_rwlock_write_unlock(platform_rwlock_t* rw);

/* Condition variable */
platform_condvar_t* platform_condvar_create(void);
void platform_condvar_destroy(platform_condvar_t* cv);
void platform_condvar_wait(platform_condvar_t* cv, platform_mutex_t* m);
/* Bounded wait: block until the condvar is signalled or timeout_ms elapses.
   The caller must hold the mutex on entry; it is re-acquired before return.
   Returns 0 when signalled, -1 when the timeout elapsed. Used by the
   peer-book actor's synchronous request round-trips (src/Network/peer_book.c)
   so an unresponsive actor cannot hang an HTTP/wire worker forever. */
int platform_condvar_timed_wait(platform_condvar_t* cv, platform_mutex_t* m,
                                uint32_t timeout_ms);
void platform_condvar_signal(platform_condvar_t* cv);
void platform_condvar_broadcast(platform_condvar_t* cv);

/* Barrier */
platform_barrier_t* platform_barrier_create(unsigned int count);
void platform_barrier_destroy(platform_barrier_t* b);
int platform_barrier_wait(platform_barrier_t* b);

#ifdef __cplusplus
}
#endif

#endif /* SA_PLATFORM_THREAD_H */
