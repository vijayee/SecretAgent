//
// Created by victor on 5/6/25.
//

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
extern "C" {
#include "../src/Actor/message_queue.h"
#include "../src/Util/allocator.h"
}

static message_node_t* make_node(uint32_t type, void* payload) {
  message_node_t* node = (message_node_t*)get_clear_memory(sizeof(message_node_t));
  node->msg.type = type;
  node->msg.payload = payload;
  node->msg.payload_destroy = NULL;
  ATOMIC_STORE(&node->next, (message_node_t*)NULL);
  return node;
}

TEST(TestMessageQueue, TestInitDestroy) {
  message_queue_t queue;
  message_queue_init(&queue);
  EXPECT_TRUE(message_queue_isempty(&queue));
  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestPushPop) {
  message_queue_t queue;
  message_queue_init(&queue);

  message_node_t* node = make_node(42, NULL);
  message_queue_push(&queue, node, node, NULL);

  message_node_t* popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 42u);

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestPushReturnsEmpty) {
  message_queue_t queue;
  message_queue_init(&queue);

  message_node_t* node1 = make_node(1, NULL);
  bool was_empty = false;
  bool pushed = message_queue_push(&queue, node1, node1, &was_empty);
  EXPECT_TRUE(pushed);
  EXPECT_TRUE(was_empty);

  message_node_t* node2 = make_node(2, NULL);
  pushed = message_queue_push(&queue, node2, node2, &was_empty);
  EXPECT_TRUE(pushed);
  EXPECT_FALSE(was_empty);

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestPopEmpty) {
  message_queue_t queue;
  message_queue_init(&queue);

  message_node_t* popped = message_queue_pop(&queue);
  EXPECT_EQ(popped, nullptr);

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestMarkEmpty) {
  message_queue_t queue;
  message_queue_init(&queue);

  EXPECT_TRUE(message_queue_markempty(&queue));

  message_node_t* node = make_node(1, NULL);
  message_queue_push(&queue, node, node, NULL);

  EXPECT_FALSE(message_queue_markempty(&queue));

  message_node_t* popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);

  EXPECT_TRUE(message_queue_markempty(&queue));

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestIsEmpty) {
  message_queue_t queue;
  message_queue_init(&queue);

  EXPECT_TRUE(message_queue_isempty(&queue));

  message_node_t* node = make_node(1, NULL);
  message_queue_push(&queue, node, node, NULL);

  EXPECT_FALSE(message_queue_isempty(&queue));

  message_node_t* popped = message_queue_pop(&queue);
  (void)popped;

  EXPECT_TRUE(message_queue_isempty(&queue));

  message_queue_destroy(&queue);
}

TEST(TestMessageQueue, TestBatchPush) {
  message_queue_t queue;
  message_queue_init(&queue);

  message_node_t* first = make_node(1, NULL);
  message_node_t* second = make_node(2, NULL);
  message_node_t* third = make_node(3, NULL);

  ATOMIC_STORE(&first->next, second);
  ATOMIC_STORE(&second->next, third);
  ATOMIC_STORE(&third->next, (message_node_t*)NULL);

  bool was_empty = false;
  bool pushed = message_queue_push(&queue, first, third, &was_empty);
  EXPECT_TRUE(pushed);
  EXPECT_TRUE(was_empty);

  message_node_t* popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 1u);

  popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 2u);

  popped = message_queue_pop(&queue);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(popped->msg.type, 3u);

  message_queue_destroy(&queue);
}

TEST(MessageQueueTeardown, PushAfterDestroyFreesMessageAndReturnsFalse) {
  message_queue_t queue;
  message_queue_init(&queue);

  // Destroy the queue on this thread.
  message_queue_destroy(&queue);

  // Now push from another thread (simulating a late MsQuic-callback send).
  std::thread late_sender([&]() {
    message_node_t* node = (message_node_t*)calloc(1, sizeof(message_node_t));
    node->msg.payload = calloc(16, 1);
    node->msg.payload_destroy = free;
    bool success = message_queue_push(&queue, node, node, NULL);
    EXPECT_FALSE(success) << "push into a destroyed queue must return false, not crash";
    // On failure the contract is: push freed the node AND the payload.
    // No further cleanup here — push already did it.
  });
  late_sender.join();
  // No assert beyond "no crash" — valgrind at step 7 asserts no leak / no double-free.
}

TEST(MessageQueueTeardown, ConcurrentPushAndDestroyNoCrash) {
  for (int iteration = 0; iteration < 200; iteration++) {
    message_queue_t queue;
    message_queue_init(&queue);

    std::thread destroyer([&]() {
      // Spin briefly to race with the senders.
      for (int spin = 0; spin < 100; spin++) {
        // no-op spin to let senders get going
      }
      message_queue_destroy(&queue);
    });

    std::vector<std::thread> senders;
    for (int sender_index = 0; sender_index < 4; sender_index++) {
      senders.emplace_back([&]() {
        for (int send = 0; send < 100; send++) {
          message_node_t* node = (message_node_t*)calloc(1, sizeof(message_node_t));
          node->msg.payload = calloc(16, 1);
          node->msg.payload_destroy = free;
          // Push returns false if the queue was destroyed (push freed node + payload).
          message_queue_push(&queue, node, node, NULL);
        }
      });
    }

    destroyer.join();
    for (auto& sender : senders) sender.join();
  }
  SUCCEED();  // No crash, no UAF.
}