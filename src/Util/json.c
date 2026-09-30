//
// Created by victor on 9/29/26.
//

#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "allocator.h"

/* ------------------------------------------------------------------ */
/* Contract notes (the choices this codec makes):                     */
/*                                                                    */
/* Depth cap: SA_JSON_MAX_DEPTH below. Container nesting deeper than  */
/* 16 levels is rejected with a one-line parse error.                 */
/*                                                                    */
/* Objects are INSERTION-ORDERED flat pair lists, not sorted: keys    */
/* are scanned linearly (fine at record scale) and round-trip output  */
/* preserves the order the record was built in. A later               */
/* json_object_set on an existing key REPLACES the value.             */
/*                                                                    */
/* Lone surrogates: \uD800..\uDFFF are NEVER surrogate-paired by this */
/* codec — the code point is encoded to UTF-8 directly (WTF-8 style). */
/* The record format never emits surrogate pairs, so we do not pay    */
/* for pair joining.                                                  */
/*                                                                    */
/* Integer literals that do not fit int64 fall back to double (may    */
/* lose precision); non-finite doubles serialize as 0.                */
/*                                                                    */
/* The header documents \uXXXX (BMP) as the extent of escape support. */
/*                                                                    */
/* This file never calls raw malloc/calloc for allocation — only      */
/* get_memory/get_clear_memory (which abort OOM). Freeing is plain    */
/* free(), matching what the header promises to callers.              */
/* ------------------------------------------------------------------ */

/* Maximum container nesting accepted while parsing. Event records   */
/* never go deeper than ~8; 16 leaves headroom at a memorable bound.  */
#define SA_JSON_MAX_DEPTH 16

typedef struct json_pair_t json_pair_t;

struct json_value_t {
  json_type_e type;
  union {
    int bool_;                        /* JSON_BOOL */
    int64_t int_;                     /* JSON_INT */
    double double_;                   /* JSON_DOUBLE */
  } as;
  char* string;                       /* JSON_STRING, NULL otherwise */
  json_pair_t* pairs;                 /* JSON_OBJECT */
  json_value_t** children;            /* JSON_ARRAY */
  size_t size;                        /* pair count / child count */
  size_t cap;                         /* allocated capacity */
};

struct json_pair_t {
  char* key;
  json_value_t* value;
};

static char* _json_heap_str(const char* s);

/* ------------------------------------------------------------------ */
/* Growable output buffer                                             */
/* ------------------------------------------------------------------ */

typedef struct _buffer_t {
  char* data;
  size_t len;
  size_t cap;
} _buffer_t;

static void _buffer_init(_buffer_t* b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static void _buffer_free(_buffer_t* b) {
  free(b->data);
  _buffer_init(b);
}

static void _buffer_reserve(_buffer_t* b, size_t extra) {
  size_t needed = b->len + extra;
  if (b->cap >= needed) return;
  if (b->cap == 0) b->cap = 64;
  while (b->cap < needed) b->cap *= 2;
  char* next = (char*)get_memory(b->cap);
  if (b->data != NULL) {
    memcpy(next, b->data, b->len);
    free(b->data);
  }
  b->data = next;
}

static void _buffer_put(_buffer_t* b, const char* s, size_t n) {
  _buffer_reserve(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
}

static void _buffer_put_char(_buffer_t* b, char c) {
  _buffer_reserve(b, 1);
  b->data[b->len] = c;
  b->len += 1;
}

static void _buffer_put_int(_buffer_t* b, int64_t i) {
  char tmp[24];
  int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)i);
  if (n > 0) _buffer_put(b, tmp, (size_t)n);
}

static void _buffer_put_double(_buffer_t* b, double d) {
  /* 17 significant digits round-trip every finite binary64 double when
     strtod is correctly rounded (C99, MSVC >= 2015). Type erasure: integral
     doubles and -0.0 re-parse as JSON_INT — json_as_int/_double bridge types. */
  char tmp[64];
  int n = snprintf(tmp, sizeof(tmp), "%.17g", d);
  if (n > 0) _buffer_put(b, tmp, (size_t)n);
}

