//
// Created by victor on 9/30/26.
//
// Async HTTP/1.1 client on the poll-dancer reactor — the async twin of the
// retired src/Net/http sync client. One POST, one response, connection per
// request. The transport discipline is MOVED from the sync client, not
// reinvented, per the streams-port contract:
//
//   URL parsing — _http_parse_url and _http_host_header are _parse_url and
//     _host_header lifted from src/Net/http.c verbatim (leak-tested shape;
//     bracketed v6 literals; port validation).
//   Hardening constants — _HTTP_BODY_MAX / _HTTP_READ_MAX / the bounded
//     header block are the sync client's proven values.
//   Response decode — http-parser (pinned lib) now owns framing: Content-
//     Length and Transfer-Encoding: chunked (with every dialect the parser
//     tolerates natively) decode through on_body; the sync client's chunked
//     framing peel (_parse_chunked/_decode_chunked) is NOT carried. Two of
//     its rules survive because callers can hand back either framing: a
//     decoded chunked empty body is a NON-NULL zero-length string, while
//     Content-Length: 0 (and an empty close-delimited tail) is a NULL body.
//   Statuses are pass-through: anything <200 or >=300 completes normally.
//
// Threading per the contract: submit may run on any thread and marshals
// every fd/watcher step onto the loop thread via streams_loop_call. The
// client's own state splits cleanly:
//   - loop-serial (no lock): the in-flight and deferred request lists, every
//     request's transport state (fd, watcher, timer, buffers) — only the
//     loop thread touches them, in message order.
//   - client->dead under c->lock: the one cross-thread fact, written by
//     destroy, read at the completion claim. The claim is a snapshot under
//     the lock; the callback itself fires UNLOCKED (a callback that reenter
//     the client — submit/destroy — must not deadlock on its own guard). A
//     concurrent destroy either observes the completion's claim or cancels
//     the request before it fires — never both.
//
// pd-object lifetime follows the http_server.c destroy-stack idiom: a
// completion STOPS the watcher/timer (idempotent, safe mid-batch) but the
// pd destroy is deferred to a drain op between loop ticks, because another
// event in the same epoll batch may still carry the pd objects' pointers.
// Records outlive the batch; only the drain frees them.

#include "http_client.h"

#include "../Util/allocator.h"
#include "../Util/log.h"
#include "../Platform/platform_thread.h"
#include <poll-dancer/poll-dancer.h>
#include <http_parser.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <strings.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

/* ---------------- moved hardening constants (src/Net/http.c) ---------------- */

#define _HTTP_HOST_MAX 256
#define _HTTP_HEADER_BLOCK_MAX 4096
/* Absurd-body reject: a response body larger than this is treated as a
   transport failure and never assembled — enforced on BOTH framings: on the
   Content-Length promise (rejected up front, before the read loop can grow
   toward it) and on the running total inside the decode. Model completions
   are kilobytes; 64 MiB is orders past any real turn. */
#define _HTTP_BODY_MAX (64u * 1024u * 1024u)
/* Read-loop ceiling on the raw wire size (header block + framing + body):
   a legal response of a cap-sized body never needs more than the body cap
   plus the header-block bound plus chunk-framing slack, so a peer pushing
   past this is flooding (or lying about a length) — the buffer stops
   growing and the response is a transport failure. */
#define _HTTP_READ_MAX (_HTTP_BODY_MAX + _HTTP_HEADER_BLOCK_MAX + 4096)

/* Single-shot recv chunk: the response is parsed straight off the wire — no
   raw-response accumulator (http-parser delivers header/body splits). */
#define _HTTP_CHUNK_SIZE 8192

/* ---------------- types ---------------- */

typedef struct http_client_req_t http_client_req_t;

struct http_client_t {
  streams_loop_thread_t* lt;      /* borrowed; the client never owns the thread */
  platform_mutex_t* lock;         /* guards the dead flag around completion claims */
  int dead;                       /* set under lock by destroy; claims read it */
  void* destroy_joiner;           /* destroy's wait record; loop-serial use only */
  http_client_req_t* inflight;    /* requests with live watchers (loop-serial) */
  http_client_req_t* deferred;    /* stopped requests awaiting teardown (loop-serial) */
};

