//
// Created by victor on 10/6/26.
//

#include "persona.h"

#include "../Util/allocator.h"
#include "../Util/json.h"
#include "../Util/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* persona.c — the persona slice's PURE half (spec §1-§2): the voice
   record's parse + field validation (the refusals are loud, one line each),
   the falsifiability meta-rule as data (every principle's text must appear
   in some test_spec entry), and the compose (the persona group + the
   context block + the tool-conditional guidance + the base's position by
   placement + the placeholder catalog). A pure data unit in the lifecycle
   module's discipline: no store, no model, no actor — refuse-loud on the
   record-shape rules, render-not-crash on the author's typos, and
   get_memory/get_clear_memory for every allocation. */

/* ------------------------------------------------------------------ */
/* Small heap-string + growable-buffer helpers                         */
/* ------------------------------------------------------------------ */

static char* _p_dup(const char* s) {
  if (s == NULL) return NULL;
  size_t n = strlen(s);
  char* copy = get_memory(n + 1);
  memcpy(copy, s, n + 1);
  return copy;
}

typedef struct _p_buf_t {
  char* text;
  size_t len, capacity;
} _p_buf_t;

static void _p_buf_addn(_p_buf_t* buf, const char* text, size_t n) {
  if (text == NULL) return;
  size_t need = buf->len + n + 1;
  if (need > buf->capacity) {
    size_t cap = buf->capacity == 0 ? 256 : buf->capacity;
    while (cap < need) cap *= 2;
    char* grown = get_memory(cap);
    if (buf->text != NULL) memcpy(grown, buf->text, buf->len + 1);
    free(buf->text);
    buf->text = grown;
    buf->capacity = cap;
  }
  memcpy(buf->text + buf->len, text, n);
  buf->text[buf->len + n] = '\0';
  buf->len += n;
}

static void _p_buf_add(_p_buf_t* buf, const char* text) {
  if (text == NULL) return;
  _p_buf_addn(buf, text, strlen(text));
}

/* A block appended into a composed body: empty parts render absent, the
   non-empty ones join the running body with a BLANK line (the "\n\n"
   part separator — the compose's pinned shape). */
static void _p_buf_add_block(_p_buf_t* buf, const char* block) {
  if (block == NULL || block[0] == '\0') return;
  if (buf->len > 0) _p_buf_add(buf, "\n\n");
  _p_buf_add(buf, block);
}

/* A string field from a JSON object: the key absent or not a string means
   "not set" (the caller decides whether that refuses). */
static const char* _p_field_str(json_value_t* obj, const char* key) {
  json_value_t* v = json_get(obj, key);
  if (v == NULL || json_type(v) != JSON_STRING) return NULL;
  return json_as_string(v);
}

/* ------------------------------------------------------------------ */
/* The load (spec §1): the field refusals + the deep copy              */
/* ------------------------------------------------------------------ */

/* One REQUIRED string field: 0 = ok (*out_ = heap dup), -1 = the field is
   absent, not a string, or empty (the caller's log line names the rule). */
static int _p_required_string(json_value_t* obj, const char* key,
                              char** out_) {
  const char* v = _p_field_str(obj, key);
  if (v == NULL || v[0] == '\0') return -1;
  *out_ = _p_dup(v);
  return 0;
}

/* One OPTIONAL string array ("principles"/"test_spec"): absent = {NULL,0}
   and OK; present = every element must be a string (one bad element
   refuses the whole load — rc -1 + the caller's loud line). */
static int _p_string_array(json_value_t* obj, const char* key,
                           const char* what, char*** out_, size_t* n_) {
  *out_ = NULL;
  *n_ = 0;
  json_value_t* v = json_get(obj, key);
  if (v == NULL) return 0;
  if (json_type(v) != JSON_ARRAY) {
    log_error("persona_record_load: the %s field is not an array — refused "
              "loud", what);
    return -1;
  }
  size_t n = json_size(v);
  if (n == 0) return 0;
  char** items = get_clear_memory(n * sizeof(char*));
  for (size_t i = 0; i < n; i++) {
    json_value_t* e = json_at(v, i);
    if (e == NULL || json_type(e) != JSON_STRING) {
      log_error("persona_record_load: %s element %zu is not a string — "
                "refused loud", what, i);
      for (size_t k = 0; k < n; k++) free(items[k]);
      free(items);
      return -1;
    }
    items[i] = _p_dup(json_as_string(e));
  }
  *out_ = items;
  *n_ = n;
  return 0;
}

