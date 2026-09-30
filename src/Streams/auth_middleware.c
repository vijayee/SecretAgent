//
// Created by victor on 5/22/26.
//

#define _DEFAULT_SOURCE

#include "auth_middleware.h"
#include "http_request.h"
#include "http_response.h"
#include "http_headers.h"
#include "../Util/bcrypt.h"
#include "../Util/allocator.h"

#include <stdlib.h>
#include <string.h>

struct auth_middleware_t {
  char* bcrypt_hash;
  bool allow_local_no_auth;
  http_server_t* server;
};

auth_middleware_t* auth_middleware_create(const char* bcrypt_hash,
                                          bool allow_local_no_auth,
                                          http_server_t* server) {
  if (bcrypt_hash == NULL) {
    return NULL;
  }
  auth_middleware_t* auth = get_clear_memory(sizeof(auth_middleware_t));
  auth->bcrypt_hash = strdup(bcrypt_hash);
  if (auth->bcrypt_hash == NULL) {
    free(auth);
    return NULL;
  }
  auth->allow_local_no_auth = allow_local_no_auth;
  auth->server = server;
  return auth;
}

void auth_middleware_destroy(auth_middleware_t* auth) {
  if (auth == NULL) return;
  free(auth->bcrypt_hash);
  free(auth);
}

static int _auth_handler(http_request_t* request, http_response_t* response, void* user_data) {
  auth_middleware_t* auth = (auth_middleware_t*)user_data;

  /* Opt-out: on a loopback binding with the flag set, skip bearer auth.
     This is the only path that lets a no-bearer request through; the
     default (allow_local_no_auth=false) still requires bearer on loopback. */
  if (auth->allow_local_no_auth && auth->server != NULL &&
      http_server_is_local_binding(auth->server)) {
    request->is_authenticated = 1;
    return 0;
  }

  const char* auth_header = http_headers_get(&request->headers, "Authorization");
  if (auth_header == NULL) {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_set_header(response, "WWW-Authenticate", "Bearer");
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Authentication required", 22);
    http_response_end(response);
    return 1;
  }

  /* Check for "Bearer " prefix (case-sensitive, per RFC 6750) */
  if (strncmp(auth_header, "Bearer ", 7) != 0) {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_set_header(response, "WWW-Authenticate", "Bearer");
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Invalid authentication scheme", 29);
    http_response_end(response);
    return 1;
  }

  const char* token = auth_header + 7;
  if (*token == '\0') {
    http_response_set_status(response, HTTP_STATUS_UNAUTHORIZED);
    http_response_set_header(response, "WWW-Authenticate", "Bearer");
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Empty token", 11);
    http_response_end(response);
    return 1;
  }

  if (bcrypt_check(token, auth->bcrypt_hash) != 0) {
    http_response_set_status(response, HTTP_STATUS_FORBIDDEN);
    http_response_set_header(response, "Content-Type", "text/plain");
    http_response_write(response, "Invalid API key", 15);
    http_response_end(response);
    return 1;
  }

  request->is_authenticated = 1;
  return 0;
}

http_middleware_t auth_middleware_handler(void) {
  return _auth_handler;
}