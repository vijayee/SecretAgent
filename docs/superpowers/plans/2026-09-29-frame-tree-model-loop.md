# Frame Tree + Model-Driven Control over WaveDB — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A model-driven agent frame tree on the extracted actor runtime: model client (OpenAI-compatible/Ollama) drives a turn loop whose only tool is a Python cell; four bridge verbs (`remember/recall/spawn/report`) are actor behaviors; all state lives as WaveDB subtrees of one root database per process with one atomic root batch per effect; ends with a demo CLI running against local Ollama.

**Architecture:** `src/Frame/` (frame store + loop + model client) + `src/Util/json.{h,c}` (minimal JSON codec the log format needs) + `src/Net/http.{h,c}` (minimal HTTP/1.1 client for the model endpoint) + `src/Python/py_agent.c` (the four verbs added to the injected module) + `tools/frame-demo/`. WaveDB enters as a pinned submodule built by ExternalProject. Chunky `.c` files are written to the frozen contracts in this plan; small files and tests are embedded in full (the pyrt plan's proven structure).

**Tech Stack:** C11, WaveDB (submodule, CMake), CPython 3.12.13 subinterpreters (existing), GoogleTest, the extracted actor runtime.

**Spec:** `docs/superpowers/specs/2026-09-29-frame-tree-model-loop-design.md`. **References:** `docs/wavedb-exploration.md` (verified WaveDB facts — READ FIRST), `docs/design-answers.md` §5-7, `docs/STYLE_GUIDE.md` + good-actors payload rules.

**Frozen boundaries (hard rules):** never modify anything under `liboffs/`, `WaveDB/`, `deepseek-harness/`, `prime-agent/`, `onyx/`, `claude-code-source-code/`. `src/Actor`, `src/Scheduler`, `src/RefCounter`, `src/Util` are frozen EXCEPT where a task says (json.c is a NEW Util member, additive). All ctest runs: `setarch -R ctest --test-dir cmake-build-debug ...`. All test binaries: `cmake-build-debug/test/testsecretagent`. Conventional commits, no Co-Authored-By, never leave TODOs. Escalate on any ambiguity you cannot resolve from these documents rather than inventing behavior.

## File structure

```
deps/wavedb/                           (Task 1: submodule pinned 9c87530 + nested submodule init)
CMakeLists.txt                         (Task 1: wavedb-ext ExternalProject + SA_HAS_WDB define + link)
src/Util/json.{h,c}                    (Task 2: minimal JSON codec — encode/decode)
src/Frame/frame_messages.h             (Task 3: frame message types + payloads)
src/Frame/frame.{h,c}                  (Tasks 4-6: frame store, seq restore, batches, spawn/report/join, behaviors)
src/Python/py_agent.{h,c}              (Task 7: four verb methods on the injected module)
src/Net/http.{h,c}                     (Task 8: minimal HTTP/1.1 POST client)
src/Frame/model.{h,c}                  (Task 9: OpenAI-compatible client over http)
src/Frame/loop.{h,c}                   (Task 10: turn engine + context derivation)
tools/frame-demo/main.c                (Task 11: demo CLI)
test/test_frame.cpp, test/test_json.cpp, test/test_http.cpp, test/test_loop.cpp  (Tasks 2-10)
test/CMakeLists.txt                    (each task: add sources)
atlas/workflow.json                    (Task 12: evidence updates)
```

---

### Task 1: WaveDB as a dependency

**Files:**
- Create: `deps/wavedb` (submodule pinned at `9c87530`)
- Modify: `CMakeLists.txt`, `.gitignore` (nothing — build lands in the binary dir)

- [ ] **Step 1: Add the submodule + nested init**

```bash
git submodule add https://github.com/vijayee/WaveDB.git deps/wavedb
git -C deps/wavedb submodule update --init   # googletest, hashmap, libcbor, xxhash
```
Expected: `.gitmodules` gains the entry; nested submodules populate. Commit the pins exactly as WaveDB's HEAD recorded them (`9c87530` at read time). If the URL is unreachable offline, copy from the local symlink instead and report the deviation.

- [ ] **Step 2: Inspect WaveDB's build options (gate before wiring)**

```bash
grep -n "option(" deps/wavedb/CMakeLists.txt | head -12
```
Record every option name (tests/examples/bindings toggles). The ExternalProject invocation below uses `-D<WDB>_BUILD_TESTS=OFF`-style flags — adapt to the REAL option names found here and record the mapping in the commit message.

- [ ] **Step 3: Wire CMake**

Insert into root `CMakeLists.txt` after the `SA_ENABLE_PYTHON` block, before `add_library(secretagent STATIC ${C_SRC})`:

```cmake
# WaveDB: the durable floor (embedded HBTrie/MVCC/WAL/subtree store). Built
# from the pinned submodule via ExternalProject (same pattern as cpython-ext).
option(SA_ENABLE_WDB "Build the WaveDB-backed frame store" ON)
if(SA_ENABLE_WDB)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/wavedb/CMakeLists.txt)
    message(FATAL_ERROR "deps/wavedb submodule missing. Run: git submodule update --init --recursive")
  endif()
  include(ExternalProject)
  set(WDB_ROOT    ${CMAKE_CURRENT_SOURCE_DIR}/deps/wavedb)
  set(WDB_BUILD   ${CMAKE_BINARY_DIR}/deps/wavedb)
  # OPTIONS: replace the -D flags with WaveDB's REAL option names (Task 1 Step 2).
  set(WDB_CONFIGURE ${CMAKE_COMMAND} -S ${WDB_ROOT} -B ${WDB_BUILD}
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -D<wdb-tests-option>=OFF -D<wdb-examples-option>=OFF -D<wdb-bindings-option>=OFF)
  ExternalProject_Add(wavedb-ext
    SOURCE_DIR ${WDB_ROOT}
    CONFIGURE_COMMAND ${WDB_CONFIGURE}
    BUILD_COMMAND ${CMAKE_COMMAND} --build ${WDB_BUILD} --target wavedb
    INSTALL_COMMAND ${CMAKE_COMMAND} -E echo "Skipping install; consuming build-dir artifact"
    BUILD_BYPRODUCTS ${WDB_BUILD}/libwavedb.a
    BINARY_DIR ${WDB_BUILD}
  )
endif()
```

And after the `SA_ENABLE_PYTHON` link block:

```cmake
if(SA_ENABLE_WDB)
  target_compile_definitions(secretagent PUBLIC SA_HAS_WDB=1)
  target_include_directories(secretagent PRIVATE ${WDB_ROOT}/src)
  target_link_libraries(secretagent PUBLIC ${WDB_BUILD}/libwavedb.a)
  add_dependencies(secretagent wavedb-ext)
endif()
```

(The `wavedb` target's own public headers include project-internal dirs — if linking fails on an include path, add `target_include_directories(secretagent PRIVATE ${WDB_BUILD}/...)` for the generated config headers WaveDB produces, and record it.)

- [ ] **Step 4: Build + verify the artifact**

```bash
cmake -S . -B cmake-build-debug -DSA_BUILD_TESTS=OFF && cmake --build cmake-build-debug -j
```
Expected: `cmake-build-debug/deps/wavedb/libwavedb.a` exists; the library builds. (If WaveDB's build needs `cxx17` set internally, that is its own project's business — do not modify the submodule.)

- [ ] **Step 5: Commit**

```bash
git add .gitmodules CMakeLists.txt deps/wavedb
git commit -m "build: add WaveDB submodule, ExternalProject wiring, and store feature gate"
```

---

### Task 2: Minimal JSON codec (`src/Util/json.{h,c}`)

**Files:**
- Create: `src/Util/json.h`, `src/Util/json.c`
- Test: `test/test_json.cpp` (registered like test_pyrt.cpp: `if(SA_HAS_JSON)` is NOT gated — always compiled)

The log format needs: encode flat objects + strings/numbers/bools; decode into a tiny DOM; no streaming, no external deps. Scope limit: this is a JSON *record* codec (flat-ish objects, arrays of scalars, nested objects to depth 8), NOT a general-purpose JSON library.

- [ ] **Step 1: Write `src/Util/json.h` (frozen API)**

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_JSON_H
#define SA_JSON_H

#include <stddef.h>
#include <stdint.h>

/* Minimal JSON codec for event records. Values are one JSON document.
   Escapes supported: \" \\ \/ \b \f \n \r \t \uXXXX (BMP). */
typedef struct json_value_t json_value_t;

/* Types */
typedef enum json_type_e {
  JSON_NULL = 0, JSON_BOOL, JSON_INT, JSON_DOUBLE, JSON_STRING, JSON_OBJECT, JSON_ARRAY
} json_type_e;

/* Parse: returns NULL on malformed input; *error_msg (optional) gets a
   one-line reason (heap-owned, free() it). */
json_value_t* json_parse(const char* text, size_t len, char** error_msg);

/* Serialize one value into a freshly malloc'd NUL-terminated string. */
char* json_serialize(const json_value_t* value);

/* Accessors (borrowed): */
json_type_e json_type(const json_value_t* v);
int64_t     json_as_int(const json_value_t* v);
double      json_as_double(const json_value_t* v);
int         json_as_bool(const json_value_t* v);
const char* json_as_string(const json_value_t* v);
size_t      json_size(const json_value_t* v);            /* object/array length */
json_value_t* json_get(const json_value_t* obj, const char* key);   /* NULL if absent */
json_value_t* json_at(const json_value_t* array, size_t index);

/* Construction (heap-owned values; json_value_destroy frees recursively): */
json_value_t* json_new_null(void);
json_value_t* json_new_bool(int b);
json_value_t* json_new_int(int64_t i);
json_value_t* json_new_double(double d);
json_value_t* json_new_string(const char* s);            /* copies */
json_value_t* json_new_object(void);
json_value_t* json_new_array(void);
int json_object_set(json_value_t* obj, const char* key, json_value_t* value);   /* takes value */
int json_array_append(json_value_t* array, json_value_t* value);                /* takes value */

void json_value_destroy(json_value_t* v);

#endif // SA_JSON_H
```

- [ ] **Step 2: Write the failing tests (`test/test_json.cpp`, registered in test/CMakeLists.txt next to test_pyrt.cpp — add `test_json.cpp` to the add_executable list unconditionally)**

```cpp
//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/Util/json.h"
}

TEST(TestJson, TestRoundTripFlatObject) {
  json_value_t* o = json_new_object();
  json_object_set(o, "seq", json_new_int(42));
  json_object_set(o, "type", json_new_string("cell.result"));
  json_object_set(o, "ok", json_new_bool(1));
  json_object_set(o, "empty", json_new_null());
  char* text = json_serialize(o);

  char* err = NULL;
  json_value_t* back = json_parse(text, strlen(text), &err);
  ASSERT_NE(back, nullptr) << (err ? err : "no error message");
  EXPECT_EQ(json_as_int(json_get(back, "seq")), 42);
  EXPECT_STREQ(json_as_string(json_get(back, "type")), "cell.result");
  EXPECT_EQ(json_as_bool(json_get(back, "ok")), 1);
  EXPECT_EQ(json_type(json_get(back, "empty")), JSON_NULL);

  free(text);
  json_value_destroy(o);
  json_value_destroy(back);
}

TEST(TestJson, TestParseMalformedIsNotNullNoCrash) {
  const char* bad[] = {"{\"a\":}", "{\"a\" 1}", "[1, 2,]", "{\"a\":\"\\x\"}"};
  for (const char* text : bad) {
    char* err = NULL;
    json_value_t* v = json_parse(text, strlen(text), &err);
    EXPECT_EQ(v, nullptr) << "input: " << text;
    if (err) free(err);
  }
}

TEST(TestJson, TestEscapesAndUnicode) {
  const char* in = "{\"s\":\"line\\nbreak \\u00e9 \\\"q\\\" \\\\\"}";
  char* err = NULL;
  json_value_t* v = json_parse(in, strlen(in), &err);
  ASSERT_NE(v, nullptr);
  const char* s = json_as_string(json_get(v, "s"));
  ASSERT_NE(s, nullptr);
  EXPECT_NE(strstr(s, "\n"), nullptr);
  EXPECT_NE(strstr(s, "é"), nullptr);
  json_value_destroy(v);
}

TEST(TestJson, TestArraysAndNesting) {
  const char* in = "{\"tools\":[\"a\",\"b\"],\"ctx\":{\"depth\":2},\"rows\":[{\"n\":1},{\"n\":2}]}";
  char* err = NULL;
  json_value_t* v = json_parse(in, strlen(in), &err);
  ASSERT_NE(v, nullptr);
  ASSERT_EQ(json_size(json_get(v, "tools")), 2u);
  EXPECT_STREQ(json_as_string(json_at(json_get(v, "tools"), 1)), "b");
  EXPECT_EQ(json_as_int(json_get(json_get(v, "ctx"), "depth")), 2);
  ASSERT_EQ(json_size(json_get(v, "rows")), 2u);
  json_value_destroy(v);
}
```

- [ ] **Step 3: Red → implement → green**

Run: `cmake --build cmake-build-debug -j` (expect undefined `json_parse`). Then implement `json.c` to the contract (recursive-descent parser, DOM with contiguous per-type storage or linked values — implementer's choice, but NO realloc-into-shared pointers). Then green:

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add src/Util/json.h src/Util/json.c test/test_json.cpp test/CMakeLists.txt
git commit -m "feat: minimal JSON codec for event records"
```

---

### Task 3: Frame messages + event schema (`src/Frame/frame_messages.h`)

**Files:**
- Create: `src/Frame/frame_messages.h`

Full listing — the frozen event/message vocabulary:

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_FRAME_MESSAGES_H
#define SA_FRAME_MESSAGES_H

#include <stdint.h>

/* Frame-layer message types (carried in the generic message_t envelope). */
typedef enum frame_message_type_e {
  FRM_REMEMBER = 0,     /* cell -> frame: durable state write */
  FRM_RECALL,           /* cell -> frame: resolve a key up the lineage chain */
  FRM_REPLY,            /* frame -> pyrt: corr-matched answer to a bridge request */
  FRM_CELL_EXECUTE,     /* loop -> frame: run this cell (from the model's tool call) */
  FRM_STOP              /* control: end the frame when the queue drains */
} frame_message_type_e;

/* Event types (stored at sessions/<sid>/events/<seq>, JSON, %020d seq). ONLY
   msg.append produces context. */
typedef enum frame_event_type_e {
  EV_MSG_APPEND = 0,    /* payload {role, content} */
  EV_FRAME_SPAWN,       /* payload {child_sid, goal, depth} */
  EV_FRAME_REPORT,      /* payload {child_sid, text} — the one context-producing child event */
  EV_FRAME_JOIN,        /* payload {child_sid} */
  EV_CELL_RUN,          /* payload {code, corr} */
  EV_CELL_RESULT,       /* payload {corr, status, text} */
  EV_STATE_REMEMBER,    /* payload {key, value} */
  EV_CONTROL            /* payload {kind, text} — interrupt/shutdown/error */
} frame_event_type_e;

/* Bridge request payloads (ownership transfers with the message): */
typedef struct frm_remember_payload_t { uint64_t corr; char* key; char* json_value; } frm_remember_payload_t;
/* reply: corr + status + heap text */
typedef struct frm_reply_payload_t { uint64_t corr; uint8_t status; char* text; } frm_reply_payload_t;
void frm_remember_payload_destroy(void* p);
void frm_reply_payload_destroy(void* p);

/* JSON event record shape (authoritative):
   {"seq":<int>,"type":"<event-name>","frame":"<sid-path>","corr":<int|null>,
    "at":"<iso>","cause":<int|null>,"payload":{...}}  */

#endif // SA_FRAME_MESSAGES_H
```
(Payload destroyer definitions live in frame.c. Add `test/CMakeLists.txt`: nothing — frame tests come with Task 4.)

- [ ] **Step 1: Commit**

```bash
git commit -m "feat: frame message vocabularies and event record schema"
```

---

### Task 4: Frame store (`src/Frame/frame.{h,c}`)

**Files:**
- Create: `src/Frame/frame.h` (frozen API below), `src/Frame/frame.c`
- Test: `test/test_frame.cpp` (full listings below; registered in test/CMakeLists.txt under `if(SA_HAS_WDB) target_sources(...)`)

- [ ] **Step 1: Write `src/Frame/frame.h` (frozen API — signatures never change in later tasks)**

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_FRAME_H
#define SA_FRAME_H

#include "../Actor/actor.h"
#include "../Player-Nothing.h"   /* NEVER actually included — see Note */
#include <stddef.h>
#include <stdint.h>
```
(The line above is a deliberate trap from an earlier draft — REMOVE it. The real file:)

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_FRAME_H
#define SA_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef SA_HAS_WDB
#include "../Actor/actor.h"

typedef struct wave_database_root_t wave_database_root_t;   /* opaque; owns ONE root db */

/* Config (immutable after create): */
typedef struct frame_config_t {
  const char* model_base_url;    /* e.g. http://127.0.0.1:11434 (Ollama) */
  const char* model_api_key;     /* may be NULL/empty for Ollama */
  const char* model_name;        /* e.g. a local model tag */
  unsigned max_depth;            /* SA_FRAME_MAX_DEPTH equivalent (default 4) */
} frame_config_t;

wave_database_root_t* wave_db_open(const char* location /* NULL = in-memory */);
void wave_db_close(wave_database_root_t* db);

typedef struct frame_t frame_t;

/* parent NULL = top-level session (sid generated); spec may be NULL for root. */
frame_t* frame_create(wave_database_root_t* db, frame_t* parent,
                      const char* goal, const frame_config_t* cfg);
const char* frame_sid(const frame_t* f);            /* full subtree path, e.g. sessions/<sid> */
uint8_t frame_is_done(const frame_t* f);
void frame_destroy(frame_t* f);

/* Store operations (called by frame behaviors; ONE root batch per effect): */
int frame_remember(frame_t* f, const char* key, const char* json_value);   /* state/local + event */
/* Resolve: own local/ first, then ctx/ up the lineage chain (shadowing). */
char* frame_recall(frame_t* f, const char* key);        /* malloc'd JSON text; NULL if unresolvable */
int frame_append_msg(frame_t* f, const char* role, const char* content);   /* msg.append event */

#endif /* SA_HAS_WDB */

#endif // SA_FRAME_H
```

(If `wave_db_open`'s real WaveDB config plumbing needs `database_config_t` — build it inside frame.c via `database_create_with_config` with `sync_only = 0`; the store layer hides WaveDB types from callers entirely.)

- [ ] **Step 2: Contract for `frame.c` (the whole implementation follows this; acceptance = the tests below + zero warnings):**

| Concern | Contract |
|---|---|
| Subtrees | Root db opened ONCE per `wave_database_root_t`; `sessions/<sid>/*` subtree prefix composed as full path. frame sid generation: 8-hex random (see `identifier` naming in WaveDB research; `rand` from the platform). Children: `sessions/<parent-sid>/frames/<child8hex>` — composed at the ROOT, no nested subtrees |
| Boot/seq restore | `frame_create` reverse-scans the frame's events (`database_scan_start_reverse`) for the largest `%020d` seq; seq counter continues past it (restart correctness) |
| Event append | `events/<%020llu>`, value = JSON record (`json_serialize`), one root batch per append: event put + (`EV_STATe_REMEMBER` variants also put the state key + lineage triple ops when spawned). Batch size cap honored: > 120KB → fail-loud (`log_error` + return -3), never silent truncation |
| Batch atomicity | Every effect (append/remember/spawn/report/join) = ONE `database_batch_sync_raw` (concurrent mode, `sync_only = 0` always) |
| ctx/local semantics | `frame_remember` writes `state/local/<key>` + `EV_STATE_REMEMBER`; `frame_recall` resolves own `local/`, then own `ctx/` (`database_get_sync` on the subtree), then walks `meta/parent` lineage upward reading ONLY `state/ctx/` keys (per-hop subtree open + get; bounded by depth). `meta/parent` read from `meta/parent` key value |
| Actor | `frame_t` embeds `actor_t` FIRST; dispatch handles `FRM_*` message types from the bridge + `FRM_CELL_EXECUTE` from the loop; payload destroyers per frame_messages.h; NULL pool for test-driven frames (pumped by hand), scheduler pool for live ones |
| Ownership | Every `get_memory/get_clear_memory/strdup` has one owner; payloads use the destroyers; no leaks (valgrind gate) |

- [ ] **Step 3: Write the failing tests first (`test/test_frame.cpp`; full content — the harness + four tests, then implement)**

```cpp
//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <string>
extern "C" {
#include "../src/Frame/frame.h"
#include "../src/Util/json.h"
}

#ifdef SA_HAS_WDB

static frame_config_t test_config(void) {
  frame_config_t cfg;
  cfg.model_base_url = NULL;
  cfg.model_api_key = NULL;
  cfg.model_name = "unused";
  cfg.max_depth = 4;
  return cfg;
}

TEST(TestFrame, TestCreateRootAndChildGeneratesPaths) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);   /* in-memory */
  frame_t* root = frame_create(db, NULL, "do the dishes", &cfg);
  ASSERT_NE(root, nullptr);
  EXPECT_STREQ(strncmp(frame_sid(root), "sessions/", 9) == 0 ? frame_sid(root) : "", frame_sid(root));
  EXPECT_NE(strlen(frame_sid(root)), 9u);          /* a real sid followed */

  frame_t* child = frame_create(db, root, "scrub plate", &cfg);
  ASSERT_NE(child, nullptr);
  std::string child_sid = frame_sid(child);
  EXPECT_NE(child_sid.find(root_sid_g) == std::string::npos, false) << "child path composes from root";

  frame_destroy(child);
  frame_destroy(root);
  wave_db_close(db);
}
```
NOTE: the snippet above references `root_sid_g` — an intentional leftover; the shipped test stores `std::string root_sid = frame_sid(root)` and checks `child_sid.rfind(root_sid, 0) == 0` (prefix composition). Ship THAT version.

```cpp
TEST(TestFrame, TestRememberRecallAndShadowing) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* root = frame_create(db, NULL, NULL, &cfg);
  frame_t* child = frame_create(db, root, NULL, &cfg);

  EXPECT_EQ(frame_remember(root, "temperature", "20"), 0);
  /* child resolves the inheritable parent key... (NOTE: remember writes
     local/ — for an INHERITABLE write use the ctx variant:) */
  /* Real contract: frame_remember_ctx(root, "temperature", "20") writes ctx/ */
  char* v = frame_recall(child, "temperature");
  ASSERT_NE(v, nullptr) << "inherited via ctx lineage walk";
  EXPECT_STREQ(v, "20");
  free(v);

  /* Shadow: child writes its own ctx key; recall stops at the shadow. */
  EXPECT_EQ(frame_remember_ctx(child, "temperature", "33"), 0);
  v = frame_recall(child, "temperature");
  EXPECT_STREQ(v, "33");
  free(v);

  /* local/ never inherits: */
  EXPECT_EQ(frame_remember_local(root, "scratch", "1"), 0);
  v = frame_recall(child, "scratch");
  EXPECT_EQ(v, nullptr);
  free(NULL);

  frame_destroy(child);
  frame_destroy(root);
  wave_db_close(db);
}
```
NOTE: the real API exposes THREE remember variants (`frame_remember_local`, `frame_remember_ctx`) — adjust `frame.h` to match (the design's `ctx/local` split). The shipped header:

```c
int frame_remember_local(frame_t* f, const char* key, const char* json_value);
int frame_remember_ctx(frame_t* f, const char* key, const char* json_value);
char* frame_recall(frame_t* f, const char* key);
```

```cpp
TEST(TestFrame, TestMessageAppendAndSeqContinue) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  EXPECT_EQ(frame_append_msg(f, "user", "hello"), 0);
  EXPECT_EQ(frame_append_msg(f, "assistant", "hi"), 0);

  /* Restart: destroy the frame, reopen the same location-less db is NOT
     possible for an in-memory db — this variant uses a temp dir: */
  wave_db_close(db);
  db = wave_db_open(NULL);
  frame_t* f2 = frame_create(db, NULL, NULL, &cfg);
  /* in-memory db does NOT persist: the RESTART variant of this test lives in
     Task 10 (restart/replay) with a real (disk) location. Here just assert
     seq counting works within one frame: */
  EXPECT_EQ(frame_append_msg(f2, "user", "again"), 0);
  frame_destroy(f2);
  wave_db_close(db);
}
```
NOTE: seq-continuation-across-restart assertions belong to Task 10 with a disk-backed temp dir (`mkdtemp`). This test only checks in-frame seq monotonic behavior.

- [ ] **Step 4: Red → implement → green (as in pyrt's tasks); valgrind gate on TestFrame.* (stripped-copy workaround)**

- [ ] **Step 5: Commit**

```bash
git add src/Frame/frame.h src/Frame/frame.c test/test_frame.cpp test/CMakeLists.txt
git commit -m "feat: frame store on WaveDB subtrees with lineage-walk recall"
```

---

### Task 5: Spawn / report / join + lineage triples (extend frame.c)

**Files:**
- Modify: `src/Frame/frame.h` (additions: `frame_spawn`, `frame_report`, `frame_join`)
- Test: `test/test_frame.cpp` (appended tests)

New API (frozen):

```c
/* Admission-only spawn (PA semantics): validates depth, creates child subtree,
   ONE root batch: child meta/events + parent's frame.spawn event + lineage
   triple ops. Returns the child frame immediately (its loop is driven
   separately). frame_spawn FAILS (NULL + log_error) if depth exceeded. */