struct http_client_req_t {
  /* submit-side copies (the caller's memory may vanish after submit) */
  char* url;
  char* api_key;
  char* body;
  size_t body_len;
  /* parsed URL (moved parser) */
  char host[_HTTP_HOST_MAX];
  uint16_t port;
  char* path;
  char* dns_error;                /* heap reason when the resolve failed at submit */
  struct addrinfo* res;           /* resolve result; owns the address chain */
  /* the wire: request head + body as one contiguous buffer */
  char* wire;
  size_t wire_len;
  size_t wire_off;
  uint32_t timeout_ms;
  /* transport (loop-serial) */
  int fd;
  pd_watcher_t* watcher;
  pd_timer_t* timer;
  int connecting;
  int done;                       /* a completion was claimed (or cancelled) */
  /* response accumulation (loop-serial) */
  http_parser parser;
  size_t raw_total;               /* bytes received on the wire */
  size_t decoded_len;             /* body bytes accumulated */
  char* dec_buf;
  size_t dec_cap;
  int header_complete;
  int message_complete;
  int saw_chunked;
  int has_cl;
  int close_delimited;
  uint64_t cl;
  int cap_error;
  int status;
  /* wiring */
  http_client_t* client;
  http_client_completion_fn on_done;
  void* ctx;
  struct http_client_req_t* next;
};

/* destroy's wait record: the caller waits until the loop thread's destroy
   op ran — after that, no callback of this client can fire (the destroy op
   is the last op enqueued for the client, and the loop thread is serial). */
typedef struct client_destroy_joiner_t {
  platform_mutex_t* mutex;
  platform_condvar_t* cv;
  int done;
} client_destroy_joiner_t;

/* ---------------- moved helpers (src/Net/http.c) ---------------- */

/* Heap-allocated transport reason; truncated reason strings are safe — never
   let a va_args snprintf failure null out an error the caller frees blindly.
   (The variadic wrapper and the in-place va_list builder share this one
   truncation discipline.) */
static char* _error_vstring(const char* fmt, va_list args) {
  char* err = get_memory(_HTTP_HEADER_BLOCK_MAX);
  int written = vsnprintf(err, _HTTP_HEADER_BLOCK_MAX, fmt, args);
  if (written < 0 || written >= (int)_HTTP_HEADER_BLOCK_MAX) {
    err[_HTTP_HEADER_BLOCK_MAX - 1] = '\0';
  }
  return err;
}

static char* _error_string(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char* err = _error_vstring(fmt, args);
  va_end(args);
  return err;
}

/* Parse http://host[:port]/path — moved from src/Net/http.c (_parse_url).
   IPv6 literals must be bracketed: an unbracketed literal cannot be split
   into host:port, so it is rejected here (getaddrinfo would reject it
   anyway). *path_out is heap-owned by the caller. */
static int _http_parse_url(const char* url, char* host, size_t host_size,
                           char** path_out, uint16_t* port_out) {
  const char* prefix = "http://";
  const char* rest;
  const char* host_start;
  const char* host_end;
  const char* port_end;
  const char* path_start = NULL;
  int port;
  size_t host_len;

  if (url == NULL) return -1;
  if (strncmp(url, prefix, 7) != 0) return -1;
  rest = url + 7;
  host_start = rest;
  host_end = strchr(host_start, ':');
  port_end = strchr(host_start, '/');

  if (host_start[0] == '[') {
    /* Bracketed v6 literal: [host][:port][/path]. A literal contains ':', so
       the generic host:port split below cannot be used; anchor on ']' and
       strip the brackets before resolution. */
    const char* close;
    const char* after;
    if (host_start[1] == ']') return -1;  /* empty literal host */
    close = strchr(host_start, ']');
    if (close == NULL) return -1;
    host_len = (size_t)(close - host_start - 1);
    if (host_len >= host_size) return -1;
    memcpy(host, host_start + 1, host_len);
    host[host_len] = '\0';
    after = close + 1;
    if (*after == ':') {
      port = (int)strtol(after + 1, NULL, 10);
      path_start = strchr(after, '/');
    } else {
      port = 80;
      path_start = (*after == '/') ? after : NULL;
    }
  } else if (host_end && (!port_end || host_end < port_end)) {
    /* Has port */
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= host_size) return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    port = (int)strtol(host_end + 1, NULL, 10);
    path_start = strchr(host_end, '/');
  } else if (port_end) {
    /* No port, has path */
    host_len = (size_t)(port_end - host_start);
    if (host_len == 0 || host_len >= host_size) return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    port = 80;
    path_start = port_end;
  } else {
    /* No port, no path */
    host_len = strlen(host_start);
    if (host_len == 0 || host_len >= host_size) return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    port = 80;
    path_start = "/";
  }

  if (port <= 0 || port > 65535) return -1;
  if (!path_start || path_start[0] == '\0') path_start = "/";

  *path_out = get_memory(strlen(path_start) + 1);
  strcpy(*path_out, path_start);
  *port_out = (uint16_t)port;
  return 0;
}

/* Host header formatting, moved from src/Net/http.c (_host_header): an
   endpoint host keeps the port explicit, a v6 literal keeps its brackets. */
