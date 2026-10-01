# Turn/Step Lifecycle + Crash Repair — Design

**Date:** 2026-10-01
**Status:** owner-agreed shape settled in session (the core trio only; lifecycle boundaries are log machinery the derive folds away, EXCEPT the crash-repair `repair` brief which is model-visible with the details upfront — never a multi-step chase for what went wrong; approach A: a new pure module + two thin integration points). Every open question this design raises is pinned below — no TBDs. The two adjacent matrix open questions (the cancel fence / wake latching; the pooled-engine cell watchdog) are deliberately OUT (§8).
**Atlas slice:** a new node (delivery; parity feature matrix §6 slice rank #2) — evidence: `test/test_lifecycle.cpp` (new) + extended restart tests in `test/test_loop.cpp` / `test/test_frame.cpp`.
**Parity sources:** `docs/parity-feature-matrix.md` §3 rows 4 (turn/step lifecycle envelope) and 5 (crash-repair closers), and §6 slice 2's sketch. DSH ported intents: `deepseek-harness/packages/core/session/src/repair.ts` + `tests/repair.spec.ts` (the closer synthesis and its pinned model-visible wording), `packages/core/agent-loop/src/agent.ts:354-385` (the `finally`-discipline: step/end and turn/end on every exit), `packages/core/core/session/src/types.ts` (the reason union), `packages/core/agent/src/consumed-work.ts` (a turn that ends before its first step still closes balanced).

## Why this slice exists

The engine's failure and crash paths are honest but invisible: a turn that died mid-flight
leaves an open tail with no synthesizer — restart replay restores seq + context
(`test_loop.cpp` TestRestartReplayRestoresSeqAndContext) but nothing closes what was in
flight, and the model never learns what was cut. Today's control events
(`model-error-final`, `turn-limit`, `commit-error`, …) ride the log but the derive walk
skips them, so the model never reads them either. The matrix names both gaps as the
strongest un-landed DSH essence: step boundaries + a full `turn.end` reason union
(row 4) and pure deterministic crash-repair closers over a truncated log (row 5).

One fact reshapes the port: our turn engine sends a FRESH plain-roles array every turn
("NO tool_call/tool-role history is reconstructed", loop.c:81-86) — so DSH's strongest
closer motivation (providers reject dangling assistant tool-calls in replayed history)
DOES NOT APPLY here. The closers port as model-facing INFORMATION (owner decision:
details upfront, not a fetch), and DSH's live `ToolCallRecovery` class collapses: an
unanswered RECORD cell is only ever a crash, which only ever repairs through resume
(§5's non-port list).

## Decisions carried in (already settled with the owner)

| Decision | Value | Authority |
|---|---|---|
| Slice scope | the core trio ONLY: step/turn boundaries, the reason union, the resume repair closers | owner, 2026-10-01 |
| Cancel fence / wake latching | OUT (steering/interrupt slice; matrix §3 open question 7) | owner, 2026-10-01 |
| Pooled-engine cell watchdog | OUT (slice 3's interrupt decision; matrix §3 open question 4) | owner, 2026-10-01 |
| What the boundaries FEED | log machinery — the derive folds `turn.*`/`step.*` away exactly like `control` today | owner, 2026-10-01 |
| What the model SEES | crash-repair closers ONLY, model-visible, details upfront (the interrupted cell's code + seq, the cut's recorded facts) — never a one-line pointer that forces a chase | owner, 2026-10-01 |
| Non-crash failures | UNCHANGED: failure turns still exit via control events the derive skips; `cell.result` failure text still projects as today | owner, 2026-10-01 |
| Where the work lives | approach A: NEW module `src/Frame/lifecycle.{h,c}` (pure core) + thin integration in loop.c/frame.c | owner, 2026-10-01 |
| Tests mirror | DSH `repair.spec.ts` intents (the branches + verbatim-wording table), adapted to our vocabulary | owner, 2026-10-01 |
| Out of scope | derive compaction/aging (the `repair` brief persists in the log like every history record), Windows, per-step attribution of every event (DSH's per-event `turn`/`step` fields — the envelope keeps the two levels but records stay flat payloads) | owner, 2026-10-01 |

## 1. The event vocabulary — five new types on the existing write path

Records ride the EXISTING event path (`_frame_event_write` / `_frame_event_post_fire` —
the same `type` + JSON `payload` shape every event has). Zero new actor, thread, lock,
or store round trip in a healthy turn beyond §3's stated turn-entry batch.

| type | payload | who writes it | derive |
|---|---|---|---|
| `turn.start` | `{turn}` | engine turn entry (fire-and-post, the control events' discipline) | skipped |
| `turn.end` | `{turn, reason:{kind,text?}}` | the turn's end rule (§3) | skipped |
| `step.start` | `{turn, step}` | rides the turn's first durable record batch (the reply batch) | skipped |
| `step.end` | `{turn, step}` | rides the cell-result / finish batch | skipped |
| `repair` | `{turn, text}` | the resume closer batch ONLY (never the live engine) | **RENDERED**: one user-role message, text verbatim (the derive's existing message cap truncates at source) |

- **The reason union (open, DSH `TurnEndReasonMap`'s port, ours):**
  - `completed` — the turn completed its cycle (a cell ran and answered; the engine continues;
    the FINAL turn of an ok run also ends `completed` when its content ended the engine).
  - `error` — the engine's failure surfaces: model error after its retry, cell loss, commit
    refusal, the OOM/lost-reply shapes. The record's `text` carries the existing control
    kind's wording (`model-error-final`, `commit-error`, …) verbatim — the union member's
    detail is the control event's own text, never invented twice.
  - `turn-limit` — the budget cap refused a turn entry; the engine ends failed. The
    refused turn NEVER opened (the cap check precedes turn entry), so no `turn.end`
    carries this reason in today's engine: the cap failure's control event + the previous
    turn's already-closed record are the whole envelope. `turn-limit` exists in the union
    for the vocabulary's completeness and the steering slice's later budget turns; today
    NO writer emits it.
  - `interrupted` — the crash-repair closers' cause (§4). The ONLY reason whose turn.end
    is written on a caller thread rather than the engine.
  - `aborted`, `blocked` — RESERVED names (steering/interrupt slice). No writer this slice.
  - Unknown kinds on replay fold loud (the refine fold's render-not-crash rule: a malformed
    lifecycle record renders a loud skip — never crashes the derive, never silently drops).
- **Numbering:** `turn` = one model-call cycle — the SAME noun the turn cap counts
  (frame.h's `frame_set_loop_turn_cap` contract). `step` = the model call + the cell it
  requested; exactly one step per turn today, but the envelope holds for when
  steering/refinement-in-cycle makes a turn multi-step. The counter is RESTORED, never
  invented: counted once from the events the resume/derive scan already materializes
  (newest recorded `turn` number + 1; the `_frame_restore_seq` discipline — monotonic,
  gaps recorded, never a lock, never a second writer).
- **Derive treatment** (the ONE projection change): `turn.*` and `step.*` fold away
  exactly as `control` does today — log spine only. The `repair` type gets ONE new branch
  in the derive's pass B: rendered as a user-role message with `text` verbatim (the
  existing per-message cap applies). `cell.*`, `msg.append`, state/report handling are
  untouched.
- **Balance rule (the resume check's whole truth):** the log is balanced iff the newest
  lifecycle record is a `turn.end`. No crash flag, no extra state — the log alone says
  whether repair is needed.

## 2. The lifecycle module (`src/Frame/lifecycle.{h,c}`) — the pure half

New module, the refine slice's discipline: pure data work, style-guide module rules, no
store/model/actor code. Caps under the `SA_` ifndef discipline (`-D` overridable).

- **The lifecycle cursor** (internal struct): folds an event-tail into — newest recorded
  turn number, open-turn index (NULL when balanced), open-step index, the in-flight cell's
  facts (the open `cell.run`'s seq + code reference: `cell.run` committed, no matching
  `cell.result` yet), and the last event's seq. `cell.result` pairs a pending `cell.run`
  by corr; boundary records (`step.end`, `turn.end`) close pending. Turn/step numbers come
  from the records' OWN payload fields (never the record type), so a turn whose
  `turn.start` was refused (fire-and-post, control discipline) still folds and still
  balances — the fold never depends on a record that may legitimately be absent. A
  malformed lifecycle record (a second `turn.start` over an open turn, corrupt payloads)
  is logged loud and skipped — the refine fold's render-not-crash rule.
- **`lifecycle_closers(tail, cause)`** — pure, deterministic: returns the synthetic
  closer events to append after the tail, in seq order, empty for a balanced/empty tail:
  1. `step.end` for the open step (if any) — closes BEFORE the turn (DSH's order).
  2. `turn.end {reason: "interrupted"}` — always when the turn is open.
  3. `repair {turn, text}` — the model-visible brief (§4's wording).
  Seqs continue the log contiguously (`last.seq + 1`, each closer + 1); no timestamps are
  invented — our records carry seqs, not times, so "reuse the last real event's time" has
  no analogue (the refine fold's discipline applies instead: the record lines render from
  the seq, the brief's text from the records' own content).
- **The brief's wording — cause `interrupted`, the two pinned shapes (the parity mirror's
  verbatim-wording table, DSH `CLOSER_TEXT` adapted — our model never saw a dangling
  provider call, so the brief quotes the ENGINE'S recorded facts):**
  - **started** (an open `cell.run` exists: the crash cut between the cell's audit commit
    and its result): one text block whose pinned parts are:
    the lead line `The previous turn was interrupted before its result was recorded.`;
    the details, quoted from the log (details UPFRONT — owner decision): the interrupted
    cell's seq and its code verbatim (the `cell.run` payload's text, capped by
    `SA_LIFECYCLE_BRIEF_CODE_CHARS`, truncation-at-source loud); the fact line
    `Its outcome is unknown.`; and the guidance `Decide whether to retry from the cell's
    semantics: retry only if the operation is read-only or idempotent; if it may have
    side effects, first verify external state or ask the user. Do not retry blindly.`
  - **not-started** (the turn opened, the crash cut before any cell audit): the lead line
    `The previous turn was interrupted before the cell started. No cell execution was
    recorded.` + the guidance's retry-if-still-needed clause
    (`Retry it if it is still needed.`), no code quote.
  - The engine's in-run failure paths NEVER need a brief: a failed model call wrote
    nothing to close (§3), and a cell loss carries its own paired `cell.result`
    (loop.c's audit-honesty fix — the refusal's status-1 record). The brief exists at
    resume only.
- **Caps (new, the SA_* ifndef discipline):** `SA_LIFECYCLE_BRIEF_CODE_CHARS` (the
  quoted cell code's cap, default 1200), `SA_LIFECYCLE_TAIL_EVENTS` (the resume tail
  scan's window, default 512 = the shared scan window max), `SA_LIFECYCLE_MAX_CLOSERS`
  (the composed closer batch's record cap, default 4: step.end + turn.end + repair +
  margin — one cell per turn today keeps it at 3).

## 3. The engine integration — the batch riders (loop.c / frame.c, ONLY as listed)

**Boundaries ride existing batches; zero new round trips beyond the turn-entry batch:**

| engine moment | existing batch | riders |
|---|---|---|
| turn entry (after the cap check, before the model dispatch) | NEW small fire-and-post batch (one record) | `turn.start {turn}` |
| model reply commits | `FRAME_STORE_CELL_RUN` (tool path) / the content path's `FRAME_STORE_FINISH` | `step.start {turn, step}` |
| cell result | the `cell.result` batch (`_frame_event_post_fire` cell.result) | `step.end` + `turn.end {reason:"completed"}` |
| engine finishes on the content path | the `FRAME_STORE_FINISH` batch | `step.end` (if open) + `turn.end` with the TERMINAL reason |
| failure exits (`model-error-final`, cell loss, commit refusal, the OOM shapes) | the failure's own control/fail batch | `turn.end {reason:"error", text:<the control kind/detail>}` |

- **The Finally-Discipline (DSH agent.ts:354-385, ported as a rule):** EVERY exit path of
  a turn closes it — the reason never NULL (the `error` member's text = the control kind's
  wording, never invented twice; a path with no control kind = the `error` member with the
  plain detail text). `step.end` rides every step exit's batch.
- **Terminal attribution:** the LAST turn's `turn.end` carries the engine's terminal
  reason; every earlier turn ends `completed` (its cycle answered). A turn that fails
  before entering a step still opens (turn entry) and closes (`error`) — a
  `turn.end` shaped exactly like the engine's other failure closes (DSH consumed-work's
  rule: a turn with no entered step has no step records, but turn records always pair).
- **Turn numbering restore:** once per engine begin / resume — the highest recorded
  `turn` number + 1 from the events the resume/derive already scans (no extra round trip;
  the `_frame_restore_seq` discipline). Gaps recorded loud (like the restore's seq gaps).
- **The derive walk** gains the one `repair` branch (§1). NO other derive change: the
  result-ring flush rule ("only results since the newest msg.append") stays as-is this
  slice — its lifecycle-aware refinement is slice 3's budget-table business.

## 4. The resume repair (`frame_resume`)

On the caller's thread, after the seq restore, before the engine starts:

1. **Tail scan:** ONE `_frame_sync_scan` over the events range, the newest
   `SA_LIFECYCLE_TAIL_EVENTS` window.
2. **Cursor + closers (pure):** fold; balanced (newest lifecycle record is a `turn.end`,
   or no lifecycle records at all — a pre-lifecycle log) → nothing, resume proceeds
   exactly as today. Open turn → compose the closers (§2's order, one cell in flight
   today).
3. **ONE atomic closer batch** through the sync family (`_frame_sync_batch`):
   `step.end` + `turn.end {reason:"interrupted"}` + `repair {turn, text}` (+ the resume's
   own `meta/status` put when the resumed configuration starts the engine — same batch,
   one round trip). Cap-checked before the post (loud).
4. **Then the engine starts.** The next derive projects the `repair` brief as a user-role
   message — the model reads the full crash briefing in its very next context: what was
   cut, the interrupted cell's code and seq, the outcome-unknown fact, and the retry
   guidance. Details upfront, never a chase.
5. **Refusals:** scan refusal, batch refusal, deadline — the sync family's documented
   loud consequences (pooled store refuses loud; the deadline leaves the batch to the
   store's FIFO — the same recorded consequence as every other sync). Resume refuses
   rather than half-repairs.
6. **Idempotency:** the balance check is the whole guarantee — once the closer batch
   commits, the newest lifecycle record is a `turn.end`, so a second resume composes
   nothing. An ENGINE-PAUSED frame (child-pending, not crashed) re-resumes into a
   balanced tail too: the children-yield turn ends `completed` (the turn's model call
   answered; the CHILDREN phase is engine scheduling, not turn lifecycle — `blocked`
   stays reserved for the steering slice). Resume folds nothing and continues.

## 5. The deliberate divergences and non-ports

- **No dangling-provider-call problem:** our derive reconstructs plain roles only
  (loop.c:81-86) — DSH's per-pending-call synthetic `tool/result` closers (repairs the
  provider transcript) do NOT port; the brief carries the engine facts instead.
- **No live `ToolCallRecovery`:** DSH's in-step recovery class collapses to the resume
  closer: every in-run failure path records (paired `cell.result`) or writes nothing; an
  unanswered RECORDED cell is only ever a crash → resume.
- **Flat records:** DSH tags every event with `turn`/`step` fields; our existing events
  (msg.append, cell.*, ctx, control) keep their payloads UNTOUCHED — only the five new
  types carry turn/step numbers. Backfilling the existing vocabulary is a later slice's
  need (per-step attribution), never this one.
- **No sticky reason rule:** DSH's "max-tokens once set must not be downgraded" is
  trivially ours (one model call per turn — the terminal turn IS the last).
- **`turn-limit` emits no records today** (the cap check precedes turn entry) — the union
  member exists for the vocabulary's completeness and the steering slice's budget turns.

## 6. What does NOT change (the freeze list)

`src/Actor`, `src/Scheduler`, `src/RefCounter`, `src/Util`, `src/Streams`, `src/Buffer`,
`src/Python`, `src/Frame/loop.c`'s derive caps and ring rules, `src/Frame/model.{h,c}`,
`src/Frame/refine.{h,c}`, `src/Frame/frame_messages.h` — all untouched. The frame layer's
changes are EXACTLY: `src/Frame/lifecycle.{h,c}` (new), `loop.c` (the turn-entry batch, the
boundary riders in the batches named §3, the derive's one `repair` branch, the turn-number
restore), `frame.c` (frame_resume's closer scan/batch), `frame_internal.h` (the
declarations), and the tests. No locks anywhere (`src/Frame/`'s standing grep: model.c's
recorded exceptions unchanged). NO TODO/FIXME/XXX/HACK anywhere. Reference dirs read-only.
Conventional commits, no Co-Authored-By.

## 7. Test parity plan (`test/test_lifecycle.cpp` new; extends test_loop.cpp / test_frame.cpp)

Mirroring DSH `repair.spec.ts`'s intents, adapted to our vocabulary (citations inline):

- [1] Balanced log → `[]`; empty log → `[]` (repair.spec:104-114).
- [2] Open turn, no step → `[turn.end]`, reason interrupted; open step → `[step.end,
  turn.end]` in that order, contiguous seqs continuing the tail (:116-133).
- [3] The started/not-started distinction maps to `cell.run`-recorded vs not: the brief
  quotes the cell's code + seq ONLY on started; the not-started shape says no cell
  execution was recorded (:135-176, the wording table).
- [4] An ANSWERED cell (`cell.run` + matching `cell.result`) synthesizes nothing (:178-207).
- [5] Only the still-open turn: earlier completed turns with their own answered cells are
  never touched (:235-284).
- [6] A `cell.result` NOT matching the pending `cell.run` (corr mismatch) does not
  acknowledge it (:79-97's discipline, corr-mapped).
- [7] Orphan `cell.run` without a pairing / a `repair`-less balanced tail — graceful:
  step+turn close, no synthetic quote (:354-365).
- [8] The brief's wording pinned VERBATIM per (started) case (the exact strings above,
  §2) (:368-426's wording table port).
- [9] Determinism: the same tail composes byte-identical closers; re-invocation stable
  (:64-77).
- [10] Malformed lifecycle records fold loud (the refine skip-line rule).
- [11] The engine envelope (row 4's port, test_frame/test_loop): a scripted frame run's
  log shows the turn/step envelope in order — turn.start/step.start/open-records/step.end/
  turn.end per cycle, ONE atomic batch per record group, every failure exit closes (the
  model-error-final turn's turn.end carries `error` + the control text; no turn ever left
  open by an alive run).
- [12] Turn numbering across a resume continues past the pre-restart turns (no renumber).
- [13] The RESTART integration (extends test_loop.cpp's restart test): seed a live frame,
  cut the store mid-cell (a committed `cell.run`, no `cell.result`), close/reopen,
  `frame_resume` asserts: the closer batch committed BEFORE the engine starts (balance
  restored, reason interrupted), and the next derive's captured request carries the
  `repair` brief verbatim as a user message quoting the cell's code + seq.
- [14] Idempotency: a second resume over a repaired tail composes nothing.
- [15] Standing verification (every task): three configs green via `setarch -R ctest`
  (debug / ASan; OFF excludes new suites with the lifecycle tests' gate — follow
  refine/test registration discipline), valgrind on the new filters (stripped-copy,
  verified-run OK-count), the no-locks grep.

## 8. Acceptance criteria (whole slice)

1. Three configs green; new suites registered under the correct gates.
2. Valgrind clean on the new filters; no-locks grep clean (model.c exceptions unchanged).
3. Boundaries: every engine exit closes its turn — a full scripted/engine-driven run's log
   shows the paired envelope; turn numbering restores across restarts.
4. Reason union: pinned members with the `error` member carrying the control kinds' text.
5. Repair: a truncated tail (both cut windows) composes the deterministic closers; the
   resume commits ONE atomic batch and the brief is model-visible in the next derive with
   the details upfront; an idempotent resume recomposes nothing.
6. The freeze list holds (§6); no TODOs; conventional atomic commits; no Co-Authored-By.
7. Atlas: a NEW truthful node (in-progress until the owner accepts; the gate-review flow
   as recorded on the closed restarts node).

## 9. Known pending (recorded, not built here)

- The cancel fence / wake latching and the pooled-engine cell watchdog — the steering /
  interrupt slices own them; `aborted`/`blocked` are their reserved union members.
- Derive compaction/aging — the `repair` brief persists like every history record; a
  memory slice that ages the derive inherits the rule that lifecycle linears fold first.
- Multi-step turns (steering/refinement-in-cycle) — the `step` level's reason to exist;
  today one step per turn.
- The turn-entry batch adds one small store write per turn — if the turn volume ever
  presses, the steering slice's budget table (slice 3) is where batching it tighter is
  decided, not here.