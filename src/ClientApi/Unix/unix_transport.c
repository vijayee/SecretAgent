//
// Created by victor on 10/03/26.
//
/* Ported from liboffs src/ClientAPI/Unix/unix_transport.c (byte-except-
 * includes-with-adaptations). Adaptations:
 *   - liboffs's platform_local_* helpers (its Platform/platform_local.c) are
 *     the static _local_* helpers below — the same POSIX AF_UNIX shape over
 *     OUR platform_socket's PLATFORM_AF_LOCAL family. The Windows named-pipe
 *     backend (and its blocking-accept / separate _pipe_loop_thread dance)
 *     is NOT ported: our platform exposes no pipe listener; the transport is
 *     AF_UNIX-only, on POSIX with a readiness listen watcher, on Windows with
 *     the same accept-poll workaround liboffs's IOCP branch uses.
 *   - The daemon parameters and fields (the caches, the api-key hash, the
 *     health/update-status/config machinery) are gone; the client-api session
 *     server rides in their place (borrowed).
 * THE DESTROY MUTEX (the standing no-locks exception, documented): the
 * _destroy_stack_* pair is liboffs's private crossing between each
 * connection's actor dispatch (pool workers) and the transport's loop thread
 * — the only place a stopped watcher may be freed. Justified as the honest
 * port of the ported machinery's own threading discipline. */

#include "unix_transport.h"
#include "unix_connection.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include "../../Actor/message.h"
#include "../../Actor/message_queue.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

typedef int ca_transport_tu_anchor_t;   /* ISO C: a non-empty TU when gated */

#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include <poll-dancer/poll-dancer.h>   /* gated: libcbor/pd arrive via the
                                          streams/wavedb gates (handlers.c's
                                          empty-TU idiom) */

static void* _server_thread(void* arg);
/* _accept_callback drives accept via the readiness listen watcher (epoll/
 * kqueue); Windows IOCP can't deliver a READ event on a listening socket, so
 * the Windows AF_UNIX path polls accept() in _server_thread instead and never
 * references this callback. liboffs's pipe-accept branch is not ported (our
 * Platform carries no named-pipe listener). */
#ifndef _WIN32
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                             pd_event_t events, void* user_data);
#endif

/* --- the platform_local_* replacements (liboffs POSIX shape) --------------- */

static platform_socket_t* _local_listen(const char* path) {
  platform_socket_t* sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
  if (sock == NULL) {
    return NULL;
  }

  platform_address_t addr;
  memset(&addr, 0, sizeof(addr));
  addr.family = PLATFORM_AF_LOCAL;
  strncpy(addr.local.path, path, sizeof(addr.local.path) - 1);

  platform_file_unlink(path); /* remove stale socket file */

  if (platform_socket_bind(sock, &addr) != 0 ||
      platform_socket_listen(sock, 128) != 0) {
    platform_socket_destroy(sock);
    return NULL;
  }
  return sock;
}

static platform_socket_t* _local_accept(platform_socket_t* listener) {
  return platform_socket_accept(listener, NULL);
}

static void _local_cleanup(const char* path) {
  platform_file_unlink(path);
}

/* POSIX AF_UNIX listeners don't need rearming after accept — the listening
 * fd stays in the listening state and the next accept can proceed
 * immediately. The rearm concept exists only for the Windows named-pipe
 * backend that is not ported. Kept so the accept machinery keeps liboffs's
 * shape (the call sites survive); the impl is a no-op, as liboffs's is. */
static int _local_rearm(platform_socket_t* listener) {
  (void)listener;
  return 0;
}

/* --- the destroy stack (liboffs verbatim) ---------------------------------- */

static void _destroy_stack_init(unix_transport_t* transport) {
  transport->destroy_lock = platform_mutex_create();
  transport->destroy_head = NULL;
}

static void _destroy_stack_push(unix_transport_t* transport, pd_watcher_t* watcher) {
  unix_transport_destroy_node_t* node =
      get_clear_memory(sizeof(unix_transport_destroy_node_t));
  node->watcher = watcher;
  platform_mutex_lock(transport->destroy_lock);
  node->next = transport->destroy_head;
  transport->destroy_head = node;
  platform_mutex_unlock(transport->destroy_lock);
  pd_loop_async_send(transport->loop, NULL);
}

