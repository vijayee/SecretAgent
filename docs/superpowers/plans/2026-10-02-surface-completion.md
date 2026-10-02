# Surface Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the seven-verb surface's remaining gaps — the durable `emit` event, the
`frame_interrupt` entrypoint with the pooled-cell watchdog (interrupt + poison), the
`agent.keys()` verb, one budget table per boundary with truncate-marker-at-source, and
print()-stdout capture in the embedded cell backend.

**Architecture:** All policy lives in `frame.c` dispatch cases (frame-owned seams). The budget
table is a new bottom-layer module `src/Util/budget.{h,c}` every capped site includes. The
watchdog is a short-lived condvar thread per pooled cell that posts a message into the frame's
own mailbox at the deadline. pyrt/py_agent gain only source-side caps, stdout capture, and the
interrupt flag they already have.

**Tech Stack:** C11, the project's actor runtime (src/Actor), WaveDB store actor, CPython 3.12
subinterpreters, GoogleTest (C++17) with `extern "C"` headers.

**Spec:** `docs/superpowers/specs/2026-10-02-surface-completion-design.md` — read it FIRST.

**Standing verification bar (every task's final step):**
- `setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure`
- Valgrind on a STRIPPED copy of the touched test binary (this machine's valgrind can't parse
  the debuginfo; see memory/feedback-env-asan). gtest filters use `SuiteName.TestName`. The
  disk-class tests are documented valgrind-excluded (ASan is their proof).
- ASan opt-in dir `cmake-build-asan` when touching threading (`setarch -R ctest --test-dir cmake-build-asan`).
- Read `docs/STYLE_GUIDE.md` before writing any C.
- Never touch the read-only reference dirs or anything under `deps/` (except nothing here —
  this slice has NO substrate changes).

---

### Task 1: The budget module (Util/budget + test_budget)

**Files:**
- Create: `src/Util/budget.h`
- Create: `src/Util/budget.c`
- Create: `test/test_budget.cpp`
- Modify: `test/CMakeLists.txt` (plain-gate test list)

- [ ] **Step 1: Write the failing tests (`test/test_budget.cpp`)**

```cpp
//
// Created by victor on 10/02/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <string>
extern "C" {
#include "../src/Util/budget.h"
}

TEST(TestBudget, TestCleanCopyCarriesNoMarker) {
  char* out = NULL;
  uint8_t truncated = 7;
  budget_truncate_with_marker("small text", 1024, &out, &truncated);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "small text");
  EXPECT_EQ(truncated, 0);
  free(out);
}

TEST(TestBudget, TestExactBoundaryIsClean) {
  const char* s = "abcdef";
  char* out = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(s, 6, &out, &truncated);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "abcdef");
  EXPECT_EQ(truncated, 0);
  free(out);
}

TEST(TestBudget, TestTruncationAppendsTheMarker) {
  std::string big(100, 'x');
  char* out = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(big.c_str(), 32, &out, &truncated);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(truncated, 1);
  std::string expect(32, 'x');
  expect += "\n[budget: truncated at 100 bytes]";
  EXPECT_STREQ(out, expect.c_str());
  free(out);
}

TEST(TestBudget, TestRefusalsCarryNoMarker) {
  char* out = (char*)1;
  uint8_t truncated = 0;
  budget_truncate_with_marker(NULL, 32, &out, &truncated);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(truncated, 0);
  out = (char*)1;
  budget_truncate_with_marker("x", 0, &out, &truncated);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(truncated, 0);
}

TEST(TestBudget, TestDefaultsArePinned) {
  EXPECT_EQ(SA_BUDGET_CELL_RESULT_BYTES, 32u * 1024u);
  EXPECT_EQ(SA_BUDGET_EMIT_BYTES, 16u * 1024u);
  EXPECT_EQ(SA_BUDGET_BRIDGE_VALUE_BYTES, 16u * 1024u);
  EXPECT_EQ(SA_BUDGET_KEYS_MAX, 256u);
  EXPECT_EQ(SA_FRAME_CELL_WATCHDOG_MS, 300000u);
  EXPECT_EQ(SA_BUDGET_LOOP_MSG_CAP, 4000);
  EXPECT_EQ(SA_BUDGET_LOOP_SNAPSHOT, 500);
  EXPECT_EQ(SA_BUDGET_LOOP_REPORT, 300);
  EXPECT_EQ(SA_BUDGET_LOOP_EMIT, 300);
}
```

- [ ] **Step 2: Register the suite on the PLAIN gate in `test/CMakeLists.txt`**

In the top `add_executable(testsecretagent ...)` list, next to `test_lifecycle.cpp` (the
plain-gate comment block), add a sibling line with its own comment:

```cmake
            # The budget suite is PURE (Util constants + the marker helper):
            # no WDB, no streams — same plain gate as test_lifecycle.
            test_budget.cpp
```

- [ ] **Step 3: Build and run to verify the tests FAIL (budget.c missing)**

Run: `setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug -R TestBudget -V`
Expected: link/build FAILURE — `budget_truncate_with_marker` unresolved / header missing.

- [ ] **Step 4: Write `src/Util/budget.h`**

```c
//
// Created by victor on 10/02/26.
//

/* ONE budget table per boundary (surface-completion spec §4): every byte/entry
   cap in the runtime has a name here. Source caps are applied AT the source
   actor (bounded durable text — the truncate-marker helper is the only
   enforcement); projection caps bound what the derive renders. The transport
   boundary (_HTTP_BODY_MAX / _HTTP_READ_MAX in src/Streams/http_client.c) is
   NOT policy — it stays there, cross-referenced here. */

#ifndef SA_BUDGET_H
#define SA_BUDGET_H

#include <stddef.h>
#include <stdint.h>

/* Source caps (the runtime's own payloads, cut before they become durable). */
#define SA_BUDGET_CELL_RESULT_BYTES (32u * 1024u)
#define SA_BUDGET_EMIT_BYTES (16u * 1024u)
#define SA_BUDGET_BRIDGE_VALUE_BYTES (16u * 1024u)

/* Listing caps. */
#define SA_BUDGET_KEYS_MAX 256

/* The pooled cell's watchdog default (frame_config_t.cell_watchdog_ms;
   0 = disabled). */
#define SA_FRAME_CELL_WATCHDOG_MS 300000u

/* Projection caps (moved verbatim from src/Frame/loop.c; values unchanged). */
#define SA_BUDGET_LOOP_MSG_CAP 4000
#define SA_BUDGET_LOOP_SNAPSHOT 500
#define SA_BUDGET_LOOP_REPORT 300
#define SA_BUDGET_LOOP_EMIT 300

/* Truncate a text to cap bytes, appending the one marker shape ("\
n[budget:
   truncated at <orig> bytes]") when a cut happened. The OUTPUT is
   min(strlen(text), cap) bytes of text + the marker, so the total never
   exceeds cap + strlen(marker) and the marker names the ORIGINAL length.
   Clean copy (no marker) when strlen(text) <= cap; *out_truncated != 0 only
   on a real cut.
   Refusal: *out_text = NULL, *out_truncated = 0 — on NULL text, cap 0, or
   out-of-memory. Refusals are LOUD at the call site (never silent truncation
   to empty). */
void budget_truncate_with_marker(const char* text, size_t cap,
                                 char** out_text, uint8_t* out_truncated);

#endif /* SA_BUDGET_H */
```

- [ ] **Step 5: Write `src/Util/budget.c`**

```c
//
// Created by victor on 10/02/26.
//

#include "budget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ONE marker shape (the spec's pin): a newline + the cut's facts. */
#define _BUDGET_MARKER_FMT "\n[budget: truncated at %llu bytes]"

void budget_truncate_with_marker(const char* text, size_t cap,
                                 char** out_text, uint8_t* out_truncated) {
  if (out_text == NULL || out_truncated == NULL) return;
  *out_text = NULL;
  *out_truncated = 0;
  if (text == NULL || cap == 0) return;

  size_t n = strlen(text);
  if (n <= cap) {
    char* copy = strdup(text);
    if (copy != NULL) {
      *out_text = copy;   /* clean: no marker on fitting text */
    }
    return;
  }

  char marker[64];
  int m = snprintf(marker, sizeof(marker), _BUDGET_MARKER_FMT,
                   (unsigned long long)n);
  if (m < 0 || (size_t)m >= sizeof(marker)) {
    return;   /* the marker refused to compose: a refusal, never a bad shape */
  }
  char* out = (char*)malloc(cap + (size_t)m + 1);
  if (out == NULL) return;
  memcpy(out, text, cap);
  memcpy(out + cap, marker, (size_t)m);
  out[cap + (size_t)m] = '\0';
  *out_text = out;
  *out_truncated = 1;
}
```

- [ ] **Step 6: Build and run the suite — PASS**

Run: `setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug -R TestBudget --output-on-failure`
Expected: 5 tests PASS.

- [ ] **Step 7: Commit**

```bash
git add src/Util/budget.h src/Util/budget.c test/test_budget.cpp test/CMakeLists.txt
git commit -m "feat: the budget table module (Util/budget) with the shared truncate marker"
```

---

### Task 2: The cell-result source cap in pyrt (both backends)

`_pyrt_post_result` takes OWNERSHIP of its text and is the ONE posting point for both the
subinterpreter and subprocess backends (`pyrt.c:143-160`, called at :534 and :596) — the cap
goes there once and covers both.

**Files:**
- Modify: `src/Python/pyrt.c` (`_pyrt_post_result`)
- Modify: `test/test_pyrt.cpp`

- [ ] **Step 1: Write the failing test** — in `test/test_pyrt.cpp`, after the harness
  (`py_frame_*` helpers), add:

```cpp
static void py_frame_drain(py_frame_t* self) {
  /* Pump until the mailbox holds nothing more (the cap posts results the
     same way every other cell does — the drain needs no deadline). */
  while (self->results.size() == 0) py_frame_pump(self, 2000);
}

TEST(TestPyrt, TestResultTextCappedAtSourceWithMarker) {
  py_frame_t* self = py_frame_create();
  /* 100 KiB result through the embedded backend: the source cap cuts it
     before anything durable or routed sees the full text. */
  std::string code = "r = 'x' * 102400\nr";
  py_frame_execute(self, code.c_str());
  py_frame_drain(self);

  ASSERT_EQ(self->results.size(), 1u);
  pyrt_result_payload_t* r = self->results[0];
  ASSERT_NE(r->text, nullptr);
  EXPECT_EQ(r->status, 0);
  /* The bounded shape: cap bytes of text + the marker naming the ORIGINAL. */
  EXPECT_EQ(strlen(r->text), 32u * 1024u);
  EXPECT_EQ(r->text[32u * 1024u], '\n');
  EXPECT_NE(strstr(r->text, "[budget: truncated at 102400 bytes]"), nullptr);

  py_frame_free(self);
}

TEST(TestPyrt, TestSmallResultUnchangedByCap) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "1 + 1");
  py_frame_drain(self);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  ASSERT_NE(self->results[0]->text, nullptr);
  EXPECT_STREQ(self->results[0]->text, "2");
  py_frame_free(self);
}
```

- [ ] **Step 2: Add the include and the cap to `_pyrt_post_result`**

At the top include block of `src/Python/pyrt.c` add:

```c
#include "../Util/budget.h"
```

In `_pyrt_post_result` (currently `if (text == NULL) return;` then the payload build), apply
the cap between the NULL check and the payload build — the function OWNS `text`, and the cap
replaces the free:

```c
static void _pyrt_post_result(pyrt_t* py, uint64_t corr, uint8_t status, char* text) {
  if (text == NULL) return;
  /* The ONE source cap (budget table §4): both backends post through here,
     so the durable/routed result shape is bounded in one place. The marker
     travels with the text — the model learns the cut at its next derive. */
  char* capped = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(text, SA_BUDGET_CELL_RESULT_BYTES, &capped,
                              &truncated);
  free(text);
  if (capped == NULL) {
    /* The budget helper refused (OOM/cap 0): the corr-matched shape is never
       abandoned — post the loud literal instead. */
    capped = strdup("pyrt: the result budget refused the text");
    if (capped == NULL) return;
    truncated = 0;
  }
  (void)truncated;
  /* ... existing payload build continues with `text` replaced by `capped`
     (use its OWN local: rename the incoming parameter's first use or bind
     `text = capped;` right after the ownership exchange above and leave the
     payload build untouched). */
```

Concretely: after the block above, insert `text = capped;` and leave the rest of the function
byte-identical.

- [ ] **Step 3: Build + run the suite**

Run: `setarch -R ctest --test-dir cmake-build-debug -R "TestPyrt.TestResultTextCappedAtSourceWithMarker|TestPyrt.TestSmallResultUnchangedByCap" --output-on-failure`
Expected: PASS. Then the full suite:
`setarch -R ctest --test-dir cmake-build-debug --output-on-failure` — expected all green
(218 existing + the new budget suite + these).

- [ ] **Step 4: Commit**

```bash
git add src/Python/pyrt.c test/test_pyrt.cpp
git commit -m "feat: the cell-result source budget cap in the pyrt result poster"
```

---

### Task 3: stdout capture in the embedded cell helper

**Files:**
- Modify: `src/Python/pyrt.c` (`_PYRT_HELPERS` script, pyrt.c:83-98)
- Modify: `test/test_pyrt.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(TestPyrt, TestPrintedStdoutIsTheResult) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "print('hello world')");
  py_frame_drain(self);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);
  ASSERT_NE(self->results[0]->text, nullptr);
  EXPECT_STREQ(self->results[0]->text, "hello world");
  py_frame_free(self);
}

TEST(TestPyrt, TestStdoutAndReturnValueCompose) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "print('head')\n41 + 1");
  py_frame_drain(self);
  ASSERT_EQ(self->results.size(), 1u);
  ASSERT_NE(self->results[0]->text, nullptr);
  EXPECT_STREQ(self->results[0]->text, "head\n=> 42");
  py_frame_free(self);
}

TEST(TestPyrt, TestExceptionKeepsPartialStdout) {
  py_frame_t* self = py_frame_create();
  py_frame_execute(self, "print('before the crash')\n1 / 0");
  py_frame_drain(self);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 1);
  ASSERT_NE(self->results[0]->text, nullptr);
  const char* text = self->results[0]->text;
  EXPECT_NE(strstr(text, "before the crash"), nullptr);
  EXPECT_NE(strstr(text, "ZeroDivisionError"), nullptr);
  py_frame_free(self);
}
```

- [ ] **Step 2: Replace the `_PYRT_HELPERS` cell executor (pyrt.c:83-98)**

The new script (note the C-escaping: every python `\n` stays `\\n` inside the C string):

```c
static const char _PYRT_HELPERS[] =
    "import actor\n"
    "def __sa_exec_cell(code):\n"
    "    import traceback, io, sys\n"
    "    buf = io.StringIO()\n"
    "    old = sys.stdout\n"
    "    sys.stdout = buf\n"
    "    try:\n"
    "        try:\n"
    "            result = eval(compile(code, '<cell>', 'eval'), globals())\n"
    "        except SyntaxError:\n"
    "            exec(compile(code, '<cell>', 'exec'), globals())\n"
    "            result = None\n"
    "        result_txt = None if result is None else repr(result)\n"
    "        if result is not None:\n"
    "            globals()['_'] = result\n"
    "        sys.stdout = old\n"
    "        out = buf.getvalue()\n"
    "        if out.endswith('\\n'):\n"
    "            out = out[:-1]\n"
    "        if out:\n"
    "            if result_txt is not None:\n"
    "                return 0, out + '\\n=> ' + result_txt\n"
    "            return 0, out\n"
    "        return 0, result_txt if result_txt is not None else ''\n"
    "    except BaseException:\n"
    "        status, text = 1, traceback.format_exc()\n"
    "        sys.stdout = old\n"
    "        out = buf.getvalue()\n"
    "        return status, (out + '\\n' + text) if out else text\n";
```

The doc comment above it gains ONE line: `Printed stdout is captured per cell and IS the
result (the subprocess envelope's parity — spec §5); the last expression's repr joins it as a
closing "=> repr" line when both exist.`

- [ ] **Step 3: Build + run**

Run: `setarch -R ctest --test-dir cmake-build-debug -R TestPyrt --output-on-failure` —
expected PASS (all of TestPyrt; the repr cells of the old tests exercise the no-stdout path).
Full suite green, then commit.

```bash
git add src/Python/pyrt.c test/test_pyrt.cpp
git commit -m "feat: embedded cells capture stdout as the result (subprocess parity)"
```

---

### Task 4: The emit event + the loud ephemeral consumption + the derive's emit lines

**Files:**
- Modify: `src/Python/py_agent.c` (`_py_agent_emit` source cap)
- Modify: `src/Frame/frame.c` (PYRT_EMIT / PYRT_LOG / PYRT_STATUS cases)
- Modify: `src/Frame/loop.c` (derive branch)
- Modify: `test/test_py_agent.cpp`, `test/test_loop.cpp`

- [ ] **Step 1: Write the failing py_agent test** — in `test/test_py_agent.cpp`, an emit
  request arrives at the owning frame and carries the capped text (the durability itself is
  the frame's job — the bridge test only pins the post shape):

```cpp
TEST(TestPyAgent, TestEmitPostsWithThePayload) {
  /* harness: bridge_frame_t in recording mode (answer semantics do not
     matter — PYRT_EMIT is post-and-done, no corr is awaited). But the
     harness's dispatch records FRM_* verbs, not PYRT_* posts: those land in
     the OWNING frame's mailbox (a real frame), which this harness fakes.
     So the honest shape: extend the harness's dispatch with a
     `case PYRT_EMIT:` that records the text into a new
     `std::vector<std::string> emits`, mirroring its PYRT_LOG handling —
     then this test asserts the payload arrived. */
  bridge_frame_t* self = bridge_frame_create();
  ASSERT_EQ(pyrt_execute(self->pyrt,
                         strdup("import actor\nactor.emit('the report text')")),
            0);
  bridge_frame_pump(self);
  ASSERT_EQ(self->results.size(), 1u);
  EXPECT_EQ(self->results[0]->status, 0);

  ASSERT_EQ(self->emits.size(), 1u);
  EXPECT_EQ(self->emits[0], "the report text");
  bridge_frame_free(self);
}
```

ADAPT-NOTE: the harness (`bridge_frame_t`, test_py_agent.cpp:35-180) records `FRM_*` verb
requests; PYRT_* posts land the same way (the owning actor's mailbox) — extend its dispatch
with a `case PYRT_EMIT:` recording into `std::vector<std::string> emits`, mirroring its
`PYRT_LOG` handling byte-for-byte, and remember the clear-alloc teardown rule (the
`py_frame_free` note: drain the vector buffers explicitly). If the real helper names differ
from the test body above, adapt to them — they are stable: `bridge_frame_create`,
`bridge_frame_pump`, `bridge_frame_free`.

- [ ] **Step 2: Write the failing frame/derive test in `test/test_loop.cpp`** — reuse the
  scripted-loop harness EXACTLY as `TestScriptedLoopRunsCellAndCompletes` (test_loop.cpp:296)
  does:

```cpp
TEST(TestLoop, TestEmitIsDurableAndProjected) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "emit it", &cfg);
  ASSERT_NE(f, nullptr);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.emit('emitted: part one')\"}"}}]}}]})json";
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","content":"all done"}}]})json";
  std::vector<std::string> replies = {turn1, turn2};

  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;

  frame_set_model_backend(f, &sm.base);
  EXPECT_EQ(frame_run_loop(f), 0);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  size_t emit_at = (size_t)-1;
  std::string emit_text;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* type_v = json_get(rec, "type");
    if (type_v != NULL && strcmp(json_as_string(type_v), "emit") == 0) {
      emit_at = i;
      json_value_t* p = json_get(rec, "payload");
      ASSERT_NE(p, nullptr);
      emit_text = json_as_string(json_get(p, "text"));
    }
  }
  EXPECT_NE(emit_at, (size_t)-1) << "no emit record landed in the events stream";
  EXPECT_EQ(emit_text, "emitted: part one");
  json_value_destroy(events);

  /* The derive projected it: the captured model request carries the line. */
  ASSERT_EQ(sm.captured.size(), 2u);   /* turn 1's derive + turn 2's */
  EXPECT_NE(sm.captured[1].find("emit: emitted: part one"), std::string::npos)
      << "the emitted text did not re-derive into the model's context";

  frame_destroy(f);
  wave_db_close(db);
}
```

- [ ] **Step 3: The py_agent source cap** — `_py_agent_emit` becomes:

```c
static PyObject* _py_agent_emit(PyObject* self, PyObject* args) {
  (void)self;
  const char* text = NULL;
  if (!PyArg_ParseTuple(args, "s:emit", &text)) {
    return NULL;
  }
  /* The source cap (budget table §4): bounded durable text at the ONE
     boundary — the frame trusts what its runtime posts. A cut is LOUD here
     too: the py-agent surface sees a log line naming the cut. */
  char* capped = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(text, SA_BUDGET_EMIT_BYTES, &capped, &truncated);
  if (capped == NULL) {
    PyErr_NoMemory();
    return NULL;
  }
  pyrt_post_text(PYRT_EMIT, capped);
  if (truncated != 0) {
    pyrt_post_text(PYRT_LOG, "agent.emit: the payload was truncated at the "
                             "emit budget");
  }
  free(capped);
  Py_RETURN_NONE;
}
```

Add `#include "../Util/budget.h"` to py_agent.c's include block. (The `PyArg_ParseTuple`
format `"s:emit"` — add the function name to the error message, mirroring the other verbs'
style.)

- [ ] **Step 4: The frame's explicit pyrt-text cases** — in `frame.c`'s
  `_frame_behavior_impl`, immediately AFTER `case PYRT_RESULT`'s whole block (the
  `#ifdef SA_HAS_PYTHON`-guarded section, before `case FRM_TURN`), add:

```c
    case PYRT_EMIT: {
      /* The write verb's durable half (surface spec §1): ONE emit record in
         the frame's own events stream. Fire-and-post, no lifecycle riders
         (a side effect, never a step boundary) — and mode-indifferent: a
         store batch composes while a cell is pending exactly like the bridge
         verbs' posts. The text is ALREADY capped at source (py_agent's emit
         boundary); the frame trusts bounded text. */
      pyrt_text_payload_t* t = (pyrt_text_payload_t*)msg->payload;
      msg->payload = NULL;
      if (t == NULL || t->text == NULL) {
        log_error("frame: PYRT_EMIT with no payload at '%s'", f->sid_path);
        pyrt_text_payload_destroy(t);
        break;
      }
      json_value_t* payload = json_new_object();
      if (payload == NULL) {
        log_error("frame: out of memory building the emit payload at '%s'",
                  f->sid_path);
      } else {
        json_object_set(payload, "text", json_new_string(t->text));
        if (_frame_event_post_fire(f, "emit", payload) != 0) {
          /* The event post owns the payload on every path — a refusal is the
             store worker's loud log; nothing retries. */
          log_error("frame: the emit event was refused at '%s'",
                    f->sid_path);
        }
      }
      pyrt_text_payload_destroy(t);
      break;
    }
    case PYRT_LOG:
      /* Live narration — messages are verbs, not nouns (DA msgs 4211-4212):
         consumed on receipt, DELIBERATELY not a store record. The
         line lands in the owning process's log only; the derive skips
         log/status forever. */
      if (msg->payload != NULL) {
        pyrt_text_payload_t* t = (pyrt_text_payload_t*)msg->payload;
        if (t->text != NULL) log_info("pyrt-log: %s", t->text);
      }
      break;   /* actor_run's payload_destroy frees the text */
    case PYRT_STATUS:
      /* The current activity — same ephemeral contract as PYRT_LOG. */
      if (msg->payload != NULL) {
        pyrt_text_payload_t* t = (pyrt_text_payload_t*)msg->payload;
        if (t->text != NULL) log_info("pyrt-status: %s", t->text);
      }
      break;
```

- [ ] **Step 5: The derive's emit branch** — in `loop.c`'s Pass-B fold
  (`_loop_projection...`, the chain at loop.c:426-477), add an `else if` after the
  `LIFE_EVENT_REPAIR` branch:

```c
    } else if (strcmp(type_name, "emit") == 0) {
      /* The write verb's durable half, re-derived (spec §1): emitted text
         rides the result ring like cell results — event order, bounded at
         its own projection cap. */
      json_value_t* text_v = json_get(payload, "text");
      if (text_v != NULL) {
        const char* text = json_as_string(text_v);
        char* et = _loop_trunc((text != NULL && text[0] != '\0') ? text
                                                                  : "(empty emit)",
                               SA_BUDGET_LOOP_EMIT);
        size_t line_len = strlen("emit: ") + strlen(et);
        char* line = get_memory(line_len + 1);
        if (line != NULL) {
          snprintf(line, line_len + 1, "emit: %s", et);
          _loop_result_push(result_ring, &nresults, line);
        } else {
          free(et);
        }
      }
```

(Match the neighboring branches' exact local names for the ring/array — they are in scope at
that point in the function; if any differs, adapt to the real ones.)

Then rename loop.c's three projection `#define`s to the table entries: replace
`SA_LOOP_MSG_CAP` → `SA_BUDGET_LOOP_MSG_CAP`, `SA_LOOP_SNAPSHOT_VALUE_CAP` →
`SA_BUDGET_LOOP_SNAPSHOT`, `SA_LOOP_REPORT_LINE_CAP` → `SA_BUDGET_LOOP_REPORT` **throughout
loop.c** (this is Task 9's grep gate applied early to the names the new branch introduces —
do the mechanical rename of ALL uses in this task so loop.c never carries both spellings),
and `#include "../Util/budget.h"` in loop.c's includes. DELETE the three old `#define` lines
and their comments (the WHY comments move to budget.h — already there).

- [ ] **Step 6: Build + run**

Run: `setarch -R ctest --test-dir cmake-build-debug --output-on-failure`
Expected: all green including the two new tests.

- [ ] **Step 7: Commit**

```bash
git add src/Python/py_agent.c src/Frame/frame.c src/Frame/loop.c test/test_py_agent.cpp test/test_loop.cpp
git commit -m "feat: the emit event (the write verb's durable half) + loud ephemeral pyrt text consumption"
```

---

### Task 5: The interrupt seam — `frame_interrupt`, the FRM_INT synthesis, poison

**Files:**
- Modify: `src/Frame/frame_messages.h` (FRM_INT)
- Modify: `src/Frame/frame.h` (the public entry + the poison contract doc)
- Modify: `src/Frame/frame_internal.h` (`_frame_engine_terminate` export)
- Modify: `src/Frame/frame.c` (FRM_INT case + `_frame_interrupt_apply` + poisoned refusal +
  the quiet late-result drop)
- Modify: `src/Frame/loop.c` (un-declare the static, keep the definition)
- Modify: `test/test_loop.cpp`

- [ ] **Step 1: Write the failing test** — in `test/test_loop.cpp` (inline mode; the pool
  comes in Task 6):

```cpp
TEST(TestLoop, TestFrameInterruptCutsCellAndAbortsTurn) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "interrupt me", &cfg);
  ASSERT_NE(f, nullptr);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import time\\nprint('started')\\ntime.sleep(3)\\n'the real result'\"}"}}]}}]})json";

  std::vector<std::string> replies = {turn1};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  /* A second thread delivers the interrupt ~300ms into the sleeping cell.
     DETERMINISTIC ENOUGH: the cell sleeps 3s (pyrt's own thread), the driver
     pumps on this thread, and the interrupt lands deep inside the run — the
     test_pyrt interrupt suite already uses a sleep-gated second thread (READ
     its TestInterrupt FIRST and mirror the gating idiom if it differs —
     but keep the timing margins generous: interrupt at 300ms, cell sleep 3s). */
  std::thread killer([f]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    frame_interrupt(f);
  });
```

STOP — the racing-thread probe above is fragile. Instead, interrupt from a thread gated on
the pyrt cell actually running, using the SAME signal the interrupt test in
`test/test_pyrt.cpp`'s interrupt suite uses — READ THAT TEST FIRST and mirror its
gating idiom exactly. If it gates on a sleep-then-interrupt, an alternative DETERMINISTIC
shape exists: `sm.steer_*` machinery is for msg.append only, so gate on the monotonic clock
WITH a generous cell (sleep 3s) and interrupt at 300ms, then `join` the killer thread
BEFORE asserting. Keep `std::thread` gating: `frame_create` → `frame_start`-free shape is
NOT used here; the run loop is synchronous, so the second thread is genuinely concurrent —
this is a legitimate multi-threaded test (the ASan dir proves the race handling).

Full test body (final):

```cpp
TEST(TestLoop, TestFrameInterruptCutsCellAndAbortsTurn) {
  py_agent_init();
  frame_config_t cfg = test_config();
  cfg.cell_watchdog_ms = 0;   /* deadline watchdog OFF: this is the caller's
                                 interrupt, not the timer's (Task 6 pins the
                                 watcher) */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "interrupt me", &cfg);
  ASSERT_NE(f, nullptr);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import time\\nprint('started')\\ntime.sleep(3)\"}"}}]}}]})json";

  std::vector<std::string> replies = {turn1};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  std::thread killer([f]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    frame_interrupt(f);
  });

  /* The engine ends ABORTED (a failure exit for the loop), never ok. */
  EXPECT_EQ(frame_run_loop(f), 1);
  killer.join();

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  /* The synthesized close rides ONE batch: cell.result(status 1, the
     interrupt text) + step.end + turn.end{aborted}. */
  int saw_result = 0, saw_step_end = 0, saw_turn_end = 0;
  size_t turn_end_at = (size_t)-1;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* type_v = json_get(rec, "type");
    if (type_v == NULL) continue;
    const char* tn = json_as_string(type_v);
    if (strcmp(tn, "cell.result") == 0) {
      json_value_t* p = json_get(rec, "payload");
      if (p != NULL && json_as_int(json_get(p, "status")) == 1 &&
          strstr(json_as_string(json_get(p, "text")), "interrupted") != NULL) {
        saw_result = 1;
      }
    } else if (strcmp(tn, "step.end") == 0) {
      saw_step_end = 1;
    } else if (strcmp(tn, "turn.end") == 0) {
      json_value_t* p = json_get(rec, "payload");
      json_value_t* reason = json_get(p, "reason");
      if (reason != NULL &&
          strcmp(json_as_string(json_get(reason, "kind")), "aborted") == 0) {
        saw_turn_end = 1;
        turn_end_at = i;
      }
    }
  }
  EXPECT_EQ(saw_result, 1);
  EXPECT_EQ(saw_step_end, 1);
  EXPECT_EQ(saw_turn_end, 1);
  ASSERT_NE(turn_end_at, (size_t)-1);
  EXPECT_EQ(turn_end_at, json_size(events) - 1) << "the turn end is the log's newest record";
  json_value_destroy(events);

  /* The poison contract: a resumed turn's cell refuses corr-matched loud. */
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"1\"}"}}]}}]})json";
  std::string turn3 =
      R"json({"choices":[{"message":{"role":"assistant","content":"never called"}}]})json";
  std::vector<std::string> replies2 = {turn2, turn3};
  sm.replies = &replies2;
  EXPECT_EQ(frame_run_loop(f), 0);   /* the refused cell is failure data the
                                        model reads; the content turn ends the
                                        run cleanly */
  frame_destroy(f);   /* destroy on a poisoned frame joins the hung pyrt thread:
                         documented cost — the 3s sleep bounds it here */
  wave_db_close(db);
}
```

(Include `<thread>` and `<chrono>` at the file's include block. `cfg.cell_watchdog_ms`
exists only after Task 6's field — for THIS task, omit that one line and add it back in
Task 6.)

- [ ] **Step 2: The message vocabulary** — in `src/Frame/frame_messages.h`, add to
  `frame_message_type_e` (after `FRM_REPORT_BIND`, which gains a trailing comma):

```c
  FRM_REPORT_BIND,        /* (trailing comma ADDED; the enumeration continues) */
  FRM_INT                 /* interrupt (owner -> the frame's own mailbox): cut
                             the pending cell + close an open turn aborted —
                             the reason union's RESERVED `aborted` first
                             writer. No payload. */
```

(FRM_CELL_WATCHDOG joins the enum in Task 6 — its payload handoff belongs to the watchdog
machinery; keep Task 5's vocabulary minimal.)

- [ ] **Step 3: The public API + config field** — in `src/Frame/frame.h`, after
  `int frame_start(frame_t* f);` (line ~121), add:

```c
/* Interrupt the frame: cut the pending cell (a corr-matched status-1 result
   synthesizes immediately — the cooperative-only pyrt interrupt cannot
   preempt the running interpreter, so the cell's REAL result lands later and
   drops quietly) and close an open turn with the lifecycle's `aborted`
   reason. The frame's runtime is POISONED: every further cell on THIS frame
   refuses corr-matched loud until the frame is torn down (the wedge is
   contained; destroy joins the hung interpreter thread — a documented
   cost). FRM_STOP remains the drain-at-boundary control. */
void frame_interrupt(frame_t* f);
```

- [ ] **Step 4: Frame state fields** — in `frame.c`'s `struct frame_t`, next to the
  cell-slot fields (after `cell_status`), add:

```c
  uint8_t pyrt_poisoned;      /* an interrupt wedged this frame's runtime
                                 (in-memory, per-process: a restart rebuilds a
                                 fresh runtime and the durable log already
                                 carries the aborted turn) */
  uint64_t cell_interrupted_corr;   /* the pyrt corr whose REAL result is
                                       expected late and drops quietly */
```

- [ ] **Step 5: The synthesis + dispatch cases** — in `frame.c`, before
  `_frame_behavior_impl`'s definition (near the other engine helpers), add:

```c
/* The interrupt synthesis (spec §2): ONE mechanism, three callers, their
   own texts. arm_cut = 1 for the frame_interrupt entry (cases may arm
   pyrt's boundary cut); arm_cut = 0 for the DEADLINE callers (the inline
   driver's cell deadline, the pooled watchdog) — a deadline must never arm
   a cut against a FUTURE legitimate cell.
   Case 1 (cell pending): the corr-matched cell.result (status 1) + the
   lifecycle riders [step.end, turn.end{aborted}] in ONE fire-and-post batch.
   Case 2 (turn open, no cell): the riders minus cell.result. Case 3
   (nothing open): arm the boundary cut only (per arm_cut), no store write.
   After cases 1-2: the compose-time facts flip exactly like the cell-result
   close (turn_open/step_open die), the interrupt corr is kept (the REAL
   result drops quietly when it lands), the frame poisons, and the ENGINE
   ends via _frame_engine_terminate (a child binds its failure report with
   the same reason text). A store-stage refusal leaves the pre-allocated seq
   rolled back and the tail to resume-repair, the standing discipline.
   GUARD: this function compiles in every python build AND the no-python one
   — the f->pyrt accesses are #ifdef SA_HAS_PYTHON-wrapped INSIDE the body. */
static void _frame_interrupt_apply(frame_t* f, uint8_t arm_cut,
                                   const char* reason_text) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: interrupt apply on a dead frame");
    return;
  }
  const char* names[3] = {"cell.result", LIFE_EVENT_STEP_END,
                          LIFE_EVENT_TURN_END};
  json_value_t* payloads[3];
  size_t n = 0;
  uint8_t cell_was_pending = f->cell_pending;
  uint8_t with_riders =
      (f->engine.engine_live != 0 && f->engine.turn_open != 0) ? 1 : 0;
  if (cell_was_pending != 0) {
    json_value_t* cell_result = json_new_object();
    if (cell_result == NULL) {
      log_error("frame: out of memory building the interrupt's cell.result at "
                "'%s'", f->sid_path);
      /* The cell slot still closes (the interrupt's contract never waits on
         an OOM); the batch goes out without the result record. */
    } else {
      json_object_set(cell_result, "corr",
                      json_new_int((int64_t)f->cell_corr));
      json_object_set(cell_result, "status", json_new_int((int64_t)1));
      json_object_set(cell_result, "text", json_new_string("pyrt: interrupted"));
      payloads[n++] = cell_result;
    }
  }
  if (with_riders != 0) {
    payloads[n++] = lifecycle_step_json(f->engine.turn_counter, 1);
    payloads[n++] =
        lifecycle_turn_end_json(f->engine.turn_counter, LIFE_REASON_ABORTED,
                                reason_text);
  }
  if (n == 0) {
    if (arm_cut != 0) {
      /* Case 3, true interrupt: the boundary cut, and nothing else. */
#ifdef SA_HAS_PYTHON
      if (f->pyrt != NULL) pyrt_interrupt(f->pyrt);
#endif
    } else {
      /* A deadline firing at nothing-open: the cell completed before the
         deadline's dispatch ran — a benign race, logged once. */
      log_info("frame: the cell deadline fired at an already-complete cell "
               "at '%s' — no-op", f->sid_path);
    }
    return;
  }
  int rc = _frame_event_batch_post_fire(f, names, payloads, n,
                                        "interrupt close");
  if (rc == 0) {
    /* The close POSTED: the compose-time facts follow (the store's records
       stay the truth) — mirrors _frame_engine_result_close_post's discipline
       with the ABORTED reason and the interrupted corr's keep. */
    f->engine.turn_open = 0;
    f->engine.step_open = 0;
    if (cell_was_pending != 0) {
      f->cell_interrupted_corr = f->cell_pyrt_corr;
      f->cell_pending = 0;
      f->cell_status = 1;
    }
    f->pyrt_poisoned = 1;
#ifdef SA_HAS_PYTHON
    if (f->pyrt != NULL) pyrt_interrupt(f->pyrt);
#endif
    _frame_engine_terminate(f, 0, reason_text);
    return;
  }
  log_error("frame: the interrupt close at '%s' was refused pre-post — the "
            "tail stays for resume-repair", f->sid_path);
  _frame_engine_terminate(f, 0, reason_text);
}
```

In `_frame_behavior_impl`'s switch, add (outside any `#ifdef` guard issues — `_frame_engine_terminate`
and lifecycle helpers exist in every WDB build; the case goes between `FRM_REPORT_BIND` and
`default:`):

```c
    case FRM_INT:
      /* The interrupt entry (frame.h's frame_interrupt): the synthesis is ONE
         mechanism with the caller's wording (spec §2). No payload. The
         case runs in python-less builds too — the apply guards its pyrt
         accesses internally; the FRM_INT case is unconditional. */
      _frame_interrupt_apply(f, 1, "aborted: interrupted at the frame's request");
      break;
```

Public entry (with `_frame_post`, whose signature is `(actor_t*, uint32_t type, void* payload,
payload_destroy_fn, const char* label)` — mirror the `FRM_TURN` posts):

```c
void frame_interrupt(frame_t* f) {
  if (f == NULL || f->st == NULL) {
    log_error("frame_interrupt: dead frame");
    return;
  }
  _frame_post(&f->actor, (uint32_t)FRM_INT, NULL, NULL, "interrupt");
}
```

- [ ] **Step 6: Export `_frame_engine_terminate`** — remove `static` from its forward
  declaration at loop.c:637 and its definition at loop.c:1214, and add to
  `frame_internal.h` (near the engine contract comment):

```c
/* End the live engine (loop.c): the failure surface a child's report bind
   rides. The interrupt synthesis on the frame's dispatch thread calls this
   after the synthesized close POSTED (the store's FIFO commits the close
   ahead of the terminate's bind). ok=0: engine_failed + the child's failure
   bind; a top frame just ends. */
void _frame_engine_terminate(frame_t* f, uint8_t ok, const char* text);
```

- [ ] **Step 7: The poisoned refusal + the quiet late drop** — in `FRM_CELL_EXECUTE`'s
  dispatch case, add BEFORE the lazy-boot branch (after the corr/code checks at
  frame.c:2077-2081):

```c
      if (f->pyrt_poisoned != 0) {
        /* The poison contract (spec §2): an interrupted frame's cells refuse
           loud until the frame is torn down. The slot fills synchronously
           (status, no pending) — the loop's wait reads it directly; the
           standing refusal contract, never a new path. */
        log_error("frame: cell corr %llu refused — runtime poisoned by an "
                  "interrupted cell at '%s'",
                  (unsigned long long)cp->corr, f->sid_path);
        f->cell_status = 1;
        frm_cell_payload_destroy(cp);
        break;
      }
```

In `PYRT_RESULT`'s dispatch case, add the expected-late shape as a branch BETWEEN the
normal-completion `if` and the loud unclaimed `else`:

```c
      } else if (f->cell_interrupted_corr != 0 &&
                 r->corr == f->cell_interrupted_corr) {
        /* The interrupted cell's REAL result, late and unclaimable (the
           poison contract): the EXPECTED shape the interrupt synthesis set
           up — a documented quiet drop, distinct from the genuinely-unmatched
           loud error below (spec §2). */
        log_info("frame: the interrupted cell's real result (pyrt corr %llu) "
                 "landed after the synthesis — dropped quietly (the poison "
                 "contract)",
                 (unsigned long long)r->corr);
      } else {
```

- [ ] **Step 8: The inline driver's deadline unification** — in `loop.c`'s phase-deadline
  branch (the `_loop_fail(f, e, kind, NULL)` for `kind = "cell-timeout"`, loop.c:1632), replace
  the CELL-phase branch:

```c
      const char* kind = "store-timeout";
      if (e->phase == FRAME_PHASE_CELL) {
        /* The unification (spec §2): the inline driver's cell deadline runs
           the SAME interrupt synthesis the pooled watchdog posts — the
           corr-matched close + poison — instead of giving up with the cell
           slot left pending. arm_cut = 0: a deadline never arms a cut.
           A batch refusal leaves the turn_open facts untouched, so
           _loop_fail's rider path closes the open tail itself (both branches
           land balanced). */
        _frame_interrupt_apply(f, 0, "aborted: cell exceeded the watchdog deadline");
        _loop_fail(f, e, "cell-timeout", NULL);
        break;
      }
      if (e->phase == FRAME_PHASE_MODEL) kind = "model-await";
      log_error("loop: the engine at '%s' awaited phase %u past its %u ms "
                "deadline — the engine ends failed loud", frame_sid(f),
                (unsigned)e->phase, _loop_phase_deadline_ms(e->phase));
      _loop_fail(f, e, kind, NULL);
      break;
```

`_frame_interrupt_apply` needs a `frame_internal.h` declaration (the loop driver calls it):

```c
/* The interrupt synthesis (frame.c): the corr-matched close + poison + the
   boundary cut, under the caller's reason wording. arm_cut = 1 means a true
   interrupt (the boundary cut arms); 0 = a deadline (never arms). The
   inline driver's cell deadline and FRM_INT/FRM_CELL_WATCHDOG's dispatches
   are its callers. */
void _frame_interrupt_apply(frame_t* f, uint8_t arm_cut, const char* reason_text);
```

- [ ] **Step 9: Build + run**

Run: `setarch -R ctest --test-dir cmake-build-debug --output-on-failure`
Expected: all green including `TestLoop.TestFrameInterruptCutsCellAndAbortsTurn`.
Then the ASan dir for the threading: `setarch -R ctest --test-dir cmake-build-asan -R TestFrameInterrupt --output-on-failure`
(expected green; if the dir is stale, build it first).

- [ ] **Step 10: Commit**

```bash
git add src/Frame/frame_messages.h src/Frame/frame.h src/Frame/frame_internal.h \
        src/Frame/frame.c src/Frame/loop.c test/test_loop.cpp
git commit -m "feat: frame_interrupt entry — corr-matched cell cut + aborted turn + runtime poison"
```

---

### Task 6: The pooled-cell watchdog

**Files:**
- Modify: `src/Frame/frame.h` (`frame_config_t.cell_watchdog_ms`)
- Modify: `src/Frame/frame.c` (the watchdog thread + `FRM_CELL_WATCHDOG` + lifecycle wiring)
- Modify: `test/test_loop.cpp`

- [ ] **Step 1: Write the failing test** — in `test/test_loop.cpp`:

```cpp
TEST(TestLoop, TestPooledWatchdogInterruptsTheDeadCell) {
  py_agent_init();
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);

  frame_config_t cfg = test_config();
  cfg.pool = pool;
  cfg.cell_watchdog_ms = 150;   /* short: the watchdog fires inside the test */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "watchdog me", &cfg);
  ASSERT_NE(f, nullptr);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import time\\ntime.sleep(3)\"}"}}]}}]})json";

  std::vector<std::string> replies = {turn1};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  EXPECT_EQ(frame_start(f), 0);
  EXPECT_EQ(scheduler_pool_wait_for_idle(pool), 0);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  int saw_turn_end = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* type_v = json_get(rec, "type");
    if (type_v == NULL) continue;
    if (strcmp(json_as_string(type_v), "turn.end") != 0) continue;
    json_value_t* p = json_get(rec, "payload");
    json_value_t* reason = json_get(p, "reason");
    if (reason != NULL &&
        strcmp(json_as_string(json_get(reason, "kind")), "aborted") == 0 &&
        strcmp(json_as_string(json_get(reason, "text")),
               "aborted: cell exceeded the watchdog deadline") == 0) {
      saw_turn_end = 1;
    }
  }
  EXPECT_EQ(saw_turn_end, 1);
  json_value_destroy(events);
  EXPECT_EQ(frame_is_done(f), 1);   /* the child-style terminal — a top frame's
                                       poison ends the engine; a hung runtime
                                       joins at destroy (bounded here by the
                                       3s sleep) */

  frame_destroy(f);
  wave_db_close(db);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}
```

MIRROR-NOTE: the pooled tree tests already create + start a pool and drive engines through
`scheduler_pool_wait_for_idle` — grep `TEST(TestFrame, TestPooledTree` / `TestPooled` in
test_frame.cpp/test_loop.cpp and mirror their exact teardown ordering if the one above
differs (the pool stop/destroy order and the frame-lifetime handoff matter).

- [ ] **Step 2: Config field** — in `frame.h`'s `frame_config_t`, after `model_timeout_ms`:

```c
  unsigned cell_watchdog_ms;     /* one RUNNING CELL's bound, ms; fires on a
                                    POOLED frame only (the inline driver's
                                    phase deadline covers the inline shape).
                                    The synthesis is frame_interrupt's, under
                                    the watchdog wording. 0 = disabled. */
```

And in `frame_create`'s copy block (wherever `model_timeout_ms` is copied — grep
`model_timeout_ms` in frame.c for the exact site):

