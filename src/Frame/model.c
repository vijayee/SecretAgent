//
// Created by victor on 9/29/26.
//
// OpenAI-compatible completion client over the Task 8 http client.
//
// A completion boundary — nothing more. One POST per call to
// <base>/v1/chat/completions (Ollama-compatible), the first choice decoded
// into a model_reply_t. NO streaming, NO retries: the loop (Task 10) owns the
// turn structure and any retry policy.
//
// Contract notes (choices this client makes):
//
//   - messages is BORROWED (caller owns the json_value_t) and must be a JSON
//     array; tools may be NULL to get the canned single `execute` cell tool,
//     or a caller-owned JSON array passed through in place (supersets like
//     scripted test tools). Borrowed values are copied into the request via
//     serialize -> re-parse; the caller never loses ownership.
//   - `tool_calls[0].function.arguments` arrives in TWO shapes across
//     OpenAI-compatible servers: a JSON STRING containing the argument
//     object ({"code": "..."}) — the OpenAI shape Ollama emits — or, in
//     lenient servers, the argument object directly. Both are decoded here;
//     in the string form the string's content must parse back to an object
//     carrying a string `code`.
//   - The unnamed `char**` 4th parameter of complete() is the raw serialized
//     response body out-param (heap, caller frees; NULL when nothing decoded;
//     tolerated as NULL by callers). Useful for logging the exchange.
//   - Errors: complete returns nonzero and sets *error_out to a one-line heap
//     string, prefixed "model client:". Transport failures surface the http
//     layer's reason; non-2xx responses carry the status CODE plus a bounded
//     body excerpt (a non-2xx arrives success-shaped from http: status=code,
//     error=NULL); decode failures carry the json_parse reason.
//   - The http client is constructed per call (one POST, one connection per
//     request) over the process's ONE streams loop thread, which the first
//     backend mounts and the last backend tears down. No connection reuse in
//     this milestone; the reconnect cost is turn-scale, and port-refusals
//     fail instantly while blocked connects are bounded by the client timer.
//   - complete() is synchronous on the calling turn thread: submit, wait on
//     the completion record's condvar, then decode. Blocking the caller is
//     acceptable for the same reason py_agent's bridge wait is — the caller
//     is a dedicated runtime/turn thread, the wait is BOUNDED (the client's
//     request timer plus dispatch slack), and nothing on the reactor loop
//     ever waits on a caller (the completion callback is µs-scale).
//
// Body is compiled only in the WaveDB build: frame_config_t lives inside
// frame.h's SA_HAS_WDB guard (same gate as frame.c's store body).

#include "model.h"

#include "../Streams/http_client.h"
#include "../Streams/loop_thread.h"
#include "../Platform/platform.h"
#include "../RefCounter/refcounter.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include "../Util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SA_HAS_WDB

/* One-shot POST bound: the model client has no connection reuse and no
   retries, so the timeout only has to cover one send/recv exchange. The
   request timer rides the streams client's loop timer; frame_config_t's
   model_timeout_ms overrides it per-backend (0 = this default) — local
   models on big tool-calling turns can run minutes. */
#define SA_MODEL_TIMEOUT_MS 30000
/* Dispatch slack over the client's own request timer: the waiter bound is
   timeout_ms + this. When the completion misses even THAT, the request was
   lost (loop thread died mid-flight) — the failure is loud, not a hang. */
#define SA_MODEL_SLACK_MS 5000
/* Heap error strings are formatted into a bounded scratch (truncation-safe
   like the http layer's reason strings). */
#define SA_MODEL_ERROR_MAX 512
/* Non-2xx error strings carry at most this many body bytes as an excerpt. */
#define SA_MODEL_EXCERPT_MAX 200
/* Joined URL scratch; local endpoints never approach this (overflow is a
   loud request-build error, not silent truncation). */
#define SA_MODEL_URL_MAX 1024

/* The http backend rides BOTH gates: the completion boundary is the WaveDB
   gate's shape (frame_config_t), but its transport is the streams client —
   so the machinery below is wrapped in SA_HAS_STREAMS too. Under a
   WDB-no-streams build only the three always-linked functions below remain
   (the frame refuses its default-backend build there in the same shape). */
#if defined(SA_HAS_STREAMS)

