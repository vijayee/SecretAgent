#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <stdlib.h>
  #include <synchapi.h>
#else
  #define _POSIX_C_SOURCE 200809L
  #include <stdlib.h>
  #include <pthread.h>
  #include <time.h>
  #include <errno.h>
  /* POSIX branch only: unexpected condvar-wait errors are logged (see
     platform_condvar_timed_wait). */
  #include "../Util/log.h"
#endif

#include "platform_thread.h"

#ifdef _WIN32

struct platform_thread_t {
  HANDLE handle;
  platform_thread_fn_t fn;
  void* arg;
  void* result;
  int detached;
};

struct platform_mutex_t     { CRITICAL_SECTION handle; };
struct platform_rwlock_t    { SRWLOCK handle; };
struct platform_condvar_t   { CONDITION_VARIABLE handle; };
struct platform_barrier_t   { SYNCHRONIZATION_BARRIER handle; };

static DWORD WINAPI _thread_wrapper(LPVOID arg) {
  platform_thread_t* t = (platform_thread_t*)arg;
  t->result = t->fn(t->arg);
  if (t->detached) {
    free(t);
  }
  return 0;
}

platform_thread_t* platform_thread_create(platform_thread_fn_t fn, void* arg) {
  platform_thread_t* t = (platform_thread_t*)calloc(1, sizeof(platform_thread_t));
  if (t == NULL) return NULL;
  t->fn = fn;
  t->arg = arg;
  t->handle = CreateThread(NULL, 0, _thread_wrapper, t, 0, NULL);
  if (t->handle == NULL) {
    free(t);
    return NULL;
  }
  return t;
}

void* platform_thread_join(platform_thread_t* thread) {
  if (thread == NULL) return NULL;
  WaitForSingleObject(thread->handle, INFINITE);
  void* result = thread->result;
  CloseHandle(thread->handle);
  free(thread);
  return result;
}

void platform_thread_detach(platform_thread_t* thread) {
  if (thread == NULL) return;
  thread->detached = 1;
  CloseHandle(thread->handle);
}

uint64_t platform_thread_self(void) {
  return (uint64_t)GetCurrentThreadId();
}

int platform_thread_setup_stack(void) {
  return 0;
}

/* --- Mutex --- */

platform_mutex_t* platform_mutex_create(void) {
  platform_mutex_t* m = (platform_mutex_t*)calloc(1, sizeof(platform_mutex_t));
  if (m == NULL) return NULL;
  InitializeCriticalSection(&m->handle);
  return m;
}

void platform_mutex_destroy(platform_mutex_t* m) {
  if (m == NULL) return;
  DeleteCriticalSection(&m->handle);
  free(m);
}

void platform_mutex_lock(platform_mutex_t* m)   { EnterCriticalSection(&m->handle); }
void platform_mutex_unlock(platform_mutex_t* m) { LeaveCriticalSection(&m->handle); }

/* --- RWLock --- */

platform_rwlock_t* platform_rwlock_create(void) {
  platform_rwlock_t* rw = (platform_rwlock_t*)calloc(1, sizeof(platform_rwlock_t));
  if (rw == NULL) return NULL;
  InitializeSRWLock(&rw->handle);
  return rw;
}

void platform_rwlock_destroy(platform_rwlock_t* rw) {
  free(rw);
}

void platform_rwlock_read_lock(platform_rwlock_t* rw)    { AcquireSRWLockShared(&rw->handle); }
void platform_rwlock_read_unlock(platform_rwlock_t* rw)  { ReleaseSRWLockShared(&rw->handle); }
void platform_rwlock_write_lock(platform_rwlock_t* rw)   { AcquireSRWLockExclusive(&rw->handle); }
void platform_rwlock_write_unlock(platform_rwlock_t* rw) { ReleaseSRWLockExclusive(&rw->handle); }

/* --- Condition Variable --- */

platform_condvar_t* platform_condvar_create(void) {
  platform_condvar_t* cv = (platform_condvar_t*)calloc(1, sizeof(platform_condvar_t));
  if (cv == NULL) return NULL;
  InitializeConditionVariable(&cv->handle);
  return cv;
}

void platform_condvar_destroy(platform_condvar_t* cv) {
  free(cv);
}

