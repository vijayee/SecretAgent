//
// Created on 5/6/25.
//

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <cstdint>

extern "C" {
#include "../src/Scheduler/deque.h"
}

// Sentinel values: DEQUE_EMPTY = (void*)0, DEQUE_ABORT = (void*)1
// Use values >= 100 to avoid collisions with sentinels.
#define PTR_VAL(v) ((void*)(intptr_t)(v))

TEST(TestDeque, TestInitDestroy) {
  deque_t deque;
  deque_init(&deque);
  EXPECT_TRUE(deque_isempty(&deque));
  EXPECT_EQ(deque_size(&deque), 0u);
  deque_destroy(&deque);
}

TEST(TestDeque, TestPushPop) {
  deque_t deque;
  deque_init(&deque);

  deque_push(&deque, PTR_VAL(42));
  EXPECT_FALSE(deque_isempty(&deque));
  EXPECT_EQ(deque_size(&deque), 1u);

  void* item = deque_pop(&deque);
  EXPECT_EQ((intptr_t)item, 42);
  EXPECT_TRUE(deque_isempty(&deque));

  deque_destroy(&deque);
}

TEST(TestDeque, TestPushPopMultiple) {
  deque_t deque;
  deque_init(&deque);

  for (int i = 0; i < 10; i++) {
    deque_push(&deque, PTR_VAL(100 + i));
  }
  EXPECT_EQ(deque_size(&deque), 10u);

  // Pop returns in LIFO order
  for (int i = 9; i >= 0; i--) {
    void* item = deque_pop(&deque);
    ASSERT_NE(item, DEQUE_EMPTY);
    ASSERT_NE(item, DEQUE_ABORT);
    EXPECT_EQ((intptr_t)item, 100 + i);
  }
  EXPECT_TRUE(deque_isempty(&deque));

  deque_destroy(&deque);
}

TEST(TestDeque, TestPopEmpty) {
  deque_t deque;
  deque_init(&deque);

  void* item = deque_pop(&deque);
  EXPECT_EQ(item, DEQUE_EMPTY);

  deque_destroy(&deque);
}

TEST(TestDeque, TestStealFromNonEmpty) {
  deque_t deque;
  deque_init(&deque);

  for (int i = 0; i < 5; i++) {
    deque_push(&deque, PTR_VAL(100 + i));
  }

  // Steal returns in FIFO order (oldest first)
  for (int i = 0; i < 5; i++) {
    void* item = deque_steal(&deque);
    ASSERT_NE(item, DEQUE_EMPTY);
    ASSERT_NE(item, DEQUE_ABORT);
    EXPECT_EQ((intptr_t)item, 100 + i);
  }

  void* item = deque_steal(&deque);
  EXPECT_EQ(item, DEQUE_EMPTY);

  deque_destroy(&deque);
}

TEST(TestDeque, TestStealFromEmpty) {
  deque_t deque;
  deque_init(&deque);

  void* item = deque_steal(&deque);
  EXPECT_EQ(item, DEQUE_EMPTY);

  deque_destroy(&deque);
}

TEST(TestDeque, TestPopAndStealRace) {
  deque_t deque;
  deque_init(&deque);

  // Push exactly one item (use value > 1 to avoid sentinel collision)
  deque_push(&deque, PTR_VAL(99));

  std::atomic<int> success_count{0};

  auto steal_fn = [&deque, &success_count]() {
    void* item = deque_steal(&deque);
    if (item != DEQUE_EMPTY && item != DEQUE_ABORT) {
      success_count.fetch_add(1);
    }
  };

  std::thread thief(steal_fn);

  void* item = deque_pop(&deque);
  if (item != DEQUE_EMPTY && item != DEQUE_ABORT) {
    success_count.fetch_add(1);
  }

  thief.join();

  // Exactly one of pop or steal should have gotten the item
  EXPECT_EQ(success_count.load(), 1);

  deque_destroy(&deque);
}

