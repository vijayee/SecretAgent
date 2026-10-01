# Turn/Step Lifecycle + Crash Repair — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** SecretAgent's turn/step lifecycle slice: a complete audit envelope in the log (`turn.start`/`step.start`/`step.end`/`turn.end` with a reason union, riding the engine's EXISTING batches + one fire-and-post batch at turn entry), and crash repair on `frame_resume` — pure deterministic closers over a truncated tail with the MODEL-VISIBLE briefing carried in the log (details upfront: the interrupted cell's code + seq).

**Architecture:** a NEW pure module `src/Frame/lifecycle.{h,c}` (the cursor fold + closer composition + the reason/type constants + the riders' payload composers) in the exact discipline of `src/Frame/refine.{h,c}`; `src/Frame/loop.c` gains the batch riders + the turn-number restore + the derive's ONE new projection branch; `src/Frame/frame.c` gains `frame_resume`'s closer path (the sync family's discipline). NO new actor, NO new thread, NO lock anywhere — the store actor remains the only serializer.

**Spec:** `docs/superpowers/specs/2026-10-01-turn-lifecycle-design.md` (§1 the vocabulary, §2 the module, §3 the batch riders + counter restore, §4 the resume repair, §5 the deliberate non-ports, §6 the freeze list, §7 the test-parity mapping, §8 acceptance). The spec carries the pinned strings (the brief's wording) — implement them VERBATIM.

**References:** DSH (READ-ONLY, never modify): `deepseek-harness/packages/core/session/src/repair.ts` (the closer shape + CLOSER_TEXT), `packages/core/session/tests/repair.spec.ts` (the mirrored intents + the verbatim wording table), `packages/core/agent-loop/src/agent.ts:330-395` (the finally-discipline: step/end + turn/end on every exit), `packages/core/agent/src/consumed-work.ts` (turn-with-no-step closes balanced). Landed runtime idioms (READ-ONLY refs — read before their task): `src/Frame/refine.{h,c}` (the module discipline + the fold's parse shape + the render-not-crash rule), `src/Frame/loop.c` (the engine overview comment :19-104; `frame_run_loop` :1354+; the derive's pass B :390-461; the cell-run round trip `_loop_post_cell_run` :691-711; the content path `_loop_content_path` :742+; the refusal-paired cell.result :960-990; the failure surface `_loop_fail`/`_frame_engine_terminate` :490-525; the cap check :1175; the turn-limit fail :1167-1175), `src/Frame/frame.c` (`_frame_event_write` / `_frame_event_post_fire` — the event record path; `_frame_events_bounds` ~:494; the reply's batch compose :765-787; `_frame_remember_sync`'s sync shape; `_frame_set_status_done`; `_frame_restore_seq` ~:2110; `frame_resume` ~:2770-2850; the single-flight sync slot + `_frame_sync_scan`/`_frame_sync_batch` from the refine slice Task 3), `src/Frame/frame_internal.h`, `test/test_loop.cpp` (scripted_complete idiom; TestRestartReplayRestoresSeqAndContext), `test/test_frame.cpp` (the batch/scan-capture + refusal idioms; TestJoinResetsARecallLeftoverSlot), `test/test_refine.cpp` (the seed helpers + the store round-trip test shapes), `docs/STYLE_GUIDE.md`, and the good-actors payload rules (ownership transfers with every message; every heap field carried by a payload has a destroyer).

**Frozen boundaries (hard rules):** never modify `liboffs/`, `WaveDB/`, `deepseek-harness/`, `prime-agent/`, `onyx/`, `claude-code-source-code/`, anything under `deps/`. `src/Actor`, `src/Scheduler`, `src/RefCounter`, `src/Util`, `src/Streams`, `src/Buffer`, `src/Python`, `src/Frame/model.{h,c}`, `src/Frame/refine.{h,c}`, `src/Frame/frame_messages.h` and `tools/` are NOT touched by this plan. `src/Frame/loop.c` and `src/Frame/frame.c` change ONLY as their tasks specify (the batch riders, the counter restore, the resume closer path, the derive's one branch — every other engine behavior byte stays put). Never leave a TODO (TODO/FIXME/HACK/XXX) anywhere this plan touches. Conventional commits, no Co-Authored-By.

## Standing verification idioms (used throughout)

```bash
# build
cmake --build cmake-build-debug -j
# suite (everything hereafter assumes this instead of bare ctest)
setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
# ASan suite (dies randomly at __asan_init WITHOUT setarch -R)
cmake --build cmake-build-asan --target testsecretagent -j && \
setarch -R ctest --test-dir cmake-build-asan --output-on-failure 2>/dev/null | tail -3
# valgrind on a filter (stripped copy; VERIFY it ran: grep -c "OK ]" on output)
cp cmake-build-debug/test/testsecretagent /tmp/vg && strip --strip-debug /tmp/vg
setarch -R valgrind --leak-check=full --suppressions=test/vg-cpython.supp \
  --suppressions=test/vg-wavedb.supp /tmp/vg --gtest_filter='TestLifecycle.*' 2>&1 | \
  tee /tmp/vg_out.txt | tail -4 && grep -c "OK ]" /tmp/vg_out.txt
# the no-lock rule (standing check in every store-touching task)
{ grep -rn "platform_mutex\|platform_rwlock\|platform_barrier\|platform_sem" src/Frame/ \
  && echo "VIOLATION: a lock in the frame layer"; } || echo "no locks in the frame layer"
# expected: "no locks in the frame layer" (model.c's recorded exceptions unchanged)
```

Disk-test discipline (unchanged): every fresh store rides `getenv("TMPDIR")`-based scratch dirs
created and destroyed by the test; NEVER `sa-demo-db`/the repo's demo database. Live gates: NONE
in this plan (the restart tests are disk+scripted; no model endpoint needed).

## File structure

```
src/Frame/lifecycle.h            (Task 1: types, constants, the composers, the cursor, closers)
src/Frame/lifecycle.c            (Task 1: the pure unit — cursor fold, brief, closers)
src/Frame/frame_internal.h       (Task 2: nothing unless a helper declaration is strictly
                                  needed — prefer NOT adding; the engine's riders are internal)
src/Frame/loop.c                 (Task 2: turn-entry batch, step/turn riders, counter restore,
                                  terminate's turn.end; Task 3: the derive's repair branch)
src/Frame/frame.c                (Task 3: frame_resume's tail scan + cursor + closer batch)
test/test_lifecycle.cpp          (NEW, Task 1: the pure suite — mirrors repair.spec.ts)
test/CMakeLists.txt              (Task 1: registration — PLAIN gate, no WDB needed)
test/test_loop.cpp               (Task 2/3: envelope + failure-exit tests; restart extension)
test/test_frame.cpp              (Task 2: batch-riding shape; Task 3: resume idempotency)
atlas/workflow.json + atlas.html (Task 4: the truthful in-progress node)
```

---

### Task 1: The lifecycle module — cursor, reasons, closers, brief (pure)

TDD first: the repair.spec.ts-mirrored core runs against PURE functions — no store, no model,
no actor. Everything is deterministic in-memory data work in the style guide's module discipline.

**Files:**
- Create: `src/Frame/lifecycle.h`, `src/Frame/lifecycle.c`
- Create: `test/test_lifecycle.cpp`, modify `test/CMakeLists.txt`

- [ ] **Step 1: Register the suite + write the failing tests**

`test/CMakeLists.txt` — at the top of the `BUILD_TESTING` blocks where the plain (non-WDB)
suites are gathered, alongside the other plain-registered test files:

```cmake
      # The lifecycle suite is PURE (cursor fold + closers over a json array
      # text): no WDB, no streams — it registers on the plain gate so the OFF
      # build keeps it.
      target_sources(testsecretagent PRIVATE test_lifecycle.cpp)
```

(Read the file's existing structure first — register it exactly where the other
no-WDB test files are listed. The OFF-config build must keep ALL of this suite.)

`test/test_lifecycle.cpp` — the suite skeleton + Task 1's tests (gtest, extern "C" the
module header; cursors fold a joint JSON-ARRAY text of event records — the SAME shape
`refine_fold_parse` consumes: `{"type": "...", "payload": {...}}` objects — hand-composed
here in arrays, one event per record, in ascending order):

```cpp
// Created by victor on 10/1/26.
//
// test_lifecycle.cpp — the parity mirror (spec §7, DSH repair.spec.ts intents
// 1-10 adapted to our vocabulary; citations inline). Pure: no store, no model.
#include <gtest/gtest.h>

extern "C" {
#include "../src/Frame/lifecycle.h"
}

#include <string>

/* Test helpers build event-record arrays by hand; the seed helpers live here. */

TEST(TestLifecycle, TestBalancedTailComposesNothing) {
  /* port of repair.spec:104-114: a tail whose newest lifecycle record is a
     turn.end composes [] ; an EMPTY array composes []; an array with NO
     lifecycle records at all (a pre-lifecycle log — only msg.append/cell.*)
     composes [] — balance is about turn.END records, never about unknown
     types. */
}

TEST(TestLifecycle, TestOpenTurnNoStepClosesWithTurnEndOnly) {
  /* port of repair.spec:116-123: an open turn (a turn.start, no turn.end)
     with NO step records composes exactly [turn.end {reason interrupted}] at
     the seq after the tail's last. */
}

TEST(TestLifecycle, TestOpenStepClosesStepThenTurn) {
  /* port of repair.spec:125-133: an open step (step.start, no step.end)
     inside an open turn composes [turn.end-adjacent step.end, turn.end] in
     THIS order: repair (if an in-flight cell demands a quote — this test:
     none) FIRST, then step.end, then turn.end, contiguous seqs. */
}

TEST(TestLifecycle, TestInFlightCellQuotedInBriefStarted) {
  /* port of repair.spec:135-176, 320-352: a cell.run committed with NO
     matching cell.result (the crash cut after the audit): the closer carries
     the repair brief first, its text contains the started-cause lead line
     "The previous turn was interrupted before its result was recorded.",
     the in-flight cell's seq, the cell.run's CODE verbatim (short one), the
     fact line "Its outcome is unknown.", and the retry-guidance line with
     "retry only if the operation is read-only or idempotent" and "Do not
     retry blindly." — details upfront, never a pointer. */
}

TEST(TestLifecycle, TestTurnOpenNoCellRecordedBriefNotStarted) {
  /* port of repair.spec's not-started wording: an open turn whose records
     carry NO cell.run (the crash cut between the model call and the audit)
     briefs "The previous turn was interrupted before the cell started. No
     cell execution was recorded." + "Retry it if it is still needed." and
     NO code quote — the not-started shape. */
}

TEST(TestLifecycle, TestAnsweredCellComposesNoQuote) {
  /* port of repair.spec:178-207: a cell.run WITH its matching cell.result
     (corr) composes [step.end, turn.end] with NO repair brief — the call
     is answered; nothing dangling. */
}

TEST(TestLifecycle, TestOnlyTheStillOpenTurn) {
  /* port of repair.spec:235-284: two turns in one tail — turn 1 completed
     (its own closed cell) and turn 2 cut open mid-cell: the closers quote
     turn 2's cell, carry turn 2's numbers, and never touch turn 1. Also:
     closers' turn numbers come from the RECORDS' OWN payloads, not seqs. */
}

TEST(TestLifecycle, TestResultMismatchDoesNotAcknowledge) {
  /* port of repair.spec:79-97, corr-mapped: a cell.result whose corr does
     NOT match the open cell.run's does not acknowledge it — the brief
     still quotes the open cell; a SECOND cell.run over an open one is the
     malformed record: loud log line + skipped, the fold survives with the
     FIRST cell in flight (the render-not-crash rule). */
}

TEST(TestLifecycle, TestCursorToleratesRefusedTurnStart) {
  /* spec §2's tolerance pin: records whose turn.start never committed
     (step.start/cell.run carrying turn numbers alone) fold fine — numbers
     come from payloads. The cursor's turn counter = the newest recorded
     turn number regardless of which record type carried it. */
}

TEST(TestLifecycle, TestWordingPinnedVerbatim) {
  /* port of repair.spec:368-426's wording table: both brief shapes pinned
     EXACT string-equal (the tests build the tails and compare the whole
     text); the started shape's code quote truncates at
     SA_LIFECYCLE_BRIEF_CODE_CHARS with loud truncation-at-source (the log
     line), matching the spec's cap discipline. */
}

TEST(TestLifecycle, TestDeterministicAndSeqContiguous) {
  /* port of repair.spec:64-77: the SAME tail composes byte-identical
     closers twice; seqs = last_seq+1, +2, +3... contiguous; the compose is
     pure (no randomness, no clock). */
}

TEST(TestLifecycle, TestMalformedLifecycleRecordsFoldLoudSkip) {
  /* the render-not-crash rule (spec §1/§2, refine's fold discipline):
     corrupt payloads (non-object payload, non-int turn) log loud + skip;
     the fold survives and still balances on the turn.end records it
     understood. */
}

TEST(TestLifecycle, TestCursorRejectsNullInputLoud) {
  /* the module posture (refine's): NULL fold/input = loud refuse, no
     silent empty-fold leniency. */
}
```

- [ ] **Step 2: Write the frozen contracts**

`src/Frame/lifecycle.h` (the whole Task 1 slice — Types + constants live here for good):

```c
//
// Created by victor on 10/1/26.
//

#ifndef SA_LIFECYCLE_H
#define SA_LIFECYCLE_H

#include "../Util/json.h"

/* --- caps (SA_* ifndef discipline — a build can override with -D) -------- */
#ifndef SA_LIFECYCLE_BRIEF_CODE_CHARS
#define SA_LIFECYCLE_BRIEF_CODE_CHARS 1200   /* the brief's quoted cell code */
#endif
#ifndef SA_LIFECYCLE_TAIL_EVENTS
#define SA_LIFECYCLE_TAIL_EVENTS 512         /* the resume tail scan's window */
#endif
#ifndef SA_LIFECYCLE_MAX_CLOSERS
#define SA_LIFECYCLE_MAX_CLOSERS 4           /* one closer batch's record cap */
#endif

/* --- event type + reason constants (the vocabulary's single truth) -------
   The turn/step envelopes' writers are the ENGINE (loop.c's riders) and
   the RESUME closers (frame.c); reasons "aborted"/"blocked" are RESERVED
   for the steering/interrupt slice (no writer this slice). */
extern const char LIFE_EVENT_TURN_START[];   /* "turn.start" */
extern const char LIFE_EVENT_TURN_END[];     /* "turn.end" */
extern const char LIFE_EVENT_STEP_START[];   /* "step.start" */
extern const char LIFE_EVENT_STEP_END[];     /* "step.end" */
extern const char LIFE_EVENT_REPAIR[];       /* "repair" — model-visible */

extern const char LIFE_REASON_COMPLETED[];    /* "completed" */
extern const char LIFE_REASON_ERROR[];        /* "error" */
extern const char LIFE_REASON_TURN_LIMIT[];   /* "turn-limit" (no writer today) */
extern const char LIFE_REASON_INTERRUPTED[];  /* "interrupted" */
extern const char LIFE_REASON_ABORTED[];      /* "aborted" — reserved */
extern const char LIFE_REASON_BLOCKED[];      /* "blocked" — reserved */

/* --- payload composers (the engine's riders; caller owns the DOM) --------
   Every rider's payload shape is FROZEN here. turn/step are JSON ints
   (unpadded — payloads never sort); turn_end's kind is one of the
   LIFE_REASON_* literals; text = the control kind's wording verbatim
   (NULL/"" renders the field absent — never an empty string). */
json_value_t* lifecycle_turn_start_json(uint64_t turn);
json_value_t* lifecycle_step_json(uint64_t turn, uint64_t step);
json_value_t* lifecycle_turn_end_json(uint64_t turn, const char* kind,
                                      const char* text);

/* --- the cursor (spec §2): the fold over an event tail -------------------
   The cursor holds STATE-OF-THE-TAIL, not event history. All heap fields
   are owned by the cursor and destroyed by lifecycle_cursor_destroy. */
typedef struct lifecycle_cursor_t {
  uint64_t turn;             /* the NEWEST recorded turn number (payloads) */
  uint64_t step;             /* the newest recorded step within `turn` */
  uint8_t turn_open;         /* 1 = a turn.start without its turn.end */
  uint8_t step_open;         /* 1 = a step.start without its step.end */
  uint8_t cell_inflight;     /* 1 = a cell.run committed, no matching result */
  uint64_t inflight_seq;     /* the cell.run record's seq (0 when none) */
  char* inflight_code;       /* the cell.run payload's code (heap; NULL none) */
  uint64_t inflight_corr;    /* the cell.run payload's corr (0 when none) */
  char* inflight_demand;     /* the result the cell never produced is unknown;
                              no demand text exists in the log today — this
                              field stays NULL this slice, reserved */
  uint64_t last_seq;         /* the tail's newest record's seq */
} lifecycle_cursor_t;

/* Fold a joint JSON-ARRAY text of event records (the scan reply's shape —
   the SAME contract refine_fold_parse consumes: {"type","payload"} objects,
   ascending) into the cursor. Unknown types pass through (they move
   nothing); malformed lifecycle records log loud + skip (the render-not-
   crash rule); turn/step numbers come from the records' OWN payloads.
   Returns 0, or -1 with a loud log on NULL input. */
int lifecycle_cursor_fold(const char* events_array_json, lifecycle_cursor_t* c);

/* The cursor's lifecycle (the folded heap fields). */
void lifecycle_cursor_destroy(lifecycle_cursor_t* cursor);

/* --- the closers (spec §2/§4): the synthesized closer records ------------ */

typedef struct lifecycle_closer_t {
  const char* type;        /* BORROWED (a LIFE_EVENT_* literal) */
  uint64_t seq;            /* last_seq+1, contiguous, in order */
  json_value_t* payload;   /* owned by the closers' destroy */
} lifecycle_closer_t;

typedef struct lifecycle_closers_t {
  lifecycle_closer_t* items;   /* heap array */
  size_t n;
} lifecycle_closers_t;

/* Compose the deterministic closer records for the cursor's tail state
   (spec §4's order: the `repair` brief FIRST (when a brief exists — an
   open turn ALWAYS briefs: both shapes say something), then `step.end`
   (when open), then `turn.end {reason: "interrupted"}`). Empty tail /
   balanced tail (no open turn) = {NULL, 0} with NO allocation. The brief's
   text is composed HERE (the two pinned shapes, spec §2) and rides the
   repair record's payload {turn, text}. Returns the closers, or -1 +
   loud log on a NULL cursor (the module posture). */
int lifecycle_closers_compose(const lifecycle_cursor_t* cursor,
                              lifecycle_closers_t* out);

/* The closers' lifecycle (every payload DOM + the items array). */
void lifecycle_closers_destroy(lifecycle_closers_t* closers);

#endif /* defined(SA_LIFECYCLE_H) */
```

(No WDB include — the module is pure; the engine's integration keeps its own frame.h
types. `inflight_demand` stays NULL/reserved this slice per the spec — it exists so the
cursor's shape is stable when the reply-demand quote arrives with a later slice.)

**The brief's pinned wording (verbatim strings to implement, spec §2):**
- started lead: `The previous turn was interrupted before its result was recorded.`
- started details line: `The cell was executing (harness-log seq <seq>):` then the
  cell's code verbatim (whitespace preserved; > SA_LIFECYCLE_BRIEF_CODE_CHARS →
  truncated at the cap with `...` and a loud log line at compose time —
  truncation-at-source, never silence).
- started facts+guidance: `Its outcome is unknown. Decide whether to retry from the
  cell's semantics: retry only if the operation is read-only or idempotent; if it may
  have side effects, first verify external state or ask the user. Do not retry blindly.`
- not-started: `The previous turn was interrupted before the cell started. No cell
  execution was recorded. Retry it if it is still needed.`
- The brief carries the open turn's number in the repair record's payload `turn`.

- [ ] **Step 3: Green + full suite + ASan**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
cmake --build cmake-build-asan --target testsecretagent -j && setarch -R ctest --test-dir cmake-build-asan --output-on-failure 2>/dev/null | tail -3
```

Expected: green everywhere (baseline 189/189 + Task 1's tests); TestLifecycle prints OK
in the tail-3 window; the OFF build (Task 4 reconfigures) must compile this suite too.

- [ ] **Step 4: Valgrind on TestLifecycle.* (verify it actually ran)**

```bash
cp cmake-build-debug/test/testsecretagent /tmp/vg && strip --strip-debug /tmp/vg
setarch -R valgrind --leak-check=full --suppressions=test/vg-cpython.supp \
  --suppressions=test/vg-wavedb.supp /tmp/vg --gtest_filter='TestLifecycle.*' 2>&1 | \
  tee /tmp/vg_out.txt | tail -4 && grep -c "OK ]" /tmp/vg_out.txt
```

Expected: 0 errors, 0 definitely lost, and a count > 0 (never an empty run).

- [ ] **Step 5: Commit**

```bash
git add src/Frame/lifecycle.h src/Frame/lifecycle.c test/test_lifecycle.cpp test/CMakeLists.txt
git commit -m "feat: lifecycle cursor, reason union, and crash closers"
```

---

### Task 2: The engine's riders — turn entry, step/turn pairs, terminal reasons

The engine writes the envelope on EXISTING batches + one fire-and-post batch at turn
entry. The DSH finally-discipline becomes a TESTED RULE: every engine exit closes its
turn.

**Files:**
- Modify: `src/Frame/loop.c` (the riders + the counter restore; nothing else)
- Modify: `test/test_loop.cpp`, `test/test_frame.cpp`

- [ ] **Step 1: Tests first (the envelope order + failure closes)**

`test/test_loop.cpp` (the scripted-backend idiom; each drives a REAL engine run and
reads the frame's committed events through a scan capture):

```cpp
/* A full scripted run's log shows the envelope in seq order: for one model
   cycle — turn.start, step.start (riding the turn's first durable record
   batch: the FRAME_STORE_CELL_RUN audit on the tool path / the content
   batch on the content path), the turn's records, step.end, turn.end —
   ONE atomic batch per record group (the riders share the existing batch's
   commit). Assert the ORDER and the payload fields (turn numbers
   monotonically +1 from 1; step numbers 1 within the turn). */
TEST(TestLoop, TestEngineRunWritesTheTurnEnvelopeInOrder) { ... }

/* Multi-cycle: two tool cycles + a finishing content turn end the engine —
   turns 1..k each close: tool-path turns via the cell.result batch's
   [step.end + turn.end {completed}]; the FINAL turn via the finish batch's
   [step.end + turn.end {terminal reason}]. turn.end's reason is NEVER NULL
   on any turn. */
TEST(TestLoop, TestMultiCycleTurnsEachClose) { ... }

/* The failure surface closes (the DSH finally-discipline): a scripted
   model error twice → model-error-final — the turn's turn.end carries
   reason "error" + text "model-error-final"; a turn that failed BEFORE the
   model replied still has its turn.start committed (turn entry) and gets
   its own turn.end — NO turn is ever left open by an alive engine. */
TEST(TestLoop, TestModelFailureClosesTheTurnWithError) { ... }

/* The turn cap: cap=1 (one cycle budget) — the second cycle's cap check
   fails BEFORE turn entry: the refused turn's turn.start never commits;
   the engine ends failed with the control "turn-limit" event; the log's
   newest lifecycle record is still turn 1's turn.end (balanced). */
TEST(TestLoop, TestTurnCapRefusesBeforeTurnEntry) { ... }

/* The turn-number restore: seed a session with lifecycle records (turn 3
   committed pre-restart), then start the engine — its first turn is 4
   (restored from the log, never renumbered from 1). */
TEST(TestLoop, TestTurnNumbersRestoreFromTheLog) { ... }
```

`test/test_frame.cpp` — the riders are ATOMIC with their batches:

```cpp
/* The children-yield shape: a content turn with live children pending
   yields at the finish reply (status stays running) — the yield's turn.end
   {reason completed} rides the SAME batch as the msg.append + NO status
   put (the yield batch's exact shape preserved); a later child report
   resumes the engine. Assert the batch commit is atomic (nothing
   half-committed on the yield's seq boundary). */
TEST(TestFrame, TestChildrenYieldTurnEndRidesTheFinishBatch) { ... }
```

- [ ] **Step 2: The riders (loop.c — reality-check first, then edit)**

READ THE PHASE MACHINE FIRST (loop.c:19-104's overview + `frame_run_loop` :1354+ and
each compose site). The integration's REQUIREMENTS are pinned; ADAPT the exact code
to the file's reality. The five riders (loop.c ONLY — frame.c untouched this task):

1. **Turn entry** (after the turn cap check passes, before the model dispatch — find
   the repost/turn-step entry point; the entry must happen on EVERY cycle including
   the first): compose `lifecycle_turn_start_json(turn)` and post it fire-and-post
   (`_frame_event_post_fire(f, LIFE_EVENT_TURN_START, payload)`) — the control events'
   discipline: a refusal logs loud and the engine continues. The turn counter lives on
   the engine state; RESTORE it lazily when first needed: from the events the derive
   already scanned (the newest recorded turn number + 1) — no extra scan; before that
   first restore the counter is 0-unknown (the restore's loud gap rule). If on a given
   turn entry the counter has NOT been restored yet (engine start before any derive
   scanned), restore from the scan the entry path itself can see on the frame's
   existing event material OR — pinned simpler: restore from the PREVIOUS turn's
   derive scan reply kept on the engine state (the engine retains no message history;
   the derive's parsed events die each turn — so keep ONE uint64_t `turn_counter` +
   `turn_known` flag on the engine state, restored from the derive's scan on the first
   entry, +1 per entry thereafter). A dead engine never carries the counter across a
   restart: the resume path (Task 3) restores through the log.
2. **`step.start`** rides the turn's first durable record batch: the tool path's
   `FRAME_STORE_CELL_RUN` batch composer (frame.c's cell-run audit — find where the
   cell.run record's batch is composed; ADD the step.start record as an put op in the
   SAME batch, composed by `lifecycle_step_json(e->turn, 1)` with the seq allocated
   from the frame's own seq counter — the batch's seq pre-allocation discipline) and
   the content path's `FRAME_STORE_FINISH` batch (`_frame_engine_finish_post` — the
   msg.append's batch) the same way. If a batch's composer gains an op, its seq
   allocation + refusal rollback (`_frame_seq_rollback`) handling adapts exactly as the
   other ops' do (read `_frame_engine_finish_post`'s put_ops + seq handling first).
   NOTE the counter restore reads the derive's scan BEFORE turn entry — see 1's pin.
3. **`step.end` + `turn.end {completed}`** ride the cell.result write on the TOOL path:
   the normal cell.result is posted at frame.c's cell-reply path (find it) and the
   ENGINE's refusal-paired compose is loop.c:960-990 — BOTH sites gain the riders so
   the pair is never split by a crash between them (pin: replace the single-record
   fire with ONE fire-and-post batch of [cell.result, step.end, turn.end] — if the
   single-record `_frame_event_post_fire` cannot carry three records, ADD the minimal
   fire-and-post batch variant to frame.c (declarations in frame_internal.h) and use
   it — the ONLY allowed addition this task makes anywhere outside loop.c), and the
   next turn re-derives from them (existing behavior).
4. **Terminal `turn.end`** rides the FINISH batch (`_frame_engine_finish_post`): the
   content path's turn closes `completed` (an ok content turn ends the engine; the
   CHILDREN-yield case ends the TURN completed too — §4.6's pin) — with the terminal
   reason when the engine's terminate is the failure path (`_frame_engine_terminate`:
   the reason = `LIFE_REASON_ERROR` + text = the control kind's wording verbatim,
   `LIFE_REASON_TURN_LIMIT` is emitted by NO writer today — spec §5).
5. **Engine-state bookkeeping** (the finally-discipline's bookkeeping): `step` tracking
   lives on the engine state (`turn_open`/`step_open` flags mirrored in memory so the
   terminate + finish paths know what their batch must carry — the store's records
   stay the truth; the flags are the compose-time facts).

Realism note: the two cell.result write sites + both batch composers + the terminate
are the FIVE edit sites; keep every other engine behavior byte-identical (the derive,
the ring, the caps, the model call, the CHILDREN machinery, the resume guard). The
turn-number restore's gap rule: a restored turn counter whose log has gaps (e.g. a
crash between cycles) logs loud and continues (the `_frame_restore_seq` discipline).

- [ ] **Step 3: Green + suite + ASan + no-locks grep**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
cmake --build cmake-build-asan --target testsecretagent -j && setarch -R ctest --test-dir cmake-build-asan --output-on-failure 2>/dev/null | tail -3
{ grep -rn "platform_mutex\|platform_rwlock\|platform_barrier\|platform_sem" src/Frame/ \
  && echo "VIOLATION: a lock in the frame layer"; } || echo "no locks in the frame layer"
```

- [ ] **Step 4: Valgrind filters broadened**

`TestLifecycle.*:TestEngineRunWritesTheTurnEnvelopeInOrder:TestMultiCycleTurnsEachClose:TestModelFailureClosesTheTurnWithError:TestTurnCapRefusesBeforeTurnEntry:TestTurnNumbersRestoreFromTheLog:TestFrame.TestChildrenYieldTurnEndRidesTheFinishBatch` (stripped, verified-run OK-count).

- [ ] **Step 5: Commit**

```bash
git add src/Frame/loop.c src/Frame/frame_internal.h src/Frame/frame.c \
  test/test_loop.cpp test/test_frame.cpp
(git add on the files ACTUALLY changed this task — the minimal variant is
 frame_internal.h + frame.c ONLY IF the fire-and-post batch variant was needed)
git commit -m "feat: engine writes the turn/step envelope with terminal reasons"
```

---

### Task 3: The resume repair — tail scan, closers, ONE batch; the derive's repair branch

`frame_resume` folds the cursor, composes the closers, commits ONE atomic batch before
the engine starts; the derive projects the `repair` brief.

**Files:**
- Modify: `src/Frame/frame_internal.h` (the closer-path helper's declaration — the
  resume repair composes in frame.c; declare what the task needs)
- Modify: `src/Frame/frame.c` (the resume closer path + NOTHING else)
- Modify: `src/Frame/loop.c` (ONE projection branch — the derive's `repair` render)
- Modify: `test/test_loop.cpp`, `test/test_frame.cpp`

- [ ] **Step 1: Tests first (restart extension + the derive branch + idempotency)**

`test/test_loop.cpp` / `test/test_frame.cpp` (the reload/scan-capture + pump idioms;
each drives REAL store round trips):

```cpp
/* The crashed-tail variant × the two cut windows:
   (a) cell.run committed, cell.result NOT (cut after the audit): resume
       composes [repair (quoting the cell's seq + code), step.end, turn.end
       {interrupted}] as ONE atomic batch BEFORE the engine starts; the
       next derive's captured request carries the repair brief VERBATIM as
       a user message; the original records are byte-identical after
       (append-only, NOTHING mutated); balance restored (the newest
       lifecycle record is the turn.end).
   (b) turn.start committed, NO step/no cell.run (cut before the audit):
       resume composes [repair (the not-started text), turn.end] — no
       step.end (no open step; DSH's order holds). */
TEST(TestLoop, TestRestartRepairsTheCutAfterTheCellAudit) { ... }
TEST(TestLoop, TestRestartRepairsTheCutBeforeTheCellAudit) { ... }

/* Idempotency: a SECOND resume over a repaired tail composes NOTHING (the
   balance rule; the second resume's next derive carries no second brief);
   an ENGINE-PAUSED frame (child-pending) resuming fresh — balanced tail
   (the children-yield turn ends completed) — composes nothing. */
TEST(TestFrame, TestSecondResumeComposesNothing) { ... }

/* The pre-lifecycle log (records with NO lifecycle types at all): resume
   composes nothing (balanced) — the envelope's absence is not a
   truncation. */
TEST(TestFrame, TestPreLifecycleLogResumesUntouched) { ... }

/* The pooled-store refusal: resume on a POOLED store refuses loud without
   hanging (the sync family's discipline — `_frame_sync_store_refused`).
   */
TEST(TestFrame, TestResumeRepairRefusesOnPooledStore) { ... }
```

(The restart tests ride `test_loop.cpp`'s TestRestartReplayRestoresSeqAndContext's
scratch-disk idiom — mkdtemp-style fresh dirs, TMPDIR-based, created and destroyed
in the test, never `sa-demo-db`.)

- [ ] **Step 2: The contracts**

`src/Frame/frame_internal.h` — the resume repair's helper:

```c
/* --- the resume repair (spec §4) ------------------------------------------ */

/* The crash-repair pass frame_resume runs before the engine starts, on the
   caller's thread (the sync family's discipline): fold the tail scan's
   events into a lifecycle cursor (SA_LIFECYCLE_TAIL_EVENTS window),
   compose the closers when the tail is UNBALANCED (the newest lifecycle
   record is not a turn.end), and commit ONE atomic batch of the closer
   records as event puts on the frame's events keys (the seq pre-allocation
   + rollback discipline; the seq counter advances by the closers' count).
   Empty closers = no-op. Refused scan/batch/deadline = resume fails loud
   (the sync family's documented consequences; resume refuses rather than
   half-repairs; `_frame_seq_rollback` releases the pre-allocated seqs on
   the compose-refusal path). Returns 0 (repaired or already balanced), -1
   loud otherwise. */
int _frame_resume_repair(frame_t* f);
```

`src/Frame/frame.c` — `_frame_resume_repair` implemented on the sync family's
discipline + called from `frame_resume`'s path (read frame.c's `frame_resume` + the
seq restore FIRST — the closer path rides the events' bounds + the key composition
the event writes already use; the batch composes the closers' records as put ops
whose keys are the zero-padded events keys (`_frame_events_bounds`' discipline,
`events/%020llu`); the ops' payloads serialize the closer DOMs (json_serialize); the
ownership story mirrors `_frame_sync_batch`'s composer-never-frees rule). `frame.c`
gains NOTHING else this task.

`src/Frame/loop.c` — the derive's pass B gains exactly ONE branch:

```c
    } else if (strcmp(type_name, LIFE_EVENT_REPAIR) == 0) {
      /* The crash-repair brief (spec §4): REPAIR events render as a user-
         role message, text verbatim — the model reads the full crash
         briefing in its next derive with the details upfront. The payload's
         text is capped at source (the derive's msg cap). */
      json_value_t* text_v = json_get(payload, "text");
      if (text_v != NULL) {
        const char* text = json_as_string(text_v);
        char* tt = _loop_trunc((text != NULL) ? text : "", SA_LOOP_MSG_CAP);
        json_value_t* m = json_new_object();
        json_object_set(m, "role", json_new_string("user"));
        json_object_set(m, "content", json_new_string(tt));
        free(tt);
        json_array_append(out, m);
      }
      /* A repair record with no text folds as nothing (malformed — the
         render-not-crash rule; the record's loud line already happened at
         fold time via the events parse). */
    }
```

(the branch rides RIGHT beside the `cell.result` branch; the ring's flush rule is
UNTOUCHED this slice — spec §3's pin)

- [ ] **Step 3: Green + suite + ASan + no-locks grep** (Task 2's commands)

- [ ] **Step 4: Valgrind filters broadened** (Task 2's list + the four new tests)

- [ ] **Step 5: Commit**

```bash
git add src/Frame/frame_internal.h src/Frame/frame.c src/Frame/loop.c \
  test/test_loop.cpp test/test_frame.cpp
git commit -m "feat: resume crash repair synthesizes the lifecycle closers"
```

---

### Task 4: Full verification + the Atlas node

**Files:**
- Modify: `atlas/workflow.json` (+ rebuilt `atlas/atlas.html`)

- [ ] **Step 1: All three configs green + valgrind + no-locks**

```bash
cmake --build cmake-build-debug -j && cmake --build cmake-build-asan --target testsecretagent -j
setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
setarch -R ctest --test-dir cmake-build-asan --output-on-failure 2>/dev/null | tail -3
cmake -S . -B cmake-build-off -DSA_ENABLE_PYTHON=OFF -DSA_ENABLE_WDB=OFF -DSA_ENABLE_STREAMS=OFF -DSA_BUILD_TESTS=ON && cmake --build cmake-build-off -j && setarch -R ctest --test-dir cmake-build-off --output-on-failure 2>/dev/null | tail -3
setarch -R valgrind --leak-check=full --suppressions=test/vg-cpython.supp --suppressions=test/vg-wavedb.supp /tmp/vg \
  --gtest_filter='TestLifecycle.*:TestEngineRunWritesTheTurnEnvelopeInOrder:TestModelFailureClosesTheTurnWithError:TestFrame.TestSecondResumeComposesNothing' 2>&1 | tail -4
{ grep -rn "platform_mutex\|platform_rwlock\|platform_barrier\|platform_sem" src/Frame/ \
  && echo "VIOLATION: a lock in the frame layer"; } || echo "no locks in the frame layer"
```

Expected: every config green (the OFF config carries test_lifecycle.cpp per Task 1's
registration note — verify), valgrind 0 errors (verified-executing — the OK-count
grep), "no locks in the frame layer".

- [ ] **Step 2: Atlas evidence — open the node truthfully**

`atlas/workflow.json`: a NEW leaf node matching the record shape of the refine slice's
`refine-continual-harness` node (kind "leaf", intent "delivery", status "in-progress"
until the owner accepts it — NEVER a fabricated "completed"):
`id: "turn-step-lifecycle-crash-repair"`,
`label: "Turn/step lifecycle envelope in the log + crash repair with model-visible closers"`.
Description carries: the envelope's batch-riding shape (the riders on existing batches +
the turn-entry fire-and-post), the reason union (the reserved members named), the
balance rule + the resume repair's ONE-batch/append-only rule, the brief's pinned
wording + details-upfront (owner decision), the deliberate non-ports (spec §5: no
dangling-provider problem, no live recovery class, flat records), the parity rows
4/5 citations, evidence references (the committed test names + the standing suite
counts), and the edge from atlas-root / to the steering-interrupt slice's needs as the
schema dictates (the exact edge/references structures the sibling nodes use — the
built atlas.html's rebuild must emit no error).

```bash
cd atlas && node build.js && node build.js --check && node validate.js
```

- [ ] **Step 3: Commit(s)**

```bash
git add atlas/workflow.json atlas/atlas.html
git commit -m "docs: turn-lifecycle slice evidence; atlas lifecycle node"
```

---

## Acceptance criteria (whole plan)

1. All three configs green (ON / ASan under `setarch -R` / OFF; test_lifecycle.cpp on
   the plain gate so the OFF build carries the pure suite).
2. Valgrind clean on TestLifecycle.* + the touched test_loop/test_frame filters
   (verified-executing runs only — the OK-count grep).
3. The no-locks grep stays clean (the store actor remains the only serializer; model.c's
   recorded exceptions unchanged).
4. Every lifecycle write rides an existing batch (or the one turn-entry fire-and-post);
   the resume repair is ONE atomic batch; the log is append-only — the closer records
   never mutate an original.
5. The envelope pairs on every exit (the DSH finally-discipline): no turn ever left open
   by an alive engine; `turn.end`'s reason is never NULL; `turn-limit` emits no records
   today (spec §5's pin).
6. The brief is model-visible with the details upfront: the interrupted cell's code + seq
   quoted, both shapes pinned verbatim in tests.
7. The balance rule is the whole crash check: a repaired tail re-resumes into a no-op;
   pre-lifecycle logs resume untouched.
8. The freeze list holds (spec §6): only lifecycle.{h,c} + the named loop.c/frame.c hunks
   + the tests; no TODOs anywhere touched; atomic conventional commits; no Co-Authored-By.
9. Atlas updated truthfully (a new in-progress node with REAL evidence).

## Known pending (recorded, not built here)

- The cancel fence / wake latching and `aborted`/`blocked` writers: the steering /
  interrupt slices own them (spec §9); the union's names are reserved.
- The pooled-engine cell watchdog: the interrupt slice's decision (matrix open Q4).
- Multi-step turns (steering/refinement-in-cycle): the `step` level's future; today one
  step per turn.
- The derive's ring flush rule ("only results since the newest msg.append") stays
  untouched; a lifecycle-aware refinement is slice 3's budget-table business.
- DSH's per-event `turn`/`step` tagging of every record type: not built (spec §5 — flat
  records; the five new types carry the numbers).