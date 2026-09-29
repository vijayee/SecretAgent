# CPython as an Actor (pyrt) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An actor frame executes Python cells in a live CPython 3.12.13 subinterpreter without blocking the actor — `log`/`status` output streams out live, results return corr-matched, with lazy boot, a pool cap, idle eviction, interrupts, and a subprocess fallback backend.

**Architecture:** `src/Python/` (liboffs module style) adds the pyrt runtime: each frame that runs code gets a dedicated pyrt OS thread (lazily booted on first EXECUTE) holding its own CPython subinterpreter with a per-interpreter GIL (PEP 684). Cells run through an injected `__sa_exec_cell` helper; Python output crosses out via an injected `actor` module whose C callbacks post messages to the owning actor's mailbox. CPython is a pinned git submodule built by CMake `ExternalProject` into `libpython3.12.a`; `SA_ENABLE_PYTHON=OFF` removes the whole feature from the build.

**Tech Stack:** C11, CPython tag `v3.12.13` (`deps/cpython` submodule, static `libpython3.12.a`), CMake ExternalProject, GoogleTest, the extracted actor runtime (`actor_t`, `message_t`, platform layer).

**Spec:** `docs/superpowers/specs/2026-09-29-cpython-as-actor-design.md`. **Design answers:** `docs/design-answers.md` §4-5. **Style:** `docs/STYLE_GUIDE.md`.

**Frozen boundaries (hard rules):** never modify anything under `liboffs/`, `WaveDB/`, `deepseek-harness/`, `onyx/`, `prime-agent/`, `claude-code-source-code/` (read-only reference symlinks). `src/Actor`, `src/Scheduler`, `src/RefCounter`, `src/Util` are frozen reference implementations — only `src/Platform/` may gain files, and only where a task says so. All ctest runs use `setarch -R ctest ...`. All test binaries: `cmake-build-debug/test/testsecretagent`.

**How `pyrt.c` is written (deliberate plan structure):** Tasks 1-3 freeze the small files completely (full listings). `pyrt.c` and `py_subprocess.c` are implemented against a **contract** (Task 4 / Task 9 below) — a function-by-function table with exact signatures, semantics, ownership, and invariants, plus the CPython API usage notes verified against the pinned headers. The contract is the plan's "complete content": it is exhaustive about names, behavior, and ordering, and it is *verification-gated* (`grep` against `deps/cpython` headers) precisely because CPython's subinterpreter API is the one place this project must never trust memory over headers. Tests are always embedded in full.

## File structure

```
src/Platform/platform_process.{h,c}   (Task 1: platform_core_count — tiny liboffs module, pool-cap default)
src/Platform/platform.h               (Task 1: one added include)
CMakeLists.txt                        (Tasks 1: SA_ENABLE_PYTHON, cpython-ext, link/filter)
deps/cpython/                         (Task 1: submodule pinned v3.12.13)
src/Python/pyrt_messages.h            (Task 2: full listing)
src/Python/pyrt.h                     (Task 3: full listing, final)
src/Python/pyrt.c                     (Task 4: written to the frozen contract)
src/Python/py_subprocess.{h,c}        (Task 9: header full listing; .c to the subprocess contract)
test/test_pyrt.cpp                    (Tasks 3-9: full test listings, grown additively)
test/CMakeLists.txt                   (Task 3: one target_sources block)
atlas/workflow.json                   (Task 10: slice → in-progress)
```

---

### Task 1: CPython 3.12.13 submodule + platform_process + ExternalProject

**Files:**
- Create: `deps/cpython` (submodule), `src/Platform/platform_process.h`, `src/Platform/platform_process.c`
- Modify: `src/Platform/platform.h` (one include line), `CMakeLists.txt`

- [ ] **Step 1: Add the CPython submodule pinned at v3.12.13**

```bash
git submodule add --depth 1 --branch v3.12.13 https://github.com/python/cpython.git deps/cpython
```
Expected: `.gitmodules` gains `[submodule "deps/cpython"]`. If shallow-by-tag fails: `git submodule add https://github.com/python/cpython.git deps/cpython` + `git -C deps/cpython checkout v3.12.13`; commit the gitlink either way.

- [ ] **Step 2: Extract `platform_process` (only new Platform module; needed by the pool-cap default)**

```bash
cp liboffs/src/Platform/platform_process.h liboffs/src/Platform/platform_process.c src/Platform/
sed -i 's/\bOFFS_/SA_/g' src/Platform/platform_process.h src/Platform/platform_process.c
```
Add one line to `src/Platform/platform.h` after `#include "platform_thread.h"`:

```c
#include "platform_process.h"
```

- [ ] **Step 3: Verify the API against the pinned headers (gates everything)**

```bash
grep -n "use_main_obmalloc\|check_multi_interp_extensions\|PyInterpreterConfig_OWN_GIL" deps/cpython/Include/cpython/initconfig.h | head -5
grep -n "PyStatus Py_NewInterpreterFromConfig\|void Py_EndInterpreter" deps/cpython/Include/cpython/pylifecycle.h
grep -n "install_signal_handlers" deps/cpython/Include/cpython/initconfig.h | head -2
grep -n "PyThreadState_SetAsyncExc" deps/cpython/Include/cpython/pystate.h
grep -n "PyErr_SetInterruptEx\|void PyErr_SetInterrupt(" deps/cpython/Include/cpython/pylifecycle.h | head -3
```
Expected on v3.12.13: (1) `PyInterpreterConfig` int fields `use_main_obmalloc`, `check_multi_interp_extensions`, `gil` + constant `PyInterpreterConfig_OWN_GIL`; (2) `PyStatus Py_NewInterpreterFromConfig(PyThreadState **new_tstate, const PyInterpreterConfig *config)` and `void Py_EndInterpreter(PyThreadState *tstate)`; (3) `PyConfig` field `install_signal_handlers`; (4) `PyThreadState_SetAsyncExc`; (5) `PyErr_SetInterruptEx` and/or `PyErr_SetInterrupt`. **If any name differs: every later reference adapts to what the headers say; record the rename in the commit message. The grep results decide the interrupt mechanism (Task 6) — nothing in this plan overrides the pinned headers.**

