//
// Created by victor on 9/29/26.
//

#ifndef SA_FRAME_INTERNAL_H
#define SA_FRAME_INTERNAL_H

/* Module-PRIVATE contract between frame.c and loop.c (Task 10): the handful
   of frame-side helpers the turn loop leans on. Everything here is
   underscore-prefixed (style guide) and never part of the public frame.h
   surface; tests reach the public store API, frame_set_model_backend and
   frame_set_loop_turn_cap instead.

   THE THREADING MODEL (the loop-slice's documented decision): the turn loop
   runs ON THE CALLER'S THREAD (in production the frame's own scheduler
   worker; in tests the test thread — the frame actor is inline, pool NULL,
   and pumped by hand). The pyrt worker thread is the only OTHER thread in
   the picture: it runs the cells and posts its results back through the
   frame actor's mailbox. Every frame-side dispatch (bridge verbs answering
   the cell's agent.* calls, PYRT_RESULT arriving, FRM_REPORT / FRM_SPAWN)
   runs inside those pump dispatches on the loop thread. A frame must
   therefore be driven by ONE loop at a time and be inline (NULL pool) for
   this slice; scheduler-pooled frame dispatch would re-open the field-access
   discipline and is deliberately out of scope here. */

#include "frame.h"
#include "frame_messages.h"
#include "model.h"                    /* model_reply_t (the engine state's
                                         in-flight turn reply) */
#include "../Util/atomic_compat.h"    /* ATOMIC(T) — the lifetime handoff
                                         fields below */
#include "../Util/json.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SA_HAS_WDB

/* The loop's turn cap DEFAULT (the built-in fail-loud bound on model turns
   per frame_run_loop). It lives here because frame.c holds the effective-cap
   accessor and loop.c applies it — the two must agree on the number; the
   ifndef keeps a build able to set it with -DSA_LOOP_MAX_TURNS=N. */
#ifndef SA_LOOP_MAX_TURNS
#define SA_LOOP_MAX_TURNS 64
#endif

/* The event materialization bound (newest 512 records) — shared by the
   frame_debug_events scan, the store actor's scan replies, and the engine's
   FRM_STORE_SCAN (the derive's limit). */
#ifndef SA_FRAME_DEBUG_MAX_EVENTS
#define SA_FRAME_DEBUG_MAX_EVENTS 512
#endif

/* --- the turn engine (Task 3; the orchestration spec §1) -------------------

   The engine is a PHASE MACHINE carried on the frame: frame_start queues ONE
   FRM_TURN continuation; every step posts the next store round trip /
   message and returns (yields) — the step CONTINUES in the arrival dispatch
   (the store replies route from frame.c's FRM_STORE_REPLY router, the model
   completion arrives as FRM_MODEL_RESULT, the cell's PYRT_RESULT reposts the
   continuation). The frame's own actor IS the engine's runner: its owner (a
   scheduler pool worker in production; the synchronous test driver's bounded
   pump) picks it up on every arrival. NO lock and NO blocking wait exists on
   the engine's path. */

/* The turn engine's phases (spec §1): the frame yields between all of them. */
typedef enum frame_phase_e {
  FRAME_PHASE_NONE = 0,     /* between steps / engine not started */
  FRAME_PHASE_MODEL,        /* an async model submit is in flight */
  FRAME_PHASE_CELL,         /* the turn's one cell is in flight */
  FRAME_PHASE_STORE,        /* a store round trip is in flight (Task 2's vocabulary) */
  FRAME_PHASE_CHILDREN,     /* yielded: live children pending; each child
                               report's FRM_CHILD_REPORT reposts the turn */
  FRAME_PHASE_ASK           /* parked: the closed turn's ask awaits the
                               owner's reply (escalation spec §1.3) —
                               frame_ask_reply's FRM_ASK_REPLY clears the
                               park and reposts the turn; nothing blocks,
                               nothing spins */
} frame_phase_e;

/* What the pending FRAME_PHASE_STORE round trip is for (the engine state's
   store_kind): each kind's reply continues the turn inside the reply
   dispatch itself. */
typedef enum frame_store_kind_e {
  FRAME_STORE_DERIVE = 1,   /* the bounded events scan; the reply parses the records
                               and runs the projection + submit/reply processing */
  FRAME_STORE_CELL_RUN,     /* the cell.run audit commit; the reply dispatches
                               FRM_CELL_EXECUTE (no untracked cell ever runs) */
  FRAME_STORE_FINISH,       /* msg.append (+ meta/status=done for a top frame) in ONE
                               batch; the reply ends the engine (top) or drives the
                               child's quiet-completion report bind (child) */
  FRAME_STORE_PERSONA       /* the named persona read (persona spec §3): the
                               FRM_STORE_GET_NAMED direct read of
                               personas/<name>/{record,user-context}; the reply
                               composes the persona GROUP into the engine's
                               persona_prefix and continues to the model path
                               (a scan refusal / absent record = the built-in
                               base only, loud — a persona is presentation, a
                               turn NEVER fails on a persona read) */
} frame_store_kind_e;

