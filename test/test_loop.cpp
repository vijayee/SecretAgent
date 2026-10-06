//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
extern "C" {
#include "../src/Frame/frame.h"
#include "../src/Frame/frame_messages.h"
#include "../src/Frame/model.h"
#include "../src/Frame/loop.h"
#include "../src/Frame/persona.h"          /* the compose expectations' pure API */
#include "../src/Frame/persona_records.h"  /* the shipped hammer record */
#include "../src/Scheduler/scheduler.h"   /* the pooled tests' pool API */
#include "../src/Util/json.h"
#include "../src/Util/allocator.h"
}

#if defined(SA_HAS_WDB) && defined(SA_HAS_PYTHON)
/* CPython include dirs live on the secretagent target's PRIVATE scope; this
   file reaches py_agent_init through a bare extern declaration instead of
   pulling py_agent.h (which would demand <Python.h> here). The call is the
   suite's re-mount idiom from test_py_agent.cpp: idempotent, and it
   re-registers the bridge reply sink after other suites' demounts. */
extern "C" void py_agent_init(void);
#endif

/* The WDB gate mirrors test_frame.cpp: the frame store API only exists while
   the WaveDB-backed build is compiled in. The python gate is nested where
   cells really run (both WDB and PYTHON). */
#if defined(SA_HAS_WDB)

/* The engine's private contract (frame_internal.h): the mid-turn-destroy pin
   below drives the frame's round-trip surface BY HAND (_frame_pump) to reach
   the submit boundary (phase=MODEL, one slot held) deterministically and
   single-threaded. */
extern "C" {
#include "../src/Frame/frame_internal.h"
}

static frame_config_t test_config(void) {
  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));   /* additive fields (pool, timeout) default sensibly */
  cfg.model_base_url = NULL;
  cfg.model_api_key = NULL;
  cfg.model_name = "unused";
  cfg.max_depth = 4;
  return cfg;
}

/* NOTE on the guard idiom: C preprocessor macros cannot be joined with `&&` —
   the plan's listing note is honored by writing the real form below:
     #if defined(SA_HAS_WDB) && defined(SA_HAS_PYTHON)   */

/* The audit-trail surface: load frame_debug_events and search it. */
static json_value_t* load_events(frame_t* f) {
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

static bool event_is(json_value_t* rec, const char* type_name) {
  json_value_t* type_v = json_get(rec, "type");
  return type_v != NULL && strcmp(json_as_string(type_v), type_name) == 0;
}

/* --- the turn-envelope tests' readers (the records are the frozen shape:
       {"seq","type","frame","corr","at","cause","payload"}) --------------- */

static long long rec_seq(json_value_t* rec) {
  json_value_t* seq = json_get(rec, "seq");
  return (seq != NULL) ? (long long)json_as_int(seq) : -1;
}

static json_value_t* payload_of(json_value_t* rec) {
  return json_get(rec, "payload");
}

static size_t count_type(json_value_t* events, const char* type_name) {
  size_t n = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), type_name)) n++;
  }
  return n;
}

static long long first_seq_of(json_value_t* events, const char* type_name) {
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, type_name)) return rec_seq(rec);
  }
  return -1;
}

/* The FIRST record of `type_name` whose payload `turn` matches (NULL when
   absent — the callers assert on it). */
static json_value_t* life_record_of_turn(json_value_t* events,
                                         const char* type_name,
                                         long long turn) {
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, type_name)) continue;
    json_value_t* payload = payload_of(rec);
    json_value_t* turn_v = (payload != NULL) ? json_get(payload, "turn") : NULL;
    if (turn_v != NULL && (long long)json_as_int(turn_v) == turn) return rec;
  }
  return NULL;
}

/* Count the records of `type_name` whose payload `turn` matches. */
static size_t count_turn(json_value_t* events, const char* type_name,
                         long long turn) {
  size_t n = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, type_name)) continue;
    json_value_t* payload = payload_of(rec);
    json_value_t* turn_v = (payload != NULL) ? json_get(payload, "turn") : NULL;
    if (turn_v != NULL && (long long)json_as_int(turn_v) == turn) n++;
  }
  return n;
}

/* The FIRST msg.append record with the given role + content (NULL when
   absent). */
static json_value_t* find_msg_append(json_value_t* events, const char* role,
                                     const char* content) {
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "msg.append")) continue;
    json_value_t* payload = payload_of(rec);
    json_value_t* role_v = (payload != NULL) ? json_get(payload, "role") : NULL;
    json_value_t* content_v =
        (payload != NULL) ? json_get(payload, "content") : NULL;
    if (role_v != NULL && content_v != NULL &&
        strcmp(json_as_string(role_v), role) == 0 &&
        strcmp(json_as_string(content_v), content) == 0) {
      return rec;
    }
  }
  return NULL;
}

/* The turn.end's reason kind ("" when the record is malformed — the caller
   asserts against a kind name; reading the kind directly pins "the reason
   is never NULL" without an invented default). */
static std::string turn_end_kind(json_value_t* rec) {
  json_value_t* payload = payload_of(rec);
  json_value_t* reason = (payload != NULL) ? json_get(payload, "reason") : NULL;
  json_value_t* kind = (reason != NULL) ? json_get(reason, "kind") : NULL;
  return (kind != NULL) ? std::string(json_as_string(kind)) : std::string();
}

#if defined(SA_HAS_PYTHON)

/* Scripted model: pops pre-queued raw OpenAI-shaped replies; when the queue
   empties it answers with `fallback` (the turn-cap test's always-tool-calling
   model), or fails loud when that is empty too. Each call CAPTURES the
   serialized messages array (the derived-context surface; the restart test
   asserts the replayed msg.append reached it), and optionally appends a user
   steering message at a chosen 1-based call index — the loop runs on this
   SAME thread, so "steering between turns" is exactly that: a message
   appended while the loop sits at the model boundary picks up on the next
   derive. */
typedef struct scripted_model_t {
  model_backend_t base;
  std::vector<std::string>* replies;
  std::vector<std::string> captured;
  frame_t* steer_frame;
  const char* steer_text;
  unsigned steer_on;
  std::string* fallback;
} scripted_model_t;

/* Decode ONE canned completion body into a model_reply_t (the same shapes
   model.c decodes: message.content string-or-absent; tool_calls[0].function
   .arguments as a JSON string containing the argument object). */
static int scripted_decode(const std::string& body, model_reply_t** reply_out,
                           char** error_out) {
  char* err = NULL;
  json_value_t* root = json_parse(body.c_str(), body.size(), &err);
  if (err != NULL) free(err);
  if (root == NULL) {
    *error_out = strdup("scripted model: body is not valid JSON");
    return -1;
  }
  json_value_t* choices = json_get(root, "choices");
  json_value_t* choice = (choices != NULL && json_type(choices) == JSON_ARRAY)
                             ? json_at(choices, 0) : NULL;
  json_value_t* message =
      (choice != NULL && json_type(choice) == JSON_OBJECT) ? json_get(choice, "message")
                                                           : NULL;
  if (message == NULL) {
    json_value_destroy(root);
    *error_out = strdup("scripted model: no message in choices[0]");
    return -1;
  }
  model_reply_t* r = (model_reply_t*)get_clear_memory(sizeof(model_reply_t));

  json_value_t* content = json_get(message, "content");
  if (content == NULL || json_type(content) == JSON_NULL) {
    r->content = strdup("");
  } else {
    r->content = strdup(json_as_string(content));
  }

  json_value_t* calls = json_get(message, "tool_calls");
  if (calls != NULL && json_type(calls) == JSON_ARRAY && json_size(calls) > 0) {
    json_value_t* fn = json_get(json_at(calls, 0), "function");
    json_value_t* args = (fn != NULL) ? json_get(fn, "arguments") : NULL;
    json_value_t* args_obj = args;
    json_value_t* parsed_args = NULL;
    if (args != NULL && json_type(args) == JSON_STRING) {
      char* aerr = NULL;
      const char* args_text = json_as_string(args);
      parsed_args = json_parse(args_text, strlen(args_text), &aerr);
      if (aerr != NULL) free(aerr);
      if (parsed_args == NULL) {
        json_value_destroy(root);
        model_reply_destroy(r);
        *error_out = strdup("scripted model: arguments string is not JSON");
        return -1;
      }
      args_obj = parsed_args;
    }
    if (args_obj == NULL || json_type(args_obj) != JSON_OBJECT) {
      json_value_destroy(root);
      model_reply_destroy(r);
      *error_out = strdup("scripted model: arguments are neither string nor object");
      return -1;
    }
    json_value_t* code = json_get(args_obj, "code");
    if (code == NULL || json_type(code) != JSON_STRING) {
      if (parsed_args != NULL) json_value_destroy(parsed_args);
      json_value_destroy(root);
      model_reply_destroy(r);
      *error_out = strdup("scripted model: no string `code` in arguments");
      return -1;
    }
    r->tool_code = strdup(json_as_string(code));
    if (parsed_args != NULL) json_value_destroy(parsed_args);
  }
  json_value_destroy(root);
  *reply_out = r;
  return 0;
}

static int scripted_complete(void* self, json_value_t* messages, json_value_t* tools,
                             char** raw_out, model_reply_t** reply_out,
                             char** error_out) {
  (void)tools;
  (void)raw_out;
  *reply_out = NULL;
  *error_out = NULL;
  scripted_model_t* sm = (scripted_model_t*)self;

  /* The derived-context surface: serialize what the model saw, every call. */
  char* seen = json_serialize(messages);
  if (seen != NULL) {
    sm->captured.emplace_back(seen);
    free(seen);
  }

  /* Steering hook: run the loop on THIS thread — appending a user message
     at the model boundary is exactly "steering between turns" here. */
  if (sm->steer_frame != NULL && sm->steer_text != NULL &&
      sm->captured.size() == (size_t)sm->steer_on) {
    EXPECT_EQ(frame_append_msg(sm->steer_frame, "user", sm->steer_text), 0);
  }

  std::string body;
  if (!sm->replies->empty()) {
    body = sm->replies->front();
    sm->replies->erase(sm->replies->begin());
  } else if (sm->fallback != NULL) {
    body = *sm->fallback;
  } else {
    *error_out = strdup("scripted model: queue empty (the loop issued an "
                        "unexpected extra turn)");
    return -1;
  }
  return scripted_decode(body, reply_out, error_out) == 0 ? 0 : -1;
}

TEST(TestLoop, TestScriptedLoopRunsCellAndCompletes) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "do a thing", &cfg);
  ASSERT_NE(f, nullptr);

  /* turn 1: the model calls execute with a cell that remembers + reports;
     turn 2 ("all done") stays UNCONSUMED — report semantics mean the report
     ends the top frame and the loop stops before ever asking the model
     again. The leftover is asserted below. */
  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('n', 7)\\nactor.report('done: ' + str(7))\"}"}}]}}]})json";
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","content":"all done"}}]})json";
  std::vector<std::string> replies = {turn1, turn2};

  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;

  /* The scripted backend MUST be set before first use: the default http
     backend constructs only when no override is present. */
  frame_set_model_backend(f, &sm.base);

  EXPECT_EQ(frame_run_loop(f), 0);

  /* The loop ended AT THE REPORT: turn 2 was never requested (a report is a
     completion declaration at every depth — see loop.c's REPORT SEMANTICS). */
  EXPECT_EQ(replies.size(), 1u);

  /* recall n == 7: remember('n', 7) coerced the int through repr to the JSON
     value 7; the raw stored JSON text is "7". */
  char* n = frame_recall(f, "n");
  ASSERT_NE(n, nullptr);
  EXPECT_STREQ(n, "7");
  free(n);

  /* report at the TOP frame marks done (completion declaration — the decided
     semantics; see loop.c's REPORT SEMANTICS note). */
  EXPECT_EQ(frame_is_done(f), 1);

  /* Audit trail: the turn envelope rides the engine's existing batches —
     turn.start (the turn entry) + step.start riding the cell.run audit
     batch + the cell's own records + the cell.result batch carrying
     step.end + turn.end. */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  static const char* expected_order[] = {
      "turn.start", "step.start", "cell.run", "state.remember",
      "frame.report", "cell.result", "step.end", "turn.end"};
  ASSERT_EQ(json_size(events), 8u);
  for (size_t i = 0; i < 8; i++) {
    EXPECT_TRUE(event_is(json_at(events, i), expected_order[i]))
        << "record " << i << " was not " << expected_order[i];
  }

  json_value_t* remember_payload = json_get(json_at(events, 3), "payload");
  ASSERT_NE(remember_payload, nullptr);
  EXPECT_STREQ(json_as_string(json_get(remember_payload, "key")), "n");
  EXPECT_EQ(json_as_int(json_get(remember_payload, "value")), 7);
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestSteeringBetweenTurnsReordersCells) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "steer me", &cfg);
  ASSERT_NE(f, nullptr);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('a', 11)\"}"}}]}}]})json";
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('b', 22)\"}"}}]}}]})json";
  std::string turn3 =
      R"json({"choices":[{"message":{"role":"assistant","content":"both cells ran"}}]})json";
  std::vector<std::string> replies = {turn1, turn2, turn3};

  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = f;
  sm.steer_text = "steering: also make 22";
  sm.steer_on = 2;   /* steer at the SECOND model call */
  sm.fallback = NULL;

  frame_set_model_backend(f, &sm.base);
  EXPECT_EQ(frame_run_loop(f), 0);

  char* a = frame_recall(f, "a");
  ASSERT_NE(a, nullptr);
  EXPECT_STREQ(a, "11");
  free(a);
  char* b = frame_recall(f, "b");
  ASSERT_NE(b, nullptr);
  EXPECT_STREQ(b, "22");
  free(b);

  /* Turn order on the audit trail: both cells ran, and the steering user
     message sits BETWEEN them (cell.run #1 ... msg.append(user) ... cell.run
     #2), with the final assistant content last. */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  std::vector<std::string> order;
  std::vector<size_t> runs;
  size_t steer_at = (size_t)-1;
  size_t n_run = 0, n_result = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "cell.run")) {
      n_run++;
      runs.push_back(i);
    } else if (event_is(rec, "cell.result")) {
      n_result++;
    } else if (event_is(rec, "msg.append")) {
      json_value_t* p = json_get(rec, "payload");
      if (p != NULL &&
          strcmp(json_as_string(json_get(p, "role")), "user") == 0 &&
          strcmp(json_as_string(json_get(p, "content")),
                 "steering: also make 22") == 0) {
        steer_at = i;
      }
    }
  }
  EXPECT_EQ(n_run, 2u);
  EXPECT_EQ(n_result, 2u);
  EXPECT_NE(steer_at, (size_t)-1);
  EXPECT_EQ(runs.size(), 2u);
  if (steer_at != (size_t)-1 && runs.size() == 2u) {
    EXPECT_GT(steer_at, runs[0]) << "steering happened after cell 1";
    EXPECT_LT(steer_at, runs[1]) << "the loop re-derived before cell 2";
  }
  json_value_destroy(events);

  EXPECT_EQ(frame_is_done(f), 1);
  frame_destroy(f);
  wave_db_close(db);
}

/* The write verb's durable half (spec §1): the cell's emit becomes ONE
   stored `emit` record in the frame's own events stream, AND the derive
   renders it back to the model as a bounded `emit: <text>` line — the verb
   is real in both directions. */
TEST(TestLoop, TestEmitIsDurableAndProjected) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "emit a part", &cfg);
  ASSERT_NE(f, nullptr);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.emit('emitted: part one')\"}"}}]}}]})json";
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","content":"all done"}}]})json";
  std::vector<std::string> replies = {turn1, turn2};

  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;

  frame_set_model_backend(f, &sm.base);
  EXPECT_EQ(frame_run_loop(f), 0);
  EXPECT_EQ(replies.size(), 0u);

  /* Durable half: exactly one emit record in the frame's events stream, its
     payload carrying the text verbatim. */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(count_type(events, "emit"), 1u);
  json_value_t* emit_rec = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "emit")) {
      emit_rec = json_at(events, i);
      break;
    }
  }
  ASSERT_NE(emit_rec, nullptr);
  json_value_t* emit_payload = payload_of(emit_rec);
  ASSERT_NE(emit_payload, nullptr);
  json_value_t* emit_text = json_get(emit_payload, "text");
  ASSERT_NE(emit_text, nullptr);
  EXPECT_STREQ(json_as_string(emit_text), "emitted: part one");
  json_value_destroy(events);

  /* Projected half: the turn-2 derive (the model's SECOND view) carries the
     emitted line. The turn-1 derive precedes the cell, so only capture [1]
     can show it. */
  ASSERT_EQ(sm.captured.size(), 2u);
  EXPECT_NE(sm.captured[1].find("emit: emitted: part one"), std::string::npos)
      << "the derive did not project the emit: " << sm.captured[1];

  frame_destroy(f);
  wave_db_close(db);
}

