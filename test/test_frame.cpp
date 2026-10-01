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
  /* A POOLED frame requires a POOLED store (the Task-2 guard refuses the
     mixed shape at create — a pooled frame posting into an un-pumped inline
     store mailbox would hang): the store actor rides the SAME pool. */
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_t* parent = frame_create(db, NULL, "tree root", &cfg);
  ASSERT_NE(parent, nullptr);
  EXPECT_EQ(frame_pool(parent), pool) << "frame_create attaches the config's pool";

  /* frame_spawn is a synchronous store API — inline-store-only (§5): on this
     pooled store it refuses loud, so the CHILD-inherits-pool assertion rides
     frame_create's parent link instead (the same _frame_alloc inheritance). */
  frame_t* child = frame_create(db, parent, "leaf", &cfg);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(frame_pool(child), pool) << "children inherit the pool down the lineage";

  /* An inline default is unchanged: a pool-less config means pool NULL. */
  frame_config_t plain = test_config();
  frame_t* inline_frame = frame_create(db, NULL, NULL, &plain);
  ASSERT_NE(inline_frame, nullptr);
  EXPECT_EQ(frame_pool(inline_frame), nullptr);

  frame_destroy(child);
  frame_destroy(inline_frame);
  frame_destroy(parent);
  scheduler_pool_stop(pool);      /* documented order: stop, close, destroy */
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}

TEST(TestFrame, TestResumePoolMismatchRefusesLoud) {
  /* frame_resume resolves the cfg's pool BEFORE the guard fires: a resumed
     frame whose pool disagrees with the store's is a mailbox nobody pumps
     on one side — refuse loud now, never hang at first pump. */
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);

  /* A POOLED frame on a POOLED store — the working shape whose birth record
     must be visible to resume. */
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_config_t pooled = test_config();
  pooled.pool = pool;
  frame_t* f = frame_create(db, NULL, "pooled resume pin", &pooled);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  frame_destroy(f);

  /* The pinned direction: resume that frame with a cfg that carries NO
     pool. The RESOLVED frame pool (NULL) mismatches the pooled store's. */
  frame_config_t plain = test_config();
  EXPECT_EQ(frame_resume(db, sid.c_str(), &plain), nullptr)
      << "a pooled store's frame resumed pool-less must refuse, not hang";

  /* The landed direction still fires at the right time: a POOLED resume cfg
     against an inline-store root. */
  wave_database_root_t* inline_db = wave_db_open(NULL);
  ASSERT_NE(inline_db, nullptr);
  EXPECT_EQ(frame_resume(inline_db, sid.c_str(), &pooled), nullptr)
      << "a POOLED resume cfg on an inline store must refuse, not hang";

  wave_db_close(inline_db);
  wave_db_close(db);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
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
   Since Task 2 (the store actor) a bridge verb's corr answer is routed by the
   FRM_STORE_REPLY router: the dispatch composes and posts the store message,
   the pump runs the store actor, and ONE more frame pump routes the reply —
   so plain members are still fine (every hop lands on the caller's thread),
   but a store round trip is now TWO pump lines after frame_dispatch. */
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

/* The audit-trail surface (test_loop.cpp's helpers, verbatim): load
   frame_debug_events and search it. */
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

  /* FRM_REMEMBER{corr=77} → the behavior composes the ctx write (the
     INHERITABLE layer: a cell's remember() is a durable shared-by-default
     write) and posts it to the store actor; the store reply is corr-matched
     by the FRAME's router (the bridge answer is no longer synchronous with
     the dispatch — the store actor is the reply source now). */
  message_t req = bridge_request(FRM_REMEMBER, 77, "mode", "\"fast\"");
  frame_dispatch(f, &req);
  EXPECT_EQ(req.payload, nullptr) << "behavior consumed the payload";
  EXPECT_EQ(completion.corrs.size(), 0u)
      << "no synchronous bridge answer anymore — the store reply routes it";
  wave_db_pump(db);                       /* the store actor commits the batch */
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);   /* the reply router answers */

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

  /* FRM_RECALL{corr=78} → the walk is a store message now; the router
     replies with the resolved JSON text. */
  completion.corrs.clear();
  completion.statuses.clear();
  completion.texts.clear();
  message_t req2 = bridge_request(FRM_RECALL, 78, "mode", NULL);
  frame_dispatch(f, &req2);
  wave_db_pump(db);                       /* the store actor runs the walk */
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);   /* the reply router answers */
  ASSERT_EQ(completion.corrs.size(), 1u);
  EXPECT_EQ(completion.corrs[0], 78u);
  EXPECT_EQ(completion.statuses[0], 0u);
  EXPECT_STREQ(completion.texts[0].c_str(), "\"fast\"");

  /* A recall of a key nobody remembers answers status 1 corr-matched — the
     python waiter is never stranded silently. */
  message_t req3 = bridge_request(FRM_RECALL, 79, "absent", NULL);
  frame_dispatch(f, &req3);
  wave_db_pump(db);
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);
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
     text — the bridge registry reads status, not text, for a failure; the
     refusal rides the store's reply through the router). */
  message_t req2 = bridge_request(FRM_RECALL, 91, "alsomissing", NULL);
  frame_dispatch(f, &req2);
  wave_db_pump(db);
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);
  ASSERT_EQ(completion.corrs.size(), 2u);
  EXPECT_EQ(completion.corrs[1], 91u);
  EXPECT_EQ(completion.statuses[1], 1u);
  EXPECT_EQ(completion.texts[1], "");

  bridge_completion_mount(NULL);
  frame_destroy(f);
  wave_db_close(db);
}

