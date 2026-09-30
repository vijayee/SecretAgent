//
// Created by victor on 5/8/26.
//
#ifndef OFFS_CORS_H
#define OFFS_CORS_H

#include "http_server.h"
#include "http_request.h"
#include "http_response.h"

typedef struct {
  char* allow_origin;
  char* allow_methods;
  char* allow_headers;
  char* max_age;
  uint8_t allow_credentials;
} cors_config_t;

cors_config_t* cors_config_default(void);
cors_config_t* cors_config_offsystem(void);
void cors_config_destroy(cors_config_t* config);

int cors_middleware(http_request_t* request, http_response_t* response, void* user_data);

#endif // OFFS_CORS_H