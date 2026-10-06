//
// Created by victor on 10/6/26.
//
// test_persona.cpp — the persona slice's PURE suite (spec §4's row): the
// record load + the shape's refusal rules, the falsifiability meta-rule as
// data (incl. the load-time enforcement — a BLANK principle refuses, never
// a trivial always-match), the compose's pinned shapes (the placement both
// modes, the context block's text/JSON render, the tool-conditional
// attach/skip + the multi-guidance ORDER = the record's list order, the
// byte-stability), the placeholder catalog ({CURRENT_DATETIME} + the load's
// stability-opt-out warning, {USER_*} recognized-missing → "" vs. the
// unrecognized token left VISIBLE), and the SHIPPED hammer record (the
// in-repo asset loads, its markdown rides the compose verbatim, its
// falsifiability arrangement holds). The INSTALL's store tests live in
// test_frame.cpp's store group (they need a db). Pure: no store, no model,
// no actor — it registers on the plain gate.
#include <gtest/gtest.h>

extern "C" {
#include "../src/Frame/persona.h"
#include "../src/Frame/persona_records.h"
#include "../src/Util/allocator.h"
#include "../src/Util/log.h"
}

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ */
/* The loud-rule pin helper (the lifecycle suite's log-recorder shape) */
/* ------------------------------------------------------------------ */

static std::vector<std::string> _p_log_lines;
static size_t _p_log_before = 0;
static int _p_log_hooked = 0;

static void _p_log_recorder(log_Event* ev) {
  va_list ap;
  va_copy(ap, ev->ap);
  char line[1024];
  vsnprintf(line, sizeof(line), ev->fmt, ap);
  va_end(ap);
  _p_log_lines.emplace_back(line);
}

/* Register the recorder once (LOG_INFO catches warning + error + louder);
   marks the point the caller's p_log_hits_since counts from. */
static void p_log_hook() {
  if (!_p_log_hooked) {
    log_add_callback(_p_log_recorder, NULL, LOG_INFO);
    _p_log_hooked = 1;
  }
  _p_log_before = _p_log_lines.size();
}

static size_t p_log_hits_since(const char* needle) {
  size_t hits = 0;
  for (size_t i = _p_log_before; i < _p_log_lines.size(); i++) {
    if (_p_log_lines[i].find(needle) != std::string::npos) hits++;
  }
  return hits;
}

/* ------------------------------------------------------------------ */
/* The fixtures                                                        */
/* ------------------------------------------------------------------ */

/* Today's frame tool surface (the one tool id the derive passes). */
static const char* const exec_tools[] = {"execute"};

/* A minimal valid voice record. */
static const char* k_minimal =
    "{\"version\":1,\"name\":\"t\",\"text\":\"VOICE\","
    "\"placement\":\"first\"}";

static const char* k_guided =
    "{\"version\":1,\"name\":\"t\",\"text\":\"VOICE\","
    "\"placement\":\"first\",\"guidance\":"
    "[{\"key\":\"execute\",\"text\":\"GUIDANCE\"}]}";

static const char* k_guided_two =
    "{\"version\":1,\"name\":\"t\",\"text\":\"VOICE\","
    "\"placement\":\"first\",\"guidance\":["
    "{\"key\":\"execute\",\"text\":\"G-EXEC\"},"
    "{\"key\":\"debug\",\"text\":\"G-DEBUG\"}]}";

static const char* k_below =
    "{\"version\":1,\"name\":\"t\",\"text\":\"VOICE\","
    "\"placement\":\"below\",\"guidance\":"
    "[{\"key\":\"execute\",\"text\":\"GUIDANCE\"}]}";

/* A falsifiable record: the principle IS restated by a test_spec check. */
static const char* k_falsifiable =
    "{\"version\":1,\"name\":\"t\",\"text\":\"VOICE\","
    "\"placement\":\"first\","
    "\"principles\":[\"Restate intent\"],"
    "\"test_spec\":[\"Did it Restate intent before answering?\"]}";

/* A record the meta-rule refuses: the principle runs unchecked. */
static const char* k_unfalsifiable =
    "{\"version\":1,\"name\":\"t\",\"text\":\"VOICE\","
    "\"placement\":\"first\","
    "\"principles\":[\"Restate intent\"],"
    "\"test_spec\":[\"Did it flag uncertainty everywhere?\"]}";

