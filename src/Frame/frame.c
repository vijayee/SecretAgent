//
// Created by victor on 9/29/26.
//

#include "frame.h"
#include "frame_internal.h"
#include "frame_messages.h"
#include "frame_bridge.h"
#include "lifecycle.h"
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

void frm_cell_payload_destroy(void* p) {
  frm_cell_payload_t* cp = (frm_cell_payload_t*)p;
  if (cp == NULL) return;
  free(cp->code);
  free(cp);
}

void frm_model_payload_destroy(void* p) {
  frm_model_payload_t* mp = (frm_model_payload_t*)p;
  if (mp == NULL) return;
  free(mp->body);
  free(mp->error);
  free(mp);
}

void frm_child_report_payload_destroy(void* p) {
  frm_child_report_payload_t* rp = (frm_child_report_payload_t*)p;
  if (rp == NULL) return;
  free(rp->child_sid);
  free(rp);
}

void frm_store_batch_payload_destroy(void* p) {
  frm_store_batch_payload_t* bp = (frm_store_batch_payload_t*)p;
  if (bp == NULL) return;
  if (bp->ops != NULL) {
    for (size_t i = 0; i < bp->nops; i++) {
      free(bp->ops[i].key);
      free(bp->ops[i].value);
    }
    free(bp->ops);
  }
  free(bp);
}

void frm_store_scan_payload_destroy(void* p) {
  frm_store_scan_payload_t* sp = (frm_store_scan_payload_t*)p;
  if (sp == NULL) return;
  free(sp->start);
  free(sp->end);
  free(sp);
}

void frm_store_recall_payload_destroy(void* p) {
  frm_store_recall_payload_t* rp = (frm_store_recall_payload_t*)p;
  if (rp == NULL) return;
  free(rp->key);
  free(rp->sid_path);
  free(rp);
}

void frm_store_keys_payload_destroy(void* p) {
  frm_store_keys_payload_t* kp = (frm_store_keys_payload_t*)p;
  if (kp == NULL) return;
  free(kp->sid_path);
  free(kp->scope);
  free(kp);
}

void frm_store_reply_payload_destroy(void* p) {
  frm_store_reply_payload_t* rp = (frm_store_reply_payload_t*)p;
  if (rp == NULL) return;
  if (rp->records != NULL) {
    for (size_t i = 0; i < rp->n; i++) free(rp->records[i]);
    free(rp->records);
  }
  free(rp);
}

void frm_report_bind_payload_destroy(void* p) {
  frm_report_bind_payload_t* bp = (frm_report_bind_payload_t*)p;
  if (bp == NULL) return;
  free(bp->child_sid);
  free(bp->child_event_text);
  free(bp->text);
  free(bp);
}

#ifdef SA_HAS_WDB

#include "model.h"

#include <Database/database.h>
#include <Database/database_subtree.h>
#include <Database/database_iterator.h>
#include <HBTrie/path.h>
#include <Layers/graph/graph.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef SA_HAS_PYTHON
#include "../Python/pyrt.h"   /* brings pyrt_messages.h; the frame's OWN runtime */
#endif
/* NOT platform.h: its platform_thread.h barrier declarations collide with
   WaveDB's own threadding.h barrier prototypes (the family-shared symbol
   set this build aliases) — a frame TU includes WaveDB headers via
   Database/. The cell-wait clock needs the TIME wrappers; the POOLED cell
   watchdog needs platform_sync.h — platform_thread.h's thread/mutex/condvar
   subset WITHOUT the colliding barrier prototypes. */
#include "../Platform/platform_time.h"
#include "../Platform/platform_sync.h"
#include "../Util/budget.h"   /* SA_FRAME_CELL_WATCHDOG_MS (the cfg-less default) */

/* One effect = one root batch. A concurrent-mode batch that exceeds the WAL
   file size is REJECTED by WaveDB (default cap 128 KB); we refuse anything
   approaching that limit up front so rejection is loud and early. */
#define SA_FRAME_MAX_BATCH_BYTES (120 * 1024)

/* --- the store round trips (frame.c-internal; §5's seq discipline) --------

   Every store touch is a MESSAGE to the root's store actor. A frame composes
   on its own dispatch thread (single-runner discipline — no lock), PRE-
   ALLOCATES its seq at compose time, and awaits the corr-matched reply
   through ONE of the slots below (or lets the FRM_STORE_REPLY router answer
   a registered bridge corr). All slots are single-flight: fully overwritten
   at compose, cleared by their awaiter.

   frame_sync_slot_t: the DIRECT sync API's slot (remember/append/join/
   status/recall) — one call awaits it on the caller's (pump-wait) thread. */
typedef struct frame_sync_slot_t {
  uint8_t in_use, done;    /* in_use = a direct sync call awaits; done = the
                              reply landed */
  uint64_t corr;           /* the awaited corr */
  int rc;                  /* the store's refusal code (0 = committed) */
  char* text;              /* a recall reply's resolved text (transfer) */
  char** records;          /* a scan reply's materialized raw record texts
                              (transfer, ascending; the sync scan joins them
                              into its array reply and frees) */
  size_t n;                /* the records count */
} frame_sync_slot_t;

/* The spawn admissions IN FLIGHT (a corr-keyed list, not one slot): the
   orchestration slice made cell-verb spawns OVERLAPPABLE — a pooled parent's
   cell can start the next actor.spawn while a previous one's store reply is
   still routing (the py-agent's bounded bridge wait can also give up on a
   busy mailbox: the post is still DELIVERED into a busy mailbox (actor_send
   answers delivered-vs-refused, busy included — false is the only refusal),
   so one slot would strand the older
   admission's child. Every admission carries its own entry; every store
   reply corr-matches ITS entry. The list is the parent's dispatch-thread
   single-writer. */
typedef struct frm_spawn_pending_t {
  uint64_t corr;
  uint64_t bridge_corr;    /* 0 = the direct sync caller awaits; nonzero = a
                              cell-verb spawn (the router answers + cleans) */
  uint8_t done;
  int rc;                  /* the store's refusal code (0 = committed) */
  uint64_t own_seq;        /* the parent's PRE-ALLOCATED admission seq */
  frame_t* child;          /* the allocated child (NULL once released) */
  struct frm_spawn_pending_t* next;
} frm_spawn_pending_t;

/* The report bind: the CHILD awaits its own store corr (the reply routes
   back to the child's actor); the router releases the direct caller and
   answers a cell-side bridge corr. */
typedef struct frame_bind_slot_t {
  uint8_t in_use, done;
  uint64_t corr, bridge_corr;
  uint8_t engine_driven;   /* 1 = post FRM_CHILD_REPORT on the reply (the
                              terminate wires its own binds with this) */
  uint8_t failed;          /* engine_driven only: the terminate's outcome
                              flag — the FRM_CHILD_REPORT resumes the parent
                              with failed = 0 (quiet) or 1 (failure) */
  int rc;                  /* the store's refusal code (0 = committed) */
  uint64_t own_seq;        /* the child's pre-allocated seq */
} frame_bind_slot_t;

/* The behavior-posted store round trips' registered corr answers: the
   bridge corr of a cell verb waiting one store hop. Every mutation happens
   on the frame's OWN dispatch thread (actor single-runner discipline) — a
   list, no lock. The KIND says what answer shape the reply router composes
   from the store's raw records. */
typedef enum frm_bridge_kind_e {
  FRM_BRIDGE_TEXT = 0,    /* remember/spawn/report: the plain corr answer
                             (the store commit/refusal, text unused today) */
  FRM_BRIDGE_RECALL = 1,  /* the answer carries the resolved value text
                             (records[0]) */
  FRM_BRIDGE_KEYS = 2     /* the answer composes the sorted key-name array
                             from the records (the keys verb; spec §3) */
} frm_bridge_kind_e;

typedef struct frm_bridge_pending_t {
  uint64_t corr;           /* the frame's OWN store round-trip corr */
  uint64_t answer_corr;    /* the corr the reply must be answered under (the
                              bridge verb's py-agent corr) */
  frm_bridge_kind_e kind;  /* the verb's answer shape (the router composes) */
  struct frm_bridge_pending_t* next;
} frm_bridge_pending_t;

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

/* frame_debug_events materialization bound: newest 512 records max (the
   SHARED bound rides frame_internal.h — frame.c's scans, the store actor's
   scan replies, and the engine's FRM_STORE_SCAN must agree on the number). */

struct wave_database_root_t {
  actor_t store_actor;        /* FIRST member (house rule: actor states lead
                                 with actor_t) — the ONE serializer: every
                                 frame-layer store op executes inside this
                                 actor's behavior, ONE message at a time */
  database_t* db;             /* the ONE root database */
  graph_layer_t* lineage;     /* subtree-mode graph layer over lineage_st */
  database_subtree_t* lineage_st;  /* reserved "lineage" subtree (open for life) */
  uint32_t rng;               /* xorshift32 state for sid randomness */
  ATOMIC(uint64_t) counter;   /* sid uniqueness counter */
  scheduler_pool_t* store_pool;  /* BORROWED; NULL = inline (tests/demo pump
                                    by hand) — the dual-driver rule's knob */
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
  unsigned model_timeout_ms;  /* 0 = built-in default (model_timeout_ms_resolve) */
  unsigned cell_watchdog_ms;  /* one pooled RUNNING CELL's bound, ms;
                                 0 = disabled (SA_FRAME_CELL_WATCHDOG_MS is
                                 the cfg-less default) */
  scheduler_pool_t* pool;     /* BORROWED from the config (inherited down the
                                 lineage): the frame actor's scheduler pool;
                                 NULL = the inline shape (the owner pumps). */
  uint64_t seq;               /* last allocated event seq (0 = none yet) */
  uint32_t depth;
  /* --- the turn-loop pieces (Task 10; see frame_internal.h) ---------------
     loop_turn_cap: runtime cap override, 0 = SA_LOOP_MAX_TURNS default.
     backend / owned_backend: the loop's model backend. `backend` is the
     BORROWED injected override (frame_set_model_backend — never freed here;
     scripted test backends are caller stack objects). `owned_backend` is the
     lazily-built DEFAULT http backend: built once at first use from the
     frame's own config, freed HERE. A set override always wins and the
     default is then never built. */
  struct model_backend_t* backend;
  struct model_backend_t* owned_backend;
  unsigned loop_turn_cap;
  /* The pending-cell slot: ONE cell at a time. FRM_CELL_EXECUTE fills it;
     PYRT_RESULT completes it (status filled, pending cleared) after writing
     the paired cell.result event. Single-writer discipline: every access
     happens on the frame's dispatch thread (the loop's thread — the frame is
     inline); the pyrt worker only pushes into the mailbox. */
#ifdef SA_HAS_PYTHON
  pyrt_t* pyrt;               /* the frame's OWN runtime; lazy, freed at destroy */
#endif
  uint8_t cell_pending;       /* 1 while FRM_CELL_EXECUTE is in flight */
  uint64_t cell_corr;         /* the loop's audit corr for the pending cell */
  uint64_t cell_pyrt_corr;    /* the pyrt executor corr it was handed as */
  uint8_t cell_status;        /* completion status of the pending/last cell */
  uint8_t pyrt_poisoned;      /* an interrupt wedged this frame's runtime
                                 (in-memory, per-process: a restart rebuilds a
                                 fresh runtime and the durable log already
                                 carries the aborted turn) */
  uint64_t cell_interrupted_corr;   /* the pyrt corr whose REAL result is
                                       expected late and drops quietly */
  frame_cell_watchdog_t* cell_watchdog;   /* the POOLED cell's armed
                                             watchdog (NULL = none/idle);
                                             written ONLY on this frame's
                                             dispatch thread — frame.c's
                                             disarm protocol owns it */
  platform_thread_t* delayed_post_thread; /* the armed retry backoff's
                                             one-shot timer (NULL = none);
                                             JOINABLE — reaped at the FRM_TURN
                                             dispatch's head and in
                                             frame_destroy FIRST. Dispatch-
                                             thread-only bookkeeping (arm +
                                             reap), like the cell slot; the
                                             thread itself never touches frame
                                             state (guards spec §3). */
  uint8_t stop_requested;     /* FRM_STOP: the loop drains, then stops */
  /* The nesting depth of THIS frame's behavior dispatches (0 = not inside a
     mailbox dispatch). The single-runner discipline keeps it exact; the sync
     store APIs consult it: a caller INSIDE the frame's own dispatch can
     never pump this frame's mailbox (a nested actor_run on the sentinel
     queue frees the outer run's node — the ASan-proven reentrancy hazard),
     so its writes go FIRE-AND-POST (the store's FIFO commits them ahead of
     the engine's next awaited trip) and its result-returning reads refuse
     loud instead of deadlocking. */
  uint8_t dispatch_depth;
  /* The TURN ENGINE's state (Task 3; the machine behind frame_start / the
     FRM_TURN continuations — spec §1; loop.c's handlers lean on it through
     _frame_engine_state). Single-writer discipline like the cell slot: every
     access is on the frame's dispatch thread (an inline frame's owner
     thread, or this actor's one pool worker). */
  frame_engine_state_t engine;
  /* --- the store round trip (Task 2; §5) --------------------------------- */
  uint64_t store_corr_seq;          /* the frame's OWN round-trip key space:
                                       a nonzero allocator (0 = fire-and-post) */
  frm_bridge_pending_t* bridge_pending;   /* corr answers waiting one store
                                             hop (registered pre-post) */
  frame_sync_slot_t sync;           /* the direct sync API's awaited reply */
  frm_spawn_pending_t* spawn_pending;   /* the spawn admissions in flight
                                           (the bridge one's corr answers +
                                           cleanup live in the router) */
  frame_bind_slot_t bind_slot;      /* the report bind's awaited commit */
  /* --- the spawned-children RECORD bookkeeping (Task 5; spec §1 §4) -------- */
  frame_t* owned_children;   /* the UNADOPTED (cell-verb) spawned child records
                                whose record lives on past the admission reply
                                once START runs: the parent frees them all at
                                ITS teardown (a started child's record has no
                                other owner). Owned by this parent's dispatch
                                thread (single-writer, the house discipline). */
  frame_t* owned_next;       /* THIS record's link in its OWNING parent's
                                teardown list (the parent's dispatch thread
                                writes it in the START branch; only the
                                parent's teardown reads it) */
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
   "cause","payload"}. "corr" is JSON null in this slice — the bridge verb
   corrs route through the reply registry, never into the records. "cause"
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

/* The recall resolve walk (own local/ -> own ctx/ -> the meta/parent hops'
   ctx/): a pure store computation over the walk's subtree reads. It runs in
   the STORE actor's behavior (the serialized read); `own` is the already-
   opened starting subtree, `max_hops` the frame's depth budget. Bounded
   always: a corrupt/looped meta/parent chain cannot spin forever; every
   per-hop subtree is closed again on every path. */