static void _buffer_put_utf8(_buffer_t* b, uint32_t cp) {
  if (cp < 0x80) {
    _buffer_put_char(b, (char)cp);
  } else if (cp < 0x800) {
    _buffer_put_char(b, (char)(0xC0 | (cp >> 6)));
    _buffer_put_char(b, (char)(0x80 | (cp & 0x3F)));
  } else {
    _buffer_put_char(b, (char)(0xE0 | (cp >> 12)));
    _buffer_put_char(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    _buffer_put_char(b, (char)(0x80 | (cp & 0x3F)));
  }
}

static int _hex4(const char* p, uint32_t* out) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    uint32_t d;
    if (c >= '0' && c <= '9') {
      d = (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = (uint32_t)(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = (uint32_t)(c - 'A') + 10;
    } else {
      return 0;
    }
    v = (v << 4) | d;
  }
  *out = v;
  return 1;
}

/* ------------------------------------------------------------------ */
/* Container growth (grow = copy the array, never through values)     */
/* ------------------------------------------------------------------ */

static void _pair_array_put(json_pair_t** arr, size_t* count, size_t* cap,
                            json_pair_t pair) {
  if (*count == *cap) {
    size_t next_cap = (*cap == 0) ? 4 : *cap * 2;
    json_pair_t* next = (json_pair_t*)get_memory(next_cap * sizeof(json_pair_t));
    if (*count != 0) {
      memcpy(next, *arr, *count * sizeof(json_pair_t));
    }
    free(*arr);
    *arr = next;
    *cap = next_cap;
  }
  (*arr)[*count] = pair;
  *count += 1;
}

static void _value_array_put(json_value_t*** arr, size_t* count, size_t* cap,
                             json_value_t* v) {
  if (*count == *cap) {
    size_t next_cap = (*cap == 0) ? 4 : *cap * 2;
    json_value_t** next =
      (json_value_t**)get_memory(next_cap * sizeof(json_value_t*));
    if (*count != 0) {
      memcpy(next, *arr, *count * sizeof(json_value_t*));
    }
    free(*arr);
    *arr = next;
    *cap = next_cap;
  }
  (*arr)[*count] = v;
  *count += 1;
}

/* ------------------------------------------------------------------ */
/* Parser                                                             */
/* ------------------------------------------------------------------ */

typedef struct _parser_t {
  const char* text;
  size_t len;
  size_t pos;
  char* error;   /* heap, set on first failure */
} _parser_t;

static void _parser_fail(_parser_t* p, const char* reason) {
  if (p->error != NULL) return;   /* first error wins */
  int n = snprintf(NULL, 0, "json error at byte %zu: %s", p->pos, reason);
  if (n < 0) return;
  p->error = (char*)get_clear_memory((size_t)n + 1);
  snprintf(p->error, (size_t)n + 1, "json error at byte %zu: %s", p->pos, reason);
}

static void _skip_ws(_parser_t* p) {
  while (p->pos < p->len) {
    char c = p->text[p->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      p->pos += 1;
    } else {
      break;
    }
  }
}

static int _is_digit(char c) { return c >= '0' && c <= '9'; }

static json_value_t* _parse_value(_parser_t* p, int depth);

/* p->pos sits just past the opening quote. Consumes through the
   closing quote on success. */
static json_value_t* _parse_string_body(_parser_t* p) {
  _buffer_t out;
  _buffer_init(&out);
  while (p->pos < p->len) {
    char c = p->text[p->pos];
    if ((unsigned char)c < 0x20) {
      _parser_fail(p, "control character in string");
      break;
    }
    if (c == '"') {
      p->pos += 1;
      _buffer_put_char(&out, '\0');
      json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
      v->type = JSON_STRING;
      v->string = out.data;
      return v;
    }
    if (c != '\\') {
      p->pos += 1;
      _buffer_put_char(&out, c);
      continue;
    }
    /* Escape sequence */
    if (p->pos + 2 > p->len) {
      _parser_fail(p, "unterminated escape");
      break;
    }
    char esc = p->text[p->pos + 1];
    p->pos += 2;
    if (esc == '"') {
      _buffer_put_char(&out, '"');
    } else if (esc == '\\') {
      _buffer_put_char(&out, '\\');
    } else if (esc == '/') {
      _buffer_put_char(&out, '/');
    } else if (esc == 'b') {
      _buffer_put_char(&out, '\b');
    } else if (esc == 'f') {
      _buffer_put_char(&out, '\f');
    } else if (esc == 'n') {
      _buffer_put_char(&out, '\n');
    } else if (esc == 'r') {
      _buffer_put_char(&out, '\r');
    } else if (esc == 't') {
      _buffer_put_char(&out, '\t');
    } else if (esc == 'u') {
      uint32_t cp;
      if (p->pos + 4 > p->len) {
        _parser_fail(p, "truncated \\u escape");
        break;
      }
      if (!_hex4(p->text + p->pos, &cp)) {
        _parser_fail(p, "bad \\u escape");
        break;
      }
      p->pos += 4;
      _buffer_put_utf8(&out, cp);
    } else {
      _parser_fail(p, "unknown escape");
      break;
    }
  }
  if (p->error == NULL) {
    _parser_fail(p, "unterminated string");
  }
  _buffer_free(&out);
  return NULL;
}

static json_value_t* _parse_number(_parser_t* p) {
  size_t start = p->pos;
  if (p->pos < p->len && p->text[p->pos] == '-') p->pos += 1;
  size_t digits_start = p->pos;
  if (p->pos < p->len && p->text[p->pos] == '0') {
    p->pos += 1;
  } else {
    while (p->pos < p->len && _is_digit(p->text[p->pos])) p->pos += 1;
  }
  if (p->pos == digits_start) {
    _parser_fail(p, "expected value");
    return NULL;
  }
  int is_double = 0;
  if (p->pos < p->len && p->text[p->pos] == '.') {
    is_double = 1;
    p->pos += 1;
    size_t frac_start = p->pos;
    while (p->pos < p->len && _is_digit(p->text[p->pos])) p->pos += 1;
    if (p->pos == frac_start) {
      _parser_fail(p, "missing digits after decimal point");
      return NULL;
    }
  }
  if (p->pos < p->len && (p->text[p->pos] == 'e' || p->text[p->pos] == 'E')) {
    is_double = 1;
    p->pos += 1;
    if (p->pos < p->len && (p->text[p->pos] == '+' || p->text[p->pos] == '-')) {
      p->pos += 1;
    }
    size_t exp_start = p->pos;
    while (p->pos < p->len && _is_digit(p->text[p->pos])) p->pos += 1;
    if (p->pos == exp_start) {
      _parser_fail(p, "missing exponent digits");
      return NULL;
    }
  }
  size_t n = p->pos - start;
  char tmp[320];
  if (n >= sizeof(tmp)) {
    _parser_fail(p, "number too long");
    return NULL;
  }
  memcpy(tmp, p->text + start, n);
  tmp[n] = '\0';
  json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
  v->type = is_double ? JSON_DOUBLE : JSON_INT;
  if (is_double) {
    v->as.double_ = strtod(tmp, NULL);
    return v;
  }
  errno = 0;
  long long i = strtoll(tmp, NULL, 10);
  if (errno == ERANGE) {
    /* Documented fallback: out-of-range integer literal becomes a double. */
    v->type = JSON_DOUBLE;
    v->as.double_ = strtod(tmp, NULL);
  } else {
    v->as.int_ = i;
  }
  return v;
}

static json_value_t* _parse_object(_parser_t* p, int depth) {
  json_value_t* o = (json_value_t*)get_clear_memory(sizeof(json_value_t));
  o->type = JSON_OBJECT;
  _skip_ws(p);
  if (p->pos < p->len && p->text[p->pos] == '}') {
    p->pos += 1;
    return o;
  }
  while (1) {
    _skip_ws(p);
    if (p->pos >= p->len || p->text[p->pos] != '"') {
      _parser_fail(p, "expected object key");
      json_value_destroy(o);
      return NULL;
    }
    p->pos += 1;
    json_value_t* kv = _parse_string_body(p);
    if (kv == NULL) {
      json_value_destroy(o);
      return NULL;
    }
    char* key = (char*)get_memory(strlen(kv->string) + 1);
    strcpy(key, kv->string);
    json_value_destroy(kv);
    _skip_ws(p);
    if (p->pos >= p->len || p->text[p->pos] != ':') {
      _parser_fail(p, "expected ':' after object key");
      free(key);
      json_value_destroy(o);
      return NULL;
    }
    p->pos += 1;
    json_value_t* value = _parse_value(p, depth + 1);
    if (value == NULL) {
      free(key);
      json_value_destroy(o);
      return NULL;
    }
    json_pair_t pair = { key, value };
    _pair_array_put(&o->pairs, &o->size, &o->cap, pair);
    _skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == ',') {
      p->pos += 1;
      continue;
    }
    if (p->pos < p->len && p->text[p->pos] == '}') {
      p->pos += 1;
      return o;
    }
    _parser_fail(p, "expected ',' or '}' in object");
    json_value_destroy(o);
    return NULL;
  }
}