static int _http_host_header(const char* host, uint16_t port, char* out, size_t out_len) {
  int written;
  if (host == NULL || out == NULL || out_len == 0) return -1;
  if (strchr(host, ':') != NULL) {
    written = snprintf(out, out_len, "[%s]:%u", host, (unsigned)port);
  } else {
    written = snprintf(out, out_len, "%s:%u", host, (unsigned)port);
  }
  if (written < 0 || (size_t)written >= out_len) return -1;
  return 0;
}

/* Grow the response buffer (get_memory semantics: never a failure return,
   contents carried over). Moved from src/Net/http.c (_buf_grow). */
static char* _buf_grow(char* buf, size_t used, size_t need) {
  char* grown = get_memory(need);
  memcpy(grown, buf, used);
  free(buf);
  return grown;
}

/* One send step, moved from src/Net/http.c's send idiom: MSG_NOSIGNAL so a
   peer-closed socket never SIGPIPEs (Apple's send lacks the flag). Never
   retries — the watcher drives EAGAIN. */
static ssize_t _send_step(int fd, const char* buf, size_t len) {
#ifdef __APPLE__
  return send(fd, buf, len, 0);
#else
  return send(fd, buf, len, MSG_NOSIGNAL);
#endif
}

static int _set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------------- request lifecycle (loop thread) ---------------- */

static void _req_complete(http_client_req_t* req, int status, char* body,
                          size_t body_len, char* error);
static void _req_parse_error(http_client_req_t* req);
static void _op_drain(void* p);
static void _op_start(void* p);

/* Frees every heap record the request owns (never the pd objects: they die
   in the deferred teardown). Safe from the submit thread (reject paths) and
   from the loop thread (_op_start's cancel path). */
static void _req_free_request_side(http_client_req_t* req) {
  /* Each member is freed exactly once and nulled: finalize's transfers
     below, and the deferred teardown (drain op) free only survivors. */
  free(req->url);
  req->url = NULL;
  free(req->api_key);
  req->api_key = NULL;
  free(req->body);
  req->body = NULL;
  free(req->path);
  req->path = NULL;
  free(req->wire);
  req->wire = NULL;
  free(req->dec_buf);   /* untouched when the body transferred to the callback */
  req->dec_buf = NULL;
  free(req->dns_error);
  req->dns_error = NULL;
  if (req->res != NULL) {
    freeaddrinfo(req->res);
    req->res = NULL;
  }
}

static void _req_cancel_unstarted(http_client_req_t* req) {
  _req_free_request_side(req);
  free(req);
}

static void _client_inflight_remove(http_client_t* c, http_client_req_t* req) {
  http_client_req_t** p = &c->inflight;
  while (*p != NULL && *p != req) p = &(*p)->next;
  if (*p == req) *p = req->next;
}

/* Teardown of the live transport: STOP the pd objects (idempotent, safe
   mid-epoll-batch — destroys are deferred to the drain op), close the fd.
   The connection is closed inside the client before any completion fires. */
static void _req_transport_stop(http_client_req_t* req) {
  if (req->timer != NULL) {
    pd_timer_stop(req->timer);      /* disarm; destroy is the drain op's job */
  }
  if (req->watcher != NULL) {
    pd_watcher_stop(req->watcher);  /* detach from epoll; same deferral */
  }
  if (req->fd >= 0) {
    close(req->fd);
    req->fd = -1;
  }
}

/* THE single completion seam. Called on the loop thread only; at most one
   on_done ever runs per request. Tear down the transport and request-side
   memory BEFORE the callback (the completion contract). If destroy already
   marked the client dead, the completion is cancelled and its heap strings
   freed here — callbacks that have not fired will never fire. */
static void _req_complete(http_client_req_t* req, int status, char* body,
                          size_t body_len, char* error) {
  http_client_t* c = req->client;

  req->done = 1;
  _req_transport_stop(req);
  _client_inflight_remove(c, req);
  /* The record moves to the deferred list: it must outlive its own watcher/
     timer (a later event in this same epoll batch can still address them);
     the drain op between loop ticks does the actual pd destroys + free. */
  req->next = c->deferred;
  c->deferred = req;
  if (streams_loop_call(c->lt, _op_drain, c) != 0) {
    log_error("http_client: drain op undeliverable (loop gone)");
  }
  _req_free_request_side(req);

  platform_mutex_lock(c->lock);
  int dead = c->dead;
  platform_mutex_unlock(c->lock);
  /* Fire UNLOCKED: the callback is the caller's code and must not call back
     into this same client (submit/destroy would deadlock on the very guard
     it fired under). The snapshot still serializes the fire-vs-cancel — a
     destroy that wins the claim is either queued after this fire (loop
     thread is serial, the request record outlives the batch) or cancelled
     the request before it could ever get here. */
  if (dead) {
    free(body);
    free(error);
  } else {
    req->on_done(req->ctx, status, body, body_len, error);
  }
}

