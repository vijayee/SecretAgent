//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_RESPONSE_H
#define OFFS_HTTP_RESPONSE_H

#include "stream.h"
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include "http_headers.h"
#include "http_status.h"

typedef struct http_connection_t http_connection_t;

typedef struct http_response_t {
  stream_t stream;
  uint16_t status_code;
  http_headers_t headers;
  uint8_t headers_sent;
  uint8_t is_piped;
  uint8_t keep_alive;
  /* 1 = the body length is not known until the response ends (e.g. the load
     ndjson surface). _send_headers must NOT stamp a Content-Length from the
     accumulated body_length; the response is close-delimited, and the caller
     forces keep_alive = 0 before the first write so it terminates the
     connection at http_response_end. */
  uint8_t unknown_length;
  size_t body_length;
  http_connection_t* connection;
} http_response_t;

http_response_t* http_response_create(scheduler_pool_t* pool, http_connection_t* connection);
void http_response_destroy(http_response_t* response);

void http_response_set_status(http_response_t* response, uint16_t status);
void http_response_set_header(http_response_t* response, const char* name, const char* value);
void http_response_write(http_response_t* response, const char* data, size_t length);
void http_response_end(http_response_t* response);

void http_response_pipe(http_response_t* response, stream_t* source);

const char* mime_type_from_extension(const char* filename);

#endif // OFFS_HTTP_RESPONSE_H