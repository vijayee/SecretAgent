//
// Created by victor on 10/04/26.
//
/* Ported from liboffs src/ClientAPI/TCP/tcp_connection.h (byte-except-
 * includes-with-adaptations): the machine part survives, liboffs's daemon
 * handler machinery does not — this transport carries the client-api wire's
 * ONE dispatch (the ca_session_handle bridge; see tcp_connection.c) plus the
 * AUTH machinery (which the unix carrier has none of — a TCP listener is
 * reachable across the network, so its connections authenticate FIRST), and
 * the dropped daemon services (the GET/PUT/LOAD pipelines, the block/rep/
 * peer/cache handlers, the OFD resolution, the SSL/BIO plumbing) belong to
 * liboffs's daemon, not this carrier.
 *
 * The connection implements the client-api handlers' ca_session_conn_t:
 *   send    = _tcp_connection_send (the iface's consumer half),
 *   retain  = the connection's refcounter reference,
 *   release = the refcounter's dereference-to-free.
 *
 * THE AUTH EXCHANGE (liboffs's _tcp_handle_auth shape, adapted): a fresh
 * connection starts UNauthenticated (liboffs's is_authenticated = the hash
 * is NULL ? 1 : 0 — our transport REFUSES a NULL hash at create, so every
 * connection here starts at 0 and the first frame MUST be the wire's AUTH
 * pair); an AUTH frame is bcrypt-checked against the transport's hash:
 * match → the state flips + a status-0 response; mismatch → a status-1
 * response, then the connection CLOSES (the graceful-close path). Any
 * OTHER frame before authentication is refused loud (CA_ERROR) and the
 * connection closes. */

#ifndef SA_TCP_CONNECTION_H
#define SA_TCP_CONNECTION_H

/* The transport rides BOTH gates (handlers.h's gating rule verbatim — it
   reaches the handlers' ca_session_conn_t + server and the poll-dancer loop;
   a build without either has nothing for a transport to glue together). */
#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include <stdint.h>
#include <stddef.h>
#include "../../RefCounter/refcounter.h"
#include "../../Buffer/buffer.h"
#include "../../Util/atomic_compat.h"
#include "../../Actor/actor.h"
#include "../../Network/stream_framer.h"
#include "../handlers.h"
#include "../../Platform/platform.h"
#ifdef SA_HAS_STREAMS
#include <poll-dancer/poll-dancer.h>
#endif

typedef struct tcp_transport_t tcp_transport_t;

/* The watcher-update/stop message's body (liboffs's shape verbatim): the
   watcher rides the transport's actor so watcher mutations never race the
   loop thread; the socket is carried on the STOP form and destroyed together
   with the watcher on the transport actor thread (STOP_WATCHER only): the
   unregister must precede the fd's close, or the closed fd number can be
   reused by an accepted connection whose registration the deferred
   unregister then removes by fd value. */
typedef struct {
  pd_watcher_t* watcher;
  pd_event_t events;
  platform_socket_t* sock;
} tcp_watcher_update_payload_t;

/* The tcp transport family's own message vocabulary (the style guide: each
   module owns its message_type_e). The values mirror liboffs's shared
   message.h catalog numbering verbatim (its TCP_CONNECTION_* block —
   including its TCP_CONNECTION_READABLE slot, unused on this port's POSIX
   machinery but kept so the catalog's numbering survives the port). */
typedef enum tcp_msg_type_e {
  TCP_CONNECTION_DATA = 180,
  TCP_CONNECTION_READABLE = 181,   /* liboffs's IOCP-SSL slot — not used on
                                      this port (SSL dropped); numbered to
                                      keep the rest of the block in place */
  TCP_CONNECTION_HANGUP = 182,
  TCP_CONNECTION_ERROR = 183,
  TCP_CONNECTION_WRITE = 184,
  TCP_CONNECTION_WRITABLE = 185,
  TCP_CONNECTION_CLOSE = 186,
  /* the transport-actor's watcher messages (the server side) */
  TCP_SERVER_UPDATE_WATCHER = 187,
  TCP_SERVER_STOP_WATCHER = 188
} tcp_msg_type_e;

typedef struct tcp_connection_t {
  refcounter_t refcounter;   /* FIRST: the ca_session_conn_t's retain/release
                                pair IS this counter — the transport's vec
                                holds the base ref, the server's held refs
                                (subscriptions, in-flight closures) pin the
                                object past teardown until their tails */
  actor_t actor;
  platform_socket_t* sock;
  ATOMIC(pd_watcher_t*) watcher;
  stream_framer_t* framer;
  buffer_t* write_buffer;
  uint8_t write_pending;
  /* atomic, not plain: the teardown thread stores it while scheduler
     workers load it at the dispatch's entry gate (see tcp_connection.c) */
  ATOMIC(uint8_t) is_closing;
  /* touched ONLY on the connection's own dispatch (authenticate-and-flip is
     a dispatch-time decision) — plain is exact there */
  uint8_t is_authenticated;
  tcp_transport_t* transport;
  ca_session_server_t* server;   /* borrowed from the transport; the dispatch
                                    hands it every authenticated frame */
  ca_session_conn_t iface;       /* the handlers' view of this connection */
} tcp_connection_t;

tcp_connection_t* tcp_connection_create(tcp_transport_t* transport,
                                        platform_socket_t* sock);
void tcp_connection_destroy(tcp_connection_t* connection);

void tcp_connection_dispatch(void* state, message_t* msg);
void tcp_connection_write(tcp_connection_t* connection,
                          const uint8_t* data, size_t length);
void tcp_connection_close(tcp_connection_t* connection);

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */

#endif // SA_TCP_CONNECTION_H