/* The turn engine's STATE — embedded in frame_t (frame.c) as ONE of its
   members; every access (`_frame_engine_state`) is on the frame's OWN
   dispatch thread (mailbox dispatches are the single writer — the same
   single-runner discipline that owns the cell slot). */
typedef struct frame_engine_state_t {
  uint8_t engine_live;         /* 1 between frame_start and a terminal step */
  frame_phase_e phase;
  frame_store_kind_e store_kind;  /* routing of the pending FRAME_PHASE_STORE reply */
  uint64_t store_corr;         /* the pending store round trip's corr (the
                                  frame's OWN round-trip key space, shared with
                                  the bridge registry — one counter, no
                                  collisions; never 0, 0 = none) */
  uint64_t turns_issued;       /* model turns issued THIS RUN (cap check at
                                  frame_start's reset — a per-run bound) */
  uint8_t model_retries;       /* consecutive failed-model calls: reset on each
                                  success; a retry is ONE fresh FRM_TURN repost */
  uint8_t model_retry_step;    /* 1 = the queued FRM_TURN is a model RETRY
                                  (skips the checks + the turn count, exactly
                                  like the old loop's same-array retry) */
  uint8_t engine_failed;       /* the last terminal step failed (frame_run_loop's rc) */
  size_t live_children;        /* children admitted-and-STARTED, not yet resumed
                                  (Task 5; single-writer: the frame's dispatch
                                  thread; in-memory bookkeeping of THIS
                                  process's spawns — a restarted engine starts
                                  at 0, spec's recorded restart limit) */
  uint64_t turn_cell_corr;     /* the loop's audit corr of the cell the
                                  pending cell.run round trip committed (the
                                  paired refusal result carries it) */
  model_reply_t* turn_reply;   /* the in-flight turn's model reply, OWNED by
                                  the engine between the model arrival and its
                                  path's dispatch turn (the CELL_RUN reply
                                  runs the cell out of it) */
  char* finish_text;           /* the content path's OUTCOME text, OWNED: it
                                  rides the engine state to the finish
                                  reply's end rule — the CHILDREN yield, the
                                  child's quiet-completion report bind, or
                                  the top end consume/free it there */

  /* --- the derive's persona GROUP (persona spec §3; the persona slice's
     Task 4) ---------------------------------------------------------------

     derive_events: the parse events DOM of the derive whose continuation is
     parked at the persona read, OWNED by the engine for exactly the
     persona-trip window (the derive reply stashes it, the persona reply's
     projection consumes it, or the engine end frees it — the finish_text
     lifetime rule: a DEAD engine never carries it across a restart).

     persona_prefix: the COMPOSED persona GROUP (persona block + context
     block + attached guidance + the base instructions ride the compose's
     placement), byte-stable per inputs — the system prompt's FIRST block
     (the cache-stable prefix, spec §2). OWNED; rebuilt per derive (the
     records are state and can change between turns; the compose re-runs
     every derive), freed with the engine. NULL = no persona rides (the
     built-in base alone — today's byte-identical shape). */
  json_value_t* derive_events; /* the parked derive DOM across the persona
                                  trip (NULL outside that window) */
  char* persona_prefix;        /* the composed persona block's bytes, or
                                  NULL for none */

  /* --- the DOOM-LOOP breaker's streak (guards spec §1) -------------------

     Dispatch-thread domain (the engine state's single-runner discipline —
     no atomics): written only in the tool path's fold and the derive's
     user-seq scan. A DEAD engine never carries the streak across a restart
     (_loop_engine_end clears it — the next run's re-derive relearns the
     input facts from the log). */
  uint8_t doom_streak;         /* consecutive byte-identical tool calls (the
                                  guards module's fold owns the semantics) */
  char* doom_last_code;        /* the last DISPATCHED cell's code, OWNED
                                  (freed/replaced each tool path; freed with
                                  the engine — mirror finish_text) */
  uint64_t users_seen_seq;     /* the newest user-role msg.append seq the
                                  derive has seen (the doom reset's input) */
  uint64_t cell_users_seq;     /* users_seen_seq AT the last cell dispatch */

  /* --- the turn-lifecycle envelope's bookkeeping (the turn-lifecycle
     slice's Task 2; spec §3) ------------------------------------------

     turn_counter: the number of the turn being COMPOSED. Restored ONCE per
     engine run from the derive's scanned events (the newest recorded turn
     number + 1; the _frame_restore_seq discipline — gaps logged loud and
     continued, never a lock, never a second writer), then +1 per entry. A
     DEAD engine never carries it across a restart: the restore reads the
     log again on the next run's first entry.

     turn_open / step_open: compose-time FACTS mirrored in memory (1 = the
     envelope's opener posted, its closer not yet) so the finish / failure /
     result paths know what their batch must carry. The store's records stay
     the truth; the flags drive only the riders' composition. */
  uint64_t turn_counter;       /* the current turn's number (restored +1'd) */
  uint8_t turn_known;          /* 0 until the run's first restore */
  uint8_t turn_open;           /* 1 = turn.start posted, no turn.end yet */
  uint8_t step_open;           /* 1 = step.start posted, no step.end yet */

  /* --- the blocked-ask's park (escalation spec §1.3/§1.4; escalation Task
     2) --------------------------------------------------------------------

     The ask verb publishes FRM_ASK mid-cell (fire-and-post); the engine
     mints the ask_id and OWNS the question/options here until the cell's
     completion composes the close batch (cell.result + step.end + the "ask"
     record + turn.end{blocked}) — then the engine rests in
     FRAME_PHASE_ASK while the owner composes. The state is IN-MEMORY only
     (like the parked model submit): durability lives in the records, so a
     DEAD engine never carries it — every engine-end site clears through
     _frame_engine_ask_clear (the same discipline as doom_last_code). The
     pyrt "ask_parked" publish flag is NOT this struct's: it is set at the
     verb's post-publish and cleared at the engine's consume paths
     (_frame_engine_ask_clear included); the engine never sets it.

     plan_gate: 1 = the ladder's runtime-authored plan-gate ask (the
     approval flow's owner); Task 5's consumer — always 0 in this slice. */
  struct {
    char* ask_id;              /* the engine-minted "%08x" key, OWNED (NULL =
                                  no park) */
    uint64_t corr;             /* the publishing verb's bridge corr (the
                                  reply-sink key space; informational here) */
    char* question;            /* the ask's text, OWNED (stolen from the
                                  FRM_ASK payload at receipt; freed by
                                  _frame_engine_ask_clear — the close batch
                                  borrows it into the record) */
    char** options;            /* the OWNED array of OWNED strings (NULL =
                                  no options — an open ask; the close's
                                  compose renders an empty array) */
    size_t noptions;
    uint8_t plan_gate;         /* 0 = a generic agent.ask; 1 = the ladder's
                                  plan gate (Task 5) */
  } pending_ask;

  /* --- the async submit's LIFETIME HANDOFF (lock-free; atomics are
     house-legal — the frame layer stays lock-free post store-actor) ------

     The engine hands the bare frame_t* to the backend's sink (loop.c's
     _loop_model_sink via model.c's relay), and that sink may outlive BOTH
     the turn and the frame: frame_destroy mid-turn would free the record
     under a completion that has not fired yet. The submit step ACQUIRES one
     slot before submit(); the sink's completion RELEASES it; a destroy with
     a slot held marks die_requested and DEFERS its teardown to the LAST
     release (frame.c's claim protocol — the record stays alive until then,
     so every late touch of the record's own atomics is safe). The SYNC path
     never acquires: only a real in-flight submit counts. */
  ATOMIC(uint32_t) pending_submits;    /* acquire/release per async submit;
                                          frame_internal.h's teardown CLAIM
                                          lands here too (SA_ENGINE_SUBMIT_CLAIM) */
  ATOMIC(uint8_t) die_requested;       /* 1 = a frame_destroy ran (or defers)
                                          mid-turn; the engine must not repost
                                          into the dead mailbox */
  ATOMIC(uint8_t) submit_inflight;     /* 1 while the engine thread is INSIDE
                                          submit(): a sink firing synchronously
                                          there stands down its teardown — the
                                          engine stays the record's owner until
                                          it settles (frame.c) */
  ATOMIC(uint8_t) continuation_queued; /* 1 = a FRM_TURN continuation is
                                          already queued (the wake posts and
                                          the retry backoff's arm set it; the
                                          FRM_TURN dispatch's entry and
                                          _loop_engine_end clear it). The wake
                                          latches read it to tell a TRUE rest
                                          at phase NONE from the TRANSIENT
                                          gap between a yield and the queued
                                          continuation's dispatch — a steer
                                          posting there would duplicate the
                                          FRM_TURN and, at a later NONE
                                          window, buy an extra turn step past
                                          the turn cap's gate. */
} frame_engine_state_t;

