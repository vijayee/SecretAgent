//
// Created by victor on 9/29/26.
//

#ifndef SA_LOOP_H
#define SA_LOOP_H

#include "../Frame/frame.h"

/* frame_t is declared only in the WaveDB-gated section of frame.h (the whole
   frame/loop API rides the store), so the loop's one public entry point is
   gated the same way — a python/wavedb-free build sees nothing here,
   exactly like model.h's API rides on the same frame.h section. */
#ifdef SA_HAS_WDB

struct frame_t;

/* Runs the frame's turn loop to completion via the frame's OWN event-driven
   engine (the orchestration plan §6): when the engine is not already live it
   is STARTED (frame_start's ONE FRM_TURN continuation); an already-live
   engine is PUMPED from where it is. The driver returns when the engine is
   terminal or yielded:
     0 = clean end-of-run (no tool call on the last turn; also a stop
         request / a mid-flight report's engine end),
     1 = failed loud (the engine's terminal step was a failure), or
     2 = yielded LIVE with an external input pending: live children (Task
         5's FRAME_PHASE_CHILDREN) or a parked ask (FRAME_PHASE_ASK — the
         owner composes through frame_ask_reply; the reply's posted
         FRM_ASK_REPLY clears the park and the next run loop resumes).
   The driver bounded-pumps the frame's, its live ancestors', and the inline
   store's mailboxes with 1 ms sleeps between cycles; per-phase deadlines
   (SA_LOOP_CELL_WAIT_MS / SA_LOOP_MODEL_WAIT_MS / SA_LOOP_STORE_WAIT_MS)
   break any stalled wait loud ("cell-timeout" / "model-await" /
   "store-timeout" + the engine ends failed) — never a hang. Production
   event-driven runs use frame_start on POOLED frames instead (the pool owns
   pacing); frame_run_loop stays the inline/test driver. */
int frame_run_loop(frame_t* f);

#endif /* SA_HAS_WDB */

#endif // SA_LOOP_H