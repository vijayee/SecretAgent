//
// Created by victor on 10/02/26.
//

#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/Frame/guards.h"
}

/* --- the doom fold ------------------------------------------------------ */

TEST(TestGuards, TestDoomTripsAtThreshold) {
  uint8_t tripped = 0;
  /* three identical calls: two increments land, the third trips */
  uint8_t streak = guards_doom_next(0, 1, 0, &tripped);
  EXPECT_EQ(streak, 1); EXPECT_EQ(tripped, 0);
  streak = guards_doom_next(streak, 1, 0, &tripped);
  EXPECT_EQ(streak, 2); EXPECT_EQ(tripped, 0);
  streak = guards_doom_next(streak, 1, 0, &tripped);
  EXPECT_EQ(tripped, 1) << "the threshold-th identical call trips";
  EXPECT_EQ(streak, 3);
}

TEST(TestGuards, TestDoomResetsOnDifferentCode) {
  uint8_t s = 0, tripped = 0;
  s = guards_doom_next(s, 1, 0, &tripped);
  s = guards_doom_next(s, 1, 0, &tripped);
  s = guards_doom_next(s, 0, 0, &tripped);   /* a DIFFERENT cell */
  EXPECT_EQ(s, 1);
  EXPECT_EQ(tripped, 0);
}

TEST(TestGuards, TestDoomResetsOnFreshInput) {
  uint8_t s = 0, tripped = 0;
  s = guards_doom_next(s, 1, 0, &tripped);
  s = guards_doom_next(s, 1, 0, &tripped);
  /* fresh user input between cells resets BEFORE the identity check even
     when the code is identical again */
  s = guards_doom_next(s, 1, 1, &tripped);
  EXPECT_EQ(s, 1) << "the steered model is not looping";
  EXPECT_EQ(tripped, 0);
}

TEST(TestGuards, TestDoomThresholdIsThree) {
  EXPECT_EQ(SA_GUARDS_DOOM_THRESHOLD, 3);
  /* passing NULL tripped is legal: the fold answers the streak; with
     tripped == NULL the trip is simply not reported to anyone */
  EXPECT_EQ(guards_doom_next(SA_GUARDS_DOOM_THRESHOLD - 1u, 1, 0, NULL),
            SA_GUARDS_DOOM_THRESHOLD);
}

TEST(TestGuards, TestStreakSaturates) {
  uint8_t tripped = 0;
  uint8_t s = guards_doom_next(UINT8_MAX, 1, 0, &tripped);
  EXPECT_EQ(tripped, 1);
  EXPECT_EQ(s, UINT8_MAX) << "the streak never wraps into a fresh streak";
}

/* --- the retry table ----------------------------------------------------- */

TEST(TestGuards, TestCauseClasses) {
  EXPECT_EQ(guards_retry_cause(-1, NULL), GUARDS_CAUSE_TRANSPORT);
  EXPECT_EQ(guards_retry_cause(500, NULL), GUARDS_CAUSE_SERVER);
  EXPECT_EQ(guards_retry_cause(503, NULL), GUARDS_CAUSE_SERVER);
  EXPECT_EQ(guards_retry_cause(429, NULL), GUARDS_CAUSE_RATE);
  EXPECT_EQ(guards_retry_cause(400, NULL), GUARDS_CAUSE_OVERFLOW);
  EXPECT_EQ(guards_retry_cause(404, NULL), GUARDS_CAUSE_OVERFLOW);
  EXPECT_EQ(guards_retry_cause(200, NULL), GUARDS_CAUSE_FALLBACK);
  /* the overload shape: a 2xx decode whose finish_reason is the load-stop */
  EXPECT_EQ(guards_retry_cause(200, "load"), GUARDS_CAUSE_OVERLOAD);
  EXPECT_EQ(guards_retry_cause(429, "load"), GUARDS_CAUSE_RATE)
      << "a genuine rate limit is not shadowed by a stray load reason";
  EXPECT_EQ(guards_retry_cause(404, "load"), GUARDS_CAUSE_OVERFLOW);
}

TEST(TestGuards, TestRetryPlans) {
  EXPECT_EQ(guards_retry_plan(GUARDS_CAUSE_TRANSPORT)->retryable, 1);
  EXPECT_EQ(guards_retry_plan(GUARDS_CAUSE_TRANSPORT)->cap, 5);
  EXPECT_EQ(guards_retry_plan(GUARDS_CAUSE_SERVER)->cap, 5);
  EXPECT_EQ(guards_retry_plan(GUARDS_CAUSE_RATE)->cap, 5);
  EXPECT_EQ(guards_retry_plan(GUARDS_CAUSE_OVERLOAD)->retryable, 0);
  EXPECT_EQ(guards_retry_plan(GUARDS_CAUSE_OVERFLOW)->retryable, 0);
  EXPECT_EQ(guards_retry_plan(GUARDS_CAUSE_FALLBACK)->cap, 1)
      << "the fallback keeps today's once-only rule";
}

TEST(TestGuards, TestInvalidCauseFallsBack) {
  /* an out-of-range cause (an enum drift's worst case) must never say
     "retry 5 times" — the fallback's once-only plan is the sink */
  EXPECT_EQ(guards_retry_plan((guards_cause_e)99)->cap, 1);
  EXPECT_EQ(guards_retry_plan((guards_cause_e)99)->retryable, 1);
  EXPECT_EQ(guards_retry_plan((guards_cause_e)-1)->cap, 1);
}

TEST(TestGuards, TestBackoffProgressionAndRetryAfter) {
  const guards_retry_plan_t* p = guards_retry_plan(GUARDS_CAUSE_TRANSPORT);
  EXPECT_EQ(guards_retry_backoff_ms(p, 0, 0), 0u);
  EXPECT_EQ(guards_retry_backoff_ms(p, 1, 0), 250u);
  EXPECT_EQ(guards_retry_backoff_ms(p, 2, 0), 500u);
  EXPECT_EQ(guards_retry_backoff_ms(p, 3, 0), 1000u);
  EXPECT_EQ(guards_retry_backoff_ms(p, 4, 0), 1000u);
  EXPECT_EQ(guards_retry_backoff_ms(p, 9, 0), 1000u) << "clamped to the last";
  /* rate honors retry-after seconds when present, capped at 5 s */
  const guards_retry_plan_t* rate = guards_retry_plan(GUARDS_CAUSE_RATE);
  EXPECT_EQ(guards_retry_backoff_ms(rate, 0, 3), 3000u);
  EXPECT_EQ(guards_retry_backoff_ms(rate, 0, 30), 5000u) << "capped";
  EXPECT_EQ(guards_retry_backoff_ms(rate, 0, 0), 0u) << "absent = the table's";
  /* non-retryable classes never get a backoff worth asking for */
  EXPECT_EQ(guards_retry_backoff_ms(guards_retry_plan(GUARDS_CAUSE_OVERLOAD), 0, 9), 0u);
}