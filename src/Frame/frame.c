//
// Created by victor on 9/29/26.
//

#include "frame.h"
#include "frame_messages.h"
#include "frame_bridge.h"
#include "../Util/allocator.h"
#include "../Util/json.h"
#include "../Util/log.h"

/* Payload destroyers (frame_messages.h contract). These are pure C and must
   stay available even in a wavedb-free build — the pyrt bridge (Task 7) may
   still allocate the payload structs it posts at frames. */
void frm_remember_payload_destroy(void* p) {
  frm_remember_payload_t* rp = (frm_remember_payload_t*)p;
  if (rp == NULL) return;
  free(rp->key);
  free(rp->json_value);
  free(rp);
}

void frm_reply_payload_destroy(void* p) {
  frm_reply_payload_t* rp = (frm_reply_payload_t*)p;
  if (rp == NULL) return;
  free(rp->text);
  free(rp);
}

void frm_spawn_payload_destroy(void* p) {
  frm_spawn_payload_t* sp = (frm_spawn_payload_t*)p;
  if (sp == NULL) return;
  free(sp->goal);
  free(sp->context_json);
  free(sp);
}

void frm_report_payload_destroy(void* p) {
  frm_report_payload_t* rp = (frm_report_payload_t*)p;
  if (rp == NULL) return;
  free(rp->text);
  free(rp);
}

#ifdef SA_HAS_WDB

#include <Database/database.h>
#include <Database/database_subtree.h>
#include <Database/database_iterator.h>
#include <HBTrie/path.h>
#include <Layers/graph/graph.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* One effect = one root batch. A concurrent-mode batch that exceeds the WAL
   file size is REJECTED by WaveDB (default cap 128 KB); we refuse anything
   approaching that limit up front so rejection is loud and early. */
#define SA_FRAME_MAX_BATCH_BYTES (120 * 1024)

/* sid layout: 8 hex chars = 20 random bits (top) | 10 counter bits (bits
   2-11; the 0xFFC mask forces the low 2 bits to 0). That leaves ~1024
   distinct counter slots per root, so uniqueness rests on the random top
   bits — the counter merely staggers siblings born between rng draws. */
#define SA_FRAME_SID_HEX_LEN 8

/* Status key values (meta/status). frame_create stamps "running"; Task 5's
   report marks "done" and join only logs (a joined child has already done-
   reported). Task 10's loop marks "done" on end-turn. */
static const char SA_FRAME_STATUS_RUNNING[] = "running";
static const char SA_FRAME_STATUS_DONE[] = "done";

/* Reserved subtree + graph layer that holds ONE lineage index per ROOT db:
   triples (child_sid_path, "parent_of", parent_sid_path) as SPO/POS/OSP/PSO
   presence keys (no schema → all four indices). The layer rides cross-subtree
   via graph_triple_expand_ops: its index ops are FULL root-database paths, so
   they merge into the very same one atomic root batch as the frame's own
   event/meta/state writes (graph.h:71-86). Built eagerly at wave_db_open —
   lazy init would race between scheduler-pooled frames. */
#define SA_FRAME_LINEAGE_SUBTREE "lineage"
#define SA_FRAME_LINEAGE_PREDICATE "parent_of"

/* frame_debug_events materialization bound: newest 512 records max. */
#define SA_FRAME_DEBUG_MAX_EVENTS 512

struct wave_database_root_t {
  database_t* db;             /* the ONE root database */
  graph_layer_t* lineage;     /* subtree-mode graph layer over lineage_st */
  database_subtree_t* lineage_st;  /* reserved "lineage" subtree (open for life) */
  uint32_t rng;               /* xorshift32 state for sid randomness */
  ATOMIC(uint64_t) counter;   /* sid uniqueness counter */
};

struct frame_t {
  actor_t actor;              /* FIRST member (style guide) */
  wave_database_root_t* root;
  frame_t* parent;            /* borrowed; NULL for top-level frames */
  database_subtree_t* st;     /* subtree prefix = sid_path (open for life) */
  char* sid_path;             /* full path from root, e.g. "sessions/<hex>" */
  char* parent_path;          /* parent's sid_path; NULL for root */
  char* goal;                 /* owned copy; NULL allowed */
  char* model_base_url;       /* owned copies of the config strings */
  char* model_api_key;
  char* model_name;
  unsigned max_depth;
  uint64_t seq;               /* last allocated event seq (0 = none yet) */
  uint32_t depth;
};

/* --- bridge reply hook (frame_bridge.h contract) ---------------------------
   The dispatch-side half: behaviors hand corr-matched answers to the installed
   sink; the python side (py_agent.c, Task 7) registers
   py_agent_note_reply(...) as that sink at runtime init. Until then the
   default sink DROPS replies loudly. */
static ATOMIC(frame_bridge_reply_fn_t) _frame_bridge_sink;
/* Debug shadow of the last reply handed to ANY sink incl. the dropper: a
   test/debug record, not synchronization state. */
static ATOMIC(uint8_t) _frame_bridge_last_seen;
static ATOMIC(uint64_t) _frame_bridge_last_corr;
static ATOMIC(uint8_t) _frame_bridge_last_status;

void frame_bridge_register_reply_sink(frame_bridge_reply_fn_t sink) {
  ATOMIC_STORE(&_frame_bridge_sink, sink);
}

/* The default sink until Task 7 installs py_agent_note_reply: DROPS the
   reply, loud — a dropped reply leaves a python `agent.*` caller blocked
   until its (bounded) timeout, which must never happen silently. */
static void _frame_bridge_drop(uint64_t corr, uint8_t status, const char* text) {
  log_error("frame_bridge: no bridge registered — DROPPING reply corr %llu "
            "status %u '%s' (a registered sink would unblock a python wait; "
            "dropped replies time out instead)",
            (unsigned long long)corr, (unsigned)status, text ? text : "(no text)");
}

/* The single reply path of the frame behaviors: record the debug shadow, then
   deliver through the installed sink (or the loud dropper). Called on the
   frame's dispatch thread; `text` is borrowed for the call only. */
static void _frame_bridge_reply(uint64_t corr, uint8_t status, const char* text) {
  ATOMIC_STORE(&_frame_bridge_last_corr, corr);
  ATOMIC_STORE(&_frame_bridge_last_status, status);
  ATOMIC_STORE(&_frame_bridge_last_seen, 1);
  frame_bridge_reply_fn_t sink = ATOMIC_LOAD(&_frame_bridge_sink);
  if (sink != NULL) {
    sink(corr, status, text);
    return;
  }
  _frame_bridge_drop(corr, status, text);
}

uint8_t frame_bridge_debug_last(uint64_t* corr_out, uint8_t* status_out) {
  if (corr_out != NULL) *corr_out = ATOMIC_LOAD(&_frame_bridge_last_corr);
  if (status_out != NULL) *status_out = ATOMIC_LOAD(&_frame_bridge_last_status);
  return ATOMIC_LOAD(&_frame_bridge_last_seen);
}

