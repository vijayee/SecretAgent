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

/* Request the running cell to stop (the mechanism is decided by Task 1
   Step 3's header verification; Task 6 implements what it permits). */
void pyrt_interrupt(pyrt_t* pyrt);

/* 1 while the backend is live (interpreter booted, or subprocess thread up). */
uint8_t pyrt_isactive(const pyrt_t* pyrt);

/* Teardown: sets shutdown, joins the thread, tears down any live backend. */
void pyrt_destroy(pyrt_t* pyrt);

#endif /* SA_HAS_PYTHON */

#endif // SA_PYRT_H