/* agent.keys(scope) (spec §3): the pulled-forward inspect member. A cell's
   keys('local') / keys('ctx') list ONLY the frame's own subtree keys —
   ascending names, never values — and an unknown scope is the fail-loud
   corr-matched refusal whose model-visible shape is None.

   Shape note (report ends the top frame's loop, so the listing turn cannot
   report): turn 1 computes the three listing texts as assignments; turn 2
   reports all three — the loop completes on THAT report. */
TEST(TestLoop, TestKeysListsOwnStateKeysOnly) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "list my keys", &cfg);
  ASSERT_NE(f, nullptr);

  /* The keys the turn will list, seeded through the direct sync store API
     BEFORE the scripted turn; the writes land out of order on purpose —
     the listing must come back sorted. */
  ASSERT_EQ(frame_remember_local(f, "gamma", "\"g\""), 0);
  ASSERT_EQ(frame_remember_local(f, "alpha", "\"a\""), 0);
  ASSERT_EQ(frame_remember_ctx(f, "ctx-two", "\"2\""), 0);
  ASSERT_EQ(frame_remember_ctx(f, "ctx-one", "\"1\""), 0);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nls = 'local: ' + str(actor.keys('local'))\\ncs = 'ctx: ' + str(actor.keys('ctx'))\\nbs = 'bogus: ' + str(actor.keys('bogus'))\"}"}}]}}]})json";
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"actor.report(ls + ', ' + cs + ', ' + bs)\"}"}}]}}]})json";
  std::vector<std::string> replies = {turn1, turn2};

  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;

  frame_set_model_backend(f, &sm.base);
  EXPECT_EQ(frame_run_loop(f), 0);
  EXPECT_EQ(replies.size(), 0u) << "both turns were consumed";

  /* THE assertion surface: the report event's text pins the sorted local and
     ctx listings (own-subtree keys only) AND the refusal's model-visible
     shape (None) in one place. */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(count_type(events, "frame.report"), 1u);
  json_value_t* report_rec = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "frame.report")) {
      report_rec = json_at(events, i);
      break;
    }
  }
  ASSERT_NE(report_rec, nullptr);
  json_value_t* report_payload = payload_of(report_rec);
  ASSERT_NE(report_payload, nullptr);
  json_value_t* report_text = json_get(report_payload, "text");
  ASSERT_NE(report_text, nullptr);
  std::string text(json_as_string(report_text));
  EXPECT_NE(text.find("local: ['alpha', 'gamma']"), std::string::npos)
      << "report text: " << text;
  EXPECT_NE(text.find("ctx: ['ctx-one', 'ctx-two']"), std::string::npos)
      << "report text: " << text;
  EXPECT_NE(text.find("bogus: None"), std::string::npos)
      << "the unknown scope's refusal answers None to the model; report text: "
      << text;
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

/* The over-cap keys listing (the budget table's KEYS_MAX at the store
   boundary): more than SA_BUDGET_KEYS_MAX names in the frame's own subtree —
   the listing clips at 256 with the "[budget: keys truncated]" marker record
   APPENDED (the reply is still one quoted array: 257 elements render —
   256 names + the marker).

   Shape note (same as TestKeysListsOwnStateKeysOnly): a report ends the top
   frame's loop, so the listing turn's result is carried by a second turn
   that reports. */
TEST(TestLoop, TestKeysListingTruncationCarriesTheMarker) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "keys truncation", &cfg);
  ASSERT_NE(f, nullptr);

  /* Seed 260 local keys: k000..k259 — lexicographic, sort-proven, seeded
     BEFORE the scripted turn. The store's scan window is
     SA_BUDGET_KEYS_MAX + 1 = 257, and the FORWARD walk is the true
     lexicographic first-N (spec §3): the reply carries the LOWEST 257
     (k000..k256); the reply router keeps the first 256 (k000..k255) and
     'k256' is what the clip drops — the over-cap count is what rides the
     truncation tell. */
  char key[8];
  for (int i = 0; i < 260; i++) {
    snprintf(key, sizeof(key), "k%03d", i);
    ASSERT_EQ(frame_remember_local(f, key, "\"0\""), 0);
  }

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nls = 'local: ' + str(actor.keys('local'))\"}"}}]}}]})json";
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"actor.report(ls)\"}"}}]}}]})json";
  std::vector<std::string> replies = {turn1, turn2};

  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;

  frame_set_model_backend(f, &sm.base);
  EXPECT_EQ(frame_run_loop(f), 0);
  EXPECT_EQ(replies.size(), 0u);

  /* THE pin: the report's text carries the clipped listing — the first two
     sorted names, the marker as the LAST array element, and exactly 257
     array elements (256 names + the marker = 256 "', '" separators). */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(count_type(events, "frame.report"), 1u);
  json_value_t* report_rec = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "frame.report")) {
      report_rec = json_at(events, i);
      break;
    }
  }
  ASSERT_NE(report_rec, nullptr);
  json_value_t* report_payload = payload_of(report_rec);
  ASSERT_NE(report_payload, nullptr);
  json_value_t* report_text = json_get(report_payload, "text");
  ASSERT_NE(report_text, nullptr);
  std::string text(json_as_string(report_text));
  size_t ls_at = text.find("local: [");
  ASSERT_NE(ls_at, std::string::npos) << "report text: " << text;
  /* The reported string IS the listing (turn 2 reports `ls` alone), so the
     suffix from "local: [" is the whole rendered array — no mid-text "]" cut
     (the marker itself carries one). */
  std::string listing = text.substr(ls_at);
  EXPECT_NE(listing.find("['k000', 'k001'"), std::string::npos)
      << "report text: " << text;
  EXPECT_NE(listing.find("'[budget: keys truncated]']"),
            std::string::npos)
      << "the marker is the listing's last element; report text: " << text;
  size_t separators = 0;
  for (size_t p = listing.find("', '"); p != std::string::npos;
       p = listing.find("', '", p + 1)) {
    separators++;
  }
  EXPECT_EQ(separators, 256u)
      << "the listing renders 257 elements (256 names + the marker)";
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestTurnLimitFailsLoud) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "spiral forever", &cfg);
  ASSERT_NE(f, nullptr);

  /* Always a tool call — one BYTE-DISTINCT cell per turn, exactly the cap's
     count: three byte-IDENTICAL cells would now trip the doom-loop breaker
     on the third (the breaker refuses the threshold-th identical call
     BEFORE the cap can), and this test pins the cap's own refusal kind. */
  std::vector<std::string> replies;
  const char* codes[3] = {"pass", "pass  # cycle 2", "pass  # cycle 3"};
  for (int i = 0; i < 3; i++) {
    char args[256];
    snprintf(args, sizeof(args),
             R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
             R"json({"type":"function","function":{"name":"execute",)json"
             R"json("arguments":"{\"code\":\"%s\"}"}}]}}]})json", codes[i]);
    replies.push_back(args);
  }
  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;

  frame_set_loop_turn_cap(f, 3);
  frame_set_model_backend(f, &sm.base);

  EXPECT_NE(frame_run_loop(f), 0);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  size_t n_run = 0;
  bool saw_turn_limit = false;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "cell.run")) n_run++;
    if (event_is(rec, "control")) {
      json_value_t* p = json_get(rec, "payload");
      if (p != NULL && json_get(p, "kind") != NULL &&
          strcmp(json_as_string(json_get(p, "kind")), "turn-limit") == 0) {
        saw_turn_limit = true;
      }
    }
  }
  EXPECT_EQ(n_run, 3u);
  EXPECT_TRUE(saw_turn_limit);
  EXPECT_EQ(frame_is_done(f), 0) << "the cap is a failure, not completion";
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestChildTurnLimitFailsIntoTheParent) {
  /* Task 6: the CHILD engine hits its own turn cap — control "turn-limit"
     stays in the child's log with the exact kind; the child's status lands
     DONE (no parent ever awaits a zombie); ONE frame.report whose text
     carries the failure binds into the parent's log; the bind reply posts
     FRM_CHILD_REPORT{failed=1} and the parent's bookkeeping (live_children
     --, the folded join) drains. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* parent = frame_create(db, NULL, "parent of failures", &cfg);
  ASSERT_NE(parent, nullptr);
  /* The parent's engine never starts — the spawn here is the admission-only
     shape (the START branch needs a live parent engine), so this test holds
     and drives the child itself. */
  frame_t* child = frame_spawn(parent, "spiral forever", NULL);
  ASSERT_NE(child, nullptr);

  /* Always a tool call — the fallback answers every turn; the cap ends the
     child after its first full turn. */
  std::string always_tool_body =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"pass\"}"}}]}}]})json";
  std::vector<std::string> replies = {};
  scripted_model_t am = {};
  am.base.complete = scripted_complete;
  am.replies = &replies;
  am.steer_frame = NULL;
  am.steer_text = NULL;
  am.steer_on = 0;
  am.fallback = &always_tool_body;
  frame_set_model_backend(child, &am.base);   /* BEFORE the child's turn 1 */
  frame_set_loop_turn_cap(child, 1);

  EXPECT_EQ(frame_run_loop(child), 1) << "the engine failed, loudly";
  EXPECT_EQ(frame_is_done(child), 1)
      << "a failed CHILD is done — never a zombie a parent awaits";
  EXPECT_EQ(frame_is_done(parent), 0)
      << "the parent is a separate engine; unbuilt here";

  /* The binding rode the store inside the driver's pumps (the terminate
     composed the bind; the parent's actor committed it; the reply routed
     back). The RESIDUAL child-notify → parent-bookkeeping chain drains in
     the Task-2 pump order — frame, ancestors, inline store. */
  wave_db_pump(db);
  actor_run(_frame_actor(child), ACTOR_BATCH_SIZE);   /* the bind reply routes */
  actor_run(_frame_actor(parent), ACTOR_BATCH_SIZE);  /* the parent's bookkeeping:
                                        live_children (loud zero-count — the
                                        engine-less caller never counted this
                                        spawn), the folded join, resume check */
  wave_db_pump(db);              /* the folded join commits */
  actor_run(_frame_actor(parent), ACTOR_BATCH_SIZE);  /* nothing left (the join
                                        is fire-and-post) */

  /* The parent's log holds ONE frame.report whose text is the failure. */
  json_value_t* events = load_events(parent);
  ASSERT_NE(events, nullptr);
  size_t n_report = 0, n_join = 0;
  std::string report_text;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "frame.report")) {
      n_report++;
      report_text = json_as_string(json_get(json_get(rec, "payload"), "text"));
    }
    if (event_is(rec, "frame.join")) n_join++;
  }
  EXPECT_EQ(n_report, 1u) << "ONE failure report per child, not one per turn";
  EXPECT_NE(report_text.find("turn-limit"), std::string::npos)
      << "the bound report's text carries the failure kind";
  EXPECT_EQ(n_join, 1u) << "the failure ALSO folds the join into the resume";
  json_value_destroy(events);

  /* The child's own log keeps the control event with the exact kind (+
     its composed frame.report record, which rode the same bind batch). */
  json_value_t* child_events = load_events(child);
  ASSERT_NE(child_events, nullptr);
  bool saw_turn_limit = false;
  bool saw_child_report = false;
  for (size_t i = 0; i < json_size(child_events); i++) {
    json_value_t* rec = json_at(child_events, i);
    if (event_is(rec, "control")) {
      json_value_t* p = json_get(rec, "payload");
      if (p != NULL && json_get(p, "kind") != NULL &&
          strcmp(json_as_string(json_get(p, "kind")), "turn-limit") == 0) {
        saw_turn_limit = true;
      }
    }
    if (event_is(rec, "frame.report")) {
      saw_child_report = true;
      EXPECT_STREQ(json_as_string(json_get(json_get(rec, "payload"), "text")),
                   "turn-limit: model turn budget exhausted");
    }
  }
  EXPECT_TRUE(saw_turn_limit) << "the child's own log keeps the exact kind";
  EXPECT_TRUE(saw_child_report) << "the child composed its own report record";
  json_value_destroy(child_events);

  frame_destroy(child);
  frame_destroy(parent);
  wave_db_close(db);
}