```c
  f->cell_watchdog_ms =
      (cfg != NULL) ? cfg->cell_watchdog_ms : SA_FRAME_CELL_WATCHDOG_MS;
```

Add `unsigned cell_watchdog_ms;` to `struct frame_t` too (next to `model_timeout_ms`).

- [ ] **Step 3: The watchdog machinery** — first the vocabulary: `frame_messages.h` adds
  `FRM_CELL_WATCHDOG` after `FRM_INT` (the enum's trailing comma moves):

```c
  FRM_CELL_WATCHDOG       /* the pooled cell's deadline fired: the SAME
                             interrupt synthesis under the watchdog wording
                             (payload = the watchdog struct, its destroyer
                             frees it on the frame's thread — see frame.c) */
```

Then in `frame.c` (top, after the frame_t struct / bridge sink section), add:

/* --- the pooled cell's watchdog (spec §2) ---------------------------------
   ONE short-lived thread per pooled running cell. The thread waits its
   deadline on a condvar; the result's arrival (the disarm) sets
   disarm_wanted + wakes it under the SAME lock, so the deadline branch and
   the disarm are mutually exclusive decisions on ONE protected state pair
   (disarm_wanted, handed_off) — there is no third outcome. At the deadline
   the thread hands the WHOLE watchdog struct to the frame as a
   FRM_CELL_WATCHDOG message payload (the frame's dispatch is the single
   cleaner — no cross-thread free, no join of a detached thread), sets
   handed_off BEFORE the post, and detaches. The disarm, if it arrives after
   a handoff, drops to the incoming message's destroyer instead of joining. */

typedef struct frame_cell_watchdog_t {
  platform_mutex_t* lock;
  platform_condvar_t* cond;
  platform_thread_t* thread;
  uint8_t disarm_wanted;   /* the result's arrival, under lock */
  uint8_t handed_off;      /* the deadline branch: set BEFORE the post, under
                              lock — the disarm reads it under the SAME lock
                              to decide join-vs-bail */
  uint32_t timeout_ms;
  frame_t* f;              /* BORROWED: the frame outlives an armed watchdog
                              (frame_destroy disarms/joins first) */
} frame_cell_watchdog_t;

static void _frame_cell_watchdog_payload_destroy(void* p) {
  frame_cell_watchdog_t* w = (frame_cell_watchdog_t*)p;
  if (w == NULL) return;
  platform_mutex_destroy(w->lock);
  platform_condvar_destroy(w->cond);
  free(w);
}

static void* _frame_cell_watchdog_main(void* arg) {
  frame_cell_watchdog_t* w = (frame_cell_watchdog_t*)arg;
  uint64_t deadline =
      platform_monotonic_ns() + (uint64_t)w->timeout_ms * 1000000ULL;
  platform_mutex_lock(w->lock);
  while (w->disarm_wanted == 0) {
    uint64_t now = platform_monotonic_ns();
    if (now >= deadline) {
      /* The deadline fired: hand the struct over (its destroyer frees it on
         the frame's thread) and detach. handed_off is set under the lock
         FIRST, so a disarm that arrives later under the same lock sees it
         and never joins a detached thread. */
      w->handed_off = 1;
      _frame_post(&w->f->actor, (uint32_t)FRM_CELL_WATCHDOG, w,
                  _frame_cell_watchdog_payload_destroy, "cell watchdog");
      platform_thread_t* self = w->thread;   /* the local survives the unlock */
      platform_mutex_unlock(w->lock);
      platform_thread_detach(self);
      return NULL;
    }
    platform_condvar_timed_wait(w->cond, w->lock,
                                (unsigned)((deadline - now) / 1000000ULL) + 1);
  }
  platform_mutex_unlock(w->lock);
  return NULL;
}

static void _frame_cell_watchdog_disarm(frame_t* f) {
  /* The frame's dispatch thread ONLY (the single writer of the pointer). */
  frame_cell_watchdog_t* w = f->cell_watchdog;
  if (w == NULL) return;
  f->cell_watchdog = NULL;
  platform_mutex_lock(w->lock);
  if (w->handed_off != 0) {
    /* The deadline won: the incoming FRM_CELL_WATCHDOG message owns the
       struct (its destroyer frees it on the frame's thread) and the watcher
       thread is detached — do not join, do not touch the lock past the
       unlock. */
    platform_mutex_unlock(w->lock);
    return;
  }
  w->disarm_wanted = 1;
  platform_condvar_broadcast(w->cond);
  platform_mutex_unlock(w->lock);
  platform_thread_join(w->thread);
  _frame_cell_watchdog_payload_destroy(w);
}
```

`_frame_cell_watchdog_arm` (called at the END of FRM_CELL_EXECUTE's success path, right
after `f->cell_pending = 1;` — only in pool mode):

```c
static void _frame_cell_watchdog_arm(frame_t* f) {
  if (f->pool == NULL || f->cell_watchdog_ms == 0) return;
  if (f->cell_watchdog != NULL) {
    /* INVARIANT: one cell at a time — the previous watchdog disarmed at its
       result's arrival. Seeing one here is a bug's loud trace, and the
       disarm-first discipline keeps the state machine safe anyway. */
    _frame_cell_watchdog_disarm(f);
  }
  frame_cell_watchdog_t* w =
      (frame_cell_watchdog_t*)get_clear_memory(sizeof(frame_cell_watchdog_t));
  if (w == NULL) return;   /* no watchdog: the hung-cell cost is the old
                              silent wedge — loud log, degrade gracefully */
  w->lock = platform_mutex_create();
  w->cond = platform_condvar_create();
  w->timeout_ms = f->cell_watchdog_ms;
  w->f = f;
  if (w->lock == NULL || w->cond == NULL) {
    _frame_cell_watchdog_payload_destroy(w);
    log_error("frame: the cell watchdog failed to build at '%s' — cells run "
              "unwatched this frame", f->sid_path);
    return;
  }
  w->thread = platform_thread_create(_frame_cell_watchdog_main, w);
  if (w->thread == NULL) {
    _frame_cell_watchdog_payload_destroy(w);
    log_error("frame: the cell watchdog thread refused at '%s'", f->sid_path);
    return;
  }
  f->cell_watchdog = w;
}
```

Call sites: at `f->cell_pending = 1;` (FRM_CELL_EXECUTE success) add
`_frame_cell_watchdog_arm(f);`. In `PYRT_RESULT`'s dispatch, call
`_frame_cell_watchdog_disarm(f);` FIRST (before the matched/unmatched branching — the real
result's arrival ends the watchdog's reason to exist for BOTH outcomes; the poisoned frame's
late-result quiet branch also passes through here, and the struct's f->cell_watchdog is
NULLed by the disarm's first line so the later FRM_CELL_WATCHDOG message can't
double-apply). Also disarm at destroy-time.

`FRM_CELL_WATCHDOG`'s dispatch case (before `FRM_TURN`):

```c
    case FRM_CELL_WATCHDOG: {
      /* The deadline won (spec §2): the payload owns the watchdog struct —
         it is NO LONGER f->cell_watchdog (the disarm-or-handoff protocol
         settled it); clean up the pointer and run the synthesis under the
         watchdog wording. If the cell completed between the deadline and
         this dispatch (the result won the race), the apply is a benign
         no-op (arm_cut = 0 at nothing-open) — cell_pending is the frame's
         truth. */
      frame_cell_watchdog_t* w = (frame_cell_watchdog_t*)msg->payload;
      msg->payload = NULL;
      if (f->cell_watchdog == w) f->cell_watchdog = NULL;
      _frame_cell_watchdog_payload_destroy(w);
      _frame_interrupt_apply(f, 0, "aborted: cell exceeded the watchdog deadline");
      break;
    }
```

(The three callers of `_frame_interrupt_apply` are pinned in Task 5: `FRM_INT` with
`arm_cut = 1`, the inline driver's deadline with `arm_cut = 0`, and this watchdog dispatch
with `arm_cut = 0` — a deadline never arms a cut against a future legitimate cell, and
poison is the containment that needs no flag.)

- [ ] **Step 4: Destroy wiring** — in `frame_destroy` (grep its definition), FIRST
  disarm the watchdog, THEN destroy the pyrt (the standing join order):

```c
  _frame_cell_watchdog_disarm(f);
```
as the first statement after the dead-frame guard. (If the frame is poisoned, this join is
the documented cost — the hung pyrt thread's join in `pyrt_destroy` bounds it the same way
it does today.)

- [ ] **Step 5: Build + run**

Run: `setarch -R ctest --test-dir cmake-build-debug -R "TestPooledWatchdog|TestFrameInterrupt" --output-on-failure`
Expected: PASS. Full suite + ASan dir green (the watchdog's cross-thread handoff is
exactly what ASan/TSan exist for): `setarch -R ctest --test-dir cmake-build-asan --output-on-failure`.

- [ ] **Step 6: Commit**

```bash
git add src/Frame/frame.h src/Frame/frame.c src/Frame/frame_internal.h test/test_loop.cpp
git commit -m "feat: the pooled cell watchdog — deadline hands off the interrupt synthesis"
```

---

### Task 7: `agent.keys(scope)` — FRM_KEYS + FRM_STORE_KEYS

**Files:**
- Modify: `src/Frame/frame_messages.h` (FRM_KEYS, FRM_STORE_KEYS, bridge kinds)
- Modify: `src/Frame/frame.c` (FRM_KEYS case, the store's FRM_STORE_KEYS behavior, the
  reply router's keys branch)
- Modify: `src/Frame/frame_internal.h` (the new post helper)
- Modify: `src/Python/py_agent.c` (the keys verb)
- Modify: `test/test_py_agent.cpp`, `test/test_loop.cpp`

- [ ] **Step 1: Write the failing tests** — in `test/test_py_agent.cpp` (the bridge-frame
  harness pre-answers `FRM_KEYS` like `FRM_RECALL` once the dispatch case exists; the FIRST
  failing test is the verb's post + answer shape):

```cpp
TEST(TestPyAgent, TestKeysVerbsResolveThroughTheBridge) {
  /* The harness's FRM_KEYS answer (added to bridge_frame_dispatch in the
     same edit as the test): scope "local" -> reply text "[\"n\"]", scope
     "bogus" -> reply status 1. Read the harness's FRM_RECALL branch FIRST
     and mirror it exactly (the answer-mode branch answering through
     py_agent_note_reply). */
  bridge_frame_t* self = bridge_frame_create();
  self->answer = 1;
  /* seed the harness's scripted answer for scope 'local': the harness's
     store vector already answers remember->recall roudtrips; extend the
     FRM_KEYS branch to carry the pinned array text (the harness's business:
     a LITERAL reply, not a real scan — the real scan is the frame/store
     tests' job). */
  ASSERT_EQ(pyrt_execute(self->pyrt, strdup(
                "import actor\n"
                "ks = actor.keys('local')\n"
                "actor.report('keys: ' + str(ks))")), 0);
  bridge_frame_pump(self);
  /* The report carried the resolved array (the harness pre-answered it). */
  ASSERT_EQ(self->results.size(), 1u);
  py_frame_free_equivalent(self);
}
```

ADAPT-NOTE: write this test by READING the existing
`TestRememberRecallCorrMatched` (test_py_agent.cpp:212) and copying its exact harness
interaction — the only new fact is the verb + the pre-answered reply shape. The harness's
dispatch gains a `case FRM_KEYS:` mirroring its `FRM_RECALL` case (pre-answer
`"[\"n\"]"`). If the harness needs a new member (e.g. `char* scripted_keys_reply`), add it
in the same edit.

And the REAL frame/store test in `test/test_loop.cpp` (the harness proves the scan):

```cpp
TEST(TestLoop, TestKeysListsOwnStateKeysOnly) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "keys test", &cfg);
  ASSERT_NE(f, nullptr);

  EXPECT_EQ(frame_remember_local(f, "gamma", "3"), 0);
  EXPECT_EQ(frame_remember_local(f, "alpha", "1"), 0);
  EXPECT_EQ(frame_remember_ctx(f, "ctx-one", "true"), 0);
  EXPECT_EQ(frame_remember_ctx(f, "ctx-two", "false"), 0);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\n"
      R"json("actor.report('local: ' + str(actor.keys('local')) + ' ctx: ' + str(actor.keys('ctx')))\"}"}}]}}]})json";

  std::vector<std::string> replies = {turn1};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);
  EXPECT_EQ(frame_run_loop(f), 0);

  /* The report event's text pins BOTH listings, lexicographic. */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  int saw_report = 0;
  std::string report;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* type_v = json_get(rec, "type");
    if (type_v == NULL || strcmp(json_as_string(type_v), "frame.report") != 0)
      continue;
    json_value_t* p = json_get(rec, "payload");
    if (p != NULL && strstr(json_as_string(json_get(p, "text")), "local:") != NULL) {
      report = json_as_string(json_get(p, "text"));
      saw_report = 1;
    }
  }
  EXPECT_EQ(saw_report, 1);
  EXPECT_NE(report.find("['alpha', 'gamma']"), std::string::npos)
      << "the local listing was not lexicographic";
  EXPECT_NE(report.find("['ctx-one', 'ctx-two']"), std::string::npos)
      << "the ctx listing was not lexicographic";
  json_value_destroy(events);

  /* The bad scope refused corr-matched loud (None to python — the failure
     log carries it; assert the refusal did NOT answer an array): */
  /* (covered in test_py_agent's refusal test below) */

  frame_destroy(f);
  wave_db_close(db);
}
```

- [ ] **Step 2: The message vocabulary** — `frame_messages.h`:

```c
  FRM_KEYS,               /* cell -> frame: list the frame's OWN state keys
                             (frm_remember_payload_t; `key` carries the scope
                             string "local"/"ctx" — the recall payload's
                             shape, unchanged) */