/* ---------------- response accumulation (loop thread) ---------------- */

/* Appends body bytes (http-parser's decoded chunk data, Content-Length body
   bytes, or a close-delimited raw tail). Returns 0, or -1 when the cap is
   hit — the parser callback halts and the error path completes the request
   (same failure class as the retired sync client's absurd-body rejects). */
static int _dec_append(http_client_req_t* req, const char* at, size_t length) {
  if (req->decoded_len + length > _HTTP_BODY_MAX) {
    req->cap_error = 1;
    return -1;
  }
  if (req->dec_buf == NULL) {
    req->dec_cap = (length + 1 < _HTTP_CHUNK_SIZE) ? _HTTP_CHUNK_SIZE : length + 1;
    req->dec_buf = get_memory(req->dec_cap);
  } else if (req->decoded_len + length + 1 > req->dec_cap) {
    while (req->dec_cap < req->decoded_len + length + 1) req->dec_cap *= 2;
    req->dec_buf = _buf_grow(req->dec_buf, req->decoded_len, req->dec_cap);
  }
  memcpy(req->dec_buf + req->decoded_len, at, length);
  req->decoded_len += length;
  req->dec_buf[req->decoded_len] = '\0';
  return 0;
}

/* http-parser callbacks (response type): capture framing facts at the
   header boundary, accumulate decoded body bytes, and mark message end.
   A nonzero return halts the parser — the caller completes the request
   through the parse-error path. */
static int _on_headers_complete(http_parser* parser) {
  http_client_req_t* req = (http_client_req_t*)parser->data;
  req->header_complete = 1;
  req->status = (int)parser->status_code;
  req->saw_chunked = (parser->flags & F_CHUNKED) ? 1 : 0;
  req->has_cl = (parser->flags & F_CONTENTLENGTH) ? 1 : 0;
  req->cl = req->has_cl ? parser->content_length : 0;
  /* NEITHER framing is a valid framing of its own: the third HTTP close-vs-
     truncate case — the body runs to connection close (HTTP/1.0 responses).
     The retired sync client read that as a SUCCESS; without this flag the
     EOF handler would misreport it as a short-body transport failure. */
  req->close_delimited = !(req->saw_chunked || req->has_cl);
  if (req->has_cl && parser->content_length > _HTTP_BODY_MAX) {
    req->cap_error = 1;   /* absurd length promise: reject before any read */
    return 1;
  }
  return 0;
}

static int _on_body(http_parser* parser, const char* at, size_t length) {
  http_client_req_t* req = (http_client_req_t*)parser->data;
  if (_dec_append(req, at, length) != 0) return 1;
  return 0;
}

static int _on_message_complete(http_parser* parser) {
  http_client_req_t* req = (http_client_req_t*)parser->data;
  req->message_complete = 1;
  return 0;
}

static http_parser_settings _parser_settings = {
  .on_message_begin = NULL,
  .on_url = NULL,
  .on_status = NULL,
  .on_header_field = NULL,
  .on_header_value = NULL,
  .on_headers_complete = _on_headers_complete,
  .on_body = _on_body,
  .on_message_complete = _on_message_complete,
  .on_chunk_header = NULL,
  .on_chunk_complete = NULL
};

/* Success shape (the retired client's rules): the request status passes
   through untouched (<200 or >=300 is the caller's decision); the decoded
   body is only non-NULL when there is one, EXCEPT a chunked body, which is
   never NULL — an empty chunked body is a non-NULL zero-length string,
   while Content-Length: 0 (or an empty close-delimited tail) stays NULL. */
static void _req_finalize(http_client_req_t* req) {
  char* body = NULL;

  /* An absurd framing claim marked at headers_complete (a Content-Length
     over the body cap halts the parser without an error) must still be a
     transport failure when the parser reaches message end — the sync
     client's up-front reject survives: NO part of a body-caplying response
     ever completes as success, whether or not any body bytes followed. */
  if (req->cap_error) {
    _req_parse_error(req);
    return;
  }

  if (req->saw_chunked) {
    if (req->dec_buf == NULL) {
      req->dec_buf = get_memory(1);
      req->dec_buf[0] = '\0';
      req->dec_cap = 1;
    }
    body = req->dec_buf;
    req->dec_buf = NULL;
  } else if ((req->has_cl || req->close_delimited) && req->decoded_len > 0) {
    body = req->dec_buf;
    req->dec_buf = NULL;
  }
  /* The client owns the request-side memory until here; body ownership
     transfers to the callback below. */
  _req_complete(req, req->status, body, req->decoded_len, NULL);
}