static char* _frame_recall_walk(database_subtree_t* own, const char* key,
                                unsigned max_hops) {
  /* 1) Own local scratch — private, so this is the shadowing top layer. */
  char* state_key = _frame_state_key("state/local/", key);
  if (state_key == NULL) return NULL;
  char* text = _frame_subtree_text(own, state_key);
  free(state_key);
  if (text != NULL) return text;

  /* 2) Own ctx. */
  state_key = _frame_state_key("state/ctx/", key);
  if (state_key == NULL) return NULL;
  text = _frame_subtree_text(own, state_key);
  free(state_key);
  if (text != NULL) return text;

  /* 3) Lineage walk: read meta/parent, open that subtree, read ONLY its
     state/ctx/<key>; repeat upward until resolved, exhausted, or the hop
     budget (max_depth + 1 ancestor reads) is spent. */
  database_subtree_t* prev = NULL;      /* per-hop subtree owed a close */
  database_subtree_t* cur = own;        /* borrowed until the first hop */
  for (unsigned hop = 0; hop <= max_hops; hop++) {
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

/* --- the store round-trip machinery (§5) ---------------------------------- */

/* Seq PRE-ALLOCATION (the lock's replacement): the frame's seq lives in its
   OWN actor (single-runner discipline — no atomic, no lock); a composer
   allocates seq = current + 1 AT COMPOSE TIME and never re-reads it. On a
   store refusal the number is abandoned: when the frame was single-flight
   the rollback restores today's no-gap discipline exactly; otherwise the
   gap stays (a recorded consequence — the wrong key is impossible, the
   store's atomic batch committed nothing half of). */
static uint64_t _frame_seq_alloc(frame_t* f) {
  f->seq = f->seq + 1;
  return f->seq;
}

void _frame_seq_rollback(frame_t* f, uint64_t abandoned) {
  if (f == NULL || abandoned == 0) return;
  if (f->seq == abandoned) {
    f->seq = abandoned - 1;
  } else {
    log_error("frame: seq %llu of '%s' was not the latest allocation (%llu) "
              "— the composer was not single-flight; the gap stays (the "
              "store's batch committed nothing half of)",
              (unsigned long long)abandoned, f->sid_path,
              (unsigned long long)f->seq);
  }
}

/* The bridge corr registry: a registered corr answer waits exactly one store
   hop. Registered BEFORE the post, from the frame's own dispatch thread.
   `corr` is the frame's OWN store corr (its reply's corr); `answer_corr` is
   the call it must be answered under — the py-agent bridge's own corr space
   (the two counters are UNRELATED: one mapped registration per round trip
   keeps them from aliasing a slot's corr in the router). */
static void _frame_bridge_pending_add(frame_t* f, uint64_t corr,
                                      uint64_t answer_corr,
                                      frm_bridge_kind_e kind) {
  if (f == NULL || corr == 0) return;
  frm_bridge_pending_t* node = get_clear_memory(sizeof(frm_bridge_pending_t));
  node->corr = corr;
  node->answer_corr = answer_corr;
  node->kind = kind;
  node->next = f->bridge_pending;
  f->bridge_pending = node;
}

/* 1 when `corr` was registered (and unlinks it: *answer_out carries the corr
   to answer under, *kind_out the verb's answer shape). */
static int _frame_bridge_pending_take(frame_t* f, uint64_t corr,
                                      uint64_t* answer_out,
                                      frm_bridge_kind_e* kind_out) {
  frm_bridge_pending_t** p = &f->bridge_pending;
  while (*p != NULL) {
    if ((*p)->corr == corr) {
      frm_bridge_pending_t* hit = *p;
      if (answer_out != NULL) *answer_out = hit->answer_corr;
      if (kind_out != NULL) *kind_out = hit->kind;
      *p = hit->next;
      free(hit);
      return 1;
    }
    p = &(*p)->next;
  }
  return 0;
}

/* A post that hands ownership to the actor system (frame_internal.h's
   contract — the store behaviors, the frame behaviors, AND the turn engine's
   handlers all post through it): the DESTROY/dropped-send case is checked
   from the target's flag (actor_send answers delivered-vs-refused, busy
   included — a queued message at a busy mailbox is still delivered; false is
   the only refusal).
   Refusals log loud. */
void _frame_post(actor_t* target, uint32_t type, void* payload,
                 void (*destroy)(void*), const char* what) {
  message_t m;
  m.type = type;
  m.payload = payload;
  m.payload_destroy = destroy;
  if (atomic_load(&target->flags) & ACTOR_FLAG_DESTROY) {
    log_error("frame: %s dropped — the target actor is destroyed (a dying "
              "requester loses only a wake-up it could not have used)", what);
    if (destroy != NULL) destroy(payload);
    return;
  }
  (void)actor_send(target, &m);
}

/* --- the POOLED cell watchdog (surface-completion spec §2) ----------------
   ONE short-lived thread per pooled running cell. The thread waits its
   deadline on a condvar; the result's arrival (the disarm) sets
   disarm_wanted + wakes it under the SAME lock, so the deadline branch and
   the disarm are mutually exclusive decisions on ONE protected state pair
   (disarm_wanted, handed_off) — there is no third outcome. At the deadline
   the thread hands the WHOLE watchdog struct to the frame as a
   FRM_CELL_WATCHDOG message payload (the frame's dispatch is the single
   cleaner — no cross-thread free, no join of a detached thread), sets
   handed_off BEFORE the post, and detaches. The disarm, if it arrives after
   a handoff, drops to the incoming message's destroyer instead of joining.

   THE PRIVATE MUTEX PAIR: the frame layer's store-actor discipline is
   lock-free (single-writer dispatch threads, fire-and-post, atomics at the
   lifetime seams); these two mutexes guard ONLY the watchdog's own
   two-state pair across the watcher thread and this frame's dispatch
   thread — no frame state is shared through them. */

typedef struct frame_cell_watchdog_t {
  platform_mutex_t* lock;
  platform_condvar_t* cond;
  platform_thread_t* thread;
  uint8_t disarm_wanted;   /* the result's arrival, under lock */
  uint8_t handed_off;      /* the deadline branch: set BEFORE the post, under
                              lock — the disarm reads it under the SAME lock
                              to decide join-vs-bail */
  uint32_t timeout_ms;
  frame_t* f;              /* BORROWED: the frame outlives an armed watchdog
                              (frame_destroy disarms/joins first) */
} frame_cell_watchdog_t;

static void _frame_cell_watchdog_payload_destroy(void* p) {
  frame_cell_watchdog_t* w = (frame_cell_watchdog_t*)p;
  if (w == NULL) return;
  platform_mutex_destroy(w->lock);
  platform_condvar_destroy(w->cond);
  free(w);
}

static void* _frame_cell_watchdog_main(void* arg) {
  frame_cell_watchdog_t* w = (frame_cell_watchdog_t*)arg;
  uint64_t deadline =
      platform_monotonic_ns() + (uint64_t)w->timeout_ms * 1000000ULL;
  platform_mutex_lock(w->lock);
  while (w->disarm_wanted == 0) {
    uint64_t now = platform_monotonic_ns();
    if (now >= deadline) {
      /* The deadline fired: hand the struct over (its destroyer frees it on
         the frame's thread) and detach. handed_off is set under the lock
         FIRST, so a disarm that arrives later under the same lock sees it
         and never joins a detached thread — and with the post UNDER the
         lock, the disarm can only ever observe a COMPLETED handoff (the
         post precedes any handed_off read through the same mutex), so the
         frame can never be freed under a not-yet-delivered message. The
         FRM_CELL_WATCHDOG dispatch takes this same lock once before its
         destroy — the handshake guarantees the watcher's unlock precedes
         the mutex's death. _frame_post's REFUSAL branch (the target's
         ACTOR_FLAG_DESTROY — unreachable here by wiring: every teardown
         runs this frame's disarm FIRST, which joins a not-yet-handed-off
         watcher) would destroy the payload while this thread holds the
         lock; the armed watch's posting path keeps the target alive. */
      w->handed_off = 1;
      _frame_post(&w->f->actor, (uint32_t)FRM_CELL_WATCHDOG, w,
                  _frame_cell_watchdog_payload_destroy, "cell watchdog");
      platform_thread_t* self = w->thread;   /* the local survives the unlock */
      platform_mutex_unlock(w->lock);
      platform_thread_detach(self);
      return NULL;
    }
    platform_condvar_timed_wait(w->cond, w->lock,
                                (unsigned)((deadline - now) / 1000000ULL) + 1);
  }
  platform_mutex_unlock(w->lock);
  return NULL;
}

static void _frame_cell_watchdog_disarm(frame_t* f) {
  /* The frame's dispatch thread ONLY (the single writer of the pointer). */
  frame_cell_watchdog_t* w = f->cell_watchdog;
  if (w == NULL) return;
  f->cell_watchdog = NULL;
  platform_mutex_lock(w->lock);
  if (w->handed_off != 0) {
    /* The deadline won: the incoming FRM_CELL_WATCHDOG message owns the
       struct (its destroyer frees it on the frame's thread; a refused send
       at a dying target is freed by _frame_post's own refusal branch) and
       the watcher thread detaches itself — do not join, do not touch the
       lock past the unlock. */
    platform_mutex_unlock(w->lock);
    return;
  }
  w->disarm_wanted = 1;
  platform_condvar_broadcast(w->cond);
  platform_mutex_unlock(w->lock);
  platform_thread_join(w->thread);
  _frame_cell_watchdog_payload_destroy(w);
}

static void _frame_cell_watchdog_arm(frame_t* f) {
  if (f->pool == NULL || f->cell_watchdog_ms == 0) return;   /* the inline
    driver's phase deadline covers pool-less shapes; 0 = the watchdog is off */
  if (f->cell_watchdog != NULL) {
    /* INVARIANT: one cell at a time — the previous watchdog disarmed at its
       result's arrival. Seeing one here is a bug's loud trace, and the
       disarm-first discipline keeps the state machine safe anyway. */
    log_error("frame: a second watchdog armed at '%s' — the cell slot's "
              "single-watch invariant broke; disarming the stale one",
              f->sid_path);
    _frame_cell_watchdog_disarm(f);
  }
  frame_cell_watchdog_t* w =
      (frame_cell_watchdog_t*)get_clear_memory(sizeof(frame_cell_watchdog_t));
  if (w == NULL) {
    log_error("frame: out of memory arming the cell watchdog at '%s' — "
              "cells run unwatched this frame (the hung-cell cost is the "
              "old silent wedge)", f->sid_path);
    return;
  }
  w->lock = platform_mutex_create();
  w->cond = platform_condvar_create();
  w->timeout_ms = f->cell_watchdog_ms;
  w->f = f;
  if (w->lock == NULL || w->cond == NULL) {
    _frame_cell_watchdog_payload_destroy(w);
    log_error("frame: the cell watchdog failed to build at '%s' — cells run "
              "unwatched this frame", f->sid_path);
    return;
  }
  /* LOCK-CREATE-PUBLISH: the thread pointer's store rides the SAME lock the
     deadline branch reads `w->thread` under — stored after the pointer's
     publication and OUTSIDE the lock, it had no happens-before edge to the
     watcher (the create's return is the only synchronization the C standard
     gives us). pthread_create cannot block on our code's locks, so holding
     the lock across the create is safe; the watcher's first action is
     locking w->lock, still HELD here, so the publish below happens-before
     any watcher progress — the old PUBLISH-then-thread invariant (a
     deadline can never fire before its frame is watching) carries over
     intact, with the dispatch thread still the pointer's single writer. */
  platform_mutex_lock(w->lock);
  w->thread = platform_thread_create(_frame_cell_watchdog_main, w);
  if (w->thread == NULL) {
    platform_mutex_unlock(w->lock);
    _frame_cell_watchdog_payload_destroy(w);   /* never published — nothing
                                                  to un-publish */
    log_error("frame: the cell watchdog thread refused at '%s' — cells run "
              "unwatched this frame", f->sid_path);
    return;
  }
  f->cell_watchdog = w;
  platform_mutex_unlock(w->lock);
}

/* --- the one-shot delayed post (guards spec §3): the retry table's backoff
   DELIBERATELY not the cell watchdog: no handoff message, no disarm
   protocol, no private mutex. WHY this is still race-free:
     - The thread's ONLY cross-thread touches are (a) reading die_requested
       (an ATOMIC) and (b) the post itself — both safe while the frame's
       memory is alive, and the frame's memory IS alive: frame_destroy JOINS
       the stored thread BEFORE any teardown step.
     - f->delayed_post_thread is written ONLY by the frame's dispatch thread
       (arm + reap) and read by nobody else (the thread never touches frame
       bookkeeping) — so the watchdog's publish-before-create race has NO
       counterpart here.
     - NO MUTEX exists in this block (the no-locks grep gate: only the
       watchdog's own quartet and model.c may carry mutexes) — the single
       writer plus the join-before-teardown ordering substitutes for one.
   The thread frees its OWN record below — the frame never dereferences the
   struct, only joins the thread.
   A delay of 0 never spawns: the caller posts directly (today's shape). */
typedef struct frame_delayed_post_t {
  uint32_t delay_ms;
  frame_t* f;                 /* BORROWED; dies after frame_destroy joins */
} frame_delayed_post_t;

static void* _frame_delayed_post_main(void* arg) {
  frame_delayed_post_t* d = (frame_delayed_post_t*)arg;
  if (d->delay_ms > 0) platform_sleep_ms(d->delay_ms);
  frame_t* f = d->f;
  if (_frame_engine_die_requested(f) != 0) {
    /* the die rule: no repost into a dying frame — the log carries it */
    log_error("loop: a retry backoff expired into the dying frame '%s' — "
              "dropped loud", frame_sid(f));
  } else {
    _frame_post(&f->actor, (uint32_t)FRM_TURN, NULL, NULL, "retry backoff");
  }
  free(d);
  return NULL;
}

/* Join the in-flight (or already-exited) timer thread; clear the pointer.
   The FRM_TURN dispatch's head and frame_destroy call it — the join's
   ordering against the teardown is what keeps the thread's die-check
   provable against live frame memory. NEVER join when NULL. */
static void _frame_delayed_post_reap(frame_t* f) {
  if (f == NULL || f->delayed_post_thread == NULL) return;
  platform_thread_join(f->delayed_post_thread);
  f->delayed_post_thread = NULL;
}

/* Arm one delayed FRM_TURN; delay_ms > 0 ONLY (0 posts directly, today's
   shape). The thread is JOINABLE so the completion sites reap it. */
int _frame_delayed_post(frame_t* f, uint32_t delay_ms) {
  if (f == NULL || f->st == NULL) return -1;
  if (f->delayed_post_thread != NULL) {
    /* INVARIANT: the engine is sequential — the previous timer's own post
       IS the dispatch that reaps it. Seeing one here is a routing bug's
       loud trace; reap-and-continue keeps the state machine safe anyway. */
    log_error("frame: a second delayed post armed at '%s' — reaping the "
              "stale one first", f->sid_path);
    _frame_delayed_post_reap(f);
  }
  frame_delayed_post_t* d =
      (frame_delayed_post_t*)get_clear_memory(sizeof(frame_delayed_post_t));
  if (d == NULL) return -1;
  d->delay_ms = delay_ms;
  d->f = f;
  platform_thread_t* t = platform_thread_create(_frame_delayed_post_main, d);
  if (t == NULL) {
    free(d);
    return -1;
  }
  f->delayed_post_thread = t;
  return 0;
}

/* The store's outgoing reply: corr-matched to the requester's actor. records
   ownership transfers (the payload destroyer frees them; on a refused send
   the local destroy frees them here). */

/* The keys scan's materialization bound: SA_BUDGET_KEYS_MAX + 1 (one record
   PAST the cap — the truncation tell the reply router reads), clamped down
   to the shared events window when that window is smaller. The clamp is
   HONEST and changes the clipped detection: with SA_FRAME_DEBUG_MAX_EVENTS
   below KEYS_MAX + 1 a fully-materialized clamped scan can no longer prove
   truncation by count alone, so the marker then depends on the window —
   never silently widened here. */
#define SA_FRAME_KEYS_SCAN_CAP                                                \
  ((SA_BUDGET_KEYS_MAX + 1 > SA_FRAME_DEBUG_MAX_EVENTS)                       \
       ? (size_t)SA_FRAME_DEBUG_MAX_EVENTS                                    \
       : (size_t)(SA_BUDGET_KEYS_MAX + 1))

static void _store_reply_send(actor_t* reply_to, uint64_t corr, int rc,
                              char** records, size_t n) {
  if (reply_to == NULL) {
    /* Fire-and-post: the store worker's own log carried the commit/refusal;
       records die here. */
    if (records != NULL) {
      for (size_t i = 0; i < n; i++) free(records[i]);
      free(records);
    }
    return;
  }
  frm_store_reply_payload_t* rp =
      get_clear_memory(sizeof(frm_store_reply_payload_t));
  rp->corr = corr;
  rp->rc = rc;
  rp->n = n;
  rp->records = records;
  _frame_post(reply_to, (uint32_t)FRM_STORE_REPLY, rp, frm_store_reply_payload_destroy,
              "store reply");
}

/* 1 when the frame's OWN behavior is on the call stack (a NESTED sync
   caller): such a caller must never pump this frame's mailbox — a nested
   actor_run's pop frees the outer run's node (the sentinel queue's design;
   the ASan-proven reentrancy hazard) and a nested await would deadlock on a
   reply its own dispatch has to return first. The sync write paths respond
   with fire-and-post (the store's FIFO commits the batch ahead of the
   engine's next awaited trip) and the result-returning reads refuse loud. */
static uint8_t _frame_nested_sync(const frame_t* f) {
  return (f != NULL && f->dispatch_depth > 0) ? 1 : 0;
}

/* The synchronous round-trip refusal (§5's inline-only rule): the direct
   sync APIs pump-wait, and a POOLED store's pacing belongs to its workers —
   mixing an arbitrary caller's thread into pool-driven serialization would
   re-open the lock question. A pooled frame cannot even get here (the
   create/resume guard); an inline frame posting at a pooled store refuses
   loud. */
static int _frame_sync_store_refused(frame_t* f, const char* op) {
  if (f != NULL && f->root != NULL && f->root->store_pool != NULL) {
    log_error("frame: %s at '%s' refuses loud — the root's store actor is "
              "POOLED and the synchronous store API is inline-store-only "
              "(production reaches this effect through the actor paths)",
              op, f->sid_path != NULL ? f->sid_path : "?");
    return 1;
  }
  return 0;
}

/* The sync slot's owned leftovers die here (the recall's transferred text, a
   deadline's late reply's records): every sync compose begins with this reset
   and the frame teardown ends with it. */
static void _frame_sync_slot_reset(frame_t* f) {
  if (f == NULL) return;
  /* The frees run BEFORE the memset — they read the fields the memset zeroes. */
  free(f->sync.text);
  if (f->sync.records != NULL) {
    for (size_t i = 0; i < f->sync.n; i++) free(f->sync.records[i]);
    free(f->sync.records);
  }
  memset(&f->sync, 0, sizeof(f->sync));
}

/* ONE pump cycle over an inline frame's whole round-trip surface
   (frame_internal.h contract — the ONLY pump-order definition). */
void _frame_pump(frame_t* f) {
  if (f == NULL) return;
  actor_run(&f->actor, ACTOR_BATCH_SIZE);
  for (frame_t* a = f->parent; a != NULL; a = a->parent) {
    if (a->st == NULL) continue;         /* live ancestors only */
    if (a->pool != NULL) continue;       /* pooled: scheduler workers own it */
    actor_run(&a->actor, ACTOR_BATCH_SIZE);
  }
  if (f->root != NULL && f->root->store_pool == NULL) {
    actor_run(&f->root->store_actor, ACTOR_BATCH_SIZE);
  }
}

actor_t* _frame_store_actor(frame_t* f) {
  return (f != NULL && f->root != NULL) ? &f->root->store_actor : NULL;
}

uint8_t _frame_store_pooled(const frame_t* f) {
  return (f != NULL && f->root != NULL && f->root->store_pool != NULL) ? 1 : 0;
}

uint64_t _frame_store_corr_next(frame_t* f) {
  return (f != NULL) ? ++f->store_corr_seq : 0;
}

frame_engine_state_t* _frame_engine_state(frame_t* f) {
  return (f != NULL) ? &f->engine : NULL;
}

/* The content path's ONE atomic turn-end batch (frame_internal.h's
   contract): the envelope riders + the assistant msg.append — or, on an
   empty assistant turn, the empty-turn control event (the exact payload
   _loop_control composes) — plus the meta/status=done put for a TOP engine,
   in ONE root batch. The rider group (Task 2; spec §3): a content turn whose
   engine state has the turn open composes step.start, step.end AND
   turn.end {reason completed} in THIS batch — the batch is the turn's first
   durable record (it is the content turn's whole cycle), so the step opens
   and closes with its records, and the turn closes with the TERMINAL
   attribution (the children-yield case ends the turn completed too — the
   completion rides the batch; resuming turns later never re-close it). A
   refused compose rolls the whole pre-allocated seq range back. */
int _frame_engine_finish_post(frame_t* f, const char* append_text,
                              int write_status, uint64_t corr,
                              actor_t* reply_to) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: finish batch on a dead frame");
    return -1;
  }
  /* The envelope riders (the compose-time facts on the engine state; the
     store's records stay the truth). */
  uint8_t ride = (f->engine.engine_live != 0 && f->engine.turn_open != 0 &&
                  f->engine.step_open == 0)
                     ? 1 : 0;
  json_value_t* rider[3];   /* step.start, step.end, turn.end */
  rider[0] = NULL;
  rider[1] = NULL;
  rider[2] = NULL;
  if (ride != 0) {
    rider[0] = lifecycle_step_json(f->engine.turn_counter, 1);
    rider[1] = lifecycle_step_json(f->engine.turn_counter, 1);
    rider[2] = lifecycle_turn_end_json(f->engine.turn_counter,
                                       LIFE_REASON_COMPLETED, NULL);
  }

  json_value_t* payload = json_new_object();
  const char* type_name = (append_text != NULL) ? "msg.append" : "control";
  if (payload == NULL) {
    log_error("frame: out of memory building the finish payload");
    if (rider[0] != NULL) json_value_destroy(rider[0]);
    if (rider[1] != NULL) json_value_destroy(rider[1]);
    if (rider[2] != NULL) json_value_destroy(rider[2]);
    return -1;
  }
  if (append_text != NULL) {
    json_object_set(payload, "role", json_new_string("assistant"));
    json_object_set(payload, "content", json_new_string(append_text));
  } else {
    json_object_set(payload, "kind", json_new_string("empty-turn"));
    json_object_set(payload, "text", json_new_null());
  }

  /* This batch is the turn's FIRST durable record batch: allocate the seqs
     for the whole record group up front (the single-flight compose). */
  size_t nev = (ride != 0) ? 4 : 1;   /* step.start, main, step.end, turn.end */
  uint64_t seq = 0;
  for (size_t i = 0; i < nev; i++) {
    uint64_t s = _frame_seq_alloc(f);
    if (i == 0) seq = s;
  }
  char* evtexts[4];
  char* evkeys[4];
  memset(evtexts, 0, sizeof(evtexts));
  memset(evkeys, 0, sizeof(evkeys));
  const char* evtypes[4];
  evtypes[0] = (ride != 0) ? LIFE_EVENT_STEP_START : type_name;
  evtypes[1] = type_name;
  evtypes[2] = (ride != 0) ? LIFE_EVENT_STEP_END : NULL;
  evtypes[3] = (ride != 0) ? LIFE_EVENT_TURN_END : NULL;
  json_value_t* main_payloads[4];
  main_payloads[0] = rider[0];
  main_payloads[1] = payload;
  main_payloads[2] = rider[1];
  main_payloads[3] = rider[2];
  int rc = 0;
  size_t composed = 0;   /* records whose payload already consumed */
  for (size_t i = 0; i < nev; i++) {
    evtexts[i] = _frame_event_json(f, seq + i, evtypes[i], main_payloads[i]);
    composed = i + 1;
    if (evtexts[i] == NULL) {
      rc = -1;
      break;
    }
  }
  if (rc == 0) {
    for (size_t i = 0; i < nev; i++) {
      evkeys[i] = _frame_event_key(f->sid_path, seq + i);
      if (evkeys[i] == NULL) {
        rc = -1;
        break;
      }
    }
  }
  char* status_val = (write_status != 0)
      ? (char*)get_memory(strlen(SA_FRAME_STATUS_DONE) + 1) : NULL;
  if (status_val != NULL) {
    memcpy(status_val, SA_FRAME_STATUS_DONE, strlen(SA_FRAME_STATUS_DONE) + 1);
  }
  frm_store_op_t put_ops[5];   /* zeroed: is_delete is not a composer's field */
  memset(put_ops, 0, sizeof(put_ops));
  size_t nops = 0;
  for (size_t i = 0; i < nev; i++) {
    put_ops[nops].key = evkeys[i];              /* OWNED by the round trip */
    put_ops[nops].value = (uint8_t*)evtexts[i]; /* OWNED */
    put_ops[nops].value_len = (evtexts[i] != NULL) ? strlen(evtexts[i]) : 0;
    nops++;
  }
  if (status_val != NULL) {
    char* k_status = _frame_subkey(f->sid_path, "meta/status");
    put_ops[nops].key = k_status;               /* OWNED */
    put_ops[nops].value = (uint8_t*)status_val; /* OWNED */
    put_ops[nops].value_len = strlen(SA_FRAME_STATUS_DONE);
    nops++;
  }
  /* The batch cap: the composers mirror the store behavior's check —
     never truncation (the event record alone already carries the big
     text; the status put adds only its fixed-size key). */
  size_t total = 0;
  for (size_t i = 0; i < nops; i++) {
    if (put_ops[i].key == NULL || put_ops[i].value == NULL) {
      log_error("frame: out of memory composing the finish batch at '%s'",
                f->sid_path);
      rc = -1;
      break;
    }
    total += strlen(put_ops[i].key) + put_ops[i].value_len;
  }
  if (rc == 0 && total > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame: the finish batch at '%s' is %zu bytes, exceeding the "
              "%d-byte WAL batch cap — refusing, never truncating",
              f->sid_path, total, (int)SA_FRAME_MAX_BATCH_BYTES);
    rc = -3;
  }
  if (rc != 0) {
    for (size_t i = 0; i < nops; i++) {
      free((void*)put_ops[i].key);
      free((void*)put_ops[i].value);
    }
    for (size_t j = composed; j < nev; j++) {
      json_value_destroy(main_payloads[j]);
    }
    for (size_t i = nev; i > 0; i--) _frame_seq_rollback(f, seq + i - 1);
    return rc;
  }

  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = (frm_store_op_t*)get_clear_memory(nops * sizeof(frm_store_op_t));
  memcpy(bp->ops, put_ops, nops * sizeof(frm_store_op_t));
  bp->nops = nops;
  bp->op_name = "turn finish";                  /* BORROWED literal */
  bp->reply_to = reply_to;
  bp->corr = corr;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "finish batch");
  return 0;
}

/* Bounded pure-drain pump-wait (the _frame_cell_wait shape) until `done_slot`
   flips. 0 = completed; nonzero = the deadline broke (the slot is abandoned —
   the late reply becomes the router's loud drop). */
static int _frame_slot_wait(frame_t* f, uint8_t* done_slot, unsigned timeout_ms) {
  if (*done_slot) return 0;
  uint64_t deadline =
      platform_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
  for (;;) {
    _frame_pump(f);
    if (*done_slot) return 0;
    if (platform_monotonic_ns() >= deadline) {
      log_error("frame: store round trip corr %llu at '%s' still unanswered "
                "after %u ms — the wait gives up (the store keeps the batch; "
                "its reply routes whenever it lands)",
                (unsigned long long)f->sync.corr, f->sid_path, timeout_ms);
      return -1;
    }
    platform_sleep_ms(1);
  }
}

/* The store actor's dispatch (§5): every frame-layer store operation —
   atomic batches, the bounded events scan, the recall resolve walk —
   executes HERE, one message at a time. Serialization comes from being ONE
   actor; no platform lock exists anywhere in the frame layer (the owner's
   amendment). All three behaviors are µs-scale (WaveDB's read path +
   in-memory batch composition only happen in the store's own thread); the
   store stays a PURE DRAIN — it never blocks on a resource. Payloads are
   claimed (msg->payload = NULL) and consumed here on every path. */
