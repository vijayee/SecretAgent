//
// Created by victor on 9/29/26.
//
// Minimal HTTP/1.1 POST client for local model endpoints.
//
// Borrowed from liboffs (Victor's own code, extracted and trimmed to the
// MINIMAL contract — http:// only, Content-Length both ways, no chunked
// encoding, connection per request):
//
//   URL parsing (http://host[:port]/path, bracketed v6 literals):
//     liboffs/src/ClientLibs/c/offs_client.c   offs_http_get() URL section
//   Host header formatting (v6 literals keep their brackets):
//     liboffs/src/Network/endpoint.c           endpoint_host_header()
//   Request build/send, read-until-headers, case-insensitive Content-Length
//   scan (RFC 7230), and the SO_RCVTIMEO/SO_SNDTIMEO idiom:
//     liboffs/src/ClientLibs/c/offs_client.c   offs_http_get()
//   MSG_NOSIGNAL on send (no SIGPIPE on a peer-closed socket):
//     liboffs/src/Platform/platform_socket.c   platform_socket_send()
//
// Trimmed rather than carried:
//   - liboffs's platform_socket abstraction (poll-dancer watcher plumbing,
//     dual-stack listeners, Windows named-pipe sockets) is more than a
//     one-POST client needs; transport goes through plain POSIX sockets.
//     That also drops the string-address -> platform_address_parse
//     round-trip: we connect straight on the getaddrinfo result.
//   - Windows headers/branches dropped (SecretAgent builds POSIX-only today).
//   - offs_http_get's read-till-close is generalized: the response is
//     searched for the header end, the status line and Content-Length are
//     parsed from it, and the body is read to Content-Length (or, when the
//     server omits Content-Length with Connection: close, to close).
//
// Added beyond the liboffs contract (local proxies answer even non-streaming
// requests with Transfer-Encoding: chunked):
//   - chunked body decode after the read: chunk framing
//     (RFC 7230 §4.1) is peeled off into a contiguous body — chunk
//     extensions and trailer fields are discarded; malformed framing is a
//     transport failure exactly like a short Content-Length body.

#include "http.h"

#include "../Util/allocator.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>

#define _HTTP_HOST_MAX 256
#define _HTTP_HEADER_BLOCK_MAX 4096
/* Absurd-body reject: a decoded (or raw-promised) response body larger than
   this is treated as a transport failure, never assembled. Model completions
   are kilobytes; 64 MiB is orders past any real turn. */
#define _HTTP_BODY_MAX (64u * 1024u * 1024u)

/* ---------------------------------------------------------------- */
/* Response plumbing                                                */
/* ---------------------------------------------------------------- */

/* A response that did not make it to a valid HTTP round trip: status -1,
   no body, and a heap-allocated transport-level reason. */
static http_response_t* _http_error_response(const char* fmt, ...) {
  http_response_t* r = get_clear_memory(sizeof(http_response_t));
  va_list args;
  int written;

  r->status = -1;
  r->body = NULL;
  r->body_len = 0;
  r->error = get_memory(_HTTP_HEADER_BLOCK_MAX);
  va_start(args, fmt);
  written = vsnprintf(r->error, _HTTP_HEADER_BLOCK_MAX, fmt, args);
  va_end(args);
  if (written < 0 || written >= (int)_HTTP_HEADER_BLOCK_MAX) {
    /* Truncated reason strings are safe; never let a va_args snprintf
       failure null out the error the caller will free blindly. */
    r->error[_HTTP_HEADER_BLOCK_MAX - 1] = '\0';
  }
  return r;
}

void http_response_destroy(http_response_t* r) {
  if (r == NULL) return;
  free(r->body);
  free(r->error);
  free(r);
}

/* ---------------------------------------------------------------- */
/* URL parsing (extracted from offs_http_get)                        */
/* ---------------------------------------------------------------- */

/* Parse http://host[:port]/path — the offs_http_get URL section adapted to
   return a failure code instead of NULL. IPv6 literals must be bracketed:
   an unbracketed literal cannot be split into host:port, so it is rejected
   here (the getaddrinfo call below would reject it anyway). */
