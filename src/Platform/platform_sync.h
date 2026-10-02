//
// Created by victor on 10/02/26.
//

#ifndef SA_PLATFORM_SYNC_H
#define SA_PLATFORM_SYNC_H

/* The thread / mutex / condvar SUBSET of platform_thread.h — WITHOUT the
   barrier prototypes, DELIBERATELY. WHY it exists: WaveDB's
   src/Util/threadding.h declares its own platform_barrier_{init,wait,destroy}
   family (the aliased, family-shared SYNCHRONIZATION_BARRIER symbol set this
   build alias-links), and those declarations CONFLICT textually with
   platform_thread.h's platform_barrier_create/wait/destroy. DO NOT add a
   barrier — or any other absent prototype — here: a barrier prototype in
   this header would collide with WaveDB's at link time for every TU on the
   Database/ include chain, and the link error surfaces far from the cause.
   Any TU that also includes WaveDB headers (frame.c's Database/ include
   chain) must not include platform_thread.h at all — it takes this
   barrier-free subset instead. platform_thread.c's single implementation
   backs BOTH headers; every declaration below is copied VERBATIM from
   platform_thread.h's matching block. Keep the two in sync. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct platform_thread_t platform_thread_t;
typedef struct platform_mutex_t platform_mutex_t;
typedef struct platform_condvar_t platform_condvar_t;

typedef void* (*platform_thread_fn_t)(void* arg);

/* Thread lifecycle */
platform_thread_t* platform_thread_create(platform_thread_fn_t fn, void* arg);
void* platform_thread_join(platform_thread_t* thread);
void platform_thread_detach(platform_thread_t* thread);

/* Mutex */
platform_mutex_t* platform_mutex_create(void);
void platform_mutex_destroy(platform_mutex_t* m);
void platform_mutex_lock(platform_mutex_t* m);
void platform_mutex_unlock(platform_mutex_t* m);

/* Condition variable */
platform_condvar_t* platform_condvar_create(void);
void platform_condvar_destroy(platform_condvar_t* cv);
void platform_condvar_wait(platform_condvar_t* cv, platform_mutex_t* m);
/* Bounded wait: block until the condvar is signalled or timeout_ms elapses.
   The caller must hold the mutex on entry; it is re-acquired before return.
   Returns 0 when signalled, -1 when the timeout elapsed. */
int platform_condvar_timed_wait(platform_condvar_t* cv, platform_mutex_t* m,
                                uint32_t timeout_ms);
void platform_condvar_signal(platform_condvar_t* cv);
void platform_condvar_broadcast(platform_condvar_t* cv);

#ifdef __cplusplus
}
#endif

#endif // SA_PLATFORM_SYNC_H
