//
// Created by victor on 5/7/26.
//
#include "http_response.h"
#include "http_connection.h"
#include "http_request.h"
#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Buffer/buffer.h"
#include <poll-dancer/poll-dancer.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
  #include "../Platform/platform_posix_compat.h"
  #include <winsock2.h>
#else
  #include <unistd.h>
  #include <sys/socket.h>
#endif

void _http_response_dispatch(void* state, message_t* msg);

static void _send_headers(http_response_t* response) {
  if (response->headers_sent) {
    return;
  }
  response->headers_sent = 1;

  /* The body length of an unknown-length response only settles when the
     response ends — emit close-delimited framing instead of stamping a
     Content-Length from the bytes written so far. */
  if (!response->unknown_length &&
      http_headers_get(&response->headers, "Content-Length") == NULL) {
    char content_length_str[32];
    snprintf(content_length_str, sizeof(content_length_str), "%zu", response->body_length);
    http_headers_set(&response->headers, "Content-Length", content_length_str);
  }

  if (http_headers_get(&response->headers, "Connection") == NULL) {
    http_headers_set(&response->headers, "Connection", response->keep_alive ? "keep-alive" : "close");
  }

  const char* phrase = http_status_str((enum http_status)response->status_code);
  char status_line[256];
  int line_len = snprintf(status_line, sizeof(status_line),
                           "HTTP/1.1 %d %s\r\n",
                           response->status_code, phrase);
  http_connection_write(response->connection, status_line, (size_t)line_len);

  for (int i = 0; i < response->headers.headers.length; i++) {
    http_header_t* header = &response->headers.headers.data[i];
    char header_line[1024];
    int header_len = snprintf(header_line, sizeof(header_line),
                              "%s: %s\r\n",
                              header->name, header->value);
    http_connection_write(response->connection, header_line, (size_t)header_len);
  }

  http_connection_write(response->connection, "\r\n", 2);
}

http_response_t* http_response_create(scheduler_pool_t* pool, http_connection_t* connection) {
  http_response_t* response = get_clear_memory(sizeof(http_response_t));
  refcounter_init((refcounter_t*)response);
  stream_init((stream_t*)response, push, writeable_stream, 0, pool,
              (void (*)(stream_t*))http_response_destroy);
  /* stream_t is the first member of http_response_t, so (stream_t*)response
     aliases response->stream: stream_init registered and scheduled
     response->stream.actor, and all stream machinery operates on that one
     actor. Give it the response-specific dispatch so streamed writes route
     through _http_response_dispatch. */
  response->stream.actor.state = response;
  response->stream.actor.dispatch = _http_response_dispatch;
  response->status_code = HTTP_STATUS_OK;
  http_headers_init(&response->headers);
  response->headers_sent = 0;
  response->keep_alive = connection && connection->request ? connection->request->keep_alive : 0;
  response->connection = connection;
  return response;
}

void http_response_destroy(http_response_t* response) {
  if (response == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*)response)) {
    http_headers_deinit(&response->headers);
    stream_deinit((stream_t*)response);
    free(response);
  }
}

void _http_response_dispatch(void* state, message_t* msg) {
  (void)state;
  (void)msg;
  switch (msg->type) {
    default:
      break;
  }
}

void http_response_set_status(http_response_t* response, uint16_t status) {
  response->status_code = status;
}

void http_response_set_header(http_response_t* response, const char* name, const char* value) {
  http_headers_set(&response->headers, name, value);
}

void http_response_write(http_response_t* response, const char* data, size_t length) {
  if (response->connection == NULL) {
    return;
  }
  response->body_length += length;
  _send_headers(response);
  http_connection_write(response->connection, data, length);
}

void http_response_end(http_response_t* response) {
  if (response->connection == NULL) {
    return;
  }
  _send_headers(response);
  if (!response->keep_alive) {
    http_connection_close(response->connection);
  }
}