```
(after `FRM_INT`; and in the store vocabulary, after `FRM_STORE_RECALL`):

```c
  FRM_STORE_KEYS          /* -> store actor: keys listing — a bounded reverse
                             scan over ONE state/<scope> subtree returning KEY
                             NAMES (the scan reply carries values; the listing
                             is its own verb), frm_store_keys_payload_t */
```

Payload (with the other bridge payloads):

```c
/* keys: the store-side listing request. */
typedef struct frm_store_keys_payload_t { uint64_t corr; char* sid_path; char* scope; } frm_store_keys_payload_t;
void frm_store_keys_payload_destroy(void* p);
```

(destroyer mirrors `frm_store_recall_payload_destroy` — `free(sid_path); free(scope); free(p);`
— READ the recall destroyer FIRST and mirror it byte-for-byte in style.)

- [ ] **Step 3: The store behavior** — in frame.c's `_store_behavior` (after
  `case FRM_STORE_RECALL`'s block):

```c
    case FRM_STORE_KEYS: {
      /* The keys listing (spec §3): a bounded reverse scan over the frame's
         OWN state/<scope> subtree, returning KEY NAMES (the tail segments) —
         the scan reply's records are VALUES, so the listing is its own verb.
         Never a write; never another subtree (the payload carries the frame's
         own sid_path, and the scope is a closed set the frame side validates
         before posting). */
      frm_store_keys_payload_t* kp = (frm_store_keys_payload_t*)msg->payload;
      msg->payload = NULL;
      if (kp == NULL || kp->sid_path == NULL || kp->scope == NULL) {
        log_error("store: FRM_STORE_KEYS with no payload/prefix — loud drop");
        frm_store_keys_payload_destroy(kp);
        break;
      }
      size_t base = strlen(kp->sid_path);
      char* lo = get_memory(base + strlen("/state//") + strlen(kp->scope) + 1);
      char* hi = get_memory(base + strlen("/state//0") + strlen(kp->scope) + 1);
      if (lo == NULL || hi == NULL) {
        free(lo);
        free(hi);
        _store_reply_send(kp->reply_to, kp->corr, -1, NULL, 0);
        frm_store_keys_payload_destroy(kp);
        break;
      }
      snprintf(lo, base + strlen("/state//") + strlen(kp->scope) + 1, "%s/state/%s",
               kp->sid_path, kp->scope);
      snprintf(hi, base + strlen("/state//0") + strlen(kp->scope) + 1, "%s/state/%s0",
               kp->sid_path, kp->scope);
      path_t* start = path_create_from_raw(lo, strlen(lo), '/', 0);
      path_t* end = path_create_from_raw(hi, strlen(hi), '/', 0);
      free(lo);
      free(hi);
      if (start == NULL || end == NULL) {
        if (start != NULL) path_destroy(start);
        if (end != NULL) path_destroy(end);
        _store_reply_send(kp->reply_to, kp->corr, -1, NULL, 0);
        frm_store_keys_payload_destroy(kp);
        break;
      }
      database_iterator_t* iter = database_scan_start_reverse(root->db, start, end);
      if (iter == NULL) {
        path_destroy(start);
        path_destroy(end);
        log_error("store: keys scan failed for '%s' — refusing the reply",
                  kp->sid_path);
        _store_reply_send(kp->reply_to, kp->corr, -1, NULL, 0);
        frm_store_keys_payload_destroy(kp);
        break;
      }
      /* The bounds are consumed by the scan; the listing cap leaves room for
         the "more existed" detection (the router clips + marks). */
      size_t cap = SA_BUDGET_KEYS_MAX + 1;
      if (cap > SA_FRAME_DEBUG_MAX_EVENTS) cap = SA_FRAME_DEBUG_MAX_EVENTS;
      char** records = (char**)get_clear_memory(cap * sizeof(char*));
      size_t n = 0;
      int oom = 0;
      if (records == NULL) {
        oom = 1;
      } else {
        path_t* key = NULL;
        identifier_t* value = NULL;
        while (n < cap) {
          path_t* k = NULL;
          int src = database_scan_prev(iter, &k, &value);
          if (src != 0) break;
          /* The tail segment IS the key (state/<scope>/<key>): the scan
             consumed start — the path's depth-1 tail copy. */
          size_t segs = path_length(k);
          identifier_t* last = path_get(k, segs - 1);
          size_t len = 0;
          uint8_t* data = identifier_get_data_copy(last, &len);
          char* name = NULL;
          if (data != NULL) {
            name = (char*)get_memory(len + 1);
            if (name != NULL) {
              memcpy(name, data, len);
              name[len] = '\0';
            }
            free(data);
          }
          if (name == NULL) oom = 1;
          else records[n++] = name;
          path_destroy(k);
          identifier_destroy(value);
          value = NULL;
          if (oom) break;
        }
        database_scan_end(iter);
      }
      if (oom) {
        for (size_t i = 0; i < n; i++) {
          free(records[i]);
        }
        free(records);
        records = NULL;
        n = 0;
        log_error("store: keys listing hit OOM materializing at '%s'",
                  kp->sid_path);
      }
      _store_reply_send(kp->reply_to, kp->corr, 0, records, records != NULL ? n : 0);
      frm_store_keys_payload_destroy(kp);
      break;
    }