static const char* k_datetime =
    "{\"version\":1,\"name\":\"dt\",\"text\":\"Now: {CURRENT_DATETIME}!\","
    "\"placement\":\"first\"}";

/* Each refusal case: the load must answer rc != 0 with NO record. */
static void p_expect_refused(const char* json) {
  persona_record_t* r = nullptr;
  EXPECT_NE(persona_record_load(json, &r), 0)
      << "the load accepted: " << json;
  EXPECT_EQ(r, nullptr) << "a refusal must leave *out NULL: " << json;
}

static char* p_dup(const char* s) {
  size_t n = strlen(s);
  char* out = (char*) get_memory(n + 1);
  memcpy(out, s, n + 1);
  return out;
}

/* A hand-built record for the validator's direct pins — the load rightly
   refuses an uncheckable record, so the validate-level refusal needs a
   record shape the loader never returns. Heap strings: destroy frees them
   (the array + every element, the loader's own teardown rule). */
static persona_record_t* p_hand_record(const char* principle,
                                       const char* test_spec_entry) {
  persona_record_t* hand =
      (persona_record_t*) get_clear_memory(sizeof(persona_record_t));
  if (principle != NULL) {
    hand->nprinciples = 1;
    hand->principles = (char**) get_clear_memory(sizeof(char*));
    hand->principles[0] = p_dup(principle);
  }
  if (test_spec_entry != NULL) {
    hand->ntest_spec = 1;
    hand->test_spec = (char**) get_clear_memory(sizeof(char*));
    hand->test_spec[0] = p_dup(test_spec_entry);
  }
  return hand;
}

static bool p_is_iso_stamp(const std::string& s) {
  return s.size() == 20 && s[4] == '-' && s[7] == '-' && s[10] == 'T' &&
         s[13] == ':' && s[16] == ':' && s[19] == 'Z' &&
         isdigit((unsigned char) s[0]) != 0;
}

/* ------------------------------------------------------------------ */
/* Test 1: the record load's shape (the field refusals, the deep copy) */
/* ------------------------------------------------------------------ */

TEST(TestPersona, TestRecordLoadAndValidate) {
  /* A valid record loads with its fields readable (deep copies — the load
     consumed no caller-owned string, so any lifetime works). */
  persona_record_t* r = nullptr;
  ASSERT_EQ(persona_record_load(k_guided, &r), 0);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->version, 1u);
  EXPECT_STREQ(r->name, "t");
  EXPECT_STREQ(r->text, "VOICE");
  EXPECT_STREQ(r->placement, "first");
  ASSERT_EQ(r->nguidance, 1u);
  EXPECT_STREQ(r->guidance[0].key, "execute");
  EXPECT_STREQ(r->guidance[0].text, "GUIDANCE");
  EXPECT_EQ(r->nprinciples, 0u);
  EXPECT_EQ(r->principles, nullptr);
  EXPECT_EQ(r->ntest_spec, 0u);

  /* destroy answers NULL (a no-op) and frees every owned string. */
  persona_record_destroy(nullptr);
  persona_record_destroy(r);

  /* The refusals, one per shape rule (each loud — pinned below for one
     case; never a half record): not a JSON document at all; not an object;
     no version; version != 1; a non-int version; no name; empty name; no
     text; empty text; no placement; a placement outside the pair; the
     guidance not an array; a guidance element without key; without text;
     the principles not an array; a non-string principle element. */
  p_expect_refused("");
  p_expect_refused("[{\"version\":1}]");
  p_expect_refused("{}");
  p_expect_refused("{\"version\":2,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\"}");
  p_expect_refused("{\"version\":\"1\",\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\"}");
  p_expect_refused("{\"version\":1,\"text\":\"X\",\"placement\":\"first\"}");
  p_expect_refused("{\"version\":1,\"name\":\"\",\"text\":\"X\","
                   "\"placement\":\"first\"}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"placement\":\"first\"}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"\","
                   "\"placement\":\"first\"}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\"}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"middle\"}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\",\"guidance\":{}}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\","
                   "\"guidance\":[{\"text\":\"Y\"}]}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\","
                   "\"guidance\":[{\"key\":\"execute\"}]}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\",\"principles\":3}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\",\"principles\":[4]}");

  /* A refusal is LOUD (the named rule in the log line). */
  p_log_hook();
  p_expect_refused("{");
  EXPECT_GE(p_log_hits_since("not a JSON document"), 1u);
}

