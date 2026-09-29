//
// Created by victor on 3/30/25.
//

#include "allocator.h"
#include <stdlib.h>
#include "log.h"

void* get_memory(size_t size) {
  void* mem = malloc(size);
  if (mem == NULL) {
    log_error("Out of memory");
    abort();
  }
  return mem;
}
void* get_clear_memory(size_t size) {
   void* mem =  calloc(1, size);
   if (mem == NULL) {
     log_error("Out of memory");
     abort();
   }
   return mem;
}