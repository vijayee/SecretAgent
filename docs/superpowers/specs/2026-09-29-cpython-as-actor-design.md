# CPython as an Actor (pyrt) — Design

**Date:** 2026-09-29
**Status:** Approved (approach A)
**Atlas slice:** `execute-agent-code-without-blocking` (S002)
**Source rationale:** docs/design-answers.md §4-5; docs/chat.json messages 4204-4216

## Goal

An actor frame can execute Python inside the runtime without blocking: the actor
stays responsive (can still receive and act on messages) while a cell of Python
runs, output streams out live, and a result comes back corr-matched. The
interpreter is a bound tool — never the substrate.

## Dependency: CPython 3.12 from source

- `deps/cpython` = git submodule pinning tag `v3.12.13` of
  `https://github.com/python/cpython` (same dependency discipline as the
  googletest submodule; liboffs's ExternalProject pattern for source-built
  externals is the wiring style).
- Root CMake gains `option(SA_ENABLE_PYTHON "Build the Python execution layer" ON)`.
  When ON, an `ExternalProject_Add(cpython-ext, ...)` configures and builds
  CPython from the submodule:
  - Linux: `configure --prefix <build>/deps/cpython-install` (default static
    `libpython3.12.a`, no test modules, no ensurepip), build with
    `${CMAKE_COMMAND} --build`.
  - Windows: the build command switches to CPython's `PCBuild` path (same
    ExternalProject; per-platform command strings, liboffs external-dep style).
  - `BUILD_BYPRODUCTS` = the built `libpython3.12.a` (and the shared build on
    Windows).
- The `secretagent` library links `libpython3.12.a` and carries
  CPython's `Include/` plus the *generated* headers (`pyconfig.h`) as
  include dirs. `SA_ENABLE_PYTHON=OFF` excludes the whole feature (sources
  filtered out of the `GLOB_RECURSE` target) so embedders without Python
  requirements keep a libpython-free library.

## Components (src/Python/, liboffs module style)

- `src/Python/pyrt.h` / `pyrt.c` — the Python runtime for one frame:
  - `pyrt_t`: dedicated OS thread, its subinterpreter `PyThreadState*`,
    work mailbox, boot state (global init flag + per-actor state).
  - `pyrt_boot(actor_t* owner)` (lazy — called by the owner's behavior on the
    first EXECUTE), `pyrt_execute(pyrt_t*, uint64_t corr, char* code)`,
    `pyrt_interrupt(pyrt_t*)`, `pyrt_destroy(pyrt_t*)`.
  - Boot is lazy: a frame that never executes code never boots a subinterpreter
    or a thread (the #294 lesson: never pay per-frame interpreter cost for
    frames that don't compute).
- `src/Python/pyrt_messages.h` — this module's message enum and payloads
  (module-defined message types, per the style guide / good-actors conventions;
  `message_t` from src/Actor/message.h is the envelope):
  - `PYRT_EXECUTE` — payload `{uint64_t corr; char* code; size_t code_len;}`;
    ownership of `code` transfers with the message.
  - `PYRT_RESULT` — `{uint64_t corr; uint8_t status; char* text;}` where
    `text` is the trailing expression's repr (status 0) or an error traceback
    (status 1); one RESULT per EXECUTE.
  - `PYRT_LOG` / `PYRT_STATUS` — `{char* text;}` transferred with the message;
    ephemeral narration, routed/forwarded/dropped by behaviors.
  - `PYRT_EMIT` — future durable channel; in this milestone it posts the
    message and the owning actor's behavior decides what to do (the
    persistence slice wires it into WaveDB later).
  - All payloads set `payload_destroy = free` (or a custom destructor); a NULL
    payload_destroy is a leak, never acceptable.
