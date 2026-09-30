# Frame Orchestration — Design

**Date:** 2026-09-30
**Status:** agreed with owner in session (sections approved inline; the mechanics below — pool attachment, turn-state fields, message set — were resolved from the code by the planning pass, recorded here as decisions). AMENDED 2026-09-30 by the owner before implementation: the WaveDB root is wrapped in a store actor — *"this suggests we need to wrap wavedb in an actor therefore serializing writes and reads and eliminating the need for locks"* — which REPLACES §5's per-frame write-lock decision (the original §5 decision and the consequences the owner approved are recorded in the rewritten §5).
**Atlas slice:** `drive-model-driven-frame-tree` milestone (spawned frames actually run, fully event-driven); folds in `subdivide-session-into-accountable-agents`' verification assertions and closes `remember-session-across-restarts` as evidence-only.
**Predecessor spec:** docs/superpowers/specs/2026-09-30-liboffs-streams-port-design.md (transport shape — "posting completions into an owner actor's mailbox is the documented extension point"; THIS slice builds that glue).

## Why this slice exists

The turn loop (src/Frame/loop.c) is a **synchronous**, **inline** engine: whoever calls
`frame_run_loop` holds a thread for the whole frame run — through every derive, every
model call (60–150 s against a local model was the measured shape), every cell wait, and
every turn. Today a frame actor is created with `pool = NULL` (frame.c:1241/1431 —
`actor_init(&f->actor, f, _frame_behavior, NULL)`) and its mailbox is pumped by the
loop's own thread or `_frame_cell_wait`; "driving a child is the tree slices' business"
was a literal TODO-shaped refusal in the FRM_SPAWN handler (spawned children are admitted,
their `frame_t` is destroyed on the spot, and nothing ever runs them).

This slice makes the frame **be** the actor: the turn loop dissolves into scheduled
turn-step continuations, children get started when they are admitted, parents resume from
child events, and no thread anywhere waits on a child or on a model call. Owner's exact
words (2026-09-30): *"the children should event their results back to the parents. the
model can resume what's left after the event. no thread join style blocking. Everything
is event driven. Or in ponylang behavior driven."*

## Decisions carried in (already settled with the owner)

| Decision | Value | Authority |
|---|---|---|
| The frame IS the actor | the turn loop is scheduled continuations, not a held thread | owner, 2026-09-30 (Section 1 approved) |
| Model call | rides the async transport — `http_client_submit` on the streams loop thread; workers never block on the model call | streams-port slice + owner |
| Child results | event back to the parent; the parent re-derives what's left — no join, ever | owner, exact words above |
| Spawn semantics | spawn = admit + start (admission stays PA-fail-loud; the child additionally begins running) | owner |
| Child failure | fail loud into the parent: one `frame.report` with the failure text bound into the parent's log, child done, parent resumes | owner |
| Join semantics | `frame_join` becomes bookkeeping on the resume path (child joined, parent continues); never a wait | owner |
| Atlas node close | `remember-session-across-restarts` closes evidence-only (its proof tests exist); `subdivide-session-into-accountable-agents`' evidence items become THIS plan's acceptance criteria | owner |
| Out of scope | seven-verb surface beyond existing bridge verbs; REST routes (desktop slice); Windows; L5 persona | owner |

## 1. The turn engine: one frame, scheduled turn-step continuations

The synchronous `for(;;)` of `frame_run_loop` dissolves into a **phase machine** carried
on the frame. One turn = at most four dispatches of the frame's own behavior, each an
activation the actor yields between:

```
frame_start ──▶ FRM_TURN ─┬─▶ derive ─▶ submit model ─phase=MODEL─▶ (yield)
                          │                                 │
                          │   streams loop thread: completion
                          │                                 ▼
                          │                          FRM_MODEL_RESULT ─phase=NONE─▶ (yield)
                          │                                 │
                          │   tool path: cell.run event + FRM_CELL_EXECUTE
                          │                                 ▼
                          │                            phase=CELL ─▶ (yield)
                          │                                 │
                          │   pyrt worker: PYRT_RESULT (completes the cell slot)
                          │                                 ▼
                          │                         repost FRM_TURN  ─▶ next turn
                          │
                          └─▶ content path: msg.append (or empty-turn control)
                                            ├─ live_children > 0 ─▶ phase=CHILDREN ─▶ (yield; done resuming)
                                            └─ live_children == 0 ─▶ top: status done ─▶ engine ends
                                                                     └─ child: quiet-completion report ─▶ engine ends
```