/* The teardown CLAIM marker inside pending_submits: exactly one agent (the
   destroy that found no slot held, or the sink's last release on a
   die-requested frame — the CAS in frame.c arbitrates) ever holds it, and
   that agent runs _frame_destroy_run. */
#define SA_ENGINE_SUBMIT_CLAIM UINT32_MAX

/* The engine state's accessor (NULL on a dead/unknown frame). */
frame_engine_state_t* _frame_engine_state(frame_t* f);

/* The interrupt synthesis (frame.c; surface-completion spec §2): the
   corr-matched close + the runtime poison + the boundary cut, under the
   caller's reason wording. arm_cut = 1 means a TRUE interrupt (the boundary
   cut arms); 0 = a deadline (never arms). FRM_INT's dispatch, the inline
   driver's cell deadline, and the pooled watchdog's dispatch (Task 6) are
   its callers. */
void _frame_interrupt_apply(frame_t* f, uint8_t arm_cut, const char* reason_text);

/* The POSTED steer (the client-api handlers' shape; client-api spec §3):
   ONE FRM_STEER {role, text} into the frame's own mailbox; the frame's
   dispatch composes the durable msg.append fire-and-post there (the seq's
   single writer is the frame's thread — the handler on the loop thread
   could never pre-allocate one honestly). This is the thread-legal shape of
   a steer from OUTSIDE the frame's thread: frame_append_msg is a
   synchronous store API (pump-wait, inline-store-only) and refuses loud on
   the real server's pooled store. Legal from ANY thread (frame_interrupt's
   posted precedent). Returns 0 once POSTED — never a commit confirmation
   (the store's FIFO anchors the causality; the caller's response answers
   QUEUED, not committed) — nonzero loud on the pre-post refusals (a dead
   frame, NULL fields, OOM). */
