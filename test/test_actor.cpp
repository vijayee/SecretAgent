//
// Created by victor on 5/6/25.
//

#include <gtest/gtest.h>
#include <vector>
extern "C" {
#include "../src/Actor/actor.h"
#include "../src/Actor/message_queue.h"
#include "../src/Scheduler/scheduler.h"
}

static void test_dispatch(void* state, message_t* msg) {
  auto* count = static_cast<int*>(state);
  (*count)++;
  (void)msg;
}

static void record_dispatch(void* state, message_t* msg) {
  auto* vec = static_cast<std::vector<uint32_t>*>(state);
  vec->push_back(msg->type);
}

static int payload_destroy_count;
static void test_payload_destroy(void* payload) {
  payload_destroy_count++;
  free(payload);
}

TEST(TestActor, TestInitDestroy) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);
  EXPECT_TRUE(message_queue_isempty(&actor.queue));
  actor_destroy(&actor);
}

TEST(TestActor, TestSendReturnsEmpty) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  message_t msg;
  msg.type = 1;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  bool was_empty = actor_send(&actor, &msg);
  EXPECT_TRUE(was_empty);

  actor_destroy(&actor);
}

TEST(TestActor, TestSendReturnsNotEmpty) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  message_t msg1;
  msg1.type = 1;
  msg1.payload = NULL;
  msg1.payload_destroy = NULL;

  bool was_empty = actor_send(&actor, &msg1);
  EXPECT_TRUE(was_empty);

  message_t msg2;
  msg2.type = 2;
  msg2.payload = NULL;
  msg2.payload_destroy = NULL;

  was_empty = actor_send(&actor, &msg2);
  EXPECT_FALSE(was_empty);

  actor_destroy(&actor);
}

TEST(TestActor, TestRunDispatchesMessages) {
  std::vector<uint32_t> received;
  actor_t actor;
  actor_init(&actor, &received, record_dispatch, NULL);

  message_t msg1;
  msg1.type = 10;
  msg1.payload = NULL;
  msg1.payload_destroy = NULL;

  message_t msg2;
  msg2.type = 20;
  msg2.payload = NULL;
  msg2.payload_destroy = NULL;

  message_t msg3;
  msg3.type = 30;
  msg3.payload = NULL;
  msg3.payload_destroy = NULL;

  actor_send(&actor, &msg1);
  actor_send(&actor, &msg2);
  actor_send(&actor, &msg3);

  actor_run(&actor, 32);

  ASSERT_EQ(received.size(), 3u);
  EXPECT_EQ(received[0], 10u);
  EXPECT_EQ(received[1], 20u);
  EXPECT_EQ(received[2], 30u);

  actor_destroy(&actor);
}

TEST(TestActor, TestRunBatchSize) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  for (int i = 0; i < 5; i++) {
    message_t msg;
    msg.type = i;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&actor, &msg);
  }

  actor_run(&actor, 2);
  EXPECT_EQ(count, 2);

  actor_destroy(&actor);
}

TEST(TestActor, TestRunReturnsTrueWhenMoreRemain) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  for (int i = 0; i < 5; i++) {
    message_t msg;
    msg.type = i;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&actor, &msg);
  }

  bool has_more = actor_run(&actor, 2);
  EXPECT_TRUE(has_more);

  actor_destroy(&actor);
}

TEST(TestActor, TestRunReturnsFalseWhenEmpty) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  message_t msg;
  msg.type = 1;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&actor, &msg);

  bool has_more = actor_run(&actor, 32);
  EXPECT_FALSE(has_more);
  EXPECT_EQ(count, 1);

  actor_destroy(&actor);
}

TEST(TestActor, TestPayloadDestroy) {
  payload_destroy_count = 0;
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  message_t msg;
  msg.type = 1;
  msg.payload = malloc(16);
  msg.payload_destroy = test_payload_destroy;

  actor_send(&actor, &msg);
  actor_run(&actor, 32);

  EXPECT_EQ(payload_destroy_count, 1);

  actor_destroy(&actor);
}

TEST(TestActor, TestBackpressureApplySetsPressuredFlag) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  EXPECT_EQ(atomic_load(&actor.flags) & ACTOR_FLAG_PRESSURED, 0);
  backpressure_apply(&actor);
  EXPECT_NE(atomic_load(&actor.flags) & ACTOR_FLAG_PRESSURED, 0);

  actor_destroy(&actor);
}

TEST(TestActor, TestBackpressureReleaseClearsPressuredFlag) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  backpressure_apply(&actor);
  EXPECT_NE(atomic_load(&actor.flags) & ACTOR_FLAG_PRESSURED, 0);

  backpressure_release(&actor);
  EXPECT_EQ(atomic_load(&actor.flags) & ACTOR_FLAG_PRESSURED, 0);

  actor_destroy(&actor);
}

