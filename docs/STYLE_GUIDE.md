# SecretAgent Style Guide

This document captures the coding conventions, organizational patterns, and stylistic choices observed throughout the SecretAgent codebase. Follow these when extending the library.

## 1. Repository & Directory Layout

### Top-Level Structure

```
SecretAgent/
├── src/            # All library source code (by module)
├── test/           # Unit tests (GoogleTest, C++)
├── deps/           # Git submodule dependencies (googletest)
├── docs/           # This guide + implementation plans
└── CMakeLists.txt  # Root build file
```

Symlinked reference repositories at the top level (`liboffs/`, `WaveDB/`, `deepseek-harness/`, `onyx/`, `prime-agent/`, `claude-code-source-code/`) are read-only reference codebases. Never modify, build into, or commit anything from them.

### `src/` Module Layout

Each module gets its own PascalCase directory under `src/`. The directory name is the module name:

```
src/
├── Actor/       # Actors, mailboxes (message queue), backpressure, and the size-class memory pool
├── Scheduler/   # Work-stealing deque (deque.c) and the scheduler worker pool
├── RefCounter/  # Reference counting primitives (escrow yield/reference/deferred-deref)
├── Platform/    # Cross-platform threading, time, and compiler shims
└── Util/        # Cross-cutting utilities (allocator, atomic C/C++ compat, logging)
```

Within each module directory:

```
src/ModuleName/
├── module_name.h    # Public header
├── module_name.c    # Public implementation
└── sub_module.h     # Sub-component (if module is complex)
    sub_module.c
```

### Test Layout

Tests live in `test/` and mirror module names:

```
test/
├── CMakeLists.txt
├── test_main.cpp
├── test_actor.cpp
├── test_message_queue.cpp
├── test_pool.cpp
├── test_deque.cpp
├── test_scheduler.cpp
└── test_refcounter.cpp
```

## 2. Naming Conventions

### 2.1 Types

All types use `snake_case` with a `_t` suffix:

```c
typedef struct {
  ATOMIC(uint32_t) packed_state;
  uint8_t is_actor;
} refcounter_t;

typedef struct {
  ATOMIC(uint8_t) flags;
  scheduler_pool_t* pool;
  void* state;
  void (*dispatch)(void* state, message_t* msg);
} actor_t;
```

### 2.2 Enums

Enums use `snake_case` values with a `_e` suffix on the type:

```c
typedef enum {
  ACTOR_QUEUE_IDLE = 0,
  ACTOR_QUEUE_QUEUED = 1,
  ACTOR_QUEUE_RUNNING = 2,
} actor_queue_state_e;

typedef enum {
  FETCH_REQUEST = 0,
  FETCH_RESULT = 1,
} fetch_msg_type_e;
```

Enums use explicit integer values where they carry semantic meaning (message types, queue states). Message type enums (`*_msg_type_e`) are application-defined and live in the header of the module that owns the actor. Flag masks (`ACTOR_FLAG_*`) are `#define` constants, not enums, because they are combined with bitwise-or into a single atomic word.

### 2.3 Functions

Functions follow the `module_action()` pattern. The module prefix is lowercase snake_case matching the filename:

```c
// Creation / destruction
scheduler_pool_t* scheduler_pool_create(size_t worker_count);
void              scheduler_pool_destroy(scheduler_pool_t* pool);

// Actions
void          actor_init(actor_t* actor, void* state, void (*dispatch)(void* state, message_t* msg), scheduler_pool_t* pool);
bool          actor_send(actor_t* actor, message_t* msg);
bool          actor_run(actor_t* actor, size_t batch_size);
void          scheduler_inject(scheduler_pool_t* pool, actor_t* actor);

// Queries
uint16_t      refcounter_count(refcounter_t* refcounter);
size_t        deque_size(deque_t* deque);
bool          message_queue_isempty(message_queue_t* queue);
```

Private / internal functions use a leading underscore:

```c
static void _inject_queue_push(inject_queue_t* queue, actor_t* actor);
```

### 2.4 Macros

Macros are `UPPER_CASE` with underscores:

