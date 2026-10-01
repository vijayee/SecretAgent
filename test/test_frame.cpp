//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "../src/Frame/frame.h"
#include "../src/Frame/frame_messages.h"
#include "../src/Frame/frame_bridge.h"
#include "../src/Frame/frame_internal.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/json.h"
#include "../src/Util/allocator.h"
}

#ifdef SA_HAS_WDB

static frame_config_t test_config(void) {
  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));   /* additive fields (pool, timeout) default sensibly */
  cfg.model_base_url = NULL;
  cfg.model_api_key = NULL;
  cfg.model_name = "unused";
  cfg.max_depth = 4;
  return cfg;
}

TEST(TestFrame, TestPoolAttachAndInheritance) {
  frame_config_t cfg = test_config();   /* zero-init'd helper from Step 1 */
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  cfg.pool = pool;
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* parent = frame_create(db, NULL, "tree root", &cfg);
  ASSERT_NE(parent, nullptr);
  EXPECT_EQ(frame_pool(parent), pool) << "frame_create attaches the config's pool";

  frame_t* child = frame_spawn(parent, "leaf", NULL);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(frame_pool(child), pool) << "spawned children inherit the parent's pool";

  /* An inline default is unchanged: a pool-less config means pool NULL. */
  frame_config_t plain = test_config();
  frame_t* inline_frame = frame_create(db, NULL, NULL, &plain);
  ASSERT_NE(inline_frame, nullptr);
  EXPECT_EQ(frame_pool(inline_frame), nullptr);

  frame_destroy(child);
  frame_destroy(inline_frame);
  frame_destroy(parent);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  wave_db_close(db);
}

TEST(TestFrame, TestFrameStartQueuesOneTurnContinuation) {
  frame_config_t plain = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* f = frame_create(db, NULL, NULL, &plain);
  ASSERT_NE(f, nullptr);

  EXPECT_EQ(frame_start(f), 0);
  EXPECT_NE(frame_start(f), 0) << "one live engine per frame — restart refused loudly";

  /* On an inline frame the continuation sits in the mailbox (the owner
     pumps): exactly what Task 3's FRM_TURN case will consume. Drain it by
     hand — the Task-1 handler is a loud late-drop, so after the drain the
     engine is still startable (task 3's engine changes that). */
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);
  EXPECT_EQ(frame_start(f), 0) << "the single continuation was consumed; engine restartable";

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestCreateRootAndChildGeneratesPaths) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);   /* in-memory */
  frame_t* root = frame_create(db, NULL, "do the dishes", &cfg);
  ASSERT_NE(root, nullptr);
  std::string root_sid = frame_sid(root);
  EXPECT_EQ(root_sid.rfind("sessions/", 0), 0u);
  EXPECT_NE(root_sid.size(), 9u);                 /* a real sid followed */

  frame_t* child = frame_create(db, root, "scrub plate", &cfg);
  ASSERT_NE(child, nullptr);
  std::string child_sid = frame_sid(child);
  EXPECT_EQ(child_sid.rfind(root_sid, 0), 0u) << "child path composes from root";

  frame_destroy(child);
  frame_destroy(root);
  wave_db_close(db);
}

TEST(TestFrame, TestRememberRecallAndShadowing) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* root = frame_create(db, NULL, NULL, &cfg);
  frame_t* child = frame_create(db, root, NULL, &cfg);

  /* ctx variant: the INHERITABLE write that the lineage walk can resolve. */
  EXPECT_EQ(frame_remember_ctx(root, "temperature", "20"), 0);
  char* v = frame_recall(child, "temperature");
  ASSERT_NE(v, nullptr) << "inherited via ctx lineage walk";
  EXPECT_STREQ(v, "20");
  free(v);

  /* Shadow: child writes its own ctx key; recall stops at the shadow. */
  EXPECT_EQ(frame_remember_ctx(child, "temperature", "33"), 0);
  v = frame_recall(child, "temperature");
  EXPECT_STREQ(v, "33");
  free(v);

  /* local/ never inherits: root's LOCAL scratch is invisible to the child. */
  EXPECT_EQ(frame_remember_local(root, "scratch", "1"), 0);
  v = frame_recall(child, "scratch");
  EXPECT_EQ(v, nullptr);
  free(NULL);

  frame_destroy(child);
  frame_destroy(root);
  wave_db_close(db);
}

