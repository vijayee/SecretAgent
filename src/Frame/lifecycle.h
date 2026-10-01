//
// Created by victor on 10/1/26.
//

#ifndef SA_LIFECYCLE_H
#define SA_LIFECYCLE_H

#include "../Util/json.h"

/* --- caps (SA_* ifndef discipline — a build can override with -D) -------- */
#ifndef SA_LIFECYCLE_BRIEF_CODE_CHARS
#define SA_LIFECYCLE_BRIEF_CODE_CHARS 1200   /* the brief's quoted cell code */
#endif
#ifndef SA_LIFECYCLE_TAIL_EVENTS
#define SA_LIFECYCLE_TAIL_EVENTS 512         /* the resume tail scan's window */
#endif
#ifndef SA_LIFECYCLE_MAX_CLOSERS
#define SA_LIFECYCLE_MAX_CLOSERS 4           /* one closer batch's record cap */
#endif

/* --- event type + reason constants (the vocabulary's single truth) -------
   The turn/step envelopes' writers are the ENGINE (loop.c's riders) and
   the RESUME closers (frame.c); reasons "aborted"/"blocked" are RESERVED
   for the steering/interrupt slice (no writer this slice). */
extern const char LIFE_EVENT_TURN_START[];   /* "turn.start" */
extern const char LIFE_EVENT_TURN_END[];     /* "turn.end" */
extern const char LIFE_EVENT_STEP_START[];   /* "step.start" */
extern const char LIFE_EVENT_STEP_END[];     /* "step.end" */
extern const char LIFE_EVENT_REPAIR[];       /* "repair" — model-visible */

extern const char LIFE_REASON_COMPLETED[];    /* "completed" */
extern const char LIFE_REASON_ERROR[];        /* "error" */
extern const char LIFE_REASON_TURN_LIMIT[];   /* "turn-limit" (no writer today) */
extern const char LIFE_REASON_INTERRUPTED[];  /* "interrupted" */
extern const char LIFE_REASON_ABORTED[];      /* "aborted" — reserved */
extern const char LIFE_REASON_BLOCKED[];      /* "blocked" — reserved */

/* --- payload composers (the engine's riders; caller owns the DOM) --------
   Every rider's payload shape is FROZEN here. turn/step are JSON ints
   (unpadded — payloads never sort); turn_end's kind is one of the
   LIFE_REASON_* literals; text = the control kind's wording verbatim
   (NULL/"" renders the field absent — never an empty string). */
json_value_t* lifecycle_turn_start_json(uint64_t turn);
json_value_t* lifecycle_step_json(uint64_t turn, uint64_t step);
json_value_t* lifecycle_turn_end_json(uint64_t turn, const char* kind,
                                      const char* text);

/* --- the cursor (spec §2): the fold over an event tail -------------------
   The cursor holds STATE-OF-THE-TAIL, not event history. All heap fields
   are owned by the cursor and destroyed by lifecycle_cursor_destroy. */
typedef struct lifecycle_cursor_t {
  uint64_t turn;             /* the NEWEST recorded turn number (payloads) */
  uint64_t step;             /* the newest recorded step within `turn` */
  uint8_t turn_open;         /* 1 = a turn.start without its turn.end */
  uint8_t step_open;         /* 1 = a step.start without its step.end */
  uint8_t cell_inflight;     /* 1 = a cell.run committed, no matching result */
  uint64_t inflight_seq;     /* the cell.run record's seq (0 when none) */
  char* inflight_code;       /* the cell.run payload's code (heap; NULL none) */
  uint64_t inflight_corr;    /* the cell.run payload's corr (0 when none) */
  char* inflight_demand;     /* the result the cell never produced is unknown;
                              no demand text exists in the log today — this
                              field stays NULL this slice, reserved */
  uint64_t last_seq;         /* the tail's newest record's seq */
} lifecycle_cursor_t;

/* Fold a joint JSON-ARRAY text of event records (the scan reply's shape —
   the SAME contract refine_fold_parse consumes: {"type","payload"} objects,
   ascending) into the cursor. Each array element is ONE event RECORD: either
   the record's own JSON object ({"seq","type",...,"payload"} — the frozen
   event-record shape) or the record TEXT wrapped as a JSON string (the scan
   reply's joint form, exactly what refine_fold_parse's records ride as).
   Every element's "seq" (JSON int) and "type" (string) are required — a
   readable seq occupies a real log key, so it moves last_seq even when the
   record's payload skips. Unknown types pass through (they move nothing);
   malformed lifecycle records log loud + skip (the render-not-crash rule);
   turn/step numbers come from the records' OWN payloads.
   Returns 0, or -1 with a loud log on NULL input. */
int lifecycle_cursor_fold(const char* events_array_json, lifecycle_cursor_t* c);

/* The cursor's lifecycle (the folded heap fields). The fold initializes the
   cursor (a prior's content is overwritten — fold into a fresh cursor). */
void lifecycle_cursor_destroy(lifecycle_cursor_t* cursor);

/* --- the closers (spec §2/§4): the synthesized closer records ------------ */

typedef struct lifecycle_closer_t {
  const char* type;        /* BORROWED (a LIFE_EVENT_* literal) */
  uint64_t seq;            /* last_seq+1, contiguous, in order */
  json_value_t* payload;   /* owned by the closers' destroy */
} lifecycle_closer_t;

typedef struct lifecycle_closers_t {
  lifecycle_closer_t* items;   /* heap array */
  size_t n;
} lifecycle_closers_t;

/* Compose the deterministic closer records for the cursor's tail state
   (spec §4's order: the `repair` brief FIRST (when a brief exists — an
   open turn ALWAYS briefs: both shapes say something), then `step.end`
   (when open), then `turn.end {reason: "interrupted"}`). Empty tail /
   balanced tail (no open turn) = {NULL, 0} with NO allocation. The brief's
   text is composed HERE (the two pinned shapes, spec §2) and rides the
   repair record's payload {turn, text}. Returns the closers, or -1 +
   loud log on a NULL cursor (the module posture). */
int lifecycle_closers_compose(const lifecycle_cursor_t* cursor,
                              lifecycle_closers_t* out);

/* The closers' lifecycle (every payload DOM + the items array). */
void lifecycle_closers_destroy(lifecycle_closers_t* closers);

#endif /* defined(SA_LIFECYCLE_H) */