- [ ] **Step 4: Wire CMake — insert into root `CMakeLists.txt` after `add_subdirectory(deps)`, before `add_library(secretagent STATIC ${C_SRC})`**

```cmake
# Optional Python execution layer: CPython 3.12 built from the pinned
# deps/cpython submodule via ExternalProject (the liboffs external-dep
# pattern). OFF keeps the library libpython-free for embedders that do
# not host Python.
option(SA_ENABLE_PYTHON "Build the Python execution layer (requires deps/cpython)" ON)
if(SA_ENABLE_PYTHON)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/cpython/Include/Python.h)
    message(FATAL_ERROR "deps/cpython submodule missing. Run: git submodule update --init --recursive")
  endif()
  include(ExternalProject)
  set(CPYTHON_ROOT    ${CMAKE_CURRENT_SOURCE_DIR}/deps/cpython)
  set(CPYTHON_BUILD   ${CMAKE_BINARY_DIR}/deps/cpython)
  set(CPYTHON_CONFIGURE ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD} bash ${CPYTHON_ROOT}/configure --without-ensurepip --disable-test-modules)
  set(CPYTHON_INSTALL ${CMAKE_COMMAND} -E echo "Skipping install; consuming build-dir artifacts directly")
  ExternalProject_Add(cpython-ext
    SOURCE_DIR ${CPYTHON_ROOT}
    CONFIGURE_COMMAND ${CPYTHON_CONFIGURE}
    BUILD_COMMAND make -j$(nproc)
    INSTALL_COMMAND ${CPYTHON_INSTALL}
    BUILD_BYPRODUCTS ${CPYTHON_BUILD}/libpython3.12.a
    BINARY_DIR ${CPYTHON_BUILD}
  )
endif()
if(NOT SA_ENABLE_PYTHON)
  list(FILTER C_SRC EXCLUDE REGEX "src/Python/")
endif()
```

And after the existing `target_include_directories(secretagent PRIVATE ${C_INC})`:

```cmake
if(SA_ENABLE_PYTHON)
  target_compile_definitions(secretagent PUBLIC SA_HAS_PYTHON=1)
  target_include_directories(secretagent PRIVATE ${CPYTHON_ROOT}/Include ${CPYTHON_BUILD})
  # PUBLIC: any host embedding the static library carries the CPython runtime
  # (pyrt is part of the library ABI while SA_HAS_PYTHON is defined).
  target_link_libraries(secretagent PUBLIC ${CPYTHON_BUILD}/libpython3.12.a)
  target_link_libraries(secretagent PUBLIC crypt dl m)
  target_link_libraries(secretagent PUBLIC pthread)
  add_dependencies(secretagent cpython-ext)
endif()
```

- [ ] **Step 5: Build the dependency green**

```bash
cmake -S . -B cmake-build-debug -DSA_BUILD_TESTS=OFF && cmake --build cmake-build-debug -j
```
Expected: cpython-ext builds (minutes — do not interrupt). `cmake-build-debug/deps/cpython/libpython3.12.a` + generated `pyconfig.h` exist; the library still builds.

- [ ] **Step 6: Commit**

```bash
git add .gitmodules CMakeLists.txt deps/cpython src/Platform/platform_process.h src/Platform/platform_process.c src/Platform/platform.h
git commit -m "build: add CPython 3.12.13 submodule, ExternalProject wiring, and platform_process extraction"
```

---

### Task 2: Message types and payloads — `src/Python/pyrt_messages.h`

**Files:**
- Create: `src/Python/pyrt_messages.h`

- [ ] **Step 1: Write the header (full content)**

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_PYRT_MESSAGES_H
#define SA_PYRT_MESSAGES_H

#include <stdint.h>

/* Module-defined message types for the Python execution layer, carried in
   the generic message_t envelope (uint32_t type). Messages are transient
   control; only effects (WaveDB, later slice) persist. */
typedef enum pyrt_message_type_e {
  PYRT_EXECUTE = 0,   /* actor -> pyrt thread: run this code cell */
  PYRT_RESULT,        /* pyrt thread -> owning actor: cell finished */
  PYRT_LOG,           /* python -> owning actor: live narration */
  PYRT_STATUS,        /* python -> owning actor: current status */
  PYRT_EMIT,          /* python -> owning actor: durable-payload candidate */
  PYRT_INTERRUPT      /* actor -> pyrt backend: stop the running cell */
} pyrt_message_type_e;

/* EXECUTE. Ownership of `code` transfers with the message. `corr` is the
   request correlation id echoed on the RESULT. */
typedef struct pyrt_execute_payload_t {
  uint64_t corr;
  char* code;
} pyrt_execute_payload_t;

void pyrt_execute_payload_destroy(void* payload);

/* RESULT. status 0 = ok (text = repr of the trailing expression; empty when
   None); status 1 = error (text = traceback / exit text). One per EXECUTE. */
typedef struct pyrt_result_payload_t {
  uint64_t corr;
  uint8_t status;
  char* text;
} pyrt_result_payload_t;

void pyrt_result_payload_destroy(void* payload);

/* LOG / STATUS / EMIT. Ownership of `text` transfers with the message. */
typedef struct pyrt_text_payload_t {
  char* text;
} pyrt_text_payload_t;

void pyrt_text_payload_destroy(void* payload);

