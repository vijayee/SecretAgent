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

TEST(TestLoop, TestTurnLimitFailsLoud) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "spiral forever", &cfg);
  ASSERT_NE(f, nullptr);

  /* Always a tool call — an empty queue re-answers the fallback. */
  std::string fallback =
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":"{\"code\":\"pass\"}"}}]}}]})json";
  std::vector<std::string> replies = {};
  scripted_model_t sm = {};   /* zero-init: model_backend_t's additive vtable members (submit) default NULL — the sync-scripted shape */
  sm.base.complete = scripted_complete;
  sm.replies = &replies;
  sm.steer_frame = NULL;
  sm.steer_text = NULL;
  sm.steer_on = 0;
  sm.fallback = &fallback;

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
  on_done(on_done_ctx, 200, heap_body, strlen(heap_body), NULL);
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
  hm.held_fn(hm.held_ctx, 200, body, strlen(body), NULL);

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

#endif /* SA_HAS_WDB */
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


