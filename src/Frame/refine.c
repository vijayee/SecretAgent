//
// Created by victor on 10/1/26.
//

#include "refine.h"

#ifdef SA_HAS_WDB

#include "frame_internal.h"   /* the backend fetch + the refine slice's
                                 _frame_sync_scan round-trip helpers */
#include "../Util/allocator.h"
#include "../Util/log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* refine.c — the refine slice's DATA half (the orchestrator spec: the fold,
   the edit validation, the evidence gate, the apply semantics with version
   guards, the record's edit-element composition, and the fold's canonical
   views — the FNV-1a-64 fingerprint and the bounded digest render) plus the
   review pass (spec §3 step 4): the no-tools backend call over the composed
   prompt and the JSON proposal decode under the output-cap guard — decode
   ONLY (the store-riding runners refine_run / refine_rollback land with
   their own tasks; the store actor remains the only serializer this module
   ever answers to, and the review performs no store WRITE at all). */

const char* const REFINE_KINDS[REFINE_KINDS_COUNT] = {
    "prompt", "memory", "skill", "subagent",
};
const char REFINE_ID_BASE_PROMPT[] = "base_system_prompt";

/* ------------------------------------------------------------------ */
/* Small heap-string + refusal helpers                                 */
/* ------------------------------------------------------------------ */

static char* _refine_dup(const char* s) {
  if (s == NULL) return NULL;
  size_t n = strlen(s);
  char* copy = get_memory(n + 1);
  memcpy(copy, s, n + 1);
  return copy;
}

/* A malloc'd one-line refusal string (the validation contract's return
   type: NULL = valid, malloc'd text = refused). */
static char* _refine_error(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  size_t len = needed > 0 ? (size_t) needed : 1;
  char* text = get_memory(len + 1);
  va_start(args, fmt);
  vsnprintf(text, len + 1, fmt, args);
  va_end(args);
  return text;
}

/* PA's compactText (refinement.ts:504-511): whitespace runs collapse to one
   space, ends trimmed, long text ellipsized to max_chars. Malloc'd. */
static char* _refine_compact(const char* text, size_t max_chars) {
  size_t n = text != NULL ? strlen(text) : 0;
  char* normalized = get_memory(n + 1);
  size_t w = 0;
  int pending_space = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char) text[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      if (w > 0) pending_space = 1;
      continue;
    }
    if (pending_space) normalized[w++] = ' ';
    pending_space = 0;
    normalized[w++] = (char) c;
  }
  /* A pending flag never survives to here unless the text ended in blank
     runs — nothing is flushed, so no trailing space can be stored. */
  if (w > max_chars && max_chars >= 3) {
    strcpy(normalized + max_chars - 3, "...");
  } else {
    normalized[w] = '\0';
  }
  return normalized;
}

/* The slug port (refinement.ts:396-405): trim, lowercase, non-ASCII-
   alphanumerics collapse to '_', edges trimmed, capped at 80, falling back
   to the kind when nothing survives. Malloc'd. Locale-independent by
   design (explicit ASCII classes — a slug is a store path component). */
static char* _refine_slug(const char* raw, const char* fallback) {
  const char* src = (raw != NULL) ? raw : fallback;
  size_t n = strlen(src);
  char* out = get_memory(n + 1);
  size_t w = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char) src[i];
    char mapped;
    if (c >= 'a' && c <= 'z') {
      mapped = (char) c;
    } else if (c >= 'A' && c <= 'Z') {
      mapped = (char) (c - 'A' + 'a');
    } else if (c >= '0' && c <= '9') {
      mapped = (char) c;
    } else {
      if (w == 0 || out[w - 1] == '_') continue;   /* edge + run collapse */
      out[w++] = '_';
      continue;
    }
    out[w++] = mapped;
  }
  while (w > 0 && out[w - 1] == '_') w--;          /* trailing trim (pre-slice,
                                                      refinement.ts:272) */
  out[w] = '\0';
  if (w > 80) w = 80;                              /* .slice(0, 80) */
  out[w] = '\0';
  if (w == 0) {
    free(out);
    return _refine_dup(fallback);
  }
  return out;
}

/* ------------------------------------------------------------------ */
/* The fold: entry + record-line storage                               */
/* ------------------------------------------------------------------ */

static void _entry_free_strings(refine_entry_t* e) {
  free(e->kind); e->kind = NULL;
  free(e->id); e->id = NULL;
  free(e->title); e->title = NULL;
  free(e->content); e->content = NULL;
  free(e->path); e->path = NULL;
  free(e->reference); e->reference = NULL;
  free(e->arguments); e->arguments = NULL;
}

/* Overwrites the row from borrowed strings (the fold owns the copies).
   Strings NOT provided as the entry's defaults are the caller's business —
   every field here is copied verbatim. */
static void _entry_set(refine_entry_t* e, const char* kind, const char* id,
                       const char* title, const char* content, const char* path,
                       const char* reference, const char* arguments,
                       unsigned version, uint8_t deleted, uint64_t seq) {
  free(e->kind); e->kind = _refine_dup(kind);
  free(e->id); e->id = _refine_dup(id);
  if (title != NULL) { free(e->title); e->title = _refine_dup(title); }
  if (content != NULL) { free(e->content); e->content = _refine_dup(content); }
  if (path != NULL) { free(e->path); e->path = _refine_dup(path); }
  if (reference != NULL) { free(e->reference); e->reference = _refine_dup(reference); }
  if (arguments != NULL) { free(e->arguments); e->arguments = _refine_dup(arguments); }
  e->version = version;
  e->deleted = deleted;
  e->seq = seq;
}

/* Grows the entries array to hold one more row and returns the free slot. */
static refine_entry_t* _fold_grow(refine_fold_t* fold) {
  if (fold->nentries == fold->capacity) {
    size_t cap = fold->capacity == 0 ? 8 : fold->capacity * 2;
    refine_entry_t* entries = get_clear_memory(cap * sizeof(refine_entry_t));
    if (fold->entries != NULL) {
      memcpy(entries, fold->entries, fold->nentries * sizeof(refine_entry_t));
      free(fold->entries);
    }
    fold->entries = entries;
    fold->capacity = cap;
  }
  refine_entry_t* slot = &fold->entries[fold->nentries++];
  memset(slot, 0, sizeof(*slot));
  return slot;
}

/* Any row (live OR tombstone) for (kind,id) — the put path reuses rows; the
   live-refusing paths use refine_entry_find. */
static refine_entry_t* _row_find(const refine_fold_t* fold, const char* kind,
                                 const char* id) {
  for (size_t i = 0; i < fold->nentries; i++) {
    refine_entry_t* e = &fold->entries[i];
    if (strcmp(e->kind, kind) == 0 && strcmp(e->id, id) == 0) return e;
  }
  return NULL;
}

refine_entry_t* refine_entry_put(refine_fold_t* fold, const char* kind,
                                 const char* id, const char* title,
                                 const char* content, const char* path,
                                 const char* reference, const char* arguments,
                                 unsigned version, uint8_t deleted,
                                 uint64_t seq) {
  if (fold == NULL || kind == NULL || id == NULL) return NULL;
  refine_entry_t* row = _row_find(fold, kind, id);
  if (row == NULL) row = _fold_grow(fold);
  _entry_set(row, kind, id, title, content, path, reference, arguments,
             version, deleted, seq);
  return row;
}

refine_entry_t* refine_entry_find(const refine_fold_t* fold, const char* kind,
                                  const char* id) {
  if (fold == NULL || kind == NULL || id == NULL) return NULL;
  for (size_t i = 0; i < fold->nentries; i++) {
    refine_entry_t* e = &fold->entries[i];
    if (e->deleted) continue;   /* a tombstone is not a live entry */
    if (strcmp(e->kind, kind) == 0 && strcmp(e->id, id) == 0) return e;
  }
  return NULL;
}

static void _record_line_put(refine_fold_t* fold, const char* line) {
  if (fold->nrecords == fold->rcapacity) {
    size_t cap = fold->rcapacity == 0 ? 8 : fold->rcapacity * 2;
    char** lines = get_clear_memory(cap * sizeof(char*));
    if (fold->record_lines != NULL) {
      memcpy(lines, fold->record_lines, fold->nrecords * sizeof(char*));
      free(fold->record_lines);
    }
    fold->record_lines = lines;
    fold->rcapacity = cap;
  }
  fold->record_lines[fold->nrecords++] = _refine_dup(line);
}

