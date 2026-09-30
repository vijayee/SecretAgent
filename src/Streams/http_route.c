//
// Created by victor on 5/7/26.
//
#include "http_route.h"
#include "http_request.h"
#include "http_response.h"
#include "../Util/allocator.h"
#include <string.h>
#include <stdio.h>

int http_route_init(http_route_t* route, int method, const char* pattern,
                    http_handler_t handler, void* user_data, void (*user_data_destroy)(void*)) {
  route->method = method;
  route->handler = handler;
  route->user_data = user_data;
  route->user_data_destroy = user_data_destroy;
  route->headers_complete_handler = NULL;
  int result = regcomp(&route->pattern, pattern, REG_EXTENDED);
  if (result != 0) {
    char error_buffer[256];
    regerror(result, &route->pattern, error_buffer, sizeof(error_buffer));
    fprintf(stderr, "http_route_init: regex compilation failed: %s\n", error_buffer);
    return result;
  }
  return 0;
}

void http_route_deinit(http_route_t* route) {
  regfree(&route->pattern);
  if (route->user_data_destroy != NULL && route->user_data != NULL) {
    route->user_data_destroy(route->user_data);
  }
}

int http_route_match(http_route_t* route, int method, const char* path,
                     vec_capture_t* captures) {
  if (route->method != method) {
    return 0;
  }

  size_t num_matches = route->pattern.re_nsub + 1;
  regmatch_t* matches = get_memory(sizeof(regmatch_t) * num_matches);
  int result = regexec(&route->pattern, path, num_matches, matches, 0);
  if (result != 0) {
    free(matches);
    return 0;
  }

  vec_init(captures);
  for (size_t i = 0; i < num_matches; i++) {
    if (matches[i].rm_so == -1) {
      continue;
    }
    http_route_capture_t capture;
    capture.start = (size_t)matches[i].rm_so;
    capture.length = (size_t)(matches[i].rm_eo - matches[i].rm_so);
    capture.match = get_memory(capture.length + 1);
    memcpy(capture.match, path + capture.start, capture.length);
    capture.match[capture.length] = '\0';
    vec_push(captures, capture);
  }
  free(matches);
  return 1;
}

void http_route_dispatch(http_route_t* route, http_request_t* request, http_response_t* response) {
  if (route->handler != NULL) {
    route->handler(request, response, route->user_data);
  }
}

void vec_capture_deinit(vec_capture_t* captures) {
  for (int i = 0; i < captures->length; i++) {
    free(captures->data[i].match);
  }
  vec_deinit(captures);
}