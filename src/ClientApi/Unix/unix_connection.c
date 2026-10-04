//
// Created by victor on 10/03/26.
//
/* Ported from liboffs src/ClientAPI/Unix/unix_connection.c — the CONNECTION
 * MACHINERY ONLY (1562 lines there; the ~1300 lines of daemon-op handlers —
 * the GET/PUT/LOAD pipelines, the auth exchange, the block/config/peer/
 * health/update handlers, the OFD async resolution — are liboffs's daemon
 * surface and are NOT this carrier's).
 *
 * OUR dispatch (the plan's Task-5 bridge) replaces liboffs's ~40-op handler
 * switch with ONE line: ca_wire_decode(item-form) → ca_session_handle
 * (server, type, payload, &conn->iface). THE PAYLOAD'S OWNERSHIP IS
 * HANDLERS.H'S CONTRACT, PINNED HERE:
 *   - ca_wire_decode COPIES every field out of the cbor item into heap
 *     payload structs (the wire's landed review fact), so the ITEM's decref
 *     below is safe — the item dies here, the payload lives on.
 *   - ca_session_handle TRANSFERS the payload on 0 (accepted: it rides the
 *     loop-thread work closure and dies there) and DESTROYS IT INTERNALLY on
 *     nonzero (refused) — so the dispatch destroys NOTHING after the call, on
 *     either path.
 *   - the connection-level refusal (a send that cannot queue): our return
 *     nonzero leaves the payload untouched, and the handlers drop it loud —
 *     the ca_session_conn_t contract's sender half.
 * NO auth path: an AF_UNIX socket's file permission IS the auth. */

#include "unix_connection.h"
#include "unix_transport.h"
#include "../client_api_wire.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
  #include <unistd.h>
#endif
#include <errno.h>
#include <stdio.h>

typedef int ca_tu_anchor_t;   /* ISO C: a non-empty TU when gated */

#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include <cbor.h>   /* gated: libcbor arrives via the wavedb gate */

#define READ_BUFFER_SIZE 65536

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                      pd_event_t events, void* user_data);
static void _unix_dispatch_frame(unix_connection_t* conn, cbor_item_t* frame);

/* --- send frames ---------------------------------------------------------- */

/* Raw form: serialize bytes to a length-prefixed frame and queue the write.
   The one write path both _send and _send_error compose through. */
static void _unix_connection_send_frame_raw(unix_connection_t* conn,
                                            const uint8_t* raw,
                                            size_t raw_len) {
  size_t framed_len;
  uint8_t* framed = stream_frame_encode(raw, raw_len, &framed_len);
  if (framed != NULL) {
    unix_connection_write(conn, framed, framed_len);
    free(framed);
  }
}

static void _unix_connection_send_error(unix_connection_t* conn,
                                        uint64_t req_id, uint8_t status,
                                        const char* message) {
  if (conn == NULL || message == NULL) {
    return;
  }
  ca_error_t error_msg;
  memset(&error_msg, 0, sizeof(error_msg));
  error_msg.req_id = req_id;
  error_msg.status = status;
  error_msg.text = (char*)message;   /* borrowed for the encode only */
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  if (ca_wire_encode(CA_ERROR, &error_msg, &raw, &raw_len) != 0) {
    return;
  }
  _unix_connection_send_frame_raw(conn, raw, raw_len);
  free(raw);
}

/* --- the ca_session_conn_t implementation --------------------------------- */

/* The handlers-side send (type + payload in, ENCODE + frame + QUEUE out).
   THE PAYLOAD'S OWNERSHIP (handlers.h's contract, both halves pinned):
   returns 0 = queued AND consumed (encoded on the caller's stack, then
   destroyed here); returns nonzero = refused — the payload stays the
   SENDER's, untouched (the handlers destroy it via ca_wire_payload_destroy).
   Refusals check the connection's liveness FIRST so a refused send never
   touches the payload. */
