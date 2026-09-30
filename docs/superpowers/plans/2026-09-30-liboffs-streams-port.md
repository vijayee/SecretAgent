# Liboffs Streams/HTTP Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the runtime's synchronous throwaway HTTP client with liboffs's production stack — the `poll-dancer` async event loop, the express-like HTTP server core, and a new async client on that loop — behind one `SA_ENABLE_STREAMS` gate.

**Architecture:** `deps/` gains three pinned submodules (poll-dancer, http-parser, pcre2-for-Windows). `src/Streams/` receives the copied express-like core (server/connection/route/request/response/headers/cors/auth_middleware) plus the family-closure files it needs from liboffs (`Util/vec`, `Util/validation`, `Util/error`, `Util/bcrypt`, `Buffer/buffer`, `Streams/stream(s)/file-stream`, `Platform/platform_random`, `Platform/platform_regex_compat`). `src/Net/http.{h,c}` is deleted; `model.c` consumes a NEW async client (`http_client_submit` → completion on the loop thread → model waits on a refcounted record). The loop's worker-yield restructure is the NEXT slice's work — this slice only proves the transport.

**Spec:** `docs/superpowers/specs/2026-09-30-liboffs-streams-port-design.md`.

**References:** `docs/backpressure-and-timer-stall.md` (poll-dancer contracts: never hold loop_lock across waits; timer_destroy int contract), `docs/STYLE_GUIDE.md`, `docs/wavedb-exploration.md` not needed here. The liboffs tree at `/home/victor/Workspace/src/github.com/vijayee/liboffs` is READ-ONLY reference — port by copying, never modify or build in it.

**Frozen boundaries (hard rules):** never modify `liboffs/`, `WaveDB/`, `deepseek-harness/`, `prime-agent/`, `onyx/`, `claude-code-source-code/`, or anything already under `deps/` except ADDING the three new submodules. `src/Actor`, `src/Scheduler`, `src/RefCounter` frozen. New `src/Util`/`src/Buffer`/`src/Platform` members are additive ports only. All ctest runs: `setarch -R ctest --test-dir cmake-build-debug --output-on-failure` (ASLR quirk). Test binary: `cmake-build-debug/test/testsecretagent`. Valgrind needs `--strip-debug` copies (unstripped DWARF5 makes valgrind 3.18.1 bail BEFORE running anything — a "0 leaks" from an unstripped run is empty evidence; verify with `grep -c "OK ]"` on the test output). Conventional commits, no Co-Authored-By, never leave TODOs, atomics fine. Escalate on ambiguity you cannot resolve from the spec + liboffs sources rather than inventing behavior.

## File structure

```
deps/poll-dancer/        (Task 1: submodule pin a05b9e1e519c63238ded3a36cdabf79f85a216b3)
deps/http-parser/        (Task 1: submodule pin ec8b5ee63f0e51191ea43bb0c6eac7bfbff3141d)
deps/pcre2/              (Task 1: submodule pin ff92e0b9cea5b5ae3af12ba930d03556684f098b — WIN32-only build path)
deps/CMakeLists.txt      (Task 1: http_parser + streams_closure static targets; Task 1: poll-dancer ExternalProject)
CMakeLists.txt           (Task 1: SA_ENABLE_STREAMS option + link + SA_HAS_STREAMS define + alias extension)
src/Util/vec.{h,c}       (Task 2: port)  src/Util/validation.{h,c}  (Task 2)  src/Util/error.{h,c}  (Task 2)
src/Util/bcrypt.{h,c}    (Task 2)  src/Buffer/buffer.{h,c}  (Task 2)
src/Platform/platform_random.{h,c}    (Task 2)  src/Platform/platform_regex_compat.h  (Task 2)
src/Streams/stream.h, streams.{h,c}, file-stream.{h,c}   (Task 2: liboffs src/Streams port)
src/Streams/http_status.h, http_headers.{h,c}, http_request.{h,c}, http_response.{h,c}
src/Streams/http_connection.{h,c}, http_route.{h,c}, http_server.{h,c}, cors.{h,c}, auth_middleware.{h,c}   (Task 2)
src/Streams/loop_thread.{h,c}, http_client.{h,c}   (Task 4: NEW code, full contract in plan)
src/Frame/model.c        (Task 5: consume the async client via completion record)
src/Net/                 (Task 5: DELETED)
test/test_streams_server.cpp, test/test_streams_client.cpp   (Tasks 3-4)
test/test_http.cpp       (Task 5: deleted; its framing tests live on in test_streams_client.cpp)
test/CMakeLists.txt      (Tasks 3-5)
atlas/workflow.json      (Task 6: evidence)
```