static int _parse_url(const char* url, char* host, size_t host_size,
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

/* Host header formatting, extracted from endpoint_host_header — an endpoint
   host keeps the port explicit, and a v6 literal keeps its brackets. */
static int _host_header(const char* host, uint16_t port, char* out, size_t out_len) {
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

/* ---------------------------------------------------------------- */
/* Transport (POSIX sockets; send/recv idiom borrowed from liboffs)  */
/* ---------------------------------------------------------------- */

static int _send_all(int fd, const char* buf, size_t len,
                     const struct timeval* timeout) {
  size_t sent = 0;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, timeout, sizeof(*timeout));
  while (sent < len) {
    ssize_t n;
#ifdef __APPLE__
    n = send(fd, buf + sent, len - sent, 0);
#else
    n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
#endif
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return -1;
    }
    sent += (size_t)n;
  }
  return 0;
}

/* Header block ends at CRLF CRLF. Returns the offset one past the block
   end, or 0 while the block is still incomplete. */
static size_t _find_header_end(const char* buf, size_t len) {
  size_t i;
  for (i = 0; i + 3 < len; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
      return i + 4;
    }
  }
  return 0;
}

/* Case-insensitive Content-Length parse over the header block only
   (the scan itself is borrowed from offs_http_get, which scans the whole
   buffer — the block is bounded here so a body containing the literal can
   never hijack the header scan). */
static ssize_t _parse_content_length(const char* headers, size_t len) {
  size_t i;
  for (i = 0; i + 15 < len; i++) {
    const char* p;
    if (strncasecmp(headers + i, "Content-Length:", 15) != 0) continue;
    p = headers + i + 15;
    while (*p == ' ') p++;
    return (ssize_t)strtoul(p, NULL, 10);
  }
  return -1;
}

/* Case-insensitive scan of the header block for a Transfer-Encoding whose
   value declares the chunked transfer codings (RFC 7230 §3.3.1): the match
   tolerates parameters after ';' on the token, surrounding encodings
   separated by ',', and OWS around those commas (RFC 7230 §3.3.1). When
   chunked is declared, Content-Length must be ignored — chunked wins by
   RFC (and by what proxies actually send). */
static int _parse_chunked(const char* headers, size_t len) {
  size_t i;
  for (i = 0; i + 18 < len; i++) {
    const char* value;
    const char* end = headers + len;
    if (strncasecmp(headers + i, "Transfer-Encoding:", 18) != 0) continue;
    value = headers + i + 18;
    while (value < end && (*value == ' ' || *value == '\t')) value++;
    while (value < end) {
      size_t tok_len;
      const char* tok = value;
      while (value < end && *value != ',' && *value != '\r' &&
             *value != ' ' && *value != '\t') value++;
      tok_len = (size_t)(value - tok);
      if (tok_len >= 7 && strncasecmp(tok, "chunked", 7) == 0 &&
          (tok_len == 7 || tok[7] == ';')) {
        return 1;
      }
      while (value < end && (*value == ',' || *value == ' ' ||
                             *value == '\t')) value++;
      if (value >= end || *value == '\r') return 0;
      if (tok_len == 0) return 0;
    }
    return 0;
  }
  return 0;
}

/* First CRLF inside [p, search_end), or NULL. All chunked reads are bounded
   by search_end == end-of-received-data, never past. */
static const char* _find_crlf(const char* p, const char* search_end) {
  while (p + 1 < search_end) {
    if (p[0] == '\r' && p[1] == '\n') return p;
    p++;
  }
  return NULL;
}

/* Grow the response buffer (get_memory semantics: never a failure return,
   contents carried over). */
static char* _buf_grow(char* buf, size_t used, size_t need) {
  char* grown = get_memory(need);
  memcpy(grown, buf, used);
  free(buf);
  return grown;
}

/* Decode chunked framing (RFC 7230 §4.1) into a contiguous NUL-terminated
   buffer. Chunk extensions on a size line and the trailer section after the
   zero-size chunk are parsed past, contents discarded. Returns NULL on any
   malformed frame or overflow of _HTTP_BODY_MAX — same failure class as a
   short Content-Length body; on success *decoded_len carries the body
   length (the NUL is not counted). */