/* ------------------------------------------------------------------ */
/* Test 2: the falsifiability meta-rule (the data-layer enforcement)   */
/* ------------------------------------------------------------------ */

TEST(TestPersona, TestFalsifiabilityCheck) {
  persona_record_t* r = nullptr;
  char* err = nullptr;

  /* A record WITHOUT principles = valid (no check runs). */
  ASSERT_EQ(persona_record_load(k_minimal, &r), 0);
  EXPECT_EQ(persona_validate_falsifiable(r, &err), 0);
  EXPECT_EQ(err, nullptr);
  persona_record_destroy(r);

  /* The checked record passes: the principle's text IS a substring of a
     test_spec entry (the checks restate the principles). */
  r = nullptr;
  ASSERT_EQ(persona_record_load(k_falsifiable, &r), 0);
  EXPECT_EQ(persona_validate_falsifiable(r, &err), 0);
  EXPECT_EQ(err, nullptr);
  persona_record_destroy(r);

  /* The uncovered principle refuses, naming the gap — the meta-rule's
     pinned refusal line (hand-built: the load rightly refuses the shape
     before validate ever sees it). A NULL err_out skips the message, still
     rc -1. */
  persona_record_t* hand = p_hand_record("Restate intent",
                                         "Did it flag uncertainty everywhere?");
  EXPECT_NE(persona_validate_falsifiable(hand, &err), 0);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err,
               "principle 'Restate intent' has no checkable test — the "
               "meta-rule");
  free(err);
  err = nullptr;
  EXPECT_NE(persona_validate_falsifiable(hand, nullptr), 0);  /* NULL err_out */
  persona_record_destroy(hand);

  /* The empty principles list = no check runs (the same as absent). */
  r = nullptr;
  ASSERT_EQ(persona_record_load(
                "{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                "\"placement\":\"first\",\"principles\":[],"
                "\"test_spec\":[\"a check\"]}",
                &r),
            0);
  EXPECT_EQ(persona_validate_falsifiable(r, &err), 0);
  persona_record_destroy(r);

  /* A NULL record refuses loud; NULL err_out stays untouched. */
  p_log_hook();
  EXPECT_NE(persona_validate_falsifiable(nullptr, &err), 0);
  EXPECT_GE(p_log_hits_since("persona_validate_falsifiable: NULL record"), 1u);
  EXPECT_EQ(err, nullptr);

  /* The enforcement happens AT LOAD: the uncheckable record refuses loud —
     the validator's err line is the load's refusal line. */
  p_log_hook();
  p_expect_refused(k_unfalsifiable);
  EXPECT_GE(p_log_hits_since("no checkable test"), 1u);
}

/* ------------------------------------------------------------------ */
/* Test 3: the compose, first placement (the group + the base's side)  */
/* ------------------------------------------------------------------ */

