//
// Created by victor on 10/03/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
extern "C" {
#include "../src/Network/stream_framer.h"
}

TEST(TestStreamFramer, TestEncodeDecodeRoundTrip) {
  const char* msg = "hello framer";
  size_t framed_len = 0;
  uint8_t* framed = stream_frame_encode((const uint8_t*)msg, strlen(msg),
                                        &framed_len);
  ASSERT_NE(framed, nullptr);
  EXPECT_EQ(framed_len, strlen(msg) + 4);
  EXPECT_EQ(framed[0], 0) << "big-endian length prefix: <64KiB message";
  EXPECT_EQ(framed[1], 0);

  stream_framer_t* fr = stream_framer_create();
  ASSERT_NE(fr, nullptr);
  /* feed ONE byte at a time — the accumulator must reassemble */
  for (size_t i = 0; i < framed_len; i++) {
    EXPECT_EQ(stream_framer_feed(fr, framed + i, 1), 0);
  }
  size_t out_len = 0;
  uint8_t* out = stream_framer_next(fr, &out_len);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out_len, strlen(msg));
  EXPECT_EQ(memcmp(out, msg, out_len), 0);
  free(out);
  EXPECT_EQ(stream_framer_next(fr, &out_len), nullptr) << "no spurious frames";
  stream_framer_destroy(fr);
  free(framed);
}

TEST(TestStreamFramer, TestTwoFramesInOneFeed) {
  size_t a_len = 0, b_len = 0;
  uint8_t* a = stream_frame_encode((const uint8_t*)"A", 1, &a_len);
  uint8_t* b = stream_frame_encode((const uint8_t*)"BBB", 3, &b_len);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  std::vector<uint8_t> both(a, a + a_len);
  both.insert(both.end(), b, b + b_len);
  free(a);
  free(b);

  stream_framer_t* fr = stream_framer_create();
  ASSERT_NE(fr, nullptr);
  ASSERT_EQ(stream_framer_feed(fr, both.data(), both.size()), 0);
  size_t out_len = 0;
  uint8_t* out = stream_framer_next(fr, &out_len);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out_len, 1u);
  EXPECT_EQ(out[0], 'A');
  free(out);
  out = stream_framer_next(fr, &out_len);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out_len, 3u);
  EXPECT_EQ(out[0], 'B');
  free(out);
  stream_framer_destroy(fr);
}

TEST(TestStreamFramer, TestOversizedDeclaredLengthRefuses) {
  /* a peer advertising a length over the cap: the 4 header bytes are tiny,
     so feed() takes them; the DECLARED length above the cap refuses loud at
     extraction — next() answers NULL (the slow-drip defense, audit #3:
     "Reject any declared length above the cap" lives in stream_framer_next,
     and feed's guard bounds the buffered BYTE count, not the declared
     length). Pinned against the ported code's actual edge. */
  stream_framer_t* fr = stream_framer_create();
  ASSERT_NE(fr, nullptr);
  uint8_t header[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};   /* way over 2 MB */
  EXPECT_EQ(stream_framer_feed(fr, header, 4), 0);
  size_t out_len = 0;
  EXPECT_EQ(stream_framer_next(fr, &out_len), nullptr)
      << "a declared length over the 2MB cap refuses at extraction";
  EXPECT_EQ(out_len, 0u);
  stream_framer_destroy(fr);
}

TEST(TestStreamFramer, TestCapIsTwoMB) {
  EXPECT_EQ(STREAM_FRAMER_MAX_FRAME_SIZE, (size_t)(2 * 1024 * 1024));
}