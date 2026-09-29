//
// Created by victor on 5/6/25.
//

#ifndef SA_DEQUE_H
#define SA_DEQUE_H

#include "../Util/atomic_compat.h"
#include <stddef.h>
#include <stdbool.h>

#define DEQUE_INITIAL_CAPACITY 1024
#define DEQUE_MIN_CAPACITY 8
#define DEQUE_EMPTY ((void*)0)
#define DEQUE_ABORT ((void*)1)

typedef struct _deque_array_t {
  size_t size;
  struct _deque_array_t* next;
  ATOMIC(void*) buffer[];
} _deque_array_t;

typedef struct deque_t {
  ATOMIC(size_t) bottom;
  ATOMIC(size_t) top;
  ATOMIC(struct _deque_array_t*) array;
} deque_t;

void deque_init(deque_t* deque);
void deque_destroy(deque_t* deque);
void deque_push(deque_t* deque, void* item);
void* deque_pop(deque_t* deque);
void* deque_steal(deque_t* deque);
bool deque_isempty(deque_t* deque);
size_t deque_size(deque_t* deque);

#endif // SA_DEQUE_H