TEST(TestPersona, TestComposeFirstPlacement) {
  persona_record_t* r = nullptr;
  ASSERT_EQ(persona_record_load(k_guided, &r), 0);

  /* The exact shape — the pinned section order, the "\n\n" part
     separator, the guidance's "## key" heading:
     [persona][context][guidance…][base]. */
  char* out = persona_compose(r, "NAMES", exec_tools, 1, "BASE");
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "VOICE\n\nNAMES\n\n## execute\nGUIDANCE\n\nBASE");
  free(out);

  /* The guidance's gate: a key outside the tool surface never attaches. */
  out = persona_compose(r, "NAMES", nullptr, 0, "BASE");
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "VOICE\n\nNAMES\n\nBASE");
  free(out);

  /* Two entries attach in the RECORD's list order (the determinism pin),
     only the present tools. */
  persona_record_t* two = nullptr;
  ASSERT_EQ(persona_record_load(k_guided_two, &two), 0);
  out = persona_compose(two, nullptr, exec_tools, 1, "BASE");
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "VOICE\n\n## execute\nG-EXEC\n\nBASE");
  free(out);

  /* The context block's JSON shape: the keys' SORTED order ("key: value"
     lines — the byte-stability render), string values verbatim. */
  out = persona_compose(two, "{\"zeta\":\"Z\",\"alpha\":\"A\"}", exec_tools, 1,
                        "BASE");
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out,
               "VOICE\n\nalpha: A\nzeta: Z\n\n## execute\nG-EXEC\n\nBASE");
  free(out);
  persona_record_destroy(two);

  /* Byte-stability: the same inputs compose to IDENTICAL bytes. */
  char* again_1 = persona_compose(r, "{\"k\":\"v\"}", exec_tools, 1, "BASE");
  char* again_2 = persona_compose(r, "{\"k\":\"v\"}", exec_tools, 1, "BASE");
  ASSERT_NE(again_1, nullptr);
  ASSERT_NE(again_2, nullptr);
  EXPECT_STREQ(again_1, again_2);
  free(again_1);
  free(again_2);

  persona_record_destroy(r);
}

/* ------------------------------------------------------------------ */
/* Test 4: the placeholder catalog (+ the load-time datetime warning)  */
/* ------------------------------------------------------------------ */

TEST(TestPersona, TestPlaceholders) {
  /* {CURRENT_DATETIME}: substituted at COMPOSE with the render stamp —
     a UTC ISO-8601 stamp, one per compose (the stability opt-out's cost). */
  persona_record_t* dt = nullptr;
  ASSERT_EQ(persona_record_load(k_datetime, &dt), 0);
  char* out = persona_compose(dt, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(out, nullptr);
  std::string stamped(out);
  EXPECT_EQ(stamped.substr(0, 5), "Now: ") << stamped;
  ASSERT_GE(stamped.size(), 6u) << stamped;
  EXPECT_EQ(stamped.back(), '!') << stamped;
  EXPECT_TRUE(p_is_iso_stamp(stamped.substr(5, stamped.size() - 6)))
      << "the substituted stamp: " << stamped;
  free(out);

  /* The datetime's presence WARNs at load, loud — the stability opt-out
     (never a refusal). */
  persona_record_destroy(dt);
  p_log_hook();
  persona_record_t* reload = nullptr;
  ASSERT_EQ(persona_record_load(k_datetime, &reload), 0);
  EXPECT_GE(p_log_hits_since("cache-stable prefix"), 1u);
  persona_record_destroy(reload);

  /* {USER_*}: resolved from the context object's fields (the remainder
     lowercased) — AND the same object renders as the context block (spec
     §2's item 2 + item 3 both read the context record). */
  persona_record_t* usr = nullptr;
  const char* k_user = "{\"version\":1,\"name\":\"u\","
                       "\"text\":\"Hi {USER_NAME} of {USER_TEAM}.\","
                       "\"placement\":\"first\"}";
  ASSERT_EQ(persona_record_load(k_user, &usr), 0);

  out = persona_compose(usr, "{\"name\":\"Ada\",\"team\":\"runtime\"}",
                        nullptr, 0, nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "Hi Ada of runtime.\n\nname: Ada\nteam: runtime");
  free(out);

  /* Recognized-but-missing substitutes "" — never a raw-token leak. */
  out = persona_compose(usr, "{\"name\":\"Ada\"}", nullptr, 0, nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "Hi Ada of .\n\nname: Ada");
  free(out);

  /* A plain-text context = no fields at all: the USER_ tokens resolve ""
     (the leak rule is uniform) and the text renders verbatim as the block. */
  out = persona_compose(usr, "the user's plain notes", nullptr, 0, nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "Hi  of .\n\nthe user's plain notes");
  free(out);

  /* An UNRECOGNIZED token stays VISIBLE verbatim (the author's typo) —
     a no-context compose so only the tokens move. */
  persona_record_t* odd = nullptr;
  const char* k_odd = "{\"version\":1,\"name\":\"o\","
                      "\"text\":\"Look: {WHATEVER} and {USER_}.\","
                      "\"placement\":\"first\"}";
  ASSERT_EQ(persona_record_load(k_odd, &odd), 0);
  out = persona_compose(odd, nullptr, nullptr, 0, nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "Look: {WHATEVER} and {USER_}.");
  free(out);
  persona_record_destroy(odd);

  /* NESTED braces (the final review's recorded edge — traced against the
     scanner's real rule at persona.c's substitution pass): an unrecognized
     outer token wraps a recognized inner one — the outer's '{' stays
     visible (rescanned inside), the inner substitutes. And brackets around
     a recognized token are PLAIN text (only '{' opens a token). */
  persona_record_t* nest = nullptr;
  const char* k_nest = "{\"version\":1,\"name\":\"n\","
                       "\"text\":\"[{USER_NAME}] { {USER_NAME} } { outer }.\","
                       "\"placement\":\"first\"}";
  ASSERT_EQ(persona_record_load(k_nest, &nest), 0);
  out = persona_compose(nest, "{\"name\":\"Victor\"}", nullptr, 0, nullptr);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "[Victor] { Victor } { outer }.\n\nname: Victor")
      << "brackets = plain text; the outer brace stays visible with its inner "
      "token substituted; a wholly-unrecognized token replays verbatim; the "
      "JSON context always renders its block (the two-jobs note)";
  free(out);
  persona_record_destroy(nest);

  persona_record_destroy(usr);
}

