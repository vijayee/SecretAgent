//
// Created by victor on 10/03/26.
//
/* Ported from liboffs src/ClientAPI/Unix/unix_connection.h (byte-except-
 * includes-with-adaptations): the machine part survives, liboffs's ~40-op
 * handler machinery does not — this transport carries the client-api wire's
 * ONE dispatch (the ca_session_handle bridge; see unix_connection.c) and the
 * dropped daemon services (block/put/get pipelines, auth, config/peer/health
 * handlers, OFD resolution) belong to the daemon, not this carrier.
 *
 * The connection implements the client-api handlers' ca_session_conn_t:
 *   send    = _unix_connection_send (the iface's consumer half),
 *   retain  = the connection's refcounter reference,
 *   release = the refcounter's dereference-to-free.
 * NO auth: an AF_UNIX socket's file permission IS the auth. */

#ifndef SA_UNIX_CONNECTION_H
#define SA_UNIX_CONNECTION_H

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

typedef struct unix_transport_t unix_transport_t;

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
} unix_watcher_update_payload_t;

/* The unix transport family's own message vocabulary (the style guide: each
   module owns its message_type_e). The values mirror liboffs's shared
   message.h catalog numbering verbatim (its UNIX_CONNECTION_* block); the
   gaps are other modules' entries there. */
typedef enum unix_msg_type_e {
  UNIX_CONNECTION_DATA = 172,
  UNIX_CONNECTION_HANGUP = 173,
  UNIX_CONNECTION_ERROR = 174,
  UNIX_CONNECTION_WRITE = 175,
  UNIX_CONNECTION_WRITABLE = 176,
  UNIX_CONNECTION_CLOSE = 177,
  /* the transport-actor's watcher messages (the server side) */
  UNIX_SERVER_UPDATE_WATCHER = 178,
  UNIX_SERVER_STOP_WATCHER = 179
} unix_msg_type_e;

typedef struct unix_connection_t {
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
     workers load it at the dispatch's entry gate (see unix_connection.c) */
  ATOMIC(uint8_t) is_closing;
  unix_transport_t* transport;
  ca_session_server_t* server;   /* borrowed from the transport; the dispatch
                                    hands it every decoded frame */
  ca_session_conn_t iface;       /* the handlers' view of this connection */
} unix_connection_t;

unix_connection_t* unix_connection_create(unix_transport_t* transport,
                                          platform_socket_t* sock);
void unix_connection_destroy(unix_connection_t* connection);

void unix_connection_dispatch(void* state, message_t* msg);
void unix_connection_write(unix_connection_t* connection,
                           const uint8_t* data, size_t length);
void unix_connection_close(unix_connection_t* connection);

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */

#endif // SA_UNIX_CONNECTION_H