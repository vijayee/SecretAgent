//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>
extern "C" {
#include "../src/Frame/frame.h"
#include "../src/Frame/persona_records.h"
#include "../src/Frame/frame_messages.h"
#include "../src/Frame/frame_bridge.h"
#include "../src/Frame/frame_internal.h"
#include "../src/Frame/loop.h"
#include "../src/Frame/model.h"
#include "../src/Platform/platform_time.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/json.h"
#include "../src/Util/allocator.h"
#include "../src/Util/log.h"
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

/* The steer test's pre-refusal gate: 1 when the frame's log is empty. */
static int debug_event_count_is_zero(frame_t* f) {
  size_t n = 0;
  debug_event_count(f, &n);
  return n == 0;
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

TEST(TestFrame, TestSteerComposesMsgAppendFromOutside) {
  /* The posted steer (client-api spec §3): _frame_steer_post is the
     thread-legal shape of frame_append_msg for a caller NOT on the frame's
     thread — ONE FRM_STEER into the frame's own mailbox, whose dispatch
     composes the durable msg.append fire-and-post (the sync append would
     refuse loud on a POOLED store; this is the handler's route). The steer
     commits; the pre-post validation refusals commit nothing. */
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);

  EXPECT_EQ(_frame_steer_post(f, NULL, "roleless"), -1)
      << "validation refuses pre-post: no role, no post";
  EXPECT_EQ(_frame_steer_post(NULL, "user", "frameless"), -1);
  EXPECT_EQ(debug_event_count_is_zero(f), 1)
      << "the refusals committed nothing";

  EXPECT_EQ(_frame_steer_post(f, "user", "hello from the wire"), 0);
  _frame_pump(f);   /* the FRM_STEER compose + the store batch's round trip */

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(json_size(events), 1u) << "exactly ONE msg.append committed";
  json_value_t* rec = json_at(events, 0);
  ASSERT_TRUE(event_is(rec, "msg.append"));
  json_value_t* payload = json_get(rec, "payload");
  ASSERT_NE(payload, nullptr);
  EXPECT_STREQ(json_as_string(json_get(payload, "role")), "user");
  EXPECT_STREQ(json_as_string(json_get(payload, "content")),
               "hello from the wire");
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

/* The parked ask's DISPLACED shapes (the framing gate): an ask at an
   engine-less frame (engine_live 0 / no open turn) has no turn close to ride
   — the dispatch refuses LOUD (never silence, never a stranded park) and
   consumes the payload (the case retires the shell through its own
   destroyer; nothing parks). */
static int _ask_drop_lines = 0;
static void _ask_drop_recorder(log_Event* ev) {
  va_list ap;
  va_copy(ap, ev->ap);
  char line[512];
  vsnprintf(line, sizeof(line), ev->fmt, ap);
  va_end(ap);
  if (strstr(line, "no live OPEN turn to close in") != NULL) {
    _ask_drop_lines++;
  }
}

TEST(TestFrame, TestFrmAskAtAnEnginelessFrameRefusesLoud) {
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  log_add_callback(_ask_drop_recorder, NULL, LOG_ERROR);

  frm_ask_payload_t* ap =
      (frm_ask_payload_t*)get_clear_memory(sizeof(*ap));
  ap->corr = 4242;
  ap->question = strdup("which db?");
  char** options = (char**)get_clear_memory(sizeof(char*));
  options[0] = strdup("a");
  ap->options = options;
  ap->noptions = 1;
  message_t m;
  m.type = (uint32_t)FRM_ASK;
  m.payload = ap;
  m.payload_destroy = frm_ask_payload_destroy;

  int lines_before = _ask_drop_lines;
  ASSERT_TRUE(actor_send(_frame_actor(f), &m));
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);
  EXPECT_GT(_ask_drop_lines, lines_before)
      << "the dispatch logged the engine-less ask's loud refusal";

  /* Nothing parked: the log holds ZERO ask records (the refusal consumed
     the payload; no ask state ever boxed). */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(json_size(events), 0u) << "an engine-less ask commits nothing";
  json_value_destroy(events);

  /* The frame survived the refusal: its mailbox still routes — a second ask
     refuses loud again (the frame stays startable, no park ever stood). */
  frm_ask_payload_t* ap2 =
      (frm_ask_payload_t*)get_clear_memory(sizeof(*ap2));
  ap2->corr = 4243;
  ap2->question = strdup("which engine?");
  message_t m2;
  m2.type = (uint32_t)FRM_ASK;
  m2.payload = ap2;
  m2.payload_destroy = frm_ask_payload_destroy;
  int lines_after_first = _ask_drop_lines;
  ASSERT_TRUE(actor_send(_frame_actor(f), &m2));
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);
  EXPECT_GT(_ask_drop_lines, lines_after_first)
      << "the frame's mailbox still routes after a loud refusal";

  frame_destroy(f);
  wave_db_close(db);
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

TEST(TestFrame, TestPersonaNameCarriedAndDefaulted) {
  /* The config field's copy contract (persona spec §3): a persona-carrying
     cfg dups the name INTO the frame; a persona-less cfg carries none —
     every persona-less frame stays byte-identical to today (Task 4's derive
     pins hold off this). The accessor is the test-visible seam (the
     _frame_goal precedent): the persona block's REAL observable — the
     composed first block in the captured prompt — is Task 4's test. */
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* plain = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(_frame_persona_name(plain), nullptr)
      << "a persona-less config carries none";
  frame_destroy(plain);

  cfg.persona_name = "hammer";
  frame_t* named = frame_create(db, NULL, "hammer session", &cfg);
  ASSERT_NE(named, nullptr);
  EXPECT_STREQ(_frame_persona_name(named), "hammer")
      << "frame_create dups the config's persona name";
  frame_destroy(named);
  wave_db_close(db);
}

TEST(TestFrame, TestSpawnInheritsPersonaName) {
  /* The spawn inheritance (persona spec §3): a spawned child composes its
     config from the parent — the persona name rides it (an owned dup, the
     same rule the model strings follow); a persona-less parent's child
     carries none. */
  frame_config_t cfg = test_config();
  cfg.persona_name = "hammer";
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* parent = frame_create(db, NULL, "parent goal", &cfg);
  ASSERT_NE(parent, nullptr);
  frame_t* child = frame_spawn(parent, "leaf goal", NULL);
  ASSERT_NE(child, nullptr);
  EXPECT_STREQ(_frame_persona_name(child), "hammer")
      << "the spawned child inherits the parent's persona name";
  frame_destroy(child);
  frame_destroy(parent);

  /* The persona-less lineage: the child carries none. */
  frame_config_t plain_cfg = test_config();
  frame_t* bare = frame_create(db, NULL, "bare parent", &plain_cfg);
  ASSERT_NE(bare, nullptr);
  frame_t* bare_child = frame_spawn(bare, "leaf goal", NULL);
  ASSERT_NE(bare_child, nullptr);
  EXPECT_EQ(_frame_persona_name(bare_child), nullptr)
      << "no persona in, no persona inherited";
  frame_destroy(bare_child);
  frame_destroy(bare);
  wave_db_close(db);
}

TEST(TestFrame, TestResumeCarriesPersonaName) {
  /* The resume copy site (persona spec §3): the post-restart config's
     persona name dups into the resumed frame; a persona-less restart cfg
     carries none — the persona rides the CONFIG, not the store. The
     done-handle resume (the read-only shape: no repair pass, no sync
     writes) keeps it in-memory. */
  frame_config_t cfg = test_config();
  cfg.persona_name = "hammer";
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "restart me", &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  ASSERT_EQ(_frame_set_status_done(f), 0);
  frame_destroy(f);

  frame_t* resumed = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(resumed, nullptr);
  EXPECT_STREQ(_frame_persona_name(resumed), "hammer")
      << "the restart cfg's persona name rides the resume";
  frame_destroy(resumed);

  frame_config_t plain_cfg = test_config();
  frame_t* bare = frame_resume(db, sid.c_str(), &plain_cfg);
  ASSERT_NE(bare, nullptr);
  EXPECT_EQ(_frame_persona_name(bare), nullptr)
      << "a persona-less restart cfg carries none";
  frame_destroy(bare);
  wave_db_close(db);
}

TEST(TestFrame, TestEscalationModeInherits) {
  /* The ladder's config-copy contract (escalation spec §2): the mode is
     a plain VALUE copy (an enum — the max_depth line's pattern) at all
     three config-copy sites — the pooled child-create inheritance, the
     frame_spawn branch, and the resume copy — and a cfg-less create
     carries FREE (0). A value outside the ladder refuses loud at
     create, never clamped to free. The mode's engine behavior (plan
     turns, the gate, bypass) is Task 5's tests; THIS pins the config. */

  /* A POOLED parent's child carries the mode (frame_spawn refuses on a
     pooled store, so the child rides frame_create's parent link — the
     TestPoolAttachAndInheritance shape; the NULL cfg forces the
     parent-heritage branch). */
  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_PLAN_ASK_ACT;
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  frame_config_t pooled_cfg = cfg;
  pooled_cfg.pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_t* parent = frame_create(db, NULL, "pooled ladder root", &pooled_cfg);
  ASSERT_NE(parent, nullptr);
  EXPECT_EQ(_frame_escalation_mode(parent), FRAME_ESCALATION_PLAN_ASK_ACT);
  frame_t* child = frame_create(db, parent, "pooled ladder leaf", NULL);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(_frame_escalation_mode(child), FRAME_ESCALATION_PLAN_ASK_ACT)
      << "the pooled child-create inheritance carries the ladder";
  frame_destroy(child);
  frame_destroy(parent);
  scheduler_pool_stop(pool);
  wave_db_close(db);
  scheduler_pool_destroy(pool);

  /* The frame_spawn branch: a spawned child inherits the parent's mode. */
  wave_database_root_t* inline_db = wave_db_open(NULL);
  ASSERT_NE(inline_db, nullptr);
  frame_t* spawn_parent = frame_create(inline_db, NULL, "spawn root", &cfg);
  ASSERT_NE(spawn_parent, nullptr);
  frame_t* spawned = frame_spawn(spawn_parent, "spawned leaf", NULL);
  ASSERT_NE(spawned, nullptr);
  EXPECT_EQ(_frame_escalation_mode(spawned), FRAME_ESCALATION_PLAN_ASK_ACT)
      << "frame_spawn's heritage branch carries the ladder";
  frame_destroy(spawned);
  frame_destroy(spawn_parent);

  /* The resume copy site: the restart cfg's mode rides the resume. */
  frame_t* resumed_src = frame_create(inline_db, NULL, "resume me", &cfg);
  ASSERT_NE(resumed_src, nullptr);
  std::string sid = frame_sid(resumed_src);
  ASSERT_EQ(_frame_set_status_done(resumed_src), 0);
  frame_destroy(resumed_src);
  frame_t* resumed = frame_resume(inline_db, sid.c_str(), &cfg);
  ASSERT_NE(resumed, nullptr);
  EXPECT_EQ(_frame_escalation_mode(resumed), FRAME_ESCALATION_PLAN_ASK_ACT)
      << "the restart cfg's mode rides the resume";
  frame_destroy(resumed);

  /* THE OTHER LADDER VALUE rides the SAME value-copy lines (BYPASS = 2):
     the parent-heritage branch (one copy line — the pooled child-create
     and the frame_spawn both land there) and the resume's cfg copy. The
     explicit-cfg create asserts above are literally the same assignment. */
  frame_config_t bypass_cfg = test_config();
  bypass_cfg.escalation_mode = FRAME_ESCALATION_BYPASS;
  frame_t* bypass_root = frame_create(inline_db, NULL, "bypass spawn root",
                                      &bypass_cfg);
  ASSERT_NE(bypass_root, nullptr);
  frame_t* bypassed = frame_spawn(bypass_root, "bypass spawned leaf", NULL);
  ASSERT_NE(bypassed, nullptr);
  EXPECT_EQ(_frame_escalation_mode(bypassed), FRAME_ESCALATION_BYPASS)
      << "BYPASS rides the parent-heritage copy line like PLAN_ASK_ACT";
  frame_destroy(bypassed);
  frame_destroy(bypass_root);   /* the spawn probe's own teardown (the
                                   pristine spawn section's shape) */
  frame_t* bypass_src = frame_create(inline_db, NULL, "bypass resume me",
                                     &bypass_cfg);
  ASSERT_NE(bypass_src, nullptr);
  std::string bypass_sid = frame_sid(bypass_src);
  ASSERT_EQ(_frame_set_status_done(bypass_src), 0);
  frame_destroy(bypass_src);
  frame_t* bypass_resumed = frame_resume(inline_db, bypass_sid.c_str(),
                                         &bypass_cfg);
  ASSERT_NE(bypass_resumed, nullptr);
  EXPECT_EQ(_frame_escalation_mode(bypass_resumed), FRAME_ESCALATION_BYPASS)
      << "BYPASS rides the resume's cfg copy line like PLAN_ASK_ACT";
  frame_destroy(bypass_resumed);

  /* The cfg-less shape: zeroed = FREE — every standing frame unchanged. */
  frame_config_t plain_cfg = test_config();
  frame_t* plain = frame_create(inline_db, NULL, "free root", &plain_cfg);
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(_frame_escalation_mode(plain), FRAME_ESCALATION_FREE)
      << "a zeroed cfg carries FREE (the default)";
  frame_destroy(plain);

  /* The refusal: a value > 2 fails loud (NULL + log), never a clamp. */
  frame_config_t bad_cfg = test_config();
  bad_cfg.escalation_mode = 7;
  EXPECT_EQ(frame_create(inline_db, NULL, "out of ladder", &bad_cfg), nullptr)
      << "a mode outside the ladder refuses loud";
  frame_config_t bad_resume_cfg = test_config();
  bad_resume_cfg.escalation_mode = 7;
  EXPECT_EQ(frame_resume(inline_db, sid.c_str(), &bad_resume_cfg), nullptr)
      << "a mode outside the ladder refuses loud at resume too";

  /* The CHILD-shaped refusal (the resume fail label's teardown pin): a child
     subtree's meta/parent is restored BEFORE the escalation-mode gate, so a
     refusal there must tear parent_path down with everything else — the
     label frees it like the create site's does (free(NULL) for the
     root-shaped refusal above; LSan proves the child shape here). */
  frame_t* spawn_root2 = frame_create(inline_db, NULL, "leak ladder root", &cfg);
  ASSERT_NE(spawn_root2, nullptr);
  frame_t* child2 = frame_spawn(spawn_root2, "leak ladder leaf", NULL);
  ASSERT_NE(child2, nullptr);
  std::string child_sid = frame_sid(child2);
  ASSERT_EQ(_frame_set_status_done(child2), 0);
  frame_destroy(child2);
  frame_destroy(spawn_root2);
  EXPECT_EQ(frame_resume(inline_db, child_sid.c_str(), &bad_resume_cfg), nullptr)
      << "a child-shaped resume with a mode outside the ladder refuses loud "
         "(and its restored parent_path lands in the fail label's teardown)";
  wave_db_close(inline_db);
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

TEST(TestFrame, TestJoinResetsARecallLeftoverSlot) {
  /* A successful direct recall routes the reply's records WHOLE into the
     parent's sync slot — the recall transfers out only records[0]'s copy as
     its text. The later frame_join reuses that same slot for its own batch,
     so it must reset it WHOLE (the leftover records die there, not just the
     text): under the ASan config a text-only reset fails loud with the leaked
     records array. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = test_config();
  frame_t* parent = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(parent, nullptr);
  frame_t* child = frame_spawn(parent, "leaf", NULL);
  ASSERT_NE(child, nullptr);

  EXPECT_EQ(frame_remember_ctx(parent, "mode", "\"sync\""), 0);
  char* v = frame_recall(parent, "mode");   /* the reply's records ride the slot */
  ASSERT_NE(v, nullptr);
  EXPECT_STREQ(v, "\"sync\"");
  free(v);

  EXPECT_EQ(frame_report(child, "found it"), 0);
  EXPECT_EQ(frame_join(child), 0) << "the join survives the leftover slot records";

  frame_destroy(child);
  frame_destroy(parent);
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

TEST(TestStore, TestPersonaInstallWritesTheHammerRecords) {
  /* persona_records_install (spec §1): the boot/direct batch puts
     personas/hammer/record (the shipped record's canonical JSON) and
     personas/hammer/meta ({"created": the ISO now}). The keys come back
     through the store actor's own bounded scan (absolute root-level
     bounds); the RE-INSTALL is idempotent-safe (a put overwrites — the
     installer decides): rc 0 again and the record's bytes identical. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  ASSERT_EQ(persona_records_install(db), 0);

  char* shipped = persona_records_hammer_record();
  ASSERT_NE(shipped, nullptr);

  /* Scan the personas subtree back (the store's own scan, root-level
     bounds; ascending order — meta sorts before record). */
  auto scan_once = [&](scan_capture_t* cap) {
    cap->rcs.clear();
    cap->counts.clear();
    cap->records_per_reply.clear();
    frm_store_scan_payload_t* sp =
        (frm_store_scan_payload_t*)get_clear_memory(sizeof(*sp));
    sp->start = strdup("personas/");
    sp->end = strdup("personas0");
    sp->limit = 6;
    sp->reply_to = &cap->actor;
    sp->corr = 151;
    message_t m;
    m.type = (uint32_t)FRM_STORE_SCAN;
    m.payload = sp;
    m.payload_destroy = frm_store_scan_payload_destroy;
    ASSERT_TRUE(actor_send(wave_db_store_actor(db), &m));
    wave_db_pump(db);
    actor_run(&cap->actor, ACTOR_BATCH_SIZE);
    ASSERT_EQ(cap->counts.size(), 1u) << "one corr-matched scan reply";
    ASSERT_EQ(cap->rcs[0], 0);
  };
  scan_capture_t cap;
  actor_init(&cap.actor, &cap, scan_capture_dispatch, NULL);
  scan_once(&cap);
  ASSERT_EQ(cap.counts[0], 2u) << "meta + record, exactly";
  const std::string meta_text = cap.records_per_reply[0][0];
  const std::string record_text = cap.records_per_reply[0][1];

  /* The stored record's bytes ARE the shipped record's canonical bytes. */
  ASSERT_EQ(record_text, std::string(shipped));
  json_value_t* rec = json_parse(record_text.c_str(), record_text.size(), NULL);
  ASSERT_NE(rec, nullptr) << "the installed record is a JSON document";
  EXPECT_EQ(json_as_int(json_get(rec, "version")), 1);
  EXPECT_STREQ(json_as_string(json_get(rec, "name")), "hammer");
  EXPECT_STREQ(json_as_string(json_get(rec, "placement")), "first");
  json_value_destroy(rec);

  /* The meta record: {"created": <the ISO stamp>} — the one stamp shape
     (20 chars, digits with T/Z separators). */
  json_value_t* meta = json_parse(meta_text.c_str(), meta_text.size(), NULL);
  ASSERT_NE(meta, nullptr) << "the installed meta is a JSON document";
  const char* created = json_as_string(json_get(meta, "created"));
  ASSERT_NE(created, nullptr);
  ASSERT_EQ(strlen(created), 20u);
  EXPECT_EQ(created[4], '-');
  EXPECT_EQ(created[10], 'T');
  EXPECT_EQ(created[19], 'Z');
  json_value_destroy(meta);

  /* Re-install: idempotent-safe — rc 0, the same record bytes back, and
     the meta still parses a proper created stamp (a put overwrites: the
     installer's idempotency, re-stamping nothing but meta/created). */
  ASSERT_EQ(persona_records_install(db), 0);
  scan_once(&cap);
  ASSERT_EQ(cap.counts[0], 2u);
  EXPECT_EQ(cap.records_per_reply[0][1], record_text)
      << "the re-installed record is byte-identical (a put overwrites)";
  json_value_t* remeta = json_parse(cap.records_per_reply[0][0].c_str(),
                                    cap.records_per_reply[0][0].size(), NULL);
  ASSERT_NE(remeta, nullptr);
  const char* recreated = json_as_string(json_get(remeta, "created"));
  ASSERT_NE(recreated, nullptr);
  ASSERT_EQ(strlen(recreated), 20u);
  EXPECT_EQ(recreated[10], 'T');
  json_value_destroy(remeta);

  actor_destroy(&cap.actor);
  free(shipped);
  wave_db_close(db);
}

TEST(TestStore, TestStoreGetNamedRepliesTheNamedValues) {
  /* FRM_STORE_GET_NAMED (the persona slice, spec §3): the named direct read
     answers ONE JSON array record — one {"key", "value"-or-null} entry per
     REQUESTED key, IN THE REQUESTED ORDER. A missing key is a first-class
     null (absence is the answer), never a refusal — no positional
     guessing, the value's key rides attached. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  ASSERT_EQ(persona_records_install(db), 0);

  char* shipped = persona_records_hammer_record();
  ASSERT_NE(shipped, nullptr);

  /* One record RIDE-ALONG put (the test's boot idiom): the user-context
     record sits beside the installed hammer records. */
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(*bp));
  bp->ops = (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
  bp->ops[0].key = strdup("personas/hammer/user-context");
  bp->ops[0].value = (uint8_t*)strdup("{\"user_name\":\"Victor\"}");
  bp->ops[0].value_len = strlen("{\"user_name\":\"Victor\"}");
  bp->ops[0].is_delete = 0;
  bp->nops = 1;
  bp->op_name = "get-named test put";
  bp->reply_to = NULL;
  bp->corr = 0;
  message_t put;
  put.type = (uint32_t)FRM_STORE_BATCH;
  put.payload = bp;
  put.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(wave_db_store_actor(db), &put));
  wave_db_pump(db);

  scan_capture_t cap;
  actor_init(&cap.actor, &cap, scan_capture_dispatch, NULL);

  /* The read: three keys, REQUESTED in this order — one present record, one
     MISSING (never installed), one present context. */
  const char* keys[3] = {"personas/hammer/record",
                         "personas/hammer/absent",
                         "personas/hammer/user-context"};
  frm_store_get_named_payload_t* gp =
      (frm_store_get_named_payload_t*)get_clear_memory(sizeof(*gp));
  gp->keys = (char**)get_clear_memory(3 * sizeof(char*));
  for (size_t i = 0; i < 3; i++) gp->keys[i] = strdup(keys[i]);
  gp->nkeys = 3;
  gp->reply_to = &cap.actor;
  gp->corr = 4242;
  message_t m;
  m.type = (uint32_t)FRM_STORE_GET_NAMED;
  m.payload = gp;
  m.payload_destroy = frm_store_get_named_payload_destroy;
  ASSERT_TRUE(actor_send(wave_db_store_actor(db), &m));
  wave_db_pump(db);
  actor_run(&cap.actor, ACTOR_BATCH_SIZE);

  ASSERT_EQ(cap.counts.size(), 1u) << "one corr-matched read reply";
  ASSERT_EQ(cap.rcs[0], 0);
  ASSERT_EQ(cap.counts[0], 1u) << "the reply is ONE array record";
  ASSERT_EQ(cap.records_per_reply[0].size(), 1u);

  json_value_t* arr = json_parse(cap.records_per_reply[0][0].c_str(),
                                 cap.records_per_reply[0][0].size(), NULL);
  ASSERT_NE(arr, nullptr) << "the reply record is the composed JSON array";
  ASSERT_EQ(json_type(arr), JSON_ARRAY);
  ASSERT_EQ(json_size(arr), 3u) << "one entry per REQUESTED key";
  for (size_t i = 0; i < 3u; i++) {
    json_value_t* entry = json_at(arr, i);
    ASSERT_NE(entry, nullptr);
    EXPECT_STREQ(json_as_string(json_get(entry, "key")), keys[i])
        << "the requested order holds, keys verbatim";
  }
  /* Present record: the stored bytes VERBATIM (unescaped back out). */
  EXPECT_STREQ(json_as_string(json_at(arr, 0) != NULL
                                  ? json_get(json_at(arr, 0), "value") : NULL),
               shipped)
      << "the record's raw bytes ride attached to their key";
  /* Missing key: "value": null — absence the answer, rc still 0. */
  json_value_t* null_value = json_get(json_at(arr, 1), "value");
  ASSERT_NE(null_value, nullptr);
  EXPECT_EQ(json_type(null_value), JSON_NULL);
  /* Present context byte-verbatim. */
  EXPECT_STREQ(json_as_string(json_get(json_at(arr, 2), "value")),
               "{\"user_name\":\"Victor\"}");
  json_value_destroy(arr);

  actor_destroy(&cap.actor);
  free(shipped);
  wave_db_close(db);
}

TEST(TestStore, TestPersonaInstallRefusesAPooledStore) {
  /* The sync install keeps the sync API's inline-store rule: at a POOLED
     store it refuses loud (rc != 0) and installs nothing. NULL also
     refuses loud. */
  EXPECT_EQ(persona_records_install(nullptr), -1);

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
  EXPECT_EQ(persona_records_install(db), -1)
      << "pooled store: the boot install refuses loud";
  wave_db_close(db);            /* documented order: stop, close, destroy */
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}

TEST(TestFrame, TestStoreBatchCarriesDeleteOpsAtomic) {
  /* The delete op (frm_store_op_t.is_delete = WaveDB raw_op_t.type 1) rides
     the store batch: a batch of [put entry, delete entry] in ONE round trip
     leaves the put ABSENT, and a batch that REFUSES — here a delete op
     carrying a value (a deletion never smuggles bytes) — commits NOTHING of
     its put sibling. The reply capture reuses the scan-capture responder
     (the test plays its own reply target and consumes the payloads). */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  actor_t* store = wave_db_store_actor(db);
  ASSERT_NE(store, nullptr);
  frame_config_t cfg = test_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  scan_capture_t cap;
  actor_init(&cap.actor, &cap, scan_capture_dispatch, NULL);

  /* Committed: put + delete of the SAME key in ONE batch — the end state is
     the DELETE's (the entry absent), the put never outlives its batch. */
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(*bp));
  bp->ops = (frm_store_op_t*)get_clear_memory(2 * sizeof(frm_store_op_t));
  bp->nops = 2;
  bp->ops[0].key = strdup((sid + "/state/local/dual").c_str());
  bp->ops[0].value = (uint8_t*)strdup("\"put first\"");
  bp->ops[0].value_len = strlen("\"put first\"");
  bp->ops[1].key = strdup((sid + "/state/local/dual").c_str());   /* OWNED twice */
  bp->ops[1].is_delete = 1;      /* value NULL, value_len 0 */
  bp->op_name = "del-test-atomic";
  bp->reply_to = &cap.actor;
  bp->corr = 4244;
  message_t m;
  m.type = (uint32_t)FRM_STORE_BATCH;
  m.payload = bp;
  m.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(store, &m));
  wave_db_pump(db);                          /* the store actor runs the batch */
  actor_run(&cap.actor, ACTOR_BATCH_SIZE);   /* the reply lands corr-matched */
  ASSERT_EQ(cap.rcs.size(), 1u);
  EXPECT_EQ(cap.rcs[0], 0) << "put + delete of the same key is one batch";

  char* absent = frame_recall(f, "dual");
  EXPECT_EQ(absent, nullptr) << "the delete won: the put is ABSENT in the end state";
  free(absent);

  /* Refused: a delete op WITH a value refuses loud (rc != 0) and the whole
     atomic batch with it — the sibling put commits NOTHING. */
  frm_store_batch_payload_t* smuggler =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(*smuggler));
  smuggler->ops = (frm_store_op_t*)get_clear_memory(2 * sizeof(frm_store_op_t));
  smuggler->nops = 2;
  smuggler->ops[0].key = strdup((sid + "/state/local/pair").c_str());
  smuggler->ops[0].value = (uint8_t*)strdup("\"should never appear\"");
  smuggler->ops[0].value_len = strlen("\"should never appear\"");
  smuggler->ops[1].key = strdup((sid + "/state/local/pair2").c_str());
  smuggler->ops[1].value = (uint8_t*)strdup("smuggled");
  smuggler->ops[1].value_len = strlen("smuggled");
  smuggler->ops[1].is_delete = 1;
  smuggler->op_name = "del-test-smuggler";
  smuggler->reply_to = &cap.actor;
  smuggler->corr = 4245;
  message_t m2;
  m2.type = (uint32_t)FRM_STORE_BATCH;
  m2.payload = smuggler;
  m2.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(store, &m2));
  wave_db_pump(db);
  actor_run(&cap.actor, ACTOR_BATCH_SIZE);
  ASSERT_EQ(cap.rcs.size(), 2u);
  EXPECT_NE(cap.rcs[1], 0) << "a DELETE carrying a value refuses loud";

  char* ghost = frame_recall(f, "pair");
  EXPECT_EQ(ghost, nullptr) << "the refusing batch committed NOTHING — not even its put";
  free(ghost);

  actor_destroy(&cap.actor);   /* drains + frees the delivered reply nodes */
  frame_destroy(f);
  wave_db_close(db);
}

