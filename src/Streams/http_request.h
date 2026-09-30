//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_REQUEST_H
#define OFFS_HTTP_REQUEST_H

#include "stream.h"
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include "../Util/vec.h"
#include "http_headers.h"
#include "http_route.h"
#include <http_parser.h>

typedef struct http_request_t {
  stream_t stream;
  int method;
  char* url;
  char* path;
  char* query_string;
  http_headers_t headers;
  buffer_t* body;
  uint64_t content_length;
  uint8_t keep_alive;
  vec_capture_t params;
  uint8_t headers_complete;
  uint8_t message_complete;
  uint8_t is_authenticated;
} http_request_t;

http_request_t* http_request_create(scheduler_pool_t* pool);
void http_request_destroy(http_request_t* request);

int http_request_method(http_request_t* request);
const char* http_request_header(http_request_t* request, const char* name);
const char* http_request_param(http_request_t* request, size_t index);

#endif // OFFS_HTTP_REQUEST_H