#endif // SA_PYRT_MESSAGES_H
```

- [ ] **Step 2: Build green** — `cmake --build cmake-build-debug -j`

- [ ] **Step 3: Commit**

```bash
git add src/Python/pyrt_messages.h
git commit -m "feat: add Python actor message types and payloads"
```

---

### Task 3: pyrt API header + test harness + red test

**Files:**
- Create: `src/Python/pyrt.h` (full, final)
- Create: `test/test_pyrt.cpp` (full content below; later tasks APPEND, never rewrite)
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write `src/Python/pyrt.h` (full, final — this is the frozen API)**

```c
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
```

- [ ] **Step 2: Create `test/test_pyrt.cpp` (full content; the harness is shared by every later test; amended: py_frame_free tears the mailbox down — see commit for the fix rationale)**

```cpp
//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <atomic>
#include <string>
#include <vector>
extern "C" {
#include "../src/Actor/actor.h"
#include "../src/Util/atomic_compat.h"
#include "../src/Util/allocator.h"
#include "../src/Platform/platform.h"
}
#ifdef SA_HAS_PYTHON
extern "C" {
#include "../src/Python/pyrt.h"
#include "../src/Python/pyrt_messages.h"
}
#endif

#ifdef SA_HAS_PYTHON

/* Test frame: embeds an actor (first member, per the style guide) and owns
   one pyrt runtime. Result payloads are transferred OUT of their messages
   (msg->payload = NULL) so actor_run never frees them; the frame frees them
   itself in py_frame_free (good-actors ownership rules). */
typedef struct py_frame_t {
  actor_t actor;
  pyrt_t* pyrt;
  std::vector<pyrt_result_payload_t*> results;
  std::vector<std::string> logs;
  std::vector<std::string> statuses;
  ATOMIC(uint8_t) got_result;
} py_frame_t;

static void py_frame_dispatch(void* state, message_t* msg) {
  py_frame_t* self = (py_frame_t*)state;
  switch (msg->type) {
    case PYRT_RESULT: {
      pyrt_result_payload_t* r = (pyrt_result_payload_t*)msg->payload;
      msg->payload = NULL;
      self->results.push_back(r);
      ATOMIC_STORE(&self->got_result, 1);
      break;
    }
    case PYRT_LOG: {
      pyrt_text_payload_t* t = (pyrt_text_payload_t*)msg->payload;
      if (t != NULL && t->text != NULL) {
        self->logs.push_back(t->text);
      }
      break;   /* payload freed by actor_run via payload_destroy */
    }
    case PYRT_STATUS: {
      pyrt_text_payload_t* t = (pyrt_text_payload_t*)msg->payload;
      if (t != NULL && t->text != NULL) {
        self->statuses.push_back(t->text);
      }
      break;
    }
    default:
      break;
  }
}

static py_frame_t* py_frame_create(void) {
  py_frame_t* self = (py_frame_t*)get_clear_memory(sizeof(py_frame_t));
  actor_init(&self->actor, self, py_frame_dispatch, NULL); /* NULL pool: pumped by hand */
  ATOMIC_STORE(&self->got_result, 0);
  return self;
}

static void py_frame_execute(py_frame_t* self, const char* code) {
  if (self->pyrt == NULL) {
    pyrt_config_t cfg;
    cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
    cfg.pool_cap = 0;
    cfg.idle_evict_ms = 0;
    self->pyrt = pyrt_create(&self->actor, &cfg);
  }
  pyrt_execute(self->pyrt, strdup(code));
}

