//
// Created by victor on 10/02/26.
//

#include "budget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ONE marker shape (the spec's pin): a newline + the cut's facts. */
#define _BUDGET_MARKER_FMT "\n[budget: truncated at %llu bytes]"

void budget_truncate_with_marker(const char* text, size_t cap,
                                 char** out_text, uint8_t* out_truncated) {
  if (out_text == NULL || out_truncated == NULL) return;
  *out_text = NULL;
  *out_truncated = 0;
  if (text == NULL || cap == 0) return;

  size_t n = strlen(text);
  if (n <= cap) {
    char* copy = strdup(text);
    if (copy != NULL) {
      *out_text = copy;   /* clean: no marker on fitting text */
    }
    return;
  }

  char marker[64];
  int m = snprintf(marker, sizeof(marker), _BUDGET_MARKER_FMT,
                   (unsigned long long)n);
  if (m < 0 || (size_t)m >= sizeof(marker)) {
    return;   /* the marker refused to compose: a refusal, never a bad shape */
  }
  char* out = (char*)malloc(cap + (size_t)m + 1);
  if (out == NULL) return;
  memcpy(out, text, cap);
  memcpy(out + cap, marker, (size_t)m);
  out[cap + (size_t)m] = '\0';
  *out_text = out;
  *out_truncated = 1;
}