static void _req_complete_transport_error(http_client_req_t* req, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char* error = _error_vstring(fmt, args);
  va_end(args);
  _req_complete(req, -1, NULL, 0, error);
}

/* Every way the wire can cut short of a valid message end — the same
   transport-failure class the sync client reported, with the same
   close-vs-truncate distinctions. */
static void _req_eof(http_client_req_t* req) {
  if (!req->header_complete) {
    _req_complete_transport_error(req,
      "http client: no complete header block from %s:%u (connection closed)",
      req->host, (unsigned)req->port);
  } else if (req->close_delimited) {
    /* No framing declared: the body ran to connection close. Feed the
       parser's EOF so on_message_complete marks the true message end (the
       raw tail was accumulated outside the parser), then finalize — the
       retired client read a close-delimited response as a success. */
    (void)http_parser_execute(&req->parser, &_parser_settings, NULL, 0);
    _req_finalize(req);
  } else if (req->saw_chunked) {
    _req_complete_transport_error(req,
      "http client: malformed chunked body from %s:%u (connection closed)",
      req->host, (unsigned)req->port);
  } else {
    _req_complete_transport_error(req,
      "http client: short body from %s:%u (expected %llu, got %zu, "
      "connection closed)",
      req->host, (unsigned)req->port,
      (unsigned long long)req->cl, req->decoded_len);
  }
}

static void _req_parse_error(http_client_req_t* req) {
  if (req->cap_error) {
    _req_complete_transport_error(req,
      "http client: response from %s:%u exceeds the 64 MiB body cap",
      req->host, (unsigned)req->port);
    return;
  }
  _req_complete_transport_error(req,
    "http client: parse error from %s:%u (%s)",
    req->host, (unsigned)req->port,
    http_errno_name((enum http_errno)req->parser.http_errno));
}

/* ---------------- pd callbacks (loop thread) ---------------- */

/* The per-request timeout: one one-shot loop timer per request, created at
   start. Firing is always a transport failure; a response that beat the
   clock leaves done=1 and this becomes a no-op (never two completions). */
static void _req_timeout(pd_loop_t* loop, pd_watcher_t* watcher,
                         pd_event_t events, void* tuser_data) {
  http_client_req_t* req = (http_client_req_t*)tuser_data;
  (void)loop;
  (void)watcher;
  (void)events;
  if (req->done) return;
  _req_complete_transport_error(req,
    "http client: timeout from %s:%u", req->host, (unsigned)req->port);
}

