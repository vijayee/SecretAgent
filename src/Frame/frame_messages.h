//
// Created by victor on 9/29/26.
//

#ifndef SA_FRAME_MESSAGES_H
#define SA_FRAME_MESSAGES_H

#include <stdint.h>

/* Frame-layer message types (carried in the generic message_t envelope).

   The numeric base keeps the frame vocabulary numerically DISJOINT from the
   pyrt vocabulary (pyrt_messages.h, PYRT_EXECUTE..PYRT_INTERRUPT = 0..5):
   these message types share ONE physical queue — the owning frame actor's
   mailbox receives both the pyrt thread's PYRT_* posts and its cells' FRM_*
   bridge requests — so equal numeric values would silently misroute
   dispatch. */
typedef enum frame_message_type_e {
  FRM_REMEMBER = 0x1000,  /* cell -> frame: durable state write */
  FRM_RECALL,             /* cell -> frame: resolve a key up the lineage chain */
  FRM_REPLY,              /* frame -> pyrt: corr-matched answer to a bridge request */
  FRM_CELL_EXECUTE,       /* loop -> frame: run this cell (from the model's tool call) */
  FRM_STOP,               /* control: end the frame when the queue drains */
  FRM_SPAWN,              /* cell -> frame: admission-only child spawn */
  FRM_REPORT              /* cell -> frame: the cell's frame.report verb */
} frame_message_type_e;

/* Event types (stored at sessions/<sid>/events/<seq>, JSON, %020d seq). ONLY
   msg.append produces context. */
typedef enum frame_event_type_e {
  EV_MSG_APPEND = 0,    /* payload {role, content} */
  EV_FRAME_SPAWN,       /* payload {child_sid, goal, depth} */
  EV_FRAME_REPORT,      /* payload {child_sid, text} — the one context-producing child event */
  EV_FRAME_JOIN,        /* payload {child_sid} */
  EV_CELL_RUN,          /* payload {code, corr} */
  EV_CELL_RESULT,       /* payload {corr, status, text} */
  EV_STATE_REMEMBER,    /* payload {key, value} */
  EV_CONTROL            /* payload {kind, text} — interrupt/shutdown/error */
} frame_event_type_e;

/* Bridge request payloads (ownership transfers with the message): */
typedef struct frm_remember_payload_t { uint64_t corr; char* key; char* json_value; } frm_remember_payload_t;
/* spawn: admission-only child request; context_json NULL = no handoff key. */
typedef struct frm_spawn_payload_t { uint64_t corr; char* goal; char* context_json; } frm_spawn_payload_t;
/* report: the cell's frame ends with this text. */
typedef struct frm_report_payload_t { uint64_t corr; char* text; } frm_report_payload_t;
/* reply: corr + status + heap text */
typedef struct frm_reply_payload_t { uint64_t corr; uint8_t status; char* text; } frm_reply_payload_t;
/* cell execute (loop -> frame): `corr` pairs the cell.run event with the
   cell.result event on the audit trail (the LOOP's corr space — pyrt's
   executor corr stays inside the runtime). Ownership of `code` transfers. */
typedef struct frm_cell_payload_t { uint64_t corr; char* code; } frm_cell_payload_t;
void frm_remember_payload_destroy(void* p);
void frm_spawn_payload_destroy(void* p);
void frm_report_payload_destroy(void* p);
void frm_reply_payload_destroy(void* p);
void frm_cell_payload_destroy(void* p);

/* JSON event record shape (authoritative):
   {"seq":<int>,"type":"<event-name>","frame":"<sid-path>","corr":<int|null>,
    "at":"<iso>","cause":<int|null>,"payload":{...}}  */

#endif // SA_FRAME_MESSAGES_H