/* The sessions-listing reply capture: the store actor answers a RAW
   FRM_STORE_LIST_SESSIONS at the requester actor directly (a test double —
   the production reply target is Task 4's ca_session_server actor), and the
   frame router's corr slots do not cover a raw listing probe (an unrelated
   corr drops loud — the router's contract). Same shape as scan_capture_t. */
typedef struct sessions_capture_t {
  actor_t actor;
  std::vector<int64_t> rcs;
  std::vector<size_t> counts;
  std::vector<std::vector<std::string>> records_per_reply;
} sessions_capture_t;

static void sessions_capture_dispatch(void* state, message_t* msg) {
  sessions_capture_t* cap = (sessions_capture_t*)state;
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

/* The notice capture: the watcher actor double — one FRM_STORE_NOTICE per
   committed matching event record, consumed here on the test's pump. */
typedef struct notice_capture_t {
  actor_t actor;
  std::vector<std::string> sids;
  std::vector<uint64_t> seqs;
  std::vector<std::string> records;
} notice_capture_t;

static void notice_capture_dispatch(void* state, message_t* msg) {
  notice_capture_t* cap = (notice_capture_t*)state;
  if (msg->type != (uint32_t)FRM_STORE_NOTICE) return;
  frm_store_notice_t* np = (frm_store_notice_t*)msg->payload;
  msg->payload = NULL;
  if (np == NULL) return;
  cap->sids.emplace_back(np->sid_path != NULL ? np->sid_path : "");
  cap->seqs.push_back(np->seq);
  cap->records.emplace_back(np->record_json != NULL ? np->record_json : "");
  frm_store_notice_destroy(np);
}

TEST(TestStore, TestStoreListsSessionsAndTheirMeta) {
  /* The client-API sessions listing (Task 3; spec §3): TWO TOP frames (one
     left running; one marked done; a spawned CHILD's subtree nests at
     "sessions/<parent>/frames/<hex>" — never a first-level entry) — the
     listing enumerates the root's sessions/ first-level entries and answers
     ONE heap JSON row per session with the meta the store actually holds:
     status, created, depth. GOAL: no composer writes a meta/goal key today
     (frame_create's batch carries created/status/depth[/parent] only) — the
     row's "goal" is the empty sentinel (the wire's ""), the pinned
     what-exists shape. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  actor_t* store = wave_db_store_actor(db);
  ASSERT_NE(store, nullptr);
  frame_config_t cfg = test_config();
  frame_t* running = frame_create(db, NULL, "top goal", &cfg);
  frame_t* done = frame_create(db, NULL, "second goal", &cfg);
  ASSERT_NE(running, nullptr);
  ASSERT_NE(done, nullptr);
  EXPECT_EQ(_frame_set_status_done(done), 0) << "the second frame ends done";

  sessions_capture_t cap;
  actor_init(&cap.actor, &cap, sessions_capture_dispatch, NULL);
  frm_store_sessions_payload_t* lp =
      (frm_store_sessions_payload_t*)get_clear_memory(sizeof(*lp));
  lp->corr = 5511;
  lp->reply_to = &cap.actor;
  message_t m;
  m.type = (uint32_t)FRM_STORE_LIST_SESSIONS;
  m.payload = lp;
  m.payload_destroy = frm_store_sessions_payload_destroy;
  ASSERT_TRUE(actor_send(store, &m));
  wave_db_pump(db);                     /* the store actor walks + answers */
  actor_run(&cap.actor, ACTOR_BATCH_SIZE);

  ASSERT_EQ(cap.rcs.size(), 1u);
  ASSERT_EQ(cap.rcs[0], 0);
  ASSERT_EQ(cap.counts[0], 2u) << "two first-level sessions/ entries";

  /* Find + read each row (the handlers parse the same rows in Task 4). */
  int saw_running = 0, saw_done = 0;
  for (size_t i = 0; i < cap.counts[0]; i++) {
    const std::string& row = cap.records_per_reply[0][i];
    json_value_t* j = json_parse(row.c_str(), row.size(), NULL);
    ASSERT_NE(j, nullptr) << "row " << i << " composes as JSON: " << row;
    const char* sid = json_as_string(json_get(j, "sid"));
    ASSERT_NE(sid, nullptr);
    int64_t depth = json_as_int(json_get(j, "depth"));
    std::string status = json_as_string(json_get(j, "status"));
    std::string created = json_as_string(json_get(j, "created"));
    std::string goal = json_as_string(json_get(j, "goal"));
    if (std::string(sid) == frame_sid(running)) {
      EXPECT_EQ(status, "running") << "the frame stamps running at birth";
      saw_running = 1;
    } else if (std::string(sid) == frame_sid(done)) {
      EXPECT_EQ(status, "done") << "the frame was marked done";
      saw_done = 1;
    } else {
      ADD_FAILURE() << "an unknown session row: " << sid;
    }
    EXPECT_EQ(depth, 0) << "a top frame's depth";
    EXPECT_FALSE(created.empty()) << "every born session carries meta/created";
    EXPECT_EQ(goal, "") << "pinned: no composer writes a meta/goal key today";
    json_value_destroy(j);
  }
  EXPECT_EQ(saw_running, 1);
  EXPECT_EQ(saw_done, 1);

  actor_destroy(&cap.actor);
  frame_destroy(done);
  frame_destroy(running);
  wave_db_close(db);
}

TEST(TestStore, TestStoreNotifyWatchesASubtree) {
  /* The events' subscription fan-out (Task 3; spec §3): a watcher on
     sessions/<f1> sees ONLY f1's committed EVENT records — another sid's
     batch and a meta write under the watched sid stay silent; unwatch =
     the subscription's only removal (then silent). */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  actor_t* store = wave_db_store_actor(db);
  ASSERT_NE(store, nullptr);
  frame_config_t cfg = test_config();
  frame_t* f1 = frame_create(db, NULL, NULL, &cfg);
  frame_t* f2 = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f1, nullptr);
  ASSERT_NE(f2, nullptr);
  std::string sid1 = frame_sid(f1);

  notice_capture_t cap;
  actor_init(&cap.actor, &cap, notice_capture_dispatch, NULL);
  frm_store_watch_t* wp = (frm_store_watch_t*)get_clear_memory(sizeof(*wp));
  wp->sid_path = strdup(sid1.c_str());
  wp->watcher = &cap.actor;
  message_t m;
  m.type = (uint32_t)FRM_STORE_WATCH;
  m.payload = wp;
  m.payload_destroy = frm_store_watch_destroy;
  ASSERT_TRUE(actor_send(store, &m));
  wave_db_pump(db);                     /* the subscription registers */

  /* A batch under ANOTHER sid: no notice. */
  EXPECT_EQ(frame_remember_local(f2, "other", "\"v\""), 0);
  EXPECT_EQ(cap.seqs.size(), 0u) << "other sids' events stay silent";

  /* A NOT-events write under the watched sid (the meta/status put): no
     notice — the fan-out filters committed event records. */
  EXPECT_EQ(_frame_set_status_done(f1), 0);
  EXPECT_EQ(cap.seqs.size(), 0u) << "meta writes stay silent";

  /* A committed EVENT record under the watched sid: ONE notice with the
     record's {sid, seq} and its exact committed text. */
  EXPECT_EQ(frame_remember_local(f1, "wkey", "\"v\""), 0);
  actor_run(&cap.actor, ACTOR_BATCH_SIZE);
  ASSERT_EQ(cap.seqs.size(), 1u);
  EXPECT_EQ(cap.sids[0], sid1);
  json_value_t* rec = json_parse(cap.records[0].c_str(), cap.records[0].size(), NULL);
  ASSERT_NE(rec, nullptr) << "the notice carries the committed record JSON";
  EXPECT_EQ(json_as_int(json_get(rec, "seq")), (int64_t)cap.seqs[0])
      << "the notice's seq matches its record's seq";
  EXPECT_STREQ(json_as_string(json_get(rec, "type")), "state.remember");
  json_value_destroy(rec);
  EXPECT_GT(cap.seqs[0], 0u);
  cap.sids.clear(); cap.seqs.clear(); cap.records.clear();

  /* Unwatch: the subscription's ONLY removal. */
  frm_store_watch_t* up = (frm_store_watch_t*)get_clear_memory(sizeof(*up));
  up->sid_path = strdup(sid1.c_str());
  up->watcher = &cap.actor;
  message_t m2;
  m2.type = (uint32_t)FRM_STORE_UNWATCH;
  m2.payload = up;
  m2.payload_destroy = frm_store_watch_destroy;
  ASSERT_TRUE(actor_send(store, &m2));
  wave_db_pump(db);
  EXPECT_EQ(frame_remember_local(f1, "wkey2", "\"v2\""), 0);
  actor_run(&cap.actor, ACTOR_BATCH_SIZE);
  EXPECT_EQ(cap.seqs.size(), 0u) << "the unwatched watcher is silent";

  /* The DEAD-watcher shape (frame_messages.h's recorded contract): a watch
     whose watcher was destroyed WITHOUT an unwatch — its future notices
     keep posting and refuse loud at the post (the payload dies there;
     valgrind watches this), until the unwatch still removes the entry. */
  notice_capture_t dead;
  actor_init(&dead.actor, &dead, notice_capture_dispatch, NULL);
  frm_store_watch_t* dwp = (frm_store_watch_t*)get_clear_memory(sizeof(*dwp));
  dwp->sid_path = strdup(sid1.c_str());
  dwp->watcher = &dead.actor;
  message_t dm;
  dm.type = (uint32_t)FRM_STORE_WATCH;
  dm.payload = dwp;
  dm.payload_destroy = frm_store_watch_destroy;
  ASSERT_TRUE(actor_send(store, &dm));
  wave_db_pump(db);
  actor_destroy(&dead.actor);       /* dies UNWATCHED */
  EXPECT_EQ(frame_remember_local(f1, "wkey3", "\"v3\""), 0);
  /* The refused-notice post already happened inside the commit's pump. The
     teardown unwatch removes the entry for good. */
  frm_store_watch_t* dup2 = (frm_store_watch_t*)get_clear_memory(sizeof(*dup2));
  dup2->sid_path = strdup(sid1.c_str());
  dup2->watcher = &dead.actor;
  message_t dm2;
  dm2.type = (uint32_t)FRM_STORE_UNWATCH;
  dm2.payload = dup2;
  dm2.payload_destroy = frm_store_watch_destroy;
  ASSERT_TRUE(actor_send(store, &dm2));
  wave_db_pump(db);

  actor_destroy(&cap.actor);
  frame_destroy(f1);
  frame_destroy(f2);
  wave_db_close(db);
}

TEST(TestStore, TestStoreNoticeCarriesTheCommittedRecord) {
  /* The notice's record_json is the batch op's committed value bytes — the
     EXACT text, strduplicated by the fan-out before the payload destroy
     reclaims the ops. A raw batch with one events op (a synthetic seq key)
     + one state op rides ONE notice; a DUPLICATE watch (idempotent) never
     doubles it. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  actor_t* store = wave_db_store_actor(db);
  ASSERT_NE(store, nullptr);
  frame_config_t cfg = test_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);

  notice_capture_t cap;
  actor_init(&cap.actor, &cap, notice_capture_dispatch, NULL);
  for (int i = 0; i < 2; i++) {          /* the second watch is idempotent */
    frm_store_watch_t* wp =
        (frm_store_watch_t*)get_clear_memory(sizeof(*wp));
    wp->sid_path = strdup(sid.c_str());
    wp->watcher = &cap.actor;
    message_t m;
    m.type = (uint32_t)FRM_STORE_WATCH;
    m.payload = wp;
    m.payload_destroy = frm_store_watch_destroy;
    ASSERT_TRUE(actor_send(store, &m));
  }
  /* The whole-tree watcher: "sessions/" ends its own prefix boundary, so it
     sees every event record. The half-sid watcher ("sessions/<4hex>"): the
     prefix does NOT end at a segment boundary — the sid is 8 hex — so it
     stays silent. */
  notice_capture_t tree;
  actor_init(&tree.actor, &tree, notice_capture_dispatch, NULL);
  frm_store_watch_t* twp = (frm_store_watch_t*)get_clear_memory(sizeof(*twp));
  twp->sid_path = strdup("sessions/");
  twp->watcher = &tree.actor;
  message_t tm;
  tm.type = (uint32_t)FRM_STORE_WATCH;
  tm.payload = twp;
  tm.payload_destroy = frm_store_watch_destroy;
  ASSERT_TRUE(actor_send(store, &tm));
  notice_capture_t half;
  actor_init(&half.actor, &half, notice_capture_dispatch, NULL);
  frm_store_watch_t* hwp = (frm_store_watch_t*)get_clear_memory(sizeof(*hwp));
  hwp->sid_path = strdup((sid.substr(0, strlen("sessions/") + 4)).c_str());
  hwp->watcher = &half.actor;
  message_t hm;
  hm.type = (uint32_t)FRM_STORE_WATCH;
  hm.payload = hwp;
  hm.payload_destroy = frm_store_watch_destroy;
  ASSERT_TRUE(actor_send(store, &hm));
  wave_db_pump(db);

  const char record_text[] =
      "{\"seq\":21,\"type\":\"state.remember\",\"frame\":\"probe\","
      "\"corr\":null,\"at\":\"2026-10-03T00:00:00Z\",\"cause\":null,"
      "\"payload\":{\"key\":\"probe\",\"value\":\"42\"}}";
  std::string event_key = sid + "/events/00000000000000000021";
  std::string state_key = sid + "/state/local/probe";
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(*bp));
  bp->ops = (frm_store_op_t*)get_clear_memory(2 * sizeof(frm_store_op_t));
  bp->nops = 2;
  bp->ops[0].key = strdup(event_key.c_str());
  bp->ops[0].value = (uint8_t*)strdup(record_text);
  bp->ops[0].value_len = strlen(record_text);
  bp->ops[1].key = strdup(state_key.c_str());
  bp->ops[1].value = (uint8_t*)strdup("42");
  bp->ops[1].value_len = strlen("42");
  bp->op_name = "notify probe";
  message_t m;
  m.type = (uint32_t)FRM_STORE_BATCH;
  m.payload = bp;
  m.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(store, &m));
  wave_db_pump(db);
  actor_run(&cap.actor, ACTOR_BATCH_SIZE);

  ASSERT_EQ(cap.seqs.size(), 1u)
      << "one events op + an idempotent watch = ONE notice (no duplicates)";
  EXPECT_EQ(cap.seqs[0], 21u) << "the seq parses from the key's tail digits";
  EXPECT_EQ(cap.sids[0], sid);
  EXPECT_STREQ(cap.records[0].c_str(), record_text)
      << "the notice's record_json is the committed record's EXACT text";

  /* The boundary rule's both sides: the whole-tree watcher saw exactly the
     one record; the half-sid watcher saw nothing. */
  actor_run(&tree.actor, ACTOR_BATCH_SIZE);
  ASSERT_EQ(tree.seqs.size(), 1u) << "the 'sessions/' watch rides every record";
  EXPECT_STREQ(tree.records[0].c_str(), record_text);
  EXPECT_EQ(half.seqs.size(), 0u)
      << "a non-boundary prefix never matches ('sessions/<4hex>' is not a "
         "subtree)";

  /* The state op committed too — the store saw the whole batch. */
  char* recalled = frame_recall(f, "probe");
  ASSERT_NE(recalled, nullptr);
  EXPECT_STREQ(recalled, "42");
  free(recalled);

  actor_destroy(&cap.actor);
  actor_destroy(&tree.actor);
  actor_destroy(&half.actor);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestStore, TestStoreNotifySeesNestedChildEvents) {
  /* The store_notify parse's NESTED shape: a spawned CHILD frame commits
     events under its OWN subtree "sessions/<parent>/frames/<hex>/events/
     <seq>" (frame_spawn's sid_path compose; the batch is hand-composed to
     pin the PARSE). A watcher on the PARENT's sid (a multi-segment prefix
     across the frames/ boundary) and a watcher on the CHILD's own path each
     see the notice: notice sid == "sessions/<parent>/frames/<hex>", seq
     exact, the record text exact. An events key with extra segments after
     the seq, and a non-digit tail, never notify. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  actor_t* store = wave_db_store_actor(db);
  ASSERT_NE(store, nullptr);
  frame_config_t cfg = test_config();
  frame_t* parent = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(parent, nullptr);
  std::string pid = frame_sid(parent);
  /* The child subtree path compose (frame_spawn's _frame_alloc):
     "<parent full path>/frames/<8hex>". */
  std::string child = pid + "/frames/deadbeef";

  notice_capture_t cap;
  actor_init(&cap.actor, &cap, notice_capture_dispatch, NULL);
  frm_store_watch_t* wp = (frm_store_watch_t*)get_clear_memory(sizeof(*wp));
  wp->sid_path = strdup(pid.c_str());
  wp->watcher = &cap.actor;
  message_t m;
  m.type = (uint32_t)FRM_STORE_WATCH;
  m.payload = wp;
  m.payload_destroy = frm_store_watch_destroy;
  ASSERT_TRUE(actor_send(store, &m));
  notice_capture_t childcap;
  actor_init(&childcap.actor, &childcap, notice_capture_dispatch, NULL);
  frm_store_watch_t* cwp = (frm_store_watch_t*)get_clear_memory(sizeof(*cwp));
  cwp->sid_path = strdup(child.c_str());
  cwp->watcher = &childcap.actor;
  message_t cm;
  cm.type = (uint32_t)FRM_STORE_WATCH;
  cm.payload = cwp;
  cm.payload_destroy = frm_store_watch_destroy;
  ASSERT_TRUE(actor_send(store, &cm));
  wave_db_pump(db);                     /* the subscriptions register */

  const char record_text[] =
      "{\"seq\":7,\"type\":\"state.remember\",\"frame\":\"probe\","
      "\"corr\":null,\"at\":\"2026-10-03T00:00:00Z\",\"cause\":null,"
      "\"payload\":{\"key\":\"probe\",\"value\":\"42\"}}";
  std::string event_key = child + "/events/00000000000000000007";
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(*bp));
  bp->ops = (frm_store_op_t*)get_clear_memory(3 * sizeof(frm_store_op_t));
  bp->nops = 3;
  bp->ops[0].key = strdup(event_key.c_str());
  bp->ops[0].value = (uint8_t*)strdup(record_text);
  bp->ops[0].value_len = strlen(record_text);
  /* An events key with EXTRA segments after the seq: no notice ever. */
  bp->ops[1].key = strdup((child + "/events/7/extra").c_str());
  bp->ops[1].value = (uint8_t*)strdup("junk");
  bp->ops[1].value_len = strlen("junk");
  /* A non-digit seq tail: no notice ever. */
  bp->ops[2].key = strdup((child + "/events/7th").c_str());
  bp->ops[2].value = (uint8_t*)strdup("junk2");
  bp->ops[2].value_len = strlen("junk2");
  bp->op_name = "nested notify probe";
  message_t bm;
  bm.type = (uint32_t)FRM_STORE_BATCH;
  bm.payload = bp;
  bm.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(store, &bm));
  wave_db_pump(db);
  actor_run(&cap.actor, ACTOR_BATCH_SIZE);
  actor_run(&childcap.actor, ACTOR_BATCH_SIZE);

  ASSERT_EQ(cap.seqs.size(), 1u)
      << "the parent's watch sees the nested child event (ONE notice)";
  EXPECT_EQ(cap.seqs[0], 7u) << "the seq parses from the nested key's tail";
  EXPECT_EQ(cap.sids[0], child)
      << "the notice's sid is the child's FULL subtree path";
  EXPECT_STREQ(cap.records[0].c_str(), record_text)
      << "the notice's record_json is the committed record's EXACT text";

  ASSERT_EQ(childcap.seqs.size(), 1u)
      << "the child's own-path watch sees its commit too";
  EXPECT_EQ(childcap.seqs[0], 7u);
  EXPECT_EQ(childcap.sids[0], child);
  EXPECT_STREQ(childcap.records[0].c_str(), record_text);

  actor_destroy(&cap.actor);
  actor_destroy(&childcap.actor);
  frame_destroy(parent);
  wave_db_close(db);
}

TEST(TestFrame, TestSyncScanAndBatchRefuseOnPooledStore) {
  /* The two NEW sync store helpers refuse LOUD on a POOLED store — the same
     inline-only rule as wave_db_pump's pump refusal (no hang, no mailbox
     steal): production reaches these effects through the actor paths. */
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_config_t plain = test_config();
  frame_t* f = frame_create(db, NULL, NULL, &plain);
  ASSERT_NE(f, nullptr);

  char* text = (char*)0x1;
  EXPECT_NE(_frame_sync_scan(f, "sessions/x", "sessions/x0", 4, &text), 0)
      << "the sync scan refuses loud on a pooled store";
  EXPECT_EQ(text, nullptr) << "the refused scan returns no text";

  /* The refused batch's ops ownership still TRANSFERS: the helper frees the
     array and the ops' heap fields itself (valgrind watches this teardown). */
  frm_store_op_t* ops = (frm_store_op_t*)get_clear_memory(2 * sizeof(frm_store_op_t));
  ops[0].key = strdup("/state/local/a");
  ops[0].value = (uint8_t*)strdup("\"x\"");
  ops[0].value_len = strlen("\"x\"");
  ops[1].key = strdup("/state/local/b");
  ops[1].is_delete = 1;
  EXPECT_NE(_frame_sync_batch(f, ops, 2, "pooled refusal probe"), 0)
      << "the sync batch refuses loud on a pooled store";

  frame_destroy(f);
  scheduler_pool_stop(pool);       /* documented order: stop, close, destroy */
  wave_db_close(db);
  scheduler_pool_destroy(pool);
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

/* --- the resume repair (the plan's Task-3 step 1; spec §4's idempotency,
       pre-lifecycle, and pooled-refusal pins) --------------------------------

   The seed machinery hand-composes event records in the FROZEN shape
   ({"seq","type","frame","corr","at","cause","payload"}) and commits them at
   the frame's own zero-padded events keys through the sync family — the
   same shape the engine's writes ride. Scratch-disk idiom: fresh mkdtemp
   dirs created and destroyed by the test, never sa-demo-db; write-after-
   write assertions ride the NEXT session (the recorded WaveDB defect — a
   boot-restored session's own writes are invisible to that session's scans,
   durable and visible to the fresh one). */

#include <cstdlib>
#include <filesystem>
#include <vector>

/* One event record of the frozen shape; consumes `payload`. */
static std::string fr_record_head(long long seq, const std::string& sid,
                                  const char* type_name,
                                  json_value_t* payload) {
  json_value_t* rec = json_new_object();
  EXPECT_NE(rec, nullptr);
  json_object_set(rec, "seq", json_new_int(seq));
  json_object_set(rec, "type", json_new_string(type_name));
  json_object_set(rec, "frame", json_new_string(sid.c_str()));
  json_object_set(rec, "corr", json_new_null());
  json_object_set(rec, "at", json_new_string("2026-10-01T00:00:00Z"));
  json_object_set(rec, "cause",
                  (seq > 1) ? json_new_int(seq - 1) : json_new_null());
  json_object_set(rec, "payload", payload);
  char* text = json_serialize(rec);
  json_value_destroy(rec);
  std::string out((text != nullptr) ? text : "");
  free(text);
  return out;
}

/* The seed records the tests below use (the pre-lifecycle log's vocabulary:
   msg.append + the cell.run/cell.result pair — NO lifecycle types at all). */
static std::string fr_msg_record(long long seq, const std::string& sid,
                                 const char* role, const char* content) {
  json_value_t* payload = json_new_object();
  json_object_set(payload, "role", json_new_string(role));
  json_object_set(payload, "content", json_new_string(content));
  return fr_record_head(seq, sid, "msg.append", payload);
}

static std::string fr_cell_run_record(long long seq, const std::string& sid,
                                      const char* code, long long corr) {
  json_value_t* payload = json_new_object();
  json_object_set(payload, "code", json_new_string(code));
  json_object_set(payload, "corr", json_new_int(corr));
  return fr_record_head(seq, sid, "cell.run", payload);
}

static std::string fr_cell_result_record(long long seq, const std::string& sid,
                                         long long corr) {
  json_value_t* payload = json_new_object();
  json_object_set(payload, "corr", json_new_int(corr));
  json_object_set(payload, "status", json_new_int(0));
  json_object_set(payload, "text", json_new_string("ok"));
  return fr_record_head(seq, sid, "cell.result", payload);
}

static std::string fr_turn_start_record(long long seq, const std::string& sid,
                                        long long turn) {
  json_value_t* payload = json_new_object();
  json_object_set(payload, "turn", json_new_int(turn));
  return fr_record_head(seq, sid, "turn.start", payload);
}

/* Seed the records at the frame's own events keys (seqs 1..n, the ZERO-
   PADDED key shape the engine's writes use) as ONE atomic batch. */
static int fr_seed(frame_t* f, const std::string& sid,
                   const std::vector<std::string>& records) {
  frm_store_op_t* ops =
      (frm_store_op_t*)get_clear_memory(records.size() * sizeof(frm_store_op_t));
  for (size_t i = 0; i < records.size(); i++) {
    char key[96];
    snprintf(key, sizeof(key), "%s/events/%020llu", sid.c_str(),
             (unsigned long long)i + 1);
    ops[i].key = strdup(key);
    ops[i].value = (uint8_t*)strdup(records[i].c_str());
    ops[i].value_len = records[i].size();
  }
  return _frame_sync_batch(f, ops, records.size(), "resume-repair seed");
}

/* A reloaded record's byte echo (the seeds and echoes are all
   json_serialize outputs — equality here is byte-identical). */
static std::string fr_echo(json_value_t* rec) {
  char* raw = json_serialize(rec);
  std::string out((raw != nullptr) ? raw : "");
  free(raw);
  return out;
}

/* The reloaded record at an exact log seq (NULL when absent). */
static json_value_t* fr_at_seq(json_value_t* events, long long seq) {
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* v = json_get(rec, "seq");
    if (v != NULL && (long long)json_as_int(v) == seq) return rec;
  }
  return nullptr;
}

static size_t fr_count_type(json_value_t* events, const char* type_name) {
  size_t n = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), type_name)) n++;
  }
  return n;
}

TEST(TestFrame, TestPreLifecycleLogResumesUntouched) {
  /* The pre-lifecycle log (the plan's Task-3 step 1): records with NO
     lifecycle types at all (msg.append + the cell.run/cell.result pair) —
     the envelope's absence is NOT a truncation: the balance rule composes
     NOTHING at resume, the resume proceeds untouched, and the log is
     byte-identical after. Scratch-disk idiom; the write-read assertions ride
     the NEXT session (the recorded WaveDB same-session write-invisibility —
     see the block comment above). VALGRIND EXCLUSION (the sibling
     convention test_loop.cpp records for its scratch-disk restart tests):
     spinning under valgrind's emulation here — the ASan suite runs this
     test unexcluded. */
  frame_config_t cfg = test_config();
  char tmpl[] = "/tmp/sa-repair-XXXXXX";
  char* got = mkdtemp(tmpl);
  ASSERT_NE(got, nullptr);
  std::string dir(got);
  std::string loc = dir + "/db";

  /* Session 1: the pre-lifecycle log, seeded at the events keys. */
  wave_database_root_t* db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "pre-lifecycle era", &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  std::vector<std::string> seed;
  seed.push_back(fr_msg_record(1, sid, "user", "pre-lifecycle era"));
  seed.push_back(fr_cell_run_record(2, sid, "pass()", 9));
  seed.push_back(fr_cell_result_record(3, sid, 9));
  ASSERT_EQ(fr_seed(f, sid, seed), 0);
  frame_destroy(f);   /* not done, no lifecycle records — a LEGAL old shape */
  wave_db_close(db);

  /* Session 2: the resume composes NOTHING (balanced) — no refuse, no
     hang; the frame stays exactly as it was. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* resumed = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(resumed, nullptr)
      << "a pre-lifecycle log resumes untouched (balanced)";
  frame_destroy(resumed);
  wave_db_close(db);

  /* Session 3: byte-identical, no lifecycle types anywhere. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* handle = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(handle, nullptr);
  json_value_t* events = load_events(handle);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(json_size(events), 3u) << "nothing composed, nothing appended";
  for (size_t i = 0; i < 3; i++) {
    json_value_t* rec = fr_at_seq(events, (long long)i + 1);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(fr_echo(rec), seed[i]) << "seed record " << i + 1 << " mutated";
  }
  EXPECT_EQ(fr_count_type(events, "repair"), 0u);
  EXPECT_EQ(fr_count_type(events, "turn.start"), 0u);
  EXPECT_EQ(fr_count_type(events, "turn.end"), 0u);
  EXPECT_EQ(fr_count_type(events, "step.start"), 0u);
  EXPECT_EQ(fr_count_type(events, "step.end"), 0u);
  json_value_destroy(events);
  frame_destroy(handle);
  wave_db_close(db);
  std::filesystem::remove_all(dir);
}

TEST(TestFrame, TestResumeRepairRefusesOnPooledStore) {
  /* The pooled-store refusal (spec §4.5): the repair is a SYNC-FAMILY flow —
     the caller's thread must pump its awaited commit — so the NOT-done
     resume's repair on a POOLED store refuses LOUD before anything is
     pre-allocated or posted: resume refuses rather than half-repairs, and
     NEVER hangs (no post, no pump on a store whose pacing belongs to its
     scheduler workers). */
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_config_t pooled = test_config();
  pooled.pool = pool;
  frame_t* f = frame_create(db, NULL, NULL, &pooled);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  frame_destroy(f);   /* not done — no engine ever ran on it */

  EXPECT_EQ(frame_resume(db, sid.c_str(), &pooled), nullptr)
      << "the not-done frame's repair refuses loud on a POOLED store — "
         "no hang, no half-repair";

  scheduler_pool_stop(pool);       /* documented order: stop, close, destroy */
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}

/* --- Task 5: the orchestration slice (spawn = admit + start; the parent
   yields at FRAME_PHASE_CHILDREN and resumes on child reports) -------------

   The pooled tree tests run REAL scheduler-pool workers with a scripted SYNC
   backend completing inline inside the derive-reply dispatch: µs canned
   replies — model.h's §6 carve-out is about REAL model calls on a pool
   worker (a production backend implements submit), not µs canned ones.
   Python gate: the spawn cells run through the frame's own pyrt. */

#if defined(SA_HAS_PYTHON)

extern "C" void py_agent_init(void);   /* idempotent; re-mounts the bridge sink */

/* Decode ONE canned completion body into a model_reply_t (test_loop.cpp's
   scripted_decode, verbatim shape: message.content string-or-absent;
   tool_calls[0].function.arguments as a JSON string containing the argument
   object). */
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

/* JSON-string escaping (test-loop body shapes carry two layers: the
   arguments value is itself a JSON-encoded string). */
static std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:   out += c;
    }
  }
  return out;
}

/* An `execute` tool-call completion body whose `code` argument is `code`. */
static std::string canned_cell_body(const std::string& code) {
  std::string inner = std::string("{\"code\":\"") + json_escape(code) + "\"}";
  return std::string(
             R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
             R"json({"type":"function","function":{"name":"execute",)json"
             R"json("arguments":")json") + json_escape(inner) +
             std::string(R"json("}}]}}]})json");
}

/* A content-only completion body. */
static std::string canned_content_body(const std::string& text) {
  return std::string(
      R"json({"choices":[{"message":{"role":"assistant","content":")json") + text +
      std::string(R"json("}}]})json");
}

/* The inline test's harness shape (test_loop.cpp's scripted_model_t): a
   queue of canned bodies popped in order. */
typedef struct spawn_then_content_model_t {
  model_backend_t base;
  std::vector<std::string> replies;
} spawn_then_content_model_t;

static int spawn_then_content_complete(void* self, json_value_t* messages,
                                       json_value_t* tools, char** raw_out,
                                       model_reply_t** reply_out,
                                       char** error_out) {
  (void)tools;
  (void)raw_out;
  *reply_out = NULL;
  *error_out = NULL;
  spawn_then_content_model_t* sm = (spawn_then_content_model_t*)self;
  if (sm->replies.empty()) {
    *error_out = strdup("spawn-then-content model: queue empty");
    return -1;
  }
  std::string body = sm->replies.front();
  sm->replies.erase(sm->replies.begin());
  return scripted_decode(body, reply_out, error_out) == 0 ? 0 : -1;
}

/* The pooled tests' harness: canned replies KEYED by the derived system
   prompt's Goal line — the pool schedules the tree's frames in any order, so
   a keyed queue (not one shared ordered queue) is what makes the canned
   replies order-proof. An exhausted queue repeats its LAST canned reply —
   "any later parent turn" in the plan's listing. */
typedef struct goal_keyed_model_t {
  model_backend_t base;
  std::map<std::string, std::deque<std::string>> queues;
  std::map<std::string, std::string> sticky;
} goal_keyed_model_t;