static json_value_t* _parse_array(_parser_t* p, int depth) {
  json_value_t* a = (json_value_t*)get_clear_memory(sizeof(json_value_t));
  a->type = JSON_ARRAY;
  _skip_ws(p);
  if (p->pos < p->len && p->text[p->pos] == ']') {
    p->pos += 1;
    return a;
  }
  while (1) {
    json_value_t* item = _parse_value(p, depth + 1);
    if (item == NULL) {
      json_value_destroy(a);
      return NULL;
    }
    _value_array_put(&a->children, &a->size, &a->cap, item);
    _skip_ws(p);
    if (p->pos < p->len && p->text[p->pos] == ',') {
      p->pos += 1;
      continue;
    }
    if (p->pos < p->len && p->text[p->pos] == ']') {
      p->pos += 1;
      return a;
    }
    _parser_fail(p, "expected ',' or ']' in array");
    json_value_destroy(a);
    return NULL;
  }
}

static json_value_t* _parse_value(_parser_t* p, int depth) {
  if (depth > SA_JSON_MAX_DEPTH) {
    _parser_fail(p, "max nesting depth (16) exceeded");
    return NULL;
  }
  _skip_ws(p);
  if (p->pos >= p->len) {
    _parser_fail(p, "unexpected end of input");
    return NULL;
  }
  char c = p->text[p->pos];
  if (c == '{') {
    p->pos += 1;
    return _parse_object(p, depth);
  }
  if (c == '[') {
    p->pos += 1;
    return _parse_array(p, depth);
  }
  if (c == '"') {
    p->pos += 1;
    return _parse_string_body(p);
  }
  if (c == 'n') {
    if (p->pos + 4 <= p->len && memcmp(p->text + p->pos, "null", 4) == 0) {
      p->pos += 4;
      return (json_value_t*)get_clear_memory(sizeof(json_value_t));
    }
    _parser_fail(p, "bad literal");
    return NULL;
  }
  if (c == 't') {
    if (p->pos + 4 <= p->len && memcmp(p->text + p->pos, "true", 4) == 0) {
      p->pos += 4;
      json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
      v->type = JSON_BOOL;
      v->as.bool_ = 1;
      return v;
    }
    _parser_fail(p, "bad literal");
    return NULL;
  }
  if (c == 'f') {
    if (p->pos + 5 <= p->len && memcmp(p->text + p->pos, "false", 5) == 0) {
      p->pos += 5;
      json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
      v->type = JSON_BOOL;
      return v;
    }
    _parser_fail(p, "bad literal");
    return NULL;
  }
  if (c == '-' || _is_digit(c)) {
    return _parse_number(p);
  }
  _parser_fail(p, "unexpected character");
  return NULL;
}

