//
// Created by victor on 5/7/26.
//
#include "http_connection.h"
#include "http_server.h"
#include "http_request.h"
#include "http_response.h"
#include "http_route.h"
#include "../Util/allocator.h"
#include "../Buffer/buffer.h"
#include "stream.h"
#include "../Util/validation.h"
#include "../Actor/actor.h"
#include "../Actor/message.h"
#include "streams_messages.h"
#include "../Scheduler/scheduler.h"
#include "../Util/log.h"
#include <string.h>
#ifdef _WIN32
  #include "../Platform/platform_posix_compat.h"
#else
  #include <unistd.h>
#endif
#include <errno.h>
#include <ctype.h>
#include <stdio.h>

#define READ_BUFFER_SIZE 4096
#define WRITE_BUFFER_BACKPRESSURE_THRESHOLD (256 * 1024)  /* 256KB */

static int _on_message_begin(http_parser* parser);
static int _on_url(http_parser* parser, const char* at, size_t length);
static int _on_header_field(http_parser* parser, const char* at, size_t length);
static int _on_header_value(http_parser* parser, const char* at, size_t length);
static int _on_headers_complete(http_parser* parser);
static int _on_body(http_parser* parser, const char* at, size_t length);
static int _on_message_complete(http_parser* parser);

static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data);
static void _connection_idle_timer_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                            pd_event_t events, void* user_data);
#ifndef _WIN32
/* POSIX-only: SSL_read runs against the kernel socket, which still holds the
 * bytes on epoll/kqueue. On Windows the worker instead decrypts ciphertext fed
 * into the memory read BIO by _connection_ssl_data_handle. */
static void _connection_do_reads(http_connection_t* connection);
#endif

static http_parser_settings _parser_settings = {
  .on_message_begin = _on_message_begin,
  .on_url = _on_url,
  .on_status = NULL,
  .on_header_field = _on_header_field,
  .on_header_value = _on_header_value,
  .on_headers_complete = _on_headers_complete,
  .on_body = _on_body,
  .on_message_complete = _on_message_complete,
  .on_chunk_header = NULL,
  .on_chunk_complete = NULL
};

static void _reset_header_accumulator(http_connection_t* connection) {
  if (connection->header_field != NULL) {
    connection->header_field[0] = '\0';
    connection->header_field_len = 0;
  }
  if (connection->header_value != NULL) {
    connection->header_value[0] = '\0';
    connection->header_value_len = 0;
  }
}

static int _accumulate_field(http_connection_t* connection, const char* at, size_t length) {
  if (connection->header_field_len + length > OFFS_MAX_HEADER_FIELD_LEN) {
    return -1;
  }
  if (connection->header_field == NULL) {
    connection->header_field_cap = length * 2 + 1;
    connection->header_field = get_memory(connection->header_field_cap);
  } else if (connection->header_field_len + length + 1 > connection->header_field_cap) {
    connection->header_field_cap = (connection->header_field_len + length) * 2 + 1;
    connection->header_field = realloc(connection->header_field, connection->header_field_cap);
  }
  memcpy(connection->header_field + connection->header_field_len, at, length);
  connection->header_field_len += length;
  connection->header_field[connection->header_field_len] = '\0';
  return 0;
}

static int _accumulate_value(http_connection_t* connection, const char* at, size_t length) {
  if (connection->header_value_len + length > OFFS_MAX_HEADER_VALUE_LEN) {
    return -1;
  }
  if (connection->header_value == NULL) {
    connection->header_value_cap = length * 2 + 1;
    connection->header_value = get_memory(connection->header_value_cap);
  } else if (connection->header_value_len + length + 1 > connection->header_value_cap) {
    connection->header_value_cap = (connection->header_value_len + length) * 2 + 1;
    connection->header_value = realloc(connection->header_value, connection->header_value_cap);
  }
  memcpy(connection->header_value + connection->header_value_len, at, length);
  connection->header_value_len += length;
  connection->header_value[connection->header_value_len] = '\0';
  return 0;
}

static char* _url_decode(const char* src, size_t length) {
  char* decoded = get_memory(length + 1);
  size_t decoded_len = 0;
  for (size_t i = 0; i < length; i++) {
    if (src[i] == '%' && i + 2 < length && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
      char hex[3] = {src[i + 1], src[i + 2], '\0'};
      decoded[decoded_len++] = (char)strtol(hex, NULL, 16);
      i += 2;
    } else if (src[i] == '+') {
      decoded[decoded_len++] = ' ';
    } else {
      decoded[decoded_len++] = src[i];
    }
  }
  decoded[decoded_len] = '\0';
  return decoded;
}

static void _flush_header(http_connection_t* connection) {
  if (connection->header_field != NULL && connection->header_field_len > 0 &&
      connection->header_value != NULL && connection->header_value_len > 0) {
    http_headers_set(&connection->request->headers, connection->header_field, connection->header_value);
    connection->header_count++;
  }
  _reset_header_accumulator(connection);
}

static int _on_message_begin(http_parser* parser) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  if (connection->request != NULL) {
    DESTROY(connection->request, http_request);
  }
  connection->request = http_request_create(connection->server->pool);
  _reset_header_accumulator(connection);
  connection->header_count = 0;
  return 0;
}

