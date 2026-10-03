//
// Created by victor on 10/1/26.
//

#include "lifecycle.h"

#include "../Util/allocator.h"
#include "../Util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* lifecycle.c — the turn-lifecycle slice's PURE half (spec §2): the event
   vocabulary's constants, the riders' payload composers, the cursor fold
   over an event tail (state-of-the-tail, not event history), the closer
   composition (the deterministic crash-repair records), and the two pinned
   model-visible brief shapes. No store, no model, no actor — the module is
   a pure data unit in the refine slice's discipline (refuse-loud on NULL,
   render-not-crash on malformed records, get_memory/get_clear_memory). */

const char LIFE_EVENT_TURN_START[] = "turn.start";
const char LIFE_EVENT_TURN_END[] = "turn.end";
const char LIFE_EVENT_STEP_START[] = "step.start";
const char LIFE_EVENT_STEP_END[] = "step.end";
const char LIFE_EVENT_REPAIR[] = "repair";

const char LIFE_REASON_COMPLETED[] = "completed";
const char LIFE_REASON_ERROR[] = "error";
const char LIFE_REASON_TURN_LIMIT[] = "turn-limit";
const char LIFE_REASON_INTERRUPTED[] = "interrupted";
const char LIFE_REASON_ABORTED[] = "aborted";
const char LIFE_REASON_BLOCKED[] = "blocked";
const char LIFE_REASON_DOOM_LOOP[] = "doom-loop";

/* ------------------------------------------------------------------ */
/* The pinned brief wording (spec §2 — VERBATIM, word-for-word)        */
/* ------------------------------------------------------------------ */

/* The started shape: an open cell.run exists — the crash cut between the
   cell's audit commit and its result. Four parts, newline-joined: the lead
   line, the details line quoting the recorded seq, the cell's code
   verbatim, and the fact+guidance line. */
static const char _LIFE_STARTED_LEAD[] =
    "The previous turn was interrupted before its result was recorded.";
static const char _LIFE_STARTED_DETAILS[] =
    "The cell was executing (harness-log seq %llu):";
static const char _LIFE_STARTED_FACTS[] =
    "Its outcome is unknown. Decide whether to retry from the cell's "
    "semantics: retry only if the operation is read-only or idempotent; if "
    "it may have side effects, first verify external state or ask the user. "
    "Do not retry blindly.";

/* The not-started shape: the turn opened, the crash cut before any cell
   audit — one line, no code quote. */
static const char _LIFE_NOT_STARTED[] =
    "The previous turn was interrupted before the cell started. No cell "
    "execution was recorded. Retry it if it is still needed.";

/* ------------------------------------------------------------------ */
/* Small heap-string + helpers                                         */
/* ------------------------------------------------------------------ */

static char* _life_dup(const char* s) {
  if (s == NULL) return NULL;
  size_t n = strlen(s);
  char* copy = get_memory(n + 1);
  memcpy(copy, s, n + 1);
  return copy;
}

/* A growable render buffer (the brief builds parts, then joins). */
typedef struct _life_buf_t {
  char* text;
  size_t len, capacity;
} _life_buf_t;

static void _life_buf_add(_life_buf_t* buf, const char* fmt, ...) {
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

/* Is `kind` a member of the reason union (the pinned LIFE_REASON_* set)? */
static int _life_kind_known(const char* kind) {
  static const char* const KNOWN[] = {
      LIFE_REASON_COMPLETED,  LIFE_REASON_ERROR,      LIFE_REASON_TURN_LIMIT,
      LIFE_REASON_INTERRUPTED, LIFE_REASON_ABORTED,   LIFE_REASON_BLOCKED,
      LIFE_REASON_DOOM_LOOP,
  };
  for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
    if (strcmp(kind, KNOWN[i]) == 0) return 1;
  }
  return 0;
}