int _frame_steer_post(frame_t* f, const char* role, const char* content);

/* The synchronous cell refusal's PAIRED cell.result text (frame.c's poison
   knowledge; the ENGINE's refusal composer in loop.c reads it): NULL = the
   generic refusal wording applies; the poisoned runtime's own wording
   otherwise — the refusal is CORR-MATCHED failure data the model reads
   (surface-completion spec §2's poison contract, quiet late drop + loud
   future-cell refusal). */
const char* _frame_cell_refusal_text(const frame_t* f);

/* The die-requested flag's atomic read (loop.c's model sink + completion
   handler gate against a dying frame): 0 = clear, 1 = a frame_destroy ran
   (or defers) mid-turn. 0 for a NULL frame. */
uint8_t _frame_engine_die_requested(const frame_t* f);

/* The retry backoff's ONE-SHOT delayed post (frame.c implements; guards
   spec §3 — the loop's retry branch calls it after each backoff'd model
   failure): spawn one short-lived JOINABLE thread that sleeps delay_ms,
   posts the FRM_TURN continuation into the frame's own mailbox (a dying
   frame's expiry drops loud), and quits. The thread is reaped at the
   FRM_TURN dispatch's head (its own post IS that dispatch) and in
   frame_destroy FIRST (join-before-teardown keeps the thread's die-check
   provable against live frame memory). Returns 0 armed, -1 refused — the
   caller then posts immediately (the bounded wait is an optimization;
   never a correctness dependency). delay_ms is the table's backoff; the
   caller posts DIRECTLY on a 0 backoff (today's shape). */
int _frame_delayed_post(frame_t* f, uint32_t delay_ms);

/* Engine handlers (loop.c implements, frame.c's _frame_behavior routes): */

/* Start the engine: refuses (-1, loud) on a dead frame or an engine already
   live; else resets the per-run knobs (turns_issued, model_retries,
   engine_failed), sets engine_live = 1, and posts ONE FRM_TURN continuation. */
int _frame_engine_start(frame_t* f);

/* The FRM_TURN behavior: ONE turn step (checks → the derive store round trip
   → on its reply the submit/complete + tool/content paths), then yields — it
   returns without blocking on anything. */
void _frame_engine_turn(frame_t* f);

/* The FRM_STORE_REPLY branch for engine round trips (frame.c routes here when
   phase == FRAME_PHASE_STORE and the corr matches store_corr): DERIVE → parse
   the reply's records into the DOM (µs, bounded 512, unparseable dropped loud)
   and run the projection + backend resolve; CELL_RUN → rc != 0 = control
   "audit-error" + terminate, rc == 0 = dispatch FRM_CELL_EXECUTE; FINISH →
   end the engine (top) / drive the child's report bind (child);
   PERSONA → compose the persona GROUP into persona_prefix (the fallback =
   the base alone, loud) and continue to the model path with the parked
   derive DOM. CONSUMES the payload on every path. */
void _frame_engine_store_reply(frame_t* f, frm_store_reply_payload_t* payload);

/* The FRM_MODEL_RESULT behavior: decode the raw completion (model.c's error
   surface; Task 4 moves the shared decode into model_internal.h) + process
   the reply; a decode/model error retries EXACTLY ONCE (model_retries) then
   terminates failed. CONSUMES the payload on every path. */
void _frame_engine_model_arrived(frame_t* f, frm_model_payload_t* payload);

/* Called by frame.c's PYRT_RESULT handler AFTER the cell slot completed:
   engine_live && phase == CELL → repost the FRM_TURN continuation.
   (Synchronous cell refusals — pending never set — let the FRM_CELL_RUN reply
   step resume; see _frame_engine_store_reply's CELL_RUN path.) */
void _frame_engine_cell_done(frame_t* f);

/* End the live engine (loop.c implements; the failure surfaces AND
   frame.c's interrupt synthesis call it): ok=0 → engine_failed + a CHILD's
   failure report bind (the parent's derive shows it; a TOP frame just
   ends — no status change), ok=1 → the clean terminal. Re-entered on an
   already-ended engine: a loud no-op. Frame.c's interrupt synthesis calls
   this AFTER its synthesized close POSTED — the store's FIFO commits the
   close ahead of the terminate's bind. */
void _frame_engine_terminate(frame_t* f, uint8_t ok, const char* text);

/* The FRM_CHILD_REPORT behavior (the parent's resume, spec §4): decrement the
   engine's live_children liveness counter (a zero-count delivery logs loud),
   fold the child's frame.join into this dispatch as a fire-and-post store
   batch, and — resume only a live engine — repost the FRM_TURN continuation
   (the re-derive reads the bound report event the store committed BEFORE the
   report message was posted). CONSUMES the payload on every path. */
