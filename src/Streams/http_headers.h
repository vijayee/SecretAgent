//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_HEADERS_H
#define OFFS_HTTP_HEADERS_H

#include "../Util/vec.h"

typedef struct {
  char* name;
  char* value;
} http_header_t;

typedef vec_t(http_header_t) vec_header_t;

typedef struct {
  vec_header_t headers;
} http_headers_t;

void http_headers_init(http_headers_t* headers);
void http_headers_deinit(http_headers_t* headers);
void http_headers_set(http_headers_t* headers, const char* name, const char* value);
const char* http_headers_get(http_headers_t* headers, const char* name);
void http_headers_remove(http_headers_t* headers, const char* name);
size_t http_headers_count(http_headers_t* headers);

#endif // OFFS_HTTP_HEADERS_H