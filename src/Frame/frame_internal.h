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
#include "../Util/json.h"

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
  FRAME_PHASE_CHILDREN      /* yielded: live children pending (Task 5 fills this) */
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
  uint64_t turn_cell_corr;     /* the loop's audit corr of the cell the
                                  pending cell.run round trip committed (the
                                  paired refusal result carries it) */
  model_reply_t* turn_reply;   /* the in-flight turn's model reply, OWNED by
                                  the engine between the model arrival and its
                                  path's dispatch turn (the CELL_RUN reply
                                  runs the cell out of it) */
} frame_engine_state_t;

/* The engine state's accessor (NULL on a dead/unknown frame). */
frame_engine_state_t* _frame_engine_state(frame_t* f);

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

/* The FRM_CHILD_REPORT behavior (Task 5 fills it; Task 3 declares the route):
   bookkeeping + resume-only-a-live-engine. CONSUMES the payload on every
   path. */
void _frame_engine_child_report(frame_t* f, frm_child_report_payload_t* payload);

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

/* The content path's ONE atomic turn-end batch (loop.c's FRAME_STORE_FINISH
   round trip): the assistant msg.append event — or, on an empty assistant
   turn, the empty-turn control event (payload {kind, text:null}, the exact
   shape _loop_control composes) — at the frame's PRE-ALLOCATED seq, plus the
   meta/status=done put for a TOP engine (a child's status stays "running"
   and its content end reports via Task 6's terminate), in ONE root batch.
   Returns 0 once POSTED; nonzero on the pre-post refusals (already logged;
   the seq rolled back — nothing was posted). */
int _frame_engine_finish_post(frame_t* f, const char* append_text,
                              int write_status, uint64_t corr,
                              actor_t* reply_to);

#endif /* SA_HAS_WDB */

#ifdef __cplusplus
}
#endif

#endif // SA_FRAME_INTERNAL_H