/* A payload field as JSON int: 0 and *out set, or -1 when the field is
   absent, not an int, or NEGATIVE (the caller logs + skips the record —
   the render-not-crash rule; a negative number is the same corrupt-record
   class as a missing one — cast to the unsigned cursor it would wrap to a
   huge key, so it refuses exactly like an absent field; the skip's loud
   line names the shape, never the record's VALUE). */
static int _life_payload_int(json_value_t* payload, const char* key,
                             uint64_t* out) {
  json_value_t* v = json_get(payload, key);
  if (v == NULL || json_type(v) != JSON_INT) return -1;
  if (json_as_int(v) < 0) return -1;
  *out = (uint64_t) json_as_int(v);
  return 0;
}

/* ------------------------------------------------------------------ */
/* The payload composers (the engine's riders)                         */
/* ------------------------------------------------------------------ */

json_value_t* lifecycle_turn_start_json(uint64_t turn) {
  json_value_t* obj = json_new_object();
  json_object_set(obj, "turn", json_new_int((int64_t) turn));
  return obj;
}

json_value_t* lifecycle_step_json(uint64_t turn, uint64_t step) {
  json_value_t* obj = json_new_object();
  json_object_set(obj, "turn", json_new_int((int64_t) turn));
  json_object_set(obj, "step", json_new_int((int64_t) step));
  return obj;
}

json_value_t* lifecycle_turn_end_json(uint64_t turn, const char* kind,
                                      const char* text) {
  if (kind == NULL) {
    log_error("lifecycle_turn_end_json: NULL reason kind — refused loud");
    return NULL;
  }
  json_value_t* obj = json_new_object();
  json_object_set(obj, "turn", json_new_int((int64_t) turn));
  json_value_t* reason = json_new_object();
  json_object_set(reason, "kind", json_new_string(kind));
  /* text = the control kind's wording verbatim; NULL/"" renders the field
     absent — never an empty string (the frozen composer contract). */
  if (text != NULL && text[0] != '\0') {
    json_object_set(reason, "text", json_new_string(text));
  }
  json_object_set(obj, "reason", reason);
  return obj;
}

/* ------------------------------------------------------------------ */
/* The cursor fold                                                     */
/* ------------------------------------------------------------------ */

static void _life_cell_close(lifecycle_cursor_t* c) {
  free(c->inflight_code);
  c->inflight_code = NULL;
  c->inflight_seq = 0;
  c->inflight_corr = 0;
  c->cell_inflight = 0;
}

/* One KNOWN record's fold (the lifecycle types pair with the existing
   cell.run/cell.result pair — the fold's whole reason to read them). The
   record's seq+type were already read (and last_seq moved); `payload` is
   the record's payload DOM (may be anything — the handlers validate). */