```

(If `_store_reply_send`'s signature includes a records-count param — it does, seen at
frame.c:1207 — `records` is CONSUMED by it: mirror FRM_STORE_SCAN's ownership comment. Adapt
the free-on-oom branch if `records == NULL` and `_store_reply_send` handles NULL.)

- [ ] **Step 4: The frame-side FRM_KEYS case + the router's keys branch** — the pending
  registration needs a KIND. In frame_internal.h (near `_frame_bridge_pending_add`):

```c
/* The bridge pending's reply KINDS (the router branch behavior). */
typedef enum frm_bridge_kind_e {
  FRM_BRIDGE_RECALL = 1,   /* the resolved text rides records[0] */
  FRM_BRIDGE_KEYS = 2      /* the reply is a JSON array composed from the
                              records' key names */
} frm_bridge_kind_e;
```

Change `_frame_bridge_pending_add(f, store_corr, corr, is_recall)`'s last parameter to
`frm_bridge_kind_e kind` (grep its declaration + the FRM_RECALL/FRM_REMEMBER call sites and
update all — FRM_REMEMBER posts pass `0` today where the param is unused; give them
`FRM_BRIDGE_RECALL` only where the router reads it — read the add/take signatures FIRST and
change the param type atomically).

The FRM_KEYS case (mirroring FRM_RECALL's dispatch shape):

```c
    case FRM_KEYS: {
      /* The listing verb (spec §3): scope validated at the bridge boundary —
         a closed set, the refusal corr-matched loud BEFORE any post. */
      frm_remember_payload_t* rp = (frm_remember_payload_t*)msg->payload;
      msg->payload = NULL;
      uint64_t corr = 0;
      uint8_t status;
      if (rp == NULL) {
        status = 1;
        log_error("frame: FRM_KEYS with no payload at '%s'", f->sid_path);
      } else if ((corr = rp->corr) == 0) {
        status = 1;
        log_error("frame: FRM_KEYS with corr 0 at '%s' — nothing to match",
                  f->sid_path);
      } else if (rp->key == NULL ||
                 (strcmp(rp->key, "local") != 0 &&
                  strcmp(rp->key, "ctx") != 0)) {
        status = 1;
        log_error("frame: FRM_KEYS scope '%s' refused at '%s' — the scope is "
                  "closed to local|ctx", rp->key != NULL ? rp->key : "(null)",
                  f->sid_path);
      } else {
        uint64_t store_corr = ++f->store_corr_seq;
        _frame_bridge_pending_add(f, store_corr, corr, FRM_BRIDGE_KEYS);
        if (_frame_keys_post(f, rp->key, store_corr, &f->actor) != 0) {
          (void)_frame_bridge_pending_take(f, store_corr, NULL, NULL);
          status = 1;
          log_error("frame: FRM_KEYS '%s' refused at '%s' (corr %llu)",
                    rp->key, f->sid_path, (unsigned long long)corr);
          _frame_bridge_reply(corr, status, NULL);
        } else {
          status = 0;   /* the router answers at the reply */
        }
      }
      if (status != 0) {
        /* nothing further — the reply above already dispatched */
      }
      frm_remember_payload_destroy(rp);
      break;
    }
