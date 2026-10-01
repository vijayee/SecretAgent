//
// Created by victor on 9/29/26.
//

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

/* NOTE on the guard idiom: C preprocessor macros cannot be joined with `&&` —
   the plan's listing note is honored by writing the real form below:
     #if defined(SA_HAS_WDB) && defined(SA_HAS_PYTHON)   */
#if defined(SA_HAS_PYTHON)

static frame_config_t test_config(void) {
  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));   /* additive fields (pool, timeout) default sensibly */
  cfg.model_base_url = NULL;
  cfg.model_api_key = NULL;
  cfg.model_name = "unused";
  cfg.max_depth = 4;
  return cfg;
}

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

  scripted_model_t sm;
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

  /* TEMP DEBUG */
  { char* dbg = frame_debug_events(f); fprintf(stderr, "DBGEVENTS=%s\n", dbg ? dbg : "(null)"); for (auto& s : sm.captured) fprintf(stderr, "DBGCAPTURE=%s\n", s.c_str()); if (dbg) free(dbg); }

  /* Audit trail: exactly four records — cell.run, state.remember (n = 7),
     frame.report (the cell's own completion report), cell.result. */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(json_size(events), 4u);
  ASSERT_TRUE(event_is(json_at(events, 0), "cell.run"));
  ASSERT_TRUE(event_is(json_at(events, 1), "state.remember"));
  ASSERT_TRUE(event_is(json_at(events, 2), "frame.report"));
  ASSERT_TRUE(event_is(json_at(events, 3), "cell.result"));

  json_value_t* remember_payload = json_get(json_at(events, 1), "payload");
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

  scripted_model_t sm;
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
  scripted_model_t sm;
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
  scripted_model_t sm;
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

#endif /* python gate */

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
  recording_model_t rm;
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

#endif /* SA_HAS_WDB */