static void _pipe_on_data(void* ctx, void* chunk) {
    http_response_t* response = (http_response_t*)ctx;
    buffer_t* buf = (buffer_t*)chunk;
    /* Defensive sentinel: historically a heap-corruption bug left buffer->data
       NULL in the response piping path (see docs/OPERATIONS.md "Known issues").
       The root cause is under investigation; guard the entry so a corrupted
       chunk logs and drops instead of dereferencing NULL in http_response_write. */
    if (buf == NULL || buf->data == NULL) {
        log_error("_pipe_on_data: NULL buf or buf->data (buf=%p data=%p size=%zu) — heap corruption suspected",
                 (void*)buf, buf ? (void*)buf->data : NULL,
                 buf ? (size_t)buf->size : (size_t)0);
        return;
    }
    http_response_write(response, (const char*)buf->data, buf->size);
}

static void _pipe_on_close(void* ctx, void* unused) {
    (void)unused;
    http_response_t* response = (http_response_t*)ctx;
    http_connection_t* conn = response->connection;
    uint8_t keep_alive = response->keep_alive;
    http_response_end(response);
    response->connection = NULL;
    http_response_destroy(response);
    if (conn) {
        if (keep_alive) {
            conn->piped_pending = 0;
        }
        http_connection_destroy(conn);
    }
}

static void _pipe_on_error(void* ctx, async_error_t* error) {
    (void)error;
    http_response_t* response = (http_response_t*)ctx;
    if (!response->headers_sent) {
        http_response_set_status(response, 404);
        http_response_set_header(response, "Content-Length", "0");
    }
    http_response_end(response);
    /* Do NOT destroy response/connection here.  _pipe_on_close fires
       immediately after (stream_deactivate sends error then close) and
       owns the final cleanup.  Neither pointer is nulled so that
       _pipe_on_close can access them safely. */
}

void http_response_pipe(http_response_t* response, stream_t* source) {
    if (!response || !source) return;
    response->is_piped = 1;
    response->connection->piped_pending = 1;
    refcounter_reference((refcounter_t*)response->connection);
    refcounter_reference((refcounter_t*)response);
    stream_subscribe(source, data_event, response,
                     (void(*)(void*, void*))_pipe_on_data, NULL);
    stream_once(source, close_event, response,
                (void(*)(void*, void*))_pipe_on_close, NULL);
    stream_once(source, error_event, response,
                (void(*)(void*, void*))_pipe_on_error, NULL);
}

static const char* _mime_map[][2] = {
    {"html", "text/html"},
    {"htm", "text/html"},
    {"css", "text/css"},
    {"js", "application/javascript"},
    {"json", "application/json"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"webp", "image/webp"},
    {"bmp", "image/bmp"},
    {"tiff", "image/tiff"},
    {"tif", "image/tiff"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"mkv", "video/x-matroska"},
    {"avi", "video/x-msvideo"},
    {"mov", "video/quicktime"},
    {"wmv", "video/x-ms-wmv"},
    {"flv", "video/x-flv"},
    {"mp3", "audio/mpeg"},
    {"ogg", "audio/ogg"},
    {"wav", "audio/wav"},
    {"flac", "audio/flac"},
    {"aac", "audio/aac"},
    {"m4a", "audio/mp4"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"pdf", "application/pdf"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
    {"tar", "application/x-tar"},
    {"rar", "application/vnd.rar"},
    {"7z", "application/x-7z-compressed"},
    {"doc", "application/msword"},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"xls", "application/vnd.ms-excel"},
    {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"ppt", "application/vnd.ms-powerpoint"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"txt", "text/plain"},
    {"csv", "text/csv"},
    {"xml", "application/xml"},
    {"md", "text/markdown"},
    {"ofd", "application/cbor"},
    {NULL, NULL}
};

const char* mime_type_from_extension(const char* filename) {
    if (!filename) return "application/octet-stream";
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "application/octet-stream";
    const char* ext = dot + 1;
    for (int i = 0; _mime_map[i][0] != NULL; i++) {
        if (strcasecmp(ext, _mime_map[i][0]) == 0) {
            return _mime_map[i][1];
        }
    }
    return "application/octet-stream";
}