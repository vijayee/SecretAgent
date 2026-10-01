//
// Created by victor on 10/1/26.
//
// test_lifecycle.cpp — the parity mirror (spec §7, DSH repair.spec.ts intents
// 1-10 adapted to our vocabulary; citations inline). Pure: no store, no model.
#include <gtest/gtest.h>

extern "C" {
#include "../src/Frame/lifecycle.h"
}

#include <cstring>
#include <string>
#include <vector>

/* Test helpers build event-record arrays by hand; the seed helpers live here.

   A record is the FROZEN event-record shape ({"seq","type",...,"payload"}).
   The joint tail text is a JSON ARRAY: each record rides either as its own
   object element or as its TEXT wrapped in a string (the scan reply's joint
   form, the SAME contract refine_fold_parse consumes). One event per
   record, ascending. */

static std::string lc_json_escape(const std::string& raw) {
  std::string out;
  for (char c : raw) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += c;
    }
  }
  return out;
}

/* The scan-reply form: the record text wrapped as one array element. */
static std::string lc_json_quote(const std::string& raw) {
  return "\"" + lc_json_escape(raw) + "\"";
}

static std::string lc_record(const char* type, const std::string& payload_json,
                             uint64_t seq) {
  return std::string("{\"seq\":") + std::to_string(seq) +
         ",\"type\":\"" + type + "\",\"corr\":null,\"payload\":" +
         payload_json + "}";
}

static std::string rec_msg(uint64_t seq) {
  return lc_record("msg.append",
                   "{\"role\":\"assistant\",\"content\":\"hi\"}", seq);
}

static std::string rec_control(uint64_t seq, const char* kind) {
  return lc_record("control",
                   std::string("{\"kind\":\"") + kind + "\"}", seq);
}

static std::string rec_turn_start(uint64_t seq, uint64_t turn) {
  return lc_record("turn.start", "{\"turn\":" + std::to_string(turn) + "}",
                   seq);
}

static std::string rec_step(uint64_t seq, const char* type, uint64_t turn,
                            uint64_t step) {
  return lc_record(type, "{\"turn\":" + std::to_string(turn) +
                             ",\"step\":" + std::to_string(step) + "}",
                   seq);
}

static std::string rec_turn_end(uint64_t seq, uint64_t turn,
                                const char* kind) {
  return lc_record("turn.end",
                   "{\"turn\":" + std::to_string(turn) +
                       ",\"reason\":{\"kind\":\"" + std::string(kind) +
                       "\"}}",
                   seq);
}

static std::string rec_cell_run(uint64_t seq, uint64_t corr,
                                const std::string& code) {
  return lc_record("cell.run",
                   "{\"code\":\"" + lc_json_escape(code) +
                       "\",\"corr\":" + std::to_string(corr) + "}",
                   seq);
}

static std::string rec_cell_result(uint64_t seq, uint64_t corr) {
  return lc_record("cell.result",
                   "{\"corr\":" + std::to_string(corr) +
                       ",\"status\":0,\"text\":\"ok\"}",
                   seq);
}

/* The joint tail text: `as_objects` pastes the records as bare elements
   (the module's other accepted input shape), otherwise each is
   string-wrapped (the scan reply's joint form). */
static std::string lc_joint(const std::vector<std::string>& records,
                            bool as_objects = false) {
  std::string out = "[";
  for (size_t i = 0; i < records.size(); i++) {
    if (i != 0) out += ",";
    out += as_objects ? records[i] : lc_json_quote(records[i]);
  }
  return out + "]";
}

/* The closer payload's brief text — the repair record's pinned field. */
static std::string lc_closer_text(const lifecycle_closers_t& closers,
                                  size_t index) {
  json_value_t* text_v = json_get(closers.items[index].payload, "text");
  return (text_v != nullptr) ? json_as_string(text_v) : std::string();
}

/* The whole payload DOM serialized (byte-identical comparison in the
   determinism test). */
static std::string lc_closer_dom(const lifecycle_closers_t& closers,
                                 size_t index) {
  char* raw = json_serialize(closers.items[index].payload);
  std::string out = (raw != nullptr) ? raw : "";
  free(raw);
  return out;
}