static void _store_behavior(void* state, message_t* msg) {
  wave_database_root_t* root = (wave_database_root_t*)state;
  if (msg == NULL) return;
  switch (msg->type) {
    case FRM_STORE_BATCH: {
      /* ONE atomic root batch across every touched subtree: map the ops
         (keys/values stay where the composer left them), cap-check per op
         and in total (the composers' cap checks MIRRORED here — fail-loud,
         no silent truncation), run, and corr-match the reply back. */
      frm_store_batch_payload_t* bp = (frm_store_batch_payload_t*)msg->payload;
      msg->payload = NULL;
      if (bp == NULL) {
        log_error("store: FRM_STORE_BATCH with no payload — dropping loud");
        break;
      }
      int rc = 0;
      raw_op_t* ops = (bp->nops > 0)
          ? (raw_op_t*)get_clear_memory(bp->nops * sizeof(raw_op_t)) : NULL;
      size_t total = 0;
      for (size_t i = 0; i < bp->nops && rc == 0; i++) {
        if (bp->ops[i].key == NULL) {
          log_error("store: batch '%s' op %zu has no key — refused loud",
                    bp->op_name != NULL ? bp->op_name : "?", i);
          rc = -1;
          break;
        }
        if (bp->ops[i].is_delete) {
          /* The DELETE op (WaveDB raw_op_t.type 1): a deletion never
             smuggles bytes — a delete carrying a value or a length refuses
             LOUD and the whole batch with it. */
          if (bp->ops[i].value != NULL || bp->ops[i].value_len != 0) {
            log_error("store: batch '%s' op %zu is a DELETE carrying a "
                      "value — refused loud",
                      bp->op_name != NULL ? bp->op_name : "?", i);
            rc = -1;
            break;
          }
          ops[i].key = bp->ops[i].key;
          ops[i].key_len = strlen(bp->ops[i].key);
          ops[i].value = NULL;
          ops[i].value_len = 0;
          ops[i].type = 1;   /* WaveDB: 0 = put, 1 = delete */
          total += ops[i].key_len;
          continue;
        }
        if (bp->ops[i].value_len > SA_FRAME_MAX_BATCH_BYTES) {
          log_error("store: batch '%s' op %zu's value is %zu bytes, exceeding "
                    "the %d-byte WAL batch cap — refusing, never truncating",
                    bp->op_name != NULL ? bp->op_name : "?", i,
                    bp->ops[i].value_len, (int)SA_FRAME_MAX_BATCH_BYTES);
          rc = -3;
          break;
        }
        ops[i].key = bp->ops[i].key;
        ops[i].key_len = strlen(bp->ops[i].key);
        /* WaveDB's puts need a NON-NULL value even at length 0 (presence
           markers — the graph triple ops' empty marker); the composers heap
           their values. */
        if (bp->ops[i].value == NULL) {
          log_error("store: batch '%s' op %zu has no value — refused loud",
                    bp->op_name != NULL ? bp->op_name : "?", i);
          rc = -1;
          break;
        }
        ops[i].value = (const uint8_t*)bp->ops[i].value;
        ops[i].value_len = bp->ops[i].value_len;
        ops[i].type = 0;   /* all frame-layer writes are puts */
        total += ops[i].key_len + ops[i].value_len;
      }
      if (rc == 0 && total > SA_FRAME_MAX_BATCH_BYTES) {
        log_error("store: batch '%s' is %zu bytes, exceeding the %d-byte WAL "
                  "batch cap — refusing, never truncating",
                  bp->op_name != NULL ? bp->op_name : "?", total,
                  (int)SA_FRAME_MAX_BATCH_BYTES);
        rc = -3;
      }
      if (rc == 0) {
        rc = database_batch_sync_raw(root->db, '/', ops, bp->nops);
        if (rc != 0) {
          log_error("store: batch '%s' failed (%d) at the root — nothing "
                    "committed", bp->op_name != NULL ? bp->op_name : "?", rc);
        } else if (bp->reply_to == NULL) {
          log_info("store: batch '%s' committed (fire-and-post)",
                   bp->op_name != NULL ? bp->op_name : "?");
        }
      }
      actor_t* reply_to = bp->reply_to;         /* borrowed; survives the free */
      uint64_t corr = bp->corr;
      free(ops);                                /* the raw mapping array */
      frm_store_batch_payload_destroy(bp);
      /* The reply fires even on a refusal (the requester awaits corr-matched;
         reply_to NULL = fire-and-post — nothing waits). */
      _store_reply_send(reply_to, corr, rc, NULL, 0);
      break;
    }
    case FRM_STORE_SCAN: {
      /* The bounded reverse range read (the derive's store trip): the
         ABSOLUTE composed bounds (WaveDB's subtree bounded scans are broken
         in both directions — the root-level discipline, unchanged), the
         newest-records materialization moved here, and the raw texts
         REVERSED to ascending order in the reply. The store worker never
         parses JSON. */
      frm_store_scan_payload_t* sp = (frm_store_scan_payload_t*)msg->payload;
      msg->payload = NULL;
      if (sp == NULL) {
        log_error("store: FRM_STORE_SCAN with no payload — dropping loud");
        break;
      }
      path_t* start = (sp->start != NULL)
          ? path_create_from_raw(sp->start, strlen(sp->start), '/', 0) : NULL;
      path_t* end = (sp->end != NULL)
          ? path_create_from_raw(sp->end, strlen(sp->end), '/', 0) : NULL;
      int rc = 0;
      size_t n = 0;
      char** records = NULL;
      if (start == NULL || end == NULL) {
        log_error("store: scan '%s'..'%s' — bound composition failed",
                  sp->start != NULL ? sp->start : "(null)",
                  sp->end != NULL ? sp->end : "(null)");
        if (start != NULL) path_destroy(start);
        if (end != NULL) path_destroy(end);
        rc = -1;
      } else {
        database_iterator_t* iter = database_scan_start_reverse(root->db, start, end);
        if (iter == NULL) {
          path_destroy(start);
          path_destroy(end);
          log_error("store: reverse scan failed — refusing the scan reply");
          rc = -1;
        } else {
          /* The scan honors the payload's declared limit: a scan asking for
             MORE than the store's materialization window (or limit 0) gets
             the window max — clamped, never extended further. */
          size_t cap = sp->limit;
          if (cap == 0 || cap > SA_FRAME_DEBUG_MAX_EVENTS) {
            cap = SA_FRAME_DEBUG_MAX_EVENTS;
          }
          char* texts[SA_FRAME_DEBUG_MAX_EVENTS];   /* newest-first (descending seq) */
          n = 0;
          int oom = 0;
          path_t* key = NULL;
          identifier_t* value = NULL;
          while (n < cap) {
            path_t* k = NULL;
            identifier_t* v = NULL;
            int src = database_scan_prev(iter, &k, &v);
            if (src != 0) break;                 /* -1: out of records, -2: error */
            size_t len = 0;
            uint8_t* data = identifier_get_data_copy(v, &len);
            if (data != NULL) {
              char* text = (char*)get_memory(len + 1);
              if (text == NULL) {
                free(data);
                oom = 1;
              } else {
                memcpy(text, data, len);
                text[len] = '\0';
                texts[n++] = text;
              }
              free(data);
            } else {
              log_error("store: scan record value copy failed");
              oom = 1;
            }
            path_destroy(k);
            identifier_destroy(v);
          }
          database_scan_end(iter);
          /* The scan consumed the bounds' lifetime (they are not destroyed
             again on this path). */
          if (oom) {
            log_error("store: scan materialization hit a bound — the reply "
                      "carries the %zu records it got", n);
          }
          /* Emit ascending (the caller reads oldest -> newest). The mid-
             loop materialization refusals keep the reply's partial shape
             clean (a skipped record never leaves a NULL slot). */
          if (n > 0) {
            records = (char**)get_clear_memory(n * sizeof(char*));
            if (records == NULL) {
              /* The array failed: the materialized texts have no reply to
                 ride — freed here, same discipline as the refusal path. */
              for (size_t i = 0; i < n; i++) free(texts[i]);
              rc = -1;
            } else {
              for (size_t i = 0; i < n; i++) records[i] = texts[n - 1 - i];
            }
          }
        }
      }
      if (rc != 0 && records != NULL) {
        for (size_t i = 0; i < n; i++) free(records[i]);
        free(records);
        records = NULL;
        n = 0;
      }
      _store_reply_send(sp->reply_to, sp->corr, rc, records, records != NULL ? n : 0);
      frm_store_scan_payload_destroy(sp);
      break;
    }
    case FRM_STORE_RECALL: {
      /* The recall resolve walk, run INSIDE this dispatch = one serialized
         read walk (the frame's behavior stays lock-free). The resolved text
         rides records[0]; rc != 0 (walk refused / unresolvable / budget
         spent) answers with no record — today's NULL shape. */
      frm_store_recall_payload_t* rp = (frm_store_recall_payload_t*)msg->payload;
      msg->payload = NULL;
      if (rp == NULL) {
        log_error("store: FRM_STORE_RECALL with no payload — dropping loud");
        break;
      }
      int rc = 0;
      char** records = NULL;
      if (rp->key == NULL || rp->sid_path == NULL ||
          _frame_key_valid(rp->key, "recall") == 0) {
        log_error("store: recall request needs a valid key and a sid path — "
                  "refused loud");
        rc = -1;
      } else {
        database_subtree_t* own = database_subtree_open(root->db, rp->sid_path, '/');
        if (own == NULL) {
          log_error("store: the recall walk's start subtree '%s' is unopenable",
                    rp->sid_path);
          rc = -1;
        } else {
          char* resolved = _frame_recall_walk(own, rp->key, rp->max_hops);
          database_subtree_close(own);
          if (resolved != NULL) {
            records = (char**)get_clear_memory(sizeof(char*));
            if (records == NULL) {
              free(resolved);
              rc = -1;
            } else {
              records[0] = resolved;
            }
          } else {
            rc = -1;   /* unresolvable: today's silent-NULL shape, corr-matched */
          }
        }
      }
      _store_reply_send(rp->reply_to, rp->corr, rc, records, rc == 0 ? 1 : 0);
      if (rc != 0 && records != NULL) free(records);
      frm_store_recall_payload_destroy(rp);
      break;
    }
    case FRM_STORE_KEYS: {
      /* The keys verb's bounded scan (spec §3), run INSIDE this dispatch =
         one serialized read (the frame's behavior stays lock-free). The
         FORWARD range walk over the frame's OWN state/<scope> subtree.
         The listing is lexicographic first-N (spec §3): the FORWARD walk is
         the first-N in one bounded pass — no sort needed in the router for
         the un-clipped case (FRM_STORE_SCAN keeps its reverse walk: it
         exists for newest-events-first).
         ABSOLUTE composed bounds (WaveDB's subtree bounded scans are broken
         in both directions: the root-level discipline, unchanged) —
         materializes the scanned keys' NAME tail segments as the reply's
         records[]; a VALUE never crosses back. One record PAST
         SA_BUDGET_KEYS_MAX rides the reply: the requester's router reads the
         over-cap count as the truncation tell, clips, and appends the
         marker. An empty subtree sends rc 0 with NO records — the empty
         array "[]" answer, not an error. */
      frm_store_keys_payload_t* kp = (frm_store_keys_payload_t*)msg->payload;
      msg->payload = NULL;
      if (kp == NULL) {
        log_error("store: FRM_STORE_KEYS with no payload — dropping loud");
        break;
      }
      int rc = 0;
      size_t n = 0;
      char** records = NULL;
      if (kp->sid_path == NULL || kp->scope == NULL ||
          (strcmp(kp->scope, "local") != 0 && strcmp(kp->scope, "ctx") != 0)) {
        log_error("store: keys request needs a sid path and the scope "
                  "'local' or 'ctx' — refused loud");
        rc = -1;
      } else {
        /* The bounds: <sid_path>/state/<scope> .. <that>0 ("0" sorts past
           every '/'-separated name beneath it — the derive's events-scan
           bound shape, composed root-level). */
        size_t base_len =
            strlen(kp->sid_path) + strlen("/state/") + strlen(kp->scope);
        char* lo = get_memory(base_len + 1);
        char* hi = get_memory(base_len + 2);
        if (lo == NULL || hi == NULL) {
          log_error("store: out of memory composing the keys scan bounds");
          free(lo);
          free(hi);
          rc = -1;
        } else {
          snprintf(lo, base_len + 1, "%s/state/%s", kp->sid_path, kp->scope);
          snprintf(hi, base_len + 2, "%s0", lo);
          path_t* start = path_create_from_raw(lo, strlen(lo), '/', 0);
          path_t* end = path_create_from_raw(hi, strlen(hi), '/', 0);
          free(lo);   /* the bounds' TEXT dies here; the paths ride the scan */
          free(hi);
          if (start == NULL || end == NULL) {
            log_error("store: keys scan '%s/state/%s' — bound composition "
                      "failed", kp->sid_path, kp->scope);
            if (start != NULL) path_destroy(start);
            if (end != NULL) path_destroy(end);
            rc = -1;
          } else {
            database_iterator_t* iter =
                database_scan_start(root->db, start, end);
            if (iter == NULL) {
              log_error("store: keys scan failed — refusing the scan reply");
              rc = -1;
            } else {
              /* The scan honors the clamped cap (SA_FRAME_KEYS_SCAN_CAP,
                 compile-time: KEYS_MAX + 1 clamped to the shared events
                 window — see the macro's honest-clamp note). */
              char* names[SA_FRAME_KEYS_SCAN_CAP];   /* scan order (ascending) */
              n = 0;
              int oom = 0;
              while (n < SA_FRAME_KEYS_SCAN_CAP) {
                path_t* k = NULL;
                identifier_t* v = NULL;
                int src = database_scan_next(iter, &k, &v);
                if (src != 0) {
                  /* -1: out of records; -2: the walk errored mid-pass —
                     the reply carries what was gathered, loud (the scan
                     case's error shape). */
                  if (src < -1) {
                    log_error("store: keys scan failed mid-pass (%d) — the "
                              "reply carries the %zu names it got", src, n);
                  }
                  break;
                }
                if (k != NULL && path_length(k) >= 1) {
                  /* The KEY NAME tail segment (the values are never read):
                     the _frame_restore_seq extraction idiom, unchanged. */
                  identifier_t* last = path_get(k, path_length(k) - 1);
                  size_t len = 0;
                  uint8_t* data = identifier_get_data_copy(last, &len);
                  if (data != NULL) {
                    char* name = (char*)get_memory(len + 1);
                    if (name == NULL) {
                      oom = 1;
                    } else {
                      memcpy(name, data, len);
                      name[len] = '\0';
                      names[n++] = name;
                    }
                    free(data);
                  } else {
                    log_error("store: keys scan record name copy failed");
                    oom = 1;
                  }
                } else {
                  log_error("store: keys scan record lost its key");
                  oom = 1;
                }
                path_destroy(k);
                identifier_destroy(v);
              }
              database_scan_end(iter);
              /* The scan consumed the bounds' lifetime (they are not
                 destroyed again on this path). */
              if (oom) {
                log_error("store: keys scan materialization hit a bound — "
                          "the reply carries the %zu names it got", n);
              }
              if (n > 0) {
                records = (char**)get_clear_memory(n * sizeof(char*));
                if (records == NULL) {
                  /* The array failed: the materialized names have no reply
                     to ride — freed here, same discipline as the refusal
                     path. */
                  for (size_t i = 0; i < n; i++) free(names[i]);
                  n = 0;
                  rc = -1;
                } else {
                  for (size_t i = 0; i < n; i++) records[i] = names[i];
                }
              }
            }
          }
        }
      }
      _store_reply_send(kp->reply_to, kp->corr, rc, records,
                        records != NULL ? n : 0);
      /* The records' ownership left with the reply (or died inside the
         fire-and-post send): every rc != 0 path here refuses with
         records == NULL, so nothing is cleaned after the send. */
      frm_store_keys_payload_destroy(kp);
      break;
    }
    case FRM_STORE_REPLY:   /* replies LEAVE the store; arriving = routing bug */
    default:
      if (msg->payload_destroy != NULL && msg->payload != NULL) {
        msg->payload_destroy(msg->payload);
        msg->payload = NULL;
      }
      log_error("store: unhandled message type %u at the store actor — "
                "dropping loud", (unsigned)msg->type);
      break;
  }
}

/* One event record = ONE store round trip: the record composes against the
   frame's PRE-ALLOCATED seq and the one-op batch is POSTED at the root's
   store actor — no direct write happens at a frame thread any more. The
   store's FIFO order also anchors causality (a fire-and-post control event
   commits ahead of anything its frame posts after it). Nothing is written
   before the batch, so a refusal never half-applies. CONSUMES the payload
   on every path. seq_out carries the PRE-ALLOCATED seq (also on the
   refusal paths — for a best-effort rollback). Returns 0 once POSTED;
   -1/-3 on the pre-post refusals (already logged; nothing was posted). */
int _frame_event_post(frame_t* f, const char* type_name, json_value_t* payload,
                      uint64_t corr, actor_t* reply_to, uint64_t* seq_out) {
  if (seq_out != NULL) *seq_out = 0;
  if (f == NULL || f->st == NULL) {
    log_error("frame: event '%s' on a dead frame",
              type_name != NULL ? type_name : "?");
    json_value_destroy(payload);
    return -1;
  }
  uint64_t seq = _frame_seq_alloc(f);
  if (seq_out != NULL) *seq_out = seq;
  char* text = _frame_event_json(f, seq, type_name, payload);   /* consumes payload */
  if (text == NULL) return -1;

  char* evkey = _frame_event_key(f->sid_path, seq);
  if (evkey == NULL) {
    free(text);
    return -1;
  }

  size_t text_len = strlen(text);
  if (text_len > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame: event record %zu bytes exceeds the %d-byte WAL batch cap "
              "— refusing, never truncating",
              text_len, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(evkey);
    free(text);
    return -3;
  }

  frm_store_batch_payload_t* bp =
      get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = get_clear_memory(sizeof(frm_store_op_t));
  bp->nops = 1;
  bp->ops[0].key = evkey;              /* OWNED: the store behavior frees it */
  bp->ops[0].value = (uint8_t*)text;   /* OWNED */
  bp->ops[0].value_len = text_len;
  bp->op_name = type_name;             /* BORROWED (a type literal) */
  bp->reply_to = reply_to;
  bp->corr = corr;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "event batch");
  return 0;
}

/* Fire-and-post event write (corr 0, reply_to NULL): nothing awaits the
   reply — the engine's control events and PYRT_RESULT's cell.result; a
   pre-post refusal is rolled back in the frame's own seq, a store-stage one
   leaves the recorded gap (the store worker logs it). Returns the pre-post
   rc (0 = posted). */
int _frame_event_post_fire(frame_t* f, const char* type_name,
                           json_value_t* payload) {
  uint64_t seq = 0;
  int rc = _frame_event_post(f, type_name, payload, 0, NULL, &seq);
  if (rc != 0) _frame_seq_rollback(f, seq);
  return rc;
}

/* ONE ATOMIC multi-record EVENT batch (frame_internal.h's contract): every
   op is an event record at the frame's pre-allocated seqs, composed per seq,
   posted as ONE FRM_STORE_BATCH. CONSUMES every payload on every path. */
int _frame_event_batch_post(frame_t* f, const char** type_names,
                            json_value_t** payloads, size_t nops,
                            uint64_t corr, actor_t* reply_to,
                            uint64_t* first_seq_out, const char* op_name) {
  if (first_seq_out != NULL) *first_seq_out = 0;
  if (nops == 0) return 0;   /* nothing to post — a posted nothing */
  if (type_names == NULL || payloads == NULL) {
    log_error("frame: event batch needs type names and payloads");
    for (size_t i = 0; i < nops; i++) json_value_destroy(payloads[i]);
    return -1;
  }
  if (f == NULL || f->st == NULL) {
    log_error("frame: event batch on a dead frame");
    for (size_t i = 0; i < nops; i++) json_value_destroy(payloads[i]);
    return -1;
  }
  /* The seqs pre-allocate CONTIGUOUSLY in one stretch (the compose is
     single-flight — the frame's ONE dispatch thread composes and posts
     without another allocation in between), so the record group's keys are
     first..first+n-1 and the rollback runs in reverse order. */
  uint64_t first = 0;
  for (size_t i = 0; i < nops; i++) {
    uint64_t s = _frame_seq_alloc(f);
    if (i == 0) first = s;
  }
  if (first_seq_out != NULL) *first_seq_out = first;

  char** texts = (char**)get_clear_memory(nops * sizeof(char*));
  char** keys = (char**)get_clear_memory(nops * sizeof(char*));
  size_t composed = nops;
  size_t total = 0;
  int rc = 0;
  for (size_t i = 0; i < nops; i++) {
    texts[i] = _frame_event_json(f, first + i, type_names[i], payloads[i]);
    /* _frame_event_json CONSUMES the payload (on failure too). */
    if (texts[i] == NULL) {
      composed = i + 1;
      rc = -1;
      break;
    }
  }
  if (rc == 0) {
    for (size_t j = 0; j < nops; j++) {
      if (strlen(texts[j]) > SA_FRAME_MAX_BATCH_BYTES) {
        log_error("frame: event record %zu bytes exceeds the %d-byte WAL "
                  "batch cap — refusing, never truncating",
                  strlen(texts[j]), (int)SA_FRAME_MAX_BATCH_BYTES);
        rc = -3;
        break;
      }
      total += strlen(texts[j]);
    }
  }
  if (rc == 0) {
    for (size_t j = 0; j < nops; j++) {
      keys[j] = _frame_event_key(f->sid_path, first + j);
      if (keys[j] == NULL) {
        rc = -1;
        break;
      }
      total += strlen(keys[j]);
    }
    if (rc == 0 && total > SA_FRAME_MAX_BATCH_BYTES) {
      log_error("frame: the event batch at '%s' is %zu bytes, exceeding the "
                "%d-byte WAL batch cap — refusing, never truncating",
                f->sid_path, total, (int)SA_FRAME_MAX_BATCH_BYTES);
      rc = -3;
    }
  }
  if (rc != 0) {
    for (size_t j = 0; j < nops; j++) {
      free(texts[j]);
      free(keys[j]);
    }
    free(texts);
    free(keys);
    for (size_t j = composed; j < nops; j++) json_value_destroy(payloads[j]);
    return rc;
  }

  frm_store_batch_payload_t* bp =
      get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = get_clear_memory(nops * sizeof(frm_store_op_t));
  for (size_t j = 0; j < nops; j++) {
    bp->ops[j].key = keys[j];              /* OWNED: the store behavior frees */
    bp->ops[j].value = (uint8_t*)texts[j]; /* OWNED */
    bp->ops[j].value_len = strlen(texts[j]);
  }
  bp->nops = nops;
  bp->op_name = (op_name != NULL) ? op_name : "event batch";
  bp->reply_to = reply_to;
  bp->corr = corr;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "event batch");
  free(texts);   /* the arrays only; the strings moved into the ops */
  free(keys);
  return 0;
}

/* The fire-and-post shape; a pre-post refusal rolls the whole pre-allocated
   seq range back (reverse order keeps each rollback single-flight). */
int _frame_event_batch_post_fire(frame_t* f, const char** type_names,
                                 json_value_t** payloads, size_t nops,
                                 const char* op_name) {
  uint64_t first = 0;
  int rc = _frame_event_batch_post(f, type_names, payloads, nops, 0, NULL,
                                   &first, op_name);
  if (rc != 0 && first != 0) {
    for (size_t i = nops; i > 0; i--) _frame_seq_rollback(f, first + i - 1);
  }
  return rc;
}

/* The tool path's PAIRED cell.result close (frame_internal.h's contract):
   the audit's answer and the envelope's closers ride ONE atomic
   fire-and-post batch (Task 2 rider 3 — the single-record fire cannot carry
   the three records). */
int _frame_engine_result_close_post(frame_t* f, json_value_t* result_payload,
                                    uint8_t with_riders) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: the paired cell.result close on a dead frame");
    json_value_destroy(result_payload);
    return -1;
  }
  const char* names[3] = {"cell.result", LIFE_EVENT_STEP_END,
                          LIFE_EVENT_TURN_END};
  json_value_t* payloads[3];
  size_t n = 1;
  payloads[0] = result_payload;
  if (with_riders != 0) {
    payloads[n++] = lifecycle_step_json(f->engine.turn_counter, 1);
    payloads[n++] = lifecycle_turn_end_json(f->engine.turn_counter,
                                            LIFE_REASON_COMPLETED, NULL);
  }
  int rc = _frame_event_batch_post_fire(f, names, payloads, n,
                                        "cell.result close");
  if (rc != 0) {
    log_error("frame: the paired cell.result close at '%s' was refused "
              "pre-post", f->sid_path);
  }
  if (with_riders != 0 && rc == 0) {
    /* The close POSTED — the engine's compose-time facts follow (the
       store's records stay the truth; a store-stage refusal left the open
       tail, the driver's loud stall or the resume repair's business). */
    f->engine.turn_open = 0;
    f->engine.step_open = 0;
  }
  return rc;
}

/* The interrupt synthesis (surface-completion spec §2): ONE mechanism, three
   callers, their own texts. arm_cut = 1 for the frame_interrupt entry (cases
   may arm pyrt's boundary cut); arm_cut = 0 for the DEADLINE callers (the
   inline driver's cell deadline, the pooled watchdog) — a deadline must
   never arm a cut against a FUTURE legitimate cell.
   Case 1 (cell pending): the corr-matched cell.result (status 1) + the
   lifecycle riders [step.end, turn.end{aborted}] in ONE fire-and-post
   batch. Case 2 (turn open, no cell): the riders minus cell.result. Case 3
   (nothing open): arm the boundary cut only (per arm_cut), no store write.
   After cases 1-2: the compose-time facts flip exactly like the cell-result
   close (the helper clears turn_open/step_open when the batch posts), the
   interrupt corr is kept (the REAL result drops quietly when it lands), the
   frame poisons, and the ENGINE ends via _frame_engine_terminate (a child
   binds its failure report with the same reason text). A store-stage
   refusal leaves the pre-allocated seq rolled back and the tail to
   resume-repair, the standing discipline.
   GUARD: this function compiles in every python build AND the no-python
   one — the f->pyrt accesses are #ifdef SA_HAS_PYTHON-wrapped INSIDE the
   body. */
