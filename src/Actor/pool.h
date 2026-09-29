//
// Created by victor on 5/6/25.
//

#ifndef SA_POOL_H
#define SA_POOL_H

#include "../Platform/platform.h"
#include <stddef.h>
#include <stdint.h>

#define POOL_MIN_BITS     4
#define POOL_MAX_BITS     11
#define POOL_COUNT        (POOL_MAX_BITS - POOL_MIN_BITS + 1)
#define POOL_ALIGN        1024
#define POOL_MIN_THRESHOLD 32
#define POOL_MAX_THRESHOLD 1024

typedef struct pool_item_t {
  struct pool_item_t* next;
} pool_item_t;

typedef struct pool_local_t {
  pool_item_t* head;
  size_t length;
  char* start;
  char* end;
} pool_local_t;

typedef struct pool_global_t {
  pool_item_t* head;
  size_t count;
  platform_mutex_t* lock;
} pool_global_t;

void*  pool_alloc(size_t index);
void   pool_free(size_t index, void* pointer);
size_t pool_index(size_t size);
size_t pool_used_size(size_t index);
void   pool_thread_cleanup(void);

#define POOL_ALLOC(TYPE)  ((TYPE*)pool_alloc(pool_index(sizeof(TYPE))))
#define POOL_FREE(TYPE, VALUE)  pool_free(pool_index(sizeof(TYPE)), (VALUE))

#endif // SA_POOL_H
