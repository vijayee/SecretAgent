//
// Created by victor on 9/29/26.
//

#ifndef SA_HTTP_H
#define SA_HTTP_H

#include <stddef.h>
#include <stdint.h>

/* Minimal HTTP/1.1 client for local model endpoints (Ollama/vLLM style).
   One POST, one response; the body framing is transparent to the caller —
   Content-Length responses pass through as-is, Transfer-Encoding: chunked
   responses are decoded into a single contiguous body (chunk extensions and
   trailers are discarded); connection per request (fine for turn-scale
   rates). */
typedef struct http_response_t {
  int status;            /* HTTP code, or -1 (transport error) */
  char* body;            /* heap; free() it */
  size_t body_len;
  char* error;           /* heap; transport-level reason; free() it; NULL on success */
} http_response_t;

http_response_t* http_post_json(const char* url,        /* http://host:port/path */
                                const char* api_key,    /* NULL = no auth header */
                                const char* body_json,  /* NUL-terminated */
                                uint32_t timeout_ms);

void http_response_destroy(http_response_t* r);

#endif // SA_HTTP_H