static void py_frame_pump(py_frame_t* self, int timeout_ms) {
  int waited = 0;
  while (ATOMIC_LOAD(&self->got_result) == 0 && waited < timeout_ms) {
    actor_run(&self->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
    waited++;
  }
  actor_run(&self->actor, ACTOR_BATCH_SIZE);
}

static void py_frame_free(py_frame_t* self) {
  if (self->pyrt != NULL) {
    pyrt_destroy(self->pyrt); /* join first: the mailbox must stop accepting sends */
  }
  /* Drain the mailbox: actor_destroy runs message_queue_destroy, which frees */
  /* any queued-but-undispatched payloads plus the queue's sentinel. */
  actor_destroy(&self->actor);
  for (auto* r : self->results) {
    pyrt_result_payload_destroy(r);
  }
  free(self);
}

#endif /* SA_HAS_PYTHON */

#ifndef SA_HAS_PYTHON
/* Keeps the file a valid non-empty test unit in libpython-free builds. */
TEST(TestPyrt, TestCompiledWithoutPython) { SUCCEED(); }
#endif

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
```

- [ ] **Step 3: Add to `test/CMakeLists.txt` after the `add_executable(testsecretagent ...)` block:**

```cmake
    if(SA_HAS_PYTHON)
      target_sources(testsecretagent PRIVATE test_pyrt.cpp)
    endif()
```

- [ ] **Step 4: Run the build to see it fail**

```bash
cmake -S . -B cmake-build-debug -DSA_BUILD_TESTS=ON && cmake --build cmake-build-debug -j
```
Expected: FAIL — `undefined reference to pyrt_create` / `pyrt_execute` / `pyrt_destroy`.

- [ ] **Step 5: Commit the red state**

```bash
git add src/Python/pyrt.h test/test_pyrt.cpp test/CMakeLists.txt
git commit -m "test: add pyrt test harness and first failing execution contract"
```

---

### Task 4: pyrt implementation (write `src/Python/pyrt.c` to this contract)

**Files:**
- Create: `src/Python/pyrt.c`

**The contract below is complete. Every symbol, signature, semantic, and invariant the implementer needs is here; where the language of good-actors applies it is cited. If a pinned-header fact contradicts the contract, the header wins and the contract text is amended in the commit.**

- [ ] **Step 1: Write `src/Python/pyrt.c` implementing all of the following (verify each by the listed acceptance gate: `cmake --build cmake-build-debug -j`, zero warnings)**

Private declarations (`_`-prefix per style; C11 atomics used directly, per the liboffs `actor.c`/`pool.c` idiom — no new macros invented):

| Symbol | Signature | Semantics |
|---|---|---|
| `_tls_pyrt` | `static _Thread_local pyrt_t*` | the pyrt thread's owning runtime; set at thread start, NULLed at exit; read by all outbound callbacks |
| `_pyrt_post_text(py, type, text)` | `static void (pyrt_t*, uint32_t, const char*)` | `strdup(text)` into a `pyrt_text_payload_t` (`payload_destroy = pyrt_text_payload_destroy`), post via `actor_send(py->owner, &msg)`; no-op when owner NULL. **Non-blocking; may not take any lock other than what actor_send needs; never re-enters Python** |
| `_py_actor_log/_status/_emit` | `static PyObject* (PyObject*, PyObject*)` — `METH_VARARGS` | parse one string, `_pyrt_post_text(..., PYRT_LOG/PYRT_STATUS/PYRT_EMIT, ...)` (emit receives its `text` argument; no structured payload in this milestone), `Py_RETURN_NONE` |
| `_py_actor_methods` | `static PyMethodDef[]` | keys `log`, `status`, `emit` |
| `_py_actor_moduledef` | `static struct PyModuleDef` | name `"actor"`, `-1` per-interpreter state |
| `_py_actor_create(void)` | `static PyObject* (void)` | `PyModule_Create(&_py_actor_moduledef)` |
| `_pyrt_global_init(void)` | `static uint8_t (void)` | idempotent process-wide boot: benign-race mutex install (`_Atomic(platform_mutex_t*)` + `atomic_compare_exchange_strong`, loser destroys its own mutex — liboffs `pool.c` idiom); `PyImport_AppendInittab("actor", _py_actor_create)` **before any Py_Initialize**; `PyConfig_InitPythonConfig`, `config.install_signal_handlers = 0`, `Py_InitializeFromConfig`, `PyConfig_Clear`; then **`PyEval_SaveThread()` on the init thread; `_pyrt_global_ready = 1`** |
| `_pyrt_pool_init(cap)` | `static void (size_t)` | idempotent; `_pyrt_cap = cap ? cap : platform_core_count() * 2` (first created runtime wins; later calls no-op); creates `_pyrt_slots_lock` (mutex) + `_pyrt_slots_cond` (condvar); `_pyrt_live = 0` |
| `_pyrt_slot_acquire(void)` | `static void (void)` | loop under `_pyrt_slots_lock` while `_pyrt_live >= _pyrt_cap`: `platform_condvar_wait`; then `_pyrt_live += 1` + broadcast. **Callers must hold no other lock** |
| `_pyrt_slot_release(void)` | `static void (void)` | under slot lock: `_pyrt_live -= 1` (floor 0) + broadcast |
| `_PYRT_HELPERS` | `static const char[]` | exactly the Python source below |

`_PYRT_HELPERS` (exact string; it runs once per subinterpreter with `PyRun_String(..., Py_file_input, main_dict, main_dict)`):

```python
def __sa_exec_cell(code):
    import traceback
    try:
        try:
            result = eval(compile(code, '<cell>', 'eval'), globals())
        except SyntaxError:
            exec(compile(code, '<cell>', 'exec'), globals())
            result = None
        if result is not None:
            globals()['_'] = result
            return 0, repr(result)
        return 0, ''
    except BaseException:
        return 1, traceback.format_exc()
```

`pyrt_t` (private struct): `actor_t* owner; pyrt_backend_e backend; platform_mutex_t* lock; platform_condvar_t* condition; pyrt_work_node_t* head; pyrt_work_node_t* tail; uint8_t shutdown; ATOMIC(uint8_t) active; ATOMIC(uint8_t) interrupt_req; ATOMIC(uint64_t) corr_counter; platform_thread_t* thread; unsigned long thread_id; PyThreadState* tstate; PyObject* main_dict; unsigned idle_evict_ms;` where `pyrt_work_node_t` is `{ pyrt_execute_payload_t* exec; pyrt_work_node_t* next; }`.

Subinterpreter backend (pyrt thread only):

| Symbol | Signature | Semantics |
|---|---|---|
| `_pyrt_interp_boot(py)` | `static uint8_t (pyrt_t*)` | `PyInterpreterConfig` zeroed, `use_main_obmalloc = 0`, `check_multi_interp_extensions = 0`, `gil = PyInterpreterConfig_OWN_GIL`; `PyStatus st = Py_NewInterpreterFromConfig(&tstate, &config)` — abort on `PyStatus_Exception(st)`; on success store `py->tstate` (installed as current on this thread), capture `py->main_dict = PyModule_GetDict(PyImport_AddModule("__main__"))` (borrowed), run `_PYRT_HELPERS` with `PyRun_String(..., Py_file_input, py->main_dict, py->main_dict)` (`Py_DECREF(rc)` on success; on failure `PyErr_Clear`, `Py_EndInterpreter`, clear fields, return 0); finally `py->active = 1` |
| `_pyrt_interp_teardown(py)` | `static void (pyrt_t*)` | on the pyrt thread with its tstate current: `Py_EndInterpreter(py->tstate)`; `py->tstate = NULL; py->main_dict = NULL; py->active = 0;` `_pyrt_slot_release()` |
| `_pyrt_run_cell_sub(py, code, &text)` | `static uint8_t` | look up `__sa_exec_cell` in `py->main_dict` (missing → `PyErr_Clear(); *text = strdup("pyrt: helpers missing"); return 1`); `PyObject_CallFunction(fn, "s", code)` (NULL → `PyErr_Clear`; text = "pyrt: cell failed to run"; return 1); parse the returned tuple `PyArg_ParseTuple(result, "is", &status, &text)` (failure → "pyrt: bad cell result"); `Py_DECREF(result)`; `*text = strdup(text or "")`; return `status != 0` |
| `_pyrt_post_result(py, corr, status, text)` | `static void` | owner NULL → `free(text); return`; else wrap in `pyrt_result_payload_t` (fields corr/status/text, `payload_destroy = pyrt_result_payload_destroy`) and `actor_send`. `text` ownership transfers; a NULL text means "nothing happened" (never routed) |

The pyrt thread (`static void* _pyrt_thread(void* arg)`), in order:

1. `_tls_pyrt = py`; `py->thread_id = (unsigned long)platform_thread_self()`.
2. If `!_pyrt_global_init()` → return 0 (owner still learns via its own timeout; boot failures never strand an execute silently).
3. If `py->backend == SA_PYRT_BACKEND_SUBPROCESS` → `ATOMIC_STORE(&py->active, 1)` (thread is the resource; cells are per-call).
4. Loop:
   - Lock `py->lock`; while `head == NULL && !shutdown && !(idle_evict_ms && interp_live)` → `platform_condvar_wait(py->condition, py->lock)`; with eviction armed (`idle_evict_ms != 0 && interp_live`), wait with `platform_condvar_timed_wait(py->condition, py->lock, py->idle_evict_ms)` and on timeout, if still no work and not shutting down: unlock, `_pyrt_interp_teardown(py)`, `interp_live = 0`, relock, continue.
   - `shutdown && head == NULL` → unlock, break (loop tail tears the backend down).
   - Pop head (`head/tail` maintained under the lock); read `shutdown` into `had_shutdown`; **unlock before ANY dispatch work** (slot acquire, GIL acquire, fork, subprocess spawn must never hold `py->lock` — interrupt() and destroy() take that lock).
   - Subprocess backend → the Task 8 dispatch (below).
   - Subinterpreter backend: if `interrupt_req` set and no interpreter live → clear the flag, destroy the EXEC payload, continue (stale interrupt for a finished cell). If `!interp_live`: `_pyrt_slot_acquire()` (may block; nothing held), then `_pyrt_interp_boot(py)` (failure → post RESULT with corr and text "pyrt: subinterpreter boot failed", destroy exec, continue), set `interp_live = 1`.
   - Run: `_pyrt_run_cell_sub(py, exec->code, &text)` → `ATOMIC_STORE(&py->interrupt_req, 0)` → `_pyrt_post_result(py, exec->corr, status, text)` → `pyrt_execute_payload_destroy(exec)` → if `had_shutdown` break.
5. Tail (before returning): if `interp_live` → `_pyrt_interp_teardown(py)`; `_tls_pyrt = NULL`; return 0.

Public API (frozen signatures from `pyrt.h`):

| Function | Body contract |
|---|---|
| `pyrt_create(owner, cfg)` | `_pyrt_global_init()` (NULL on failure); `_pyrt_pool_init(cfg ? cfg->pool_cap : 0)`; allocate `pyrt_t` with `get_clear_memory`; store owner/backend (`SUBINTERPRETER` if cfg NULL)/`idle_evict_ms`; create mutex+condvar; zero the atomics/head/tail/shutdown; return. Thread is NOT created here (lazy boot) |
| `pyrt_execute(py, code)` | NULL-arg → free(code) if any, return 0. Build `pyrt_execute_payload_t` (clear-alloc): `code = code` (ownership), `corr = ATOMIC_FETCH_ADD(&py->corr_counter, 1) + 1`. If `py->thread == NULL`: `platform_thread_create(_pyrt_thread, py)` (failure → destroy exec, return 0), store thread. Queue the exec under `py->lock` (tail append), broadcast, unlock, return `exec->corr` |
| `pyrt_interrupt(py)` | NULL-safe. `ATOMIC_STORE(&py->interrupt_req, 1)`; forced delivery appended by Task 6 |
| `pyrt_isactive(py)` | NULL-safe `ATOMIC_LOAD(&py->active)` cast to uint8_t |
| `pyrt_destroy(py)` | NULL-safe. Under `py->lock`: `shutdown = 1`, broadcast, unlock; `platform_thread_join(py->thread)` if non-NULL; destroy mutex+condvar; `free(py)`. (The thread tears down its backend on exit; destroy does NOT touch Python state from this thread) |

- [ ] **Step 2: Add the first contract test (append before the closing `#endif /* SA_HAS_PYTHON */` of the harness block)**

```cpp
TEST(TestPyrt, TestExecuteReturnsCorrMatchedResult) {
  py_frame_t* self = py_frame_create();

  py_frame_execute(self, "21 * 2");
  py_frame_pump(self, 10000);

  ASSERT_LT(self->results.size(), 2u);
  ASSERT_EQ(self->results.size(), 1u);
  pyrt_result_payload_t* r = self->results[0];
  EXPECT_EQ(r->status, 0);
  EXPECT_EQ(r->corr, 1u);
  EXPECT_STREQ(r->text, "42");

  py_frame_free(self);
}
```

- [ ] **Step 3: Run the suite green**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
```
Expected: 100% (58 prior + 1 new). If stdlib discovery fails at first subinterpreter boot, check `env | grep PYTHON` and CPython's stdlib-relative lookup; fix at the root cause, never in the test.

- [ ] **Step 4: Valgrind + Commit**

```bash
valgrind --leak-check=full --error-exitcode=1 cmake-build-debug/test/testsecretagent --gtest_filter='TestPyrt.*' 2>&1 | tail -8
```
Expected: `ERROR SUMMARY: 0 errors`; CPython-owned "still reachable" globals are the documented acceptable class (style guide §6 note). Then:

```bash
git add src/Python/pyrt.c test/test_pyrt.cpp
git commit -m "feat: pyrt runtime - nonblocking subinterpreter execution with corr-matched results"
```

---

### Task 5: Streaming + namespace persistence (append tests)

**Files:**
- Test: `test/test_pyrt.cpp`

- [ ] **Step 1: Append both tests before the closing `#endif /* SA_HAS_PYTHON */`**

```cpp
TEST(TestPyrt, TestNamespacePersistsAcrossCells) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "x = 41");
  py_frame_pump(self, 10000);
  ATOMIC_STORE(&self->got_result, 0);
  py_frame_execute(self, "x + 1");
  py_frame_pump(self, 10000);

  ASSERT_LT(self->results.size(), 3u);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->status, 0);
  EXPECT_STREQ(self->results[1]->text, "42");

  py_frame_free(self);
}

TEST(TestPyrt, TestLogStreamsBeforeResult) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "for i in range(3):\n    import actor\n    actor.log('tick %d' % i)\n1 + 1");

  /* Stream check: narration visible while the cell is still running. */
  int waited = 0;
  while (ATOMIC_LOAD(&self->got_result) == 0 && waited < 2000) {
    actor_run(&self->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
    waited++;
    if (waited > 100 && self->logs.size() > 0) {
      break;
    }
  }
  EXPECT_GT(self->logs.size(), 0u);
  EXPECT_EQ(ATOMIC_LOAD(&self->got_result), 0);   /* streaming, not buffered */

  py_frame_pump(self, 10000);
  ASSERT_LT(self->results.size(), 2u);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  EXPECT_EQ(self->logs.size(), 3u);
  EXPECT_STREQ(self->logs[0].c_str(), "tick 0");

  py_frame_free(self);
}
```

- [ ] **Step 2: Run + Commit**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
```
Expected: 100% (60). If `import actor` fails inside a subinterpreter, the `PyImport_AppendInittab` placement (before any `Py_Initialize`) is wrong — fix there, never in the test.

```bash
git add test/test_pyrt.cpp
git commit -m "test: namespace persistence and live log streaming across cells"
```

---

### Task 6: Interrupt path (mechanism from Task 1 Step 3 header verification)

**Files:**
- Modify: `src/Python/pyrt.c` (`pyrt_interrupt` body), `test/test_pyrt.cpp`

- [ ] **Step 1: Decide FORCED vs COOPERATIVE from Task 1's verification, and implement exactly one:**

**FORCED** is available if the pinned headers expose `PyThreadState_SetAsyncExc` and the verification probe (below) shows it raises into the cell. Implementation (no GIL games from the interrupter — the pyrt thread is the only thread that may touch its subinterpreter's state, so the interrupt is *requested* via a work node that the pyrt thread processes between byte-code batches only if CPython permits):

```python
# in a probe cell (NOT committed) run once during development:
#   long-running loop + interrupt -> observe whether KeyboardInterrupt lands
```

If FORCED is confirmed, `pyrt_interrupt` posts an INTERRUPT work node; the pyrt thread's cell wrapper checks the flag; and because a C-level cell run cannot be preempted safely from outside, **the only spec-clean forced mechanism available on 3.12.13 subinterpreters is the cooperative one unless the Task 1 grep shows a cross-thread entry point; otherwise:**

```c
void pyrt_interrupt(pyrt_t* py) {
  if (py == NULL) {
    return;
  }
  ATOMIC_STORE(&py->interrupt_req, 1);   /* cooperative: honored at cell boundary */
}
```
with the thread loop addition: at the TOP of each cell iteration, if `interrupt_req` is set, the queued cell is answered as a failed cell (strdup'd text "pyrt: interrupted at boundary") and the flag cleared. A long-running cell that never yields is documented (spec §Interrupts fallback) as not pre-emptible in this milestone — escalated to the owner if a forced path is found later. **If, and only if, Task 1's grep output showed a per-thread mechanism that plausibly works (e.g. `PyErr_SetInterruptEx` semantics documented as per-interpreter in the pinned headers), implement that path INSTEAD and keep the test below asserting only the behavior it verifies.**

- [ ] **Step 2: Append the test (two variants; ship exactly ONE, per the Step 1 choice):**

FORCED variant:

```cpp
TEST(TestPyrt, TestInterruptStopsCellAndKeepsRuntimeAlive) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "sum(range(10**8))");
  platform_sleep_ms(200);
  pyrt_interrupt(self->pyrt);
  py_frame_pump(self, 5000);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 1);
  EXPECT_NE(strstr(self->results[0]->text, "KeyboardInterrupt"), nullptr);

  ATOMIC_STORE(&self->got_result, 0);
  py_frame_execute(self, "2 + 2");
  py_frame_pump(self, 10000);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->status, 0);
  EXPECT_STREQ(self->results[1]->text, "4");

  py_frame_free(self);
}
```

COOPERATIVE variant (replaces it verbatim if Step 1 chose cooperative-only):

```cpp
TEST(TestPyrt, TestInterruptHonoredAtCellBoundary) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "1 + 1");
  pyrt_interrupt(self->pyrt);          /* arrives before/while the cell runs */
  py_frame_pump(self, 10000);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);   /* fast cell finished normally */
  EXPECT_STREQ(self->results[0]->text, "2");

  /* A long cell cannot be preempted (documented spec fallback): interrupt()
     only sets the flag, and the flag is cleared when the next cell starts
     without an interrupt. No hang, no double result. */
  ATOMIC_STORE(&self->got_result, 0);
  py_frame_execute(self, "2 + 3");
  py_frame_pump(self, 10000);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->text, "5");

  py_frame_free(self);
}
```

- [ ] **Step 3: Run + Commit**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
git add src/Python/pyrt.c test/test_pyrt.cpp
git commit -m "feat: pyrt interrupt path per pinned CPython 3.12 verification"
```

