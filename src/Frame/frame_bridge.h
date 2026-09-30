//
// Created by victor on 9/29/26.
//

#ifndef SA_FRAME_BRIDGE_H
#define SA_FRAME_BRIDGE_H

#include <stdint.h>

/* Reply half of the python bridge: how a frame behavior hands a corr-matched
   answer back to a blocked `agent.*` caller (py_agent.c, Task 7).

   Delivery shape: the frame behavior calls the installed sink DIRECTLY on the
   frame's dispatch thread (scheduler worker, or the test thread that pumps
   frame_dispatch). The sink must not block and must not free `text` — the
   text is borrowed, valid for the call only (the real Task 7 registry copies
   it into its completion record before waking the waiter).

   Until a sink is registered the built-in DROPPER answers every reply with a
   loud log_error: a dropped reply leaves the python caller blocked until its
   (bounded) timeout, which must never happen silently. */
typedef void (*frame_bridge_reply_fn_t)(uint64_t corr, uint8_t status, const char* text);

/* Install the reply sink. The expected production sink is py_agent.c's
   `py_agent_note_reply(corr, status, text)`, registered once at runtime init;
   passing NULL resets to the built-in dropper (tests use this to demount
   their recording sinks). Registration is a one-way startup-time wire —
   re-registering while frames dispatch on other threads is a caller bug. */
void frame_bridge_register_reply_sink(frame_bridge_reply_fn_t sink);

/* Test/debug accessor: the LAST reply passed through the reply path (to a
   registered sink OR to the default dropper). Returns 1 when a reply has been
   seen, 0 otherwise; either output may be NULL. */
uint8_t frame_bridge_debug_last(uint64_t* corr_out, uint8_t* status_out);

#endif // SA_FRAME_BRIDGE_H