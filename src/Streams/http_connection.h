//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_CONNECTION_H
#define OFFS_HTTP_CONNECTION_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <http_parser.h>
#include "../RefCounter/refcounter.h"
#include "../Buffer/buffer.h"
#include "../Util/vec.h"
#include "../Actor/actor.h"
#include "../Platform/platform.h"
#include "http_route.h"
#include "../Util/atomic_compat.h"
#include <poll-dancer/poll-dancer.h>

typedef struct http_server_t http_server_t;
typedef struct http_request_t http_request_t;
typedef struct http_response_t http_response_t;

typedef struct {
  pd_watcher_t* watcher;
  pd_event_t events;
  pd_timer_t* timer;   /* set when STOP_WATCHER carries an idle timer teardown */
} watcher_update_payload_t;

typedef struct http_connection_t {
  refcounter_t refcounter;
  actor_t actor;
  http_server_t* server;
  ATOMIC(platform_socket_t*) sock;
  ATOMIC(pd_watcher_t*) watcher;
  SSL* ssl;
  /* Windows IOCP only: memory BIO pair decoupling OpenSSL from the socket so
   * the worker can decrypt ciphertext the I/O thread drained from the watcher
   * (the kernel socket is already empty by the time the worker runs). rbio is
   * the ciphertext ingress (fed by _connection_ssl_data_handle); wbio captures
   * TLS records SSL_write/the handshake emit for the socket send path. SSL_free
   * owns and frees both. NULL/unused on POSIX, where SSL_set_fd is used. */
  BIO* rbio;
  BIO* wbio;
  http_parser parser;
  http_request_t* request;
  buffer_t* write_buffer;
  uint8_t write_pending;
  ATOMIC(uint8_t) read_pending;      /* I/O thread has sent READABLE; actor hasn't processed it yet */
  char* header_field;
  size_t header_field_len;
  size_t header_field_cap;
  char* header_value;
  size_t header_value_len;
  size_t header_value_cap;
  size_t header_count;     /* number of headers flushed for the current request */
  uint8_t header_saw_value; /* a value callback landed since the last flush */
  uint8_t headers_complete;
  uint8_t request_complete;
  uint8_t is_ssl;
  uint8_t piped_pending;
  uint8_t is_closing;
  http_route_t* streaming_route;
  /* Slowloris defense: one-shot idle timer armed on the I/O loop. Fires after
     idle_timeout_ms with no I/O activity -> CLOSE. hard_deadline_ms is the
     absolute monotonic-ms cutoff by which the request must complete; checked
     on each read. Both owned by the I/O thread (set on create, re-armed in the
     read callback, torn down via the server actor's deferred destroy stack). */
  pd_timer_t* idle_timer;
  uint64_t hard_deadline_ms;
} http_connection_t;

http_connection_t* http_connection_create(http_server_t* server, platform_socket_t* sock);
void http_connection_destroy(http_connection_t* connection);

void http_connection_dispatch(void* state, message_t* msg);
void http_connection_write(http_connection_t* connection, const char* data, size_t length);
void http_connection_close(http_connection_t* connection);

#endif // OFFS_HTTP_CONNECTION_H