/* The pinned started shape's full text for a cell.run recorded at `seq`
   with the code quoted verbatim (the plan's Step 2 wording, newline-joined
   parts; truncation-at-source with "..." when the code passes the cap). */
static std::string lc_started_text(uint64_t seq, const char* code_quote) {
  std::string out =
      "The previous turn was interrupted before its result was recorded.\n";
  out += "The cell was executing (harness-log seq " + std::to_string(seq) +
         "):\n";
  out += code_quote;
  out += "\n";
  out +=
      "Its outcome is unknown. Decide whether to retry from the cell's "
      "semantics: retry only if the operation is read-only or idempotent; "
      "if it may have side effects, first verify external state or ask the "
      "user. Do not retry blindly.";
  return out;
}

static const char* kNotStartedText =
    "The previous turn was interrupted before the cell started. No cell "
    "execution was recorded. Retry it if it is still needed.";

TEST(TestLifecycle, TestBalancedTailComposesNothing) {
  /* port of repair.spec:104-114: a tail whose newest lifecycle record is a
     turn.end composes [] ; an EMPTY array composes []; an array with NO
     lifecycle records at all (a pre-lifecycle log — only msg.append/cell.*)
     composes [] — balance is about turn.END records, never about unknown
     types. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  std::string balanced =
      lc_joint({rec_turn_start(0, 1), rec_turn_end(1, 1, "completed")});
  ASSERT_EQ(lifecycle_cursor_fold(balanced.c_str(), &c), 0);
  EXPECT_EQ(c.turn_open, 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  EXPECT_EQ(out.n, 0u);
  EXPECT_EQ(out.items, nullptr) << "balanced tail = NO allocation";
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);

  /* An empty array folds and composes nothing. */
  ASSERT_EQ(lifecycle_cursor_fold("[]", &c), 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  EXPECT_EQ(out.n, 0u);
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);

  /* A pre-lifecycle log: unknown types pass through — they move nothing,
     and nothing here opens a turn. */
  std::string pre = lc_joint({rec_msg(4),
                              rec_control(5, "model-error-final"),
                              rec_cell_run(6, 3, "stray()"),
                              rec_cell_result(7, 3)});
  ASSERT_EQ(lifecycle_cursor_fold(pre.c_str(), &c), 0);
  EXPECT_EQ(c.turn_open, 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  EXPECT_EQ(out.n, 0u);
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestOpenTurnNoStepClosesWithTurnEndOnly) {
  /* port of repair.spec:116-123: an open turn (a turn.start, no turn.end)
     with NO step records composes exactly [turn.end {reason interrupted}] at
     the seq after the tail's last.
     ADAPTED (the frozen closer contract: an open turn ALWAYS briefs — plan
     Step 2 + Task 3(b)'s repair shape): the turn.end is preceded by its
     repair brief — no in-flight cell, so the NOT-STARTED shape. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  std::string tail = lc_joint({rec_turn_start(0, 1)});
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  EXPECT_EQ(c.turn_open, 1);
  EXPECT_EQ(c.turn, 1u);
  EXPECT_EQ(c.last_seq, 0u);

  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 2u);
  EXPECT_STREQ(out.items[0].type, LIFE_EVENT_REPAIR);
  EXPECT_EQ(out.items[0].seq, 1u);           /* last_seq + 1 */
  EXPECT_STREQ(out.items[1].type, LIFE_EVENT_TURN_END);
  EXPECT_EQ(out.items[1].seq, 2u);           /* contiguous */
  EXPECT_EQ(lc_closer_text(out, 0), kNotStartedText);
  EXPECT_EQ((uint64_t)json_as_int(json_get(out.items[0].payload, "turn")), 1u);

  json_value_t* reason = json_get(out.items[1].payload, "reason");
  ASSERT_NE(reason, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reason, "kind")),
               LIFE_REASON_INTERRUPTED);
  EXPECT_EQ((uint64_t)json_as_int(json_get(out.items[1].payload, "turn")), 1u);

  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestOpenStepClosesStepThenTurn) {
  /* port of repair.spec:125-133: an open step (step.start, no step.end)
     inside an open turn composes [turn.end-adjacent step.end, turn.end] in
     THIS order: repair FIRST, then step.end, then turn.end, contiguous seqs.
     No in-flight cell here, so the brief is the not-started shape (the
     frozen contract briefs every open turn; the DSH-raw comment's quote
     rider only rides the started shape). */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  std::string tail = lc_joint({rec_turn_start(0, 1),
                               rec_step(1, "step.start", 1, 1)});
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  EXPECT_EQ(c.turn_open, 1);
  EXPECT_EQ(c.step_open, 1);

  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);
  EXPECT_STREQ(out.items[0].type, LIFE_EVENT_REPAIR);
  EXPECT_EQ(out.items[0].seq, 2u);
  EXPECT_STREQ(out.items[1].type, LIFE_EVENT_STEP_END);
  EXPECT_EQ(out.items[1].seq, 3u);
  EXPECT_STREQ(out.items[2].type, LIFE_EVENT_TURN_END);
  EXPECT_EQ(out.items[2].seq, 4u);

  /* step.end closes BEFORE the turn (DSH's order) and carries the records'
     own numbers — never the seqs. */
  json_value_t* sp = out.items[1].payload;
  EXPECT_EQ((uint64_t)json_as_int(json_get(sp, "turn")), 1u);
  EXPECT_EQ((uint64_t)json_as_int(json_get(sp, "step")), 1u);
  EXPECT_EQ(lc_closer_text(out, 0), kNotStartedText);

  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestInFlightCellQuotedInBriefStarted) {
  /* port of repair.spec:135-176, 320-352: a cell.run committed with NO
     matching cell.result (the crash cut after the audit): the closer carries
     the repair brief first, its text contains the started-cause lead line
     "The previous turn was interrupted before its result was recorded.",
     the in-flight cell's seq, the cell.run's CODE verbatim (short one), the
     fact line "Its outcome is unknown.", and the retry-guidance line with
     "retry only if the operation is read-only or idempotent" and "Do not
     retry blindly." — details upfront, never a pointer. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  std::string tail = lc_joint({rec_turn_start(0, 2),
                               rec_step(1, "step.start", 2, 1),
                               rec_cell_run(2, 5, "do_stuff()")});
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  EXPECT_EQ(c.cell_inflight, 1);
  EXPECT_EQ(c.inflight_seq, 2u);
  EXPECT_EQ(c.inflight_corr, 5u);
  ASSERT_NE(c.inflight_code, nullptr);
  EXPECT_STREQ(c.inflight_code, "do_stuff()");

  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);   /* repair, step.end, turn.end */
  EXPECT_STREQ(out.items[0].type, LIFE_EVENT_REPAIR);
  EXPECT_EQ(out.items[0].seq, 3u);
  EXPECT_STREQ(out.items[1].type, LIFE_EVENT_STEP_END);
  EXPECT_EQ(out.items[1].seq, 4u);
  EXPECT_STREQ(out.items[2].type, LIFE_EVENT_TURN_END);
  EXPECT_EQ(out.items[2].seq, 5u);

  std::string text = lc_closer_text(out, 0);
  /* details upfront — the whole block, not a pointer */
  EXPECT_EQ(text, lc_started_text(2, "do_stuff()"));
  EXPECT_EQ((uint64_t)json_as_int(json_get(out.items[0].payload, "turn")), 2u);

  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestTurnOpenNoCellRecordedBriefNotStarted) {
  /* port of repair.spec's not-started wording: an open turn whose records
     carry NO cell.run (the crash cut between the model call and the audit)
     briefs "The previous turn was interrupted before the cell started. No
     cell execution was recorded." + "Retry it if it is still needed." and
     NO code quote — the not-started shape. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  std::string tail = lc_joint({rec_turn_start(0, 3),
                               rec_step(1, "step.start", 3, 1)});
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  EXPECT_EQ(c.cell_inflight, 0);
  EXPECT_EQ(c.inflight_code, nullptr);

  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);
  EXPECT_STREQ(out.items[0].type, LIFE_EVENT_REPAIR);
  /* the whole pinned text, string-equal — one line, no details, no quote */
  EXPECT_EQ(lc_closer_text(out, 0), kNotStartedText);
  EXPECT_EQ((uint64_t)json_as_int(json_get(out.items[0].payload, "turn")), 3u);

  /* the boundary pair still rides after the brief */
  EXPECT_STREQ(out.items[1].type, LIFE_EVENT_STEP_END);
  EXPECT_STREQ(out.items[2].type, LIFE_EVENT_TURN_END);

  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestAnsweredCellComposesNoQuote) {
  /* port of repair.spec:178-207: a cell.run WITH its matching cell.result
     (corr) composes [step.end, turn.end] with NO repair brief — the call
     is answered; nothing dangling.
     ADAPTED (the frozen closer contract briefs every OPEN turn — plan
     Step 2): the brief here is the NOT-STARTED shape — an answered cell
     leaves no open cell.run to quote, and the answered+boundaries-missing
     tail is unreachable in the real engine (the cell.result, step.end and
     turn.end riders commit as ONE batch), so the fact line stays truthful
     where the brief can be produced by a real crash. The DSH-raw pin the
     port keeps: no CODE QUOTE for an answered call. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  std::string tail = lc_joint({rec_turn_start(0, 2),
                               rec_step(1, "step.start", 2, 1),
                               rec_cell_run(2, 8, "done_thing()"),
                               rec_cell_result(3, 8)});
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  EXPECT_EQ(c.turn_open, 1);          /* the crash cut before the turn.end */
  EXPECT_EQ(c.step_open, 1);          /* and before the step.end */
  EXPECT_EQ(c.cell_inflight, 0);      /* the corr-8 result answered the run */

  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);
  EXPECT_STREQ(out.items[1].type, LIFE_EVENT_STEP_END);
  EXPECT_STREQ(out.items[2].type, LIFE_EVENT_TURN_END);
  /* NO code quote — the answered call's code never dangles into the brief */
  std::string text = lc_closer_text(out, 0);
  EXPECT_EQ(text.find("done_thing()"), std::string::npos);
  EXPECT_EQ(text.find("Its outcome is unknown"), std::string::npos);
  EXPECT_EQ(text.find("The previous turn was interrupted before its result "
                      "was recorded."),
            std::string::npos);

  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestOnlyTheStillOpenTurn) {
  /* port of repair.spec:235-284: two turns in one tail — turn 1 completed
     (its own closed cell) and turn 2 cut open mid-cell: the closers quote
     turn 2's cell, carry turn 2's numbers, and never touch turn 1. Also:
     closers' turn numbers come from the RECORDS' OWN payloads, not seqs. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  std::string tail = lc_joint({
      rec_turn_start(0, 1),
      rec_step(1, "step.start", 1, 1),
      rec_cell_run(2, 9, "old_code()"),
      rec_cell_result(3, 9),
      rec_step(4, "step.end", 1, 1),
      rec_turn_end(5, 1, "completed"),
      rec_turn_start(6, 2),
      rec_step(7, "step.start", 2, 1),
      rec_cell_run(8, 11, "new_code()"),
  });
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  EXPECT_EQ(c.turn_open, 1);
  EXPECT_EQ(c.turn, 2u);          /* turn 2's OWN payload turn */
  EXPECT_EQ(c.cell_inflight, 1);  /* turn 1's answered cell: closed at t.end */
  EXPECT_EQ(c.inflight_corr, 11u);
  ASSERT_NE(c.inflight_code, nullptr);
  EXPECT_STREQ(c.inflight_code, "new_code()");
  EXPECT_EQ(c.last_seq, 8u);

  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);
  /* seqs continue after the tail's last record (8): 9, 10, 11. */
  EXPECT_EQ(out.items[0].seq, 9u);
  EXPECT_EQ(out.items[1].seq, 10u);
  EXPECT_EQ(out.items[2].seq, 11u);

  /* The quote is turn 2's cell — turn 1's cell is never touched. */
  std::string text = lc_closer_text(out, 0);
  EXPECT_NE(text.find("new_code()"), std::string::npos);
  EXPECT_NE(text.find("harness-log seq 8"), std::string::npos);
  EXPECT_EQ(text.find("old_code()"), std::string::npos);
  EXPECT_EQ(text.find("harness-log seq 2"), std::string::npos);
  /* The boundary pair carries turn 2's payload numbers, not seqs. */
  json_value_t* ep = out.items[1].payload;
  EXPECT_EQ((uint64_t)json_as_int(json_get(ep, "turn")), 2u);
  EXPECT_EQ((uint64_t)json_as_int(json_get(ep, "step")), 1u);
  json_value_t* end = out.items[2].payload;
  EXPECT_EQ((uint64_t)json_as_int(json_get(end, "turn")), 2u);
  json_value_t* reason = json_get(end, "reason");
  ASSERT_NE(reason, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reason, "kind")),
               LIFE_REASON_INTERRUPTED);

  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestResultMismatchDoesNotAcknowledge) {
  /* port of repair.spec:79-97, corr-mapped: a cell.result whose corr does
     NOT match the open cell.run's does not acknowledge it — the brief
     still quotes the open cell; a SECOND cell.run over an open one is the
     malformed record: loud log line + skipped, the fold survives with the
     FIRST cell in flight (the render-not-crash rule). */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  std::string tail = lc_joint({rec_turn_start(0, 1),
                               rec_step(1, "step.start", 1, 1),
                               rec_cell_run(2, 21, "keep_me()"),
                               rec_cell_result(3, 99)});
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  EXPECT_EQ(c.cell_inflight, 1) << "a corr mismatch never acknowledges";
  EXPECT_EQ(c.inflight_corr, 21u);
  ASSERT_NE(c.inflight_code, nullptr);
  EXPECT_STREQ(c.inflight_code, "keep_me()");

  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);
  EXPECT_EQ(lc_closer_text(out, 0), lc_started_text(2, "keep_me()"));
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);

  /* The second cell.run over the still-open first: loud + skipped, the
     fold survives with the FIRST cell in flight. */
  std::string tail2 = lc_joint({rec_turn_start(0, 1),
                                rec_step(1, "step.start", 1, 1),
                                rec_cell_run(2, 21, "keep_me()"),
                                rec_cell_result(3, 99),
                                rec_cell_run(4, 22, "second()")});
  ASSERT_EQ(lifecycle_cursor_fold(tail2.c_str(), &c), 0);
  EXPECT_EQ(c.cell_inflight, 1);
  EXPECT_EQ(c.inflight_corr, 21u);
  ASSERT_NE(c.inflight_code, nullptr);
  EXPECT_STREQ(c.inflight_code, "keep_me()");
  EXPECT_EQ(c.inflight_seq, 2u);

  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);
  std::string text = lc_closer_text(out, 0);
  EXPECT_NE(text.find("keep_me()"), std::string::npos);
  EXPECT_EQ(text.find("second()"), std::string::npos);
  EXPECT_NE(text.find("harness-log seq 2"), std::string::npos);
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestCursorToleratesRefusedTurnStart) {
  /* spec §2's tolerance pin: records whose turn.start never committed
     (step.start/cell.run carrying turn numbers alone) fold fine — numbers
     come from payloads. The cursor's turn counter = the newest recorded
     turn number regardless of which record type carried it. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  /* The turn.start was refused; the step and cell records carried on. The
     fold's turn counter comes from step.start's OWN payload. */
  std::string tail = lc_joint({rec_step(5, "step.start", 2, 1),
                               rec_cell_run(6, 7, "cut()")});
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  EXPECT_EQ(c.turn, 2u);
  EXPECT_EQ(c.step, 1u);
  EXPECT_EQ(c.turn_open, 1);       /* a step with no turn.end = open turn */
  EXPECT_EQ(c.step_open, 1);
  EXPECT_EQ(c.cell_inflight, 1);
  EXPECT_EQ(c.inflight_seq, 6u);
  EXPECT_EQ(c.last_seq, 6u);
  lifecycle_cursor_destroy(&c);

  /* And the refused turn.start still balances once the turn's end lands —
     the fold never depends on a record that may legitimately be absent. */
  std::string balanced = lc_joint({rec_step(5, "step.start", 2, 1),
                                   rec_step(6, "step.end", 2, 1),
                                   rec_turn_end(7, 2, "completed")});
  ASSERT_EQ(lifecycle_cursor_fold(balanced.c_str(), &c), 0);
  EXPECT_EQ(c.turn, 2u);
  EXPECT_EQ(c.turn_open, 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  EXPECT_EQ(out.n, 0u);
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestWordingPinnedVerbatim) {
  /* port of repair.spec:368-426's wording table: both brief shapes pinned
     EXACT string-equal (the tests build the tails and compare the whole
     text); the started shape's code quote truncates at
     SA_LIFECYCLE_BRIEF_CODE_CHARS with loud truncation-at-source (the log
     line), matching the spec's cap discipline. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  /* The started shape: EXACT string-equal on the whole text (whitespace
     inside the code preserved — the code keeps its own newlines). */
  std::string code = "for i in range(3):\n    print(i)";
  std::string started = lc_joint({rec_turn_start(0, 4),
                                  rec_step(1, "step.start", 4, 1),
                                  rec_cell_run(2, 13, code)});
  ASSERT_EQ(lifecycle_cursor_fold(started.c_str(), &c), 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);
  EXPECT_EQ(lc_closer_text(out, 0), lc_started_text(2, code.c_str()));
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);

  /* The not-started shape: EXACT string-equal on the whole text. */
  std::string not_started = lc_joint({rec_turn_start(0, 5)});
  ASSERT_EQ(lifecycle_cursor_fold(not_started.c_str(), &c), 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 2u);
  EXPECT_EQ(lc_closer_text(out, 0), kNotStartedText);
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);

  /* The cap: a long code truncates at SA_LIFECYCLE_BRIEF_CODE_CHARS with
     "..." — the truncation happens at SOURCE (the brief's text), after a
     loud log line at compose time. */
  std::string long_code(SA_LIFECYCLE_BRIEF_CODE_CHARS + 40, 'x');
  std::string long_tail = lc_joint({rec_turn_start(0, 4),
                                    rec_step(1, "step.start", 4, 1),
                                    rec_cell_run(2, 17, long_code)});
  ASSERT_EQ(lifecycle_cursor_fold(long_tail.c_str(), &c), 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);
  std::string truncated_code =
      long_code.substr(0, (size_t)SA_LIFECYCLE_BRIEF_CODE_CHARS - 3) + "...";
  EXPECT_EQ(lc_closer_text(out, 0), lc_started_text(2, truncated_code.c_str()));
  /* the full code never appears — the tail never rides uncapped */
  EXPECT_EQ(lc_closer_text(out, 0).find(long_code), std::string::npos);
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);

  /* A code AT the cap is quoted verbatim — only a PAST-cap code truncates. */
  std::string exact_code(SA_LIFECYCLE_BRIEF_CODE_CHARS, 'y');
  std::string exact_tail = lc_joint({rec_turn_start(0, 4),
                                     rec_step(1, "step.start", 4, 1),
                                     rec_cell_run(2, 19, exact_code)});
  ASSERT_EQ(lifecycle_cursor_fold(exact_tail.c_str(), &c), 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  ASSERT_EQ(out.n, 3u);
  EXPECT_EQ(lc_closer_text(out, 0), lc_started_text(2, exact_code.c_str()));
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestDeterministicAndSeqContiguous) {
  /* port of repair.spec:64-77: the SAME tail composes byte-identical
     closers twice; seqs = last_seq+1, +2, +3... contiguous; the compose is
     pure (no randomness, no clock). */
  lifecycle_closers_t a;
  lifecycle_closers_t b;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));

  std::string tail = lc_joint({rec_turn_start(0, 1),
                               rec_step(1, "step.start", 1, 1),
                               rec_cell_run(2, 31, "twice()")});

  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &a), 0);
  lifecycle_cursor_destroy(&c);   /* a fold overwrites state — destroy the
                                     folded heap before folding again */
  memset(&c, 0, sizeof(c));
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &b), 0);
  lifecycle_cursor_destroy(&c);
  ASSERT_EQ(a.n, b.n);
  ASSERT_EQ(a.n, 3u);
  for (size_t i = 0; i < a.n; i++) {
    EXPECT_EQ(a.items[i].seq, b.items[i].seq);
    EXPECT_STREQ(a.items[i].type, b.items[i].type);
    EXPECT_EQ(lc_closer_dom(a, i), lc_closer_dom(b, i))
        << "the same tail composes byte-identical closers";
  }
  EXPECT_EQ(a.items[0].seq, 3u);
  EXPECT_EQ(a.items[1].seq, 4u);
  EXPECT_EQ(a.items[2].seq, 5u);
  lifecycle_closers_destroy(&a);
  lifecycle_closers_destroy(&b);

  /* The fold also takes the tail's records as BARE OBJECT elements (the
     module's other accepted element shape) — same fold, same closers. */
  std::string object_form =
      lc_joint({rec_turn_start(0, 1), rec_step(1, "step.start", 1, 1),
                rec_cell_run(2, 31, "twice()")},
               true);
  memset(&c, 0, sizeof(c));
  ASSERT_EQ(lifecycle_cursor_fold(object_form.c_str(), &c), 0);
  ASSERT_EQ(lifecycle_closers_compose(&c, &a), 0);
  lifecycle_cursor_destroy(&c);
  ASSERT_EQ(a.n, 3u);
  EXPECT_EQ(lc_closer_text(a, 0), lc_started_text(2, "twice()"));
  lifecycle_closers_destroy(&a);
}