```c
#define REFERENCE(N, T)     (T*) refcounter_reference((refcounter_t*) N)
#define YIELD(N)            refcounter_yield((refcounter_t*) N)
#define DEREFERENCE(N)      refcounter_dereference((refcounter_t*) N); N = NULL
#define DESTROY(N, T)       T##_destroy(N); N = NULL
#define CONSUME(N, T)       (T*) refcounter_consume((refcounter_t**) &N)
#define POOL_ALLOC(TYPE)    ((TYPE*)pool_alloc(pool_index(sizeof(TYPE))))
#define POOL_FREE(TYPE, VALUE)  pool_free(pool_index(sizeof(TYPE)), (VALUE))
#define ACTOR_BATCH_SIZE    32
#define ACTOR_FLAG_DESTROY  0x08
```

Type-generic macros (REFERENCE, DESTROY, CONSUME, POOL_ALLOC) take both the variable and its type to provide type safety through casting. Atomic flag flags are tested with bitwise-mask literals, e.g. `atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY`.

### 2.5 Includes

Order: own header first, then project headers by module, then system headers.

```c
// In src/Actor/actor.c:
#include "actor.h"                  // Own header first
#include "../Scheduler/scheduler.h" // Project modules
#include "../Util/allocator.h"
#include <stdbool.h>                // System headers last

// In test/test_actor.cpp:
#include <gtest/gtest.h>            // Test framework first
#include <vector>
extern "C" {
#include "../src/Actor/actor.h"     // Tested module
#include "../src/Scheduler/scheduler.h"
}
```

Local/private headers use quotes `""`. System headers use angle brackets `<>`.

**One known cross-module edge:** `src/Scheduler/scheduler.h` includes `../Actor/actor.h`
(it needs the full `actor_t`), while `src/Actor/actor.c` includes
`../Scheduler/scheduler.h`. The reverse edge is broken with a forward declaration
(`typedef struct scheduler_pool_t scheduler_pool_t;` in actor.h) and the pool
registers itself into actors via the pool pointer set at `actor_init`. When adding a
new module, break cycles with forward declarations the same way — never with a new
cyclic include pair.

## 3. File Conventions

### 3.1 Header Files

Every header has an include guard using the `#ifndef` pattern:

```c
// src/ModuleName/module_name.h
#ifndef SA_MODULE_NAME_H
#define SA_MODULE_NAME_H

#include <stdint.h>

// ... declarations ...

#endif // SA_MODULE_NAME_H
```

The guard prefix is `SA_` for SecretAgent headers; `#ifndef SA_MODULE_NAME_H`. (Vendored third-party files keep their original guards — e.g. `Util/log.h` uses `LOG_H` and its rxi copyright.)

Struct types are typically `typedef`'d anonymous structs. The struct tag is the same as the type name when needed for self-referential pointers or opaque declaration:

```c
typedef struct muted_sender_node_t {
  struct actor_t* sender;
  struct muted_sender_node_t* next;
} muted_sender_node_t;
```

### 3.2 Source Files

Every `.c` file includes its own `.h` first:

```c
// src/Actor/actor.c
#include "actor.h"
#include "../Scheduler/scheduler.h"
#include "../Util/allocator.h"
// ...
```

### 3.3 File Header Comments

Source files start with a creation comment:

```c
//
// Created by victor on 5/6/25.
//
```

Files adapted from third-party sources retain their original copyright headers:

```c
/**
 * Copyright (c) 2020 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
 */
```

## 4. Formatting & Style

### 4.1 Indentation & Spacing

- **2-space indentation**, no tabs.
- Opening braces on the same line (Egyptian style).
- Single space between `if`/`while`/`for` and the opening parenthesis.

```c
void backpressure_apply(actor_t* actor) {
  atomic_fetch_or(&actor->flags, ACTOR_FLAG_MUTED);
  backpressure_release(actor);
}
```

### 4.2 Conditionals

Single-statement bodies go on their own line without braces. Prefer early returns over deep nesting:

```c
void scheduler_pool_defer_cleanup(scheduler_pool_t* pool, void* object, void (*destructor)(void*)) {
  if (object == NULL) return;
  refcounter_reference((refcounter_t*) object);
  pending_deref_node_t* node = get_clear_memory(sizeof(pending_deref_node_t));
  node->object = object;
  node->destructor = destructor;
  platform_mutex_lock(pool->deref_lock);
  node->next = pool->pending_derefs;
  pool->pending_derefs = node;
  platform_mutex_unlock(pool->deref_lock);
}
```

