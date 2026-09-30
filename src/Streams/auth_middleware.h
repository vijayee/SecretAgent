//
// Created by victor on 5/22/26.
//

#ifndef OFFS_AUTH_MIDDLEWARE_H
#define OFFS_AUTH_MIDDLEWARE_H

#include "http_server.h"
#include <stdbool.h>

typedef struct auth_middleware_t auth_middleware_t;

/* Create an auth middleware that checks Bearer tokens against a bcrypt hash.
   bcrypt_hash is the stored hash. When allow_local_no_auth is true and the
   server is bound to a loopback address, bearer auth is skipped (the
   opt-out for local-only deployments); otherwise bearer is required even on
   loopback. server may be NULL (bearer always required). Returns NULL if
   bcrypt_hash is NULL. */
auth_middleware_t* auth_middleware_create(const char* bcrypt_hash,
                                          bool allow_local_no_auth,
                                          http_server_t* server);

void auth_middleware_destroy(auth_middleware_t* auth);

/* Returns the middleware handler function for http_server_use() */
http_middleware_t auth_middleware_handler(void);

#endif /* OFFS_AUTH_MIDDLEWARE_H */