static int goal_keyed_complete(void* self, json_value_t* messages,
                               json_value_t* tools, char** raw_out,
                               model_reply_t** reply_out, char** error_out) {
  (void)tools;
  (void)raw_out;
  *reply_out = NULL;
  *error_out = NULL;
  goal_keyed_model_t* gk = (goal_keyed_model_t*)self;

  std::string key;
  if (json_size(messages) > 0) {
    json_value_t* sys = json_at(messages, 0);
    json_value_t* content = (sys != NULL) ? json_get(sys, "content") : NULL;
    const char* text = (content != NULL) ? json_as_string(content) : "";
    const char* goal = strstr(text, "Goal: ");
    if (goal != NULL) {
      goal += strlen("Goal: ");
      key.assign(goal, strcspn(goal, "\r\n"));
    }
  }
  std::string body;
  auto q = gk->queues.find(key);
  if (q != gk->queues.end() && !q->second.empty()) {
    body = q->second.front();
    q->second.pop_front();
    gk->sticky[key] = body;
  } else {
    auto s = gk->sticky.find(key);
    if (s == gk->sticky.end()) {
      *error_out = strdup("goal-keyed model: no canned reply for the goal line");
      return -1;
    }
    body = s->second;
  }
  return scripted_decode(body, reply_out, error_out) == 0 ? 0 : -1;
}

/* Poller for the pooled tests: the pool runs everything — no joins, ever;
   the test waits for the terminal status like a real embedder would. */
static bool wait_frame_done(frame_t* f, int round10ms) {
  for (int i = 0; i < round10ms && frame_is_done(f) == 0; i++)
    platform_sleep_ms(10);
  return frame_is_done(f) != 0;
}

TEST(TestFrameTree, TestPooledParentSpawnsChildAndResumesOnReport) {
  py_agent_init();
  frame_config_t cfg = test_config();
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  cfg.pool = pool;

  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;    /* the store actor rides the SAME pool (a pooled
                              frame requires a pooled store — Task 2's guard) */
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_t* parent = frame_create(db, NULL, "parent goal", &cfg);
  ASSERT_NE(parent, nullptr);

  /* The parent's turn 1 = execute the spawn cell; the child's turn 1 =
     content-only "leaf done quietly"; any later parent turn = content-only
     "parent observed leaf" (the sticky repeat). */
  goal_keyed_model_t gk = {};   /* zero-init: the vtable's members set below */
  gk.base.complete = goal_keyed_complete;
  gk.queues["parent goal"].push_back(canned_cell_body(
      "import actor\nactor.spawn('leaf goal', None)\nprint('spawned')"));
  gk.queues["parent goal"].push_back(canned_content_body("parent observed leaf"));
  gk.queues["leaf goal"].push_back(canned_content_body("leaf done quietly"));
  frame_set_model_backend(parent, &gk.base);

  ASSERT_EQ(frame_start(parent), 0);   /* the event-driven entry */
  EXPECT_TRUE(wait_frame_done(parent, 6000))
      << "the parent resumed on the child's report and completed";

  /* The child's subtree's effects, bound in the parent's log: the spawn
     event + the child's frame.report (quiet completion) + the folded
     frame.join. */
  json_value_t* events = load_events(parent);
  ASSERT_NE(events, nullptr);
  size_t n_report = 0, n_spawn = 0, n_join = 0;
  std::string child_sid;
  std::string report_text;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* payload = json_get(rec, "payload");
    if (event_is(rec, "frame.spawn")) n_spawn++;
    if (event_is(rec, "frame.report")) {
      n_report++;
      if (child_sid.empty()) {
        child_sid = json_as_string(json_get(payload, "child_sid"));
        report_text = json_as_string(json_get(payload, "text"));
      }
    }
    if (event_is(rec, "frame.join")) n_join++;
  }
  EXPECT_EQ(n_spawn, 1u);
  EXPECT_EQ(n_report, 1u) << "exactly ONE report per child, regardless of the "
                             "child's work volume (S004 accountability)";
  EXPECT_EQ(n_join, 1u) << "join folded into the resume path";
  EXPECT_FALSE(child_sid.empty());
  EXPECT_EQ(report_text, "leaf done quietly")
      << "the quiet completion REPORTS its assistant content into the parent";

  /* The child's own log, WITHOUT a live handle (the spawn happened inside
     the engine; the test never held the child): frame_resume(db, child_sid,
     &plain) with a POOL-LESS config opens its DONE subtree as an inline
     handle. A resumed done frame re-runs nothing (frame_start refuses for
     done frames); the handle's reads ride the documented DIRECT debug scan
     (frame_debug_events) — NO sync write API is available on it because the
     store here is POOLED (spec §5's inline-only rule: a sync write would
     refuse loud, which is exactly the shape this assertion documents).
     Assert its ONE frame.report with the quiet content, then destroy the
     handle. */
  {
    frame_config_t plain = test_config();   /* pool NULL: inline handle shape */
    frame_t* resumed = frame_resume(db, child_sid.c_str(), &plain);
    ASSERT_NE(resumed, nullptr);
    json_value_t* child_events = load_events(resumed);
    ASSERT_NE(child_events, nullptr);
    size_t n_child_report = 0;
    for (size_t i = 0; i < json_size(child_events); i++) {
      json_value_t* rec = json_at(child_events, i);
      if (event_is(rec, "frame.report")) {
        n_child_report++;
        EXPECT_STREQ(json_as_string(json_get(json_get(rec, "payload"), "text")),
                     "leaf done quietly");
      }
    }
    EXPECT_EQ(n_child_report, 1u);
    json_value_destroy(child_events);
    frame_destroy(resumed);
  }
  json_value_destroy(events);

  frame_destroy(parent);   /* the resumed-child handle was destroyed above */
  scheduler_pool_stop(pool);   /* documented order: stop, then close, then destroy */
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}

TEST(TestFrameTree, TestPooledChildFailureResumesParentWithTheFailure) {
  /* The pooled shape end to end: parent turn 1 spawns (its engine is live,
     so the spawn reply STARTS the child and counts it); the child — cap
     inherited via frame_set_loop_turn_cap on the PARENT, with an
     always-tool sticky reply keyed to its own goal — hits its turn cap and
     fails loud; the parent RESUMES (the confirmed bind commit's
     FRM_CHILD_REPORT{failed=1}) and its next turn's derive shows the
     failure line; the parent answers content and completes done.
     Assertions: child done; parent done; the parent's log has exactly ONE
     report for the child whose text carries the failure kind. All
     actor-driven (no pumps, no joins — the pool runs everything; poll
     frame_is_done). */
  py_agent_init();
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
  frame_t* parent = frame_create(db, NULL, "parent of failures", &cfg);
  ASSERT_NE(parent, nullptr);

  /* The cap lands on the CHILD through the START branch's inheritance
     (child->loop_turn_cap = parent->loop_turn_cap) — set BEFORE the spawn
     turn. Parent budget (3): turn 1 = spawn cell, turn 2 = content while
     the child runs (the CHILDREN yield), turn 3 = the post-report resume's
     content (done). The child (same inherited cap 3) runs tool cells on
     turns 1..3, so its turn 4 hits the cap → control turn-limit → the
     terminate binds "turn-limit: model turn budget exhausted" into the
     parent. (A parent cap of 2 cannot survive the resume turn — the
     failing parent then is a different, wrong shape.) */
  goal_keyed_model_t gk = {};
  gk.base.complete = goal_keyed_complete;
  gk.queues["parent of failures"].push_back(canned_cell_body(
      "import actor\nactor.spawn('spiral forever', None)\nprint('spawned')"));
  gk.queues["parent of failures"].push_back(
      canned_content_body("parent observed the failure"));
  /* The child cycles tool cells on turns 1..3 — the cells must be
     BYTE-DISTINCT, not one sticky body: three BYTE-IDENTICAL cells would
     now trip the doom-loop breaker BEFORE the turn cap (the breaker refuses
     the threshold-th identical call), and the child's bound report would
     carry "doom-loop" instead of the cap's "turn-limit" this test pins. */
  gk.queues["spiral forever"].push_back(canned_cell_body("pass"));
  gk.queues["spiral forever"].push_back(canned_cell_body("pass\n# cycle 2"));
  gk.queues["spiral forever"].push_back(canned_cell_body("pass\n# cycle 3"));
  frame_set_loop_turn_cap(parent, 3);
  frame_set_model_backend(parent, &gk.base);

  ASSERT_EQ(frame_start(parent), 0);
  EXPECT_TRUE(wait_frame_done(parent, 6000))
      << "the parent resumed on the FAILED child's report and completed";

  json_value_t* events = load_events(parent);
  ASSERT_NE(events, nullptr);
  size_t n_spawn = 0, n_report = 0, n_join = 0;
  std::string child_sid;
  std::string report_text;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* payload = json_get(rec, "payload");
    if (event_is(rec, "frame.spawn")) {
      n_spawn++;
      child_sid = json_as_string(json_get(payload, "child_sid"));
    } else if (event_is(rec, "frame.report")) {
      n_report++;
      report_text = json_as_string(json_get(payload, "text"));
    } else if (event_is(rec, "frame.join")) {
      n_join++;
    }
  }
  EXPECT_EQ(n_spawn, 1u);
  EXPECT_EQ(n_report, 1u) << "ONE failure report bound for the failed child";
  EXPECT_NE(report_text.find("turn-limit"), std::string::npos)
      << "the bound report's text carries the failure kind";
  EXPECT_EQ(n_join, 1u);
  EXPECT_FALSE(child_sid.empty());
  json_value_destroy(events);

  /* The child is done, its own log keeps the exact control kind, and its
     composed frame.report record rides its own subtree (read through the
     engine-less resumed handle, like the quiet-completion shape above). */
  {
    frame_config_t plain = test_config();
    frame_t* resumed = frame_resume(db, child_sid.c_str(), &plain);
    ASSERT_NE(resumed, nullptr);
    EXPECT_EQ(frame_is_done(resumed), 1u)
        << "a failed CHILD is done — never a zombie a parent awaits";
    json_value_t* child_events = load_events(resumed);
    ASSERT_NE(child_events, nullptr);
    bool saw_turn_limit = false, saw_report = false;
    for (size_t i = 0; i < json_size(child_events); i++) {
      json_value_t* rec = json_at(child_events, i);
      json_value_t* payload = json_get(rec, "payload");
      if (event_is(rec, "control")) {
        json_value_t* k = (payload != NULL) ? json_get(payload, "kind") : NULL;
        if (k != NULL && strcmp(json_as_string(k), "turn-limit") == 0) {
          saw_turn_limit = true;
        }
      } else if (event_is(rec, "frame.report")) {
        saw_report = true;
        EXPECT_STREQ(
            json_as_string(json_get(payload, "text")),
            "turn-limit: model turn budget exhausted");
      }
    }
    EXPECT_TRUE(saw_turn_limit);
    EXPECT_TRUE(saw_report);
    json_value_destroy(child_events);
    frame_destroy(resumed);
  }

  frame_destroy(parent);
  scheduler_pool_stop(pool);
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}

TEST(TestFrameTree, TestPooledTreeKeepsOneReportPerChildWithContiguousSeq) {
  /* The accountability storm: N children spawned from one parent cell, each
     content-quitting; the parent's log holds N bound reports in a CONTIGUOUS
     seq chain under concurrent scheduling — the property the old per-frame
     write-lock test chased, proven without a lock (the store actor is the
     only serializer; the seq counters pre-allocate in each frame's own
     dispatch). */
  py_agent_init();
  frame_config_t cfg = test_config();
  scheduler_pool_t* pool = scheduler_pool_create(4);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  cfg.pool = pool;
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_t* parent = frame_create(db, NULL, "storm root", &cfg);
  ASSERT_NE(parent, nullptr);

  /* The parent's first turn: ONE cell that spawns N = 8 children in a loop;
     its next (content) turn quits while the children run ->
     FRAME_PHASE_CHILDREN. */
  goal_keyed_model_t gk = {};
  gk.base.complete = goal_keyed_complete;
  gk.queues["storm root"].push_back(canned_cell_body(
      "import actor\nfor i in range(8):\n"
      "    actor.spawn('leaf ' + str(i), None)\n"
      "print('spawned 8')"));
  gk.queues["storm root"].push_back(canned_content_body("storm observed"));
  for (int i = 0; i < 8; i++) {
    gk.queues["leaf " + std::to_string(i)].push_back(
        canned_content_body("leaf " + std::to_string(i) + " done quietly"));
  }
  frame_set_model_backend(parent, &gk.base);

  ASSERT_EQ(frame_start(parent), 0);
  EXPECT_TRUE(wait_frame_done(parent, 6000));

  /* Assert: the ONE spawn cell + 8 frame.spawn events + 8 bound frame.report
     events + 8 frame.join events; every record's cause == the previous
     record's seq; seq runs 1..N_total contiguous — the store actor's
     serialization produced no gaps and no duplicates without any lock. */
  json_value_t* events = load_events(parent);
  ASSERT_NE(events, nullptr);
  size_t n_run = 0, n_spawn = 0, n_report = 0, n_join = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    ASSERT_EQ(json_as_int(json_get(rec, "seq")), (int64_t)(i + 1))
        << "the seq chain runs 1..N contiguous";
    json_value_t* cause = json_get(rec, "cause");
    if (i == 0) {
      EXPECT_EQ(json_type(cause), JSON_NULL);
    } else {
      EXPECT_EQ(json_as_int(cause), (int64_t)i)
          << "every record's cause is the previous record's seq";
    }
    if (event_is(rec, "cell.run")) {
      n_run++;
    } else if (event_is(rec, "frame.spawn")) {
      n_spawn++;
    } else if (event_is(rec, "frame.report")) {
      n_report++;
    } else if (event_is(rec, "frame.join")) {
      n_join++;
    }
  }
  EXPECT_EQ(n_run, 1u);
  EXPECT_EQ(n_spawn, 8u);
  EXPECT_EQ(n_report, 8u) << "exactly ONE report per child under concurrent "
                             "scheduling";
  EXPECT_EQ(n_join, 8u);
  json_value_destroy(events);

  frame_destroy(parent);
  scheduler_pool_stop(pool);
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}

TEST(TestFrameTree, TestChildAskParksTheChildAndTheParentWaits) {
  /* THIS SPEC bullet's tree shape (§7's child-frame ask: "parent stays
     parked in CHILDREN; one owner-surface ask"): a child's `actor.ask`
     parks the CHILD only — the parent's parked CHILDREN await stands
     (its engine never sees the child's park), and the OWNER's answer
     resumes the CHILD, whose report then binds up and resumes the
     PARENT. THE SIMPLIFICATION (honest pin of the COOPERATION, not the
     full spawn flow): the child is admitted by the TEST-HELD
     `frame_spawn` on the LIVE parent engine — the same START branch a
     cell's `actor.spawn` reply rides (the admission + start + count), so
     it still gives the honest START-branch live_children and the
     inherited backend — but the test HOLDS the child handle so the
     owner's reply can reach its mailbox. The parent's turn 1 is a
     content turn (its yield rides the finish batch's live_children
     rule); the ladder mode is free (the ask machinery is all modes' —
     the modes' own ask pins live in the ladder family). */

  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* parent = frame_create(db, NULL, "parent goal", &cfg);
  ASSERT_NE(parent, nullptr);

  goal_keyed_model_t gk = {};   /* zero-init: the vtable's members set below */
  gk.base.complete = goal_keyed_complete;
  gk.queues["parent goal"].push_back(
      canned_content_body("parent waits on the leaf"));
  gk.queues["parent goal"].push_back(
      canned_content_body("parent saw the leaf's answer"));
  gk.queues["child goal"].push_back(
      canned_cell_body("import actor\nactor.ask('child needs direction?', "
                       "['go', 'stop'])\nprint('asked')"));
  gk.queues["child goal"].push_back(canned_content_body("child done quietly"));
  frame_set_model_backend(parent, &gk.base);

  /* The engine is queued FIRST (frame_start), THEN the spawn — so the START
     branch sees the live engine and the child starts + counts. */
  ASSERT_EQ(frame_start(parent), 0);
  frame_t* child = frame_spawn(parent, "child goal", NULL);
  ASSERT_NE(child, nullptr);
  ASSERT_EQ(_frame_engine_state(parent)->live_children, 1u)
      << "the START branch counted the child";

  /* THE PARENT'S YIELD: the content turn's finish saw the live child and
     yields — run_loop rc 2, parked in CHILDREN, still running. */
  EXPECT_EQ(frame_run_loop(parent), 2) << "yielded awaiting the child";
  EXPECT_EQ(frame_is_done(parent), 0);
  {
    frame_engine_state_t* pe = _frame_engine_state(parent);
    ASSERT_NE(pe, nullptr);
    EXPECT_EQ(pe->phase, FRAME_PHASE_CHILDREN) << "the parked-CHILDREN state";
  }

  /* THE CHILD ASKS: the ask parks the CHILD (run_loop rc 2, one "ask"
     record) and the parent's park does not move — the tree's cooperation
     under test. */
  EXPECT_EQ(frame_run_loop(child), 2) << "the child parked on its ask";
  {
    frame_engine_state_t* ce = _frame_engine_state(child);
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->phase, FRAME_PHASE_ASK) << "the child's own park";
    frame_engine_state_t* pe = _frame_engine_state(parent);
    EXPECT_EQ(pe->phase, FRAME_PHASE_CHILDREN)
        << "the child's ask did not move the parent's park";
  }
  json_value_t* child_events = load_events(child);
  ASSERT_NE(child_events, nullptr);
  EXPECT_EQ(fr_count_type(child_events, "ask"), 1u)
      << "exactly ONE owner-surface ask from the child";
  std::string ask_id;
  json_value_t* ask_rec = NULL;
  for (size_t i = 0; i < json_size(child_events); i++) {
    json_value_t* rec = json_at(child_events, i);
    if (event_is(rec, "ask")) {
      ask_rec = rec;
      break;
    }
  }
  ASSERT_NE(ask_rec, nullptr);
  {
    json_value_t* ap = json_get(ask_rec, "payload");
    ASSERT_NE(ap, nullptr);
    EXPECT_STREQ(json_as_string(json_get(ap, "question")),
                 "child needs direction?");
    json_value_t* id = json_get(ap, "askId");
    ASSERT_NE(id, nullptr);
    ask_id = json_as_string(id);
  }
  ASSERT_EQ(ask_id.size(), 8u);
  EXPECT_EQ(fr_count_type(child_events, "ask.reply"), 0u)
      << "nothing resolved while parked";
  json_value_destroy(child_events);
  json_value_t* parent_events = load_events(parent);
  ASSERT_NE(parent_events, nullptr);
  EXPECT_EQ(fr_count_type(parent_events, "ask"), 0u)
      << "the parent's log NEVER holds the child's ask (one owner-surface "
         "ask lives in the child's subtree)";
  json_value_destroy(parent_events);

  /* THE OWNER'S ANSWER: the parked CHILD consumes it, its next content turn
     is its quiet-completion outcome, and the engine ends — the report's
     bind may trail the child's engine end (the residual chain is the
     driver drain's business, pumped below). */
  EXPECT_EQ(frame_ask_reply(child, ask_id.c_str(), 0, "the tree says go"), 0);
  EXPECT_EQ(frame_run_loop(child), 0) << "the answer resumed the child";
  EXPECT_EQ(frame_is_done(child), 1);

  /* The parent's park stands through the residual chain: the engine-driven
     bind composed + posted during the child's drive (the pump order), its
     corr reply routes at the CHILD's actor — still queued for the driver.
     The interim run pumps empty and returns the standing rc 2. */
  EXPECT_EQ(frame_run_loop(parent), 2)
      << "the bind's reply awaits its route at the child";
  /* The residual pump (the TestChildTurnLimit precedent's shape): dispatch
     the bind reply at the child — its route posts FRM_CHILD_REPORT at the
     parent (the run drains the child's mailbox and returns false). */
  actor_run(_frame_actor(child), ACTOR_BATCH_SIZE);
  EXPECT_EQ(frame_run_loop(parent), 0)
      << "the report's resume continued the parent to done";
  EXPECT_EQ(frame_is_done(parent), 1) << "the parent continued and completed";

  /* THE BIND's effects in the parent's log (+ the child's own honest ask
     trail). */
  parent_events = load_events(parent);
  ASSERT_NE(parent_events, nullptr);
  size_t n_spawn = 0, n_report = 0, n_join = 0;
  std::string report_text;
  for (size_t i = 0; i < json_size(parent_events); i++) {
    json_value_t* rec = json_at(parent_events, i);
    if (event_is(rec, "frame.spawn")) n_spawn++;
    if (event_is(rec, "frame.report")) {
      n_report++;
      report_text = json_as_string(
          json_get(json_get(rec, "payload"), "text"));
    }
    if (event_is(rec, "frame.join")) n_join++;
  }
  EXPECT_EQ(n_spawn, 1u);
  EXPECT_EQ(n_report, 1u)
      << "the child's report FLOWED BACK UP through the park";
  EXPECT_EQ(report_text, "child done quietly")
      << "the quiet completion reports the child's own content";
  EXPECT_EQ(n_join, 1u);
  EXPECT_EQ(fr_count_type(parent_events, "ask"), 0u);
  json_value_destroy(parent_events);

  child_events = load_events(child);
  ASSERT_NE(child_events, nullptr);
  EXPECT_EQ(fr_count_type(child_events, "ask.reply"), 1u)
      << "the owner's answer consumed exactly once";
  json_value_destroy(child_events);

  frame_destroy(child);
  frame_destroy(parent);
  wave_db_close(db);
}

