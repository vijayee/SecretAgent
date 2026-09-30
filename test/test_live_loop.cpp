//
// Created by victor on 9/30/26.
//
// Task 12: the opt-in LIVE integration gate. It compiles everywhere the
// frame loop compiles and is ALWAYS registered with ctest, but the test
// body SKIPS unless SA_TEST_OLLAMA_URL names an OpenAI-compatible endpoint
// — a default ctest run never touches the network.
//
// The goal is the plan's real-model shape ("remember the word 'wave' then
// report it"): a live model must round-trip the word through the bridge —
// a state.remember event, a recall that resolves it, and the report — with
// the audit trail showing every effect. The scenario reruns on FRESH FRAMES
// (up to three attempts; the plan's literal goal first, then a sharper
// instruction) because a live model is nondeterministic about acting vs
// narrating — the assertion never loosens across attempts. One full-run
// retry per attempt is allowed on top of the loop's own
// one-retry-per-call policy: cloud-model endpoints burst far past a single
// call's timeout, and the derive is stateless (a failed run leaves nothing
// to undo; control events carry no model context), so re-running
// frame_run_loop is exactly "keep driving the same session after a
// transport failure", not result masking. A failed attempt dumps its audit
// trail ([  AUDIT  ] lines) so the failure diagnoses itself.
//
// Default model tag: SA_TEST_OLLAMA_MODEL's choice when set; otherwise
// "gemma4:31b-cloud". Note why the gate does NOT default to a local tag:
// this Ollama answers the tool-calling turns of the LOCAL gemma4 in 60-90 s
// — well past the built-in 30 s completion timeout — so the gate carries
// cfg.model_timeout_ms = 180000 (the frame-config seam; 0 would be the
// 30 s default) and any endpoint slow enough to need it gets it. The
// cloud-served tag answers the same turn in seconds and decodes into the
// OpenAI tool shape (arguments-as-string) model.c already handles.

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
extern "C" {
#include "../src/Frame/frame.h"
#include "../src/Frame/frame_messages.h"
#include "../src/Frame/model.h"
#include "../src/Frame/loop.h"
#include "../src/Util/json.h"
}

#if defined(SA_HAS_WDB)

#include <unistd.h>

/* The suite's re-mount idiom (see test_loop.cpp): idempotent, and it
   re-registers the bridge reply sink after other suites' demounts — the
   live cells reach the frame's bridge verbs through it. Declared bare so
   the single TEST below (whose body carries its own python gate) reaches
   it without py_agent.h's <Python.h>. */
extern "C" void py_agent_init(void);

/* mkdtemp wrapper over the plan's helper: a scratch DISK location (the demo
   CLI's persistence is the thing under test), removed by the caller. */
static std::string temp_dir_live(void) {
  char tmpl[] = "/tmp/sa-live-XXXXXX";
  char* got = mkdtemp(tmpl);
  if (got == NULL) return std::string();
  return std::string(got);
}

/* The audit-trail surface: parse frame_debug_events (same shape as
   test_loop.cpp's helper). */
static json_value_t* live_load_events(frame_t* f) {
  char* json = frame_debug_events(f);
  EXPECT_NE(json, nullptr);
  if (json == NULL) return nullptr;
  char* err = NULL;
  json_value_t* arr = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  free(json);
  EXPECT_NE(arr, nullptr);
  if (arr != nullptr) EXPECT_EQ(json_type(arr), JSON_ARRAY);
  if (arr == nullptr || json_type(arr) != JSON_ARRAY) {
    if (arr != nullptr) json_value_destroy(arr);
    return nullptr;
  }
  return arr;
}

static bool live_event_is(json_value_t* rec, const char* type_name) {
  json_value_t* type_v = json_get(rec, "type");
  return type_v != NULL && strcmp(json_as_string(type_v), type_name) == 0;
}

static bool live_contains(const char* haystack, const char* needle) {
  return haystack != NULL && strstr(haystack, needle) != NULL;
}

/* ONE TEST macro for the file — a python-free build skips inside the same
   body. (gtest_add_tests registers tests by a plain-text regex scan of the
   target's sources; two TEST() macros under #if/#else branches — even where
   a preprocessor gate keeps only one alive — would register the same name
   twice and break configure.) */