### 4.3 Switch Statements

Switch cases fall through with explicit `break`:

```c
switch (msg->type) {
  case FETCH_REQUEST:
    _worker_handle_request(worker, msg);
    break;
  case FETCH_RESULT:
    _worker_handle_result(worker, msg);
    break;
  default:
    break;
}
```

### 4.4 Variable Declarations

Variables declared at the top of the function or at the top of a block scope, one per line:

```c
message_t msg;
msg.type = FETCH_REQUEST;
msg.payload = request;
msg.payload_destroy = free;
actor_send(&worker->actor, &msg);
```

Pointer `*` goes next to the variable name (not the type):

```c
actor_t* actor;        // ✅
actor_t *actor;        // ❌
```

## 5. Core Patterns

### 5.1 Reference Counting

> **Reality check:** this guide is shared with the liboffs codebase family, and the
> examples below use liboffs-style types (`job_t`, `fetch_request_t`, `cache_node_t`)
> that have not been ported yet — they illustrate the convention, not current files.
> Today in SecretAgent: no struct embeds `refcounter_t` yet, and the core runtime
> types (`actor_t`, `scheduler_pool_t`, `message_queue_t`, `deque_t`) are lifetime-
> managed, not refcounted — they use `actor_destroy` / `scheduler_pool_defer_cleanup`
> instead of refcount teardown. The refcounting pattern below is what any NEW
> shared, refcounted object must follow (it is already used by `scheduler_pool_defer_cleanup`
> to hold references across deferrals).

Every heap-allocated shared object embeds `refcounter_t` as its first member. The refcounter is always initialized via `refcounter_init()` which sets the initial count to 1. It must be the last statement of a creation function.

The refcount state is a single atomic 32-bit word (count:16, yield:8, pending_deref:8) manipulated by CAS, so the escrow transfer (yield / pending_deref / count) is one atomic transaction — see `src/RefCounter/refcounter.h`.

```c
job_t* job_create(scheduler_t* owner) {
  job_t* job = get_clear_memory(sizeof(job_t));
  job->owner = owner;
  refcounter_init((refcounter_t*) job);  // Last statement of creation
  return job;
}
```

Destructors always follow the same pattern:

```c
void job_destroy(job_t* job) {
  refcounter_dereference((refcounter_t*) job);
  if (refcounter_count((refcounter_t*) job) == 0) {
    refcounter_destroy_lock((refcounter_t*) job);
    free(job);
  }
}
```

For a dereference where the caller must learn whether the object died (to avoid touching it afterwards), use `refcounter_dereference_is_zero()`.

Ownership transfer macros:

| Macro | Effect |
|-------|--------|
| `REFERENCE(obj, T)` | Increment refcount, return typed pointer |
| `YIELD(obj)` | Transfer ownership without incrementing |
| `DEREFERENCE(obj)` | Drop a reference, null out the pointer |
| `DESTROY(obj, T)` | Call destructor, null out pointer |
| `CONSUME(obj, T)` | Claim ownership from a yielded reference |

### 5.2 Memory Allocation

Always use the project wrappers — never raw `malloc`/`calloc`:

```c
void* ptr = get_memory(size);         // malloc wrapper, aborts on OOM
void* ptr = get_clear_memory(size);   // calloc wrapper (zeroed), aborts on OOM
```

Small, frequently allocated fixed-size objects use the size-class pool instead (Actor module):

```c
message_node_t* node = POOL_ALLOC(message_node_t);   // size-class pool
POOL_FREE(message_node_t, node);
pool_thread_cleanup();  // per thread, before the pool is torn down
```

(The extracted runtime currently allocates `message_node_t` via `get_clear_memory`
in `message_queue.c`; the pool is available for size-classed hot objects.)

### 5.3 Platform Abstraction

Cross-platform code goes through the opaque wrappers in `src/Platform/`. Threading primitives are heap-allocated opaque structs created and destroyed through the API — never raw `pthread_*` calls in library code:

```c
platform_mutex_t* lock = platform_mutex_create();
platform_mutex_lock(lock);
/* critical section */
platform_mutex_unlock(lock);
platform_mutex_destroy(lock);
```

