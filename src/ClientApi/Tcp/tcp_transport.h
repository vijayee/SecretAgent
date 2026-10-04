//
// Created by victor on 10/04/26.
//
/* Ported from liboffs src/ClientAPI/TCP/tcp_transport.h (byte-except-
 * includes-with-adaptations — the unix port's idiom). ADAPTATIONS against
 * the liboffs struct:
 *   - DROPPED: block_cache/ofd_cache/tuple_cache (daemon stores),
 *     health_ctx, peer_node (the daemon's node), and the TLS plumbing
 *     (ssl_ctx — TLS is NOT in this slice; the transport is plaintext CBOR
 *     over TCP, which is exactly why the api-key auth below is REQUIRED).
 *   - RETURNED (vs the unix port, which dropped it): api_key_hash — TCP
 *     NEEDS auth (a socket reachable across the network cannot lean on
 *     file permissions the way AF_UNIX does). The transport carries the
 *     BCRYPT hash of the api key; each connection's FIRST exchange is the
 *     wire's AUTH pair (see tcp_connection.c). The hash is OWNED (a copy
 *     of the caller's string, freed in destroy) and REQUIRED: a NULL or
 *     empty hash REFUSES create (returns NULL) — the transport never
 *     listens unauthenticated.
 *   - KEPT: the pool (the connections' actors attach to it), host, port,
 *     max_connections (+ its setter), the destroy-node stack.
 *   - ADDED: `server` — the client-api session server, created by the
 *     embedder and BORROWED here (the unix port's shape); and the
 *     out_addr parameter on create, which receives the BOUND address
 *     (a port-0 bind reports the real port — the platform's
 *     platform_socket_bound_address; the embedder may also pass NULL). */

#ifndef SA_TCP_TRANSPORT_H
#define SA_TCP_TRANSPORT_H

#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include <stdint.h>
#include <stddef.h>
#include "../../Actor/actor.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/vec.h"
#include "../../Scheduler/scheduler.h"
#include "../../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include "tcp_connection.h"
#include "../handlers.h"

typedef vec_t(tcp_connection_t*) vec_tcp_connection_t;

typedef struct tcp_transport_destroy_node_t {
  pd_watcher_t* watcher;
  struct tcp_transport_destroy_node_t* next;
} tcp_transport_destroy_node_t;

typedef struct tcp_transport_t {
  actor_t actor;                     /* FIRST: dispatches the watcher
                                        update/stop messages the connections
                                        send it (the destroy stack's pusher) */
  pd_loop_t* loop;                   /* OWNED: the accept thread runs it */
  platform_thread_t* thread;         /* the run thread */
  ATOMIC(uint8_t) running;
  platform_socket_t* listen_sock;
  pd_watcher_t* listen_watcher;
  vec_tcp_connection_t connections;
  scheduler_pool_t* pool;            /* borrowed */
  size_t max_connections;            /* 0 = unbounded */
  ATOMIC(size_t) active_connections;
  platform_mutex_t* destroy_lock;    /* PRIVATE destroy pair (the standing
                                        no-locks exception, documented): the
                                        stop/destroy of a connection watcher
                                        is pushed from the connection's actor
                                        dispatch (a pool worker) and drained
                                        on THIS transport's loop thread — the
                                        pair is the only crossing */
  tcp_transport_destroy_node_t* destroy_head;
  char* host;                        /* OWNED */
  uint16_t port;
  char* api_key_hash;                /* OWNED copy; REQUIRED (bcrypt $2b$) */
  ca_session_server_t* server;       /* borrowed; never freed here */
} tcp_transport_t;

tcp_transport_t* tcp_transport_create(scheduler_pool_t* pool,
                                      ca_session_server_t* server,
                                      const char* host,
                                      uint16_t port,
                                      const char* api_key_hash,
                                      platform_address_t* out_addr);
void tcp_transport_destroy(tcp_transport_t* transport);
void tcp_transport_start(tcp_transport_t* transport);
void tcp_transport_stop(tcp_transport_t* transport);
void tcp_transport_set_max_connections(tcp_transport_t* transport,
                                       size_t max_connections);

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */

#endif // SA_TCP_TRANSPORT_H