/* --- small helpers ------------------------------------------------------ */

/* xorshift32 over a per-root state; seeded once at wave_db_open. The plan
   asks for platform randomness for sids; a libc rand() call is process-wide
   and not thread-safe for frames created from scheduler workers, so the
   platform's "random" contribution is this time/address-seeded generator,
   and uniqueness comes from the per-root counter. */
static uint32_t _root_rng_next(wave_database_root_t* root) {
  uint32_t x = root->rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  root->rng = x;
  return x;
}

static void _frame_sid_generate(wave_database_root_t* root, char out[9]) {
  uint32_t bits = (_root_rng_next(root) & 0xFFFFF000u);
  bits |= (uint32_t)(atomic_fetch_add(&root->counter, 1) & 0xFFCu);
  snprintf(out, 9, "%08x", (unsigned)bits);
}

/* UTC ISO-8601 wall clock for the event record's "at" field. */
static void _frame_iso_now(char out[25]) {
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

/* Split a '/'-delimited string into a path_t for iterator bounds (relative
   to a subtree). Empty trailing components are dropped, matching WaveDB's
   own database_scan_range semantics. */
static path_t* _frame_path_from(const char* s) {
  return path_create_from_raw(s, strlen(s), '/', 0);
}

/* Get a text value from a subtree: malloc'd NUL-terminated copy, or NULL
   (not found / error). Caller frees with free(). */
static char* _frame_subtree_text(database_subtree_t* st, const char* key) {
  uint8_t* val = NULL;
  size_t val_len = 0;
  int rc = database_subtree_get_sync_raw(st, key, strlen(key), '/', &val, &val_len);
  if (rc != 0 || val == NULL) return NULL;
  char* text = get_memory(val_len + 1);
  if (text == NULL) {
    free(val);
    return NULL;
  }
  memcpy(text, val, val_len);
  text[val_len] = '\0';
  database_raw_value_free(val);
  return text;
}

/* Event record JSON per the frozen shape: {"seq","type","frame","corr","at",
   "cause","payload"}. Bridge events carry a corr (Task 6 will use it); a
   direct store write has none, so "corr" is JSON null in this slice. "cause"
   = the seq of the RECORDING frame's PREVIOUS event (the audit chain); JSON
   null only when seq <= 1. `frame_path` names the recording frame's subtree —
   a report/bind writes the child's record with the child's path + seq and a
   separate parent's record with the parent's, so the path is explicit.
   Consumes `payload`. */
static char* _frame_event_json_full(const char* frame_path, uint64_t seq,
                                    const char* type_name, json_value_t* payload) {
  char iso[25];
  _frame_iso_now(iso);
  json_value_t* rec = json_new_object();
  if (rec == NULL) {
    json_value_destroy(payload);
    log_error("frame: out of memory building event record");
    return NULL;
  }
  json_object_set(rec, "seq", json_new_int((int64_t)seq));
  json_object_set(rec, "type", json_new_string(type_name));
  json_object_set(rec, "frame", json_new_string(frame_path));
  json_object_set(rec, "corr", json_new_null());
  json_object_set(rec, "at", json_new_string(iso));
  if (seq <= 1) {
    json_object_set(rec, "cause", json_new_null());
  } else {
    json_object_set(rec, "cause", json_new_int((int64_t)(seq - 1)));
  }
  json_object_set(rec, "payload", payload);
  char* text = json_serialize(rec);
  json_value_destroy(rec);
  return text;
}

/* Convenience for the recording frame itself. */
static char* _frame_event_json(frame_t* f, uint64_t seq, const char* type_name,
                               json_value_t* payload) {
  return _frame_event_json_full(f->sid_path, seq, type_name, payload);
}

/* Compose "<sid_path>/<rel>" — a key whose FULL root-database path is usable
   in a cross-subtree root batch (malloc'd, free() it). */
static char* _frame_subkey(const char* sid_path, const char* rel) {
  size_t len = strlen(sid_path) + 1 + strlen(rel);
  char* out = get_memory(len + 1);
  if (out == NULL) {
    log_error("frame: out of memory composing '%s/%s'", sid_path, rel);
    return NULL;
  }
  snprintf(out, len + 1, "%s/%s", sid_path, rel);
  return out;
}

/* Compose "<sid_path>/events/<%020llu seq>" (malloc'd, free() it). */
static char* _frame_event_key(const char* sid_path, uint64_t seq) {
  char evkey[32];
  snprintf(evkey, sizeof(evkey), "events/%020llu", (unsigned long long)seq);
  return _frame_subkey(sid_path, evkey);
}

static int _frame_key_valid(const char* key, const char* op) {
  if (key == NULL || key[0] == '\0' || strchr(key, '/') != NULL) {
    log_error("frame: %s key must be non-empty and free of '/': '%s'",
              op, key ? key : "(null)");
    return 0;
  }
  return 1;
}

static json_value_t* _frame_parse_or_null(const char* json_text) {
  char* err = NULL;
  json_value_t* v = json_parse(json_text, strlen(json_text), &err);
  if (v == NULL) {
    log_error("frame: value is not valid JSON (%s)", err ? err : "parse failed");
  }
  if (err) free(err);
  return v;
}

/* Compose "state/local/<key>" / "state/ctx/<key>" (malloc'd, free() it). */
static char* _frame_state_key(const char* state_prefix, const char* key) {
  size_t len = strlen(state_prefix) + strlen(key);
  char* out = get_memory(len + 1);
  if (out == NULL) {
    log_error("frame: out of memory composing state key");
    return NULL;
  }
  snprintf(out, len + 1, "%s%s", state_prefix, key);
  return out;
}

/* remember = the state put + the state.remember event in ONE root batch.
   Failures happen BEFORE any write except the batch itself, so no effect
   half-applies. */
static int _frame_remember_variant(frame_t* f, const char* key, const char* json_value,
                                   const char* state_prefix) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: remember on a dead frame");
    return -1;
  }
  if (!_frame_key_valid(key, "remember")) return -1;
  if (json_value == NULL) {
    log_error("frame: remember '%s' needs a JSON value", key);
    return -1;
  }

  json_value_t* parsed = _frame_parse_or_null(json_value);
  if (parsed == NULL) return -1;

  uint64_t seq = f->seq + 1;
  size_t state_len = strlen(state_prefix) + strlen(key);
  if (state_len > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame: remember '%s' — prefixed state key %zu chars + %zu-byte "
              "value exceeds the %d-byte WAL batch cap — refusing, never "
              "truncating",
              key, state_len, (size_t)strlen(json_value),
              (int)SA_FRAME_MAX_BATCH_BYTES);
    json_value_destroy(parsed);
    return -3;
  }

  json_value_t* payload = json_new_object();   /* {key, value} */
  if (payload == NULL) {
    json_value_destroy(parsed);
    log_error("frame: out of memory building remember payload");
    return -1;
  }
  json_object_set(payload, "key", json_new_string(key));
  json_object_set(payload, "value", parsed);

  char* state_key = get_memory(state_len + 1);
  char* evkey = get_memory(sizeof("events/00000000000000000000"));
  char* text = _frame_event_json(f, seq, "state.remember", payload);
  if (state_key == NULL || evkey == NULL || text == NULL) {
    free(state_key);
    free(evkey);
    free(text);
    log_error("frame: out of memory building remember batch");
    return -1;
  }
  snprintf(evkey, sizeof("events/00000000000000000000"), "events/%020llu",
           (unsigned long long)seq);
  snprintf(state_key, state_len + 1, "%s%s", state_prefix, key);

  size_t text_len = strlen(text);
  if (text_len > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame: event record %zu bytes exceeds the %d-byte WAL batch cap "
              "— refusing, never truncating (trim the value)",
              text_len, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(state_key);
    free(evkey);
    free(text);
    return -3;
  }

  raw_op_t ops[2];
  ops[0].key = evkey;
  ops[0].key_len = strlen(evkey);
  ops[0].value = (const uint8_t*)text;
  ops[0].value_len = text_len;
  ops[0].type = 0;
  ops[1].key = state_key;
  ops[1].key_len = state_len;
  ops[1].value = (const uint8_t*)json_value;
  ops[1].value_len = strlen(json_value);
  ops[1].type = 0;

  int rc = database_subtree_batch_sync_raw(f->st, '/', ops, 2);
  free(text);
  free(state_key);
  free(evkey);
  if (rc != 0) {
    log_error("frame: remember batch failed (%d); '%s' of %s is unwritten",
              rc, key, f->sid_path);
    return rc;
  }
  f->seq = seq;
  return 0;
}

