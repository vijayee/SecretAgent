//
// Created by victor on 5/16/25.
//

#include "stream_framer.h"
#include "../Util/allocator.h"
#include <string.h>

#define STREAM_FRAMER_INITIAL_CAPACITY 256
#define STREAM_FRAMER_LENGTH_PREFIX_SIZE 4

struct stream_framer_t {
  uint8_t* buffer;
  size_t capacity;
  size_t used;
};

stream_framer_t* stream_framer_create(void) {
  stream_framer_t* framer = get_clear_memory(sizeof(stream_framer_t));
  if (framer == NULL) return NULL;
  framer->capacity = STREAM_FRAMER_INITIAL_CAPACITY;
  framer->buffer = get_clear_memory(framer->capacity);
  if (framer->buffer == NULL) {
    free(framer);
    return NULL;
  }
  framer->used = 0;
  return framer;
}

void stream_framer_destroy(stream_framer_t* framer) {
  if (framer == NULL) return;
  if (framer->buffer != NULL) {
    free(framer->buffer);
  }
  free(framer);
}

int stream_framer_feed(stream_framer_t* framer, const uint8_t* data, size_t len) {
  if (framer == NULL) return -1;
  if (len == 0) return 0;
  if (data == NULL) return -1;

  /* Reject any feed that would push the buffered bytes past the cap + prefix.
     This also bounds the slow-drip attack: a peer advertising a huge length
     cannot make us allocate beyond STREAM_FRAMER_MAX_FRAME_SIZE. */
  if (len > STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE) return -1;
  if (framer->used > STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE - len) {
    return -1;
  }

  size_t needed = framer->used + len;
  if (needed > framer->capacity) {
    size_t new_capacity = framer->capacity;
    while (new_capacity < needed) {
      /* Guard against doubling-overflow: never grow past the cap + prefix. */
      if (new_capacity > STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE) {
        new_capacity = STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE;
        break;
      }
      size_t next = new_capacity * 2;
      if (next <= new_capacity) {
        /* Multiplication overflow — clamp to the cap. */
        new_capacity = STREAM_FRAMER_MAX_FRAME_SIZE + STREAM_FRAMER_LENGTH_PREFIX_SIZE;
        break;
      }
      new_capacity = next;
    }
    if (new_capacity < needed) {
      /* The cap prevented us from reaching `needed`; the caller is asking for
         more than the framer will hold. */
      return -1;
    }
    uint8_t* new_buffer = realloc(framer->buffer, new_capacity);
    if (new_buffer == NULL) return -1;
    framer->buffer = new_buffer;
    framer->capacity = new_capacity;
  }

  memcpy(framer->buffer + framer->used, data, len);
  framer->used += len;
  return 0;
}

uint8_t* stream_framer_next(stream_framer_t* framer, size_t* out_len) {
  if (out_len != NULL) *out_len = 0;
  if (framer == NULL) return NULL;

  if (framer->used < STREAM_FRAMER_LENGTH_PREFIX_SIZE) return NULL;

  uint32_t length = ((uint32_t)framer->buffer[0] << 24) |
                    ((uint32_t)framer->buffer[1] << 16) |
                    ((uint32_t)framer->buffer[2] << 8) |
                    (uint32_t)framer->buffer[3];

  /* Reject any declared length above the cap. Without this, a peer can pin
     arbitrarily large allocations by advertising 4 GiB and slow-dripping.
     The cap also closes the 32-bit overflow on total_message_size = 4 + length
     (length <= 2 MB => total < 4 GiB, no wrap). See audit #3. */
  if ((size_t)length > STREAM_FRAMER_MAX_FRAME_SIZE) {
    return NULL;
  }

  size_t total_message_size = STREAM_FRAMER_LENGTH_PREFIX_SIZE + (size_t)length;

  if (framer->used < total_message_size) return NULL;

  uint8_t* payload = get_clear_memory((size_t)length);
  if (payload == NULL) return NULL;
  if (length > 0) {
    memcpy(payload, framer->buffer + STREAM_FRAMER_LENGTH_PREFIX_SIZE, (size_t)length);
  }

  size_t remaining = framer->used - total_message_size;
  if (remaining > 0) {
    memmove(framer->buffer, framer->buffer + total_message_size, remaining);
  }
  framer->used = remaining;

  if (out_len != NULL) *out_len = (size_t)length;
  return payload;
}

uint8_t* stream_frame_encode(const uint8_t* data, size_t data_len, size_t* out_len) {
  if (out_len != NULL) *out_len = 0;
  if (data == NULL && data_len > 0) return NULL;
  /* Reject oversize frames symmetrically with stream_framer_next's cap, so
     the sender fails clearly instead of the receiver silently dropping.
     See docs/liboffs-audit-report.md #3. */
  if (data_len > STREAM_FRAMER_MAX_FRAME_SIZE) return NULL;

  size_t total = STREAM_FRAMER_LENGTH_PREFIX_SIZE + data_len;
  uint8_t* frame = get_clear_memory(total);
  if (frame == NULL) return NULL;

  frame[0] = (uint8_t)((data_len >> 24) & 0xFF);
  frame[1] = (uint8_t)((data_len >> 16) & 0xFF);
  frame[2] = (uint8_t)((data_len >> 8) & 0xFF);
  frame[3] = (uint8_t)(data_len & 0xFF);

  if (data_len > 0 && data != NULL) {
    memcpy(frame + STREAM_FRAMER_LENGTH_PREFIX_SIZE, data, data_len);
  }

  if (out_len != NULL) *out_len = total;
  return frame;
}