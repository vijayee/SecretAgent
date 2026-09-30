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
//   - The http client is constructed per call (http_post_json is one-shot, one
//     connection per request) — no connection reuse in this milestone; the
//     reconnect cost is turn-scale, and port-refusals fail instantly while
//     blocked connects are bounded by the kernel default.
//
// Body is compiled only in the WaveDB build: frame_config_t lives inside
// frame.h's SA_HAS_WDB guard (same gate as frame.c's store body).

#include "model.h"

#include "../Net/http.h"
#include "../Util/allocator.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SA_HAS_WDB

/* One-shot POST bound: the model client has no connection reuse and no
   retries, so the timeout only has to cover one send/recv exchange.
   frame_config_t's model_timeout_ms overrides it per-backend (0 = this
   default) — local models on big tool-calling turns can run minutes. */
#define SA_MODEL_TIMEOUT_MS 30000
/* Heap error strings are formatted into a bounded scratch (truncation-safe
   like the http layer's reason strings). */
#define SA_MODEL_ERROR_MAX 512
/* Non-2xx error strings carry at most this many body bytes as an excerpt. */
#define SA_MODEL_EXCERPT_MAX 200
/* Joined URL scratch; local endpoints never approach this (overflow is a
   loud request-build error, not silent truncation). */
#define SA_MODEL_URL_MAX 1024

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

typedef struct _model_http_backend_t {
  model_backend_t base;      /* FIRST member: casts between the types are exact */
  char* base_url;
  char* api_key;             /* NULL when unused (Ollama) */
  char* model_name;
  unsigned timeout_ms;       /* one-shot POST bound (resolved from the config) */
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

  http_response_t* r = http_post_json(url, b->api_key, request_text,
                                      b->timeout_ms);
  free(request_text);
  if (r == NULL) {
    if (error_out != NULL) {
      *error_out = _model_error("model client: transport: http client returned "
                                "no response");
    }
    return -1;
  }
  if (r->status < 200 || r->status >= 300) {
    if (error_out != NULL) {
      char* excerpt = _model_excerpt(r->body, r->body_len);
      /* Transport failures (status -1) carry the http layer's reason in
         r->error and no body — surface it so a live misdiagnosis never
         reads as an empty reply. */
      const char* detail =
        (excerpt != NULL && excerpt[0] != '\0') ? excerpt
        : (r->error != NULL && r->error[0] != '\0') ? r->error
        : "(no body)";
      *error_out = _model_error("model client: HTTP %d: %s", r->status, detail);
      free(excerpt);
    }
    net_http_response_destroy(r);
    return -1;
  }

  model_reply_t* decoded = NULL;
  char* decode_err = NULL;
  if (_model_decode_body(r->body, r->body_len, &decoded, &decode_err) != 0) {
    if (error_out != NULL) *error_out = decode_err; else free(decode_err);
    net_http_response_destroy(r);
    return -1;
  }
  if (raw_out != NULL && r->body != NULL) {
    *raw_out = _model_heap_str(r->body);
  }
  net_http_response_destroy(r);
  *reply = decoded;
  return 0;
}

unsigned model_timeout_ms_resolve(unsigned cfg_ms) {
  return (cfg_ms != 0) ? cfg_ms : SA_MODEL_TIMEOUT_MS;
}

model_backend_t* model_http_backend_create(const frame_config_t* cfg) {
  if (cfg == NULL) return NULL;
  if (cfg->model_base_url == NULL || cfg->model_base_url[0] == '\0') return NULL;
  if (cfg->model_name == NULL || cfg->model_name[0] == '\0') return NULL;

  /* Not a refcounted object: the backend is a plain owned vtable node, freed
     exactly once through model_backend_destroy. */
  _model_http_backend_t* b =
    (_model_http_backend_t*)get_clear_memory(sizeof(_model_http_backend_t));
  b->base.complete = _model_http_complete;
  b->base_url = _model_heap_str(cfg->model_base_url);
  b->model_name = _model_heap_str(cfg->model_name);
  b->timeout_ms = model_timeout_ms_resolve(cfg->model_timeout_ms);
  /* Empty key == no key (Ollama); NULL out rather than carrying "". */
  b->api_key = (cfg->model_api_key != NULL && cfg->model_api_key[0] != '\0')
                 ? _model_heap_str(cfg->model_api_key) : NULL;
  return &b->base;
}

void model_backend_destroy(model_backend_t* mb) {
  if (mb == NULL) return;
  _model_http_backend_t* b = (_model_http_backend_t*)mb;
  free(b->base_url);
  free(b->api_key);
  free(b->model_name);
  free(b);
}

void model_reply_destroy(model_reply_t* r) {
  if (r == NULL) return;
  free(r->content);
  free(r->tool_code);
  free(r->finish_reason);
  free(r);
}

#endif /* SA_HAS_WDB */