//
// Created by victor on 10/02/26.
//

#include "guards.h"

#include <string.h>

/* --- the doom fold -------------------------------------------------------- */

uint8_t guards_doom_next(uint8_t streak, uint8_t identical_code,
                         uint8_t fresh_input, uint8_t* tripped) {
  if (tripped != NULL) *tripped = 0;
  if (fresh_input != 0) {
    streak = 0;   /* the reset runs BEFORE the identity check */
  }
  if (identical_code != 0) {
    if (streak != UINT8_MAX) streak = (uint8_t)(streak + 1);
    if (tripped != NULL && streak >= SA_GUARDS_DOOM_THRESHOLD) {
      *tripped = 1;
    }
  } else {
    streak = 1;
  }
  return streak;
}

/* --- the retry table ------------------------------------------------------- */

static const guards_retry_plan_t _guards_plans[] = {
    {1, 5},   /* TRANSPORT: retry, 5  */
    {1, 5},   /* SERVER:   retry, 5  */
    {1, 5},   /* RATE:     retry, 5  */
    {0, 0},   /* OVERLOAD: fail loud */
    {0, 0},   /* OVERFLOW: fail loud */
    {1, 1},   /* FALLBACK: today's once-only */
};
#define _GUARDS_BACKOFF_MS_MAX 5000u

static unsigned _guards_backoff_step(uint8_t retry_index) {
  /* 0/250/500/1000/1000 — doublings capped at the head (spec §2's table). */
  static const unsigned steps[] = {0u, 250u, 500u, 1000u, 1000u};
  static const size_t steps_n = sizeof(steps) / sizeof(steps[0]);
  size_t i = (size_t)retry_index;
  if (i >= steps_n) i = steps_n - 1;
  return steps[i];
}

guards_cause_e guards_retry_cause(int http_status, const char* finish_reason) {
  if (http_status < 0) return GUARDS_CAUSE_TRANSPORT;
  /* the overload shape: ONLY a 2xx decode whose finish_reason is the
     load-stop — a 429/4xx/5xx with a stray load reason keeps its own class */
  if (http_status >= 200 && http_status < 300 && finish_reason != NULL &&
      strcmp(finish_reason, "load") == 0) {
    return GUARDS_CAUSE_OVERLOAD;
  }
  if (http_status == 429) return GUARDS_CAUSE_RATE;
  if (http_status >= 400 && http_status < 500) return GUARDS_CAUSE_OVERFLOW;
  if (http_status >= 500 && http_status < 600) return GUARDS_CAUSE_SERVER;
  return GUARDS_CAUSE_FALLBACK;
}

const guards_retry_plan_t* guards_retry_plan(guards_cause_e cause) {
  size_t i = (size_t)(int)cause;
  /* an out-of-range cause (an enum drift's worst case) must never inherit
     TRANSPORT's retry-5 plan — the FALLBACK plan is the sink */
  if (i >= (sizeof(_guards_plans) / sizeof(_guards_plans[0]))) {
    i = (size_t)GUARDS_CAUSE_FALLBACK;
  }
  return &_guards_plans[i];
}

unsigned guards_retry_backoff_ms(const guards_retry_plan_t* plan,
                                 uint8_t retry_index, unsigned retry_after_sec) {
  if (plan == NULL || plan->retryable == 0) return 0;
  if (retry_after_sec > 0) {
    unsigned ms = retry_after_sec * 1000u;
    if (ms > _GUARDS_BACKOFF_MS_MAX || ms / 1000u < retry_after_sec) {
      return _GUARDS_BACKOFF_MS_MAX;   /* the cap also guards the multiply */
    }
    return ms;
  }
  return _guards_backoff_step(retry_index);
}