Other primitives follow the same shape: `platform_condvar_*`, `platform_barrier_*`, `platform_thread_create/join/detach`. Time helpers are `platform_sleep_ms(ms)` and `platform_monotonic_ns()` from `platform_time.h`. Compiler shims live in `platform_compiler.h`.

Cross-platform atomics (C11 `<stdatomic.h>` vs C++ `<atomic>`) go through the `ATOMIC`/`ATOMIC_STORE`/`ATOMIC_LOAD`/`ATOMIC_FETCH_*` macros from `src/Util/atomic_compat.h`:

```c
ATOMIC(uint8_t) flags;                    // works in both C11 and C++
atomic_fetch_or(&actor->flags, ACTOR_FLAG_DESTROY);
```

Library code may use C11 `atomic_*` functions directly (as `actor.c` does); the `ATOMIC(T)` typedef macro is what keeps struct fields portable when the header is also included from C++ tests.

### 5.4 Actor & Async Result Message Pattern

The exemplified types (`worker_t`, `fetch_request_t`, `cache_node_t`) are liboffs-style
illustrations; in SecretAgent the pattern is new-module-facing, and a plain `actor_t`
pumped by hand (as the tests do) is also valid.

Asynchronous work is expressed with actors, not contexts + promises. An actor is a message-driven object with an embedded mailbox; a scheduler pool runs queued actors on worker threads, and inline actors (`pool = NULL`) are pumped manually with `actor_run`.

**a) Every actor embeds `actor_t` as its FIRST member.**

```c
// 1. Module header: actor struct + its own message types
typedef enum {
  FETCH_REQUEST = 0,
  FETCH_RESULT = 1,   // request/result naming: FETCH → FETCH_RESULT
} fetch_msg_type_e;

typedef struct {
  actor_t actor;              // MUST be the first member
  scheduler_pool_t* pool;
} worker_t;

// 2. Creation initializes the embedded actor
worker_t* worker_create(scheduler_pool_t* pool) {
  worker_t* worker = get_clear_memory(sizeof(worker_t));
  worker->pool = pool;
  actor_init(&worker->actor, worker, worker_dispatch, pool);
  return worker;
}
```

**b) Dispatch is `void my_dispatch(void* state, message_t* msg)` and switches on `msg->type`.**

```c
void worker_dispatch(void* state, message_t* msg) {
  worker_t* worker = state;
  switch (msg->type) {
    case FETCH_REQUEST: {
      fetch_request_t* req = msg->payload;
      fetch_result_t* result = get_clear_memory(sizeof(fetch_result_t));
      result->reply_to = req->reply_to;   // ownership transfers with the message
      result->block = _worker_fetch(worker, req);
      /* ... answer the requester asynchronously ... */
      break;
    }
    case FETCH_RESULT:
      break;
    default:
      break;
  }
}
```

**c) Heap payloads MUST set `msg.payload_destroy = free`** (or a custom destructor). The mailbox calls `payload_destroy(payload)` when a message is dropped, drained, or destroyed — a NULL `payload_destroy` with a non-NULL heap `payload` leaks.

```c
void send_request(worker_t* worker, actor_t* reply_to) {
  fetch_request_t* req = get_clear_memory(sizeof(fetch_request_t));
  req->reply_to = reply_to;
  message_t msg;
  msg.type = FETCH_REQUEST;
  msg.payload = req;
  msg.payload_destroy = free;     // REQUIRED for heap payloads — NULL leaks
  actor_send(&worker->actor, &msg);
}
```

**d) Payload ownership TRANSFERS on `actor_send`.** After ownership moves into the message, null out result fields you consumed in the completion dispatch so `payload_destroy` cannot free what you now own:

```c
case FETCH_RESULT: {
  fetch_result_t* r = (fetch_result_t*) msg->payload;
  cache_node_t* node = r->block;
  r->block = NULL;          // consumed here — keep payload_destroy from freeing it
  _cache_store(cache, node);
  break;
}
```

**e) Async results are messages back to a `reply_to` actor.** Requests carry a pointer to the requesting actor; the completion is a `*_RESULT` message sent to it:

```c
static void _send_fetch_result(actor_t* reply_to, cache_node_t* block) {
  fetch_result_t* result = get_clear_memory(sizeof(fetch_result_t));
  result->block = block;
  message_t msg;
  msg.type = FETCH_RESULT;    // request/result naming: CACHE_GET → CACHE_GET_RESULT
  msg.payload = result;
  msg.payload_destroy = free;
  actor_send(reply_to, &msg);
}
```