Every arrow above is a **mailbox dispatch or a repost** — never a wait. The actor yields
to its scheduler pool between phases; a pool worker picks it up again when the next
arrival lands in the mailbox (model completion, PYRT_RESULT, child report). The derive
itself is one MORE store round trip (§5): the FRM_TURN step posts the bounded events scan
(`FRAME_PHASE_STORE`), yields; the `FRM_STORE_REPLY` router parses the materialized
records and runs the projection + submit in that same dispatch. The projection is
unchanged (stateless, from the store, bounded caps — nothing accumulates in engine
memory), which is what makes any arrival order benign: whichever dispatch runs
last re-derives from the store and sees everything.

### Engine state (frame_t additions, single-writer via mailbox dispatch)

```c
/* One live engine per frame (frame_internal.h): */
typedef enum frame_phase_e {
  FRAME_PHASE_NONE = 0,   /* between steps (a step just ran / engine not started) */
  FRAME_PHASE_MODEL,      /* an async model submit is in flight */
  FRAME_PHASE_CELL,       /* the turn's one cell is in flight */
  FRAME_PHASE_STORE,      /* a store round trip is in flight (§5: the derive
                             scan, the awaited cell.run commit, the finish batch) */
  FRAME_PHASE_CHILDREN,   /* yielded: live children; the engine resumes on FRM_CHILD_REPORT */
} frame_phase_e;
/* What the pending FRAME_PHASE_STORE round trip is for: */
typedef enum frame_store_kind_e {
  FRAME_STORE_DERIVE = 1,   /* the bounded events scan; the reply runs the projection */
  FRAME_STORE_CELL_RUN,     /* the cell.run audit commit; the reply dispatches
                               FRM_CELL_EXECUTE (no untracked cell may ever run) */
  FRAME_STORE_FINISH        /* msg.append (+ meta/status=done for a top frame) in one
                               batch; the reply ends the engine (top) or drives the
                               child's report bind (child) */
} frame_store_kind_e;
/* frame_t gains: */
uint8_t        engine_live;    /* 1 between frame_start and a terminal step */
frame_phase_e  phase;
frame_store_kind_e store_kind; /* routing of the pending FRAME_PHASE_STORE reply */
uint64_t       store_corr;     /* the frame's own round-trip key for that reply */
unsigned       turns_issued;   /* model turns issued THIS RUN (cap check) */
uint8_t        model_retries;  /* consecutive failed-model calls (retry-once is per call) */
uint8_t        engine_failed;  /* the last terminal step failed (frame_run_loop's rc) */
size_t         live_children;  /* children admitted-and-started, not yet resumed (THIS process only) */
```

`turns_issued` resets at `frame_start`, so the cap stays a per-run bound exactly like the
old loop's (the live gate's full-run retry gets a fresh budget — unchanged behavior).
`live_children` is in-memory bookkeeping of THIS process's spawns; a restart-resumed
frame starts at 0 — children that survived the process on disk are the restart/reconcile
slice's business, recorded as a known limit.

### Message vocabulary (frame_messages.h owns the FRM_* surface)

```c
FRM_TURN,          /* engine -> itself: the scheduled turn-step continuation (payload NULL) */
FRM_MODEL_RESULT,  /* transport -> frame: the model completion arrived (frm_model_payload_t) */
FRM_CHILD_REPORT   /* child -> parent: this child's engine is terminal (frm_child_report_payload_t) */
/* The store vocabulary (Section 5 — all types owned by frame_messages.h): */
FRM_STORE_BATCH,   /* any frame thread -> store actor: one atomic op list (frm_store_batch_payload_t) */
FRM_STORE_SCAN,    /* -> store actor: bounded reverse range read (frm_store_scan_payload_t) */
FRM_STORE_RECALL,  /* -> store actor: the lineage resolve walk (frm_store_recall_payload_t) */
FRM_STORE_REPLY,   /* store actor -> requester: corr-matched result (frm_store_reply_payload_t) */
FRM_REPORT_BIND    /* child frame actor -> parent frame actor: compose the cross-subtree
                      report batch there (frm_report_bind_payload_t; Section 5) */
```