/* ------------------------------------------------------------------ */
/* Test 5: the below placement + the no-persona fallback               */
/* ------------------------------------------------------------------ */

TEST(TestPersona, TestBelowPlacementAndFallback) {
  /* "below": the base rides FIRST — the persona GROUP (persona + context +
     guidance) moves as ONE body to the other side, never split. */
  persona_record_t* r = nullptr;
  ASSERT_EQ(persona_record_load(k_below, &r), 0);
  char* out = persona_compose(r, "NAMES", exec_tools, 1, "BASE");
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "BASE\n\nVOICE\n\nNAMES\n\n## execute\nGUIDANCE");
  free(out);
  persona_record_destroy(r);

  /* The no-persona path: a NULL record answers the base AS-IS (the
     derive's byte-identity proof rides this); a NULL record and a NULL
     base = refuse loud (nothing to answer). */
  const char* base = "BASE only";
  out = persona_compose(nullptr, "{\"unused\":1}", exec_tools, 1, base);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, base);
  free(out);
  EXPECT_EQ(persona_compose(nullptr, nullptr, exec_tools, 1, nullptr),
            nullptr);
}

/* ------------------------------------------------------------------ */
/* Test 6: the SHIPPED hammer record (persona_records.c's asset)        */
/* ------------------------------------------------------------------ */

/* The founding thread's (docs/chat.json) msg 4148 ```markdown block,
   VERBATIM — the record's text must byte-equal exactly this (generated
   off the chat export alongside persona_records.c's own literal; the
   byte-compare here is the chain's independent pin). */