/* The guidance array: each element one object carrying non-empty string
   "key" AND "text" (absent field = a bad element — the section would be
   unattachable or unrenderable). */
static int _p_guidance_array(json_value_t* obj,
                             struct persona_guidance_t** out_, size_t* n_) {
  *out_ = NULL;
  *n_ = 0;
  json_value_t* v = json_get(obj, "guidance");
  if (v == NULL) return 0;
  if (json_type(v) != JSON_ARRAY) {
    log_error("persona_record_load: the guidance field is not an array — "
              "refused loud");
    return -1;
  }
  size_t n = json_size(v);
  if (n == 0) return 0;
  struct persona_guidance_t* items =
      get_clear_memory(n * sizeof(struct persona_guidance_t));
  for (size_t i = 0; i < n; i++) {
    json_value_t* e = json_at(v, i);
    const char* key = (e != NULL) ? _p_field_str(e, "key") : NULL;
    const char* text = (e != NULL) ? _p_field_str(e, "text") : NULL;
    if (key == NULL || key[0] == '\0' || text == NULL || text[0] == '\0') {
      log_error("persona_record_load: guidance element %zu carries no "
                "non-empty key AND text pair — refused loud", i);
      for (size_t k = 0; k < n; k++) {
        free(items[k].key);
        free(items[k].text);
      }
      free(items);
      return -1;
    }
    items[i].key = _p_dup(key);
    items[i].text = _p_dup(text);
  }
  *out_ = items;
  *n_ = n;
  return 0;
}

/* The falsifiability refusal line, pinned (the meta-rule's shape). */
static void _p_err_no_checkable_test(const char* principle, char** err_out) {
  if (err_out == NULL) return;
  size_t n = strlen(principle);
  size_t cap = strlen("principle '' has no checkable test — the meta-rule") +
               n + 1;
  char* err = get_memory(cap);
  snprintf(err, cap, "principle '%s' has no checkable test — the meta-rule",
           principle);
  *err_out = err;
}

int persona_validate_falsifiable(const persona_record_t* record,
                                 char** err_out) {
  if (err_out != NULL) *err_out = NULL;
  if (record == NULL) {
    log_error("persona_validate_falsifiable: NULL record — refused loud");
    return -1;
  }
  /* The optional list absent = the meta-rule never fires (spec §1: the
     principles are optional; the hammer's authored record runs the check). */
  for (size_t i = 0; i < record->nprinciples; i++) {
    const char* principle = record->principles[i];
    if (principle == NULL) continue;
    int checkable = 0;
    for (size_t j = 0; j < record->ntest_spec; j++) {
      if (record->test_spec[j] != NULL &&
          strstr(record->test_spec[j], principle) != NULL) {
        checkable = 1;
        break;
      }
    }
    if (!checkable) {
      _p_err_no_checkable_test(principle, err_out);
      return -1;
    }
  }
  return 0;
}

