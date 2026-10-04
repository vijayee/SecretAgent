//
// Created by victor on 10/04/26.
//
/* Ported from liboffs src/ClientAPI/TCP/tcp_transport.c (byte-except-
 * includes-with-adaptations). Adaptations:
 *   - The daemon parameters and fields (the caches, health_ctx, peer_node)
 *     are gone; the client-api session server rides in their place
 *     (borrowed) — the unix port's shape.
 *   - TLS: DROPPED (the ssl_ctx/SSL_* plumbing — cert/key loading, the SSL
 *     listener branches, the SSL teardown). TLS is NOT in this slice; the
 *     transport is plaintext CBOR over TCP, which is the load-bearing reason
 *     the api-key auth is REQUIRED here and REFUSES its absence at create.
 *   - The listen: liboffs's platform_listen_socket_create + platform_socket
 *     _listen pair over OUR platform (the same two-step, and our helper
 *     binds dual-stack — it serves both socket families).
 *   - NO POSIX-readiness-vs-Ice-Caps divergence to invent: the POSIX branch
 *     (the readiness listen watcher driving accept — liboffs's shape) is the
 *     ported path; the Windows accept-poll branch is ported from liboffs's
 *     Windows branch (our Platform machinery serves both families through
 *     the same platform_socket API).
 * THE DESTROY MUTEX (the standing no-locks exception, documented): the
 * _destroy_stack_* pair is liboffs's private crossing between each
 * connection's actor dispatch (pool workers) and the transport's loop thread
 * — the only place a stopped watcher may be freed. Justified as the honest
 * port of the ported machinery's own threading discipline.
 * THE TEARDOWN ORDER (tcp_transport_destroy): the corrected unix-transport
 * shape (c967ae3) 1:1 — pass 1 marks closing + destroys the fd + fires the
 * server purge + drops the transport link (touching NO watcher), the flag
 * pass, the pool's idle wait, then the final reverse free loop through
 * tcp_connection_destroy (the refcounter-gated release-based free). liboffs
 *'s own free loop tears the watchers/actors down INLINE — the unix review's
 * finding exactly; this port follows the corrected order. */

#include "tcp_transport.h"
#include "tcp_connection.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include "../../Actor/message.h"
#include "../../Actor/message_queue.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

typedef int ca_transport_tc_anchor_t;   /* ISO C: a non-empty TU when gated */

#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include <poll-dancer/poll-dancer.h>   /* gated: libcbor/pd arrive via the
                                          streams/wavedb gates (handlers.c's
                                          empty-TU idiom) */

static void* _server_thread(void* arg);
static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                             pd_event_t events, void* user_data);

/* --- the destroy stack (liboffs verbatim) ---------------------------------- */

static void _destroy_stack_init(tcp_transport_t* transport) {
  transport->destroy_lock = platform_mutex_create();
  transport->destroy_head = NULL;
}

static void _destroy_stack_push(tcp_transport_t* transport, pd_watcher_t* watcher) {
  tcp_transport_destroy_node_t* node =
      get_clear_memory(sizeof(tcp_transport_destroy_node_t));
  node->watcher = watcher;
  platform_mutex_lock(transport->destroy_lock);
  node->next = transport->destroy_head;
  transport->destroy_head = node;
  platform_mutex_unlock(transport->destroy_lock);
  pd_loop_async_send(transport->loop, NULL);
}

static void _destroy_stack_drain(tcp_transport_t* transport) {
  tcp_transport_destroy_node_t* node;
  platform_mutex_lock(transport->destroy_lock);
  node = transport->destroy_head;
  transport->destroy_head = NULL;
  platform_mutex_unlock(transport->destroy_lock);
  while (node != NULL) {
    tcp_transport_destroy_node_t* next = node->next;
    pd_watcher_destroy(node->watcher);
    free(node);
    node = next;
  }
}

static void _destroy_stack_destroy(tcp_transport_t* transport) {
  _destroy_stack_drain(transport);
  platform_mutex_destroy(transport->destroy_lock);
}