static void _destroy_stack_drain(unix_transport_t* transport) {
  unix_transport_destroy_node_t* node;
  platform_mutex_lock(transport->destroy_lock);
  node = transport->destroy_head;
  transport->destroy_head = NULL;
  platform_mutex_unlock(transport->destroy_lock);
  while (node != NULL) {
    unix_transport_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(unix_transport_t* transport) {
  _destroy_stack_drain(transport);
  platform_mutex_destroy(transport->destroy_lock);
}

void _unix_server_dispatch(void* state, message_t* msg) {
  unix_transport_t* transport = (unix_transport_t*)state;
  switch (msg->type) {
    case UNIX_SERVER_UPDATE_WATCHER: {
      unix_watcher_update_payload_t* payload =
          (unix_watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        pd_watcher_update(payload->watcher, payload->events);
      }
      break;
    }
    case UNIX_SERVER_STOP_WATCHER: {
      unix_watcher_update_payload_t* payload =
          (unix_watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        /* Unregister BEFORE the fd's close (below): this unregister targets
           the fd by number, so if the fd were already closed and reused by a
           newly accepted connection, the DEL here would strip that
           connection's registration and it would never see another read. */
        pd_watcher_stop(payload->watcher);
        _destroy_stack_push(transport, payload->watcher);
      }
      if (payload->sock != NULL) {
        platform_socket_destroy(payload->sock);
        payload->sock = NULL;
      }
      break;
    }
    default:
      break;
  }
}

unix_transport_t* unix_transport_create(scheduler_pool_t* pool,
                                        ca_session_server_t* server,
                                        const char* socket_path) {
  unix_transport_t* transport = get_clear_memory(sizeof(unix_transport_t));
  transport->pool = pool;
  transport->server = server;
  actor_init(&transport->actor, transport, _unix_server_dispatch,
             transport->pool);
  transport->loop = pd_loop_create(NULL);
  vec_init(&transport->connections);
  transport->running = 0;
  transport->listen_sock = NULL;
  transport->listen_watcher = NULL;
  transport->max_connections = 0;
  atomic_store(&transport->active_connections, 0);
  _destroy_stack_init(transport);

  transport->socket_path = get_memory(strlen(socket_path) + 1);
  memcpy(transport->socket_path, socket_path, strlen(socket_path) + 1);

  transport->listen_sock = _local_listen(socket_path);
  if (transport->listen_sock == NULL) {
    log_error("unix_transport_create: _local_listen failed for %s",
              socket_path);
    perror("_local_listen");
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->socket_path);
    free(transport);
    return NULL;
  }

  platform_socket_set_nonblocking(transport->listen_sock);

  return transport;
}

void unix_transport_destroy(unix_transport_t* transport) {
  if (transport == NULL) {
    return;
  }
  /* unix_transport_stop (called above when running) joins the server thread;
     on POSIX the server thread IS the loop thread. By the time we reach the
     connection teardown pass below, the pd-loop thread is done and no
     callback can fire on a unix_connection_t during the teardown — no race
     with the watcher mutations that follow. The join covers the case where
     stop was never called (running was never set); without it, the
     concurrent loop thread would race every fd/watcher destroy below. */
  if (atomic_load(&transport->running)) {
    unix_transport_stop(transport);
  }
  /* Drain the destroy stack BEFORE we tear down connection watchers. The
   * destroy stack holds STOP_WATCHER pushes (via _connection_stop_watcher)
   * that were enqueued to the transport's actor and may still be pending
   * when the server thread exits. If we destroy those watchers again from
   * the per-connection cleanup pass below, we double-free their pd_watcher_t
   * and corrupt the heap (liboffs's recorded source of test flakiness — the
   * next test's allocation hits the corrupted tcache/fastbin). */
  _destroy_stack_drain(transport);
  if (transport->listen_sock != NULL) {
    platform_socket_destroy(transport->listen_sock);
    transport->listen_sock = NULL;
  }
  /* The connection teardown pass: the loop is stopped (the server thread is
     joined above), so each connection's watcher and fd tear down directly —
     no actor round trip, no destroy stack. THE ORDER MATTERS twice over:
     (1) this pass runs BEFORE pd_loop_destroy — the loop's own destroy frees
     every watcher still registered in it, so a connection freeing later
     (deferred, below) may not touch its watcher again; and (2) the server's
     purge hook (ca_session_conn_closed) fires while the server is still
     reachable — it drops this connection's subscriptions and pending store
     round trips, releasing the refs the server pinned. */
  for (int i = 0; i < transport->connections.length; i++) {
    unix_connection_t* conn = transport->connections.data[i];
    conn->is_closing = 1;
    atomic_fetch_sub(&transport->active_connections, 1);
    pd_watcher_t* watcher = ATOMIC_EXCHANGE(&conn->watcher, NULL);
    if (watcher != NULL) {
      pd_watcher_stop(watcher);
      pd_watcher_destroy(watcher);
    }
    if (conn->sock != NULL) {
      platform_socket_destroy(conn->sock);
      conn->sock = NULL;
    }
    if (conn->server != NULL) {
      ca_session_conn_closed(conn->server, &conn->iface);
      conn->server = NULL;
    }
    conn->transport = NULL;
  }
  for (int i = 0; i < transport->connections.length; i++) {
    unix_connection_t* conn = transport->connections.data[i];
    atomic_fetch_or(&conn->actor.flags, ACTOR_FLAG_DESTROY);
  }
  if (!atomic_load_explicit(&transport->pool->terminate, memory_order_acquire)) {
    scheduler_pool_wait_for_idle(transport->pool);
  }
  for (int i = transport->connections.length - 1; i >= 0; i--) {
    unix_connection_t* conn = transport->connections.data[i];
    /* THE RELEASE-BASED FREE (the ca_session_conn_t's lifetime contract):
       the vec held the base ref; the destroy's dereference frees at zero.
       A connection still pinned by a server ref (a subscription or an
       in-flight closure that outlived the teardown pass) frees at THAT
       last release — ca_session_server_destroy's unrefs — with its
       watcher/fd already gone (the pass above), so the deferred free can
       touch none of the loop's machinery. */
    unix_connection_destroy(conn);
  }
  vec_deinit(&transport->connections);
  if (transport->socket_path != NULL) {
    _local_cleanup(transport->socket_path);
    free(transport->socket_path);
  }
  actor_destroy(&transport->actor);
  _destroy_stack_destroy(transport);
  pd_loop_destroy(transport->loop);
  free(transport);
}

#ifndef _WIN32
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                             pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  unix_transport_t* transport = (unix_transport_t*)user_data;

  if (events & PD_EVENT_READ) {
    platform_socket_t* client_sock = _local_accept(transport->listen_sock);
    if (client_sock == NULL) {
      /* Only log permanent accept failures (EBADF, EINVAL, ENOTSOCK,
         EOPNOTSUPP). Transient errors like EAGAIN, ECONNABORTED, EINTR are
         normal. */
      if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK ||
          errno == EOPNOTSUPP) {
        log_error("unix_transport: accept failed permanently (errno=%d)",
                  errno);
      }
      return;
    }

    if (transport->max_connections > 0 &&
        atomic_load(&transport->active_connections) >=
            transport->max_connections) {
      platform_socket_destroy(client_sock);
      return;
    }

    unix_connection_t* connection =
        unix_connection_create(transport, client_sock);
    if (connection == NULL) {
      platform_socket_destroy(client_sock);
      return;
    }
    vec_push(&transport->connections, connection);
    atomic_fetch_add(&transport->active_connections, 1);

    /* POSIX AF_UNIX: rearm is the no-op (see _local_rearm). */
    _local_rearm(transport->listen_sock);
  }
}
#endif