TEST(TestLoop, TestSilentEmptyTurnLeavesControlTrail) {
  /* A reply with neither a tool call nor content (reasoning models stop
     like this) must NOT vanish: the turn still lands in the audit trail
     as a control event, then the top frame completes as done. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "go quiet", &cfg);
  ASSERT_NE(f, nullptr);

  std::vector<std::string> replies = {
      R"json({"choices":[{"message":{"role":"assistant","content":""}}]})json"};
  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;

  frame_set_model_backend(f, &sm.base);

  EXPECT_EQ(frame_run_loop(f), 0);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  bool saw_empty_turn = false;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "control")) {
      json_value_t* p = json_get(rec, "payload");
      if (p != NULL && json_get(p, "kind") != NULL &&
          strcmp(json_as_string(json_get(p, "kind")), "empty-turn") == 0) {
        saw_empty_turn = true;
      }
    }
  }
  EXPECT_TRUE(saw_empty_turn) << "an empty stop must be visible in the audit trail";
  EXPECT_EQ(frame_is_done(f), 1) << "the model chose to stop — the frame ends done";
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestEngineRunWritesTheTurnEnvelopeInOrder) {
  /* A full scripted cycle's log shows the envelope in seq order (the plan's
     Task-2 step 1): turn.start → step.start (riding the turn's first durable
     record batch, the cell.run audit) → the turn's records → [cell.result +
     step.end + turn.end completed] — ONE atomic batch per record group, the
     payload fields correct (turns count +1 from 1; step 1 within the turn). */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "envelope cycle", &cfg);
  ASSERT_NE(f, nullptr);

  /* ONE cycle: the cell remembers + reports — the report ends the frame at
     turn 1, so the whole run is one turn's envelope. */
  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('k', 5)\\nactor.report('done: ' + str(5))\"}"}}]}}]})json";
  std::vector<std::string> replies = {turn1};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  EXPECT_EQ(frame_run_loop(f), 0);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);

  /* The log's FIRST record is turn 1's turn.start (the envelope opens the
     log — nothing ran before the engine). */
  json_value_t* ts = life_record_of_turn(events, "turn.start", 1);
  ASSERT_NE(ts, nullptr);
  EXPECT_EQ(rec_seq(ts), 1);

  /* The envelope types in the frozen order, nothing else between groups
     beyond the turn's own records. */
  static const char* order[] = {"turn.start", "step.start", "step.end",
                                "turn.end"};
  long long seqs[4];
  size_t found = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    for (size_t k = 0; k < 4; k++) {
      if (event_is(rec, order[k])) {
        seqs[k] = rec_seq(rec);
        found++;
      }
    }
  }
  EXPECT_EQ(found, 4u) << "one envelope pair, exactly once";
  EXPECT_LT(seqs[0], seqs[1]);
  EXPECT_LT(seqs[1], seqs[2]);
  EXPECT_LT(seqs[2], seqs[3]);

  /* ONE atomic batch per group: step.start rides the cell.run audit batch;
     step.end + turn.end ride the cell.result close batch. */
  long long run_seq = first_seq_of(events, "cell.run");
  long long res_seq = first_seq_of(events, "cell.result");
  ASSERT_GT(run_seq, 0);
  ASSERT_GT(res_seq, 0);
  EXPECT_EQ(seqs[1], run_seq - 1) << "step.start rides the audit batch";
  EXPECT_EQ(seqs[2], res_seq + 1) << "step.end rides the result batch";
  EXPECT_EQ(seqs[3], res_seq + 2) << "turn.end rides the result batch";

  /* The payload fields: turn 1 (+1 from 1), step 1 within the turn, the
     content turn's close reason completed with NO invented text. */
  json_value_t* ss = life_record_of_turn(events, "step.start", 1);
  json_value_t* se = life_record_of_turn(events, "step.end", 1);
  json_value_t* te = life_record_of_turn(events, "turn.end", 1);
  ASSERT_NE(ss, nullptr);
  ASSERT_NE(se, nullptr);
  ASSERT_NE(te, nullptr);
  EXPECT_EQ(json_as_int(json_get(payload_of(ss), "step")), 1);
  EXPECT_EQ(json_as_int(json_get(payload_of(se), "step")), 1);
  json_value_t* reason = json_get(payload_of(te), "reason");
  ASSERT_NE(reason, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "completed");
  EXPECT_EQ(json_get(reason, "text"), nullptr)
      << "a completed close invents no text";

  EXPECT_EQ(frame_is_done(f), 1);
  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestMultiCycleTurnsEachClose) {
  /* Multi-cycle (the plan's Task-2 step 1): two tool cycles + a final
     content turn end the engine — turns 1..k each close: tool-path turns
     via the cell.result batch's [step.end + turn.end {completed}]; the FINAL
     turn via the finish batch's [step.end + turn.end {completed}] riding the
     msg.append. Every turn.end's reason is never NULL. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "multi cycle", &cfg);
  ASSERT_NE(f, nullptr);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"actor.remember('a', 11)\"}"}}]}}]})json";
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"actor.remember('b', 22)\"}"}}]}}]})json";
  std::string turn3 =
      R"json({"choices":[{"message":{"role":"assistant","content":"both cells ran"}}]})json";
  std::vector<std::string> replies = {turn1, turn2, turn3};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  EXPECT_EQ(frame_run_loop(f), 0);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);

  /* Each turn opens exactly once and closes exactly once, completed. */
  for (long long k = 1; k <= 3; k++) {
    json_value_t* ts = life_record_of_turn(events, "turn.start", k);
    json_value_t* te = life_record_of_turn(events, "turn.end", k);
    ASSERT_NE(ts, nullptr) << "turn " << k << " never opened";
    ASSERT_NE(te, nullptr) << "turn " << k << " never closed";
    /* Exactly one open + one close record carries this turn's number — the
       envelope pairs once per turn (no double turn.end from a racy close). */
    EXPECT_EQ(count_turn(events, "turn.start", k), 1u)
        << "turn " << k << " opened twice";
    EXPECT_EQ(count_turn(events, "turn.end", k), 1u)
        << "turn " << k << " closed twice";
    EXPECT_EQ(turn_end_kind(te), "completed")
        << "turn " << k << "'s turn.end reason is never NULL";

    json_value_t* ss = life_record_of_turn(events, "step.start", k);
    json_value_t* se = life_record_of_turn(events, "step.end", k);
    ASSERT_NE(ss, nullptr) << "turn " << k << "'s step never started";
    ASSERT_NE(se, nullptr) << "turn " << k << "'s step never ended";
    EXPECT_EQ(json_as_int(json_get(payload_of(ss), "step")), 1);
    EXPECT_EQ(json_as_int(json_get(payload_of(se), "step")), 1);
    EXPECT_LT(rec_seq(ts), rec_seq(ss));
    EXPECT_LT(rec_seq(ss), rec_seq(se));
    EXPECT_LT(rec_seq(se), rec_seq(te));
  }

  /* The tool turns' pairs ride the audit/result batches: step.start at
     cell.run-1; step.end and turn.end at cell.result+1, +2. */
  std::vector<long long> run_seqs, res_seqs;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "cell.run")) run_seqs.push_back(rec_seq(rec));
    if (event_is(rec, "cell.result")) res_seqs.push_back(rec_seq(rec));
  }
  ASSERT_EQ(run_seqs.size(), 2u);
  ASSERT_EQ(res_seqs.size(), 2u);
  for (size_t k = 0; k < 2; k++) {
    json_value_t* ss = life_record_of_turn(
        events, "step.start", (long long)k + 1);
    json_value_t* se = life_record_of_turn(events, "step.end", (long long)k + 1);
    json_value_t* te =
        life_record_of_turn(events, "turn.end", (long long)k + 1);
    ASSERT_NE(ss, nullptr);
    ASSERT_NE(se, nullptr);
    ASSERT_NE(te, nullptr);
    EXPECT_EQ(rec_seq(ss), run_seqs[k] - 1) << "step.start rides the audit";
    EXPECT_EQ(rec_seq(se), res_seqs[k] + 1) << "step.end rides the result";
    EXPECT_EQ(rec_seq(te), res_seqs[k] + 2) << "turn.end rides the result";
  }

  /* The FINAL content turn's finish batch is ONE atomic group:
     [step.start, msg.append, step.end, turn.end] contiguous. */
  json_value_t* m = find_msg_append(events, "assistant", "both cells ran");
  ASSERT_NE(m, nullptr);
  long long m_seq = rec_seq(m);
  json_value_t* ss3 = life_record_of_turn(events, "step.start", 3);
  json_value_t* se3 = life_record_of_turn(events, "step.end", 3);
  json_value_t* te3 = life_record_of_turn(events, "turn.end", 3);
  ASSERT_NE(ss3, nullptr);
  ASSERT_NE(se3, nullptr);
  ASSERT_NE(te3, nullptr);
  EXPECT_EQ(rec_seq(ss3), m_seq - 1) << "step.start rides the finish batch";
  EXPECT_EQ(rec_seq(se3), m_seq + 1) << "step.end rides the finish batch";
  EXPECT_EQ(rec_seq(te3), m_seq + 2) << "turn.end rides the finish batch";

  EXPECT_EQ(frame_is_done(f), 1);
  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestTurnCapRefusesBeforeTurnEntry) {
  /* The turn cap (the plan's Task-2 step 1): cap=1 — the second cycle's cap
     check fails BEFORE turn entry: the refused turn's turn.start never
     commits; the engine ends failed with the control "turn-limit" event;
     the log's newest lifecycle record is still turn 1's turn.end
     (balanced). `turn-limit` is emitted by NO writer (spec §5) — no
     turn.end carries it. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "one turn only", &cfg);
  ASSERT_NE(f, nullptr);

  std::string fallback =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"pass\"}"}}]}}]})json";
  std::vector<std::string> replies = {};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = &fallback;
  frame_set_loop_turn_cap(f, 1);
  frame_set_model_backend(f, &sm.base);

  EXPECT_NE(frame_run_loop(f), 0) << "the cap is a failure, loud";
  EXPECT_EQ(frame_is_done(f), 0) << "the cap refusal makes no status change";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);

  ASSERT_EQ(count_type(events, "turn.start"), 1u)
      << "only the admitted turn opened";
  EXPECT_EQ(json_as_int(json_get(payload_of(life_record_of_turn(
                                     events, "turn.start", 1)), "turn")), 1);
  ASSERT_EQ(count_type(events, "turn.end"), 1u)
      << "only the admitted turn closed";
  json_value_t* te = life_record_of_turn(events, "turn.end", 1);
  ASSERT_NE(te, nullptr);
  EXPECT_EQ(turn_end_kind(te), "completed");

  ASSERT_EQ(count_type(events, "cell.run"), 1u);
  ASSERT_EQ(count_type(events, "cell.result"), 1u);

  /* The turn-limit control event committed AFTER turn 1's close — and no
     turn.start for the refused turn anywhere. */
  size_t n_limit = 0;
  long long limit_seq = -1;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "control")) {
      json_value_t* p = payload_of(rec);
      json_value_t* k = (p != NULL) ? json_get(p, "kind") : NULL;
      if (k != NULL && strcmp(json_as_string(k), "turn-limit") == 0) {
        n_limit++;
        limit_seq = rec_seq(rec);
      }
    }
  }
  ASSERT_EQ(n_limit, 1u);
  ASSERT_GT(limit_seq, 0);
  EXPECT_EQ(limit_seq, rec_seq(te) + 1)
      << "the cap refusal ran right after turn 1's close";

  /* Balanced: turn 1's turn.end is the NEWEST lifecycle record. */
  long long newest_life = -1;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "turn.start") || event_is(rec, "turn.end") ||
        event_is(rec, "step.start") || event_is(rec, "step.end")) {
      newest_life = rec_seq(rec);
    }
  }
  EXPECT_EQ(newest_life, rec_seq(te));

  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

/* The ASYNC scripted backend (the plan's Task-3 listing): submit copies what
   it needs (the serialized messages), then fires the engine's model sink
   EXACTLY ONCE, synchronously within submit (model.h's contract) with ONE
   canned content-only completion — the sink posts the FRM_MODEL_RESULT, the
   shape a production backend's loop-thread completion lands in after
   model.c's relay forwards it. */
typedef struct async_model_t {
  model_backend_t base;
  std::vector<std::string> replies;
  std::vector<std::string> captured;
} async_model_t;

static int async_submit(void* self, json_value_t* messages, json_value_t* tools,
                        model_response_sink_fn on_done, void* on_done_ctx) {
  (void)tools;
  async_model_t* am = (async_model_t*)self;
  char* seen = json_serialize(messages);
  if (seen != NULL) {
    am->captured.emplace_back(seen);
    free(seen);
  }
  if (am->replies.empty()) {
    /* Rejected before any I/O: the sink never fires. */
    return -1;
  }
  std::string body = am->replies.front();
  am->replies.erase(am->replies.begin());
  if (on_done == NULL) return -1;
  char* heap_body = strdup(body.c_str());
  if (heap_body == NULL) return -1;
  on_done(on_done_ctx, 200, heap_body, strlen(heap_body), NULL, NULL);
  return 0;
}

TEST(TestLoop, TestAsyncScriptedBackendDrivesTheSameEngine) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "async drive", &cfg);
  ASSERT_NE(f, nullptr);

  /* The async scripted backend: submit() stashes the serialized messages and
     POSTS an FRM_MODEL_RESULT straight back into the frame's mailbox (the
     shape a production backend's loop-thread completion lands in after the
     model.c relay forwards it) with ONE canned content-only completion. */
  async_model_t am = {};   /* zero-init: the vtable's members are set explicitly below */
  am.base.complete = NULL;                 /* async-only: the engine must take the submit path */
  am.base.submit = async_submit;
  am.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"async done"}}]})json");
  frame_set_model_backend(f, &am.base);

  /* The pump drives the SAME engine; the async step YIELDS at
     FRAME_PHASE_MODEL and the posted result resumes it; the derive itself
     rode the store actor's FRM_STORE_SCAN round trip before that. */
  EXPECT_EQ(frame_run_loop(f), 0);
  ASSERT_EQ(am.captured.size(), 1u);
  EXPECT_NE(am.captured[0].find("Goal: async drive"), std::string::npos);
  EXPECT_EQ(frame_is_done(f), 1);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  bool saw_assistant = false;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "msg.append") &&
        strcmp(json_as_string(json_get(json_get(rec, "payload"), "role")),
               "assistant") == 0 &&
        strcmp(json_as_string(json_get(json_get(rec, "payload"), "content")),
               "async done") == 0) {
      saw_assistant = true;
    }
  }
  EXPECT_TRUE(saw_assistant);
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

/* --- the retry branch (guards spec §2) --------------------------------------
   The retry table drives the engine's model-failure branch: server/transport/
   rate classes retry with the class's backoff (the one-shot delayed-post
   timer owns the wait), overload/overflow fail loud and never repost, and
   the fallback class keeps today's once-only rule. The async scripted
   harness again: each delivery is a (status, body, retry-after) the fake
   backend hands the engine's sink — the sink CONSUMES the headers it is
   passed (the completion's ownership contract: the engine's sink deinits +
   frees the capture), so every header-carrying delivery heap-constructs its
   own capture and hands it over exactly once. Timing pins: FLOORS only
   (timing ceilings are flaky — never one). */

#if defined(SA_HAS_STREAMS)
extern "C" {
#include "../src/Streams/http_headers.h"
}
#endif

typedef struct retry_delivery_t {
  int status;
  const char* body;          /* NULL = no body (the empty response shape) */
  unsigned retry_after_sec;  /* 0 = no retry-after header */
} retry_delivery_t;

typedef struct retry_model_t {
  model_backend_t base;
  std::vector<retry_delivery_t> deliveries;
  size_t next;
  /* The steer-on-model-call machinery, mirrored from scripted_model_t: the
     loop runs on the SAME thread as this submit, so a user message appended
     inside it is exactly "steering between turns" — the steer at a chosen
     1-based call lands at the model boundary and the NEXT derive reads it
     as fresh input. Zero-init = never steers. */
  frame_t* steer_frame;
  const char* steer_text;
  unsigned steer_on;
  unsigned calls;
} retry_model_t;

static int retry_submit(void* self, json_value_t* messages, json_value_t* tools,
                        model_response_sink_fn on_done, void* on_done_ctx) {
  (void)tools;
  (void)messages;
  retry_model_t* rm = (retry_model_t*)self;
  if (rm->next >= rm->deliveries.size() || on_done == NULL) {
    return -1;   /* rejected before any I/O: the sink never fires */
  }
  rm->calls++;
  if (rm->steer_frame != NULL && rm->steer_text != NULL &&
      rm->calls == rm->steer_on) {
    EXPECT_EQ(frame_append_msg(rm->steer_frame, "user", rm->steer_text), 0);
  }
  const retry_delivery_t& d = rm->deliveries[rm->next++];
  http_headers_t* headers = nullptr;
#if defined(SA_HAS_STREAMS)
  if (d.retry_after_sec > 0) {
    headers = (http_headers_t*)malloc(sizeof(http_headers_t));
    EXPECT_NE(headers, nullptr);
    if (headers != nullptr) {
      http_headers_init(headers);
      http_headers_set(headers, "retry-after",
                       std::to_string(d.retry_after_sec).c_str());
    }
  }
#endif
  char* heap_body = (d.body != nullptr) ? strdup(d.body) : nullptr;
  on_done(on_done_ctx, d.status, heap_body,
          (heap_body != nullptr) ? strlen(heap_body) : 0, NULL, headers);
  return 0;
}

static size_t count_control_kind(json_value_t* events, const char* kind) {
  size_t n = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "control")) continue;
    json_value_t* p = (rec != nullptr) ? payload_of(rec) : nullptr;
    json_value_t* k = (p != nullptr) ? json_get(p, "kind") : nullptr;
    if (k != nullptr && strcmp(json_as_string(k), kind) == 0) n++;
  }
  return n;
}

static std::string control_text_of(json_value_t* events, const char* kind) {
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "control")) continue;
    json_value_t* p = payload_of(rec);
    json_value_t* k = (p != nullptr) ? json_get(p, "kind") : nullptr;
    if (k == nullptr || strcmp(json_as_string(k), kind) != 0) continue;
    json_value_t* t = json_get(p, "text");
    return (t != nullptr) ? std::string(json_as_string(t)) : std::string();
  }
  return std::string();
}

TEST(TestLoop, TestServerFailureRetriesWithBackoff) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "retry it", &cfg);
  ASSERT_NE(f, nullptr);

  /* Four 503s (the server class, cap 5) then success: the server class's
     backoff steps for the four retries are 0/250/500/1000 ms — the run
     cannot complete faster than the waits. */
  retry_model_t rm = {};   /* zero-init: the vtable's members set explicitly */
  rm.base.complete = NULL;   /* async-only: the engine takes the submit path */
  rm.base.submit = retry_submit;
  std::string done_body =
      R"json({"choices":[{"message":{"role":"assistant","content":"recovered"}}]})json";
  rm.deliveries = {{503, nullptr, 0},
                   {503, nullptr, 0},
                   {503, nullptr, 0},
                   {503, nullptr, 0},
                   {200, done_body.c_str(), 0}};
  frame_set_model_backend(f, &rm.base);

  auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(frame_run_loop(f), 0) << "the run completed after four retries";
  auto t1 = std::chrono::steady_clock::now();
  long long elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  EXPECT_GE(elapsed_ms, 1000)
      << "the server class's backoff waits ran (a floor, never a ceiling)";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(count_control_kind(events, "model-error"), 4u)
      << "one control per retry, exactly";
  EXPECT_EQ(count_control_kind(events, "model-error-final"), 0u);
  EXPECT_EQ(rm.next, 5u) << "the backend saw five deliveries total";
  EXPECT_EQ(frame_is_done(f), 1);

  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestRateFailureHonorsRetryAfter) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "slow down", &cfg);
  ASSERT_NE(f, nullptr);

  /* Two 429s whose retry-after asks 2 s, then success: the rate class's
     backoff honors the provider's pacing — 2 x 2000 ms of waiting. */
  retry_model_t rm = {};
  rm.base.complete = NULL;
  rm.base.submit = retry_submit;
  std::string done_body =
      R"json({"choices":[{"message":{"role":"assistant","content":"paced"}}]})json";
  rm.deliveries = {{429, nullptr, 2}, {429, nullptr, 2}, {200, done_body.c_str(), 0}};
  frame_set_model_backend(f, &rm.base);

  auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(frame_run_loop(f), 0) << "the run completed after two retries";
  auto t1 = std::chrono::steady_clock::now();
  long long elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
#if defined(SA_HAS_STREAMS)
  EXPECT_GE(elapsed_ms, 4000) << "both retry-after waits were honored";