TEST(TestFrame, TestInlineParentYieldsAwaitingChildren) {
  /* NO pool: the inline shape. The parent's scripted model answers its FIRST
     turn with the spawn tool call (its cell admits + starts the child — the
     child's engine queues, nobody pumps an inline actor but its driver), and
     its SECOND turn with content-only text while the child is live:
     frame_run_loop returns 2 — yielded, live, awaiting children (the child's
     own loop is the test's business, as in the synchronous world today). */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* parent = frame_create(db, NULL, "inline await", &cfg);
  ASSERT_NE(parent, nullptr);
  spawn_then_content_model_t scm = {};   /* zero-init: the vtable's members set below */
  scm.base.complete = spawn_then_content_complete;
  scm.replies.push_back(canned_cell_body(
      "import actor\nactor.spawn('leaf goal', None)\nprint('spawned')"));
  scm.replies.push_back(canned_content_body("parent waits on the leaf"));
  frame_set_model_backend(parent, &scm.base);

  EXPECT_EQ(frame_run_loop(parent), 2) << "yielded awaiting children";
  EXPECT_EQ(frame_is_done(parent), 0) << "live children hold the frame running";

  json_value_t* events = load_events(parent);
  ASSERT_NE(events, nullptr);
  size_t n_spawn = 0;
  std::string child_sid;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "frame.spawn")) {
      n_spawn++;
      child_sid = json_as_string(json_get(json_get(rec, "payload"), "child_sid"));
    }
  }
  EXPECT_EQ(n_spawn, 1u);
  EXPECT_FALSE(child_sid.empty());
  json_value_destroy(events);

  frame_destroy(parent);
  wave_db_close(db);
}

TEST(TestFrame, TestChildrenYieldTurnEndRidesTheFinishBatch) {
  /* The children-yield shape (the plan's Task-2 step 1): a content turn with
     live children pending yields at the finish reply (status stays running)
     — the yield's turn.end {reason completed} rides the SAME batch as the
     msg.append, and NO status put rides it. The yield batch's event record
     group is contiguous by seq — one atomic commit, nothing half-committed
     on the yield's seq boundary. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* parent = frame_create(db, NULL, "inline yield batch", &cfg);
  ASSERT_NE(parent, nullptr);
  goal_keyed_model_t gk = {};   /* zero-init: the vtable's members set below */
  gk.base.complete = goal_keyed_complete;
  gk.queues["inline yield batch"].push_back(canned_cell_body(
      "import actor\nactor.spawn('leaf goal', None)\nprint('spawned')"));
  gk.queues["inline yield batch"].push_back(
      canned_content_body("parent waits on the leaf"));
  gk.queues["leaf goal"].push_back(canned_content_body("leaf done quietly"));
  frame_set_model_backend(parent, &gk.base);

  EXPECT_EQ(frame_run_loop(parent), 2) << "yielded awaiting children";
  /* The driver drained the store before returning: the YIELD batch (with its
     envelope riders) has committed — and no status put rode it. */
  EXPECT_EQ(frame_is_done(parent), 0)
      << "the yield batch leaves the frame running (the resuming turn's own "
         "finish batch writes done)";

  json_value_t* events = load_events(parent);
  ASSERT_NE(events, nullptr);

  /* Locate the yield's msg.append and read the batch around it by seq. */
  json_value_t* m = nullptr;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "msg.append")) continue;
    json_value_t* p = json_get(rec, "payload");
    if (p != NULL && json_get(p, "role") != NULL &&
        strcmp(json_as_string(json_get(p, "role")), "assistant") == 0 &&
        json_get(p, "content") != NULL &&
        strcmp(json_as_string(json_get(p, "content")),
               "parent waits on the leaf") == 0) {
      m = rec;
      break;
    }
  }
  ASSERT_NE(m, nullptr) << "the yield's msg.append committed";
  long long m_seq = (long long)json_as_int(json_get(m, "seq"));

  /* The record AT each adjacent seq (by binary position in the ascending
     scan — the log's own seq order). */
  json_value_t* ss = nullptr;
  json_value_t* se = nullptr;
  json_value_t* te = nullptr;
  json_value_t* ts2 = nullptr;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    long long s = (long long)json_as_int(json_get(rec, "seq"));
    if (s == m_seq - 1) ss = rec;
    if (s == m_seq + 1) se = rec;
    if (s == m_seq + 2) te = rec;
    if (event_is(rec, "turn.start")) {
      json_value_t* p = json_get(rec, "payload");
      if (p != NULL && json_get(p, "turn") != NULL &&
          (long long)json_as_int(json_get(p, "turn")) == 2) {
        ts2 = rec;
      }
    }
  }
  ASSERT_NE(ss, nullptr) << "the yield batch's step.start (m_seq-1)";
  ASSERT_NE(se, nullptr) << "the yield batch's step.end (m_seq+1)";
  ASSERT_NE(te, nullptr) << "the yield batch's turn.end (m_seq+2)";
  ASSERT_NE(ts2, nullptr) << "turn 2 opened before the batch";

  EXPECT_TRUE(event_is(ss, "step.start"));
  EXPECT_TRUE(event_is(se, "step.end"));
  EXPECT_TRUE(event_is(te, "turn.end"));

  /* Contiguity IS the atomicity: the four records of the yield batch carry
     seqs m-1..m+2 — nothing interleaved, nothing half-committed, no gap. */
  json_value_t* pss = json_get(ss, "payload");
  json_value_t* pse = json_get(se, "payload");
  json_value_t* pte = json_get(te, "payload");
  ASSERT_NE(pss, nullptr);
  ASSERT_NE(pse, nullptr);
  ASSERT_NE(pte, nullptr);
  EXPECT_EQ((long long)json_as_int(json_get(pss, "turn")), 2);
  EXPECT_EQ((long long)json_as_int(json_get(pss, "step")), 1);
  EXPECT_EQ((long long)json_as_int(json_get(pse, "turn")), 2);
  EXPECT_EQ((long long)json_as_int(json_get(pse, "step")), 1);
  EXPECT_EQ((long long)json_as_int(json_get(pte, "turn")), 2);
  json_value_t* reason = json_get(pte, "reason");
  ASSERT_NE(reason, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "completed")
      << "the yield's turn ends completed (spec §4.6's pin)";
  EXPECT_LT((long long)json_as_int(json_get(ts2, "seq")), m_seq - 1);

  json_value_destroy(events);
  frame_destroy(parent);   /* the CHILD's record dies with the parent's
                              teardown list (the engine-less caller's shape) */
  wave_db_close(db);
}

TEST(TestFrame, TestSecondResumeComposesNothing) {
  /* Idempotency (the plan's Task-3 step 1; spec §4.6): the balance rule is
     the whole guarantee — the resume repair's compose is empty whenever the
     tail is balanced, whatever the frame's pause semantics.
     (a) On disk: a repaired tail re-resumes into NOTHING — the second
         resume's closer-compose is empty, and the next fresh session proves
         no second batch ever landed (no second repair brief). The scratch-
         disk idiom + the next-session read discipline (the recorded WaveDB
         defect: a restored session's own writes are invisible to that
         session's scans).
     (b) In-memory: the ENGINE-PAUSED shape — a children-yield frame (its
         yield batch ended the turn completed; the engine parked at
         FRAME_PHASE_CHILDREN with live children) re-resumed fresh composes
         nothing: the CHILDREN phase is engine scheduling, not turn
         lifecycle.
     VALGRIND EXCLUSION (the sibling convention test_loop.cpp records for
     its scratch-disk restart tests): this test spins under valgrind's
     emulation here — the ASan suite runs it unexcluded. */

  /* --- (a) the repaired tail's second resume ------------------------------ */
  frame_config_t cfg = test_config();
  char tmpl1[] = "/tmp/sa-repair-XXXXXX";
  char* got1 = mkdtemp(tmpl1);
  ASSERT_NE(got1, nullptr);
  std::string dir1(got1);
  std::string loc = dir1 + "/db";

  wave_database_root_t* db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "repair idempotency", &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  std::vector<std::string> seed;
  seed.push_back(fr_turn_start_record(1, sid, 1));
  ASSERT_EQ(fr_seed(f, sid, seed), 0);
  frame_destroy(f);   /* the crash-cut frame, closed durable */
  wave_db_close(db);

  /* The first resume repairs: [repair (not-started brief), turn.end]. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* first = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(first, nullptr);
  frame_destroy(first);
  wave_db_close(db);

  /* THE SECOND resume over the repaired tail: balanced — composes nothing,
     succeeds (no refuse, no hang). */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* second = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(second, nullptr)
      << "a repaired tail is balanced; the second resume is a no-op";
  frame_destroy(second);
  wave_db_close(db);

  /* The fresh session: exactly ONE closer batch exists — no second repair
     brief, no second turn.end. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* handle = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(handle, nullptr);
  json_value_t* events = load_events(handle);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(json_size(events), 3u)
      << "opener + ONE closer batch [repair, turn.end] — nothing else";
  EXPECT_EQ(fr_count_type(events, "repair"), 1u)
      << "no second repair brief composed";
  EXPECT_EQ(fr_count_type(events, "turn.end"), 1u);
  json_value_destroy(events);
  frame_destroy(handle);
  wave_db_close(db);
  std::filesystem::remove_all(dir1);

  /* --- (b) the ENGINE-PAUSED (children-yield) shape ----------------------- */
  py_agent_init();
  wave_database_root_t* idb = wave_db_open(NULL);
  ASSERT_NE(idb, nullptr);
  frame_t* parent = frame_create(idb, NULL, "yielded repair probe", &cfg);
  ASSERT_NE(parent, nullptr);
  goal_keyed_model_t gk = {};   /* zero-init: the vtable's members set below */
  gk.base.complete = goal_keyed_complete;
  gk.queues["yielded repair probe"].push_back(canned_cell_body(
      "import actor\nactor.spawn('probe leaf', None)\nprint('spawned')"));
  gk.queues["yielded repair probe"].push_back(
      canned_content_body("parent waits on the leaf"));
  gk.queues["probe leaf"].push_back(canned_content_body("leaf done quietly"));
  frame_set_model_backend(parent, &gk.base);

  EXPECT_EQ(frame_run_loop(parent), 2) << "yielded awaiting children";

  /* The yielded frame's tail is BALANCED (the yield's turn ended completed)
     — capture the log's record count before the second handle resumes. */
  json_value_t* before = load_events(parent);
  ASSERT_NE(before, nullptr);
  size_t count_before = json_size(before);
  json_value_destroy(before);

  /* The fresh handle on the SAME session (in-memory: every write visible —
     the shape where a repaired log's briefs actually reach derives today):
     the repair scans the tail, folds the CLOSED turn, composes nothing. */
  frame_t* fresh = frame_resume(idb, frame_sid(parent), &cfg);
  ASSERT_NE(fresh, nullptr)
      << "the child-pending frame's tail is balanced — resume composes nothing";
  json_value_t* after = load_events(fresh);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(json_size(after), count_before)
      << "the paused engine's balanced tail composed NOTHING at resume";
  EXPECT_EQ(fr_count_type(after, "repair"), 0u);
  json_value_destroy(after);
  frame_destroy(fresh);

  frame_destroy(parent);   /* the live spawn's teardown (loud, expected) */
  wave_db_close(idb);
}

/* --- escalation Task 2: the park — close batch, the reply, the stale drop

   The ask flow (spec §1): turn 1's cell calls actor.ask; the cell completes;
   ONE atomic close batch lands [cell.result, step.end, "ask", turn.end
   {blocked}]; the engine rests in FRAME_PHASE_ASK (frame_run_loop returns 2,
   live); frame_ask_reply posts FRM_ASK_REPLY; the dispatch composes ONE
   reply batch ["ask.reply", msg.append user] and reposts the FRM_TURN
   continuation — a fresh turn derives the answer. */

/* The ask tests' log needles (each test installs its own recorder). */
static int _ask_belt_lines = 0;
static int _ask_stale_lines = 0;

static void _ask_belt_recorder(log_Event* ev) {
  va_list ap;
  va_copy(ap, ev->ap);
  char line[512];
  vsnprintf(line, sizeof(line), ev->fmt, ap);
  va_end(ap);
  if (strstr(line, "second ask arrived") != NULL) _ask_belt_lines++;
}

static void _ask_stale_recorder(log_Event* ev) {
  va_list ap;
  va_copy(ap, ev->ap);
  char line[512];
  vsnprintf(line, sizeof(line), ev->fmt, ap);
  va_end(ap);
  if (strstr(line, "no park holds it") != NULL) _ask_stale_lines++;
}

/* The ask tests' scripted model: canned bodies popped in order; every
   model call's derived-messages array serializes into `captured` (the
   answer's arrive-by-derive pin). */
typedef struct ask_capture_model_t {
  model_backend_t base;
  std::vector<std::string> replies;
  std::vector<std::string> captured;
} ask_capture_model_t;

static int ask_capture_complete(void* self, json_value_t* messages,
                                json_value_t* tools, char** raw_out,
                                model_reply_t** reply_out, char** error_out) {
  (void)tools;
  (void)raw_out;
  *reply_out = NULL;
  *error_out = NULL;
  ask_capture_model_t* cm = (ask_capture_model_t*)self;
  char* seen = json_serialize(messages);
  if (seen != NULL) {
    cm->captured.push_back(std::string(seen));
    free(seen);
  }
  if (cm->replies.empty()) {
    *error_out = strdup("ask capture model: queue empty (an unexpected "
                        "model turn)");
    return -1;
  }
  std::string body = cm->replies.front();
  cm->replies.erase(cm->replies.begin());
  return scripted_decode(body, reply_out, error_out) == 0 ? 0 : -1;
}

/* Park one turn whose cell runs `ask_code` (the shared setup): installs the
   scripted model with `replies`, runs the loop, returns the frame (parked:
   frame_run_loop returned 2, the frame NOT done). The turn's close-batch
   records are asserted here (the SAME-batch shape is the park's heart) and
   the minted ask_id is returned via *ask_id_out. */
static frame_t* ask_park_and_pin(wave_database_root_t* db,
                                 ask_capture_model_t* cm,
                                 std::vector<std::string> replies,
                                 const std::string& ask_code,
                                 std::string* ask_id_out);

