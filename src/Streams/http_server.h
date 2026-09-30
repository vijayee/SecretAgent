//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_SERVER_H
#define OFFS_HTTP_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include <openssl/ssl.h>
#include <http_parser.h>
#include "../Actor/actor.h"
#include "../Util/atomic_compat.h"
#include "../Util/vec.h"
#include "../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include "../Scheduler/scheduler.h"
#include "http_route.h"
#include "http_connection.h"

// Middleware signature: return 0 = continue, non-zero = stop chain
typedef int (*http_middleware_t)(http_request_t* request, http_response_t* response, void* user_data);

typedef struct {
  http_middleware_t handler;
  void* user_data;
  void (*user_data_destroy)(void*);
} http_middleware_entry_t;

typedef vec_t(http_middleware_entry_t) vec_middleware_t;

typedef vec_t(http_connection_t*) vec_connection_t;

typedef struct server_destroy_node_t {
  pd_watcher_t* watcher;     /* valid when type == 0 */
  pd_timer_t* timer;         /* valid when type == 1 */
  platform_socket_t* sock;   /* valid when type == 2 */
  uint8_t type;              /* 0 = watcher, 1 = timer, 2 = socket */
  struct server_destroy_node_t* next;
} server_destroy_node_t;

typedef struct http_server_t {
  actor_t actor;
  pd_loop_t* loop;
  platform_thread_t* thread;
  ATOMIC(uint8_t) running;
  platform_socket_t* listen_sock;
  pd_watcher_t* listen_watcher;
  vec_route_t routes;
  vec_middleware_t middlewares;
  vec_connection_t connections;
  SSL_CTX* ssl_ctx;
  scheduler_pool_t* pool;
  size_t max_connections;
  ATOMIC(size_t) active_connections;
  ATOMIC(uint8_t) draining;
  uint8_t is_local_binding;
  /* Per-connection slowloris timeouts (set from config via
     http_server_set_timeouts, or left at the 30s/60s defaults). */
  uint32_t idle_timeout_ms;
  uint32_t hard_timeout_ms;
  platform_mutex_t* destroy_lock;
  server_destroy_node_t* destroy_head;
} http_server_t;

http_server_t* http_server_create(scheduler_pool_t* pool, const char* host, uint16_t port);
void http_server_destroy(http_server_t* server);

http_server_t* http_server_create_ssl(scheduler_pool_t* pool, const char* host, uint16_t port,
                                       const char* cert_path, const char* key_path);

void http_server_get(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data);
void http_server_put(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data);
void http_server_post(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data);
void http_server_delete(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data);

void http_server_get_with_data(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data, void (*user_data_destroy)(void*));
void http_server_put_with_data(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data, void (*user_data_destroy)(void*));
void http_server_post_with_data(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data, void (*user_data_destroy)(void*));
void http_server_delete_with_data(http_server_t* server, const char* pattern, http_handler_t handler, void* user_data, void (*user_data_destroy)(void*));

void http_server_listen(http_server_t* server);
void http_server_stop(http_server_t* server);
void http_server_drain(http_server_t* server);
void http_server_set_max_connections(http_server_t* server, size_t max_connections);
void http_server_set_timeouts(http_server_t* server, uint32_t idle_ms, uint32_t hard_ms);

void http_server_dispatch(http_server_t* server, http_request_t* request, http_response_t* response);

/* Defer a connection socket's close+free to the I/O thread's destroy stack so
   it is never freed while the I/O-thread read callback may still be using it. */
void http_server_defer_socket_destroy(http_server_t* server, platform_socket_t* sock);

http_route_t* http_server_match_route(http_server_t* server, int method, const char* path);

void http_server_use(http_server_t* server, http_middleware_t middleware, void* user_data, void (*user_data_destroy)(void*));

uint8_t http_server_is_local_binding(const http_server_t* server);

#endif // OFFS_HTTP_SERVER_H