```c
/* model completion: the http body and error move in RAW (steal-slot, exactly
   model.c's completion record shape); the FRM_MODEL_RESULT behavior decodes
   via model_internal.h's body decode — the streams completion stays µs-scale
   (field writes + actor_send) and never json-parses megabytes on the reactor
   thread. Ownership of body/error transfers to the frame behavior. */
typedef struct frm_model_payload_t { int status; char* body; size_t body_len; char* error; } frm_model_payload_t;
/* child terminal: the parent re-schedules its engine. The parent-side log
   binding ALREADY happened (the child's report bind batch was confirmed
   committed by the store actor's reply BEFORE this message is posted — see
   §5), so this payload is bookkeeping only: which child ended, and whether
   to log the resume loudly (the failed flag — the parent's thread still
   logs the failure even though the binding already landed). */
typedef struct frm_child_report_payload_t { char* child_sid; uint8_t failed; } frm_child_report_payload_t;
```

Both ride `actor_send` (payloads get `frm_*_destroy`), which is the documented universal
send: it schedules a pooled actor or queues an inline one — the same path pyrt already
uses to return PYRT_RESULT.

### The turn step (loop.c keeps the engine; the handlers are loop.c's)

`_frame_engine_turn` (invoked by frame.c's `_frame_behavior` on FRM_TURN):

1. `!engine_live` → drop (late repost after a terminal step; loud log).
2. `stop_requested` → engine ends (FRM_STOP keeps its control-not-interruption shape;
   no status change — document: stop during `FRAME_PHASE_CHILDREN` leaves the frame
   awaiting; children are their own engines).
3. `frame_is_done` → engine ends (a report landed mid-flight and already ended this frame).
4. `turns_issued == cap` → control `turn-limit` → terminate-failure (see §4).
5. `turn++`; derive → `control derive-error` on failure; backend resolve → `control model-missing`.
6. Backend declares `submit` (§3) → `submit(...)`: `rc == 0` → `phase = FRAME_PHASE_MODEL`,
   destroy the derived messages (the backend copied everything it needs), return — yield.
   `rc != 0` → the sink will never fire → `control submit-failed` → terminate-failure.
7. Backend is sync-only (`submit == NULL` — every existing scripted test backend):
   `complete()` runs INLINE inside this dispatch — blocking the actor, acceptable ONLY on
   the documented inline/test driver (see §6), never on a pool worker in production (the
   production backend implements `submit`). Then the reply processes as below.

The reply processing (`_frame_engine_reply`, shared by the sync and async arrival paths —
"scripted backends drive the SAME code"): model error → `control model-error` + one retry
(`model_retries++` then repost FRM_TURN; the re-derive is provably equivalent to the old
loop's same-array retry because the derive is stateless from the store — a steering write
that slipped in only ADDS context; second consecutive failure → `model-error-final` +
terminate) → tool path = today's loop.c tool path byte-for-byte (python gate → `cell.run`
audit → FRM_CELL_EXECUTE dispatch → refuse paths write the paired status-1 `cell.result`
event the old loop silently skipped) → content path = today's content path (msg.append /
loud `empty-turn` control) with the ending rule of §4.

`PYRT_RESULT` (frame.c) gains one tail call after slot completion:
`_frame_engine_cell_done(f)` — `engine_live && phase == CELL` → repost FRM_TURN. Synchronous
cell refusals (pending already set, pyrt boot refusal, corr 0) never set `cell_pending`, so
the FRM_TURN dispatch itself checks "still not pending after FRM_CELL_EXECUTE" and resumes
directly — same rule, one place.

### Public API (frame.h — frozen here)

```c
/* Begin (or restart after an ended run) the event-driven turn engine: ONE
   turn-step continuation is queued on the frame's actor; the actor yields to
   its scheduler pool between phases and re-runs on every arrival (model
   completion, cell result, child report). Refuses loudly (-1 + log_error) on a
   dead frame or an engine that is already live (one engine per frame). */
int frame_start(frame_t* f);
```

## 2. Pool attachment (resolved mechanics)

**Decision: the pool rides the frame, inherited down the lineage — a
`scheduler_pool_t* pool` field on `frame_config_t` (borrowed; NULL = today's inline
shape, which every existing test keeps). A process default pool is deliberately NOT
built here.** Rationale:

- The scheduler API (scheduler.h) has no process-scoped pool; a default would have to
  answer lifecycle questions (when do worker threads start, who calls stop, what happens
  to running frames at destroy) that belong to the embedding process. The demo CLI and
  desktop slice create an explicit pool; `frame_create`/`frame_spawn` only inherit and
  attach. Escalation-free by construction — and the field is additive (existing callers
  zero-init their configs, so `pool = NULL` is automatic).
- model.c's process-wide loop thread precedent does not transfer: the streams loop has no
  per-actor state and its mount is a pin-count over stateless backends; a scheduler pool
  carries an actor registry and owns teardown rules.
