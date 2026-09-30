//
// Created by victor on 3/18/25.
//
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "buffer.h"
#include "../Util/allocator.h"
#include <string.h>



buffer_t* buffer_create(size_t size) {
  buffer_t* buf = get_clear_memory(sizeof(buffer_t));
  buf->data = get_clear_memory(size);
  buf->size = size;
  buf->capacity = size;
  refcounter_init((refcounter_t*) buf);
  return buf;
}

buffer_t* buffer_create_with_capacity(size_t size, size_t capacity) {
  buffer_t* buf = get_clear_memory(sizeof(buffer_t));
  buf->data = get_clear_memory(capacity);
  buf->size = size;
  buf->capacity = capacity;
  refcounter_init((refcounter_t*) buf);
  return buf;
}

void buffer_ensure_capacity(buffer_t* buf, size_t needed) {
  if (buf->capacity >= needed) {
    return;
  }
  size_t new_capacity = buf->capacity == 0 ? needed : buf->capacity;
  while (new_capacity < needed) {
    new_capacity *= 2;
  }
  void* new_data = realloc(buf->data, new_capacity);
  if (new_data == NULL) {
    /* OOM: consistent with get_memory/get_clear_memory (src/Util/allocator.c),
       which abort on allocation failure. Assigning NULL to buf->data would
       leak the old buffer and leave buf->capacity inconsistent with the
       still-referenced old allocation, inviting a later double-free or
       NULL deref. Abort is the project's established OOM contract. */
    abort();
  }
  buf->data = new_data;
  buf->capacity = new_capacity;
}

buffer_t* buffer_create_from_pointer_copy(uint8_t* data, size_t size) {
  buffer_t* buf = get_clear_memory(sizeof(buffer_t));
  buf->size = size;
  buf->capacity = size;
  buf->data = get_memory(size);
  buffer_copy_from_pointer(buf, data, size);
  refcounter_init((refcounter_t*) buf);
  return buf;
}

buffer_t* buffer_create_from_existing_memory(uint8_t* data, size_t size) {
  buffer_t* buf = get_clear_memory(sizeof(buffer_t));
  buf->data = data;
  buf->size = size;
  buf->capacity = size;
  refcounter_init((refcounter_t*) buf);
  return buf;
}

buffer_t* buffer_copy(buffer_t* buf) {
  if (!buf) return NULL;
  return buffer_create_from_pointer_copy(buf->data, buf->size);
}

buffer_t* buffer_concat(buffer_t* buf1, buffer_t* buf2) {
  buffer_t* buf = buffer_create(buf1->size + buf2->size);
  memcpy(buf->data, buf1->data, buf1->size);
  memcpy(buf->data + buf1->size, buf2->data, buf2->size);
  return buf;
}

void buffer_copy_from_pointer(buffer_t* buf, uint8_t* data, size_t size) {
  if (buf->size > size) {
    memcpy(buf->data, data, size);
  } else {
    memcpy(buf->data, data, buf->size);
  }
}

void buffer_destroy(buffer_t* buf) {
  if (refcounter_dereference_is_zero((refcounter_t*)buf)) {
    free(buf->data);
    refcounter_destroy_lock(&buf->refcounter);
    free(buf);
  }
}

uint8_t buffer_get_index(buffer_t* buf, size_t index) {
  return buf->data[index];
}

uint8_t buffer_set_index(buffer_t* buf, size_t index, uint8_t value) {
  return buf->data[index] = value;
}

buffer_t* buffer_slice(buffer_t* buf, size_t start, size_t end) {
  if (start > end) {
    return NULL;
  }
  if (buf->size < end) {
    return NULL;
  }
  if (buf->size < start) {
    return NULL;
  }

  uint8_t* startPtr = buf->data + start;
  size_t sliceSize = end - start;
  return buffer_create_from_pointer_copy(startPtr, sliceSize);
}

buffer_t* buffer_xor(buffer_t* buf1, buffer_t* buf2) {
  size_t size = buf1->size;
  if (buf2->size > buf1->size) {
    size = buf2->size;
  }
  size_t min = buf1->size;
  buffer_t* largest = buf2;
  if (buf2->size < buf1->size) {
    min = buf2->size;
    largest = buf1;
  }

  buffer_t* result = buffer_create(size);
  for (size_t i = 0; i < min; i++) {
   result->data[i] = buf1->data[i] ^ buf2->data[i];
  }
  for (size_t i = min; i < largest->size; i++) {
    result->data[i] = largest->data[i] ^ 0;
  }
  return result;
}

buffer_t* buffer_or(buffer_t* buf1, buffer_t* buf2) {
  size_t size = buf1->size;
  if (buf2->size > buf1->size) {
    size = buf2->size;
  }
  size_t min = buf1->size;
  buffer_t* largest = buf2;
  if (buf2->size < buf1->size) {
    min = buf2->size;
    largest = buf1;
  }

  buffer_t* result = buffer_create(size);
  for (size_t i = 0; i < min; i++) {
    result->data[i] = buf1->data[i] | buf2->data[i];
  }
  for (size_t i = min; i < largest->size; i++) {
    result->data[i] = largest->data[i] | 0;
  }
  return result;
}

buffer_t* buffer_and(buffer_t* buf1, buffer_t* buf2) {
  size_t size = buf1->size;
  if (buf2->size > buf1->size) {
    size = buf2->size;
  }
  size_t min = buf1->size;
  buffer_t* largest = buf2;
  if (buf2->size < buf1->size) {
    min = buf2->size;
    largest = buf1;
  }

  buffer_t* result = buffer_create(size);
  for (size_t i = 0; i < min; i++) {
    result->data[i] = buf1->data[i] & buf2->data[i];
  }
  for (size_t i = min; i < largest->size; i++) {
    result->data[i] = largest->data[i] & 0;
  }
  return result;
}

buffer_t* buffer_not(buffer_t* buf) {
  buffer_t* result = buffer_create(buf->size);
  for (size_t i = 0; i < buf->size; i++) {
    result->data[i] = ~buf->data[i];
  }
  return result;
}

int8_t buffer_compare(buffer_t* buf1, buffer_t* buf2) {
  size_t min_length = buf1->size < buf2->size ? buf1->size : buf2->size;

  for (size_t i = 0; i < min_length; i++) {
    uint8_t val1 = buf1->data[i];
    uint8_t val2 = buf2->data[i];
    if (val1 < val2) return -1;
    if (val1 > val2) return 1;
  }

  if (buf1->size < buf2->size) return -1;
  if (buf1->size > buf2->size) return 1;
  return 0;
}

cbor_item_t* buffer_to_cbor(buffer_t* buf) {
  return cbor_build_bytestring(buf->data, buf->size);
}

buffer_t* cbor_to_buffer(cbor_item_t* cbor) {
  uint8_t* data = (uint8_t*) cbor_bytestring_handle(cbor);
  size_t size =  cbor_bytestring_length(cbor);
  return buffer_create_from_pointer_copy(data, size);
}