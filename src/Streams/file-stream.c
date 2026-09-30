#include "file-stream.h"
#include "../Util/allocator.h"
#include "../Util/error.h"
#include "../Buffer/buffer.h"
#include <string.h>

void _readable_pull_file_stream_on_pull(readable_pull_file_stream_t* stream);

readable_push_file_stream_t* readable_push_file_stream_create(scheduler_pool_t* pool, char* filename, size_t chunk_size, int* error_code) {
  *error_code = 0;
  readable_push_file_stream_t* rs= get_clear_memory(sizeof(readable_push_file_stream_t));

  rs->filename = strdup(filename);
  rs->chunk_size = chunk_size;
  rs->file = platform_file_open(rs->filename, PLATFORM_O_RDONLY, 0644);
  if (rs->file == NULL) {
    /* File open failed (e.g., file not found, permission denied). Set the
     * error code and return a non-NULL but unusable stream. Callers must
     * check `error_code` before using the stream; platform_file_* on the
     * NULL file handle will safely no-op. */
    *error_code = -1;
    stream_init((stream_t*) rs, push, readable_stream, 1, pool,
                (void(*)(stream_t*))readable_push_file_stream_destroy);
    stream_close_handler((stream_t*) rs,(void(*)(stream_t*))readable_push_file_stream_close);
    return rs;
  }
  int64_t file_size = platform_file_seek(rs->file, 0, PLATFORM_SEEK_END);
  rs->file_size = (int32_t)file_size;
  if (platform_file_seek(rs->file, 0, PLATFORM_SEEK_SET) < 0) {
    *error_code = -1;
  }
  stream_init((stream_t*) rs, push, readable_stream, 1, pool,
              (void(*)(stream_t*))readable_push_file_stream_destroy);
  readable_stream_push_handler((stream_t*) rs, (void (*)(stream_t*))readable_push_file_stream_push);
  stream_close_handler((stream_t*) rs,(void(*)(stream_t*))readable_push_file_stream_close);
  return rs;
}
void readable_push_file_stream_destroy(readable_push_file_stream_t* stream) {
  if (refcounter_dereference_is_zero((refcounter_t*) stream)) {
    platform_file_close(stream->file);
    stream_deinit((stream_t*) stream);
    free(stream->filename);
    free(stream);
  }
}

void readable_push_file_stream_push(readable_push_file_stream_t* stream) {
  if (stream->stream.is_deactivated == 1 || stream->file == NULL) {
    return;
  }
  int32_t diff = stream->file_size - stream->cursor;
  if (diff <= 0) {
    return;
  }
  size_t size;
  if (stream->chunk_size > (size_t)diff) {
    size = diff;
  } else {
    size = stream->chunk_size;
  }
  uint8_t* buf = get_memory(size);
  size_t bytes = platform_file_read(stream->file, buf, size);
  if (bytes != size) {
    free(buf);
    stream_notify((stream_t*)stream, error_event, OFFS_ERROR_TRANSFER("Invalid Read Size"), (void (*)(void*))error_destroy);
    return;
  }

  buffer_t* buffer = buffer_create_from_existing_memory(buf, size);

  stream->cursor += size;
  stream_notify((stream_t*)stream, data_event, CONSUME(buffer, buffer_t), (void (*)(void*))buffer_destroy);
  if (stream->file_size == stream->cursor) {
    stream_notify((stream_t*) stream, complete_event, NULL, NULL);
    stream_close((stream_t*) stream);
  } else {
    readable_push_stream_push((stream_t*) stream);
  }
}

