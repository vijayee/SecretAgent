//
// Created by victor on 2/4/26.
//

#ifndef OFFS_FILE_STREAM_H
#define OFFS_FILE_STREAM_H
#include "stream.h"
#include <stdio.h>
#include "../Buffer/buffer.h"
#include "../Platform/platform.h"

#define DEFAULT_CHUNK_SIZE 128000

typedef struct {
  stream_t stream;
  size_t chunk_size;
  platform_file_t* file;
  char* filename;
  int32_t file_size;
  int32_t cursor;
} readable_push_file_stream_t;
readable_push_file_stream_t* readable_push_file_stream_create(scheduler_pool_t* pool, char* filename, size_t chunk_size, int* error_code);
void readable_push_file_stream_destroy(readable_push_file_stream_t* stream);
void readable_push_file_stream_push(readable_push_file_stream_t* stream);
void readable_push_file_stream_close(readable_push_file_stream_t* stream);

typedef struct {
  stream_t stream;
  platform_file_t* file;
  char* filename;
  int32_t cursor;
} writeable_push_file_stream_t;

writeable_push_file_stream_t* writeable_push_file_stream_create(scheduler_pool_t* pool, char* filename);
void writeable_push_file_stream_write(writeable_push_file_stream_t* stream, buffer_t* data);
void writeable_push_file_stream_destroy(writeable_push_file_stream_t* stream);
void writeable_push_file_stream_close(writeable_push_file_stream_t* stream);


typedef struct {
  stream_t stream;
  size_t chunk_size;
  platform_file_t* file;
  char* filename;
  int32_t file_size;
  int32_t cursor;
} readable_pull_file_stream_t;

readable_pull_file_stream_t* readable_pull_file_stream_create(scheduler_pool_t* pool, char* filename, size_t chunk_size, int* error_code);
void readable_pull_file_stream_destroy(readable_pull_file_stream_t* stream);
void readable_pull_file_stream_close(readable_pull_file_stream_t* stream);


typedef struct {
  stream_t stream;
  platform_file_t* file;
  char* filename;
  int32_t cursor;
} writeable_pull_file_stream_t;

writeable_pull_file_stream_t* writeable_pull_file_stream_create(scheduler_pool_t* pool, char* filename);
void writeable_pull_file_stream_write(writeable_pull_file_stream_t* stream, buffer_t* data);
void writeable_pull_file_stream_pull(writeable_pull_file_stream_t* stream);
void writeable_pull_file_stream_destroy(writeable_pull_file_stream_t* stream);
void writeable_pull_file_stream_close(writeable_pull_file_stream_t* stream);

#endif //OFFS_FILE_STREAM_H