#else
  EXPECT_GE(elapsed_ms, 0) << "the no-streams shape cannot deliver "
                              "retry-after: this pin is vacuous there "
                              "(the streams-gated tests prove the real "
                              "behavior)";
#endif

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(count_control_kind(events, "model-error"), 2u);
  EXPECT_EQ(count_control_kind(events, "model-error-final"), 0u);
  EXPECT_EQ(rm.next, 3u);
  EXPECT_EQ(frame_is_done(f), 1);

  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestOverloadFailsLoudNeverRetries) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "overloaded", &cfg);
  ASSERT_NE(f, nullptr);

  /* The load-stop shape: a 200 whose decode carries finish_reason "load".
     The reply DECODES fine — the old code turned it into an empty content
     turn (the frame-tree slice's flagged follow-up: the zero-token load-stop
     mapping to empty-turn/done); now the engine fails loud and NEVER
     retries. */
  retry_model_t rm = {};
  rm.base.complete = NULL;
  rm.base.submit = retry_submit;
  std::string load_body =
      R"json({"choices":[{"message":{"role":"assistant","content":""},)json"
      R"json("finish_reason":"load"}]})json";
  rm.deliveries = {{200, load_body.c_str(), 0}};
  frame_set_model_backend(f, &rm.base);

  EXPECT_EQ(frame_run_loop(f), 1) << "the engine failed loud";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(count_control_kind(events, "model-error"), 0u)
      << "no retry repost happened";
  EXPECT_EQ(count_control_kind(events, "model-error-final"), 1u);
  std::string final_text = control_text_of(events, "model-error-final");
  EXPECT_NE(final_text.find("overload"), std::string::npos)
      << "the final control names the overload class";
  EXPECT_EQ(rm.next, 1u) << "the backend saw exactly one delivery (zero reposts)";
  EXPECT_EQ(count_type(events, "turn.end"), 1u)
      << "the failed turn still closed (the finally-discipline)";

  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestOverflowNeverRetries) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "too big", &cfg);
  ASSERT_NE(f, nullptr);

  /* A client-side refusal (http 400): retrying blind is never right — the
     engine fails loud on the FIRST overflow, no repost, no retry. */
  retry_model_t rm = {};
  rm.base.complete = NULL;
  rm.base.submit = retry_submit;
  rm.deliveries = {{400, nullptr, 0}};
  frame_set_model_backend(f, &rm.base);

  EXPECT_EQ(frame_run_loop(f), 1) << "the engine failed loud";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(count_control_kind(events, "model-error"), 0u)
      << "no retry repost happened";
  EXPECT_EQ(count_control_kind(events, "model-error-final"), 1u);
  std::string final_text = control_text_of(events, "model-error-final");
  EXPECT_NE(final_text.find("overflow:"), std::string::npos)
      << "the final control carries the overflow class's name";
  EXPECT_EQ(rm.next, 1u) << "the backend saw exactly one delivery (zero reposts)";

  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

/* --- the interrupt seam (surface-completion spec §2) ------------------------
   frame_interrupt's whole contract, in one inline-mode pair: the synthesis
   (cut cell + aborted turn) with the runtime's POISON, and the idle shape
   that must write nothing. The interrupt thread is genuinely concurrent —
   the ASan dir is the race's proof (the pinned discipline for the
   multi-threaded tests). */

TEST(TestLoop, TestFrameInterruptCutsCellAndAbortsTurn) {
  py_agent_init();
  frame_config_t cfg = test_config();
  cfg.cell_watchdog_ms = 0;   /* the deadline watchdog OFF: this is the
                                 caller's interrupt, not the timer's — a
                                 300 ms interrupt racing a default 5 min
                                 deadline would only pollute the pin */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "interrupt me", &cfg);
  ASSERT_NE(f, nullptr);

  /* A cell that sleeps ~3 s: pyrt's interrupt is COOPERATIVE-ONLY (pinned
     CPython 3.12.13), so the running cell cannot be cut — the interrupt's
     own SYNTHESIS closes it corr-matched now, and the cell's REAL result
     lands late and drops quietly. Timing margins are generous: interrupt at
     ~300 ms (well into the sleep — the pyrt boot is far faster in every
     non-valgrind build), cell sleep 3 s. */
  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import time\\nprint('started')\\ntime.sleep(3)\"}"}}]}}]})json";

  std::vector<std::string> replies = {turn1};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  std::thread killer([f]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    frame_interrupt(f);
  });

  /* The engine ends ABORTED (a failure exit for the loop), never ok. */
  EXPECT_EQ(frame_run_loop(f), 1);
  killer.join();

  /* The synthesized close rode ONE batch: cell.result (status 1, the
     interrupt text) + step.end + turn.end {aborted}; the turn.end is the
     log's NEWEST record (the engine ended; nothing posted after). */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  int saw_result = 0, saw_step_end = 0, saw_turn_end = 0;
  size_t turn_end_at = (size_t)-1;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* type_v = json_get(rec, "type");
    if (type_v == NULL) continue;
    const char* tn = json_as_string(type_v);
    if (strcmp(tn, "cell.result") == 0) {
      json_value_t* p = json_get(rec, "payload");
      if (p != NULL && json_as_int(json_get(p, "status")) == 1 &&
          strstr(json_as_string(json_get(p, "text")), "interrupted") != NULL) {
        saw_result = 1;
      }
    } else if (strcmp(tn, "step.end") == 0) {
      saw_step_end = 1;
    } else if (strcmp(tn, "turn.end") == 0) {
      json_value_t* p = json_get(rec, "payload");
      json_value_t* reason = (p != NULL) ? json_get(p, "reason") : NULL;
      json_value_t* kind = (reason != NULL) ? json_get(reason, "kind") : NULL;
      if (kind != NULL &&
          strcmp(json_as_string(kind), "aborted") == 0) {
        saw_turn_end = 1;
        turn_end_at = i;
      }
    }
  }
  EXPECT_EQ(saw_result, 1);
  EXPECT_EQ(saw_step_end, 1);
  EXPECT_EQ(saw_turn_end, 1);
  ASSERT_NE(turn_end_at, (size_t)-1);
  EXPECT_EQ(turn_end_at, json_size(events) - 1)
      << "the aborted turn.end is the log's newest record";
  json_value_destroy(events);

  /* The frame still functions (the poison is the RUNTIME's, not the
     frame's): recall runs its round trip without a deadlock. */
  char* v = frame_recall(f, "nonexistent-key");
  free(v);   /* NULL ok — the point is frame_recall did not crash */

  /* Let the interrupted cell's 3 s sleep run out so its REAL pyrt result
     parks in the frame's mailbox; the SECOND run's first pump then
     dispatches the quiet-drop branch deterministically (it writes nothing —
     and the cell's sleep has bounded the wait, so frame_destroy's join
     below is instant). */
  std::this_thread::sleep_for(std::chrono::milliseconds(4000));

  /* The poison contract: a resumed turn's cell refuses corr-matched loud —
     its paired status-1 cell.result names the poison, and the run itself
     continues (the refusal is failure data the model reads; the content
     turn ends it cleanly). */
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('after', 1)\"}"}}]}}]})json";
  std::string turn3 =
      R"json({"choices":[{"message":{"role":"assistant","content":"the poisoned refusal came through"}}]})json";
  std::vector<std::string> replies2 = {turn2, turn3};
  sm.replies = &replies2;
  EXPECT_EQ(frame_run_loop(f), 0);

  events = load_events(f);
  ASSERT_NE(events, nullptr);
  int saw_poison = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* type_v = json_get(rec, "type");
    if (type_v == NULL) continue;
    if (strcmp(json_as_string(type_v), "cell.result") == 0) {
      json_value_t* p = json_get(rec, "payload");
      if (p != NULL && json_as_int(json_get(p, "status")) == 1 &&
          strstr(json_as_string(json_get(p, "text")), "poisoned") != NULL) {
        saw_poison = 1;
      }
    }
  }
  EXPECT_EQ(saw_poison, 1)
      << "the resumed turn's cell must refuse corr-matched loud with the "
         "poison text";
  json_value_destroy(events);

  frame_destroy(f);   /* documented cost on a wedged runtime — the join; here
                         bounded: the interrupt's cell already finished */
  wave_db_close(db);
}

TEST(TestLoop, TestFrameInterruptArmsNoStoreWriteWhenNothingOpen) {
  /* Interrupt an IDLE frame: no pending cell, no open turn — the boundary
     cut arms only (pyrt has not even booted here) and NO store record
     appears. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "idle interrupt", &cfg);
  ASSERT_NE(f, nullptr);

  frame_interrupt(f);
  _frame_pump(f);   /* the inline owner delivers the queued FRM_INT */

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  size_t before = json_size(events);
  json_value_destroy(events);
  EXPECT_EQ(before, 0u) << "an idle interrupt wrote nothing durable";

  char* v = frame_recall(f, "k");   /* the frame still works (no deadlock) */
  free(v);
  frame_destroy(f);
  wave_db_close(db);
}

/* --- the pooled cell watchdog (surface-completion spec §2) ----------------

   The POOLED shape of the interrupt seam: a watchdog thread per running
   cell waits its deadline on a condvar; the REAL result's arrival disarms
   it under the airtight protocol; at the deadline it hands the whole
   watchdog struct to the frame as a FRM_CELL_WATCHDOG payload and the
   frame's dispatch runs the SAME synthesis under the watchdog wording. The
   pool runs everything (no joins, the test_frame.cpp pooled-tree idiom);
   the tests poll the durable log / the terminal status. */

/* Poller for the DEAD-CELL test: the frame never becomes done (a failed TOP
   frame keeps its status — the pinned shape), so the gate is the durable
   turn.end {aborted, watchdog wording} record itself, re-scanned off the
   (idle-then) pooled store. */
static bool wait_pooled_watchdog_record(frame_t* f, int round10ms) {
  for (int i = 0; i < round10ms; i++) {
    json_value_t* events = load_events(f);
    if (events != NULL) {
      for (size_t j = 0; j < json_size(events); j++) {
        json_value_t* rec = json_at(events, j);
        if (!event_is(rec, "turn.end")) continue;
        json_value_t* p = payload_of(rec);
        json_value_t* reason = (p != NULL) ? json_get(p, "reason") : NULL;
        if (reason != NULL &&
            strcmp(json_as_string(json_get(reason, "kind")), "aborted") == 0 &&
            strcmp(json_as_string(json_get(reason, "text")),
                   "aborted: cell exceeded the watchdog deadline") == 0) {
          json_value_destroy(events);
          return true;
        }
      }
      json_value_destroy(events);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

/* Poller for the pooled tests that end DONE (the disarm-wins race shape):
   frame_is_done flips when the finish batch commits. */
static bool wait_pooled_done(frame_t* f, int round10ms) {
  for (int i = 0; i < round10ms && frame_is_done(f) == 0; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return frame_is_done(f) != 0;
}

/* The pooled store + frame pair for the watchdog tests (a POOLED frame
   requires a POOLED store — frame_create refuses loud otherwise; the pooled
   tree tests' exact setup). */
TEST(TestLoop, TestPooledWatchdogInterruptsTheDeadCell) {
  py_agent_init();
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  frame_config_t cfg = test_config();
  cfg.pool = pool;
  cfg.cell_watchdog_ms = 150;   /* short: the deadline fires inside the test */

  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;   /* the store actor rides the SAME pool */
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "watchdog me", &cfg);
  ASSERT_NE(f, nullptr);

  /* Turn 1: a tool-call cell that sleeps 3 s — the deadline (150 ms) wins
     by two orders of magnitude. */
  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import time\\nprint('started')\\ntime.sleep(3)\\n'the real result'\"}"}}]}}]})json";
  std::vector<std::string> replies = {turn1};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  ASSERT_EQ(frame_start(f), 0);

  /* The watchdog's synthesis committed: turn.end {aborted, the watchdog
     wording} (+ one corr-matched cell.result, status 1, the interrupt
     wording). */
  ASSERT_TRUE(wait_pooled_watchdog_record(f, 800))
      << "the watchdog's synthesis never committed the aborted turn";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  int saw_result = 0;
  size_t n_cells = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "cell.result")) continue;
    n_cells++;
    json_value_t* p = payload_of(rec);
    if (p != NULL && json_as_int(json_get(p, "status")) == 1 &&
        strstr(json_as_string(json_get(p, "text")), "interrupted") != NULL) {
      saw_result = 1;
    }
  }
  EXPECT_EQ(saw_result, 1) << "the synthesis wrote the interrupted cell's "
                              "corr-matched result";
  /* The hung cell's REAL result (status 0) must NEVER write a second
     cell.result — the poison contract's quiet drop. Give the 3 s sleep its
     run-out like the interrupt test, then re-scan. */
  std::this_thread::sleep_for(std::chrono::milliseconds(4000));
  json_value_destroy(events);
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  size_t n_cells_after = count_type(events, "cell.result");
  EXPECT_EQ(n_cells_after, n_cells) << "the dead cell's real result wrote no "
                                       "durable record (the quiet drop)";
  /* A failed TOP frame makes no status change (the pinned
     cap-is-a-failure shape) — done is 0, but the ENGINE ended (no further
     turns run against the poisoned runtime; the events above are final). */
  EXPECT_EQ(frame_is_done(f), 0)
      << "a failed top frame keeps its status (the failure is the log's)";
  json_value_destroy(events);

  scheduler_pool_stop(pool);   /* documented order: stop the pool FIRST, then
                                  frame_destroy, then the db close, then the
                                  pool destroy (frame.c's teardown note) */
  frame_destroy(f);   /* joins the hung pyrt thread — bounded by the 3 s
                         sleep (past here), the documented cost */
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}

TEST(TestLoop, TestPooledWatchdogDisarmsAtTheRealResult) {
  /* The disarm path's pin (the watchdog-race shape): the deadline sits
     10 s out, the pooled cell completes in milliseconds — the REAL result's
     arrival disarms the watcher (join under the protocol) and the run is a
     CLEAN completion. No aborted turn.end, no control event, done. */
  py_agent_init();
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  frame_config_t cfg = test_config();
  cfg.pool = pool;
  cfg.cell_watchdog_ms = 10000;   /* way past a fast cell: the disarm wins */

  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "fast cell, disarm wins", &cfg);
  ASSERT_NE(f, nullptr);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"1 + 1\"}"}}]}}]})json";
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","content":"the fast cell ran watched"}}]})json";
  std::vector<std::string> replies = {turn1, turn2};
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  ASSERT_EQ(frame_start(f), 0);
  ASSERT_TRUE(wait_pooled_done(f, 6000))
      << "the fast cell completed and the frame finished done";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  int saw_aborted = 0;
  int saw_completed = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "turn.end")) continue;
    json_value_t* reason = json_get(payload_of(rec), "reason");
    if (reason == NULL) continue;
    if (strcmp(json_as_string(json_get(reason, "kind")), "aborted") == 0)
      saw_aborted = 1;
    if (strcmp(json_as_string(json_get(reason, "kind")), "completed") == 0)
      saw_completed = 1;
  }
  EXPECT_EQ(saw_aborted, 0) << "the disarm won — no deadline synthesis fired";
  EXPECT_EQ(saw_completed, 1);
  json_value_destroy(events);

  scheduler_pool_stop(pool);   /* documented order: stop the pool FIRST, then
                                  frame_destroy (frame.c's teardown note) */
  frame_destroy(f);
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}

/* --- the doom-loop breaker (the guards spec §1; task 6) -------------------

   The breaker's loop integration: the tool path folds the streak BEFORE the
   audit batch composes; the threshold-th byte-identical cell is refused —
   never dispatched, never audited — and the turn closes {doom-loop} with the
   engine ended failed. Reset on input: a NEW user-role msg.append between
   cells (steering) resets the streak before the identity check. */