TEST(TestLifecycle, TestMalformedLifecycleRecordsFoldLoudSkip) {
  /* the render-not-crash rule (spec §1/§2, refine's fold discipline):
     corrupt payloads (non-object payload, non-int turn) log loud + skip;
     the fold survives and still balances on the turn.end records it
     understood. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  std::vector<std::string> records = {
      rec_turn_start(1, 1),                         /* opens turn 1 */
      lc_record("turn.start", "{\"turn\":\"1\"}", 2),   /* non-int turn */
      lc_record("turn.end", "5", 3),                 /* payload not object */
      lc_record("step.start", "{\"turn\":1}", 4),    /* no step */
      lc_record("turn.end",
                "{\"turn\":1,\"reason\":{\"kind\":\"banana\"}}", 5),  /* unknown kind */
      "\"not a record document\"",                  /* text is not JSON */
      lc_record("turn.end", "{\"turn\":1,\"reason\":{}}", 6),  /* no kind */
      rec_turn_start(7, 9),
      rec_turn_end(8, 9, "completed"),
  };
  std::string tail = lc_joint(records);
  ASSERT_EQ(lifecycle_cursor_fold(tail.c_str(), &c), 0);
  /* the fold survives: the understood turn.end (turn 9) balances it, and
     the turn counter is the newest UNDERSTOOD record's own payload. */
  EXPECT_EQ(c.turn, 9u);
  EXPECT_EQ(c.turn_open, 0);
  EXPECT_EQ(c.last_seq, 8u);
  ASSERT_EQ(lifecycle_closers_compose(&c, &out), 0);
  EXPECT_EQ(out.n, 0u);

  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);

  /* A record without its seq is unreadable end to end — skipped whole
     (last_seq never moves to a seq the tail never stated). */
  std::string no_seq_tail = lc_joint(
      {rec_turn_end(1, 1, "completed"),
       "{\"type\":\"turn.end\",\"payload\":{\"turn\":1,\"reason\":"
       "{\"kind\":\"completed\"}}}"});
  ASSERT_EQ(lifecycle_cursor_fold(no_seq_tail.c_str(), &c), 0);
  EXPECT_EQ(c.last_seq, 1u) << "the seqless record is skipped whole";
  EXPECT_EQ(c.turn_open, 0);
  lifecycle_cursor_destroy(&c);
}