frame_t* frame_spawn(frame_t* parent, const char* goal, const char* context_json);
/* Child-side: ONE root batch: frame.report event in child + bind into parent's
   log (parent side happens via parent-registry pointer on the frame). */
int frame_report(frame_t* child, const char* text);
int frame_join(frame_t* child);   /* frame.join event in the parent; child becomes 'joined' */
```

- [ ] **Step 1: Append the tests (full listing)**

```cpp
TEST(TestFrame, TestSpawnAdmissionOnlyAndDepthCap) {
  frame_config_t cfg = test_config();
  cfg.max_depth = 1;
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* root = frame_create(db, NULL, NULL, &cfg);
  frame_t* child = frame_spawn(root, "sub goal", NULL);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(frame_spawn(child, "too deep", NULL), (frame_t*)NULL) << "depth cap fails loudly, no child";
  frame_destroy(child);
  frame_destroy(root);
  wave_db_close(db);
}

TEST(TestFrame, TestReportBindsOneEventIntoParent) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* root = frame_create(db, NULL, NULL, &cfg);
  frame_t* child = frame_spawn(root, "count things", NULL);
  EXPECT_EQ(frame_report(child, "found 7 things"), 0);

  /* The parent's log gained exactly ONE report event whose payload mentions
     the child; the ROOT's own events are unaffected: */
  /* Parent context derivation (Task 7's function) reads it: */
  EXPECT_EQ(frame_is_done(child), 1) << "report completes the child";

  frame_join(child);
  frame_destroy(child);
  frame_destroy(root);
  wave_db_close(db);
}
```
(The "parent log has exactly one report event" assertion reads the parent's events via the iterator API — the test uses `frame_debug_events(frame_t*)` — ADD this debugging accessor to frame.h: `/* test/debug accessor: malloc'd JSON array of the frame's raw event records (bounded to last 512) */ char* frame_debug_events(frame_t* f);` — and parse it with json_parse in the test, asserting the array length grew by exactly 1 and `payload.child_sid` matches. This is the "one event lands in the parent" proof.)

- [ ] **Step 2: Red → implement → green; valgrind; commit**

```bash
git add src/Frame/frame.h src/Frame/frame.c test/test_frame.cpp
git commit -m "feat: admission-only spawn, report binding, join, and lineage triples"
```

---

### Task 6: Frame behaviors (dispatch wiring for FRM_* + spawn from cells)

**Files:**
- Modify: `src/Frame/frame.c` (dispatch table for FRM_REMEMBER/RECALL via bridge)
- Test: `test/test_frame.cpp`

Contract: bridge-request behaviors apply the SAME store functions synchronously and answer via FRM_REPLY corr-matched to the pyrt module (`py_agent.c`, Task 7). The frame's dispatch handles a message and returns; it NEVER blocks beyond a µs batch.

- [ ] **Step 1: Append test**

```cpp
static void bridge_frame_dispatch(void* state, message_t* msg);
/* The frame tests here use the REAL frame dispatch (exposed for tests via
   frame_dispatch(frame_t*, message_t*)) — pump the frame actor by hand and
   assert the reply event arrives. */
