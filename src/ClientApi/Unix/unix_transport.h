//
// Created by victor on 10/03/26.
//
/* Ported from liboffs src/ClientAPI/Unix/unix_transport.h (byte-except-
 * includes-with-adaptations). ADAPTATIONS against the liboffs struct:
 *   - DROPPED: block_cache/ofd_cache/tuple_cache (daemon stores), api_key_hash
 *     (an AF_UNIX socket's file permission IS the auth), health_ctx,
 *     update_status_ctx, config_node/config_data_dir/trigger_restart
 *     (daemon config management), and the pipe-path members
 *     loop_thread/loop_running (our Platform carries no named-pipe listener —
 *     the AF_UNIX path only; on POSIX the server thread IS the loop thread).
 *   - KEPT: the pool (the connections' actors attach to it), socket_path,
 *     max_connections (+ its setter), the destroy-node stack.
 *   - ADDED: `server` — the client-api session server, created by the
 *     embedder and BORROWED here (plan Task 5); the server's own loop thread
 *     is the streams_loop_thread_t the embedder holds, and every response
 *     this connection sends (encode + frame + queue) marshals onto it. */

#ifndef SA_UNIX_TRANSPORT_H
#define SA_UNIX_TRANSPORT_H

#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include <stdint.h>
#include <stddef.h>
#include "../../Actor/actor.h"
#include "../../Util/atomic_compat.h"
#include "../../Util/vec.h"
#include "../../Scheduler/scheduler.h"
#include "../../Platform/platform.h"
#include <poll-dancer/poll-dancer.h>
#include "unix_connection.h"
#include "../handlers.h"

typedef vec_t(unix_connection_t*) vec_unix_connection_t;

typedef struct unix_transport_destroy_node_t {
  pd_watcher_t* watcher;
  struct unix_transport_destroy_node_t* next;
} unix_transport_destroy_node_t;

typedef struct unix_transport_t {
  actor_t actor;                     /* FIRST: dispatches the watcher
                                        update/stop messages the connections
                                        send it (the destroy stack's pusher) */
  pd_loop_t* loop;                   /* OWNED: the accept thread runs it */
  platform_thread_t* thread;         /* the run thread */
  ATOMIC(uint8_t) running;
  platform_socket_t* listen_sock;
  pd_watcher_t* listen_watcher;
  vec_unix_connection_t connections;
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
  unix_transport_destroy_node_t* destroy_head;
  char* socket_path;                 /* OWNED */
  ca_session_server_t* server;       /* borrowed; never freed here */
} unix_transport_t;

unix_transport_t* unix_transport_create(scheduler_pool_t* pool,
                                        ca_session_server_t* server,
                                        const char* socket_path);
void unix_transport_destroy(unix_transport_t* transport);
void unix_transport_start(unix_transport_t* transport);
void unix_transport_stop(unix_transport_t* transport);
void unix_transport_set_max_connections(unix_transport_t* transport,
                                        size_t max_connections);

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */

#endif // SA_UNIX_TRANSPORT_H