int persona_record_load(const char* record_json, persona_record_t** out) {
  if (out != NULL) *out = NULL;
  if (record_json == NULL || out == NULL) {
    log_error("persona_record_load: NULL input — refused loud");
    return -1;
  }

  char* jerr = NULL;
  json_value_t* root = json_parse(record_json, strlen(record_json), &jerr);
  if (root == NULL) {
    log_error("persona_record_load: the record is not a JSON document (%s) "
              "— refused loud", jerr != NULL ? jerr : "malformed");
    free(jerr);
    return -1;
  }
  if (json_type(root) != JSON_OBJECT) {
    log_error("persona_record_load: the record is not a JSON object — "
              "refused loud");
    json_value_destroy(root);
    return -1;
  }

  json_value_t* v = json_get(root, "version");
  if (v == NULL || json_type(v) != JSON_INT) {
    log_error("persona_record_load: the record carries no int version — "
              "refused loud");
    json_value_destroy(root);
    return -1;
  }
  if (json_as_int(v) != 1) {
    log_error("persona_record_load: the record's version is %lld, not 1 — "
              "refused loud", (long long) json_as_int(v));
    json_value_destroy(root);
    return -1;
  }

  persona_record_t* record = get_clear_memory(sizeof(*record));
  record->version = (unsigned) json_as_int(v);   /* the check above: == 1 */
  int failed = 0;
  if (_p_required_string(root, "name", &record->name) != 0) {
    log_error("persona_record_load: the record carries no non-empty name — "
              "refused loud");
    failed = 1;
  }
  if (_p_required_string(root, "text", &record->text) != 0) {
    log_error("persona_record_load: the record carries no non-empty text — "
              "refused loud");
    failed = 1;
  }
  const char* placement = _p_field_str(root, "placement");
  if (placement == NULL || (strcmp(placement, "first") != 0 &&
                            strcmp(placement, "below") != 0)) {
    log_error("persona_record_load: the placement is not \"first\" nor "
              "\"below\" — refused loud");
    failed = 1;
  } else {
    record->placement = _p_dup(placement);
  }
  if (_p_guidance_array(root, &record->guidance, &record->nguidance) != 0) {
    failed = 1;
  }
  if (_p_string_array(root, "principles", "principles", &record->principles,
                      &record->nprinciples) != 0) {
    failed = 1;
  }
  if (_p_string_array(root, "test_spec", "test_spec", &record->test_spec,
                      &record->ntest_spec) != 0) {
    failed = 1;
  }

  if (!failed) {
    /* The datetime WARNING (spec §2): the token's presence means the
       record composes with the render-time stamp — the cache-stable
       prefix opts out, the load says so loud. NOT a refusal. */
    if (strstr(record->text, "{CURRENT_DATETIME}") != NULL) {
      log_warn("persona_record_load: the persona \"%s\"'s text carries "
               "{CURRENT_DATETIME} — its compose stamps the render time and "
               "opts the record out of the cache-stable prefix", record->name);
    }
    /* The falsifiability meta-rule at load (spec §1): the validator's err
       line becomes the loud refusal's line. */
    char* err = NULL;
    if (persona_validate_falsifiable(record, &err) != 0) {
      log_error("persona_record_load: %s", err != NULL ? err
                                                       : "not falsifiable");
      free(err);
      failed = 1;
    }
  }

  json_value_destroy(root);
  if (failed) {
    persona_record_destroy(record);
    return -1;
  }
  *out = record;
  return 0;
}

void persona_record_destroy(persona_record_t* record) {
  if (record == NULL) return;
  free(record->name);
  free(record->text);
  free(record->placement);
  for (size_t i = 0; i < record->nguidance; i++) {
    free(record->guidance[i].key);
    free(record->guidance[i].text);
  }
  free(record->guidance);
  for (size_t i = 0; i < record->nprinciples; i++) {
    free(record->principles[i]);
  }
  free(record->principles);
  for (size_t i = 0; i < record->ntest_spec; i++) {
    free(record->test_spec[i]);
  }
  free(record->test_spec);
  free(record);
}

/* ------------------------------------------------------------------ */
/* The compose (spec §2): the group, the placement, the placeholders   */
/* ------------------------------------------------------------------ */

/* The UTC ISO-8601 wall clock for {CURRENT_DATETIME} (the frame event
   records' "at" field's very shape — one stamp format, one truth). */