void _tcp_server_dispatch(void* state, message_t* msg) {
  tcp_transport_t* transport = (tcp_transport_t*)state;
  switch (msg->type) {
    case TCP_SERVER_UPDATE_WATCHER: {
      tcp_watcher_update_payload_t* payload =
          (tcp_watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        pd_watcher_update(payload->watcher, payload->events);
      }
      break;
    }
    case TCP_SERVER_STOP_WATCHER: {
      tcp_watcher_update_payload_t* payload =
          (tcp_watcher_update_payload_t*)msg->payload;
      if (payload->watcher != NULL) {
        /* Unregister BEFORE the fd's close (the connection destroy hands the
           sock to this payload for exactly that order's sake). This
           unregister targets the fd by number, so if the fd were already
           closed and reused by a newly accepted connection, a DEL here would
           strip that connection's registration and it would never see
           another read. liboffs's IOCP overlapped-lifetime comment does not
           carry over: the TLS machinery it protected is dropped, and the
           pd_watcher_stop-before-push is this port's POSIX-honest order (the
           unix transport's _unix_server_dispatch verbatim). */
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

tcp_transport_t* tcp_transport_create(scheduler_pool_t* pool,
                                      ca_session_server_t* server,
                                      const char* host,
                                      uint16_t port,
                                      const char* api_key_hash,
                                      platform_address_t* out_addr) {
  if (api_key_hash == NULL || api_key_hash[0] == '\0') {
    /* FAIL-LOUD: no unauthenticated TCP. (The unix socket's file permission
       is its auth; a network listener cannot lean on that — the bcrypt hash
       is this transport's auth floor.) */
    log_error("tcp_transport_create: refused — an api_key_hash is REQUIRED "
              "(TCP listens to the network; no unauthenticated TCP)");
    return NULL;
  }
  tcp_transport_t* transport = get_clear_memory(sizeof(tcp_transport_t));
  transport->pool = pool;
  transport->server = server;
  actor_init(&transport->actor, transport, _tcp_server_dispatch,
             transport->pool);
  transport->loop = pd_loop_create(NULL);
  vec_init(&transport->connections);
  transport->running = 0;
  transport->listen_sock = NULL;
  transport->listen_watcher = NULL;
  transport->max_connections = 0;
  atomic_store(&transport->active_connections, 0);
  _destroy_stack_init(transport);

  transport->host = get_memory(strlen(host) + 1);
  memcpy(transport->host, host, strlen(host) + 1);
  transport->port = port;

  transport->api_key_hash = get_memory(strlen(api_key_hash) + 1);
  memcpy(transport->api_key_hash, api_key_hash, strlen(api_key_hash) + 1);

  transport->listen_sock = platform_listen_socket_create(host, port,
                                                         out_addr);
  if (transport->listen_sock == NULL) {
    /* platform_listen_socket_create already reported the failure. */
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->api_key_hash);
    free(transport->host);
    free(transport);
    return NULL;
  }

  platform_socket_set_nonblocking(transport->listen_sock);

  if (platform_socket_listen(transport->listen_sock, 128) < 0) {
    perror("listen");
    platform_socket_destroy(transport->listen_sock);
    pd_loop_destroy(transport->loop);
    _destroy_stack_destroy(transport);
    actor_destroy(&transport->actor);
    free(transport->api_key_hash);
    free(transport->host);
    free(transport);
    return NULL;
  }

  /* A port-0 caller learns the REAL bound port here (the listen helper's
     out_addr carries the requested address only; a fixed-port bind just
     gets the requested address confirmed). Bounded-address failure keeps
     the transport usable — the out param stays the requested address. */
  if (out_addr != NULL) {
    (void)platform_socket_bound_address(transport->listen_sock, out_addr);
  }

  return transport;
}

void tcp_transport_destroy(tcp_transport_t* transport) {
  if (transport == NULL) {
    return;
  }
  /* tcp_transport_stop (called above when running) joins the server thread;
     on POSIX the server thread IS the loop thread. By the time we reach the
     connection teardown passes below, the pd-loop thread is done and no
     callback can fire on a tcp_connection_t during the teardown — no race
     with the watcher mutations that follow. The join covers the case where
     stop was never called (running was never set); without it, the
     concurrent loop thread would race every fd/watcher destroy below. */
  if (atomic_load(&transport->running)) {
    tcp_transport_stop(transport);
  }
  /* Drain the destroy stack BEFORE we tear down connection watchers. The
     destroy stack holds STOP_WATCHER pushes that were enqueued to the
     transport's actor and may still be pending when the server thread exits.
     Destroying those watchers again from the per-connection cleanup pass
     below would double-free their pd_watcher_t (the unix transport's
     recorded source of test flakiness — liboffs's own). */
  _destroy_stack_drain(transport);
  if (transport->listen_sock != NULL) {
    platform_socket_destroy(transport->listen_sock);
    transport->listen_sock = NULL;
  }
  /* THE CONNECTION TEARDOWN PASSES (the corrected unix_transport_destroy
     order, mapped 1:1; the ASan-recorded refinement below):
     (1) THE FIRST PASS marks closing + fires the server purge + drops the
     transport link. It touches NO watcher and NO conn fd: wait_for_idle has
     NOT run yet, so a conn dispatch whose hangup the pd loop posted in its
     last pre-join iteration can still be RUNNING on a pool worker through
     this pass — its _connection_close_fd already snapshotted `conn->sock`;
     destroying the socket here (liboffs's pass-1 fd destroy; a plain
     non-atomic field both sides) double-frees exactly then (ASan's recorded
     crash). The socket dies in the REAL closed window — the final free pass
     below, post-wait — inside tcp_connection_destroy's own teardown.
     (2) the server's purge hook (ca_session_conn_closed) fires HERE — the
     last point where the server is still reachable (the unix transport's
     ours-only addition; the authenticated connections hold server
     references whose release the purge guarantees). It runs BEFORE the
     final pass's dereference-to-free — by the lifetime contract it must. */
  for (int i = 0; i < transport->connections.length; i++) {
    tcp_connection_t* conn = transport->connections.data[i];
    ATOMIC_STORE(&conn->is_closing, 1);
    if (conn->server != NULL) {
      ca_session_conn_closed(conn->server, &conn->iface);
      conn->server = NULL;
    }
    conn->transport = NULL;
  }
  for (int i = 0; i < transport->connections.length; i++) {
    tcp_connection_t* conn = transport->connections.data[i];
    atomic_fetch_or(&conn->actor.flags, ACTOR_FLAG_DESTROY);
  }
  if (!atomic_load_explicit(&transport->pool->terminate, memory_order_acquire)) {
    scheduler_pool_wait_for_idle(transport->pool);
  }
  /* THE FREE PASS (the unix transport's final loop, also post-wait): idle
     now, so nothing races these teardowns — this is the closed window above,
     realized. The decrement of active_connections lives here because
     tcp_connection_destroy only decrements when the transport link is still
     set (it is NULL for every conn in this pass). */
  for (int i = transport->connections.length - 1; i >= 0; i--) {
    tcp_connection_t* conn = transport->connections.data[i];
    atomic_fetch_sub(&transport->active_connections, 1);
    /* THE WATCHER-CLAIM STEP (both transports, the corrected shape's
       addition): the ca_session_conn_closed purge's closure runs on the
       SERVER's loop thread — NOT a pool worker — so wait_for_idle does not
       bound it. A conn whose final release is still pending (the closure's
       ref) defers its free past this teardown, and its STARTED watcher must
       not outlive the pd loop destroyed at the end of this destroy — the
       deferred free's transportless direct teardown would stop/destroy it
       against a dead loop (ASan's recorded SEGV). So the teardown claims
       any still-live watcher HERE: post-wait, the loop thread joined, no
       conn dispatch can hold the watcher mid-mutation — the closed window
       above, now covering the deferred-free case too. The deferred free's
       own teardown then exchanges a NULL and touches nothing. The fd's own
       destroy rides INSIDE tcp_connection_destroy (right after this claim —
       unregister-before-close holds). */
    pd_watcher_t* watcher = ATOMIC_EXCHANGE(&conn->watcher, NULL);
    if (watcher != NULL) {
      pd_watcher_stop(watcher);
      pd_watcher_destroy(watcher);
    }
    /* THE RELEASE-BASED FREE (the ca_session_conn_t's lifetime contract):
       the vec held the base ref; the destroy's dereference frees at zero.
       A connection still pinned by a server ref (a subscription or an
       in-flight closure that outlived the teardown pass) frees at THAT
       last release — ca_session_server_destroy's unrefs — with its watcher
       gone (the claim above) and its fd closed in its own teardown. */
    tcp_connection_destroy(conn);
  }
  vec_deinit(&transport->connections);
  if (transport->host != NULL) {
    free(transport->host);
  }
  free(transport->api_key_hash);
  actor_destroy(&transport->actor);
  _destroy_stack_destroy(transport);
  pd_loop_destroy(transport->loop);
  free(transport);
}

/* Accept one pending connection (the POSIX listen-watcher callback's shape;
   liboffs shares it with the Windows accept-poll loop the same way — the
   helper honours the max_connections cap). */
static void _tcp_transport_accept_one(tcp_transport_t* transport) {
  platform_socket_t* client_sock =
      platform_socket_accept(transport->listen_sock, NULL);
  if (client_sock == NULL) {
    /* On POSIX the listen-watcher only fires accept when READ is ready, so a
       NULL here is a real (permanent) error worth logging: EBADF, EINVAL,
       ENOTSOCK, EOPNOTSUPP. Transient errors (EAGAIN, ECONNABORTED, EINTR)
       are normal. */
    if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK ||
        errno == EOPNOTSUPP) {
      log_error("tcp_transport: accept failed permanently (errno=%d)", errno);
    }
    return;
  }

  if (transport->max_connections > 0 &&
      atomic_load(&transport->active_connections) >=
          transport->max_connections) {
    platform_socket_destroy(client_sock);
    return;
  }

  tcp_connection_t* connection = tcp_connection_create(transport, client_sock);
  if (connection == NULL) {
    platform_socket_destroy(client_sock);
    return;
  }
  vec_push(&transport->connections, connection);
  atomic_fetch_add(&transport->active_connections, 1);
}

static void _accept_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                             pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  tcp_transport_t* transport = (tcp_transport_t*)user_data;

  if (events & PD_EVENT_READ) {
    _tcp_transport_accept_one(transport);
  }
}

static void* _server_thread(void* arg) {
  tcp_transport_t* transport = (tcp_transport_t*)arg;
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
  /* Windows: poll-dancer's IOCP backend cannot deliver a READ event on a
     listening socket — issuing WSARecv on a listen socket fails with
     WSAENOTCONN, so a readiness watcher can't drive accept here. The listen
     socket is nonblocking (see tcp_transport_create); poll accept() each
     loop iteration on this thread, which also services per-connection I/O
     via pd_loop_run_once. The short loop timeout bounds accept latency and
     makes shutdown prompt: tcp_transport_stop clears `running` and posts
     pd_loop_async_send, so the next iteration exits;
     _tcp_transport_accept_one's NULL branch is silent when no connection is
     pending (WSAEWOULDBLOCK).

     As in liboffs's Windows branch we do NOT drain the destroy stack here: a
     stopped connection watcher's overlapped can still be referenced by an
     in-flight ERROR_OPERATION_ABORTED completion this same loop will process
     next — the destroy pass in tcp_transport_destroy (after this thread is
     joined) takes them down. */
  while (atomic_load(&transport->running)) {
    _tcp_transport_accept_one(transport);
    pd_loop_run_once(transport->loop, 10);
  }
  pd_loop_stop(transport->loop);
#endif

  return NULL;
}

void tcp_transport_start(tcp_transport_t* transport) {
  log_info("tcp_transport_start: transport starting on %s:%u",
           transport->host, (unsigned)transport->port);
  atomic_store(&transport->running, 1);
  transport->thread = platform_thread_create(_server_thread, transport);
}

void tcp_transport_stop(tcp_transport_t* transport) {
  atomic_store(&transport->running, 0);
  /* async_send wakes the event loop (the POSIX readiness path) */
  pd_loop_async_send(transport->loop, transport);
  platform_thread_join(transport->thread);
}

void tcp_transport_set_max_connections(tcp_transport_t* transport,
                                       size_t max_connections) {
  transport->max_connections = max_connections;
}

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */