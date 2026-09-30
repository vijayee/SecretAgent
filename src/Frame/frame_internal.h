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

/* One raw event record write: {"seq","type","frame","corr","at","cause",
   "payload"} at the next seq, ONE root batch, seq bumped only on success.
   CONSUMES the payload (on failure too — nothing is left for the caller). */
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

#endif /* SA_HAS_WDB */

#ifdef __cplusplus
}
#endif

#endif // SA_FRAME_INTERNAL_H