static void _life_fold_record(size_t index, const char* type,
                              json_value_t* payload, uint64_t seq,
                              lifecycle_cursor_t* c) {
  uint64_t turn = 0;
  uint64_t step = 0;

  if (payload == NULL || json_type(payload) != JSON_OBJECT) {
    log_error("lifecycle_cursor_fold: lifecycle record %zu's payload is not "
              "an object — skipped loud", index);
    return;
  }

  if (strcmp(type, LIFE_EVENT_TURN_START) == 0) {
    if (_life_payload_int(payload, "turn", &turn) != 0) {
      log_error("lifecycle_cursor_fold: turn.start record %zu carries no "
                "non-negative int turn — skipped loud", index);
      return;
    }
    if (c->turn_open) {
      /* Spec §2's malformed-list: a second turn.start over an open turn is
         not a state the log can state honestly — loud + skipped, the fold
         survives with the open turn it already holds. */
      log_error("lifecycle_cursor_fold: record %zu (seq %llu) is a second "
                "turn.start over the open turn — skipped loud", index,
                (unsigned long long) seq);
      return;
    }
    c->turn = turn;
    c->turn_open = 1;
    c->step_open = 0;
    return;
  }

  if (strcmp(type, LIFE_EVENT_STEP_START) == 0) {
    if (_life_payload_int(payload, "turn", &turn) != 0 ||
        _life_payload_int(payload, "step", &step) != 0) {
      log_error("lifecycle_cursor_fold: step.start record %zu carries no "
                "non-negative int turn/step pair — skipped loud", index);
      return;
    }
    /* Numbers come from the records' OWN payloads (spec §2's tolerance
       pin): a step whose turn.start was refused folds fine — the step
       record carries the turn number, so the turn is open. */
    c->turn = turn;
    c->step = step;
    c->turn_open = 1;
    c->step_open = 1;
    return;
  }

  if (strcmp(type, LIFE_EVENT_STEP_END) == 0) {
    if (_life_payload_int(payload, "turn", &turn) != 0 ||
        _life_payload_int(payload, "step", &step) != 0) {
      log_error("lifecycle_cursor_fold: step.end record %zu carries no "
                "non-negative int turn/step pair — skipped loud", index);
      return;
    }
    /* A boundary record: the newest recorded numbers, the step closed, and
       the pending cell closed with it. */
    c->turn = turn;
    c->step = step;
    c->step_open = 0;
    _life_cell_close(c);
    return;
  }

  if (strcmp(type, LIFE_EVENT_TURN_END) == 0) {
    json_value_t* reason = json_get(payload, "reason");
    if (reason == NULL || json_type(reason) != JSON_OBJECT) {
      log_error("lifecycle_cursor_fold: turn.end record %zu carries no "
                "reason object — skipped loud", index);
      return;
    }
    json_value_t* kind_v = json_get(reason, "kind");
    if (kind_v == NULL || json_type(kind_v) != JSON_STRING ||
        !_life_kind_known(json_as_string(kind_v))) {
      /* Unknown kinds on replay fold loud (the render-not-crash rule: a
         malformed lifecycle record is never silently dropped — the close
         is only honored when the reason is one of the union's members). */
      log_error("lifecycle_cursor_fold: turn.end record %zu carries a reason "
                "kind outside the union — skipped loud", index);
      return;
    }
    json_value_t* text_v = json_get(reason, "text");
    if (text_v != NULL && json_type(text_v) != JSON_STRING &&
        json_type(text_v) != JSON_NULL) {
      log_error("lifecycle_cursor_fold: turn.end record %zu's reason text is "
                "neither a string nor absent — skipped loud", index);
      return;
    }
    if (_life_payload_int(payload, "turn", &turn) != 0) {
      log_error("lifecycle_cursor_fold: turn.end record %zu carries no "
                "non-negative int turn — skipped loud", index);
      return;
    }
    c->turn = turn;
    c->turn_open = 0;
    c->step_open = 0;
    _life_cell_close(c);   /* a boundary record closes the pending cell */
    return;
  }

  if (strcmp(type, LIFE_EVENT_REPAIR) == 0) {
    /* A stored repair brief is a PAST repair's record (the resume closers
       always end their batch with a turn.end). It moves nothing here — the
       following turn.end carries the state. */
    return;
  }

  if (strcmp(type, "cell.run") == 0) {
    /* The EXISTING record type (not a lifecycle constant — the engine's
       cell audit predates the envelope). Its payload pairs the fold. */
    uint64_t corr = 0;
    json_value_t* code_v = json_get(payload, "code");
    if (_life_payload_int(payload, "corr", &corr) != 0 ||
        code_v == NULL || json_type(code_v) != JSON_STRING) {
      log_error("lifecycle_cursor_fold: cell.run record %zu carries no "
                "non-negative int corr or no code — skipped loud", index);
      return;
    }
    if (c->cell_inflight) {
      /* A second cell.run over an open one is malformed (spec §2): loud +
         skipped — the fold survives with the FIRST cell in flight. */
      log_error("lifecycle_cursor_fold: record %zu (seq %llu) is a second "
                "cell.run over the open cell — skipped loud", index,
                (unsigned long long) seq);
      return;
    }
    c->cell_inflight = 1;
    c->inflight_seq = seq;
    c->inflight_corr = corr;
    free(c->inflight_code);
    c->inflight_code = _life_dup(json_as_string(code_v));
    return;
  }

  if (strcmp(type, "cell.result") == 0) {
    /* The EXISTING record type: pairs the pending cell.run by corr. */
    uint64_t corr = 0;
    if (_life_payload_int(payload, "corr", &corr) != 0) {
      log_error("lifecycle_cursor_fold: cell.result record %zu carries no "
                "non-negative int corr — skipped loud", index);
      return;
    }
    if (!c->cell_inflight) {
      /* orphan result — never silently dropped (the fold's loud rule) */
      log_error("lifecycle_cursor_fold: cell.result record %zu (corr %llu) "
                "has no open cell.run to pair — folded loud",
                index, (unsigned long long)corr);
      return;
    }
    if (corr != c->inflight_corr) {
      /* A mismatched result never acknowledges the open cell.run (the
         repair.spec:79-97 discipline, corr-mapped): the crash may have
         shuffled the pair's order, the fold trusts the run's own corr. */
      log_error("lifecycle_cursor_fold: cell.result record %zu (corr %llu) "
                "does not pair the open cell.run's corr %llu — not "
                "acknowledged", index, (unsigned long long) corr,
                (unsigned long long) c->inflight_corr);
      return;
    }
    _life_cell_close(c);
    return;
  }

  /* An unknown type never reaches this function. */
}

