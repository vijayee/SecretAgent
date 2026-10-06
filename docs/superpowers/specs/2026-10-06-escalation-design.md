# The Escalation Slice — Blocked-Ask Protocol + the Plan→Ask→Act Ladder + Wake Latching — Design

**Date:** 2026-10-06
**Status:** owner-agreed shape settled in session (approach A — publish-then-PARK, the frame parks
the turn, never a thread; three owner forks tabled below; the bypass amendment agreed). No open
questions — every detail is pinned.
**Atlas slice:** the L5 escalation node (L3/L5) — matrix §5's remainder + row 7's wake latching +
Q7's resolution. Evidence: `test/test_loop.cpp`, `test/test_frame.cpp`, `test/test_lifecycle.cpp`,
`test/test_client_api_*.cpp`, `test/test_sa_client.cpp` extensions.
**Parity sources:** the PA blocked-promise shape (`prime-agent .../examples/extensions/question.ts`,
`permission-gate.ts`; the daemon's `extensionUiRequests: Map<id, {resolve}>` = publish-then-block
keyed by request id — transposed to C below), CC's plan→ask→act pattern
(`docs/architecture-research.html` CC section; patterns only — matrix §5), the chat's L5 doctrine
(docs/design-answers.md: the ladder and escalation rules are L5, the L3 loop stays amoral).
Matrix rows: §5/row 37 (the plan→ask→act remainder — THIS slice), row 7 (wake latching + sticky
turn outcomes — the park's latch lands; the max-tokens stickiness has no writer here, recorded),
row 12 (durable cancel fence — the ask's durable records + events replay reconcile the
park window; the in-flight FRM_STOP fence stays P), Q7 (RESOLVED — see §4), row 15 (admission —
STAYS a later slice; the ask reply is its second real input class, noted).
**The Pondr Flutter ask UX** is NOT in this slice (owner): the C surface + demo CLI close here;
the dialog is the next Pondr slice — the FFI seam is live and the ask rides the events stream
Pondr already renders.

## Why this slice exists

The persona slice landed L5's persona half (matrix row 55). What remains of L5 is the
ESCALATION half: the runtime cannot ask its owner anything. The model that needs a decision
("approve this plan?", "which of these two?") today has no surface — it can only guess or fail.
Claude Code's deepest pattern (plan → ask → act) and PA's blocked-promise shape both transpose
onto machinery SecretAgent already owns: the phase machine's park (FRAME_PHASE_MODEL, the
persona trip's parked `derive_events`), the store's atomic batches, the client-api's events
stream, the msg.append durable-input path. The slice adds the ask channel through those seams
and NOTHING new under the engine: the ladder is config, the ask is a verb + one wire pair, the
resolution is a message. Execute stays FREE by default — the owner override stands
(docs/design-answers.md Q, msgs 4173/4202); this slice adds the CHOICE, never a default gate.

## Decisions carried in (settled with the owner, 2026-10-06)

| Decision | Value | Authority |
|---|---|---|
| The park shape | **Approach A — publish-then-park**: the ENGINE parks (new `FRAME_PHASE_ASK`), the turn closes `turn.end{blocked}`; the cell never blocks. Approach B (the cell thread parks on a corr-keyed wait; the verb returns the answer in-cell) REJECTED: a parked-thread class in an engine that never blocks, pool starvation by held interpreters, a watchdog exemption, and an abandoned-cell repair protocol — all new machinery where A proceeds through the proven park shape. The model sees the answer in the next turn's derive (the derive is stateless and re-rendered every turn — no seam). | owner |
| The ladder's home | **A config enum on `frame_config_t`** (`frame_escalation_mode_e`) — the ladder is ENGINE-mechanical policy, matching how turn-caps/model-timeout configure the loop; NOT a store-loaded policy record (the persona pattern does not repeat here; the record shape may come later if per-call gates demand it — recorded). | owner |
| Ask depth | **Straight to the owner surface** — any frame's ask, top or child, publishes one owner-surface ask for the session. A parked child composes with the parent's existing CHILDREN yield (zero new blocking). The parent-mediated chain (child→parent→user) is a recorded later extension, not built. | owner |
| Pondr UX | **C surface now, the Flutter dialog after** (its own Pondr slice on the live FFI seam). | owner |
| The bypass mode | **BYPASS added as the third enum member**: plan turns still run and log, the approval gate AUTO-APPROVES (durable control record marks it), straight to act — dangerous, documented as such; distinct from `free` (the plan-then-act discipline minus the human checkpoint, with the mode's own log trail). | owner |

## 1. The blocked-ask protocol

The ask is a bridge verb the model calls, like every other `agent.*` verb. No waiting anywhere:
publish → the turn closes `blocked` → the engine parks → the reply is a message → the next turn's
derive carries the answer.

### 1.1 The verb surface

`src/Python/py_agent.c`: `agent.ask(question, options)` —
- Validates at the boundary: `question` non-empty (after strip), `options` a list of ≤8
  non-empty bounded strings (the budget table's caps; the shared truncate-marker helper does NOT
  apply here — oversized INPUT is refused, never silently truncated), `options` optional (an open
  question may carry none).
- Refusals are the verb's return value (data, never exceptions — row 13's law): empty question,
  oversized/empty option, a non-list options argument → a refusal string the model reads.
- Publishes `PYRT_ASK` (new pyrt→frame bridge message, payload `{corr, question, options[]}`) and
  returns a marker string immediately — the cell keeps running its remaining steps.
- **One ask per frame at a time**: if a parked ask already exists when `agent.ask` is called, the
  verb returns the refusal text ("ask already parked — reply pending") and NO second publish
  happens. A second ask inside the SAME turn (pre-park, the cell still running) → same refusal;
  the first ask wins, deferring to the shared turn close.

### 1.2 Frame-side messages (`src/Frame/frame_messages.h`)

- `FRM_ASK` (cell/engine direction, payload `{corr, question, options[]}`) — the pyrt bridge's
  forward of `PYRT_ASK`.
- `FRM_ASK_REPLY` (engine input, payload `{ask_id, decision, value}`) — the reply verb's landing.
- Event vocabulary grows two members, the closed list extended: `EV_ASK` (the published ask) and
  `EV_ASK_REPLY` (its resolution). Both are ORDINARY frame events — seq-ordered, in the frame's
  batch discipline, folded by the derive.

### 1.3 The park (the engine, `frame.c`)

- On `FRM_ASK`: the engine mints `ask_id` (root rng + counter — the sid's mechanism, one
  allocator, no collisions), boxes the ask, and sets engine state: `pending_ask {ask_id, corr}`
  plus `FRAME_PHASE_ASK` in the phase machine (`frame_internal.h`). The ask DEFERS to the turn
  close — no mid-cell parking anywhere.
- When the currently-running cell completes, ONE atomic batch: the `cell.result` + the
  `EV_ASK` record `{kind:"ask", askId, question, options:[…]}` + `turn.end{reason blocked}` —
  **`LIFE_REASON_BLOCKED`'s first writer** (`lifecycle.c:34` reserve honored; already in the
  fold's KNOWN list at `lifecycle.c:105`; the composer is already exercised in
  `test/test_lifecycle.cpp`). The engine then parks in `FRAME_PHASE_ASK` — the same shape as
  `FRAME_PHASE_MODEL` awaiting `FRM_MODEL_RESULT` and the persona trip's parked `derive_events`:
  nothing blocks, nothing spins; liveness stays counters-not-claims.