TEST(TestDeque, TestConcurrentSteal) {
  deque_t deque;
  deque_init(&deque);

  const int N = 100;
  const int T = 4;

  for (int i = 0; i < N; i++) {
    deque_push(&deque, PTR_VAL(100 + i));
  }

  std::atomic<int> total_stolen{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < T; t++) {
    threads.emplace_back([&deque, &total_stolen]() {
      int local_count = 0;
      for (int attempt = 0; attempt < N; attempt++) {
        void* item = deque_steal(&deque);
        if (item != DEQUE_EMPTY && item != DEQUE_ABORT) {
          local_count++;
        }
      }
      total_stolen.fetch_add(local_count);
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  // Drain remaining items from the deque via pop
  while (true) {
    void* item = deque_pop(&deque);
    if (item == DEQUE_EMPTY) break;
    total_stolen.fetch_add(1);
  }

  // All N items should be accounted for (no duplicates, no losses)
  EXPECT_EQ(total_stolen.load(), N);

  deque_destroy(&deque);
}

TEST(TestDeque, TestGrowArray) {
  deque_t deque;
  deque_init(&deque);

  // Push more items than DEQUE_INITIAL_CAPACITY to trigger grow
  const int count = DEQUE_INITIAL_CAPACITY + 100;
  for (int i = 0; i < count; i++) {
    deque_push(&deque, PTR_VAL(100 + i));
  }

  // Pop all items and verify correctness (LIFO order)
  for (int i = count - 1; i >= 0; i--) {
    void* item = deque_pop(&deque);
    ASSERT_NE(item, DEQUE_EMPTY) << "Failed at index " << i;
    ASSERT_NE(item, DEQUE_ABORT) << "Got ABORT at index " << i;
    EXPECT_EQ((intptr_t)item, 100 + i);
  }

  void* item = deque_pop(&deque);
  EXPECT_EQ(item, DEQUE_EMPTY);

  deque_destroy(&deque);
}

TEST(TestDeque, TestIsEmpty) {
  deque_t deque;
  deque_init(&deque);

  EXPECT_TRUE(deque_isempty(&deque));

  deque_push(&deque, PTR_VAL(100));
  EXPECT_FALSE(deque_isempty(&deque));

  void* item = deque_pop(&deque);
  (void)item;
  EXPECT_TRUE(deque_isempty(&deque));

  deque_destroy(&deque);
}

TEST(TestDeque, TestSize) {
  deque_t deque;
  deque_init(&deque);

  EXPECT_EQ(deque_size(&deque), 0u);

  for (int i = 0; i < 5; i++) {
    deque_push(&deque, PTR_VAL(100 + i));
  }
  EXPECT_EQ(deque_size(&deque), 5u);

  // Pop two items
  deque_pop(&deque);
  deque_pop(&deque);
  EXPECT_EQ(deque_size(&deque), 3u);

  // Steal one item
  deque_steal(&deque);
  EXPECT_EQ(deque_size(&deque), 2u);

  // Drain remaining
  deque_pop(&deque);
  deque_pop(&deque);
  EXPECT_EQ(deque_size(&deque), 0u);

  deque_destroy(&deque);
}

TEST(TestDeque, TestPushPopSteal) {
  deque_t deque;
  deque_init(&deque);

  // Push items: [100, 101, 102, 103, 104]
  for (int i = 0; i < 5; i++) {
    deque_push(&deque, PTR_VAL(100 + i));
  }
  EXPECT_EQ(deque_size(&deque), 5u);

  // Steal from bottom (FIFO): gets 100
  void* stolen = deque_steal(&deque);
  ASSERT_NE(stolen, DEQUE_EMPTY);
  ASSERT_NE(stolen, DEQUE_ABORT);
  EXPECT_EQ((intptr_t)stolen, 100);

  // Pop from top (LIFO): gets 104
  void* popped = deque_pop(&deque);
  ASSERT_NE(popped, DEQUE_EMPTY);
  ASSERT_NE(popped, DEQUE_ABORT);
  EXPECT_EQ((intptr_t)popped, 104);

  // Steal again: gets 101
  stolen = deque_steal(&deque);
  ASSERT_NE(stolen, DEQUE_EMPTY);
  ASSERT_NE(stolen, DEQUE_ABORT);
  EXPECT_EQ((intptr_t)stolen, 101);

  // Pop again: gets 103
  popped = deque_pop(&deque);
  ASSERT_NE(popped, DEQUE_EMPTY);
  ASSERT_NE(popped, DEQUE_ABORT);
  EXPECT_EQ((intptr_t)popped, 103);

  // Only 102 remains: steal it
  stolen = deque_steal(&deque);
  ASSERT_NE(stolen, DEQUE_EMPTY);
  ASSERT_NE(stolen, DEQUE_ABORT);
  EXPECT_EQ((intptr_t)stolen, 102);

  // Deque should now be empty
  EXPECT_EQ(deque_pop(&deque), DEQUE_EMPTY);
  EXPECT_EQ(deque_steal(&deque), DEQUE_EMPTY);

  deque_destroy(&deque);
}

TEST(TestDeque, TestStealAbort) {
  // When two threads race to steal from a deque with a single item,
  // exactly one must succeed. The other may get DEQUE_ABORT (if the
  // CAS fails) or DEQUE_EMPTY (if the winner incremented top before
  // the loser reads bottom). Either outcome is correct; the important
  // invariant is that exactly one thread gets the item and no data is
  // corrupted.
  deque_t deque;
  deque_init(&deque);

  deque_push(&deque, PTR_VAL(200));

  std::atomic<int> success_count{0};

  auto steal_fn = [&deque, &success_count]() {
    void* item = deque_steal(&deque);
    if (item != DEQUE_EMPTY && item != DEQUE_ABORT) {
      success_count.fetch_add(1);
    }
  };

  std::thread t1(steal_fn);
  std::thread t2(steal_fn);
  t1.join();
  t2.join();

  // Exactly one steal should succeed
  EXPECT_EQ(success_count.load(), 1);

  // The deque should now be empty
  EXPECT_EQ(deque_steal(&deque), DEQUE_EMPTY);

  deque_destroy(&deque);
}

TEST(TestDeque, TestPushPopRepush) {
  // Simulate the scheduler pattern: push an item, pop it, then push it again
  // and pop again. This mimics actor rescheduling behavior.
  deque_t deque;
  deque_init(&deque);

  void* item = PTR_VAL(999);

  // Push
  deque_push(&deque, item);
  EXPECT_EQ(deque_size(&deque), 1u);

  // Pop
  void* popped = deque_pop(&deque);
  ASSERT_NE(popped, DEQUE_EMPTY);
  ASSERT_NE(popped, DEQUE_ABORT);
  EXPECT_EQ(popped, item);
  EXPECT_EQ(deque_size(&deque), 0u);

  // Re-push (simulates actor with has_more being re-queued)
  deque_push(&deque, item);
  EXPECT_EQ(deque_size(&deque), 1u);

  // Pop again
  popped = deque_pop(&deque);
  ASSERT_NE(popped, DEQUE_EMPTY) << "deque_pop returned DEQUE_EMPTY after re-push!";
  ASSERT_NE(popped, DEQUE_ABORT) << "deque_pop returned DEQUE_ABORT after re-push!";
  EXPECT_EQ(popped, item);

  EXPECT_EQ(deque_size(&deque), 0u);
  EXPECT_TRUE(deque_isempty(&deque));

  deque_destroy(&deque);
}