TEST(TestLifecycle, TestCursorRejectsNullInputLoud) {
  /* the module posture (refine's): NULL fold/input = loud refuse, no
     silent empty-fold leniency. */
  lifecycle_cursor_t c;
  memset(&c, 0, sizeof(c));
  lifecycle_closers_t out;
  memset(&out, 0, sizeof(out));

  EXPECT_EQ(lifecycle_cursor_fold(nullptr, &c), -1);
  EXPECT_EQ(lifecycle_cursor_fold("[]", nullptr), -1);
  /* a refused fold leaves the cursor untouched — destroying it is safe */
  lifecycle_cursor_destroy(&c);

  /* a non-array tail refuses loud too — no silent empty-fold leniency */
  EXPECT_EQ(lifecycle_cursor_fold("{\"seq\":0}", &c), -1);
  EXPECT_EQ(lifecycle_cursor_fold("garbage", &c), -1);
  lifecycle_cursor_destroy(&c);

  /* the composers' posture: a NULL cursor composes nothing */
  EXPECT_EQ(lifecycle_closers_compose(nullptr, &out), -1);
  EXPECT_EQ(lifecycle_closers_compose(&c, nullptr), -1);
  EXPECT_EQ(out.items, nullptr);
  lifecycle_closers_destroy(&out);
  lifecycle_cursor_destroy(&c);
}