/* msg.append = the conversation-turn event in ONE root batch. */
static int _frame_append_msg(frame_t* f, const char* role, const char* content) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: append_msg on a dead frame");
    return -1;
  }
  if (role == NULL || content == NULL) {
    log_error("frame: append_msg needs role and content");
    return -1;
  }

  json_value_t* payload = json_new_object();   /* {role, content} */
  if (payload == NULL) {
    log_error("frame: out of memory building msg.append payload");
    return -1;
  }
  json_object_set(payload, "role", json_new_string(role));
  json_object_set(payload, "content", json_new_string(content));

  uint64_t seq = f->seq + 1;
  char* evkey = get_memory(sizeof("events/00000000000000000000"));
  char* text = _frame_event_json(f, seq, "msg.append", payload);
  if (evkey == NULL || text == NULL) {
    free(evkey);
    free(text);
    log_error("frame: out of memory building msg.append batch");
    return -1;
  }
  snprintf(evkey, sizeof("events/00000000000000000000"), "events/%020llu",
           (unsigned long long)seq);

  size_t text_len = strlen(text);
  if (text_len > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame: event record %zu bytes exceeds the %d-byte WAL batch cap "
              "— refusing, never truncating (split the message)",
              text_len, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(evkey);
    free(text);
    return -3;
  }

  raw_op_t ops[1];
  ops[0].key = evkey;
  ops[0].key_len = strlen(evkey);
  ops[0].value = (const uint8_t*)text;
  ops[0].value_len = text_len;
  ops[0].type = 0;

  int rc = database_subtree_batch_sync_raw(f->st, '/', ops, 1);
  free(evkey);
  free(text);
  if (rc != 0) {
    log_error("frame: msg.append batch failed (%d); seq %llu of %s is unwritten",
              rc, (unsigned long long)seq, f->sid_path);
    return rc;
  }
  f->seq = seq;
  return 0;
}

/* Dispatch a queued FRM_* message. The behaviors run the SAME synchronous
   store functions every caller uses (one batch per effect) and answer
   corr-matched THROUGH the bridge hook (frame_bridge.h), never by queueing at
   an actor: the awaiting python cell blocks on its own completion record,
   woken synchronously from this dispatch. The frame never blocks beyond a µs
   batch.

   Payload ownership: FRM_REMEMBER / FRM_RECALL payloads are CONSUMED here
   (msg->payload = NULL; the behavior destroys the payload itself), which
   makes both delivery paths (queue + actor_run, and the tests' direct
   frame_dispatch) work without claiming it twice. FRM_REPLY is the OUTGOING
   answer shape, so one arriving at a frame is a routing bug. The loop's
   verbs (FRM_CELL_EXECUTE / FRM_STOP) and the cell-side spawn/report bridge
   verbs (FRM_SPAWN / FRM_REPORT — posted by py_agent.c) stay mailbox-cleaned
   until the slices that drive them claim them. Unknown types are ignored
   likewise. */
static void _frame_behavior(void* state, message_t* msg) {
  frame_t* f = (frame_t*)state;
  if (msg == NULL) return;
  switch (msg->type) {
    case FRM_REMEMBER: {
      frm_remember_payload_t* rp = (frm_remember_payload_t*)msg->payload;
      msg->payload = NULL;
      uint64_t corr = 0;
      uint8_t status;
      if (rp == NULL) {
        status = 1;
        log_error("frame: FRM_REMEMBER with no payload at '%s'", f->sid_path);
      } else if ((corr = rp->corr) == 0) {
        status = 1;
        log_error("frame: FRM_REMEMBER with corr 0 at '%s' — nothing to match",
                  f->sid_path);
      } else if (frame_remember_ctx(f, rp->key, rp->json_value) != 0) {
        /* The store already validated key + JSON + batch cap ("the same
           validation as every remember") and failed LOUDLY before writing. */
        status = 1;
        log_error("frame: FRM_REMEMBER '%s' refused at '%s' (corr %llu)",
                  rp->key ? rp->key : "(null)", f->sid_path,
                  (unsigned long long)corr);
      } else {
        status = 0;
      }
      /* Bridge verbs write the INHERITABLE layer: a cell's remember() is a
         durable, shared-by-default write (the frame_remember_ctx call above);
         cells use the ctx/local split only through the store API directly. */
      _frame_bridge_reply(corr, status, NULL);
      frm_remember_payload_destroy(rp);
      break;
    }
    case FRM_RECALL: {
      frm_remember_payload_t* rp = (frm_remember_payload_t*)msg->payload;
      msg->payload = NULL;
      uint64_t corr = 0;
      uint8_t status;
      char* text = NULL;
      if (rp == NULL) {
        status = 1;
        log_error("frame: FRM_RECALL with no payload at '%s'", f->sid_path);
      } else {
        corr = rp->corr;
        if (corr != 0 && (text = frame_recall(f, rp->key)) != NULL) {
          status = 0;
        } else if (corr != 0) {
          status = 1;
          log_error("frame: FRM_RECALL '%s' unresolvable from '%s' (corr %llu)",
                    rp->key ? rp->key : "(null)", f->sid_path,
                    (unsigned long long)corr);
        } else {
          status = 1;
          log_error("frame: FRM_RECALL with corr 0 at '%s' — nothing to match",
                    f->sid_path);
        }
      }
      _frame_bridge_reply(corr, status, text);
      free(text);
      frm_remember_payload_destroy(rp);
      break;
    }
    case FRM_REPLY:
      /* Replies LEAVE frames through the bridge hook; a frame cannot wait on
         one. This only happens from a routing bug: loud, dropped. */
      log_error("frame: FRM_REPLY received at '%s' — replies are bridge-hook "
                "side effects, not queued messages; dropping", f->sid_path);
      break;
    case FRM_CELL_EXECUTE:
    case FRM_STOP:
      /* The turn loop claims FRM_CELL_EXECUTE / FRM_STOP in Task 10. Until
         then the frame ignores them and payload cleanup stays with the
         mailbox per the style guide. */
      break;
    default:
      break;
  }
}