TEST(TestLoop, TestDoomLoopRefusesTheThirdIdenticalCell) {
  /* turn1/turn2/turn3 all = execute "import actor\nactor.remember('n', 1)"
     — byte-identical code, no steering between; turn4 = content "never"
     (never requested after the trip). */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "doom loop", &cfg);
  ASSERT_NE(f, nullptr);

  std::string cell =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('n', 1)\"}"}}]}}]})json";
  std::string turn4 =
      R"json({"choices":[{"message":{"role":"assistant","content":"never"}}]})json";
  std::vector<std::string> replies = {cell, cell, cell, turn4};

  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  /* The engine ended FAILED at the trip (frame_run_loop's failed rc). */
  EXPECT_EQ(frame_run_loop(f), 1);
  EXPECT_EQ(frame_is_done(f), 0) << "a failed TOP frame keeps its status";
  EXPECT_EQ(replies.size(), 1u) << "turn 4 was never requested";

  /* The two real cells ran the remember; the third never did. */
  char* n = frame_recall(f, "n");
  ASSERT_NE(n, nullptr);
  EXPECT_STREQ(n, "1");
  free(n);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);

  /* Exactly TWO cell.run records — the third call NEVER ran (and nothing
     pointless was ever audited). */
  EXPECT_EQ(count_type(events, "cell.run"), 2u);
  EXPECT_EQ(count_type(events, "state.remember"), 2u);

  /* The third turn OPENED at its entry (the derive preceded the trip), but
     NO step.start for it: the audit batch never composed. */
  EXPECT_EQ(count_turn(events, "turn.start", 3), 1u);
  EXPECT_EQ(count_turn(events, "step.start", 3), 0u);

  /* The breaker's control record with the model-visible text landed. */
  json_value_t* control = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "control")) continue;
    json_value_t* p = payload_of(rec);
    json_value_t* k = (p != NULL) ? json_get(p, "kind") : NULL;
    if (k != NULL && strcmp(json_as_string(k), "doom-loop") == 0) control = rec;
  }
  ASSERT_NE(control, nullptr) << "the breaker's control record is loud";
  json_value_t* cp = payload_of(control);
  EXPECT_STREQ(json_as_string(json_get(cp, "text")),
               "doom-loop guard: the last 3 cells were identical; the turn "
               "is refused — vary the approach");

  /* The close: turn 3's turn.end {doom-loop, the breaker's text}, riding
     AFTER the control record; the turn.end is the NEWEST record in the log
     (the trip never dispatched, never audited anything after). */
  json_value_t* te = life_record_of_turn(events, "turn.end", 3);
  ASSERT_NE(te, nullptr);
  EXPECT_EQ(turn_end_kind(te), "doom-loop");
  EXPECT_EQ((long long)json_as_int(json_get(payload_of(te), "turn")), 3);
  json_value_t* reason = json_get(payload_of(te), "reason");
  ASSERT_NE(reason, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reason, "text")),
               "doom-loop guard: the last 3 cells were identical; the turn "
               "is refused — vary the approach");
  ASSERT_EQ(json_size(events), rec_seq(te));
  EXPECT_TRUE(event_is(json_at(events, json_size(events) - 1), "turn.end"))
      << "the newest record is the doomed turn's close";
  EXPECT_GT(rec_seq(te), rec_seq(control))
      << "the close rides the trip's own batch, control first";

  /* Turns 1 and 2 closed completed via their cells' result batches — the
     breaker touched only the threshold-th call. */
  EXPECT_EQ(turn_end_kind(life_record_of_turn(events, "turn.end", 1)),
            "completed");
  EXPECT_EQ(turn_end_kind(life_record_of_turn(events, "turn.end", 2)),
            "completed");

  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestDoomStreakResetsOnSteering) {
  /* turn1 + turn2 identify; THEN a steer (the scripted steer machinery from
     TestSteeringBetweenTurnsReordersCells); THEN two MORE identical cells —
     the 4-cell pattern trips WITHOUT the reset; with it the run completes. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "steered out of the loop", &cfg);
  ASSERT_NE(f, nullptr);

  std::string cell =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('n', 1)\"}"}}]}}]})json";
  std::string turn5 =
      R"json({"choices":[{"message":{"role":"assistant","content":"steered through"}}]})json";
  std::vector<std::string> replies = {cell, cell, cell, cell, turn5};

  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = f;
  sm.steer_text = "steering: try a different approach";
  sm.steer_on = 2;   /* steer at the SECOND model call — the steer lands
                        between cell 2 and cell 3, so the THIRD identical
                        call re-derives from fresh input (the reset's input) */
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  EXPECT_EQ(frame_run_loop(f), 0);
  EXPECT_EQ(frame_is_done(f), 1);

  /* All four identical cells ran — the streak reset before the third. */
  char* n = frame_recall(f, "n");
  ASSERT_NE(n, nullptr);
  EXPECT_STREQ(n, "1");
  free(n);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(count_type(events, "cell.run"), 4u);
  EXPECT_EQ(count_type(events, "cell.result"), 4u);
  /* The steer's user message landed, and no doom control ever fired. */
  EXPECT_NE(find_msg_append(events, "user", "steering: try a different "
                            "approach"), nullptr);
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "control")) continue;
    json_value_t* p = payload_of(rec);
    json_value_t* k = (p != NULL) ? json_get(p, "kind") : NULL;
    EXPECT_TRUE(k == NULL || strcmp(json_as_string(k), "doom-loop") != 0)
        << "no breaker control fired on a steered loop";
  }
  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestDoomStreakResetsOnDifferentCell) {
  /* A, B, A: the streak never survives a different cell — turn 3 is
     identical to turn 1 but DIFFERENT from turn 2, so it is streak 1. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "vary the key", &cfg);
  ASSERT_NE(f, nullptr);

  std::string cell_n =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('n', 1)\"}"}}]}}]})json";
  std::string cell_m =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('m', 2)\"}"}}]}}]})json";
  std::string turn4 =
      R"json({"choices":[{"message":{"role":"assistant","content":"varied"}}]})json";
  std::vector<std::string> replies = {cell_n, cell_m, cell_n, turn4};

  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  EXPECT_EQ(frame_run_loop(f), 0);
  EXPECT_EQ(frame_is_done(f), 1);

  /* All three cells ran (both remembers landed). */
  char* n = frame_recall(f, "n");
  ASSERT_NE(n, nullptr);
  EXPECT_STREQ(n, "1");
  free(n);
  char* m = frame_recall(f, "m");
  ASSERT_NE(m, nullptr);
  EXPECT_STREQ(m, "2");
  free(m);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(count_type(events, "cell.run"), 3u);
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "control")) continue;
    json_value_t* p = payload_of(rec);
    json_value_t* k = (p != NULL) ? json_get(p, "kind") : NULL;
    EXPECT_TRUE(k == NULL || strcmp(json_as_string(k), "doom-loop") != 0)
        << "no breaker control fired on a varied pattern";
  }
  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

/* --- the doom x retry cross-seam (guards spec §1 through the reply path's
   failure branch) ----------------------------------------------------------

   The CROSS-SEAM fact only the composed run can trace: the model-failure
   branch (loop.c's reply path) folds ONLY model_retries/model_retry_step —
   it never touches the doom streak. Whether the consecutive-identical count
   survives a model-error repost is exactly what these two runs pin. The
   backend is the retry family's async fake (status script), delivering the
   doom suite's byte-identical cell; its steer fields extend the steer-on-
   model-call machinery the doom suite already steers with. */

TEST(TestLoop, TestModelRetryDoesNotResetTheDoomStreak) {
  /* delivery script: exec(A) OK (streak 1), a 503 FAILURE (the server
     class's repost — the SAME turn, no user append side), then exec(A)
     twice more with no steering between. The THIRD exec(A) — the one AFTER
     the failed turn — must still be the threshold-th identical call: the
     run ends 1, the control record carries "doom-loop", and cell.run
     counts 2 (the third call was refused, not the fourth). A retry neither
     counts as a tool call nor resets the streak. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "doom through a retry", &cfg);
  ASSERT_NE(f, nullptr);

  std::string cell =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('n', 1)\"}"}}]}}]})json";
  std::string turn5 =
      R"json({"choices":[{"message":{"role":"assistant","content":"never"}}]})json";
  retry_model_t rm = {};   /* zero-init: the vtable's members set explicitly
                              (no steer fields — this run never steers) */
  rm.base.complete = NULL;   /* async-only: the engine takes the submit path */
  rm.base.submit = retry_submit;
  rm.deliveries = {{200, cell.c_str(), 0},
                   {503, nullptr, 0},
                   {200, cell.c_str(), 0},
                   {200, cell.c_str(), 0},
                   {200, turn5.c_str(), 0}};
  frame_set_model_backend(f, &rm.base);

  /* The engine ended FAILED at the trip (the doom close's failed rc). */
  EXPECT_EQ(frame_run_loop(f), 1);
  EXPECT_EQ(frame_is_done(f), 0) << "a failed TOP frame keeps its status";
  EXPECT_EQ(rm.next, 4u) << "the fifth delivery (the content turn) was never "
                            "requested — the trip happened on the THIRD "
                            "identical call, after the retry";

  /* The two real cells ran the remember; the third never did. */
  char* n = frame_recall(f, "n");
  ASSERT_NE(n, nullptr);
  EXPECT_STREQ(n, "1");
  free(n);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(count_type(events, "cell.run"), 2u) << "a retry counts as no "
      "tool call — two real cells ran, the identical count survived them";
  EXPECT_EQ(count_type(events, "state.remember"), 2u);

  /* The failure seam fired exactly once and reposted (never final): the
     streak had to survive THIS repost to trip on the third exec(A). */
  EXPECT_EQ(count_control_kind(events, "model-error"), 1u);
  EXPECT_EQ(count_control_kind(events, "model-error-final"), 0u);

  /* The breaker's control record landed with the model-visible text. */
  json_value_t* control = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "control")) continue;
    json_value_t* p = payload_of(rec);
    json_value_t* k = (p != NULL) ? json_get(p, "kind") : NULL;
    if (k != NULL && strcmp(json_as_string(k), "doom-loop") == 0) control = rec;
  }
  ASSERT_NE(control, nullptr) << "the breaker's control record is loud — the "
      "streak counted ACROSS the failed turn";
  json_value_t* cp = payload_of(control);
  EXPECT_STREQ(json_as_string(json_get(cp, "text")),
               "doom-loop guard: the last 3 cells were identical; the turn "
               "is refused — vary the approach");

  /* The close: the reposted-into turn is still turn 3 (the retry never
     renumbers), its turn.end {doom-loop, the breaker's text} rides after
     the control record and is the log's NEWEST record. */
  json_value_t* te = life_record_of_turn(events, "turn.end", 3);
  ASSERT_NE(te, nullptr);
  EXPECT_EQ(turn_end_kind(te), "doom-loop");
  EXPECT_EQ((long long)json_as_int(json_get(payload_of(te), "turn")), 3);
  json_value_t* reason = json_get(payload_of(te), "reason");
  ASSERT_NE(reason, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reason, "text")),
               "doom-loop guard: the last 3 cells were identical; the turn "
               "is refused — vary the approach");
  ASSERT_EQ(json_size(events), rec_seq(te));
  EXPECT_TRUE(event_is(json_at(events, json_size(events) - 1), "turn.end"))
      << "the newest record is the doomed turn's close";
  EXPECT_GT(rec_seq(te), rec_seq(control))
      << "the close rides the trip's own batch, control first";

  /* Turns 1 and 2 closed completed — the 503's repost stayed turn 2 (the
     retry never renumbered) and its cell closed that turn normally. */
  EXPECT_EQ(turn_end_kind(life_record_of_turn(events, "turn.end", 1)),
            "completed");
  EXPECT_EQ(turn_end_kind(life_record_of_turn(events, "turn.end", 2)),
            "completed");

  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestSteeringAfterRetryResetsTheStreak) {
  /* The SAME script with one steer: it lands inside the first POST-retry
     submit (the 1-based call 3 — the repost's model boundary), so the
     NEXT derive reads the user append as fresh input. Without the reset,
     call 4's exec(A) would be the threshold-th identical (streak 2 carried
     across the failed turn + call 3); with it, the post-steer cells run
     streak 1 then 2. The discriminating pin: FOUR cell.runs, the run
     completes, no breaker control ever fired — and model-error counts 1,
     so the retry genuinely happened inside this run. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "steered out through a retry", &cfg);
  ASSERT_NE(f, nullptr);

  std::string cell =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('n', 1)\"}"}}]}}]})json";
  std::string done =
      R"json({"choices":[{"message":{"role":"assistant","content":"the steer cleared it"}}]})json";
  retry_model_t rm = {};   /* zero-init: the vtable's members set explicitly */
  rm.base.complete = NULL;
  rm.base.submit = retry_submit;
  rm.deliveries = {{200, cell.c_str(), 0},
                   {503, nullptr, 0},
                   {200, cell.c_str(), 0},   /* the repost's cell — the steer
                                                lands at THIS call's boundary */
                   {200, cell.c_str(), 0},
                   {200, cell.c_str(), 0},
                   {200, done.c_str(), 0}};
  rm.steer_frame = f;
  rm.steer_text = "steering: vary the approach";
  rm.steer_on = 3;   /* the first post-retry call */
  frame_set_model_backend(f, &rm.base);

  EXPECT_EQ(frame_run_loop(f), 0);
  EXPECT_EQ(frame_is_done(f), 1);

  /* All four identical cells ran the remember (the fourth is the proof:
     without the steer's reset it would have been the threshold-th). */
  char* n = frame_recall(f, "n");
  ASSERT_NE(n, nullptr);
  EXPECT_STREQ(n, "1");
  free(n);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(count_type(events, "cell.run"), 4u);
  EXPECT_EQ(count_type(events, "cell.result"), 4u);
  EXPECT_EQ(count_control_kind(events, "model-error"), 1u)
      << "the retry genuinely happened inside this run";
  EXPECT_EQ(count_control_kind(events, "model-error-final"), 0u);

  /* The steer's user message landed between cell 1 and the repost's cell
     (the submit's boundary), and no breaker control ever fired. */
  std::vector<size_t> runs;
  size_t steer_at = (size_t)-1;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "cell.run")) {
      runs.push_back(i);
    } else if (event_is(rec, "msg.append")) {
      json_value_t* p = json_get(rec, "payload");
      if (p != NULL &&
          strcmp(json_as_string(json_get(p, "role")), "user") == 0 &&
          strcmp(json_as_string(json_get(p, "content")),
                 "steering: vary the approach") == 0) {
        steer_at = i;
      }
    }
  }
  EXPECT_NE(steer_at, (size_t)-1);
  if (steer_at != (size_t)-1 && runs.size() == 4u) {
    EXPECT_GT(steer_at, runs[0]) << "the steering landed after cell 1";
    EXPECT_LT(steer_at, runs[1]) << "the repost's cell audited after it";
  }
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "control")) continue;
    json_value_t* p = payload_of(rec);
    json_value_t* k = (p != NULL) ? json_get(p, "kind") : NULL;
    EXPECT_TRUE(k == NULL || strcmp(json_as_string(k), "doom-loop") != 0)
        << "no breaker control fired — the steer's fresh input reset the "
           "streak across the failed turn";
  }
  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

#endif /* python gate */

/* --- the mid-turn-destroy pin (the frame lifetime's red/green) ------------

   THE HAZARD: the engine hands the bare frame_t* to the model sink, and the
   backend may complete it at ANY point — including AFTER frame_destroy
   freed the record mid-turn. The sink then dereferenced freed memory
   (frame_sid(f), _frame_actor(f)) BEFORE its own death check could ever see
   the death: no lock exists or should exist, so the die cannot be detected
   by reading the dying record itself.

   THE FIX's shape (frame.c/frame_internal.h): the submit step holds ONE
   pending-submit slot before submit(); the sink's release gives it back;
   frame_destroy mid-turn marks die_requested (and the actor's DESTROY
   refuse) and DEFERS its teardown while a slot is held — the LAST release
   runs the teardown instead of freeing underneath anyone.

   DETERMINISTIC SEQUENCING (documented honestly): a TRUE concurrent
   mid-turn destroy cannot be staged deterministically, so the test replays
   the load-bearing ordering single-threaded — the completion fires on the
   test thread after frame_destroy returned, exactly the "free before the
   sink's first frame touch" order that made the old code read freed
   memory. That is the same decision path the racing interleaving takes
   (die_requested checked after the record stayed alive through the slot's
   deferral), so the ASan red/green outcome carries. */
typedef struct held_model_t {
  model_backend_t base;
  model_response_sink_fn held_fn;   /* the engine's sink, recorded by submit */
  void* held_ctx;                   /* its ctx (the frame), recorded too */
  int armed;                        /* 1 once submit recorded the pair */
  const char* body;                 /* the completion the sink never saw */
} held_model_t;

static int held_submit(void* self, json_value_t* messages, json_value_t* tools,
                       model_response_sink_fn on_done, void* on_done_ctx) {
  (void)tools;
  (void)messages;
  held_model_t* hm = (held_model_t*)self;
  hm->held_fn = on_done;
  hm->held_ctx = on_done_ctx;
  hm->armed = 1;
  /* Accepted (rc 0) but the completion NEVER fires here — the sink hangs
     until the test invokes it: a production backend's loop-thread
     completion, replayed. */
  return (on_done != NULL) ? 0 : -1;
}