void readable_push_file_stream_read(readable_push_file_stream_t* stream, size_t size, void* ctx, void (*cb)(void*, void*)) {
  if (stream->stream.is_deactivated == 1) {
    stream_notify((stream_t*)stream, error_event, OFFS_ERROR_TRANSFER("Stream is already destroyed"), (void (*)(void*))error_destroy);
  } else {
    int32_t diff = stream->file_size - stream->cursor;
    if (size > (size_t)diff) {
      size = (size_t)diff;
    }
    uint8_t* buf = get_memory(size);
    size_t bytes = platform_file_read(stream->file, buf, size);
    if (bytes != size) {
      free(buf);
      stream_notify((stream_t*)stream, error_event, OFFS_ERROR_TRANSFER("Invalid Read Size"), (void (*)(void*))error_destroy);
      return;
    }

    buffer_t* buffer = buffer_create_from_existing_memory(buf, size);

    stream->cursor += size;
    cb(ctx, CONSUME(buffer, buffer_t));
  }
}

void readable_push_file_stream_close(readable_push_file_stream_t* stream) {
  uint8_t deactivated = stream->stream.is_deactivated;
  if (deactivated == 0) {
    stream->stream.is_deactivated = 1;
    stream->stream.is_piped = 0;
    stream_unsubscribe_pipe_notifiers((stream_t*) stream);
    stream_notify((stream_t*) stream, close_event, NULL, NULL);
  }
}

writeable_push_file_stream_t* writeable_push_file_stream_create(scheduler_pool_t* pool, char* filename) {
  writeable_push_file_stream_t* ws = get_clear_memory(sizeof(writeable_push_file_stream_t));
  stream_init((stream_t*) ws, push, writeable_stream, 0, pool, (void(*)(stream_t*)) writeable_push_file_stream_destroy);
  writeable_stream_write_handler((stream_t*) ws, (void (*)(stream_t*, void*)) writeable_push_file_stream_write);
  stream_close_handler((stream_t*) ws, (void(*)(stream_t*))writeable_push_file_stream_close);
  ws->filename = strdup(filename);
  ws->file = platform_file_open(ws->filename, PLATFORM_O_WRONLY | PLATFORM_O_CREAT | PLATFORM_O_TRUNC, 0644);
  return ws;
}

void writeable_push_file_stream_write(writeable_push_file_stream_t* stream, buffer_t* data) {
  if (stream->file == NULL) {
    DESTROY(data, buffer);
    return;
  }
  platform_file_write(stream->file, data->data, data->size);
  DESTROY(data, buffer);
}
void writeable_push_file_stream_destroy(writeable_push_file_stream_t* stream) {
  if (refcounter_dereference_is_zero((refcounter_t*) stream)) {
    platform_file_close(stream->file);
    stream_deinit((stream_t*) stream);
    free(stream->filename);
    free(stream);
  }
}
void writeable_push_file_stream_close(writeable_push_file_stream_t* stream) {
  uint8_t deactivated = stream->stream.is_deactivated;
  if (deactivated == 0) {
    stream->stream.is_deactivated = 1;
    stream->stream.is_piped = 0;
    stream_unsubscribe_pipe_notifiers((stream_t*) stream);
    stream_notify((stream_t*) stream, close_event, NULL, NULL);
  }
}

readable_pull_file_stream_t* readable_pull_file_stream_create(scheduler_pool_t* pool, char* filename, size_t chunk_size, int* error_code) {
  *error_code = 0;
  readable_pull_file_stream_t* rs= get_clear_memory(sizeof(readable_pull_file_stream_t));

  rs->filename = strdup(filename);
  rs->chunk_size = chunk_size;
  rs->file = platform_file_open(rs->filename, PLATFORM_O_RDONLY, 0644);
  if (rs->file == NULL) {
    *error_code = -1;
    stream_init((stream_t*) rs, pull, readable_stream, 0, pool,
                (void(*)(stream_t*))readable_pull_file_stream_destroy);
    stream_close_handler((stream_t*) rs,(void(*)(stream_t*))readable_pull_file_stream_close);
    return rs;
  }
  int64_t file_size = platform_file_seek(rs->file, 0, PLATFORM_SEEK_END);
  rs->file_size = (int32_t)file_size;
  if (platform_file_seek(rs->file, 0, PLATFORM_SEEK_SET) < 0) {
    *error_code = -1;
  }
  stream_init((stream_t*) rs, pull, readable_stream, 0, pool,
              (void(*)(stream_t*))readable_pull_file_stream_destroy);
  stream_close_handler((stream_t*) rs,(void(*)(stream_t*))readable_pull_file_stream_close);
  readable_stream_pull_handler((stream_t*)rs,(void(*)(stream_t*)) _readable_pull_file_stream_on_pull);
  return rs;
}