static char* _decode_chunked(const char* src, size_t src_len, size_t* decoded_len) {
  const char* p = src;
  const char* end = src + src_len;
  char* decoded = NULL;
  size_t decoded_used = 0;
  size_t decoded_cap = 0;

  while (1) {
    const char* crlf = _find_crlf(p, end);
    const char* q;
    size_t digits;
    char size_text[17];
    unsigned long long chunk_size;

    if (crlf == NULL) goto malformed;              /* unfinished size line */
    digits = 0;
    q = p;
    while (q < crlf && *q != ';') {                /* hex digits, then ext */
      if (!isxdigit((unsigned char)*q)) goto malformed;
      if (digits >= sizeof(size_text) - 1) goto malformed;  /* not a size */
      size_text[digits++] = *q;
      q++;
    }
    if (digits == 0) goto malformed;               /* empty size line */
    size_text[digits] = '\0';
    chunk_size = strtoull(size_text, NULL, 16);
    p = crlf + 2;

    if (chunk_size == 0) break;                    /* terminator reached */

    if ((size_t)(end - p) < chunk_size) goto malformed;  /* data truncated */
    if (chunk_size > _HTTP_BODY_MAX ||
        decoded_used + (size_t)chunk_size > _HTTP_BODY_MAX) {
      goto malformed;                              /* absurd total body */
    }
    if (decoded == NULL) {
      decoded_cap = (size_t)chunk_size + 1 < 8192 ? 8192 : (size_t)chunk_size + 1;
      decoded = get_memory(decoded_cap);
    } else if (decoded_used + (size_t)chunk_size + 1 > decoded_cap) {
      while (decoded_cap < decoded_used + (size_t)chunk_size + 1) decoded_cap *= 2;
      decoded = _buf_grow(decoded, decoded_used, decoded_cap);
    }
    memcpy(decoded + decoded_used, p, (size_t)chunk_size);
    decoded_used += (size_t)chunk_size;
    p += (size_t)chunk_size;
    if (end - p < 2 || p[0] != '\r' || p[1] != '\n') goto malformed;
    p += 2;
  }

  /* Trailer section: header lines until the empty line; content ignored. */
  while (1) {
    const char* crlf = _find_crlf(p, end);
    if (crlf == NULL) goto malformed;              /* unfinished trailer */
    if (crlf == p) break;                          /* empty line: done */
    p = crlf + 2;
  }

  decoded[decoded_used] = '\0';
  *decoded_len = decoded_used;
  return decoded;

malformed:
  free(decoded);
  return NULL;
}