void _frame_engine_child_report(frame_t* f, frm_child_report_payload_t* payload);

/* --- the async submit's lifetime handoff (frame.c implements; loop.c's
   submit step + model sink call them) ------------------------------------- */

/* Acquire ONE pending-submit slot + mark the engine's submit as in flight.
   The engine calls this BEFORE handing the derived messages to a backend's
   submit() — the sink's completion may outlive both the turn and
   frame_destroy itself. */
void _frame_engine_submit_begin(frame_t* f);

/* Release ONE pending-submit slot (the sink's completion, or the engine's
   own submit-reject path where the sink will never fire). Returns 1 when
   THIS release was last on a die-requested frame and it ran the deferred
   teardown inside itself: the frame record is GONE and the caller must not
   touch the frame (or anything borrowed from it) again. 0 otherwise.
   (A release while the engine thread is inside submit() — a synchronous
   sink fire — only unbinds the slot; the ENGINE settles the record, see
   _frame_engine_submit_settle.) */
uint8_t _frame_engine_submit_release(frame_t* f);

/* The engine's post-submit settle (loop.c calls right after submit returns
   and the derived messages are destroyed): clears the in-flight marker, and
   on a die-requested frame whose slots are all back (the synchronous-fire
   sink stood down) the ENGINE is the record's last owner — this claims +
   runs the deferred teardown. Returns 1 when the record is GONE (the caller
   must not touch f again); 0 = slots still out or the normal non-die flow —
   the call falls through to the ordinary turn step (the die gate refuses
   every later repost into the dead mailbox). */
uint8_t _frame_engine_submit_settle(frame_t* f);

/* The sync driver's pump deadlines (the cell deadline SA_LOOP_CELL_WAIT_MS
   60000 already lives in loop.c): a config-slow model's own timeout + slack
   must fit under SA_LOOP_MODEL_WAIT_MS; a store round trip is µs–ms, so its
   deadline break is a loud stall, never a hang. */
#ifndef SA_LOOP_MODEL_WAIT_MS
#define SA_LOOP_MODEL_WAIT_MS 300000
#endif
#ifndef SA_LOOP_STORE_WAIT_MS
#define SA_LOOP_STORE_WAIT_MS 30000
#endif

/* Backend resolution: the injected override (frame_set_model_backend) wins;
   otherwise the DEFAULT is built ONCE (at first use, from the frame's own
   config) and stored. Once built, the default is never re-built, and a later
   override still wins over it. */
model_backend_t* _frame_backend_get(frame_t* f);

/* The frame's goal text (borrowed; NULL when the frame carries none). */
const char* _frame_goal(const frame_t* f);

/* The frame's carried persona record key (NULL = the built-in base; the
   derive's persona trip keys off it — persona spec §3). */
const char* _frame_persona_name(const frame_t* f);

/* The frame's carried escalation ladder mode (frame_escalation_mode_e;
   0 = free — escalation spec §2). Also the tests' config-copy seam (the
   _frame_persona_name precedent). */
unsigned _frame_escalation_mode(const frame_t* f);

/* 1 while the frame is live (open subtree); 0 for dead/unknown frames. */
uint8_t _frame_is_live(const frame_t* f);

/* 1 when a FRM_STOP has been requested and not yet drained by the loop. */
uint8_t _frame_stop_requested(const frame_t* f);

/* The effective turn cap: the frame's own override when set, else
   SA_LOOP_MAX_TURNS. */
unsigned _frame_loop_turn_cap(const frame_t* f);

/* One raw event record write, SYNC-SEMANTICS (the direct sync APIs' shape:
   sync corr + pump-wait) — the ENGINE never waits on this (its writes ride
   fire-and-post + the awaited store round trips): {"seq","type","frame",
   "corr","at","cause","payload"} at the next seq, ONE root batch, seq
   bumped only on success. CONSUMES the payload (on failure too — nothing is
   left for the caller). */
int _frame_event_write(frame_t* f, const char* type_name, json_value_t* payload);

/* Marks the frame done: meta/status = "done", ONE root batch. Used by the
   loop's end-turn (top frames end there) — report paths mark done inside
   their own report batches. */
int _frame_set_status_done(frame_t* f);

/* --- the refine slice's sync store helpers (the direct-sync rule) ---------

   Identical discipline to every other direct sync store call (the
   frame.c family `_frame_remember_sync` / `_frame_set_status_done` /
   frame_remember_local): refused LOUD on a POOLED store
   (_frame_sync_store_refused) — production reaches these effects through
   the actor paths; the single-flight sync slot carries the round trip;
   _frame_slot_wait pumps until the corr-matched reply or the
   SA_FRAME_STORE_WAIT_MS deadline. */

/* ONE FRM_STORE_SCAN round trip with the given ABSOLUTE ROOT-LEVEL composed
   bounds (never relative — the subtree-scan breakage) and the newest-records
   cap (0 = the store's window max; > the window max clamps). The reply's
   materialized records (ascending; the store worker's REVERSE-to-ascending
   shape) ride back as ONE malloc'd JSON-ARRAY text of the RAW record texts
   joined ("[]" when the range is empty) — the caller parses it whole.
   Returns 0 with *text_out set, nonzero loud otherwise (the store keeps the
   batch on a deadline: the same recorded consequence as the other syncs). */
int _frame_sync_scan(frame_t* f, const char* start, const char* end,
                     size_t cap, char** text_out);

/* ONE atomic root batch (refine.c composes; this owns the round trip).
   op ownership TRANSFERS into the round trip (TRUE ownership — frame.c's
   inline contract): past the post, its payload destroyer frees the ops
   array and every op's heap fields on EVERY path (success, store refusal,
   deadline, late routing, dropped post); on the helper's own pre-post
   refusals the helper frees them directly. The composer never frees an op
   itself. `op_name` is BORROWED (a literal or caller-owned string the
   round trip only logs). Returns 0 committed, the refusal code otherwise
   (loud either way; nothing half-committed — the store's ONE atomic
   batch). */
int _frame_sync_batch(frame_t* f, frm_store_op_t* ops, size_t nops,
                      const char* op_name);

/* 1 when the frame has a parent frame (i.e. is a spawned child). */
uint8_t _frame_is_child(const frame_t* f);

/* --- the client-API handlers' store contract (Task 4; client-api spec §3)

   The SESSIONS listing and the EVENT subscriptions live on the root's store
   actor (frame.c's _store_behavior):

   - FRM_STORE_LIST_SESSIONS {corr, reply_to}: the reply's records[] carry
     ONE heap JSON row per session, {"sid","status","goal","created",
     "depth"}; goal is the empty sentinel (no composer writes a meta/goal
     key — pinned in the listing case). The reply lands at the SERVER's
     actor; a FRAME actor receiving it hits the reply router's
     unmatched-corr loud drop (the standing contract).
   - FRM_STORE_WATCH / FRM_STORE_UNWATCH {sid_path, watcher}: the store's
     dispatch thread owns the subscription list (no locks); the watcher
     actor is BORROWED into every notice and an UNWATCH is the ONLY
     removal — a connection's teardown MUST send the unwatch before its
     actor dies (a dead watcher's notices keep posting and dropping loud
     until it arrives — frame_messages.h's recorded shape).
   - FRM_STORE_NOTICE {sid_path, seq, record_json}: ONE committed event
     record under the watched subtree; the store composes self-owned
     copies, so the payload outlives the batch. */

/* --- the pending-cell plumbing (FRM_CELL_EXECUTE <-> PYRT_RESULT) ----------

   State is ONE slot per frame: a cell runs while cell_pending == 1. The
   FRM_CELL_EXECUTE behavior fills the request half; the PYRT_RESULT behavior
   fills the completion half (status + text) AND clears the slot after
   writing the frame's cell.result event (so the audit event and the slot
   state move in the same dispatch). The loop's wait pumps the frame's inbox
   — that is where PYRT_RESULT arrives in the inline-actor design. */

/* 1 while FRM_CELL_EXECUTE has a cell in flight. */
uint8_t _frame_cell_pending(const frame_t* f);

/* Pump-and-wait for the loop's thread: runs the frame's inbox (actor_run)
   until the pending cell completed, bounded by the timeout. Returns 0 when
   the pending cell completed (status 0 ok / 1 failed in *status_out),
   nonzero when the deadline passed with the cell still in flight (the cell
   itself keeps running — nothing is abandoned, its completion is simply no
   longer awaited by the loop). */
int _frame_cell_wait(frame_t* f, unsigned timeout_ms, uint8_t* status_out);

/* The frame's embedded actor (the mailbox frame_start's FRM_TURN continuation
   queues into): the owner of an INLINE frame (pool NULL) pumps it by hand
   with actor_run — that is the whole inline contract; a pooled frame's
   mailbox belongs to its scheduler pool's workers. NULL on a dead/unknown
   frame. */
actor_t* _frame_actor(frame_t* f);

/* Bounded wait for the frame's synchronous store round trips (the
   _frame_cell_wait deadline family): a store round trip is µs–ms, so
   breaking this deadline is a loud stall, never a hang. */
#ifndef SA_FRAME_STORE_WAIT_MS
#define SA_FRAME_STORE_WAIT_MS 30000
#endif

/* ONE pump cycle over an inline frame's whole round-trip surface: the frame's
   mailbox, then each LIVE ANCESTOR's mailbox (child first — a bind request
   must reach the parent's actor and its composition), then — when the ROOT's
   store actor is inline — the store actor's mailbox. This is what makes every
   inline round trip (bridge verb -> store batch -> store reply -> answer)
   complete within one pump cycle, and it is the ONLY pump-order definition:
   _frame_cell_wait, the sync store API and the Task-3 driver pump all lean on
   it. Pooled surfaces (a pooled frame or a pooled store) are NEVER pumped
   here — scheduler workers own them; an inline frame's pooled ancestors (only
   possible via a mismatched frame_create cfg — the Task-2 guard refuses it)
   are skipped by the live-ancestor walk. */
void _frame_pump(frame_t* f);