static int _unix_connection_send(void* c, uint64_t type, void* payload) {
  unix_connection_t* conn = (unix_connection_t*)c;
  if (conn == NULL || conn->sock == NULL) {
    return -1;
  }
  /* THREADED: called on the loop thread ONLY (the handlers marshal every
     send there); the encode is µs-scale on the caller's stack, the write is
     queued to the connection's actor. */
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  if (ca_wire_encode(type, payload, &raw, &raw_len) != 0) {
    /* an undecodable payload struct is the handler's bug — refused loud,
       the payload still the sender's */
    log_error("unix_connection: ca_wire_encode refused a %llu frame",
              (unsigned long long)type);
    return -1;
  }
  /* consumed on the queued path: the wire's destroy knows the struct */
  ca_wire_payload_destroy(type, payload);
  _unix_connection_send_frame_raw(conn, raw, raw_len);
  free(raw);
  return 0;
}

static void _unix_connection_retain(void* c) {
  refcounter_reference((refcounter_t*)c);
}

static void _unix_connection_release(void* c) {
  unix_connection_t* conn = (unix_connection_t*)c;
  if (conn != NULL) {
    unix_connection_destroy(conn);   /* refcounter-gated: frees at zero */
  }
}

/* --- watcher helpers (liboffs's shapes verbatim) ---------------------------- */

static void _connection_update_watcher(unix_connection_t* connection,
                                       pd_event_t events) {
  if (connection->transport == NULL) return;
  pd_watcher_t* watcher = ATOMIC_LOAD(&connection->watcher);
  if (watcher == NULL) return;
  unix_watcher_update_payload_t* payload =
      get_clear_memory(sizeof(unix_watcher_update_payload_t));
  payload->watcher = watcher;
  payload->events = events;
  message_t msg;
  msg.type = UNIX_SERVER_UPDATE_WATCHER;
  msg.payload = payload;
  msg.payload_destroy = free;
  actor_send(&connection->transport->actor, &msg);
}

static void _connection_stop_watcher(unix_connection_t* connection) {
  pd_watcher_t* watcher = ATOMIC_EXCHANGE(&connection->watcher, NULL);
  if (watcher == NULL) return;
  if (connection->transport != NULL) {
    unix_watcher_update_payload_t* payload =
        get_clear_memory(sizeof(unix_watcher_update_payload_t));
    payload->watcher = watcher;
    payload->events = 0;
    message_t msg;
    msg.type = UNIX_SERVER_STOP_WATCHER;
    msg.payload = payload;
    msg.payload_destroy = free;
    actor_send(&connection->transport->actor, &msg);
  } else {
    pd_watcher_stop(watcher);
    pd_watcher_destroy(watcher);
  }
}

static void _connection_close_fd(unix_connection_t* connection) {
  /* Snapshot the socket pointer and clear the field before destroying it.
     This matches the snapshot/clear pattern used for connection->watcher
     (see _connection_stop_watcher). It prevents a race with
     unix_transport_destroy, which checks `conn->sock != NULL` before
     destroying: by clearing the field first, the racing reader either sees
     NULL (and skips its own destroy) or has already lost the race and we
     are the sole owner here. */
  platform_socket_t* sock = connection->sock;
  connection->sock = NULL;
  if (sock != NULL) {
    platform_socket_destroy(sock);
  }
  ATOMIC_STORE(&connection->is_closing, 1);
  /* The handlers' teardown hook (handlers.h: the transports call it on every
     connection close — from any thread; it marshals). Fires BEFORE the
     transport reference is dropped by the destroy pass, so the server can
     purge this connection's subscriptions and pending round trips. ONE CALL
     per connection: clearing the borrow after it (and the transport pass's
     non-NULL check) makes a closed connection's second teardown pass skip —
     the purge runs exactly once. */
  if (connection->server != NULL) {
    ca_session_conn_closed(connection->server, &connection->iface);
    connection->server = NULL;
  }
}

/* --- the dispatch bridge --------------------------------------------------- */

static void _unix_dispatch_frame(unix_connection_t* conn, cbor_item_t* frame) {
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  int drc = ca_wire_decode(frame, &type, &payload, &req_id, &status);
  if (drc != 0) {
    /* An undecodable frame answers ONE error frame, echoing whatever req_id
       the wire could extract (the wire fills req_id on every refusal it can
       decode element 1 for — only a hopeless frame answers req_id 0).
       (The plan's sketch zeroed req_id before sending; the landed wire's
       echo contract supersedes that — the error frame tells the truth.) */
    _unix_connection_send_error(conn, req_id, 1, "malformed wire frame");
    return;
  }
  if (conn->server == NULL) {
    /* The transport is tearing down (the destroy pass dropped the borrow):
       refused loud, payload dies here. */
    ca_wire_payload_destroy(type, payload);
    log_error("unix_connection: a frame arrived on a transportless "
              "connection — refused loud");
    return;
  }
  if (ca_session_handle(conn->server, type, payload, &conn->iface) != 0) {
    /* The handler refused (dying server or dead loop) and destroyed the
       payload ITSELF; the connection is gone (nothing more can be sent). */
    log_error("unix: the handler refused a frame on a dead connection");
  }
  /* NO destroy on either path: on 0 the server consumed the payload (it
     rides the loop-thread closure); on nonzero the handler destroyed it. */
}

