# Frame Tree + Model-Driven Control over WaveDB — Design

**Date:** 2026-09-29
**Status:** Approved
**Atlas slice:** replaces/absorbs the delivery order of `subdivide-session-into-accountable-agents` (S004) and most of `remember-session-across-restarts` (S003) — the Atlas map is recut accordingly (Slice A: model-driven frame tree; Slice B: restart/replay verification as S003's evidence pass).
**Source rationale:** docs/design-answers.md; docs/wavedb-exploration.md; docs/architecture-research.html (verified harness maps/critiques); docs/chat.json messages 4176-4202.

## Goal

A model-driven agent frame tree on the extracted actor runtime: an OpenAI-compatible model client drives a frame's turn loop; the model's only tool is a Python cell executed by pyrt; frame state is WaveDB-backed (one root database per process, frames as root-level subtrees, one atomic root batch per effect); frames spawn children admission-only and report back one event per child. Ends with a model-driven session running against a local Ollama from a small demo CLI.

## Decisions carried in (rationalized; each previously settled)

| Decision | Value | Authority |
|---|---|---|
| Model loop in the slice | YES (OpenAI-compatible client) | owner, 2026-09-29 |
| Model's tool surface | ONE tool (execute = Python cell, PA-verified: createAllToolDefinitions → {ipython}) | PA research |
| Verbs | actor *behaviors* behind the in-process bridge — NOT model tool calls. Verbs as tool-call surfaces are a later frame-subtraction option | owner instinct + in-process boundary collapsing PA's host_request wire protocol |
| Verb set | `remember`, `recall`, `spawn`, `report` (+ ephemeral `log`/`status`). `write`=`remember`, `read`=`recall` cut as synonyms; `inspect` deferred (YAGNI) | this design |
| Surface API | collapses for MVP: child sees parent state two ways only — spawn-spec handoff + recall's lineage walk. No closure vtable | this design |
| Data substrate | ONE WaveDB per process (`sync_only=0` always — sync-only does not compose with multi-actor access). Frames = root-level subtrees of composed absolute paths; NO subtree-of-subtree; no clones/snapshots ever needed (fork = lineage pointer + new subtree) | wavedb-exploration.md; owner |
| Event log | one JSON record per event at one key; `sessions/<sid>/events/<%020d-seq>`; zero-padded for byte-lexical ordering | wavedb-exploration.md (g)(e) |
| Write path | sync-inline from behaviors (µs-scale ops; non-blocking bar = "must not block >~1ms"). Promise path is the documented upgrade, triggered by: `wal_sync_mode=immediate` (fsync on calling path), large batches (compaction/surface rewrites), or write-combining. Read path: always direct lock-free MVCC | this design (revised from earlier promise advice, honestly) |
| Batches | one root `database_batch_sync_raw` per effect: event + state + lineage triple ops (`graph_triple_expand_ops`, `graph.h:71-86`). Must fit `wal_config.max_file_size` (default 128 KB) — rejection is fail-loud, never silent truncation | wavedb-exploration.md §7(d) |
| Seq allocation | frame-local in-memory counter; restored at boot by a reverse scan for the frame's latest event seq (no persisted counter to desync) | this design |

## Data model

```
sessions/<sid>/events/<%020d-seq>     value = one JSON event record; scan = forward/resume; reverse = "latest N"
sessions/<sid>/meta/created|parent|depth|status
sessions/<sid>/state/ctx/<key>        inheritable frame state; shadow by writing locally; unset = delete local key
sessions/<sid>/state/local/<key>      private scratch, never inherited
```

Event record: `{"seq":S, "type":T, "frame":"sessions/<sid>", "corr":C, "at":iso, "cause":prev_seq, "payload":{…}}`. Event types (deliberately few; ONLY `msg.append` ever produces context):

| Type | Meaning |
|---|---|
| `msg.append` | a conversation turn (role, content) — the only context-producing event |
| `frame.spawn` / `frame.report` / `frame.join` | tree events; parent's context grows by ONE report event per child (child transcript stays in the child's subtree) |
| `cell.run` / `cell.result` | Python cells, corr-matched (the audit trail) |
| `state.remember` | durable state writes, logged for replay |
| `control.*` | interrupt/shutdown/error records |

