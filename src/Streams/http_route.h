//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_ROUTE_H
#define OFFS_HTTP_ROUTE_H

#ifdef _WIN32
  /* POSIX <regex.h> has no equivalent in the Windows SDK. The
   * platform_regex_compat shim provides a minimal literal-match API. */
  #include "../Platform/platform_regex_compat.h"
#else
  #include <regex.h>
#endif
#include "../Util/vec.h"

typedef struct http_request_t http_request_t;
typedef struct http_response_t http_response_t;
typedef struct http_connection_t http_connection_t;

typedef void (*http_handler_t)(http_request_t* request, http_response_t* response, void* user_data);
typedef int (*http_headers_complete_handler_t)(http_connection_t* connection,
                                                http_request_t* request,
                                                http_response_t* response);

typedef struct {
  int method;
  regex_t pattern;
  http_handler_t handler;
  void* user_data;
  void (*user_data_destroy)(void*);
  http_headers_complete_handler_t headers_complete_handler;
} http_route_t;

typedef vec_t(http_route_t) vec_route_t;

typedef struct {
  char* match;
  size_t start;
  size_t length;
} http_route_capture_t;

typedef vec_t(http_route_capture_t) vec_capture_t;

int http_route_init(http_route_t* route, int method, const char* pattern,
                    http_handler_t handler, void* user_data, void (*user_data_destroy)(void*));
void http_route_deinit(http_route_t* route);
int http_route_match(http_route_t* route, int method, const char* path,
                     vec_capture_t* captures);
void http_route_dispatch(http_route_t* route, http_request_t* request, http_response_t* response);
void vec_capture_deinit(vec_capture_t* captures);

#endif // OFFS_HTTP_ROUTE_H