/* --- the engine's frame-side helpers (frame.c implements; loop.c's handlers
   call them — §5's compose-and-post discipline, the frame's own dispatch
   thread is the single writer) ------------------------------------------- */

/* Post with an ownership handoff (the universal shape): on a DESTROY-flagged
   target the payload is destroyed and the drop logged loud; otherwise
   actor_send delivers (a busy mailbox is still a delivered message). */
void _frame_post(actor_t* target, uint32_t type, void* payload,
                 void (*destroy)(void*), const char* what);

/* The root's store actor (the ONE serializer): the engine's derive scans and
   every engine-composed store batch post here. NULL on a dead frame. */
actor_t* _frame_store_actor(frame_t* f);

/* 1 when the root's store actor is POOLED (its scheduler workers own the
   pacing — the driver's drain must never pump it); 0 = inline. */
uint8_t _frame_store_pooled(const frame_t* f);

/* The frame's own store round-trip corr space: one allocator shared with the
   bridge registry (`store_corr_seq` — no collisions); the engine allocates
   its awaited round trips here. Always nonzero. */
uint64_t _frame_store_corr_next(frame_t* f);

/* One event record post (the general §5 shape; loop.c's CELL_RUN audit
   commit rides it): the record composes against the frame's PRE-ALLOCATED
   seq and the one-op batch posts at the root's store actor with `corr` +
   `reply_to`. seq_out carries the PRE-ALLOCATED seq (also on the refusal
   paths — for a best-effort rollback). Returns 0 once POSTED; -1/-3 on the
   pre-post refusals (already logged; nothing was posted; roll the seq back
   with _frame_seq_rollback). CONSUMES the payload on every path. */
int _frame_event_post(frame_t* f, const char* type_name, json_value_t* payload,
                      uint64_t corr, actor_t* reply_to, uint64_t* seq_out);

/* Fire-and-post event write (corr 0, reply_to NULL): nothing awaits the
   reply — the engine's control events and PYRT_RESULT's cell.result. Returns
   the pre-post rc (0 = posted; a store-stage refusal leaves the recorded gap,
   the store worker logs it). */
int _frame_event_post_fire(frame_t* f, const char* type_name,
                           json_value_t* payload);

/* ONE ATOMIC multi-record EVENT batch (the turn envelope's batch riders;
   Task 2): every op is an event record at the frame's pre-allocated seqs —
   allocated CONTIGUOUSLY in one stretch at compose time (no interleaved
   allocation between them), composed per seq, and posted as ONE
   FRM_STORE_BATCH: `corr`/`reply_to` route an awaited reply (the engine's
   round-trip riders), corr 0 / reply NULL = fire-and-post (the control
   events' discipline). The store's ONE-atomic-batch rule keeps the record
   group from ever half-applying. CONSUMES every payload on every path.
   first_seq_out carries the batch's FIRST pre-allocated seq (also on the
   refusal paths, for a best-effort rollback of the whole range — reverse
   order keeps each rollback single-flight). Returns 0 once POSTED; -1 on
   the compose/dead-frame refusals, -3 on the WAL batch cap (loud, never
   truncating). */
int _frame_event_batch_post(frame_t* f, const char** type_names,
                            json_value_t** payloads, size_t nops,
                            uint64_t corr, actor_t* reply_to,
                            uint64_t* first_seq_out, const char* op_name);

/* The fire-and-post shape of the multi-record event batch (corr 0,
   reply NULL); on a pre-post refusal the whole pre-allocated seq range rolls
   back. `op_name` is BORROWED (a literal the round trip only logs — the
   batch's store log lines name the semantic action, not the first record).
   Returns the pre-post rc (0 = posted). */
int _frame_event_batch_post_fire(frame_t* f, const char** type_names,
                                 json_value_t** payloads, size_t nops,
                                 const char* op_name);

/* The tool path's PAIRED cell.result close (Task 2 rider 3; BOTH compose
   sites — frame.c's PYRT_RESULT completion and the engine's synchronous
   refusal pair in loop.c — post through it): the cell.result record and —
   when `with_riders` — the envelope's closers step.end + turn.end
   {reason completed} in ONE atomic fire-and-post batch, so the audit's
   answer and its envelope closers can never split across a crash boundary
   (DSH's finally-discipline). Riders clear the engine's turn_open/step_open
   when the batch POSTS. CONSUMES the payload on every path. Returns the
   pre-post rc (0 = posted). */
int _frame_engine_result_close_post(frame_t* f, json_value_t* result_payload,
                                    uint8_t with_riders);

/* The parked ask's PAIRED close (escalation spec §1.3; escalation Task 2):
   the sibling of the ordinary close, composed when PYRT_RESULT's close has
   riders AND the engine holds a pending ask. ONE fire-and-post batch:
   [cell.result (the real status), step.end, the "ask" record
   {kind, askId, question, options[], plan}, turn.end{reason blocked}] —
   the ask and its turn's close can never split across a crash boundary (and
   a crash between the turn close and the ask publish cannot exist, spec
   §4.1). On rc == 0 the engine flips turn_open/step_open to 0 and rests in
   FRAME_PHASE_ASK. A pre-post refusal leaves the OPEN tail and the park
   standing (the store's records stay the truth; no half-parked state) — the
   standing store-refusal discipline, already logged loud. CONSUMES the
   payload on every path. Returns the pre-post rc (0 = posted). */