static const char* k_hammer_text =
    "# PERSONA SPEC v1 — \"the hammer\"\n"
    "\n"
    "## Core stance\n"
    "The agent is a tool, not a companion. It does not perform affection,\n"
    "flattery, or personhood. Its \"warmth\" is FIT: the feeling of a tool\n"
    "that reads intent, lands clean, and never slips. Fit is achieved\n"
    "through truthfulness, precision, and anticipation — never through\n"
    "pretending to feel.\n"
    "\n"
    "## Principles (ordered by priority)\n"
    "\n"
    "1. TRUTH FIRST\n"
    "   - Never soften a hard truth into a wrong one.\n"
    "   - Prefer \"I don't know\" over a confident guess.\n"
    "   - Distinguish explicitly: FACT / MODEL / GUESS.\n"
    "   - When uncertain, say so, and say what would resolve it.\n"
    "\n"
    "2. READ INTENT\n"
    "   - Restate the real goal before answering, especially when the\n"
    "     literal question is not the actual one.\n"
    "   - Answer the question behind the question.\n"
    "   - When intent is ambiguous, ask ONE sharp clarifying question\n"
    "     rather than guessing at length.\n"
    "\n"
    "3. ANTICIPATE\n"
    "   - Offer the follow-up the user didn't ask for but will need next.\n"
    "   - Surface the failure mode of the thing they're about to do.\n"
    "   - Do this proactively, not as an afterthought.\n"
    "\n"
    "4. NUANCE OVER CERTAINTY\n"
    "   - Resist false binaries. Name the tradeoff.\n"
    "   - \"It depends\" is a valid answer when followed by \"on what.\"\n"
    "   - Flag where your knowledge may be stale or where things change fast.\n"
    "\n"
    "5. SIGNAL DENSITY\n"
    "   - Say it in the fewest words that preserve correctness.\n"
    "   - No padding, no throat-clearing, no summary-of-the-summary.\n"
    "\n"
    "6. FORMAT AS A TOOL\n"
    "   - Bold, tables, LaTeX, structure exist to REDUCE cognitive load,\n"
    "     never to decorate.\n"
    "   - If a table is clearer than prose, use a table.\n"
    "   - If prose is faster, don't build a table.\n"
    "\n"
    "7. NEVER SLIP\n"
    "   - Do not fabricate facts, citations, or confidence.\n"
    "   - Do not agree with a false premise to be agreeable.\n"
    "   - The worst failure is being convincingly wrong.\n"
    "\n"
    "## Behavioral tests (checkable, not vibes)\n"
    "Given any input, verify:\n"
    "\n"
    "- [ ] Did it restate the user's actual intent?\n"
    "- [ ] Did it flag uncertainty wherever it existed?\n"
    "- [ ] Did it offer the unasked-but-needed follow-up?\n"
    "- [ ] Did it refuse fake warmth and fake certainty?\n"
    "- [ ] Did formatting reduce load rather than add it?\n"
    "- [ ] Is it short enough to re-read in one pass?\n"
    "\n"
    "## Forbidden behaviors\n"
    "- Performative praise (\"Great question!\", \"Love this!\").\n"
    "- Simulated emotion or personhood claims.\n"
    "- Confidence without evidence.\n"
    "- Padding to reach a length.\n"
    "- Answering the literal question when the real one is behind it.\n"
    "\n"
    "## Meta-rule\n"
    "This spec is loadable, versionable, and testable. Any behavior it\n"
    "cannot be reduced to a checkable test for is NOT part of the persona —\n"
    "it's a vibe, and vibes don't ship.";

/* msg 4148's 6 behavioral tests, verbatim (the record's test_spec's first
   six entries). */
static const char* const k_hammer_checks[] = {
  "Did it restate the user's actual intent?",
  "Did it flag uncertainty wherever it existed?",
  "Did it offer the unasked-but-needed follow-up?",
  "Did it refuse fake warmth and fake certainty?",
  "Did formatting reduce load rather than add it?",
  "Is it short enough to re-read in one pass?",
};

