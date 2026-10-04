//
// Created by victor on 5/16/25.
//

#ifndef OFFS_STREAM_FRAMER_H
#define OFFS_STREAM_FRAMER_H

#include <stdint.h>
#include <stddef.h>

/* Hard cap on a single framed message. Blocks are <= 128 KB; 2 MB gives
   headroom for future larger payloads and blocks any peer from pinning
   arbitrarily large allocations by advertising a huge length and slow-dripping
   bytes. See docs/liboffs-audit-report.md #3. */
#define STREAM_FRAMER_MAX_FRAME_SIZE ((size_t)(2 * 1024 * 1024))

/* Encode a framed message: [4-byte big-endian length][data].
 * Returns heap-allocated buffer, or NULL on failure.
 * Caller must free() the returned buffer. */
uint8_t* stream_frame_encode(const uint8_t* data, size_t data_len, size_t* out_len);

/* Accumulator for receiving framed messages from a byte stream.
 * Feed bytes in, extract complete messages out. */
typedef struct stream_framer_t stream_framer_t;

stream_framer_t* stream_framer_create(void);
void stream_framer_destroy(stream_framer_t* framer);

/* Feed received bytes into the framer. Returns 0 on success, -1 on error. */
int stream_framer_feed(stream_framer_t* framer, const uint8_t* data, size_t len);

/* Try to extract the next complete framed message.
 * Returns heap-allocated buffer of the message payload (without length prefix),
 * or NULL if no complete message is available.
 * Caller must free() the returned buffer.
 * out_len receives the payload length. */
uint8_t* stream_framer_next(stream_framer_t* framer, size_t* out_len);

#endif // OFFS_STREAM_FRAMER_H