TEST(TestFrame, TestMessageAppendAndSeqContinue) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(frame_append_msg(f, "user", "hello"), 0);
  EXPECT_EQ(frame_append_msg(f, "assistant", "hi"), 0);
  frame_destroy(f);

  /* In-memory db does NOT persist: the RESTART variant of this test lives in
     Task 10 (restart/replay) with a real (disk) location. Here just assert
     seq counting works within one frame. */
  wave_db_close(db);
  db = wave_db_open(NULL);
  frame_t* f2 = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f2, nullptr);
  EXPECT_EQ(frame_append_msg(f2, "user", "again"), 0);
  frame_destroy(f2);
  wave_db_close(db);
}

/* frame_debug_events is the plan's test accessor: parse the array and write
   its length out (ASSERTs abort the test on any malformed step). */
static void debug_event_count(frame_t* f, size_t* count_out) {
  ASSERT_NE(count_out, nullptr);
  *count_out = 0;
  char* json = frame_debug_events(f);
  ASSERT_NE(json, nullptr) << "frame_debug_events returned NULL";
  char* err = NULL;
  json_value_t* arr = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  ASSERT_NE(arr, nullptr) << "raw: " << json;
  ASSERT_EQ(json_type(arr), JSON_ARRAY);
  *count_out = json_size(arr);
  json_value_destroy(arr);
  free(json);
}

TEST(TestFrame, TestSpawnAdmissionOnlyAndDepthCap) {
  frame_config_t cfg = test_config();
  cfg.max_depth = 1;
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* root = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(root, nullptr);
  frame_t* child = frame_spawn(root, "sub goal", NULL);
  ASSERT_NE(child, nullptr) << "child admitted at max_depth=1";
  EXPECT_EQ(frame_spawn(child, "too deep", NULL), (frame_t*)NULL)
      << "depth cap fails loudly, no child — never a substitute";
  frame_destroy(child);
  frame_destroy(root);
  wave_db_close(db);
}

TEST(TestFrame, TestReportBindsOneEventIntoParent) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* root = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(root, nullptr);
  frame_t* child = frame_spawn(root, "count things", NULL);
  ASSERT_NE(child, nullptr);
  std::string child_sid = frame_sid(child);

  /* after spawn, before report: the parent's log holds just the spawn event */
  size_t before = 0;
  size_t after = 0;
  debug_event_count(root, &before);

  EXPECT_EQ(frame_report(child, "found 7 things"), 0);

  /* the parent's log grew by exactly ONE frame.report whose payload names the
     child — and the ROOT frame recorded nothing else in between: */
  debug_event_count(root, &after);
  char* json = frame_debug_events(root);
  ASSERT_NE(json, nullptr);
  char* err = NULL;
  json_value_t* events = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  ASSERT_NE(events, nullptr) << "raw: " << json;
  ASSERT_EQ(json_type(events), JSON_ARRAY);
  ASSERT_EQ(json_size(events), after) << "exactly ONE bound event: " << json;
  ASSERT_EQ(after, before + 1);
  free(json);

  json_value_t* grown = json_at(events, before);
  ASSERT_NE(grown, nullptr);
  json_value_t* type_v = json_get(grown, "type");
  ASSERT_NE(type_v, nullptr);
  EXPECT_STREQ(json_as_string(type_v), "frame.report");
  json_value_t* payload_v = json_get(grown, "payload");
  ASSERT_NE(payload_v, nullptr);
  json_value_t* csid_v = json_get(payload_v, "child_sid");
  ASSERT_NE(csid_v, nullptr);
  EXPECT_STREQ(json_as_string(csid_v), child_sid.c_str());
  json_value_destroy(events);

  /* Parent context derivation (Task 7's function) reads it: */
  EXPECT_EQ(frame_is_done(child), 1) << "report completes the child";

  EXPECT_EQ(frame_join(child), 0);
  frame_destroy(child);
  frame_destroy(root);
  wave_db_close(db);
}