void platform_condvar_wait(platform_condvar_t* cv, platform_mutex_t* m) {
  SleepConditionVariableCS(&cv->handle, &m->handle, INFINITE);
}

int platform_condvar_timed_wait(platform_condvar_t* cv, platform_mutex_t* m,
                                uint32_t timeout_ms) {
  /* SleepConditionVariableCS returns nonzero when the variable was signalled
     (or the mutex was abandoned) and zero on timeout. */
  if (SleepConditionVariableCS(&cv->handle, &m->handle, timeout_ms)) {
    return 0;
  }
  return -1;
}

void platform_condvar_signal(platform_condvar_t* cv)    { WakeConditionVariable(&cv->handle); }
void platform_condvar_broadcast(platform_condvar_t* cv) { WakeAllConditionVariable(&cv->handle); }

/* --- Barrier --- */

platform_barrier_t* platform_barrier_create(unsigned int count) {
  platform_barrier_t* b = (platform_barrier_t*)calloc(1, sizeof(platform_barrier_t));
  if (b == NULL) return NULL;
  if (!InitializeSynchronizationBarrier(&b->handle, count, -1)) {
    free(b);
    return NULL;
  }
  return b;
}

void platform_barrier_destroy(platform_barrier_t* b) {
  if (b == NULL) {
    return;
  }
  /* InitializeSynchronizationBarrier allocates a kernel-backed barrier object
     that platform_barrier_wait never auto-deletes (it enters with
     SYNCHRONIZATION_BARRIER_FLAGS_NO_DELETE), so an explicit
     DeleteSynchronizationBarrier is required -- without it the object leaks
     (one per scheduler_pool, i.e. one per offs_node_restart cycle). Mirrors the
     pthread_barrier_destroy in the POSIX branch. */
  DeleteSynchronizationBarrier(&b->handle);
  free(b);
}

int platform_barrier_wait(platform_barrier_t* b) {
  return EnterSynchronizationBarrier(&b->handle, SYNCHRONIZATION_BARRIER_FLAGS_NO_DELETE) ? 1 : 0;
}

#else

struct platform_thread_t {
  pthread_t handle;
  platform_thread_fn_t fn;
  void* arg;
  int detached;
};

struct platform_mutex_t   { pthread_mutex_t  handle; };
struct platform_rwlock_t  { pthread_rwlock_t handle; };
struct platform_condvar_t { pthread_cond_t   handle; };
struct platform_barrier_t { pthread_barrier_t handle; };

/* --- Thread --- */

static void* _thread_wrapper(void* arg) {
  platform_thread_t* t = (platform_thread_t*)arg;
  void* result = t->fn(t->arg);
  if (t->detached) {
    free(t);
  }
  return result;
}

platform_thread_t* platform_thread_create(platform_thread_fn_t fn, void* arg) {
  platform_thread_t* t = (platform_thread_t*)calloc(1, sizeof(platform_thread_t));
  if (t == NULL) {
    return NULL;
  }
  t->fn = fn;
  t->arg = arg;
  if (pthread_create(&t->handle, NULL, _thread_wrapper, t) != 0) {
    free(t);
    return NULL;
  }
  return t;
}

void* platform_thread_join(platform_thread_t* thread) {
  if (thread == NULL) {
    return NULL;
  }
  void* result;
  pthread_join(thread->handle, &result);
  free(thread);
  return result;
}

void platform_thread_detach(platform_thread_t* thread) {
  if (thread == NULL) {
    return;
  }
  thread->detached = 1;
  pthread_detach(thread->handle);
}

uint64_t platform_thread_self(void) {
  return (uint64_t)pthread_self();
}

int platform_thread_setup_stack(void) {
  return 0;
}

/* --- Mutex --- */

platform_mutex_t* platform_mutex_create(void) {
  platform_mutex_t* m = (platform_mutex_t*)calloc(1, sizeof(platform_mutex_t));
  if (m == NULL) {
    return NULL;
  }
  if (pthread_mutex_init(&m->handle, NULL) != 0) {
    free(m);
    return NULL;
  }
  return m;
}

