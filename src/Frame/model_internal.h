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
#endif /* SA_HAS_WDB */

#endif // SA_MODEL_INTERNAL_H