static void _req_watcher(pd_loop_t* loop, pd_watcher_t* watcher,
                         pd_event_t events, void* tuser_data) {
  http_client_req_t* req = (http_client_req_t*)tuser_data;
  (void)loop;
  (void)watcher;
  if (req->done) return;

  /* Non-blocking connect settle: a refused connection arrives as ERROR/
     HANGUP (or as WRITE with a pending SO_ERROR). */
  if (req->connecting) {
    if (events & (PD_EVENT_WRITE | PD_EVENT_ERROR | PD_EVENT_HANGUP)) {
      int so_err = 0;
      socklen_t so_len = sizeof(so_err);
      if (getsockopt(req->fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) != 0) {
        so_err = errno;
      }
      if (so_err != 0) {
        _req_complete_transport_error(req,
          "http client: connect to %s:%u failed (%s)",
          req->host, (unsigned)req->port, strerror(so_err));
        return;
      }
      req->connecting = 0;
    } else {
      return;
    }
  }

  /* Send the request until it is fully out; EAGAIN parks on the WRITE
     watcher (drain semantics carried from the sync client's send-all). */
  while (req->wire_off < req->wire_len) {
    ssize_t n = _send_step(req->fd, req->wire + req->wire_off,
                           req->wire_len - req->wire_off);
    if (n > 0) {
      req->wire_off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    _req_complete_transport_error(req,
      "http client: send to %s:%u failed (%s)",
      req->host, (unsigned)req->port, strerror(errno));
    return;
  }

  /* Fully sent: switch the watcher to reads for the response. */
  if (pd_watcher_update(watcher, PD_EVENT_READ) != PD_OK) {
    _req_complete_transport_error(req,
      "http client: watcher for %s:%u failed", req->host, (unsigned)req->port);
    return;
  }
  if (!(events & PD_EVENT_READ)) return;   /* data arrives in a later batch */

  /* Response read loop: parse straight off the wire. The parse stops being
     fed once a framing-less response is past its headers — a body delimited
     only by connection close cannot terminate inside http-parser, so the
     raw tail is accumulated by hand and finalized at EOF (the retired
     client's read-to-close rule). */
  while (!req->done) {
    char chunk[_HTTP_CHUNK_SIZE];
    ssize_t n = recv(req->fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      _req_complete_transport_error(req,
        "http client: recv from %s:%u failed (%s)",
        req->host, (unsigned)req->port, strerror(errno));
      return;
    }
    if (n == 0) {
      _req_eof(req);
      return;
    }

    req->raw_total += (size_t)n;
    if (req->raw_total > _HTTP_READ_MAX) {
      _req_complete_transport_error(req,
        "http client: response from %s:%u exceeds the wire read cap "
        "(header block + framing + body)",
        req->host, (unsigned)req->port);
      return;
    }

    if (!req->header_complete || !req->close_delimited) {
      size_t nparsed = http_parser_execute(&req->parser, &_parser_settings,
                                           chunk, (size_t)n);
      if (nparsed != (size_t)n) {
        _req_parse_error(req);
        return;
      }
    } else {
      if (_dec_append(req, chunk, (size_t)n) != 0) {
        _req_parse_error(req);   /* cap_error set by _dec_append */
        return;
      }
    }
    if (req->message_complete) {
      _req_finalize(req);
      return;
    }
  }
}

/* ---------------- loop-thread ops ---------------- */

/* Deferred teardown of completed/cancelled records: now that no epoll batch
   is processing, the pd objects can be destroyed and the records freed. */
static void _op_drain(void* p) {
  http_client_t* c = (http_client_t*)p;
  http_client_req_t* req = c->deferred;
  c->deferred = NULL;
  while (req != NULL) {
    http_client_req_t* next = req->next;
    if (req->watcher != NULL) {
      pd_watcher_stop(req->watcher);   /* idempotent after completion's stop */
      pd_watcher_destroy(req->watcher);
    }
    if (req->timer != NULL) {
      pd_timer_stop(req->timer);
      pd_timer_destroy(req->timer);
    }
    free(req->dec_buf);
    free(req);
    req = next;
  }
}

static void _op_start(void* p) {
  http_client_req_t* req = (http_client_req_t*)p;
  http_client_t* c = req->client;

  /* dead is the one cross-thread fact: read it under the client lock. */
  platform_mutex_lock(c->lock);
  int dead = c->dead;
  platform_mutex_unlock(c->lock);
  if (dead || req->done) {
    /* Cancelled before the transport ever started: free quietly, no fire. */
    _req_cancel_unstarted(req);
    return;
  }
  if (req->res == NULL) {
    /* Resolve failed at submit (a transport failure the caller must see
       with a reason, like the retired client's DNS error response). */
    char* dns_error = req->dns_error;
    req->dns_error = NULL;   /* ownership moves to the completion */
    _req_complete(req, -1, NULL, 0, dns_error);
    return;
  }

  /* Connect on the first address that launches (EINPROGRESS is a launch).
     The resolve chain lives in req->res (freed with the request). */
  struct addrinfo* it;
  int fd = -1;
  int connect_errno = 0;
  for (it = req->res; it != NULL; it = it->ai_next) {
    fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      connect_errno = errno;
      continue;
    }
    if (_set_nonblocking(fd) != 0) {
      connect_errno = errno;
      close(fd);
      fd = -1;
      continue;
    }
    if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
    if (errno != EINPROGRESS) {
      connect_errno = errno;
      close(fd);
      fd = -1;
      continue;
    }
    break;
  }
  if (fd < 0) {
    _req_complete_transport_error(req,
      "http client: connect to %s:%u failed (%s)",
      req->host, (unsigned)req->port, strerror(connect_errno));
    return;
  }
  req->fd = fd;
  req->connecting = 1;

  req->watcher = pd_watcher_create(streams_loop_raw(c->lt), fd, PD_EVENT_WRITE,
                                   _req_watcher, req);
  if (req->watcher == NULL || pd_watcher_start(req->watcher) != PD_OK) {
    _req_complete_transport_error(req,
      "http client: watcher for %s:%u failed", req->host, (unsigned)req->port);
    return;
  }

  req->timer = pd_timer_create(streams_loop_raw(c->lt), req->timeout_ms, 0,
                               _req_timeout, req);
  if (req->timer == NULL || pd_timer_start(req->timer) != PD_OK) {
    _req_complete_transport_error(req,
      "http client: timer for %s:%u failed", req->host, (unsigned)req->port);
    return;
  }

  /* The request is on the wire's way: track it for cancel-at-destroy. */
  req->next = c->inflight;
  c->inflight = req;
}