Verification idioms used throughout:

```bash
# build
cmake --build cmake-build-debug -j
# suite (everything hereafter assumes this instead of bare ctest)
setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
# valgrind on a filter (stripped copy; VERIFY it ran: grep -c "OK ]" on output)
cp cmake-build-debug/test/testsecretagent /tmp/vg && strip --strip-debug /tmp/vg
setarch -R valgrind --leak-check=full --suppressions=test/vg-cpython.supp \
  --suppressions=test/vg-wavedb.supp /tmp/vg --gtest_filter='<filter>' 
# ASan suite
setarch -R ctest --test-dir cmake-build-asan --output-on-failure 2>/dev/null | tail -3
```

---

### Task 1: The three dependencies, wired and aliased

**Files:**
- Create: `deps/poll-dancer`, `deps/http-parser`, `deps/pcre2` (submodules)
- Modify: `deps/CMakeLists.txt`, root `CMakeLists.txt`

- [ ] **Step 1: Add the submodules (offline fallback allowed, like wavedb's task)**

```bash
git submodule add https://github.com/vijayee/poll-dancer.git deps/poll-dancer
git submodule add https://github.com/nodejs/http-parser.git deps/http-parser
git submodule add https://github.com/PCRE2Project/pcre2.git deps/pcre2
git -C deps/poll-dancer checkout a05b9e1e519c63238ded3a36cdabf79f85a216b3
git -C deps/http-parser checkout ec8b5ee63f0e51191ea43bb0c6eac7bfbff3141d
git -C deps/pcre2      checkout ff92e0b9cea5b5ae3af12ba930d03556684f098b
git submodule update --init --recursive deps/poll-dancer   # if it has nested pins
```
If any URL is unreachable, `cp -a` from the local checkouts instead (`/home/victor/Workspace/src/github.com/vijayee/poll-dancer`, `liboffs/deps/http-parser`, `liboffs/deps/pcre2`) and RECORD the deviation in the commit message.

- [ ] **Step 2: Wire CMake (`deps/CMakeLists.txt` + root)**

Insert AFTER the wavedb block in `deps/CMakeLists.txt` (read it first — mirror its exact ExternalProject + alias-archive shape):

```cmake
# ---- poll-dancer: the IO event loop (epoll/kqueue/iocp) ------------------
# Built from the pinned submodule via ExternalProject (same pattern as
# wavedb-ext). Backend selection is poll-dancer's own CMake (platform-based).
option(SA_ENABLE_STREAMS "Build the streams/http transport (requires deps/*)" ON)
if(SA_ENABLE_STREAMS)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer/CMakeLists.txt)
    message(FATAL_ERROR "deps/poll-dancer submodule missing. Run: git submodule update --init --recursive")
  endif()
  set(PD_ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer)
  set(PD_BUILD ${CMAKE_BINARY_DIR}/deps/poll-dancer)
  set(PD_CONFIGURE ${CMAKE_COMMAND} -S ${PD_ROOT} -B ${PD_BUILD}
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DPOLL_DANCER_BUILD_TESTS=OFF -DPOLL_DANCER_BUILD_EXAMPLES=OFF
      -DPOLL_DANCER_BUILD_SHARED=OFF -DPOLL_DANCER_THREAD_SAFE=ON)
  ExternalProject_Add(poll-dancer-ext
    SOURCE_DIR ${PD_ROOT}
    CONFIGURE_COMMAND ${PD_CONFIGURE}
    BUILD_COMMAND ${CMAKE_COMMAND} --build ${PD_BUILD} --target poll_dancer
    INSTALL_COMMAND ${CMAKE_COMMAND} -E echo "Skipping install; consuming build-dir artifact"
    BUILD_BYPRODUCTS ${PD_BUILD}/libpoll_dancer.a
    BINARY_DIR ${PD_BUILD}
  )
endif()
```

(If the real option names or the static-library artifact name differ from
`libpoll_dancer.a`, read `deps/poll-dancer/CMakeLists.txt` lines 60-80 and use
the REAL names; record the mapping in the commit message.)

http-parser and pcre2 are NOT ExternalProjects — http-parser has no CMakeLists
(at all: gyp only); compile both as plain static libs in `deps/CMakeLists.txt`:

```cmake
if(SA_ENABLE_STREAMS)
  add_library(http_parser STATIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/http-parser/http_parser.c)
  set_property(TARGET http_parser PROPERTY POSITION_INDEPENDENT_CODE ON)
  target_include_directories(http_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/deps/http-parser)
  # pcre2: WIN32-only (the platform_regex_compat.h shim maps POSIX regex to
  # PCRE2's pcre2posix on Windows; Linux uses <regex.h>). Wire the build when
  # a Windows toolchain exists; a placeholder check keeps Linux green:
  # if(WIN32) ExternalProject/AddObjectLibrary for pcre2 (pcre2-8 static +
  # pcre2-posix) with PCRE2_SUPPORT_JIT as poll-dancer/liboffs records it. endif()
endif()
```

Root `CMakeLists.txt`, after the `SA_ENABLE_WDB` link block:

```cmake
if(SA_ENABLE_STREAMS)
  target_compile_definitions(secretagent PUBLIC SA_HAS_STREAMS=1)
  target_include_directories(secretagent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/http-parser)
  add_dependencies(secretagent poll-dancer-ext)
endif()
```
(The link of the archives goes in after Task 2 creates `src/Streams` sources —
add `target_link_libraries(secretagent PUBLIC ${PD_BUILD}/libpoll_dancer.a http_parser …)` in THIS task, referencing the byproduct; empty sources still link.)

- [ ] **Step 3: Probe and extend the alias machinery**

```bash
nm cmake-build-debug/deps/poll-dancer/libpoll_dancer.a | grep " T \| D \| B " | awk '{print $3}' | sort > /tmp/pd_syms.txt
comm -12 <(nm cmake-build-debug/libsecretagent.a | grep " T " | awk '{print $3}' | sort) /tmp/pd_syms.txt
```
Do the same for `http_parser.c`'s object. For every colliding symbol shown,
add it to the redefine-syms file the wavedb alias step uses (read
`CMakeLists.txt:100-140` for that file's mechanism and naming convention —
wave the prefix per-archive, e.g. `pdx_`). If poll-dancer defines NO colliding
symbols (likely — it is pd_*-namespaced), record "no aliases needed" in the
commit message instead of inventing work. http-parser is `http_*`-namespaced —
probe it the same way.

- [ ] **Step 4: Build + verify the untouched suites**

```bash
cmake -S . -B cmake-build-debug -DSA_BUILD_TESTS=OFF && cmake --build cmake-build-debug -j
setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
```
Expected: `libpoll_dancer.a` + `libhttp_parser.a` artifacts exist; suite count unchanged (121).

- [ ] **Step 5: Commit**

```bash
git add .gitmodules deps/poll-dancer deps/http-parser deps/pcre2 deps/CMakeLists.txt CMakeLists.txt
git commit -m "build: pin poll-dancer, http-parser, pcre2; wire the streams transport gate"
```

---

### Task 2: Family-closure port + the express-like core → `src/Streams/`

Copy by file, never edit liboffs. Where an include path differs, fix OUR copy.
CRITICAL LAYOUT FACT: liboffs `src/ClientAPI/HTTP/x.c` reaching `../../Util/…`
needs the SAME relative depth as our `src/Streams/x.c` (`../Util/…`) — the
depths happen to match, so most family includes survive verbatim. Verify per
file against this table (adjust ONLY where liboffs's actual include names
differ from ours):

| Copy from → to | Include fixes needed |
|---|---|
| `liboffs/src/Util/vec.{h,c}` → `src/Util/vec.{h,c}` | none expected (family layout identical) |
| `liboffs/src/Util/validation.{h,c}` → `src/Util/validation.{h,c}` | none expected |
| `liboffs/src/Util/error.{h,c}` → `src/Util/error.{h,c}` | none expected |
| `liboffs/src/Util/bcrypt.{h,c}` → `src/Util/bcrypt.{h,c}` | none expected (`../Platform/platform_random.h` resolves once Task 2 also ports it) |
| `liboffs/src/Buffer/buffer.{h,c}` → `src/Buffer/buffer.{h,c}` (NEW dir) | none expected (`../Util/*`, `../RefCounter/*` depth matches) |
| `liboffs/src/Platform/platform_random.{h,c}` → `src/Platform/` | none expected |
| `liboffs/src/Platform/platform_regex_compat.h` → `src/Platform/` | none (header-only `_WIN32` shim; Linux path uses `<regex.h>`) |
| `liboffs/src/Streams/stream.h` → `src/Streams/stream.h` | fix any `…/../…` upward deps the same way (`../Util/*`, `../Buffer/buffer.h`, `../RefCounter/*`) |
| `liboffs/src/Streams/streams.{h,c}` → `src/Streams/streams.{h,c}` | same as above; it may include `Util/error.h` (ported) and `file-stream.h` |
| `liboffs/src/Streams/file-stream.{h,c}` → `src/Streams/` | same |
| `liboffs/src/ClientAPI/HTTP/http_status.h`, `http_headers.{h,c}`, `http_request.{h,c}`, `http_response.{h,c}`, `http_connection.{h,c}`, `http_route.{h,c}`, `http_server.{h,c}`, `cors.{h,c}`, `auth_middleware.{h,c}` → `src/Streams/` | `"…/../Streams/stream.h"` → `"stream.h"`; everything else (`../Util/*`, `../Buffer/buffer.h`, `../Platform/*`, `../Actor/*`, `../Scheduler/*`, `../RefCounter/*`) keeps depth |

- [ ] **Step 1: Copy the closure + core per the table**

```bash
mkdir -p src/Buffer
# per table: cp liboffs/src/... the files above
```
Fix includes ONLY per the table; if a ported file includes something beyond
the table (grep every ported file's `#include "` lines before compiling), add
the missing file to the closure from liboffs and record it in the commit.

- [ ] **Step 2: Compile-gate the new module**

Root `CMakeLists.txt`: add `src/Streams/*.c` + `src/Buffer/buffer.c` +
the new `src/Util/*.c` files to the `C_SRC`/target lists INSIDE the
`SA_ENABLE_STREAMS` block (OFF must stay sans streams: the new sources are
gated, not global). The OFF-build guard mirrors wavedb's: `#ifdef
SA_HAS_STREAMS` wherever a public header is consumed by non-gated code (none
yet in this task — the core library API gains nothing).

- [ ] **Step 3: Build green**

```bash
cmake --build cmake-build-debug -j 2>&1 | grep -iE "error|warning" | head -20
```
Expected: zero errors; fix include paths until green. If a ported file needs
MORE than an include-path fix to compile (family API drift between liboffs
and us — e.g. `actor_t` shape differences), STOP and escalate with the exact
diff-of-contracts rather than adapting semantics silently.

- [ ] **Step 4: Commit**

```bash
git add src/Util src/Buffer src/Platform src/Streams CMakeLists.txt deps/CMakeLists.txt
git commit -m "refactor: port liboffs streams/express-http closure into src/Streams (unchanged sources, include paths only)"
```

---

### Task 3: Server loopback test suite (`test/test_streams_server.cpp`)

**Files:**
- Create: `test/test_streams_server.cpp`
- Modify: `test/CMakeLists.txt` (add `test_streams_server.cpp` to `testsecretagent`'s sources under the streams gate — mirror how test_live_loop.cpp is registered)

- [ ] **Step 1: Write the failing tests (full listing)**

```cpp
//
// Created by victor on 9/30/26.
//
// Task 3: the ported express-like core must do its job on loopback: bind,
// route, parse a request, run middleware, write a response.

#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
extern "C" {
#include "../src/Streams/http_server.h"
#include "../src/Streams/http_request.h"
#include "../src/Streams/http_response.h"
#include "../src/Streams/cors.h"
#include "../src/Streams/auth_middleware.h"
}
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* A tiny client: connect loopback, send raw, read till close (the SERVER is
   the thing under test; the raw-socket client is fixture, like Task 8's was).
   Returns the full response text. */
static std::string raw_roundtrip(uint16_t port, const std::string& request);

static uint16_t bind_loopback(int* fd_out);   /* same idiom as Task 8's fake_server_listen */

static void server_thread_run(http_server_t* server, std::atomic<uint8_t>* stop);

TEST(TestStreamsServer, TestBindRouteRequestResponse) {
  int listen_fd = -1;
  uint16_t port = bind_loopback(&listen_fd);
  ASSERT_GE(port, 0u);

  http_server_t* server = http_server_create("127.0.0.1", port);   // signature per http_server.h — adapt to the REAL create API
  ASSERT_NE(server, nullptr);
  ASSERT_EQ(http_server_route(server, "GET", "/hello", _hello_handler), 0);  // handler: writes one 200 "ok" body
  std::atomic<uint8_t> stop;
  stop.store(0);
  std::thread svr(server_thread_run, server, &stop);

  std::string resp = raw_roundtrip(port, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_NE(resp.find("200"), std::string::npos);
  EXPECT_NE(resp.find("ok"), std::string::npos);

  stop.store(1);
  svr.join();
  http_server_destroy(server);
  close(listen_fd);
}

TEST(TestStreamsServer, TestUnknownRouteIs404)  { /* same shape, expect 404 in the raw response */ }
TEST(TestStreamsServer, TestMalformedRequestIs400) { /* send "GET /x bogus\r\n\r\n"; expect 400 */ }
TEST(TestStreamsServer, TestCorsMiddlewareEngages) {
  /* http_server_use(server, cors_middleware(...)) — an OPTIONS preflight
     gets the Access-Control-Allow-Origin header back; a GET carries it too. */ }
TEST(TestStreamsServer, TestAuthMiddlewareBearerAndLoopbackOptOut) {
  /* auth_middleware_create(hash, allow_local_no_auth=true) on loopback:
     no token → still 200 (loopback opt-out). With allow_local_no_auth=false:
     no/incorrect Bearer → 401; matching Bearer → 200. Build the hash with
     bcrypt_hash() from src/Util/bcrypt.h. */ }
```

The handler/middleware signatures above are SHAPES — read the ported
`http_server.h`/`http_request.h`/`http_response.h` and write the tests against
the REAL API (liboffs's own tests, if any exist under liboffs build-test dirs,
are the reference for correct usage patterns — copy only usage shapes, not
offs-specific fixtures). Adapt each EXPECT to the real response text (status
line format, header casing).

- [ ] **Step 2: Red → green**

```bash
cmake --build cmake-build-debug -j
setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
```
Expected: all TestStreamsServer tests green (the core exists; this suite only
proves the port — a failing test here is a port defect, fix by include/call
adaptation, never by semantic change).

- [ ] **Step 3: Valgrind on TestStreamsServer (stripped copy, verified-executing)**

- [ ] **Step 4: Commit**

```bash
git add test/test_streams_server.cpp test/CMakeLists.txt
git commit -m "test: loopback proofs for the ported express-like server core"
```

---

### Task 4: The async client (`loop_thread` + `http_client`) — NEW code, TDD

**Files:**
- Create: `src/Streams/loop_thread.h`, `src/Streams/loop_thread.c`, `src/Streams/http_client.h`, `src/Streams/http_client.c`
- Test: `test/test_streams_client.cpp` (replaces the deleted `test_http.cpp` in Task 5 — create it here with the client tests; framing tests land in Task 5)

- [ ] **Step 1: Write `loop_thread.h` + `http_client.h` (frozen contracts)**

```c
//
// Created by victor on 9/30/26.
//

#ifndef SA_STREAMS_LOOP_THREAD_H
#define SA_STREAMS_LOOP_THREAD_H

#include <stddef.h>

/* The runtime's one IO reactor thread: runs a poll-dancer pd_loop forever
   until destroy. Other threads reach the loop only through streams_loop_call
   (thread-safe marshalling — pd_loop_async_send + the async watcher pd
   exposes; if pd's async-call API differs, adapt HERE, never at call sites).
   Fail-loud discipline: if the loop thread dies, log_error + abort the
   process (a half-dead reactor must never limp). */
typedef struct streams_loop_thread_t streams_loop_thread_t;

streams_loop_thread_t* streams_loop_create(void);
/* Runs fn(ctx) ON the loop thread. 0 ok; nonzero if the loop is gone. May
   block only on pd's async queue (µs). fn MUST be µs-scale (it runs between
   the loop's fd dispatches). */
int streams_loop_call(streams_loop_thread_t* lt, void (*fn)(void*), void* ctx);
/* Returns the underlying pd_loop_t for watchers created by callers, e.g.
   http_client; NULL if the loop thread is dead. */
struct pd_loop;   /* forward; real name from deps/poll-dancer/include/poll-dancer/types.h */
struct pd_loop* streams_loop_raw(streams_loop_thread_t* lt);
void streams_loop_destroy(streams_loop_thread_t* lt);

#endif // SA_STREAMS_LOOP_THREAD_H
```

```c
//
// Created by victor on 9/30/26.
//

#ifndef SA_HTTP_CLIENT_H
#define SA_HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "loop_thread.h"

/* Async HTTP/1.1 client on poll-dancer: ONE POST, one response — the async
   twin of the retired src/Net/http.c contract. Non-blocking connect/send/
   recv on watchers; the timeout is a loop timer. The completion runs ON THE
   LOOP THREAD and MUST be µs-scale. Ownership: body and error are heap and
   the CALLBACK owns them (free() or stash). status: HTTP code, or -1 for
   transport errors (error then set; body NULL). Connection is closed and all
   request-side memory freed inside the client before the callback fires. */
typedef void (*http_client_completion_fn)(void* ctx, int status,
                                          char* body, size_t body_len,
                                          char* error);

typedef struct http_client_t http_client_t;

/* The client owns the loop thread (or borrows one passed in). submit COPIES
   url/api_key/body into the request (the caller's memory may vanish after
   return — the future orchestration slice relies on this). Returns 0 on
   accept; nonzero = rejected BEFORE any I/O (NULL/empty url or body, no loop
   memory) — the completion NEVER fires for a rejected submit, and the ctx is
   untouched (caller still owns it). */
http_client_t* http_client_create(streams_loop_thread_t* lt);
int http_client_submit(http_client_t* c, const char* url, const char* api_key,
                       const char* body_json, uint32_t timeout_ms,
                       http_client_completion_fn on_done, void* ctx);
/* Cancels in-flight requests this client owns and destroys the client. All
   submit-callbacks that have not fired yet NEVER will fire (so a caller that
   destroys the client owns the ctx cleanup). */
void http_client_destroy(http_client_t* c);

#endif // SA_HTTP_CLIENT_H
```

- [ ] **Step 2: Write the failing tests (`test/test_streams_client.cpp`)**

Port the fake-server fixture from the current `test/test_http.cpp` verbatim
(fake_server_listen / canned-reply server thread — test-side sockets are the
allowed fixture). Core tests, full code shapes:

```cpp
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
extern "C" {
#include "../src/Streams/http_client.h"
}
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* completion record: mutex + cond + captured fields (the model.c record's
   shape, minus refcounting — single callback, loop thread frees body/err
   INTO the record). */
typedef struct wait_result_t {
  std::mutex m; std::condition_variable cv; bool done = false;
  int status = 0; std::string body; std::string error;
} wait_result_t;

static void capture_cb(void* ctx, int status, char* body, size_t body_len, char* error) {
  wait_result_t* w = (wait_result_t*)ctx;
  { std::lock_guard<std::mutex> g(w->m);
    w->status = status;
    if (body) w->body.assign(body, body_len);
    if (error) w->error.assign(error);
    w->done = true; }
  w->cv.notify_one();
  free(body); free(error);
}
/* wait_with_pump: while !done && waited<3000: run_one iteration of the loop
   thread IS running on its own thread — just wait on the cv with a deadline. */

TEST(TestStreamsClient, TestSubmitCompletesRoundTrip) {
  /* canned server answers 200 Content-Length body "{\"ok\":true}" on the path;
     submit via a real http_client on a real loop thread; assert status 200,
     body contains ok:true, error empty; the REQUEST seen server-side contains
     POST, Content-Length, Content-Type: application/json. */
}

TEST(TestStreamsClient, TestDeadEndpointReportsTransport) {
  /* http://127.0.0.1:1 → completion status -1, error heap non-empty, body null. */
}

TEST(TestStreamsClient, TestTimeoutFiresOnce) {
  /* hang-server fixture (accept + hold 3s, no reply), submit timeout_ms=300:
     completion fires ONCE with status -1 + error (the words "timeout" or the
     loop timer's reason); ~300-2500ms elapsed on the cv wait. */
}

TEST(TestStreamsClient, TestSubmitCopiesArguments) {
  /* submit; scribble over the caller's url/body buffer immediately; the
     completion must still carry the ORIGINAL request (server sees the
     original path/body). Pins the copy discipline the orchestration slice
     will rely on. */
}

TEST(TestStreamsClient, TestRejectedSubmitNeverCallsBack) {
  /* NULL body_json → nonzero rc, ctx untouched, no callback within 50ms. */
}

TEST(TestStreamsClient, TestDestroyCancelsInflight) {
  /* hang server + submit timeout 5000 + destroy the client at ~50ms: no
     callback fires within 2s; destroy returns <2s; no leak (the client owns
     the in-flight record → freed by destroy). */
}
```

- [ ] **Step 3: Red → implement → green**

Implement `loop_thread.c` (pd_loop_create + platform thread; `pd_loop_run`;
async call-in per pd's actual API — READ
`deps/poll-dancer/include/poll-dancer/poll-dancer.h` FIRST: the async watcher
section around `pd_loop_async_send` at :270 and the timer API; if pd's
thread-safe async call requires a pre-created async watcher, create it inside
`streams_loop_create`) and `http_client.c`:

- URL parse: MOVE the `_parse_url` logic from `src/Net/http.c` (it works and
  is leak-tested) — the deletion in Task 5 makes this a move, not a copy.
- Request state machine on watchers: non-blocking socket + `pd_watcher` on
  connect-writable → send the (prebuilt) request, draining `send()` EAGAIN →
  watch read, accumulate into a cap-checked buffer (carry `_HTTP_BODY_MAX`/
  `_HTTP_READ_MAX` discipline from the old client: reject promised CL > cap,
  total > READ_MAX — port those constants and their tests) → parse headers
  with **http-parser** (`http_parser_execute`, chunked handling native, per
  the pinned http-parser) honoring the SAME empty-body non-NULL rule the old
  client pinned (chunked-empty → body "" non-NULL; CL:0 → NULL body) → single
  completion. Statuses: `<200 || >=300` still complete normally (the caller
  reads the body; model.c's error path is unchanged).
- Timeout: one `pd_timer` per request, armed at submit; on fire → completion
  (-1, "http client: timeout …") + cleanup.
- Failure cleanup: every error path stops watchers, closes, frees, THEN calls
  the completion if it must (callers' invariants: after a non-rejected submit,
  the completion ALWAYS fires exactly once — including after
  `http_client_destroy` mid-flight, which must NOT fire).
- Log with the family `log_*` (the alias step covers it if needed).

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
```
Expected: TestStreamsClient green; suite 121+6.

- [ ] **Step 4: Valgrind on TestStreamsClient (stripped, verified) — 0 leaks**
- [ ] **Step 5: Commit**

```bash
git add src/Streams/loop_thread.h src/Streams/loop_thread.c src/Streams/http_client.h src/Streams/http_client.c test/test_streams_client.cpp test/CMakeLists.txt
git commit -m "feat: async http client on the poll-dancer loop thread"
```

---

### Task 5: Rewire the model client, delete `src/Net`, port the framing tests

**Files:**
- Modify: `src/Frame/model.c` (+ `model.h` only if a comment changes), `test/test_model_decode.cpp` (only if error-text pins change), `test/CMakeLists.txt`
- Delete: `src/Net/http.{h,c}`, `test/test_http.cpp`

- [ ] **Step 1: model.c swaps the transport (vtable unchanged)**

Replace the `http_post_json` body with:

```c
/* The completion record: refcounted because the loop thread's callback and
   this waiter both hold it; whoever finishes last frees (refcounter family
   rule; the loop's callback holds the ctx ref across dispatch). */
typedef struct _model_completion_t {
  refcounter_t refcounter;                 /* FIRST member per style guide */
  platform_mutex_t* lock;
  platform_condvar_t* cv;
  _Atomic(uint8_t) done;
  int status;
  char* body; size_t body_len;
  char* error;
} _model_completion_t;
```

`_model_http_complete` becomes: build request text (unchanged) → allocate
record (`get_clear_memory`, `refcounter_init` LAST), take one extra ref for
the callback → `http_client_submit(b->loop, url, b->api_key, request_text,
b->timeout_ms, _model_completion_cb, record)` → wait `b->timeout_ms + 5000ms`
on the condvar (the client owns timeout semantics; the extra window is the
callback's dispatch slack — a lost completion is a transport bug and this wait
turns it loud instead of hung) → on done: copy status/body/error OUT (body
steal: the record's body moves to the caller's response object), release the
waiter ref, return per the existing shapes (`*raw_out` from the body when
requested, model_reply decode unchanged). The backend gains the loop-thread
pointer: `model_http_backend_create(cfg)` creates/holds THE process loop
thread (one per process — `streams_loop_create` on first backend; documented:
"the runtime's one reactor"), freed at the LAST `model_backend_destroy`
(a `streams_loop_thread_t*` static guarded by creation count, family style —
simplest correct shape; escalate if you find a natural owner instead).

The completion callback (runs on the LOOP thread, µs-scale):

```c
static void _model_completion_cb(void* ctx, int status, char* body, size_t body_len, char* error) {
  _model_completion_t* c = (void*)ctx;
  platform_mutex_lock(c->lock);
  c->status = status; c->body = body; c->body_len = body_len; c->error = error;
  atomic_store(&c->done, 1);
  platform_condvar_broadcast(c->cv);
  platform_mutex_unlock(c->lock);
  /* ref released by the waiter after it steals; see _model_completion_wait */
  _model_completion_unref(c);
  (void)0;
}
```
(The exact ref handoff: waiter releases after READING the record's body under
the lock; the callback releases its own ref on entry-completion. Write the
refcounter so a callback-vs-waiter race cannot free mid-read: waiter takes
its ref BEFORE submit and only unrefs after the done-flag read — document the
invariant at the record.)

- [ ] **Step 2: Delete the dead transport, port the framing tests**

```bash
git rm -q src/Net/http.h src/Net/http.c test/test_http.cpp
```
Re-create the Task-8-era hardening pins in `test/test_streams_client.cpp`
(the fake-canned harness exists there now) — the test names KEEP their old
meanings: chunked multi-chunk decode (native via http-parser now), chunked
size/header dialects, absurd-size claim rejected, repeated TE field-lines,
truncated chunk, missing final CRLF, empty-body non-NULL (chunked ""), CL:0
NULL-body rule, CL > cap rejected, dead endpoint, hang-timeout. Each was
green pre-port; each must be green post-port. Where the old tests asserted
error STRING bytes, loosen to the stable substrings (e.g. "chunked") since
the reason text may change shape.

- [ ] **Step 3: Full suite green (ON + ASan)**

```bash
cmake --build cmake-build-debug -j && cmake --build cmake-build-asan --target testsecretagent -j
setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
setarch -R ctest --test-dir cmake-build-asan --output-on-failure 2>/dev/null | tail -3
```
Expected: all green; `test_model_decode.cpp` tests unchanged or minimally
updated (error-surface contract: status codes + reason substrings).

- [ ] **Step 4: Valgrind (decode/client/loop filters, stripped copies) — 0 leaks**
- [ ] **Step 5: Commit**

```bash
git add -A src/Net src/Frame/model.c test/
git commit -m "feat: model client rides the async transport; sync http client retired"
```

---

### Task 6: Live gate + full verification + Atlas evidence

**Files:**
- Modify: `atlas/workflow.json` (+ rebuilt `atlas/atlas.html`), nothing else

- [ ] **Step 1: Live gate through the async transport**

```bash
SA_TEST_OLLAMA_URL=http://127.0.0.1:11434 SA_TEST_OLLAMA_MODEL=gemma4:latest \
  setarch -R ctest --test-dir cmake-build-debug -R TestLiveLoop --output-on-failure 2>/dev/null | tail -4
```
Expected: PASS (the model round-trip rides http_client_submit → loop thread →
completion record → model decode). If the local model narrates instead of
acting, the gate's own fresh-frame reruns absorb it — rerun once before
concluding failure.

- [ ] **Step 2: All three configs green**

ON (cmake-build-debug) and ASan (cmake-build-asan) per Task 5's commands; OFF:
`cmake -S . -B cmake-build-off -DSA_ENABLE_PYTHON=OFF -DSA_ENABLE_WDB=OFF
-SA_ENABLE_STREAMS=OFF -DSA_BUILD_TESTS=ON`, build, run — the OFF archive
stays sans streams/wavedb/python (the new sources are all inside gates).

- [ ] **Step 3: Valgrind across the moved surfaces (stripped copies, verified-executing)**

Filters: `TestStreamsClient.*:TestStreamsServer.*:TestModelDecode.*:TestFrame.*`
(0 errors, 0 definitely-lost; if TestFrame's possibly-lost glibc-TLS contexts
appear, they are the documented vg-wavedb.supp class).

- [ ] **Step 4: Atlas evidence**

`atlas/workflow.json`: the `drive-model-driven-frame-tree` description's
transport sentence changes to the async story (sync client retired; model
completions ride the loop thread; the worker-yield restructure named as the
orchestration slice's work); add the ported-module evidence lines (pins,
server loopback tests, framing tests re-pinned, config counts) and Windows
status HONESTLY: "Windows/IOCP code paths compile-gated; Windows verification
needs a Windows environment — recorded pending" (matching how the frame-tree
evidence handled it). Run `node build.js && node build.js --check && node
validate.js` from `atlas/`; commit whatever rebuild emits.

- [ ] **Step 5: Commit(s)**

```bash
git add atlas/workflow.json atlas/atlas.html
git commit -m "docs: model transport rides the ported streams stack"
```

---

## Acceptance criteria (whole plan)

1. All configs green (ON / ASan under `setarch -R` / OFF with no streams).
2. Valgrind clean on the new module's suites (verified-executing runs only);
   ASan green on the full suite.
3. The live gate passes against local Ollama through the async client.
4. The express-like server core answers a loopback route test battery.
5. All Task-8-era framing hardening re-pinned on the async client.
6. Atlas evidence updated truthfully; no TODOs anywhere touched; atomic
   conventional commits.

## Known pending (recorded, not built here)

- Windows verification (needs a Windows toolchain/environment; iocp.c +
  regex shim + pcre2 path are compile-gated so the Linux build stays green).
- The turn loop's worker-yield restructure (orchestration slice, spec parked).
- auth_middleware routes content (desktop slice).