```

ADAPT-NOTE: mirror `case FRM_RECALL`'s EXACT control flow (frame.c:2017-2048) — note recall
does NOT set `status = 0` at the end (it lets the router answer); copy the recall case's
real final shape rather than the sketch's `status` juggling. The sketch above is the
INTENT; the implementation must be the recall case with `_frame_keys_post` substituted.

The post helper (in frame.c, near `_frame_recall_post`):

```c
static int _frame_keys_post(frame_t* f, const char* scope, uint64_t corr,
                            actor_t* reply_to) {
  if (f == NULL || f->st == NULL) return -1;
  frm_store_keys_payload_t* kp =
      (frm_store_keys_payload_t*)get_clear_memory(sizeof(frm_store_keys_payload_t));
  if (kp == NULL) return -1;
  kp->corr = corr;
  kp->sid_path = strdup(f->sid_path);
  kp->scope = strdup(scope);
  if (kp->sid_path == NULL || kp->scope == NULL) {
    frm_store_keys_payload_destroy(kp);
    return -1;
  }
  kp->reply_to = reply_to;
  _frame_post(_frame_store_actor(f), (uint32_t)FRM_STORE_KEYS, kp,
              frm_store_keys_payload_destroy, "keys listing");
  return 0;
}
```

The reply router's branch (in `_frame_store_reply_route`'s step 5, extending the
`is_recall` machinery):

```c
      uint8_t status = (r->rc == 0) ? 0 : 1;
      char* text = NULL;
      if (kind == FRM_BRIDGE_RECALL && status == 0 && r->n >= 1 &&
          r->records != NULL && r->records[0] != NULL) {
        text = strdup(r->records[0]);
        if (text == NULL) status = 1;
      } else if (kind == FRM_BRIDGE_KEYS && status == 0) {
        /* The listing: sort the names ascending, clip to
           SA_BUDGET_KEYS_MAX, close with the marker when the scan carried
           MORE (the store-side cap = MAX+1 — the clipped tail says so). */
        if (r->n > 0 && r->records != NULL) {
          size_t n = r->n;
          uint8_t clipped = (n > SA_BUDGET_KEYS_MAX &&
                             kind == FRM_BRIDGE_KEYS) ? 1 : 0;
```

STOP — the router composes JSON: use the project's json module. Write the branch body as
the implementer-verifiable version (the full listing):

```c
      } else if (kind == FRM_BRIDGE_KEYS && status == 0) {
        /* The listing (spec §3): lexicographic ascending, clipped to
           SA_BUDGET_KEYS_MAX with the marker record when the scan carried
           MORE (the store-side limit = MAX+1 makes the check exact). The
           store returned newest-first (reverse scan) — sort ascending
           FIRST, then clip. */
        if (r->n == 0 || r->records == NULL) {
          text = strdup("[]");
        } else {
          qsort(r->records, r->n, sizeof(char*), _frame_keys_cmp);
          size_t shown =
              (r->n > SA_BUDGET_KEYS_MAX) ? SA_BUDGET_KEYS_MAX : r->n;
          json_value_t* arr = json_new_array();
          if (arr == NULL) {
            status = 1;
          } else {
            for (size_t i = 0; i < shown; i++) {
              json_array_append(arr, json_new_string(r->records[i]));
            }
            if (r->n > SA_BUDGET_KEYS_MAX) {
              json_array_append(arr, json_new_string("[budget: keys truncated]"));
            }
            char* rendered = json_dump(arr);
            json_value_destroy(arr);
            text = rendered;   /* the reply sink copies it */
          }
        }
      }
```

plus the comparator near the router:

```c
static int _frame_keys_cmp(const void* a, const void* b) {
  const char* sa = *(const char* const*)a;
  const char* sb = *(const char* const*)b;
  return strcmp(sa, sb);
}
```

(`json_dump` — READ src/Util/json.h for the real serializer name; adapt if it's
`json_dump_text`/`json_stringify`. Include `<stdlib.h>` for qsort.)

- [ ] **Step 5: The py_agent verb** — the method, mirroring `_py_agent_recall` exactly
  (py_agent.c:390-422) with scope as the key:

```c
/* keys(scope) -> list | None — the pulled-forward inspect member (spec §3):
   the frame's OWN state subtree's KEY NAMES, never values. `local`/`ctx`
   only (the closed set); a refusal or timeout answers None exactly like
   recall's unresolvable — the loud log the reply path carries tells them
   apart for the operator. */
static PyObject* _py_agent_keys(PyObject* self, PyObject* args) {
  (void)self;
  const char* scope = NULL;
  if (!PyArg_ParseTuple(args, "s:keys", &scope)) {
    return NULL;
  }
  frm_remember_payload_t* rp =
      (frm_remember_payload_t*)get_clear_memory(sizeof(frm_remember_payload_t));
  if (rp == NULL) {
    PyErr_NoMemory();
    return NULL;
  }
  rp->key = strdup(scope);
  rp->json_value = NULL;
  if (rp->key == NULL) {
    frm_remember_payload_destroy(rp);
    PyErr_NoMemory();
    return NULL;
  }
  uint8_t status = 0;
  char* text = NULL;
  uint64_t corr = _py_agent_next_corr();
  rp->corr = corr;
  py_agent_wait_rc_e rc =
      _py_agent_request((uint32_t)FRM_KEYS, rp, frm_remember_payload_destroy,
                        corr, &status, &text);
  if (rc == PY_AGENT_OK && status == 0 && text != NULL) {
    PyObject* out = _py_agent_json_load(text);
    free(text);
    return out;
  }
  free(text);
  Py_RETURN_NONE;
}
```

Add to `_py_agent_verb_methods`:

```c
    {"keys", _py_agent_keys, METH_VARARGS, "List this frame's OWN state keys (local|ctx); returns a list or None."},
```

- [ ] **Step 6: Build + run**

`setarch -R ctest --test-dir cmake-build-debug --output-on-failure` — all green.

- [ ] **Step 7: Commit**

```bash
git add src/Frame/frame_messages.h src/Frame/frame.c src/Frame/frame_internal.h \
        src/Python/py_agent.c test/test_py_agent.cpp test/test_loop.cpp
git commit -m "feat: agent.keys(scope) — the pulled-forward inspect member over the store actor"
```

---

### Task 8: The bridge value caps (remember / report / spawn)

**Files:**
- Modify: `src/Python/py_agent.c`
- Modify: `test/test_py_agent.cpp`

- [ ] **Step 1: Write the failing test** — mirror `TestEmitPostsWithThePayload`'s shape
  (Task 4 Step 1): an `actor.remember('k', big_value)` and `actor.report(big)` posts with
  the capped text (the harness records `req_a`/`req_b`; the captured value carries the
  marker):

```cpp
TEST(TestPyAgent, TestBridgeValuesCappedAtSource) {
  bridge_frame_t* self = bridge_frame_create();
  ASSERT_EQ(pyrt_execute(self->pyrt, strdup(
                "import actor\n"
                "actor.remember('k' * 20, 'y' * 100000)\n"
                "actor.report('z' * 100000)")), 0);
  bridge_frame_pump(self);
  ASSERT_EQ(self->results.size(), 1u);
  /* Both posts arrived; each carries the marker naming the ORIGINAL 100000. */
  int saw_remember_marker = 0, saw_report_marker = 0;
  for (size_t i = 0; i < self->req_a.size(); i++) {
    if (self->req_a[i] == "kkkkkkkkkkkkkkkkkkkk" &&
        self->req_b[i].find("[budget: truncated at 100000 bytes]") !=
            std::string::npos) {
      saw_remember_marker = 1;
    }
    if (self->req_b[i].size() == 0) continue;
  }
  for (size_t i = 0; i < self->req_a.size(); i++) {
    if (self->req_a[i].find("[budget: truncated at 100000 bytes]") !=
        std::string::npos) {
      saw_report_marker = 1;   /* report's text rides req_a in this harness */
    }
  }
  EXPECT_EQ(saw_remember_marker, 1);
  EXPECT_EQ(saw_report_marker, 1);
  bridge_frame_free(self);
}
```

ADAPT-NOTE: the harness's record vectors' exact roles (`req_a` = remember value? report
text?) were noted at harness creation — READ the FRM_REMEMBER/FRM_REPORT dispatch branches
(test_py_agent.cpp:73-180) and use the REAL fields. If report text actually rides `req_a`,
the second loop above is right; if it rides `req_b`, swap. The assertion targets are: BOTH
the remember value and the report text carry the marker at their first 16 KiB + marker.

- [ ] **Step 2: The caps** — in py_agent.c, apply at each compose point:

`_py_agent_remember`: after `char* value = _py_agent_json_of(val_o);`:

```c
  /* The bridge cap (budget table §4): bounded durable text at the source.
     The marker rides the stored value — the recall reader sees the cut. */
  char* capped = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(value, SA_BUDGET_BRIDGE_VALUE_BYTES, &capped,
                              &truncated);
  free(value);
  if (capped == NULL) {
    free(key);
    PyErr_NoMemory();
    return NULL;
  }
  value = capped;
  if (truncated != 0) {
    pyrt_post_text(PYRT_LOG, "agent.remember: the value was truncated at the bridge budget");
  }
```

`_py_agent_report`: same shape for its text (report's text via `_py_agent_text_of`? READ the
real coercion order at py_agent.c:484-500 and apply the cap to the text BEFORE the payload
compose); `_py_agent_spawn`: same for `context_json` (its heap copy after the `|O` arg parse).
Each truncation posts the budget log line with the verb's name.

- [ ] **Step 3: Build + run + commit**

```bash
setarch -R ctest --test-dir cmake-build-debug --output-on-failure
git add src/Python/py_agent.c test/test_py_agent.cpp
git commit -m "feat: the bridge value budget at the remember/report/spawn sources"
```

---

### Task 9: The cap-mechanical sweep + grep gate

**Files:**
- Modify: `src/Frame/loop.c` (any `#define` cap survivors)
- Verify: no cap-magnitude `#define`s remain anywhere else the table owns

- [ ] **Step 1: The grep gate**

```bash
grep -n "#define.*CAP\|#define.*MAX" src/Frame/loop.c src/Python/pyrt.c src/Python/py_agent.c
```
Expected: ONLY non-cap-sense hits (`SA_LOOP_MAX_TURNS` — a COUNT of turns, not a byte budget,
stays; `_PYRT_SLOT_WAIT_MS` stays). NO byte-cap `#define` survives: every one is either
renamed to the table (Task 4 Step 5 renamed the derive trio) or already gone.

- [ ] **Step 2: Fix stragglers if the gate catches any, then full suite + valgrind**

```bash
setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure
cp cmake-build-debug/testsecretagent /tmp/sa_vg_test && strip /tmp/sa_vg_test
valgrind --leak-check=full --show-leak-kinds=definite --error-exitcode=42 \
  /tmp/sa_vg_test --gtest_filter='TestBudget.*:TestPyrt.*:TestPyAgent.*:TestLoop.*' \
  2>&1 | tail -30
```
(The gtest filter names every suite this slice touched — `TestBudget.*` needs NO exclusion.
Disk-class tests are already documented valgrind-excluded; if the filter pulls one in, add a
`-:` exclusion with its full `SuiteName.TestName` prefix.)

- [ ] **Step 3: Commit (fixes, if any)**

```bash
git add -u src/
git commit -m "refactor: the budget table owns every cap constant; loop.c carries none"
```

---

### Task 10: Docs + the parity matrix land-record + the final verification sweep

**Files:**
- Modify: `docs/parity-feature-matrix.md` (rows + escalations)
- Modify: `docs/superpowers/specs/2026-10-02-surface-completion-design.md` (one wording fix)
- Modify: memory file `project_secretagent.md` (session state)

- [ ] **Step 1: The matrix land updates** — with commit refs from this slice's real commits:

- Row 25 (interrupt a running cell): **L** — `frame_interrupt` + the pooled watchdog (the
  cooperative-only limit documented as the poison contract; the subprocess kill path stays a
  recorded candidate). Layer L2/L3; test-parity = test_pyrt interrupt + new frame tests.
- Row 29 (budget table): **L** — `src/Util/budget.{h,c}`, the one table with the marker at
  source; transport stays cross-referenced.
- Row 87 (§2 write verb row): the "emit is not durable yet" block becomes: **L** — the emit
  events kind, source-capped, derive-rendered (`test_loop.cpp` TestEmitIsDurableAndProjected).
  The §2 verdict's "6th half-built" becomes "6 verbs + deferred inspect's keys member pulled
  forward (`agent.keys`)".
- New row (the inspect pull-forward): **L (DEV)** — `agent.keys(scope)` only; events scan/
  child listing/`read(path)` stay YAGNI.
- Escalation 4 (watchdog): RESOLVED — interrupt + poison; the escalation block's entry
  rewritten to its resolution + commit.
- Escalation 6 (inspect pull-forward): RESOLVED — keys pulled; scan/child listing deferred.

- [ ] **Step 2: The spec's one wording fix** — the emit section says "newest-first"; the
  derive renders in EVENT ORDER (event order is the whole projection's ordering):

  In `docs/superpowers/specs/2026-10-02-surface-completion-design.md` §1, replace "newest-first,
  each capped" with "in event order (the projection's one ordering), each capped".

- [ ] **Step 3: The full verification sweep**

```bash
setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure
# OFF build (no python, no streams — the surface's pyrt work must vanish cleanly):
cmake -S . -B cmake-build-off -DSA_ENABLE_PYTHON=OFF -DSA_ENABLE_STREAMS=OFF && \
  cmake --build cmake-build-off -j && \
  ctest --test-dir cmake-build-off --output-on-failure
# ASan full:
setarch -R ctest --test-dir cmake-build-asan --output-on-failure
# no-locks discipline:
grep -rn "platform_mutex_lock\|pthread_mutex" src/Frame/ | grep -v "model.c"
```
Expected: ON suite ~218+new all green; OFF suite 78+TestBudget green; ASan green; the
frame layer's lock greps stay empty (the watchdog's mutex belongs to the per-cell watchdog
machinery and lives in its own pair of lock/unlock helpers — run the grep and justify EACH
hit in the plan notes if one appears; the discipline's documented exception model is
model.c's install-once mounts).

- [ ] **Step 4: Session-state memory update** — append the surface-completion block to
  `project_secretagent.md`'s progress section (the standing pattern: commits range, what
  landed, known-pending: the subprocess kill candidate, inspect's remaining half,
  `blocked`-reason steering, derive compaction).

- [ ] **Step 5: Commit docs**

```bash
git add docs/parity-feature-matrix.md docs/superpowers/specs/2026-10-02-surface-completion-design.md
git commit -m "docs: surface-completion lands — matrix rows 25/29/emit + escalations Q4/Q6 resolved"
```