static const char* EXECUTE_TOOL_DESCRIPTION =
  "Execute one Python cell in the frame's interpreter. Give the COMPLETE cell "
  "body as `code`; state and child frames go through the actor verbs "
  "(remember/recall/spawn/report). One tool call per turn.";

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

/* Heap copy via get_memory (aborts on OOM; callers free with free()). */
static char* _model_heap_str(const char* s) {
  size_t n = strlen(s);
  char* out = (char*)get_memory(n + 1);
  memcpy(out, s, n + 1);
  return out;
}

/* One-line heap error string (bounded, truncation-safe). */
static char* _model_error(const char* fmt, ...) {
  char* out = (char*)get_memory(SA_MODEL_ERROR_MAX);
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(out, SA_MODEL_ERROR_MAX, fmt, args);
  va_end(args);
  if (written < 0 || written >= SA_MODEL_ERROR_MAX) {
    out[SA_MODEL_ERROR_MAX - 1] = '\0';
  }
  return out;
}

/* First bytes of a response body, as a heap string, for error excerpts.
   Anything non-printable is dropped so control bytes never reach logs. */
static char* _model_excerpt(const char* body, size_t body_len) {
  size_t n = body_len < SA_MODEL_EXCERPT_MAX ? body_len : SA_MODEL_EXCERPT_MAX;
  char* out = (char*)get_memory(n + 1);
  size_t used = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)body[i];
    if (c < 0x20 || c == 0x7F) continue;
    out[used++] = (char)c;
  }
  out[used] = '\0';
  return out;
}

/* Owned deep copy of a BORROWED json value, via serialize -> re-parse (the
   codec round-trips every shape it emits). JSON_NULL/NULL yields NULL. */
static json_value_t* _model_owned_copy(const json_value_t* v, char** error_out) {
  if (v == NULL || json_type(v) == JSON_NULL) return NULL;
  char* text = json_serialize(v);
  char* parse_err = NULL;
  json_value_t* copy = json_parse(text, strlen(text), &parse_err);
  free(text);
  if (copy == NULL) {
    *error_out = _model_error("model client: request build: cannot copy the "
                              "supplied value: %s",
                              parse_err ? parse_err : "parse failed");
  }
  free(parse_err);
  return copy;
}

/* ------------------------------------------------------------------ */
/* Request build                                                      */
/* ------------------------------------------------------------------ */

static json_value_t* _model_execute_tool(void) {
  json_value_t* code_param = json_new_object();
  json_object_set(code_param, "type", json_new_string("string"));

  json_value_t* properties = json_new_object();
  json_object_set(properties, "code", code_param);

  json_value_t* parameters = json_new_object();
  json_object_set(parameters, "type", json_new_string("object"));
  json_object_set(parameters, "properties", properties);

  json_value_t* required = json_new_array();
  json_array_append(required, json_new_string("code"));
  json_object_set(parameters, "required", required);

  json_value_t* function = json_new_object();
  json_object_set(function, "name", json_new_string("execute"));
  json_object_set(function, "description", json_new_string(EXECUTE_TOOL_DESCRIPTION));
  json_object_set(function, "parameters", parameters);

  json_value_t* tool = json_new_object();
  json_object_set(tool, "type", json_new_string("function"));
  json_object_set(tool, "function", function);

  json_value_t* tools = json_new_array();
  json_array_append(tools, tool);
  return tools;
}

/* ------------------------------------------------------------------ */
/* The process's one streams loop                                     */
/* ------------------------------------------------------------------ */

/* Every http model backend rides ONE reactor thread per process: the first
   backend create mounts it, the LAST backend destroy tears it down (a plain
   user count under the same lock). The thread itself is a dedicated runtime
   thread whose completions callback for microseconds — the same shape as
   py_agent's bridge wait (dedicated worker thread, bounded caller-side
   block), so blocking a turn on the completion record below is acceptable.
   The guard mutex doubles as the mount lock and is never freed (installed
   once, like py_agent's registry mount — a process-lifetime primitive). */
static _Atomic(platform_mutex_t*) _model_loop_guard = NULL;
static streams_loop_thread_t* _model_loop = NULL;   /* guarded by the guard lock */
static unsigned _model_loop_users = 0;              /* guarded by the guard lock */