TEST(TestLoop, TestDestroyMidTurnDefersToThePendingSink) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "destroy under me", &cfg);
  ASSERT_NE(f, nullptr);

  held_model_t hm = {};   /* zero-init: the vtable's members are set below */
  hm.base.complete = NULL;           /* async-only: the engine takes the submit path */
  hm.base.submit = held_submit;
  hm.body = R"json({"choices":[{"message":{"role":"assistant","content":"never seen"}}]})json";
  frame_set_model_backend(f, &hm.base);

  EXPECT_EQ(frame_start(f), 0);
  int guard = 0;
  while (!hm.armed && guard++ < 100) {
    _frame_pump(f);   /* the turn, the derive's store round trip, the submit */
  }
  ASSERT_TRUE(hm.armed) << "the engine reached the submit boundary";
  ASSERT_NE(hm.held_fn, nullptr);
  ASSERT_EQ(hm.held_ctx, (void*)f);   /* the sink carries the bare frame */

  /* Destroy MID-TURN with the sink pending: the deferred teardown (die-
     requested; the record stays alive for the release). The old code freed
     f HERE and the sink invocation below dereferenced the freed record. */
  frame_destroy(f);

  /* The test then plays the backend's completion thread: fire the recorded
     sink EXACTLY ONCE — it must read die_requested, drop the completion,
     and let its release (the last one) run the teardown. No crash, no
     use-after-free, exactly one free. (A re-entry destroy is only safe
     while the deferral holds, i.e. strictly BEFORE this fire — after it the
     record is gone and even reading it is the caller's own bug.) */
  char* body = strdup(hm.body);
  ASSERT_NE(body, nullptr);
  hm.held_fn(hm.held_ctx, 200, body, strlen(body), NULL, NULL);

  wave_db_close(db);
}

/* --- restart/replay (also carries the plan's S003 acceptance): a real
   disk location, close, reopen, frame_resume. Python-independent — the
   content-only scripted turn never executes a cell. ------------------------- */

/* mkdtemp wrapper (plan's helper): returns the directory path, removes it
   with the caller. */
static std::string temp_dir_mkdtemp_sa(void) {
  char tmpl[] = "/tmp/sa-loop-XXXXXX";
  char* got = mkdtemp(tmpl);
  if (got == NULL) return std::string();
  return std::string(got);
}

/* Serialize what each scripted model call received (python-independent decode
   for content-only replies — reuse of the python-gated helper's shape). A
   minimal recording model: single reply queue, content-only bodies. */
typedef struct recording_model_t {
  model_backend_t base;
  std::vector<std::string> replies;
  std::vector<std::string> captured;
} recording_model_t;

static int recording_complete(void* self, json_value_t* messages,
                              json_value_t* tools, char** raw_out,
                              model_reply_t** reply_out, char** error_out) {
  (void)tools;
  (void)raw_out;
  *reply_out = NULL;
  *error_out = NULL;
  recording_model_t* rm = (recording_model_t*)self;
  char* seen = json_serialize(messages);
  if (seen != NULL) {
    rm->captured.emplace_back(seen);
    free(seen);
  }
  if (rm->replies.empty()) {
    *error_out = strdup("recording model: queue empty");
    return -1;
  }
  std::string body = rm->replies.front();
  rm->replies.erase(rm->replies.begin());

  char* err = NULL;
  json_value_t* root = json_parse(body.c_str(), body.size(), &err);
  if (err != NULL) free(err);
  if (root == NULL) {
    *error_out = strdup("recording model: body is not valid JSON");
    return -1;
  }
  json_value_t* choices = json_get(root, "choices");
  json_value_t* choice = (choices != NULL && json_type(choices) == JSON_ARRAY)
                             ? json_at(choices, 0) : NULL;
  EXPECT_NE(choice, nullptr);
  json_value_t* message = (choice != nullptr) ? json_get(choice, "message") : nullptr;
  EXPECT_NE(message, nullptr);
  if (choice == nullptr || message == nullptr) {
    json_value_destroy(root);
    *error_out = strdup("recording model: no message in choices[0]");
    return -1;
  }
  model_reply_t* r = (model_reply_t*)get_clear_memory(sizeof(model_reply_t));
  json_value_t* content = json_get(message, "content");
  r->content = strdup((content != NULL) ? json_as_string(content) : "");
  json_value_destroy(root);
  *reply_out = r;
  return 0;
}

TEST(TestLoop, TestRestartReplayRestoresSeqAndContext) {
  frame_config_t cfg = test_config();
  std::string dir = temp_dir_mkdtemp_sa();
  ASSERT_FALSE(dir.empty());
  std::string loc = dir + "/db";

  /* Write pre-restart state. */
  wave_database_root_t* db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "persist me", &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  EXPECT_EQ(frame_append_msg(f, "user", "remember seven"), 0);
  EXPECT_EQ(frame_remember_ctx(f, "n", "7"), 0);
  frame_destroy(f);   /* durable close */
  wave_db_close(db);

  /* Reopen + resume the very same sid. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* resumed = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(resumed, nullptr) << "restart resumes an EXISTING frame";
  EXPECT_STREQ(frame_sid(resumed), sid.c_str());

  /* ctx recalled across the restart. */
  char* n = frame_recall(resumed, "n");
  ASSERT_NE(n, nullptr);
  EXPECT_STREQ(n, "7");
  free(n);

  /* The replayed msg.append reaches the next turn's derived context: the
     content-only scripted model records what it was sent. */
  recording_model_t rm = {};
  rm.base.complete = recording_complete;
  rm.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"resumed fine"}}]})json");
  frame_set_model_backend(resumed, &rm.base);
  EXPECT_EQ(frame_run_loop(resumed), 0);
  ASSERT_EQ(rm.captured.size(), 1u);
  EXPECT_NE(rm.captured[0].find("remember seven"), std::string::npos)
      << "the replayed msg.append reached the model's derived context";
  EXPECT_NE(rm.captured[0].find("n = 7"), std::string::npos)
      << "the replayed ctx snapshot reached the model's derived context";
  EXPECT_EQ(frame_is_done(resumed), 1);   /* top frame ended on the turn */
  frame_destroy(resumed);

  /* The frame's status is now done — resume it again and append one more
     message. NOTE (documented WaveDB defect): in CONCURRENT mode a
     write committed AFTER a reopen is durable (it survives and is replayed
     — verified below by the fresh session) but INVISIBLE to the same
     session's scans and point-gets (a minimal root-level probe against
     WaveDB's library reproduces this; even database_snapshot does not
     materialize it). The seq continuation is therefore asserted from the
     THIRD session's view: the record exists with a seq strictly greater
     than every pre-restart event. */
  wave_db_close(db);
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* again = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(frame_append_msg(again, "user", "more"), 0);
  frame_destroy(again);
  wave_db_close(db);

  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* again2 = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(again2, nullptr);
  json_value_t* events = load_events(again2);
  ASSERT_NE(events, nullptr);
  long long seq_of_more = -1;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* p = json_get(rec, "payload");
    long long seq = (long long)json_as_int(json_get(rec, "seq"));
    if (p != NULL && json_get(p, "content") != NULL &&
        strcmp(json_as_string(json_get(p, "content")), "more") == 0) {
      seq_of_more = seq;
    }
  }
  bool found_more = false;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    long long seq = (long long)json_as_int(json_get(rec, "seq"));
    if (seq == seq_of_more) {
      found_more = true;   /* the append itself is allowed to equal it */
      continue;
    }
    EXPECT_LT(seq, seq_of_more) << "every other event predates the resumed app";
  }
  EXPECT_TRUE(found_more);
  EXPECT_GT(seq_of_more, 2) << "the pre-restart events really carried seqs";
  json_value_destroy(events);

  frame_destroy(again2);
  wave_db_close(db);
  std::filesystem::remove_all(dir);
}

/* --- turn lifecycle envelope (the plan's Task-2 step 1; python-gated
       siblings above; these two run WITHOUT a cell ever executing) -------- */

/* Hand-composed event record for the lifecycle tests' seeds — the frozen
   record shape frame.c composes: {"seq","type","frame","corr","at","cause",
   "payload"}.  step < 0 renders the field absent; reason_kind NULL renders
   no reason object. */
static std::string make_record_json(long long seq,
                                    const std::string& sid_path,
                                    const char* type_name, long long turn,
                                    long long step, const char* reason_kind) {
  json_value_t* rec = json_new_object();
  EXPECT_NE(rec, nullptr);
  json_object_set(rec, "seq", json_new_int(seq));
  json_object_set(rec, "type", json_new_string(type_name));
  json_object_set(rec, "frame", json_new_string(sid_path.c_str()));
  json_object_set(rec, "corr", json_new_null());
  json_object_set(rec, "at", json_new_string("2026-10-01T00:00:00Z"));
  json_object_set(rec, "cause",
                  (seq > 1) ? json_new_int(seq - 1) : json_new_null());
  json_value_t* payload = json_new_object();
  json_object_set(payload, "turn", json_new_int(turn));
  if (step >= 0) json_object_set(payload, "step", json_new_int(step));
  if (reason_kind != NULL) {
    json_value_t* reason = json_new_object();
    json_object_set(reason, "kind", json_new_string(reason_kind));
    json_object_set(payload, "reason", reason);
  }
  json_object_set(rec, "payload", payload);
  char* text = json_serialize(rec);
  json_value_destroy(rec);
  EXPECT_NE(text, nullptr);
  std::string out((text != nullptr) ? text : "");
  free(text);
  return out;
}

TEST(TestLoop, TestModelFailureClosesTheTurnWithError) {
  /* The failure surface closes (the plan's Task-2 step 1 — the DSH
     finally-discipline as a TESTED RULE): a scripted model error twice →
     model-error-final — the turn's turn.end carries reason "error" + text
     "model-error-final"; the failed turn still opened (turn.start committed
     at entry) and gets its own turn.end — NO turn is ever left open by an
     alive engine. Python-independent: the model never replies, no cell ever
     runs. */
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "fail twice", &cfg);
  ASSERT_NE(f, nullptr);

  recording_model_t rm = {};   /* the EMPTY queue errors on every call */
  rm.base.complete = recording_complete;
  frame_set_model_backend(f, &rm.base);

  EXPECT_NE(frame_run_loop(f), 0) << "the engine failed loud";
  EXPECT_EQ(frame_is_done(f), 0) << "a failed TOP frame keeps its status";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);

  /* The turn OPENED at entry; the model-error retry re-derived the SAME
     turn — one turn.start only. */
  ASSERT_EQ(count_type(events, "turn.start"), 1u);
  json_value_t* ts = life_record_of_turn(events, "turn.start", 1);
  ASSERT_NE(ts, nullptr);
  EXPECT_EQ((long long)json_as_int(json_get(payload_of(ts), "turn")), 1);

  /* No step was ever entered — the model never replied. */
  EXPECT_EQ(count_type(events, "step.start"), 0u);
  EXPECT_EQ(count_type(events, "step.end"), 0u);

  /* The controls: "model-error" (the retry's) then "model-error-final",
     committed in that order (the store's FIFO). */
  size_t n_error = 0;
  long long final_seq = -1;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "control")) continue;
    json_value_t* p = payload_of(rec);
    json_value_t* k = (p != NULL) ? json_get(p, "kind") : NULL;
    if (k == NULL) continue;
    if (strcmp(json_as_string(k), "model-error") == 0) n_error++;
    if (strcmp(json_as_string(k), "model-error-final") == 0) {
      final_seq = rec_seq(rec);
    }
  }
  EXPECT_EQ(n_error, 1u);
  ASSERT_GT(final_seq, 0);

  /* THE CLOSE (the tested finally-rule): turn.end {turn 1, error, text
     "model-error-final"} AFTER the final control — one close per failure. */
  ASSERT_EQ(count_type(events, "turn.end"), 1u);
  json_value_t* te = life_record_of_turn(events, "turn.end", 1);
  ASSERT_NE(te, nullptr);
  EXPECT_GT(rec_seq(te), rec_seq(ts));
  EXPECT_GT(rec_seq(te), final_seq) << "the close rides the failure's batch";
  EXPECT_EQ(turn_end_kind(te), "error");
  json_value_t* reason = json_get(payload_of(te), "reason");
  ASSERT_NE(reason, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reason, "text")), "model-error-final");

  /* Balanced: the turn.end is the NEWEST lifecycle record in the log. */
  long long newest_life = -1;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "turn.start") || event_is(rec, "turn.end") ||
        event_is(rec, "step.start") || event_is(rec, "step.end")) {
      newest_life = rec_seq(rec);
    }
  }
  EXPECT_EQ(newest_life, rec_seq(te));

  json_value_destroy(events);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestTurnNumbersRestoreFromTheLog) {
  /* The turn-number restore (the plan's Task-2 step 1): seed a session with
     lifecycle records (turn 3 committed pre-restart), then start the
     engine — its first turn is 4 (restored from the log, never renumbered
     from 1). A DEAD engine never carries the counter across a restart: this
     runs a fully separate engine process-shape (destroyed frame, reopened
     db). VALGRIND EXCLUSION (as its sibling
     TestRestartReplayRestoresSeqAndContext, recorded on Atlas S006): the
     scratch-disk tests spin under valgrind 3.18's emulation here — the
     leak proof for this class is the ASan suite + the non-disk
     lifecycle/envelope filters. */
  frame_config_t cfg = test_config();
  std::string dir = temp_dir_mkdtemp_sa();
  ASSERT_FALSE(dir.empty());
  std::string loc = dir + "/db";

  /* Session 1: create the frame and hand-seed turns 1..3, balanced
     (turn.start/step.start/step.end/turn.end for each), seqs 1..12. */
  wave_database_root_t* db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "turn restore", &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);

  static const char* per_turn_types[4] = {"turn.start", "step.start",
                                          "step.end", "turn.end"};
  frm_store_op_t* seed = (frm_store_op_t*)get_clear_memory(12 * sizeof(frm_store_op_t));
  ASSERT_NE(seed, nullptr);
  for (size_t i = 0; i < 12; i++) {
    long long seq = (long long)i + 1;
    long long turn = (long long)(i / 4) + 1;
    const char* type = per_turn_types[i % 4];
    std::string record =
        make_record_json(seq, sid, type, turn, (i % 4 == 0) ? -1 : 1,
                         (i % 4 == 3) ? "completed" : NULL);
    char key[96];
    snprintf(key, sizeof(key), "%s/events/%020lld", sid.c_str(),
             (long long)seq);
    seed[i].key = strdup(key);
    seed[i].value = (uint8_t*)strdup(record.c_str());
    seed[i].value_len = record.size();
  }
  EXPECT_EQ(_frame_sync_batch(f, seed, 12, "lifecycle seed"), 0);
  json_value_t* seedy = load_events(f);
  ASSERT_NE(seedy, nullptr);
  EXPECT_EQ(json_size(seedy), 12u) << "the seed committed as normal events";
  json_value_destroy(seedy);

  frame_destroy(f);   /* durable close — the engine of session 1 is DEAD */
  wave_db_close(db);

  /* Session 2: a resumed frame runs ONE content turn — its engine has NO
     in-memory counter to inherit; the first entry's restore reads the log. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* resumed = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(resumed, nullptr);
  recording_model_t rm = {};
  rm.base.complete = recording_complete;
  rm.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"resumed past the seed"}}]})json");
  frame_set_model_backend(resumed, &rm.base);
  EXPECT_EQ(frame_run_loop(resumed), 0);
  EXPECT_EQ(frame_is_done(resumed), 1);
  frame_destroy(resumed);
  wave_db_close(db);

  /* Session 3: the reads (a write committed after a reopen is invisible to
     the SAME session's scans — the recorded WaveDB defect; a fresh session
     sees them). */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* handle = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(handle, nullptr);
  json_value_t* events = load_events(handle);
  ASSERT_NE(events, nullptr);

  /* Exactly the seeded turns 1..3 + the engine's restored turn 4 — the
     counter restored from the log, never renumbered from 1. */
  for (long long k = 1; k <= 4; k++) {
    EXPECT_EQ(count_turn(events, "turn.start", k), 1u)
        << "turn " << k << "'s turn.start";
    EXPECT_EQ(count_turn(events, "turn.end", k), 1u)
        << "turn " << k << "'s turn.end";
  }
  EXPECT_EQ(count_type(events, "turn.start"), 4u) << "no extra turns written";
  EXPECT_EQ(count_type(events, "turn.end"), 4u);

  /* Turn 4's envelope rode its finish batch: [step.start, msg.append,
     step.end, turn.end] contiguous, with turn.start earlier. */
  json_value_t* m = find_msg_append(events, "assistant", "resumed past the seed");
  ASSERT_NE(m, nullptr);
  long long m_seq = rec_seq(m);
  json_value_t* ss4 = life_record_of_turn(events, "step.start", 4);
  json_value_t* se4 = life_record_of_turn(events, "step.end", 4);
  json_value_t* te4 = life_record_of_turn(events, "turn.end", 4);
  json_value_t* ts4 = life_record_of_turn(events, "turn.start", 4);
  ASSERT_NE(ss4, nullptr);
  ASSERT_NE(se4, nullptr);
  ASSERT_NE(te4, nullptr);
  ASSERT_NE(ts4, nullptr);
  EXPECT_EQ(rec_seq(ss4), m_seq - 1);
  EXPECT_EQ(rec_seq(se4), m_seq + 1);
  EXPECT_EQ(rec_seq(te4), m_seq + 2);
  EXPECT_LT(rec_seq(ts4), m_seq - 1);
  EXPECT_EQ(turn_end_kind(te4), "completed");
  EXPECT_EQ(json_as_int(json_get(payload_of(ss4), "step")), 1);

  json_value_destroy(events);
  frame_destroy(handle);
  wave_db_close(db);
  std::filesystem::remove_all(dir);
}

