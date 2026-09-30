//
// Created by victor on 9/29/26.
//

#include "frame.h"
#include "frame_messages.h"
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

#ifdef SA_HAS_WDB

#include "../Util/atomic_compat.h"
#include <Database/database.h>
#include <Database/database_subtree.h>
#include <Database/database_iterator.h>
#include <HBTrie/path.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* One effect = one root batch. A concurrent-mode batch that exceeds the WAL
   file size is REJECTED by WaveDB (default cap 128 KB); we refuse anything
   approaching that limit up front so rejection is loud and early. */
#define SA_FRAME_MAX_BATCH_BYTES (120 * 1024)

/* sid layout: 8 hex chars = 20 random bits (top) | 12 counter bits (low).
   The counter makes intra-root sids collision-free for the first 4096 frames
   of a root's life; the random top bits keep sibling roots' spaces disjoint. */
#define SA_FRAME_SID_HEX_LEN 8

/* Status key values (meta/status). frame_create stamps "running"; Task 5's
   report/join and Task 10's loop mark "done". */
static const char SA_FRAME_STATUS_RUNNING[] = "running";
static const char SA_FRAME_STATUS_DONE[] = "done";

struct wave_database_root_t {
  database_t* db;             /* the ONE root database */
  uint32_t rng;               /* xorshift32 state for sid randomness */
  ATOMIC(uint64_t) counter;   /* sid uniqueness counter */
};

struct frame_t {
  actor_t actor;              /* FIRST member (style guide) */
  wave_database_root_t* root;
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
   "cause","payload"}. Bridge events carry a corr (Tasks 5-6 will use it); a
   direct store write has none, so "corr" is JSON null in this slice. "cause"
   = the seq of this frame's PREVIOUS event (the audit chain); JSON null for
   the frame's first event. Consumes `payload`. */
static char* _frame_event_json(frame_t* f, uint64_t seq, const char* type_name,
                               json_value_t* payload) {
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
  json_object_set(rec, "frame", json_new_string(f->sid_path));
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
    log_error("frame: state key '%s' (%zu bytes) exceeds the %d-byte WAL batch "
              "cap — refusing, never truncating",
              key, state_len + (size_t)strlen(json_value),
              (int)SA_FRAME_MAX_BATCH_BYTES);
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

  f->seq = seq;
  int rc = database_subtree_batch_sync_raw(f->st, '/', ops, 2);
  free(text);
  free(state_key);
  free(evkey);
  if (rc != 0) {
    log_error("frame: remember batch failed (%d); '%s' of %s is unwritten",
              rc, key, f->sid_path);
    return rc;
  }
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

  f->seq = seq;
  int rc = database_subtree_batch_sync_raw(f->st, '/', ops, 1);
  free(evkey);
  free(text);
  if (rc != 0) {
    log_error("frame: msg.append batch failed (%d); seq %llu of %s is unwritten",
              rc, (unsigned long long)seq, f->sid_path);
    return rc;
  }
  return 0;
}

/* Dispatch a queued FRM_* message. Bridge behaviors claim payloads in Task 6
   (remember/recall corr-matched replies) and the loop claims FRM_CELL_EXECUTE
   / FRM_STOP in Task 10; until those land the frame ignores messages and
   payload cleanup stays with the mailbox per the style guide. Unknown types
   are ignored likewise. */
static void frame_dispatch(void* state, message_t* msg) {
  frame_t* f = (frame_t*)state;
  (void)f;
  switch (msg->type) {
    case FRM_REMEMBER:
    case FRM_RECALL:
    case FRM_REPLY:
    case FRM_CELL_EXECUTE:
    case FRM_STOP:
      break;
    default:
      break;
  }
}

/* Largest event seq ever written under events/ — 0 when fresh. Reverse scan
   (root-level, absolute composed bounds — the subtree iterator strips result
   paths but compares them against the FULL stored bound paths, so subtree-
   relative bounds never match) over the frame's bounded events range takes
   the first (largest) key; the seq counter continues past it. */
static uint64_t _frame_restore_seq(frame_t* f) {
  size_t base = strlen(f->sid_path);
  char* lo = get_memory(base + sizeof("/events"));
  char* hi = get_memory(base + sizeof("/events0"));
  if (lo == NULL || hi == NULL) {
    free(lo);
    free(hi);
    log_error("frame: out of memory composing boot scan bounds");
    return 0;
  }
  snprintf(lo, base + sizeof("/events"), "%s/events", f->sid_path);
  snprintf(hi, base + sizeof("/events0"), "%s/events0", f->sid_path);
  path_t* start = _frame_path_from(lo);
  path_t* end = _frame_path_from(hi);
  free(lo);
  free(hi);
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
  return root;
}

void wave_db_close(wave_database_root_t* root) {
  if (root == NULL) return;
  database_destroy(root->db);
  root->db = NULL;
  free(root);
}

frame_t* frame_create(wave_database_root_t* root, frame_t* parent,
                      const char* goal, const frame_config_t* cfg) {
  if (root == NULL) {
    log_error("frame_create: NULL root");
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
    f->depth = parent->depth + 1;
  }

  f->max_depth = (cfg != NULL && cfg->max_depth > 0) ? cfg->max_depth : 4;
  if (goal != NULL) {
    f->goal = strdup(goal);
    if (f->goal == NULL) goto fail;
  }
  if (cfg != NULL) {
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
  }

  f->st = database_subtree_open(root->db, f->sid_path, '/');
  if (f->st == NULL) {
    log_error("frame_create: subtree open failed for '%s'", f->sid_path);
    goto fail;
  }

  /* Boot: continue the seq counter past any persisted events (restart-safe).
     (No events in this task — Task 10's restart/replay test depends on this.) */
  f->seq = _frame_restore_seq(f);

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
      goto fail;
    }
  }

  /* Inline actor (pool NULL): tests/loop pump the mailbox by hand. */
  actor_init(&f->actor, f, frame_dispatch, NULL);
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

#endif /* SA_HAS_WDB */