//
// Created by victor on 5/7/26.
//
#include "http_headers.h"
#include <string.h>
#ifdef _WIN32
  #include "../Platform/platform_posix_compat.h"
#else
  #include <strings.h>
#endif

void http_headers_init(http_headers_t* headers) {
  vec_init(&headers->headers);
}

void http_headers_deinit(http_headers_t* headers) {
  for (int i = 0; i < headers->headers.length; i++) {
    free(headers->headers.data[i].name);
    free(headers->headers.data[i].value);
  }
  vec_deinit(&headers->headers);
}

void http_headers_set(http_headers_t* headers, const char* name, const char* value) {
  for (int i = 0; i < headers->headers.length; i++) {
    if (strcasecmp(headers->headers.data[i].name, name) == 0) {
      free(headers->headers.data[i].value);
      headers->headers.data[i].value = strdup(value);
      return;
    }
  }
  http_header_t header;
  header.name = strdup(name);
  header.value = strdup(value);
  vec_push(&headers->headers, header);
}

const char* http_headers_get(http_headers_t* headers, const char* name) {
  for (int i = 0; i < headers->headers.length; i++) {
    if (strcasecmp(headers->headers.data[i].name, name) == 0) {
      return headers->headers.data[i].value;
    }
  }
  return NULL;
}

void http_headers_remove(http_headers_t* headers, const char* name) {
  for (int i = 0; i < headers->headers.length; i++) {
    if (strcasecmp(headers->headers.data[i].name, name) == 0) {
      free(headers->headers.data[i].name);
      free(headers->headers.data[i].value);
      vec_splice(&headers->headers, i, 1);
      return;
    }
  }
}

size_t http_headers_count(http_headers_t* headers) {
  return (size_t)headers->headers.length;
}