TEST(TestFrame, TestBridgeRememberRecallCorrMatched) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* f = frame_create(db, NULL, NULL, &cfg);

  frm_remember_payload_t* rp = (frm_remember_payload_t*)get_clear_memory(sizeof(frm_remember_payload_t));
  rp->corr = 77;
  rp->key = strdup("mode");
  rp->json_value = strdup("\"fast\"");
  message_t req;
  req.type = FRM_REMEMBER;
  req.payload = rp;
  req.payload_destroy = frm_remember_payload_destroy;
  frame_dispatch(f, &req);   /* behavior runs store + posts FRM_REPLY */

  /* Frame queue now holds FRM_REPLY{corr=77, status=0}: */
  /* drain via actor_run and assert the reply payload's corr: */
  ...assert corr == 77 && status == 0...
  wave_db_close(db);
}
```
(The exact drain/assert shape follows the py_frame pattern from test_pyrt.cpp — a small static completion struct; the implementer ports that shape exactly.)

- [ ] **Step 2: Red → implement → green; commit**

```bash
git add src/Frame/frame.c src/Frame/frame.h test/test_frame.cpp
git commit -m "feat: frame behaviors handle bridge verbs corr-matched"
```

---

### Task 7: py_agent bridge (`src/Python/py_agent.{h,c}`)

**Files:**
- Create: `src/Python/py_agent.h`, `src/Python/py_agent.c`
- Modify: `src/Python/pyrt.c` (include py_agent.h; register `agent.remember/recall/spawn/report` methods on the injected module; keep the `_tls_pyrt` TLS)
- Test: exercised via TestPyrt additions in Task 8's loop tests (bridge needs a live owning frame — the loop tests are where the python cells first call `agent.*`)

Contract:
- `py_agent.h` declares `void py_agent_register(PyMethodDef** methods, size_t* count)` (called from pyrt's module creation to append the verb methods to `log/status/emit`).
- Methods: `remember(key, value)`, `recall(key)`, `spawn(goal, context=None)`, `report(value)`: each parses python args into the frm_* payload (strings, strdup'd out of Python's heap), posts to `_tls_pyrt->owner`'s mailbox corr-matched, and blocks WAITING for the reply with a bounded timeout (500 ms): py_agent waits on a tiny per-request completion record (mutex + cond; the reply behavior signals). Timeouts answer as failures (never deadlock).

WAIT — "blocks waiting" violates the pyrt outbound rule: callbacks must be NON-BLOCKING. Reconciliation: the blocking happens on the PYTHON side of the CELL'S OWN THREAD (the pyrt worker) via a released GIL — the pyrt thread is ALREADY the frame's dedicated python thread, and pyrt's design explicitly permits the pyrt thread to block on its own work queue. The GIL is released during the wait (`Py_BEGIN_ALLOW_THREADS` / `Py_END_ALLOW_THREADS`), so the interpreter is not held; the frame behavior posts the reply into the SAME thread's completion queue. This is PA's synchronous-request shape from its synchronous request round-trips (peer_book precedent in liboffs: `platform_condvar_timed_wait`). DOCUMENT this in py_agent.c's header (why blocking here is acceptable: pyrt thread = dedicated; GIL released; bounded timeout).
- `recall` returns the JSON text (python string) or None; `spawn` returns the child sid string or None; `report` returns True/False.

- [ ] **Step 1: Commit (after loop-test integration in Task 8 proves it — this task implements + compiles + a dedicated unit test with a synthetic owning frame that pre-answers by hand):**

```cpp
TEST(TestPyAgent, TestBridgeMethodsPostCorrMatched) { ... }
```
(inside test_pyrt.cpp or a new test_py_agent.cpp — implementer's choice; registered per testing style.)

```bash
git add src/Python/py_agent.h src/Python/py_agent.c src/Python/pyrt.c test/*
git commit -m "feat: python bridge verbs (remember/recall/spawn/report) on the injected module"
```

---

### Task 8: HTTP client (`src/Net/http.{h,c}`)

**Files:**
- Create: `src/Net/http.h`, `src/Net/http.c`
- Test: `test/test_http.cpp` (a local fake server thread — gtest's std::thread is fine; bind 127.0.0.1 on an ephemeral port)

- [ ] **Step 1: Write `src/Net/http.h` (frozen API)**

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_HTTP_H
#define SA_HTTP_H

#include <stddef.h>
#include <stdint.h>

/* Minimal HTTP/1.1 client for local model endpoints (Ollama/vLLM style).
   One POST, one response; no chunked encoding (we use Content-Length both
   ways); connection per request (fine for turn-scale rates). */