/* Test/synchronous entry point (frame.h): route a message through the SAME
   behavior path the scheduler drives via the embedded actor's mailbox. */
void frame_dispatch(frame_t* f, message_t* msg) {
  if (f == NULL || f->st == NULL) {
    log_error("frame_dispatch: dead frame");
    return;
  }
  _frame_behavior(f, msg);
}

/* Compose the frame's events-range bounds as ABSOLUTE root-database paths for
   database_scan_start/_reverse. WaveDB's subtree bounded scans are broken in
   BOTH directions (the subtree iterator compares STRIPPED result paths against
   the FULL stored bound copies, so subtree-relative bounds never match) —
   EVERY scan in this file is root-level with composed absolute bounds, the
   canonical `_frame_restore_seq` pattern. On success the start/end outputs
   are consumed by the subsequent database_scan_start/_reverse call (which owns
   them); the caller therefore just passes them in and never destroys them.
   Returns 0, or -1 on OOM (both paths freed, outputs NULLed). Consumed by the
   subsequent database_scan_start/_reverse call, which owns the paths. */
static int _frame_events_bounds(frame_t* f, path_t** start, path_t** end) {
  size_t base = strlen(f->sid_path);
  char* lo = get_memory(base + sizeof("/events"));
  char* hi = get_memory(base + sizeof("/events0"));
  if (lo == NULL || hi == NULL) {
    free(lo);
    free(hi);
    *start = NULL;
    *end = NULL;
    return -1;
  }
  snprintf(lo, base + sizeof("/events"), "%s/events", f->sid_path);
  snprintf(hi, base + sizeof("/events0"), "%s/events0", f->sid_path);
  *start = _frame_path_from(lo);
  *end = _frame_path_from(hi);
  free(lo);
  free(hi);
  if (*start == NULL || *end == NULL) {
    if (*start != NULL) path_destroy(*start);
    if (*end != NULL) path_destroy(*end);
    *start = NULL;
    *end = NULL;
    return -1;
  }
  return 0;
}

/* Largest event seq ever written under events/ — 0 when fresh. Reverse scan
   (root-level, absolute composed bounds) over the frame's bounded events
   range takes the first (largest) key; the seq counter continues past it. */
static uint64_t _frame_restore_seq(frame_t* f) {
  path_t* start = NULL;
  path_t* end = NULL;
  if (_frame_events_bounds(f, &start, &end) != 0) {
    log_error("frame: out of memory composing boot scan bounds");
    return 0;
  }
  database_iterator_t* iter = database_scan_start_reverse(f->root->db, start, end);
  if (iter == NULL) {
    path_destroy(start);
    path_destroy(end);
    log_error("frame: reverse scan failed booting '%s'", f->sid_path);
    return 0;
  }

  uint64_t largest = 0;
  path_t* key = NULL;
  identifier_t* value = NULL;
  int rc = database_scan_prev(iter, &key, &value);
  if (rc == 0 && key != NULL && value != NULL && path_length(key) >= 2) {
    identifier_t* last = path_get(key, path_length(key) - 1);
    size_t len = 0;
    uint8_t* data = identifier_get_data_copy(last, &len);
    if (data != NULL) {
      char digits[21];
      size_t n = len < 20 ? len : 20;
      memcpy(digits, data, n);
      digits[n] = '\0';
      largest = strtoull(digits, NULL, 10);
      free(data);
    }
  }
  if (key != NULL) path_destroy(key);
  if (value != NULL) identifier_destroy(value);
  database_scan_end(iter);
  return largest;
}

/* --- lifecycle ----------------------------------------------------------- */

wave_database_root_t* wave_db_open(const char* location) {
  database_config_t* wcfg = database_config_default();
  if (wcfg == NULL) {
    log_error("wave_db_open: no config memory");
    return NULL;
  }
  /* Concurrent mode is the frame tree's ground state (sync_only does not
     compose with multi-actor access); always explicit. */
  database_config_set_sync_only(wcfg, 0);
  if (location == NULL) database_config_set_enable_persist(wcfg, 0);

  int err = 0;
  database_t* db = database_create_with_config(location, wcfg, &err);
  database_config_destroy(wcfg);
  if (db == NULL) {
    log_error("wave_db_open: database_create_with_config failed (err %d)", err);
    return NULL;
  }

  wave_database_root_t* root = get_clear_memory(sizeof(wave_database_root_t));
  if (root == NULL) {
    database_destroy(db);
    return NULL;
  }
  root->db = db;
  root->rng = (uint32_t)((uintptr_t)db ^ (uint32_t)time(NULL) ^ 0x9e3779b9u);
  if (root->rng == 0) root->rng = 0x2545f491u;
  atomic_store(&root->counter, 0);

  /* Lineage graph layer: ONE per root db, in the reserved "lineage" subtree.
     Subtree mode keeps it namespace-isolated (no layer-type collision at root
     scope) and makes graph_triple_expand_ops emit FULL root-database paths, so
     triple index ops merge into the same one atomic root batch as the frame
     writes. Created EAGERLY — a lazy init would race between frames pooled on
     scheduler workers. Sub-optional layer availability is not fatal here
     (nothing needs lineage until a spawn); frame_spawn fails loudly instead. */
  root->lineage_st = database_subtree_open(root->db, SA_FRAME_LINEAGE_SUBTREE, '/');
  if (root->lineage_st != NULL) {
    int gerr = 0;
    root->lineage = graph_layer_create(NULL, NULL, root->lineage_st, &gerr);
    if (root->lineage == NULL) {
      log_error("wave_db_open: lineage graph layer create failed (%d) — "
                "spawns will be refused", gerr);
      database_subtree_close(root->lineage_st);
      root->lineage_st = NULL;
    }
  }

  return root;
}

