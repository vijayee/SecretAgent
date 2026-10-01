# Parity & Feature-Incorporation Matrix

**Date:** 2026-10-01. **Purpose:** decide WHAT to build next by lining every feature/design-fact from the
research + reference harnesses against what SecretAgent has (or has not) absorbed. Research only.

Legend — **L** = LANDED (commit ref), **DEV** = DELIBERATE DEVIATION (design-answers source, keep),
**P** = PENDING. Layer: L0 WaveDB substrate · L1 unified log/tree · L2 frame/scope · L3 loop ·
L4 tool pipeline · L5 persona/escalation. "PA test" = prime-agent test whose intent would port.

---

## Section 1 — Feature matrix

Source abbreviations: **AR** = docs/architecture-research.html (verified harness study; file:line refs
inside it were re-verified on import); **DA** = docs/design-answers.md `[msg N]`; **FT** =
docs/superpowers/specs/2026-09-29-frame-tree-model-loop-design.md; **OR** =
docs/superpowers/specs/2026-09-30-frame-orchestration-design.md; **ST** =
docs/superpowers/specs/2026-09-30-liboffs-streams-port-design.md; **CP** =
docs/superpowers/specs/2026-09-29-cpython-as-actor-design.md.

| # | Feature / design fact | Source (doc + ref) | Status | Layer | Test-parity candidate |
|---|---|---|---|---|---|
| 1 | **Session-log-as-actor-state**: every model request derived from folds over an append-only per-actor log | AR (deepseek-harness "THE essence"; dsh `packages/core/session/src/index.ts:775-881`) | **L** — WaveDB subtree log per frame; derive = stateless bounded scan (`0de24d6` store scans, `a85a506`, `2f5e552`) | L1 | FT tests 1/3 (`test/test_loop.cpp` scripted loop); parity OK |
| 2 | **Durable inbox** (mailbox = event fold; survives crash) | AR (dsh "durable inbox-as-log"; `inbox.ts:27-65`) | **DEV + P** — "messages are verbs, not nouns; no message store" (DA msgs 4211-4212). Residual P: *pending user input* durability across restart is the restart/reconcile slice | L2 | new (PA `session-action-store.test.ts` intent if ever needed) |
| 3 | **Commit-before-proceed durability** (append commits before the loop proceeds) | AR (dsh critique: advisory durability is its "biggest tension"; fix = commit-then-proceed) | **L** — store batches commit µs-scale synchronously; FIFO replies order causality (OR §5) | L1 | `test/test_frame.cpp` batch atomicity |
| 4 | **Turn/step lifecycle envelope** — step/end always written, turn/end with a full reason union (aborted/error/max-tokens/blocked/completed) | AR (dsh steal-list; `agent.ts:354-385`) | **P** — we have `control.*` events + meta/status, but no step boundaries and no reason union | L3 | port dsh `repair.ts` intent → new test |
| 5 | **Crash-repair closers over a truncated log** (pure, deterministic; cause-specific model-facing wording) | AR (dsh `packages/core/session/src/repair.ts`) | **P** — restart replay restores seq + context (`test_loop.cpp` TestRestartReplayRestoresSeqAndContext), but an open turn has no synthesizer | L1 | new (repair closers) |
| 6 | **Tool-call scheduler**: exclusive-barrier + bounded-parallel pool, model-order commit, synthetic results for unstarted calls | AR (dsh `tool-calls.ts:122-260`; steal-list) | **P** (deliberate MVP: ONE cell per turn, one tool) — the turn engine's dispatch shape already leaves room | L4 | PA none (CC-owned); new test |
| 7 | **Wake latching + sticky turn outcomes** (a wake during maintenance is never lost; max-tokens never downgraded) | AR (dsh `agent.ts:214-242, 341`) | **P** — stop_requested exists (`frame.c:1953` FRM_STOP); wake-latch/sticky rules not | L3 | new |
| 8 | **Envelope-only request logging with change detection** (don't re-log identical config each step) | AR (dsh steal-list; `agent.ts:612-634`) | **P** | L1 | new |
| 9 | **Compaction as log surgery** (summary shadows a span; generation bump) | AR (dsh steal-list; `compaction-basic`) FT:48 (reserved per-frame keys when /refine lands) | **P** | L1 | PA `compaction.test.ts` intent → new |
| 10 | **Durable streaming persistence with resume cursor** (outbound stream = persistence, not a side effect) | AR (onyx `stream_buffer.py:64-151`) | **P** — assistant deltas are NOT streamed (model client returns one body); audit dump happens post-loop (`tools/frame-demo/main.c:12-20`) | L3 | new |
| 11 | **Budgeted moving-context layout** (per-block token budget, defined eviction order, re-anchoring durable instructions near end-of-context; graceful degrade, never a raise) | AR (onyx README:93-162, `llm_loop.py:405-450`, steal-list) | **P** — derive is bounded by caps but has no per-block budget or eviction policy | L3 | new |
| 12 | **Durable cancel fence checked at quiescence points** | AR (onyx steal-list; `stop_signal_checker.py`) | **P** — FRM_STOP is control-not-interruption, in-memory, not durable (OR §1 step 2) | L2 | new |
| 13 | **Tool failure is data** (typed, model-consumable outcome; never an internal-exception leak into context) | AR (onyx `tool_runner.py:147-229`) | **L** — cell.result status 1 + traceback (CP §"Error handling"; `frame.c:1880-1949`) | L4 | `test/test_pyrt.cpp` failure cells |
| 14 | **Fingerprinted side-car digest of learned state** — base prompt immutable; one bounded digest message at turn commit only when the fingerprint changed; dedup newest-only | AR (PA steal-list; `messages.ts:49,208`, `agent-session.ts:7233-7245`, `session-manager.ts:499-535`) | **P** — /refine slice's core pattern (Section 3) | L5 | PA `refinement.test.ts` → `test_refine.cpp` |
| 15 | **Admission state machine** for inputs (legal-transition table, quiescence pump, rollback with durability proof) | AR (PA steal-list #1; `session-action-store.ts:63-134`) | **P** — our mailbox is FIFO; quiescence is the phase machine (OR §1). Revisit only when steering/multi-source input lands | L2 | PA `session-action-store.test.ts` |
| 16 | **Typed framed wire bridge** (version handshake, closed vocabulary, id-paired, bounded frames, corruption repair) | AR (PA steer-list #2; `repl-manager.ts:66-175`, `repl.py:152-188`) | **DEV** — in-process bridge instead of stdio JSON (FT "Decisions": "Verbs as tool-call surfaces are a later frame-subtraction option"; wire protocol collapses). A wire protocol re-enters only with a server-based embedder | L2 | n/a while DEV |
| 17 | **host_request**: model Python is a sandbox for *intent*; authoritative state stays in the host | DA msg 4206 (axis 4) + AR (PA) | **DEV** — replaced by the injected `agent.*` bridge verbs (`src/Python/py_agent.c:549-553`) | L2 | `test/test_py_agent.cpp` |
| 18 | **Admission-only spawn with a handle that never carries the child's answer**; depth cap; model resolution fails-not-substitutes; per-child durable registry | AR (PA steal-list #3; `agent-session.ts:12611-12698`, docs/rlm-runtime.md:30) | **L (DEV on start)** — "spawn = admit + start" (owner, OR decision table): the child additionally *begins running*; results return via `report` (not agent_message). Depth cap `SA_FRAME_MAX_DEPTH`, fail-loud (`924e36d`, `f43f73b`) | L2 | PA `agent-session-recursion.test.ts` → landed tree tests (`test_loop.cpp` TestPooledTree* intent) |
| 19 | **Parent-scoped child registry survives restart; deletion = durable tombstone** | AR (PA steer-list; `docs/rlm-runtime.md:171`) | **P** (recorded limit) — `live_children` is in-memory (OR §4 "Restart limit"); lineage triples persist, registry reconcile does not exist | L1 | new (restart/reconcile) |
| 20 | **Bounded collect** (snapshot-on-timeout) and **explicit child→parent/sibling messages** (`agent_message.send`) | AR (PA `rlm-runtime.ts:386-442`, doc 171) | **P** — only `report` crosses (one event per child); no steering/collect channel for children | L2 | PA `rlm-collect.test.ts`, `agent-session-bus.test.ts` |
| 21 | **Pull-only progress notes** (children never steer the parent unbidden) | AR (PA `rlm-runtime.ts:426-433`) | **DEV-adjacent** — structurally true: only FRM_REPORT_BIND/FRM_CHILD_REPORT arrive; no progress-notes verb exists | L2 | n/a |
| 22 | **Lazy interpreter boot** (frames that never run code pay nothing) | DA msg 4208 + PA #294 lesson; CP §"Capacity" | **L** — `pyrt_boot` on first EXECUTE (`src/Python/pyrt.c`) | L1/L2 | `test/test_pyrt.cpp` lazy-boot test |
| 23 | **Interpreter pool cap + queued-then-drained overflow** (K interpreters; boot would exceed → queue, never drop) | CP §"Capacity" 2; PA #294 (AR) | **L** — `test/test_pyrt_poolcap.cpp` | L1 | PA none; CP's own test spec |
| 24 | **Idle eviction of interpreters (opt-in knob)** | CP §"Capacity" 3 | **P** (knob not built; documented opt-in) | L1 | new |
| 25 | **Interrupt a running cell** | DA msgs 4208/4214; CP §"Interrupts" | **PARTIAL** — `pyrt_interrupt` exists and is tested; the *frame* exposes no interrupt entry (`tools/frame-demo/main.c:26-32`: frame owns pyrt privately); pooled engine has no cell watchdog (OR §6 escalated) | L2/L3 | `test/test_pyrt.cpp` interrupt | 
| 26 | **Dill snapshot of the namespace** | DA msg 4206 (axis 2 = PA's "weakness"); AR (PA reject-list) | **DEV** — "the live namespace is for *thinking*; WaveDB is for *remembering*" (DA msgs 4204-4206, 4222); no snapshot ever | L2 | n/a (deliberate) |
| 27 | **Fingerprinted digest re-delivered on turn commit; parked-on-abort re-arm** | AR (PA `agent-session.ts:7155-7245`) | **P** (row 14's mechanism detail) | L5 | PA `refinement.test.ts` |
| 28 | **Generation counters before touching restarted state** | AR (PA steal-list; `repl-manager.ts:226-227,341-420`) | **P** — corr matching covers the cell path only | L2 | new |
| 29 | **One budget function per boundary** (per-frame, per-turn, aggregate caps; oversized source fails its own actor) | AR (PA steer-list "budget-everything"; `shared.ts:41-46,300`, `repl-manager.ts:73-86`) | **PARTIAL** — derive caps + `_HTTP_BODY_MAX`/`_HTTP_READ_MAX` (ST §"Failure semantics") + frame protocol; no single table per boundary | L4 | new |
| 30 | **Fail-loud refusal at every policy boundary** (unknown kwargs rejected; missing model fails) | AR (PA strengths; `agent-session.ts:12602-12645`) | **L** — spawn/bridge refuse paths write loud paired status-1 `cell.result` (OR §1 reply path) | L2 | `test/test_py_agent.cpp` refusal tests |
| 31 | **Transition-record turn loop** (State + named transition reason; recovery paths assertable) | AR (CC steal-list #1; `query.ts:204-217,1099-1305`) | **L-equivalent** — frame phase machine + `control.*` events as the resume/audit record (OR §1; `7a3ab17`) | L3 | `test/test_loop.cpp` (engine transitions pinned) |
| 32 | **Loop-exit by observed behavior, not provider status** | AR (CC steal-list #3; `query.ts:552-558`) | **L** — loop exit from whether tool calls arrived, not provider hints | L3 | FT test 3 |
| 33 | **Read-parallel / write-exclusive tool batch policy** | AR (CC steal-list #4; `toolOrchestration.ts:91-116`) | **P** — single-cell turns make it moot; the slot machine (row 6) is its prerequisite | L4 | new |
| 34 | **Durable input before any model work** | AR (CC steal-list #2; `QueryEngine.ts:436-463`) | **L** — msg.append commits via the store batch FIFO-ahead of the derive scan (`frame.c:755-769` comment; `1398-1418`) | L3 | `test_loop.cpp` steering test |
| 35 | **Dynamic context as in-band tagged messages** (one channel for memory hits/notices/attachments) | AR (CC steal-list #6; `query.ts:1580`) | **P** — child reports ride `control`-adjacent lines in the system prompt (FT loop.c description); no generic `{attachment}` kind | L3 | new |
| 36 | **Cache-stable byte-stable prompt prefix engineering** (canonical prefix, content-addressed tail variation) | AR (CC steal-list #5; `prompts.ts:114-115,573`) | **DEV/P** — derive re-renders every turn (stateless from the store); content-addressing not built. Deviation is explicit for now ("documented known cost"; ST §non-goals) | L3 | new |
| 37 | **Permission gate allow/deny/ask + plan mode (plan → ask → act)** | AR (CC design facts; `permissions.ts:486-513`, `toolExecution.ts:921,1207`; also `claude-code-source-code/src/Tool.ts:500,753`) | **DEV** — execute is free by default; "the safety story is the event log; a gate is a one-file change later when a multi-user/server threat model exists" (DA msgs 4202, 4173). Plan-first escalation is L5's remaining substance (Section 5) | L5 | new (when L5 lands) |
| 38 | **Host_request→bridge rename**: PA's stdio host_request becomes in-process bridge verbs | FT "Decisions carried in" (owner instinct) | **DEV** | L2 | `test_py_agent.cpp` |
| 39 | **Blocking joins → event resume** (join = bookkeeping on the resume path; parent waits via FRAME_PHASE_CHILDREN, never a thread join) | OR "Decisions" (owner, exact words) | **DEV** (`f43f73b`, `534ae3d`; `frame_join` idempotent-safe at joined) | L2 | `test_loop.cpp` tree/run-loop tests |
| 40 | **Message store → DB-as-log** (no separate message store; transcripts/effects ARE store records) | DA msgs 4211-4212 (supersedes msg 4208's "WaveDB message backbone") | **DEV (settled)** — NOTE on wording: the *whole DB* holds the log; the WAL is durability machinery, not "the log is the WAL" | L1 | n/a |
| 41 | **WAL≈log conflation corrected in prose** | DA 4211-4212; user feedback session | **L (documentation)** — events are store records at `sessions/<sid>/events/<seq>`; the WAL is WaveDB's internal durability | L0/L1 | n/a |
| 42 | **Store actor (zero locks)** — WaveDB root wrapped in an actor; reads/writes serialized; lock ordering/deadlock audits eliminated | OR §5 amendment (owner verbatim) | **L** — `wave_database_root_t.store_actor` first member, `_store_behavior` single-message (`frame.c:237,734,893`); model.c's install-once mount mutexes are the documented exception | L0-L2 | `test/test_frame.cpp` store-actor tests; plan OR grep gate (commit `9cd85ca`) |
| 43 | **Seq pre-allocation per actor + one-atomic-batch cross-frame effects; abandon-on-reject (best-effort roll-back)** | OR §5 "Seq discipline" | **L** | L1 | `test_frame.cpp` seq tests |
| 44 | **REST routes / desktop integration on the streams server** | ST §"What moves" (routes are the desktop slice's work) | **P** — server core + auth + cors landed (`df132e0`, `b217e2c`, auth/401-vs-403 tests); routes and actor-glue (a handler posting into a frame's mailbox — the documented extension point, ST §"client contract") do not | L2/L3 | `test/test_streams_server.cpp` continues |
| 45 | **asio-in-scheduler / transport-in-scheduler question (liboffs Actor/Scheduler)** | liboffs src/Actor, src/Scheduler (passing note) | **Settled by ST**: ONE poll-dancer loop thread per process (the IO reactor) is separate from the scheduler pool that runs actors; shared state = the mailbox mutex only (ST §"Concurrency model") | L2 | n/a |
| 46 | **Session input sources beyond the loop** (slash commands, skills, heartbeats, schedules, goals, autonomous mode, daemon sessions) | AR (PA loop facts; README "Built for Long-Running Work" section) | **P** — demo CLI = one goal, one blocking run (`tools/frame-demo/main.c` usage text) | L3/L5 | PA per-feature tests; scope when owner picks it |
| 47 | **Semantic memory (consolidation)** — Pondr as the semantic store; consolidation runs off the access path | DA msg 4220 (GeoSTM doctrine) | **P** (Pondr is the library's first client, never merged — DA msg 4218) | L4 | new; schema = `sessions/<id>/events/<seq>` both sides |
| 48 | **VectorDB / GraphQL / materialized views** | FT:48 ("deliberately NOT in this model") | **DEV** (n/a) | L0 | n/a |
| 49 | **LLM-facing error protocol surface + provider-agnostic streaming client** | AR (onyx reject/steal list, "tool ABI" line) | **PARTIAL** — `model_backend_t` vtable = the completion/submit boundary (OR §3); no streaming deltas (row 10) | L4 | `test_model_decode.cpp` |
| 50 | **Windows verification** (PCBuild cpython branch; IOCP backend compiles) | CP §"Cross-platform"; ST §"Evidence bar" 4 | **P** — no toolchain on this machine; tracked on Atlas nodes | all | n/a here |

---

## Section 2 — The seven-verb surface audit

Design contract: seven verbs, born free (no sandbox — DA msgs 4202, 4173), narrowed by subtraction
(msg 4202: `-execute` → assistant, `-remember` → stateless function, …); `remember()` writes the
frame's own subtree, sharing ONLY via `report()` up and a parent's API projection down — no third path
(msg 4202 tail; DA §7).

| Verb (design name) | Contract (msg 4202 / DA §7) | Landed shape | Asymmetries / missing |
|---|---|---|---|
| **read** | see anything in the subtree/filesystem/results | Merged into `recall` — "write=`remember`, read=`recall` cut as synonyms" (FT:19, Verbs row) | `recall` resolves only a *state key* up the lineage (`frame.h:123`; `_frame_store_recall` max_hops walk). It cannot read events, read a child's subtree, or read by path. No `read(path)` semantics — P |
| **inspect** | structured query: scan, lineage, graph — "what's in my subtree, who are my children" | **Deliberately deferred (YAGNI)** (FT:19) | Only implicit exists: the derive's internal bounded scan (engine-side), never model-visible. Without inspect the agent can recall a value but not *find* — pull forward only if real sessions demand it (recorded YAGNI, not a bug) |
| **write** | mutate state, emit output | Cut as a `remember` synonym (FT:19) + `emit` exists as the durable-payload candidate channel (`py_agent.c:546`, `pyrt_messages.h:18`) | **emit is not durable yet**: PYRT_EMIT reaches the frame mailbox and is currently dropped (no `case PYRT_EMIT` in `src/Frame/frame.c`; CP:63 reserved it for the "persistence slice wires it into WaveDB later"). The durable `write` verb's missing half = P |
| **execute** | run code, close the loop, free by default | The pyrt cell: corr-matched EXECUTE→RESULT (`frame.c:1880`), lazy boot (row 22), pool cap (row 23), `agent.log/status` stream up, `traceback` on failure | (a) single cell per turn — one execute tool per model turn (FT Goal, loop.c); (b) no frame-level interrupt entry (row 25); (c) cells are sequential — matches DA msg 4206 axis 3; (d) no per-result output cap yet inside the tool path (row 29) |
| **spawn** | fork a child frame; admission-only in PA | `frame_spawn(parent, goal, context_json)` (`frame.h:143`); cell-side `agent.spawn` (`py_agent.c:552`) → FRM_SPAWN: depth cap, one batch (spawn event + lineage triples + child subtree + meta), then **started** (DEV, row 18) | Spawn spec is `{goal, context}` only — no model/thinking override (PA `12630-12644`); child inherits pool/turn-cap/backend (`memory/project_secretagent.md` orchestration notes). Child-name reservation/uniqueness semantics: sid = generated (root rng + counter, `frame.c:379`), no user-chosen names to collide — OK |
| **report** | return a value to the parent | `frame_report` / cell-side `agent.report` → ONE `frame.report` at child's pre-allocated seq + parent's bound event + child done, ONE batch, then FRM_CHILD_REPORT resumes the parent (OR §4) | Quiet completion is added (DEV beyond contract: a content-only child reports its content implicitly — OR escalation 1, owner-approved). Top-frame report binds to itself (no parent) — `_frame_report_top_post`, `frame.c:1430` |
| **remember** | only write that survives the frame; own subtree | `frame_remember_local`/`frame_remember_ctx` (`frame.h:120-121`) + cell-side `agent.remember` which writes the **inheritable** ctx layer ("durable, shared-by-default", `frame.c:1754` comment) | Two notes: (1) cell-side remember is ctx/ — children *down* inherit its values via the recall walk; that is the "API over the parent's subtree" shape (DA msgs 4191-4196) and fixes #819 by scoping, but differs from msg 4202's "private only" reading — the ctx/local split is the settled compromise (FT data model). (2) No *list/keys* operation (inspect's domain). No session-scoping knob (`scope=local/global` from PA row 14) |

**Verdict:** the surface is 5 verbs + a 6th half-built (`emit`/`write` durability) + 1 deferred
(`inspect`). All 153 tests pin what exists; nothing violates the no-third-path sharing rule.

---

## Section 3 — /refine (the Continual Harness)

**PA's facts (README 42,58,93 + `packages/coding-agent/src/core/refinement/refinement.ts`):**
`/refine` reviews the current trajectory with an auxiliary model and applies small, evidence-backed
CRUD edits to *supplemental* harness state — it **never rewrites the immutable base system prompt**
(README:58; refinement.ts:129-180 system prompt: "The base system prompt is immutable and MUST NOT
be rewritten"), and recorded snapshots support rollback (README:58).

The OPERATIONS table, from the code's structure (`refinement.ts`, 1,432 lines):

| Surface | Operation | Where |
|---|---|---|
| Kinds | `prompt` (supplemental prompt notes) · `memory` (durable facts/decisions/failures/preferences) · `skill` (installed Python REPL skill: `reference{type:python,import,callable,call_pattern}` + `arguments`) · `subagent` (reusable delegation spec + RLM-native call form) | `RefinementKind` (`refinement.ts:35`), contract at :129-180 |
| Actions | `create` · `update` · `delete` (+ `version`-guarded updates; the stale-target edit is REJECTED when the entry changed after planning) | `RefinementAction` :36; rejects verified in `test/refinement.test.ts:152-205` |
| Scopes | `local` (session artifact dir `harness/harness_state.json`) · `global` (`~/.prime/agent/harness/`) — merge keeps global and local separate; during a local refinement global is read-only context | :18-19, 380-407, 473-483; docs/rlm-runtime.md:191 |
| State | `HarnessState = H=(ρ, G, K, M)` — prompts, subagent-specs, skills, memories, mirrored to disk; H is CRUD-able live from Python as `rlm.harness` (msg 4162; `rlm.ts:47` — create/update/delete per kind, `record_refinement`, `overview`) | `HarnessState` :64; PA README / rlm.md |
| Apply | `applyRefinementProposal` — validate edits (missing fields/unknown action/skill must carry reference+arguments), mutate state atomically, bump entry versions | :1066; `.test.ts:351-368` |
| Rollback | `rollbackProposal` — replays a recorded history event's before/after snapshots; history = `refinements.jsonl` append, malformed lines skipped, session history preferred over global | :1163, 1198, 421-466; `.test.ts:680-737, 965-997` |
| Plan | `planRefinement` — aux-model call over the serialized trajectory (last 80k chars per AR PA facts), JSON-only proposal, output budgeted (32k, policy-capped) | :1231-1316, 194-206, 201 caps; `.test.ts:588-678` |
| Auto-refine gate | `reviewAutoRefine` — a *smaller* review (40k) over "turn_interval"|"compact" reasons deciding whether to run a full refine | :116-122, 1331 |
| Delivery | the **fingerprinted digest**: `formatHarnessStateForPrompt` renders a bounded overview (6 entries/kind, 180 chars) + `harnessDigestFingerprint`; one `[harness-digest]` custom message injected at turn commit ONLY when the fingerprint changed; parked-on-abort re-arm; newest-only dedup on replay | :658, 794, 846; AR rows 14/27 |
| Ranking | harness-query term scoring (weighted term overlap, doc-frequency discount, stable id tie-break, alphabetical fallback) — the #819 lesson *mechanized* | :537-656; `.test.ts:999-1116` |

**The proposed SecretAgent shape (owner-agreed in session, 2026-09-30/10-01):**
- Supplemental state = **a WaveDB subtree** (e.g. `harness/<kind>/<id>` with `meta/version`), not
  scattered JSON files — PA's `harness_state.json`+`refinements.jsonl`+per-file reload-to-avoid-clobber
  logic (AR PA weakness row) collapses into the DB-as-log.
- **refine = a user-invoked command in the demo CLI**, doing a bounded trajectory review (recent
  events scan, bounded — reuse the derive caps) **through the store actor**, then **small
  evidence-backed writes**: `remember` scoped to the supplemental subtree — the store actor's
  atomic batches + seq pre-allocation give apply/rollback correctness for free.
- **History/rollback free from the DB-as-log**: the whole DB holds the log (the WAL is durability
  machinery, not the log — row 40 wording); refinement history is just appended store records, and
  rollback can be a recomposition, not a snapshot file. Whether rollback is *recomposition* (rewrite
  compaction) or an *inverse-edit batch* is an open design question below.
- **auto-refine = a scheduled frame behavior later** (a frame whose scheduled behavior is the
  bounded review; PA's "turn_interval|compact" reasons map to cadence + compaction as triggers).

**Open design questions for the refine slice:**
1. What is the aux reviewer? Same model config, a second configured model, or a scripted-test-only
   backend (vtable) for the offline suite? (PA has an explicit aux model + output budget.)
2. Where do refine writes land: `harness/…` at the ROOT, or per-frame? Global vs session scope maps
   to root-subtree vs frame-subtree — the #819 scoping lesson says child visibility must be a
   recall-walk question (children currently walk only their own lineage's ctx).
3. Digest delivery into the prompt: a derived `harness-digest` line in the turn projection gated on
   a fingerprint (row 14) — and does the digest live in `msg.append` history (replayed) or ride the
   derive as a re-anchored block (onyx moving-blocks, row 11)?
4. Ranking: port PA's term-IDF scorer, or stay simple (newest-first + explicit `path` grouping) until
   size forces it?

---

## Section 4 — Memory architecture gap

PA's four levels (chat.json msg 4162, table verified against arxiv 2608.23552 wording):
**L0 model weights (fine-tuning) · L1 context (compaction rewrites) · L2 REPL values + subagent
sessions ("agentic garbage collection": the model creates/retains/summarizes/deletes live values) ·
L3 the Continual Harness (prompts, memories, skills, subagents — `/refine`).**

SecretAgent's memory (DA msg 4220): **working** = model context + the live pyrt namespace;
**episodic** = WaveDB (events, frames, replayable trace); **semantic** = Pondr (future, never merged).

| Level | PA's intended content & mechanism | Ours today | Missing |
|---|---|---|---|
| L0 weights | model weights, updated by fine-tuning | out of scope (we call hosted models) | n/a; note PA's point that the model's *trained fluency in a stateful REPL* is why Python stays (msg 4162) |
| L1 context | the context window; **compaction rewrites** it (L1 surgery, shadows a span) | derive = bounded projection (replayed `msg.append` + ctx snapshot + one-line child reports) recomputed each turn; live namespace is thinking | **Compaction/shadowing + the generation bump** (row 9); an eviction ORDER (row 11); PA's per-variable 16/256 MiB caps analog = our per-turn budget (row 29) |
| L2 live values + session state | REPL namespace + subagent sessions; model-driven summarize/delete ("agentic GC") | the live namespace (no dill — DEV row 26) + frame subtrees + `state/{ctx,local}` | **No model-facing summarize/delete over its own store keys**: `remember` has no delete/unset on the model surface (only the store API's unset/revert idea from DA msg 4194); eviction knob un-built (row 24). Also: namespace is lost at frame death by design — PA's revival is ours = restart replay of the *transcript*, never the heap |
| L3 continual harness | prompts/memories/skill-descs/subagent-specs; refined, versioned, scoped local/global | frame state ctx/local + events; NOTHING consolidated cross-session | **The whole level** — the refine slice (Section 3); plus digest re-delivery (row 14) and scoped visibility for children (the #819 rule: children inherit ctx via recall only — already structural, row 35) |
| (ours) episodic | — | WaveDB events + replay + restart-resume (`test_loop.cpp` TestRestartReplay…) | open-turn repair closers (row 5) |
| (ours) semantic | Pondr, consolidation off the access path (GeoSTM doctrine) | not started; schema alignment noted in DA §6 | Pondr integration is a *schema* contract, not code — keep |

---

## Section 5 — Escalation (L5) + persona

**Claude Code's pattern** (AR claude-code section; decompiled proprietary source — patterns only,
never strings/identifiers, AR CC reject-list):
- The turn loop carries a **plan→ask→act** path: permission modes transform decisions
  (`dontAsk` ask→deny, auto→classifier, `permissions.ts:503-513,744-745`); plan mode drives a
  model-selection heuristic inside the loop (`query.ts:570-578`); the gate is a *layered chain*:
  rules/safety checks → mode transform → tool-specific `checkPermissions` → hooks, with the
  interactive dialog as the fallback (`toolExecution.ts:921,1207`; `Tool.ts:500,753`).
- The **tool pipeline is guarded pre→execute→post**: `runPreToolUseHooks` (:800) can stop execution,
  `tool.call` after `validateInput` (:1207), `runPostToolUseHooks` (:1483) — the DSH
  pre → execute → post → result pipeline under a hook surface.
- **Mapping to our frames:** (1) escalation lives at L5 as *loadable text*, not loop code — the loop
  (L3) stays amoral (DA msgs 4146 "rigorous but flat" critique, msgs 4148/4198 loop-manner split);
  (2) the frame actor's mailbox is the natural "ask" surface: an `ask` message the frame cannot
  resolve itself escalates to its parent (or the user surface), corr-matched, exactly like a bridge
  verb — no second pipeline; (3) `validateInput` ≈ the typed message payload decode + the fail-loud
  refusal convention (row 30); (4) pre/post hooks ≈ nothing yet — our only guard pair is
  `cell.run` audit → execute → `cell.result` (row 13); a pre/post hook seam is where row 6's slot
  machine and row 37's gate both plug in later, as *one* seam.
- Owner override still stands (DA msgs 4173, 4202): execute free by default; the gate is a later
  one-file change when a multi-user/server threat model exists.

**Persona as loadable text modules:** "the hammer" spec — warmth-as-fit, truth first, read intent,
anticipate, signal density, checkable behavioral tests, *falsifiable* ("Any behavior it cannot be
reduced to a checkable test for is NOT part of the persona") — is written down in full at
**chat.json msg 4148**, with the persona/context separation doctrine (voice = portable voice+stance,
loaded per-agent; context = who the user is, never welded into persona) at msg 4148 too, and the
loop/manner split at msgs 4148/4198 (DA §7). **How a text module loads (the L5 spec pointer):** the
persona is data — a versioned spec the runtime loads and injects at the prompt boundary (Onyx's
persona-essence: "a system prompt template plus placement mode plus tool manifest plus context
-attachment policy centralized into ONE serializable, loadable persona manifest" — AR Onyx
steal-list last item); on our frames this becomes: `frame_config_t` (or the spawn spec) carries a
*persona pointer into the store* (a keyed text record), and the derive injects it as the system
prompt's first block — cache-stable prefix rules (row 36) apply to how it renders. The per-user
project memory (msg 4148's second layer) is the ctx/ layer + L3 refine state, never the persona text.

---

## Section 6 — Recommended slice order (dependency-ordered)

1. **Refine / continual-harness slice** (L3/L5, unblocks the whole memory story)
   Supplemental state as a WaveDB subtree through the store actor; demo-CLI `/refine` command:
   bounded recent-events scan → aux-model review (vtable backend for tests) → small evidence-backed
   writes (`remember` to the supplemental subtree); fingerprint-gated harness-digest in the derive;
   refinement history = appended store records; rollback.
   *Tests:* port `test/refinement.test.ts` intent (stale-target rejection, atomic replace, versions,
   rollback across sessions, digest newest-only, fingerprint change gating, ranking tie-breaks) →
   `test/test_refine.cpp` (new); live-gate style opt-in for a real aux model.
2. **Turn/step lifecycle + crash-repair slice** (L1/L3; DSH's strongest un-landed essence)
   step boundaries; `turn.end` with a full reason union (aborted/error/turn-limit/model/cell);
   pure deterministic repair over a truncated log on `frame_resume` (open turn → synthesized
   closers with cause-specific wording); durable-input-before-model-work is already a store fact —
   the closers make resume *structurally* safe.
   *Tests:* new repair-closer suite (truncated log variants × cause); extends `test_loop.cpp`
   restart tests.
3. **Surface completion slice** (L2/L4, the seven-verb audit's P rows)
   `emit` durable channel wired to a store event; frame-level interrupt entrypoint (cell interrupt
   reaches a running pooled engine; the documented watchdog question resolved at the same stroke);
   `read`/`inspect` pull-forward decision resolved from #1-2's real usage (list-keys on
   `state/local|ctx` + child listing, scan-based); one budget table per boundary (derive caps +
   tool-result cap + bridge payload cap) with fail-loud truncation-at-source.
   *Tests:* extend `test/test_py_agent.cpp` (emit durability), `test/test_pyrt.cpp` (interrupt at
   frame level), new inspect/budget tests.
4. **Steering & multi-source input slice** (L3; PA's admission discipline, only when needed)
   steering writes between turns exist (`msg.append` between turns); formalize into the admission
   state machine ONLY if a second input source lands (desktop REST, heartbeats): legal-transition
   table + quiescence pump, ported from `session-action-store.ts:63-134` (row 15) — next_turn_
   boundary vs when_run_idle map to our frame phases.
   *Tests:* port `session-action-store.test.ts` intent → `test/test_admission.cpp` (new).
5. **L5 persona + escalation slice** (L5; the last big layer)
   Loadable persona pointer (store record referenced by frame config); the hammer spec as the first
   persona record, *tested as data* (its checkable-behavior list = the test spec); plan→ask→act as a
   configurable escalation ladder over the bridge/ask message (row 37), execute still free by
   default (DEV stands); tool-result budget + tombstone (CC's two kept compaction mechanisms, AR
   reject-list) fold into the derive.
   *Tests:* new persona-record + derive tests; escalation-ladder tests.

**Sequencing notes:** 1 and 2 share the store/digest machinery and 2's `turn.end` reason union is
what refine's "trajectory review" reads; 4 waits until a second input source exists (YAGNI guard);
5 is deliberately last because every earlier slice keeps the amoral loop unchanged (DA §7).

---

## Owner escalations (unanswered from the sources)

1. **Refine reviewer model**: same config / second model / vtable-only for tests (Section 3 Q1).
2. **Refine scope root-vs-frame** and digest placement history-vs-re-anchor (Section 3 Q2/Q3) —
   both affect child visibility (#819) and the cache-stable prefix (row 36).
3. **Rollback semantics**: recomposition vs inverse-edit batch (Section 3).
4. **Pooled-engine cell watchdog** (OR escalation 3, still open): hangs silently until destroy;
   decide accept vs pull interrupt forward into slice 3.
5. **Spawn model-override**: accept inherit-only (subtree property, OR §2) or add spec override
   (PA `12630-12644`) — affects slice 3's surface.
6. **`inspect` pull-forward** (Section 2): keep deferred until 1-3 create demand, or include list/
   scan now while slice 3's budget table is being written?
7. Durable **cancel fence / wake latching** — fold into slice 2's lifecycle or later with the
   desktop interrupt story?