static void* _server_thread(void* arg) {
  unix_transport_t* transport = (unix_transport_t*)arg;
  platform_thread_setup_stack();

#ifndef _WIN32
  /* POSIX: the poll-dancer readiness listen watcher (epoll/kqueue deliver
     READ when a connection is pending) drives the accept callback on this
     thread; this thread also drains the connection watchers' destroy stack
     each iteration (liboffs's drain-in-loop). */
  transport->listen_watcher =
      platform_socket_watcher_create(transport->loop, transport->listen_sock,
    PD_EVENT_READ, _accept_callback, transport);
  if (transport->listen_watcher != NULL) {
    pd_watcher_start(transport->listen_watcher);
  }

  while (atomic_load(&transport->running)) {
    _destroy_stack_drain(transport);
    pd_loop_run_once(transport->loop, 100);
  }

  if (transport->listen_watcher != NULL) {
    pd_watcher_stop(transport->listen_watcher);
    pd_watcher_destroy(transport->listen_watcher);
  }
  pd_loop_stop(transport->loop);
#else
  /* Windows AF_UNIX: poll-dancer's IOCP backend cannot deliver a READ event
     on a listening socket — issuing WSARecv on a listen socket fails with
     WSAENOTCONN, so a readiness watcher can't drive accept here. The listen
     socket is nonblocking (see unix_transport_create); poll accept() each
     loop iteration on this thread, which also services per-connection I/O
     via pd_loop_run_once. The short loop timeout bounds accept latency and
     makes shutdown prompt: unix_transport_stop clears `running` and posts
     pd_loop_async_send, so the next iteration exits; _local_accept returns
     NULL silently when no connection is pending.

     As in liboffs's Windows branch we do NOT drain the destroy stack here: a
     stopped connection watcher's overlapped can still be referenced by an
     in-flight ERROR_OPERATION_ABORTED completion this same loop will process
     next — the destroy pass in unix_transport_destroy (after this thread is
     joined) takes them down. */
  while (atomic_load(&transport->running)) {
    platform_socket_t* client_sock = _local_accept(transport->listen_sock);
    if (client_sock != NULL) {
      if (transport->max_connections > 0 &&
          atomic_load(&transport->active_connections) >=
              transport->max_connections) {
        platform_socket_destroy(client_sock);
      } else {
        unix_connection_t* connection =
            unix_connection_create(transport, client_sock);
        if (connection == NULL) {
          platform_socket_destroy(client_sock);
        } else {
          vec_push(&transport->connections, connection);
          atomic_fetch_add(&transport->active_connections, 1);
        }
      }
    }
    pd_loop_run_once(transport->loop, 10);
  }
  pd_loop_stop(transport->loop);
#endif

  return NULL;
}

void unix_transport_start(unix_transport_t* transport) {
  log_info("unix_transport_start: transport starting on %s",
           transport->socket_path);
  atomic_store(&transport->running, 1);
  transport->thread = platform_thread_create(_server_thread, transport);
}

void unix_transport_stop(unix_transport_t* transport) {
  atomic_store(&transport->running, 0);
  /* async_send wakes the event loop (the POSIX readiness path) */
  pd_loop_async_send(transport->loop, transport);
  platform_thread_join(transport->thread);
}

void unix_transport_set_max_connections(unix_transport_t* transport,
                                        size_t max_connections) {
  transport->max_connections = max_connections;
}

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */