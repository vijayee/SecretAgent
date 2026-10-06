//
// Created by victor on 9/29/26.
//

#ifndef SA_PYRT_H
#define SA_PYRT_H

#include "../Actor/actor.h"
#include <stddef.h>
#include <stdint.h>

#ifdef SA_HAS_PYTHON
#include "pyrt_messages.h"

typedef enum pyrt_backend_e {
  SA_PYRT_BACKEND_SUBINTERPRETER = 0,
  SA_PYRT_BACKEND_SUBPROCESS = 1
} pyrt_backend_e;

typedef struct pyrt_config_t {
  pyrt_backend_e backend;
  size_t pool_cap;        /* 0 = default (2x cores): max live interpreters */
  unsigned idle_evict_ms; /* 0 = off; idle interpreter torn down after ms */
} pyrt_config_t;

typedef struct pyrt_t pyrt_t;

/* cfg may be NULL (defaults). owner may be NULL (no results routed). The
   FIRST created runtime fixes the global interpreter pool cap. */
pyrt_t* pyrt_create(actor_t* owner, const pyrt_config_t* cfg);

/* Enqueue a cell. Ownership of `code` TRANSFERS (caller strdups). Returns
   the correlation id the RESULT will carry. Never blocks. Starts the pyrt
   thread lazily on the first call. */
uint64_t pyrt_execute(pyrt_t* pyrt, char* code);

/* Request the running cell to stop. COOPERATIVE-only on the pinned 3.12.13
   chain: the request is honored at cell boundaries by the pyrt thread loop;
   a running cell is not preemptible (forced paths verified inert — the
   evidence comment lives on pyrt_interrupt). */
void pyrt_interrupt(pyrt_t* pyrt);

/* 1 while the backend is live (interpreter booted, or subprocess thread up). */
uint8_t pyrt_isactive(const pyrt_t* pyrt);

/* Teardown: sets shutdown, joins the thread, tears down any live backend. */
void pyrt_destroy(pyrt_t* pyrt);

/* Read on the CALLER's thread: the owner of the runtime whose pyrt worker
   this thread is (read from the runtime's thread-local state). NULL when the
   caller is not a pyrt worker thread (e.g. a stdlib thread spawned by a
   cell) or the runtime has no owner. This is what the injected module's
   bridge verbs route into (py_agent.c). */
actor_t* pyrt_thread_owner(void);

/* The calling pyrt thread's runtime (pyrt.c's TLS — the same read
   pyrt_thread_owner routes through; NO duplicated TLS logic). NULL when the
   caller is not a pyrt worker thread. The injected module uses it as the
   token for the parked-ask flag below. */
pyrt_t* pyrt_thread_pyrt(void);

/* The blocked-ask's one-park-at-a-time flag (escalation spec §1.1): set by
   the ask verb at PUBLISH (so a second ask inside the same cell — fired
   before the frame dispatches the first — cannot miss it), read by the verb
   BEFORE publishing and refused as data while it stands, and cleared by the
   engine's consume paths (Task 2's reply + the wake branches) — the engine
   never SETS it. Atomic instance field (the interrupt_req shape) — visible
   across the pyrt thread and the frame's dispatch thread. */
void pyrt_ask_parked_set(pyrt_t* pyrt, uint8_t parked);
uint8_t pyrt_ask_parked(const pyrt_t* pyrt);

/* Route a text payload (PYRT_LOG / PYRT_STATUS / PYRT_EMIT) to the caller's
   runtime owner — the pyrt-thread-relative form of the injected module's
   stream verbs. Copies the text OUT of Python's heap inside; never re-enters
   Python. No-op on a non-pyrt thread or an ownerless runtime. */
void pyrt_post_text(uint32_t type, const char* text);

#endif /* SA_HAS_PYTHON */

#endif // SA_PYRT_H