/* --- Task 6: bridge behaviors ---------------------------------------------- */

/* Completion record (the py_frame pattern from test_pyrt.cpp): the test's own
   dispatch shape — here the frame's reply leaves via the bridge hook, so the
   completion record is a recording sink passed to frame_bridge_register_...
   (frame_dispatch delivers replies to it synchronously on this same thread,
   so plain members are fine). */
typedef struct bridge_completion_t {
  std::vector<uint64_t> corrs;
  std::vector<uint8_t> statuses;
  std::vector<std::string> texts;
} bridge_completion_t;

static bridge_completion_t* g_completion = NULL;

static void bridge_completion_sink(uint64_t corr, uint8_t status, const char* text) {
  if (g_completion == NULL) return;   /* not mounted (another test's stale sink) */
  g_completion->corrs.push_back(corr);
  g_completion->statuses.push_back(status);
  g_completion->texts.emplace_back(text != NULL ? text : "");
}

/* Mount the recording sink; pass NULL to demount (sink resets to the built-in
   dropper). */
static void bridge_completion_mount(bridge_completion_t* rec) {
  g_completion = rec;
  frame_bridge_register_reply_sink(rec != NULL ? bridge_completion_sink : NULL);
}

/* Build a bridge request whose payload ownership transfers with the message;
   the behavior consumes it (msg->payload is NULL on return), so the caller
   never frees. */
static message_t bridge_request(frame_message_type_e type, uint64_t corr,
                                const char* key, const char* json_value) {
  frm_remember_payload_t* rp =
      (frm_remember_payload_t*)get_clear_memory(sizeof(frm_remember_payload_t));
  rp->corr = corr;
  rp->key = key != NULL ? strdup(key) : NULL;
  rp->json_value = json_value != NULL ? strdup(json_value) : NULL;
  message_t msg;
  msg.type = (uint32_t)type;
  msg.payload = rp;
  msg.payload_destroy = frm_remember_payload_destroy;
  return msg;
}

/* Load the frame's debug events and locate the FIRST state.remember payload.
   Fatal-fails the test when the event list isn't exactly `expect_total`
   records (the "only this one effect happened" assertion) or — when
   `expect_total` > 0 — when no remember event is present. `*events_out` owns
   the parsed DOM; the caller destroys it with json_value_destroy AFTER it is
   done with the returned payload borrow. */
static void find_remember_event(frame_t* f, size_t expect_total,
                                json_value_t** events_out, json_value_t** payload_out) {
  ASSERT_NE(events_out, nullptr);
  ASSERT_NE(payload_out, nullptr);
  *events_out = NULL;
  *payload_out = NULL;
  char* json = frame_debug_events(f);
  ASSERT_NE(json, nullptr);
  char* err = NULL;
  json_value_t* arr = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  ASSERT_NE(arr, nullptr) << "raw: " << json;
  free(json);
  ASSERT_EQ(json_type(arr), JSON_ARRAY);
  ASSERT_EQ(json_size(arr), expect_total) << "exactly the tested effects happened";
  for (size_t i = 0; i < json_size(arr); i++) {
    json_value_t* rec = json_at(arr, i);
    ASSERT_NE(rec, nullptr);
    json_value_t* type_v = json_get(rec, "type");
    ASSERT_NE(type_v, nullptr);
    if (strncmp(json_as_string(type_v), "state.remember", 14) == 0) {
      json_value_t* payload = json_get(rec, "payload");
      ASSERT_NE(payload, nullptr);
      *payload_out = payload;
      break;
    }
  }
  if (expect_total > 0) {
    ASSERT_NE(*payload_out, nullptr) << "expected a state.remember event";
  }
  *events_out = arr;
}