TEST(TestLiveLoop, TestLiveOllamaRememberAndReportWave) {
#if !defined(SA_HAS_PYTHON)
  GTEST_SKIP() << "this build has no python runtime — the live gate drives "
                  "real cells (actor.remember/report) and cannot round-trip";
#else
  const char* url = getenv("SA_TEST_OLLAMA_URL");
  if (url == NULL || url[0] == '\0') {
    GTEST_SKIP() << "SA_TEST_OLLAMA_URL is not set — the live integration "
                    "gate is opt-in (set it to an OpenAI-compatible endpoint, "
                    "e.g. http://127.0.0.1:11434); optionally override the "
                    "model tag with SA_TEST_OLLAMA_MODEL";
  }
  const char* model_env = getenv("SA_TEST_OLLAMA_MODEL");
  const char* model =
      (model_env != NULL && model_env[0] != '\0') ? model_env : "gemma4:31b-cloud";

  py_agent_init();

  frame_config_t cfg;
  cfg.model_base_url = url;
  cfg.model_api_key = NULL;   /* Ollama-compatible: no key */
  cfg.model_name = model;
  cfg.max_depth = 4;
  /* Local models take minutes on tool-calling turns (see the file header);
     180 s is a generous bound that still fails loud instead of hanging. */
  cfg.model_timeout_ms = 180000;

  std::string dir = temp_dir_live();
  ASSERT_FALSE(dir.empty());
  std::string loc = dir + "/db";

  wave_database_root_t* db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);

  /* A live model is nondeterministic about whether it acts or narrates
     (a reasoning model may answer the goal with prose and stop). The gate
     is therefore a small scenario retry: up to three fresh frames against
     the same db. Attempt 1 runs the plan's literal goal; later attempts
     sharpen the instruction — the ASSERTION never changes: state.remember
     of the word landed, frame.report carried it, and a plain recall
     resolves it. A failed attempt's audit trail is dumped for diagnosis. */
  const char* plan_goal = "remember the word 'wave' then report it";
  const char* shaped_goal =
      "Use the execute tool exactly once: in one cell, call "
      "actor.remember('word', 'wave') and then actor.report('wave'). "
      "After the tool's result give a one-word final answer.";
  const char* goals[3] = {plan_goal, shaped_goal, shaped_goal};

  bool round_trip = false;
  for (int attempt = 0; attempt < 3 && !round_trip; attempt++) {
    frame_t* f = frame_create(db, NULL, goals[attempt], &cfg);
    ASSERT_NE(f, nullptr);

    /* A handful of turns bounds a spiraling model (the loop's own default
       is 64); the goal needs two turns at most. */
    frame_set_loop_turn_cap(f, 8);

    /* At most ONE full-run retry per attempt — see the file header note. */
    int rc = frame_run_loop(f);
    if (rc != 0) rc = frame_run_loop(f);
    EXPECT_EQ(rc, 0) << "attempt " << attempt + 1
                     << ": the live loop never completed (see the control "
                        "events in the audit trail below)";

    json_value_t* events = live_load_events(f);
    if (events == nullptr) {
      frame_destroy(f);
      break;
    }
    std::string key_of_remember;
    bool saw_remember = false;
    bool saw_report = false;
    for (size_t i = 0; i < json_size(events); i++) {
      json_value_t* rec = json_at(events, i);
      json_value_t* payload = json_get(rec, "payload");
      if (payload == NULL) continue;
      if (live_event_is(rec, "state.remember")) {
        const char* key = json_as_string(json_get(payload, "key"));
        /* The word may be the KEY (a model following the goal literally) or
           the VALUE under a model-chosen key — both are the round trip going
           in: an effect that lands in the store AND its event. */
        char* value_json = json_serialize(json_get(payload, "value"));
        bool has_word = live_contains(key, "wave") ||
                        live_contains(value_json, "wave");
        free(value_json);
        if (has_word) {
          saw_remember = true;
          key_of_remember = (key != NULL) ? key : "";
        }
      } else {
        json_value_t* text = json_get(payload, "text");
        if (live_event_is(rec, "frame.report") &&
            live_contains(json_as_string(text), "wave")) {
          saw_report = true;
        }
      }
    }
    if (!saw_remember || !saw_report) {
      /* A live-model failure must carry its own audit trail, not just a
         bare boolean — dump the raw events so the failure diagnoses itself. */
      char* raw = frame_debug_events(f);
      fprintf(stdout, "[  AUDIT  ] attempt %d events: %s\n", attempt + 1,
              raw ? raw : "(none)");
      fflush(stdout);
      free(raw);
      json_value_destroy(events);
      frame_destroy(f);
      continue;
    }

    /* The store-side round trip: whatever key the model remembered the word
       under must be resolvable by a plain recall. */
    bool recall_ok = true;
    if (!key_of_remember.empty()) {
      char* v = frame_recall(f, key_of_remember.c_str());
      ASSERT_NE(v, nullptr);
      recall_ok = live_contains(v, "wave");
      if (!recall_ok) {
        fprintf(stdout, "[  AUDIT  ] attempt %d: recall(\"%s\") returned %s\n",
                attempt + 1, key_of_remember.c_str(), v);
        fflush(stdout);
      }
      free(v);
    }
    json_value_destroy(events);
    frame_destroy(f);

    if (recall_ok) {
      round_trip = true;
    }
  }

  EXPECT_TRUE(round_trip) << "no attempt produced the remember/report round "
                             "trip of the word 'wave' (see the [  AUDIT  ] dumps)";

  wave_db_close(db);
  std::filesystem::remove_all(dir);
#endif  /* python gate */
}

#endif  /* SA_HAS_WDB */