void platform_mutex_destroy(platform_mutex_t* m) {
  if (m == NULL) {
    return;
  }
  pthread_mutex_destroy(&m->handle);
  free(m);
}

void platform_mutex_lock(platform_mutex_t* m) {
  pthread_mutex_lock(&m->handle);
}

void platform_mutex_unlock(platform_mutex_t* m) {
  pthread_mutex_unlock(&m->handle);
}

/* --- RWLock --- */

platform_rwlock_t* platform_rwlock_create(void) {
  platform_rwlock_t* rw = (platform_rwlock_t*)calloc(1, sizeof(platform_rwlock_t));
  if (rw == NULL) {
    return NULL;
  }
  if (pthread_rwlock_init(&rw->handle, NULL) != 0) {
    free(rw);
    return NULL;
  }
  return rw;
}

void platform_rwlock_destroy(platform_rwlock_t* rw) {
  if (rw == NULL) {
    return;
  }
  pthread_rwlock_destroy(&rw->handle);
  free(rw);
}

void platform_rwlock_read_lock(platform_rwlock_t* rw) {
  pthread_rwlock_rdlock(&rw->handle);
}

void platform_rwlock_read_unlock(platform_rwlock_t* rw) {
  pthread_rwlock_unlock(&rw->handle);
}

void platform_rwlock_write_lock(platform_rwlock_t* rw) {
  pthread_rwlock_wrlock(&rw->handle);
}

void platform_rwlock_write_unlock(platform_rwlock_t* rw) {
  pthread_rwlock_unlock(&rw->handle);
}

/* --- Condition Variable --- */

platform_condvar_t* platform_condvar_create(void) {
  platform_condvar_t* cv = (platform_condvar_t*)calloc(1, sizeof(platform_condvar_t));
  if (cv == NULL) {
    return NULL;
  }
  if (pthread_cond_init(&cv->handle, NULL) != 0) {
    free(cv);
    return NULL;
  }
  return cv;
}

void platform_condvar_destroy(platform_condvar_t* cv) {
  if (cv == NULL) {
    return;
  }
  pthread_cond_destroy(&cv->handle);
  free(cv);
}

void platform_condvar_wait(platform_condvar_t* cv, platform_mutex_t* m) {
  pthread_cond_wait(&cv->handle, &m->handle);
}

int platform_condvar_timed_wait(platform_condvar_t* cv, platform_mutex_t* m,
                                uint32_t timeout_ms) {
  /* The condvar is initialized without attribute clocks, so timed waits run
     against CLOCK_REALTIME: build an absolute deadline of now + timeout. */
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += (time_t)(timeout_ms / 1000u);
  deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }
  int rc = pthread_cond_timedwait(&cv->handle, &m->handle, &deadline);
  if (rc != 0 && rc != ETIMEDOUT) {
    /* Unexpected error (e.g. EINVAL/EPERM): it is not a timeout, but mapping
       it to 0 ("signalled") would send callers into a busy re-wait loop —
       make it visible. */
    log_error("platform_condvar_timed_wait: pthread_cond_timedwait failed "
              "with unexpected errno %d", rc);
  }
  return (rc == ETIMEDOUT) ? -1 : 0;
}

void platform_condvar_signal(platform_condvar_t* cv) {
  pthread_cond_signal(&cv->handle);
}

void platform_condvar_broadcast(platform_condvar_t* cv) {
  pthread_cond_broadcast(&cv->handle);
}

/* --- Barrier --- */

platform_barrier_t* platform_barrier_create(unsigned int count) {
  platform_barrier_t* b = (platform_barrier_t*)calloc(1, sizeof(platform_barrier_t));
  if (b == NULL) {
    return NULL;
  }
  if (pthread_barrier_init(&b->handle, NULL, count) != 0) {
    free(b);
    return NULL;
  }
  return b;
}

void platform_barrier_destroy(platform_barrier_t* b) {
  if (b == NULL) {
    return;
  }
  pthread_barrier_destroy(&b->handle);
  free(b);
}

int platform_barrier_wait(platform_barrier_t* b) {
  int rc = pthread_barrier_wait(&b->handle);
  return (rc == PTHREAD_BARRIER_SERIAL_THREAD) ? 1 : 0;
}

#endif /* !_WIN32 */
