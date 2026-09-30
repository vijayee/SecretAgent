//
// Created by victor on 9/29/26.
//

#ifndef SA_MODEL_H
#define SA_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "../Frame/frame.h"   /* frame_config_t carries base_url/key/model */
#include "../Util/json.h"

/* A completion boundary — nothing more. messages = JSON array of
   {role, content} objects; tools = JSON array (ONE execute tool); reply =
   parsed choice. */
typedef struct model_reply_t {
  char* content;         /* heap assistant text ("" when only a tool call) */
  char* tool_code;       /* heap code string from the execute tool call (NULL when none) */
  char* finish_reason;   /* heap, or NULL */
} model_reply_t;

/* vtable so tests inject scripted turns without network: */
typedef struct model_backend_t {
  int (*complete)(void* self, json_value_t* messages, json_value_t* tools,
                  char**, model_reply_t** reply, char** error_out);   /* 0 ok */
} model_backend_t;

model_backend_t* model_http_backend_create(const frame_config_t* cfg);
void model_backend_destroy(model_backend_t* mb);
void model_reply_destroy(model_reply_t* r);

#endif // SA_MODEL_H