void wave_db_close(wave_database_root_t* root) {
  if (root == NULL) return;
  if (root->lineage != NULL) {
    /* Layer teardown: schema snapshot + drop the layer's lineage-subtree
       reference. It never destroys the shared database (subtree mode). */
    graph_layer_destroy(root->lineage);
    root->lineage = NULL;
  }
  if (root->lineage_st != NULL) {
    /* Our own reference is the LAST subtree ref; closing it runs
       database_destroy inside the subtree once — one deref of the shared db,
       never a double free (database_destroy is refcount-aware). */
    database_subtree_close(root->lineage_st);
    root->lineage_st = NULL;
  }
  database_destroy(root->db);
  root->db = NULL;
  free(root);
}

/* Allocate and initialize a frame struct + its (unwritten) subtree:
   sid generation, composed sid_path, parent linkage, config (explicit cfg
   wins; otherwise inherit the parent's model config + depth budget),
   subtree open, seq restore, inline actor. The BIRTH META BATCH is NOT
   this function's business — frame_create writes it via the subtree
   wrapper; frame_spawn folds it into the ONE root admission batch.
   frame_spawn passes cfg=NULL to inherit; frame_create always passes the
   caller's (possibly NULL) cfg. */
static frame_t* _frame_alloc(wave_database_root_t* root, frame_t* parent,
                             const char* goal, const frame_config_t* cfg) {
  if (root == NULL) {
    log_error("frame: NULL root");
    return NULL;
  }

  frame_t* f = get_clear_memory(sizeof(frame_t));
  if (f == NULL) return NULL;
  f->root = root;

  if (parent == NULL) {
    char hex[SA_FRAME_SID_HEX_LEN + 1];
    _frame_sid_generate(root, hex);
    size_t root_len = strlen("sessions/") + SA_FRAME_SID_HEX_LEN;
    f->sid_path = get_memory(root_len + 1);
    if (f->sid_path == NULL) goto fail;
    snprintf(f->sid_path, root_len + 1, "sessions/%s", hex);
    f->depth = 0;
  } else {
    /* Child subtree path composed at the ROOT: <parent full path>/frames/<hex>.
       Arbitrary depth is path composition — never nested subtrees. */
    char hex[SA_FRAME_SID_HEX_LEN + 1];
    _frame_sid_generate(root, hex);
    size_t plen = strlen(parent->sid_path);
    size_t path_len = plen + strlen("/frames/") + SA_FRAME_SID_HEX_LEN;
    f->sid_path = get_memory(path_len + 1);
    if (f->sid_path == NULL) goto fail;
    snprintf(f->sid_path, path_len + 1, "%s/frames/%s", parent->sid_path, hex);
    f->parent_path = strdup(parent->sid_path);
    if (f->parent_path == NULL) goto fail;
    f->parent = parent;         /* borrowed: report/join bind via it */
    f->depth = parent->depth + 1;
  }

  if (cfg != NULL) {
    f->max_depth = (cfg->max_depth > 0) ? cfg->max_depth : 4;
    if (cfg->model_base_url != NULL) {
      f->model_base_url = strdup(cfg->model_base_url);
      if (f->model_base_url == NULL) goto fail;
    }
    if (cfg->model_api_key != NULL) {
      f->model_api_key = strdup(cfg->model_api_key);
      if (f->model_api_key == NULL) goto fail;
    }
    if (cfg->model_name != NULL) {
      f->model_name = strdup(cfg->model_name);
      if (f->model_name == NULL) goto fail;
    }
  } else if (parent != NULL) {
    /* Spawned children inherit the parent's depth budget and model config. */
    f->max_depth = parent->max_depth;
    if (parent->model_base_url != NULL) {
      f->model_base_url = strdup(parent->model_base_url);
      if (f->model_base_url == NULL) goto fail;
    }
    if (parent->model_api_key != NULL) {
      f->model_api_key = strdup(parent->model_api_key);
      if (f->model_api_key == NULL) goto fail;
    }
    if (parent->model_name != NULL) {
      f->model_name = strdup(parent->model_name);
      if (f->model_name == NULL) goto fail;
    }
  } else {
    f->max_depth = 4;
  }

  if (goal != NULL) {
    f->goal = strdup(goal);
    if (f->goal == NULL) goto fail;
  }

  f->st = database_subtree_open(root->db, f->sid_path, '/');
  if (f->st == NULL) {
    log_error("frame: subtree open failed for '%s'", f->sid_path);
    goto fail;
  }

  /* Boot: continue the seq counter past any persisted events (restart-safe).
     (No events in this task — Task 10's restart/replay test depends on this.) */
  f->seq = _frame_restore_seq(f);

  /* Inline actor (pool NULL): tests/loop pump the mailbox by hand. */
  actor_init(&f->actor, f, _frame_behavior, NULL);
  return f;

fail:
  if (f->st != NULL) database_subtree_close(f->st);
  free(f->sid_path);
  free(f->parent_path);
  free(f->goal);
  free(f->model_base_url);
  free(f->model_api_key);
  free(f->model_name);
  free(f);
  return NULL;
}

frame_t* frame_create(wave_database_root_t* root, frame_t* parent,
                      const char* goal, const frame_config_t* cfg) {
  frame_t* f = _frame_alloc(root, parent, goal, cfg);
  if (f == NULL) return NULL;

  /* Birth batch: ONE atomic batch with the frame's meta keys. Loop-driven
     frames start "running"; they end via the loop (Task 10). */
  {
    char iso[25];
    _frame_iso_now(iso);
    char depth_buf[11];
    snprintf(depth_buf, sizeof(depth_buf), "%u", f->depth);
    raw_op_t ops[4];
    size_t nops = 0;
    ops[nops].key = "meta/created";
    ops[nops].key_len = strlen("meta/created");
    ops[nops].value = (const uint8_t*)iso;
    ops[nops].value_len = strlen(iso);
    ops[nops].type = 0;
    nops++;
    ops[nops].key = "meta/status";
    ops[nops].key_len = strlen("meta/status");
    ops[nops].value = (const uint8_t*)SA_FRAME_STATUS_RUNNING;
    ops[nops].value_len = strlen(SA_FRAME_STATUS_RUNNING);
    ops[nops].type = 0;
    nops++;
    ops[nops].key = "meta/depth";
    ops[nops].key_len = strlen("meta/depth");
    ops[nops].value = (const uint8_t*)depth_buf;
    ops[nops].value_len = strlen(depth_buf);
    ops[nops].type = 0;
    nops++;
    if (f->parent_path != NULL) {
      ops[nops].key = "meta/parent";
      ops[nops].key_len = strlen("meta/parent");
      ops[nops].value = (const uint8_t*)f->parent_path;
      ops[nops].value_len = strlen(f->parent_path);
      ops[nops].type = 0;
      nops++;
    }
    int rc = database_subtree_batch_sync_raw(f->st, '/', ops, nops);
    if (rc != 0) {
      log_error("frame_create: meta batch failed (%d) for '%s'", rc, f->sid_path);
      frame_destroy(f);
      return NULL;
    }
  }

  return f;
}