TEST(TestFrame, TestAskParksAndBlocksTheTurn) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "decide: which way?", &cfg);
  ASSERT_NE(f, nullptr);

  ask_capture_model_t cm = {};
  cm.base.complete = ask_capture_complete;
  cm.replies = {
      canned_cell_body("import actor\nactor.ask('q?', ['yes', 'no'])"
                       "\nprint('asked')"),
      canned_content_body("done after the answer")};
  frame_set_model_backend(f, &cm.base);

  /* The park: the loop returns LIVE (rc 2) with the frame NOT done — the
     turn closed blocked and the engine rests on the parked ask. */
  EXPECT_EQ(frame_run_loop(f), 2) << "parked awaiting the owner's reply";
  EXPECT_EQ(frame_is_done(f), 0);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);

  /* The close batch is ONE atomic group of FOUR consecutive records:
     cell.result + step.end + the "ask" record + turn.end{blocked}. */
  size_t ask_index = SIZE_MAX;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "ask")) {
      ASSERT_EQ(ask_index, SIZE_MAX) << "exactly ONE ask record";
      ask_index = i;
    }
  }
  ASSERT_NE(ask_index, SIZE_MAX) << "the cell's ask was parked";
  ASSERT_GE(ask_index, 2u) << "cell.result and step.end precede it IN the batch";
  json_value_t* result_rec = json_at(events, ask_index - 2);
  json_value_t* step_end_rec = json_at(events, ask_index - 1);
  json_value_t* ask_rec = json_at(events, ask_index);
  json_value_t* turn_end_rec = json_at(events, ask_index + 1);
  ASSERT_NE(turn_end_rec, nullptr);
  ASSERT_TRUE(event_is(result_rec, "cell.result"));
  ASSERT_TRUE(event_is(step_end_rec, "step.end"));
  ASSERT_TRUE(event_is(turn_end_rec, "turn.end"));
  /* The SAME-batch proof: four consecutive seqs (turn-lifecycle's batching
     rule — the group can never half-apply across a crash). */
  long long base_seq = (long long)json_as_int(json_get(result_rec, "seq"));
  EXPECT_EQ((long long)json_as_int(json_get(step_end_rec, "seq")), base_seq + 1);
  EXPECT_EQ((long long)json_as_int(json_get(ask_rec, "seq")), base_seq + 2);
  EXPECT_EQ((long long)json_as_int(json_get(turn_end_rec, "seq")), base_seq + 3);

  json_value_t* ask_payload = json_get(ask_rec, "payload");
  ASSERT_NE(ask_payload, nullptr);
  json_value_t* ask_id_v = json_get(ask_payload, "askId");
  ASSERT_NE(ask_id_v, nullptr);
  std::string ask_id = json_as_string(ask_id_v);
  EXPECT_EQ(ask_id.size(), 8u) << "the mint: the sid allocator's 8-hex shape";
  EXPECT_STREQ(json_as_string(json_get(ask_payload, "question")), "q?");
  json_value_t* options = json_get(ask_payload, "options");
  ASSERT_NE(options, nullptr);
  ASSERT_EQ(json_size(options), 2u);
  EXPECT_STREQ(json_as_string(json_at(options, 0)), "yes");
  EXPECT_STREQ(json_as_string(json_at(options, 1)), "no");
  json_value_t* plan_v = json_get(ask_payload, "plan");
  ASSERT_NE(plan_v, nullptr) << "the plan field is PRESENT (generic ask)";
  EXPECT_EQ(json_type(plan_v), JSON_NULL) << "plan null in this slice";

  json_value_t* turn_end_payload = json_get(turn_end_rec, "payload");
  ASSERT_NE(turn_end_payload, nullptr);
  json_value_t* reason = json_get(turn_end_payload, "reason");
  ASSERT_NE(reason, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "blocked")
      << "LIFE_REASON_BLOCKED's first writer";
  EXPECT_EQ(json_as_int(json_get(turn_end_payload, "turn")), 1);

  /* NO reply record and NO user-side append while parked. */
  EXPECT_EQ(fr_count_type(events, "ask.reply"), 0u);
  EXPECT_EQ(fr_count_type(events, "msg.append"), 0u);
  size_t parked_count = json_size(events);
  json_value_destroy(events);

  /* The engine PARKS: a second run loop pumps NOTHING (no replies, no new
     turn — the parked engine issues no turns of its own). */
  EXPECT_EQ(frame_run_loop(f), 2);
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(json_size(events), parked_count) << "the parked engine is quiet";
  json_value_destroy(events);

  /* THE REPLY: the post returns 0 (posted — never a commit confirm); the
     resolution composes as the reply batch [ask.reply, msg.append] and the
     fresh turn runs to done (the model capture pins the answer's ride). */
  EXPECT_EQ(frame_ask_reply(f, ask_id.c_str(), 0, "yes"), 0);
  EXPECT_EQ(frame_run_loop(f), 0) << "the reply resumed; the fresh turn ends";

  EXPECT_EQ(frame_is_done(f), 1);
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  /* The reply batch: the ask.reply record + the user-side msg.append, the
     SAME batch (two consecutive seqs). */
  json_value_t* reply_rec = json_at(events, ask_index + 2);
  json_value_t* append_rec = json_at(events, ask_index + 3);
  ASSERT_NE(append_rec, nullptr);
  ASSERT_TRUE(event_is(reply_rec, "ask.reply"));
  ASSERT_TRUE(event_is(append_rec, "msg.append"));
  EXPECT_EQ((long long)json_as_int(json_get(reply_rec, "seq")),
            base_seq + 4) << "the reply batch starts after the close";
  json_value_t* reply_payload = json_get(reply_rec, "payload");
  ASSERT_NE(reply_payload, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reply_payload, "askId")), ask_id.c_str());
  EXPECT_STREQ(json_as_string(json_get(reply_payload, "decision")), "answer");
  EXPECT_STREQ(json_as_string(json_get(reply_payload, "value")), "yes");
  json_value_t* append_payload = json_get(append_rec, "payload");
  ASSERT_NE(append_payload, nullptr);
  EXPECT_STREQ(json_as_string(json_get(append_payload, "role")), "user");
  EXPECT_STREQ(json_as_string(json_get(append_payload, "content")), "yes")
      << "the answer's durable user-side input";
  /* TWO msg.appends total: the answer + the fresh turn's assistant content
     (the finish batch writes it). */
  EXPECT_EQ(fr_count_type(events, "msg.append"), 2u);
  /* The fresh turn started, derived the answer, and closed completed. */
  std::string second = (cm.captured.size() > 1) ? cm.captured[1] : "";
  EXPECT_NE(second.find("\"yes\""), std::string::npos)
      << "turn 2's derive carried the answer";
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestAskStaleReplyIsDroppedLoud) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "stale reply probe", &cfg);
  ASSERT_NE(f, nullptr);

  ask_capture_model_t cm = {};
  cm.base.complete = ask_capture_complete;
  cm.replies = {
      canned_cell_body("import actor\nactor.ask('hold?', ['yes', 'no'])"
                       "\nprint('held')"),
      canned_content_body("answered late")};
  frame_set_model_backend(f, &cm.base);

  EXPECT_EQ(frame_run_loop(f), 2) << "parked";
  EXPECT_EQ(frame_is_done(f), 0);

  /* The minted id (read it from the parked ask's record). */
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  size_t parked_count = json_size(events);
  std::string ask_id;
  for (size_t i = json_size(events); i > 0; i--) {
    json_value_t* rec = json_at(events, i - 1);
    if (event_is(rec, "ask")) {
      ask_id = json_as_string(json_get(json_get(rec, "payload"), "askId"));
      break;
    }
  }
  ASSERT_EQ(ask_id.size(), 8u);
  json_value_destroy(events);

  /* The API's own bounds refuse BEFORE any post (no mailbox traffic). */
  EXPECT_LT(frame_ask_reply(f, "", 0, "yes"), 0) << "empty ask_id";
  EXPECT_LT(frame_ask_reply(NULL, ask_id.c_str(), 0, "yes"), 0);
  EXPECT_LT(frame_ask_reply(f, ask_id.c_str(), 5, "yes"), 0) << "decision 0|1";

  /* A WRONG id POSTS (that is the contract: the post's ack cannot see the
     engine) — the dispatch drops it LOUD, the park stands, nothing lands. */
  log_add_callback(_ask_stale_recorder, NULL, LOG_ERROR);
  int stale_before = _ask_stale_lines;
  EXPECT_EQ(frame_ask_reply(f, "00000000", 0, "yes"), 0) << "posted";
  EXPECT_EQ(frame_run_loop(f), 2) << "the park STANDS — nothing resumed";
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(json_size(events), parked_count) << "a stale reply commits NOTHING";
  EXPECT_EQ(fr_count_type(events, "ask.reply"), 0u);
  json_value_destroy(events);
  EXPECT_GT(_ask_stale_lines, stale_before)
      << "the stale reply was dropped LOUD (the async wire's contract)";

  /* The RIGHT id still works after the stale refusals. */
  EXPECT_EQ(frame_ask_reply(f, ask_id.c_str(), 0, "yes"), 0);
  EXPECT_EQ(frame_run_loop(f), 0) << "the matched reply resumed the engine";
  EXPECT_EQ(frame_is_done(f), 1);
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  json_value_t* reply = json_at(events, parked_count);
  ASSERT_NE(reply, nullptr);
  ASSERT_TRUE(event_is(reply, "ask.reply"));
  json_value_t* reply_payload = json_get(reply, "payload");
  ASSERT_NE(reply_payload, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reply_payload, "askId")), ask_id.c_str());
  EXPECT_EQ(fr_count_type(events, "ask.reply"), 1u) << "consumed EXACTLY ONCE";
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestAskSecondAskInSameTurnRefused) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "one ask at a time", &cfg);
  ASSERT_NE(f, nullptr);

  ask_capture_model_t cm = {};
  cm.base.complete = ask_capture_complete;
  cm.replies = {
      canned_cell_body("import actor\na = actor.ask('q1?', ['yes', 'no'])\n"
                       "b = actor.ask('q2?', ['yes', 'no'])\n"
                       "print(a + ' | ' + b)"),
      canned_content_body("done after one ask")};
  frame_set_model_backend(f, &cm.base);

  EXPECT_EQ(frame_run_loop(f), 2) << "parked on the FIRST ask only";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  /* ONE ask record — the second verb call already returned the refusal
     (the publish-time flag), and the cell's own print carries it. */
  EXPECT_EQ(fr_count_type(events, "ask"), 1u);
  json_value_t* ask_rec = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "ask")) {
      ask_rec = json_at(events, i);
      break;
    }
  }
  ASSERT_NE(ask_rec, nullptr);
  EXPECT_STREQ(json_as_string(json_get(json_get(ask_rec, "payload"),
                                       "question")), "q1?");
  for (size_t i = json_size(events); i > 0; i--) {
    json_value_t* rec = json_at(events, i - 1);
    if (!event_is(rec, "cell.result")) continue;
    std::string text = json_as_string(json_get(json_get(rec, "payload"), "text"));
    EXPECT_NE(text.find("asked | ask already parked"), std::string::npos)
        << "the SECOND ask was refused at the verb: '" << text << "'";
    break;
  }
  size_t parked_count = json_size(events);
  json_value_destroy(events);

  /* The dispatch-time belt: a hand-crafted ask while PARKED refuses loud
     (the verb's flag never fired here — this is the engine's own belt). */
  log_add_callback(_ask_belt_recorder, NULL, LOG_ERROR);
  int belt_before = _ask_belt_lines;
  frm_ask_payload_t* ap = (frm_ask_payload_t*)get_clear_memory(sizeof(*ap));
  ap->corr = 777;
  ap->question = strdup("late?");
  message_t m;
  m.type = (uint32_t)FRM_ASK;
  m.payload = ap;
  m.payload_destroy = frm_ask_payload_destroy;
  ASSERT_TRUE(actor_send(_frame_actor(f), &m));
  actor_run(_frame_actor(f), ACTOR_BATCH_SIZE);
  EXPECT_GT(_ask_belt_lines, belt_before) << "the belt refused LOUD";
  EXPECT_LT(frame_ask_reply(f, "beefbeef", 0, ""), 0)
      << "an ANSWER carries its text (reject alone may be empty)";
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(json_size(events), parked_count) << "the belt committed nothing";
  EXPECT_EQ(fr_count_type(events, "ask"), 1u);
  json_value_destroy(events);

  /* Then the reply still resumes the engine (the belt refused, not broke). */
  json_value_t* events2 = load_events(f);
  ASSERT_NE(events2, nullptr);
  std::string parked_ask_id;
  for (size_t i = json_size(events2); i > 0; i--) {
    json_value_t* rec = json_at(events2, i - 1);
    if (event_is(rec, "ask")) {
      parked_ask_id = json_as_string(json_get(json_get(rec, "payload"), "askId"));
      break;
    }
  }
  ASSERT_EQ(parked_ask_id.size(), 8u);
  json_value_destroy(events2);
  EXPECT_EQ(frame_ask_reply(f, parked_ask_id.c_str(), 0, "yes"), 0);
  EXPECT_EQ(frame_run_loop(f), 0);
  EXPECT_EQ(frame_is_done(f), 1);

  frame_destroy(f);
  wave_db_close(db);
}

/* The reply's compose/post refusal leaves the PARK STANDING for a retry
   (escalation spec §1.4's exactly-once contract): the park clears only after
   the batch POSTS. Force the real refusal at the reply path — a value
   beyond the WAL batch cap (SA_FRAME_MAX_BATCH_BYTES is 120 KiB in frame.c;
   the post's cap check refuses pre-post, the seq range rolls back, no
   record lands) — then the honest retry resumes the engine. */
TEST(TestFrame, TestAskReplyRefusedBatchKeepsTheParkedAskStanding) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "the refused reply's retry", &cfg);
  ASSERT_NE(f, nullptr);

  ask_capture_model_t cm = {};
  cm.base.complete = ask_capture_complete;
  cm.replies = {
      canned_cell_body("import actor\nactor.ask('q?', ['yes', 'no'])"
                       "\nprint('asked')"),
      canned_content_body("done after the answer")};
  frame_set_model_backend(f, &cm.base);

  EXPECT_EQ(frame_run_loop(f), 2) << "parked";
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  size_t parked_count = json_size(events);
  std::string ask_id;
  for (size_t i = json_size(events); i > 0; i--) {
    json_value_t* rec = json_at(events, i - 1);
    if (event_is(rec, "ask")) {
      ask_id = json_as_string(json_get(json_get(rec, "payload"), "askId"));
      break;
    }
  }
  ASSERT_EQ(ask_id.size(), 8u);
  json_value_destroy(events);

  /* The oversized reply POSTS (the internals post is unbounded by the
     bridge budget) and the dispatch's reply batch runs into the WAL
     record cap: the compose succeeds, the post refuses PRE-post, the seq
     range rolls back, and the park STANDS (no ask.reply record, the engine
     still parked — the same oversized reply could land again). */
  std::string oversized(150 * 1024, 'x');
  EXPECT_EQ(_frame_ask_reply_post(f, ask_id.c_str(), 0, oversized.c_str()), 0)
      << "posted (the ack cannot see the engine)";
  EXPECT_EQ(frame_run_loop(f), 2) << "the refusal kept the park standing";
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(json_size(events), parked_count)
      << "the refused reply batch committed NOTHING";
  EXPECT_EQ(fr_count_type(events, "ask.reply"), 0u);
  json_value_destroy(events);

  /* The retry lands: the matched reply composes its batch, the park clears,
     and the fresh turn runs to done. */
  EXPECT_EQ(frame_ask_reply(f, ask_id.c_str(), 0, "yes"), 0);
  EXPECT_EQ(frame_run_loop(f), 0) << "the retry resumed the engine";
  EXPECT_EQ(frame_is_done(f), 1);
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(fr_count_type(events, "ask.reply"), 1u);
  json_value_t* reply = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "ask.reply")) {
      reply = json_at(events, i);
      break;
    }
  }
  ASSERT_NE(reply, nullptr);
  ASSERT_TRUE(event_is(reply, "ask.reply"));
  json_value_t* reply_payload = json_get(reply, "payload");
  ASSERT_NE(reply_payload, nullptr);
  EXPECT_STREQ(json_as_string(json_get(reply_payload, "askId")), ask_id.c_str());
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

#endif /* python gate */

/* --- escalation Task 5: the plan-ask-act ladder (+ BYPASS) — PYTHON-FREE
   (the plan turn is a CONTENT turn: no cell ever runs; the act pins below
   ride content completions and the queue-empty failure; the REAL-cell pins
   — a real tool round in act, the ask verb's bypass refusal — live in
   test_loop.cpp's python-gated family). The family's harness: a content-
   and-tool-call scripted model that captures the messages AND the request's
   tools shape ("none-pointer" = the NULL pointer / the canned execute tool;
   "json-null" = the JSON null VALUE = explicitly no tools). --------------- */

static json_value_t* lf_payload_of(json_value_t* rec) {
  return json_get(rec, "payload");
}

static long long lf_seq_of(json_value_t* rec) {
  json_value_t* s = json_get(rec, "seq");
  return (s != NULL) ? (long long)json_as_int(s) : -1;
}

/* The LAST "ask" record whose question matches the ladder's gate needle
   (the log is append-only: the LATEST gate asks last). */
static json_value_t* lf_find_gate_ask(json_value_t* events) {
  json_value_t* found = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "ask")) continue;
    json_value_t* p = lf_payload_of(rec);
    json_value_t* q = (p != NULL) ? json_get(p, "question") : NULL;
    if (q != NULL &&
        strcmp(json_as_string(q), "Approve this plan?") == 0) {
      found = rec;
    }
  }
  return found;
}

/* The FIRST control record whose payload kind matches. */
static json_value_t* lf_find_control(json_value_t* events, const char* kind) {
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "control")) continue;
    json_value_t* p = lf_payload_of(rec);
    json_value_t* k = (p != NULL) ? json_get(p, "kind") : NULL;
    if (k != NULL && strcmp(json_as_string(k), kind) == 0) return rec;
  }
  return NULL;
}

typedef struct ladder_model_t {
  model_backend_t base;
  std::vector<std::string> replies;
  std::vector<std::string> captured;
  std::vector<std::string> tools_seen;
} ladder_model_t;

/* The tools-shape capture (the ladder's pin; the loop's tools argument is
   exactly ONE of the three shapes model.c's builder accepts). */
static void lf_capture_tools(ladder_model_t* lm, json_value_t* tools) {
  if (tools == NULL) {
    lm->tools_seen.emplace_back("none-pointer");
  } else if (json_type(tools) == JSON_NULL) {
    lm->tools_seen.emplace_back("json-null");
  } else {
    char* ts = json_serialize(tools);
    lm->tools_seen.emplace_back((ts != NULL) ? std::string(ts) : std::string());
    free(ts);
  }
}

/* The decode: content-or-tool_calls (test_loop.cpp's scripted_decode
   shape, local copy — the python-gated original is out of this family's
   region). */
static int lf_decode(const std::string& body, model_reply_t** reply_out,
                     char** error_out) {
  char* err = NULL;
  json_value_t* root = json_parse(body.c_str(), body.size(), &err);
  if (err != NULL) free(err);
  if (root == NULL) {
    *error_out = strdup("ladder model: body is not valid JSON");
    return -1;
  }
  json_value_t* choices = json_get(root, "choices");
  json_value_t* choice = (choices != NULL && json_type(choices) == JSON_ARRAY)
                             ? json_at(choices, 0) : NULL;
  json_value_t* message =
      (choice != NULL && json_type(choice) == JSON_OBJECT)
          ? json_get(choice, "message") : NULL;
  if (message == NULL) {
    json_value_destroy(root);
    *error_out = strdup("ladder model: no message in choices[0]");
    return -1;
  }
  model_reply_t* r = (model_reply_t*)get_clear_memory(sizeof(model_reply_t));
  json_value_t* content = json_get(message, "content");
  r->content = strdup((content != NULL && json_type(content) != JSON_NULL)
                          ? json_as_string(content) : "");
  json_value_t* calls = json_get(message, "tool_calls");
  if (calls != NULL && json_type(calls) == JSON_ARRAY && json_size(calls) > 0) {
    json_value_t* fn = json_get(json_at(calls, 0), "function");
    json_value_t* args = (fn != NULL) ? json_get(fn, "arguments") : NULL;
    json_value_t* parsed = NULL;
    if (args != NULL && json_type(args) == JSON_STRING) {
      char* aerr = NULL;
      const char* t = json_as_string(args);
      parsed = json_parse(t, strlen(t), &aerr);
      if (aerr != NULL) free(aerr);
      if (parsed == NULL) {
        json_value_destroy(root);
        model_reply_destroy(r);
        *error_out = strdup("ladder model: arguments string is not JSON");
        return -1;
      }
      args = parsed;
    }
    if (args != NULL && json_type(args) == JSON_OBJECT) {
      json_value_t* code = json_get(args, "code");
      if (code != NULL && json_type(code) == JSON_STRING) {
        r->tool_code = strdup(json_as_string(code));
      }
    }
    if (parsed != NULL) json_value_destroy(parsed);
  }
  json_value_destroy(root);
  *reply_out = r;
  return 0;
}

static int ladder_complete(void* self, json_value_t* messages,
                           json_value_t* tools, char** raw_out,
                           model_reply_t** reply_out, char** error_out) {
  (void)raw_out;
  *reply_out = NULL;
  *error_out = NULL;
  ladder_model_t* lm = (ladder_model_t*)self;
  char* seen = json_serialize(messages);
  if (seen != NULL) {
    lm->captured.emplace_back(seen);
    free(seen);
  }
  lf_capture_tools(lm, tools);
  if (lm->replies.empty()) {
    *error_out = strdup("ladder model: queue empty");
    return -1;
  }
  std::string body = lm->replies.front();
  lm->replies.erase(lm->replies.begin());
  return lf_decode(body, reply_out, error_out) == 0 ? 0 : -1;
}

/* One canned content-only completion body (canned_content_body's shape; no
   python needed — this family's replies decode content only). */
static std::string lf_content_body(const std::string& text) {
  return std::string(
             R"json({"choices":[{"message":{"role":"assistant","content":")json") +
             text + std::string(R"json("}}]})json");
}

/* One canned `execute` tool-call completion body (the canned shape — no
   python needed: this family never RUNS the cell, and the plan gate
   refuses the shape before any tool path). */
static std::string lf_tool_body(const std::string& code) {
  std::string inner = std::string("{\"code\":\"") + code + "\"}";
  std::string esc;
  for (char c : inner) {
    if (c == '"') esc += "\\\"";
    else esc += c;
  }
  return std::string(
      R"json({"choices":[{"message":{"role":"assistant","tool_calls":[)json"
      R"json({"type":"function","function":{"name":"execute",)json"
      R"json("arguments":")json") + esc +
      std::string(R"json("}}]}}]})json");
}

