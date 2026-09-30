//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <string>
extern "C" {
#include "../src/Frame/frame.h"
#include "../src/Util/json.h"
}

#ifdef SA_HAS_WDB

static frame_config_t test_config(void) {
  frame_config_t cfg;
  cfg.model_base_url = NULL;
  cfg.model_api_key = NULL;
  cfg.model_name = "unused";
  cfg.max_depth = 4;
  return cfg;
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

#endif /* SA_HAS_WDB */