const char* frame_sid(const frame_t* f) {
  return f ? f->sid_path : NULL;
}

uint8_t frame_is_done(const frame_t* f) {
  if (f == NULL || f->st == NULL) return 0;
  char* status = _frame_subtree_text(f->st, "meta/status");
  if (status == NULL) return 0;
  int done = (strcmp(status, SA_FRAME_STATUS_DONE) == 0);
  free(status);
  return done ? 1 : 0;
}

void frame_destroy(frame_t* f) {
  if (f == NULL) return;
  /* Inline teardown: no pool owns this actor (frame_create uses pool=NULL),
     so the enclosing struct's lifetime is ours to end here. */
  atomic_fetch_or(&f->actor.flags, ACTOR_FLAG_DESTROY);
  actor_detach_pool(&f->actor);
  message_queue_destroy(&f->actor.queue);   /* drains; the queue retires payloads */
  if (f->st != NULL) database_subtree_close(f->st);
  free(f->sid_path);
  free(f->parent_path);
  free(f->goal);
  free(f->model_base_url);
  free(f->model_api_key);
  free(f->model_name);
  free(f);
}

/* --- store operations ---------------------------------------------------- */

int frame_remember_local(frame_t* f, const char* key, const char* json_value) {
  return _frame_remember_variant(f, key, json_value, "state/local/");
}

int frame_remember_ctx(frame_t* f, const char* key, const char* json_value) {
  return _frame_remember_variant(f, key, json_value, "state/ctx/");
}

char* frame_recall(frame_t* f, const char* key) {
  if (f == NULL || f->st == NULL || !_frame_key_valid(key, "recall")) return NULL;

  /* 1) Own local scratch — private, so this is the shadowing top layer. */
  char* state_key = _frame_state_key("state/local/", key);
  if (state_key == NULL) return NULL;
  char* text = _frame_subtree_text(f->st, state_key);
  free(state_key);
  if (text != NULL) return text;

  /* 2) Own ctx. */
  state_key = _frame_state_key("state/ctx/", key);
  if (state_key == NULL) return NULL;
  text = _frame_subtree_text(f->st, state_key);
  free(state_key);
  if (text != NULL) return text;

  /* 3) Lineage walk: read meta/parent, open that subtree, read ONLY its
     state/ctx/<key>; repeat upward until resolved, exhausted, or the hop
     budget (max_depth + 1 ancestor reads) is spent. Bounded always: a
     corrupt/looped meta/parent chain cannot spin forever. */
  database_subtree_t* prev = NULL;      /* per-hop subtree owed a close */
  database_subtree_t* cur = f->st;      /* borrowed from the frame at first */
  for (unsigned hop = 0; hop <= f->max_depth; hop++) {
    char* parent_path = _frame_subtree_text(cur, "meta/parent");
    if (parent_path == NULL) {
      database_subtree_close(prev);
      return NULL;                       /* root of the lineage: unresolvable */
    }
    database_subtree_t* p = database_subtree_open(cur->db, parent_path, '/');
    free(parent_path);
    if (p == NULL) {
      database_subtree_close(prev);
      return NULL;
    }
    state_key = _frame_state_key("state/ctx/", key);
    if (state_key == NULL) {
      database_subtree_close(p);
      database_subtree_close(prev);
      return NULL;
    }
    text = _frame_subtree_text(p, state_key);
    free(state_key);
    database_subtree_close(prev);
    if (text != NULL) {
      database_subtree_close(p);
      return text;
    }
    prev = p;
    cur = p;
  }
  database_subtree_close(prev);
  return NULL;                           /* hop budget spent */
}

int frame_append_msg(frame_t* f, const char* role, const char* content) {
  return _frame_append_msg(f, role, content);
}

/* --- spawn / report / join ----------------------------------------------- */

/* Admission-only spawn. Nothing is written before the batch, and the batch
   is ONE root `database_batch_sync_raw` combining three sources of keys:
     - the child's birth meta (meta/created, meta/status, meta/depth,
       meta/parent) — composed as full root paths,
     - the child's ctx handoff key (state/ctx/handoff) when context_json is
       non-NULL,
     - the parent's frame.spawn event,
     - the lineage triple index ops (graph_triple_expand_ops on the root's
       reserved lineage layer — full root-database paths via the subtree
       wrapper's prepend). All one transaction / one WAL record. */