void readable_pull_file_stream_destroy(readable_pull_file_stream_t* stream) {
  if (refcounter_dereference_is_zero((refcounter_t*) stream)) {
    platform_file_close(stream->file);
    stream_deinit((stream_t*) stream);
    free(stream->filename);
    free(stream);
  }
}

void _readable_pull_file_stream_on_pull(readable_pull_file_stream_t* stream) {
  if (stream->stream.is_deactivated == 1) {
    return;
  }
  int32_t diff = stream->file_size - stream->cursor;
  if (diff <= 0) {
    return;
  }
    size_t size;
    if (stream->chunk_size > (size_t)diff) {
      size = diff;
    } else {
      size = stream->chunk_size;
    }
    uint8_t* buf = get_memory(size);
    size_t bytes = platform_file_read(stream->file, buf, size);
    if (bytes != size) {
      free(buf);
      stream_notify((stream_t*)stream, error_event, OFFS_ERROR_TRANSFER("Invalid Read Size"), (void (*)(void*))error_destroy);
      return;
    }

    buffer_t* buffer = buffer_create_from_existing_memory(buf, size);

    stream->cursor += size;
    stream_notify((stream_t*)stream, data_event, CONSUME(buffer, buffer_t), (void (*)(void*))buffer_destroy);
    if (stream->file_size == stream->cursor) {
      stream_notify((stream_t*) stream, complete_event, NULL, NULL);
      stream_close((stream_t*) stream);
    }
}

void readable_pull_file_stream_close(readable_pull_file_stream_t* stream) {
  uint8_t deactivated = stream->stream.is_deactivated;
  if (deactivated == 0) {
    stream->stream.is_deactivated = 1;
    stream->stream.is_piped = 0;
    stream_unsubscribe_pipe_notifiers((stream_t*) stream);
    stream_notify((stream_t*) stream, close_event, NULL, NULL);
  }
}

writeable_pull_file_stream_t* writeable_pull_file_stream_create(scheduler_pool_t* pool, char* filename) {
  writeable_pull_file_stream_t* ws = get_clear_memory(sizeof(writeable_pull_file_stream_t));
  stream_init((stream_t*) ws, pull, writeable_stream, 0, pool, (void(*)(stream_t*)) writeable_pull_file_stream_destroy);
  writeable_stream_write_handler((stream_t*) ws, (void (*)(stream_t*, void*)) writeable_pull_file_stream_write);
  stream_close_handler((stream_t*) ws, (void(*)(stream_t*))writeable_pull_file_stream_close);
  ws->filename = strdup(filename);
  ws->file = platform_file_open(ws->filename, PLATFORM_O_WRONLY | PLATFORM_O_CREAT | PLATFORM_O_TRUNC, 0644);
  return ws;
}

void writeable_pull_file_stream_write(writeable_pull_file_stream_t* stream, buffer_t* data) {
  if (stream->file == NULL) {
    DESTROY(data, buffer);
    return;
  }
  platform_file_write(stream->file, data->data, data->size);
  DESTROY(data, buffer);
}
void writeable_pull_file_stream_destroy(writeable_pull_file_stream_t* stream) {
  if (refcounter_dereference_is_zero((refcounter_t*) stream)) {
    platform_file_close(stream->file);
    stream_deinit((stream_t*) stream);
    free(stream->filename);
    free(stream);
  }
}
void writeable_pull_file_stream_close(writeable_pull_file_stream_t* stream) {
  uint8_t deactivated = stream->stream.is_deactivated;
  if (deactivated == 0) {
    stream->stream.is_deactivated = 1;
    stream->stream.is_piped = 0;
    stream_unsubscribe_pipe_notifiers((stream_t*) stream);
    stream_notify((stream_t*) stream, close_event, NULL, NULL);
  }
}