TEST(TestActor, TestBackpressureMutesSenderWhenTargetPressured) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  int count = 0;
  actor_t target;
  actor_init(&target, &count, test_dispatch, pool);

  /* Mark target as pressured */
  backpressure_apply(&target);

  /* Stop the pool before sending so no worker drains the mailbox before
     the assertion. The test verifies the send reaches the queue, not that
     the worker processes it — backpressure mutes senders to the target,
     not the target's own drain. (Previously asserted size == 1 with the
     pool live, which raced the worker draining it to 0.) */
  scheduler_pool_stop(pool);

  /* Send a message from outside the scheduler context (simulating a sender).
     Since we're not in a scheduler worker, scheduler_get_current() returns
     NULL, so the sender won't be muted — the message reaches the queue. */
  message_t msg;
  msg.type = 1;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&target, &msg);

  EXPECT_EQ(atomic_load(&target.queue.size), 1u);

  actor_destroy(&target);
  scheduler_pool_destroy(pool);
}

TEST(TestActor, TestMutedActorSkippedByRun) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  message_t msg;
  msg.type = 1;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&actor, &msg);

  /* Mute the actor */
  atomic_fetch_or(&actor.flags, ACTOR_FLAG_MUTED);

  /* actor_run must STILL process the mailbox even when muted — muting
     only prevents the actor from SENDING to PRESSURED targets. Skipping
     processing entirely would deadlock the pipeline (e.g., the connection
     actor gets muted mid-request; if it stops draining its mailbox the
     HTTP body never finishes parsing and the response is never sent).
     See commit 1a57b23 "fix: eliminate backpressure deadlock". */
  bool has_more = actor_run(&actor, 32);
  EXPECT_FALSE(has_more);
  /* Count is 1 because the message was dispatched — muting does not
     skip actor_run, it only mutes outgoing actor_send to PRESSURED targets. */
  EXPECT_EQ(count, 1);

  actor_destroy(&actor);
}

TEST(TestActor, TestMailboxSizeTracking) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  EXPECT_EQ(atomic_load(&actor.queue.size), 0u);

  message_t msg;
  msg.type = 1;
  msg.payload = NULL;
  msg.payload_destroy = NULL;

  actor_send(&actor, &msg);
  EXPECT_EQ(atomic_load(&actor.queue.size), 1u);

  actor_send(&actor, &msg);
  EXPECT_EQ(atomic_load(&actor.queue.size), 2u);

  actor_run(&actor, 1);
  EXPECT_EQ(atomic_load(&actor.queue.size), 1u);

  actor_run(&actor, 1);
  EXPECT_EQ(atomic_load(&actor.queue.size), 0u);

  actor_destroy(&actor);
}

TEST(TestActor, TestAutoBackpressureOnMailboxOverflow) {
  int count = 0;
  actor_t actor;
  actor_init(&actor, &count, test_dispatch, NULL);

  /* Fill the mailbox past the threshold */
  for (size_t i = 0; i < MAILBOX_MUTE_THRESHOLD; i++) {
    message_t msg;
    msg.type = 1;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&actor, &msg);
  }

  /* The actor should now be pressured */
  EXPECT_NE(atomic_load(&actor.flags) & ACTOR_FLAG_PRESSURED, 0);

  /* Process all messages */
  actor_run(&actor, ACTOR_BATCH_SIZE);
  while (atomic_load(&actor.queue.size) > 0) {
    actor_run(&actor, ACTOR_BATCH_SIZE);
  }

  /* After draining, backpressure should be auto-released */
  EXPECT_EQ(atomic_load(&actor.flags) & ACTOR_FLAG_PRESSURED, 0);

  actor_destroy(&actor);
}

TEST(TestActor, TestBackpressureReleaseUnmutesSenders) {
  scheduler_pool_t* pool = scheduler_pool_create(2);
  scheduler_pool_start(pool);

  int sender_count = 0;
  int target_count = 0;
  actor_t sender;
  actor_t target;
  actor_init(&sender, &sender_count, test_dispatch, pool);
  actor_init(&target, &target_count, test_dispatch, pool);

  /* Mark target as pressured */
  backpressure_apply(&target);

  /* Manually add sender to target's pressured_senders list */
  muted_sender_node_t* msn = (muted_sender_node_t*)malloc(sizeof(muted_sender_node_t));
  msn->sender = &sender;
  msn->next = NULL;
  atomic_store(&target.pressured_senders, msn);

  /* Mute the sender */
  atomic_fetch_or(&sender.flags, ACTOR_FLAG_MUTED);

  /* Release pressure on target - should unmute sender */
  backpressure_release(&target);

  /* Sender should be unmuted */
  EXPECT_EQ(atomic_load(&sender.flags) & ACTOR_FLAG_MUTED, 0);
  /* Target should no longer be pressured */
  EXPECT_EQ(atomic_load(&target.flags) & ACTOR_FLAG_PRESSURED, 0);
  /* Pressured senders list should be empty */
  EXPECT_EQ(atomic_load(&target.pressured_senders), (muted_sender_node_t*)NULL);

  actor_destroy(&sender);
  actor_destroy(&target);
  scheduler_pool_wait_for_idle(pool);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}