/* Acquires the process's streams loop thread, creating it on first use, and
   pins it for this backend's lifetime. Returns NULL when the loop thread
   cannot be mounted. */
static streams_loop_thread_t* _model_loop_acquire(void) {
  platform_mutex_t* m = platform_mutex_create();
  platform_mutex_t* expected = NULL;
  if (!atomic_compare_exchange_strong(&_model_loop_guard, &expected, m)) {
    /* Lost the install race: the winner's mutex is in the guard. */
    platform_mutex_destroy(m);
    m = atomic_load(&_model_loop_guard);
  }
  if (m == NULL) {
    log_error("model_http_backend_create: loop guard install failed");
    return NULL;
  }
  platform_mutex_lock(m);
  if (_model_loop == NULL) {
    _model_loop = streams_loop_create();
  }
  if (_model_loop != NULL) {
    _model_loop_users++;
  }
  streams_loop_thread_t* loop = _model_loop;
  platform_mutex_unlock(m);
  return loop;
}

/* Drops one backend's pin; the loop dies only with the LAST backend. */
static void _model_loop_release(void) {
  platform_mutex_t* m = atomic_load(&_model_loop_guard);   /* install-once */
  if (m == NULL) return;   /* never mounted: no backend did either */
  platform_mutex_lock(m);
  if (_model_loop_users > 0 && --_model_loop_users == 0) {
    streams_loop_destroy(_model_loop);
    _model_loop = NULL;
  }
  platform_mutex_unlock(m);
}

/* ------------------------------------------------------------------ */
/* The per-request completion record (refcounted wait block)          */
/* ------------------------------------------------------------------ */

/* One POST's wait block, heap: the completion may fire long after the
   submitting frame returned (a timed-out waiter must not race the record's
   death, and a lost completion must not leak it). Ref discipline: the
   WAITER (the turn thread inside complete()) holds its own reference from
   before submit until after it has settled the record under the lock; the
   CALLBACK holds one pre-claimed reference and releases it when done
   filling+firing. Whoever releases LAST frees the record and the primitives
   — that is what makes fire-past-a-timed-out-waiter and waiter-outlived-
   cancel both safe and leak-free without ordering the two sides. */
typedef struct _model_completion_t {
  refcounter_t refcounter;    /* FIRST member: casts between the types are exact */
  platform_mutex_t* lock;     /* shared pair; freed by the last releaser */
  platform_condvar_t* cv;
  int done;                   /* set under the lock before the signal */
  int status;                 /* http status, or -1 on transport failure */
  char* body;                 /* steal-slot: heap body moves out to the waiter */
  size_t body_len;
  char* error;                /* transport reason on status -1 */
} _model_completion_t;

static _model_completion_t* _model_completion_create(void) {
  _model_completion_t* rec = get_clear_memory(sizeof(_model_completion_t));
  rec->lock = platform_mutex_create();
  rec->cv = platform_condvar_create();
  if (rec->lock == NULL || rec->cv == NULL) {
    log_error("model client: completion record primitives failed");
    if (rec->lock != NULL) platform_mutex_destroy(rec->lock);
    if (rec->cv != NULL) platform_condvar_destroy(rec->cv);
    free(rec);
    return NULL;
  }
  refcounter_init(&rec->refcounter);   /* the waiter's own reference; LAST */
  return rec;
}

/* Drops one reference; the last out tears the record down (never-freed
   steal-slot strings are the dropped waiter's leftovers — the record is the
   only owner once the completion filled them). */
static void _model_completion_release(_model_completion_t* rec) {
  if (refcounter_dereference_is_zero(&rec->refcounter)) {
    free(rec->body);
    free(rec->error);
    platform_condvar_destroy(rec->cv);
    platform_mutex_destroy(rec->lock);
    free(rec);
  }
}

/* The http client's completion — runs ON the loop thread, µs-scale (a lock,
   some field writes, a broadcast). The completion's heap strings move into
   the steal-slot; the waiter takes them, a dropped waiter leaves them to
   this record's teardown. */
static void _model_completion_on(void* ctx, int status, char* body,
                                 size_t body_len, char* error) {
  _model_completion_t* rec = (_model_completion_t*)ctx;
  platform_mutex_lock(rec->lock);
  rec->status = status;
  rec->body = body;
  rec->body_len = body_len;
  rec->error = error;
  rec->done = 1;
  platform_condvar_broadcast(rec->cv);
  platform_mutex_unlock(rec->lock);
  _model_completion_release(rec);
}