/* --- the resume repair (the plan's Task-3 step 1; spec §4 + §7 [13/14]) ---
   Python-independent: the restarted engine's scripted turns are content-only
   (no cell ever EXECUTES — the cell only appears as a SEEDED audit record the
   crash "cut" after). Scratch-disk idiom: fresh mkdtemp dirs created and
   destroyed by the test, never sa-demo-db. */

/* The cell.run seed record — the frozen event-record shape with the audit's
   payload {code, corr} (make_record_json covers the envelope's payloads only;
   the fold pairs cell.run/cell.result by the payload's corr). */
static std::string make_cell_run_record(long long seq,
                                        const std::string& sid_path,
                                        const std::string& code,
                                        long long corr) {
  json_value_t* rec = json_new_object();
  json_object_set(rec, "seq", json_new_int(seq));
  json_object_set(rec, "type", json_new_string("cell.run"));
  json_object_set(rec, "frame", json_new_string(sid_path.c_str()));
  json_object_set(rec, "corr", json_new_null());
  json_object_set(rec, "at", json_new_string("2026-10-01T00:00:00Z"));
  json_object_set(rec, "cause", json_new_int(seq - 1));
  json_value_t* payload = json_new_object();
  json_object_set(payload, "code", json_new_string(code.c_str()));
  json_object_set(payload, "corr", json_new_int(corr));
  json_object_set(rec, "payload", payload);
  char* text = json_serialize(rec);
  EXPECT_NE(text, nullptr);
  json_value_destroy(rec);
  std::string out((text != nullptr) ? text : "");
  free(text);
  return out;
}

/* A reloaded record's byte echo (the seed texts and the echoes are all
   json_serialize outputs, so equality here is BYTE-identical, whitespace
   included — the append-only pin's shape). */
static std::string echo_record(json_value_t* rec) {
  char* raw = json_serialize(rec);
  std::string out((raw != nullptr) ? raw : "");
  free(raw);
  return out;
}

/* The reloaded record at an exact log seq (NULL when absent). */
static json_value_t* record_at_seq(json_value_t* events, long long seq) {
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (rec_seq(rec) == seq) return rec;
  }
  return nullptr;
}

/* The pinned started-shape brief for a cell audited at `seq` (spec §2's
   wording verbatim — mirrors test_lifecycle.cpp's lc_started_text). */
static std::string loop_started_brief(long long seq, const std::string& code) {
  std::string out =
      "The previous turn was interrupted before its result was recorded.\n";
  out += "The cell was executing (harness-log seq " + std::to_string(seq) +
         "):\n";
  out += code + "\n";
  out +=
      "Its outcome is unknown. Decide whether to retry from the cell's "
      "semantics: retry only if the operation is read-only or idempotent; "
      "if it may have side effects, first verify external state or ask the "
      "user. Do not retry blindly.";
  return out;
}

/* The pinned not-started brief (no code quote — the shape's whole point). */
static const char* kLoopNotStartedBrief =
    "The previous turn was interrupted before the cell started. No cell "
    "execution was recorded. Retry it if it is still needed.";

/* The serialized user-role message the derive's repair branch must produce
   (the model-visible pin: the brief rides the derived request VERBATIM, as a
   user message, before anything else of the turn). */
static std::string user_msg_json(const std::string& content) {
  json_value_t* m = json_new_object();
  json_object_set(m, "role", json_new_string("user"));
  json_object_set(m, "content", json_new_string(content.c_str()));
  char* raw = json_serialize(m);
  json_value_destroy(m);
  std::string out((raw != nullptr) ? raw : "");
  free(raw);
  return out;
}

TEST(TestLoop, TestRestartRepairsTheCutAfterTheCellAudit) {
  /* The crashed-tail restart, cut AFTER the cell audit (the plan's Task-3
     step 1a; spec §4 + §7 [13]): the committed cell.run with NO cell.result
     repairs BEFORE any engine runs — ONE atomic closer batch [repair
     (quoting the cell's seq + code), step.end, turn.end {reason
     interrupted}] — and the next derive carries the brief VERBATIM as a
     user message; the originals are byte-identical after (append-only) and
     the repaired tail is balanced.

     SUBSTRATE NOTE (the recorded WaveDB defect, same class as the sibling
     restart tests): a boot-restored session's OWN writes are invisible to
     THAT session's scans (durable — the next fresh session materializes
     them). The closer batch commits in the reopened session, so the batch's
     AFTERMATH is verified in the FRESH session: the closers sitting at seqs
     4..6 ahead of any engine record, and the fresh engine's derive (a
     restored session reads every pre-boot record) carrying the brief.
     VALGRIND EXCLUSION (the sibling convention recorded on
     TestRestartReplayRestoresSeqAndContext): scratch-disk tests spin under
     valgrind's emulation here — the leak proof for this class is the ASan
     suite (this test runs there unexcluded). */

  frame_config_t cfg = test_config();
  std::string dir = temp_dir_mkdtemp_sa();
  ASSERT_FALSE(dir.empty());
  std::string loc = dir + "/db";

  /* The cut: turn 1 opened, its step started, the cell audited at seq 3 —
     no result, no step.end, no turn.end (the tail a crash leaves). */
  wave_database_root_t* db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "cut after the audit", &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  const std::string cell_code = "print('interrupted mid-flight')";
  std::string seed[3];
  /* The seed ops ride the HEAP — the sync batch TRANSFERS the array's
     ownership and frees it on every path (the composer-never-frees rule). */
  frm_store_op_t* seed_ops =
      (frm_store_op_t*)get_clear_memory(3 * sizeof(frm_store_op_t));
  static const char* seed_types[3] = {"turn.start", "step.start", "cell.run"};
  for (size_t i = 0; i < 3; i++) {
    long long seq = (long long)i + 1;
    seed[i] = (i == 2)
                  ? make_cell_run_record(seq, sid, cell_code, 77)
                  : make_record_json(seq, sid, seed_types[i], 1,
                                     (i == 1) ? 1 : -1, NULL);
    char key[96];
    snprintf(key, sizeof(key), "%s/events/%020lld", sid.c_str(), seq);
    seed_ops[i].key = strdup(key);
    seed_ops[i].value = (uint8_t*)strdup(seed[i].c_str());
    seed_ops[i].value_len = seed[i].size();
  }
  ASSERT_EQ(_frame_sync_batch(f, seed_ops, 3, "cut-tail seed"), 0);
  frame_destroy(f);   /* the crash — no engine ran after the audit */
  wave_db_close(db);

  /* The reopened session: frame_resume RETURNS only after the closer
     batch's commit is confirmed (the sync family awaits the store's batch
     answer) — the repair runs before any engine exists on this handle. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* resumed = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(resumed, nullptr) << "the cut tail repairs at resume";
  frame_destroy(resumed);   /* no engine started: the closers only */
  wave_db_close(db);

  /* The fresh session: the repaired tail is durable, the engine's first
     derive reads it whole (pre-boot records are visible in a restored
     session — see the substrate note). */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* check = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(check, nullptr);
  json_value_t* events = load_events(check);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(json_size(events), 6u)
      << "the cut tail plus ONE closer batch, committed BEFORE the engine "
         "(nothing else wrote between them)";

  /* The originals are byte-identical after (append-only — the closer batch
     never mutated a record). */
  for (long long seq = 1; seq <= 3; seq++) {
    json_value_t* rec = record_at_seq(events, seq);
    ASSERT_NE(rec, nullptr) << "seed record " << seq;
    EXPECT_EQ(echo_record(rec), seed[seq - 1])
        << "seed record " << seq << " mutated";
  }

  /* The closers in the pinned order, contiguous seqs 4..6: repair (turn 1,
     the STARTED brief quoting the cell's seq 3 + its code verbatim),
     step.end {turn 1, step 1}, turn.end {turn 1, interrupted}. */
  json_value_t* rep = record_at_seq(events, 4);
  json_value_t* se = record_at_seq(events, 5);
  json_value_t* te = record_at_seq(events, 6);
  ASSERT_NE(rep, nullptr);
  ASSERT_NE(se, nullptr);
  ASSERT_NE(te, nullptr);
  EXPECT_TRUE(event_is(rep, "repair"));
  EXPECT_TRUE(event_is(se, "step.end"));
  EXPECT_TRUE(event_is(te, "turn.end"));
  json_value_t* rp = payload_of(rep);
  ASSERT_NE(rp, nullptr);
  EXPECT_EQ((long long)json_as_int(json_get(rp, "turn")), 1);
  ASSERT_NE(json_get(rp, "text"), nullptr);
  EXPECT_STREQ(json_as_string(json_get(rp, "text")),
               loop_started_brief(3, cell_code).c_str())
      << "the brief quotes the interrupted cell's seq + code (details "
         "upfront)";
  EXPECT_EQ(json_as_int(json_get(payload_of(se), "step")), 1);
  EXPECT_EQ((long long)json_as_int(json_get(payload_of(se), "turn")), 1);
  EXPECT_EQ(turn_end_kind(te), "interrupted")
      << "balance restored: the log's newest lifecycle record is a turn.end";
  EXPECT_EQ((long long)json_as_int(json_get(payload_of(te), "turn")), 1);

  /* The next derive carries the brief VERBATIM as a user message. */
  recording_model_t rm = {};
  rm.base.complete = recording_complete;
  rm.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"repaired"}}]})json");
  frame_set_model_backend(check, &rm.base);
  EXPECT_EQ(frame_run_loop(check), 0);
  ASSERT_EQ(rm.captured.size(), 1u);
  EXPECT_NE(rm.captured[0].find(user_msg_json(loop_started_brief(3, cell_code))),
            std::string::npos)
      << "the repair brief reaches the model VERBATIM as a user message";

  /* Still append-only after the engine's own run: the originals unchanged,
     the repair brief exactly once (the engine's turn-2 envelope never
     re-briefed). */
  json_value_destroy(events);
  events = load_events(check);
  ASSERT_NE(events, nullptr);
  for (long long seq = 1; seq <= 3; seq++) {
    json_value_t* rec = record_at_seq(events, seq);
    ASSERT_NE(rec, nullptr) << "seed record " << seq;
    EXPECT_EQ(echo_record(rec), seed[seq - 1])
        << "seed record " << seq << " mutated by the run";
  }
  EXPECT_EQ(count_type(events, "repair"), 1u);
  EXPECT_EQ(frame_is_done(check), 1);

  json_value_destroy(events);
  frame_destroy(check);
  wave_db_close(db);
  std::filesystem::remove_all(dir);
}

TEST(TestLoop, TestRestartRepairsTheCutBeforeTheCellAudit) {
  /* The crashed-tail restart, cut BEFORE any cell audit (the plan's Task-3
     step 1b; repair.spec's not-started wording): turn.start committed, NO
     step/cell records — the closers are [repair (the NOT-STARTED brief, no
     code quote), turn.end {reason interrupted}] and NO step.end exists (no
     open step; DSH's order holds). The disk/substrate discipline is the
     sibling's (see TestRestartRepairsTheCutAfterTheCellAudit): asserts ride
     the fresh session; VALGRIND EXCLUSION there applies to this test too. */

  frame_config_t cfg = test_config();
  std::string dir = temp_dir_mkdtemp_sa();
  ASSERT_FALSE(dir.empty());
  std::string loc = dir + "/db";

  /* The cut: ONLY the turn's opener committed — the crash fell between the
     model's turn decision and the cell audit. */
  wave_database_root_t* db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "cut before the cell", &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  std::string seed = make_record_json(1, sid, "turn.start", 1, -1, NULL);
  /* The seed op rides the HEAP — the sync batch transfers the array's
     ownership (see the sibling restart test's seed shape). */
  frm_store_op_t* seed_ops =
      (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
  char seed_key[96];
  snprintf(seed_key, sizeof(seed_key), "%s/events/%020lld", sid.c_str(),
           (long long)1);
  seed_ops[0].key = strdup(seed_key);
  seed_ops[0].value = (uint8_t*)strdup(seed.c_str());
  seed_ops[0].value_len = seed.size();
  ASSERT_EQ(_frame_sync_batch(f, seed_ops, 1, "cut-tail seed"), 0);
  frame_destroy(f);
  wave_db_close(db);

  /* The reopened session: the repair commits before the engine exists. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* resumed = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(resumed, nullptr) << "the not-started cut repairs at resume";
  frame_destroy(resumed);
  wave_db_close(db);

  /* The fresh session: [turn.start, repair {turn 1, the NOT-STARTED brief},
     turn.end {turn 1, interrupted}] — no step.end anywhere. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* check = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(check, nullptr);
  json_value_t* events = load_events(check);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(json_size(events), 3u)
      << "the opener + ONE closer batch: repair, turn.end — no step.end";

  json_value_t* rec1 = record_at_seq(events, 1);
  ASSERT_NE(rec1, nullptr);
  EXPECT_EQ(echo_record(rec1), seed) << "the original record is untouched";

  json_value_t* rep = record_at_seq(events, 2);
  json_value_t* te = record_at_seq(events, 3);
  ASSERT_NE(rep, nullptr);
  ASSERT_NE(te, nullptr);
  EXPECT_TRUE(event_is(rep, "repair"));
  json_value_t* rp = payload_of(rep);
  ASSERT_NE(rp, nullptr);
  EXPECT_EQ((long long)json_as_int(json_get(rp, "turn")), 1);
  ASSERT_NE(json_get(rp, "text"), nullptr);
  EXPECT_STREQ(json_as_string(json_get(rp, "text")), kLoopNotStartedBrief)
      << "the not-started shape: no code quote, the pinned wording";
  EXPECT_EQ(json_get(rp, "code"), nullptr)
      << "the not-started brief carries no code quote";
  EXPECT_TRUE(event_is(te, "turn.end"));
  EXPECT_EQ(turn_end_kind(te), "interrupted");
  EXPECT_EQ((long long)json_as_int(json_get(payload_of(te), "turn")), 1);

  /* The step.close never fired: no open step existed — the closers' order
     (repair, turn.end) is the whole batch. */
  EXPECT_EQ(count_type(events, "step.end"), 0u);
  EXPECT_EQ(count_type(events, "step.start"), 0u);
  EXPECT_EQ(count_type(events, "repair"), 1u) << "no second brief";

  json_value_destroy(events);
  frame_destroy(check);
  wave_db_close(db);
  std::filesystem::remove_all(dir);
}

/* --- the persona injection (the persona slice, spec §2-§3) ----------------

   The derive's second store trip (FRM_STORE_GET_NAMED over the persona
   subtree) composes the persona GROUP into the system prompt's FIRST block;
   a persona-less frame keeps today's byte-identical shape. The expected
   bytes are composed here through the PURE persona API with the turn
   instruction's byte-pinned text — the loop composes with the SAME base,
   so a prefix match pins the whole injection's shape. */

/* The turn instruction's bytes, pinned here byte-for-byte (loop.c's
   SA_LOOP_INSTRUCTION — the no-persona pin below is that text's proof). */
static const char TEST_LOOP_INSTRUCTION[] =
    "You drive one frame of an agent session. Your only tool is `execute`: "
    "its `code` argument is ONE python cell run in this frame's interpreter "
    "(one shared namespace per frame). Inside cells the injected `actor` "
    "module provides the verbs: actor.remember(key, value), "
    "actor.recall(key), actor.spawn(goal, context=None), actor.report(value).\n"
    "Finish the frame by calling actor.report inside your last cell (a "
    "completion declaration), or simply by answering WITHOUT a tool call.\n";

/* ONE direct store put through the store actor (the test's boot idiom —
   the same fire-and-post batch shape persona_records_install's install
   takes, pumped to quiescence): key/value as raw utf8 texts. */
static void test_store_put(wave_database_root_t* db, const char* key,
                           const char* value) {
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(*bp));
  bp->ops = (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
  bp->ops[0].key = strdup(key);
  bp->ops[0].value = (uint8_t*)strdup(value);
  bp->ops[0].value_len = strlen(value);
  bp->ops[0].is_delete = 0;
  bp->nops = 1;
  bp->op_name = "persona test put";
  bp->reply_to = NULL;
  bp->corr = 0;
  message_t m;
  m.type = (uint32_t)FRM_STORE_BATCH;
  m.payload = bp;
  m.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(wave_db_store_actor(db), &m));
  wave_db_pump(db);
}