/* --- Task 2: the store actor (WaveDB behind ONE mailbox — no locks) -------- */

TEST(TestStore, TestStoreActorBatchIsAtomicAndAnswered) {
  /* The store actor inline: a batch posted to ITS mailbox is executed as ONE
     root transaction, and its corr-matched reply lands in the requester's
     mailbox. The test plays the requester by posting a batch whose reply_to
     is a frame actor it drains by hand. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  actor_t* store = wave_db_store_actor(db);
  ASSERT_NE(store, nullptr);

  frame_config_t cfg = test_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);   /* inline frame = the reply target */
  ASSERT_NE(f, nullptr);

  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(*bp));
  bp->ops = (frm_store_op_t*)get_clear_memory(2 * sizeof(frm_store_op_t));
  bp->nops = 2;
  /* The composed FULL root-database paths (the store batch's contract) ride
     the reply target's own subtree. */
  bp->ops[0].key =
      strdup((std::string(frame_sid(f)) + "/state/local/k1").c_str());
  bp->ops[0].value = (uint8_t*)strdup("\"one\"");
  bp->ops[0].value_len = strlen("\"one\"");
  bp->ops[1].key =
      strdup((std::string(frame_sid(f)) + "/state/ctx/k2").c_str());
  bp->ops[1].value = (uint8_t*)strdup("\"two\"");
  bp->ops[1].value_len = strlen("\"two\"");
  bp->op_name = "store-test";
  bp->reply_to = _frame_actor(f);
  bp->corr = 4242;
  message_t m;
  m.type = (uint32_t)FRM_STORE_BATCH;
  m.payload = bp;
  m.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(store, &m));

  /* Nothing is committed outside the store actor's runs — and from the
     caller thread there IS no observation before a run: an inline sync store
     call (frame_recall) posts and PUMPS, and its pump cycle runs the whole
     queued store actor (the batch rides FIFO ahead of the probe's own
     recall). So the contract is asserted as the two-pump routing: the
     explicit pump commits the batch, the frame's actor routes the corr
     4242 reply (dropped loud — the test plays an external requester), and
     the recalls below observe exactly the committed state. */
  EXPECT_EQ(wave_db_pump(db), 0);          /* the store actor runs the batch */
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);   /* the reply dispatches on the frame actor */

  /* The batch committed atomically (both keys or neither), and the frame's
     OWN log is untouched by the unrelated store batch. */
  char* v = frame_recall(f, "k1");
  ASSERT_NE(v, nullptr);
  EXPECT_STREQ(v, "\"one\"");
  free(v);
  v = frame_recall(f, "k2");
  ASSERT_NE(v, nullptr);
  EXPECT_STREQ(v, "\"two\"");
  free(v);
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(json_size(events), 0u);
  json_value_destroy(events);

  /* A store-level rejection: an oversized op posted straight to the store
     actor is refused loud and commits NOTHING (fail-loud stays the store's
     contract; the composers' cap checks are mirrored here). */
  frm_store_batch_payload_t* bad =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(*bad));
  bad->ops = (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
  bad->nops = 1;
  bad->ops[0].key =
      strdup((std::string(frame_sid(f)) + "/state/local/big").c_str());
  std::string blob(200 * 1024, 'x');       /* > SA_FRAME_MAX_BATCH_BYTES */
  bad->ops[0].value = (uint8_t*)strdup(blob.c_str());
  bad->ops[0].value_len = blob.size();
  bad->op_name = "store-test-oversized";
  bad->reply_to = _frame_actor(f);
  bad->corr = 4243;
  message_t bad_msg;
  bad_msg.type = (uint32_t)FRM_STORE_BATCH;
  bad_msg.payload = bad;
  bad_msg.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(store, &bad_msg));
  wave_db_pump(db);
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);   /* the refusal reply routes (dropped loud
                                                   — an unmatched corr — but observable via
                                                   the recall below) */
  char* big_after = frame_recall(f, "big");
  EXPECT_EQ(big_after, nullptr) << "the oversized batch committed nothing";
  free(big_after);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestStore, TestPooledStorePumpRefusesLoud) {
  /* The dual-driver rule's other half: a POOLED store's pacing belongs to its
     workers; wave_db_pump must refuse loud, not steal a mailbox run. */
  frame_config_t cfg = test_config();
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  cfg.pool = pool;
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  EXPECT_NE(wave_db_store_actor(db), nullptr);
  EXPECT_NE(wave_db_pump(db), 0) << "pooled store: the pump refuses loud, it never pumps";
  wave_db_close(db);            /* documented order: stop, close, destroy */
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(TestStore, TestStoreRecallWalkRidesTheStoreActor) {
  /* The resolve walk (own local/ -> own ctx/ -> ancestor ctx/) is now a
     store MESSAGE, not a synchronous frame-side read: a cell's actor.recall
     is answered from the FRM_STORE_REPLY router. Uses the test_frame
     bridge harness (bridge_completion_mount). */
  wave_database_root_t* db = wave_db_open(NULL);
  frame_config_t cfg = test_config();
  frame_t* parent = frame_create(db, NULL, NULL, &cfg);
  frame_t* child = frame_spawn(parent, NULL, NULL);
  frame_remember_ctx(parent, "inherited", "\"hello\"");   /* ancestor state */
  frame_remember_local(child, "local", "\"mine\"");

  bridge_completion_t completion;
  bridge_completion_mount(&completion);

  /* The cell-verb shape: FRM_RECALL dispatched at the child is now
     compose-and-post; the bridge corr answer waits for the store reply. */
  message_t req = bridge_request(FRM_RECALL, 91, "inherited", NULL);
  frame_dispatch(child, &req);      /* posts the store recall; no bridge reply YET */
  EXPECT_EQ(completion.corrs.size(), 0u)
      << "no synchronous bridge answer anymore — the store actor is the reply source";

  wave_db_pump(db);                 /* the store actor runs the walk */
  actor_run(_frame_actor(child), ACTOR_BATCH_SIZE);   /* the router answers the corr */
  ASSERT_EQ(completion.corrs.size(), 1u);
  EXPECT_EQ(completion.corrs[0], 91u);
  EXPECT_EQ(completion.statuses[0], 0u) << "the walk resolved through the store actor";
  EXPECT_EQ(completion.texts[0], "\"hello\"");
  completion.corrs.clear(); completion.statuses.clear(); completion.texts.clear();

  /* local/ shadows ctx/ within one frame (unchanged semantics, same path): */
  message_t req2 = bridge_request(FRM_RECALL, 92, "local", NULL);
  frame_dispatch(child, &req2);
  wave_db_pump(db);
  actor_run(_frame_actor(child), ACTOR_BATCH_SIZE);
  ASSERT_EQ(completion.corrs.size(), 1u);
  EXPECT_EQ(completion.corrs[0], 92u);
  EXPECT_EQ(completion.texts[0], "\"mine\"");

  bridge_completion_mount(NULL);
  frame_destroy(child);
  frame_destroy(parent);
  wave_db_close(db);
}

/* A scan-reply capture: the store actor answers a RAW FRM_STORE_SCAN at the
   requester actor directly, and the frame router's corr slots do not cover a
   raw scan probe (an unrelated corr drops loud — the router's contract), so
   the test plays its own reply target and consumes the reply payload here. */
typedef struct scan_capture_t {
  actor_t actor;
  std::vector<int64_t> rcs;
  std::vector<size_t> counts;
  std::vector<std::vector<std::string>> records_per_reply;
} scan_capture_t;

static void scan_capture_dispatch(void* state, message_t* msg) {
  scan_capture_t* cap = (scan_capture_t*)state;
  if (msg->type != (uint32_t)FRM_STORE_REPLY) return;
  frm_store_reply_payload_t* r = (frm_store_reply_payload_t*)msg->payload;
  msg->payload = NULL;   /* consumed — payload_destroy must not free it twice */
  if (r == NULL) return;
  cap->rcs.push_back(r->rc);
  cap->counts.push_back(r->n);
  std::vector<std::string> recs;
  for (size_t i = 0; i < r->n; i++)
    recs.emplace_back(r->records[i] != NULL ? r->records[i] : "");
  cap->records_per_reply.push_back(recs);
  frm_store_reply_payload_destroy(r);
}

TEST(TestStore, TestStoreScanHonorsRequestedLimit) {
  /* A scan posted at the store actor carries its own newest-record limit and
     the reply carries EXACTLY that many records — the newest window of the
     range, emitted ascending — even when the range holds more. The store
     never invents behavior beyond the declared cap (window clamp only). */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  actor_t* store = wave_db_store_actor(db);
  ASSERT_NE(store, nullptr);
  frame_config_t cfg = test_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);

  /* Six remember events land in the frame's events range (one record each);
     seqs 1..6. */
  for (int i = 1; i <= 6; i++)
    EXPECT_EQ(frame_remember_local(f, ("k" + std::to_string(i)).c_str(), "\"v\""), 0);

  scan_capture_t cap;
  actor_init(&cap.actor, &cap, scan_capture_dispatch, NULL);   /* inline: the test drains */

  frm_store_scan_payload_t* sp =
      (frm_store_scan_payload_t*)get_clear_memory(sizeof(*sp));
  sp->start = strdup((std::string(frame_sid(f)) + "/events").c_str());
  sp->end = strdup((std::string(frame_sid(f)) + "/events0").c_str());
  sp->limit = 3;
  sp->reply_to = &cap.actor;
  sp->corr = 77;
  message_t m;
  m.type = (uint32_t)FRM_STORE_SCAN;
  m.payload = sp;
  m.payload_destroy = frm_store_scan_payload_destroy;
  ASSERT_TRUE(actor_send(store, &m));
  wave_db_pump(db);                     /* the store actor runs the scan */

  actor_run(&cap.actor, ACTOR_BATCH_SIZE);   /* the test's reply target drains */
  ASSERT_EQ(cap.counts.size(), 1u) << "one corr-matched scan reply";
  ASSERT_EQ(cap.rcs[0], 0);
  ASSERT_EQ(cap.counts[0], 3u) << "the reply carries EXACTLY the requested limit";
  ASSERT_EQ(cap.records_per_reply[0].size(), 3u);

  /* Ascending: the newest three OF the six (seqs 4, 5, 6), oldest first. */
  for (size_t i = 0; i < 3; i++) {
    json_value_t* rec = json_parse(cap.records_per_reply[0][i].c_str(),
                                   cap.records_per_reply[0][i].size(), NULL);
    ASSERT_NE(rec, nullptr) << "record " << i << " is the composed event JSON";
    EXPECT_EQ(json_as_int(json_get(rec, "seq")), (int64_t)(4 + i));
    json_value_destroy(rec);
  }

  /* actor_destroy drains + frees the mailbox (the delivered reply node). */
  actor_destroy(&cap.actor);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestReportBindIsOneCrossSubtreeBatch) {
  /* The cross-frame effect is ONE atomic batch, composed at the PARENT's
     actor with the parent's PRE-ALLOCATED seq; the child only ever allocated
     its own. Observable as exact per-log seq chains after the reply. */
  wave_database_root_t* db = wave_db_open(NULL);
  frame_config_t cfg = test_config();
  frame_t* parent = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(parent, nullptr);
  frame_t* child = frame_spawn(parent, "leaf", NULL);   /* spawn = parent seq 1 */
  ASSERT_NE(child, nullptr);

  EXPECT_EQ(frame_report(child, "leaf's verdict"), 0);  /* the bind (sync shape pumps) */

  /* Parent log: frame.spawn @ seq 1, bound frame.report @ seq 2 (the parent
     allocated 2 at ITS actor — the child could not touch it). */
  json_value_t* events = load_events(parent);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(json_size(events), 2u);
  EXPECT_TRUE(event_is(json_at(events, 0), "frame.spawn"));
  EXPECT_EQ(json_as_int(json_get(json_at(events, 1), "seq")), 2);
  EXPECT_TRUE(event_is(json_at(events, 1), "frame.report"));
  EXPECT_STREQ(json_as_string(json_get(json_get(json_at(events, 1), "payload"), "child_sid")),
               frame_sid(child));
  json_value_destroy(events);

  /* Child log: exactly ONE frame.report @ seq 1, cause = spawn-free, status
     done. */
  json_value_t* cev = load_events(child);
  ASSERT_NE(cev, nullptr);
  ASSERT_EQ(json_size(cev), 1u);
  EXPECT_TRUE(event_is(json_at(cev, 0), "frame.report"));
  EXPECT_EQ(json_as_int(json_get(json_at(cev, 0), "seq")), 1);
  json_value_destroy(cev);
  EXPECT_EQ(frame_is_done(child), 1);

  /* Refusal = NOTHING half-applies in EITHER log: an oversized report text is
     refused at compose; both logs keep their exact prior state. */
  std::string big(200 * 1024, 'x');        /* > SA_FRAME_MAX_BATCH_BYTES */
  EXPECT_NE(frame_report(child, big.c_str()), 0) << "refused loud, never truncated";
  json_value_t* p2 = load_events(parent);
  ASSERT_NE(p2, nullptr);
  ASSERT_EQ(json_size(p2), 2u) << "the parent's log did not half-apply";
  json_value_destroy(p2);
  json_value_t* c2 = load_events(child);
  ASSERT_NE(c2, nullptr);
  ASSERT_EQ(json_size(c2), 1u) << "the child's log did not half-apply";
  json_value_destroy(c2);

  frame_destroy(child);
  frame_destroy(parent);
  wave_db_close(db);
}

#endif /* SA_HAS_WDB */