- Outbound `actor` module (what Python code calls):
  - Registered once via `PyImport_AppendInittab("actor", ...)` **before** the
    first `Py_Initialize`, so it exists in the main interpreter and in every
    subinterpreter.
  - Methods `log(text)`, `status(text)`, `emit(kind, data)`, `report(value)`
    are C callbacks: they `strdup` their arguments out of the Python heap into
    C-owned strings, post the corresponding message to the owning actor's
    mailbox, and return immediately.
  - Callback rules (hard, from the good-actors conventions): non-blocking (may
    take the owning actor's inbox mutex only and return), never re-enter
    Python, never retain a borrowed `PyObject*` past the call. The owning
    actor is found via `_Thread_local` set by the pyrt thread at boot.

## Execution model (embedded subinterpreters, one per actor)

1. First interpreter boot anywhere in the process: `Py_InitializeFromConfig()`
   once (global init guard), then the booting thread releases the GIL
   (`PyEval_SaveThread`) so the main thread never blocks holding it.
2. Per-actor pyrt thread: on its first EXECUTE, creates its subinterpreter via
   `Py_NewInterpreterFromConfig` with `PyInterpreterConfig` (per-interpreter
   GIL / `PyInterpreterConfig_OWN_GIL`, `use_main_obmalloc = 0`,
   `check_multi_interp_extensions = 0`), swaps in its thread state, and holds
   that state for the frame's life. **These field names and constants are
   verified against the pinned 3.12.13 headers before anything is written** —
   the subinterpreter configuration API churned across 3.12/3.13
   (PEP 684 → 734) and memory is not a source of truth.
3. Thread loop: recv work from the mailbox → acquire the subinterpreter's
   thread state → `PyRun_String`-equivalent execution of the cell in the
   subinterpreter's `__main__` globals → capture the trailing `repr()` (or the
   error traceback text) → post `PYRT_RESULT{corr, status, text}` to the owner
   → repeat.
4. The subinterpreter's `__main__` namespace persists across cells — that live
   namespace is the frame's working memory ("thinking"); the durable side is
   WaveDB (later slices), not the interpreter. No `dill`-style namespace
   snapshotting in this layer.
5. Teardown: `pyrt_destroy` posts a shutdown, the thread runs
   `Py_EndInterpreter` for its subinterpreter and exits; the owner joins the
   thread. Never block the actor: EXECUTE is a handoff, RESULT is a message.

## Subprocess backend (the swap-in path)

Same module, same message contract: a backend enum on `pyrt_boot` selects
subinterpreter (primary) or subprocess (fallback — `python -c` cell over pipes,
no libpython at link time). The subprocess backend exists for (a) hosts that
built with `SA_ENABLE_PYTHON=OFF`-style constraints on other platforms, and
(b) cells importing C extensions that refuse to live in a subinterpreter. It is
stateless per call (no live namespace) — that is its documented limitation, not
a regression: the primary backend owns the "load once, slice across turns"
property.

## Capacity and lifecycle (scaling to subagent trees)

Actors are multiplexed — the scheduler pool's few worker threads never wait on
Python (EXECUTE is a handoff), so subagent trees cost nothing scheduler-side.
Python's cost is OS-level and lands exactly at first EXECUTE:

| Resource | Cost per live Python frame | Practical bound |
|---|---|---|
| Parked pyrt thread | ~8-16 KB kernel state, lazy stack | thousands |
| Subinterpreter after imports | ~1-5 MB+ (per-interpreter module state; non-multiphase C extensions are duplicated) | ~10^3-10^4 live frames ≈ memory |
| Concurrently executing cells | one core each (per-interpreter GIL) | more than cores → kernel timesharing |

Contained by lifecycle design:
1. **Lazy boot** — frames that never run code never thread up; a tree of
   routing/read-only subagents pays nothing.
2. **Interpreter pool cap** — a semaphore at boot holds only K interpreters
   live at once (K configurable, default ~2× cores). An actor whose boot would
   exceed K has its EXECUTE queued, not dropped: the actor stays hot and the
   work drains as interpreters free up. This is backpressure, the same
   discipline as the mailbox mute threshold, and it turns the fan-out storm
   into throttling instead of exhaustion.