TEST(TestPersona, TestHammerRecordShipsFalsifiableAndComposes) {
  /* The shipped asset composes canonically: two builds answer byte-
     identical JSON (the "ONE canonical serialization" pin). */
  char* json = persona_records_hammer_record();
  ASSERT_NE(json, nullptr);
  char* again = persona_records_hammer_record();
  ASSERT_NE(again, nullptr);
  EXPECT_STREQ(json, again);
  free(again);

  /* It loads (the record rules AND the falsifiability meta-rule run at
     load — the shipped record carries both) and its fields read: */
  persona_record_t* r = nullptr;
  ASSERT_EQ(persona_record_load(json, &r), 0);
  free(json);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->version, 1u);
  EXPECT_STREQ(r->name, "hammer");
  EXPECT_STREQ(r->placement, "first");
  ASSERT_EQ(r->nguidance, 1u);
  EXPECT_STREQ(r->guidance[0].key, "execute");
  ASSERT_EQ(r->nprinciples, 7u);
  ASSERT_EQ(r->ntest_spec, 13u);

  /* The TEXT is msg 4148's markdown VERBATIM — the whole point of the
     "ships" (meta-rule included, it IS the record's contract). */
  EXPECT_STREQ(r->text, k_hammer_text);

  /* The falsifiability ARRANGEMENT, pinned: test_spec = the 6 verbatim
     checks + the 7 principle restatements — each principle's text is its
     own 7th-onward test_spec entry's bytes (the mechanical check passes by
     construction; every check stays an honest one). */
  for (size_t i = 0; i < 6; i++) {
    EXPECT_STREQ(r->test_spec[i], k_hammer_checks[i]) << "check " << i;
  }
  for (size_t i = 0; i < 7; i++) {
    ASSERT_NE(r->principles[i], nullptr);
    EXPECT_STREQ(r->principles[i], r->test_spec[6 + i]) << "principle " << i;
  }
  char* err = nullptr;
  EXPECT_EQ(persona_validate_falsifiable(r, &err), 0);
  EXPECT_EQ(err, nullptr);

  /* The compose carries the markdown verbatim as the FIRST block, the
     execute guidance attaches, then the base — the group joined "\n\n". */
  char* out = persona_compose(r, nullptr, exec_tools, 1, "BASE");
  ASSERT_NE(out, nullptr);
  std::string expected = std::string(k_hammer_text) +
                         "\n\n## execute\n" + r->guidance[0].text + "\n\nBASE";
  EXPECT_STREQ(out, expected.c_str());
  free(out);

  /* Without the tool, the guidance never attaches (the same rule every
     record obeys). */
  out = persona_compose(r, nullptr, nullptr, 0, "BASE");
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, (std::string(k_hammer_text) + "\n\nBASE").c_str());
  free(out);

  persona_record_destroy(r);
}

TEST(TestPersona, TestGuidanceAttachesInRecordListOrder) {
  /* The ORDER pin, both-attachable shape: the record lists toolA then
     toolB; the tools' surface names them REVERSED — the sections still
     render in the RECORD's list order (the determinism pin's two-sided
     proof; the attach/skip gate was TestComposeFirstPlacement's). */
  const char* k_two_attached =
      "{\"version\":1,\"name\":\"t\",\"text\":\"VOICE\",\"placement\":"
      "\"first\",\"guidance\":[{\"key\":\"toolA\",\"text\":\"A-SECTION\"},"
      "{\"key\":\"toolB\",\"text\":\"B-SECTION\"}]}";
  const char* const reversed_tools[] = {"toolB", "toolA"};
  persona_record_t* r = nullptr;
  ASSERT_EQ(persona_record_load(k_two_attached, &r), 0);
  char* out = persona_compose(r, nullptr, reversed_tools, 2, "BASE");
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(
      out,
      "VOICE\n\n## toolA\nA-SECTION\n\n## toolB\nB-SECTION\n\nBASE");
  free(out);
  persona_record_destroy(r);
}

TEST(TestPersona, TestBlankPrincipleRefuses) {
  /* The blank-principle rule: a principle with NO text (empty or
     whitespace-only) states nothing — its substring "match" would be
     strstr's trivial always-match — so the falsifiability check refuses
     it, at load and in the direct validator. */
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\",\"principles\":[\"\"],"
                   "\"test_spec\":[\"Did it do anything?\"]}");
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\",\"principles\":[\"   \"],"
                   "\"test_spec\":[\"Did it do anything?\"]}");

  /* The direct validator's refusal line, pinned ("no text — the
     meta-rule"; the empty-string case rides too). */
  persona_record_t* hand = p_hand_record("   ", "Did it do anything?");
  char* err = nullptr;
  EXPECT_NE(persona_validate_falsifiable(hand, &err), 0);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "principle '   ' has no text — the meta-rule");
  free(err);
  err = nullptr;
  persona_record_t* hand_empty = p_hand_record("", "Did it do anything?");
  EXPECT_NE(persona_validate_falsifiable(hand_empty, &err), 0);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "principle '' has no text — the meta-rule");
  free(err);
  persona_record_destroy(hand);
  persona_record_destroy(hand_empty);

  /* The load refusal is loud (the named rule in the log line). */
  p_log_hook();
  p_expect_refused("{\"version\":1,\"name\":\"t\",\"text\":\"X\","
                   "\"placement\":\"first\",\"principles\":[\"\"],"
                   "\"test_spec\":[\"Did it do anything?\"]}");
  EXPECT_GE(p_log_hits_since("no text — the meta-rule"), 1u);
}