frame_t* frame_spawn(frame_t* parent, const char* goal, const char* context_json) {
  if (parent == NULL || parent->st == NULL) {
    log_error("frame_spawn: no live parent frame");
    return NULL;
  }

  /* Admission only: fail loud, never substitute. */
  if (parent->depth + 1 > parent->max_depth) {
    log_error("frame_spawn: depth %u would exceed max_depth %u of '%s' — "
              "admission refused",
              parent->depth + 1u, parent->max_depth, parent->sid_path);
    return NULL;
  }
  if (context_json != NULL) {
    json_value_t* parsed_ctx = _frame_parse_or_null(context_json);
    if (parsed_ctx == NULL) {
      log_error("frame_spawn: handoff context must be valid JSON");
      return NULL;
    }
    json_value_destroy(parsed_ctx);   /* stored verbatim as raw text */
  }

  frame_t* child = _frame_alloc(parent->root, parent, goal, NULL);
  if (child == NULL) return NULL;

  uint64_t pseq = parent->seq + 1;

  json_value_t* payload = json_new_object();
  if (payload == NULL) {
    log_error("frame_spawn: out of memory building spawn payload");
    frame_destroy(child);
    return NULL;
  }
  json_object_set(payload, "child_sid", json_new_string(child->sid_path));
  json_object_set(payload, "goal",
                  goal != NULL ? json_new_string(goal) : json_new_null());
  json_object_set(payload, "depth", json_new_int((int64_t)child->depth));
  char* event_text = _frame_event_json_full(parent->sid_path, pseq,
                                            "frame.spawn", payload);
  if (event_text == NULL) {
    frame_destroy(child);
    return NULL;
  }

  char iso[25];
  _frame_iso_now(iso);
  char depth_buf[11];
  snprintf(depth_buf, sizeof(depth_buf), "%u", child->depth);

  char* k_created = _frame_subkey(child->sid_path, "meta/created");
  char* k_status = _frame_subkey(child->sid_path, "meta/status");
  char* k_depth = _frame_subkey(child->sid_path, "meta/depth");
  char* k_parent = _frame_subkey(child->sid_path, "meta/parent");
  char* k_handoff = (context_json != NULL)
                        ? _frame_subkey(child->sid_path, "state/ctx/handoff")
                        : NULL;
  char* k_event = _frame_event_key(parent->sid_path, pseq);
  if (k_created == NULL || k_status == NULL || k_depth == NULL ||
      k_parent == NULL || (context_json != NULL && k_handoff == NULL) ||
      k_event == NULL) {
    free(k_created);
    free(k_status);
    free(k_depth);
    free(k_parent);
    free(k_handoff);
    free(k_event);
    free(event_text);
    frame_destroy(child);
    return NULL;
  }

  size_t total_bytes = 0;
  raw_op_t ops[10];   /* 5 meta/handoff + 1 event + 4 index ops, maxed out */
  size_t nops = 0;
  ops[nops].key = k_created;
  ops[nops].key_len = strlen(k_created);
  ops[nops].value = (const uint8_t*)iso;
  ops[nops].value_len = strlen(iso);
  ops[nops].type = 0;
  total_bytes += ops[nops].key_len + ops[nops].value_len;
  nops++;
  ops[nops].key = k_status;
  ops[nops].key_len = strlen(k_status);
  ops[nops].value = (const uint8_t*)SA_FRAME_STATUS_RUNNING;
  ops[nops].value_len = strlen(SA_FRAME_STATUS_RUNNING);
  ops[nops].type = 0;
  total_bytes += ops[nops].key_len + ops[nops].value_len;
  nops++;
  ops[nops].key = k_depth;
  ops[nops].key_len = strlen(k_depth);
  ops[nops].value = (const uint8_t*)depth_buf;
  ops[nops].value_len = strlen(depth_buf);
  ops[nops].type = 0;
  total_bytes += ops[nops].key_len + ops[nops].value_len;
  nops++;
  ops[nops].key = k_parent;
  ops[nops].key_len = strlen(k_parent);
  ops[nops].value = (const uint8_t*)parent->sid_path;
  ops[nops].value_len = strlen(parent->sid_path);
  ops[nops].type = 0;
  total_bytes += ops[nops].key_len + ops[nops].value_len;
  nops++;
  if (context_json != NULL) {
    ops[nops].key = k_handoff;
    ops[nops].key_len = strlen(k_handoff);
    ops[nops].value = (const uint8_t*)context_json;
    ops[nops].value_len = strlen(context_json);
    ops[nops].type = 0;
    total_bytes += ops[nops].key_len + ops[nops].value_len;
    nops++;
  }
  ops[nops].key = k_event;
  ops[nops].key_len = strlen(k_event);
  ops[nops].value = (const uint8_t*)event_text;
  ops[nops].value_len = strlen(event_text);
  ops[nops].type = 0;
  total_bytes += ops[nops].key_len + ops[nops].value_len;
  nops++;

  /* Fail loud before the write, never silent truncation. */
  if (total_bytes > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame_spawn: admission batch for '%s' is %zu bytes, exceeding "
              "the %d-byte WAL batch cap — refusing, never truncating "
              "(trim the handoff context)",
              child->sid_path, total_bytes, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(k_created);
    free(k_status);
    free(k_depth);
    free(k_parent);
    free(k_handoff);
    free(k_event);
    free(event_text);
    frame_destroy(child);
    return NULL;
  }

  /* Lineage triple (child_sid_path, "parent_of", parent_sid_path): expand the
     index ops against the root's reserved lineage layer and merge them into
     the same one batch. 1-4 ops per triple (schema-less default = all four). */
  raw_op_t gops[4];
  size_t ngo = graph_triple_expand_ops(parent->root->lineage,
                                       child->sid_path,
                                       SA_FRAME_LINEAGE_PREDICATE,
                                       parent->sid_path, 0, gops, 4);
  if (ngo == 0) {
    log_error("frame_spawn: lineage triple expansion failed for '%s' — no "
              "lineage index op was produced, refusing the admission",
              child->sid_path);
    free(k_created);
    free(k_status);
    free(k_depth);
    free(k_parent);
    free(k_handoff);
    free(k_event);
    free(event_text);
    frame_destroy(child);
    return NULL;
  }
  for (size_t i = 0; i < ngo; i++) {
    total_bytes += gops[i].key_len;
    ops[nops + i] = gops[i];
  }

  if (total_bytes > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame_spawn: admission batch for '%s' is %zu bytes, exceeding "
              "the %d-byte WAL batch cap — refusing, never truncating",
              child->sid_path, total_bytes, (int)SA_FRAME_MAX_BATCH_BYTES);
    for (size_t i = 0; i < ngo; i++) free((void*)gops[i].key);
    free(k_created);
    free(k_status);
    free(k_depth);
    free(k_parent);
    free(k_handoff);
    free(k_event);
    free(event_text);
    frame_destroy(child);
    return NULL;
  }
  nops += ngo;

  int rc = database_batch_sync_raw(parent->root->db, '/', ops, nops);
  for (size_t i = 0; i < nops; i++) free((void*)ops[i].key);
  free(event_text);
  if (rc != 0) {
    log_error("frame_spawn: admission batch failed (%d) for '%s' under '%s' — "
              "nothing committed", rc, child->sid_path, parent->sid_path);
    frame_destroy(child);
    return NULL;
  }
  parent->seq = pseq;
  return child;
}

/* Report: the child's frame.report event, the parent's bound frame.report
   event, and the child's status → done, in ONE root batch. Both events carry
   payload {child_sid, text}; each keeps its own frame's seq cause chain. */
