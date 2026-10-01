//
// Created by victor on 10/1/26.
//

#include "refine.h"

#ifdef SA_HAS_WDB

#include "../Util/allocator.h"
#include "../Util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* refine.c — the refine slice's DATA half (the orchestrator spec: the fold,
   the edit validation, the evidence gate, the apply semantics with version
   guards, the record's edit-element composition). The store-riding runners
   (refine_run / refine_rollback) and the fingerprint/digest views land with
   their own tasks; the store actor remains the only serializer this module
   ever answers to. */

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
   -1 on shape malformation (the caller logs + skips). */
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

#endif /* SA_HAS_WDB */