3. **Eviction (opt-in knob, default generous)** — an interpreter idle beyond a
   timeout is torn down and re-booted lazily on next use. Same semantics as
   lazy boot; it is the memory GC knob for long-lived frames, off by default.
4. **Explicit teardown** — subinterpreter + thread die with the frame
   (`pyrt_destroy`), so trees that complete do not accumulate interpreters.

The subprocess backend's equivalent cap is the number of concurrent spawned
processes, bounded by the same semaphore. Per-interpreter module duplication
for non-multiphase C extensions remains a documented cost; the fallback path
covers the heavy ones.

An implementation test exercises the cap: fan out more executing frames than K
and assert queue-then-drain behavior (no loss, no deadlock, actor responsive
throughout).

## Interrupts

`PYRT_INTERRUPT` is handled on the pyrt thread at the top of its loop and via
a pending-raise into the running cell (`PyErr_SetInterrupt` family) so a
runaway cell can be stopped without killing the interpreter. Interrupt
semantics on per-interpreter-GIL 3.12 are an explicit verification item: the
implementer reads the pinned headers and proves the behavior with a test; if
the API doesn't support clean per-cell interrupts, the fallback is ending the
pyrt thread on interrupt and re-booting lazily (documented in the test that
establishes which is true).

## Data flow (one pass)

```
actor_send(EXECUTE{corr, code})
  → owner's dispatch enqueues work to its pyrt thread + returns (actor stays hot)
  → pyrt thread runs the cell
      → actor.log()/actor.status() posts stream up live (per-cell attributed)
  → RESULT{corr, status, repr-or-traceback} posted back
  → owner's RESULT behavior routes it (reply to corr, report up, store)
```

Ownership: payloads transfer at `actor_send`; receivers or `actor_run` free
them via `payload_destroy`. Messages are transient control — the only durable
trace is the effect writes (WaveDB, future slice), never the message queue.

## Error handling

- Cell exception: `PYRT_RESULT` with `status=1` and the traceback text; the
  actor lives on, the subinterpreter resumes.
- Subinterpreter boot failure: the EXECUTE is answered (never silently dropped)
  with an error result, and the actor continues.
- Subprocess backend: process exit code → status; stderr → text.
- Unloadable module or missing interpreter on the host side: build-time
  failure, never a runtime null-pointer path (`SA_ENABLE_PYTHON=OFF` removes
  the module's sources from the build entirely).

## Testing (TDD; suites join test/test_pyrt.cpp, style: gtest + extern "C")

1. Boot/teardown is valgrind-clean (no `Python.h` leaks from the embedded
   build).
2. EXECUTE returns a corr-matched RESULT.
3. Namespace persists across two cells (`x = 1` then `x2 = x + 1` reads `x`).
4. LOG lines arrive before RESULT — streaming, not buffered-at-exit.
5. Actor stays responsive: a control message is processed while a slow cell
   runs, and the interrupt path ends the cell (the test establishes what 3.12
   actually permits and asserts that reality).
6. Failing cell → RESULT with traceback text, no process failure.
7. Lazy boot: no pyrt thread exists before the first EXECUTE.
8. Subprocess backend passes the same EXECUTE/RESULT contract.
Full suite (existing 58 + new) stays green, valgrind clean; sanitizer runs use
`setarch -R ctest` (documented machine-ASLR quirk).

## Cross-platform status

Linux is the verified path in this milestone. Windows remains a completion
criterion of the Atlas slice (the PCBuild ExternalProject branch), tracked in
the implementation plan rather than silently dropped.

## Risks

- CPython's configure+make on first configure costs minutes (bounded: module
  pinned, no tests); subsequent builds are incremental.
- Subinterpreter C-extension compatibility: historically numpy etc. fail — that
  is exactly what the subprocess fallback exists for; the primary backend must
  not hard-wire subinterpreters.
- Interrupt semantics under per-interpreter GIL: verified against the pinned
  3.12.13 headers before being relied on (see Interrupts).