- Spawned children inherit the parent's pool pointer exactly as they inherit the parent's
  model config (frame.c `_frame_alloc`'s inherit branch, extended with one line) — the
  pool is a property of the SUBTREE, so a tree is always on one pool.

Mechanics (all existing machinery — nothing new in Actor/Scheduler):

- `_frame_alloc` / `frame_resume` call `actor_init(&f->actor, f, _frame_behavior, pool)`
  with that pool (frame.h's config field is typed behind Actor/actor.h's own forward
  declaration of `scheduler_pool_t` — no new include edge).
- `actor_init(pool != NULL)` does the registry registration + pending-counter routing
  (actor.c:21-33); `actor_send` does queue-state transition + `scheduler_inject`
  (actor.c:217-228). So "start the child" is literally an `actor_send` of FRM_TURN.
- `frame_destroy` gains the pooled branch the style guide documents (5.5f family rule):
  when the embedded actor's pool is non-NULL, teardown goes through `actor_destroy`'s
  RUNNING/queue-state waits (after `scheduler_pool_stop` set `stopped`, the wait breaks
  out) instead of the inline detach path. Documented caller order: stop the pool, then
  destroy frames, then destroy the pool.

## 3. The model call rides the async transport (model.c)

`model_backend_t` gains ONE optional member; the existing `complete` is untouched (the
vtable's other consumers and all scripted tests keep compiling and passing):

```c
/* model.h additions (orchestration slice): after a rc==0 submit the sink fires
   EXACTLY ONCE, on the streams loop thread or synchronously within submit —
   OWNERSHIP of body/error transfers to the sink (heap; free() or consume). A
   rc != 0 return means rejected before any I/O: the sink will NEVER fire.
   submit must copy what it needs from messages/tools before it returns. */
typedef void (*model_response_sink_fn)(void* ctx, int status, char* body,
                                       size_t body_len, char* error);

typedef struct model_backend_t {
  int (*complete)(void* self, json_value_t* messages, json_value_t* tools,
                  char**, model_reply_t** reply, char** error_out);
  /* NULL = sync-only backend (scripted tests; the engine drains it inline) */
  int (*submit)(void* self, json_value_t* messages, json_value_t* tools,
                model_response_sink_fn on_done, void* on_done_ctx);
} model_backend_t;
```

The http backend (`_model_http_submit`) reuses model.c's existing machinery, one
POST-shaped piece added per layer:

- **Request build** unchanged (`_model_request_text` serializes/copies messages + tools
  BEFORE the return — the engine destroys its array right after submit).
- **Relay record** (tiny heap struct `{ model_response_sink_fn fn; void* ctx; }`): the
  http completion ctx is the relay; the loop-thread completion forwards
  `status/body/body_len/error` to `fn(ctx, …)` (ownership moves straight through) and
  frees the relay.
- **Client lifetime**: one `http_client_create` per POST as today, BUT destruction is
  deferred — the completion forwards, then `streams_loop_call(b->loop, …, client)` queues
  `http_client_destroy` as a later loop op (the client contract forbids destroy from
  inside its own completion). If the loop is already gone, the client leaks at shutdown —
  a dead reactor already aborts the process (loop_thread.h discipline), noted on the record.
- **Decode moves OFF the reactor**: the streams completion forwards the raw body; the
  frame's FRM_MODEL_RESULT behavior decodes it on the frame's own thread. model.c's
  `_model_http_complete` and the new engine share ONE exported helper
  (src/Frame/model_internal.h):

  ```c
  /* status+body+error → decoded reply, with model.c's exact error surface
     ("model client: HTTP %d: <body excerpt | transport reason | (no body)>").
     0 ok (reply_out owns the reply); nonzero (error_out owns the reason). */
  int _model_result_from_http(int status, const char* body, size_t body_len,
                              const char* transport_error,
                              model_reply_t** reply_out, char** error_out);
  ```

  `_model_http_complete` is refactored to consume the same helper — one error surface,
  two delivery modes, and test_model_decode keeps pinning the real text.

The worker-yield guarantee is now structural: a pool worker that handles FRM_TURN
submits and returns (µs-scale); the model's 60–150 s happens on the reactor thread; the
worker is free the whole time.

## 4. Termination, children, and accountability

**Every child termination is observable by its parent exactly once — no silent rot.**
The engine's terminal step (`_frame_engine_terminate(f, ok, text)`):

- engine ends (`engine_live = 0`, `phase = NONE`, `engine_failed` set).
- TOP frame failure: no status change — `frame_is_done` stays 0 (TestTurnLimitFailsLoud's
  pinned "the cap is a failure, not completion").
- CHILD (any terminal kind — report verb, quiet content end, or failure): if the frame is
  not already done, ONE `FRM_REPORT_BIND` to the parent's actor (§5): the parent composes
  ONE atomic batch (the child's `frame.report` event at the child's pre-allocated seq +
  the child's status→done + the bound report event) and the store actor's reply routes
  back to the CHILD, whose router posts ONE `FRM_CHILD_REPORT{child_sid, failed}` on the
  PARENT's mailbox: the parent's engine resumes only after the binding is CONFIRMED
  committed (stronger than racing it).

Consequent semantics changes (each a deliberate recut of loop.c's REPORT SEMANTICS
comment, which predates running children):

- **Quiet completion**: a child whose turn ends with assistant content and no report verb
  no longer dangles "running" — its content IS its outcome; the engine reports it. A
  child that ends content-only and keeps living was a hang waiting to happen once parents
  resume on child events (the old "dangling-join stragglers are the tree's business" note
  is superseded: THIS tree runs them).
- **Failed children are done**: a turn-limit/derive/model/cell-timeout failure marks the
  child done so a parent can never await a zombie; the failure text is what the parent's
  derive shows (the accountability surface), and the child's own log carries the control
  event with the exact kind.
- **Parent resumption**: FRM_CHILD_REPORT → `live_children--` → repost FRM_TURN → the
  parent re-derives (child report lines are in the system prompt) → next model turn "can
  resume what's left after the event". Each child report is exactly one resume — one
  report per child, parent context grows by exactly one report event per child
  (subdivide-session's first assertion, now structurally true).
- **Enter the yield**: a content-only turn ending while `live_children > 0` does NOT end
  the frame — `phase = FRAME_PHASE_CHILDREN`, status stays "running", the engine goes
  quiet until a child ends. (A parent that spawns and stops asking the model is exactly
  "ends its turn awaiting children".)
- **`frame_join` on the resume path**: the parent's FRM_CHILD_REPORT behavior folds the
  child's bookkeeping INTO the resume dispatch (one log line + the rescheduled turn), so
  join stops being a separately-driven call in the event-driven flow. The public
  `frame_join` stays for direct-API callers and is idempotent-safe at "joined" (a second
  join for the same child sid is a loud no-op).

**Restart limit (recorded, not fixed here):** `live_children` is in-memory; a frame
resumed between spawn and child-done carries no live-children count — its engine (if
restarted) ends on its next content turn and any disk-"running" children stay
unresumed. The restart/reconcile slice (re-linking lineage parents) owns it.

## 5. The store actor: WaveDB behind one mailbox (owner amendment, supersedes the per-frame write lock)

**Amendment (owner, 2026-09-30, verbatim):** *"this suggests we need to wrap wavedb in an
actor therefore serializing writes and reads and eliminating the need for locks."* The
original §5 (one `platform_mutex_t* write_lock` per frame, taken in lineage order) is
SUPERSEDED and its consequences were reviewed and approved by the owner in session.

**Decision: `wave_database_root_t` becomes an actor-owned store.** Its behavior executes
store operations — batches, the bounded events scan, the recall resolve walk — ONE
message at a time on the store actor's own thread (a pool worker, or the driver's thread
for the inline shape). Serialization comes from being ONE actor: no
platform_mutex/rwlock/barrier anywhere in the frame layer, no lock-order discipline to
document or deadlock-audit.

Structure (`wave_database_root_t`, still opaque in frame.h, defined in frame.c):

```c
struct wave_database_root_t {
  actor_t store_actor;        /* FIRST member (house rule: actor states lead with actor_t) */
  database_t* db;             /* the ONE root database */
  graph_layer_t* lineage;     /* subtree-mode graph layer over lineage_st */
  database_subtree_t* lineage_st;  /* reserved "lineage" subtree (open for life) */
  uint32_t rng;               /* xorshift32 state for sid randomness */
  ATOMIC(uint64_t) counter;   /* sid uniqueness counter */
  scheduler_pool_t* store_pool;  /* BORROWED; NULL = inline (tests/demo pump by hand) */
};
```

It is not refcounted (it is a process-wide root with explicit open/close), so the
refcounter-first rule does not apply; the actor-first rule does, and is met by
`store_actor` as the first member. `wave_db_open` becomes a thin wrapper over the new
`wave_db_open_config` (location + store pool) — the store actor's init/destroy IS the
`wave_db_open/close` lifecycle: `wave_db_open_config` runs today's existing
`database_create_with_config` shape (same config, `sync_only=0`, fail-loud on failure)
unchanged, then `actor_init(&root->store_actor, root, _store_behavior, store_pool)`;
`wave_db_close` runs `actor_destroy(&root->store_actor)` first (queues retire pending
payloads; the IDLE/RUNNING waits break out for inline and stopped pools), then the
existing graph/subtree/db teardown. The dual-driver rule applies to the store actor too:
NULL pool = inline, hand-pumped by tests/demos (`wave_db_pump`, which refuses loud on a
pooled store); a producer pool is carried/borrowed exactly like the frame pool of §2.

### Message contracts (frame_messages.h owns the payload structs + destroyers)

```c
typedef struct frm_store_op_t { char* key; uint8_t* value; size_t value_len; } frm_store_op_t;
/* one put; key/value heap, OWNERSHIP transfers with the payload */

typedef struct frm_store_batch_payload_t {
  frm_store_op_t* ops; size_t nops;   /* OWNED; consumed by the store behavior */
  const char* op_name;                /* BORROWED label for the store worker's loud log */
  actor_t* reply_to;                  /* BORROWED; NULL = fire-and-post (audit/control
                                         writes whose commitment nothing waits on) */
  uint64_t corr;                      /* the requester's round-trip key (0 = fire-and-post) */
} frm_store_batch_payload_t;

typedef struct frm_store_scan_payload_t {
  char* start; char* end;             /* OWNED absolute root-level bounds */
  size_t limit;                       /* newest-record cap (SA_FRAME_DEBUG_MAX_EVENTS) */
  actor_t* reply_to; uint64_t corr;
} frm_store_scan_payload_t;

typedef struct frm_store_recall_payload_t {
  char* key; char* sid_path;          /* OWNED; the resolve walk's start */
  unsigned max_hops;                  /* the frame's depth budget */
  actor_t* reply_to; uint64_t corr;
} frm_store_recall_payload_t;

/* The round-trip result (store -> requester; the ROUTER on the requester's
   actor decides what to do with it). rc = 0 committed / the store's refusal
   code. `records` carries the MATERIALIZED RAW record texts (heap, ascending
   seq order) for scans and the recall-walk resolution (n == 1 or 0); batch
   replies carry n == 0. The store worker does NOT parse JSON — payload
   decision (resolved from the code): the derive's records are raw texts, the
   FRM_STORE_REPLY router on the frame parses each (µs-scale, bounded 512)
   into the DOM the projection already consumes. Raw texts are honest (the
   store deals in store values, not app JSON) and the cheap side of the
   parse/serialize trade (serializing a DOM into an array on the store worker
   would parse-then-serialize every record; raw copy is one memcpy per
   record). Unparseable raw records are dropped LOUD by the router. */
typedef struct frm_store_reply_payload_t {
  uint64_t corr; int rc; size_t n; char** records;   /* records; OWNED */
} frm_store_reply_payload_t;

/* child frame actor -> parent frame actor: COMPOSE THE CROSS-SUBTREE REPORT
   BATCH THERE (the parent's actor pre-allocates the parent's seq — see the
   seq discipline below). All heap strings OWNED by the payload.
   engine_driven (the terminate path) routes the FRM_CHILD_REPORT resume. */
typedef struct frm_report_bind_payload_t {
  actor_t* reply_to;         /* BORROWED: the CHILD's actor gets the store reply */
  uint64_t corr;             /* the child's own store round-trip key */
  uint64_t bridge_corr;      /* the cell's bridge corr, or 0 when not cell-side */
  uint8_t engine_driven;     /* 1 = the child posts FRM_CHILD_REPORT on the reply */
  char* child_sid;           /* owned */
  uint64_t child_seq;        /* the child's pre-allocated seq */
  char* child_event_text;    /* owned; the child's OWN frame.report record JSON */
  char* text;                /* owned; the report text (the parent re-composes its bound event) */
} frm_report_bind_payload_t;
```

### Seq discipline (the lock's replacement)

Every `f->seq` stays in its own frame actor, single-threaded — no lock, no atomic:

- **Seq numbers are PRE-ALLOCATED at compose time** (the frame reads `f->seq`, allocates
  `seq = f->seq + 1`, and writes `f->seq = seq` on its own actor thread — this is the
  owner-approved "the originating frame actor(s) pre-allocate their seq counters").
- The composing frame of a cross-subtree effect allocates ONLY its own seq: in a report
  bind, the CHILD's actor allocates `child_seq` and composes the child's own record; the
  PARENT's actor (receiving `FRM_REPORT_BIND`) allocates the parent's seq and composes the
  WHOLE batch (child's record + child status=done + the parent's bound event) before
  posting `FRM_STORE_BATCH` with `reply_to =` the child's actor. One composer per batch;
  every seq allocated in its own actor; the batch is ONE atomic root
  `database_batch_sync_raw` — wave_spawn admission, join, remember, events, status writes
  that span a fresh child subtree stay composed by the (single-live-writer) spawning
  frame exactly as today.
- **On a store rejection** the allocated numbers are abandoned: the frame's reply handler
  attempts a best-effort ROLL-BACK (`if (f->seq == abandoned) f->seq = abandoned - 1;` —
  exact when the frame was single-flight, which restores today's no-gap discipline
  whenever there is no concurrent compose) and logs loud when a gap must stay. No store
  state half-applies (the batch is atomic); only the counter may carry a gap, never a
  wrong key.
- Structural single-flight: the engine is one turn step at a time (one awaited batch per
  phase change); bridge verbs (remember/recall) and PYRT_RESULT's cell.result post
  corr-scoped batches that do not await — FIFO at the store means a frame's composes never
  reuse a seq: bump-at-compose handles even overlapped batches from one frame's dispatches.

### Reads

- **The derive's bounded events scan and the recall resolve walk ARE store messages**
  (`FRM_STORE_SCAN` / `FRM_STORE_RECALL`) — serialized reads, FIFO-ordered with the
  writes, so a derive can never observe an uncommitted-then-reordered future. The
  from-cells remember/recall round trips ride the EXISTING py-agent bridge wait pattern
  (bounded pure-drain wait on the pyrt thread) — the store actor is their reply source
  now: the frame's behavior composes and posts the store message INSTEAD of answering
  synchronously, and the `FRM_STORE_REPLY` router answers the corr through the bridge sink.
- Two DELIBERATE carve-outs, documented as such: `_frame_restore_seq` (frame_resume boot,
  before any engine on that frame exists) and `frame_debug_events` (test/debug accessor,
  called between runs) keep their direct root-level scans — both are WaveDB
  concurrent-mode-shaped reads, never an engine's causal read. `frame_is_done` keeps its
  direct meta/status read: the engine's use is advisory (its causal truth is the
  store-serialized derive; a store-FIFO derive always sees the report a `frame_is_done`
  check could lag behind), and the public API keeps its sync contract.
- **The synchronous public API (`frame_recall` / `frame_remember_local` /
  `frame_remember_ctx` / `frame_append_msg` / `frame_spawn` / `frame_report` /
  `frame_join`) stays synchronous by PUMPING, not by locking:** on an inline store actor
  the direct caller composes, posts, and pump-waits (bounded, corr-matched, the same
  shape `_frame_cell_wait` runs) — tests keep every existing call site unchanged. On a
  POOLED store the synchronous API refuses loud: production's pooled store is actor-paced,
  and mixing an arbitrary caller's thread into pool-driven serialization would re-open the
  lock question. A pooled FRAME therefore REQUIRES a pooled store — enforced loud at
  `frame_create`/`frame_resume` (a pooled frame whose root store is inline could post into
  a mailbox nobody pumps, i.e. a hang posing as an API call).

## 6. The synchronous driver keeps working (compat contract)

`frame_run_loop` stays the caller-thread driver — reimplemented over the SAME engine
(`frame_start` + a bounded pump of `actor_run`:

```c
int frame_run_loop(frame_t* f);
/* 0 = clean end-of-run; 1 = failed loud (engine_failed); 2 = yielded awaiting
   children (live); the pump deadlines: FRAME_PHASE_CELL stays
   SA_LOOP_CELL_WAIT_MS (60000), FRAME_PHASE_MODEL gains SA_LOOP_MODEL_WAIT_MS
   (300000 — a config-slow model's own timeout + slack must fit under it),
   FRAME_PHASE_STORE gains SA_LOOP_STORE_WAIT_MS (30000 — a store round trip
   is µs–ms; a breaking deadline is a loud store stall, never a hang). */
```

Contract: existing scripted (sync) backends drive the same engine inline — every current
test_loop test keeps its assertions byte-for-byte; the live gate/demo CLI keep calling
`frame_run_loop` (its pump sleeps 1 ms between mailbox checks like `_frame_cell_wait`
does today; the model call no longer blocks inside a recv, it arrives as a message).
Production event-driven runs use `frame_start` on pooled frames instead (Task 5's tree
tests) — `frame_run_loop` on a pooled frame is legal (it pumps; workers race it harmlessly
via the mailbox) but pointless; the plan's tree tests use `frame_start` +
`scheduler_pool_wait_for_idle`.

Known asymmetry, recorded honestly: the POOLED engine has no cell watchdog (a hung pyrt
cell hangs that frame's turn indefinitely) — the sync driver's SA_LOOP_CELL_WAIT_MS
deadline cannot exist without a frame-side timer facility (out of scope; the desktop
slice's interrupt story covers hung cells with interrupt). Escalated below.

## 7. Atlas folding (part of this slice's evidence task)

- **`remember-session-across-restarts`** closes EVIDENCE-ONLY: its required proof already
  exists (TestLoop.TestRestartReplayRestoresSeqAndContext + the live gate's disk
  persistence). workflow.json's node gains the evidence note (kill-and-resume proof test,
  the WaveDB concurrent-mode same-session visibility defect it documents, the third-session
  assertion shape) and its completionEvidence is updated to say the Windows bullet is
  pending-verification; status moves to `completed` only if `node validate.js` accepts
  without a review record — otherwise it stays `in-progress` with the evidence text
  (validator check recorded, escalated honestly).
- **`subdivide-session-into-accountable-agents`**'s verification folds into this plan's
  acceptance criteria (Task 6 asserts at tree scale: one report per child regardless of
  child work volume; the tree derives from lineage pointers; no #819 scoping blind spot —
  children see inherited ctx only through the resolve walk).

## Explicit non-goals

- Anything beyond the existing bridge verbs on the surface; REST routes (desktop slice).
- Windows verification (pending elsewhere, as recorded on the atlas nodes).
- L5 persona / escalation.
- A process-default scheduler pool (Section 2 — escalated deliberately).
- Frame-side timer/watchdog facilities for pooled cells (Section 6 — escalated).

## Escalations for the owner

1. **Quiet-completion reports for children** — RESOLVED in this slice's plan (the owner
   approved the event-driven resume shape; Tasks 3/5/6 implement the content-end report +
   resume). Recorded here rather than silently dropped: a child that answers content and
   never calls actor.report reports its content implicitly.
2. **No process-default pool** — still open/deliberate: every embedding caller that wants
   event-driven frames creates a scheduler pool and hands it in via `frame_config_t` and
   `wave_db_open_config` (the store pool rides the same caller decision). If the desktop
   slice wants defaults, they are additive later fields.
3. **Pooled engine has no cell watchdog** — still open (a hung cell hangs that frame's
   turn, silently — loudly logged only at frame_destroy). Accept for this slice, or pull
   interrupt wiring forward?
4. **Atlas validator completion lifecycle** — recorded in §7: `completed` only if the
   validator accepts without a review record; otherwise the node stays `in-progress` with
   its evidence text and the validator gate noted — never a faked review record.

Additional consequences of §5's amendment the owner should glance at (recorded decisions,
not blockers): pre-allocated seq numbers may leave a key GAP in `events/` after a store
rejection (best-effort roll-back collapses the single-flight case; never a wrong key);
the synchronous frame API (`frame_recall`, `frame_spawn`, …) requires an inline store
actor and refuses loud against a pooled one.

## Risks

- The engine rewrite touches every test_loop test's harness indirectly (dispatch order
  changes; the audit trail content must not): the plan re-runs the scripted suite at
  every step first.
- actor_send from the streams loop thread under backpressure has mute paths (a fast model
  + slow parent can pressure a mailbox): the engine's re-posts are one-message-per-arrival,
  bounded by the model's own turn cadence — no runaway queuing to flag beyond a note.
- Store round trips add dispatch hops to the sync paths tests exercise (a bridge verb's
  answer is now store-reply-routed): every inline driver (frame_run_loop,
  `_frame_cell_wait`, the direct pump-waits) pumps frame, then live ancestors, then the
  inline store actor in ONE cycle, so round trips stay completion-in-one-pump — the plan's
  tests re-run at every step first.
- Bounded-wait consumers (py_agent's SA_PY_AGENT_WAIT_MS, the engine's phase deadlines,
  the direct sync API's pump) must each cover the store round trip's µs–ms latency — the
  existing bounds (30 s+, minutes elsewhere) dwarf it; a store WAL stall shows up as a
  loud deadline break, not a hang, and the store worker's own log carries the refusal.