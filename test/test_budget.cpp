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

TEST(TestBudget, TestCapPlusOneAddsTheMarker) {
  std::string s(6, 'a');
  s += "b";   /* 7 bytes, cap 6 = exactly under-the-wire cut */
  char* out = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(s.c_str(), 6, &out, &truncated);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(truncated, 1);
  EXPECT_NE(strstr(out, "[budget: truncated at 7 bytes]"), nullptr);
  /* The head is still a valid prefix (starts 'aaaaaa', marker appended in
     place of nothing — UTF-8 safe trivially here). */
  EXPECT_EQ(out[0], 'a');
  EXPECT_EQ(out[5], 'a');
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

TEST(TestBudget, TestCutNeverSplitsAUtf8Sequence) {
  /* "ab" + 28 'c' puts a 3-byte CJK char (E4 B8 80) astride the 32-byte
     boundary: the naive cut at 32 slices off its last byte (0x80). */
  std::string text = "ab" + std::string(28, 'c') + "\xE4\xB8\x80 tail";
  char* out = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(text.c_str(), 32, &out, &truncated);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(truncated, 1);
  /* The cut backed up over the continuation bytes: the head is 30 bytes
     ("ab" + 28 'c') and the byte adjacent to the marker is ASCII 'c' —
     never a starter or continuation byte of the split sequence. */
  const char* marker = strstr(out, "\n[budget: truncated at");
  ASSERT_NE(marker, nullptr);
  EXPECT_EQ(marker - out, 30);
  EXPECT_EQ((unsigned char)marker[-1], (unsigned char)'c');
  /* The marker still names the ORIGINAL length (38). */
  EXPECT_NE(strstr(out, "truncated at 38 bytes"), nullptr);
  free(out);
}

TEST(TestBudget, TestCleanCopyIsIndependent) {
  char buffer[16];
  memcpy(buffer, "independent", 12);
  char* out = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(buffer, 32, &out, &truncated);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "independent");
  buffer[0] = 'I';
  EXPECT_EQ(out[0], 'i');   /* the caller mutating the source cannot bleed in */
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