int lifecycle_cursor_fold(const char* events_array_json, lifecycle_cursor_t* c) {
  if (events_array_json == NULL || c == NULL) {
    log_error("lifecycle_cursor_fold: NULL input — refused loud");
    return -1;
  }
  /* The fold initializes the cursor (a re-fold overwrites prior state;
     the caller folds into a fresh cursor and destroys the folded heap). */
  memset(c, 0, sizeof(*c));

  char* jerr = NULL;
  json_value_t* array = json_parse(events_array_json, strlen(events_array_json),
                                   &jerr);
  if (array == NULL) {
    log_error("lifecycle_cursor_fold: the tail is not a JSON document (%s)",
              jerr != NULL ? jerr : "malformed");
    free(jerr);
    return -1;
  }
  if (json_type(array) != JSON_ARRAY) {
    log_error("lifecycle_cursor_fold: the tail is not a JSON array — refused "
              "loud");
    json_value_destroy(array);
    return -1;
  }

  for (size_t i = 0; i < json_size(array); i++) {
    json_value_t* element = json_at(array, i);
    if (element == NULL) {
      log_error("lifecycle_cursor_fold: tail element %zu is unreadable — "
                "skipped loud", i);
      continue;
    }

    /* Each element is ONE event record: its own object, or its text
       wrapped as a JSON string (the scan reply's joint form — the SAME
       contract refine_fold_parse's records ride as). */
    json_value_t* record = NULL;
    uint8_t owned = 0;
    if (json_type(element) == JSON_STRING) {
      const char* text = json_as_string(element);
      record = json_parse(text, strlen(text), &jerr);
      owned = 1;
      if (record == NULL) {
        log_error("lifecycle_cursor_fold: record %zu text is not a JSON "
                  "document (%s) — skipped loud", i,
                  jerr != NULL ? jerr : "malformed");
        free(jerr);
        jerr = NULL;
        continue;
      }
    } else if (json_type(element) == JSON_OBJECT) {
      record = element;
    } else {
      log_error("lifecycle_cursor_fold: tail element %zu is not a record "
                "document — skipped loud", i);
      continue;
    }

    /* The record contract: "seq" (JSON int — the log's own seq, the closer
       seqs' base, NON-negative) + "type" (the event's type name). A
       readable seq occupies a real log key, so it moves last_seq EVEN when
       the record's payload then skips — the closers' seqs can never collide
       with a stored record's key. */
    json_value_t* seq_v = json_get(record, "seq");
    json_value_t* type_v = json_get(record, "type");
    if (seq_v == NULL || json_type(seq_v) != JSON_INT ||
        type_v == NULL || json_type(type_v) != JSON_STRING) {
      log_error("lifecycle_cursor_fold: record %zu violates the event-record "
                "contract (a seq and a type are required) — skipped loud", i);
      if (owned) json_value_destroy(record);
      continue;
    }
    const char* type = json_as_string(type_v);
    if (json_as_int(seq_v) < 0) {
      /* A NEGATIVE seq is the same corrupt-record class: cast to the
         unsigned cursor it would move last_seq to a huge key and the
         closers' seqs would collide with nothing — so the record skips
         loud and last_seq stays at the prior real record. */
      log_error("lifecycle_cursor_fold: record %zu (%s) carries a negative "
                "seq — skipped loud, last_seq untouched", i, type);
      if (owned) json_value_destroy(record);
      continue;
    }
    uint64_t seq = (uint64_t) json_as_int(seq_v);
    c->last_seq = seq;

    /* Unknown types pass through (they move nothing). The fold also reads
       the EXISTING cell.run/cell.result records — they pair the in-flight
       cell; only the five lifecycle types carry the envelope's numbers. */
    if (strcmp(type, LIFE_EVENT_TURN_START) == 0 ||
        strcmp(type, LIFE_EVENT_STEP_START) == 0 ||
        strcmp(type, LIFE_EVENT_STEP_END) == 0 ||
        strcmp(type, LIFE_EVENT_TURN_END) == 0 ||
        strcmp(type, LIFE_EVENT_REPAIR) == 0 ||
        strcmp(type, "cell.run") == 0 ||
        strcmp(type, "cell.result") == 0) {
      _life_fold_record(i, type, json_get(record, "payload"), seq, c);
    }
    if (owned) json_value_destroy(record);
  }

  json_value_destroy(array);
  return 0;
}

