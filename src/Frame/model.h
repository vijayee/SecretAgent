//
// Created by victor on 9/29/26.
//

#ifndef SA_MODEL_H
#define SA_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "../Frame/frame.h"   /* frame_config_t carries base_url/key/model */
#include "../Util/json.h"
/* The sink's headers param (below) references http_headers_t: the ported
   header module's type. model.h has no streams gate of its own — the sink
   typedef and the backend vtable compile in EVERY build (the OFF configs'
   scripted/sync backends ride them) — so the include rides unconditionally
   beside them. The header is a declaration-only include there (its .c is
   streams-compiled); no OFF translation unit ever dereferences a headers
   pointer, because no http backend can exist without streams. */
#include "../Streams/http_headers.h"

/* A completion boundary — nothing more. messages = JSON array of
   {role, content} objects; tools = JSON array (ONE execute tool); reply =
   parsed choice. */
typedef struct model_reply_t {
  char* content;         /* heap assistant text ("" when only a tool call) */
  char* tool_code;       /* heap code string from the execute tool call (NULL when none) */
  char* finish_reason;   /* heap, or NULL */
} model_reply_t;

/* Asynchronous completion delivery (orchestration slice). After a rc==0
   submit, the sink fires EXACTLY ONCE — on the streams loop thread or
   synchronously within submit — and it takes OWNERSHIP of body and error
   (heap; free() or consume). headers = the completion's captured response
   headers, BORROWED for the call's duration (read-then-return: the model.c
   relay hands the pointer straight through and the final owner deinits +
   frees it — the engine's sink in loop.c; the defensive no-sink path frees
   it, too). NULL for the headerless shapes (transport failures, headerless
   or scripted backends). A rc != 0 return means rejected before any I/O:
   the sink will NEVER fire for that call. submit must copy or serialize
   everything it needs from messages/tools before it returns. NULL = the
   backend is sync-only (scripted tests; the engine drains it inline). */
typedef void (*model_response_sink_fn)(void* ctx, int status, char* body,
                                       size_t body_len, char* error,
                                       http_headers_t* headers);

/* vtable so tests inject scripted turns without network: */
typedef struct model_backend_t {
  int (*complete)(void* self, json_value_t* messages, json_value_t* tools,
                  char**, model_reply_t** reply, char** error_out);   /* 0 ok */
  /* NULL = sync-only backend (scripted tests; the engine drains it inline) */
  int (*submit)(void* self, json_value_t* messages, json_value_t* tools,
                model_response_sink_fn on_done, void* on_done_ctx);
} model_backend_t;

/* The http backend is built from the frame config, which lives inside
   frame.h's SA_HAS_WDB gate — so this one declaration rides that same gate
   (model.c's own body is gated identically; see model.c:50). */
#ifdef SA_HAS_WDB

model_backend_t* model_http_backend_create(const frame_config_t* cfg);

#endif /* SA_HAS_WDB */

/* The completion timeout the http backend passes to the http client: the
   frame config's model_timeout_ms when nonzero, otherwise the built-in
   default (SA_MODEL_TIMEOUT_MS's 30000). Declared outside the WDB gate
   (pure arithmetic) but defined in model.c's gated body — reachable from
   the same builds that can call model_http_backend_create. Exposed so tests
   can pin the 0-means-default derivation. */
unsigned model_timeout_ms_resolve(unsigned cfg_ms);

void model_backend_destroy(model_backend_t* mb);
void model_reply_destroy(model_reply_t* r);

#endif // SA_MODEL_H