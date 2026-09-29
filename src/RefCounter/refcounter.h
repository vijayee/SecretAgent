//
// Created by victor on 3/18/25.
//

#ifndef SA_REFCOUNTER_H
#define SA_REFCOUNTER_H
#include <stdint.h>
#include <stdbool.h>
#include "../Platform/platform.h"
#include "../Util/atomic_compat.h"
#define REFERENCE(N,T) (T*) refcounter_reference((refcounter_t*) N)
#define YIELD(N) refcounter_yield((refcounter_t*) N)
#define DEREFERENCE(N) refcounter_dereference((refcounter_t*) N); N = NULL
#define DESTROY(N,T)  T##_destroy(N); N = NULL
#define CONSUME(N, T) (T*) refcounter_consume((refcounter_t**) &N)
#define SA_ATOMIC
/* Atomic refcounter state: a single 32-bit word manipulated by CAS so the
   escrow transfer (yield/pending_deref/count) is one atomic transaction.
   Layout: bits 0-15 = count, bits 16-23 = yield, bits 24-31 = pending_deref.
   The actor and non-actor paths share this word; the actor path's CAS always
   succeeds first try (single-threaded per actor). is_actor is kept for
   debug/introspection only and no longer branches behavior.
   See docs/concurrency-pass.md F1. */
typedef struct refcounter_t {
#ifdef SA_ATOMIC
  ATOMIC(uint32_t) packed_state;
  uint8_t is_actor;
#else
  uint16_t count;
  uint8_t yield;
  uint8_t pending_deref;
  platform_mutex_t* lock;
#endif
} refcounter_t;

/* Pack/unpack helpers for the atomic-state word. Layout: count:16, yield:8,
   pending:8. Provided as static inline so both refcounter.c and tests can
   share the exact same decode. */
static inline uint32_t refcounter_pack(uint16_t count, uint8_t yield, uint8_t pending) {
  return ((uint32_t)count) | ((uint32_t)yield << 16) | ((uint32_t)pending << 24);
}
static inline uint16_t refcounter_packed_count(uint32_t state) { return (uint16_t)(state & 0xFFFFu); }
static inline uint8_t  refcounter_packed_yield(uint32_t state) { return (uint8_t)((state >> 16) & 0xFFu); }
static inline uint8_t  refcounter_packed_pending(uint32_t state) { return (uint8_t)((state >> 24) & 0xFFu); }

void refcounter_init(refcounter_t* refcounter);
void refcounter_init_actor(refcounter_t* refcounter);
void refcounter_yield(refcounter_t* refcounter);
void* refcounter_reference(refcounter_t* refcounter);
void refcounter_dereference(refcounter_t* refcounter);
bool refcounter_dereference_is_zero(refcounter_t* refcounter);
refcounter_t* refcounter_consume(refcounter_t** refcounter);
uint16_t refcounter_count(refcounter_t* refcounter);
uint8_t refcounter_pending_derefs(refcounter_t* refcounter);
void refcounter_destroy_lock(refcounter_t* refcounter);
#endif //SA_REFCOUNTER_H