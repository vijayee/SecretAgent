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

/* Runs the frame's turn loop to completion on the CURRENT thread (the frame's
   own scheduler worker runs it in production; tests drive it inline).
   Returns 0 on clean completion (no tool call), nonzero on error. */
int frame_run_loop(frame_t* f);

#endif /* SA_HAS_WDB */

#endif // SA_LOOP_H