static int _on_url(http_parser* parser, const char* at, size_t length) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  size_t current_len = connection->request->url != NULL ? strlen(connection->request->url) : 0;
  if (current_len + length > OFFS_MAX_URL_LEN) {
    return -1;
  }
  if (connection->request->url == NULL) {
    connection->request->url = get_memory(length + 1);
    memcpy(connection->request->url, at, length);
    connection->request->url[length] = '\0';
  } else {
    connection->request->url = realloc(connection->request->url, current_len + length + 1);
    memcpy(connection->request->url + current_len, at, length);
    connection->request->url[current_len + length] = '\0';
  }
  return 0;
}

static int _on_header_field(http_parser* parser, const char* at, size_t length) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  if (connection->header_field_len > 0 && connection->header_value_len > 0) {
    _flush_header(connection);
  }
  /* Reject once the per-request header count is exceeded. header_count is
     bumped in _flush_header, so this checks the count of already-accepted
     headers before starting a new one. */
  if (connection->header_count >= OFFS_MAX_HEADER_COUNT) {
    return -1;
  }
  if (_accumulate_field(connection, at, length) != 0) {
    return -1;
  }
  return 0;
}

static int _on_header_value(http_parser* parser, const char* at, size_t length) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  if (_accumulate_value(connection, at, length) != 0) {
    return -1;
  }
  return 0;
}

static int _on_headers_complete(http_parser* parser) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  _flush_header(connection);
  connection->request->method = parser->method;
  connection->request->content_length = parser->content_length;
  connection->request->keep_alive = http_should_keep_alive(parser);
  connection->headers_complete = 1;

  if (connection->request->url != NULL) {
    char* url = connection->request->url;
    char* query = strchr(url, '?');
    if (query != NULL) {
      size_t path_len = query - url;
      char* raw_path = get_memory(path_len + 1);
      memcpy(raw_path, url, path_len);
      raw_path[path_len] = '\0';
      connection->request->path = _url_decode(raw_path, path_len);
      free(raw_path);
      connection->request->query_string = _url_decode(query + 1, strlen(query + 1));
    } else {
      connection->request->path = _url_decode(url, strlen(url));
    }
  }

  // Try early route match for streaming PUT
  if (connection->request->path != NULL && connection->request->method == HTTP_PUT) {
    http_route_t* route = http_server_match_route(connection->server,
                                                   connection->request->method,
                                                   connection->request->path);
    if (route != NULL && route->headers_complete_handler != NULL) {
      connection->streaming_route = route;
      http_response_t* response = http_response_create(connection->server->pool, connection);
      int streaming = route->headers_complete_handler(connection, connection->request, response);
      if (streaming) {
        http_response_destroy(response);
      } else {
        connection->streaming_route = NULL;
        DESTROY(response, http_response);
      }
    }
  }

  return 0;
}

static int _on_body(http_parser* parser, const char* at, size_t length) {
  http_connection_t* connection = (http_connection_t*)parser->data;

  /* Defensive sentinel: historically a heap-corruption bug left buffer->data
     NULL in the body handlers (see docs/OPERATIONS.md "Known issues"). The
     root cause is under investigation; guard the entry so a corrupted pointer
     logs and aborts the parse instead of dereferencing NULL. http-parser
     never legitimately invokes on_body with at==NULL or length==0, so these
     checks are free in the uncorrupted path. */
  if (at == NULL || length == 0 || connection == NULL || connection->request == NULL) {
    log_error("_on_body: NULL body pointer or length (at=%p length=%zu connection=%p request=%p) — heap corruption suspected",
              (const void*)at, length, (void*)connection,
              connection ? (void*)connection->request : NULL);
    return 1;
  }

  // Streaming mode: pipe body chunks directly into the request stream
  if (connection->streaming_route != NULL) {
    buffer_t* chunk = buffer_create_from_pointer_copy((uint8_t*)at, length);
    stream_notify((stream_t*)connection->request, data_event,
                  CONSUME(chunk, buffer_t), (void (*)(void*))buffer_destroy);
    return 0;
  }

  if (connection->request->content_length > OFFS_MAX_BUFFERED_BODY_SIZE) {
    return 1;
  }

  if (connection->request->body == NULL) {
    size_t initial_capacity = connection->request->content_length > 0
      ? (size_t)connection->request->content_length
      : (length < 8192 ? 8192 : length * 2);
    connection->request->body = buffer_create_with_capacity(0, initial_capacity);
    buffer_ensure_capacity(connection->request->body, length);
    if (connection->request->body->data == NULL) {
      log_error("_on_body: buffer->data NULL after ensure_capacity — heap corruption suspected");
      return 1;
    }
    memcpy(connection->request->body->data, at, length);
    connection->request->body->size = length;
  } else {
    size_t needed = connection->request->body->size + length;
    buffer_ensure_capacity(connection->request->body, needed);
    if (connection->request->body->data == NULL) {
      log_error("_on_body: buffer->data NULL after ensure_capacity (append path) — heap corruption suspected");
      return 1;
    }
    memcpy(connection->request->body->data + connection->request->body->size, at, length);
    connection->request->body->size = needed;
  }
  return 0;
}

static int _on_message_complete(http_parser* parser) {
  http_connection_t* connection = (http_connection_t*)parser->data;
  connection->request_complete = 1;

  // Streaming mode: signal end of body data and skip normal dispatch
  if (connection->streaming_route != NULL) {
    stream_notify((stream_t*)connection->request, close_event, NULL, NULL);
    connection->streaming_route = NULL;
    DESTROY(connection->request, http_request);
    connection->request = NULL;
    http_parser_init(&connection->parser, HTTP_REQUEST);
    connection->headers_complete = 0;
    connection->request_complete = 0;
    return 0;
  }

  http_response_t* response = http_response_create(
    connection->server->pool, connection);

  http_server_dispatch(connection->server, connection->request, response);

  if (response->is_piped) {
    http_response_destroy(response);
  } else {
    DESTROY(response, http_response);
  }

  DESTROY(connection->request, http_request);
  connection->request = NULL;

  http_parser_init(&connection->parser, HTTP_REQUEST);
  connection->headers_complete = 0;
  connection->request_complete = 0;
  return 0;
}