/* --- connection dispatch (runs on scheduler worker threads) ---------------- */

void unix_connection_dispatch(void* state, message_t* msg) {
  unix_connection_t* connection = (unix_connection_t*)state;

  /* The ENTRY GATE: reads the closing flag as this dispatch picks its work
     up. It is atomic — the teardown thread stores it while workers load it.
     But it gates NEW work only: a dispatch already past this line still runs
     (a queued WRITE, say), which is exactly the in-flight window the
     teardown closes by ORDER — its conn-watcher stop/destroy waits until the
     pool goes idle, so an in-flight _connection_update_watcher can never
     touch a dead pd_watcher_t. liboffs's gate is the same first-line check;
     ours additionally reads it well-defined. */
  if (ATOMIC_LOAD(&connection->is_closing)) {
    return;
  }

  switch (msg->type) {
    case UNIX_CONNECTION_DATA: {
      buffer_t* data = (buffer_t*)msg->payload;
      msg->payload = NULL;
      if (connection->sock == NULL) {
        DESTROY(data, buffer);
        break;
      }
      stream_framer_feed(connection->framer, data->data, data->size);
      DESTROY(data, buffer);

      size_t frame_len;
      uint8_t* frame_data;
      while ((frame_data = stream_framer_next(connection->framer, &frame_len)) != NULL) {
        struct cbor_load_result load_result;
        cbor_item_t* cbor_item = cbor_load(frame_data, frame_len, &load_result);
        free(frame_data);
        if (cbor_item == NULL || load_result.error.code != CBOR_ERR_NONE) {
          /* malformed CBOR: the frame carries nothing decodable — one
             error frame (req_id 0; no request id exists to echo), the
             stream itself stays healthy (its framing prefix is intact) */
          if (cbor_item != NULL) {
            cbor_decref(&cbor_item);
          }
          _unix_connection_send_error(connection, 0, 1,
                                      "malformed wire frame");
          continue;
        }
        _unix_dispatch_frame(connection, cbor_item);
        /* THE ITEM'S LIFETIME: cbor_load handed us the ref; the dispatch's
           bridge copied every field out into the payload structs (the
           wire's decode), so this decref is safe. No double-decref: the
           guard above keeps the vendored NULL-hazard shape honest. */
        cbor_decref(&cbor_item);
      }
      break;
    }

    case UNIX_CONNECTION_HANGUP:
    case UNIX_CONNECTION_ERROR: {
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    case UNIX_CONNECTION_WRITE: {
      buffer_t* buf = (buffer_t*)msg->payload;
      msg->payload = NULL;
      if (connection->sock == NULL) {
        DESTROY(buf, buffer);
        break;
      }
#ifdef _WIN32
      /* poll-dancer's IOCP backend drives only READ through overlapped I/O;
       * pd_watcher_update with PD_EVENT_WRITE is a no-op, so the
       * buffer-then-arm-WRITE strategy used on POSIX (below) can never flush
       * a partial send — a large frame would stall forever once the kernel
       * send buffer fills. On a local AF_UNIX socket the peer drains the
       * kernel buffer continuously, so a bounded synchronous retry lets the
       * full frame through without blocking the connection actor for long.
       * If the peer genuinely stalls we cap the retries and fall back to
       * write_buffer + arming WRITE (harmless on IOCP, keeps the POSIX path
       * intact). */
      {
        buffer_t* combined = buf;
        if (connection->write_buffer != NULL &&
            connection->write_buffer->size > 0) {
          size_t total = connection->write_buffer->size + buf->size;
          combined = buffer_create(total);
          memcpy(combined->data, connection->write_buffer->data,
                 connection->write_buffer->size);
          memcpy(combined->data + connection->write_buffer->size,
                 buf->data, buf->size);
          combined->size = total;
          DESTROY(connection->write_buffer, buffer);
          connection->write_buffer = NULL;
          DESTROY(buf, buffer);
        }
        size_t sent_total = 0;
        for (int attempts = 0; attempts < 2000 && sent_total < combined->size;
             attempts++) {
          ssize_t sent = platform_socket_send(connection->sock,
                                              combined->data + sent_total,
                                              combined->size - sent_total);
          if (sent > 0) {
            sent_total += (size_t)sent;
          } else if (sent == 0) {
            break;  /* peer closed */
          } else {
            platform_sleep_ms(1);  /* EAGAIN/EWOULDBLOCK: back off and retry */
          }
        }
        if (sent_total == combined->size) {
          DESTROY(combined, buffer);
        } else if (sent_total == 0) {
          DESTROY(combined, buffer);
          _connection_stop_watcher(connection);
          _connection_close_fd(connection);
        } else {
          size_t remaining = combined->size - sent_total;
          memmove(combined->data, combined->data + sent_total, remaining);
          combined->size = remaining;
          connection->write_buffer = combined;
          connection->write_pending = 1;
          _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        }
        break;
      }
#else
      if (connection->write_buffer != NULL &&
          connection->write_buffer->size > 0) {
        buffer_ensure_capacity(connection->write_buffer,
                               connection->write_buffer->size + buf->size);
        memcpy(connection->write_buffer->data +
                   connection->write_buffer->size,
               buf->data, buf->size);
        connection->write_buffer->size += buf->size;
        DESTROY(buf, buffer);
        break;
      }
      ssize_t sent = platform_socket_send(connection->sock, buf->data,
                                          buf->size);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          connection->write_buffer = buf;
          connection->write_pending = 1;
          _connection_update_watcher(connection,
                                     PD_EVENT_READ | PD_EVENT_WRITE);
        } else {
          DESTROY(buf, buffer);
          _connection_stop_watcher(connection);
          _connection_close_fd(connection);
        }
      } else if (sent == 0) {
        DESTROY(buf, buffer);
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      } else if ((size_t)sent < buf->size) {
        size_t remaining = buf->size - (size_t)sent;
        connection->write_buffer = buffer_create(remaining);
        memcpy(connection->write_buffer->data, buf->data + sent, remaining);
        connection->write_buffer->size = remaining;
        connection->write_pending = 1;
        _connection_update_watcher(connection,
                                   PD_EVENT_READ | PD_EVENT_WRITE);
        DESTROY(buf, buffer);
      } else {
        DESTROY(buf, buffer);
      }
      break;
#endif
    }

    case UNIX_CONNECTION_WRITABLE: {
      if (connection->sock == NULL) {
        break;
      }
      if (connection->write_buffer == NULL || connection->write_buffer->size == 0) {
        connection->write_pending = 0;
        _connection_update_watcher(connection, PD_EVENT_READ);
        break;
      }
      ssize_t sent = platform_socket_send(connection->sock,
                                          connection->write_buffer->data,
                                          connection->write_buffer->size);
      if (sent > 0) {
        if ((size_t)sent >= connection->write_buffer->size) {
          DESTROY(connection->write_buffer, buffer);
          connection->write_buffer = NULL;
          connection->write_pending = 0;
          if (ATOMIC_LOAD(&connection->is_closing)) {
            if (connection->sock != NULL) {
              platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
            }
            _connection_stop_watcher(connection);
            _connection_close_fd(connection);
            break;
          }
          _connection_update_watcher(connection, PD_EVENT_READ);
        } else {
          size_t remaining = connection->write_buffer->size - (size_t)sent;
          memmove(connection->write_buffer->data,
                  connection->write_buffer->data + sent, remaining);
          connection->write_buffer->size = remaining;
        }
      } else if (sent == 0) {
        DESTROY(connection->write_buffer, buffer);
        connection->write_buffer = NULL;
        connection->write_pending = 0;
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        DESTROY(connection->write_buffer, buffer);
        connection->write_buffer = NULL;
        connection->write_pending = 0;
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      }
      break;
    }

    case UNIX_CONNECTION_CLOSE: {
      if (connection->write_pending) {
        ATOMIC_STORE(&connection->is_closing, 1);
        _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        break;
      }
      if (connection->sock != NULL) {
        platform_socket_shutdown(connection->sock, PLATFORM_SHUT_WR);
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    default:
      break;
  }
}

/* --- read callback (runs on the transport's loop thread) ------------------- */

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                      pd_event_t events, void* user_data) {
  (void)loop;
  unix_connection_t* connection = (unix_connection_t*)user_data;

  if (events & PD_EVENT_WRITE) {
    message_t writable_msg;
    writable_msg.type = UNIX_CONNECTION_WRITABLE;
    writable_msg.payload = NULL;
    writable_msg.payload_destroy = NULL;
    actor_send(&connection->actor, &writable_msg);
  }

  if (events & (PD_EVENT_HANGUP | PD_EVENT_ERROR)) {
    message_t msg;
    msg.type = (events & PD_EVENT_HANGUP) ? UNIX_CONNECTION_HANGUP
                                          : UNIX_CONNECTION_ERROR;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    pd_watcher_t* claimed = ATOMIC_EXCHANGE(&connection->watcher, NULL);
    if (claimed != NULL) {
      pd_watcher_stop(claimed);
      /* Defer watcher destruction through server actor's destroy stack */
      if (connection->transport != NULL) {
        unix_watcher_update_payload_t* payload =
            get_clear_memory(sizeof(unix_watcher_update_payload_t));
        payload->watcher = claimed;
        payload->events = 0;
        message_t stop_msg;
        stop_msg.type = UNIX_SERVER_STOP_WATCHER;
        stop_msg.payload = payload;
        stop_msg.payload_destroy = free;
        actor_send(&connection->transport->actor, &stop_msg);
      } else {
        pd_watcher_destroy(claimed);
      }
    }
    return;
  }

  if (events & PD_EVENT_READ) {
    /* On Windows IOCP, the bytes that triggered this completion are
     * sitting in the watcher's internal buffer. Drain them into a
     * local buffer; if more than one buffer's worth is pending (rare
     * for a small request frame) we loop until the buffer is empty.
     * On POSIX backends pd_watcher_drain_read returns 0 and we fall
     * back to the synchronous platform_socket_recv. */
    uint8_t buffer[READ_BUFFER_SIZE];
    size_t total_read = 0;
    size_t n = pd_watcher_drain_read(watcher, buffer, sizeof(buffer));
    while (n > 0) {
      total_read += n;
      if (n < sizeof(buffer)) break; /* buffer fully drained */
      /* Buffer was exactly full: try once more in case more bytes are
       * already pending in a follow-up completion we haven't seen yet. */
      n = pd_watcher_drain_read(watcher, buffer, sizeof(buffer));
    }
    if (total_read == 0) {
      /* POSIX path: synchronous read. */
      ssize_t bytes_read = platform_socket_recv(connection->sock, buffer,
                                                sizeof(buffer));
      if (bytes_read <= 0) {
        if (bytes_read == 0) {
          message_t msg;
          msg.type = UNIX_CONNECTION_HANGUP;
          msg.payload = NULL;
          msg.payload_destroy = NULL;
          actor_send(&connection->actor, &msg);
          return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        message_t msg;
        msg.type = UNIX_CONNECTION_ERROR;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
        return;
      }
      total_read = (size_t)bytes_read;
    }

    buffer_t* data = buffer_create_from_pointer_copy(buffer, total_read);
    message_t msg;
    msg.type = UNIX_CONNECTION_DATA;
    msg.payload = data;
    msg.payload_destroy = (void (*)(void*))buffer_destroy;
    actor_send(&connection->actor, &msg);
  }
}

/* --- create / destroy ------------------------------------------------------ */

unix_connection_t* unix_connection_create(unix_transport_t* transport,
                                          platform_socket_t* sock) {
  unix_connection_t* connection =
      get_clear_memory(sizeof(unix_connection_t));
  refcounter_init((refcounter_t*)connection);
  connection->transport = transport;
  connection->server = transport->server;
  connection->sock = sock;
  connection->write_buffer = NULL;
  connection->write_pending = 0;
  ATOMIC_STORE(&connection->is_closing, 0);
  connection->framer = stream_framer_create();
  /* the handlers' view of this connection: the send machinery above, the
     refcounter pair for the lifetime */
  connection->iface.conn = connection;
  connection->iface.send = _unix_connection_send;
  connection->iface.retain = _unix_connection_retain;
  connection->iface.release = _unix_connection_release;

  actor_init(&connection->actor, connection, unix_connection_dispatch,
             transport->pool);

  platform_socket_set_nonblocking(sock);

  ATOMIC_STORE(&connection->watcher,
               platform_socket_watcher_create(transport->loop, sock,
    PD_EVENT_READ, _connection_read_callback, connection));
  if (ATOMIC_LOAD(&connection->watcher) == NULL) {
    /* Watcher creation failed: the connection could never receive I/O, so
       returning it half-built would leave the caller with an accepted-but-
       dead socket. Tear down what create built (loop-registry actor +
       framer) and return NULL; the caller's NULL branch destroys the socket
       and rearms the listener. unix_connection_destroy is not usable here —
       it would decrement active_connections and pop an entry that was never
       pushed. */
    actor_destroy(&connection->actor);
    stream_framer_destroy(connection->framer);
    free(connection);
    return NULL;
  }
  pd_watcher_start(ATOMIC_LOAD(&connection->watcher));

  return connection;
}

void unix_connection_destroy(unix_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  /* The refcounter pair IS the ca_session_conn_t's lifetime: the transport's
     vec holds the base ref; the server's held refs (subscriptions, in-flight
     closures) pin the object — the LAST release frees, whenever that is. */
  if (refcounter_dereference_is_zero((refcounter_t*)connection)) {
    if (connection->transport != NULL) {
      atomic_fetch_sub(&connection->transport->active_connections, 1);
      vec_remove(&connection->transport->connections, connection);
    }
    /* the actor's OWN teardown: flag + backpressure + registry detach + the
       RUNNING/queue-state waits + queue destroy (the transport destroy pass
       has already quiesced the pool; from a deferred free — a last release
       inside the server's unrefs — the actor may still sit queued/running in
       a LIVE pool, and this wait handles exactly that) */
    actor_destroy(&connection->actor);
    if (ATOMIC_LOAD(&connection->watcher) != NULL) {
      pd_watcher_t* watcher = ATOMIC_EXCHANGE(&connection->watcher, NULL);
      if (watcher != NULL) {
        /* Hand the socket to the STOP_WATCHER payload when the transport's
           actor is still reachable: closing it here — before the transport
           actor has unregistered the watcher — frees the fd number for
           reuse by an accepted connection, and the later unregister then
           removes THAT connection's (same-numbered) fd from the loop's
           epoll, leaving it accepted but never read. (From a transportless
           destroy — the transport teardown's own release pass, whose loop
           is already stopped — the direct pair is safe.) */
        unix_watcher_update_payload_t* payload = NULL;
        if (connection->transport != NULL) {
          payload = get_clear_memory(sizeof(unix_watcher_update_payload_t));
          payload->watcher = watcher;
          payload->events = 0;
          payload->sock = connection->sock;
          connection->sock = NULL;
          message_t msg;
          msg.type = UNIX_SERVER_STOP_WATCHER;
          msg.payload = payload;
          msg.payload_destroy = free;
          actor_send(&connection->transport->actor, &msg);
        } else {
          pd_watcher_stop(watcher);
          pd_watcher_destroy(watcher);
        }
      }
    }
    if (connection->sock != NULL) {
      platform_socket_destroy(connection->sock);
      connection->sock = NULL;
    }
    connection->transport = NULL;
    if (connection->framer != NULL) {
      stream_framer_destroy(connection->framer);
    }
    if (connection->write_buffer != NULL) {
      DESTROY(connection->write_buffer, buffer);
    }
    free(connection);
  }
}

void unix_connection_write(unix_connection_t* connection,
                           const uint8_t* data, size_t length) {
  if (connection == NULL || connection->sock == NULL) {
    return;
  }
  buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, length);
  message_t msg;
  msg.type = UNIX_CONNECTION_WRITE;
  msg.payload = buf;
  msg.payload_destroy = (void (*)(void*))buffer_destroy;
  actor_send(&connection->actor, &msg);
}

void unix_connection_close(unix_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  message_t msg;
  msg.type = UNIX_CONNECTION_CLOSE;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&connection->actor, &msg);
}

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */