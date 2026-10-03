//
// Created by victor on 10/02/26.
//

/* The engine's GUARDS (the guards spec, 2026-10-02): PURE policy — no store,
   no threads, no actor. The doom-loop breaker's streak fold and the
   model-failure retry table both fold facts the ENGINE observed; the callers
   act on the answers (the tool path refuses + closes; the reply path reposts
   or fails loud). Constants only — no config knobs (the budget-table
   discipline). */

#ifndef SA_GUARDS_H
#define SA_GUARDS_H

#include <stddef.h>
#include <stdint.h>

/* --- the doom-loop breaker ------------------------------------------------ */

/* The breaker's threshold: the Nth byte-identical consecutive cell is
   refused before its audit (opencode's processor.ts DOOM_LOOP_THRESHOLD = 3;
   doc steal-line: an event to the runtime's policy, never a self-approving
   ask). ifndef discipline — a build can override with -D. */
#ifndef SA_GUARDS_DOOM_THRESHOLD
#define SA_GUARDS_DOOM_THRESHOLD 3
#endif

/* The doom streak's next value. identical_code = the incoming cell code is
   byte-identical to the last dispatched one; fresh_input = a NEW user-role
   msg.append arrived since the last dispatch (the steered model is not
   looping — the reset runs BEFORE the identity check).
   *tripped (may be NULL) = the threshold crossed THIS call: the incoming
   cell must be REFUSED (never dispatched, never audited). The returned
   streak stores back; the caller frees/replaces its code copy. */
uint8_t guards_doom_next(uint8_t streak, uint8_t identical_code,
                         uint8_t fresh_input, uint8_t* tripped);

/* --- the model-failure retry table ---------------------------------------- */

typedef enum guards_cause_e {
  GUARDS_CAUSE_TRANSPORT = 0,   /* http status -1 (no response) */
  GUARDS_CAUSE_SERVER,          /* http 5xx */
  GUARDS_CAUSE_RATE,            /* http 429 */
  GUARDS_CAUSE_OVERLOAD,        /* the provider load-stop shape (2xx decode
                                   whose finish_reason is "load") */
  GUARDS_CAUSE_OVERFLOW,        /* http 4xx — a client-side refusal; retrying
                                   blind is never right */
  GUARDS_CAUSE_FALLBACK         /* anything else (decode failures on 2xx
                                   bodies included) */
} guards_cause_e;

typedef struct guards_retry_plan_t {
  uint8_t retryable;   /* 0 = fail loud immediately, never repost */
  uint8_t cap;         /* RETRIES AFTER the original attempt (cap 5 = up to
                          6 calls); the fallback's 1 = today's once-only rule */
} guards_retry_plan_t;

/* The cause from the failure's observed facts: http_status (-1 = transport;
   2xx = the decode's own business — passed through UNLESS finish_reason is
   the provider's load-stop). finish_reason may be NULL. */
guards_cause_e guards_retry_cause(int http_status, const char* finish_reason);

/* The class's plan (never NULL). */
const guards_retry_plan_t* guards_retry_plan(guards_cause_e cause);

/* The backoff BEFORE retry number `retry_index` (0-based: the first retry's
   wait). rate: retry_after_sec when present (>0), capped at 5 s, else the
   table's progression. Non-retryable classes answer 0 (the caller must have
   refused them anyway — the answer keeps the API total). */
unsigned guards_retry_backoff_ms(const guards_retry_plan_t* plan,
                                 uint8_t retry_index, unsigned retry_after_sec);

#endif /* SA_GUARDS_H */