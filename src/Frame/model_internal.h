//
// Created by victor on 9/30/26.
//

#ifndef SA_MODEL_INTERNAL_H
#define SA_MODEL_INTERNAL_H

#include "../Frame/model.h"
#include <stddef.h>
#include "../Util/json.h"

#ifdef SA_HAS_WDB
/* status + raw body + transport error → decoded reply, with model.c's exact
   error surface ("model client: HTTP %d: <body excerpt | transport reason |
   (no body)>"). 0 ok (*reply_out owns the reply); nonzero (*error_out owns
   the reason). The single helper behind BOTH the synchronous complete() and
   the engine's FRM_MODEL_RESULT behavior — one error surface, two
   delivery modes. */
int _model_result_from_http(int status, const char* body, size_t body_len,
                            const char* transport_error,
                            model_reply_t** reply_out, char** error_out);

#if defined(SA_HAS_STREAMS)
/* The http request-body builder, exposed for the refine slice's no-tools
   contract test (the SAME function _model_http_complete uses internally —
   one body builder, one truth). Returns the serialized request document
   (heap, caller frees), 0 ok / nonzero + *error_out loud. tools semantics:
   NULL pointer = the canned execute tool (unchanged); a JSON array rides
   verbatim with tool_choice "auto"; a JSON NULL is EXPLICITLY no tools —
   BOTH the "tools" and "tool_choice" keys are omitted from the request. */
int _model_request_body(const char* model_name, json_value_t* messages,
                        json_value_t* tools, char** body_out, char** error_out);
#endif /* SA_HAS_STREAMS */
#endif /* SA_HAS_WDB */

#endif // SA_MODEL_INTERNAL_H