/* Helper: send a watcher update to the server actor (runs on scheduler thread).
 * pd_watcher_update is thread-safe (epoll_ctl MOD). No-op when server is NULL
 * (shutdown) since the I/O thread is already stopped. */
static void _connection_update_watcher(http_connection_t* connection, pd_event_t events) {
  if (connection->server == NULL) return;
  pd_watcher_t* watcher = ATOMIC_LOAD(&connection->watcher);
  if (watcher == NULL) return;
  watcher_update_payload_t* payload = get_clear_memory(sizeof(watcher_update_payload_t));
  payload->watcher = watcher;
  payload->events = events;
  message_t msg;
  msg.type = HTTP_SERVER_UPDATE_WATCHER;
  msg.payload = payload;
  msg.payload_destroy = free;
  actor_send(&connection->server->actor, &msg);
}

/* Helper: stop the connection's watcher via the server actor (runs on scheduler thread).
 * Uses atomic exchange to claim the watcher — only one caller will succeed,
 * preventing double-free when both the I/O callback and a dispatch handler
 * try to stop the same watcher. The server actor calls pd_watcher_stop (thread-safe)
 * and defers pd_watcher_destroy to the I/O thread. When server is NULL (shutdown),
 * the I/O thread is already stopped so we stop+destroy directly. */
static void _connection_stop_watcher(http_connection_t* connection) {
  pd_watcher_t* watcher = ATOMIC_EXCHANGE(&connection->watcher, NULL);
  if (watcher == NULL) return;
  if (connection->server != NULL) {
    watcher_update_payload_t* payload = get_clear_memory(sizeof(watcher_update_payload_t));
    payload->watcher = watcher;
    payload->events = 0;
    message_t msg;
    msg.type = HTTP_SERVER_STOP_WATCHER;
    msg.payload = payload;
    msg.payload_destroy = free;
    actor_send(&connection->server->actor, &msg);
  } else {
    pd_watcher_stop(watcher);
    pd_watcher_destroy(watcher);
  }
}

/* Close the fd and mark connection as closing. Used from dispatch (worker thread). */
static void _connection_close_fd(http_connection_t* connection) {
  platform_socket_t* sock = ATOMIC_EXCHANGE(&connection->sock, NULL);
  if (sock != NULL) {
    if (connection->server != NULL) {
      http_server_defer_socket_destroy(connection->server, sock);
    } else {
      platform_socket_destroy(sock);
    }
  }
  connection->is_closing = 1;
}

#ifdef _WIN32
/* poll-dancer's IOCP backend never delivers PD_EVENT_WRITE for the server (it
 * issues no overlapped writes of its own), so the buffer-then-arm-WRITE strategy
 * used on POSIX can't flush a partial send on Windows — a large response body
 * stalls once the kernel send buffer fills. On loopback the peer drains
 * continuously, so a bounded synchronous retry completes the send. Returns
 * 0 = fully sent, 1 = partial (cap exhausted), -1 = peer closed, -2 = hard
 * error. *out_sent receives the bytes accepted. Mirrors
 * tcp_connection.c:_connection_send_all_blocking for the plain case. */
static int _connection_send_raw_blocking(http_connection_t* connection,
                                          const uint8_t* data, size_t len,
                                          size_t* out_sent) {
  size_t sent_total = 0;
  platform_socket_t* sock = ATOMIC_LOAD(&connection->sock);
  for (int attempts = 0; attempts < 2000 && sent_total < len; attempts++) {
    ssize_t sent = platform_socket_send(sock, data + sent_total,
                                         len - sent_total);
    if (sent > 0) {
      sent_total += (size_t)sent;
    } else if (sent == 0) {
      *out_sent = sent_total;
      return -1;  /* peer closed */
    } else {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        *out_sent = sent_total;
        return -2;  /* hard error */
      }
      platform_sleep_ms(1);  /* EAGAIN: back off and retry */
    }
  }
  *out_sent = sent_total;
  return (sent_total == len) ? 0 : 1;
}

/* Drain ciphertext OpenSSL emitted into the write BIO (handshake response
 * records, key-update records, or SSL_write output) and send it with bounded
 * retry. Returns 0 on success, -1 if a record could not be fully flushed — the
 * caller must close the connection, since TLS records are session-ordered and
 * must not be partially dropped. */