typedef struct http_response_t {
  int status;            /* HTTP code, or -1 (transport error) */
  char* body;            /* heap; free() it */
  size_t body_len;
  char* error;           /* heap; transport-level reason; free() it; NULL on success */
} http_response_t;

http_response_t* http_post_json(const char* url,        /* http://host:port/path */
                                const char* api_key,    /* NULL = no auth header */
                                const char* body_json,  /* NUL-terminated */
                                uint32_t timeout_ms);

void http_response_destroy(http_response_t* r);

#endif // SA_HTTP_H
```

- [ ] **Step 2: Tests (full listing)**

```cpp
//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
extern "C" {
#include "../src/Net/http.h"
}

/* Fake server: one-connection TCP thread that reads the request and replies
   with a canned HTTP/1.1 response. POSIX sockets used directly in test code
   (test-side socket code is allowed; the RUNTIME uses src/Net/http only). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static void _serve_once(int client_fd);
static void fake_server_run(int listen_fd, std::string* seen_body, std::atomic<uint8_t>* seen);

static int fake_server_listen(uint16_t* port_out) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, (struct sockaddr*)&addr, &len);
  *port_out = ntohs(addr.sin_port);
  return fd;
}

TEST(TestHttp, TestPostJsonRoundTrip) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run, listen_fd, &seen_body, &seen);

  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  http_response_t* r = http_post_json(url.c_str(), NULL, "{\"model\":\"m\",\"messages\":[]}", 3000);
  wait for seen...
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->status, 200);
  EXPECT_NE(strstr(r->body, "\"ok\":true"), nullptr);
  EXPECT_NE(seen_body.find("/v1/chat/completions"), std::string::npos);
  EXPECT_NE(seen_body.find("chat/completions"), std::string::npos);   /* the path reached the server */
  ...EXPECT_NE(seen_body.find("Content-Length"), std::string::npos);

  http_response_destroy(r);
  server.join();
}