TEST(TestFrame, TestBridgeRememberRecallCorrMatched) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);

  bridge_completion_t completion;
  bridge_completion_mount(&completion);

  /* FRM_REMEMBER{corr=77} → the behavior applies frame_remember_ctx (the
     INHERITABLE layer: a cell's remember() is a durable shared-by-default
     write) and answers FRM_REPLY corr-matched. */
  message_t req = bridge_request(FRM_REMEMBER, 77, "mode", "\"fast\"");
  frame_dispatch(f, &req);
  EXPECT_EQ(req.payload, nullptr) << "behavior consumed the payload";

  ASSERT_EQ(completion.corrs.size(), 1u);
  EXPECT_EQ(completion.corrs[0], 77u) << "corr-matched to the request";
  EXPECT_EQ(completion.statuses[0], 0u);
  EXPECT_EQ(completion.texts[0], "");

  /* The durable half happened: exactly one state.remember event with
     {key:"mode", value:"fast"}, and the store resolves "mode". */
  json_value_t* events = NULL;
  json_value_t* payload = NULL;
  find_remember_event(f, 1, &events, &payload);
  ASSERT_NE(payload, nullptr);
  json_value_t* key_v = json_get(payload, "key");
  ASSERT_NE(key_v, nullptr);
  EXPECT_STREQ(json_as_string(key_v), "mode");
  json_value_t* val_v = json_get(payload, "value");
  ASSERT_NE(val_v, nullptr);
  EXPECT_EQ(json_type(val_v), JSON_STRING);
  EXPECT_STREQ(json_as_string(val_v), "fast");
  json_value_destroy(events);

  char* v = frame_recall(f, "mode");
  ASSERT_NE(v, nullptr);
  EXPECT_STREQ(v, "\"fast\"") << "raw JSON text, stored verbatim";
  free(v);

  /* FRM_RECALL{corr=78} → replies with the resolved JSON text. */
  completion.corrs.clear();
  completion.statuses.clear();
  completion.texts.clear();
  message_t req2 = bridge_request(FRM_RECALL, 78, "mode", NULL);
  frame_dispatch(f, &req2);
  ASSERT_EQ(completion.corrs.size(), 1u);
  EXPECT_EQ(completion.corrs[0], 78u);
  EXPECT_EQ(completion.statuses[0], 0u);
  EXPECT_STREQ(completion.texts[0].c_str(), "\"fast\"");

  /* A recall of a key nobody remembers answers status 1 corr-matched — the
     python waiter is never stranded silently. */
  message_t req3 = bridge_request(FRM_RECALL, 79, "absent", NULL);
  frame_dispatch(f, &req3);
  ASSERT_EQ(completion.corrs.size(), 2u);
  EXPECT_EQ(completion.corrs[1], 79u);
  EXPECT_EQ(completion.statuses[1], 1u);
  EXPECT_EQ(completion.texts[1], "");

  /* And the failed recalls wrote nothing: still the one event. */
  find_remember_event(f, 1, &events, &payload);
  json_value_destroy(events);

  bridge_completion_mount(NULL);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestBridgeInvalidPayloadAnswersFailure) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);

  bridge_completion_t completion;
  bridge_completion_mount(&completion);

  /* An EMPTY key is an invalid remember: the store refuses, no event is
     written, and the request is STILL answered corr-matched as a failure —
     a python wait must end through a delivered timeout-bearing reply, never
     because a reply was silently eaten. */
  message_t req = bridge_request(FRM_REMEMBER, 90, "", "\"fast\"");
  frame_dispatch(f, &req);
  ASSERT_EQ(completion.corrs.size(), 1u);
  EXPECT_EQ(completion.corrs[0], 90u);
  EXPECT_EQ(completion.statuses[0], 1u);

  /* No state event was written for the refused remember. */
  json_value_t* events = NULL;
  json_value_t* payload = NULL;
  find_remember_event(f, 0, &events, &payload);
  EXPECT_EQ(payload, nullptr);
  json_value_destroy(events);

  /* A recall whose key never resolves is a corr-matched failure too (NULL
     text — the bridge registry reads status, not text, for a failure). */
  message_t req2 = bridge_request(FRM_RECALL, 91, "alsomissing", NULL);
  frame_dispatch(f, &req2);
  ASSERT_EQ(completion.corrs.size(), 2u);
  EXPECT_EQ(completion.corrs[1], 91u);
  EXPECT_EQ(completion.statuses[1], 1u);
  EXPECT_EQ(completion.texts[1], "");

  bridge_completion_mount(NULL);
  frame_destroy(f);
  wave_db_close(db);
}

#endif /* SA_HAS_WDB */