json_value_t* json_parse(const char* text, size_t len, char** error_msg) {
  if (error_msg != NULL) *error_msg = NULL;
  if (text == NULL) {
    if (error_msg != NULL) *error_msg = _json_heap_str("json error: input is NULL");
    return NULL;
  }
  _parser_t parser = { text, len, 0, NULL };
  json_value_t* v = _parse_value(&parser, 0);
  if (v != NULL) {
    _skip_ws(&parser);
    if (parser.pos != parser.len) {
      _parser_fail(&parser, "trailing characters after value");
      json_value_destroy(v);
      v = NULL;
    }
  }
  if (error_msg != NULL) *error_msg = parser.error;
  else free(parser.error);
  return v;
}

/* Small heap string for top-level errors (caller frees). */
static char* _json_heap_str(const char* s) {
  size_t n = strlen(s);
  char* out = (char*)get_memory(n + 1);
  memcpy(out, s, n + 1);
  return out;
}

/* ------------------------------------------------------------------ */
/* Serializer                                                         */
/* ------------------------------------------------------------------ */

/* Compact, minimal escaping: ", \ and control characters (short names
   \b \f \n \r \t, then \u00XX for the rest). Forward slash is NOT
   escaped. Output is compact — no whitespace. */
static void _serialize_string(const char* s, _buffer_t* b);

