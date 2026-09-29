//
// Created by victor on 5/6/25.
//

#include <gtest/gtest.h>
#include <cstring>
extern "C" {
#include "../src/Actor/pool.h"
}

TEST(TestPool, TestIndexSizeMapping) {
  for (size_t bits = POOL_MIN_BITS; bits <= POOL_MAX_BITS; bits++) {
    size_t size = 1UL << bits;
    size_t index = pool_index(size);
    EXPECT_EQ(index, bits - POOL_MIN_BITS);
  }

  size_t index0 = pool_index(1);
  EXPECT_EQ(index0, 0u);

  size_t index_max = pool_index(1UL << POOL_MAX_BITS);
  EXPECT_EQ(index_max, (size_t)(POOL_MAX_BITS - POOL_MIN_BITS));

  size_t index_oversized = pool_index(1UL << 20);
  EXPECT_EQ(index_oversized, (size_t)(POOL_COUNT - 1));
}

TEST(TestPool, TestUsedSizeMapping) {
  for (size_t i = 0; i < POOL_COUNT; i++) {
    size_t expected = 1UL << (POOL_MIN_BITS + i);
    EXPECT_EQ(pool_used_size(i), expected);
  }
}

TEST(TestPool, TestAllocFree) {
  for (size_t i = 0; i < POOL_COUNT; i++) {
    void* ptr = pool_alloc(i);
    ASSERT_NE(ptr, nullptr);
    pool_free(i, ptr);
  }
}

TEST(TestPool, TestAllocContent) {
  for (size_t i = 0; i < POOL_COUNT; i++) {
    void* ptr = pool_alloc(i);
    ASSERT_NE(ptr, nullptr);

    size_t size = pool_used_size(i);
    unsigned char* bytes = static_cast<unsigned char*>(ptr);
    for (size_t j = 0; j < size; j++) {
      EXPECT_EQ(bytes[j], 0u) << "byte at offset " << j << " not zero-initialized in pool index " << i;
    }
    pool_free(i, ptr);
  }
}

TEST(TestPool, TestThreadCleanup) {
  pool_thread_cleanup();
  SUCCEED();
}