**f) Self-destructing transient actors.** One-shot actors dispose of themselves: set `ACTOR_FLAG_DESTROY` before cleanup so no new send is accepted, then unregister and free — or defer the free to the pool so it runs only after workers quiesce:

```c
// Inline teardown (caller owns the actor and knows no pool traversal is in flight):
atomic_fetch_or(&actor->flags, ACTOR_FLAG_DESTROY);
actor_detach_pool(actor);                      // registry-only unregister
message_queue_destroy(&actor->queue);          // drains + frees payloads
free(worker);

// Deferred teardown on a pool — free after scheduler_pool_drain_pending_derefs()
// or scheduler_pool_destroy runs it:
scheduler_pool_defer_cleanup(pool, actor, (void (*)(void*)) worker_destroy);
```

Completion-actor test pattern: an inline actor with `pool = NULL` has no worker threads — the test pumps the mailbox itself with `actor_run` and polls a completion flag with plain C11/C++ atomics:

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/Util/atomic_compat.h"
#include "../src/Actor/actor.h"
#include "../src/Scheduler/scheduler.h"
}

typedef struct {
  actor_t actor;              // FIRST member
  ATOMIC(uint8_t) done;
} completion_actor_t;

static void completion_dispatch(void* state, message_t* msg) {
  if (msg->type == CACHE_GET_RESULT) {
    completion_actor_t* completion = state;
    ATOMIC_STORE(&completion->done, 1);
  }
}