/* Destroy op: runs on the loop thread AFTER every other op this client ever
   enqueued (FIFO) and after every watcher/timer callback of this client —
   the loop thread is serial. Cancels anything still in flight, drains the
   deferred teardowns, and only then releases the waiting destroy caller.
   The client's lock and record are NOT touched here: a submit thread may
   still be inside (or blocked on) that mutex right now — http_client_destroy
   tears them down only after this op has run and the joiner confirmed it. */
static void _op_client_destroy(void* p) {
  http_client_t* c = (http_client_t*)p;
  client_destroy_joiner_t* joiner = (client_destroy_joiner_t*)c->destroy_joiner;

  http_client_req_t* req = c->inflight;
  c->inflight = NULL;
  while (req != NULL) {
    http_client_req_t* next = req->next;
    req->done = 1;             /* cancelled: no completion will ever fire */
    _req_transport_stop(req);
    _req_free_request_side(req);
    req->next = c->deferred;
    c->deferred = req;
    req = next;
  }
  _op_drain(c);

  platform_mutex_lock(joiner->mutex);
  joiner->done = 1;
  platform_condvar_signal(joiner->cv);
  platform_mutex_unlock(joiner->mutex);
}

/* ---------------- public API ---------------- */

http_client_t* http_client_create(streams_loop_thread_t* lt) {
  if (lt == NULL) return NULL;
  http_client_t* c = get_clear_memory(sizeof(http_client_t));
  c->lt = lt;
  c->lock = platform_mutex_create();
  if (c->lock == NULL) {
    log_error("http_client_create: client lock creation failed");
    free(c);
    return NULL;
  }
  return c;
}

int http_client_submit(http_client_t* c, const char* url, const char* api_key,
                       const char* body_json, uint32_t timeout_ms,
                       http_client_completion_fn on_done, void* ctx) {
  if (c == NULL || on_done == NULL) return -1;
  if (url == NULL || body_json == NULL) return -1;   /* NULL api_key allowed */

  http_client_req_t* req = get_clear_memory(sizeof(http_client_req_t));
  req->fd = -1;
  req->client = c;
  req->timeout_ms = timeout_ms;
  req->on_done = on_done;
  req->ctx = ctx;
  http_parser_init(&req->parser, HTTP_RESPONSE);
  req->parser.data = req;

  if (_http_parse_url(url, req->host, sizeof(req->host), &req->path,
                      &req->port) != 0) {
    log_error("http_client_submit: unparsable url '%s'", url);
    _req_cancel_unstarted(req);
    return -1;
  }

  /* Copy the caller's arguments: caller memory may vanish after return. */
  req->body_len = strlen(body_json);
  req->body = get_memory(req->body_len + 1);
  memcpy(req->body, body_json, req->body_len + 1);
  req->url = get_memory(strlen(url) + 1);
  strcpy(req->url, url);
  if (api_key != NULL) {
    req->api_key = get_memory(strlen(api_key) + 1);
    strcpy(req->api_key, api_key);
  }

  /* Build the request head under the bounded-block discipline the retired
     client proved: request line, Host, Content-Type, Content-Length,
     Connection: close, then Authorization when a key is given — each piece
     bounds-checked so a long path or key is a REJECTION (before any I/O),
     never a silently truncated request. */
  char host_header[300];
  char header_block[_HTTP_HEADER_BLOCK_MAX];
  size_t header_used;
  if (_http_host_header(req->host, req->port, host_header,
                        sizeof(host_header)) != 0) {
    log_error("http_client_submit: host header for %s:%u overflows the header "
              "buffer", req->host, (unsigned)req->port);
    _req_cancel_unstarted(req);
    return -1;
  }
  header_used = (size_t)snprintf(header_block, sizeof(header_block),
                                 "POST %s HTTP/1.1\r\n"
                                 "Host: %s\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n",
                                 req->path, host_header, req->body_len);
  if (header_used == 0 || header_used >= sizeof(header_block)) {
    log_error("http_client_submit: request overflows the header buffer");
    _req_cancel_unstarted(req);
    return -1;
  }
  if (api_key != NULL) {
    size_t key_len = strlen(api_key);
    size_t auth_len = strlen("Authorization: Bearer \r\n") - 1 + key_len;
    if (header_used + auth_len + 3 > sizeof(header_block)) {
      log_error("http_client_submit: Authorization header overflows the "
                "header buffer");
      _req_cancel_unstarted(req);
      return -1;
    }
    memcpy(header_block + header_used, "Authorization: Bearer ", 22);
    memcpy(header_block + header_used + 22, api_key, key_len);
    header_used += 22 + key_len;
    header_block[header_used] = '\r';
    header_block[header_used + 1] = '\n';
    header_used += 2;
  }
  header_block[header_used] = '\r';
  header_block[header_used + 1] = '\n';
  header_block[header_used + 2] = '\0';

  /* One contiguous wire buffer (head + body): the send state machine hands
     it to the watcher without depending on any header-sized bound. */
  header_used = strlen(header_block);
  req->wire_len = header_used + req->body_len;
  req->wire = get_memory(req->wire_len);
  memcpy(req->wire, header_block, header_used);
  memcpy(req->wire + header_used, req->body, req->body_len);

  /* Resolve on the SUBMIT thread: blocking DNS must never touch the
     reactor. Numeric literals resolve inside getaddrinfo itself. */
  {
    char port_str[8];
    struct addrinfo hints;
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)req->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(req->host, port_str, &hints, &req->res) != 0 ||
        req->res == NULL) {
      req->res = NULL;
      req->dns_error = _error_string("http client: resolve %s:%u failed (DNS)",
                                     req->host, (unsigned)req->port);
      log_error("http_client_submit: resolve %s:%u failed (DNS)",
                req->host, (unsigned)req->port);
    }
  }

  /* Marshal the transport start onto the loop thread — under the client
     lock, so a concurrent destroy either queues its destroy op AFTER this
     op (the start runs and quietly cancels) or has already rejected. */
  platform_mutex_lock(c->lock);
  if (c->dead) {
    platform_mutex_unlock(c->lock);
    log_error("http_client_submit: client already destroyed; submit rejected");
    _req_cancel_unstarted(req);
    return -1;
  }
  int rc = streams_loop_call(c->lt, _op_start, req);
  platform_mutex_unlock(c->lock);
  if (rc != 0) {
    log_error("http_client_submit: loop thread gone; submit rejected");
    _req_cancel_unstarted(req);
    return -1;
  }
  return 0;
}