TEST(TestHttp, TestTransportErrorIsReported) {
  http_response_t* r = http_post_json("http://127.0.0.1:1/x", NULL, "{}", 300);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->status, -1);
  ASSERT_NE(r->error, nullptr);
  http_response_destroy(r);
}
```
(The `_serve_once`/`fake_server_run` bodies are straightforward TCP accept/read/write code written by the implementer: read until `

', reply 200 with a canned body containing "ok":true and a Content-Length; they are part of the test's fixture, not the runtime.)

- [ ] **Step 3: Red → implement → green (implement http.c: parse URL host:port, TCP connect, send POST with headers [Host, Content-Type: application/json, Content-Length, Authorization when key], read headers → parse status + Content-Length → read body); valgrind on TestHttp.*; `setarch -R` ctest green**

- [ ] **Step 4: Commit**

```bash
git add src/Net/http.h src/Net/http.c test/test_http.cpp test/CMakeLists.txt
git commit -m "feat: minimal HTTP/1.1 POST client for model endpoints"
```

---

### Task 9: Model client (`src/Frame/model.{h,c}`)

**Files:**
- Create: `src/Frame/model.h` (frozen API), `src/Frame/model.c`
- Test: additions to test_loop.cpp come with Task 10; here a direct decode test

- [ ] **Step 1: Write `src/Frame/model.h` (frozen API)**

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_MODEL_H
#define SA_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "../Frame/frame.h"   /* frame_config_t carries base_url/key/model */
#include "../Util/json.h"