/* ------------------------------------------------------------------ */
/* Backend                                                            */
/* ------------------------------------------------------------------ */

typedef struct _model_http_backend_t {
  model_backend_t base;      /* FIRST member: casts between the types are exact */
  char* base_url;
  char* api_key;             /* NULL when unused (Ollama) */
  char* model_name;
  unsigned timeout_ms;       /* one-shot POST bound (resolved from the config) */
  streams_loop_thread_t* loop;   /* borrowed; pinned by _model_loop_acquire */
} _model_http_backend_t;

/* Fills *body_out with the serialized request document. Caller frees. */
static int _model_request_text(const _model_http_backend_t* b,
                              json_value_t* messages, json_value_t* tools,
                              char** body_out, char** error_out) {
  *body_out = NULL;
  *error_out = NULL;
  if (json_type(messages) != JSON_ARRAY) {
    *error_out = _model_error("model client: request build: messages must be "
                              "a JSON array");
    return -1;
  }
  if (tools != NULL && json_type(tools) != JSON_ARRAY && json_type(tools) != JSON_NULL) {
    *error_out = _model_error("model client: request build: tools must be a "
                              "JSON array or NULL");
    return -1;
  }
  json_value_t* owned_messages = _model_owned_copy(messages, error_out);
  if (owned_messages == NULL) return -1;

  json_value_t* owned_tools = NULL;
  if (tools != NULL && json_type(tools) == JSON_ARRAY) {
    owned_tools = _model_owned_copy(tools, error_out);
  } else {
    owned_tools = _model_execute_tool();
  }
  if (owned_tools == NULL) {
    json_value_destroy(owned_messages);
    return -1;
  }

  json_value_t* req = json_new_object();
  json_object_set(req, "model", json_new_string(b->model_name));
  json_object_set(req, "messages", owned_messages);    /* takes value */
  json_object_set(req, "tools", owned_tools);          /* takes value */
  /* tool_choice is part of the request contract, whatever tools carry it. */
  json_object_set(req, "tool_choice", json_new_string("auto"));
  char* text = json_serialize(req);
  json_value_destroy(req);
  *body_out = text;
  return 0;
}

