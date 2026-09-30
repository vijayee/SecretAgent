//
// Created by victor on 5/8/26.
//
#include "cors.h"
#include "../Util/allocator.h"
#include <string.h>

cors_config_t* cors_config_default(void) {
  cors_config_t* config = get_clear_memory(sizeof(cors_config_t));
  config->allow_origin = strdup("*");
  config->allow_methods = strdup("GET, PUT, POST, DELETE, OPTIONS");
  config->allow_headers = strdup("Content-Type");
  config->max_age = strdup("86400");
  config->allow_credentials = 0;
  return config;
}

cors_config_t* cors_config_offsystem(void) {
  cors_config_t* config = get_clear_memory(sizeof(cors_config_t));
  config->allow_origin = strdup("*");
  config->allow_methods = strdup("GET, PUT, POST, DELETE, OPTIONS");
  config->allow_headers = strdup("Content-Type, Range, type, file-name, stream-length, server-address");
  config->max_age = strdup("86400");
  config->allow_credentials = 0;
  return config;
}

void cors_config_destroy(cors_config_t* config) {
  if (config == NULL) return;
  free(config->allow_origin);
  free(config->allow_methods);
  free(config->allow_headers);
  free(config->max_age);
  free(config);
}

int cors_middleware(http_request_t* request, http_response_t* response, void* user_data) {
  cors_config_t* config = (cors_config_t*)user_data;

  // Set common headers on every response
  http_response_set_header(response, "Access-Control-Allow-Origin", config->allow_origin);
  http_response_set_header(response, "Access-Control-Expose-Headers", "Content-Type, Content-Range, Content-Length");

  // Handle OPTIONS preflight
  if (request->method == HTTP_OPTIONS) {
    http_response_set_header(response, "Access-Control-Allow-Methods", config->allow_methods);
    http_response_set_header(response, "Access-Control-Allow-Headers", config->allow_headers);
    http_response_set_header(response, "Access-Control-Max-Age", config->max_age);
    if (config->allow_credentials) {
      http_response_set_header(response, "Access-Control-Allow-Credentials", "true");
    }
    http_response_set_status(response, 204);
    http_response_end(response);
    return 1;  // stop middleware chain
  }

  return 0;  // continue to next middleware / route handler
}