int frame_report(frame_t* child, const char* text) {
  if (child == NULL || child->st == NULL) {
    log_error("frame_report: dead frame");
    return -1;
  }
  if (text == NULL) {
    log_error("frame_report: report text required");
    return -1;
  }
  frame_t* parent = child->parent;
  if (parent == NULL || parent->st == NULL) {
    log_error("frame_report: no live parent log to bind '%s' into",
              child->sid_path);
    return -1;
  }

  uint64_t cseq = child->seq + 1;
  uint64_t pseq = parent->seq + 1;

  json_value_t* child_payload = json_new_object();
  if (child_payload == NULL) {
    log_error("frame_report: out of memory building report payload");
    return -1;
  }
  json_object_set(child_payload, "child_sid", json_new_string(child->sid_path));
  json_object_set(child_payload, "text", json_new_string(text));
  char* child_text = _frame_event_json_full(child->sid_path, cseq,
                                            "frame.report", child_payload);
  if (child_text == NULL) return -1;

  json_value_t* parent_payload = json_new_object();
  if (parent_payload == NULL) {
    log_error("frame_report: out of memory building bound report payload");
    free(child_text);
    return -1;
  }
  json_object_set(parent_payload, "child_sid", json_new_string(child->sid_path));
  json_object_set(parent_payload, "text", json_new_string(text));
  char* parent_text = _frame_event_json_full(parent->sid_path, pseq,
                                             "frame.report", parent_payload);
  if (parent_text == NULL) {
    free(child_text);
    return -1;
  }

  char* k_cev = _frame_event_key(child->sid_path, cseq);
  char* k_pev = _frame_event_key(parent->sid_path, pseq);
  char* k_status = _frame_subkey(child->sid_path, "meta/status");
  if (k_cev == NULL || k_pev == NULL || k_status == NULL) {
    free(k_cev);
    free(k_pev);
    free(k_status);
    free(child_text);
    free(parent_text);
    return -1;
  }

  raw_op_t ops[3];
  ops[0].key = k_cev;
  ops[0].key_len = strlen(k_cev);
  ops[0].value = (const uint8_t*)child_text;
  ops[0].value_len = strlen(child_text);
  ops[0].type = 0;
  ops[1].key = k_pev;
  ops[1].key_len = strlen(k_pev);
  ops[1].value = (const uint8_t*)parent_text;
  ops[1].value_len = strlen(parent_text);
  ops[1].type = 0;
  ops[2].key = k_status;
  ops[2].key_len = strlen(k_status);
  ops[2].value = (const uint8_t*)SA_FRAME_STATUS_DONE;
  ops[2].value_len = strlen(SA_FRAME_STATUS_DONE);
  ops[2].type = 0;

  size_t total_bytes = strlen(k_cev) + strlen(child_text) +
                       strlen(k_pev) + strlen(parent_text) +
                       strlen(k_status) + strlen(SA_FRAME_STATUS_DONE);
  if (total_bytes > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame_report: report batch from '%s' is %zu bytes, exceeding "
              "the %d-byte WAL batch cap — refusing, never truncating",
              child->sid_path, total_bytes, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(k_cev);
    free(k_pev);
    free(k_status);
    free(child_text);
    free(parent_text);
    return -3;
  }

  int rc = database_batch_sync_raw(child->root->db, '/', ops, 3);
  free(k_cev);
  free(k_pev);
  free(k_status);
  free(child_text);
  free(parent_text);
  if (rc != 0) {
    log_error("frame_report: report batch failed (%d) for '%s' into '%s' — "
              "nothing committed", rc, child->sid_path, parent->sid_path);
    return rc;
  }
  child->seq = cseq;
  parent->seq = pseq;
  return 0;
}

/* Join: ONE frame.join event in the parent's log {child_sid}; the child's
   status stays "done" (report already marked it) and the frame_t destruction
   remains the caller's job — join only logs the close in the parent. */
int frame_join(frame_t* child) {
  if (child == NULL || child->st == NULL) {
    log_error("frame_join: dead frame");
    return -1;
  }
  frame_t* parent = child->parent;
  if (parent == NULL || parent->st == NULL) {
    log_error("frame_join: no live parent log to join '%s' out of",
              child->sid_path);
    return -1;
  }

  uint64_t pseq = parent->seq + 1;
  json_value_t* payload = json_new_object();
  if (payload == NULL) {
    log_error("frame_join: out of memory building join payload");
    return -1;
  }
  json_object_set(payload, "child_sid", json_new_string(child->sid_path));
  char* parent_text = _frame_event_json_full(parent->sid_path, pseq,
                                             "frame.join", payload);
  if (parent_text == NULL) return -1;

  char* k_pev = _frame_event_key(parent->sid_path, pseq);
  if (k_pev == NULL) {
    free(parent_text);
    return -1;
  }

  raw_op_t ops[1];
  ops[0].key = k_pev;
  ops[0].key_len = strlen(k_pev);
  ops[0].value = (const uint8_t*)parent_text;
  ops[0].value_len = strlen(parent_text);
  ops[0].type = 0;

  size_t total_bytes = strlen(k_pev) + strlen(parent_text);
  if (total_bytes > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame_join: join batch for '%s' is %zu bytes, exceeding the "
              "%d-byte WAL batch cap — refusing, never truncating",
              child->sid_path, total_bytes, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(k_pev);
    free(parent_text);
    return -3;
  }

  int rc = database_batch_sync_raw(child->root->db, '/', ops, 1);
  free(k_pev);
  free(parent_text);
  if (rc != 0) {
    log_error("frame_join: join batch failed (%d) for '%s' out of '%s' — "
              "nothing committed", rc, child->sid_path, parent->sid_path);
    return rc;
  }
  parent->seq = pseq;
  return 0;
}

/* Test/debug accessor: the frame's raw event records as a malloc'd JSON array.
   Root-level REVERSE scan over the frame's events range with absolute composed
   bounds (see _frame_events_bounds — subtree bounded scans are broken in both
   directions), materializing at most the newest SA_FRAME_DEBUG_MAX_EVENTS
   records, emitted in ascending seq order. */
char* frame_debug_events(frame_t* f) {
  if (f == NULL || f->st == NULL) return NULL;

  path_t* start = NULL;
  path_t* end = NULL;
  if (_frame_events_bounds(f, &start, &end) != 0) {
    log_error("frame_debug_events: out of memory composing scan bounds");
    return NULL;
  }
  database_iterator_t* iter = database_scan_start_reverse(f->root->db, start, end);
  if (iter == NULL) {
    log_error("frame_debug_events: reverse scan failed on '%s'", f->sid_path);
    return NULL;
  }

  char* texts[SA_FRAME_DEBUG_MAX_EVENTS];   /* newest-first (descending seq) */
  size_t n = 0;
  path_t* key = NULL;
  identifier_t* value = NULL;
  while (n < SA_FRAME_DEBUG_MAX_EVENTS) {
    path_t* k = NULL;
    identifier_t* v = NULL;
    int rc = database_scan_prev(iter, &k, &v);
    if (rc != 0) break;                    /* -1: out of records, -2: error */
    size_t len = 0;
    uint8_t* data = identifier_get_data_copy(v, &len);
    if (data != NULL) {
      char* text = get_memory(len + 1);
      if (text == NULL) {
        free(data);
        log_error("frame_debug_events: out of memory materializing a record");
      } else {
        memcpy(text, data, len);
        text[len] = '\0';
        texts[n++] = text;
      }
      free(data);
    } else {
      log_error("frame_debug_events: record value copy failed on '%s'",
                f->sid_path);
    }
    path_destroy(k);
    identifier_destroy(v);
  }
  database_scan_end(iter);

  /* Emit ascending: the caller reads the array oldest → newest. */
  json_value_t* arr = json_new_array();
  if (arr == NULL) {
    log_error("frame_debug_events: out of memory building event array");
    return NULL;
  }
  for (size_t i = n; i-- > 0;) {
    char* err = NULL;
    json_value_t* rec = json_parse(texts[i], strlen(texts[i]), &err);
    if (err != NULL) {
      free(err);
    }
    if (rec != NULL) {
      json_array_append(arr, rec);
    } else {
      log_error("frame_debug_events: unparseable event record dropped "
                "(%zu remaining of '%s')", i, f->sid_path);
    }
    free(texts[i]);
  }
  char* out = json_serialize(arr);
  if (out == NULL) {
    log_error("frame_debug_events: out of memory serializing event array");
  }
  json_value_destroy(arr);
  return out;
}

#endif /* SA_HAS_WDB */