//
// Created by victor on 10/02/26.
//

/* ONE budget table per boundary (surface-completion spec §4): every byte/entry
   cap in the runtime has a name here. Source caps are applied AT the source
   actor (bounded durable text — the truncate-marker helper is the only
   enforcement); projection caps bound what the derive renders. The transport
   boundary (_HTTP_BODY_MAX / _HTTP_READ_MAX in src/Streams/http_client.c) is
   NOT policy — it stays there, cross-referenced here. */

#ifndef SA_BUDGET_H
#define SA_BUDGET_H

#include <stddef.h>
#include <stdint.h>

/* Source caps (the runtime's own payloads, cut before they become durable). */
#define SA_BUDGET_CELL_RESULT_BYTES (32u * 1024u)
#define SA_BUDGET_EMIT_BYTES (16u * 1024u)
#define SA_BUDGET_BRIDGE_VALUE_BYTES (16u * 1024u)

/* Listing caps. */
#define SA_BUDGET_KEYS_MAX 256

/* The pooled cell's watchdog default (frame_config_t.cell_watchdog_ms;
   0 = disabled). */
#define SA_FRAME_CELL_WATCHDOG_MS 300000u

/* Projection caps (moved verbatim from src/Frame/loop.c; values unchanged). */
#define SA_BUDGET_LOOP_MSG_CAP 4000
#define SA_BUDGET_LOOP_SNAPSHOT 500
#define SA_BUDGET_LOOP_REPORT 300
#define SA_BUDGET_LOOP_EMIT 300

/* Truncate a text to cap bytes, appending the one marker shape
   ("\n[budget: truncated at <original-length> bytes]") when a cut happened.
   The OUTPUT is min(strlen(text), cap) bytes of text + the marker, so the
   total never exceeds cap + strlen(marker) and the marker names the ORIGINAL
   length. Clean copy (no marker) when strlen(text) <= cap; *out_truncated
   is nonzero only on a real cut.
   Refusal: *out_text = NULL, *out_truncated = 0 — on NULL text, cap 0, or
   out-of-memory. Refusals are LOUD at the call site (never a silent
   truncate-to-empty).
   Precondition: out_text and out_truncated are both non-NULL (a NULL
   out-param is a caller bug — the function returns without touching the
   other output). */
void budget_truncate_with_marker(const char* text, size_t cap,
                                 char** out_text, uint8_t* out_truncated);

#endif /* SA_BUDGET_H */