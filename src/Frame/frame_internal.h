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
  FRAME_PHASE_CHILDREN      /* yielded: live children pending; each child
                               report's FRM_CHILD_REPORT reposts the turn */
} frame_phase_e;

/* What the pending FRAME_PHASE_STORE round trip is for (the engine state's
   store_kind): each kind's reply continues the turn inside the reply
   dispatch itself. */
typedef enum frame_store_kind_e {
  FRAME_STORE_DERIVE = 1,   /* the bounded events scan; the reply parses the records
                               and runs the projection + submit/reply processing */
  FRAME_STORE_CELL_RUN,     /* the cell.run audit commit; the reply dispatches
                               FRM_CELL_EXECUTE (no untracked cell ever runs) */
  FRAME_STORE_FINISH        /* msg.append (+ meta/status=done for a top frame) in ONE
                               batch; the reply ends the engine (top) or drives the
                               child's quiet-completion report bind (child) */
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
} frame_engine_state_t;

/* The teardown CLAIM marker inside pending_submits: exactly one agent (the
   destroy that found no slot held, or the sink's last release on a
   die-requested frame — the CAS in frame.c arbitrates) ever holds it, and
   that agent runs _frame_destroy_run. */
#define SA_ENGINE_SUBMIT_CLAIM UINT32_MAX

/* The engine state's accessor (NULL on a dead/unknown frame). */
frame_engine_state_t* _frame_engine_state(frame_t* f);

/* The die-requested flag's atomic read (loop.c's model sink + completion
   handler gate against a dying frame): 0 = clear, 1 = a frame_destroy ran
   (or defers) mid-turn. 0 for a NULL frame. */
uint8_t _frame_engine_die_requested(const frame_t* f);

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
   end the engine (top) / drive the child's report bind (child).
   CONSUMES the payload on every path. */
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

/* 1 when the frame has a parent frame (i.e. is a spawned child). */
uint8_t _frame_is_child(const frame_t* f);

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