/* The malformed record's skip line: a bounded diagnostic, labeled by TYPE,
   never by value (refinement.ts:493-505's rule ported to the record texts —
   a corrupt store element must not inject arbitrary unbounded text into
   every session's digest). */
static void _fold_push_skip_line(refine_fold_t* fold, const char* reason) {
  char line[200];
  snprintf(line, sizeof(line),
           "- harness: skipped malformed refinement record (%.120s)", reason);
  _record_line_put(fold, line);
}

void refine_fold_destroy(refine_fold_t* fold) {
  if (fold == NULL) return;
  for (size_t i = 0; i < fold->nentries; i++) _entry_free_strings(&fold->entries[i]);
  free(fold->entries);
  fold->entries = NULL;
  fold->nentries = 0;
  fold->capacity = 0;
  for (size_t i = 0; i < fold->nrecords; i++) free(fold->record_lines[i]);
  free(fold->record_lines);
  fold->record_lines = NULL;
  fold->nrecords = 0;
  fold->rcapacity = 0;
}

/* ------------------------------------------------------------------ */
/* The canonical views (the FNV-1a-64 fingerprint + the bounded digest */
/* render; the delivery gate compares fingerprints at turn commit)     */
/* ------------------------------------------------------------------ */

/* The fingerprint's salt version string — bump on ANY change to the
   material or its serialization (refinement.ts:25-30's rule): fingerprints
   minted under different versions never compare equal, so a changed render
   contract can never reuse an old digest's state. */
#define REFINE_FINGERPRINT_SALT "refine-fingerprint-v1;"
#define REFINE_FNV64_BASIS 0xcbf29ce484222325ULL
#define REFINE_FNV64_PRIME 0x100000001b3ULL

/* FNV-1a-64 over one byte run — the rolling hash update. */
static uint64_t _refine_fnv64(uint64_t hash, const void* bytes, size_t len) {
  const unsigned char* p = (const unsigned char*) bytes;
  for (size_t i = 0; i < len; i++) {
    hash ^= (uint64_t) p[i];
    hash *= REFINE_FNV64_PRIME;
  }
  return hash;
}

static uint64_t _refine_hash_string(uint64_t hash, const char* s) {
  return _refine_fnv64(hash, s, s != NULL ? strlen(s) : 0);
}

/* A malloc'd view of the fold's LIVE rows (tombstones excluded — they
   render nothing, so they materialize nothing). Caller frees the array,
   never the rows it points at. */
static const refine_entry_t** _fold_live_rows(const refine_fold_t* fold,
                                              size_t* count) {
  const refine_entry_t** rows =
      get_memory((fold->nentries != 0 ? fold->nentries : 1) * sizeof(*rows));
  size_t n = 0;
  for (size_t i = 0; i < fold->nentries; i++) {
    if (fold->entries[i].deleted) continue;
    rows[n++] = &fold->entries[i];
  }
  *count = n;
  return rows;
}

/* The fingerprint's normalization: (kind,id) sort (refinement.ts:783's
   entry-order line). */
static int _entry_material_cmp(const void* a, const void* b) {
  const refine_entry_t* ea = *(const refine_entry_t* const*) a;
  const refine_entry_t* eb = *(const refine_entry_t* const*) b;
  int kind_cmp = strcmp(ea->kind, eb->kind);
  if (kind_cmp != 0) return kind_cmp;
  return strcmp(ea->id, eb->id);
}

/* The digest's newest-first row order, stable by id on equal seqs. */
static int _entry_newest_cmp(const void* a, const void* b) {
  const refine_entry_t* ea = *(const refine_entry_t* const*) a;
  const refine_entry_t* eb = *(const refine_entry_t* const*) b;
  if (ea->seq != eb->seq) return ea->seq > eb->seq ? -1 : 1;
  return strcmp(ea->id, eb->id);
}

char* refine_fold_fingerprint(const refine_fold_t* fold) {
  if (fold == NULL) {
    log_error("refine_fold_fingerprint: NULL fold — refused loud");
    return NULL;
  }
  uint64_t hash = _refine_fnv64(REFINE_FNV64_BASIS, REFINE_FINGERPRINT_SALT,
                                sizeof(REFINE_FINGERPRINT_SALT) - 1);
  size_t nlive = 0;
  const refine_entry_t** rows = _fold_live_rows(fold, &nlive);
  qsort(rows, nlive, sizeof(*rows), _entry_material_cmp);
  for (size_t i = 0; i < nlive; i++) {
    const refine_entry_t* e = rows[i];
    char version[16];
    snprintf(version, sizeof(version), "%u", e->version);
    hash = _refine_hash_string(hash, "e;");
    hash = _refine_hash_string(hash, e->kind);
    hash = _refine_hash_string(hash, ";");
    hash = _refine_hash_string(hash, e->id);
    hash = _refine_hash_string(hash, ";");
    hash = _refine_hash_string(hash, version);
    hash = _refine_hash_string(hash, ";");
    hash = _refine_hash_string(hash, e->path != NULL ? e->path : "");
    hash = _refine_hash_string(hash, ";");
    hash = _refine_hash_string(hash, e->content != NULL ? e->content : "");
    hash = _refine_hash_string(hash, ";");
    /* Only skills render the kernel call contract, so another kind can
       change these fields without changing a single digest byte. */
    if (strcmp(e->kind, "skill") == 0) {
      hash = _refine_hash_string(hash, "r;");
      hash = _refine_hash_string(hash, e->reference != NULL ? e->reference : "");
      hash = _refine_hash_string(hash, ";a;");
      hash = _refine_hash_string(hash, e->arguments != NULL ? e->arguments : "");
      hash = _refine_hash_string(hash, ";");
    }
  }
  free(rows);
  size_t nrecords = fold->nrecords;
  for (size_t i = 0; i < nrecords; i++) {
    hash = _refine_hash_string(hash, "l;");
    hash = _refine_hash_string(hash, fold->record_lines[i]);
    hash = _refine_hash_string(hash, ";");
  }
  char* hex = get_memory(17);
  snprintf(hex, 17, "%016llx", (unsigned long long) hash);
  return hex;
}

/* A growable render buffer (the digest builds lines, then joins). */
typedef struct _refine_buf_t {
  char* text;
  size_t len, capacity;
} _refine_buf_t;

static void _refine_buf_add(_refine_buf_t* buf, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed <= 0) return;
  size_t need = buf->len + (size_t) needed + 1;
  if (need > buf->capacity) {
    size_t cap = buf->capacity == 0 ? 256 : buf->capacity;
    while (cap < need) cap *= 2;
    char* text = get_memory(cap);
    if (buf->text != NULL) memcpy(text, buf->text, buf->len + 1);
    free(buf->text);
    buf->text = text;
    buf->capacity = cap;
  }
  va_start(args, fmt);
  vsnprintf(buf->text + buf->len, (size_t) needed + 1, fmt, args);
  va_end(args);
  buf->len += (size_t) needed;
}

/* One record line's digest form: a stored "<seq>;<trigger>;" line renders
   "- <seq> <trigger>"; a malformed record's skip line already carries its
   "- " label and renders verbatim (refinement.ts:479-483's render rule).
   Every stored line is one of exactly those two shapes (the skip-line
   helper's "- " prefix, or fold-parse's "%llu;<trigger>;" fmt), so the
   first ';' is always present on a trigger line. */
static void _refine_digest_record_line(_refine_buf_t* buf, const char* line) {
  if (strncmp(line, "- ", 2) == 0) {
    _refine_buf_add(buf, "%s\n", line);
    return;
  }
  const char* semi = strchr(line, ';');
  size_t seq_len = (size_t) (semi - line);
  char seq[24];
  if (seq_len >= sizeof(seq)) seq_len = sizeof(seq) - 1;
  memcpy(seq, line, seq_len);
  seq[seq_len] = '\0';
  const char* trig = semi + 1;
  size_t trig_len = strlen(trig);
  if (trig_len > 0 && trig[trig_len - 1] == ';') trig_len--;
  char* trigger = get_memory(trig_len + 1);
  memcpy(trigger, trig, trig_len);
  trigger[trig_len] = '\0';
  char* compacted = _refine_compact(trigger, SA_REFINE_DIGEST_CONTENT_CHARS);
  _refine_buf_add(buf, "- %s %s\n", seq, compacted);
  free(trigger);
  free(compacted);
}