Lineage: graph triples `(childSid, parent_of, parentSid)` — index ops ride in the same root batch as the event. Depth-N ancestry = N+1 SPO/POS prefix scans (cheap per hop). Zero-padded seq; JSON values (the log is the audit trail — human-readable is the safety doctrine).

Deliberately NOT in this model: VectorDB (Pondr's consolidation layer), GraphQL (Pondr-facing ontology), materialized views (message history/tree derive by replay); the only reserved keys are compaction summaries, per-frame, when the /refine-equivalent lands.

## Components

- **`src/Frame/frame.c/.h`** — `frame_t`: owning struct, `actor_t` embedded FIRST (style guide), its subtree prefix, in-memory seq counter, `pyrt_t*` (lazy), config (`SA_FRAME_MAX_DEPTH`, model config). `frame_create(db, parent|NULL, spec)`, `frame_destroy`. Frame root = parent-NULL top-level session.
- **`src/Frame/model.c/.h`** — OpenAI-compatible client: `complete(messages, tools) → {content, tool_calls}`; config: base_url/api_key/model; Ollama works as-is. Behind a small vtable so tests inject scripted turns (no network in unit tests).
- **`src/Frame/bridge.c`** (pyrt extension) — injected `actor` module gains `agent.remember(k,v)`, `agent.recall(k)`, `agent.spawn(spec)`, `agent.report(v)`: each posts a typed message to the owning frame's mailbox (corr-matched), the frame's behavior applies the root WaveDB batch and answers corr-matched. In-process: no wire protocol. Awaiting inside a cell uses a bounded timeout; on timeout the request is answered as a failure, never deadlocked.
- **`src/Frame/loop.c/.h`** — the turn engine: derive context (bounded projection: replayed `msg.append` + ctx snapshot + one-line child reports — NOT a transcript dump) → call model with the single `execute` tool → cell → `cell.run`/`cell.result` events → next turn. Steering: `msg.append` between turns. Top frame ends when a turn produces no tool call.
- **`tools/frame-demo/`** — demo CLI: goal + model config → runs a top frame against the configured endpoint.

## Spawn / report semantics (PA-verified)

`spawn(spec {goal, context})`: validate `depth < SA_FRAME_MAX_DEPTH` (fail-loud, never substitute), child subtree = `sessions/<sid>/frames/<8hex>` (composed absolute path from root; arbitrary depth), persist `frame.spawn` + lineage triples in ONE parent batch, return handle immediately. Child runs its own loop (own model config inherited unless overridden). `report(v)` appends ONE `frame.report` event up + binds; `frame.join` closes the child. Child sees parent `ctx/` only via recall; never `local/`.

## Error handling

- Spawn failures: fail-loud per-request refusal (validity), corr-matched.
- Cell errors: `cell.result` status 1 + traceback text (pyrt already does this).
- Model/HTTP errors: `control.error` event; retry once, then end the turn.
- Batch over the WAL size cap: fail-loud with the trim suggestion; never silently truncate.
- No TODOs anywhere; every failure path leaves an event.

## Testing (gtest; all offline)

1. Frame store CRUD + batch atomicity on in-memory WaveDB (`location = NULL`).
2. Bridge verb round-trips: corr-matched remember/recall/spawn/report.
3. Loop with injected model vtable (scripted: cell → result → done) — proves the L1-L4 stack.
4. Restart/replay: build tree → `database_destroy` → reopen → resume with correct seq continuation + derived context (S003's acceptance evidence).
5. Opt-in integration: demo loop against a live Ollama, only when `SA_TEST_OLLAMA_URL` is set; skipped otherwise.
6. Full suite + valgrind + ASan/`setarch -R` invariants as before.

## Risks

- Context derivation is where token bloat tries to creep in — the derive step is a bounded projection with explicit caps.
- The model-client vtable must stay a completion boundary (no Claude-Code-shaped ergonomics creep).
- Windows: PCBuild wiring remains an explicit open item on slice 1's record, not silently absorbed here.