void _frame_interrupt_apply(frame_t* f, uint8_t arm_cut,
                            const char* reason_text) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: interrupt apply on a dead frame");
    return;
  }
  const char* names[3];   /* filled in step with payloads (an OOM'd cell.result
                             record is SKIPPED, so indices must never alias) */
  json_value_t* payloads[3];
  size_t n = 0;
  uint8_t cell_was_pending = f->cell_pending;
  uint8_t with_riders =
      (f->engine.engine_live != 0 && f->engine.turn_open != 0) ? 1 : 0;
  if (cell_was_pending != 0) {
    json_value_t* cell_result = json_new_object();
    if (cell_result == NULL) {
      log_error("frame: out of memory building the interrupt's cell.result "
                "at '%s'", f->sid_path);
      /* The cell slot still closes (the interrupt's contract never waits on
         an OOM); the batch goes out without the result record. */
    } else {
      json_object_set(cell_result, "corr",
                      json_new_int((int64_t)f->cell_corr));
      json_object_set(cell_result, "status", json_new_int((int64_t)1));
      json_object_set(cell_result, "text", json_new_string("pyrt: interrupted"));
      names[n] = "cell.result";
      payloads[n++] = cell_result;
    }
  }
  if (with_riders != 0) {
    names[n] = LIFE_EVENT_STEP_END;
    payloads[n++] = lifecycle_step_json(f->engine.turn_counter, 1);
    names[n] = LIFE_EVENT_TURN_END;
    payloads[n++] =
        lifecycle_turn_end_json(f->engine.turn_counter, LIFE_REASON_ABORTED,
                                reason_text);
  }
  if (n == 0) {
    if (cell_was_pending != 0) {
      /* The OOM corner (the review's finding): the cell.result record
         itself OOM'd AND there are no riders — without this branch the
         pending slot would never clear and the case-3 log below would
         misreport the corner as an already-complete cell. The interrupt's
         contract never waits on an OOM: the slot STILL closes inline (no
         durable result record exists — the missing one is the
         resume-repair-visible gap the OOM log above already named) and the
         poison stands, so no later cell can claim the wedged runtime. The
         engine, if it is still live, keeps awaiting a corr whose next real
         result drops quietly — the poisoned refusal and the deadline
         bounds are the containment. */
      log_error("frame: the interrupted cell at '%s' closes WITHOUT its "
                "durable cell.result (out of memory) — the tail's missing "
                "record is resume-repair-visible loud", f->sid_path);
      f->cell_interrupted_corr = f->cell_pyrt_corr;
      f->cell_pending = 0;
      f->cell_status = 1;
      f->pyrt_poisoned = 1;
#ifdef SA_HAS_PYTHON
      if (f->pyrt != NULL) pyrt_interrupt(f->pyrt);
#endif
      return;
    }
    if (arm_cut != 0) {
      /* Case 3, true interrupt: the boundary cut, and nothing else. */
#ifdef SA_HAS_PYTHON
      if (f->pyrt != NULL) pyrt_interrupt(f->pyrt);
#endif
      log_info("frame: the interrupt armed the boundary cut at '%s' "
               "(nothing open was cut)",
               (f->sid_path != NULL) ? f->sid_path : "?");
    } else {
      /* A deadline firing at nothing open: the cell completed before the
         deadline's dispatch ran — a benign race, logged once. Truthful
         wording only when NO cell was pending (the OOM corner above owns
         the pending one). */
      log_info("frame: the cell deadline fired at an already-complete cell "
               "at '%s' — no-op", f->sid_path);
    }
    return;
  }
  int rc = _frame_event_batch_post_fire(f, names, payloads, n,
                                        "interrupt close");
  if (rc == 0) {
    /* The close POSTED: the compose-time facts follow (the store's records
       stay the truth) — mirrors _frame_engine_result_close_post's
       discipline with the ABORTED reason and the interrupted corr's keep. */
    f->engine.turn_open = 0;
    f->engine.step_open = 0;
    if (cell_was_pending != 0) {
      f->cell_interrupted_corr = f->cell_pyrt_corr;
      f->cell_pending = 0;
      f->cell_status = 1;
    }
    f->pyrt_poisoned = 1;
#ifdef SA_HAS_PYTHON
    if (f->pyrt != NULL) pyrt_interrupt(f->pyrt);
#endif
    _frame_engine_terminate(f, 0, reason_text);
    return;
  }
  log_error("frame: the interrupt close at '%s' was refused pre-post — the "
            "tail stays for resume-repair", f->sid_path);
  _frame_engine_terminate(f, 0, reason_text);
}

/* The SYNC-SEMANTICS write (frame_internal.h contract): post the event
   batch with a sync corr and pump-wait the reply — the caller keeps the
   "rc 0 = committed" contract. Its callers are the direct sync APIs
   (frame_append_msg etc.); the ENGINE's writes ride fire-and-post and the
   awaited store round trips instead. POOLED store: refuse loud (the sync
   store API is inline-only). */
int _frame_event_write(frame_t* f, const char* type_name, json_value_t* payload) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: event '%s' on a dead frame",
              type_name != NULL ? type_name : "?");
    json_value_destroy(payload);
    return -1;
  }
  if (_frame_sync_store_refused(f, "event write")) {
    json_value_destroy(payload);
    return -1;
  }
  if (_frame_nested_sync(f)) {
    /* A caller inside this frame's own dispatch cannot await its reply (the
       reply routes when this very dispatch returns) — fire-and-post: the
       store's FIFO commits it ahead of the engine's next awaited trip, and
       a store-stage refusal is the store worker's loud log. */
    log_info("frame: a nested event write '%s' at '%s' posts fire-and-post "
             "(no await inside a dispatch)", type_name, f->sid_path);
    return _frame_event_post_fire(f, type_name, payload);
  }
  uint64_t seq = 0;
  _frame_sync_slot_reset(f);
  f->sync.in_use = 1;
  f->sync.corr = ++f->store_corr_seq;
  int rc = _frame_event_post(f, type_name, payload, f->sync.corr,
                             &f->actor, &seq);
  if (rc != 0) {
    f->sync.in_use = 0;
    _frame_seq_rollback(f, seq);
    return rc;
  }
  int wait_rc = _frame_slot_wait(f, &f->sync.done, SA_FRAME_STORE_WAIT_MS);
  f->sync.in_use = 0;
  if (wait_rc != 0) {
    /* The DEADLINE broke, not the store: the batch may still commit, so the
       seq stays pre-allocated (a gap) — rolling it back could duplicate a
       seq key on the next compose. */
    return -1;
  }
  rc = f->sync.rc;
  if (rc != 0) {
    _frame_seq_rollback(f, seq);
    log_error("frame: '%s' event batch failed (%d); seq %llu of '%s' is "
              "unwritten", type_name != NULL ? type_name : "?", rc,
              (unsigned long long)seq, f->sid_path);
  }
  return rc;
}

/* remember = the state put + the state.remember event in ONE store batch
   (the raw JSON value stored verbatim, today's shape). Failures happen
   BEFORE any write except the batch itself, so no effect half-applies.
   corr 0 / reply NULL = fire-and-post (the store's FIFO anchors causality).
   CONSUMES nothing but its own composed keys; the seq is pre-allocated and
   rolled back on every pre-post refusal. */
static int _frame_remember_post(frame_t* f, const char* key, const char* json_value,
                                const char* state_prefix, uint64_t corr,
                                actor_t* reply_to, uint64_t* seq_out) {
  if (seq_out != NULL) *seq_out = 0;
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

  uint64_t seq = _frame_seq_alloc(f);
  if (seq_out != NULL) *seq_out = seq;
  size_t state_len = strlen(state_prefix) + strlen(key);
  if (state_len > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame: remember '%s' — prefixed state key %zu chars + %zu-byte "
              "value exceeds the %d-byte WAL batch cap — refusing, never "
              "truncating",
              key, state_len, (size_t)strlen(json_value),
              (int)SA_FRAME_MAX_BATCH_BYTES);
    json_value_destroy(parsed);
    _frame_seq_rollback(f, seq);
    return -3;
  }

  json_value_t* payload = json_new_object();   /* {key, value} */
  if (payload == NULL) {
    json_value_destroy(parsed);
    log_error("frame: out of memory building remember payload");
    _frame_seq_rollback(f, seq);
    return -1;
  }
  json_object_set(payload, "key", json_new_string(key));
  json_object_set(payload, "value", parsed);

  char* rel = _frame_state_key(state_prefix, key);
  char* state_key = (rel != NULL) ? _frame_subkey(f->sid_path, rel) : NULL;
  char* evkey = _frame_event_key(f->sid_path, seq);
  char* text = _frame_event_json(f, seq, "state.remember", payload);
  if (rel == NULL || state_key == NULL || evkey == NULL || text == NULL) {
    free(rel);
    free(state_key);
    free(evkey);
    free(text);
    log_error("frame: out of memory building remember batch");
    _frame_seq_rollback(f, seq);
    return -1;
  }
  free(rel);

  size_t text_len = strlen(text);
  if (text_len > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame: event record %zu bytes exceeds the %d-byte WAL batch cap "
              "— refusing, never truncating (trim the value)",
              text_len, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(state_key);
    free(evkey);
    free(text);
    _frame_seq_rollback(f, seq);
    return -3;
  }

  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = (frm_store_op_t*)get_clear_memory(2 * sizeof(frm_store_op_t));
  bp->nops = 2;
  bp->ops[0].key = evkey;              /* OWNED by the store round trip */
  bp->ops[0].value = (uint8_t*)text;   /* OWNED */
  bp->ops[0].value_len = text_len;
  bp->ops[1].key = state_key;          /* OWNED */
  bp->ops[1].value = (uint8_t*)get_memory(strlen(json_value) + 1); /* heap copy —
      the store behavior frees it (the caller's text stays borrowed) */
  if (bp->ops[1].value != NULL) {
    memcpy(bp->ops[1].value, json_value, strlen(json_value) + 1);
    bp->ops[1].value_len = strlen(json_value);
  }
  bp->op_name = "state.remember";      /* BORROWED literal */
  bp->reply_to = reply_to;
  bp->corr = corr;
  if (bp->ops[1].value == NULL) {
    frm_store_batch_payload_destroy(bp);
    _frame_seq_rollback(f, seq);
    log_error("frame: out of memory building remember batch");
    return -1;
  }
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "remember batch");
  return 0;
}

/* The sync public remember (pump-waits the corr-matched reply). */
static int _frame_remember_sync(frame_t* f, const char* key, const char* json_value,
                                const char* state_prefix) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: remember on a dead frame");
    return -1;
  }
  if (_frame_sync_store_refused(f, "remember")) return -1;
  if (_frame_nested_sync(f)) {
    /* Nested (inside this frame's own dispatch): fire-and-post, no await. */
    uint64_t nseq = 0;
    int nrc = _frame_remember_post(f, key, json_value, state_prefix, 0, NULL, &nseq);
    if (nrc != 0) _frame_seq_rollback(f, nseq);
    return (nrc == 0) ? 0 : nrc;
  }
  uint64_t seq = 0;
  _frame_sync_slot_reset(f);
  f->sync.in_use = 1;
  f->sync.corr = ++f->store_corr_seq;
  int rc = _frame_remember_post(f, key, json_value, state_prefix,
                                f->sync.corr, &f->actor, &seq);
  if (rc != 0) {
    f->sync.in_use = 0;
    _frame_seq_rollback(f, seq);
    return rc;
  }
  int wait_rc = _frame_slot_wait(f, &f->sync.done, SA_FRAME_STORE_WAIT_MS);
  f->sync.in_use = 0;
  if (wait_rc != 0) {
    /* Deadline, not refusal: the batch may still commit — the seq gap stays. */
    return -1;
  }
  rc = f->sync.rc;
  if (rc != 0) {
    _frame_seq_rollback(f, seq);
    log_error("frame: remember batch failed (%d); '%s' of %s is unwritten",
              rc, key, f->sid_path);
  }
  return rc;
}

/* msg.append = the conversation-turn event in ONE root batch (the general
   event write, with the {role, content} payload). */
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

  return _frame_event_write(f, "msg.append", payload);
}

/* The TOP-frame report variant (documented loop decision: a report marks the
   reporting frame done at EVERY depth; a child binds its report event into
   the parent's log, a top frame has no parent log and reports into its OWN
   log — payload shape unchanged, so child_sid carries the reportING frame's
   own path). Own event + status done in ONE store batch, POSTED (never a
   direct write at the frame's dispatch); the report verb's corr answer is
   carried by the store reply through the bridge-pending registry, so the
   cell-side caller keeps its corr-matched contract across the extra hop. A
   refusal at the store leaves the pre-allocated seq as the recorded gap. */
static void _frame_report_top_post(frame_t* f, uint64_t bridge_corr,
                                   const char* text) {
  if (f->st == NULL) {
    log_error("frame: report on a dead frame");
    _frame_bridge_reply(bridge_corr, 1, NULL);
    return;
  }
  if (text == NULL) {
    log_error("frame_report: report text required");
    _frame_bridge_reply(bridge_corr, 1, NULL);
    return;
  }

  uint64_t seq = _frame_seq_alloc(f);
  json_value_t* payload = json_new_object();   /* {child_sid, text} */
  if (payload == NULL) {
    log_error("frame: out of memory building report payload");
    _frame_seq_rollback(f, seq);
    _frame_bridge_reply(bridge_corr, 1, NULL);
    return;
  }
  json_object_set(payload, "child_sid", json_new_string(f->sid_path));
  json_object_set(payload, "text", json_new_string(text));
  char* event_text = _frame_event_json_full(f->sid_path, seq, "frame.report",
                                            payload);
  if (event_text == NULL) {
    _frame_seq_rollback(f, seq);
    _frame_bridge_reply(bridge_corr, 1, NULL);
    return;
  }

  char* k_ev = _frame_event_key(f->sid_path, seq);
  char* k_status = _frame_subkey(f->sid_path, "meta/status");
  if (k_ev == NULL || k_status == NULL) {
    free(k_ev);
    free(k_status);
    free(event_text);
    _frame_seq_rollback(f, seq);
    _frame_bridge_reply(bridge_corr, 1, NULL);
    return;
  }

  size_t total_bytes = strlen(k_ev) + strlen(event_text) +
                       strlen(k_status) + strlen(SA_FRAME_STATUS_DONE);
  if (total_bytes > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame_report: top report batch for '%s' is %zu bytes, exceeding "
              "the %d-byte WAL batch cap — refusing, never truncating",
              f->sid_path, total_bytes, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(k_ev);
    free(k_status);
    free(event_text);
    _frame_seq_rollback(f, seq);
    _frame_bridge_reply(bridge_corr, 1, NULL);
    return;
  }

  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = (frm_store_op_t*)get_clear_memory(2 * sizeof(frm_store_op_t));
  bp->nops = 2;
  bp->ops[0].key = k_ev;                    /* OWNED by the store round trip */
  bp->ops[0].value = (uint8_t*)event_text;  /* OWNED */
  bp->ops[0].value_len = strlen(event_text);
  bp->ops[1].key = k_status;                /* OWNED */
  bp->ops[1].value = (uint8_t*)get_memory(strlen(SA_FRAME_STATUS_DONE) + 1);
  if (bp->ops[1].value != NULL) {
    memcpy(bp->ops[1].value, SA_FRAME_STATUS_DONE, strlen(SA_FRAME_STATUS_DONE) + 1);
    bp->ops[1].value_len = strlen(SA_FRAME_STATUS_DONE);
  }
  if (bp->ops[1].value == NULL) {
    frm_store_batch_payload_destroy(bp);
    _frame_seq_rollback(f, seq);
    _frame_bridge_reply(bridge_corr, 1, NULL);
    return;
  }
  bp->op_name = "frame.report (top)";       /* BORROWED literal */
  bp->reply_to = &f->actor;                 /* the reply routes to the bridge corr */
  bp->corr = ++f->store_corr_seq;
  _frame_bridge_pending_add(f, bp->corr, bridge_corr,
                            FRM_BRIDGE_TEXT);   /* BEFORE the post */
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "top report batch");
}

/* Dispatch a queued FRM_* message. Every store write/read composes here and
   POSTS at the root's store actor (§5: the ONE serializer); the corr-matched
   reply routes back through _frame_store_reply_route, which answers THROUGH
   the bridge hook (frame_bridge.h) for registered cell verbs or into the
   direct sync caller's slot. The frame never blocks beyond a µs post; the
   awaiting python cell blocks on its own completion record, woken when the
   reply's router hands its corr to the bridge sink.

   Payload ownership: the request payloads are CONSUMED here
   (msg->payload = NULL; the behavior destroys the payload itself), which
   makes both delivery paths (queue + actor_run, and the tests' direct
   frame_dispatch) work without claiming it twice. FRM_REPLY never arrives at
   a frame (the answers leave through the bridge hook — a routing bug).
   The loop's verbs are claimed here: FRM_CELL_EXECUTE boots/feeds the
   frame's own pyrt, PYRT_RESULT completes the one pending-cell slot,
   FRM_STOP sets the drain-then-stop flag the loop reads; FRM_SPAWN /
   FRM_REPORT (the cell-side bridge verbs posted by py_agent.c) compose the
   same store operations the direct API uses — now as store round trips
   answered by the reply router. Unknown types are ignored. */

/* Forward (the store round trips live in the store-ops sections below):
   the behaviors compose + post them. */
static int _frame_recall_post(frame_t* f, const char* key, uint64_t corr,
                              actor_t* reply_to);
static int _frame_keys_post(frame_t* f, const char* scope, uint64_t corr,
                            actor_t* reply_to);
static frame_t* _frame_spawn_post(frame_t* parent, const char* goal,
                                  const char* context_json,
                                  uint64_t bridge_corr, uint64_t corr);
/* _frame_report_bind_post — declared in frame_internal.h (the turn engine's
   terminal step is one of its callers; loop.c runs the engine). */
static void _frame_report_bind_compose(frame_t* parent,
                                       frm_report_bind_payload_t* b);

/* --- the keys verb's reply composition (surface-completion spec §3) ------- */

/* The qsort comparator: the scanned key names ascending by strcmp. */
static int _frame_keys_cmp(const void* a, const void* b) {
  const char* const* sa = (const char* const*)a;
  const char* const* sb = (const char* const*)b;
  return strcmp(*sa, *sb);
}

/* The keys verb's reply text: the store's forward walk already emits the
   names ascending, so the records arrive sorted — the qsort stays as the
   router's own defense (the records are reordered IN PLACE, never freed
   here). The clip + marker are the contract: keep the lexicographic FIRST
   SA_BUDGET_KEYS_MAX, and — ONLY when the scan carried more than the cap —
   close with the marker. An empty scan composes "[]". Returns a heap text
   the caller frees, or NULL loud on an OOM refusal. */
static char* _frame_keys_reply_text(char** records, size_t n) {
  size_t listed = (n > SA_BUDGET_KEYS_MAX) ? SA_BUDGET_KEYS_MAX : n;
  if (records != NULL && n > 0) {
    qsort(records, n, sizeof(char*), _frame_keys_cmp);
  }
  json_value_t* arr = json_new_array();
  if (arr == NULL) {
    log_error("frame: out of memory composing the keys reply array");
    return NULL;
  }
  if (records != NULL) {
    for (size_t i = 0; i < listed; i++) {
      json_value_t* name = json_new_string(records[i] != NULL ? records[i] : "");
      /* json_array_append returns 1 = the value was consumed; 0 = refused
         WITHOUT taking it (the string is still ours to destroy). */
      if (name == NULL || json_array_append(arr, name) != 1) {
        json_value_destroy(name);
        json_value_destroy(arr);
        log_error("frame: out of memory composing the keys reply array");
        return NULL;
      }
    }
  }
  if (n > SA_BUDGET_KEYS_MAX) {
    json_value_t* marker = json_new_string("[budget: keys truncated]");
    if (marker == NULL || json_array_append(arr, marker) != 1) {
      json_value_destroy(marker);
      json_value_destroy(arr);
      log_error("frame: out of memory composing the keys truncation marker");
      return NULL;
    }
  }
  char* text = json_serialize(arr);
  json_value_destroy(arr);
  if (text == NULL) {
    log_error("frame: out of memory serializing the keys reply");
  }
  return text;
}

/* The FRM_STORE_REPLY router (the frame's OWN reply path; consumes the
   payload on every path). Priority order:
     1. spawn_pending  — the admission's commit/refusal;
     2. bind_pending   — the report bind's result (the child's seq rollback +
       the cell's bridge answer live here);
     3. f->sync + corr match — the DIRECT sync caller's slot (single slot:
       direct sync calls are sequential on the pump owner's thread in the
       only shape that supports them — the inline shape);
     4. the engine's store round trips: Task 3's handlers route here;
     5. the registered cell-verb bridge corrs — answered through the bridge
       sink;
     6. otherwise a LOUD drop (a routing bug / an abandoned slot's late
       reply). */
static void _frame_store_reply_route(frame_t* f, frm_store_reply_payload_t* r) {
  if (f == NULL) {
    if (r != NULL) frm_store_reply_payload_destroy(r);
    return;
  }
  if (r == NULL) return;

  /* 1. The spawn admissions (the corr-keyed entries). */
  {
    frm_spawn_pending_t* pe = f->spawn_pending;
    while (pe != NULL && pe->corr != r->corr) pe = pe->next;
    if (pe != NULL) {
      pe->done = 1;
      pe->rc = r->rc;
      if (r->rc != 0) {
        log_error("frame: the spawn admission for '%s' was refused by the "
                  "store (%d) — nothing committed",
                  (pe->child != NULL) ? frame_sid(pe->child) : "?", r->rc);
        _frame_seq_rollback(f, pe->own_seq);
      }
      /* START (spawn = admit + start; the plan's reply-handler branch): on a
         CONFIRMED commit and a LIVE turn engine on this frame, the admitted
         child becomes the admission's live product — it inherits this
         frame's engine knobs (loop_turn_cap) and the BORROWED backend
         override (frame.h documents the borrowed inheritance: production
         leaves it NULL for everyone; tests set it once on the parent), its
         engine starts (ONE FRM_TURN queued here or scheduled on the
         inherited pool), and it is counted among this frame's live
         children. ONLY now — never before the batch was handed to the
         store. Admissions OVERLAP (a cell's next actor.spawn can compose
         while a previous reply still routes), so each entry stands alone. */
      uint8_t started = 0;
      if (r->rc == 0 && pe->child != NULL) {
        frame_t* child = pe->child;
        if (f->engine.engine_live != 0 && child->st != NULL) {
          child->loop_turn_cap = f->loop_turn_cap;
          child->backend = f->backend;
          if (_frame_engine_start(child) == 0) {
            started = 1;
            f->engine.live_children++;
            if (pe->bridge_corr != 0) {
              /* A cell-verb child is UNADOPTED (no caller takes it): this
                 parent owns the record and frees it at its teardown — the
                 counter keeps the parent's engine schedulable (the
                 pending-children shape), never a CAS teardown claim. */
              child->owned_next = f->owned_children;
              f->owned_children = child;
            }
          }
          /* else: frame_start logged loud (dead/already-live engine). */
        } else if (f->engine.engine_live == 0) {
          log_info("frame: the admission of '%s' committed under '%s' with "
                   "no live engine on the parent — the child stays "
                   "admission-only (the engine-less caller starts or drives "
                   "it)", frame_sid(child), f->sid_path);
        }
      }
      if (pe->bridge_corr != 0) {
        /* The cell-verb corr answer: the child's sid text (already known at
           compose — the wait covered only the store hop; when the cell
           already moved on, py_agent's own registry logs the drop loud). */
        char* sid = (r->rc == 0 && pe->child != NULL)
                        ? strdup(frame_sid(pe->child)) : NULL;
        _frame_bridge_reply(pe->bridge_corr, (r->rc == 0) ? 0 : 1, sid);
        free(sid);
        if (!started) {
          /* NEVER started (the admission was refused, or no live engine
             could own a cell-verb child): the record is released — the
             admission-only shape. */
          if (r->rc == 0) {
            log_error("frame: the cell-verb spawn's child '%s' stays "
                      "unstarted at '%s' (no live engine on the parent) — "
                      "released loud", frame_sid(pe->child), f->sid_path);
          } else {
            log_error("frame: the cell-verb spawn's child for corr %llu is "
                      "released loud (the admission was refused)",
                      (unsigned long long)pe->bridge_corr);
          }
          frame_destroy(pe->child);
        }
        pe->child = NULL;    /* a STARTED child's record is the tracked
                                teardown's now */
        /* The router unlinks + frees the entry (the cell-verb caller is
           done with it). */
        frm_spawn_pending_t** p = &f->spawn_pending;
        while (*p != NULL && *p != pe) p = &(*p)->next;
        if (*p == pe) *p = pe->next;
        free(pe);
      }
      /* bridge_corr == 0: the direct sync caller reads its entry (done/rc/
         child) and takes it off the list itself — the entry's memory is
         this frame's dispatch thread's, so the read is race-free. */
      frm_store_reply_payload_destroy(r);
      return;
    }
  }

  /* 2. The report bind (routed back to the CHILD's actor). */
  if (f->bind_slot.in_use && r->corr == f->bind_slot.corr) {
    f->bind_slot.done = 1;
    f->bind_slot.rc = r->rc;
    if (r->rc != 0) {
      log_error("frame: the report bind from '%s' was refused (%d) — its own "
                "seq rolls back best-effort; the refusing batch committed "
                "nothing", f->sid_path, r->rc);
      _frame_seq_rollback(f, f->bind_slot.own_seq);
    }
    if (f->bind_slot.bridge_corr != 0) {
      _frame_bridge_reply(f->bind_slot.bridge_corr, (r->rc == 0) ? 0 : 1, NULL);
    }
    /* engine_driven == 1: the parent's resume posts FROM THE CONFIRMED COMMIT
       — and from a refusal too (a parent must never hang because a WAL write
       failed; the seq rollback above ran best-effort and the refusal is
       logged). The bind slot carries the terminate's own failed flag: a
       quiet-completion bind resumes with failed = 0, the failure binds
       resume with failed = 1 (the parent's log_error names the child). */
    if (f->bind_slot.engine_driven != 0) {
      if (r->rc != 0) {
        log_error("frame: the engine-driven report bind from '%s' was refused "
                  "(%d) — the parent still resumes (never hang on a WAL "
                  "failure)", f->sid_path, r->rc);
      }
      _frame_child_notify_post(f, f->bind_slot.failed);
    }
    f->bind_slot.in_use = 0;
    frm_store_reply_payload_destroy(r);
    return;
  }

  /* 3. The direct sync caller's slot. */
  if (f->sync.in_use && r->corr == f->sync.corr) {
    f->sync.done = 1;
    f->sync.rc = r->rc;
    if (r->rc == 0) {
      /* The reply's raw records transfer WHOLE (the sync scan joins them
         into its array text; the recall's resolution is records[0]). */
      f->sync.records = r->records;
      f->sync.n = r->n;
      r->records = NULL;
      r->n = 0;
      f->sync.text = (f->sync.n >= 1 && f->sync.records[0] != NULL)
          ? strdup(f->sync.records[0]) : NULL;   /* the recall's resolution */
    }
    frm_store_reply_payload_destroy(r);
    return;
  }

  /* 4. The engine's store round trips (Task 3): loop.c's handler continues
     the turn step inside this dispatch (the payload's ownership moves with
     it) when the engine awaits one. */
  if (f->engine.engine_live != 0 && f->engine.phase == FRAME_PHASE_STORE &&
      f->engine.store_corr != 0 && r->corr == f->engine.store_corr) {
    _frame_engine_store_reply(f, r);   /* consumes the payload */
    return;
  }

  /* 5. The registered cell-verb bridge corrs. */
  {
    frm_bridge_kind_e kind = FRM_BRIDGE_TEXT;
    uint64_t answer_corr = 0;
    if (_frame_bridge_pending_take(f, r->corr, &answer_corr, &kind)) {
      uint8_t status = (r->rc == 0) ? 0 : 1;
      char* text = NULL;
      if (kind == FRM_BRIDGE_RECALL) {
        if (status == 0 && r->n >= 1 && r->records != NULL &&
            r->records[0] != NULL) {
          text = strdup(r->records[0]);
          if (text == NULL) status = 1;
        }
      } else if (kind == FRM_BRIDGE_KEYS) {
        if (status == 0) {
          text = _frame_keys_reply_text(r->records, r->n);
          if (text == NULL) status = 1;
        }
      }
      _frame_bridge_reply(answer_corr, status, text);
      free(text);
      frm_store_reply_payload_destroy(r);
      return;
    }
  }

  /* 6. */
  log_error("frame: unmatched store reply corr %llu at '%s' — dropped loud",
            (unsigned long long)r->corr, f->sid_path);
  frm_store_reply_payload_destroy(r);
}