static void _serialize_value(const json_value_t* v, _buffer_t* b) {
  switch (v->type) {
    case JSON_NULL:
      _buffer_put(b, "null", 4);
      return;
    case JSON_BOOL:
      if (v->as.bool_) _buffer_put(b, "true", 4);
      else _buffer_put(b, "false", 5);
      return;
    case JSON_INT:
      _buffer_put_int(b, v->as.int_);
      return;
    case JSON_DOUBLE:
      if (v->as.double_ != v->as.double_ ||   /* NaN */
          v->as.double_ * 0.0 != 0.0) {       /* Inf */
        /* Documented fallback: non-finite doubles serialize as 0. */
        _buffer_put_char(b, '0');
        return;
      }
      _buffer_put_double(b, v->as.double_);
      return;
    case JSON_STRING:
      _serialize_string(v->string, b);
      return;
    case JSON_OBJECT:
      _buffer_put_char(b, '{');
      for (size_t i = 0; i < v->size; i++) {
        if (i != 0) _buffer_put_char(b, ',');
        _serialize_string(v->pairs[i].key, b);
        _buffer_put_char(b, ':');
        _serialize_value(v->pairs[i].value, b);
      }
      _buffer_put_char(b, '}');
      return;
    case JSON_ARRAY:
      _buffer_put_char(b, '[');
      for (size_t i = 0; i < v->size; i++) {
        if (i != 0) _buffer_put_char(b, ',');
        _serialize_value(v->children[i], b);
      }
      _buffer_put_char(b, ']');
      return;
  }
}

static void _serialize_string(const char* s, _buffer_t* b) {
  _buffer_put_char(b, '"');
  if (s != NULL) {
    for (const unsigned char* p = (const unsigned char*)s; *p != 0; p++) {
      unsigned char c = *p;
      if (c == '"') _buffer_put(b, "\\\"", 2);
      else if (c == '\\') _buffer_put(b, "\\\\", 2);
      else if (c == '\b') _buffer_put(b, "\\b", 2);
      else if (c == '\f') _buffer_put(b, "\\f", 2);
      else if (c == '\n') _buffer_put(b, "\\n", 2);
      else if (c == '\r') _buffer_put(b, "\\r", 2);
      else if (c == '\t') _buffer_put(b, "\\t", 2);
      else if (c < 0x20) {
        char esc[7] = { '\\', 'u', '0', '0', 0, 0, 0 };
        static const char hex[] = "0123456789abcdef";
        esc[4] = hex[(c >> 4) & 0xF];
        esc[5] = hex[c & 0xF];
        _buffer_put(b, esc, 6);
      } else {
        _buffer_put_char(b, (char)c);
      }
    }
  }
  _buffer_put_char(b, '"');
}

