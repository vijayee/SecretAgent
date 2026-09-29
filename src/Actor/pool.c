//
// Created by victor on 5/6/25.
//

#include "pool.h"
#include "../Platform/platform.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include <stdlib.h>
#include <string.h>

static pool_global_t _pool_global[POOL_COUNT];
static _Thread_local pool_local_t _pool_local[POOL_COUNT];
static ATOMIC(int) _pool_init_done = 0;
static _Atomic(platform_mutex_t*) _pool_init_mutex = NULL;

static void _pool_global_init(void) {
  for (size_t index = 0; index < POOL_COUNT; index++) {
    size_t used_size = pool_used_size(index);
    size_t threshold = POOL_ALIGN / used_size;
    if (threshold < POOL_MIN_THRESHOLD) {
      threshold = POOL_MIN_THRESHOLD;
    }
    if (threshold > POOL_MAX_THRESHOLD) {
      threshold = POOL_MAX_THRESHOLD;
    }
    _pool_global[index].head = NULL;
    _pool_global[index].count = threshold;
    _pool_global[index].lock = platform_mutex_create();
  }
}

static void _pool_global_init_once(void) {
  if (ATOMIC_LOAD(&_pool_init_done)) {
    return;
  }
  /* Create the init mutex with a benign race: any of the racing mutexes is
   * fine to use for serializing the actual init. If our local allocation
   * fails, fall back to whatever was previously installed (or just retry
   * the initialization with no guard — the worst case is double-init which
   * is idempotent because we re-check _pool_init_done under the guard). */
  platform_mutex_t* m = platform_mutex_create();
  platform_mutex_t* expected = NULL;
  if (!atomic_compare_exchange_strong(&_pool_init_mutex, &expected, m)) {
    /* Lost the race: another thread installed its mutex as the guard. The
     * winner's mutex is used for serialization; our local `m` is never
     * installed and never owned by anyone else, so destroying it here is
     * safe (no concurrent locker can hold it). Leaving it allocated would
     * leak one mutex per losing thread per first-init race. */
    platform_mutex_destroy(m);
  }
  platform_mutex_t* guard = atomic_load(&_pool_init_mutex);
  if (guard != NULL) {
    platform_mutex_lock(guard);
  }
  if (!ATOMIC_LOAD(&_pool_init_done)) {
    _pool_global_init();
    ATOMIC_STORE(&_pool_init_done, 1);
  }
  if (guard != NULL) {
    platform_mutex_unlock(guard);
  }
}

static void _pool_push_global(size_t index, pool_item_t* list_head) {
  platform_mutex_lock(_pool_global[index].lock);
  list_head->next = _pool_global[index].head;
  _pool_global[index].head = list_head;
  platform_mutex_unlock(_pool_global[index].lock);
}

static pool_item_t* _pool_pull_global(size_t index) {
  platform_mutex_lock(_pool_global[index].lock);
  pool_item_t* head = _pool_global[index].head;
  if (head != NULL) {
    _pool_global[index].head = NULL;
  }
  platform_mutex_unlock(_pool_global[index].lock);
  return head;
}

static void* _pool_alloc_block(size_t index) {
  size_t used_size = pool_used_size(index);
  if (used_size < POOL_ALIGN) {
    pool_local_t* local = &_pool_local[index];
    if (local->start == NULL || local->start >= local->end) {
      size_t block_size = POOL_ALIGN;
      local->start = get_clear_memory(block_size);
      local->end = local->start + block_size;
    }
    void* pointer = local->start;
    local->start += used_size;
    return pointer;
  }
  void* pointer = NULL;
#if defined(_MSC_VER)
  pointer = _aligned_malloc(used_size, POOL_ALIGN);
  if (pointer == NULL) {
    abort();
  }
#else
  if (posix_memalign(&pointer, POOL_ALIGN, used_size) != 0) {
    abort();
  }
#endif
  memset(pointer, 0, used_size);
  return pointer;
}

size_t pool_index(size_t size) {
  if (size == 0) {
    size = 1;
  }
  size_t bits = POOL_MIN_BITS;
  while ((1UL << bits) < size) {
    bits++;
  }
  size_t index = bits - POOL_MIN_BITS;
  if (index >= POOL_COUNT) {
    index = POOL_COUNT - 1;
  }
  return index;
}

size_t pool_used_size(size_t index) {
  return (1UL << (POOL_MIN_BITS + index));
}

void* pool_alloc(size_t index) {
  _pool_global_init_once();
  pool_local_t* local = &_pool_local[index];
  if (local->head != NULL) {
    pool_item_t* item = local->head;
    local->head = item->next;
    local->length--;
    return item;
  }
  pool_item_t* global_head = _pool_pull_global(index);
  if (global_head != NULL) {
    local->head = global_head->next;
    local->length = 0;
    return global_head;
  }
  return _pool_alloc_block(index);
}

void pool_free(size_t index, void* pointer) {
  if (pointer == NULL) {
    return;
  }
  _pool_global_init_once();
  pool_item_t* item = (pool_item_t*)pointer;
  pool_local_t* local = &_pool_local[index];
  item->next = local->head;
  local->head = item;
  local->length++;
  if (local->length >= _pool_global[index].count) {
    pool_item_t* list_head = local->head;
    local->head = NULL;
    local->length = 0;
    _pool_push_global(index, list_head);
  }
}

void pool_thread_cleanup(void) {
  for (size_t index = 0; index < POOL_COUNT; index++) {
    pool_local_t* local = &_pool_local[index];
    if (local->head != NULL) {
      _pool_push_global(index, local->head);
      local->head = NULL;
      local->length = 0;
    }
    local->start = NULL;
    local->end = NULL;
  }
}