/* A completion boundary — nothing more. messages = JSON array of
   {role, content} objects; tools = JSON array (ONE execute tool); reply =
   parsed choice. */
typedef struct model_reply_t {
  char* content;         /* heap assistant text ("" when only a tool call) */
  char* tool_code;       /* heap code string from the execute tool call (NULL when none) */
  char* finish_reason;   /* heap, or NULL */
} model_reply_t;

/* vtable so tests inject scripted turns without network: */
typedef struct model_backend_t {
  int (*complete)(void* self, json_value_t* messages, json_value_t* tools,
                  char**, model_reply_t** reply, char** error_out);   /* 0 ok */
} model_backend_t;

model_backend_t* model_http_backend_create(const frame_config_t* cfg);
void model_backend_destroy(model_backend_t* mb);
void model_reply_destroy(model_reply_t* r);

#endif // SA_MODEL_H
```

- [ ] **Step 2: Contract for `model.c`:** POST `{model, messages, tools:[{type:"function", function:{name:"execute", parameters:{code:string}}}], tool_choice:"auto"}` to `<base>/v1/chat/completions` (Ollama-compatible); parse the first choice: `message.content` → content; `message.tool_calls[0].function.arguments` (string or object form, both) → extract `code`; failures → status + error string. NO streaming, no retries here (loop owns retry).

- [ ] **Step 3: Direct decode test (append to test_json.cpp or a new test_model_decode.cpp; implementer choice, registered):** feed a canned completions JSON body through the decode path and assert `tool_code` extraction. 

```bash
git add src/Frame/model.h src/Frame/model.c test/*
git commit -m "feat: OpenAI-compatible completion client with tool-call decode"
```

---

### Task 10: Loop engine (`src/Frame/loop.{h,c}`) — the turn engine + restart/replay

**Files:**
- Create: `src/Frame/loop.h`, `src/Frame/loop.c`
- Test: `test/test_loop.cpp` (full listings below)

- [ ] **Step 1: Write `src/Frame/loop.h` (frozen API)**

```c
//
// Created by victor on 9/29/26.
//

#ifndef SA_LOOP_H
#define SA_FRAME_LOOP_H

#include "../Frame/frame.h"

struct frame_t;

/* Runs the frame's turn loop to completion on the CURRENT thread (the frame's
   own scheduler worker runs it in production; tests drive it inline).
   Returns 0 on clean completion (no tool call), nonzero on error. */