char* json_serialize(const json_value_t* value) {
  _buffer_t b;
  _buffer_init(&b);
  if (value != NULL) _serialize_value(value, &b);
  else _buffer_put(&b, "null", 4);
  _buffer_reserve(&b, 1);
  b.data[b.len] = '\0';
  return b.data;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */

json_type_e json_type(const json_value_t* v) {
  return v == NULL ? JSON_NULL : v->type;
}

int64_t json_as_int(const json_value_t* v) {
  if (v == NULL) return 0;
  if (v->type == JSON_INT) return v->as.int_;
  if (v->type == JSON_DOUBLE) return (int64_t)v->as.double_;
  return 0;
}

double json_as_double(const json_value_t* v) {
  if (v == NULL) return 0.0;
  if (v->type == JSON_DOUBLE) return v->as.double_;
  if (v->type == JSON_INT) return (double)v->as.int_;
  return 0.0;
}

int json_as_bool(const json_value_t* v) {
  if (v == NULL || v->type != JSON_BOOL) return 0;
  return v->as.bool_;
}

const char* json_as_string(const json_value_t* v) {
  if (v == NULL || v->type != JSON_STRING) return NULL;
  return v->string;
}

size_t json_size(const json_value_t* v) {
  if (v == NULL || (v->type != JSON_OBJECT && v->type != JSON_ARRAY)) return 0;
  return v->size;
}

json_value_t* json_get(const json_value_t* obj, const char* key) {
  if (obj == NULL || obj->type != JSON_OBJECT || key == NULL) return NULL;
  for (size_t i = 0; i < obj->size; i++) {
    if (strcmp(obj->pairs[i].key, key) == 0) return obj->pairs[i].value;
  }
  return NULL;
}

json_value_t* json_at(const json_value_t* array, size_t index) {
  if (array == NULL || array->type != JSON_ARRAY || index >= array->size) {
    return NULL;
  }
  return array->children[index];
}

/* ------------------------------------------------------------------ */
/* Construction                                                       */
/* ------------------------------------------------------------------ */

json_value_t* json_new_null(void) {
  return (json_value_t*)get_clear_memory(sizeof(json_value_t));
}

json_value_t* json_new_bool(int b) {
  json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
  v->type = JSON_BOOL;
  v->as.bool_ = b ? 1 : 0;
  return v;
}

json_value_t* json_new_int(int64_t i) {
  json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
  v->type = JSON_INT;
  v->as.int_ = i;
  return v;
}

json_value_t* json_new_double(double d) {
  json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
  v->type = JSON_DOUBLE;
  v->as.double_ = d;
  return v;
}

json_value_t* json_new_string(const char* s) {
  json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
  v->type = JSON_STRING;
  if (s != NULL) {
    size_t n = strlen(s);
    v->string = (char*)get_memory(n + 1);
    memcpy(v->string, s, n + 1);
  }
  return v;
}

json_value_t* json_new_object(void) {
  json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
  v->type = JSON_OBJECT;
  return v;
}

json_value_t* json_new_array(void) {
  json_value_t* v = (json_value_t*)get_clear_memory(sizeof(json_value_t));
  v->type = JSON_ARRAY;
  return v;
}

/* Returns 0 WITHOUT taking ownership on failure — caller still owns *value. */
int json_object_set(json_value_t* obj, const char* key, json_value_t* value) {
  if (obj == NULL || obj->type != JSON_OBJECT || key == NULL || value == NULL) {
    return 0;
  }
  for (size_t i = 0; i < obj->size; i++) {
    if (strcmp(obj->pairs[i].key, key) == 0) {
      json_value_destroy(obj->pairs[i].value);
      obj->pairs[i].value = value;
      return 1;
    }
  }
  size_t n = strlen(key);
  char* copy = (char*)get_memory(n + 1);
  memcpy(copy, key, n + 1);
  json_pair_t pair = { copy, value };
  _pair_array_put(&obj->pairs, &obj->size, &obj->cap, pair);
  return 1;
}

int json_array_append(json_value_t* array, json_value_t* value) {
  if (array == NULL || array->type != JSON_ARRAY || value == NULL) return 0;
  _value_array_put(&array->children, &array->size, &array->cap, value);
  return 1;
}

void json_value_destroy(json_value_t* v) {
  if (v == NULL) return;
  free(v->string);
  if (v->type == JSON_OBJECT) {
    for (size_t i = 0; i < v->size; i++) {
      free(v->pairs[i].key);
      json_value_destroy(v->pairs[i].value);
    }
    free(v->pairs);
  } else if (v->type == JSON_ARRAY) {
    for (size_t i = 0; i < v->size; i++) {
      json_value_destroy(v->children[i]);
    }
    free(v->children);
  }
  free(v);
}