char* refine_fold_digest(const refine_fold_t* fold) {
  if (fold == NULL) {
    log_error("refine_fold_digest: NULL fold — refused loud");
    return NULL;
  }
  _refine_buf_t buf = {NULL, 0, 0};
  size_t nentries = fold->nentries;
  size_t nrecords = fold->nrecords;
  size_t live_total = 0;
  for (size_t i = 0; i < nentries; i++) {
    if (!fold->entries[i].deleted) live_total++;
  }
  if (live_total == 0) {
    _refine_buf_add(&buf, "harness: empty\n");
  } else {
    _refine_buf_add(&buf, "harness: %zu\n", live_total);
  }

  const refine_entry_t** rows =
      get_memory((nentries != 0 ? nentries : 1) * sizeof(*rows));
  for (int k = 0; k < REFINE_KINDS_COUNT; k++) {
    const char* kind = REFINE_KINDS[k];
    size_t n = 0;
    for (size_t i = 0; i < nentries; i++) {
      const refine_entry_t* e = &fold->entries[i];
      if (e->deleted || strcmp(e->kind, kind) != 0) continue;
      rows[n++] = e;
    }
    _refine_buf_add(&buf, "%s: %zu\n", kind, n);
    if (n == 0) continue;
    qsort(rows, n, sizeof(*rows), _entry_newest_cmp);
    size_t rendered = n < (size_t) SA_REFINE_DIGEST_ENTRIES_PER_KIND
                          ? n
                          : (size_t) SA_REFINE_DIGEST_ENTRIES_PER_KIND;
    for (size_t r = 0; r < rendered; r++) {
      char* content =
          _refine_compact(rows[r]->content, SA_REFINE_DIGEST_CONTENT_CHARS);
      _refine_buf_add(&buf, "- %s %s v%u: %s\n", rows[r]->id,
                      rows[r]->path != NULL ? rows[r]->path : "(none)",
                      rows[r]->version, content);
      free(content);
    }
    if (n > (size_t) SA_REFINE_DIGEST_ENTRIES_PER_KIND) {
      _refine_buf_add(&buf, "- +%zu older %s entries\n",
                      n - (size_t) SA_REFINE_DIGEST_ENTRIES_PER_KIND, kind);
    }
  }
  free(rows);

  if (nrecords > 0) {
    _refine_buf_add(&buf, "refinements:\n");
    size_t newest = nrecords < (size_t) SA_REFINE_DIGEST_REFINEMENTS
                        ? nrecords
                        : (size_t) SA_REFINE_DIGEST_REFINEMENTS;
    for (size_t i = nrecords - newest; i < nrecords; i++) {
      _refine_digest_record_line(&buf, fold->record_lines[i]);
    }
  }
  /* The "harness: ..." header always lands first, so buf.text is set. */
  if (buf.text[buf.len - 1] == '\n') buf.text[buf.len - 1] = '\0';
  return buf.text;
}

/* ------------------------------------------------------------------ */
/* Edit lifecycle + destruction                                        */
/* ------------------------------------------------------------------ */

void refine_edit_destroy(refine_edit_t* e) {
  if (e == NULL) return;
  free(e->action); e->action = NULL;
  free(e->kind); e->kind = NULL;
  free(e->id); e->id = NULL;
  free(e->title); e->title = NULL;
  free(e->content); e->content = NULL;
  free(e->path); e->path = NULL;
  free(e->reference); e->reference = NULL;
  free(e->arguments); e->arguments = NULL;
  free(e->reason); e->reason = NULL;
  e->expect_version = 0;
  e->evidence_first = 0;
  e->evidence_last = 0;
}

/* ------------------------------------------------------------------ */
/* JSON field plumbing (the record decode's own shapes)                */
/* ------------------------------------------------------------------ */

static const char* _refine_dom_string(const json_value_t* dom, const char* key) {
  json_value_t* v = json_get(dom, key);
  if (v == NULL || json_type(v) != JSON_STRING) return NULL;
  const char* s = json_as_string(v);
  return (s != NULL && s[0] != '\0') ? s : NULL;
}

/* Parses a provided (non-NULL) string field as a JSON OBJECT. NULL when the
   text fails to parse as an object — the "requires ... object when
   provided" refusals ride this. */
static json_value_t* _refine_object_field(const char* text) {
  if (text == NULL) return NULL;
  char* jerr = NULL;
  json_value_t* v = json_parse(text, strlen(text), &jerr);
  free(jerr);
  if (v == NULL || json_type(v) != JSON_OBJECT) {
    json_value_destroy(v);
    return NULL;
  }
  return v;
}

/* The skill reference contract (refinement.ts:1035-1043): type python, a
   non-empty import, and a non-empty callable or call_pattern. */
static char* _refine_skill_reference_refusal(const char* action,
                                             const char* reference) {
  json_value_t* dom = _refine_object_field(reference);
  if (dom == NULL) {
    /* validateEdit's object-ness line already ran; a NULL here means the
       provided reference is not an object at all. */
    return _refine_error("%s requires reference to be an object when provided",
                         action);
  }
  const char* type = _refine_dom_string(dom, "type");
  char* refuse = NULL;
  if (type == NULL || strcmp(type, "python") != 0) {
    refuse = _refine_error("%s skill reference.type must be python", action);
  } else {
    const char* import = _refine_dom_string(dom, "import");
    if (import == NULL) import = _refine_dom_string(dom, "python_import");
    const char* callable = _refine_dom_string(dom, "callable");
    if (callable == NULL) callable = _refine_dom_string(dom, "call_pattern");
    if (import == NULL) {
      refuse = _refine_error("%s skill requires python import", action);
    } else if (callable == NULL) {
      refuse = _refine_error("%s skill requires callable or call_pattern", action);
    }
  }
  json_value_destroy(dom);
  return refuse;
}

/* The edit's COMPUTED id: an explicit id, else the create slug
   (refinement.ts:1071's computedId line). Caller frees the slug result.
   Guarded on action/kind NULLs because the record-element composer calls
   it before validate has run. */