TEST(TestFrame, TestPlanAskActPlansToolsNullThenGatesThenActs) {
  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_PLAN_ASK_ACT;
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "ladder: plan then act", &cfg);
  ASSERT_NE(f, nullptr);

  ladder_model_t lm = {};
  lm.base.complete = ladder_complete;
  lm.replies = {
      lf_content_body("Plan: 1. measure 2. cut 3. report"),
      lf_content_body("act phase's first step")};
  frame_set_model_backend(f, &lm.base);

  /* TURN 1 = the plan turn: tools-null request, the plan block, the
     runtime-authored gate parking the frame. */
  EXPECT_EQ(frame_run_loop(f), 2) << "the plan turn parks at the gate";
  EXPECT_EQ(frame_is_done(f), 0);
  ASSERT_EQ(lm.captured.size(), 1u);
  ASSERT_EQ(lm.tools_seen.size(), 1u);
  EXPECT_EQ(lm.tools_seen[0], "json-null")
      << "the plan request is built tools-NULL (model.c's no-tools shape)";
  EXPECT_NE(lm.captured[0].find("PLAN mode"), std::string::npos)
      << "the plan block rode the system prompt";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  json_value_t* ask_rec = lf_find_gate_ask(events);
  ASSERT_NE(ask_rec, nullptr);
  size_t ask_index = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    if (json_at(events, i) == ask_rec) { ask_index = i; break; }
  }
  json_value_t* p = lf_payload_of(ask_rec);
  ASSERT_NE(p, nullptr);
  json_value_t* id = json_get(p, "askId");
  ASSERT_NE(id, nullptr);
  std::string ask_id = json_as_string(id);
  EXPECT_EQ(ask_id.size(), 8u) << "the root allocator's mint";
  EXPECT_STREQ(json_as_string(json_get(p, "question")), "Approve this plan?");
  json_value_t* options = json_get(p, "options");
  ASSERT_NE(options, nullptr);
  ASSERT_EQ(json_size(options), 2u);
  EXPECT_STREQ(json_as_string(json_at(options, 0)), "Approve");
  EXPECT_STREQ(json_as_string(json_at(options, 1)), "Reject");
  json_value_t* plan_v = json_get(p, "plan");
  ASSERT_NE(plan_v, nullptr);
  EXPECT_EQ(json_type(plan_v), JSON_STRING);
  EXPECT_STREQ(json_as_string(plan_v), "Plan: 1. measure 2. cut 3. report")
      << "the plan text rides the ask record below the cap";

  /* The close batch: [step.start, control plan-requested, msg.append (the
     plan text), ask, step.end, turn.end{blocked}] — SIX consecutive seqs
     (the turn.start rode its own turn-entry batch, so the ask sits at the
     batch's fourth position). */
  json_value_t* rec = json_at(events, ask_index - 3);
  ASSERT_NE(rec, nullptr);
  ASSERT_TRUE(event_is(rec, "step.start")) << "the batch's first record";
  for (long long k = 0; k <= 5; k++) {
    json_value_t* at = json_at(events, ask_index - 3 + (size_t)k);
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(lf_seq_of(at), lf_seq_of(rec) + k)
        << "the close batch is ONE atomic group; record " << k;
  }
  json_value_t* ctrl_rec = json_at(events, ask_index - 2);
  ASSERT_TRUE(event_is(ctrl_rec, "control"));
  EXPECT_STREQ(json_as_string(json_get(lf_payload_of(ctrl_rec), "kind")),
               "plan-requested");
  json_value_t* plan_append = json_at(events, ask_index - 1);
  ASSERT_TRUE(event_is(plan_append, "msg.append"));
  EXPECT_STREQ(json_as_string(json_get(lf_payload_of(plan_append), "role")),
               "assistant");
  json_value_t* turn_end_rec = json_at(events, ask_index + 2);
  ASSERT_TRUE(event_is(turn_end_rec, "turn.end"));
  {
    json_value_t* tp = lf_payload_of(turn_end_rec);
    json_value_t* reason = json_get(tp, "reason");
    ASSERT_NE(reason, nullptr);
    EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "blocked");
    EXPECT_EQ(json_as_int(json_get(tp, "turn")), 1);
  }
  EXPECT_EQ(fr_count_type(events, "msg.append"), 1u)
      << "only the assistant plan text — no user-side append while parked";
  json_value_destroy(events);

  /* THE APPROVE (decision 0, value "Approve"): the reply batch ALSO carries
     the durable control record — [ask.reply, control auto:false,
     msg.append user] — and the ACT turn runs (tools again, no plan block),
     its content completing the frame normally. */
  EXPECT_EQ(frame_ask_reply(f, ask_id.c_str(), 0, "Approve"), 0);
  EXPECT_EQ(frame_run_loop(f), 0) << "the act phase ran to completion";
  EXPECT_EQ(frame_is_done(f), 1);
  ASSERT_EQ(lm.captured.size(), 2u);
  ASSERT_EQ(lm.tools_seen.size(), 2u);
  EXPECT_EQ(lm.tools_seen[1], "none-pointer")
      << "the act turn's request carries `execute` again";
  EXPECT_EQ(lm.captured[1].find("PLAN mode"), std::string::npos)
      << "the plan block left the derive at the approval";
  EXPECT_NE(lm.captured[1].find("Plan: 1. measure"), std::string::npos)
      << "the plan text reached the derive through the durable msg.append";

  events = load_events(f);
  ASSERT_NE(events, nullptr);
  {
    /* The reply batch's THREE records: ask.reply, control{plan-approved,
       auto false}, msg.append user — consecutive seqs IN THAT ORDER. */
    json_value_t* approved = lf_find_control(events, "plan-approved");
    ASSERT_NE(approved, nullptr);
    size_t a = 0;
    for (size_t i = 0; i < json_size(events); i++) {
      if (json_at(events, i) == approved) { a = i; break; }
    }
    json_value_t* reply_r = json_at(events, a - 1);
    json_value_t* append_r = json_at(events, a + 1);
    ASSERT_TRUE(event_is(reply_r, "ask.reply"));
    ASSERT_TRUE(event_is(append_r, "msg.append"));
    EXPECT_EQ(lf_seq_of(reply_r) + 1, lf_seq_of(approved));
    EXPECT_EQ(lf_seq_of(approved) + 1, lf_seq_of(append_r));
    json_value_t* ap = lf_payload_of(approved);
    EXPECT_EQ(json_type(json_get(ap, "auto")), JSON_BOOL);
    EXPECT_EQ(json_as_bool(json_get(ap, "auto")), 0)
        << "owner-approved, not AUTO";
    json_value_t* rp = lf_payload_of(reply_r);
    EXPECT_STREQ(json_as_string(json_get(rp, "decision")), "answer");
    json_value_t* up = lf_payload_of(append_r);
    EXPECT_STREQ(json_as_string(json_get(up, "role")), "user");
    EXPECT_STREQ(json_as_string(json_get(up, "content")), "Approve");
    EXPECT_EQ(fr_count_type(events, "ask"), 1u) << "no re-ask in act";
  }
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestPlanGateRejectReplansAndReplanCarriesTheText) {
  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_PLAN_ASK_ACT;
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "ladder: replan", &cfg);
  ASSERT_NE(f, nullptr);

  ladder_model_t lm = {};
  lm.base.complete = ladder_complete;
  lm.replies = {lf_content_body("Plan one: measure"), lf_content_body("Plan two: cut"), lf_content_body("Plan three: report")};
  frame_set_model_backend(f, &lm.base);

  EXPECT_EQ(frame_run_loop(f), 2);
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  json_value_t* ask1 = lf_find_gate_ask(events);
  ASSERT_NE(ask1, nullptr);
  std::string id1 = json_as_string(json_get(lf_payload_of(ask1), "askId"));
  json_value_destroy(events);

  /* REJECT with no text: the standing default wording; NO control record
     (the ladder revisits plan — no transition happened). */
  EXPECT_EQ(frame_ask_reply(f, id1.c_str(), 1, ""), 0);
  EXPECT_EQ(frame_run_loop(f), 2) << "the next turn is plan AGAIN";

  events = load_events(f);
  ASSERT_NE(events, nullptr);
  {
    json_value_t* reply = NULL;
    size_t reply_index = 0;
    for (size_t i = 0; i < json_size(events); i++) {
      if (event_is(json_at(events, i), "ask.reply")) {
        reply = json_at(events, i);
        reply_index = i;
        break;
      }
    }
    ASSERT_NE(reply, nullptr);
    json_value_t* rp = lf_payload_of(reply);
    EXPECT_STREQ(json_as_string(json_get(rp, "decision")), "reject");
    json_value_t* text_v = json_get(rp, "value");
    ASSERT_NE(text_v, nullptr) << "the empty reject renders value null";
    EXPECT_EQ(json_type(text_v), JSON_NULL);
    EXPECT_EQ(json_as_string(text_v), nullptr)
        << "json_as_string on a null value answers NULL (the standing shape)";
    /* The SAME batch's user-side append rides NEXT (ask.reply, then
       msg.append — the two-record reject shape). */
    json_value_t* append = json_at(events, reply_index + 1);
    ASSERT_NE(append, nullptr);
    ASSERT_TRUE(event_is(append, "msg.append"));
    json_value_t* ap = lf_payload_of(append);
    EXPECT_STREQ(json_as_string(json_get(ap, "role")), "user");
    EXPECT_STREQ(json_as_string(json_get(ap, "content")),
                 "Plan rejected: revise and re-propose")
        << "the standing default wording (no objection text)";
    EXPECT_EQ(lf_find_control(events, "plan-approved"), nullptr)
        << "no approval control record on a reject";
  }
  EXPECT_EQ(_frame_engine_state(f)->ladder_act, 0u)
      << "a reject never flips the ladder's ACT marker (the next turn is "
         "plan again)";
  json_value_destroy(events);

  /* The replan turn ran tools-NULL and carried the plan block again
     (turn 2 is plan). */
  ASSERT_EQ(lm.tools_seen.size(), 2u);
  EXPECT_EQ(lm.tools_seen[1], "json-null");
  ASSERT_EQ(lm.captured.size(), 2u);
  EXPECT_NE(lm.captured[1].find("PLAN mode"), std::string::npos);
  EXPECT_NE(lm.captured[1].find("Plan one: measure"), std::string::npos)
      << "the prior plan text rode the durable msg.append into the replan";

  /* REJECT WITH text: the objection IS the record's content, verbatim. */
  json_value_t* events2 = load_events(f);
  ASSERT_NE(events2, nullptr);
  json_value_t* ask2 = lf_find_gate_ask(events2);
  ASSERT_NE(ask2, nullptr);
  std::string id2 = json_as_string(json_get(lf_payload_of(ask2), "askId"));
  EXPECT_NE(id2, id1) << "a fresh mint per gate";
  json_value_destroy(events2);
  EXPECT_EQ(frame_ask_reply(f, id2.c_str(), 1, "skip step 3"), 0);
  EXPECT_EQ(frame_run_loop(f), 2) << "still plan after the second reject";

  events = load_events(f);
  ASSERT_NE(events, nullptr);
  {
    size_t asks = 0;
    for (size_t i = 0; i < json_size(events); i++) {
      if (event_is(json_at(events, i), "ask")) asks++;
    }
    EXPECT_EQ(asks, 3u) << "three gated plan turns";
    json_value_t* last_user = NULL;
    for (size_t i = 0; i < json_size(events); i++) {
      json_value_t* r = json_at(events, i);
      if (!event_is(r, "msg.append")) continue;
      json_value_t* ap = lf_payload_of(r);
      if (strcmp(json_as_string(json_get(ap, "role")), "user") == 0) {
        last_user = r;
      }
    }
    ASSERT_NE(last_user, nullptr);
    json_value_t* ap = lf_payload_of(last_user);
    EXPECT_STREQ(json_as_string(json_get(ap, "content")), "skip step 3")
        << "the objection rides VERBATIM";
    EXPECT_EQ(lf_find_control(events, "plan-approved"), nullptr)
        << "still no approval: three gates, zero transitions";
  }
  json_value_destroy(events);
  /* The third plan turn's derive carried the whole replan history. */
  ASSERT_EQ(lm.captured.size(), 3u);
  ASSERT_EQ(lm.tools_seen.size(), 3u);
  EXPECT_EQ(lm.tools_seen[2], "json-null");
  EXPECT_NE(lm.captured[2].find("Plan one: measure"), std::string::npos);
  EXPECT_NE(lm.captured[2].find("Plan two: cut"), std::string::npos);
  EXPECT_EQ(lm.captured[2].find("Plan three: report"), std::string::npos)
      << "the model's own fresh reply is not in its own derive";

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestPlanModeToolCallReplyRefusesLoud) {
  /* The plan turn ran tools-NULL — a tool-call reply is an unexpected
     shape there: LOUD ("plan-mode"), turn ends error, gate-free state
     (a later run re-enters plan). No cell ever ran. */
  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_PLAN_ASK_ACT;
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "ladder: bad shape", &cfg);
  ASSERT_NE(f, nullptr);

  ladder_model_t lm = {};
  lm.base.complete = ladder_complete;
  lm.replies = {lf_tool_body("print('ignored')")};
  frame_set_model_backend(f, &lm.base);

  EXPECT_EQ(frame_run_loop(f), 1) << "the plan-mode refusal failed loud";
  ASSERT_EQ(lm.tools_seen.size(), 1u);
  EXPECT_EQ(lm.tools_seen[0], "json-null")
      << "the refusal's turn RAN the tools-shaped request too";
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(lf_find_gate_ask(events), nullptr)
      << "the unexpected reply never gated";
  json_value_t* turn_end = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "turn.end")) {
      turn_end = json_at(events, i);
    }
  }
  ASSERT_NE(turn_end, nullptr);
  {
    json_value_t* tp = lf_payload_of(turn_end);
    json_value_t* reason = json_get(tp, "reason");
    ASSERT_NE(reason, nullptr);
    EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "error");
  }
  ASSERT_NE(lf_find_control(events, "plan-mode"), nullptr)
      << "the refusal's control record rides the close";
  json_value_destroy(events);
  /* The gate-free state: a re-run re-enters plan (queue empty now → the
     fallback's once-only retry exhausts → still failed loud). */
  EXPECT_EQ(frame_run_loop(f), 1);
  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestPlanModeEmptyPlanTurnFailsLoud) {
  /* THE EMPTY PLAN'S LOUD FAILURE (escalation spec §2.2): a plan turn's
     reply text IS the plan — an EMPTY reply has nothing for the owner to
     approve. The old empty-turn degrade path (which would gate on a
     plan:null, and the bypass's close would auto-approve one even) is gone:
     the turn fails loud ("plan-mode"), NO ask record is composed, and the
     loop resumes — the next turn is STILL plan and gates normally. */
  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_PLAN_ASK_ACT;
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "ladder: empty plan", &cfg);
  ASSERT_NE(f, nullptr);

  ladder_model_t lm = {};
  lm.base.complete = ladder_complete;
  lm.replies = {lf_content_body(""), lf_content_body("Plan: the retried plan")};
  frame_set_model_backend(f, &lm.base);

  EXPECT_EQ(frame_run_loop(f), 1) << "the empty plan turn failed loud";
  EXPECT_EQ(frame_is_done(f), 0);
  ASSERT_EQ(lm.tools_seen.size(), 1u);
  EXPECT_EQ(lm.tools_seen[0], "json-null")
      << "the empty plan's turn ran the plan-shaped request";
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(fr_count_type(events, "ask"), 0u)
      << "no ask record — nothing to approve";
  EXPECT_EQ(lf_find_gate_ask(events), nullptr)
      << "the gate never fired on the empty plan";
  EXPECT_EQ(lf_find_control(events, "plan-requested"), nullptr)
      << "no phase transition was written";
  EXPECT_EQ(lf_find_control(events, "plan-approved"), nullptr);
  json_value_t* failure = lf_find_control(events, "plan-mode");
  ASSERT_NE(failure, nullptr) << "the failure's control rides its close";
  {
    json_value_t* fp = lf_payload_of(failure);
    json_value_t* text_v = json_get(fp, "text");
    ASSERT_NE(text_v, nullptr);
    EXPECT_STREQ(json_as_string(text_v), "the model returned an empty plan");
  }
  json_value_t* turn_end = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "turn.end")) {
      turn_end = json_at(events, i);
    }
  }
  ASSERT_NE(turn_end, nullptr);
  {
    json_value_t* tp = lf_payload_of(turn_end);
    json_value_t* reason = json_get(tp, "reason");
    ASSERT_NE(reason, nullptr);
    EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "error");
  }
  json_value_destroy(events);

  /* The loop resumed: the NEXT turn is STILL plan and it gates normally
     (the retried plan content closes at the gate). */
  EXPECT_EQ(frame_run_loop(f), 2) << "the next turn is STILL plan";
  ASSERT_EQ(lm.tools_seen.size(), 2u);
  EXPECT_EQ(lm.tools_seen[1], "json-null");
  EXPECT_NE(lm.captured[1].find("PLAN mode"), std::string::npos);
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  json_value_t* gate = lf_find_gate_ask(events);
  ASSERT_NE(gate, nullptr) << "the retried plan gated normally";
  json_value_t* plan_v = json_get(lf_payload_of(gate), "plan");
  ASSERT_NE(plan_v, nullptr);
  EXPECT_EQ(json_type(plan_v), JSON_STRING);
  EXPECT_STREQ(json_as_string(plan_v), "Plan: the retried plan");
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestApprovalPersistsAcrossRestart) {
  /* The approval consult (escalation spec §2.4): the durable control record
     is the approval's mechanism — the approve lands, the act turn FAILS
     loud (this python-free shape: the act turn's model queue is empty —
     the fallback class's once-only retry exhausts; the frame stays NOT
     done), the frame is destroyed, and the resumed run's FIRST derive
     re-learns ACT from the log's plan-approved record: no re-gate, no
     re-ask, and the act request is built TOOLS-shaped.
     NOTE (the recorded safe default): the derive window is bounded to the
     newest 512 records — a very long log may scroll the approval past it,
     and a re-gate (re-asking) is the SAFE default there. */
  char tmpl[] = "/tmp/sa-ladder-XXXXXX";
  char* got = mkdtemp(tmpl);
  ASSERT_NE(got, nullptr);
  std::string dir = std::string(got);
  std::string loc = dir + "/db";

  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_PLAN_ASK_ACT;
  wave_database_root_t* db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "durable approval", &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);

  ladder_model_t lm = {};
  lm.base.complete = ladder_complete;
  lm.replies = {lf_content_body("Plan: durable edition")};
  frame_set_model_backend(f, &lm.base);

  EXPECT_EQ(frame_run_loop(f), 2);
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  json_value_t* ask_rec = lf_find_gate_ask(events);
  ASSERT_NE(ask_rec, nullptr);
  std::string ask_id =
      json_as_string(json_get(lf_payload_of(ask_rec), "askId"));
  json_value_destroy(events);
  EXPECT_EQ(frame_ask_reply(f, ask_id.c_str(), 0, "Approve"), 0);
  /* The act turn fails loud twice (the queue is empty; the fallback class
     retries once) — the approval's control record is already durable. */
  EXPECT_EQ(frame_run_loop(f), 1);
  EXPECT_EQ(frame_is_done(f), 0);

  frame_destroy(f);
  wave_db_close(db);

  /* RE-OPEN + RESUME: the derive's consult sees the control record. */
  db = wave_db_open(loc.c_str());
  ASSERT_NE(db, nullptr);
  frame_t* resumed = frame_resume(db, sid.c_str(), &cfg);
  ASSERT_NE(resumed, nullptr);
  ladder_model_t lm2 = {};
  lm2.base.complete = ladder_complete;
  lm2.replies = {lf_content_body("the act phase's resumed step")};
  frame_set_model_backend(resumed, &lm2.base);

  EXPECT_EQ(frame_run_loop(resumed), 0);
  ASSERT_EQ(lm2.captured.size(), 1u);
  ASSERT_EQ(lm2.tools_seen.size(), 1u);
  EXPECT_EQ(lm2.tools_seen[0], "none-pointer")
      << "the resumed run consulted the log: this turn is ACT (tools)";
  EXPECT_EQ(lm2.captured[0].find("PLAN mode"), std::string::npos)
      << "no plan block rides an act turn's prompt";
  events = load_events(resumed);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(fr_count_type(events, "ask"), 1u)
      << "NO re-ask: the approval carried the restart";
  json_value_destroy(events);
  EXPECT_EQ(frame_is_done(resumed), 1) << "the act content ended the frame";

  frame_destroy(resumed);
  wave_db_close(db);
  std::filesystem::remove_all(dir);
}