static void _frame_behavior_impl(void* state, message_t* msg) {
  frame_t* f = (frame_t*)state;
  if (msg == NULL) return;
  switch (msg->type) {
    case FRM_REMEMBER: {
      /* The compose-and-POST shape (§5): the INHERITABLE ctx write posts at
         the store actor; the corr answer comes from the FRM_STORE_REPLY
         router via the bridge registry (registered BEFORE the post). */
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
      } else {
        /* Bridge verbs write the INHERITABLE layer: a cell's remember() is a
           durable, shared-by-default write (state/ctx/); cells use the
           ctx/local split only through the store API directly. */
        uint64_t store_corr = ++f->store_corr_seq;
        _frame_bridge_pending_add(f, store_corr, corr, FRM_BRIDGE_TEXT);
        if (_frame_remember_post(f, rp->key, rp->json_value, "state/ctx/",
                                 store_corr, &f->actor, NULL) != 0) {
          /* The compose refused loud (validation/cap) — nothing was posted,
             so the corr answer comes NOW, not from the store. */
          (void)_frame_bridge_pending_take(f, store_corr, NULL, NULL);
          status = 1;
          log_error("frame: FRM_REMEMBER '%s' refused at '%s' (corr %llu)",
                    rp->key ? rp->key : "(null)", f->sid_path,
                    (unsigned long long)corr);
          _frame_bridge_reply(corr, status, NULL);
        }
        /* else: the router answers the corr at the store reply. */
      }
      frm_remember_payload_destroy(rp);
      break;
    }
    case FRM_RECALL: {
      /* The resolve walk is a store MESSAGE now: compose + post; the router
         answers (the resolved text or the refusal status) at the reply. */
      frm_remember_payload_t* rp = (frm_remember_payload_t*)msg->payload;
      msg->payload = NULL;
      uint64_t corr = 0;
      uint8_t status;
      if (rp == NULL) {
        status = 1;
        log_error("frame: FRM_RECALL with no payload at '%s'", f->sid_path);
      } else if ((corr = rp->corr) == 0) {
        status = 1;
        log_error("frame: FRM_RECALL with corr 0 at '%s' — nothing to match",
                  f->sid_path);
      } else {
        uint64_t store_corr = ++f->store_corr_seq;
        _frame_bridge_pending_add(f, store_corr, corr, FRM_BRIDGE_RECALL);
        if (_frame_recall_post(f, rp->key, store_corr, &f->actor) != 0) {
          (void)_frame_bridge_pending_take(f, store_corr, NULL, NULL);
          status = 1;
          log_error("frame: FRM_RECALL '%s' refused at '%s' (corr %llu)",
                    rp->key ? rp->key : "(null)", f->sid_path,
                    (unsigned long long)corr);
          _frame_bridge_reply(corr, status, NULL);
        }
      }
      frm_remember_payload_destroy(rp);
      break;
    }
    case FRM_KEYS: {
      /* The pulled-forward inspect member (spec §3): the listing is a
         bounded scan of the frame's OWN state/<scope> subtree — KEY NAMES
         ONLY — and the scope is a CLOSED set validated HERE, before any
         post: anything else is the standard fail-loud corr-matched
         refusal. The store compose + post mirrors FRM_RECALL exactly; the
         router answers the sorted key-name array at the reply. */
      frm_remember_payload_t* rp = (frm_remember_payload_t*)msg->payload;
      msg->payload = NULL;
      uint64_t corr = 0;
      uint8_t status;
      if (rp == NULL) {
        status = 1;
        log_error("frame: FRM_KEYS with no payload at '%s'", f->sid_path);
      } else if ((corr = rp->corr) == 0) {
        status = 1;
        log_error("frame: FRM_KEYS with corr 0 at '%s' — nothing to match",
                  f->sid_path);
      } else if (rp->key == NULL ||
                 (strcmp(rp->key, "local") != 0 && strcmp(rp->key, "ctx") != 0)) {
        status = 1;
        log_error("agent.keys: unknown scope '%s' at '%s' (corr %llu) — the "
                  "scope is 'local' or 'ctx'",
                  rp->key ? rp->key : "(null)", f->sid_path,
                  (unsigned long long)corr);
        _frame_bridge_reply(corr, status, NULL);
      } else {
        uint64_t store_corr = ++f->store_corr_seq;
        _frame_bridge_pending_add(f, store_corr, corr, FRM_BRIDGE_KEYS);
        if (_frame_keys_post(f, rp->key, store_corr, &f->actor) != 0) {
          (void)_frame_bridge_pending_take(f, store_corr, NULL, NULL);
          status = 1;
          log_error("frame: FRM_KEYS '%s' refused at '%s' (corr %llu)",
                    rp->key, f->sid_path, (unsigned long long)corr);
          _frame_bridge_reply(corr, status, NULL);
        }
      }
      frm_remember_payload_destroy(rp);
      break;
    }
    case FRM_REPLY:
      /* Replies LEAVE frames through the bridge hook; a frame cannot wait on
         one. This only happens from a routing bug: loud, dropped. */
      log_error("frame: FRM_REPLY received at '%s' — replies are bridge-hook "
                "side effects, not queued messages; dropping", f->sid_path);
      break;
    case FRM_CELL_EXECUTE: {
      /* The loop's verb (claimed in Task 10): boot the frame's OWN pyrt
         lazily, run the cell, and let the PYRT_RESULT dispatch complete the
         one pending-cell slot (the paired cell.result event is written by
         that completion). Synchronous REFUSAL paths never set the pending
         flag; they fill the status instead — the loop's wait reads the slot
         state directly when it sees no pending cell. */
      frm_cell_payload_t* cp = (frm_cell_payload_t*)msg->payload;
      msg->payload = NULL;
      if (cp == NULL) {
        log_error("frame: FRM_CELL_EXECUTE with no payload at '%s'", f->sid_path);
        break;
      }
      if (cp->corr == 0 || cp->code == NULL) {
        log_error("frame: FRM_CELL_EXECUTE needs corr and code at '%s'",
                  f->sid_path);
        frm_cell_payload_destroy(cp);
        break;
      }
      if (f->cell_pending) {
        log_error("frame: FRM_CELL_EXECUTE corr %llu refused — a cell is "
                  "already in flight at '%s' (the loop is strictly "
                  "single-cell)",
                  (unsigned long long)cp->corr, f->sid_path);
        f->cell_status = 1;
        frm_cell_payload_destroy(cp);
        break;
      }
      if (f->pyrt_poisoned != 0) {
        /* The poison contract (surface-completion spec §2): an interrupted
           frame's cells refuse loud until the frame is torn down. The slot
           fills synchronously (status, no pending) — the engine's refusal
           composer writes the CORR-MATCHED paired cell.result from
           _frame_cell_refusal_text's wording; the standing refusal
           contract, never a new path. */
        log_error("frame: cell corr %llu refused — runtime poisoned by an "
                  "interrupted cell at '%s'",
                  (unsigned long long)cp->corr, f->sid_path);
        f->cell_status = 1;
        frm_cell_payload_destroy(cp);
        break;
      }
#ifdef SA_HAS_PYTHON
      /* The frame's OWN runtime: owner = this frame's actor, so cells'
         agent.* verbs arrive in the frame inbox (answered corr-matched by
         the behaviors above) and results come back as PYRT_RESULT. */
      if (f->pyrt == NULL) {
        pyrt_config_t pc;
        pc.backend = SA_PYRT_BACKEND_SUBINTERPRETER;
        pc.pool_cap = 0;             /* the process-wide default (2x cores) */
        pc.idle_evict_ms = 0;
        f->pyrt = pyrt_create(&f->actor, &pc);
        if (f->pyrt == NULL) {
          log_error("frame: pyrt runtime boot failed at '%s' — cell corr %llu "
                    "answered as a failed result",
                    f->sid_path, (unsigned long long)cp->corr);
          f->cell_status = 1;
          frm_cell_payload_destroy(cp);
          break;
        }
      }
      f->cell_corr = cp->corr;
      f->cell_status = 0;
      f->cell_pyrt_corr = pyrt_execute(f->pyrt, strdup(cp->code));
      if (f->cell_pyrt_corr == 0) {
        /* pyrt's boot/execute refusal contract: the RESULT never arrives, so
           the cell completes right here as a failure. */
        log_error("frame: pyrt execute refused corr %llu at '%s'",
                  (unsigned long long)cp->corr, f->sid_path);
        f->cell_status = 1;
        frm_cell_payload_destroy(cp);
        break;
      }
      f->cell_pending = 1;
      _frame_cell_watchdog_arm(f);   /* POOLED only: the deadline watches
                                        this one cell (spec §2); the arm's
                                        pool check makes pool-less shapes
                                        free, and the SYNCHRONOUS refusal
                                        paths above never reach it */
#else
      log_error("frame: FRM_CELL_EXECUTE at '%s' but this build has no python "
                "runtime — answering as a failed cell (corr %llu)",
                f->sid_path, (unsigned long long)cp->corr);
      f->cell_status = 1;
#endif
      frm_cell_payload_destroy(cp);
      break;
    }
#ifdef SA_HAS_PYTHON
    case PYRT_RESULT: {
      /* The cell finished. When it is the pending cell: write the paired
         cell.result event AND complete the slot in the same dispatch —
         audit and wait-state move together. An unmatched result (nothing
         pending, or a stale corr) is dropped loud: nothing in this module
         executes cells outside the loop's single-cell discipline. */
      pyrt_result_payload_t* r = (pyrt_result_payload_t*)msg->payload;
      msg->payload = NULL;
      if (r == NULL) {
        log_error("frame: PYRT_RESULT with no payload at '%s'", f->sid_path);
        break;
      }
      /* The disarm FIRST (surface-completion spec §2): the real result's
         arrival ends the armed watchdog's reason to exist for BOTH the
         matched and the quiet-drop (poisoned frame's late) outcomes — the
         struct's f->cell_watchdog is NULLed here, so a later
         FRM_CELL_WATCHDOG message can never double-apply. The frame's
         dispatch thread is the disarm's single writer. */
      _frame_cell_watchdog_disarm(f);
      if (f->cell_pending && r->corr != 0 && r->corr == f->cell_pyrt_corr) {
        json_value_t* result_payload = json_new_object();
        if (result_payload == NULL) {
          log_error("frame: out of memory building cell.result payload at '%s'",
                    f->sid_path);
        } else {
          json_object_set(result_payload, "corr",
                          json_new_int((int64_t)f->cell_corr));
          json_object_set(result_payload, "status",
                          json_new_int((int64_t)r->status));
          json_object_set(result_payload, "text",
                          (r->text != NULL) ? json_new_string(r->text)
                                            : json_new_null());
          /* FIRE-AND-POST (corr 0, reply_to NULL): the slot completes in
             this same dispatch exactly as today, and the event's commitment
             is FIFO-ahead of anything the frame posts after it (the next
             derive's scan). The TURN-LIFECYCLE riders (Task 2 rider 3): a
             live engine whose turn is open and whose awaited cell this IS
             (phase CELL) closes the turn right here — step.end +
             turn.end {completed} ride the SAME atomic batch, so the audit's
             answer and its envelope closers can never split. */
          uint8_t with_riders =
              (f->engine.engine_live != 0 &&
               f->engine.phase == FRAME_PHASE_CELL &&
               f->engine.turn_open != 0)
                  ? 1 : 0;
          if (_frame_engine_result_close_post(f, result_payload,
                                              with_riders) != 0) {
            log_error("frame: the cell.result event for corr %llu was refused "
                      "pre-post at '%s' (already logged)",
                      (unsigned long long)f->cell_corr, f->sid_path);
          }
        }
        f->cell_status = r->status;
        f->cell_pending = 0;
        /* The engine awaits the cell (Task 3): repost the turn continuation
           — a no-op unless the engine is live in FRAME_PHASE_CELL. */
        _frame_engine_cell_done(f);
      } else if (f->cell_interrupted_corr != 0 &&
                 r->corr == f->cell_interrupted_corr) {
        /* The interrupted cell's REAL result, late and unclaimable (the
           poison contract): the EXPECTED shape the interrupt synthesis set
           up — a documented quiet drop, distinct from the genuinely-
           unknown-corr loud error below (surface-completion spec §2). */
        log_info("frame: the interrupted cell's real result (pyrt corr %llu) "
                 "landed after the synthesis — dropped quietly (the poison "
                 "contract)",
                 (unsigned long long)r->corr);
      } else {
        log_error("frame: unclaimed PYRT_RESULT corr %llu at '%s' — no "
                  "pending cell matches; dropping",
                  (unsigned long long)r->corr, f->sid_path);
      }
      pyrt_result_payload_destroy(r);
      break;
    }
    case PYRT_EMIT: {
      /* The write verb's durable half (surface-completion spec §1): ONE
         record in the frame's OWN events stream — the single-record
         fire-and-post (corr 0, reply_to NULL), the same shape as the
         cell.result audit write minus the lifecycle riders (emit is a side
         effect, not a step boundary; it composes while a cell is pending,
         between turns, and in the demo driver alike). The text arrives
         source-capped (py_agent's SA_BUDGET_EMIT_BYTES cut — the frame
         trusts bounded text from its runtime). Store-refused: the loud
         pre-post refusal contract, no corr to answer. */
      pyrt_text_payload_t* t = (pyrt_text_payload_t*)msg->payload;
      msg->payload = NULL;
      if (t == NULL) {
        log_error("frame: PYRT_EMIT with no payload at '%s'", f->sid_path);
        break;
      }
      /* The text value is composed FIRST and refused pre-post when it fails:
         a json_object_set on a NULL value silently drops the key and the
         record would ship hollow ({}) — an empty durable emit, no loud
         trace. (A NULL t->text still composes the JSON null — a null emit is
         a shape, not a failure.) */
      json_value_t* text_value =
          (t->text != NULL) ? json_new_string(t->text) : json_new_null();
      if (text_value == NULL) {
        log_error("frame: out of memory building the emit payload's text at "
                  "'%s' — the emit record is refused pre-post", f->sid_path);
        pyrt_text_payload_destroy(t);
        break;
      }
      json_value_t* payload = json_new_object();
      if (payload == NULL) {
        json_value_destroy(text_value);
        log_error("frame: out of memory building the emit payload at '%s' — "
                  "the emit record is refused pre-post", f->sid_path);
        pyrt_text_payload_destroy(t);
        break;
      }
      json_object_set(payload, "text", text_value);
      if (_frame_event_post_fire(f, "emit", payload) != 0) {
        log_error("frame: the emit record was refused pre-post at '%s' "
                  "(already logged)", f->sid_path);
      }
      pyrt_text_payload_destroy(t);
      break;
    }
    case PYRT_LOG:
    case PYRT_STATUS: {
      /* Deliberately NOT store records: log/status are MESSAGES (verbs —
         narration and heartbeat), not nouns in the audit stream (spec §1);
         the frame consumes them on receipt as loud log lines, the derive
         never renders them. Replaying them into the store would turn the
         frame's context into narration history. */
      pyrt_text_payload_t* t = (pyrt_text_payload_t*)msg->payload;
      msg->payload = NULL;
      log_info("frame: pyrt %s at '%s': %s",
               (msg->type == (uint32_t)PYRT_LOG) ? "log" : "status",
               f->sid_path,
               (t != NULL && t->text != NULL) ? t->text : "(none)");
      if (t != NULL) pyrt_text_payload_destroy(t);
      break;
    }
#endif
    case FRM_TURN:
      /* The engine's scheduled turn-step continuation (Task 3): ONE turn
         step — the checks and the derive's store round trip — then a yield,
         with the step continued by the arrival dispatches. No payload.
         The HEAD REAPS the retry backoff's timer first (guards spec §3):
         the previous backoff's own FRM_TURN IS this dispatch, so its join
         is always immediate — idempotent (NULL = none), never a wait. */
      _frame_delayed_post_reap(f);
      _frame_engine_turn(f);
      break;
    case FRM_MODEL_RESULT: {
      /* The model completion arrived RAW (the engine's sink posts it — the
         Task-4 http relay forwards into this same shape): the frame's OWN
         dispatch decodes it (loop.c) and processes the reply. Consumed on
         every path. */
      frm_model_payload_t* mp = (frm_model_payload_t*)msg->payload;
      msg->payload = NULL;
      _frame_engine_model_arrived(f, mp);
      break;
    }
    case FRM_CHILD_REPORT: {
      /* The child-engine terminal resume (Task 5 fills the behavior; Task 3
         declares the route here). Consumed on every path. */
      frm_child_report_payload_t* crp = (frm_child_report_payload_t*)msg->payload;
      msg->payload = NULL;
      _frame_engine_child_report(f, crp);
      break;
    }
    case FRM_STOP:
      /* Control, not interruption: a cell in flight runs to its boundary;
         the loop (which pumps this inbox) drains and then stops. */
      f->stop_requested = 1;
      log_info("frame: stop requested at '%s'", f->sid_path);
      break;
    case FRM_SPAWN: {
      /* The cell-side spawn verb, answered corr-matched: the admission
         composes (the parent's PRE-ALLOCATED seq) and posts at the store
         actor; the ROUTER answers this corr with the child's sid text when
         the store reply lands (waiter's bounded py-agent wait covers the
         extra hop). Task 5: the reply route STARTS a committed child (when
         an engine is live on the parent) instead of releasing it — the child
         is the admission's live product; its record joins the parent's
         teardown list. */
      frm_spawn_payload_t* sp = (frm_spawn_payload_t*)msg->payload;
      msg->payload = NULL;
      if (sp == NULL) {
        log_error("frame: FRM_SPAWN with no payload at '%s'", f->sid_path);
        break;
      }
      if (sp->corr == 0) {
        log_error("frame: FRM_SPAWN with corr 0 at '%s' — nothing to match",
                  f->sid_path);
      } else {
        uint64_t store_corr = ++f->store_corr_seq;
        frame_t* child =
            _frame_spawn_post(f, sp->goal, sp->context_json, sp->corr,
                              store_corr);
        if (child == NULL) {
          log_error("frame: spawn refused from a cell at '%s' (corr %llu) — "
                    "the child admission never happened", f->sid_path,
                    (unsigned long long)sp->corr);
          _frame_bridge_reply(sp->corr, 1, NULL);
        }
        /* else: the router answers the corr at the store reply. */
      }
      frm_spawn_payload_destroy(sp);
      break;
    }
    case FRM_REPORT: {
      /* The cell-side report verb: the REPORTING frame ends here. Children
         bind via FRM_REPORT_BIND (the parent composes the cross-subtree
         batch; the router answers this corr at the store reply); top frames
         post their own report + status in ONE store batch whose reply is
         answered through the bridge registry. Either way the reporting
         frame's status flips to done. */
      frm_report_payload_t* rp = (frm_report_payload_t*)msg->payload;
      msg->payload = NULL;
      if (rp == NULL) {
        log_error("frame: FRM_REPORT with no payload at '%s'", f->sid_path);
        break;
      }
      if (rp->corr == 0) {
        log_error("frame: FRM_REPORT with corr 0 at '%s' — nothing to match",
                  f->sid_path);
      } else if (f->parent != NULL) {
        if (_frame_report_bind_post(f, rp->corr, 0, 0, rp->text) != 0) {
          log_error("frame: report batch failed for '%s' (corr %llu) — "
                    "answered as a refusal before any post", f->sid_path,
                    (unsigned long long)rp->corr);
          _frame_bridge_reply(rp->corr, 1, NULL);
        }
        /* else: the router answers the corr at the store reply. */
      } else {
        _frame_report_top_post(f, rp->corr, rp->text);
        /* every refusal path answers the corr inside; the store reply
           routes the commit through the bridge registry */
      }
      frm_report_payload_destroy(rp);
      break;
    }
    case FRM_STORE_REPLY: {
      /* The store's corr-matched reply: route it (consumes the payload on
         every path). */
      frm_store_reply_payload_t* r = (frm_store_reply_payload_t*)msg->payload;
      msg->payload = NULL;
      _frame_store_reply_route(f, r);
      break;
    }
    case FRM_STORE_BATCH:
    case FRM_STORE_SCAN:
    case FRM_STORE_RECALL:
    case FRM_STORE_KEYS:
      /* Store OPERATIONS at a frame actor: a routing bug (they belong at the
         root's store actor). Loud drop; the payload retires here. */
      log_error("frame: a store operation arrived at the FRAME actor of '%s' "
                "— store ops belong at the root's store actor; dropping loud",
                f->sid_path);
      if (msg->payload != NULL) msg->payload_destroy(msg->payload);
      msg->payload = NULL;
      break;
    case FRM_REPORT_BIND: {
      /* child -> parent: compose the cross-subtree report batch HERE (the
         parent's own seq is pre-allocated in this dispatch — the child could
         never touch it) and post the ONE store batch whose reply routes back
         to the child. */
      frm_report_bind_payload_t* b = (frm_report_bind_payload_t*)msg->payload;
      msg->payload = NULL;
      _frame_report_bind_compose(f, b);
      break;
    }
    case FRM_INT:
      /* The interrupt entry (frame.h's frame_interrupt): the synthesis is
         ONE mechanism with the caller's wording (surface-completion spec
         §2). No payload. The DISARM FIRST (the PYRT_RESULT case's own
         discipline): an interrupt during a pooled cell ends the armed
         watchdog's reason to exist — the apply synthesizes the cell.close
         itself and needs no watcher; leaving it armed would let a false
         deadline fire long after the fact (a misleading already-complete-cell
         no-op plus the next arm's single-watch false alarm). Disarm is
         idempotent and NULL-safe. The case runs in python-less builds too —
         the apply guards its pyrt accesses internally; the FRM_INT case is
         unconditional. */
      _frame_cell_watchdog_disarm(f);
      _frame_interrupt_apply(f, 1, "aborted: interrupted at the frame's request");
      break;
    case FRM_CELL_WATCHDOG: {
      /* The deadline won (surface-completion spec §2): the payload OWNS the
         watchdog struct — it is no longer f->cell_watchdog (the
         disarm-or-handoff protocol settled it; the handoff's post ran
         UNDER the watchdog lock, and the lock/unlock handshake below
         guarantees the watcher has unlocked before the mutex here dies).
         Clean the pointer, free the struct, then run the synthesis under
         the watchdog wording. arm_cut = 0: a deadline never arms a cut
         against a FUTURE legitimate cell. If the cell completed between
         the deadline and this dispatch (the result won the race), the
         apply is a benign no-op — cell_pending is the frame's truth. */
      frame_cell_watchdog_t* w = (frame_cell_watchdog_t*)msg->payload;
      msg->payload = NULL;
      if (f->cell_watchdog == w) f->cell_watchdog = NULL;
      platform_mutex_lock(w->lock);
      platform_mutex_unlock(w->lock);
      _frame_cell_watchdog_payload_destroy(w);
      _frame_interrupt_apply(f, 0,
                             "aborted: cell exceeded the watchdog deadline");
      break;
    }
    default:
      break;
  }
}

