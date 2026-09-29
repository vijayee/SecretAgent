# Actor Runtime Extraction from liboffs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the Pony-style actor runtime (actor + scheduler + message queue + pool + platform/util deps) from `liboffs` into SecretAgent as a self-compiling, fully tested C library, copying liboffs's codestyle, submodule pattern, gtest style, and CMake style.

**Architecture:** liboffs's actor runtime is the extracted unit: `src/Actor/{actor,message_queue,pool}`, `src/Scheduler/{scheduler,deque}`, plus their dependency closure (`src/Util/{allocator,atomic_compat,log}`, `src/Platform/{compiler,posix_compat,time,thread}`, `src/RefCounter/refcounter`). `message.h` and the `platform.h` umbrella are re-authored minimal (liboffs versions carry app-specific message enums / socket abstractions that are NOT part of the runtime). Project-specific header-guard prefix is `SA_` (liboffs uses `OFFS_`). All liboffs test suites that cover the extracted code come over verbatim in `test/`.

**Tech Stack:** C11, CMake ≥3.22, GoogleTest (git submodule at `deps/googletest`), C++17 (tests only), valgrind for leak checks.

**Source repo (read-only, symlinked):** `./liboffs` → `/home/victor/Workspace/src/github.com/vijayee/liboffs`. NEVER modify files under `liboffs/` — extract only (copy, then edit the copy).

**Conventions copied from liboffs (verify against `liboffs/docs/STYLE_GUIDE.md` when in doubt):**
- Include guards: `#ifndef SA_<MODULE_NAME>_H` (was `OFFS_` in liboffs)
- `snake_case_t` types, `module_action()` functions, `_`-prefixed private functions
- 2-space indent, Egyptian braces, pointer `*` beside the variable name
- `get_memory()` / `get_clear_memory()` — never raw malloc
- Commit messages: conventional commits (`feat:`, `build:`, `test:`, `docs:`), no Co-Authored-By lines, no TODOs in completed work

---

### Task 1: Scaffold the repo

**Files:**
- Create: `.gitignore` (replace current stray content)

- [ ] **Step 1: Write the project .gitignore**

The current `.gitignore` ends with a corruption (`WaveDB/ The n`). Replace the whole file with:

```gitignore
# Symlinked reference codebases (read-only, never tracked)
claude-code-source-code/
deepseek-harness/
onyx/
prime-agent/
liboffs/
WaveDB/

# IDE
/.idea

# CMake build trees
/cmake-build-*
/build*

# CMake artifacts
CMakeCache.txt
CMakeFiles/
CTestTestfile.cmake
DartConfiguration.tcl
Makefile
_deps/
cmake_install.cmake

# Compiled test binaries
test/testsecretagent
Testing/

# Crash artifacts
core.*
vgcore.*
```

- [ ] **Step 2: Verify git status is clean of reference repos**

Run: `git status --short`
Expected: no `liboffs`, `WaveDB`, `deepseek-harness`, `onyx`, `prime-agent`, `claude-code-source-code` entries; only `docs/` and `.idea/` remain untracked (docs/ is fine to track).

- [ ] **Step 3: Commit**

```bash
git add .gitignore docs/
git commit -m "chore: initialize SecretAgent project gitignore"
```

---

### Task 2: Git submodule pattern, deps, and root CMake skeleton

**Files:**
- Create: `deps/googletest` (git submodule)
- Create: `deps/CMakeLists.txt`
- Create: `CMakeLists.txt`

- [ ] **Step 1: Add the googletest submodule (liboffs pattern: source-level dep as git submodule under `deps/`)**

```bash
git submodule add https://github.com/google/googletest.git deps/googletest
```
This creates `.gitmodules` following the liboffs pattern (`[submodule "deps/googletest"]`).

- [ ] **Step 2: Write `deps/CMakeLists.txt`**

```cmake
set(CMAKE_CXX_STANDARD 17)

# GoogleTest is a source-level submodule dep (deps/googletest, pinned in
# .gitmodules) — the same pattern liboffs uses. It is consumed from
# test/CMakeLists.txt against GTest::gtest_main / GTest::gmock targets.
add_subdirectory(googletest)
```

- [ ] **Step 3: Write root `CMakeLists.txt`**

Structure mirrors `liboffs/CMakeLists.txt` (C11 + C++17, `file(GLOB_RECURSE)` over `src/`, static library, opt-in sanitizers, guarded test subdirectory). Full content:

```cmake
cmake_minimum_required(VERSION 3.22)
project(secretagent LANGUAGES C CXX VERSION 0.1.0)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Optional build surface. Embedding projects set this to OFF to skip every
# test executable.
option(SA_BUILD_TESTS "Build SecretAgent test binaries" ON)

# MSVC does not enable C11 <stdatomic.h> by default — the atomic_compat.h header
# in this project includes it unconditionally, so the /experimental:c11atomics
# flag is required on MSVC builds. Other compilers ship <stdatomic.h> by default.
# Apply it globally to every C target in this file before any add_library().
if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()

file(GLOB_RECURSE C_SRC "src/*/*.c")
file(GLOB_RECURSE C_INC "src/*/*.h")

include(CTest)
add_subdirectory(deps)

add_library(secretagent STATIC ${C_SRC})
target_include_directories(secretagent PRIVATE ${C_INC})

# Opt-in AddressSanitizer instrumentation of the secretagent library only (not
# external deps). /Z7 embeds debug info so ASan stack traces symbolicate.
# Default OFF; normal builds are unaffected. Test binary must also enable ASan
# so the runtime is pulled in at link time — see SA_ENABLE_ASAN in test/CMakeLists.txt.
option(SA_ENABLE_ASAN "Instrument the secretagent library with AddressSanitizer for debugging" OFF)
if(SA_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(secretagent PRIVATE /fsanitize=address /Z7)
  else()
    target_compile_options(secretagent PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(secretagent INTERFACE -fsanitize=address)
  endif()
endif()

# Optional UndefinedBehaviorSanitizer and ThreadSanitizer (GCC/Clang only).
# TSan and ASan are mutually exclusive — do not enable both in the same build.
option(SA_ENABLE_UBSAN "Instrument the secretagent library with UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
if(SA_ENABLE_UBSAN AND NOT MSVC)
  target_compile_options(secretagent PRIVATE -fsanitize=undefined -fno-omit-frame-pointer -g)
  target_link_options(secretagent INTERFACE -fsanitize=undefined)
endif()

option(SA_ENABLE_TSAN "Instrument the secretagent library with ThreadSanitizer (GCC/Clang). Mutually exclusive with SA_ENABLE_ASAN." OFF)
if(SA_ENABLE_TSAN AND NOT MSVC)
  if(SA_ENABLE_ASAN)
    message(FATAL_ERROR "SA_ENABLE_TSAN and SA_ENABLE_ASAN are mutually exclusive; enable only one.")
  endif()
  target_compile_options(secretagent PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(secretagent INTERFACE -fsanitize=thread)
endif()

if(SA_BUILD_TESTS)
  add_subdirectory(test)
endif()
```

- [ ] **Step 4: Verify the empty project configures and builds**

```bash
cmake -S . -B cmake-build-debug && cmake --build cmake-build-debug
```
Expected: configure succeeds (`deps/googletest` builds as part of deps); `libsecretagent.a` links (currently with zero sources — if your CMake refuses an empty static lib, it still configures; Task 3 adds real sources).

- [ ] **Step 5: Commit**

```bash
git add .gitmodules CMakeLists.txt deps/CMakeLists.txt deps/googletest
git commit -m "build: add googletest submodule and root CMake skeleton (liboffs pattern)"
```

---

### Task 3: Extract `src/Util` (allocator, atomic_compat, log)

**Files:**
- Create: `src/Util/allocator.h`, `src/Util/allocator.c` (from `liboffs/src/Util/allocator.*`)
- Create: `src/Util/atomic_compat.h` (from `liboffs/src/Util/atomic_compat.h`)
- Create: `src/Util/log.h`, `src/Util/log.c` (from `liboffs/src/Util/log.*` — third-party rxi log + extensions; keep its copyright header untouched)

- [ ] **Step 1: Copy the files**

```bash
mkdir -p src/Util
cp liboffs/src/Util/allocator.h liboffs/src/Util/allocator.c \
   liboffs/src/Util/atomic_compat.h \
   liboffs/src/Util/log.h liboffs/src/Util/log.c src/Util/
```

- [ ] **Step 2: Rewrite include guards OFFS_ → SA_ (project-authored files)**

log.h/log.c are third-party (rxi, `LOG_H` guard) — only `allocator.*` and `atomic_compat.h` carry the `OFFS_` prefix:

```bash
sed -i 's/\bOFFS_/SA_/g' src/Util/allocator.h src/Util/allocator.c src/Util/atomic_compat.h
```
Expected result: `#ifndef SA_ALLOCATOR_H`, `#ifndef SA_ATOMIC_COMPAT_H` (and the in-file `SA_ATOMIC_COMPAT_H` define). Run `grep -n 'SA_' src/Util/*.h src/Util/allocator.c` to confirm.

- [ ] **Step 3: Verify the library still builds**

```bash
cmake --build cmake-build-debug
```
Expected: `get_memory`/`get_clear_memory` compile; `log.c` compiles standalone (it only depends on system headers on non-Windows).

- [ ] **Step 4: Commit**

```bash
git add src/Util
git commit -m "feat: extract Util module (allocator, atomic_compat, log) from liboffs"
```

---

### Task 4: Extract `src/Platform` (compiler, posix_compat, time, thread) + new umbrella

**Files:**
- Create: `src/Platform/platform_compiler.h` (from liboffs)
- Create: `src/Platform/platform_posix_compat.h`, `src/Platform/platform_posix_compat.c` (from liboffs)
- Create: `src/Platform/platform_time.h`, `src/Platform/platform_time.c` (from liboffs)
- Create: `src/Platform/platform_thread.h`, `src/Platform/platform_thread.c` (from liboffs)
- Create: `src/Platform/platform.h` (NEW — minimal umbrella; liboffs's umbrella pulls socket/file/process modules that are not part of the runtime)

- [ ] **Step 1: Copy the files**

```bash
mkdir -p src/Platform
cp liboffs/src/Platform/platform_compiler.h \
   liboffs/src/Platform/platform_posix_compat.h liboffs/src/Platform/platform_posix_compat.c \
   liboffs/src/Platform/platform_time.h liboffs/src/Platform/platform_time.c \
   liboffs/src/Platform/platform_thread.h liboffs/src/Platform/platform_thread.c \
   src/Platform/
sed -i 's/\bOFFS_/SA_/g' src/Platform/*.h src/Platform/*.c
```

- [ ] **Step 2: Write the minimal umbrella `src/Platform/platform.h`**

liboffs's umbrella pulls socket/file/process headers; the runtime needs only threads/time/compiler/posix-shims:

```c
#ifndef SA_PLATFORM_H
#define SA_PLATFORM_H

#include "platform_compiler.h"
#include "platform_posix_compat.h"
#include "platform_time.h"
#include "platform_thread.h"

#endif /* SA_PLATFORM_H */
```

- [ ] **Step 3: Verify the library builds**

```bash
cmake --build cmake-build-debug
```
Expected: `platform_thread.c` compiles (its only project dependency is `../Util/log.h`, already present).

- [ ] **Step 4: Commit**

```bash
git add src/Platform
git commit -m "feat: extract Platform module (compiler, posix_compat, time, thread) from liboffs"
```

---

### Task 5: Extract `src/RefCounter`

**Files:**
- Create: `src/RefCounter/refcounter.h`, `src/RefCounter/refcounter.c` (from liboffs)

- [ ] **Step 1: Copy and rewrite guards**

```bash
mkdir -p src/RefCounter
cp liboffs/src/RefCounter/refcounter.h liboffs/src/RefCounter/refcounter.c src/RefCounter/
sed -i 's/\bOFFS_/SA_/g; s/\bLIBOFFS_/SA_/g' src/RefCounter/refcounter.h src/RefCounter/refcounter.c
```
Expected: guard becomes `SA_REFCOUNTER_H`; the `OFFS_ATOMIC` feature macro becomes `SA_ATOMIC` in BOTH files (the packed-state branch stays active).

- [ ] **Step 2: Verify the library builds and link-check refcounter symbols**

```bash
cmake --build cmake-build-debug
nm cmake-build-debug/libsecretagent.a | grep -c refcounter_init
```
Expected: build succeeds; `grep -c` is nonzero (symbols present).

- [ ] **Step 3: Commit**

```bash
git add src/RefCounter
git commit -m "feat: extract RefCounter module from liboffs"
```

---

### Task 6: Extract `src/Scheduler` (deque, scheduler)

**Files:**
- Create: `src/Scheduler/deque.h`, `src/Scheduler/deque.c` (from liboffs)
- Create: `src/Scheduler/scheduler.h`, `src/Scheduler/scheduler.c` (from liboffs)

- [ ] **Step 1: Copy and rewrite guards**

```bash
mkdir -p src/Scheduler
cp liboffs/src/Scheduler/deque.h liboffs/src/Scheduler/deque.c \
   liboffs/src/Scheduler/scheduler.h liboffs/src/Scheduler/scheduler.c \
   src/Scheduler/
sed -i 's/\bOFFS_/SA_/g' src/Scheduler/deque.h src/Scheduler/deque.c \
                          src/Scheduler/scheduler.h src/Scheduler/scheduler.c
```

- [ ] **Step 2: Resolve the one forward-dep: `../RefCounter/refcounter.h`**

`scheduler.c` includes `"../RefCounter/refcounter.h"` for `scheduler_pool_defer_cleanup` — path already satisfied from Task 5. Include in this task's build check:

```bash
cmake --build cmake-build-debug
```
Expected: build succeeds; `log_error` (scheduler.c, wait_for_idle) and `refcounter_reference` (defer_cleanup) resolve.

- [ ] **Step 3: Commit**

```bash
git add src/Scheduler
git commit -m "feat: extract Scheduler module (work-stealing deque + scheduler pool) from liboffs"
```

---

### Task 7: Extract `src/Actor` (actor, message_queue, pool) + new message.h

**Files:**
- Create: `src/Actor/actor.h`, `src/Actor/actor.c` (from liboffs)
- Create: `src/Actor/message_queue.h`, `src/Actor/message_queue.c` (from liboffs)
- Create: `src/Actor/pool.h`, `src/Actor/pool.c` (from liboffs)
- Create: `src/Actor/message.h` (NEW)

- [ ] **Step 1: Copy and rewrite guards**

```bash
mkdir -p src/Actor
cp liboffs/src/Actor/actor.h liboffs/src/Actor/actor.c \
   liboffs/src/Actor/message_queue.h liboffs/src/Actor/message_queue.c \
   liboffs/src/Actor/pool.h liboffs/src/Actor/pool.c \
   src/Actor/
sed -i 's/\bOFFS_/SA_/g' src/Actor/actor.h src/Actor/actor.c \
                          src/Actor/message_queue.h src/Actor/message_queue.c \
                          src/Actor/pool.h src/Actor/pool.c
```

- [ ] **Step 2: Write the new minimal `src/Actor/message.h`**

liboffs's `message.h` carries the entire application's message enum (Section/Network/HTTP/... payloads) — that is app territory, not runtime. The runtime needs only the generic message envelope:

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_MESSAGE_H
#define SA_MESSAGE_H

#include <stdint.h>

/* Generic message. `type` semantics are application-defined (each module
   defines its own message_type_e in its own header, per liboffs convention).
   Payload ownership transfers with the message; payload_destroy frees it. */
typedef struct message_t {
  uint32_t type;
  void* payload;
  void (*payload_destroy)(void*);
} message_t;

#endif // SA_MESSAGE_H
```

`message_queue.h` includes `"message.h"` — satisfied.

- [ ] **Step 3: Verify the full library builds and actor symbols are present**

```bash
cmake --build cmake-build-debug
nm cmake-build-debug/libsecretagent.a | grep -cE 'actor_init|message_queue_push|scheduler_pool_create'
```
Expected: build succeeds; count ≥ 3.

- [ ] **Step 4: Commit**

```bash
git add src/Actor
git commit -m "feat: extract Actor module (actors, message queue, size pool) from liboffs"
```

---

### Task 8: Port the gtest suite

**Files:**
- Create: `test/CMakeLists.txt`
- Create: `test/test_main.cpp` (NEW, simplified)
- Create: `test/test_actor.cpp` (copied from liboffs, guards rewritten)
- Create: `test/test_message_queue.cpp` (copied)
- Create: `test/test_pool.cpp` (copied)
- Create: `test/test_deque.cpp` (copied)
- Create: `test/test_scheduler.cpp` (copied)
- Create: `test/test_refcounter.cpp` (copied)

Test style copied from liboffs (`liboffs/test/test_actor.cpp` etc.): GoogleTest, C++17, C headers in `extern "C"`, naming `TEST(TestModule, TestFunction_Scenario)`.

- [ ] **Step 1: Copy the test files**

Relative includes (`../src/Actor/actor.h`) resolve identically in the new layout:

```bash
mkdir -p test
cp liboffs/test/test_actor.cpp liboffs/test/test_message_queue.cpp \
   liboffs/test/test_pool.cpp liboffs/test/test_deque.cpp \
   liboffs/test/test_scheduler.cpp liboffs/test/test_refcounter.cpp test/
sed -i 's/\bOFFS_/SA_/g' test/*.cpp
```

- [ ] **Step 2: Write `test/test_main.cpp`**

```cpp
//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
extern "C" {
#include "../src/Platform/platform.h"
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
```

- [ ] **Step 3: Write `test/CMakeLists.txt`**

Mirrors `liboffs/test/CMakeLists.txt` (in-tree googletest, sanitizer gating, gtest_add_tests with WORKING_DIRECTORY):

```cmake
if (BUILD_TESTING)
    project(testsecretagent C CXX)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)

    # Build googletest if not already found
    if (NOT TARGET GTest::gtest_main)
      set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
      add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/googletest ${CMAKE_BINARY_DIR}/deps/googletest)
    endif()

    add_executable(testsecretagent test_main.cpp
            test_actor.cpp
            test_message_queue.cpp
            test_pool.cpp
            test_deque.cpp
            test_scheduler.cpp
            test_refcounter.cpp
            )

    # Match the SA_ENABLE_ASAN gate from the parent CMakeLists.txt: when on,
    # instrument the test exe and pull in the ASan runtime.
    if(SA_ENABLE_ASAN)
      if(MSVC)
        target_compile_options(testsecretagent PRIVATE /fsanitize=address /Z7)
        target_link_options(testsecretagent PRIVATE /DEBUG)
      else()
        target_compile_options(testsecretagent PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(testsecretagent PRIVATE -fsanitize=address)
      endif()
    endif()
    if(SA_ENABLE_UBSAN AND NOT MSVC)
      target_compile_options(testsecretagent PRIVATE -fsanitize=undefined -fno-omit-frame-pointer -g)
      target_link_options(testsecretagent PRIVATE -fsanitize=undefined)
    endif()
    if(SA_ENABLE_TSAN AND NOT MSVC)
      if(SA_ENABLE_ASAN)
        message(FATAL_ERROR "SA_ENABLE_TSAN and SA_ENABLE_ASAN are mutually exclusive; enable only one.")
      endif()
      target_compile_options(testsecretagent PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
      target_link_options(testsecretagent PRIVATE -fsanitize=thread)
    endif()

    target_link_libraries(testsecretagent PRIVATE secretagent)
    target_link_libraries(testsecretagent PRIVATE pthread)
    target_link_libraries(testsecretagent PRIVATE GTest::gtest_main)
    target_link_libraries(testsecretagent PRIVATE GTest::gmock)

    target_include_directories(testsecretagent PUBLIC ${C_INC})
    target_include_directories(testsecretagent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

    include(GoogleTest)
    gtest_add_tests(TARGET testsecretagent TEST_LIST test_list)
    set_tests_properties(${test_list} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
```

- [ ] **Step 4: Configure, build, run the full suite**

```bash
cmake -S . -B cmake-build-debug && cmake --build cmake-build-debug -j
ctest --test-dir cmake-build-debug --output-on-failure
```
Expected: configure + build succeed; ctest reports 100% pass rate across ~40 tests (TestActor 13, TestMessageQueue 9, TestPool 5, TestDeque 14, TestScheduler 11, TestRefCounter 4 — counts approximate; every copied suite must pass).

- [ ] **Step 5: Commit**

```bash
git add test/
git commit -m "test: port actor runtime gtest suites (actor, message_queue, pool, deque, scheduler, refcounter)"
```

---

### Task 9: Memory-leak verification

liboffs convention (per its CLAUDE.md): always check tests for memory leaks after completing an implementation.

**Files:** none (verification only)

- [ ] **Step 1: Run valgrind against the test binary**

```bash
valgrind --leak-check=full --error-exitcode=1 cmake-build-debug/test/testsecretagent 2>&1 | tail -15
```
Expected: `ERROR SUMMARY: 0 errors`; leaked bytes from liboffs-sourced code = 0. (Known acceptable exception: deque grows by keeping old `_deque_array_t`s in a linked list — bounded by worker count, not a leak per iteration. If valgrind reports other real leaks, fix them before proceeding — do NOT leave TODOs).

- [ ] **Step 2: Commit (only if a fix was needed)**

```bash
git add -u
git commit -m "fix: resolve memory leaks surfaced by valgrind in actor runtime"
```

---

### Task 10: Styleguide for future agents

**Files:**
- Create: `docs/STYLE_GUIDE.md`
- Create: `CLAUDE.md`

- [ ] **Step 1: Write `docs/STYLE_GUIDE.md`**

Start from `liboffs/docs/STYLE_GUIDE.md` and apply these changes (keep every section otherwise; the content above is the source of truth):

1. Rename all project references: "Liboffs Project" → "SecretAgent Project"; `liboffs` → `SecretAgent`; guard prefix `OFFS_` → `SA_` (state: "The guard prefix is `SA_` for SecretAgent headers").
2. Replace the module-tree examples with the actual modules: `src/Actor/` (actors, mailbox, backpressure), `src/Scheduler/` (work-stealing deque, scheduler pool), `src/RefCounter/`, `src/Platform/` (threading/time/compiler shims), `src/Util/` (allocator, atomic C/C++ compat, logging).
3. Keep all convention sections verbatim (naming, `_t`/`_e` suffixes, `module_action()`, `REFERENCE`/`YIELD`/`DESTROY`/`CONSUME` macros, `get_memory()`/`get_clear_memory()`, 2-space/Egyptian formatting, `module_action()` test naming, C11/C++17/GoogleTest build conventions).
4. In §1 Repository layout, describe this repo: `src/` (library code by module), `test/` (GoogleTest, mirrors module names), `deps/` (git submodules: googletest), `docs/` (this guide + plans), root `CMakeLists.txt`.
5. Delete examples referencing modules not present here (BlockCache/Streams/Workers examples) — replace with the actor/scheduler equivalents that exist here.

Verify with: `grep -c 'SA_' docs/STYLE_GUIDE.md` (nonzero) and `grep -ci 'liboffs' docs/STYLE_GUIDE.md` — the only hits must be intentional provenance mentions, if any.

- [ ] **Step 2: Write `CLAUDE.md`**

```markdown
# SecretAgent Project

Always read and follow the coding conventions in [STYLE_GUIDE.md](./docs/STYLE_GUIDE.md) when
writing or modifying C code in this project.

## No TODOs in Completed Work

Never leave a TODO/FIXME/HACK/XXX comment in code and mark a task as completed. Every TODO
represents unfinished work. When completing a task:
- Implement the code the TODO describes, or
- If it requires design decisions beyond scope, escalate to the user rather than leaving it
- A task is not done until every TODO in the files it touched is resolved

## Git Commit Conventions

- **Do NOT add "Co-Authored-By" lines to commit messages.** All commits should have only the
  author's information.
- Use clear, descriptive commit messages following conventional commit format
  (e.g., "feat:", "fix:", "docs:", "test:", "refactor:", "chore:")
- Keep commits focused and atomic - one logical change per commit

## Testing

- Tests are GoogleTest (C++17) with C headers wrapped in `extern "C"`, mirroring module names:
  `test/test_<module>.cpp`
- After any implementation, run `ctest --test-dir cmake-build-debug --output-on-failure` and
  verify no memory leaks (valgrind on the test binary) before marking work done.
- Sanitizer builds are opt-in: `-DSA_ENABLE_ASAN=ON`, `-DSA_ENABLE_UBSAN=ON`,
  `-DSA_ENABLE_TSAN=ON` (TSan and ASan are mutually exclusive).

## Key Patterns

- Reference-counted structs have `refcounter_t refcounter` as the first member
- Types use `_t` suffix, functions follow `module_action()` naming
- Create functions use `get_clear_memory()` and call `refcounter_init()` last
- Never touch the symlinked `liboffs/`, `WaveDB/`, `deepseek-harness/`, `onyx/`, `prime-agent/`,
  `claude-code-source-code/` directories — they are read-only reference codebases
```

- [ ] **Step 3: Commit**

```bash
git add docs/STYLE_GUIDE.md CLAUDE.md
git commit -m "docs: add style guide and CLAUDE.md for future agents"
```

---

## Acceptance criteria (whole plan)

1. `cmake -S . -B cmake-build-debug && cmake --build cmake-build-debug -j` succeeds.
2. `ctest --test-dir cmake-build-debug --output-on-failure` — 100% pass (all ported suites).
3. Valgrind shows no leaks/errors in library code.
4. Repository layout mirrors liboffs: `src/<Module>/{module.h,module.c}`, `test/test_<module>.cpp`, `deps/` submodules, root + test CMakeLists matching liboffs's structure/style, `docs/STYLE_GUIDE.md`, `CLAUDE.md`.
5. No symlinks tracked, no TODOs, every commit atomic with a conventional message.