TEST(TestFrame, TestBypassPlansThenAutoApprovesWithoutAsking) {
  /* DANGEROUS mode (escalation spec §2.3): the plan turn still runs and is
     still logged, but its close AUTO-APPROVES — the durable control record
     {plan-approved, auto true} INSTEAD of any ask record; NO park; the
     engine CONTINUES (the next turn runs, tools = execute). The owner
     surface is never involved (the flow's own ask-verb refusal lives in
     test_loop.cpp's python-gated family). */
  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_BYPASS;
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "bypass: no gate", &cfg);
  ASSERT_NE(f, nullptr);

  ladder_model_t lm = {};
  lm.base.complete = ladder_complete;
  lm.replies = {
      lf_content_body("Plan: automatically approved edition"),
      lf_content_body("the act phase ran without asking")};
  frame_set_model_backend(f, &lm.base);

  /* ONE run loop drives the whole continuation: plan → auto-approve →
     act → done. NEVER rc 2 (no park exists in bypass). But first, the
     in-memory flip is observed at the plan turn's end (the engine is live
     at phase NONE with the continuation queued, its ACT marker set — the
     bypass's own continuation idiom) via the by-hand pump. */
  EXPECT_EQ(frame_start(f), 0);
  for (int i = 0; i < 64 && lm.captured.size() < 1; i++) {
    _frame_pump(f);
  }
  ASSERT_EQ(lm.captured.size(), 1u);
  {
    frame_engine_state_t* e = _frame_engine_state(f);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->engine_live, 1u) << "the plan close did NOT end the engine";
    EXPECT_EQ(e->ladder_act, 1u)
        << "the auto-approval flipped the ladder's ACT marker in memory";
  }
  /* The continuation completes the flow normally. */
  EXPECT_EQ(frame_run_loop(f), 0);
  EXPECT_EQ(frame_is_done(f), 1);
  ASSERT_EQ(lm.captured.size(), 2u);
  ASSERT_EQ(lm.tools_seen.size(), 2u);
  EXPECT_EQ(lm.tools_seen[0], "json-null")
      << "the plan turn ran (tools-null, the plan block: the same shape)";
  EXPECT_NE(lm.captured[0].find("PLAN mode"), std::string::npos);
  EXPECT_EQ(lm.tools_seen[1], "none-pointer")
      << "the flow CONTINUED — the next turn ran with tools";
  EXPECT_EQ(lm.captured[1].find("PLAN mode"), std::string::npos);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(lf_find_gate_ask(events), nullptr)
      << "NO ask record — the bypass never asks";
  json_value_t* approved = lf_find_control(events, "plan-approved");
  ASSERT_NE(approved, nullptr);
  {
    json_value_t* ap = lf_payload_of(approved);
    ASSERT_NE(ap, nullptr);
    json_value_t* a = json_get(ap, "auto");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(json_type(a), JSON_BOOL);
    EXPECT_EQ(json_as_bool(a), 1) << "the bypass approval is AUTO";
  }
  EXPECT_EQ(fr_count_type(events, "msg.append"), 2u)
      << "the plan text (durable artifact) + the act content";
  EXPECT_EQ(fr_count_type(events, "turn.end"), 2u);
  json_value_t* first_end = NULL;
  size_t turn_count = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "turn.end")) {
      if (turn_count++ == 0) first_end = json_at(events, i);
    }
  }
  ASSERT_NE(first_end, nullptr);
  EXPECT_EQ(turn_count, 2u);
  {
    json_value_t* tp = lf_payload_of(first_end);
    json_value_t* reason = json_get(tp, "reason");
    ASSERT_NE(reason, nullptr);
    EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "completed")
        << "the plan turn still closes its own envelope (completed)";
  }
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestPlanGateBeltRefusesAStandingParkedAsk) {
  /* THE DOUBLE-BOXING BELT (loop.c's `_loop_plan_gate_post` pre-box check):
     the gate never boxes over a standing park — one ask at a time. The belt
     is UNREACHABLE by the honest flow (a parked engine re-enters no turns —
     the phase guards; only a race could stand a second park), so this pin
     FAULT-INJECTS the standing park: a hand-crafted `pending_ask.ask_id` on
     the still-dead engine, before frame_start — exactly the raced-shape the
     belt exists for. The verdict: the gate fires at the plan turn's close
     and REFUSES — the fail close (control {plan-mode, "the plan gate found
     a standing ask"} + turn.end{error}), ZERO ask records (the gate never
     boxed over it — no park state ever reached the records), and the forged
     string dies with the engine's end (the ask-clear funnel; valgrind
     proves the single free). */
  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_PLAN_ASK_ACT;
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "belt: a forged standing ask", &cfg);
  ASSERT_NE(f, nullptr);

  ladder_model_t lm = {};
  lm.base.complete = ladder_complete;
  lm.replies = {
      lf_content_body("Plan: 1. forge 2. refuse 3. resume"),
      lf_content_body("Plan: the honest plan after the belt"),
      lf_content_body("the act phase ran after the belt")};
  frame_set_model_backend(f, &lm.base);

  frame_engine_state_t* e = _frame_engine_state(f);
  ASSERT_NE(e, nullptr);
  e->pending_ask.ask_id = strdup("aa00beef");   /* THE FORGE: the engine
                                                   owns it from here (the
                                                   end-funnels clear it —
                                                   never a test free) */
  EXPECT_EQ(frame_run_loop(f), 1) << "the belt failed the turn loud";
  EXPECT_EQ(frame_is_done(f), 0)
      << "a failed terminate never ends a top frame's status";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(fr_count_type(events, "ask"), 0u)
      << "the gate refused to box over the standing park";
  EXPECT_EQ(fr_count_type(events, "ask.reply"), 0u);
  json_value_t* failure = lf_find_control(events, "plan-mode");
  ASSERT_NE(failure, nullptr) << "the belt's loud verdict";
  {
    json_value_t* fp = lf_payload_of(failure);
    ASSERT_NE(fp, nullptr);
    json_value_t* text_v = json_get(fp, "text");
    ASSERT_NE(text_v, nullptr);
    EXPECT_STREQ(json_as_string(text_v), "the plan gate found a standing ask");
  }
  json_value_t* turn_end = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "turn.end")) {
      turn_end = json_at(events, i);
    }
  }
  ASSERT_NE(turn_end, nullptr);
  {
    json_value_t* tp = lf_payload_of(turn_end);
    json_value_t* reason = json_get(tp, "reason");
    ASSERT_NE(reason, nullptr);
    EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "error");
  }
  /* The standing park stayed UNTOUCHED in the records: no phase transition
     rode the turn (this is NOT the gate's close — it never composed). */
  EXPECT_EQ(lf_find_control(events, "plan-requested"), nullptr);
  EXPECT_EQ(lf_find_control(events, "plan-approved"), nullptr);
  EXPECT_EQ(fr_count_type(events, "msg.append"), 0u)
      << "the plan text never durable-committed (the gate never reached its "
         "own close batch)";
  EXPECT_EQ(fr_count_type(events, "turn.start"), 1u);
  EXPECT_EQ(fr_count_type(events, "turn.end"), 1u);
  json_value_destroy(events);

  /* RESUMABLE: the belt refused and did not break — the next run re-enters
     plan and gates HONESTLY (the real gate ask stands, the forge is gone):
     the forged id is not the parked one, and the ladder completes. */
  EXPECT_EQ(frame_run_loop(f), 2) << "the next run is STILL plan";
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  json_value_t* gate = lf_find_gate_ask(events);
  ASSERT_NE(gate, nullptr) << "the retried plan's turn gated normally";
  {
    json_value_t* gp = lf_payload_of(gate);
    json_value_t* id = json_get(gp, "askId");
    ASSERT_NE(id, nullptr);
    std::string real_id = json_as_string(id);
    EXPECT_EQ(real_id.size(), 8u);
    EXPECT_NE(real_id, "aa00beef") << "the gate minted a FRESH id";
    json_value_destroy(events);
    /* The approve consumes and the act turn runs to done. */
    EXPECT_EQ(frame_ask_reply(f, real_id.c_str(), 0, "approve"), 0);
  }
  EXPECT_EQ(frame_run_loop(f), 0) << "the ladder completed after the belt";
  EXPECT_EQ(frame_is_done(f), 1);
  ASSERT_EQ(lm.captured.size(), 3u);
  ASSERT_EQ(lm.tools_seen.size(), 3u);
  EXPECT_EQ(lm.tools_seen[2], "none-pointer")
      << "the approved act turn ran with tools";

  frame_destroy(f);
  wave_db_close(db);
}

/* --- the model's clarifying ask MID-PLAN (escalation spec §2.2) ----------
   A python-gated corner of the ladder family: the plan turn's ONE allowed
   tool call is the model's ask — a REAL cell runs it through the frame's
   own pyrt (the standing ask family's shape, this plan phase's pin). */
#if defined(SA_HAS_PYTHON)

extern "C" void py_agent_init(void);   /* idempotent; re-mounts the bridge sink */

TEST(TestFrame, TestModelAskDuringPlanStaysPlanAfterTheReply) {
  /* PLAN_ASK_ACT; turn 1's model turn replies a tool call whose REAL cell
     calls actor.ask("which storage?", ['wave','onyx']) — the model's
     clarifying question mid-plan: the ask verb is the ONE tool call a plan
     turn allows, and the Task-2 flow parks the turn with the CELL's ask
     record — NOT the plan gate's. THE OBSERVABLE SHAPE difference (the
     review's pin): the model ask's record carries plan JSON NULL while the
     gate's carries the turn's plan text, and the parked ask's bookkeeping
     distinguishes them the same way (the cell's bridge corr NONZERO +
     plan_gate 0 — the gate's is corr 0 / plan_gate 1). The answer "wave"
     rides [ask.reply + msg.append] with NO plan-approved control record
     written ANYWHERE (the gate hasn't fired), and the NEXT model request is
     STILL plan-shaped (tools json-null + the plan block) — that turn's own
     content closes at the gate, whose ask NOW rides the plan text. */
  py_agent_init();
  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_PLAN_ASK_ACT;
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "mid-plan clarification", &cfg);
  ASSERT_NE(f, nullptr);

  ladder_model_t lm = {};
  lm.base.complete = ladder_complete;
  /* The ask cell rides canned_cell_body (the python-gated family's builder:
     it escapes the script's newlines — lf_tool_body escapes quotes only
     enough for its one-line standing pin). */
  lm.replies = {
      canned_cell_body("import actor\nactor.ask('which storage?', "
                       "['wave', 'onyx'])\nprint('asked')"),
      lf_content_body("Plan: store on wave, mirror on onyx")};
  frame_set_model_backend(f, &lm.base);

  /* Turn 1 (plan): the model's ask cell RAN (a real cell), and the turn
     parked on the cell's ask. */
  EXPECT_EQ(frame_run_loop(f), 2) << "the model's ask parked the plan turn";
  EXPECT_EQ(frame_is_done(f), 0);
  ASSERT_EQ(lm.captured.size(), 1u);
  ASSERT_EQ(lm.tools_seen.size(), 1u);
  EXPECT_EQ(lm.tools_seen[0], "json-null")
      << "the plan request stays tools-null across the model ask too";

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);

  /* THE MODEL ASK'S RECORD: question + options are the CELL's; the plan
     field is present and JSON NULL (the gate's record rides the plan
     text — the observable distinction). */
  json_value_t* ask_rec = NULL;
  size_t ask_index = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (!event_is(rec, "ask")) continue;
    json_value_t* q = json_get(lf_payload_of(rec), "question");
    ASSERT_NE(q, nullptr);
    if (strcmp(json_as_string(q), "which storage?") == 0) {
      ASSERT_EQ(ask_rec, nullptr) << "the one model ask";
      ask_rec = rec;
      ask_index = i;
    }
  }
  ASSERT_NE(ask_rec, nullptr) << "the cell's ask was parked";
  std::string ask_id;
  {
    json_value_t* p = lf_payload_of(ask_rec);
    json_value_t* id = json_get(p, "askId");
    ASSERT_NE(id, nullptr);
    ask_id = json_as_string(id);
    EXPECT_EQ(ask_id.size(), 8u);
    json_value_t* options = json_get(p, "options");
    ASSERT_NE(options, nullptr);
    ASSERT_EQ(json_size(options), 2u);
    EXPECT_STREQ(json_as_string(json_at(options, 0)), "wave");
    EXPECT_STREQ(json_as_string(json_at(options, 1)), "onyx");
    json_value_t* plan_v = json_get(p, "plan");
    ASSERT_NE(plan_v, nullptr) << "the plan field is PRESENT (a model ask too)";
    EXPECT_EQ(json_type(plan_v), JSON_NULL)
        << "THE SHAPE: a model ask's record carries plan NULL — the plan "
           "gate's rides the turn's plan text";
  }
  {
    frame_engine_state_t* e = _frame_engine_state(f);
    ASSERT_NE(e, nullptr);
    EXPECT_NE(e->pending_ask.corr, 0ull)
        << "the CELL's bridge corr stands (the gate's park is corr 0)";
    EXPECT_EQ(e->pending_ask.plan_gate, 0u)
        << "the gate has not fired — this park is a model ask's";
  }

  /* The close batch: cell.result + step.end + ask + turn.end{blocked} —
     FOUR consecutive seqs (the Task-2 shape, plan turn or free turn). */
  {
    ASSERT_GE(ask_index, 2u) << "cell.result and step.end precede it";
    long long base_seq = lf_seq_of(json_at(events, ask_index - 2));
    for (long long k = 0; k <= 2; k++) {
      json_value_t* at = json_at(events, ask_index - 2 + (size_t)k);
      ASSERT_NE(at, nullptr);
      EXPECT_EQ(lf_seq_of(at), base_seq + k)
          << "the ask close is ONE atomic group; record " << k;
    }
    EXPECT_TRUE(event_is(json_at(events, ask_index - 2), "cell.result"));
    EXPECT_TRUE(event_is(json_at(events, ask_index - 1), "step.end"));
    json_value_t* turn_end_rec = json_at(events, ask_index + 1);
    ASSERT_NE(turn_end_rec, nullptr);
    ASSERT_TRUE(event_is(turn_end_rec, "turn.end"));
    json_value_t* tp = lf_payload_of(turn_end_rec);
    json_value_t* reason = json_get(tp, "reason");
    ASSERT_NE(reason, nullptr);
    EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "blocked");
  }

  /* NO gate machinery anywhere: the question is the model's, the gate has
     not fired. */
  EXPECT_EQ(lf_find_gate_ask(events), nullptr)
      << "no runtime-authored gate ask in this turn";
  EXPECT_EQ(lf_find_control(events, "plan-requested"), nullptr);
  EXPECT_EQ(lf_find_control(events, "plan-approved"), nullptr);
  EXPECT_EQ(lf_find_control(events, "plan-mode"), nullptr)
      << "the ask cell is the ANSWERED shape, not the plan-mode refusal";
  json_value_destroy(events);

  /* THE ANSWER: the ride batch [ask.reply, msg.append user "wave"] — then a
     fresh turn that is STILL plan (no plan-approved control was ever
     written), whose plan content closes at the GATE. */
  EXPECT_EQ(frame_ask_reply(f, ask_id.c_str(), 0, "wave"), 0);
  EXPECT_EQ(frame_run_loop(f), 2) << "the next turn is STILL plan (it gated)";
  ASSERT_EQ(lm.captured.size(), 2u);
  ASSERT_EQ(lm.tools_seen.size(), 2u);
  EXPECT_EQ(lm.tools_seen[1], "json-null")
      << "the next request is STILL tools-null";
  EXPECT_NE(lm.captured[1].find("PLAN mode"), std::string::npos)
      << "the plan block still rides";
  EXPECT_NE(lm.captured[1].find("wave"), std::string::npos)
      << "the answer reached the derive as the recorded msg.append";
  {
    frame_engine_state_t* e = _frame_engine_state(f);
    ASSERT_NE(e, nullptr);
    EXPECT_NE(e->pending_ask.ask_id, nullptr) << "the gate parked turn 2";
    EXPECT_EQ(e->pending_ask.plan_gate, 1u) << "the GATE holds this park";
    EXPECT_EQ(e->pending_ask.corr, 0ull)
        << "the gate's boxing carries no bridge corr";
  }

  events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(lf_find_control(events, "plan-approved"), nullptr)
      << "NO plan-approved control record was ever written";
  json_value_t* gate = lf_find_gate_ask(events);
  ASSERT_NE(gate, nullptr) << "the gate fired at turn 2's close";
  {
    json_value_t* gp = lf_payload_of(gate);
    json_value_t* plan_v = json_get(gp, "plan");
    ASSERT_NE(plan_v, nullptr);
    EXPECT_EQ(json_type(plan_v), JSON_STRING)
        << "THE GATE'S SHAPE: the plan text rides (the model ask's is NULL)";
    EXPECT_STREQ(json_as_string(plan_v), "Plan: store on wave, mirror on onyx");
  }
  /* The answer's ride batch: [ask.reply, msg.append user "wave"]. */
  json_value_t* reply_rec = NULL;
  size_t reply_index = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "ask.reply") &&
        strcmp(json_as_string(json_get(lf_payload_of(rec), "askId")),
               ask_id.c_str()) == 0) {
      reply_rec = rec;
      reply_index = i;
      break;
    }
  }
  ASSERT_NE(reply_rec, nullptr);
  {
    json_value_t* rp = lf_payload_of(reply_rec);
    EXPECT_STREQ(json_as_string(json_get(rp, "decision")), "answer");
    EXPECT_STREQ(json_as_string(json_get(rp, "value")), "wave");
    json_value_t* append_rec = json_at(events, reply_index + 1);
    ASSERT_NE(append_rec, nullptr);
    /* The seqs are contiguous — the append is the NEXT record. */
    ASSERT_EQ(lf_seq_of(reply_rec) + 1, lf_seq_of(append_rec));
    ASSERT_TRUE(event_is(append_rec, "msg.append"));
    json_value_t* ap = lf_payload_of(append_rec);
    EXPECT_STREQ(json_as_string(json_get(ap, "role")), "user");
    EXPECT_STREQ(json_as_string(json_get(ap, "content")), "wave");
  }
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestFrame, TestPlanCellNeedleSpoofFailsLoud) {
  /* THE ASK EXEMPTION'S OUTCOME CHECK (the review's spoof guard): the
     reply-time needle scan reads the cell code's TEXT — `actor.ask` inside a
     COMMENT matches it and exempts the plan turn's tool call. THE SPOOFED
     CELL **DOES RUN** (the accepted consequence: the needle scan is
     intent-only; the escape is loud-visible and the model self-corrects on
     its next derive) — `print(1+1)` executes and its cell.result answers the
     audit honestly — but the cell publishes NO ask: py_agent's ask verb is
     never called (the log proves it — ZERO "ask" records anywhere), so the
     close-side outcome check takes the verdict: the fail close — control
     {plan-mode, "the exempted ask cell published no ask"} + turn.end{error}
     — and the terminate. The frame stays resumable: the next run is STILL
     plan (ladder_act never flipped) and gates normally. */
  py_agent_init();
  frame_config_t cfg = test_config();
  cfg.escalation_mode = FRAME_ESCALATION_PLAN_ASK_ACT;
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "plan: spoofed ask needle", &cfg);
  ASSERT_NE(f, nullptr);

  ladder_model_t lm = {};
  lm.base.complete = ladder_complete;
  lm.replies = {
      canned_cell_body("# actor.ask in a comment\nprint(1+1)"),
      lf_content_body("Plan: the retried plan after the loud failure")};
  frame_set_model_backend(f, &lm.base);

  EXPECT_EQ(frame_run_loop(f), 1) << "the spoofed exempted cell failed loud";
  EXPECT_EQ(frame_is_done(f), 0) << "the failed terminate never ends a top frame";
  ASSERT_EQ(lm.tools_seen.size(), 1u);
  EXPECT_EQ(lm.tools_seen[0], "json-null")
      << "the exempted cell's turn was a PLAN turn (tools-null)";
  ASSERT_EQ(lm.captured.size(), 1u);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);

  /* THE RECORDER SHAPE: zero publishes — no "ask" record exists anywhere
     (the ask verb's publish always parks + records; nothing was answered). */
  EXPECT_EQ(fr_count_type(events, "ask"), 0u)
      << "the spoofed cell published NO ask";
  EXPECT_EQ(fr_count_type(events, "ask.reply"), 0u);
  EXPECT_EQ(lf_find_gate_ask(events), nullptr)
      << "the gate never fired on the spoofed turn";

  /* THE AUDIT STAYS HONEST: the cell DID run — its cell.result pair answers
     the turn's cell.run line. Find the LAST cell.result (this turn's). */
  json_value_t* result_rec = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "cell.result")) {
      result_rec = json_at(events, i);
    }
  }
  ASSERT_NE(result_rec, nullptr)
      << "the ran (spoofed) cell's audit answer is still written";

  /* THE FAIL CLOSE: control {plan-mode, ...} + turn.end{error} AFTER the
     cell.result (the bare close's order). */
  json_value_t* failure = lf_find_control(events, "plan-mode");
  ASSERT_NE(failure, nullptr) << "the loud verdict's control record";
  ASSERT_GT(lf_seq_of(failure), lf_seq_of(result_rec))
      << "the audit answer precedes the fail close";
  {
    json_value_t* fp = lf_payload_of(failure);
    json_value_t* text_v = json_get(fp, "text");
    ASSERT_NE(text_v, nullptr);
    EXPECT_STREQ(json_as_string(text_v),
                 "the exempted ask cell published no ask");
  }
  json_value_t* turn_end = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    if (event_is(json_at(events, i), "turn.end")) {
      turn_end = json_at(events, i);
    }
  }
  ASSERT_NE(turn_end, nullptr);
  {
    json_value_t* tp = lf_payload_of(turn_end);
    json_value_t* reason = json_get(tp, "reason");
    ASSERT_NE(reason, nullptr);
    EXPECT_STREQ(json_as_string(json_get(reason, "kind")), "error");
  }
  /* THE FAIL CLOSE IS ONE BATCH: control + step.end (the cell's step durably
     started) + turn.end{error} — three consecutive seqs after the cell.result
     (the standing failure-close shape, plan turn or free turn). */
  {
    long long base_seq = lf_seq_of(failure);
    json_value_t* found_step = NULL;
    for (size_t i = 0; i < json_size(events); i++) {
      json_value_t* rec = json_at(events, i);
      if (!event_is(rec, "step.end")) continue;
      if (lf_seq_of(rec) == base_seq + 1) {
        found_step = rec;
        break;
      }
    }
    ASSERT_NE(found_step, nullptr) << "the fail close carries its step.end";
    EXPECT_EQ(lf_seq_of(turn_end), base_seq + 2)
        << "the control + step.end + turn.end group is contiguous";
  }
  /* NO phase transition rode the spoofed turn (this is NOT the gate). */
  EXPECT_EQ(lf_find_control(events, "plan-requested"), nullptr);
  EXPECT_EQ(lf_find_control(events, "plan-approved"), nullptr);
  json_value_destroy(events);

  /* THE FRAME RESUMABLE: the next run is STILL plan — and it gates normally
     on the retried plan (the loud failure's consequence, the honest
     self-correction path). */
  EXPECT_EQ(frame_run_loop(f), 2) << "the next run is STILL plan";
  ASSERT_EQ(lm.tools_seen.size(), 2u);
  EXPECT_EQ(lm.tools_seen[1], "json-null")
      << "the next request is STILL tools-null";
  EXPECT_NE(lm.captured[1].find("PLAN mode"), std::string::npos)
      << "the plan block still rides";
  events = load_events(f);
  ASSERT_NE(events, nullptr);
  json_value_t* gate = lf_find_gate_ask(events);
  ASSERT_NE(gate, nullptr) << "the retried plan's turn gated normally";
  {
    json_value_t* gp = lf_payload_of(gate);
    json_value_t* plan_v = json_get(gp, "plan");
    ASSERT_NE(plan_v, nullptr);
    EXPECT_EQ(json_type(plan_v), JSON_STRING);
    EXPECT_STREQ(json_as_string(plan_v),
                 "Plan: the retried plan after the loud failure");
  }
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}

#endif /* SA_HAS_PYTHON */

#endif /* SA_HAS_WDB */