/* The behavior's dispatch wrapper: tracks THIS frame's dispatch depth around
   the real switch — the sync store APIs consult it (see frame_t's
   dispatch_depth note: a nested caller must never pump this frame's mailbox). */
static void _frame_behavior(void* state, message_t* msg) {
  frame_t* f = (frame_t*)state;
  if (f != NULL) f->dispatch_depth++;
  _frame_behavior_impl(state, msg);
  if (f != NULL) f->dispatch_depth--;
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

/* --- loop plumbing (frame_internal.h contract) ---------------------------- */

int _frame_set_status_done(frame_t* f) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: status->done on a dead frame");
    return -1;
  }
  if (_frame_sync_store_refused(f, "status->done")) return -1;
  char* k_status = _frame_subkey(f->sid_path, "meta/status");
  if (k_status == NULL) return -1;
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
  bp->nops = 1;
  bp->ops[0].key = k_status;    /* OWNED by the store round trip */
  bp->ops[0].value = (uint8_t*)get_memory(strlen(SA_FRAME_STATUS_DONE) + 1);
  if (bp->ops[0].value == NULL) {
    frm_store_batch_payload_destroy(bp);
    return -1;
  }
  memcpy(bp->ops[0].value, SA_FRAME_STATUS_DONE, strlen(SA_FRAME_STATUS_DONE) + 1);
  bp->ops[0].value_len = strlen(SA_FRAME_STATUS_DONE);
  bp->op_name = "status->done"; /* BORROWED literal */
  _frame_sync_slot_reset(f);
  f->sync.in_use = 1;
  f->sync.corr = ++f->store_corr_seq;
  bp->reply_to = &f->actor;
  bp->corr = f->sync.corr;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "status batch");
  int wait_rc = _frame_slot_wait(f, &f->sync.done, SA_FRAME_STORE_WAIT_MS);
  f->sync.in_use = 0;
  if (wait_rc != 0) return -1;   /* deadline: the batch may still commit */
  int rc = f->sync.rc;
  if (rc != 0) {
    log_error("frame: status->done batch failed (%d) at '%s'", rc, f->sid_path);
    return rc;
  }
  return 0;
}

/* --- the refine slice's sync store helpers (frame_internal.h's contract) --- */

/* The ops array's teardown (the sync batch's ownership contract): every op's
   heap fields plus the array itself. */
static void _frame_ops_destroy(frm_store_op_t* ops, size_t nops) {
  if (ops == NULL) return;
  for (size_t i = 0; i < nops; i++) {
    free(ops[i].key);
    free(ops[i].value);
  }
  free(ops);
}

int _frame_sync_scan(frame_t* f, const char* start, const char* end,
                     size_t cap, char** text_out) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: sync scan on a dead frame");
    return -1;
  }
  if (start == NULL || end == NULL || text_out == NULL) {
    log_error("frame: sync scan at '%s' needs composed bounds and an output",
              f->sid_path != NULL ? f->sid_path : "?");
    return -1;
  }
  *text_out = NULL;
  if (_frame_sync_store_refused(f, "sync scan")) return -1;
  if (_frame_nested_sync(f)) {
    log_error("frame: nested sync scan at '%s' refuses loud — a read can "
              "never return from inside the frame's own dispatch",
              f->sid_path);
    return -1;
  }
  frm_store_scan_payload_t* sp =
      (frm_store_scan_payload_t*)get_clear_memory(sizeof(frm_store_scan_payload_t));
  sp->start = strdup(start);   /* OWNED; rides the payload */
  sp->end = strdup(end);
  if (sp->start == NULL || sp->end == NULL) {
    frm_store_scan_payload_destroy(sp);
    return -1;
  }
  sp->limit = cap;
  _frame_sync_slot_reset(f);
  f->sync.in_use = 1;
  f->sync.corr = ++f->store_corr_seq;
  sp->reply_to = &f->actor;
  sp->corr = f->sync.corr;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_SCAN, sp,
              frm_store_scan_payload_destroy, "sync scan");
  int wait_rc = _frame_slot_wait(f, &f->sync.done, SA_FRAME_STORE_WAIT_MS);
  f->sync.in_use = 0;
  if (wait_rc != 0) return -1;   /* deadline: the scan reply routes late */
  int rc = f->sync.rc;
  if (rc != 0) return rc;        /* the store logged its scan refusal loud */
  /* Join the reply's materialized raw record texts (ascending) as ONE
     JSON-ARRAY text — each record a quoted string element; "[]" when the
     range is empty. The caller parses the array whole. */
  json_value_t* arr = json_new_array();
  for (size_t i = 0; i < f->sync.n; i++) {
    if (f->sync.records[i] == NULL) continue;   /* never happens today */
    json_array_append(arr, json_new_string(f->sync.records[i]));
  }
  char* text = json_serialize(arr);
  json_value_destroy(arr);
  _frame_sync_slot_reset(f);   /* the join copied the texts; the ride dies */
  *text_out = text;
  return 0;
}

int _frame_sync_batch(frame_t* f, frm_store_op_t* ops, size_t nops,
                      const char* op_name) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: sync batch '%s' on a dead frame",
              op_name != NULL ? op_name : "?");
    _frame_ops_destroy(ops, nops);
    return -1;
  }
  if (ops == NULL || nops == 0) {
    log_error("frame: sync batch '%s' at '%s' carries no ops — refused loud",
              op_name != NULL ? op_name : "?", f->sid_path);
    _frame_ops_destroy(ops, nops);
    return -1;
  }
  if (_frame_sync_store_refused(f, "sync batch")) {
    _frame_ops_destroy(ops, nops);
    return -1;
  }
  if (_frame_nested_sync(f)) {
    log_error("frame: nested sync batch '%s' at '%s' refuses loud — a batch "
              "whose committed/failed answer is required never posts "
              "unawaited", op_name != NULL ? op_name : "?", f->sid_path);
    _frame_ops_destroy(ops, nops);
    return -1;
  }
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(frm_store_batch_payload_t));
  /* The ops ownership TRANSFERS into the payload: its destroyer frees the
     array and every op's heap fields on every round-trip path (success,
     store refusal, deadline, late routing) — this helper never frees an op
     once composed. */
  bp->ops = ops;
  bp->nops = nops;
  bp->op_name = op_name;        /* BORROWED */
  _frame_sync_slot_reset(f);
  f->sync.in_use = 1;
  f->sync.corr = ++f->store_corr_seq;
  bp->reply_to = &f->actor;
  bp->corr = f->sync.corr;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "sync batch");
  int wait_rc = _frame_slot_wait(f, &f->sync.done, SA_FRAME_STORE_WAIT_MS);
  f->sync.in_use = 0;
  if (wait_rc != 0) return -1;   /* deadline: the batch may still commit */
  int rc = f->sync.rc;
  if (rc != 0) {
    log_error("frame: sync batch '%s' failed (%d) at '%s' — nothing committed",
              op_name != NULL ? op_name : "?", rc, f->sid_path);
  }
  return rc;
}

uint8_t _frame_is_child(const frame_t* f) {
  return (f != NULL && f->parent != NULL) ? 1 : 0;
}

const char* _frame_goal(const frame_t* f) {
  return (f != NULL) ? f->goal : NULL;
}

uint8_t _frame_is_live(const frame_t* f) {
  return (f != NULL && f->st != NULL) ? 1 : 0;
}

uint8_t _frame_stop_requested(const frame_t* f) {
  return (f != NULL) ? f->stop_requested : 1;
}

unsigned _frame_loop_turn_cap(const frame_t* f) {
  return (f != NULL && f->loop_turn_cap > 0) ? f->loop_turn_cap
                                             : SA_LOOP_MAX_TURNS;
}

/* Backend resolution: the injected override wins; the DEFAULT is built
   EXACTLY ONCE at first use from the frame's own config and becomes
   frame-owned (freed in frame_destroy). A set override short-circuits this
   before any default construction fires — that is the documented discipline
   (a scripted test backend must never trigger an http-backend build). */
model_backend_t* _frame_backend_get(frame_t* f) {
  if (f == NULL) return NULL;
  if (f->backend != NULL) return f->backend;
  if (f->owned_backend != NULL) return f->owned_backend;

#if defined(SA_HAS_STREAMS)
  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.model_base_url = f->model_base_url;
  cfg.model_api_key = f->model_api_key;
  cfg.model_name = f->model_name;
  cfg.max_depth = f->max_depth;
  cfg.model_timeout_ms = f->model_timeout_ms;
  model_backend_t* mb = model_http_backend_create(&cfg);
  if (mb == NULL) {
    log_error("frame: no backend set and the default http backend cannot be "
              "built from the config of '%s' (base_url/model missing)", f->sid_path);
    return NULL;
  }
  f->owned_backend = mb;
  return mb;
#else
  /* The default backend rides the streams transport in model.c; a no-streams
     build has NO creatable default, so this is a loud build-shape refusal —
     the message must not claim a config problem that does not exist. Scripted
     backends via frame_set_model_backend remain the no-streams path. */
  log_error("frame: no backend set and no streams transport in this build "
            "(SA_ENABLE_STREAMS=OFF) — inject a backend on '%s' via "
            "frame_set_model_backend", f->sid_path);
  return NULL;
#endif
}

uint8_t _frame_cell_pending(const frame_t* f) {
  return (f != NULL) ? f->cell_pending : 0;
}

/* The synchronous refusal's paired cell.result text (frame_internal.h's
   contract): the poison refusal is CORR-MATCHED failure data the model
   reads — the paired composer names the wedge, the generic refusals keep
   their standing wording (NULL). */
const char* _frame_cell_refusal_text(const frame_t* f) {
  if (f != NULL && f->pyrt_poisoned != 0) {
    return "pyrt: runtime poisoned by an interrupted cell";
  }
  return NULL;
}

int _frame_cell_wait(frame_t* f, unsigned timeout_ms, uint8_t* status_out) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: cell wait on a dead frame");
    return -1;
  }
  if (status_out == NULL) {
    log_error("frame: cell wait needs a status out-param");
    return -1;
  }
  /* Synchronous completion path: the FRM_CELL_EXECUTE behavior refuses
     (pending stayed 0, status filled) — nothing to pump. */
  if (f->cell_pending == 0) {
    *status_out = f->cell_status;
    return 0;
  }

  uint64_t deadline = platform_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
  for (;;) {
    /* The pump runs EVERYTHING the inbound inbox holds (frame_internal.h's
       ONE pump order): the running cell's bridge verbs (each composes + posts
       a store message), the live ancestors' mailboxes (a bind must reach the
       parent's composition), the inline store actor (whose execution of the
       batch routes the corr answers back — waking the pyrt thread), then the
       PYRT_RESULT completing the slot. A round trip completes within
       consecutive pump cycles; the deadline is the only bound that breaks. */
    _frame_pump(f);
    if (f->cell_pending == 0) {
      *status_out = f->cell_status;
      return 0;
    }
    if (platform_monotonic_ns() >= deadline) {
      log_error("frame: cell corr %llu still in flight after %u ms at '%s' — "
                "the wait gives up (the cell itself is NOT abandoned; its own "
                "result completes the slot whenever it lands)",
                (unsigned long long)f->cell_corr, timeout_ms, f->sid_path);
      return -1;
    }
    platform_sleep_ms(1);
  }
}

/* --- lifecycle ----------------------------------------------------------- */

wave_database_root_t* wave_db_open_config(const wave_database_config_t* cfg) {
  const char* location = (cfg != NULL) ? cfg->location : NULL;
  scheduler_pool_t* store_pool = (cfg != NULL) ? cfg->store_pool : NULL;

  database_config_t* wcfg = database_config_default();
  if (wcfg == NULL) {
    log_error("wave_db_open_config: no config memory");
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
    log_error("wave_db_open_config: database_create_with_config failed (%d)",
              err);
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
  root->store_pool = store_pool;   /* BORROWED; NULL = the inline shape */

  /* The store actor owns the single-serializer role (§5): every frame-layer
     store op — batches, scans, the recall walk — runs inside its behavior,
     one message at a time, so the frame layer needs no lock at all. A NULL
     pool = the inline shape: tests/demos pace it via wave_db_pump. */
  actor_init(&root->store_actor, root, _store_behavior, store_pool);

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
      log_error("wave_db_open_config: lineage graph layer create failed (%d) "
                "— spawns will be refused", gerr);
      database_subtree_close(root->lineage_st);
      root->lineage_st = NULL;
    }
  }

  return root;
}

/* The inline-store wrapper (wave_database_config_t{location, NULL}). */
wave_database_root_t* wave_db_open(const char* location) {
  wave_database_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.location = location;
  return wave_db_open_config(&cfg);
}

actor_t* wave_db_store_actor(wave_database_root_t* db) {
  return (db != NULL) ? &db->store_actor : NULL;
}

int wave_db_pump(wave_database_root_t* db) {
  if (db == NULL) {
    log_error("wave_db_pump: NULL root");
    return -1;
  }
  if (db->store_pool != NULL) {
    /* The dual-driver rule's other half: a POOLED store's pacing belongs to
       its scheduler workers; pumping it by hand would steal a mailbox run
       out from under their dispatches. Refuse loud, never steal. */
    log_error("wave_db_pump: the store actor is POOLED — its workers own the "
              "pacing; the manual pump refuses loud");
    return -2;
  }
  actor_run(&db->store_actor, ACTOR_BATCH_SIZE);
  return 0;
}

void wave_db_close(wave_database_root_t* root) {
  if (root == NULL) return;
  /* The store actor's teardown IS the front of the db lifecycle: its queue
     drains (retiring any pending round-trip payloads), the RUNNING/queue
     waits break out once a stop has put the pool into `stopped` (documented
     caller order: stop the pool, then close the db, then destroy the pool).
     Inline: no-op waits, then the drain. */
  actor_destroy(&root->store_actor);
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
    f->model_timeout_ms = cfg->model_timeout_ms;
    f->cell_watchdog_ms = cfg->cell_watchdog_ms;
    f->pool = cfg->pool;        /* BORROWED, exactly like `backend` */
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
    /* Spawned children inherit the parent's depth budget, model config, and
       pool (a tree always sits on ONE pool). */
    f->max_depth = parent->max_depth;
    f->model_timeout_ms = parent->model_timeout_ms;
    f->cell_watchdog_ms = parent->cell_watchdog_ms;   /* a pooled tree's
        children run their cells under the same one-cell bound */
    f->pool = parent->pool;
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
    /* The cfg-less, parent-less create (frame_spawn's inherit path passes
       cfg NULL AND a parent; this branch is the plain cfg-less top frame):
       the budget table's default governs the pooled cell bound. */
    f->max_depth = 4;
    f->cell_watchdog_ms = SA_FRAME_CELL_WATCHDOG_MS;
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

  /* The pool guard (§5): a POOLED frame REQUIRES a POOLED store — a pooled
     frame posting into an un-pumped inline store mailbox is a hang posing as
     an API call, and hangs lie. Loud, before anything else happens. */
  if (f->pool != NULL && root->store_pool == NULL) {
    log_error("frame: a POOLED frame on '%s' requires a POOLED store — "
              "open the root with wave_db_open_config and a store pool "
              "(the inline store would never be pumped); refusing loud",
              f->sid_path);
    goto fail;
  }

  /* Boot: continue the seq counter past any persisted events (restart-safe).
     (No events in this task — Task 10's restart/replay test depends on this.) */
  f->seq = _frame_restore_seq(f);

  /* Inline actor (pool NULL): tests/loop pump the mailbox by hand. A pool
     from the config attaches the actor to that pool — the engine
     (frame_start) schedules onto it instead. */
  actor_init(&f->actor, f, _frame_behavior, f->pool);
  /* The engine-state atomics (frame_internal.h's lifetime handoff) are
     stored to explicitly — the record's zeroing is not a defined-value
     atomic initialization. */
  ATOMIC_STORE(&f->engine.pending_submits, 0);
  ATOMIC_STORE(&f->engine.die_requested, 0);
  ATOMIC_STORE(&f->engine.submit_inflight, 0);
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
     frames start "running"; they end via the loop (Task 10). The direct
     write (not a store post) is the birth-batch carve-out — pre-announcement:
     fresh subtree, single writer, direct write. */
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

/* --- the resume repair (spec §4; frame_internal.h's contract) --------------
   The crash's open tail is repaired ON THE CALLER'S THREAD before any engine
   can start: one tail scan (the sync family) -> the cursor fold -> the
   closer compose (pure) -> ONE awaited atomic batch committing the closer
   records as event puts on the frame's zero-padded events keys. The balance
   rule is the whole crash check: a repaired tail — or a never-truncated
   one — folds nothing, and a second resume composes nothing. */

/* The closer batch's compose + the ONE awaited post (the only commit path;
   `_frame_resume_repair` calls it when the compose returned closers). The
   records compose at the frame's OWN pre-allocated seqs — the seq counter
   continues the events range's KEYS (the store's key truth), so a tail whose
   newest record's payload skipped can never collide a stored record's key,
   which a fold-seq-based key could. The ops transfer into the sync batch
   (the composer-never-frees rule); the record compose consumed the closer
   payloads, so the closers' destroyer only releases what the compose left. */
static int _frame_resume_repair_commit(frame_t* f,
                                       lifecycle_closers_t* closers) {
  size_t n = closers->n;
  if (n == 0) return 0;   /* balanced / empty tail — NOTHING to commit */

  /* The seqs pre-allocate CONTIGUOUSLY in one stretch (single-flight — this
     is the resume's caller thread and nothing else allocates between them);
     the range rolls back in reverse on every pre-post refusal (the
     `_frame_seq_rollback` single-flight discipline). */
  uint64_t first = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t s = _frame_seq_alloc(f);
    if (i == 0) first = s;
  }

  /* The compose mirrors the event batch's stages: full event records (the
     frozen shape — each record's seq matches its key), per-record and
     total WAL cap checks loud, keys on the zero-padded events key shape. */
  char** texts = (char**)get_clear_memory(n * sizeof(char*));
  char** keys = (char**)get_clear_memory(n * sizeof(char*));
  size_t total = 0;
  int rc = 0;
  for (size_t i = 0; i < n; i++) {
    /* _frame_event_json CONSUMES the payload (on failure too). */
    texts[i] = _frame_event_json(f, first + i, closers->items[i].type,
                                 closers->items[i].payload);
    closers->items[i].payload = NULL;
    if (texts[i] == NULL) {
      rc = -1;
      break;
    }
  }
  if (rc == 0) {
    for (size_t j = 0; j < n; j++) {
      if (strlen(texts[j]) > SA_FRAME_MAX_BATCH_BYTES) {
        log_error("frame: the resume repair's closer record %zu is %zu "
                  "bytes, exceeding the %d-byte WAL batch cap — refusing, "
                  "never truncating", j, strlen(texts[j]),
                  (int)SA_FRAME_MAX_BATCH_BYTES);
        rc = -3;
        break;
      }
      total += strlen(texts[j]);
      keys[j] = _frame_event_key(f->sid_path, first + j);
      if (keys[j] == NULL) {
        rc = -1;
        break;
      }
      total += strlen(keys[j]);
    }
    if (rc == 0 && total > SA_FRAME_MAX_BATCH_BYTES) {
      log_error("frame: the resume repair's closer batch at '%s' is %zu "
                "bytes, exceeding the %d-byte WAL batch cap — refusing, "
                "never truncating", f->sid_path, total,
                (int)SA_FRAME_MAX_BATCH_BYTES);
      rc = -3;
    }
  }
  if (rc != 0) {
    for (size_t j = 0; j < n; j++) {
      free(texts[j]);
      free(keys[j]);
    }
    free(texts);
    free(keys);
    for (size_t i = n; i > 0; i--) _frame_seq_rollback(f, first + i - 1);
    lifecycle_closers_destroy(closers);   /* the compose's leftovers */
    return rc;
  }

  frm_store_op_t* ops =
      (frm_store_op_t*)get_clear_memory(n * sizeof(frm_store_op_t));
  for (size_t j = 0; j < n; j++) {
    ops[j].key = keys[j];              /* OWNED: the store round trip frees */
    ops[j].value = (uint8_t*)texts[j]; /* OWNED */
    ops[j].value_len = strlen(texts[j]);
  }
  free(texts);   /* the arrays only — the strings moved into the ops */
  free(keys);
  lifecycle_closers_destroy(closers);   /* the payloads were consumed
                                           (NULLed); the items array dies */
  int rc2 = _frame_sync_batch(f, ops, n, "resume repair");
  if (rc2 != 0) {
    /* The store's refusal or the deadline — nothing composed here frees
       (the ops transferred at the post; the sync family owns their teardown
       on every path). A deadline's late commit still leaves a BALANCED
       tail: the NEXT resume folds nothing — the balance rule is the whole
       guarantee. Resume refuses rather than half-repairs. */
    log_error("frame: the resume repair's closer batch at '%s' did not "
              "commit (rc %d) — resume refuses loud", f->sid_path, rc2);
    return -1;
  }
  return 0;
}