int frame_run_loop(frame_t* f);

#endif // SA_LOOP_H
```

- [ ] **Step 2: Contract for `loop.c`:**

- **Context derivation** (`derive` private): bounded projection = the frame's `msg.append` events replayed in order (each truncated to a per-message cap), + the frame's `state/ctx/` snapshot (bounded count), + one-line-per-child report summaries (from `frame.report` events). Explicit caps constant at top of file; NOTHING materialized to disk.
- **Turn**: derive → model complete (single `execute` tool) → if `tool_code`: `EV_CELL_RUN` event → hand the code to the frame's own pyrt via `FRM_CELL_EXECUTE` (corr-matched reply) → `EV_CELL_RESULT` event → loop again; if content only: `msg.append` assistant event, loop ends (top frame semantics); child frames end on `frame.report`.
- **Steering**: tests/user can append user messages between turns; the loop re-derives.

- [ ] **Step 3: Tests (full listings)**

```cpp
//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "../src/Frame/frame.h"
#include "../src/Frame/model.h"
#include "../src/Frame/loop.h"
#include "../src/Util/json.h"
}

#ifdef SA_HAS_WDB && defined(SA_HAS_PYTHON)
/* NOTE: C preprocessor has no && for macros — the real guard:
   #if defined(SA_HAS_WDB) && defined(SA_HAS_PYTHON)  */

/* Scripted model: pops pre-queued completions. */
typedef struct scripted_model_t {
  model_backend_t base;
  std::vector<std::string>* replies;   /* raw OpenAI-shaped JSON bodies */
} scripted_model_t;

static int scripted_complete(void* self, json_value_t* messages, json_value_t* tools,
                             model_backend_t* mb, model_reply_t** reply, char** error_out) {
  (void)messages; (void)tools;
  scripted_model_t* sm = (scripted_model_t*)self;
  ...parse sm->replies->front() into a model_reply_t; pop; return 0...
}