void lifecycle_cursor_destroy(lifecycle_cursor_t* cursor) {
  if (cursor == NULL) return;
  free(cursor->inflight_code);
  cursor->inflight_code = NULL;
  /* inflight_demand is reserved (the shape-stability field): it stays NULL
     this slice, but the destroyer covers it so the field's lifecycle is
     already owned. */
  free(cursor->inflight_demand);
  cursor->inflight_demand = NULL;
}

/* ------------------------------------------------------------------ */
/* The brief (the two pinned shapes)                                   */
/* ------------------------------------------------------------------ */

/* The quoted code: verbatim, whitespace preserved; past the cap it
   truncates at source with "..." and a loud line — never silence. Malloc'd
   (when truncated) or NULL to mean "the caller quotes it as-is". */
static char* _life_quote_code(const lifecycle_cursor_t* cursor) {
  const char* code = (cursor->inflight_code != NULL) ? cursor->inflight_code
                                                     : "";
  size_t len = strlen(code);
  if (len <= (size_t) SA_LIFECYCLE_BRIEF_CODE_CHARS) return NULL;
  log_error("lifecycle_closers_compose: the in-flight cell's code (seq %llu) "
            "is %zu chars — the brief's quote truncates at the "
            "SA_LIFECYCLE_BRIEF_CODE_CHARS cap (%d)",
            (unsigned long long) cursor->inflight_seq, len,
            SA_LIFECYCLE_BRIEF_CODE_CHARS);
  size_t keep = (size_t) SA_LIFECYCLE_BRIEF_CODE_CHARS;
  char* quoted = get_memory(keep + 1);
  if (keep >= 3) {
    memcpy(quoted, code, keep - 3);
    memcpy(quoted + keep - 3, "...", 3);
  } else {
    memcpy(quoted, code, keep);
  }
  quoted[keep] = '\0';
  return quoted;
}

/* The brief's text (malloc'd, the composer frees): the STARTED shape when
   an open cell.run exists, the NOT-STARTED shape otherwise — an open turn
   ALWAYS briefs (the frozen contract), the shape names the recorded fact. */
