# Escalation Implementation Plan (L5's escalation half)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The blocked-ask: `agent.ask` publishes a durable ask, the turn closes `blocked`, the engine parks, the reply (wire pair CA_ASK_REPLY 16/17) lands as a user-side msg.append and the next turn re-enters; plus the ladder (`escalation_mode`: free | plan-ask-act | bypass — children inherit) and the park's wake latching.

**Architecture:** NOTHING blocks and nothing spins — the ask is the model-submit park's shape: publish → one atomic close batch (ask record + `turn.end{blocked}`) → `FRAME_PHASE_ASK` → `FRM_ASK_REPLY` corr/ask-id-matched → reply batch (`EV_ASK_REPLY` + user-side msg.append) → self-posted `FRM_TURN`. Approvals with lifetime = the durable `plan-approved` control record; the wake latches live at the park's quiescence point.

**Tech Stack:** C11 + WaveDB store actor + the json util + libcbor (the wire) + the existing script/capture test harnesses.

**Spec:** docs/superpowers/specs/2026-10-06-escalation-design.md — READ FIRST.

**Standing gates:** `setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure` (339/339 at dfbc9d0) + ASan (`cmake-build-asan`) + OFF (`cmake-build-off`) + off-verify + valgrind (strip --strip-debug copies; FULL Suite.Test filters) + no TODOs + no Co-Authored-By + STYLE_GUIDE.md.

**Style law for this slice:** the loop (L3) stays amoral — the ladder's gate reads a config field; the ask verb is ordinary publish-data; every refusal is data the model reads, never an exception; every write rides ONE atomic batch.

---

### Task 1: the ask vocabulary + `agent.ask` (publish-only)

**Files:** Modify `src/Frame/frame_messages.h`, `src/Python/pyrt_messages.h`, `src/Python/pyrt.c`, `src/Python/py_agent.h/c`; Test `test/test_py_agent.cpp`.

- [x] **Step 1: the failing tests** (`test/test_py_agent.cpp`, the `bridge_frame_t` harness — records `results`/`emits`; ADD an `asks` vector recording FRM_ASK payloads):

```cpp
TEST(TestPyAgent, TestAskPublishesWithoutTheAnswer) {
  /* bridge_frame_execute(self, "import actor\nprint(actor.ask('which db?', ['a','b']), end='')")
     pump; results[0]->status==0; the printed marker string is the ASK's value
     ("asked"); asks.size()==1 with the question + both options; NOT waited on
     (the cell COMPLETED — the verb is fire-and-post, the emit pattern). */}

TEST(TestPyAgent, TestAskRefusalsAreData) {
  /* actor.ask('') → the refusal string ("ask: the question is empty") and NO
     FRM_ASK posted. 9 options → refusal (>8). One option '' → refusal. A
     non-list options → refusal. NONE of these post. */}

TEST(TestPyAgent, TestAskRefusedWhileAParkIsPending) {
  /* pyrt's ask_parked flag SET by hand (pyrt_ask_parked_set(self->pyrt, 1)) →
     actor.ask(...) → the refusal "ask already parked — reply pending"; NO
     second publish. Clear the flag (set 0) → publishes again. */}
```