---

### Task 7: Error cells (append test only)

**Files:**
- Test: `test/test_pyrt.cpp`

- [ ] **Step 1: Append the test**

```cpp
TEST(TestPyrt, TestFailingCellReturnsTracebackNotCrash) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "1 / 0");
  py_frame_pump(self, 10000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 1);
  EXPECT_NE(strstr(self->results[0]->text, "ZeroDivisionError"), nullptr);

  py_frame_execute(self, "40 + 2");   /* interpreter survived the exception */
  py_frame_pump(self, 10000);
  ASSERT_LT(self->results.size(), 3u);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->status, 0);
  EXPECT_STREQ(self->results[1]->text, "42");

  py_frame_free(self);
}
```

- [ ] **Step 2: Run + Commit**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
git add test/test_pyrt.cpp
git commit -m "test: cell errors return traceback results, interpreter survives"
```

---

### Task 8: Lazy boot + idle eviction + pool cap (config already plumbed in Task 4 — append tests only)

**Files:**
- Test: `test/test_pyrt.cpp`

- [ ] **Step 1: Append the tests**

```cpp
TEST(TestPyrt, TestLazyBootNoInterpreterBeforeFirstExecute) {
  py_frame_t* self = py_frame_create();
  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
  cfg.pool_cap = 0;
  cfg.idle_evict_ms = 0;
  self->pyrt = pyrt_create(&self->actor, &cfg);
  EXPECT_EQ(pyrt_isactive(self->pyrt), 0);   /* nothing booted yet */

  py_frame_execute(self, "1 + 1");
  int waited = 0;
  while (ATOMIC_LOAD(&self->got_result) == 0 && waited < 10000) {
    actor_run(&self->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
    waited++;
    if (waited > 200) {
      EXPECT_EQ(pyrt_isactive(self->pyrt), 1);   /* live well before result */
    }
  }
  EXPECT_EQ(pyrt_isactive(self->pyrt), 1);   /* stays live after the cell */
  py_frame_free(self);
}

TEST(TestPyrt, TestIdleEvictionTearsInterpreterDownAndDiscontinuesNamespace) {
  py_frame_t* self = py_frame_create();
  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
  cfg.pool_cap = 0;
  cfg.idle_evict_ms = 80;
  self->pyrt = pyrt_create(&self->actor, &cfg);
  pyrt_execute(self->pyrt, strdup("a1 = 7"));
  py_frame_pump(self, 10000);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);

  int waited = 0;
  while (pyrt_isactive(self->pyrt) != 0 && waited < 3000) {
    actor_run(&self->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(5);
    waited++;
  }
  EXPECT_EQ(pyrt_isactive(self->pyrt), 0);   /* evicted after the idle timeout */

  /* Eviction drops the namespace BY DESIGN (opt-in memory reclamation; frames
     needing continuity must not arm idle_evict_ms — documented in the spec). */
  pyrt_execute(self->pyrt, strdup("a1 == 7"));
  py_frame_pump(self, 10000);
  ASSERT_LT(self->results.size(), 3u);
  ASSERT_EQ(self->results.size(), 2u);
  EXPECT_EQ(self->results[1]->status, 1);   /* NameError: fresh namespace */

  py_frame_free(self);
}

TEST(TestPyrt, TestPoolCapQueuesExecutesAndDrains) {
  py_frame_t* a = py_frame_create();
  py_frame_t* b = py_frame_create();

  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
  cfg.pool_cap = 1;               /* smallest pool: the FIRST create fixes it */
  cfg.idle_evict_ms = 0;
  a->pyrt = pyrt_create(&a->actor, &cfg);
  b->pyrt = pyrt_create(&b->actor, &cfg);

  pyrt_execute(a->pyrt, strdup("sum(range(10**7))"));   /* holds the only slot */
  pyrt_execute(b->pyrt, strdup("7 * 6"));               /* queues, never fails */
  int waited = 0;
  while (ATOMIC_LOAD(&b->got_result) == 0 && waited < 500) {
    actor_run(&a->actor, ACTOR_BATCH_SIZE);
    actor_run(&b->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(1);
    waited++;
  }
  EXPECT_EQ(ATOMIC_LOAD(&b->got_result), 0);   /* backpressure: b still queued */

  py_frame_free(a);                            /* releases the slot */
  waited = 0;
  while (ATOMIC_LOAD(&b->got_result) == 0 && waited < 30000) {
    actor_run(&b->actor, ACTOR_BATCH_SIZE);
    platform_sleep_ms(5);
    waited++;
  }
  ASSERT_LT(b->results.size(), 2u);
  ASSERT_EQ(b->results.size(), 1u);
  EXPECT_EQ(b->results[0]->status, 0);
  EXPECT_STREQ(b->results[0]->text, "42");

  py_frame_free(b);
}
```

- [ ] **Step 2: Run + Commit**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
```
Expected: 100% (63). Note: the pool cap is fixed by the FIRST runtime created in the process; if a differently-configured pyrt test ran first in another suite within the same binary, run this suite test-local (`./testsecretagent --gtest_filter='TestPyrt.TestPoolCap*'`) to confirm the cap semantics; if the pool-cap test is inherently order-fragile, refactor it into its own gtest binary in `test/CMakeLists.txt` (`add_executable(test_pyrt_cap test_pyrt.cpp)` with a main) and state so in the commit — never weaken the assert.

```bash
git add test/test_pyrt.cpp
git commit -m "test: lazy boot, idle eviction, and pool-cap backpressure contracts"
```

---

### Task 9: Subprocess backend (`src/Python/py_subprocess.{h,c}`)

**Files:**
- Create: `src/Python/py_subprocess.h` (full listing), `src/Python/py_subprocess.c` (to contract)
- Modify: `src/Python/pyrt.c` (two exact edits below)
- Test: `test/test_pyrt.cpp`
- (No CMake change: the root GLOB compiles it.)

- [ ] **Step 1: Write `src/Python/py_subprocess.h` (full content)**

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_PY_SUBPROCESS_H
#define SA_PY_SUBPROCESS_H

#include <stdint.h>

#ifdef SA_HAS_PYTHON
/* Runs one stateless cell in a `python3 -I -c` subprocess. Returns exit
   status (0 ok) and fills *out_text (stdout) and *out_err (stderr or
   runtime error text); both strings are strdup'd and free()d by the caller.
   Documented limitation: no live namespace across subprocess cells;
   POSIX-only in this milestone (Windows spawn ships with the PCBuild
   completion task for the slice). */
uint8_t pyrt_run_cell_subprocess(const char* code, char** out_text, char** out_err);
#endif /* SA_HAS_PYTHON */

#endif // SA_PY_SUBPROCESS_H
```

- [ ] **Step 2: Write `src/Python/py_subprocess.c` implementing this contract (acceptance gate: compile clean + valgrind 0 errors + diff read for fd discipline)**

- POSIX only: `pipe` → `fork` → child (`close` read ends, `dup2` both write ends onto STDOUT/STDERR, close the originals, `execlp("python3", "python3", "-I", "-c", code, NULL)`, `_exit(127)` on exec failure) → parent closes both write ends immediately → drain both read ends to EOF under `select` (no timeout; on `EINTR` retry; on select error mark both EOF) → `waitpid` → result.
- Buffers: `get_memory` of 1 MiB + 1 per stream; stop reading at the cap (documented truncation); NUL-terminate both before use.
- Return `status = 0` iff the child exited 0 AND stderr is empty; stdout is the caller's result text (the subprocess cell has no trailing expression; its stdout IS the result per the envelope contract); on failure the RESULT text is the stderr text, or `"pyrt: subprocess exited nonzero"` when stderr is empty.
- fd discipline (hard): every descriptor opened is closed exactly once on every path — including the child's own copies before exec — and `waitpid` runs before any return; no mid-setup failure leaks a pipe.
- This file never touches actors or messages: it returns strings; `pyrt.c` routes.

- [ ] **Step 3: Wire `src/Python/pyrt.c` (two exact edits):**

1. After the `#include "pyrt.h"` line, add:

```c
#include "py_subprocess.h"
```

2. In `_pyrt_thread`'s dispatch, replace the subprocess placeholder (the `free(exec); continue;` lines Task 4 specified at that backend branch) with:

```c
    if (py->backend == SA_PYRT_BACKEND_SUBPROCESS) {
      /* Stateless per cell: the slot is acquired for the spawn only, so the
         pool cap bounds concurrent subprocesses exactly like interpreters.
         We are already outside py->lock here (unlocked before dispatch). */
      _pyrt_slot_acquire();
      char* proc_text = NULL;
      char* proc_err = NULL;
      uint8_t status = pyrt_run_cell_subprocess(exec->code, &proc_text, &proc_err);
      if (proc_err != NULL) {
        _pyrt_post_text(py, PYRT_STATUS, proc_err);
        free(proc_err);
      }
      _pyrt_post_result(py, exec->corr, status, proc_text);
      _pyrt_slot_release();
      pyrt_execute_payload_destroy(exec);
      continue;
    }
```

- [ ] **Step 4: Append the backend test**

```cpp
TEST(TestPyrt, TestSubprocessBackendSameContract) {
  py_frame_t* self = py_frame_create();
  pyrt_config_t cfg;
  cfg.backend = SA_PYRT_BACKEND_SUBPROCESS;
  cfg.pool_cap = 0;
  cfg.idle_evict_ms = 0;
  self->pyrt = pyrt_create(&self->actor, &cfg);
  pyrt_execute(self->pyrt, strdup("print('hello subprocess')"));
  py_frame_pump(self, 20000);

  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  EXPECT_NE(strstr(self->results[0]->text, "hello subprocess"), nullptr);

  py_frame_free(self);
}
```

- [ ] **Step 5: Run + Commit**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
valgrind --leak-check=full --error-exitcode=1 cmake-build-debug/test/testsecretagent --gtest_filter='TestPyrt.*' 2>&1 | tail -6
```
Expected: 100% pass; valgrind 0 errors (fd discipline proof). Then:

```bash
git add src/Python/py_subprocess.h src/Python/py_subprocess.c src/Python/pyrt.c test/test_pyrt.cpp
git commit -m "feat: subprocess fallback backend with the same EXECUTE/RESULT contract"
```

---

### Task 10: Full-suite verification + Atlas slice progress

**Files:**
- Modify: `atlas/workflow.json`

- [ ] **Step 1: Full verification**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
valgrind --leak-check=full --error-exitcode=1 cmake-build-debug/test/testsecretagent 2>&1 | tail -8
cmake -S . -B /tmp/sa-pyrt-asan -DSA_BUILD_TESTS=ON -DSA_ENABLE_ASAN=ON && cmake --build /tmp/sa-pyrt-asan -j && setarch -R ctest --test-dir /tmp/sa-pyrt-asan --output-on-failure 2>/dev/null | tail -3 && rm -rf /tmp/sa-pyrt-asan
```
Expected: 100% pass in both configs; valgrind clean of errors; no new warnings. Also verify the OFF configuration still builds libpython-free:

```bash
cmake -S . -B /tmp/sa-pyrt-off -DSA_ENABLE_PYTHON=OFF -DSA_BUILD_TESTS=ON && cmake --build /tmp/sa-pyrt-off -j && setarch -R ctest --test-dir /tmp/sa-pyrt-off 2>/dev/null | tail -3 && rm -rf /tmp/sa-pyrt-off
```

- [ ] **Step 2: Mark the Atlas slice in-progress (evidence attached)**

In `atlas/workflow.json`, set `nodes.execute-agent-code-without-blocking.status` to `"in-progress"` and append to its `description`:

```
 MILESTONE (pyrt layer delivered): non-blocking subinterpreter EXECUTE with live
 log/status streaming, corr-matched RESULT, lazy boot, pool cap, idle eviction,
 interrupt path (mechanism per pinned-3.12 verification), subprocess fallback.
 Remaining for slice completion: Windows build (PCBuild ExternalProject branch),
 streaming-per-cell attribution gaps, then review records per lifecycle.
```
The Atlas watcher rebuilds `atlas.html`; if it is not running, run `node build.js` from `atlas/`.

- [ ] **Step 3: Commit**

```bash
git add atlas/workflow.json
git commit -m "chore(atlas): mark execute-code slice in-progress with pyrt milestone evidence"
```

---

## Acceptance criteria (whole plan)

1. Builds green in all three configurations: `SA_ENABLE_PYTHON=ON` (library + tests), `SA_ENABLE_PYTHON=OFF` (libpython-free), and `SA_ENABLE_ASAN=ON` under `setarch -R`.
2. Full ctest 100% (58 existing + TestPyrt suites); valgrind clean of errors in pyrt-owned code (CPython/pool "still reachable" globals are the documented acceptable class); no TODOs.
3. Spec completion evidence demonstrably holds: live streaming before RESULT, corr-matched RESULT, lazy boot, swappable backends, pool-cap queue-then-drain, interrupt per the verified mechanism.
4. Atlas slice `execute-agent-code-without-blocking` in-progress with milestone evidence; every commit atomic and conventional.