int _frame_resume_repair(frame_t* f) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: resume repair on a dead frame");
    return -1;
  }
  /* The sync family's inline-only pre-post refusals, checked BEFORE any seq
     is pre-allocated (resume refuses rather than half-repairs). */
  if (_frame_sync_store_refused(f, "resume repair")) return -1;
  if (_frame_nested_sync(f)) {
    log_error("frame: the resume repair at '%s' refuses loud — a caller "
              "inside the frame's own dispatch cannot await the closer "
              "batch's commit", f->sid_path);
    return -1;
  }

  /* 1. THE TAIL SCAN (spec §4.1): ONE sync scan over the frame's events
        range (the ABSOLUTE root-level composed bounds — the events-range
        scan discipline), the newest SA_LIFECYCLE_TAIL_EVENTS window. This
        is the closer path's ONLY scan — the reply rides back as one joint
        JSON-ARRAY text, exactly what the fold consumes. */
  char* lo = _frame_subkey(f->sid_path, "events");
  char* hi = _frame_subkey(f->sid_path, "events0");
  if (lo == NULL || hi == NULL) {
    free(lo);
    free(hi);
    return -1;
  }
  char* tail = NULL;
  int rc = _frame_sync_scan(f, lo, hi, SA_LIFECYCLE_TAIL_EVENTS, &tail);
  free(lo);
  free(hi);
  if (rc != 0) {
    log_error("frame: the resume repair's tail scan at '%s' was refused "
              "(%d) — resume refuses rather than half-repairs", f->sid_path,
              rc);
    return -1;
  }

  /* 2. THE CURSOR FOLD (pure): the tail's state-of-the-world — an EMPTY, a
        pre-lifecycle, or a balanced tail folds turn_open == 0. */
  lifecycle_cursor_t cursor;
  rc = lifecycle_cursor_fold(tail, &cursor);
  free(tail);
  if (rc != 0) {
    log_error("frame: the resume repair's fold at '%s' refused loud — the "
              "tail is not a parseable event-record array", f->sid_path);
    return -1;
  }
  /* 3. THE CLOSERS (pure): a balanced tail composes nothing — the resume
        proceeds exactly as it did before the slice existed. */
  lifecycle_closers_t closers;
  memset(&closers, 0, sizeof(closers));
  rc = lifecycle_closers_compose(&cursor, &closers);
  lifecycle_cursor_destroy(&cursor);
  if (rc != 0) {
    log_error("frame: the resume repair's closer compose at '%s' refused "
              "loud", f->sid_path);
    return -1;
  }
  return _frame_resume_repair_commit(f, &closers);
}

/* Boot-time restore (frame.h contract): open the existing subtree, refuse
   without ANY write when the birth record is missing, restore seq via the
   same reverse scan frame_create boots with, read depth + parent path back.
   No meta writes, no events, and the loop does NOT restart here. */
frame_t* frame_resume(wave_database_root_t* db, const char* sid,
                      const frame_config_t* cfg) {
  if (db == NULL) {
    log_error("frame_resume: NULL root");
    return NULL;
  }
  if (sid == NULL || sid[0] == '\0') {
    log_error("frame_resume: sid required");
    return NULL;
  }

  frame_t* f = get_clear_memory(sizeof(frame_t));
  if (f == NULL) return NULL;
  f->root = db;
  f->sid_path = strdup(sid);

  if (f->sid_path == NULL) {
    log_error("frame_resume: out of memory copying sid");
    free(f);
    return NULL;
  }

  f->st = database_subtree_open(db->db, f->sid_path, '/');
  if (f->st == NULL) {
    log_error("frame_resume: no subtree at '%s' — not a resumable frame",
              f->sid_path);
    goto fail;
  }
  /* The birth record is the resume gate: a path without meta/created is not
     one of this process's frames (typo, foreign prefix, missing db). */
  char* created = _frame_subtree_text(f->st, "meta/created");
  if (created == NULL) {
    log_error("frame_resume: '%s' has no birth record (meta/created) — "
              "refusing", f->sid_path);
    goto fail;
  }
  free(created);

  f->seq = _frame_restore_seq(f);

  char* depth = _frame_subtree_text(f->st, "meta/depth");
  if (depth != NULL) {
    f->depth = (uint32_t)strtoul(depth, NULL, 10);
    free(depth);
  }
  /* Live parent linkage is NOT restored (the parent frame_t is a separate
     process object); the PATH is — the tree slice re-links a resumed child. */
  f->parent_path = _frame_subtree_text(f->st, "meta/parent");
  /* Goal is not separately persisted; the post-restart config is copied in
     (NULL cfg carries none). */
  if (cfg != NULL) {
    f->max_depth = (cfg->max_depth > 0) ? cfg->max_depth : 4;
    f->model_timeout_ms = cfg->model_timeout_ms;
    f->cell_watchdog_ms = cfg->cell_watchdog_ms;
    f->pool = cfg->pool;        /* BORROWED, exactly like the model strings */
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
  } else {
    f->max_depth = 4;
  }

  /* The pool guard of frame_create, at RESOLVED time: the cfg above is
     copied already, so f->pool is what this frame will run with. A frame
     pool and a store pool must agree — a POOLED frame requires a POOLED
     store. An INLINE frame on a POOLED store is the nobody-pumps-one-side
     hang — EXCEPT the orchestration slice's documented READ-ONLY HANDLE
     shape: a DONE subtree resumed pool-less keeps its reads on the DIRECT
     debug scan (frame_debug_events/frame_is_done, never store messages),
     its sync writes refuse loud (spec §5's inline-only rule), and frame_start
     refuses done frames so no engine can ever post at the pooled store's
     mailbox. A NOT-done inline resume on a pooled store still refuses loud. */
  if (f->pool != NULL && db->store_pool == NULL) {
    log_error("frame_resume: a POOLED frame requires a POOLED store at '%s' "
              "(the resumed cfg must carry the pool) — refusing loud",
              f->sid_path);
    goto fail;
  }
  if (f->pool == NULL && db->store_pool != NULL) {
    char* status = _frame_subtree_text(f->st, "meta/status");
    uint8_t done = (status != NULL &&
                    strcmp(status, SA_FRAME_STATUS_DONE) == 0);
    free(status);
    if (!done) {
      log_error("frame_resume: the frame pool and the store pool must match "
                "at '%s' — an inline resume of a NOT-done frame on a POOLED "
                "store is a mailbox nobody pumps on one side (only a DONE "
                "subtree's read-only handle shape is legal); refusing loud",
                f->sid_path);
      goto fail;
    }
  }

  actor_init(&f->actor, f, _frame_behavior, f->pool);
  /* Explicit atomic init, as in frame_create (frame_internal.h's lifetime
     handoff starts clear on the resume path too). */
  ATOMIC_STORE(&f->engine.pending_submits, 0);
  ATOMIC_STORE(&f->engine.die_requested, 0);
  ATOMIC_STORE(&f->engine.submit_inflight, 0);

  /* The crash-repair pass (spec §4): a NOT-DONE resumed frame's tail may be
     truncated by the crash that killed the previous process — the repair
     folds the tail and, when it is UNBALANCED, commits ONE atomic closer
     batch on this caller thread (the SYNC family awaits the store's batch
     answer), so frame_resume returns only past a closed tail and no engine
     can ever start on an open one. A DONE subtree skips the pass entirely:
     every status=done write rode its terminal turn's close batch (the
     balance rule already holds — the log's newest lifecycle record is a
     turn.end), and the done handle's documented read-only shape keeps its
     sync round trips off (frame_start refuses done frames — no engine ever
     runs on this handle). A refusal makes the whole resume fail loud
     (frame_destroy; the caller retries when the store can answer) — resume
     refuses rather than half-repairs. */
  char* resume_status = _frame_subtree_text(f->st, "meta/status");
  uint8_t done = (resume_status != NULL &&
                  strcmp(resume_status, SA_FRAME_STATUS_DONE) == 0);
  free(resume_status);
  if (!done) {
    if (_frame_resume_repair(f) != 0) {
      log_error("frame_resume: the crash-repair pass at '%s' was refused — "
                "the frame is not resumable in this state (refusing loud "
                "rather than half-repairing)", f->sid_path);
      frame_destroy(f);
      return NULL;
    }
  }
  return f;

fail:
  if (f->st != NULL) database_subtree_close(f->st);
  free(f->sid_path);
  free(f->model_base_url);
  free(f->model_api_key);
  free(f->model_name);
  free(f);
  return NULL;
}

void frame_set_model_backend(frame_t* f, model_backend_t* backend) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: set_model_backend on a dead frame");
    return;
  }
  /* BORROWED (see frame.h): the frame stores the pointer without owning it;
     a NULL resets to default construction at first use. */
  f->backend = backend;
}

void frame_set_loop_turn_cap(frame_t* f, unsigned cap) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: set_loop_turn_cap on a dead frame");
    return;
  }
  f->loop_turn_cap = cap;   /* 0 = the SA_LOOP_MAX_TURNS default */
}

int frame_start(frame_t* f) {
  /* The engine's start contract lives with the turn engine (loop.c): the
     dead-frame / already-live refuses, the per-run knob reset, and the ONE
     FRM_TURN continuation it queues. */
  return _frame_engine_start(f);
}

/* The interrupt entry (frame.h's contract): ONE FRM_INT posted into the
   frame's OWN mailbox — ordinary mailbox injection, zero new locks; the
   frame's dispatch (the scheduler worker's in production, the driver's pump
   in tests) runs the synthesis. A payload-less post is the FRM_TURN
   continuation's legal shape (nothing to destroy, nothing to consume). */
void frame_interrupt(frame_t* f) {
  if (f == NULL || f->st == NULL) {
    log_error("frame_interrupt: dead frame");
    return;
  }
  _frame_post(&f->actor, (uint32_t)FRM_INT, NULL, NULL, "interrupt");
}

scheduler_pool_t* frame_pool(const frame_t* f) {
  return (f != NULL) ? f->pool : NULL;
}

actor_t* _frame_actor(frame_t* f) {
  return (f != NULL) ? &f->actor : NULL;
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

/* The teardown body — runs EXACTLY ONCE per frame, from the frame's first
   thread able to: frame_destroy directly when no async submit slot is held,
   or the model sink's LAST release (the engine's lifetime handoff,
   _frame_engine_submit_release) when a destroy marked die_requested and
   deferred. Everything below frees; the record dies at the tail. */
static void _frame_destroy_run(frame_t* f) {
#ifdef SA_HAS_PYTHON
  /* The runtime must stop BEFORE the mailbox it posts into is drained (its
     worker thread and TLS point at the frame actor; the join is bounded). A
     cell blocked on the bridge registry unblocks within its bounded wait. */
  if (f->pyrt != NULL) pyrt_destroy(f->pyrt);
  f->pyrt = NULL;
#endif
  /* Inline teardown: no pool owns this actor (a NULL-pool frame), so the
     enclosing struct's lifetime is ours to end here. POOLED frames route
     through actor_destroy instead: its RUNNING/queue-state waits break out
     once scheduler_pool_stop set `stopped` — the caller's documented order
     is stop the pool FIRST, then frame_destroy, then pool destroy. */
  if (f->pool != NULL) {
    actor_destroy(&f->actor);
  } else {
    atomic_fetch_or(&f->actor.flags, ACTOR_FLAG_DESTROY);
    actor_detach_pool(&f->actor);
    message_queue_destroy(&f->actor.queue);   /* drains; the queue retires payloads */
  }
  /* The store round trip's leftovers (a wait that never saw its reply —
     e.g. the store died first): registry nodes + a transferred text die
     here; an uncommitted admission's child dies loud with the frame. */
  {
    frm_bridge_pending_t* p = f->bridge_pending;
    while (p != NULL) {
      frm_bridge_pending_t* next = p->next;
      log_error("frame: a registered store reply for corr %llu never landed "
                "at '%s' (corrs are retired at teardown)",
                (unsigned long long)p->corr, f->sid_path);
      free(p);
      p = next;
    }
    f->bridge_pending = NULL;
  }
  _frame_sync_slot_reset(f);
  free(f->engine.finish_text);
  f->engine.finish_text = NULL;
  if (f->engine.turn_reply != NULL) {
    /* A live engine's in-flight turn reply dies here (a destroy mid-turn —
       the engine state is not the queue's business). */
    if (f->engine.engine_live != 0) {
      log_error("frame: the engine of '%s' was destroyed mid-turn (its "
                "in-flight model reply and awaited store trip die with it)",
                f->sid_path);
    }
    model_reply_destroy(f->engine.turn_reply);
    f->engine.turn_reply = NULL;
  }
  /* The spawn admissions whose store replies never routed (a teardown
     racing an in-flight admission, or a dead store): each entry's still-
     owned child is abandoned loud. */
  {
    frm_spawn_pending_t* pe = f->spawn_pending;
    f->spawn_pending = NULL;
    while (pe != NULL) {
      frm_spawn_pending_t* next = pe->next;
      if (pe->child != NULL) {
        log_error("frame: a spawn admission for '%s' at '%s' never saw its "
                  "store reply — the child is abandoned loud at teardown",
                  frame_sid(pe->child), f->sid_path);
        frame_destroy(pe->child);
        pe->child = NULL;
      }
      free(pe);
      pe = next;
    }
  }
  /* The START branch's unadopted (cell-verb) children: their records are
     this parent's — freed here, after this frame's own mailbox is drained
     (a child's dispatch still in flight completes inside its own
     actor_destroy's waits). A child with grandchildren tears down its own
     tree the same way. */
  {
    frame_t* c = f->owned_children;
    f->owned_children = NULL;
    while (c != NULL) {
      frame_t* next = c->owned_next;
      c->owned_next = NULL;
      frame_destroy(c);
      c = next;
    }
  }
  if (f->st != NULL) database_subtree_close(f->st);
  free(f->sid_path);
  free(f->parent_path);
  free(f->goal);
  free(f->model_base_url);
  free(f->model_api_key);
  free(f->model_name);
  if (f->owned_backend != NULL) {
    model_backend_destroy(f->owned_backend);
    f->owned_backend = NULL;
  }
  free(f);
}

/* The engine's lifetime handoff (frame_internal.h's contract): the ASYNC
   submit path holds the bare frame across the backend's completion — a
   frame_destroy mid-turn used to free the record under a pending sink and
   the sink then dereferenced the freed memory before its own death check
   could ever see it. The fix is lock-free (the plan's shape (b) — no locks,
   atomics are house-legal): the submit step holds one slot for the
   duration; destroy marks die_requested and, on a held slot, DEFERS
   EVERYTHING — the record and all its owned memory stay alive, and the
   LAST release (the sink itself) runs _frame_destroy_run. The decision is
   a CAS-CLAIM on pending_submits so the destroy thread and the sink's
   final release can never both read "now free" and tear down twice.

   Ownership order at destroy time (die FIRST, everything after watches it):
     1. die_requested := 1 — the sink's death gate stops touching the frame
        (beyond the still-alive record's own atomics) as soon as any
        destroy is in play;
     2. CAS(pending_submits 0 -> CLAIM): winning it means NO slot is held —
        run the teardown HERE. Failing means slots are held — mark ACTOR_FLAG
        DESTROY so every late post refuses loud through _frame_post's own
        gate, log, and let the last release claim + run the teardown. */
void frame_destroy(frame_t* f) {
  if (f == NULL) return;
  if (atomic_exchange(&f->engine.die_requested, 1) != 0) {
    /* Already dying: a deferred destroy (or its last release) owns the
       teardown — this call must not touch the record (the first dyer's
       claim already decided how it ends; a teardown may even be running
       right now, so nothing on `f` is read past this point). Loud, bare. */
    log_error("frame: frame_destroy re-entered on a die-requested frame — "
              "ignored (the deferred teardown's owner runs it)");
    return;
  }
  /* The retry backoff's timer JOINS FIRST (guards spec §3): die is now 1,
     so a joined thread's expiry lands on its die-check's loud drop — and
     every later teardown step runs against a frame whose only cross-thread
     timer thread is already gone. The join's ordering against the teardown
     is the timer's memory-safety contract: the thread's die-check and post
     touch borrowed frame memory, so it must never outlive record teardown
     — here it is joined BEFORE the watchdog disarm, the pyrt destroy's own
     join, and the mailbox teardown. */
  _frame_delayed_post_reap(f);
  /* The POOLED cell watchdog disarms/joins FIRST (spec §2): before the
     pyrt destroy's own join and before the mailbox teardown, so a not-yet-
     handed-off watcher provably exits (joined, struct freed) and an
     already-handed-off one has ALREADY delivered its message (the handoff
     posts under the watchdog lock before any handed_off read) — the
     delivering message rides the drain below, its destroyer the struct's
     single cleaner. */
  _frame_cell_watchdog_disarm(f);
  uint32_t expect = 0;
  if (atomic_compare_exchange_strong(&f->engine.pending_submits, &expect,
                                     SA_ENGINE_SUBMIT_CLAIM)) {
    if (atomic_load(&f->engine.submit_inflight) != 0) {
      /* The CLAIM wins while the engine thread is INSIDE its submit call
         (a synchronous sink fire released already): that engine is still
         running its post-submit flow on this record, so give the CLAIM
         back — the engine settles the deferred teardown itself. */
      expect = SA_ENGINE_SUBMIT_CLAIM;
      (void)atomic_compare_exchange_strong(&f->engine.pending_submits, &expect, 0);
      atomic_fetch_or(&f->actor.flags, ACTOR_FLAG_DESTROY);
      log_error("frame: '%s' was destroyed mid-turn inside its own model "
                "submit — the engine settles the deferred teardown (no lock)",
                f->sid_path);
      return;
    }
    /* No slot held, no submit in flight: the destroy IS the last releaser —
       tear down now. */
    _frame_destroy_run(f);
    return;
  }
  if (expect == SA_ENGINE_SUBMIT_CLAIM) {
    /* Another agent already claimed the record's end — never double-run. */
    return;
  }
  /* Slots held: the record stays alive; the last release runs the teardown.
     Refuse everything further through the actor's own destroy flag, so even
     a sink mid-flight inside its die check (die was still 0 then) finds its
     post dropped loud by _frame_post — never a post into a dying queue. */
  atomic_fetch_or(&f->actor.flags, ACTOR_FLAG_DESTROY);
  log_error("frame: '%s' was destroyed mid-turn with a pending async model "
            "submit — the teardown defers to the sink's last release "
            "(die-requested; no lock, the record stays alive until then)",
            f->sid_path);
}

/* The die-requested flag's atomic read (frame_internal.h's contract: the
   loop-side's die gate — the model sink and the completion handler — cannot
   reach the engine state's atomics through the incomplete frame_t). */
uint8_t _frame_engine_die_requested(const frame_t* f) {
  return (f != NULL) ? atomic_load(&f->engine.die_requested) : 0;
}

/* The engine's submit step: ONE slot held from BEFORE the submit until the
   sink's release; the sink's completion (or the engine's own submit-reject
   path — that sink will never fire) gives it back. The LAST release — or
   the engine's post-submit settle — on a die-requested frame claims the
   record (CAS against destroy's own claim) and runs the deferred teardown,
   exactly once per frame, on whichever thread got there last. */
void _frame_engine_submit_begin(frame_t* f) {
  if (f == NULL) return;
  atomic_fetch_add(&f->engine.pending_submits, 1);
  atomic_store(&f->engine.submit_inflight, 1);
}

uint8_t _frame_engine_submit_release(frame_t* f) {
  if (f == NULL) return 0;
  uint32_t before = atomic_fetch_sub(&f->engine.pending_submits, 1);
  /* The die read comes after the sub (the record is alive either way — both
     this release's slot and destroy's deferral pin it). */
  if (before != 1 || atomic_load(&f->engine.die_requested) == 0) return 0;
  if (atomic_load(&f->engine.submit_inflight) != 0) return 0;
  /* Last slot off a dying frame — but NEVER from inside the engine's own
     submit call (a synchronous sink fire): the engine thread still runs its
     post-submit flow on this record and settles instead. Otherwise claim
     the teardown. Losing the CAS means another agent (a destroy that found
     zero slots; another release under a concurrent re-acquire) owns it —
     this caller stands down, silently, without touching the record. */
  uint32_t expect = 0;
  if (!atomic_compare_exchange_strong(&f->engine.pending_submits, &expect,
                                      SA_ENGINE_SUBMIT_CLAIM)) {
    return 0;
  }
  _frame_destroy_run(f);
  return 1;
}

uint8_t _frame_engine_submit_settle(frame_t* f) {
  if (f == NULL) return 0;
  if (atomic_load(&f->engine.die_requested) == 0) {
    atomic_store(&f->engine.submit_inflight, 0);
    return 0;
  }
  /* Die-requested. Keep the inflight marker up THROUGH the decision — a
     destroy that just won the CLAIM stands down the moment it sees it, so
     this engine thread cannot race a teardown over the freed record. */
  for (;;) {
    if (atomic_load(&f->engine.pending_submits) != 0) {
      /* A slot is still out — its sink owes the release; the die gate (and
         the actor's own refuse) cover the wait. */
      atomic_store(&f->engine.submit_inflight, 0);
      return 0;
    }
    uint32_t expect = 0;
    if (!atomic_compare_exchange_strong(&f->engine.pending_submits, &expect,
                                        SA_ENGINE_SUBMIT_CLAIM)) {
      continue;   /* a just-unclaiming destroy — retry */
    }
    atomic_store(&f->engine.submit_inflight, 0);
    _frame_destroy_run(f);
    return 1;
  }
}

/* --- store operations ---------------------------------------------------- */

int frame_remember_local(frame_t* f, const char* key, const char* json_value) {
  return _frame_remember_sync(f, key, json_value, "state/local/");
}

int frame_remember_ctx(frame_t* f, const char* key, const char* json_value) {
  return _frame_remember_sync(f, key, json_value, "state/ctx/");
}

/* Compose + post FRM_STORE_RECALL: the walk runs inside the store actor's
   dispatch (serialized with every write). 0 = posted; -1 = refused before
   any post (the reply never comes — the caller must answer its corr). */
static int _frame_recall_post(frame_t* f, const char* key, uint64_t corr,
                              actor_t* reply_to) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: recall on a dead frame");
    return -1;
  }
  frm_store_recall_payload_t* rp =
      (frm_store_recall_payload_t*)get_clear_memory(sizeof(frm_store_recall_payload_t));
  rp->key = strdup(key);
  rp->sid_path = strdup(f->sid_path);
  if (rp->key == NULL || rp->sid_path == NULL) {
    frm_store_recall_payload_destroy(rp);
    return -1;
  }
  rp->max_hops = f->max_depth;
  rp->reply_to = reply_to;
  rp->corr = corr;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_RECALL, rp,
              frm_store_recall_payload_destroy, "recall walk");
  return 0;
}

/* Compose + post FRM_STORE_KEYS (spec §3): the bounded OWN-subtree scan
   runs inside the store actor's dispatch (serialized with every write).
   0 = posted; -1 = refused before any post (the reply never comes — the
   caller must answer its corr). */
static int _frame_keys_post(frame_t* f, const char* scope, uint64_t corr,
                            actor_t* reply_to) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: keys on a dead frame");
    return -1;
  }
  frm_store_keys_payload_t* kp =
      (frm_store_keys_payload_t*)get_clear_memory(sizeof(frm_store_keys_payload_t));
  kp->sid_path = strdup(f->sid_path);
  kp->scope = strdup(scope);
  if (kp->sid_path == NULL || kp->scope == NULL) {
    frm_store_keys_payload_destroy(kp);
    return -1;
  }
  kp->reply_to = reply_to;
  kp->corr = corr;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_KEYS, kp,
              frm_store_keys_payload_destroy, "keys scan");
  return 0;
}

char* frame_recall(frame_t* f, const char* key) {
  /* The sync public recall: compose + post the walk + pump-wait the
     corr-matched reply (§5: synchronous by PUMPING, never by locking). */
  if (f == NULL || f->st == NULL || !_frame_key_valid(key, "recall")) return NULL;
  if (_frame_sync_store_refused(f, "recall")) return NULL;
  if (_frame_nested_sync(f)) {
    log_error("frame: nested recall '%s' at '%s' refuses loud — a read can "
              "never return from inside the frame's own dispatch (the engine "
              "and the cell verbs use the store-actor round trips)",
              key, f->sid_path);
    return NULL;
  }
  _frame_sync_slot_reset(f);
  f->sync.in_use = 1;
  f->sync.corr = ++f->store_corr_seq;
  int rc = _frame_recall_post(f, key, f->sync.corr, &f->actor);
  if (rc != 0) {
    f->sync.in_use = 0;
    return NULL;
  }
  int wait_rc = _frame_slot_wait(f, &f->sync.done, SA_FRAME_STORE_WAIT_MS);
  f->sync.in_use = 0;
  if (wait_rc != 0) return NULL;         /* deadline: the walk still lands */
  if (f->sync.rc != 0) return NULL;      /* unresolvable — today's NULL shape */
  char* out = f->sync.text;              /* transfer */
  f->sync.text = NULL;
  return out;
}