void http_client_destroy(http_client_t* c) {
  if (c == NULL) return;

  client_destroy_joiner_t joiner;
  joiner.mutex = platform_mutex_create();
  joiner.cv = platform_condvar_create();
  joiner.done = 0;
  if (joiner.mutex == NULL || joiner.cv == NULL) {
    log_error("http_client_destroy: joiner primitives failed");
    abort();
  }

  platform_mutex_lock(c->lock);
  if (c->dead) {
    /* Already destroyed (or a concurrent destroy is running): no-op. */
    platform_mutex_unlock(c->lock);
    platform_mutex_destroy(joiner.mutex);
    platform_condvar_destroy(joiner.cv);
    return;
  }
  c->dead = 1;
  c->destroy_joiner = &joiner;
  int rc = streams_loop_call(c->lt, _op_client_destroy, c);
  platform_mutex_unlock(c->lock);

  if (rc != 0) {
    /* The loop thread is gone: nothing on it can race or fire. Teardown
       locally — pd objects that were only ever made on the loop thread
       died with the loop side of the process. */
    log_error("http_client_destroy: loop thread gone; tearing down locally");
    http_client_req_t* req = c->inflight;
    while (req != NULL) {
      http_client_req_t* next = req->next;
      if (req->fd >= 0) {
        close(req->fd);   /* the loop died without stopping this transport */
        req->fd = -1;
      }
      _req_free_request_side(req);
      free(req);
      req = next;
    }
    c->inflight = NULL;
    req = c->deferred;
    while (req != NULL) {
      http_client_req_t* next = req->next;
      free(req->dec_buf);
      free(req);
      req = next;
    }
    c->deferred = NULL;
    platform_mutex_destroy(c->lock);
    free(c);
  } else {
    /* Wait until the destroy op ran: it is the last work this client ever
       does on the loop thread, so the instant it returns, no callback of
       this client can fire. A generous cap with a fail-loud abort (a stuck
       reactor must not limp — the caller's ctx is already owned). */
    platform_mutex_lock(joiner.mutex);
    int waits = 0;
    while (!joiner.done) {
      platform_condvar_timed_wait(joiner.cv, joiner.mutex, 100);
      if (++waits > 100) {   /* 10s: the loop thread is wedged */
        log_error("http_client_destroy: loop thread never quiesced the client");
        abort();
      }
    }
    platform_mutex_unlock(joiner.mutex);
    /* The destroy op has run: no loop-thread work touches c anymore, and any
       submit that claimed the lock is past it — the lock and the record can
       die here, never while a submit thread still holds or waits on them. */
    platform_mutex_destroy(c->lock);
    free(c);
  }
  platform_mutex_destroy(joiner.mutex);
  platform_condvar_destroy(joiner.cv);
}