- If the cell completes WITHOUT an ask having arrived, everything is byte-identical to today —
  the standing pins carry (`free`-mode frames never touch any of this).

### 1.4 The reply (the resolution)

- `FRM_ASK_REPLY` arrives (the wire's reply verb → the handlers → `_frame_ask_reply_post` — the
  `_frame_interrupt_apply` posting shape). Validation inside the engine, fail-loud at the edge:
  the `ask_id` must match the parked `pending_ask` EXACTLY. No match (stale, duplicate, a reply
  for an already-answered ask, a reply when no park exists) → **dropped loud**: a log-side
  record, and the CLIENT learns through the events stream (the refusal record) — the wire ack
  CANNOT report the engine's stale outcome because the post is async (§3.1's ack contract
  says exactly this). The pending ask is consumed EXACTLY ONCE.
- A match lands ONE atomic batch: the `EV_ASK_REPLY` record
  `{kind:"ask-reply", askId, decision, value}` (decision ∈ `answer` | `reject`; value = the
  answer text or the refusal text) **plus the answer as a user-side `msg.append`** — durable
  input before model work (`frame.c:755-769`'s standing law). The `msg.append`'s record carries
  a shape the derive renders as a user message: the answer text for `answer`, the refusal
  wording for `reject` — the plan gate's rejects render "Plan rejected: revise and re-propose"
  (or the owner's revision text verbatim, §2.2); a generic `agent.ask`'s reject renders
  "Owner declined: <value>" or, when the value is empty, "Owner declined.".
- The park clears; the engine re-enters via the ordinary `FRM_TURN` continuation
  (`_frame_engine_turn`) — a fresh turn over the derive, which now shows the answer. The
  protocol's rejection ISN'T a separate path: the reject flows through the identical resume and
  surfaces as data. Model self-correction is DSH's doctrine: failure is data (row 13).
- The engine state's `pending_ask` is IN-MEMORY (like the parked model submit). Durability comes
  from the RECORDS (§1.3's ask event + §1.4's reply record): the restart story needs no
  machinery beyond what the log already holds (§4).

### 1.5 Reject cascades — none, and none needed

Asks go straight to the owner surface (owner decision — the parent-mediated chain is a recorded
later extension), so there is no chain to cascade DOWN. A reject is one record + one refusal as
data. The parked child composes with the parent's standing FRAME_PHASE_CHILDREN yield: the
parent parks already; the child's ask parks the child; the owner surface sees one dialog for
one session.

## 2. The escalation ladder — config

`frame.h`: `frame_escalation_mode_e { FRAME_ESCALATION_FREE = 0, FRAME_ESCALATION_PLAN_ASK_ACT,
FRAME_ESCALATION_BYPASS }`; the field on `frame_config_t` (`escalation_mode`). Zero = free = the
default. Immutable after create; **children inherit via the existing parent dup**
(`frame.c:4480-4487`'s persona_name/caps inheritance shape); resume carries it (the resume
site's existing config handling). The wire creates frames WITHOUT a new config field this slice
(client-created frames run `free` — the default) — §6 notes the later knob.

### 2.1 FREE (the default; the owner override stands)

No gate, no plan phase. Execute free. The frame asks ONLY if the model explicitly calls
`agent.ask` — the ask verb is available in every mode. Byte-identical engine behavior when no
ask is ever published (the persona slice's standing-pin pattern reused verbatim as the proof).

### 2.2 PLAN_ASK_ACT

The ladder is a durable PHASE of the frame, recovered from the log at every derive (the
transition records ARE the phase's state machine):

- **PLAN phase**: every turn until approved runs plan-shaped — the derive injects a
  plan-instructions block (a bounded fixed text: propose the plan, no execution), and the model
  request is built **tools-null** — `_model_request_body`'s no-tools shape (the refine slice's
  machinery, reused verbatim: tools = JSON null ⇒ the request omits tools+tool_choice). The
  model cannot execute a cell in plan; its turn result text IS the plan. At turn close the
  engine runs the SAME ask machinery as §1 but OWNER-INVOKED (runtime-authored, not verb-called):
  ask record `{question:"Approve this plan?", options:["Approve","Reject"], plan:<the turn's
  plan text, budget-capped>}` + `turn.end{blocked}` + park.
  - The model can still call `agent.ask` DURING plan (a clarifying question): parks the same
    turn identically; after the reply the frame is STILL in plan (the gate hasn't fired).
- **ACT phase (on approval)**: the `approve` answer lands as §1.4's reply, and the resume batch
  ALSO writes the phase transition as a durable control record
  `{kind:"plan-approved", auto:false}`. Act turns run the ordinary free loop — execute free.
  The approval's lifetime **is the frame's session**: the control record is the consulted state
  (§2.4). Wall-clock TTLs and per-call approvals are recorded-pending for the policy-record
  slice, not built.
- **Rejection**: `reject` with no text → the refusal record's default wording ("Plan rejected:
  revise and re-propose") → the next turn stays PLAN. `reject` WITH text → the text is the
  record: the model replans with the objection in view. Either way the ladder revisits plan —
  there is no "abort the frame" semantics on a reject (the answer record carries that data; the
  model decides its next step).

### 2.3 BYPASS (dangerous, the owner's amendment)

Plan turns still run and are still logged (the plan text stays a durable audit artifact), but
the approval gate **auto-approves**: at the plan turn's close the engine writes
`{kind:"plan-approved", auto:true}` + proceeds to act — NO ask record, NO park, NO owner
surface involvement at any point. The log trail records which mode governed every transition.
`agent.ask` under bypass **refuses as data** ("escalation bypassed: asked questions have no
answerer") — an open question with no answerer would park the frame forever; refusing loudly is
the honest behavior. The same refusal applies wherever no client owns the session (this is the
no-UI default INVERTED from PA's block-by-default, only here).

### 2.4 The approval consult

Before the engine auto-gates a plan turn it consults the recovered phase: a frame whose log
carries a `plan-approved` control record NEVER re-gates (act persists, including across
restart — the record is durable). This is the learned-approval's mechanism for this slice: the
decision is durably remembered with an explicit lifetime (the session). Consumers beyond the
one gate are the recorded extension.

## 3. The wire + the client

### 3.1 The wire (`src/ClientApi/client_api_wire.{h,c}`)

ONE new pair — the reply verb:

```c
#define CA_ASK_REPLY_REQUEST    16  /* {req_id, sid, ask_id, decision, value} —
                                     decision: 0 = answer, 1 = reject; value bounded  */
#define CA_ASK_REPLY_RESPONSE   17  /* {req_id, delivered: bool} */
```

- The response=request+1 pairing asserts extend (the compile-time chain at `client_api_wire.h:85-100`).
- Encode/decode: the CBOR switch extended both directions (bytes + loaded-item both decode
  forms); bounded strings per the standing bounds table; `cbor_decref`'s missing-NULL-guard
  rule honored on every load-failure path.
- **The ask's outbound delivery rides the EVENTS CHANNEL** — `EV_ASK` is an ordinary
  delivered record (live via FRM_STORE_WATCH/NOTIFY, replayed on reconnect). No second push
  path; no server-initiated pair. `EV_ASK_REPLY` arrives to subscribers the same way.
- A reply the server cannot bind (unknown sid, no such frame, the post refused) → ack
  `{delivered:false}`; the server logs loud.

**The ack contract (pinned):** the ack reflects ONLY the bind/post — `delivered:true` = the
reply was accepted into the frame's mailbox. The ack CANNOT report the engine's stale-ask
outcome (the post is async) — that truth lives in the events stream: a stale reply produces the
frame's own refusal/drop record there, and a client whose dialog never receives an
`EV_ASK_REPLY` re-presents or drops per its own state. This is the events-channel-is-truth
discipline the client-api slice already runs (the cursor/hold machinery).

### 3.2 The handlers (`src/ClientApi/handlers.c`)

- The dispatch switch extended: `CA_ASK_REPLY_REQUEST → _ca_on_ask_reply` — bind sid → frame,
  validate the reply shape, post `FRM_ASK_REPLY` into the frame's mailbox (the
  `_frame_steer_post` posting shape), ack per §3.1's pinned ack contract — `delivered` reflects
  ONLY the bind/post; stale-ask truth lives in the events stream.
- The pending-kind table + every new-allocation shape extended mechanically.

### 3.3 sa_client (`src/ClientLibs/c/sa_client`)

- ONE new blocking op: `sa_client_ask_reply(client, sid, ask_id, decision, value)` — the
  `sa_client_prompt` shape verbatim (one in-flight request per connection, condvar wait, the
  held-payload table's lifetime rules).
- Asks surface through the EXISTING events callback as records — no new callback shape; the
  record's `kind:"ask"` is parseable by any client. The re-entry rule (NO blocking ops from
  events callbacks — `sa_client.h:53-59`) explicitly covers ask-reply: callers reply from
  their own thread/isolate, never from inside the callback.
- Destroy-while-a-reply-is-in-flight = the documented UB class (the offs_client contract).

### 3.4 The demo CLI (`tools/frame-demo/main.c`)

- Serve mode: `--escalation plan-ask-act` (and `--escalation bypass`) — frames created with the
  mode; default `free`.
- Client mode: ask records render as a dialog-style prompt in the event stream (question +
  numbered options); a new `answer <sid> <ask-id> <pick|reject> [text]` command →
  `sa_client_ask_reply`: `pick` = an option LABEL or free text (decision `answer`, value = the
  label/text; the bare token `reject` sets decision `reject`, the optional text becomes the
  value). The serve/client loop exercises the full ladder live: plan turn → parked
  ask → approve → act turn with a real cell.

## 4. Cancel fence + wake latching (Q7 RESOLVED)

The park is the first DURABLE quiescence point, and the wakeup classes latch there:

- **Interrupt (`frame_interrupt`/FRM_INT) while parked**: the pending ask is auto-answered
  (`decision:"reject"`, value "interrupted at the frame's request"), an `EV_ASK_REPLY` record
  lands (ONE batch with the control record), the engine clears the park, the frame returns
  idle-resumable. The turn already ended durably (`blocked`) — nothing unbalanced is left; the
  poison rules stand (a post-turn interrupt is no mid-cell cut — no poison trip).
- **Steer (FRM_STEER) while parked**: the pending ask auto-answers ("superseded by new user
  input"), same refusal record, and the steer's `msg.append` serves as the next turn's input —
  the wake is honored at the park's wake (row 7's latch shape, its first writer).
- **Both arrive**: the interrupt wins (cancel first); the steer is still durably logged as
  msg.append, so the next derive carries it.
- **Interrupt DURING a plan/act turn** — today's behavior unchanged (`_frame_interrupt_apply`
  mid-cell cut, turn ends `aborted`, poison rules per the surface-completion slice).
- **The durable cancel fence** (row 12) — this slice's HALF is landed by construction: parked
  asks and their resolutions are log records, and a crash between "client sent reply" and
  "frame consumed it" reconciles through the events replay (the requestor sees no reply record;
  on reconnect it re-presents; a late reply gets `delivered:false`). The IN-FLIGHT
  mid-turn fence (FRM_STOP durable across a crash mid-cell) stays P — recorded (matrix row 12).
- **Row 7's other half** (max-tokens never downgraded): our reasons have NO max-tokens writer
  today (turn-limit has no writer either) — nothing to be sticky about; recorded, not built.

### 4.1 The restart story (free from the log)

- A crash while parked: the tail reads ask + `turn.end{blocked}` — a BALANCED turn (blocked is
  fold-known at `lifecycle.c:105`; the composer is exercised in `test/test_lifecycle.cpp:955-961`).
  `frame_resume`'s repair: no spurious closers; the pending ask is visible in the log; the
  reconnecting client replays events and re-presents. The phase recovery (§2.2/§2.4) rides the
  same records.
- A crash BETWEEN plan turn close and ask publish CANNOT EXIST (one atomic batch — §1.3).
- A client that answered a never-consumed ask (the reply raced the crash): its late
  `CA_ASK_REPLY` gets the stale drop (`delivered:false`, §3.1) — the events replay shows it
  exactly-once truth.

## 5. Error handling (every path lands data or fails loud)

- Stale/unknown `ask_id` → dropped loud, ack `delivered:false` (§3.1).
- A second `agent.ask` while parked or pre-park-in-turn → the verb's refusal return (§1.1).
- Malformed verb inputs (empty/oversized question, bad options list) → the verb's refusal
  return, data (§1.1). Boundary caps from the budget table; no silent truncation of ask fields.
- The ask's close-batch store refusal → the batch is atomic, so the turn remains OPEN and the
  engine's standing store-refusal handling applies (fail the turn loudly — the frame-tree
  slice's reply path); NO half-parked state by construction (the park is set by the same
  batch's completion).
- `agent.ask` under BYPASS → the refusal return (§2.3).
- Unknown/malformed `CA_ASK_REPLY` wire bodies → `CA_ERROR` (the standing failure pair) —
  bounded decode, fail loud.
- A reply posted into a frame mid-move (async model submit in flight, the deferred-destroy
  window) → the posting shape is the steer's (FRM_STEER rides the same mailbox rules — the
  engine consumes it at its next dispatch; no new race class).

## 6. Non-goals (recorded, not built)

- The Pondr Flutter dialog UX — the NEXT Pondr slice on the live FFI seam (owner).
- Per-call permission gates (allow/deny/ask per tool invocation) + wall-clock approval TTLs +
  the store-loaded escalation policy record — the policy-record slice, only when per-call gates
  demand it (the owner's enum choice recorded above).
- The parent-mediated ask chain (child → parent → user) — recorded extension of the depth
  decision (asks land at the owner surface this slice).
- The admission state machine (row 15) — the ask reply is genuinely its second input class; the
  slice LANDS after this one (the standing FIFO + the corr-matched reply consume suffice here).
- The in-flight durable FRM_STOP fence (row 12) — stays P; this slice lands the parked-window's
  durable reconciliation (§4).
- Row 7's max-tokens stickiness — no writer exists; recorded.
- The wire's frame-creation escalation knob (client-chosen `escalation_mode` at PROMPT-create
  time) — client frames run `free` this slice (§2); a CONFIG-pair extension if the Pondr slice
  asks for it.

## 7. Tests (the falsifiable set)

- **`test/test_loop.cpp`**: ask→park→reply resume (answer + reject paths, the one-batch
  ask/turn.end shape, the msg.append answer visible in the next derive); steer-during-park
  latch (auto-answer "superseded", the steer served); interrupt-during-park (auto-answer
  "interrupted", idle-resumable frame, no poison); both-arrive (interrupt wins, steer carried);
  second-ask refusal; stale FRM_ASK_REPLY dropped loud; the free-mode standing pins (no ask
  machinery touched); child-frame ask (parent stays parked in CHILDREN; one owner-surface ask).
- **`test/test_frame.cpp`**: the ladder — plan turn tools-null (`_model_request_body`'s
  no-tools shape asserted), the plan-instructions block in the derive, approve → act turns free
  (a real cell runs), reject → replan, reject-with-text → the text visible, the
  `plan-approved` control record, phase recovery across restart (act persists), BYPASS
  (plan → auto-approve control record `{auto:true}` → act, NO ask record ever, the ask verb's
  refusal) — plus BYPASS inheritance.
- **`test/test_lifecycle.cpp`**: `blocked`'s first-writer fold + the balanced-tail repair rule
  (no spurious closers over ask+blocked) + the repair-composer's blocked row.
- **`test/test_client_api_wire.cpp`**: CA_ASK_REPLY 16/17 encode/decode round-trips (bytes +
  loaded-item), the bounded-string refusals, the response=request+1 assert extension.
- **`test/test_client_api_handlers.cpp`**: reply → matching parked ask → the answer lands as
  msg.append + EV_ASK_REPLY; unknown sid/ask_id → delivered:false; the wire-borne steer-during-
  park; the deliver-only-ack contract (async stale detection NOT in the ack).
- **`test/test_client_api_transports.cpp`** + **`test/test_sa_client.cpp`**: the full
  ask/reply over unix + tcp-auth; the blocking `sa_client_ask_reply`; re-entry refusal from an
  events callback; the events-replay re-present path (reconnect mid-park, the ask visible).
- **Live gate (opt-in)**: serve `--escalation plan-ask-act` + client against Ollama — plan turn
  → the ask renders → `answer … approve` → an act turn runs a real cell. The bypass variant
  runs plan→act with no ask.
- **Verification bar** (standing): `setarch -R ctest --test-dir cmake-build-debug
  --output-on-failure` ON + ASan (`cmake-build-asan`), OFF (`cmake-build-off`), off-verify;
  valgrind on `strip --strip-debug` copies (full-suite names in the filters).

## 8. Docs/close-out

The parity matrix updates in the slice's final docs commit: §5/row 37 → L (the ladder), row 7 →
L-half (the park latch; max-tokens recorded), row 12 → P (recorded this slice's half), Q7 →
RESOLVED (this slice), row 15 → its trigger note (the ask channel exists; the machine next).
The known-pending ledger (memory + the plan doc) carries: the Pondr ask UX slice, the
policy-record/per-call gates extension, the parent-mediated chain, the in-flight fence.