http_response_t* http_post_json(const char* url,
                                const char* api_key,
                                const char* body_json,
                                uint32_t timeout_ms) {
  char host[_HTTP_HOST_MAX];
  uint16_t port = 0;
  char* path = NULL;
  http_response_t* r = NULL;
  struct addrinfo hints;
  struct addrinfo* res = NULL;
  struct addrinfo* it;
  int fd = -1;
  int connect_errno = 0;
  char host_header[300];
  char header_block[_HTTP_HEADER_BLOCK_MAX];
  size_t header_used;
  size_t body_len;
  struct timeval tv;
  char* resp_buf = NULL;
  size_t cap;
  size_t total = 0;
  size_t header_end = 0;
  ssize_t content_length = -1;
  int chunked = 0;
  int eof = 0;
  int status = 0;

  if (body_json == NULL) {
    return _http_error_response("http_post_json: body_json is NULL");
  }
  body_len = strlen(body_json);

  if (_parse_url(url, host, sizeof(host), &path, &port) != 0) {
    return _http_error_response("http_post_json: unparsable url '%s'",
                                url ? url : "(null)");
  }

  /* Resolve hostname. AI_ADDRCONFIG avoids picking a v6 address on a
     v4-only host (borrowed from offs_http_get). Numeric literals skip DNS
     inside getaddrinfo itself, so no literal special case is needed. */
  {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
      free(path);
      return _http_error_response("http_post_json: resolve %s:%u failed (DNS)",
                                  host, (unsigned)port);
    }
  }

  /* Connect on the first answering address. */
  for (it = res; it != NULL; it = it->ai_next) {
    fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      connect_errno = errno;
      continue;
    }
    if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
    connect_errno = errno;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    free(path);
    return _http_error_response("http_post_json: connect to %s:%u failed (%s)",
                                host, (unsigned)port, strerror(connect_errno));
  }

  /* Set send/recv timeouts on the underlying fd. The Windows branch of this
     idiom (DWORD ms count) is dropped — POSIX uses struct timeval. */
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  /* Build the request head: request line, Host, Content-Type,
     Content-Length, Connection: close, then Authorization when a key is
     given. The body follows in its own send so giant payloads never depend
     on the header buffer. */
  /* every failure past _parse_url owns fd + path */
  if (_host_header(host, port, host_header, sizeof(host_header)) != 0) {
    free(path);
    close(fd);
    return _http_error_response("http_post_json: host header for %s:%u overflows "
                                "the header buffer", host, (unsigned)port);
  }
  header_used = (size_t)snprintf(header_block, sizeof(header_block),
                                 "POST %s HTTP/1.1\r\n"
                                 "Host: %s\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n",
                                 path, host_header, body_len);
  if (header_used == 0 || header_used >= sizeof(header_block)) {
    free(path);
    close(fd);
    return _http_error_response("http_post_json: request overflows the header buffer");
  }
  if (api_key != NULL) {
    /* The Authorization header is appended in explicit bounds — a long key
       inside one %s would silently truncate the request line block. */
    size_t key_len = strlen(api_key);
    size_t auth_len = strlen("Authorization: Bearer \r\n") - 1 + key_len;
    if (header_used + auth_len + 3 > sizeof(header_block)) {
      free(path);
      close(fd);
      return _http_error_response("http_post_json: Authorization header overflows "
                                  "the header buffer");
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

  if (_send_all(fd, header_block, strlen(header_block), &tv) != 0 ||
      _send_all(fd, body_json, body_len, &tv) != 0) {
    close(fd);
    free(path);
    return _http_error_response("http_post_json: send to %s:%u failed (%s)",
                                host, (unsigned)port, strerror(errno));
  }
  /* The path outlived the connect and the request build; the response read
     no longer needs it. */
  free(path);

  /* Read the response: headers until CRLF CRLF, then the body either to
     Content-Length or to connection close. */
  cap = 8192;
  resp_buf = get_memory(cap);
  while (1) {
    ssize_t n;
    if (total + 1 >= cap) {
      resp_buf = _buf_grow(resp_buf, total, cap * 2);
      cap *= 2;
    }
    n = recv(fd, resp_buf + total, cap - total - 1, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      eof = (n == 0) || (errno == EAGAIN) || (errno == EWOULDBLOCK);
      break;
    }
    total += (size_t)n;
    resp_buf[total] = '\0';
    if (header_end == 0) {
      header_end = _find_header_end(resp_buf, total);
      if (header_end != 0) {
        content_length = _parse_content_length(resp_buf, header_end);
        chunked = _parse_chunked(resp_buf, header_end);
        if (!chunked && content_length >= 0 &&
            total >= header_end + (size_t)content_length) {
          break;
        }
      }
    } else if (!chunked && content_length >= 0 &&
               total >= header_end + (size_t)content_length) {
      break;
    }
  }
  close(fd);

  if (header_end == 0) {
    free(resp_buf);
    return _http_error_response("http_post_json: no complete header block from "
                                "%s:%u (%s)", host, (unsigned)port,
                                eof ? "connection closed" : "timed out");
  }

  /* A promised Content-Length the server never delivered is a short read —
     a transport failure, not a truncated success. (A chunked response has no
     promised length to check; framing errors surface in the decode below.) */
  if (!chunked && content_length >= 0 && total < header_end + (size_t)content_length) {
    free(resp_buf);
    return _http_error_response("http_post_json: short body from %s:%u "
                                "(expected %zd, got %zu, connection %s)",
                                host, (unsigned)port, content_length,
                                total - header_end,
                                eof ? "closed" : "timed out");
  }

  /* Status line: "HTTP/1.x <code> ...". Anything else is not an HTTP
     answer. (Parsed before the raw buffer is released below.) */
  if (strncasecmp(resp_buf, "HTTP/", 5) == 0) {
    const char* space = strchr(resp_buf, ' ');
    if (space != NULL && space < resp_buf + header_end - 2) {
      status = (int)strtol(space + 1, NULL, 10);
    }
  }
  if (status < 100 || status > 599) {
    free(resp_buf);
    return _http_error_response("http_post_json: unparsable status line from "
                                "%s:%u", host, (unsigned)port);
  }

  r = get_clear_memory(sizeof(http_response_t));
  r->status = status;

  /* Chunked framing is peeled into a contiguous body here — the caller
     never learns how the body was framed. A malformed frame (or a decoded
     body over _HTTP_BODY_MAX) is a transport failure, same class as the
     short-body rejection above. */
  if (chunked) {
    size_t decoded_len;
    r->body = _decode_chunked(resp_buf + header_end, total - header_end,
                              &decoded_len);
    free(resp_buf);
    if (r->body == NULL) {
      free(r);
      return _http_error_response("http_post_json: malformed chunked body from "
                                  "%s:%u (connection %s)", host, (unsigned)port,
                                  eof ? "closed" : "timed out");
    }
    r->body_len = decoded_len;
    return r;
  }

  r->body_len = total - header_end;
  if (r->body_len > 0) {
    r->body = get_memory(r->body_len + 1);
    memcpy(r->body, resp_buf + header_end, r->body_len);
    r->body[r->body_len] = '\0';
  }
  free(resp_buf);
  return r;
}