/* The system prompt of a captured (serialized messages-array) model call.
   Malformed captures answer "" (EXPECT'd loud — the caller's asserts fail
   on the shape). */
static std::string test_system_content(const std::string& serialized) {
  json_value_t* arr =
      json_parse(serialized.c_str(), serialized.size(), NULL);
  EXPECT_NE(arr, nullptr) << serialized;
  std::string out;
  if (arr == nullptr) return out;
  EXPECT_EQ(json_type(arr), JSON_ARRAY);
  if (json_type(arr) == JSON_ARRAY && json_size(arr) >= 1) {
    json_value_t* sys = json_at(arr, 0);
    EXPECT_NE(sys, nullptr);
    json_value_t* content = (sys != nullptr) ? json_get(sys, "content") : NULL;
    EXPECT_NE(content, nullptr);
    if (content != NULL) out = std::string(json_as_string(content));
  }
  json_value_destroy(arr);
  return out;
}

/* The hammer's record through the pure loader (the injection expectations'
   source of the record's verbatim text). */
static persona_record_t* test_hammer_record(void) {
  char* record_json = persona_records_hammer_record();
  EXPECT_NE(record_json, nullptr);
  persona_record_t* rec = NULL;
  EXPECT_EQ(persona_record_load(record_json, &rec), 0);
  EXPECT_NE(rec, nullptr);
  free(record_json);
  return rec;   /* the caller destroys */
}

TEST(TestLoop, TestNoPersonaIsByteIdentical) {
  /* A persona-less frame's captured prompt == the pre-slice shape EXACTLY:
     the first bytes are the turn instruction's, then the goal line — the
     whole system block pinned byte-for-byte (the standing derive pins all
     prove the same shape; this one names it). */
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "pin the shape", &cfg);
  ASSERT_NE(f, nullptr);

  recording_model_t rm = {};
  rm.base.complete = recording_complete;
  rm.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"pinned"}}]})json");
  frame_set_model_backend(f, &rm.base);

  EXPECT_EQ(frame_run_loop(f), 0);
  ASSERT_EQ(rm.captured.size(), 1u);
  std::string content = test_system_content(rm.captured[0]);

  std::string expected = std::string(TEST_LOOP_INSTRUCTION) + "Goal: pin the shape\n";
  EXPECT_EQ(content, expected) << "the persona-less system block moved";
  EXPECT_EQ(content.find("PERSONA SPEC"), std::string::npos);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestPersonaMissingRecordFallsBackLoud) {
  /* cfg carries a persona name whose record is NOT installed: the derive
     NEVER fails on the persona read — the built-in base rides (byte-
     identical to the no-persona shape) and a REAL model turn ran. */
  frame_config_t cfg = test_config();
  cfg.persona_name = "hammer";
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);   /* deliberately NOT persona_records_install'd */
  frame_t* f = frame_create(db, NULL, "fallback turns", &cfg);
  ASSERT_NE(f, nullptr);

  recording_model_t rm = {};
  rm.base.complete = recording_complete;
  rm.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"the turn ran"}}]})json");
  frame_set_model_backend(f, &rm.base);

  EXPECT_EQ(frame_run_loop(f), 0) << "a persona read never fails a turn";
  ASSERT_EQ(rm.captured.size(), 1u);
  std::string content = test_system_content(rm.captured[0]);

  std::string expected =
      std::string(TEST_LOOP_INSTRUCTION) + "Goal: fallback turns\n";
  EXPECT_EQ(content, expected)
      << "the fallback is the built-in base alone — the byte-identical shape";
  EXPECT_EQ(content.find("PERSONA SPEC"), std::string::npos)
      << "never a half-persona render";

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestPersonaPlaceholderCatalogResolvesLive) {
  /* The {…} placeholder catalog rides the LIVE derive exactly as the pure
     tests pin it: {USER_NAME} resolves from the user-context record, a
     recognized-but-missing key substitutes "" (never a raw token), an
     unrecognized token stays VISIBLE verbatim. */
  frame_config_t cfg = test_config();
  cfg.persona_name = "greeter";
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  test_store_put(db, "personas/greeter/record",
                 "{\"version\":1,\"name\":\"greeter\",\"text\":\"Hello "
                 "{USER_NAME}, dear {USER_TITLE}, keep {WHATEVER} visible.\","
                 "\"placement\":\"first\"}");
  /* The context record's field key = the placeholder's lowercased REMAINDER
     (persona.h's catalog pin): {USER_NAME} reads the "name" field. */
  test_store_put(db, "personas/greeter/user-context",
                 "{\"name\":\"Victor\"}");
  frame_t* f = frame_create(db, NULL, "greet me", &cfg);
  ASSERT_NE(f, nullptr);

  recording_model_t rm = {};
  rm.base.complete = recording_complete;
  rm.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"greeted"}}]})json");
  frame_set_model_backend(f, &rm.base);

  EXPECT_EQ(frame_run_loop(f), 0);
  ASSERT_EQ(rm.captured.size(), 1u);
  std::string content = test_system_content(rm.captured[0]);

  /* The compose's group order: the substituted persona text, THEN the
     context record's render (the "name: Victor" block), then the base. */
  std::string expected =
      "Hello Victor, dear , keep {WHATEVER} visible.\n\nname: Victor\n\n" +
      std::string(TEST_LOOP_INSTRUCTION) + "Goal: greet me\n";
  EXPECT_EQ(content, expected);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestLoop, TestPersonaInheritsIntoTheSpawnedChild) {
  /* The config inheritance's END-TO-END half (spec §3): a spawned child
     adopts the parent's persona record — the child's OWN derive composes
     the block into ITS prompt. */
  frame_config_t cfg = test_config();
  cfg.persona_name = "hammer";
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  ASSERT_EQ(persona_records_install(db), 0);
  test_store_put(db, "personas/hammer/user-context",
                 "{\"user_name\":\"Victor\"}");
  frame_t* parent = frame_create(db, NULL, "parent goal", &cfg);
  ASSERT_NE(parent, nullptr);

  recording_model_t rm = {};
  rm.base.complete = recording_complete;
  rm.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"the child replied"}}]})json");
  frame_set_model_backend(parent, &rm.base);   /* the borrowed override the
                                                  spawn hands the child */

  frame_t* child = frame_spawn(parent, "child goal", NULL);
  ASSERT_NE(child, nullptr);
  /* Admission-only spawn (no live engine on the parent): the caller drives
     the adopted child — the engine-side backend inheritance never ran, so
     the driver-injected backend is set on the child directly (the spawn
     inheritance under test is the PERSONA name's, Task 3's create site). */
  frame_set_model_backend(child, &rm.base);

  EXPECT_EQ(frame_run_loop(child), 0);
  ASSERT_EQ(rm.captured.size(), 1u);
  std::string content = test_system_content(rm.captured[0]);

  persona_record_t* rec = test_hammer_record();
  ASSERT_NE(rec->text, nullptr);
  std::string hammer_text = std::string(rec->text);
  persona_record_destroy(rec);
  ASSERT_GE(content.size(), hammer_text.size());
  EXPECT_EQ(content.compare(0, hammer_text.size(), hammer_text), 0)
      << "the child's prompt opens with the inherited persona block";
  EXPECT_NE(content.find("user_name: Victor"), std::string::npos);
  EXPECT_NE(content.find("Goal: child goal\n"), std::string::npos);

  frame_destroy(parent);
  frame_destroy(child);
  wave_db_close(db);
}

#endif /* SA_HAS_WDB */

/* The reopened-walk probe runs REAL tool cycles through the frame's own
   pyrt (py_agent_init + the scripted model drive cells), so it needs BOTH
   gates: test_loop.cpp is registered under the WDB gate in test/
   CMakeLists.txt, and the python half is the nested real form. */
#if defined(SA_HAS_WDB) && defined(SA_HAS_PYTHON)

TEST(TestLoop, TestResumedEngineSeesItsOwnInSessionRecords) {
  /* The substrate probe (the Task-3 implementer's engine-level claim): a
     RESUMED session's engine runs TWO tool cycles on the SAME reopened
     handle; the SECOND turn's derived context must carry the FIRST turn's
     cell result (in-session post-reopen writes visible to the SAME
     session's derives). Scratch-disk class (the documented valgrind
     exclusion; ASan covers leaks). */
  frame_config_t cfg = test_config();
  std::string dir = temp_dir_mkdtemp_sa();
  ASSERT_FALSE(dir.empty());
  std::string loc = dir + "/db";

  /* Boot 1: a durable pre-restart history to resume. */
  wave_database_root_t* db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "amnesia probe", &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  EXPECT_EQ(frame_append_msg(f, "user", "remember seven"), 0);
  frame_destroy(f);
  wave_db_close(db);

  /* Boot 2 (the reopened session): TWO tool cycles on the SAME handle. */
  py_agent_init();
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* resumed = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(resumed, nullptr);
  std::vector<std::string> replies;
  replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"print('turn one output')\"}"}}]}}]})json");
  replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"all done"}}]})json");
  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(resumed, &sm.base);
  int loop_rc = frame_run_loop(resumed);
  ASSERT_EQ(loop_rc, 0) << "two scripted cycles completed on the resumed frame";
  frame_destroy(resumed);
  wave_db_close(db);

  /* THE CROSS-BOOT CHECK: the same events range read by a FRESH handle —
     the durable truth vs what the same session's derive saw. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* fresh = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(fresh, nullptr);
  json_value_t* fresh_events = load_events(fresh);
  ASSERT_NE(fresh_events, nullptr);
  printf("[  PROBE  ] fresh-handle event count = %d\n", (int)json_size(fresh_events));
  for (size_t i = 0; i < json_size(fresh_events); i++) {
    json_value_t* rec = json_at(fresh_events, i);
    json_value_t* t = json_get(rec, "type");
    printf("[  PROBE  ]   seq=%lld type=%s\n", (long long)json_as_int(json_get(rec, "seq")),
           (t != NULL ? json_as_string(t) : "?"));
  }
  fflush(stdout);
  { size_t ncr = 0;
    for (size_t i = 0; i < json_size(fresh_events); i++) {
      json_value_t* rec = json_at(fresh_events, i);
      json_value_t* t = json_get(rec, "type");
      if (t != NULL && strcmp(json_as_string(t), "cell.result") == 0) ncr++;
    }
    printf("[  PROBE  ] fresh handles %zu cell.result records\n", ncr);
  }
  fflush(stdout);
  json_value_destroy(fresh_events);
  frame_destroy(fresh);
  wave_db_close(db);

  /* The same-session capture asserts the HEALTHY contract (the WaveDB
     reopen-walk publication fix, deps/wavedb 2cd6161): the SAME session's
     turn-1 cell.result record is visible to its own turn-2 derive — the
     projected `cell result (status 0): ...` line. (The stdout text itself
     is a cell-backend gap, not a substrate one: the pyrt exec-only backend
     never captures print() output — py_subprocess's backend does — asserted
     here as the result line's PRESENCE, not its text.) */
  EXPECT_NE(sm.captured[1].find("remember seven"), std::string::npos)
      << "the replayed pre-restart history is visible (sanity)";
  EXPECT_NE(sm.captured[1].find("cell result (status 0)"),
            std::string::npos)
      << "THE PROBE: the SAME session's turn-1 record is visible to its "
         "own turn-2 derive (the fresh view printed the full 12-record log)";

  std::filesystem::remove_all(dir);
}

/* --- the persona's FIRST-BLOCK pin, over two REAL turns (the persona
   slice, spec §2): two model calls share the persona GROUP's bytes exactly
   — the cache-stable prefix rules (matrix row 36) hold through the live
   derive, and the block sits before the turn instruction. Uses the
   python-gated scripted model: turn 1 calls execute (a cell), turn 2
   answers content. */
TEST(TestLoop, TestPersonaInjectedAsTheFirstBlock) {
  py_agent_init();
  frame_config_t cfg = test_config();
  cfg.persona_name = "hammer";
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  ASSERT_EQ(persona_records_install(db), 0);
  test_store_put(db, "personas/hammer/user-context",
                 "{\"user_name\":\"Victor\"}");
  frame_t* f = frame_create(db, NULL, "do a thing", &cfg);
  ASSERT_NE(f, nullptr);

  std::string turn1 =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"import actor\\nactor.remember('n', 7)\"}"}}]}}]})json";
  std::string turn2 =
      R"json({"choices":[{"message":{"role":"assistant","content":"all done"}}]})json";
  std::vector<std::string> replies = {turn1, turn2};

  scripted_model_t sm = {};
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = NULL;
  frame_set_model_backend(f, &sm.base);

  EXPECT_EQ(frame_run_loop(f), 0);
  ASSERT_EQ(sm.captured.size(), 2u) << "two real model turns ran";

  /* The expected FIRST block: the hammer GROUP composed through the PURE
     api with the SAME base the loop composes with — the pinned instruction
     text — plus the user-context record and the execute guidance attached.
     Whatever the loop's placement rules do, they must agree byte-for-byte. */
  persona_record_t* rec = test_hammer_record();
  ASSERT_NE(rec, nullptr);
  const char* tools[] = {"execute"};
  char* expected_prefix_c = persona_compose(
      rec, "{\"user_name\":\"Victor\"}", tools, 1, TEST_LOOP_INSTRUCTION);
  ASSERT_NE(expected_prefix_c, nullptr);
  std::string expected_prefix(expected_prefix_c);
  free(expected_prefix_c);
  persona_record_destroy(rec);

  std::string c0 = test_system_content(sm.captured[0]);
  std::string c1 = test_system_content(sm.captured[1]);

  /* The persona block IS the prompt's first block: the composed GROUP's
     exact bytes open the prompt, on BOTH turns. */
  ASSERT_GE(c0.size(), expected_prefix.size());
  ASSERT_GE(c1.size(), expected_prefix.size());
  EXPECT_EQ(c0.compare(0, expected_prefix.size(), expected_prefix), 0)
      << "turn 1: the prompt does not open with the composed persona block: "
      << c0;
  EXPECT_EQ(c1.compare(0, expected_prefix.size(), expected_prefix), 0)
      << "turn 2: the prompt does not open with the composed persona block: "
      << c1;

  /* Byte-stability across turns: the FIRST-block bytes are IDENTICAL — the
     cache-stable prefix claim, made observable. */
  EXPECT_EQ(c0.substr(0, expected_prefix.size()),
            c1.substr(0, expected_prefix.size()));

  /* The hammer's text rides VERBATIM (the record's body inside the block),
     the context record renders its key: value line, the execute guidance
     attaches (the frame's canned tool surface), and the base instructions
     follow the block. */
  EXPECT_NE(c0.find("## Core stance"), std::string::npos)
      << "the hammer's markdown inside the block";
  EXPECT_NE(c0.find("user_name: Victor"), std::string::npos);
  EXPECT_NE(c0.find("## execute"), std::string::npos);
  EXPECT_NE(c0.find("You drive one frame"), std::string::npos)
      << "the base instructions follow the persona block";
  size_t persona_pos = c0.find("# PERSONA SPEC v1");
  size_t instr_pos = c0.find("You drive one frame");
  ASSERT_NE(persona_pos, std::string::npos);
  ASSERT_NE(instr_pos, std::string::npos);
  EXPECT_LT(persona_pos, instr_pos) << "persona block FIRST, then the base";

  frame_destroy(f);
  wave_db_close(db);
}

#endif /* the reopened-walk probe's WDB+PYTHON gate */


