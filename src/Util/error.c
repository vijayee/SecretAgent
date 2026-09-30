//
// Created by victor on 4/7/25.
//
#include "error.h"
#include "allocator.h"
#include <string.h>

async_error_t* error_create(char* message, char* file, char* function, int line) {
  if ((message == NULL) || (file == NULL) || (function == NULL)) {
    return NULL;
  }
  async_error_t* error = get_clear_memory(sizeof(async_error_t));
  size_t message_len = strlen(message) + 1;
  error->message = get_memory(message_len);
  memcpy(error->message, message, message_len);
  size_t file_len = strlen(file) + 1;
  error->file = get_memory(file_len);
  memcpy(error->file, file, file_len);
  size_t function_len = strlen(function) + 1;
  error->function = get_memory(function_len);
  memcpy(error->function, function, function_len);
  error->line = line;
  refcounter_init((refcounter_t*) error);
  return error;
}
void error_destroy(async_error_t* error) {
  if (error == NULL) return;
  if (refcounter_dereference_is_zero((refcounter_t*) error)) {
    free(error->message);
    free(error->file);
    free(error->function);
    refcounter_destroy_lock((refcounter_t*) error);
    free(error);
  }
}

async_error_t* offs_error_transfer(char* message, char* file, char* function, int line) {
  async_error_t* error = error_create(message, file, function, line);
  if (error != NULL) {
    refcounter_yield((refcounter_t*) error);
  }
  return error;
}