- [x] **Step 2: the frames vocabulary** (`frame_messages.h`): `FRM_ASK` + `FRM_ASK_REPLY` appended to `frame_message_type_e` (after `FRM_STEER` — KEEP the store vocabulary's block intact); event types `EV_ASK`, `EV_ASK_REPLY` appended to `frame_event_type_e`. Payload structs beside `frm_steer_payload_t`:

```c
typedef struct frm_ask_payload_t {
  uint64_t corr;            /* the bridge corr (the reply-sink space) */
  char* question;
  char** options;           /* owned array of owned strings */
  size_t noptions;
} frm_ask_payload_t;
typedef struct frm_ask_reply_payload_t {
  char* ask_id;             /* the "%08x"-shaped key — the sid allocator's */
  uint8_t decision;         /* 0 = answer, 1 = reject */
  char* value;              /* the answer/refusal text — may be empty */
} frm_ask_reply_payload_t;
void frm_ask_payload_destroy(void* p);
void frm_ask_reply_payload_destroy(void* p);
```

  The `frm_ask_payload_destroy` frees question + each option + the array (the `frm_report_payload_destroy` shape). Event-type→name mapping (the compose switches — grep `EV_MSG_APPEND`'s name sites in frame.c): `EV_ASK → "ask"`, `EV_ASK_REPLY → "ask.reply"`.
- [x] **Step 3: the pyrt flag** (`pyrt_messages.h` + `pyrt.c`): the pyrt instance struct gains `ATOMIC(uint8_t) ask_parked;` cleared at create; accessors `void pyrt_ask_parked_set(pyrt_t*, uint8_t)` + `uint8_t pyrt_ask_parked(const pyrt_t*)` (the `pyrt_interrupt`'s interrupt_req-arm shape — the SAME instance, no TLS trick).
- [x] **Step 4: the verb** (`py_agent.c`) — follow `_py_agent_emit`'s NON-blocking shape verbatim (fire-and-post; NEVER `_py_agent_request`'s bounded wait):
  - `_py_agent_ask(self, args)`: `PyArg_ParseTuple(args, "s|O:ask", ...)`; question → `_py_agent_text_of` + `budget_truncate_with_marker` refusal rule — the question cap = `SA_BUDGET_BRIDGE_VALUE_BYTES`; EMPTY-after-strip → the refusal string return; options argument: absent = no options; a Python list → each item via `_py_agent_text_of` (non-str → repr like report), empty item or list-length > 8 → the refusal string; ANY refusal → return WITHOUT posting (data, `Py_RETURN_NONE`? NO — return the refusal STRING so the model reads it; the marker/publish returns "asked").
  - The parked pre-check FIRST: if `pyrt_ask_parked(_pyrt_thread_py())` — the emit's owner-lookup idioms give the py pointer — return `"ask already parked — reply pending"`.
  - Box `frm_ask_payload_t {corr = _py_agent_next_corr(), question, options}`, `actor_send(py->owner, FRM_ASK)`, return `"asked"`.
- [x] **Step 5:** harness gates green (test_py_agent suite); run `setarch -R ctest --test-dir cmake-build-debug --output-on-failure -R 'TestPyAgent'` (FULL names if filtering) then the whole suite; commit
  `git add -A src/Python src/Frame/frame_messages.h test/test_py_agent.cpp && git commit -m "feat: agent.ask publishes the blocked-ask — fire-and-post, refusals are data, one park at a time"`.

---

### Task 2: the park — close batch, `FRAME_PHASE_ASK`, the reply, the stale drop

**Files:** Modify `src/Frame/frame_internal.h` (phase enum + engine state + `_frame_ask_reply_post`), `src/Frame/frame.c` (FRM_ASK / FRM_ASK_REPLY cases + the close composition + the public API + the pyrt flag set/clear), `src/Frame/frame.h` (the public pair); Test `test/test_frame.cpp`.

- [x] **Step 1: the failing tests** (test_frame.cpp — the inline scripted shape: `wave_db_open(NULL)` + `test_config()` + `frame_set_model_backend`; the harness helpers `load_events/event_is/rec_seq/payload_of/count_type` are in test_loop.cpp — COPY the four small helpers into test_frame.cpp or reuse via the shared test util if one exists):

```cpp
TEST(TestFrame, TestAskParksAndBlocksTheTurn) {
  /* run a turn whose cell calls actor.ask("q?", ["yes","no"]); frame_run_loop
     returns with the frame NOT done; load_events: the SAME batch holds
     cell.result + the "ask" record {askId, question, options[yes,no]} +
     step.end + turn.end{reason.kind=="blocked"}; the engine parks (no more
     turns issue — a second frame_run_loop pumps NOTHING); after
     frame_ask_reply(f, ask_id, 0, "yes") the reply batch lands:
     the "ask.reply" record {askId, decision "answer", value "yes"} +
     msg.append (user role, content carries "yes") and a fresh turn starts
     (its derive shows the answer — pin via the model capture on turn 2). */}

TEST(TestFrame, TestAskStaleReplyIsDroppedLoud) {
  /* frame_ask_reply with a WRONG ask_id ("" and "00000000") → no reply
     batch, no new turn; the engine STILL parked; then the RIGHT id works. */}

TEST(TestFrame, TestAskSecondAskInSameTurnRefused) {
  /* a cell calling actor.ask twice → ONE ask record; the SECOND verb call
     already returned the refusal (Task 1's flag — the engine sets the flag
     at FRM_ASK receipt, NOT at park). */}
```

- [x] **Step 2: the engine state + the phase** (`frame_internal.h`):
  - `FRAME_PHASE_ASK` appended to `frame_phase_e`.
  - `frame_engine_state_t` gains `struct { char* ask_id; uint64_t corr; uint8_t plan_gate; }
    pending_ask;` (`plan_gate` = the ladder's runtime-authored ask — Task 5's consumer; 0 here)
    and `turn_known`'s blocked case needs NOTHING (the fold's KNOWN list is already there).
  - Post helper decl: `_frame_ask_reply_post(frame_t* f, const char* ask_id, uint8_t decision, const char* value)` — posting-only, 0-once-posted (the `_frame_steer_post` shape verbatim, `frame_internal.h:248`).
- [x] **Step 3: the public API** (`frame.h`):

```c
/* Deliver an owner-surface reply for a parked ask. 0 = the FRM_ASK_REPLY was
   POSTED (the park's truth lands asynchronously — never a commit confirm);
   rc<0 = the frame ref/args were invalid. Bounded: ask_id <= 40 chars,
   decision 0|1, value capped at the bridge value budget. */
int frame_ask_reply(frame_t* f, const char* ask_id, uint8_t decision,
                    const char* value);
```
- [x] **Step 4: the engine cases** (`frame.c`, the `_frame_behavior_impl` switch — insert after the `FRM_STEER` case at :3731):  **FRM_ASK** case:
  1. Validate the payload (question/options non-NULL); garbage → destroy + `log_error` + `return`.
  2. If `e->pending_ask.ask_id != NULL` → destroy + `log_error("frame: a second ask arrived while one is parked (%s) — refused", ...)` + `return` (the belt behind the verb's flag).
  3. Mint `ask_id` via `_frame_sid_generate(root, buf)` (the ROOT allocator, `frame.c:502` — the same 8-hex shape; the sid's mechanism, one allocator).
  4. OWN the ask into `e->pending_ask` (steal the payload's strings). NOTE (post-Task-1 review):
     the pyrt flag is set at the VERB's post-publish — do NOT set it here; just clear it at the
     consume paths (reply / the park's wake branches).
  5. Return — the close batch happens at the cell's completion.
- **The close batch**: extend the PYRT_RESULT → `_frame_engine_result_close_post` path (`frame.c:3487-3496`): when `e->pending_ask.ask_id != NULL` AND the turn closes normally, `with_riders` gains the ask + blocked compose — implement as a sibling `_frame_engine_ask_close_post(f, e, result_payload)` (the `_frame_engine_result_close_post` shape, `frame.c:2476-2507`): ONE `_frame_event_batch_post_fire` with `[cell.result(status 0), step.end, the "ask" record, turn.end{LIFE_REASON_BLOCKED, NULL}]`; the "ask" record's JSON: `payload {askId, question, options[], plan:<ask's plan text or null>}` (compose with the json util's set-string/set-value pairs — the `_frame_msg_append_payload` shape at `frame.c:2833`); on rc==0 clear `turn_open/step_open`, set `phase = FRAME_PHASE_ASK`.
- **FRM_ASK_REPLY case**: validate; if `e->pending_ask.ask_id == NULL || strcmp(ask_id, e->pending_ask.ask_id) != 0` → `log_error("frame: a reply for ask %s arrived — no park holds it (current %s) — dropped", ...)` + `return` (LOUD + async: the wire's ack CANNOT see this — the pinned §3.1 contract). Match: clear the park FIRST (steal the payload for the record), then ONE `_frame_event_batch_post_fire(f, names, payloads, 2+…, "ask reply")`:
  1. The `"ask.reply"` event `payload {askId, decision, value}` (decision as "answer"/"reject" string — the fold renders it).
  2. The `msg.append` via `_frame_msg_append_payload(role "user", content)`: content = `answer` → the value (or "Owner declined." when reject + empty value); REJECT-with-value → the value verbatim (the model's revision input); the plan-gate's default wording is Task 5's (this task passes the value through verbatim).
  3. Clear `pyrt_ask_parked_set(f->pyrt, 0)`, `phase = e->turns' continuation` — after the batch, POST `FRM_TURN` to the frame's OWN mailbox (the `_frame_post(f, FRM_TURN, NULL)` idiom — verify who posts FRM_TURN today: `_frame_engine_turn` is INVOKED by the FRM_TURN case; `frame_start`/the run-loop's turn continuation posts it; mirror THAT exact site — the steer's re-entry idiom).
- **`frame_ask_reply`**: validate bounds → `_frame_ask_reply_post`. The handlers consume this in Task 7.
- [x] **Step 5:** gates green + `TestFrame` suite; the existing interrupt pins MUST stay green (the ask close is the sibling, not a rewrite); commit
  `feat: the ask parks — one close batch (ask record + turn.end{blocked}), the reply resumes as durable input`.

---

### Task 3: the park's wake latching (Q7's resolution)

**Files:** Modify `src/Frame/frame.c` (the FRM_STEER + FRM_INT cases' park branches); Test `test/test_loop.cpp` (the interrupt/steer precedents live here).

- [x] **Step 1: the failing tests** (test_loop.cpp — the interrupt precedent's shape: `TestFrameInterruptCutsCellAndAbortsTurn` at :1534 drives a killer thread; the park tests use the ask flow from Task 2):

```cpp
TEST(TestLoop, TestSteerDuringParkSupersedesTheAsk) {
  /* the ask parked; frame_append_msg(f,"user","do it differently") arrives;
     pump; the pending ask auto-answered: an "ask.reply" record {decision
     "reject", value "superseded by new user input"}; NO extra turn for the
     refusal alone; the parked engine served the STEER's turn — turn 2's
     derive carries the steer text. */}

TEST(TestLoop, TestInterruptDuringParkRefusesAndStaysResumable) {
  /* the ask parked; frame_interrupt(f); the ask.reply {reject, "interrupted
     at the frame's request"} lands; the frame is IDLE-RESUMABLE (phase NONE,
     NOT terminated — the resume pins: a later steer still yields a turn). */}

TEST(TestLoop, TestInterruptWinsOverSteerAtThePark) {
  /* both injected; the ask.reply says interrupted; the steer's msg.append is
     IN the log (durable) and served by the turn AFTER the idle resume. */}

TEST(TestLoop, TestMidTurnInterruptUnchangedUnderTheAskMachinery) {
  /* a cell running long + interrupt mid-cell: turn ends "aborted", the
     poison rules stand — today's pins hold BYTE-FOR-BYTE (the pre-slice
     TestFrameInterrupt* suite IS the pin). */}
```

- [x] **Step 2: the park branches** (`frame.c` — the two cases gain a leading guard when `e->pending_ask.ask_id != NULL && phase == FRAME_PHASE_ASK`):
  - **FRM_STEER**: the steer's `msg.append` FIRST lands (the standing case body runs unchanged), THEN the park auto-answer: compose the `ask.reply` record `{askId, decision:"reject", value:"superseded by new user input"}` as ONE batch (no turn.end — the turn is already closed; the steer's turn opens via the standing steer re-entry). Clear the park + the pyrt flag.
  - **FRM_INT**: take the PARK branch INSTEAD of `_frame_interrupt_apply`: ONE batch [the `ask.reply` record `{decision:"reject", value:"interrupted at the frame's request"}`, a `control` record `{kind:"ask-interrupted"}`]; clear the park + flag; phase = NONE; return — NO terminate, NO poison (a post-turn interrupt, the spec §4).
  - The BOTH case: the FRM_INT branch answers "interrupted" and the steer's msg.append (whatever ORDER the messages arrive) is durable — served by the next turn's derive naturally. NO ordering machinery — the interrupt's reply batch and the steer's append are independent batches; the test pins the OUTCOME.
- [x] **Step 3: the repair check** (test_lifecycle.cpp addition):

```cpp
TEST(TestLifecycle, TestRepairLeavesABlockedTailBalancedAndRepublishless) {
  /* a log whose tail = [ask, step.end, turn.end{blocked}] feeds
     frame_resume's repair scanner — NO closers synthesized (balanced);
     the composer's blocked row already passes (the pinned :955 test) —
     THIS pins the REPAIR side's balance rule. */}
```
- [x] **Step 4:** gates + the FULL interrupt/watchdog suites stay green; commit
  `feat: the park's wake latch — steer supersedes, interrupt refuses, the mid-turn interrupt unchanged`.

---

### Task 4: the ladder's config — `escalation_mode` + inheritance

**Files:** Modify `src/Frame/frame.h:17-50`, `src/Frame/frame.c` (the THREE config-copy sites: `_frame_alloc` cfg copy :4437-4459 + the parent dup :4460-4483 + the resume's :4824-4842); Test `test/test_frame.cpp`.

- [x] **Step 1: the failing test**:

```cpp
TEST(TestFrame, TestEscalationModeInherits) {
  /* cfg.escalation_mode = PLAN_ASK_ACT → a POOLED parent's child config
     carries it (read the child-create inheritance site — mirror the
     persona_name test's pin shape); frame_resume with the cfg → carried;
     plain frame_create → FREE (0). */}
```
- [x] **Step 2: the field** (`frame.h`, beside `persona_name`):

```c
  /* L5 escalation ladder: 0 = free (execute free, never asks; default),
     1 = plan-ask-act (plan turns gate on one owner approval), 2 = bypass
     (DANGEROUS: plan turns auto-approve, ask verb refuses). Children
     inherit. Immutable after create. */
  unsigned escalation_mode;      /* frame_escalation_mode_e */
typedef enum frame_escalation_mode_e {
  FRAME_ESCALATION_FREE = 0,
  FRAME_ESCALATION_PLAN_ASK_ACT = 1,
  FRAME_ESCALATION_BYPASS = 2,
} frame_escalation_mode_e;
```
  The three copy sites copy the VALUE (an enum — no dup, the `max_depth` line's pattern). A >2 value at create → refused loud (the cfg-validation site — the depth-cap's refusal shape, fail loud per row 30).
- [x] **Step 3:** gates; commit `feat: the ladder's config — frame_escalation_mode with the three-way inheritance`.

---

### Task 5: the ladder — plan turns, the gate, approvals, BYPASS

**Files:** Modify `src/Frame/loop.c` (the derive/system-content + the model path's plan shape + the content path's gate + the approve's act persistence), `src/Frame/frame_internal.h` (the engine's `ladder_act` field), `src/Frame/frame.c` (the reply's plan-gate branch — Task 2's `plan_gate` flag consumer); Test `test/test_LOOP.cpp` + `test_frame.cpp`.

- [x] **Step 1: the failing tests** (test_loop.cpp — free-mode standing pins FIRST):

```cpp
TEST(TestLoop, TestFreeModeAsksOnlyWhenTheModelAsks) {
  /* FREE cfg + a model that never asks → BYTE-IDENTICAL to the pre-slice
     pins (the whole standing suite IS the pin — run it, no new code path
     taken: the model capture shows NO tools-null request, no plan block). */}

TEST(TestLoop, TestPlanAskActPlansToolsNullThenGatesThenActs) {
  /* cfg PLAN_ASK_ACT; scripted replies: [turn1 = plan text content-only,
     turn2 = a tool-call reply, turn3 = final content]. Drive:
     - turn 1's request capture: tools NULL-omitted (the refine shape —
       modeled.c _model_request_body's no-tools path), the derive's captured
       system prompt carries the plan-instructions block;
     - turn 1 closes with the "ask" record {question "Approve this plan?",
       options [Approve, Reject], plan <the text>} + turn.end{blocked};
     - frame_ask_reply(f, id, ANSWER, "Approve") → the reply batch ALSO
       carries the control {kind "plan-approved", auto false} record;
     - turn 2 runs a REAL tool round (the tools array rides again —
       "execute" present in the captured request's tools);
     - final content turn completes the frame normally. */}

TEST(TestLoop, TestPlanGateRejectReplansAndReplanCarriesTheText) {
  /* reject with value "" → the msg.append "Plan rejected: revise and
     re-propose"; the NEXT turn is plan AGAIN (tools-null). reject with
     value "skip step 3" → that text IS the append content. */}

TEST(TestLoop, TestApprovalPersistsAcrossRestart) {
  /* after approve: frame_destroy; frame_resume(new frame, same db) →
     the derive's scan sees the control record → act (turn 2 plan-shaped
     NEVER recurs: the next model request carries tools, NOT the plan
     block). */}
```

- [x] **Step 2: the engine's act state** (`frame_internal.h`): `frame_engine_state_t` gains `uint8_t ladder_act;` (0 = pre-gate). Set at approve; read at every turn entry.
- [x] **Step 3: the plan shape** (`loop.c` — the turn entry → model path):
  - In `_frame_engine_turn`'s flow (loop.c:1911-1983) / the turn entry: `uint8_t gating = (cfg->escalation_mode == PLAN_ASK_ACT && !e->ladder_act);`
  - The model request's tools param: `gating` (or a plan turn via the Task-2 reply cycle) → submit with the TOOLS-NULL value instead of the canned execute tool — `loop.c:1330` passes `mb->submit(mb, messages, NULL, ...)`; the null-tools VALUE path is `_model_request_body`'s (`model.c:568-569`, the JSON-null omission) — find refine's aux-call for the exact null-tools argument text (grep `refine_run`'s submit call) and pass THE SAME.
  - `_loop_system_content` (`loop.c:361-411`): a `plan_prefix` block riding AFTER `persona_prefix`/before `SA_LOOP_INSTRUCTION` when `gating` — a small fixed text (the plan instruction: "You are in PLAN mode: propose a concrete plan for the goal. Do not execute. Your reply IS the plan." — bounded, one string constant in loop.c; the byte-stability rule: constant per mode).
- [x] **Step 4: the gate + the phases**:
  - **PLAN_ASK_ACT + not approved**: the CONTENT path (`_loop_content_path`, loop.c:1161-1192) branches BEFORE `_frame_engine_finish_post`: instead of finishing, the engine runtime-authors the gate ask — mint ask_id, `pending_ask {ask_id, corr 0, plan_gate 1}`, one batch `[control {kind:"plan-requested"}, the "ask" record {question "Approve this plan?", options ["Approve","Reject"], plan <the turn's content, budget-capped via SA_BUDGET_BRIDGE_VALUE_BYTES>}, step.end, turn.end{blocked}]` + park (the Task-2 close shape, minus cell.result). Set `pyrt_ask_parked_set(f->pyrt, 1)`.
  - A TOOL-call reply during a PLAN turn: the model cannot call tools (tools-null) — but a model ignoring the contract returns tool_calls anyway: treat as the standing content path refuses? NO — fail the turn loud (`_loop_fail` "plan-mode: the model sent tool calls in a plan turn") — the retry table's non-retryable class... the standing model-shape validation already rejects malformed replies; add this refusal loudly, turn ends error, the NEXT turn is STILL plan (the loop resumes).
  - `agent.ask` during plan: the Task-2 flow (turn closes blocked; reply lands; since `!e->ladder_act`, the next turn is plan again).
- [x] **Step 5: the approve/reject** (`frame.c`'s FRM_ASK_REPLY case): when the consumed park's `plan_gate == 1`:
  - `decision == answer` → the reply batch GAINS the `control {kind:"plan-approved", auto:false}` record (THIRD record in the same batch); the engine sets `e->ladder_act = 1`; the msg.append content = the value or "Approved." (the derive shows the approval; the model re-enters act).
  - `decision == reject` → NO control record; msg.append = "Plan rejected: revise and re-propose" (empty value) or the value verbatim.
  - BYPASS: no ask ever — the plan turn's close composes `[control {kind:"plan-approved", auto:true}]` INSTEAD of the ask record (the same batch shape, no park), sets `ladder_act = 1` in-memory, and the flow CONTINUES to the next turn (self-posted FRM_TURN): plan → act with no gap. `agent.ask` under BYPASS → the verb's refusal — needs the flag? NO: the mode check at the VERB? The verb can't see the mode... the ENGINE: FRM_ASK under BYPASS is refused at the verb via a mode-flag check the engine publishes to pyrt at create/approve-time: extend the pyrt flag to `pyrt_escalation_bypass` (the same instance field, set at frame create when BYPASS, inherited children get their own pyrt) — the verb's SECOND pre-check returns "escalation bypassed: asked questions have no answerer".
- [x] **Step 6: the recovery consult**: `_loop_engine_on_derive`'s DOM scan (the doom-seq scan rides :1610s) ALSO recognizes `control {kind:"plan-approved"}` records in the derive window → `e->ladder_act = 1` (in-memory recovery; the derive scan is bounded — a window that has scrolled past the approval record re-gates: an EXTRA ask, never a silent skip — the recorded safe default; note it in the test's comment).
- [x] **Step 7:** gates (the standing pins + the new ladder tests; the FREE-mode byte-identity pins MUST stay green — the persona slice's no-persona pin pattern); commit
  `feat: the plan-ask-act ladder + bypass — tools-null plan turns, the durable approval, the gate consult`.

---

### Task 6: the wire pair — CA_ASK_REPLY 16/17

**Files:** Modify `src/ClientApi/client_api_wire.h`, `src/ClientApi/client_api_wire.c`; Test `test/test_client_api_wire.cpp`.

- [x] **Step 1: the failing test** (the `TestPromptRequestRoundTrip` shape, wire tests :13-39):

```cpp
TEST(TestClientApiWire, TestAskReplyRoundTrip) {
  /* [16, req_id, sid, ask_id, decision, value] → decode: type 16, req_id,
     sid, ask_id, decision 0/1, value; reject/decision 1 + empty value also
     round-trips. Response [17, req_id, delivered] round-trips with the
     bool. Bounds: ask_id > 40 → decode refusal (-1); decision > 1 → -1;
     value over the wire's text cap → -1. */}

TEST(TestClientApiWire, TestAskReplyPairAssertsExtend) {
  /* the compile-time asserts COMPILE (16 = 15+1, 17 = 16+1) — build failure
     is the test; the runtime test just exercises the pair. */}
```
- [x] **Step 2: the header** (`client_api_wire.h:61-119`): `#define CA_ASK_REPLY_REQUEST 16` / `#define CA_ASK_REPLY_RESPONSE 17` (AFTER `CA_CONFIG_RESPONSE 15` — the adjacency holds; the assert chain extends: `CA_STATIC_ASSERT(CA_ASK_REPLY_REQUEST == CA_CONFIG_RESPONSE + 1, ...)` + the response=request+1 pair). Payload structs (the prompt struct's shape, req_id FIRST):

```c
typedef struct ca_ask_reply_request_t {
  uint64_t req_id;
  char* sid;        /* heap; required */
  char* ask_id;     /* heap; required, <= 40 chars */
  uint8_t decision; /* 0 = answer, 1 = reject */
  char* value;      /* heap; may be empty → NULL by the "" rule */
} ca_ask_reply_request_t;
typedef struct ca_ask_reply_response_t {
  uint64_t req_id;
  uint8_t delivered; /* 1 = the reply entered the frame's mailbox (bind/post
                        ONLY — the engine's stale drop is events-stream truth) */
} ca_ask_reply_response_t;
```
  Bounds: `#define CA_WIRE_ASK_ID_MAX 40u` (the 8-hex shape ×5 headroom — the doc-comment); value cap = `CA_WIRE_TEXT_MAX`.
- [x] **Step 3: the codec** (`client_api_wire.c`): `_encode_ask_reply_request`/`_encode_ask_reply_response` (the `_encode_interrupt_*` idiom verbatim — `cbor_new_definite_array` + `_push_u8/_push_u64/_push_string`, the decision as `_push_u8`); `_decode_ask_reply_request`/`_decode_ask_reply_response` (the `_decode_interrupt_request` shape: array-size check, `_decode_sid_element` for sid + ASK_ID (a second `_decode_sid_element` call with `CA_WIRE_ASK_ID_MAX`, require=1), `_decode_u8` for decision with the `> 1` refusal, the destroy-on-failure shape verbatim); the encode switch (:806-851) + `_decode_frame`'s switch (:717-794) + `ca_wire_payload_destroy`'s cases extend (the interrupt-response case's `free(payload)` shape — CAREFUL: ask-reply's REQUEST carries three heap strings — its destroy frees sid/ask_id/value then the payload).
- [x] **Step 4:** gates; commit `feat: the wire's ask-reply pair — CA_ASK_REPLY 16/17, bounded, decode both bounds' refusals`.

---

### Task 7: the handlers + `sa_client_ask_reply`

**Files:** Modify `src/ClientApi/handlers.h/c`, `src/ClientLibs/c/sa_client.h/c`; Test `test/test_client_api_handlers.cpp`, `test/test_sa_client.cpp`.

- [x] **Step 1: the failing tests** (handlers):

```cpp
TEST(TestClientApiHandlers, TestAskReplyBindsAndPosts) {
  /* the fixture's scripted frame parked on an ask (drive a plan-ask turn);
     ca_session_handle(CA_ASK_REPLY_REQUEST {req_id 3, sid, ask_id, 0,
     "Approve"}, &conn.iface); conn_wait_count(2, 600); recorded frame = the
     response {req_id 3, delivered 1}; the frame's events scan later shows
     ask.reply + msg.append (the engine consumed it — pump). */}

TEST(TestClientApiHandlers, TestAskReplyUnboundGetsDeliveredFalse) {
  /* unknown sid → CA_ERROR; known sid + frame_is_done → the response
     {delivered 0}. A stale ask_id still answers delivered 1 (the POST
     succeeded — the pinned ack contract; the engine drops it async). */}
```
  (sa_client:)

```cpp
TEST(TestSaClient, TestAskReplyRoundTrip) {
  /* the REAL server+unix fixture; a parked ask (the handlers test's shape
     via the shared fixture); sa_client_ask_reply(client, sid, ask_id, 0,
     "Approve", cb, ctx) → cb(status 0); the SECOND call while the first is
     in-flight → refused (-1, the one-in-flight slot). */}
```
- [x] **Step 2: the handler** (`handlers.c`, the dispatch switch :1411-1430 gains the case; the handler mirrors `_ca_on_interrupt` :1336-1368): NULL/sid → `_ca_send_error`; `_ca_reg_find` fail or `frame_is_done` → `_ca_send` the response `{delivered:0}`; else `frame_ask_reply(f, ask_id, decision, value)` → rc==0 → `{delivered:1}`, rc<0 → `{delivered:0}` + loud log. NO new pending-kind (the fire-and-post needs no store reply).
- [x] **Step 3: the client op** (`sa_client.h` beside `sa_client_interrupt` :228, `sa_client.c` beside :875):

```c
/* Reply to a parked owner-surface ask. BLOCKING (the prompt/interrupt
   contract; ONE in-flight per connection). The events-callback re-entry
   rule covers this op: NEVER reply from inside an events callback (the
   caller's own thread). */
int sa_client_ask_reply(sa_client_t*, const char* sid, const char* ask_id,
                        uint8_t decision, const char* value,
                        sa_client_interrupt_cb_t cb, void* ctx);
```
  The body = `sa_client_interrupt` verbatim with the ask-reply request build + `CA_ASK_REPLY_RESPONSE` as `want_type`; the delivered byte rides via... the cb gets `status` only — extend `sa_client_interrupt_cb_t`? NO — keep the cb signature; the DELIVERED flag: on `delivered 0` fire cb with `fail` (status 1) + `log_error("sa_client: the ask reply was not delivered (sid %s)")`; delivered 1 → cb(0). Document in the header comment.
- [x] **Step 4: the transports e2e** (test_client_api_transports.cpp): one test over unix + one over TCP-auth mirroring `TestInterruptReachesTheFrame`-style transit (the raw `test_client_t` double writes the encoded 16-frame, reads the 17-frame) — the framer's 2 MB cap and teardown order untouched.
- [x] **Step 5:** gates green (all four client suites); commit
  `feat: the ask-reply over the wire — the handler binds sid→frame, sa_client_ask_reply blocks per the interrupt contract`.

---

### Task 8: the demo CLI — serve `--escalation` + the client's read loop + answer

**Files:** Modify `tools/frame-demo/main.c`; (the handlers' server config if present — check `ca_server_config_t`/`ca_session_server_create`'s cfg struct for where top-frame creation builds `frame_config_t` at `_ca_on_prompt`'s sid==NULL branch :1030-1110 — the server config gains `unsigned escalation_mode`; the demo fills it from the flag).

- [x] **Step 1: the serve flag** (`_demo_parse_serve`, :384-434): `--escalation plan-ask-act|bypass` (strcmp; unknown value → usage + exit 1; absent = free); `demo_serve_args_t` gains the field; the serve setup passes it into the server config struct (extend `ca_session_server_create`'s cfg + `_ca_on_prompt`'s frame_create sets `cfg.escalation_mode`); the usage text (:113-139) documents the three.
- [x] **Step 2: the client's read loop** (`_demo_client_run`, :707-772): replace the bare `while (!g_demo_sigint) usleep(50000);` (:767) with a poll loop that ALSO drains stdin lines:

```c
  /* after subscribe: */
  while (!g_demo_sigint) {
    _demo_client_stdin_pump(&pr);   /* non-blocking: poll(2) stdin 50 ms;
                                       a full line → dispatch below */
    usleep(20000);
  }
  /* the line parser: "answer <sid> <pick|reject> [text]" →
     sa_client_ask_reply(client, sid, ask_id, reject?1:0, text_or_pick,
     _demo_answer_cb, &pr);
     "quit" → break; anything else → usage one-liner. ask_id values arrive
     from the ask records the events callback remembers (the LAST unparked
     ask per sid — a small registry keyed by sid, freed at teardown). */
```
- [x] **Step 3: the ask render** (`_demo_events_cb`, :682-696): parse each record's JSON (the json util — the demo already links it); a `type "ask"` record renders:

```
== ASK <sid> <ask_id> ==
  Approve this plan?
   1. Approve
   2. Reject
   (answer <sid> <ask_id> <pick|reject> [text])
```
  plan renders as an indented block under the question; `ask.reply` renders one line (`answered: <value>` / `rejected: <value>`); everything else unchanged.
- [x] **Step 4: LIVE GATE (opt-in, the standing shape)**: serve `--escalation plan-ask-act` against Ollama; client: the plan turn → the ask renders → `answer ... 1` → an act turn runs a real cell; the bypass variant: plan → act, no ask record in the stream. Record in the plan checkbox (opt-in — run if the Ollama endpoint is up; the guards slice's live-gate discipline). NOTE (the close-out): LANDED for the serve + client (wire) legs; the DIRECT console leg stays un-re-proven (the endpoint was down for the slice task — recorded in spec §6's known-pendings; the client leg's live pass covers the ask render + answer round trip).
- [x] **Step 5:** build + serve/client smoke (unix), commit
  `feat: the demo's ask surface — serve --escalation, the client's answer command, the dialog render`.

---

### Task 9: matrix + sweep + memory (the controller's close-out)

**Files:** Modify `docs/parity-feature-matrix.md`; the memory file (the controller writes it — NOT a repo file).

- [x] **Step 1: the matrix edits** (row 55's escalation half + Section 5 + the owners' list):
  - Row 37 → **L**: the ladder (`frame_escalation_mode_e`; plan-ask-act + bypass; the ask machinery) with the commit refs; the DEV note STANDS (execute free by default) + "the per-call gate is the policy-record slice".
  - Row 2 → gain the note: the parked ASK's answer is durable input, but the PARK itself is in-memory — the events replay re-presents (this slice's shape), the full pending-input reconcile stays with row 15.
  - Row 7 → **L (half)**: the park's wake latching (steer supersede + interrupt refuse + interrupt-wins); max-tokens stickiness recorded (no writer exists).
  - Row 12 → **L (half)**: the parked-window's durable reconciliation (ask + reply records + delivered-false); the in-flight FRM_STOP fence stays P.
  - Row 15 → the trigger note: the ask reply IS the second input class (the machine is NOW justifiable — the next slice).
  - Q7 → RESOLVED (this slice's rows 7/12 halves).
- [x] **Step 2: the final sweep** (the standing bar): both build dirs + ASan + off-verify + the valgrind stripped-copy run over the NEW suites (`TestLoop.TestSteerDuringPark*`, `TestLoop.TestInterruptDuring*`, `TestFrame.TestAsk*`, `TestFrame.TestEscalation*`, `TestLifecycle.TestRepair*`, `TestClientApiWire.TestAskReply*`, `TestClientApiHandlers.TestAskReply*`, `TestSaClient.TestAskReply*` — FULL Suite.Test names) + `grep -rn "TODO\|FIXME\|HACK\|XXX" src/ test/ tools/` clean + the TODO-on-touched-files rule.
- [x] **Step 3:** the memory controller block (the slice's shape, commit range, the known-pending ledger: the Pondr ask UX slice, the policy-record/per-call gates, the parent-mediated chain, the in-flight fence, the long-log re-gate note); the final report: counts + divergence notes (the async ack contract; the BYPASS ask refusal; the window re-gate's safe default) + the NEXT seam (the Pondr ask UX — the FFI ask op + the dialog, then the admission machine row 15).
- [x] **Step 4:** commit `docs: the escalation lands — row 37 L, the park's latch + the fence's half, Q7 resolved`.