TEST(TestLoop, TestScriptedLoopRunsCellAndCompletes) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* f = frame_create(db, NULL, "do a thing", &cfg);

  std::vector<std::string> replies = {
    /* turn 1: the model calls execute with a cell that remembers + reports */
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"type\":\"function\","
    "\"function\":{\"name\":\"execute\",\"arguments\":\"{\\\"code\\\":\\\"import actor\\\\nactor.remember('n', 7)\\\\nactor.report('done: ' + str(7))\\\"}\"}}]}}]}",
    /* turn 2: no tool call — loop ends */
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"all done\"}}]}"
  };
  scripted_model_t sm = {...};
  frame_set_model_backend(f, (model_backend_t*)&sm);

  EXPECT_EQ(frame_run_loop(f), 0);
  /* Assert audit trail: the frame's events contain a state.remember for n=7
     and the assistant "all done" msg.append; recall n == 7: */
  char* n = frame_recall(f, "n");
  ASSERT_NE(n, nullptr);
  EXPECT_STREQ(n, "7");
  free(n);
  EXPECT_EQ(frame_is_done(f), 1);

  frame_destroy(f);
  wave_db_close(db);
}
```
(Add `frame_set_model_backend(frame_t*, model_backend_t*)` to frame.h — the loop uses the frame's backend unless overridden.)

RESTART/REPLAY test (Task 10 also carries S003's acceptance):

```cpp
TEST(TestLoop, TestRestartReplayRestoresSeqAndContext) {
  frame_config_t cfg = test_config();
  char* dir = temp_dir_mkdtemp_sa();   /* test helper: mkdtemp wrapper */
  std::string loc = std::string(dir) + "/db";

  wave_database_root_t* db = wave_db_open(loc.c_str());
  frame_t* f = frame_create(db, NULL, "persist me", &cfg);
  frame_append_msg(f, "user", "remember seven");
  frame_remember_ctx(f, "n", "7");
  frame_destroy(f);                      /* durable close */
  wave_db_close(db);

  db = wave_db_open(loc.c_str());
  frame_t* f2 = frame_create(db, NULL, NULL, &cfg);   /* same sid? NO: new sid — restart tests RESUME an existing frame: */
  frame_t* resumed = frame_resume(db, "sessions/<the-recorded-sid>", &cfg);
  /* Assert: seq counter continued (append after resume gets a LATER seq than
     pre-restart events), ctx recall n == 7, msg history replayed: */
  int seq_before = ...read from events count...
  frame_append_msg(resumed, "user", "more");
  ...assert seq ordering strictly increases across the restart...
  EXPECT: recall("n") == "7"
  wave_db_close(db);
}
```
(Add `frame_resume(wave_database_root_t*, const char* sid, const frame_config_t*)` to frame.h — boot-time restore is exactly this. The test's temp-dir helper is a small static helper in the test file.)

- [ ] **Step 4: Red → implement → green; valgrind on the loop/bridge/frame suites (with-python build); ASan config run under setarch -R; commit**

```bash
git add src/Frame/loop.h src/Frame/loop.c src/Frame/frame.h src/Frame/frame.c test/test_loop.cpp test/CMakeLists.txt
git commit -m "feat: turn loop engine with scripted-model tests, restart/replay restore"
```

---

### Task 11: Demo CLI (`tools/frame-demo/main.c`)

**Files:**
- Create: `tools/frame-demo/main.c`
- Modify: root `CMakeLists.txt` (add_executable(frame-demo ...) under `if(SA_ENABLE_WDB)`, link secretagent)

- [ ] **Step 1: Contract (the executable is ~120 lines):** args `--location <dir>` (default `./sa-demo-db`), `--base-url` (default `http://127.0.0.1:11434`), `--model <tag>`, `--goal "<text>"`; opens the root db (create-if-absent, real disk), creates the top frame, runs the loop, streams `log`/`status` lines to stdout as they arrive (frame events → printf), prints the final assistant content. Ctrl-C = interrupt request (signal → frame interrupt event). No daemon, no RPC.

- [ ] **Step 2: Build + smoke** — `cmake --build cmake-build-debug -j` links frame-demo. (Live-run gate is Task 12's opt-in integration test.)

- [ ] **Step 3: Commit**

```bash
git add tools/frame-demo/main.c CMakeLists.txt
git commit -m "feat: frame-demo CLI driving a model-driven frame against local Ollama"
```

---

### Task 12: Opt-in integration + Atlas evidence

**Files:**
- Modify: `atlas/workflow.json`, `test/CMakeLists.txt`

- [ ] **Step 1: Integration test (registered but skipped unless `SA_TEST_OLLAMA_URL` is set):** run one loop against the live endpoint (goal: "remember the word 'wave' then report it"), assert the events show the round trip. Skipped tests must be SKIPPED, not failing.

- [ ] **Step 2: Full verification** — all three configs (ON/ASan/OFF) green; valgrind clean on all pyrt/frame/loop binaries (stripped copies).

- [ ] **Step 3: Atlas** — `drive-model-driven-frame-tree` gains milestone evidence in the description (demo CLI + scripted-loop tests + restart/replay proof); if the restart/replay test exists, `remember-session-across-restarts` also gets its kill-and-resume evidence noted. Rebuild + validate; commit.

---

## Acceptance criteria (whole plan)

1. All configs green (ON; ASan under `setarch -R`; OFF build stays libpython/wavedb-free as gated).
2. Valgrind: no leaks in new code (WaveDB/CPython-owned classes documented as before).
3. The frame tree slice's completion evidence bullets demonstrably hold (demo CLI live run; bridge verbs corr-matched; admission-only spawn; scripted-loop tests; restart/replay proof).
4. Atlas slices updated with evidence; no TODOs; atomic conventional commits throughout.