TEST(TestCompletion, TestResultMessageCompletes) {
  completion_actor_t completion;
  actor_init(&completion.actor, &completion, completion_dispatch, NULL); // pool = NULL
  ATOMIC_STORE(&completion.done, 0);

  /* ... send requests at actors on a scheduler_pool and pump them here ... */

  while (!ATOMIC_LOAD(&completion.done)) {
    actor_run(&completion.actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(10);
  }
  actor_destroy(&completion.actor);
}
```

### 5.5 Error Handling

There is no error-object module in this project; error handling follows three rules:

- **Check returns where cheap.** Callers test the boolean returns (`actor_send`, `message_queue_push`, `deque_*`, `scheduler_pool_wait_for_idle`) and handle failure locally.
- **Log with context.** Failures that cannot be returned are reported through `src/Util/log.h` with the function name and the relevant values:

```c
log_error("scheduler_pool_start: failed to create worker thread %zu of %zu", index, worker_count);
log_error("scheduler_pool_wait_for_idle: stuck with %zu pending messages", pending);
```

  Per-module log levels are available via `log_set_module_level(log_module_t module, int level)`; the module enum (`LOG_MODULE_ACTOR`, `LOG_MODULE_CACHE`, ...) is in `log.h`.
- **Abort on allocation failure.** `get_memory`/`get_clear_memory` log `log_error("Out of memory")` and `abort()` — callers never handle OOM. Out-of-protocol internal inconsistencies (e.g. CAS loops that cannot make progress) abort the process rather than corrupt state.

## 6. Tests

Tests are written in C++ using GoogleTest. C headers are wrapped in `extern "C"`:

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "../src/Actor/actor.h"
#include "../src/Scheduler/scheduler.h"
}
```

Test naming uses a `TestModule` fixture with a `TestPascalCase` test name that
describes the behavior under test (e.g. `TEST(TestActor, TestRunDispatchesMessages)`,
`TEST(TestActor, TestPayloadDestroy)`), not a strict `TestFunction_Scenario` template.
Some suites use grouped behavior fixtures, e.g. `TEST(MessageQueueTeardown, ...)`:

```cpp
TEST(TestActor, TestRunDispatchesMessages) { ... }
TEST(TestActor, TestPayloadDestroy) { ... }
TEST(MessageQueueTeardown, PushAfterDestroyFreesMessageAndReturnsFalse) { ... }
```

Use `ASSERT_*` for fatal assertions (test stops) and `EXPECT_*` for non-fatal checks:

```cpp
scheduler_pool_t* pool = scheduler_pool_create(2);
ASSERT_NE(pool, nullptr);
EXPECT_EQ(pool->worker_count, 2);
scheduler_pool_destroy(pool);
```

After any implementation, run the full suite with:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

and check for memory leaks with valgrind on `cmake-build-debug/test/testsecretagent` before marking work done. For sanitizer builds, run the suite under `setarch -R`:

```bash
setarch -R ctest --test-dir cmake-build-debug --output-on-failure
```

This machine's kernel ASLR entropy randomly aborts ASan-instrumented binaries at `__asan_init`; `setarch -R` (disable ASLR for the process) fixes it.

## 7. Build System

### CMake Patterns

- C standard: C11 (`CMAKE_C_STANDARD 11`). MSVC additionally needs the `/experimental:c11atomics` compile flag (applied globally in the root CMakeLists.txt) because MSVC does not enable C11 `<stdatomic.h>` by default.
- C++ standard: C++17 (tests only)
- Library target: `secretagent` (static)
- Test binary: `testsecretagent`
- Source collected via `file(GLOB_RECURSE C_SRC "src/*/*.c")`
- Tests gate on `SA_BUILD_TESTS`; test discovery via `gtest_add_tests` (works for both MSVC and GNU, unlike `gtest_discover_tests`)
- Sanitizer options (root CMakeLists.txt):
  - `-DSA_ENABLE_ASAN=ON` — instruments the `secretagent` library; the gtest/gmock targets and the test binary are also instrumented so the ASan runtime links correctly
  - `-DSA_ENABLE_UBSAN=ON` — GCC/Clang only
  - `-DSA_ENABLE_TSAN=ON` — GCC/Clang only
  - ASan and TSan are mutually exclusive (the build errors out if both are set)

### Dependency Management

Dependencies are **git submodules** under `deps/` (currently just googletest), added with `add_subdirectory` and linked `PRIVATE`. They are never fetched at configure time and never vendored outside `deps/`.

- A fresh clone must populate submodules before anything builds:

  ```bash
  git submodule update --init --recursive
  ```

  Without this, configure fails in `add_subdirectory(deps)` with a clear
  missing-googletest error. Submodules are never fetched at configure time.

## 8. Summary of Patterns

| Concern | Convention |
|---------|-----------|
| Source root | `src/ModuleName/module_name.c` |
| Public header | `src/ModuleName/module_name.h` |
| Type naming | `snake_case_t` |
| Enum naming | `snake_case_e` |
| Function naming | `module_action()` |
| Private functions | `_function_name()` |
| Macros | `UPPER_CASE` |
| Include guard | `#ifndef SA_MODULE_NAME_H` |
| Indentation | 2 spaces, no tabs |
| Brace style | Egyptian (same line) |
| Pointer style | `type* name` (star next to the name) |
| Memory allocation | `get_memory()` / `get_clear_memory()` (abort on OOM) |
| Small-object allocation | `POOL_ALLOC(TYPE)` / `POOL_FREE(TYPE, VALUE)` size-class pool |
| Object lifecycle | `_create()` → refcounted, `refcounter_init()` last → `_destroy()` |
| Ownership macros | `REFERENCE`, `YIELD`, `DEREFERENCE`, `DESTROY`, `CONSUME` |
| Actor pattern | Embed `actor_t` first; `void dispatch(void* state, message_t* msg)`; request/result messages via `actor_send` with `reply_to`; heap payloads always set `payload_destroy` |
| Error handling | Checked returns + `log_error("context", ...)`; `abort()` on OOM |
| Platform abstraction | `platform_*` opaque wrappers from `platform_thread.h` / `platform_time.h` + `ATOMIC()` from `atomic_compat.h` |
| Test framework | GoogleTest, C++17, `extern "C"` wrappers |
| Test naming | `TEST(TestModule, TestPascalCase)` (behavior-describing name; some suites use grouped fixtures like `MessageQueueTeardown`) |
| Test run | `ctest --test-dir cmake-build-debug --output-on-failure` (+ valgrind; `setarch -R` for ASan builds) |
| Sanitizers | `-DSA_ENABLE_ASAN=` / `_UBSAN=` / `_TSAN=ON` (ASan and TSan mutually exclusive) |

(Note: the scheduler deque intentionally retains old `_deque_array_t` allocation
arrays for in-flight thieves — valgrind "still reachable" blocks of that class are
known-acceptable, not leaks to "fix".)