static char* _life_brief_text(const lifecycle_cursor_t* cursor) {
  if (!cursor->cell_inflight) return _life_dup(_LIFE_NOT_STARTED);

  char seq_line[80];
  snprintf(seq_line, sizeof(seq_line), _LIFE_STARTED_DETAILS,
           (unsigned long long) cursor->inflight_seq);

  char* quoted = _life_quote_code(cursor);
  const char* code = (quoted != NULL) ? quoted
                    : ((cursor->inflight_code != NULL) ? cursor->inflight_code
                                                       : "");

  _life_buf_t buf = {NULL, 0, 0};
  _life_buf_add(&buf, "%s\n%s\n%s\n%s", _LIFE_STARTED_LEAD, seq_line, code,
                _LIFE_STARTED_FACTS);
  free(quoted);
  return buf.text;
}

/* ------------------------------------------------------------------ */
/* The closers (spec §4's order: repair, step.end, turn.end)           */
/* ------------------------------------------------------------------ */

int lifecycle_closers_compose(const lifecycle_cursor_t* cursor,
                              lifecycle_closers_t* out) {
  if (out != NULL) {
    out->items = NULL;
    out->n = 0;
  }
  if (cursor == NULL || out == NULL) {
    log_error("lifecycle_closers_compose: NULL input — refused loud");
    return -1;
  }
  /* Empty tail / balanced tail (no open turn) = {NULL, 0}, NO allocation.
     The balance gate is the fold's turn_open — an open turn is exactly a
     tail whose newest lifecycle record is not a turn.end. */
  if (!cursor->turn_open) return 0;

  /* 1. the repair brief (an open turn ALWAYS briefs) ;
     2. step.end (when open) ; 3. turn.end {reason: interrupted}. */
  size_t n = 1 + (size_t) cursor->step_open + 1;
  /* The frozen one-batch cap is BELT-AND-SUSPENDERS, not a live contract:
     the fold's shape bounds n at 3 (repair + optional step.end + turn.end),
     so this refusal is unreachable today. It stands for future closer
     shapes (e.g. multi-step turns) so a shape change can never exceed the
     batch cap silently. */
  if (n > SA_LIFECYCLE_MAX_CLOSERS) {
    log_error("lifecycle_closers_compose: %zu closers exceed the one-batch "
              "cap (SA_LIFECYCLE_MAX_CLOSERS = %d) — refused loud", n,
              SA_LIFECYCLE_MAX_CLOSERS);
    return -1;
  }

  lifecycle_closer_t* items = get_clear_memory(n * sizeof(*items));
  uint64_t seq = cursor->last_seq;
  size_t i = 0;

  items[i].type = LIFE_EVENT_REPAIR;
  items[i].seq = ++seq;
  char* text = _life_brief_text(cursor);
  json_value_t* payload = json_new_object();
  json_object_set(payload, "turn", json_new_int((int64_t) cursor->turn));
  json_object_set(payload, "text", json_new_string(text));
  free(text);
  items[i].payload = payload;
  i++;

  if (cursor->step_open) {
    items[i].type = LIFE_EVENT_STEP_END;
    items[i].seq = ++seq;
    items[i].payload = lifecycle_step_json(cursor->turn, cursor->step);
    i++;
  }

  items[i].type = LIFE_EVENT_TURN_END;
  items[i].seq = ++seq;
  items[i].payload = lifecycle_turn_end_json(cursor->turn,
                                             LIFE_REASON_INTERRUPTED, NULL);
  i++;

  out->items = items;
  out->n = n;
  return 0;
}

void lifecycle_closers_destroy(lifecycle_closers_t* closers) {
  if (closers == NULL) return;
  for (size_t i = 0; i < closers->n; i++) {
    json_value_destroy(closers->items[i].payload);
  }
  free(closers->items);
  closers->items = NULL;
  closers->n = 0;
}
