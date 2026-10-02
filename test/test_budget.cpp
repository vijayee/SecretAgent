//
// Created by victor on 10/02/26.
//

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <string>
extern "C" {
#include "../src/Util/budget.h"
}

TEST(TestBudget, TestCleanCopyCarriesNoMarker) {
  char* out = NULL;
  uint8_t truncated = 7;
  budget_truncate_with_marker("small text", 1024, &out, &truncated);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "small text");
  EXPECT_EQ(truncated, 0);
  free(out);
}

TEST(TestBudget, TestExactBoundaryIsClean) {
  const char* s = "abcdef";
  char* out = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(s, 6, &out, &truncated);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "abcdef");
  EXPECT_EQ(truncated, 0);
  free(out);
}

TEST(TestBudget, TestTruncationAppendsTheMarker) {
  std::string big(100, 'x');
  char* out = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(big.c_str(), 32, &out, &truncated);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(truncated, 1);
  std::string expect(32, 'x');
  expect += "\n[budget: truncated at 100 bytes]";
  EXPECT_STREQ(out, expect.c_str());
  free(out);
}

TEST(TestBudget, TestRefusalsCarryNoMarker) {
  char* out = (char*)1;
  uint8_t truncated = 0;
  budget_truncate_with_marker(NULL, 32, &out, &truncated);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(truncated, 0);
  out = (char*)1;
  budget_truncate_with_marker("x", 0, &out, &truncated);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(truncated, 0);
}

TEST(TestBudget, TestDefaultsArePinned) {
  EXPECT_EQ(SA_BUDGET_CELL_RESULT_BYTES, 32u * 1024u);
  EXPECT_EQ(SA_BUDGET_EMIT_BYTES, 16u * 1024u);
  EXPECT_EQ(SA_BUDGET_BRIDGE_VALUE_BYTES, 16u * 1024u);
  EXPECT_EQ(SA_BUDGET_KEYS_MAX, 256u);
  EXPECT_EQ(SA_FRAME_CELL_WATCHDOG_MS, 300000u);
  EXPECT_EQ(SA_BUDGET_LOOP_MSG_CAP, 4000);
  EXPECT_EQ(SA_BUDGET_LOOP_SNAPSHOT, 500);
  EXPECT_EQ(SA_BUDGET_LOOP_REPORT, 300);
  EXPECT_EQ(SA_BUDGET_LOOP_EMIT, 300);
}