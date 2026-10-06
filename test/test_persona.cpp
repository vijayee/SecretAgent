//
// Created by victor on 10/6/26.
//
// test_persona.cpp — the persona slice's PURE suite (spec §4's row): the
// record load + the shape's refusal rules, the falsifiability meta-rule as
// data (incl. the load-time enforcement), the compose's pinned shapes (the
// placement both modes, the context block's text/JSON render, the tool-
// conditional attach/skip, the byte-stability), and the placeholder catalog
// ({CURRENT_DATETIME} + the load's stability-opt-out warning, {USER_*}
// recognized-missing → "" vs. the unrecognized token left VISIBLE). Pure:
// no store, no model, no actor — it registers on the plain gate.
#include <gtest/gtest.h>

extern "C" {
#include "../src/Frame/persona.h"
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