/* <base>/v1/chat/completions with trailing slashes on base trimmed. */
static int _model_join_url(const char* base_url, char* out, size_t out_size) {
  size_t n = strlen(base_url);
  while (n > 0 && base_url[n - 1] == '/') n--;
  int written = snprintf(out, out_size, "%.*s/v1/chat/completions", (int)n, base_url);
  if (written < 0 || (size_t)written >= out_size) return -1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Reply decode                                                       */
/* ------------------------------------------------------------------ */

/* Extract `code` from tool_calls[0].function.arguments in either shape
   (string containing the object, or the object directly). */
static int _model_decode_arguments(const json_value_t* arguments,
                                   char** code_out, char** error_out) {
  *code_out = NULL;

  json_value_t* args_object = (json_value_t*)arguments;   /* borrowed default */
  json_value_t* parsed = NULL;                            /* owned in string form */
  if (json_type(arguments) == JSON_STRING) {
    /* String form: the string's CONTENT is the JSON document. */
    const char* args_text = json_as_string(arguments);
    char* parse_err = NULL;
    parsed = json_parse(args_text, strlen(args_text), &parse_err);
    if (parsed == NULL) {
      *error_out = _model_error("model client: decode: tool call arguments "
                                "are not valid JSON: %s",
                                parse_err ? parse_err : "parse failed");
      free(parse_err);
      return -1;
    }
    args_object = parsed;
  } else if (json_type(arguments) != JSON_OBJECT) {
    *error_out = _model_error("model client: decode: tool call arguments are "
                              "neither a JSON string nor an object");
    return -1;
  }

  json_value_t* code = json_get(args_object, "code");
  if (json_type(code) != JSON_STRING) {
    json_value_destroy(parsed);
    *error_out = _model_error("model client: decode: tool call has no string "
                              "`code` argument");
    return -1;
  }
  *code_out = _model_heap_str(json_as_string(code));
  json_value_destroy(parsed);   /* NULL when the borrowed object form was used */
  return 0;
}

/* body -> model_reply_t. body may be NULL when body_len == 0 (the http layer
   NUL-terminates the buffer at +1 either way). */
static int _model_decode_body(const char* body, size_t body_len,
                              model_reply_t** reply_out, char** error_out) {
  *reply_out = NULL;
  *error_out = NULL;
  if (body == NULL || body_len == 0) {
    *error_out = _model_error("model client: decode: response body is empty");
    return -1;
  }
  char* parse_err = NULL;
  json_value_t* root = json_parse(body, body_len, &parse_err);
  if (root == NULL) {
    *error_out = _model_error("model client: decode: %s",
                              parse_err ? parse_err : "malformed response");
    free(parse_err);
    return -1;
  }
  free(parse_err);

  if (json_type(root) != JSON_OBJECT) {
    *error_out = _model_error("model client: decode: choices missing (root is "
                              "not an object)");
    json_value_destroy(root);
    return -1;
  }
  json_value_t* choices = json_get(root, "choices");
  if (json_type(choices) != JSON_ARRAY || json_size(choices) == 0) {
    *error_out = _model_error("model client: decode: choices missing");
    json_value_destroy(root);
    return -1;
  }
  json_value_t* choice = json_at(choices, 0);
  if (json_type(choice) != JSON_OBJECT) {
    *error_out = _model_error("model client: decode: first choice is not an object");
    json_value_destroy(root);
    return -1;
  }
  json_value_t* message = json_get(choice, "message");
  if (json_type(message) != JSON_OBJECT) {
    *error_out = _model_error("model client: decode: message missing in first choice");
    json_value_destroy(root);
    return -1;
  }

  model_reply_t* reply = (model_reply_t*)get_clear_memory(sizeof(model_reply_t));
  reply->content = NULL;
  reply->tool_code = NULL;
  reply->finish_reason = NULL;

  /* content: absent or null -> "" (assistant text may come only as a tool
     call); any other non-string type is a decode error. */
  json_value_t* content = json_get(message, "content");
  if (content == NULL || json_type(content) == JSON_NULL) {
    reply->content = (char*)get_memory(2);
    reply->content[0] = '\0';
  } else if (json_type(content) == JSON_STRING) {
    reply->content = _model_heap_str(json_as_string(content));
  } else {
    *error_out = _model_error("model client: decode: message.content is neither "
                              "absent nor a string");
    json_value_destroy(root);
    model_reply_destroy(reply);
    return -1;
  }

  /* Reasoning models (Ollama gemma4-class) legitimately answer with an
     empty `content` and their text in a `reasoning` string field. When
     content is EMPTY (whether absent, null, or literally ""), the reasoning
     IS the assistant's turn text — surfacing it keeps the transcript and
     the audit trail from vanishing into a silent empty completion. A
     non-empty content always wins; both are kept verbatim then. */
  if (reply->content[0] == '\0') {
    json_value_t* reasoning = json_get(message, "reasoning");
    if (reasoning != NULL && json_type(reasoning) == JSON_STRING &&
        json_as_string(reasoning)[0] != '\0') {
      char* r = _model_heap_str(json_as_string(reasoning));
      if (r != NULL) {
        free(reply->content);
        reply->content = r;
      }
    }
  }

  /* tool_calls: only the FIRST call is consumed — the single-tool surface
     means one cell per turn. */
  json_value_t* tool_calls = json_get(message, "tool_calls");
  if (tool_calls != NULL && json_type(tool_calls) == JSON_ARRAY &&
      json_size(tool_calls) > 0) {
    json_value_t* tool_call = json_at(tool_calls, 0);
    json_value_t* function = json_get(tool_call, "function");
    json_value_t* arguments = json_get(function, "arguments");
    if (_model_decode_arguments(arguments, &reply->tool_code, error_out) != 0) {
      json_value_destroy(root);
      model_reply_destroy(reply);
      return -1;
    }
  }

  json_value_t* finish_reason = json_get(choice, "finish_reason");
  if (json_type(finish_reason) == JSON_STRING) {
    reply->finish_reason = _model_heap_str(json_as_string(finish_reason));
  } else {
    reply->finish_reason = NULL;
  }

  json_value_destroy(root);
  *reply_out = reply;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Backend                                                            */
/* ------------------------------------------------------------------ */

static int _model_http_complete(void* self, json_value_t* messages,
                                json_value_t* tools, char** raw_out,
                                model_reply_t** reply, char** error_out) {
  _model_http_backend_t* b = (_model_http_backend_t*)self;
  if (error_out != NULL) *error_out = NULL;
  if (raw_out != NULL) *raw_out = NULL;
  if (reply != NULL) *reply = NULL;

  char url[SA_MODEL_URL_MAX];
  if (_model_join_url(b->base_url, url, sizeof(url)) != 0) {
    if (error_out != NULL) {
      *error_out = _model_error("model client: base url overflows the %d-byte "
                                "url buffer", SA_MODEL_URL_MAX);
    }
    return -1;
  }
  char* request_text = NULL;
  char* build_err = NULL;
  if (_model_request_text(b, messages, tools, &request_text, &build_err) != 0) {
    if (error_out != NULL) *error_out = build_err; else free(build_err);
    return -1;
  }

  /* One client per POST (the completion record below pins nothing longer);
     the request timer rides the client's own loop timer. */
  http_client_t* client = http_client_create(b->loop);
  if (client == NULL) {
    if (error_out != NULL) {
      *error_out = _model_error("model client: transport: http client "
                                "allocation failed");
    }
    free(request_text);
    return -1;
  }

  _model_completion_t* rec = _model_completion_create();
  if (rec == NULL) {
    if (error_out != NULL) {
      *error_out = _model_error("model client: transport: wait record "
                                "allocation failed");
    }
    free(request_text);
    http_client_destroy(client);
    return -1;
  }

  /* The callback's reference is claimed BEFORE submit: the completion may
     fire the instant the submit op lands on the loop thread, and it must
     never fire into a record whose only reference is a waiter that already
     left (or keep it alive only by luck of ordering). */
  refcounter_reference(&rec->refcounter);   /* the callback's reference */
  int rc = http_client_submit(client, url, b->api_key, request_text,
                              b->timeout_ms, _model_completion_on, rec);
  free(request_text);
  if (rc != 0) {
    /* Rejected before any I/O: this completion NEVER fires (the http
       client's contract), so the pre-claimed reference is dropped here. */
    _model_completion_release(rec);          /* the callback's reference */
    _model_completion_release(rec);          /* the waiter's own */
    http_client_destroy(client);
    if (error_out != NULL) {
      *error_out = _model_error("model client: transport: request refused "
                                "before any I/O");
    }
    return -1;
  }

  /* The bounded wait: the client owns the request timeout (its loop timer
     fires a transport-error completion); the +5000 ms is dispatch slack, so
     a LOST completion fails loud instead of hanging the turn forever. */
  uint64_t deadline_ns = platform_monotonic_ns() +
      ((uint64_t)b->timeout_ms + SA_MODEL_SLACK_MS) * 1000000ULL;
  int fired = 0;
  int settled = 0;             /* 1 once the steal-slot moved into the locals */
  int status = 0;
  char* body = NULL;
  size_t body_len = 0;
  char* error = NULL;
  platform_mutex_lock(rec->lock);
  while (!rec->done) {
    uint64_t now = platform_monotonic_ns();
    if (now >= deadline_ns) break;
    uint64_t remaining_ms = (deadline_ns - now) / 1000000ULL + 1;
    platform_condvar_timed_wait(rec->cv, rec->lock, (uint32_t)remaining_ms);
  }
  /* Steal the result under the lock the completion fired under. */
  fired = rec->done;
  if (fired) {
    status = rec->status;
    body = rec->body;
    rec->body = NULL;
    body_len = rec->body_len;
    error = rec->error;
    rec->error = NULL;
    settled = 1;
  }
  platform_mutex_unlock(rec->lock);

  /* http_client_destroy is the settle point: the loop thread is serial and
     this op is the client's last, so every completion callback has already
     run (filled the record and released its reference) — or the request was
     cancelled and its callback will NEVER run, so the pre-claimed reference
     must be absorbed by this waiter below. A completion that raced the
     wait's give-up filled the record in between: settle it now. */
  http_client_destroy(client);
  if (!settled) {
    platform_mutex_lock(rec->lock);
    fired = rec->done;
    if (fired) {
      status = rec->status;
      body = rec->body;
      rec->body = NULL;
      body_len = rec->body_len;
      error = rec->error;
      rec->error = NULL;
      settled = 1;
    }
    platform_mutex_unlock(rec->lock);
  }
  _model_completion_release(rec);              /* the waiter's own reference */
  if (!fired) {
    _model_completion_release(rec);            /* absorb the never-fired callback's */
  }

  if (!fired) {
    if (error_out != NULL) {
      *error_out = _model_error("model client: transport: completion never "
                                "fired within %u ms (the request was lost or "
                                "cancelled)", b->timeout_ms + SA_MODEL_SLACK_MS);
    }
    return -1;
  }
  if (status < 200 || status >= 300) {
    if (error_out != NULL) {
      char* excerpt = _model_excerpt(body, body_len);
      /* Transport failures (status -1) carry the http layer's reason in
         error and no body — surface it so a live misdiagnosis never reads
         as an empty reply. */
      const char* detail =
        (excerpt != NULL && excerpt[0] != '\0') ? excerpt
        : (error != NULL && error[0] != '\0') ? error
        : "(no body)";
      *error_out = _model_error("model client: HTTP %d: %s", status, detail);
      free(excerpt);
    }
    free(body);
    free(error);
    return -1;
  }

  model_reply_t* decoded = NULL;
  char* decode_err = NULL;
  if (_model_decode_body(body, body_len, &decoded, &decode_err) != 0) {
    if (error_out != NULL) *error_out = decode_err; else free(decode_err);
    free(body);
    return -1;
  }
  /* The stolen body (heap, NUL-terminated by the client) moves out raw. */
  if (raw_out != NULL) {
    *raw_out = body;
    body = NULL;
  }
  free(body);
  *reply = decoded;
  return 0;
}

model_backend_t* model_http_backend_create(const frame_config_t* cfg) {
  if (cfg == NULL) return NULL;
  if (cfg->model_base_url == NULL || cfg->model_base_url[0] == '\0') return NULL;
  if (cfg->model_name == NULL || cfg->model_name[0] == '\0') return NULL;

  /* Acquire the process's one streams loop BEFORE building (a loop mount
     failure refuses the backend instead of failing per request later). The
     loop create is blocking thread setup on the caller — acceptable here
     (init-time) and documented at _model_loop_acquire. */
  streams_loop_thread_t* loop = _model_loop_acquire();
  if (loop == NULL) return NULL;

  /* Not a refcounted object: the backend is a plain owned vtable node, freed
     exactly once through model_backend_destroy. */
  _model_http_backend_t* b =
    (_model_http_backend_t*)get_clear_memory(sizeof(_model_http_backend_t));
  b->base.complete = _model_http_complete;
  b->base_url = _model_heap_str(cfg->model_base_url);
  b->model_name = _model_heap_str(cfg->model_name);
  b->timeout_ms = model_timeout_ms_resolve(cfg->model_timeout_ms);
  b->loop = loop;
  /* Empty key == no key (Ollama); NULL out rather than carrying "". */
  b->api_key = (cfg->model_api_key != NULL && cfg->model_api_key[0] != '\0')
                 ? _model_heap_str(cfg->model_api_key) : NULL;
  return &b->base;
}

#endif /* SA_HAS_STREAMS — end of the http-transport machinery */

unsigned model_timeout_ms_resolve(unsigned cfg_ms) {
  return (cfg_ms != 0) ? cfg_ms : SA_MODEL_TIMEOUT_MS;
}

void model_backend_destroy(model_backend_t* mb) {
  if (mb == NULL) return;
#if defined(SA_HAS_STREAMS)
  _model_http_backend_t* b = (_model_http_backend_t*)mb;
  free(b->base_url);
  free(b->api_key);
  free(b->model_name);
  free(b);
  _model_loop_release();   /* drop this backend's pin; last out kills the loop */
#else
  /* No-streams build: no http backend can be built (model_http_backend_create
     does not exist and the frame refuses its default-backend build), so the
     destroy only ever sees a caller's own vtable node — free the shell. */
  free(mb);
#endif
}

void model_reply_destroy(model_reply_t* r) {
  if (r == NULL) return;
  free(r->content);
  free(r->tool_code);
  free(r->finish_reason);
  free(r);
}

#endif /* SA_HAS_WDB */