static void _p_stamp_now(char out[25]) {
  time_t now = 0;
  time(&now);
  struct tm tmv;
#ifdef _WIN32
  gmtime_s(&tmv, &now);
#else
  gmtime_r(&now, &tmv);
#endif
  strftime(out, 25, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

/* The context object's sorted-key render ("key: value" lines, the keys'
   sort order the byte-stability pin). Borrowed key pointers + owned
   serialized value strings. */
typedef struct _p_pair_t {
  const char* key;
  json_value_t* value;
} _p_pair_t;

static int _p_pair_cmp(const void* a, const void* b) {
  return strcmp(((_p_pair_t*) a)->key, ((_p_pair_t*) b)->key);
}

static void _p_render_context_object(_p_buf_t* block, json_value_t* obj) {
  size_t n = json_size(obj);
  _p_pair_t* pairs = get_clear_memory(n * sizeof(_p_pair_t));
  for (size_t i = 0; i < n; i++) {
    pairs[i].key = json_key_at(obj, i);
    pairs[i].value = json_value_at(obj, i);
  }
  qsort(pairs, n, sizeof(_p_pair_t), _p_pair_cmp);
  for (size_t i = 0; i < n; i++) {
    if (i > 0) _p_buf_add(block, "\n");
    _p_buf_add(block, pairs[i].key);
    _p_buf_add(block, ": ");
    if (pairs[i].value != NULL && json_type(pairs[i].value) == JSON_STRING) {
      _p_buf_add(block, json_as_string(pairs[i].value));
    } else {
      /* A non-string field values itself in compact JSON (the render-
         not-crash rule: any JSON value the record carries is usable). */
      char* serialized = json_serialize(pairs[i].value);
      _p_buf_add(block, serialized != NULL ? serialized : "(unserializable)");
      free(serialized);
    }
  }
  free(pairs);
}

/* The context block's text: a JSON object renders sorted-key "key: value"
   lines; anything else is the plain text VERBATIM. Returns a heap string
   the caller frees, or NULL when the context is NULL/empty (no block). */
static char* _p_render_context(const char* context, json_value_t** obj_out) {
  *obj_out = NULL;
  if (context == NULL || context[0] == '\0') return NULL;

  char* jerr = NULL;
  json_value_t* parsed = json_parse(context, strlen(context), &jerr);
  free(jerr);
  if (parsed != NULL && json_type(parsed) == JSON_OBJECT) {
    _p_buf_t block = {NULL, 0, 0};
    _p_render_context_object(&block, parsed);
    *obj_out = parsed;   /* alive for the placeholder pass: {USER_*} reads it */
    return block.text == NULL ? _p_dup("") : block.text;
  }
  /* A non-object document (or plain text) renders verbatim; the parsed
     value is irrelevant to the substitution pass. */
  if (parsed != NULL) json_value_destroy(parsed);
  return _p_dup(context);
}

/* Does the tool surface carry this id? (The guidance's attach gate.) */
static int _p_tool_present(const char* const* tools, size_t ntools,
                           const char* key) {
  for (size_t i = 0; i < ntools; i++) {
    if (tools[i] != NULL && strcmp(tools[i], key) == 0) return 1;
  }
  return 0;
}

/* The {USER_…} lookup: the token's remainder lowercased is the context
   object's field (BORROWED — json_get reads only); string values verbatim,
   other values compact JSON. A recognized token that resolves to nothing
   substitutes "" (never a raw token leak). `key` = the lowercased
   remainder (owned here). */
static void _p_substitute_user(_p_buf_t* out, const char* key,
                               json_value_t* ctx_obj) {
  json_value_t* v = json_get(ctx_obj, key);
  if (v != NULL && json_type(v) == JSON_STRING) {
    _p_buf_add(out, json_as_string(v));
    return;
  }
  if (v != NULL) {
    char* serialized = json_serialize(v);
    _p_buf_add(out, serialized != NULL ? serialized : "");
    free(serialized);
    return;
  }
  /* recognized-but-missing: "" */
}

/* One substitution pass over the assembled prompt (spec §2 item 3). The
   catalog: {CURRENT_DATETIME} → the render stamp (ONE stamp per compose —
   the byte-stability opt-out); {USER_<REST>} → the context object's
   "<rest lowercased>" field ("" when missing); any other {TOKEN} left
   VISIBLE verbatim. An unrecognized '{' stays in place and the scan
   continues just inside it, so plain-brace text round-trips untouched. */
static char* _p_substitute(const char* in, json_value_t* ctx_obj) {
  _p_buf_t out = {NULL, 0, 0};
  char stamp[25];
  int have_stamp = 0;

  size_t len = strlen(in);
  size_t i = 0;
  while (i < len) {
    const char* brace = memchr(in + i, '{', len - i);
    if (brace == NULL) {
      _p_buf_addn(&out, in + i, len - i);
      break;
    }
    if (brace > in + i) {
      _p_buf_addn(&out, in + i, (size_t) (brace - (in + i)));
    }
    i = (size_t) (brace - in);

    /* `i` now sits ON a '{'. Read the token up to the next '}'; a token
       with no closing brace to the end is the author's plain text —
       verbatim (render-not-crash). */
    const char* close = memchr(in + i, '}', len - i);
    if (close == NULL) {
      _p_buf_addn(&out, in + i, len - i);
      break;
    }
    size_t tlen = (size_t) (close - (in + i + 1));
    const char* tok = in + i + 1;
    if (tlen == 16 && memcmp(tok, "CURRENT_DATETIME", 16) == 0) {
      if (!have_stamp) {
        _p_stamp_now(stamp);
        have_stamp = 1;
      }
      _p_buf_add(&out, stamp);
      i = (size_t) (close - in) + 1;
      continue;
    }
    if (tlen > 5 && memcmp(tok, "USER_", 5) == 0) {
      char* key = get_memory(tlen - 5 + 1);
      for (size_t k = 5; k < tlen; k++) {
        key[k - 5] = (char) tolower((unsigned char) tok[k]);
      }
      key[tlen - 5] = '\0';
      _p_substitute_user(&out, key, ctx_obj);
      free(key);
      i = (size_t) (close - in) + 1;
      continue;
    }
    /* Unrecognized: '{' stays visible, the scan continues INSIDE it — the
       token's own bytes replay char for char (the typo stays readable). */
    _p_buf_add(&out, "{");
    i++;
  }

  if (out.text == NULL) return _p_dup("");
  return out.text;
}

char* persona_compose(const persona_record_t* record,
                      const char* context_json_or_text,
                      const char* const* tools, size_t ntools,
                      const char* base) {
  /* The no-persona path: the base AS-IS (a heap copy). */
  if (record == NULL) {
    if (base == NULL) {
      log_error("persona_compose: NULL record and NULL base — refused loud");
      return NULL;
    }
    return _p_dup(base);
  }

  /* 1. The persona GROUP: the record's text + the context block + the
        attached guidance entries, blank-line joined (the non-empty parts). */
  _p_buf_t group = {NULL, 0, 0};
  _p_buf_add_block(&group, record->text);

  json_value_t* ctx_obj = NULL;
  char* ctx_render = _p_render_context(context_json_or_text, &ctx_obj);
  _p_buf_add_block(&group, ctx_render);
  free(ctx_render);      /* the render's bytes joined; {USER_*} reads ctx_obj */

  for (size_t i = 0; i < record->nguidance; i++) {
    if (!_p_tool_present(tools, ntools, record->guidance[i].key)) continue;
    /* The attach order IS the record's list order (the determinism pin);
       the section shape: "## <key>" heading + the text body. */
    if (group.len > 0) _p_buf_add(&group, "\n\n");
    _p_buf_add(&group, "## ");
    _p_buf_add(&group, record->guidance[i].key);
    _p_buf_add(&group, "\n");
    _p_buf_add(&group, record->guidance[i].text);
  }

  /* 2. The base rides the placement: the group moves as ONE body. */
  _p_buf_t before = {NULL, 0, 0};
  const char* placement = record->placement;
  if (placement != NULL && strcmp(placement, "below") == 0) {
    _p_buf_add_block(&before, base);
    _p_buf_add_block(&before, group.text);
  } else {
    /* "first" — and any hand-built placement reads as the block-1 shape. */
    _p_buf_add_block(&before, group.text);
    _p_buf_add_block(&before, base);
  }
  free(group.text);

  /* 3. The placeholder catalog, one pass over the ASSEMBLED string. */
  char* out = _p_substitute(before.text != NULL ? before.text : "", ctx_obj);
  free(before.text);
  if (ctx_obj != NULL) json_value_destroy(ctx_obj);
  return out;
}