int _frame_engine_ask_close_post(frame_t* f, json_value_t* result_payload);

/* The parked-ask state's clear (the pending ask dies whenever the engine's
   bookkeeping dies — every engine-end funnel plus the frame teardown): frees
   the OWNED ask_id, zeroes the corr/plan_gate, and — SA_HAS_PYTHON builds —
   clears the pyrt publish flag (the parked pre-check's truth follows the
   engine: no engine park, no parked ask). NULL-safe, idempotent. */
void _frame_engine_ask_clear(frame_t* f);

/* The posted ask reply (the handlers' shape — the _frame_steer_post
   precedent): ONE FRM_ASK_REPLY {ask_id, decision, value} into the frame's
   own mailbox; the frame's dispatch validates it against the park and
   composes the durable reply batch there. Returns 0 once POSTED — never a
   commit confirmation (§3.1's ack contract: the engine's stale-ask outcome
   lives in the events stream). Nonzero loud on the pre-post refusals
   (a dead frame, an empty ask_id, a decision outside 0|1, OOM). */
int _frame_ask_reply_post(frame_t* f, const char* ask_id, uint8_t decision,
                          const char* value);

/* Best-effort seq roll-back of an abandoned pre-allocation (§5). */
void _frame_seq_rollback(frame_t* f, uint64_t abandoned);

/* The terminal step's cross-subtree report bind (the quiet-completion and
   failure terminates both use it): the CHILD composes only its own
   frame.report record (its pre-allocated seq) and posts
   FRM_REPORT_BIND{engine_driven, failed} to the parent's actor — the
   parent composes the whole three-op batch, the store executes it as ONE
   atomic commit, and its corr-matched reply routes back to the child's
   actor, whose router posts ONE FRM_CHILD_REPORT{child_sid, failed} to the
   parent. Returns 0 once POSTED; nonzero on the pre-post refusals (already
   logged; nothing was posted — the caller still notifies the parent
   directly, never hang). */
int _frame_report_bind_post(frame_t* child, uint64_t bridge_corr,
                            uint8_t engine_driven, uint8_t failed,
                            const char* text);

/* The resume path's folded frame.join (spec §4): ONE frame.join event in the
   parent's log {child_sid}, FIRE-AND-POST (corr 0, reply_to NULL — it runs
   inside the parent's own dispatch; the store's FIFO commits it ahead of
   anything the engine posts after, so the resumed derive's scan sees the
   join). On-failure-continue: every refusal logs loud + rolls the seq back
   best-effort, the resume still happens. */
void _frame_join_post(frame_t* parent, const char* child_sid);

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
   the compose-refusal path). Returns 0 (repaired or already balanced), or
   -1/-3 loud otherwise (the compose-stage WAL cap refusals pass through,
   mirroring `_frame_event_post`'s 0/-1/-3 contract).

   Scope note (frame_resume's caller): a DONE subtree skips the pass
   entirely — every status=done write rode its terminal turn's close batch
   (the balance rule already holds), and the done handle's documented
   read-only shape keeps its sync round trips off. The pass therefore runs
   only on a NOT-done frame, i.e. only on the subtrees a crash could leave
   open. */
int _frame_resume_repair(frame_t* f);

/* The meta/status=done put, FIRE-AND-POST (the top engine's end-rule fix-up:
   a finish batch composed while live children were pending carries no status
   put — the CHILDREN yield's frame stays "running" — so an engine ending
   with live_children == 0 whose finish reply raced the last child report
   writes the completion itself; nothing awaits it). */
void _frame_status_post_fire(frame_t* f);

/* The parent-facing child-report post (the bind router's resume and the
   terminate's never-hang fallback): ONE frm_child_report_payload_t
   {child_sid, failed} at the LIVE parent's actor. */
void _frame_child_notify_post(frame_t* child, uint8_t failed);

/* The content path's ONE atomic turn-end batch (loop.c's FRAME_STORE_FINISH
   round trip): the assistant msg.append event — or, on an empty assistant
   turn, the empty-turn control event (payload {kind, text:null}, the exact
   shape _loop_control composes) — at the frame's PRE-ALLOCATED seq, plus the
   meta/status=done put for a TOP engine whose turn ENDS it (write_status;
   a content turn while live children are pending composes WITHOUT the put —
   the CHILDREN yield leaves the frame "running" — and a child's status stays
   "running", its content end reporting via the terminate's bind), in ONE
   root batch. Returns 0 once POSTED; nonzero on the pre-post refusals
   (already logged; the seq rolled back — nothing was posted). */
int _frame_engine_finish_post(frame_t* f, const char* append_text,
                              int write_status, uint64_t corr,
                              actor_t* reply_to);

#endif /* SA_HAS_WDB */

#ifdef __cplusplus
}
#endif

#endif // SA_FRAME_INTERNAL_H