int frame_append_msg(frame_t* f, const char* role, const char* content) {
  return _frame_append_msg(f, role, content);
}

/* --- spawn / report / join ----------------------------------------------- */

/* Admission-only spawn, POSTED. Nothing is written before the batch, and the
   batch is ONE atomic root transaction executed by the store actor,
   combining three sources of keys:
     - the child's birth meta (meta/created, meta/status, meta/depth,
       meta/parent) — composed as full root paths,
     - the child's ctx handoff key (state/ctx/handoff) when context_json is
       non-NULL,
     - the parent's frame.spawn event (at the parent's PRE-ALLOCATED seq),
     - the lineage triple index ops (graph_triple_expand_ops on the root's
       reserved lineage layer — full root-database paths via the subtree
       wrapper's prepend).
   The composition is the spawn slice's shape UNCHANGED (the child subtree is
   fresh, so the single-live-writer condition held already); only WHERE the
   batch executes moved — into the store actor's behavior — and the caller
   learns the outcome from its corr-matched reply through `spawn_slot` (the
   direct sync caller pump-waits; a cell-verb spawn is answered by the
   router at the reply). bridge_corr != 0 marks the cell-side round trip. */
static frame_t* _frame_spawn_post(frame_t* parent, const char* goal,
                                  const char* context_json,
                                  uint64_t bridge_corr, uint64_t corr) {
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

  uint64_t pseq = _frame_seq_alloc(parent);   /* the lock's replacement */

  json_value_t* payload = json_new_object();
  if (payload == NULL) {
    log_error("frame_spawn: out of memory building spawn payload");
    _frame_seq_rollback(parent, pseq);
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
    _frame_seq_rollback(parent, pseq);
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
    _frame_seq_rollback(parent, pseq);
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
    _frame_seq_rollback(parent, pseq);
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
    _frame_seq_rollback(parent, pseq);
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
    _frame_seq_rollback(parent, pseq);
    frame_destroy(child);
    return NULL;
  }
  nops += ngo;

  /* The ONE store round trip: the composed ops move into a store batch
     (every key/value OWNED by the payload — the store behavior frees them
     after it acts; the graph ops' borrowed static empty value gets a heap
     twin), the parent's own store corr + the frame_spawn reply target are
     set, and the admission posts. The store's rc arrives as a corr-matched
     reply — today's "refused → destroy the child" contract moves to the
     reply. */
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = (frm_store_op_t*)get_clear_memory(nops * sizeof(frm_store_op_t));
  bp->nops = nops;
  for (size_t i = 0; i < nops; i++) {
    bp->ops[i].key = (char*)ops[i].key;              /* OWNED: transferred */
    size_t vlen = ops[i].value_len;
    uint8_t* v = (uint8_t*)get_memory(vlen + 1);
    if (ops[i].value != NULL) memcpy(v, ops[i].value, vlen);
    v[vlen] = '\0';
    bp->ops[i].value = v;                            /* OWNED (heap twin) */
    bp->ops[i].value_len = vlen;
  }
  free(event_text);   /* the working array's ONLY heap-owed value (the graph
                         ops carry a static marker; the rest are stack) —
                         the payload owns the copies now. The keys were
                         TRANSFERRED (freed by the store round trip). */
  bp->op_name = "spawn-admission";                   /* BORROWED literal */
  bp->reply_to = &parent->actor;                     /* today's reply contract */
  bp->corr = corr;
  /* The admission's own entry (corr-keyed; admissions overlap). Filled
     BEFORE the post so the reply's router finds it. */
  {
    frm_spawn_pending_t* pe = get_clear_memory(sizeof(*pe));
    pe->corr = corr;
    pe->bridge_corr = bridge_corr;
    pe->own_seq = pseq;
    pe->child = child;
    pe->next = parent->spawn_pending;
    parent->spawn_pending = pe;
  }
  _frame_post(&parent->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "spawn admission");
  return child;
}

/* The public admission: compose + post + pump-wait the store's commit — the
   child is returned only after the admission COMMITS (today's contract, now
   confirmed by the store reply). A refusal destroys the child exactly as
   today. The wait matches THIS call's corr among the (overlappable) pending
   admissions — the entries are the parent's dispatch thread's memory and the
   pump runs its dispatches, so the read after each pump is race-free. */
frame_t* frame_spawn(frame_t* parent, const char* goal, const char* context_json) {
  if (parent == NULL || parent->st == NULL) {
    log_error("frame_spawn: no live parent frame");
    return NULL;
  }
  if (_frame_sync_store_refused(parent, "spawn")) return NULL;
  if (_frame_nested_sync(parent)) {
    log_error("frame_spawn: a nested spawn refuses loud — the admission "
              "cannot be awaited from inside the frame's own dispatch");
    return NULL;
  }
  uint64_t corr = ++parent->store_corr_seq;
  frame_t* child = _frame_spawn_post(parent, goal, context_json, 0, corr);
  if (child == NULL) return NULL;
  uint64_t deadline =
      platform_monotonic_ns() + (uint64_t)SA_FRAME_STORE_WAIT_MS * 1000000ULL;
  frm_spawn_pending_t* pe = NULL;
  for (;;) {
    _frame_pump(parent);
    for (frm_spawn_pending_t* w = parent->spawn_pending; w != NULL; w = w->next) {
      if (w->corr == corr && w->done != 0) {
        pe = w;
        break;
      }
    }
    if (pe != NULL) break;
    if (platform_monotonic_ns() >= deadline) {
      log_error("frame_spawn: corr %llu's admission reply never arrived in "
                "time at '%s' — the child is abandoned loud",
                (unsigned long long)corr, parent->sid_path);
      break;
    }
    platform_sleep_ms(1);
  }
  uint8_t ok = (pe != NULL && pe->rc == 0);
  int rc = (pe != NULL) ? pe->rc : 0;
  /* The entry leaves the list (the caller ADOPTS the child either way; a
     started child comes back engine-live). */
  if (pe != NULL) {
    frm_spawn_pending_t** p = &parent->spawn_pending;
    while (*p != NULL && *p != pe) p = &(*p)->next;
    if (*p == pe) *p = pe->next;
    free(pe);
  }
  if (!ok) {
    if (pe != NULL) {
      log_error("frame_spawn: admission for '%s' under '%s' refused (%d) — "
                "nothing committed; the child was never born",
                frame_sid(child), parent->sid_path, rc);
    }
    frame_destroy(child);
    return NULL;
  }
  return child;
}

/* Report: the CROSS-SUBTREE round trip (§5). The CHILD composes ONLY its own
   frame.report record (its own pre-allocated seq) and posts FRM_REPORT_BIND
   to the parent's actor; the PARENT composes the whole three-op batch — the
   child's record + the child's meta/status=done + the parent's bound
   frame.report event at the parent's own pre-allocated seq — and the store
   actor executes it as ONE atomic commit, corr-matched back to the CHILD.
   Both events keep payload {child_sid, text}; each keeps its own frame's seq
   cause chain. engine_driven (the terminal step) posts FRM_CHILD_REPORT from
   the same reply route, carrying `failed` through the bind slot (the parent
   resumes loud on a failed child); a cell-verb report or a direct sync-API
   report only binds (failed is meaningless there). */
int _frame_report_bind_post(frame_t* child, uint64_t bridge_corr,
                            uint8_t engine_driven, uint8_t failed,
                            const char* text) {
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

  uint64_t cseq = _frame_seq_alloc(child);
  json_value_t* child_payload = json_new_object();
  if (child_payload == NULL) {
    log_error("frame_report: out of memory building report payload");
    _frame_seq_rollback(child, cseq);
    return -1;
  }
  json_object_set(child_payload, "child_sid", json_new_string(child->sid_path));
  json_object_set(child_payload, "text", json_new_string(text));
  char* child_text = _frame_event_json_full(child->sid_path, cseq,
                                            "frame.report", child_payload);
  if (child_text == NULL) {
    _frame_seq_rollback(child, cseq);
    return -1;
  }
  size_t child_total = strlen(child_text) + strlen("events/%020llu");
  if (child_total > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame_report: report batch from '%s' is %zu bytes, exceeding "
              "the %d-byte WAL batch cap — refusing, never truncating",
              child->sid_path, child_total, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(child_text);
    _frame_seq_rollback(child, cseq);
    return -3;
  }

  /* The child's own store round trip + the composed cross-subtree request. */
  uint64_t corr = ++child->store_corr_seq;
  frm_report_bind_payload_t* b =
      (frm_report_bind_payload_t*)get_clear_memory(sizeof(frm_report_bind_payload_t));
  b->reply_to = &child->actor;   /* the CHILD's actor receives the store reply */
  b->corr = corr;
  b->bridge_corr = bridge_corr;
  b->engine_driven = engine_driven;
  b->child_sid = strdup(child->sid_path);
  b->child_seq = cseq;
  b->child_event_text = child_text;   /* OWNED: transferred to the parent */
  b->text = strdup(text);
  if (b->child_sid == NULL || b->text == NULL) {
    frm_report_bind_payload_destroy(b);
    _frame_seq_rollback(child, cseq);
    return -1;
  }
  /* The bind slot is filled BEFORE the post (the reply routes by corr). */
  child->bind_slot.in_use = 1;
  child->bind_slot.done = 0;
  child->bind_slot.corr = corr;
  child->bind_slot.bridge_corr = bridge_corr;
  child->bind_slot.engine_driven = engine_driven;
  child->bind_slot.failed = failed;
  child->bind_slot.own_seq = cseq;
  _frame_post(&parent->actor, (uint32_t)FRM_REPORT_BIND, b,
              frm_report_bind_payload_destroy, "report bind");
  return 0;
}

/* The parent's half: the FRM_REPORT_BIND behavior composes the WHOLE
   three-op batch (the parent's own seq is pre-allocated HERE — the child
   could never touch it) and posts it to the store actor with the reply
   addressed back to the CHILD's actor (carried). On its own COMPOSE-stage
   refusal the parent rolls its seq back and releases the child with a
   refusal reply; a store-stage refusal is the store worker's loud log (the
   parent learns nothing from the store — its own seq was pre-allocated at
   compose). CONSUMES the bind payload on every path. */
static void _frame_report_bind_compose(frame_t* f, frm_report_bind_payload_t* b) {
  int rc = 0;
  uint64_t pseq = 0;
  if (f == NULL || f->st == NULL || b == NULL) {
    log_error("frame_report: report bind at a dead parent");
    if (b != NULL) {
      _store_reply_send(b->reply_to, b->corr, -1, NULL, 0);
      frm_report_bind_payload_destroy(b);
    }
    return;
  }
  if (b->corr == 0 || b->child_sid == NULL || b->child_seq == 0 ||
      b->child_event_text == NULL || b->text == NULL) {
    log_error("frame_report: an incomplete report bind payload at '%s' — "
              "refused (the child's corr still gets its refusal reply)",
              f->sid_path);
    _store_reply_send(b->reply_to, b->corr, -1, NULL, 0);
    frm_report_bind_payload_destroy(b);
    return;
  }

  pseq = _frame_seq_alloc(f);
  json_value_t* parent_payload = json_new_object();
  if (parent_payload == NULL) {
    log_error("frame_report: out of memory building bound report payload");
    _frame_seq_rollback(f, pseq);
    _store_reply_send(b->reply_to, b->corr, -1, NULL, 0);
    frm_report_bind_payload_destroy(b);
    return;
  }
  json_object_set(parent_payload, "child_sid", json_new_string(b->child_sid));
  json_object_set(parent_payload, "text", json_new_string(b->text));
  char* parent_text = _frame_event_json_full(f->sid_path, pseq,
                                             "frame.report", parent_payload);
  char* k_cev = _frame_event_key(b->child_sid, b->child_seq);
  char* k_pev = _frame_event_key(f->sid_path, pseq);
  char* k_status = _frame_subkey(b->child_sid, "meta/status");
  if (parent_text == NULL || k_cev == NULL || k_pev == NULL || k_status == NULL) {
    free(parent_text);
    free(k_cev);
    free(k_pev);
    free(k_status);
    _frame_seq_rollback(f, pseq);
    _store_reply_send(b->reply_to, b->corr, -1, NULL, 0);
    frm_report_bind_payload_destroy(b);
    return;
  }

  size_t total_bytes = strlen(k_cev) + strlen(k_pev) + strlen(k_status) +
                       strlen(parent_text) + strlen(b->child_event_text) +
                       strlen(b->text);
  if (total_bytes > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame_report: report batch from '%s' into '%s' is %zu bytes, "
              "exceeding the %d-byte WAL batch cap — refusing, never "
              "truncating", b->child_sid, f->sid_path, total_bytes,
              (int)SA_FRAME_MAX_BATCH_BYTES);
    free(k_cev);
    free(k_pev);
    free(k_status);
    free(parent_text);
    _frame_seq_rollback(f, pseq);
    _store_reply_send(b->reply_to, b->corr, -3, NULL, 0);
    frm_report_bind_payload_destroy(b);
    return;
  }

  frm_store_batch_payload_t* bp =
      get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = get_clear_memory(3 * sizeof(frm_store_op_t));
  bp->nops = 3;
  bp->ops[0].key = k_cev;                    /* OWNED by the store round trip */
  bp->ops[0].value = b->child_event_text;    /* OWNED: transferred in */
  bp->ops[0].value_len = strlen(b->child_event_text);
  b->child_event_text = NULL;                /* consumed */
  bp->ops[1].key = k_pev;                    /* OWNED */
  bp->ops[1].value = parent_text;            /* OWNED */
  bp->ops[1].value_len = strlen(parent_text);
  bp->ops[2].key = k_status;                 /* OWNED */
  bp->ops[2].value = (uint8_t*)get_memory(strlen(SA_FRAME_STATUS_DONE) + 1);
  if (bp->ops[2].value == NULL) {
    frm_store_batch_payload_destroy(bp);
    _frame_seq_rollback(f, pseq);
    _store_reply_send(b->reply_to, b->corr, -1, NULL, 0);
    frm_report_bind_payload_destroy(b);
    return;
  }
  memcpy(bp->ops[2].value, SA_FRAME_STATUS_DONE, strlen(SA_FRAME_STATUS_DONE) + 1);
  bp->ops[2].value_len = strlen(SA_FRAME_STATUS_DONE);
  bp->op_name = "frame.report (bind)";       /* BORROWED literal */
  bp->reply_to = b->reply_to;                /* BORROWED: the CHILD's actor */
  bp->corr = b->corr;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "report bind batch");
  frm_report_bind_payload_destroy(b);
}

/* The parent-facing child-report post (frame_internal.h's contract): ONE
   frm_child_report_payload_t{child_sid, failed} at the LIVE parent's actor.
   The bind router (an engine-driven bind reply) and the terminal step's
   never-hang fallback both reach the parent through it. */
void _frame_child_notify_post(frame_t* child, uint8_t failed) {
  if (child == NULL || child->st == NULL) {
    log_error("frame: the child-notify for a dead child record — dropping "
              "loud (a gone record's parent loses only a resume its own "
              "engine end already handled)");
    return;
  }
  frame_t* parent = child->parent;
  if (parent == NULL || parent->st == NULL) {
    log_error("frame: the child-notify for '%s' has no live parent log — "
              "dropping loud (no engine could be live behind it)",
              child->sid_path);
    return;
  }
  frm_child_report_payload_t* crp =
      (frm_child_report_payload_t*)get_clear_memory(sizeof(frm_child_report_payload_t));
  crp->child_sid = strdup(child->sid_path);
  if (crp->child_sid == NULL) {
    log_error("frame: out of memory building the child-notify payload for '%s'",
              child->sid_path);
    frm_child_report_payload_destroy(crp);
    return;
  }
  crp->failed = failed;
  _frame_post(&parent->actor, (uint32_t)FRM_CHILD_REPORT, crp,
              frm_child_report_payload_destroy, "child report");
}

/* The resume path's folded frame.join (frame_internal.h contract): ONE
   frame.join event in the parent's log {child_sid}, FIRE-AND-POST — it runs
   inside the parent's own dispatch (a nested caller never awaits), so the
   store's FIFO commits it ahead of the resumed derive's scan. On-failure-
   continue: every refusal logs loud + rolls the seq back best-effort and the
   resume still happens. */
void _frame_join_post(frame_t* parent, const char* child_sid) {
  if (parent == NULL || parent->st == NULL) {
    log_error("frame_join: the folded join for '%s' needs a live parent log",
              child_sid != NULL ? child_sid : "?");
    return;
  }
  if (child_sid == NULL || child_sid[0] == '\0') {
    log_error("frame_join: the folded join needs the child's sid at '%s'",
              parent->sid_path);
    return;
  }
  uint64_t pseq = _frame_seq_alloc(parent);
  json_value_t* payload = json_new_object();
  if (payload == NULL) {
    log_error("frame_join: out of memory building join payload");
    _frame_seq_rollback(parent, pseq);
    return;
  }
  json_object_set(payload, "child_sid", json_new_string(child_sid));
  char* parent_text = _frame_event_json_full(parent->sid_path, pseq,
                                             "frame.join", payload);
  if (parent_text == NULL) {
    _frame_seq_rollback(parent, pseq);
    return;
  }
  char* k_pev = _frame_event_key(parent->sid_path, pseq);
  if (k_pev == NULL) {
    free(parent_text);
    _frame_seq_rollback(parent, pseq);
    return;
  }
  size_t total_bytes = strlen(k_pev) + strlen(parent_text);
  if (total_bytes > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame_join: join batch for '%s' is %zu bytes, exceeding the "
              "%d-byte WAL batch cap — refusing, never truncating",
              child_sid, total_bytes, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(k_pev);
    free(parent_text);
    _frame_seq_rollback(parent, pseq);
    return;
  }
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
  bp->nops = 1;
  bp->ops[0].key = k_pev;                    /* OWNED by the store round trip */
  bp->ops[0].value = (uint8_t*)parent_text;  /* OWNED */
  bp->ops[0].value_len = strlen(parent_text);
  bp->op_name = "frame.join";                /* BORROWED literal */
  bp->reply_to = NULL;                       /* fire-and-post (the resume path
                                                never waits) */
  bp->corr = 0;
  _frame_post(&parent->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "join batch");
}

/* The meta/status=done put, FIRE-AND-POST (frame_internal.h contract — the
   top engine's end-rule fix-up: a finish batch composed while live children
   were pending carries NO status put (the CHILDREN yield keeps the frame
   "running"), so an engine ending with live_children == 0 whose finish reply
   raced the last child report completes the frame itself; nothing awaits
   this batch, and the store's FIFO commits it ahead of anything the engine
   posts after). */
void _frame_status_post_fire(frame_t* f) {
  if (f == NULL || f->st == NULL) {
    log_error("frame: status->done post on a dead frame");
    return;
  }
  char* k_status = _frame_subkey(f->sid_path, "meta/status");
  if (k_status == NULL) return;
  uint8_t* val = (uint8_t*)get_memory(strlen(SA_FRAME_STATUS_DONE) + 1);
  if (val == NULL) {
    free(k_status);
    log_error("frame: out of memory building the status put for '%s'",
              f->sid_path);
    return;
  }
  memcpy(val, SA_FRAME_STATUS_DONE, strlen(SA_FRAME_STATUS_DONE) + 1);
  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
  bp->nops = 1;
  bp->ops[0].key = k_status;    /* OWNED by the store round trip */
  bp->ops[0].value = val;       /* OWNED */
  bp->ops[0].value_len = strlen(SA_FRAME_STATUS_DONE);
  bp->op_name = "status->done"; /* BORROWED literal */
  bp->reply_to = NULL;
  bp->corr = 0;
  _frame_post(&f->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "status batch");
}

/* The public bind (pump-waits the corr-matched store reply at the child). */
int frame_report(frame_t* child, const char* text) {
  if (child == NULL || child->st == NULL) {
    log_error("frame_report: dead frame");
    return -1;
  }
  if (_frame_sync_store_refused(child, "report")) return -1;
  if (_frame_nested_sync(child)) {
    log_error("frame_report: a nested report refuses loud — the bind "
              "cannot be awaited from inside the frame's own dispatch");
    return -1;
  }
  int rc = _frame_report_bind_post(child, 0, 0, 0, text);
  if (rc != 0) {
    child->bind_slot.in_use = 0;
    return rc;
  }
  (void)_frame_slot_wait(child, &child->bind_slot.done, SA_FRAME_STORE_WAIT_MS);
  if (!child->bind_slot.done) {
    /* Deadline: the bind may still commit — the seq stays pre-allocated. */
    child->bind_slot.in_use = 0;
    return -1;
  }
  rc = child->bind_slot.rc;                  /* the seq rollback ran there */
  child->bind_slot.in_use = 0;
  return rc;
}

/* Join: ONE frame.join event in the parent's log {child_sid}, posted as a
   single-op store batch with the parent's PRE-ALLOCATED seq; the child's
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
  if (_frame_sync_store_refused(parent, "join")) return -1;
  if (_frame_nested_sync(parent)) {
    log_error("frame_join: a nested join refuses loud — the batch cannot be "
              "awaited from inside the parent's own dispatch");
    return -1;
  }

  uint64_t pseq = _frame_seq_alloc(parent);
  json_value_t* payload = json_new_object();
  if (payload == NULL) {
    log_error("frame_join: out of memory building join payload");
    _frame_seq_rollback(parent, pseq);
    return -1;
  }
  json_object_set(payload, "child_sid", json_new_string(child->sid_path));
  char* parent_text = _frame_event_json_full(parent->sid_path, pseq,
                                             "frame.join", payload);
  if (parent_text == NULL) {
    _frame_seq_rollback(parent, pseq);
    return -1;
  }

  char* k_pev = _frame_event_key(parent->sid_path, pseq);
  if (k_pev == NULL) {
    free(parent_text);
    _frame_seq_rollback(parent, pseq);
    return -1;
  }

  size_t total_bytes = strlen(k_pev) + strlen(parent_text);
  if (total_bytes > SA_FRAME_MAX_BATCH_BYTES) {
    log_error("frame_join: join batch for '%s' is %zu bytes, exceeding the "
              "%d-byte WAL batch cap — refusing, never truncating",
              child->sid_path, total_bytes, (int)SA_FRAME_MAX_BATCH_BYTES);
    free(k_pev);
    free(parent_text);
    _frame_seq_rollback(parent, pseq);
    return -3;
  }

  frm_store_batch_payload_t* bp =
      (frm_store_batch_payload_t*)get_clear_memory(sizeof(frm_store_batch_payload_t));
  bp->ops = (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
  bp->nops = 1;
  bp->ops[0].key = k_pev;                    /* OWNED by the store round trip */
  bp->ops[0].value = (uint8_t*)parent_text;  /* OWNED */
  bp->ops[0].value_len = strlen(parent_text);
  bp->op_name = "frame.join";                /* BORROWED literal */
  _frame_sync_slot_reset(parent);   /* a prior recall's leftover records die here too */
  parent->sync.in_use = 1;
  parent->sync.corr = ++parent->store_corr_seq;
  bp->reply_to = &parent->actor;
  bp->corr = parent->sync.corr;
  _frame_post(&child->root->store_actor, (uint32_t)FRM_STORE_BATCH, bp,
              frm_store_batch_payload_destroy, "join batch");
  int wait_rc = _frame_slot_wait(parent, &parent->sync.done,
                                 SA_FRAME_STORE_WAIT_MS);
  parent->sync.in_use = 0;
  if (wait_rc != 0) return -1;   /* the seq stays (the batch may still commit) */
  int rc = parent->sync.rc;
  if (rc != 0) {
    _frame_seq_rollback(parent, pseq);
    log_error("frame_join: join batch failed (%d) for '%s' out of '%s' — "
              "nothing committed", rc, child->sid_path, parent->sid_path);
    return rc;
  }
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