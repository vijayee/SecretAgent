//
// Created by victor on 5/6/25.
//

#include "deque.h"
#include "../Util/allocator.h"
#include <stdlib.h>

static _deque_array_t* _deque_array_create(size_t size) {
  _deque_array_t* array = get_clear_memory(sizeof(_deque_array_t) + size * sizeof(void*));
  array->size = size;
  array->next = NULL;
  return array;
}

static void _deque_grow(deque_t* deque, _deque_array_t* old_array, size_t top, size_t bottom) {
  size_t new_size = old_array->size << 1;
  _deque_array_t* new_array = _deque_array_create(new_size);
  for (size_t index = top; index < bottom; index++) {
    size_t old_index = index & (old_array->size - 1);
    size_t new_index = index & (new_size - 1);
    void* item = atomic_load_explicit(&old_array->buffer[old_index], memory_order_relaxed);
    atomic_store_explicit(&new_array->buffer[new_index], item, memory_order_relaxed);
  }
  /* Old arrays are kept alive via the linked list because in-flight thieves
     may still hold references to them. They are never freed in this
     implementation — acceptable for a scheduler deque where growth is rare
     and bounded by the number of worker threads. */
  new_array->next = old_array;
  atomic_store_explicit(&deque->array, new_array, memory_order_release);
}

void deque_init(deque_t* deque) {
  _deque_array_t* array = _deque_array_create(DEQUE_INITIAL_CAPACITY);
  atomic_store_explicit(&deque->bottom, 0, memory_order_relaxed);
  atomic_store_explicit(&deque->top, 0, memory_order_relaxed);
  atomic_store_explicit(&deque->array, array, memory_order_relaxed);
}

void deque_destroy(deque_t* deque) {
  _deque_array_t* array = atomic_load_explicit(&deque->array, memory_order_relaxed);
  while (array != NULL) {
    _deque_array_t* next = array->next;
    free(array);
    array = next;
  }
}

void deque_push(deque_t* deque, void* item) {
  size_t bottom = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
  size_t top = atomic_load_explicit(&deque->top, memory_order_acquire);
  _deque_array_t* array = atomic_load_explicit(&deque->array, memory_order_relaxed);
  if (bottom - top >= array->size) {
    _deque_grow(deque, array, top, bottom);
    array = atomic_load_explicit(&deque->array, memory_order_relaxed);
  }
  size_t mask = array->size - 1;
  atomic_store_explicit(&array->buffer[bottom & mask], item, memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
  atomic_store_explicit(&deque->bottom, bottom + 1, memory_order_relaxed);
}

void* deque_pop(deque_t* deque) {
  size_t bottom = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
  size_t top = atomic_load_explicit(&deque->top, memory_order_acquire);
  if (bottom <= top) {
    return DEQUE_EMPTY;
  }
  bottom = bottom - 1;
  _deque_array_t* array = atomic_load_explicit(&deque->array, memory_order_relaxed);
  atomic_store_explicit(&deque->bottom, bottom, memory_order_relaxed);
  atomic_thread_fence(memory_order_seq_cst);
  top = atomic_load_explicit(&deque->top, memory_order_relaxed);
  void* item = DEQUE_EMPTY;
  if (top <= bottom) {
    size_t mask = array->size - 1;
    item = atomic_load_explicit(&array->buffer[bottom & mask], memory_order_relaxed);
    if (top == bottom) {
      if (!atomic_compare_exchange_strong_explicit(&deque->top, &top, top + 1,
              memory_order_seq_cst, memory_order_relaxed)) {
        item = DEQUE_ABORT;
      }
      atomic_store_explicit(&deque->bottom, bottom + 1, memory_order_relaxed);
    }
  } else {
    atomic_store_explicit(&deque->bottom, bottom + 1, memory_order_relaxed);
  }
  return item;
}

void* deque_steal(deque_t* deque) {
  size_t top = atomic_load_explicit(&deque->top, memory_order_acquire);
  atomic_thread_fence(memory_order_seq_cst);
  size_t bottom = atomic_load_explicit(&deque->bottom, memory_order_acquire);
  if (top >= bottom) {
    return DEQUE_EMPTY;
  }
  _deque_array_t* array = atomic_load_explicit(&deque->array, memory_order_consume);
  size_t mask = array->size - 1;
  void* item = atomic_load_explicit(&array->buffer[top & mask], memory_order_relaxed);
  if (!atomic_compare_exchange_strong_explicit(&deque->top, &top, top + 1,
          memory_order_seq_cst, memory_order_relaxed)) {
    return DEQUE_ABORT;
  }
  return item;
}

bool deque_isempty(deque_t* deque) {
  size_t bottom = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
  size_t top = atomic_load_explicit(&deque->top, memory_order_relaxed);
  return bottom <= top;
}

size_t deque_size(deque_t* deque) {
  size_t bottom = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
  size_t top = atomic_load_explicit(&deque->top, memory_order_relaxed);
  return bottom > top ? bottom - top : 0;
}