static int _connection_ssl_flush_wbio(http_connection_t* connection) {
  uint8_t cipher[READ_BUFFER_SIZE];
  for (;;) {
    int n = BIO_read(connection->wbio, cipher, sizeof(cipher));
    if (n <= 0) break;
    size_t sent = 0;
    if (_connection_send_raw_blocking(connection, cipher, (size_t)n, &sent) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Send plaintext over TLS via the memory write BIO. SSL_write emits TLS records
 * into wbio — a memory BIO never blocks, so the full plaintext slice is always
 * accepted in one call — then we drain wbio and raw-send the ciphertext. wbio is
 * always drained to completion before returning, so a ciphertext send that can't
 * finish within the bounded retry is fatal for the connection (returns -2; the
 * caller closes). *out_sent receives the plaintext bytes accepted into the BIO. */
static int _connection_ssl_send_blocking(http_connection_t* connection,
                                          const uint8_t* plaintext, size_t len,
                                          size_t* out_sent) {
  size_t sent_total = 0;
  uint8_t cipher[READ_BUFFER_SIZE];
  while (sent_total < len) {
    int n = SSL_write(connection->ssl, plaintext + sent_total,
                      (int)(len - sent_total));
    if (n <= 0) {
      /* A memory write BIO never blocks, so WANT_WRITE/WANT_READ should not
       * happen here; any non-success is a hard error. Flush whatever was already
       * emitted, then fail. */
      (void)_connection_ssl_flush_wbio(connection);
      *out_sent = sent_total;
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      return -2;
    }
    sent_total += (size_t)n;
    for (;;) {
      int c = BIO_read(connection->wbio, cipher, sizeof(cipher));
      if (c <= 0) break;
      size_t csent = 0;
      if (_connection_send_raw_blocking(connection, cipher, (size_t)c, &csent) != 0) {
        *out_sent = sent_total;
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
        return -2;
      }
    }
  }
  *out_sent = sent_total;
  return 0;
}

static int _connection_send_all_blocking(http_connection_t* connection,
                                         buffer_t* combined, size_t* out_sent) {
  if (connection->is_ssl && connection->ssl != NULL) {
    return _connection_ssl_send_blocking(connection, combined->data,
                                         combined->size, out_sent);
  }
  return _connection_send_raw_blocking(connection, combined->data,
                                       combined->size, out_sent);
}

/* Windows IOCP: the I/O-thread read callback drained the watcher's completed
 * read into the DATA message payload as ciphertext. Feed it into the SSL read
 * memory BIO and pump SSL_read: OpenSSL drives the handshake internally and
 * returns SSL_ERROR_WANT_READ (need more ciphertext) or SSL_ERROR_WANT_WRITE
 * (handshake response / key-update records buffered in wbio — flush them to the
 * socket and retry) until the handshake completes, then it returns decrypted
 * application data, which is routed through http_parser. This works for TLS 1.2
 * and 1.3 (SSL_read processes the client Finished + early data in 1.3) without
 * any explicit SSL_do_handshake / SSL_is_init_complete gating. All SSL/BIO calls
 * run on this worker thread — the I/O thread never touches SSL — so there is no
 * cross-thread race on the BIO. */
static void _connection_ssl_data_handle(http_connection_t* connection,
                                        buffer_t* data) {
  if (ATOMIC_LOAD(&connection->sock) == NULL || connection->ssl == NULL) {
    return;
  }
  BIO_write(connection->rbio, data->data, (int)data->size);

  for (int batch = 0; batch < 16; batch++) {
    if (ATOMIC_LOAD(&connection->sock) == NULL) {
      return;
    }
    char buffer[READ_BUFFER_SIZE];
    int bytes_read = SSL_read(connection->ssl, buffer, sizeof(buffer));
    if (bytes_read > 0) {
      buffer_t* plain = buffer_create_from_pointer_copy((uint8_t*)buffer, (size_t)bytes_read);
      size_t nparsed = http_parser_execute(&connection->parser, &_parser_settings,
                                            (const char*)plain->data, plain->size);
      (void)nparsed;
      DESTROY(plain, buffer);
      if (connection->parser.http_errno != HPE_OK) {
        /* Parse error — close the connection */
        if (connection->piped_pending) {
          if (connection->streaming_route != NULL && connection->request != NULL) {
            stream_deactivate((stream_t*)connection->request,
                              OFFS_ERROR("Connection closed during streaming upload"));
            connection->streaming_route = NULL;
          }
        }
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
        return;
      }
      continue;  /* more decrypted application data may be available */
    }
    int ssl_err = SSL_get_error(connection->ssl, bytes_read);
    if (ssl_err == SSL_ERROR_WANT_READ) {
      /* Need more ciphertext — wait for the next DATA. Flush anything this turn
       * emitted (handshake response, key-update ack) first. */
      if (_connection_ssl_flush_wbio(connection) != 0) {
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      }
      return;
    }
    if (ssl_err == SSL_ERROR_WANT_WRITE) {
      /* wbio has pending output — flush it to the socket and retry SSL_read. */
      if (_connection_ssl_flush_wbio(connection) != 0) {
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
        return;
      }
      continue;
    }
    /* Peer close_notify (SSL_ERROR_ZERO_RETURN) or hard error. */
    message_t msg;
    msg.type = HTTP_CONNECTION_HANGUP;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    _connection_stop_watcher(connection);
    return;
  }
  (void)_connection_ssl_flush_wbio(connection);
}
#endif

#ifndef _WIN32
/* POSIX SSL write: SSL_write against the kernel socket (SSL_set_fd binding).
 * The nonblocking socket may return WANT_WRITE/WANT_READ during TLS record
 * framing or key-update renegotiation; on loopback the peer drains continuously
 * so a bounded synchronous retry completes the send without blocking the
 * connection actor for long. Mirrors the Windows IOCP SSL send path's retry
 * budget. Returns 0 = fully sent, -1 = hard error or partial (caller closes,
 * since TLS records are session-ordered and must not be partially dropped). */
static int _connection_ssl_send_posix(http_connection_t* connection,
                                       const uint8_t* data, size_t len) {
  size_t sent_total = 0;
  for (int attempts = 0; attempts < 2000 && sent_total < len; attempts++) {
    int written = SSL_write(connection->ssl, data + sent_total,
                             (int)(len - sent_total));
    if (written > 0) {
      sent_total += (size_t)written;
      continue;
    }
    int err = SSL_get_error(connection->ssl, written);
    if (err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ) {
      return -1;
    }
    platform_sleep_ms(1);
  }
  return (sent_total == len) ? 0 : -1;
}
#endif

/* Connection actor dispatch — runs on scheduler worker threads. */
void http_connection_dispatch(void* state, message_t* msg) {
  http_connection_t* connection = (http_connection_t*)state;
  if (connection->is_closing) {
    /* Connection is shutting down — discard all messages. */
    return;
  }

  switch (msg->type) {
    case HTTP_CONNECTION_READABLE: {
      /* ASIO-style: the I/O thread notified us that data is available.
         Perform the actual recv() and parsing here on the scheduler worker. */
      atomic_store(&connection->read_pending, 0);
      if (ATOMIC_LOAD(&connection->sock) == NULL) {
        break;
      }
#ifndef _WIN32
      _connection_do_reads(connection);
#endif
      break;
    }

    case HTTP_CONNECTION_DATA: {
      /* On Windows the I/O-thread read callback drains both plain and SSL
         connections and ships the bytes here (plaintext for plain, ciphertext
         for SSL). On POSIX this is the legacy pre-read path; normal SSL I/O
         goes through READABLE -> _connection_do_reads. */
      buffer_t* data = (buffer_t*)msg->payload;
      msg->payload = NULL; /* Take ownership — actor_run won't destroy it */
      if (ATOMIC_LOAD(&connection->sock) == NULL) {
        DESTROY(data, buffer);
        break;
      }
#ifdef _WIN32
      if (connection->is_ssl && connection->ssl != NULL) {
        _connection_ssl_data_handle(connection, data);
        DESTROY(data, buffer);
        break;
      }
#endif
      size_t nparsed = http_parser_execute(&connection->parser, &_parser_settings,
                                            (const char*)data->data, data->size);
      (void)nparsed;
      DESTROY(data, buffer);
      if (connection->parser.http_errno != HPE_OK) {
        /* Parse error — close the connection */
        if (connection->piped_pending) {
          if (connection->streaming_route != NULL && connection->request != NULL) {
            stream_deactivate((stream_t*)connection->request,
                              OFFS_ERROR("Connection closed during streaming upload"));
            connection->streaming_route = NULL;
          }
        }
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      }
      break;
    }

    case HTTP_CONNECTION_HANGUP:
    case HTTP_CONNECTION_ERROR: {
      if (connection->piped_pending) {
        if (connection->streaming_route != NULL && connection->request != NULL) {
          stream_deactivate((stream_t*)connection->request,
                            OFFS_ERROR("Connection closed during streaming upload"));
          connection->streaming_route = NULL;
        }
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    case HTTP_CONNECTION_WRITE: {
      buffer_t* buf = (buffer_t*)msg->payload;
      msg->payload = NULL; /* Take ownership — actor_run won't destroy it */
      if (ATOMIC_LOAD(&connection->sock) == NULL) {
        DESTROY(buf, buffer);
        break;
      }
#ifdef _WIN32
      /* IOCP never delivers PD_EVENT_WRITE, so flush the full buffer with a
       * bounded blocking retry instead of buffering + arming WRITE. See
       * _connection_send_all_blocking above. The POSIX event-driven path is
       * preserved in the #else branch. */
      {
        buffer_t* combined = buf;
        if (connection->write_buffer != NULL && connection->write_buffer->size > 0) {
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
        int rc = _connection_send_all_blocking(connection, combined, &sent_total);
        if (rc == 0) {
          DESTROY(combined, buffer);
        } else if (rc < 0) {
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
      if (connection->is_ssl && connection->ssl != NULL) {
        /* POSIX SSL write: bounded-retry SSL_write against the kernel socket.
           See _connection_ssl_send_posix. A partial send that can't finish
           within the retry budget is fatal-close (TLS records are session-
           ordered and must not be partially dropped). The plain HTTP path
           below remains event-driven (buffer + arm WRITE) since plain sends
           have no TLS renegotiation coupling. */
        int rc = _connection_ssl_send_posix(connection, buf->data, buf->size);
        DESTROY(buf, buffer);
        if (rc != 0) {
          _connection_stop_watcher(connection);
          _connection_close_fd(connection);
        }
        break;
      }
      /* If there's already buffered data, append to it */
      if (connection->write_buffer != NULL && connection->write_buffer->size > 0) {
        buffer_ensure_capacity(connection->write_buffer, connection->write_buffer->size + buf->size);
        memcpy(connection->write_buffer->data + connection->write_buffer->size,
               buf->data, buf->size);
        connection->write_buffer->size += buf->size;
        DESTROY(buf, buffer);
        break;
      }
      /* Try direct send */
      ssize_t sent = platform_socket_send(ATOMIC_LOAD(&connection->sock), buf->data, buf->size);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          connection->write_buffer = buf;
          connection->write_pending = 1;
          _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        } else {
          DESTROY(buf, buffer);
          _connection_stop_watcher(connection);
          _connection_close_fd(connection);
        }
      } else if (sent == 0) {
        /* Peer closed read side — stop writing */
        DESTROY(buf, buffer);
        _connection_stop_watcher(connection);
        _connection_close_fd(connection);
      } else if ((size_t)sent < buf->size) {
        size_t remaining = buf->size - (size_t)sent;
        connection->write_buffer = buffer_create(remaining);
        memcpy(connection->write_buffer->data, buf->data + sent, remaining);
        connection->write_buffer->size = remaining;
        connection->write_pending = 1;
        _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        DESTROY(buf, buffer);
      } else {
        DESTROY(buf, buffer);
      }
      break;
#endif
    }

    case HTTP_CONNECTION_WRITABLE: {
      if (ATOMIC_LOAD(&connection->sock) == NULL) {
        break;
      }
      if (connection->write_buffer == NULL || connection->write_buffer->size == 0) {
        connection->write_pending = 0;
        _connection_update_watcher(connection, PD_EVENT_READ);
        break;
      }
      ssize_t sent = platform_socket_send(ATOMIC_LOAD(&connection->sock), connection->write_buffer->data,
                           connection->write_buffer->size);
      if (sent > 0) {
        if ((size_t)sent >= connection->write_buffer->size) {
          DESTROY(connection->write_buffer, buffer);
          connection->write_buffer = NULL;
          connection->write_pending = 0;
          if (connection->is_closing) {
            /* All data flushed — finish the deferred close */
            if (ATOMIC_LOAD(&connection->sock) != NULL) {
              platform_socket_shutdown(ATOMIC_LOAD(&connection->sock), PLATFORM_SHUT_WR);
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
        /* Peer closed read side */
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

    case HTTP_CONNECTION_CLOSE: {
      /* If there's still buffered write data, defer the close until it's
       * flushed. Otherwise the shutdown truncates the remaining bytes,
       * causing ERR_CONTENT_LENGTH_MISMATCH on the client. */
      if (connection->write_pending) {
        connection->is_closing = 1;
        _connection_update_watcher(connection, PD_EVENT_READ | PD_EVENT_WRITE);
        break;
      }
      if (ATOMIC_LOAD(&connection->sock) != NULL) {
        platform_socket_shutdown(ATOMIC_LOAD(&connection->sock), PLATFORM_SHUT_WR);
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      break;
    }

    default:
      break;
  }
}

/* Idle/hard timeout callback — runs on the I/O thread. The connection's
   request, write_buffer, and sock state are owned by the worker (the actor),
   so this callback must NOT touch them directly. It sends an
   HTTP_CONNECTION_CLOSE message to the connection actor, which runs the
   existing close path (stop watcher, close fd) on the worker. The timer is
   one-shot, so no stop is needed here. Matches the I/O->worker deferral
   pattern used by _connection_read_callback for HANGUP/ERROR. */
static void _connection_idle_timer_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                            pd_event_t events, void* user_data) {
  (void)loop;
  (void)watcher;
  (void)events;
  http_connection_t* connection = (http_connection_t*)user_data;
  log_warn("http_connection: idle/hard timeout — closing connection");
  message_t msg;
  msg.type = HTTP_CONNECTION_CLOSE;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&connection->actor, &msg);
}

/* I/O event callback — runs on the I/O thread. For plain HTTP it drains the
   bytes that completed the read directly here and ships them to the
   connection actor as a DATA message for parsing. For SSL it can't — the
   ciphertext has to be decrypted via SSL_read on the worker — so it sends a
   READABLE notification and the actor recvs/decrypts. The split exists because
   of Windows IOCP: a completion-based backend places the read bytes in the
   watcher's overlapped buffer, which is only valid until this callback
   returns (the loop re-arms a fresh WSARecv into the same buffer afterward),
   so the worker can't recv() them later — it would see an empty socket
   (EAGAIN) and never parse anything. Draining here and sending DATA mirrors
   unix_connection and works on every backend (on POSIX
   pd_watcher_drain_read returns 0 and we fall back to a synchronous recv). */
static void _connection_read_callback(pd_loop_t* loop, pd_watcher_t* watcher,
                                       pd_event_t events, void* user_data) {
  (void)loop;
  http_connection_t* connection = (http_connection_t*)user_data;

  /* Re-arm the idle timer on any I/O activity (READ or WRITE) so a connection
     that dribbles bytes never trips the idle timeout, and check the hard
     deadline so a slow-but-steady request is bounded. Both run on the I/O
     thread, which owns the timer. The timer is one-shot; stop+start restarts
     its countdown from server->idle_timeout_ms. */
  if (connection->idle_timer != NULL && (events & (PD_EVENT_READ | PD_EVENT_WRITE))) {
    pd_timer_stop(connection->idle_timer);
    pd_timer_start(connection->idle_timer);
  }
  if (connection->hard_deadline_ms != 0 &&
      (platform_monotonic_ns() / 1000000ULL) > connection->hard_deadline_ms) {
    message_t msg;
    msg.type = HTTP_CONNECTION_CLOSE;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    _connection_stop_watcher(connection);
    return;
  }

  if (events & PD_EVENT_WRITE) {
    /* Socket is writable — notify actor to flush buffered write data */
    message_t writable_msg;
    writable_msg.type = HTTP_CONNECTION_WRITABLE;
    writable_msg.payload = NULL;
    writable_msg.payload_destroy = NULL;
    actor_send(&connection->actor, &writable_msg);
  }

  if (events & (PD_EVENT_HANGUP | PD_EVENT_ERROR)) {
    message_t msg;
    msg.type = (events & PD_EVENT_HANGUP) ? HTTP_CONNECTION_HANGUP : HTTP_CONNECTION_ERROR;
    msg.payload = NULL;
    msg.payload_destroy = NULL;
    actor_send(&connection->actor, &msg);
    /* Stop the watcher via the server actor's deferred destroy path. A
       completion-based backend (Windows IOCP) may still have an in-flight
       overlapped on this watcher; freeing it here would race that
       completion (use-after-free). _connection_stop_watcher claims the
       watcher atomically and defers pd_watcher_destroy to the I/O-thread
       destroy stack, so the pending completion is drained before the free. */
    _connection_stop_watcher(connection);
    return;
  }

  if (events & PD_EVENT_READ) {
#ifndef _WIN32
    if (connection->is_ssl && connection->ssl != NULL) {
      /* POSIX: SSL decryption runs on the worker via SSL_read against the
         socket, which still holds the bytes on epoll/kqueue, so just notify.
         One READABLE in flight at a time backpressures via the TCP window. */
      if (atomic_load(&connection->read_pending) == 0) {
        atomic_store(&connection->read_pending, 1);
        message_t msg;
        msg.type = HTTP_CONNECTION_READABLE;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
      }
      return;
    }
#endif

    /* Plain HTTP (POSIX), or every read on Windows (plain plaintext + SSL
       ciphertext): drain the watcher's completed read buffer here and ship the
       bytes to the actor as DATA. On Windows the SSL DATA is fed into the
       memory read BIO on the worker by _connection_ssl_data_handle. Loop in
       case more than one buffer's worth completed. */
    uint8_t buffer[READ_BUFFER_SIZE];
    size_t total_read = 0;
    size_t n = pd_watcher_drain_read(watcher, buffer, sizeof(buffer));
    while (n > 0) {
      total_read += n;
      if (n < sizeof(buffer)) break; /* buffer fully drained */
      n = pd_watcher_drain_read(watcher, buffer, sizeof(buffer));
    }
    if (total_read == 0) {
      /* POSIX path: synchronous recv. The socket may already be closed if the
         connection was torn down concurrently with a pending READ event. */
      platform_socket_t* sock = ATOMIC_LOAD(&connection->sock);
      if (sock == NULL) {
        return;
      }
      ssize_t bytes_read = platform_socket_recv(sock, buffer, sizeof(buffer));
      if (bytes_read <= 0) {
        if (bytes_read == 0) {
          message_t msg;
          msg.type = HTTP_CONNECTION_HANGUP;
          msg.payload = NULL;
          msg.payload_destroy = NULL;
          actor_send(&connection->actor, &msg);
          return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        message_t msg;
        msg.type = HTTP_CONNECTION_ERROR;
        msg.payload = NULL;
        msg.payload_destroy = NULL;
        actor_send(&connection->actor, &msg);
        return;
      }
      total_read = (size_t)bytes_read;
    }

    buffer_t* data = buffer_create_from_pointer_copy(buffer, total_read);
    message_t msg;
    msg.type = HTTP_CONNECTION_DATA;
    msg.payload = data;
    msg.payload_destroy = (void (*)(void*))buffer_destroy;
    actor_send(&connection->actor, &msg);
  }
}

#ifndef _WIN32
/* Perform a batch of SSL reads and HTTP parsing. Called from the connection
   actor dispatch (HTTP_CONNECTION_READABLE) on scheduler worker threads. The
   I/O-thread read callback sends READABLE only for SSL connections — plain HTTP
   is drained in the callback and shipped as a DATA message — so this path is
   SSL-only: SSL_read must run on the worker to decrypt the ciphertext against
   the kernel socket (which still holds the bytes on epoll/kqueue). On Windows
   the SSL path instead feeds IOCP-drained ciphertext into the memory read BIO
   via _connection_ssl_data_handle, so this function is unused there. */
static void _connection_do_reads(http_connection_t* connection) {
  if (!connection->is_ssl || connection->ssl == NULL) {
    return;
  }
  for (int batch = 0; batch < 16; batch++) {
    char buffer[READ_BUFFER_SIZE];
    if (ATOMIC_LOAD(&connection->sock) == NULL) {
      return;
    }

    ssize_t bytes_read = SSL_read(connection->ssl, buffer, sizeof(buffer));
    if (bytes_read <= 0) {
      int ssl_error = SSL_get_error(connection->ssl, (int)bytes_read);
      if (ssl_error == SSL_ERROR_WANT_READ) {
        return;
      }
      message_t msg;
      msg.type = HTTP_CONNECTION_HANGUP;
      msg.payload = NULL;
      msg.payload_destroy = NULL;
      actor_send(&connection->actor, &msg);
      /* Stop via the deferred path so a completion-based backend doesn't free
         the watcher's overlapped while a completion is still pending. */
      _connection_stop_watcher(connection);
      return;
    }

    buffer_t* data = buffer_create_from_pointer_copy((uint8_t*)buffer, (size_t)bytes_read);
    size_t nparsed = http_parser_execute(&connection->parser, &_parser_settings,
                                          (const char*)data->data, data->size);
    (void)nparsed;
    DESTROY(data, buffer);
    if (connection->parser.http_errno != HPE_OK) {
      /* Parse error — close the connection */
      if (connection->piped_pending) {
        if (connection->streaming_route != NULL && connection->request != NULL) {
          stream_deactivate((stream_t*)connection->request,
                            OFFS_ERROR("Connection closed during streaming upload"));
          connection->streaming_route = NULL;
        }
      }
      _connection_stop_watcher(connection);
      _connection_close_fd(connection);
      return;
    }
  }
}
#endif

http_connection_t* http_connection_create(http_server_t* server, platform_socket_t* sock) {
  http_connection_t* connection = get_clear_memory(sizeof(http_connection_t));
  refcounter_init((refcounter_t*)connection);
  connection->server = server;
  ATOMIC_STORE(&connection->sock, sock);
  connection->ssl = NULL;
  connection->rbio = NULL;
  connection->wbio = NULL;
  connection->is_ssl = 0;
  connection->headers_complete = 0;
  connection->request_complete = 0;
  connection->request = NULL;
  connection->write_buffer = NULL;
  connection->write_pending = 0;
  atomic_store(&connection->read_pending, 0);
  connection->header_field = NULL;
  connection->header_field_len = 0;
  connection->header_field_cap = 0;
  connection->header_value = NULL;
  connection->header_value_len = 0;
  connection->header_value_cap = 0;
  connection->streaming_route = NULL;
  connection->idle_timer = NULL;
  connection->hard_deadline_ms = 0;

  actor_init(&connection->actor, connection, http_connection_dispatch, server->pool);

  platform_socket_set_nonblocking(sock);

  http_parser_init(&connection->parser, HTTP_REQUEST);
  connection->parser.data = connection;

  ATOMIC_STORE(&connection->watcher, pd_watcher_create(server->loop, platform_socket_fd(sock),
    PD_EVENT_READ, _connection_read_callback, connection));
  if (ATOMIC_LOAD(&connection->watcher) != NULL) {
    pd_watcher_start(ATOMIC_LOAD(&connection->watcher));
  }

  /* Arm the slowloris idle timer + set the hard request deadline. The timer
     fires on the I/O loop after idle_timeout_ms with no I/O activity; the hard
     deadline is checked on each read callback. Both send HTTP_CONNECTION_CLOSE
     to the connection actor, which runs the existing close path on the worker. */
  connection->hard_deadline_ms =
      (platform_monotonic_ns() / 1000000ULL) + server->hard_timeout_ms;
  connection->idle_timer = pd_timer_create(server->loop, server->idle_timeout_ms,
                                           0, _connection_idle_timer_callback, connection);
  if (connection->idle_timer != NULL) {
    pd_timer_start(connection->idle_timer);
  }

  return connection;
}

void http_connection_destroy(http_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  if (refcounter_dereference_is_zero((refcounter_t*)connection)) {
    if (connection->server != NULL) {
      atomic_fetch_sub(&connection->server->active_connections, 1);
      vec_remove(&connection->server->connections, connection);
    }
    actor_destroy(&connection->actor);
    if (ATOMIC_LOAD(&connection->watcher) != NULL) {
      pd_watcher_t* watcher = ATOMIC_EXCHANGE(&connection->watcher, NULL);
      if (watcher != NULL) {
        /* During server shutdown (server == NULL), the I/O thread is already
         * stopped so we can stop and destroy the watcher directly. During
         * normal operation, defer stop+destroy to the I/O thread via the
         * server actor's destroy stack. */
        if (connection->server != NULL) {
          watcher_update_payload_t* payload = get_clear_memory(sizeof(watcher_update_payload_t));
          payload->watcher = watcher;
          payload->events = 0;
          message_t msg;
          msg.type = HTTP_SERVER_STOP_WATCHER;
          msg.payload = payload;
          msg.payload_destroy = free;
          actor_send(&connection->server->actor, &msg);
        } else {
          pd_watcher_stop(watcher);
          pd_watcher_destroy(watcher);
        }
      }
    }
    if (connection->idle_timer != NULL) {
      pd_timer_t* timer = connection->idle_timer;
      connection->idle_timer = NULL;
      /* Same deferred-stop+destroy pattern as the watcher: the timer lives on
         the I/O loop and the I/O thread may be in its callback or walking loop
         timer state, so push stop+destroy onto the I/O-thread destroy stack.
         During server shutdown (server == NULL) the I/O thread is already
         joined, so stop+destroy directly. */
      if (connection->server != NULL) {
        watcher_update_payload_t* payload = get_clear_memory(sizeof(watcher_update_payload_t));
        payload->watcher = NULL;
        payload->events = 0;
        payload->timer = timer;
        message_t msg;
        msg.type = HTTP_SERVER_STOP_WATCHER;
        msg.payload = payload;
        msg.payload_destroy = free;
        actor_send(&connection->server->actor, &msg);
      } else {
        pd_timer_stop(timer);
        pd_timer_destroy(timer);
      }
    }
    platform_socket_t* sock = ATOMIC_EXCHANGE(&connection->sock, NULL);
    if (sock != NULL) {
      platform_socket_destroy(sock);
    }
    if (connection->request != NULL) {
      DESTROY(connection->request, http_request);
    }
    if (connection->write_buffer != NULL) {
      DESTROY(connection->write_buffer, buffer);
    }
    if (connection->header_field != NULL) {
      free(connection->header_field);
    }
    if (connection->header_value != NULL) {
      free(connection->header_value);
    }
    if (connection->ssl != NULL) {
      SSL_free(connection->ssl);
    }
    free(connection);
  }
}

void http_connection_write(http_connection_t* connection, const char* data, size_t length) {
  if (connection == NULL || ATOMIC_LOAD(&connection->sock) == NULL) {
    return;
  }
  buffer_t* buf = buffer_create_from_pointer_copy((uint8_t*)data, length);
  message_t msg;
  msg.type = HTTP_CONNECTION_WRITE;
  msg.payload = buf;
  msg.payload_destroy = (void (*)(void*))buffer_destroy;
  actor_send(&connection->actor, &msg);
}

void http_connection_close(http_connection_t* connection) {
  if (connection == NULL) {
    return;
  }
  message_t msg;
  msg.type = HTTP_CONNECTION_CLOSE;
  msg.payload = NULL;
  msg.payload_destroy = NULL;
  actor_send(&connection->actor, &msg);
}