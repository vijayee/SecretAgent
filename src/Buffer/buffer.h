//
// Created by victor on 3/18/25.
//

#ifndef LIBOFFS_BUFFER_H
#define LIBOFFS_BUFFER_H

#include "../RefCounter/refcounter.h"
#include <cbor.h>

typedef struct buffer_t {
  refcounter_t refcounter;
  uint8_t* data;
  size_t size;
  size_t capacity;
} buffer_t;


buffer_t* buffer_create(size_t size);
buffer_t* buffer_create_with_capacity(size_t size, size_t capacity);
void buffer_ensure_capacity(buffer_t* buf, size_t needed);
buffer_t* buffer_create_from_pointer_copy(uint8_t* data, size_t size);
buffer_t* buffer_create_from_existing_memory(uint8_t* data, size_t size);
void buffer_copy_from_pointer(buffer_t* buf, uint8_t* data, size_t size);
buffer_t* buffer_copy(buffer_t* buf);
buffer_t* buffer_slice(buffer_t* buf, size_t start, size_t end);
buffer_t* buffer_concat(buffer_t* buf1, buffer_t* buf2);
void buffer_destroy(buffer_t* buf);
uint8_t buffer_get_index(buffer_t* buf, size_t index);
uint8_t buffer_set_index(buffer_t* buf, size_t index, uint8_t value);
buffer_t* buffer_xor(buffer_t* buf1, buffer_t* buf2);
buffer_t* buffer_or(buffer_t* buf1, buffer_t* buf2);
buffer_t* buffer_and(buffer_t* buf1, buffer_t* buf2);
buffer_t* buffer_not(buffer_t* buf);
int8_t buffer_compare(buffer_t* buf1, buffer_t* buf2);
cbor_item_t* buffer_to_cbor(buffer_t* buf);
buffer_t* cbor_to_buffer(cbor_item_t* cbor);


#endif //LIBOFFS_BUFFER_H