static const char* _refine_edit_id(const refine_edit_t* e, char** slug_out) {
  *slug_out = NULL;
  if (e->id != NULL) return e->id;
  if (e->action != NULL && strcmp(e->action, "create") == 0) {
    *slug_out = _refine_slug(e->title ? e->title : e->kind,
                             e->kind ? e->kind : REFINE_KINDS[1]);
    return *slug_out;
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Validation (the validateEdit port, refinement.ts:1002-1052)          */
/* ------------------------------------------------------------------ */

char* refine_edit_validate(const refine_edit_t* e) {
  if (e == NULL) return _refine_error("unsupported action (null)");
  const char* action = e->action != NULL ? e->action : "(null)";
  if (e->action == NULL || (strcmp(e->action, "create") != 0 &&
                            strcmp(e->action, "update") != 0 &&
                            strcmp(e->action, "delete") != 0)) {
    return _refine_error("unsupported action %s", action);
  }
  const char* kind = e->kind != NULL ? e->kind : "(null)";
  int known_kind = 0;
  for (int k = 0; k < REFINE_KINDS_COUNT; k++) {
    if (strcmp(kind, REFINE_KINDS[k]) == 0) {
      known_kind = 1;
      break;
    }
  }
  if (!known_kind) return _refine_error("unsupported kind %s", kind);
  int is_delete = strcmp(e->action, "delete") == 0;
  int is_skill = strcmp(kind, "skill") == 0;

  /* The computed id first: the immutable-base rule checks it too. */
  char* slug_id = NULL;
  const char* id = _refine_edit_id(e, &slug_id);
  if (id != NULL && strcmp(id, REFINE_ID_BASE_PROMPT) == 0) {
    free(slug_id);
    return _refine_error("base system prompt is not editable");
  }
  free(slug_id);
  slug_id = NULL;

  char* refuse = NULL;
  int is_create = strcmp(e->action, "create") == 0;
  if (!is_create && e->id == NULL) {
    refuse = _refine_error("%s requires id", e->action);
  } else if (!is_delete && (e->title == NULL || e->content == NULL)) {
    /* refinement.ts:1016 — and :1021-1023's non-empty-string variant cannot
       fire on the C side: our fields are decoded strings, so a provided-but-
       wrong-typed title is impossible (decode refuses it upstream). */
    refuse = _refine_error("%s requires title and content", e->action);
  } else if (e->path != NULL && e->path[0] == '\0') {
    refuse = _refine_error("%s requires path to be a non-empty string when "
                           "provided", e->action);
  }
  if (refuse != NULL) return refuse;

  /* The provided-object checks (refinement.ts:1025-1032); the parse DOM is
     a probe — destroyed immediately after the object-ness answer. */
  if (e->reference != NULL) {
    json_value_t* dom = _refine_object_field(e->reference);
    if (dom == NULL) {
      return _refine_error("%s requires reference to be an object when "
                           "provided", e->action);
    }
    json_value_destroy(dom);
  }
  if (e->arguments != NULL) {
    json_value_t* dom = _refine_object_field(e->arguments);
    if (dom == NULL) {
      return _refine_error("%s requires arguments to be an object when "
                           "provided", e->action);
    }
    json_value_destroy(dom);
  }

  /* The skill contract (refinement.ts:1036-1043). The reference's "NULL =
     keep" rule rides updates (refine.h's field contract): a skill CREATE
     must carry the python contract, while an update keeps the entry's
     current contract when the edit carries none (a provided one is still
     contract-checked). */
  if (is_skill && !is_delete) {
    if (e->arguments == NULL) {
      return _refine_error("%s skill requires arguments", e->action);
    }
    if (e->reference == NULL) {
      if (strcmp(e->action, "create") == 0) {
        return _refine_error("%s skill requires python reference", e->action);
      }
      return NULL;
    }
    return _refine_skill_reference_refusal(e->action, e->reference);
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* The evidence gate (spec §4, OURS — the deliberate strengthening)     */
/* ------------------------------------------------------------------ */

char* refine_edit_evidence_check(const refine_edit_t* e) {
  if (e == NULL || e->evidence_first == 0 || e->evidence_last == 0 ||
      e->evidence_first > e->evidence_last) {
    return _refine_error("edit without evidence");
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Apply (the applyRefinementProposal port + our gates: validate, then  */
/* the evidence gate, then the version guards)                          */
/* ------------------------------------------------------------------ */

static void _entry_keep_or_set(char** field, const char* value) {
  if (value != NULL) {
    free(*field);
    *field = _refine_dup(value);
  }
}

char* refine_edit_apply(refine_fold_t* fold, const refine_edit_t* e,
                        uint64_t at_seq) {
  if (fold == NULL || e == NULL) {
    log_error("refine_edit_apply: NULL fold or edit — refused loud");
    return _refine_error("unsupported action (null)");
  }

  /* Nothing is applied, ever, without the full contract: field validation
     and the evidence gate both refuse loud first (the gate's single choke
     point — a committed lesson ALWAYS carries its seq citations). */
  char* refuse = refine_edit_validate(e);
  if (refuse != NULL) return refuse;
  refuse = refine_edit_evidence_check(e);
  if (refuse != NULL) return refuse;

  char* slug_id = NULL;
  const char* id = _refine_edit_id(e, &slug_id);
  refine_entry_t* before = refine_entry_find(fold, e->kind, id);

  if (strcmp(e->action, "create") == 0) {
    if (before != NULL) {
      free(slug_id);
      return _refine_error("entry already exists");   /* :1111 */
    }
    /* A withdrawn (tombstoned) id is re-creatable at version 1 —
       the log's delete action does not block the next lesson under
       the same id (PA's records[id] was removed; our tombstone row is
       reused instead, keeping the fold's array compact). */
    refine_entry_t* row = _row_find(fold, e->kind, id);
    if (row == NULL) row = _fold_grow(fold);
    _entry_set(row, e->kind, id, e->title, e->content,
               e->path != NULL ? e->path : "general",
               e->reference != NULL ? e->reference : "{}",
               e->arguments != NULL ? e->arguments : "{}",
               1, 0, at_seq);
    free(slug_id);
    return NULL;
  }

  if (before == NULL) {
    free(slug_id);
    return _refine_error("entry not found");          /* :1102/:1115 */
  }
  if (before->version != e->expect_version) {
    char* stale = _refine_error("stale target: entry version %u, review saw %u",
                                before->version, e->expect_version);
    free(slug_id);
    return stale;
  }

  if (strcmp(e->action, "update") == 0) {
    unsigned version = before->version + 1;
    _entry_keep_or_set(&before->title, e->title);
    _entry_keep_or_set(&before->content, e->content);
    _entry_keep_or_set(&before->path, e->path);
    _entry_keep_or_set(&before->reference, e->reference);
    _entry_keep_or_set(&before->arguments, e->arguments);
    /* The fold's "{}"-when-absent invariant holds for every update. */
    if (before->reference == NULL) _entry_keep_or_set(&before->reference, "{}");
    if (before->arguments == NULL) _entry_keep_or_set(&before->arguments, "{}");
    before->version = version;
    before->seq = at_seq;
  } else if (strcmp(e->action, "delete") == 0) {
    before->deleted = 1;   /* tombstone: the newest state action was a delete */
    before->seq = at_seq;
  } else {
    free(slug_id);
    return _refine_error("unsupported action %s", e->action);
  }
  free(slug_id);
  return NULL;
}

/* ------------------------------------------------------------------ */
/* The record's JSON shapes                                            */
/* ------------------------------------------------------------------ */

static json_value_t* _refine_entry_json(const refine_entry_t* entry) {
  if (entry == NULL) return NULL;
  json_value_t* obj = json_new_object();
  json_object_set(obj, "kind", json_new_string(entry->kind));
  json_object_set(obj, "id", json_new_string(entry->id));
  json_object_set(obj, "title",
                  entry->title != NULL ? json_new_string(entry->title)
                                       : json_new_null());
  json_object_set(obj, "content",
                  entry->content != NULL ? json_new_string(entry->content)
                                         : json_new_null());
  json_object_set(obj, "path",
                  entry->path != NULL ? json_new_string(entry->path)
                                      : json_new_null());
  json_value_t* ref = (entry->reference != NULL)
                          ? json_parse(entry->reference,
                                       strlen(entry->reference), NULL)
                          : NULL;
  json_object_set(obj, "reference", ref != NULL ? ref : json_new_null());
  json_value_t* args = (entry->arguments != NULL)
                           ? json_parse(entry->arguments,
                                        strlen(entry->arguments), NULL)
                           : NULL;
  json_object_set(obj, "arguments", args != NULL ? args : json_new_null());
  json_object_set(obj, "version", json_new_int((int64_t) entry->version));
  json_object_set(obj, "seq", json_new_int((int64_t) entry->seq));
  return obj;
}

json_value_t* refine_record_edit_json(const refine_edit_t* e,
                                      const refine_entry_t* before,
                                      int applied, const char* error,
                                      unsigned after_version) {
  if (e == NULL) return NULL;
  char* slug_id = NULL;
  const char* id = _refine_edit_id(e, &slug_id);

  json_value_t* obj = json_new_object();
  json_object_set(obj, "action",
                  e->action != NULL ? json_new_string(e->action)
                                    : json_new_null());
  json_object_set(obj, "kind",
                  e->kind != NULL ? json_new_string(e->kind)
                                  : json_new_null());
  json_object_set(obj, "id", id != NULL ? json_new_string(id) : json_new_null());
  json_object_set(obj, "title",
                  e->title != NULL ? json_new_string(e->title)
                                   : json_new_null());
  json_object_set(obj, "content",
                  e->content != NULL ? json_new_string(e->content)
                                     : json_new_null());
  /* The element records the after-state path: the edit's own path, else the
     before's, else the create default (spec §4's sample carries "general"). */
  const char* path = e->path != NULL ? e->path
                                     : (before != NULL && before->path != NULL
                                            ? before->path
                                            : "general");
  json_object_set(obj, "path", json_new_string(path));
  json_object_set(obj, "version", json_new_int((int64_t) after_version));

  json_value_t* reference = NULL;
  json_value_t* arguments = NULL;
  if (e->reference != NULL) {
    reference = json_parse(e->reference, strlen(e->reference), NULL);
  }
  if (e->arguments != NULL) {
    arguments = json_parse(e->arguments, strlen(e->arguments), NULL);
  }
  json_object_set(obj, "reference", reference != NULL ? reference
                                                      : json_new_null());
  json_object_set(obj, "arguments", arguments != NULL ? arguments
                                                      : json_new_null());

  json_value_t* expect = json_new_object();
  json_object_set(expect, "version",
                  json_new_int((int64_t) e->expect_version));
  json_object_set(obj, "expect", expect);

  json_value_t* evidence = json_new_object();
  json_object_set(evidence, "first_seq", json_new_int((int64_t) e->evidence_first));
  json_object_set(evidence, "last_seq", json_new_int((int64_t) e->evidence_last));
  json_object_set(evidence, "summary",
                  e->reason != NULL ? json_new_string(e->reason)
                                    : json_new_null());
  json_object_set(obj, "evidence", evidence);

  json_value_t* before_dom = _refine_entry_json(before);
  json_object_set(obj, "before", before_dom != NULL ? before_dom
                                                    : json_new_null());
  json_object_set(obj, "applied", json_new_bool(applied));
  json_object_set(obj, "error",
                  error != NULL ? json_new_string(error) : json_new_null());

  free(slug_id);
  return obj;
}

/* ------------------------------------------------------------------ */
/* The fold (parse a scan reply's record ARRAY and apply it)            */
/* ------------------------------------------------------------------ */

static json_value_t* _refine_parse_text(const char* text) {
  char* jerr = NULL;
  json_value_t* v = json_parse(text, strlen(text), &jerr);
  free(jerr);
  return v;
}

/* Decodes ONE record edit element into a refine_edit_t. The struct is
   zeroed first, so a partial decode stays destroy-safe. Returns 0, or
   -1 on shape malformation (the fold's caller logs + skips; the proposal
   decode refuses loud). */
static int _refine_edit_decode(json_value_t* el, refine_edit_t* e) {
  memset(e, 0, sizeof(*e));
  json_value_t* v;
  v = json_get(el, "action");
  if (v == NULL || json_type(v) != JSON_STRING) return -1;
  e->action = _refine_dup(json_as_string(v));
  v = json_get(el, "kind");
  if (v == NULL || json_type(v) != JSON_STRING) return -1;
  e->kind = _refine_dup(json_as_string(v));
  v = json_get(el, "id");
  if (v != NULL && json_type(v) == JSON_STRING) e->id = _refine_dup(json_as_string(v));
  v = json_get(el, "title");
  if (v != NULL && json_type(v) == JSON_STRING) e->title = _refine_dup(json_as_string(v));
  v = json_get(el, "content");
  if (v != NULL && json_type(v) == JSON_STRING) e->content = _refine_dup(json_as_string(v));
  v = json_get(el, "path");
  if (v != NULL && json_type(v) == JSON_STRING) e->path = _refine_dup(json_as_string(v));
  v = json_get(el, "reference");
  if (v != NULL && json_type(v) == JSON_OBJECT) e->reference = json_serialize(v);
  v = json_get(el, "arguments");
  if (v != NULL && json_type(v) == JSON_OBJECT) e->arguments = json_serialize(v);

  v = json_get(el, "expect");
  if (v != NULL && json_type(v) == JSON_OBJECT) {
    json_value_t* version = json_get(v, "version");
    if (version != NULL && json_type(version) == JSON_INT) {
      e->expect_version = (unsigned) json_as_int(version);
    }
  }

  v = json_get(el, "evidence");
  if (v != NULL && json_type(v) == JSON_OBJECT) {
    /* The plain shape: {"first_seq","last_seq","summary"}; the rollback
       shape {"kind":"rollback","refineOf":N} names the target record seq —
       both satisfy the gate (the inverse edit's evidence IS the target). */
    json_value_t* first = json_get(v, "first_seq");
    json_value_t* last = json_get(v, "last_seq");
    if (first != NULL && last != NULL && json_type(first) == JSON_INT &&
        json_type(last) == JSON_INT) {
      e->evidence_first = (uint64_t) json_as_int(first);
      e->evidence_last = (uint64_t) json_as_int(last);
    }
    json_value_t* summary = json_get(v, "summary");
    if (summary != NULL && json_type(summary) == JSON_STRING) {
      e->reason = _refine_dup(json_as_string(summary));
    }
  }
  return 0;
}

int refine_fold_parse(const char* records_array_json, refine_fold_t* fold) {
  if (records_array_json == NULL || fold == NULL) {
    log_error("refine_fold_parse: NULL input — refused loud");
    return -1;
  }
  char* jerr = NULL;
  json_value_t* array = json_parse(records_array_json, strlen(records_array_json), &jerr);
  if (array == NULL) {
    log_error("refine_fold_parse: the reply is not a JSON array (%s)",
              jerr != NULL ? jerr : "malformed");
    free(jerr);
    return -1;
  }
  if (json_type(array) != JSON_ARRAY) {
    log_error("refine_fold_parse: the reply is not a JSON array — refused loud");
    json_value_destroy(array);
    return -1;
  }

  for (size_t i = 0; i < json_size(array); i++) {
    json_value_t* element = json_at(array, i);
    if (element == NULL || json_type(element) != JSON_STRING) {
      log_error("refine_fold_parse: record %zu is not a text element — "
                "skipped loud", i);
      _fold_push_skip_line(fold, "a record element is not a text");
      continue;
    }
    const char* text = json_as_string(element);
    json_value_t* record = _refine_parse_text(text);
    if (record == NULL) {
      log_error("refine_fold_parse: record %zu text is not a JSON document — "
                "skipped loud", i);
      _fold_push_skip_line(fold, "the record text is not a JSON document");
      continue;
    }
    if (json_type(record) != JSON_OBJECT) {
      log_error("refine_fold_parse: record %zu is not a JSON object — "
                "skipped loud", i);
      _fold_push_skip_line(fold, "a record is not a JSON object");
      json_value_destroy(record);
      continue;
    }

    json_value_t* seq_v = json_get(record, "seq");
    json_value_t* id_v = json_get(record, "id");
    json_value_t* trigger_v = json_get(record, "trigger");
    json_value_t* edits_v = json_get(record, "edits");
    if (seq_v == NULL || json_type(seq_v) != JSON_INT ||
        id_v == NULL || json_type(id_v) != JSON_STRING ||
        trigger_v == NULL || json_type(trigger_v) != JSON_STRING ||
        edits_v == NULL || json_type(edits_v) != JSON_ARRAY) {
      /* refinement.ts:761's skip-with-diagnostic rule: one bad record can
         never break a session's refine, and the label never carries the
         record's VALUE (an unbounded corrupt field). */
      log_error("refine_fold_parse: record %zu violates the record contract — "
                "skipped loud", i);
      _fold_push_skip_line(fold, "a record violates the record contract");
      json_value_destroy(record);
      continue;
    }
    uint64_t seq = (uint64_t) json_as_int(seq_v);

    /* The record line (the digest's refinement tail + the fingerprint's
       record material): "<seq>;<trigger-trim>;" in stored order. */
    char* trigger = _refine_compact(json_as_string(trigger_v),
                                    SA_REFINE_DIGEST_CONTENT_CHARS);
    char line[512];
    snprintf(line, sizeof(line), "%llu;%.360s;",
             (unsigned long long) seq, trigger);
    _record_line_put(fold, line);
    free(trigger);

    for (size_t j = 0; j < json_size(edits_v); j++) {
      json_value_t* el = json_at(edits_v, j);
      json_value_t* applied_v = el != NULL ? json_get(el, "applied") : NULL;
      if (applied_v == NULL || !json_as_bool(applied_v)) continue;   /* refused
                                            edits live in the record only */
      refine_edit_t edit;
      int rc = (el != NULL && json_type(el) == JSON_OBJECT)
                   ? _refine_edit_decode(el, &edit)
                   : -1;
      if (rc != 0) {
        /* Partial decodes stay destroy-safe (the struct is zeroed first). */
        refine_edit_destroy(&edit);
        log_error("refine_fold_parse: record %llu edit %zu violates the edit "
                  "contract — skipped loud", (unsigned long long) seq, j);
        continue;
      }
      char* apply_err = refine_edit_apply(fold, &edit, seq);
      if (apply_err != NULL) {
        log_error("refine_fold_parse: record %llu edit %zu refused: %s",
                  (unsigned long long) seq, j, apply_err);
        free(apply_err);
      }
      refine_edit_destroy(&edit);
    }
    json_value_destroy(record);
  }
  json_value_destroy(array);
  return 0;
}

/* ------------------------------------------------------------------ */
/* The review call (spec §3 step 4): the no-tools review + the decode  */
/* ------------------------------------------------------------------ */

/* The shared (root-level) scope's harness root: refine_run composes the
   current scope's root from it (frame_sid(f) + "/harness" for a local run);
   the review call compares scope_root against it — EQUAL = a shared run (no
   read-only-context section), anything else = a local run whose shared
   scope is read-only context (spec §1's scoping semantics). A #define so
   the "/log" range bounds compose as one literal. */
#define REFINE_SCOPE_ROOT_SHARED "harness"

/* The review's system prompt — the translated contract of refinement.ts
   :129-180: the kinds, the actions, the JSON-only output, the
   base-immutability line at its core, and OUR evidence requirement (spec
   §4's recorded divergence) spelled out. A prompt, not code. */
static const char REFINE_REVIEW_SYSTEM[] =
    "You are SecretAgent's /refine continual harness subsystem.\n"
    "\n"
    "Review the current trajectory and emit precise Create, Update, or\n"
    "Delete edits to the editable continual harness state: the persistent,\n"
    "editable set of prompt notes, memories, skills, and subagent specs that\n"
    "carries reusable behavior between sessions.\n"
    "\n"
    "Kinds:\n"
    "- prompt: supplemental prompt notes only. The base system prompt is\n"
    "  immutable and MUST NOT be rewritten; the id \"base_system_prompt\" is\n"
    "  untouchable.\n"
    "- memory: durable facts, decisions, failures, preferences, and\n"
    "  outcomes.\n"
    "- skill: an installed Python skill. Skill create/update edits MUST\n"
    "  include a `reference` object with {\"type\":\"python\"}, a Python\n"
    "  import, and a callable or call_pattern; they also MUST include an\n"
    "  `arguments` object describing accepted inputs, required fields,\n"
    "  defaults, and constraints.\n"
    "- subagent: reusable delegation specs: purpose, instructions, and when\n"
    "  to invoke them.\n"
    "\n"
    "Scope and edits:\n"
    "- Propose edits only for the requested scope's store. Entries shown as\n"
    "  read-only context are never edit targets; a needed session-specific\n"
    "  override is a local create.\n"
    "- Prefer small, specific lessons the trajectory actually evidences; do\n"
    "  not restate what the current harness state already carries.\n"
    "- Never edit source files; edits touch the harness state only.\n"
    "\n"
    "Evidence:\n"
    "- Every edit names the event seqs it rests on: evidence =\n"
    "  {\"first_seq\":<first>,\"last_seq\":<last>} citing the trajectory's\n"
    "  event records. An edit without evidence is refused.\n"
    "- Updates and deletes carry `expect.version`, the version the current\n"
    "  harness state shows; a mismatch refuses the edit as stale.\n"
    "\n"
    "Output JSON only, in exactly this shape:\n"
    "\n"
    "{\n"
    "  \"summary\": \"one sentence\",\n"
    "  \"rationale\": \"why these edits are justified by trajectory evidence\",\n"
    "  \"edits\": [\n"
    "    {\n"
    "      \"action\": \"create|update|delete\",\n"
    "      \"kind\": \"prompt|memory|skill|subagent\",\n"
    "      \"id\": \"stable id for update/delete, optional for create\",\n"
    "      \"title\": \"required for create/update except delete\",\n"
    "      \"content\": \"required for create/update except delete\",\n"
    "      \"path\": \"optional grouping path\",\n"
    "      \"reference\": {\"type\":\"python\",\"import\":\"package.module\",\n"
    "                      \"callable\":\"function_name\"},\n"
    "      \"arguments\": {},\n"
    "      \"reason\": \"why this edit is useful\",\n"
    "      \"expect\": {\"version\": 0},\n"
    "      \"evidence\": {\"first_seq\": 0, \"last_seq\": 0}\n"
    "    }\n"
    "  ]\n"
    "}\n";

/* PA's TRUNCATED_JSON_ERROR (refinement.ts:198-199, verbatim): the over-cap
   refusal AND the incomplete-reply's diagnosis share the one wording. */
#define REFINE_TRUNCATED_JSON \
  "the model stopped before completing its JSON object. This usually means " \
  "the output budget was exhausted; retry with a smaller request."

/* The bounded tail of prior refinements in the prompt (PA's
   historyForPrompt .slice(-20), refinement.ts:1277). */
#define REFINE_HISTORY_TAIL 20

/* The scan-bounds scratch: a session subtree path plus one key stays well
   inside this (frame.c's compositions are far shorter); the overflow is a
   loud refusal, never a silent truncation. */
#define REFINE_BOUND_MAX 256

/* "<root>" .. "<root>0" — the two ABSOLUTE ROOT-LEVEL scan bounds for one
   store range (never relative — the subtree-scan breakage, frame.c's
   canonical pattern). Returns 0, -1 on overflow. */
static int _refine_range_bounds(const char* root, char* lo, size_t lo_size,
                                char* hi, size_t hi_size) {
  size_t n = strlen(root);
  if (n + 2 > lo_size || n + 2 > hi_size) return -1;
  memcpy(lo, root, n + 1);
  memcpy(hi, root, n + 1);
  hi[n] = '0';
  hi[n + 1] = '\0';
  return 0;
}

/* PA's isInCompleteJson (refinement.ts:897-911, the diagnosis's shape): a
   reply truncated inside a string or a bracket run is named as the cause
   instead of the fragment. */
static int _refine_json_is_incomplete(const char* text) {
  size_t depth = 0;
  int in_string = 0;
  int escaped = 0;
  for (const char* p = text; *p != '\0'; p++) {
    char c = *p;
    if (escaped) {
      escaped = 0;
      continue;
    }
    if (in_string) {
      if (c == '\\') escaped = 1;
      else if (c == '"') in_string = 0;
      continue;
    }
    if (c == '"') in_string = 1;
    else if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') depth = depth > 0 ? depth - 1 : 0;
  }
  return in_string || depth > 0;
}

/* The reply's JSON candidate: a malloc'd trimmed copy with the first code
   fence stripped (the "```json ... ```" shape models actually emit — PA's
   unanchored fence regex, refinement.ts:943, so prose may wrap the fence;
   the lazy match ends at the FIRST closing fence, unclosed fences ride
   whole). Caller frees. */
static char* _refine_json_candidate(const char* content) {
  size_t n = content != NULL ? strlen(content) : 0;
  size_t begin = 0;
  while (begin < n && isspace((unsigned char) content[begin])) begin++;
  size_t end = n;
  while (end > begin && isspace((unsigned char) content[end - 1])) end--;
  const char* p = content + begin;
  size_t plen = end - begin;

  /* PA's unanchored fence search: the FIRST "```" anywhere in the trimmed
     reply opens the candidate. */
  const char* fence = NULL;
  size_t fence_at = 0;
  for (size_t i = 0; i + 3 <= plen; i++) {
    if (p[i] == '`' && p[i + 1] == '`' && p[i + 2] == '`') {
      fence = p + i;
      fence_at = i;
      break;
    }
  }
  if (fence == NULL) {
    char* candidate = get_memory(plen + 1);
    memcpy(candidate, p, plen);
    candidate[plen] = '\0';
    return candidate;
  }

  /* The fence body: an optional "json" tag, then the run to the FIRST
     closing fence — or, a truncated fence, everything the reply had. */
  const char* body = fence + 3;
  size_t body_len = plen - fence_at - 3;
  if (body_len >= 4 && strncmp(body, "json", 4) == 0) {
    body += 4;
    body_len -= 4;
  }
  while (body_len > 0 && isspace((unsigned char) *body)) {
    body++;
    body_len--;
  }
  char* candidate = get_memory(body_len + 1);
  memcpy(candidate, body, body_len);
  candidate[body_len] = '\0';
  char* close = strstr(candidate, "```");
  if (close != NULL) {
    size_t keep = (size_t) (close - candidate);   /* the lazy match's body */
    while (keep > 0 && isspace((unsigned char) candidate[keep - 1])) keep--;
    candidate[keep] = '\0';
  }
  return candidate;
}

/* The reply's content -> the proposal DOM (PA's extractJsonObject +
   parseProposal, refinement.ts:914-996): the fence-trimmed content parses;
   a truncated reply (inside a string or an open bracket run) is named with
   PA's truncation wording; other parse failures and a non-object reply
   refuse loud (the fall-through wording for a brace-free reply, PA's
   :963 line). NULL + *error_out (heap, the caller's) on refusal. */
static json_value_t* _refine_parse_proposal(const char* content, char** error_out) {
  *error_out = NULL;
  if (content == NULL || content[0] == '\0') {
    *error_out = _refine_error("the model did not return valid JSON: the "
                               "reply is empty");
    return NULL;
  }
  char* candidate = _refine_json_candidate(content);
  char* jerr = NULL;
  json_value_t* dom = json_parse(candidate, strlen(candidate), &jerr);
  if (dom == NULL) {
    if (_refine_json_is_incomplete(candidate)) {
      *error_out = _refine_error("%s", REFINE_TRUNCATED_JSON);
    } else if (strchr(candidate, '{') != NULL) {
      *error_out = _refine_error("the model did not return valid JSON: %s",
                                 jerr != NULL ? jerr : "malformed");
    } else {
      *error_out = _refine_error("Refiner did not return a JSON object");
    }
    free(jerr);
    free(candidate);
    return NULL;
  }
  free(jerr);
  free(candidate);
  if (json_type(dom) != JSON_OBJECT) {
    *error_out = _refine_error("Refiner JSON must be an object");
    json_value_destroy(dom);
    return NULL;
  }
  return dom;
}

/* The trajectory's bounded tail slice (PA planRefinement's .slice(-80_000),
   refinement.ts:1258): the slice drops WHOLE OLDEST array elements until
   the joint text fits <= SA_REFINE_TRAJECTORY_CHARS — never a mid-document
   cut, so the view stays parseable and evidence-citable, and the newest
   single record always survives. Consumes the input; returns the bounded
   text (or NULL loud on an unparseable reply — the scan's own shape
   guarantees an array, so this is an internal failure, never tolerated). */
static char* _refine_trajectory_tail(char* raw) {
  size_t n = strlen(raw);
  if (n <= (size_t) SA_REFINE_TRAJECTORY_CHARS) return raw;

  char* jerr = NULL;
  json_value_t* arr = json_parse(raw, n, &jerr);
  free(jerr);
  if (arr == NULL || json_type(arr) != JSON_ARRAY) {
    log_error("refine_review_call: the trajectory scan reply is not an array");
    if (arr != NULL) json_value_destroy(arr);
    free(raw);
    return NULL;
  }
  size_t count = json_size(arr);
  if (count == 0) {
    json_value_destroy(arr);
    free(raw);
    char* out = get_memory(3);
    memcpy(out, "[]", 3);
    return out;
  }
  char** texts = get_memory(count * sizeof(char*));
  size_t* lens = get_memory(count * sizeof(size_t));
  for (size_t i = 0; i < count; i++) {
    texts[i] = json_serialize(json_at(arr, i));
    lens[i] = strlen(texts[i]);
  }
  json_value_destroy(arr);

  /* The oldest start that still fits: walk from the NEWEST record back
     while the accumulated run stays inside the cap. */
  size_t best = count;
  size_t run = 0;
  for (size_t i = count; i-- > 0;) {
    run += lens[i];
    if (run + (count - i) + 1 > (size_t) SA_REFINE_TRAJECTORY_CHARS) break;
    best = i;
  }
  if (best >= count) {
    best = count - 1;   /* the newest record survives even alone-over-cap */
  }
  size_t out_len = 2;
  for (size_t i = best; i < count; i++) out_len += lens[i] + 1;
  char* out = get_memory(out_len + 1);
  size_t w = 0;
  out[w++] = '[';
  for (size_t i = best; i < count; i++) {
    if (i > best) out[w++] = ',';
    memcpy(out + w, texts[i], lens[i]);
    w += lens[i];
  }
  out[w++] = ']';
  out[w] = '\0';
  for (size_t i = 0; i < count; i++) free(texts[i]);
  free(texts);
  free(lens);
  free(raw);
  return out;
}

/* Composes the review's USER text (the spec §3 step-4 recipe): the current
   digest, the prior-refinement tail from the fold, the read-only-context
   digest for a local run against the shared scope, the bounded trajectory
   view, and the caller's instructions when carried. On success *user_out
   and *traj_out are the caller's heap strings (the caller frees BOTH, the
   trajectory even on the LATER failures — it is the review's consumed
   view). Returns 0, -1 loud (everything freed here; *traj_out NULLed). */
static int _refine_review_compose(frame_t* f, const refine_fold_t* fold,
                                  const char* scope_root,
                                  const char* instructions, char** user_out,
                                  char** traj_out) {
  char* digest = refine_fold_digest(fold);   /* never NULL: the fold is checked */
  char* traj_raw = NULL;
  char* traj = NULL;
  char* context_digest = NULL;
  char* ctx_raw = NULL;
  _refine_buf_t buf = {NULL, 0, 0};
  char lo[REFINE_BOUND_MAX];
  char hi[REFINE_BOUND_MAX];

  /* The events trajectory: the store-actor round trip (spec §3's bullet),
     the derive's own scan shape, the newest SA_REFINE_SCAN_EVENTS records,
     then the bounded char tail slice. The range is the frame's EVENTS
     subtree — "<sid>/events" .. "<sid>/events0" (frame.c's canonical
     composition), never the whole session subtree. */
  const char* sid = frame_sid(f);
  if (snprintf(lo, sizeof(lo), "%s/events", sid) >= (int) sizeof(lo) ||
      snprintf(hi, sizeof(hi), "%s/events0", sid) >= (int) sizeof(hi)) {
    log_error("refine_review_call: '%s' overflows the scan-bounds buffer", sid);
    goto compose_fail;
  }
  if (_frame_sync_scan(f, lo, hi, (size_t) SA_REFINE_SCAN_EVENTS, &traj_raw) != 0 ||
      traj_raw == NULL) {
    log_error("refine_review_call: the trajectory scan refused at '%s'",
              frame_sid(f));
    goto compose_fail;
  }
  traj = _refine_trajectory_tail(traj_raw);
  traj_raw = NULL;
  if (traj == NULL) goto compose_fail;

  /* The read-only context: a local run scans the SHARED scope's log (the
     other scope stays read-only, spec §1's scoping semantics); a shared run
     composes no context section. */
  if (strcmp(scope_root, REFINE_SCOPE_ROOT_SHARED) != 0) {
    if (_refine_range_bounds(REFINE_SCOPE_ROOT_SHARED "/log", lo, sizeof(lo),
                             hi, sizeof(hi)) != 0) {
      log_error("refine_review_call: the shared-scope bounds overflow");
      goto compose_fail;
    }
    if (_frame_sync_scan(f, lo, hi, (size_t) SA_REFINE_LOG_WINDOW, &ctx_raw) != 0 ||
        ctx_raw == NULL) {
      log_error("refine_review_call: the shared-scope context scan refused "
                "at '%s'", frame_sid(f));
      goto compose_fail;
    }
    if (strcmp(ctx_raw, "[]") != 0) {
      refine_fold_t ctx_fold;
      memset(&ctx_fold, 0, sizeof(ctx_fold));
      if (refine_fold_parse(ctx_raw, &ctx_fold) != 0) {
        log_error("refine_review_call: the shared-scope log does not fold — "
                  "refused loud");
        refine_fold_destroy(&ctx_fold);
        goto compose_fail;
      }
      context_digest = refine_fold_digest(&ctx_fold);
      refine_fold_destroy(&ctx_fold);
    }
  }

  /* The user text, PA buildPrompt's join("\n\n") shape
     (refinement.ts:1260-1270). */
  _refine_buf_add(&buf, "<current_harness_state>\n%s\n</current_harness_state>",
                  digest);
  _refine_buf_add(&buf, "\n\n<refinement_history>\n");
  if (fold->nrecords == 0) {
    _refine_buf_add(&buf, "No prior refinement history.");
  } else {
    size_t newest = fold->nrecords;
    size_t start = (newest > (size_t) REFINE_HISTORY_TAIL)
                       ? newest - (size_t) REFINE_HISTORY_TAIL
                       : 0;
    for (size_t i = start; i < newest; i++) {
      _refine_digest_record_line(&buf, fold->record_lines[i]);
    }
  }
  _refine_buf_add(&buf, "\n</refinement_history>");
  if (context_digest != NULL) {
    _refine_buf_add(&buf, "\n\n<shared_harness_context>\nread-only context: "
                          "shared-scope entries are never edit targets from "
                          "a local refinement\n%s\n</shared_harness_context>",
                    context_digest);
  }
  _refine_buf_add(&buf, "\n\n<trajectory>\n%s\n</trajectory>", traj);
  if (instructions != NULL && instructions[0] != '\0') {
    _refine_buf_add(&buf, "\n\n<user_refine_instructions>\n%s\n"
                          "</user_refine_instructions>", instructions);
  }
  _refine_buf_add(&buf, "\n\nReturn only JSON edits. If no useful edit is "
                        "justified, return an empty edits array with a "
                        "rationale.");

  free(digest);
  if (context_digest != NULL) free(context_digest);
  free(ctx_raw);
  if (buf.text != NULL) buf.text[buf.len] = '\0';
  *user_out = buf.text;
  *traj_out = traj;
  return 0;

compose_fail:
  free(digest);
  free(context_digest);
  free(ctx_raw);
  free(traj_raw);
  free(traj);
  free(buf.text);
  *user_out = NULL;
  *traj_out = NULL;
  return -1;
}

/* Walks the proposal's `edits` array into refine_edit_t's: the object
   elements are the proposal's edits (PA's normalize filter), an over-cap
   proposal refuses with PA's truncation wording, and EVERY edit is
   validateEdit-checked here — the FIRST refusal is the whole decode's
   refusal (no partial decode). The edit's top-level `reason` wins over the
   evidence's summary when the proposal carries both. Returns 0 with
   *edits_out (NULL when the proposal carries no edits — the valid empty
   proposal), -1 + *error_out loud otherwise. */
static int _refine_decode_edits(json_value_t* proposal, refine_edit_t** edits_out,
                                size_t* nedits_out, char** error_out) {
  *edits_out = NULL;
  *nedits_out = 0;
  *error_out = NULL;

  json_value_t* edits_dom = json_get(proposal, "edits");
  if (edits_dom == NULL || json_type(edits_dom) != JSON_ARRAY) {
    return 0;   /* PA's normalize: a missing/non-array edits field is [] */
  }
  size_t nobj = 0;
  for (size_t i = 0; i < json_size(edits_dom); i++) {
    if (json_type(json_at(edits_dom, i)) == JSON_OBJECT) nobj++;
  }
  if (nobj > (size_t) SA_REFINE_MAX_EDITS) {
    /* The output cap is the refuse-loud stand-in for the output reserve
       (spec §4) — the truncation wording, never a silent truncation. */
    *error_out = _refine_error("%s", REFINE_TRUNCATED_JSON);
    return -1;
  }
  if (nobj == 0) return 0;

  refine_edit_t* out = get_clear_memory(nobj * sizeof(refine_edit_t));
  size_t n = 0;
  for (size_t i = 0; i < json_size(edits_dom); i++) {
    json_value_t* el = json_at(edits_dom, i);
    if (el == NULL || json_type(el) != JSON_OBJECT) continue;   /* PA's filter */
    refine_edit_t e;
    memset(&e, 0, sizeof(e));
    int rc = _refine_edit_decode(el, &e);
    if (rc != 0) {
      /* The decode's own failure is a refusal IN ITS OWN RIGHT — never a
         fall-through to refine_edit_validate, whose only catch here is the
         partial struct's NULL action/kind (an invariant two functions away). */
      refine_edit_destroy(&e);
      for (size_t j = 0; j < n; j++) refine_edit_destroy(&out[j]);
      free(out);
      *error_out = _refine_error("edit element is malformed");
      return -1;
    }
    /* The proposal's own top-level reason wins over the evidence line's
       summary (both feeds the record's evidence summary, Task 5's compose). */
    json_value_t* reason = json_get(el, "reason");
    if (reason != NULL && json_type(reason) == JSON_STRING) {
      free(e.reason);
      e.reason = _refine_dup(json_as_string(reason));
    }
    /* The validateEdit strings refuse the whole reply (apply-stage gates —
       evidence, versions — stay per-edit). */
    char* verdict = refine_edit_validate(&e);
    if (verdict != NULL) {
      refine_edit_destroy(&e);
      for (size_t j = 0; j < n; j++) refine_edit_destroy(&out[j]);
      free(out);
      *error_out = verdict;
      return -1;
    }
    out[n++] = e;   /* moved: the caller owns the fields from here */
  }
  *edits_out = out;
  *nedits_out = n;
  return 0;
}

char* refine_review_call(frame_t* f, const refine_fold_t* fold,
                         const char* scope_root, const char* instructions,
                         char** trajectory_json,
                         refine_edit_t** edits, size_t* nedits,
                         char** summary, char** rationale) {
  if (trajectory_json != NULL) *trajectory_json = NULL;
  if (edits != NULL) *edits = NULL;
  if (nedits != NULL) *nedits = 0;
  if (summary != NULL) *summary = NULL;
  if (rationale != NULL) *rationale = NULL;
  if (f == NULL || fold == NULL || scope_root == NULL || scope_root[0] == '\0' ||
      trajectory_json == NULL || edits == NULL || nedits == NULL ||
      summary == NULL || rationale == NULL) {
    log_error("refine_review_call: NULL input — refused loud");
    return _refine_error("refine review: the frame, fold, scope root, and "
                         "every out-slot are required");
  }

  /* The frame's own configured backend (spec §6): the frame-injected
     override wins, else the once-built default. The SYNC complete() is the
     review's shape — a bounded one-shot on the caller's thread (the demo
     CLI thread in production; the model.c complete contract's documented
     bounded caller-side block). */
  model_backend_t* mb = _frame_backend_get(f);
  if (mb == NULL || mb->complete == NULL) {
    log_error("refine_review_call: '%s' has no usable model backend",
              frame_sid(f));
    return _refine_error("refine review: no usable model backend");
  }

  char* user_text = NULL;
  char* traj = NULL;
  if (_refine_review_compose(f, fold, scope_root, instructions, &user_text,
                             &traj) != 0) {
    return _refine_error("refine review: the review compose refused (the "
                         "specific cause is logged)");
  }

  /* The messages DOM: system = the review contract, user = the composed
     recipe. The tools argument is a JSON NULL VALUE — EXPLICITLY no tools
     (the request build omits both tool keys; spec §7). */
  json_value_t* messages = json_new_array();
  json_value_t* system = json_new_object();
  json_object_set(system, "role", json_new_string("system"));
  json_object_set(system, "content", json_new_string(REFINE_REVIEW_SYSTEM));
  json_array_append(messages, system);
  json_value_t* user = json_new_object();
  json_object_set(user, "role", json_new_string("user"));
  json_object_set(user, "content", json_new_string(user_text));
  free(user_text);   /* the DOM now owns the text's copy */
  json_array_append(messages, user);
  json_value_t* no_tools = json_new_null();

  model_reply_t* reply = NULL;
  char* model_err = NULL;
  int crc = mb->complete(mb, messages, no_tools, NULL, &reply, &model_err);
  json_value_destroy(messages);
  json_value_destroy(no_tools);
  if (crc != 0) {
    free(traj);
    char* refusal = (model_err != NULL)
                        ? model_err
                        : _refine_error("refine review: the model call failed");
    log_error("refine_review_call: %s", refusal);
    return refusal;
  }

  /* The DECODE (the reply's content only — no tool surface ever exists to
     carry a refine call): fence trim -> parse -> the field fence + the
     output cap. */
  char* decode_err = NULL;
  json_value_t* proposal = _refine_parse_proposal(reply->content, &decode_err);
  if (proposal == NULL) {
    model_reply_destroy(reply);
    free(traj);
    log_error("refine_review_call: the proposal decode refused: %s", decode_err);
    return decode_err;
  }
  refine_edit_t* out_edits = NULL;
  size_t out_n = 0;
  char* edits_err = NULL;
  int drc = _refine_decode_edits(proposal, &out_edits, &out_n, &edits_err);
  if (drc != 0) {
    json_value_destroy(proposal);
    model_reply_destroy(reply);
    free(traj);
    log_error("refine_review_call: the proposal decode refused: %s", edits_err);
    return edits_err;
  }

  /* The proposal's top-level fields (PA's normalize defaults ported). */
  json_value_t* v = json_get(proposal, "summary");
  *summary = _refine_dup(v != NULL && json_type(v) == JSON_STRING
                             ? json_as_string(v)
                             : "Refined continual harness state");
  v = json_get(proposal, "rationale");
  *rationale = _refine_dup(v != NULL && json_type(v) == JSON_STRING
                               ? json_as_string(v)
                               : "");
  json_value_destroy(proposal);
  model_reply_destroy(reply);
  *trajectory_json = traj;
  *edits = out_edits;
  *nedits = out_n;
  return NULL;
}

#endif /* SA_HAS_WDB */