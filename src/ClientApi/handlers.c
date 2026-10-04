//
// Created by victor on 10/04/26.
//

/* The client-api handlers (the client-api spec §3 + the plan's Task-4
   contract): the actor-glue over the frames and the store. handlers.h holds
   the threading model — THIS file only adds the two internal ownership
   facts:

   1. THE CLOSURE MARSHAL. Every path onto the server state is a heap
      closure enqueued through streams_loop_call onto the loop thread (an
      async enqueue — a same-thread caller defers to the next tick, never an
      inline nested run). Each enqueue takes one closures_live slot BEFORE
      the call and the run fn re-checks `dying`; ca_session_server_destroy's
      drain waits (bounded) for the slots to empty AND the pending store
      round trips to be answered, so no closure can ever dereference freed
      server memory.
   2. THE CONNECTION REFERENCES. Everything that may still hand a connection
      a response — a work closure for the current request, a subscription, a
      pending store round trip — holds one ref through the connection's
      refcounter pair (ca_session_conn_t); the teardown's purge and every
      closure's tail release them. A connection torn down mid-conversation
      keeps its object alive until the last handler touch; its sends then
      refuse and the handlers drop loud. */

#include "handlers.h"

typedef int ca_handlers_tu_anchor_t;   /* ISO C: a non-empty TU when gated */

#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include "../ClientApi/client_api_wire.h"
#include "../Frame/frame_internal.h"
#include "../Frame/frame_messages.h"
#include "../Platform/platform_time.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include "../Util/json.h"
#include "../Util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- internal bounds ------------------------------------------------------ */

/* The teardown's drain: in-flight closures + unanswered store round trips.
   Honest quiescence drains in µs–ms; the bound only ever breaks on a dead
   loop (whose own destroy then logs its stranded FIFO) — loud, never a
   hang. */
#ifndef _CA_DRAIN_WAIT_MS
#define _CA_DRAIN_WAIT_MS 5000
#endif

/* ONE subscription's replay-gap buffer: event records the store's watch was
   already live for while this subscription's replay had not streamed yet
   (two subscribers on one sid, or a commit racing the subscription's own
   FIFO pair — see _ca_on_events). The store's FIFO keeps it tiny; over the
   cap the record is dropped LOUD (a recorded loss, never an unbounded
   buffer). */
#define _CA_HOLD_MAX 256

/* The error text's scratch (truncation-safe, like the model layer's). */
#define _CA_ERROR_TEXT_MAX 192

/* --- the server's state (the loop thread's single-writer domain) ---------- */

/* A pending store round trip: the corr's routing data (lives until its
   reply closure removes it, or the conn-closed purge drops it). */
typedef enum ca_pending_kind_e {
  CA_PENDING_SESSIONS = 1,     /* FRM_STORE_LIST_SESSIONS's reply */
  CA_PENDING_EVENTS_SCAN = 2,  /* FRM_STORE_SCAN's reply (the replay) */
} ca_pending_kind_e;

typedef struct ca_session_pending_t {
  uint64_t corr;
  ca_pending_kind_e kind;
  uint64_t req_id;
  uint8_t op;                 /* the events request's op (the marker echoes) */
  char* sid;                  /* OWNED; NULL for the sessions listing */
  void* conn;                 /* borrowed through the iface's refcount */
  ca_session_conn_t* iface;   /* borrowed; ONE conn ref held */
  struct ca_session_pending_t* next;
} ca_session_pending_t;

/* One replay-gap hold (a notice parked until the reply's flush — see the
   subscription's `live` note). */
typedef struct ca_hold_t {
  uint64_t seq;
  char* record_json;   /* OWNED */
  struct ca_hold_t* next;
} ca_hold_t;

/* One events subscription, keyed (connection, sid) — several connections on
   one sid is the legal shape. The STORE holds ONE watch per sid (keyed on
   the server's actor — idempotent there), so the watch/unwatch boundary is
   THIS sid's total subscriber count, tracked at removal time.
   `live`: 0 until the live marker is sent — notices arriving meanwhile are
   the replay's in-flight facts, HELD (bounded) and flushed after the
   replay deduplicated by last_seq, so every record delivers EXACTLY ONCE. */
typedef struct ca_session_sub_t {
  uint64_t req_id;
  uint8_t op;                 /* the subscribing request's op */
  char* sid;                  /* OWNED; the subscribed subtree */
  void* conn;
  ca_session_conn_t* iface;   /* borrowed; ONE conn ref held */
  uint8_t live;
  uint64_t last_seq;          /* the highest seq forwarded so far */
  ca_hold_t* hold;            /* FIFO: the oldest first */
  ca_hold_t* hold_tail;
  size_t nhold;
  struct ca_session_sub_t* next;
} ca_session_sub_t;

/* One registered session: the server OWNS the frame record, and the entry
   STAYS past frame_is_done (the events channel keeps replaying a done
   session's log; steer/interrupt refuse on a done frame at the handler).
   There is NO removal during the server's life. */
typedef struct ca_session_reg_t {
  char* sid;    /* OWNED; the frame's sid path */
  frame_t* f;   /* OWNED */
  struct ca_session_reg_t* next;
} ca_session_reg_t;

struct ca_session_server_t {
  actor_t actor;                    /* FIRST member: the server IS an actor
                                       — the store's replies/notices bounce
                                       through its mailbox (dispatched by
                                       the pool's workers; every message
                                       re-marshals to the loop thread) */
  wave_database_root_t* root;       /* borrowed; never freed here */
  scheduler_pool_t* pool;           /* borrowed */
  streams_loop_thread_t* loop;      /* borrowed */
  model_backend_t* shared_backend;  /* borrowed */
  frame_config_t cfg;               /* the created frames' template (the
                                       pool member = the server's pool) */
  ATOMIC(uint8_t) dying;            /* the destroy's gate */
  ATOMIC(uint32_t) closures_live;   /* the enqueued-closures count */
  uint64_t corr_seq;                /* the server's store round-trip keys */
  ca_session_reg_t* regs;           /* loop-thread domain */
  ca_session_sub_t* subs;           /* loop-thread domain */
  ca_session_pending_t* pending;    /* loop-thread domain */
};

/* --- small helpers --------------------------------------------------------- */

static void _ca_conn_ref(ca_session_conn_t* iface) {
  if (iface != NULL && iface->retain != NULL) iface->retain(iface->conn);
}

static void _ca_conn_unref(ca_session_conn_t* iface) {
  if (iface != NULL && iface->release != NULL) iface->release(iface->conn);
}

/* Send + own-on-refusal (the ca_session_conn_t contract's sender half): the
   send consumes the payload on 0; here the payload dies on refusal. */
static int _ca_send(ca_session_conn_t* iface, uint64_t type, void* payload) {
  if (iface == NULL) {
    ca_wire_payload_destroy(type, payload);
    return -1;
  }
  if (iface->send(iface->conn, type, payload) != 0) {
    log_error("ca: a connection could not take a %llu frame — dropped loud",
              (unsigned long long)type);
    ca_wire_payload_destroy(type, payload);
    return -1;
  }
  return 0;
}

/* The ONE refusal answer for every pairing's failure (the wire's closed
   vocabulary's error frame). */
static void _ca_send_error(ca_session_conn_t* iface, uint64_t req_id,
                           uint8_t status, const char* fmt, ...) {
  char scratch[_CA_ERROR_TEXT_MAX];
  ca_error_t* err;
  va_list args;

  va_start(args, fmt);
  vsnprintf(scratch, sizeof(scratch), fmt, args);
  va_end(args);

  err = (ca_error_t*)get_clear_memory(sizeof(*err));
  err->req_id = req_id;
  err->status = status;
  err->text = (char*)get_memory(strlen(scratch) + 1);
  strcpy(err->text, scratch);
  (void)_ca_send(iface, CA_ERROR, err);
}

/* --- the loop-thread closures --------------------------------------------- */

typedef struct ca_work_t {
  ca_session_server_t* server;
  uint64_t type;
  void* payload;              /* the decoded request, transferred */
  ca_session_conn_t* iface;   /* ONE conn ref held since the enqueue */
} ca_work_t;

typedef struct ca_reply_t {
  ca_session_server_t* server;
  frm_store_reply_payload_t* rp;   /* transferred */
} ca_reply_t;

typedef struct ca_notice_t {
  ca_session_server_t* server;
  frm_store_notice_t* np;   /* transferred */
} ca_notice_t;

typedef struct ca_closed_t {
  ca_session_server_t* server;
  ca_session_conn_t* iface;   /* ONE conn ref held since the enqueue */
} ca_closed_t;

/* Arm one closure onto the loop thread: the closures_live slot goes first,
   the `dying` recheck closes the enqueuer/destroy race (the file head), and
   the enqueue is last; any refusal gives the slot back and the CALLER
   destroys what the closure would have consumed. Returns 0 armed. */
static int _ca_closure_arm(ca_session_server_t* server, void* closure,
                           void (*fn)(void*)) {
  ATOMIC_FETCH_ADD(&server->closures_live, 1);
  if (ATOMIC_LOAD(&server->dying) != 0 ||
      streams_loop_call(server->loop, fn, closure) != 0) {
    atomic_fetch_sub(&server->closures_live, 1);
    return -1;
  }
  return 0;
}

/* Every run-fn's LAST step: give the live slot back. */
static void _ca_closure_done(ca_session_server_t* server) {
  atomic_fetch_sub(&server->closures_live, 1);
}

/* --- registry + subscription bookkeeping (loop-thread domain) -------------- */

static frame_t* _ca_reg_find(ca_session_server_t* server, const char* sid) {
  for (ca_session_reg_t* r = server->regs; r != NULL; r = r->next) {
    if (strcmp(r->sid, sid) == 0) return r->f;
  }
  return NULL;
}

static void _ca_reg_add(ca_session_server_t* server, char* sid, frame_t* f) {
  ca_session_reg_t* r = (ca_session_reg_t*)get_clear_memory(sizeof(*r));

  r->sid = sid;   /* OWNED, transferred */
  r->f = f;
  r->next = server->regs;
  server->regs = r;
}

/* The registry's ONLY removal: the create-then-refuse cleanup inside
   _ca_on_prompt (a frame whose start refused). Returns the OWNED frame —
   the caller destroys it — or NULL. */
static frame_t* _ca_reg_take(ca_session_server_t* server, const char* sid) {
  for (ca_session_reg_t** rp = &server->regs; *rp != NULL; rp = &(*rp)->next) {
    if (strcmp((*rp)->sid, sid) == 0) {
      ca_session_reg_t* reg = *rp;
      *rp = reg->next;
      frame_t* f = reg->f;
      free(reg->sid);
      free(reg);
      return f;
    }
  }
  return NULL;
}

static ca_session_sub_t* _ca_sub_find(ca_session_server_t* server, void* conn,
                                      const char* sid) {
  for (ca_session_sub_t* sub = server->subs; sub != NULL; sub = sub->next) {
    if (sub->conn == conn && strcmp(sub->sid, sid) == 0) return sub;
  }
  return NULL;
}

/* Is any OTHER subscription left on this sid (the unwatch's LAST-subscriber
   boundary)? */
static uint8_t _ca_sid_subscribed(ca_session_server_t* server,
                                  const char* sid) {
  for (ca_session_sub_t* sub = server->subs; sub != NULL; sub = sub->next) {
    if (strcmp(sub->sid, sid) == 0) return 1;
  }
  return 0;
}

/* The subscription match: the record's owning subtree must sit under the
   SUBSCRIBED subtree with the store's own segment-boundary rule —
   "sessions/AB" never covers "sessions/ABC/events/..." (the store's
   fan-out rule mirrored at the fan-in). */
static uint8_t _ca_sid_covers(const char* sub_sid, const char* record_sid) {
  size_t len;

  if (sub_sid == NULL || record_sid == NULL) return 0;
  len = strlen(sub_sid);
  if (len == 0 || strlen(record_sid) < len ||
      strncmp(sub_sid, record_sid, len) != 0) {
    return 0;
  }
  if (record_sid[len] == '\0' || record_sid[len] == '/' ||
      sub_sid[len - 1] == '/') return 1;
  return 0;
}

/* Drop ONE sub (NOT its conn ref — the caller releases) + its holds. */
static void _ca_sub_free(ca_session_sub_t* sub) {
  ca_hold_t* h = sub->hold;

  while (h != NULL) {
    ca_hold_t* next = h->next;
    free(h->record_json);
    free(h);
    h = next;
  }
  free(sub->sid);
  free(sub);
}

/* --- the store's posts (the server's subscriptions + round trips) ---------- */

/* The store's watch for one sid (the server's actor is the watcher; the
   store's watch is idempotent per (watcher, sid)), fire-and-post. */
static void _ca_post_watch(ca_session_server_t* server, const char* sid) {
  frm_store_watch_t* wp = (frm_store_watch_t*)get_clear_memory(sizeof(*wp));

  wp->sid_path = (char*)get_memory(strlen(sid) + 1);
  strcpy(wp->sid_path, sid);
  wp->watcher = &server->actor;
  _frame_post(wave_db_store_actor(server->root), (uint32_t)FRM_STORE_WATCH,
              wp, frm_store_watch_destroy, "watch (events subscribe)");
}

/* The store's unwatch for a sid whose LAST subscriber left. */
static void _ca_post_unwatch(ca_session_server_t* server, const char* sid) {
  frm_store_watch_t* wp = (frm_store_watch_t*)get_clear_memory(sizeof(*wp));

  wp->sid_path = (char*)get_memory(strlen(sid) + 1);
  strcpy(wp->sid_path, sid);
  wp->watcher = &server->actor;
  _frame_post(wave_db_store_actor(server->root), (uint32_t)FRM_STORE_UNWATCH,
              wp, frm_store_watch_destroy, "unwatch (subscriber left)");
}

/* The pending entry joins (its conn ref is taken here) and its corr is the
   return value — never 0. */
static ca_session_pending_t* _ca_pending_add(ca_session_server_t* server,
                                             ca_pending_kind_e kind,
                                             uint64_t req_id, uint8_t op,
                                             const char* sid,
                                             ca_session_conn_t* iface) {
  ca_session_pending_t* pe =
      (ca_session_pending_t*)get_clear_memory(sizeof(*pe));

  pe->corr = ++server->corr_seq;   /* never 0 — the router's rule */
  pe->kind = kind;
  pe->req_id = req_id;
  pe->op = op;
  pe->sid = (sid != NULL) ? strdup(sid) : NULL;
  pe->conn = iface->conn;
  pe->iface = iface;
  _ca_conn_ref(iface);
  pe->next = server->pending;
  server->pending = pe;
  return pe;
}

/* The pending entry's removal releases its conn ref. */
static void _ca_pending_free(ca_session_pending_t* pe) {
  if (pe->sid != NULL) free(pe->sid);
  _ca_conn_unref(pe->iface);
  free(pe);
}

/* --- the events' compose helpers ------------------------------------------- */

/* One events response frame: a committed record (seq > 0, the record's OWN
   sid path — a spawned child's subtree nests beneath the subscribed root),
   or the LIVE-TRANSITION marker (seq 0, the op echoed, no record). */
static void _ca_send_events_record(ca_session_conn_t* iface,
                                   uint64_t req_id, uint8_t op,
                                   const char* sid, uint64_t seq,
                                   const char* record_json) {
  ca_events_response_t* res =
      (ca_events_response_t*)get_clear_memory(sizeof(*res));

  res->req_id = req_id;
  res->sid = (char*)get_memory(strlen(sid) + 1);
  strcpy(res->sid, sid);
  res->seq = seq;
  res->op = op;
  if (record_json != NULL) {
    res->record_json = (char*)get_memory(strlen(record_json) + 1);
    strcpy(res->record_json, record_json);
  }
  (void)_ca_send(iface, CA_EVENTS_RESPONSE, res);
}

/* One store record's "seq" field (the record's key truth echoed in the JSON
   shape); 0 = unparseable (real seqs start at 1). */
static uint64_t _ca_record_seq(const char* record_json) {
  char* err = NULL;
  json_value_t* rec = (record_json != NULL)
      ? json_parse(record_json, strlen(record_json), &err) : NULL;
  uint64_t seq = 0;

  if (err != NULL) free(err);
  if (rec == NULL) return 0;
  json_value_t* v = json_get(rec, "seq");
  if (v != NULL && json_type(v) == JSON_INT) {
    seq = (uint64_t)json_as_int(v);
  }
  json_value_destroy(rec);
  return seq;
}

/* One store record's "frame" field (the record's OWN sid path). HEAP, NULL
   when absent/unparseable — the caller falls back to the subscribed sid. */
static char* _ca_record_frame(const char* record_json) {
  char* err = NULL;
  json_value_t* rec = (record_json != NULL)
      ? json_parse(record_json, strlen(record_json), &err) : NULL;
  char* out = NULL;

  if (err != NULL) free(err);
  if (rec == NULL) return NULL;
  json_value_t* v = json_get(rec, "frame");
  if (v != NULL && json_type(v) == JSON_STRING) {
    out = strdup(json_as_string(v));
  }
  json_value_destroy(rec);
  return out;
}

/* Park one committed record into a still-replaying subscription's gap
   buffer (flushed by the scan reply's routing). */
static void _ca_sub_hold(ca_session_sub_t* sub, uint64_t seq,
                         const char* record_json) {
  if (sub->nhold >= _CA_HOLD_MAX) {
    log_error("ca: the replay-gap buffer of '%s' is full (%u) — a live "
              "record dropped loud (the store's FIFO keeps the buffer far "
              "smaller in practice)",
              sub->sid, (unsigned)_CA_HOLD_MAX);
    return;
  }
  ca_hold_t* h = (ca_hold_t*)get_clear_memory(sizeof(*h));
  h->seq = seq;
  h->record_json = strdup(record_json);
  if (sub->hold_tail != NULL) {
    sub->hold_tail->next = h;
  } else {
    sub->hold = h;
  }
  sub->hold_tail = h;
  sub->nhold++;
}

/* Flush the held notices after a replay: only records the replay did not
   cover (seq > last_seq) forward, in FIFO order; the replay was the oldest
   truth, so the store's mid-flight commits land after it, deduplicated by
   last_seq. last_seq advances with every forwarded record. */
static void _ca_sub_flush_holds(ca_session_sub_t* sub) {
  ca_hold_t* h = sub->hold;

  sub->hold = NULL;
  sub->hold_tail = NULL;
  sub->nhold = 0;
  while (h != NULL) {
    ca_hold_t* next = h->next;
    if (h->seq > sub->last_seq) {
      char* record_sid = _ca_record_frame(h->record_json);
      _ca_send_events_record(sub->iface, sub->req_id, sub->op,
                             record_sid != NULL ? record_sid : sub->sid,
                             h->seq, h->record_json);
      sub->last_seq = h->seq;
      free(record_sid);
    }
    free(h->record_json);
    free(h);
    h = next;
  }
}

/* The wire's created rides u64 (meta/created's value, the wire's comment):
   convert the store's ISO birth stamp ("YYYY-MM-DDTHH:MM:SSZ" — _frame_iso
   _now's exact gmtime format) into epoch seconds with the standard
   proleptic-Gregorian civil-days algorithm. A non-conforming stamp = 0 (the
   "absent" sentinel the u64 contract already carries). */
static uint64_t _ca_days_from_civil(unsigned y, unsigned m, unsigned d) {
  uint64_t era;
  unsigned mp, yoe, doy, doe;

  y = (m <= 2) ? (y - 1) : y;   /* the March shift */
  era = y / 400;
  yoe = y - (unsigned)(era * 400);                                   /* [0, 399] */
  mp = (m > 2) ? (m - 3) : (m + 9);
  doy = (153u * mp + 2u) / 5u + d - 1u;                              /* [0, 365] */
  doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;                    /* [0, 146096] */
  return era * 146097u + doe - 719468u;
}

static uint64_t _ca_created_seconds(const char* iso) {
  unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;

  if (iso == NULL) return 0;
  if (sscanf(iso, "%4u-%2u-%2uT%2u:%2u:%2uZ", &y, &mo, &d, &h, &mi, &sec) != 6) {
    return 0;
  }
  if (mo == 0 || mo > 12 || d == 0 || d > 31 || h > 23 || mi > 59 || sec > 59) {
    return 0;
  }
  return _ca_days_from_civil(y, mo, d) * 86400u + h * 3600u + mi * 60u + sec;
}

/* --- the store reply's routing (loop thread) ------------------------------- */

/* The sessions listing's one store row ({"sid","status","goal","created",
   "depth"}) parsed into a heap wire record. A malformed row skips loud
   (the honest partial list — the rows are the store's own compose); the
   CALLER reclaims every filled field. */
static void _ca_sessions_row_parse(const char* row,
                                   ca_sessions_record_t* rec) {
  json_value_t* j = (row != NULL) ? json_parse(row, strlen(row), NULL) : NULL;
  json_value_t* v_sid;
  json_value_t* v_status;
  json_value_t* v_goal;
  json_value_t* v_created;
  json_value_t* v_depth;

  if (j == NULL) {
    log_error("ca: a sessions listing row is not valid JSON — skipping loud");
    return;
  }
  v_sid = json_get(j, "sid");
  v_status = json_get(j, "status");
  v_goal = json_get(j, "goal");
  v_created = json_get(j, "created");
  v_depth = json_get(j, "depth");
  if (v_sid == NULL || json_type(v_sid) != JSON_STRING) {
    log_error("ca: a sessions listing row carries no sid — skipping loud");
  } else {
    rec->sid = strdup(json_as_string(v_sid));
    if (v_status != NULL && json_type(v_status) == JSON_STRING) {
      rec->status = strdup(json_as_string(v_status));
    }
    if (v_goal != NULL && json_type(v_goal) == JSON_STRING) {
      rec->goal = strdup(json_as_string(v_goal));
    }
    if (v_created != NULL && json_type(v_created) == JSON_STRING) {
      rec->created = _ca_created_seconds(json_as_string(v_created));
    }
    if (v_depth != NULL && json_type(v_depth) == JSON_INT) {
      rec->depth = (size_t)json_as_int(v_depth);
    }
  }
  json_value_destroy(j);
}

static void _ca_sessions_respond(ca_session_pending_t* pe,
                                 frm_store_reply_payload_t* rp) {
  ca_sessions_response_t* res;

  if (rp->rc != 0) {
    log_error("ca: the sessions listing was refused by the store (rc %d)",
              rp->rc);
    _ca_send_error(pe->iface, pe->req_id, 1,
                   "the sessions listing was refused (store rc %d)", rp->rc);
    return;
  }
  res = (ca_sessions_response_t*)get_clear_memory(sizeof(*res));
  res->req_id = pe->req_id;
  size_t n = (rp->records != NULL)
                 ? (rp->n <= CA_WIRE_SESSIONS_MAX ? rp->n
                                                  : (size_t)CA_WIRE_SESSIONS_MAX)
                 : 0;
  if (n > 0) {
    res->records = (ca_sessions_record_t*)get_clear_memory(
        n * sizeof(ca_sessions_record_t));
    for (size_t i = 0; i < n; i++) {
      /* COMPACT: a row that never parses is skipped — the array only holds
         filled records (a NULL-sid hole would encode as "" and the client's
         required-sid decode would refuse the WHOLE listing for it). */
      _ca_sessions_row_parse(rp->records[i], &res->records[res->nrecords]);
      if (res->records[res->nrecords].sid == NULL) {
        /* The record's fields were all left NULL — a pure-slot, nothing to
           reclaim (the wire's destroy tolerates NULL fields). */
        continue;
      }
      res->nrecords++;
    }
  }
  (void)_ca_send(pe->iface, CA_SESSIONS_RESPONSE, res);
}

/* The replay's records stream ascending; in-flight notices (the watch was
   already live at the store — see ca_session_sub_t) arrive AFTER this reply
   as holds, flushed post-replay deduplicated by last_seq. Order: the
   records, the flush, the marker, live. */
static void _ca_events_scan_respond(ca_session_server_t* server,
                                    ca_session_pending_t* pe,
                                    frm_store_reply_payload_t* rp) {
  ca_session_sub_t* sub = _ca_sub_find(server, pe->conn, pe->sid);

  if (rp->rc != 0) {
    log_error("ca: the events scan for '%s' was refused by the store (rc %d)",
              pe->sid, rp->rc);
    _ca_send_error(pe->iface, pe->req_id, 1,
                   "the events scan was refused (store rc %d)", rp->rc);
    /* The subscription died with its replay: drop it (unwatch-if-last — the
       watch itself stands at the store until this server's last sub on the
       sid goes). */
    if (sub != NULL) {
      for (ca_session_sub_t** sp = &server->subs; *sp != NULL; sp = &(*sp)->next) {
        if (*sp == sub) {
          *sp = sub->next;
          break;
        }
      }
      if (!_ca_sid_subscribed(server, sub->sid)) {
        _ca_post_unwatch(server, sub->sid);
      }
      _ca_sub_free(sub);
      _ca_conn_unref(pe->iface);
    }
    return;
  }
  if (sub == NULL) {
    /* The subscriber is gone (unsubscribed or torn down): the replay's
       records have nowhere to go — dropped loud, exactly the standing
       refused-reply shape for an unmatched routing. (The conn ref this
       pending entry held was released by the purge.) */
    log_error("ca: the events scan reply for '%s' routed to a dead "
              "subscription — dropped loud", pe->sid);
    return;
  }
  for (size_t i = 0; i < rp->n; i++) {
    const char* text = rp->records[i];
    uint64_t seq = (text != NULL) ? _ca_record_seq(text) : 0;

    if (seq == 0 || seq <= sub->last_seq) {
      log_error("ca: an events replay record for '%s' is unparseable or "
                "below the already-forwarded cursor (seq %llu) — skipped "
                "loud", pe->sid, (unsigned long long)seq);
      continue;
    }
    char* record_sid = _ca_record_frame(text);
    _ca_send_events_record(sub->iface, sub->req_id, sub->op,
                           record_sid != NULL ? record_sid : sub->sid, seq,
                           text);
    sub->last_seq = seq;
    free(record_sid);
  }
  _ca_sub_flush_holds(sub);
  /* The live-transition marker: seq 0, the op echoed, no record (the
     wire's signal shape). */
  _ca_send_events_record(sub->iface, sub->req_id, sub->op, sub->sid, 0, NULL);
  sub->live = 1;
}

static void _ca_store_reply_route(ca_session_server_t* server,
                                  frm_store_reply_payload_t* rp) {
  ca_session_pending_t** pp = &server->pending;

  while (*pp != NULL && (*pp)->corr != rp->corr) pp = &(*pp)->next;
  ca_session_pending_t* pe = *pp;
  if (pe == NULL) {
    log_error("ca: the store reply corr %llu routed to nothing — dropped "
              "loud (a torn-down connection or a routing bug)",
              (unsigned long long)rp->corr);
    frm_store_reply_payload_destroy(rp);
    return;
  }
  *pp = pe->next;
  switch (pe->kind) {
    case CA_PENDING_SESSIONS:
      _ca_sessions_respond(pe, rp);
      break;
    case CA_PENDING_EVENTS_SCAN:
      _ca_events_scan_respond(server, pe, rp);
      break;
  }
  _ca_pending_free(pe);
  frm_store_reply_payload_destroy(rp);
}

/* The dying shape: nothing sends (the requester is going down with the
   server) but the round trip's entry MUST retire — the teardown's drain
   waits on the pending list. */
static void _ca_store_reply_retire(ca_session_server_t* server,
                                   frm_store_reply_payload_t* rp) {
  ca_session_pending_t** pp = &server->pending;

  while (*pp != NULL && (*pp)->corr != rp->corr) pp = &(*pp)->next;
  if (*pp != NULL) {
    ca_session_pending_t* pe = *pp;
    *pp = pe->next;
    _ca_pending_free(pe);
  }
  frm_store_reply_payload_destroy(rp);
}

static void _ca_reply_run(void* raw) {
  ca_reply_t* c = (ca_reply_t*)raw;
  ca_session_server_t* server = c->server;
  frm_store_reply_payload_t* rp = c->rp;

  if (ATOMIC_LOAD(&server->dying) == 0) {
    _ca_store_reply_route(server, rp);
  } else {
    _ca_store_reply_retire(server, rp);
  }
  _ca_closure_done(server);
  free(c);
}

/* --- the store notice's routing (loop thread) ------------------------------ */

static void _ca_store_notice_route(ca_session_server_t* server,
                                   frm_store_notice_t* np) {
  for (ca_session_sub_t* sub = server->subs; sub != NULL; sub = sub->next) {
    if (!_ca_sid_covers(sub->sid, np->sid_path)) continue;
    if (sub->live == 0) {
      /* The replay is still streaming: hold the record (it forwards after
         the flush, deduplicated by last_seq). */
      _ca_sub_hold(sub, np->seq, np->record_json);
    } else if (np->seq > sub->last_seq) {
      _ca_send_events_record(sub->iface, sub->req_id, sub->op,
                             np->sid_path, np->seq, np->record_json);
      sub->last_seq = np->seq;
    }
  }
}

static void _ca_notice_run(void* raw) {
  ca_notice_t* c = (ca_notice_t*)raw;
  ca_session_server_t* server = c->server;
  frm_store_notice_t* np = c->np;

  if (ATOMIC_LOAD(&server->dying) == 0) {
    _ca_store_notice_route(server, np);
  }
  /* Going down: the destroy purge handles the readers; the notice is only
     data and dies here. */
  frm_store_notice_destroy(np);
  _ca_closure_done(server);
  free(c);
}

/* --- the server actor's behavior (a pool worker's dispatch — µs-scale; it
   touches NOTHING of the server's state, only re-marshals onto the loop
   thread) ------------------------------------------------------------------ */

static void _ca_server_behavior(void* state, message_t* msg) {
  ca_session_server_t* server = (ca_session_server_t*)state;

  if (msg == NULL) return;
  switch (msg->type) {
    case FRM_STORE_REPLY: {
      frm_store_reply_payload_t* rp = (frm_store_reply_payload_t*)msg->payload;
      msg->payload = NULL;
      if (rp == NULL) {
        log_error("ca: FRM_STORE_REPLY with no payload — dropped loud");
        break;
      }
      ca_reply_t* c = (ca_reply_t*)get_clear_memory(sizeof(*c));
      c->server = server;
      c->rp = rp;
      if (_ca_closure_arm(server, c, _ca_reply_run) != 0) {
        frm_store_reply_payload_destroy(rp);
        free(c);
      }
      break;
    }
    case FRM_STORE_NOTICE: {
      frm_store_notice_t* np = (frm_store_notice_t*)msg->payload;
      msg->payload = NULL;
      if (np == NULL) {
        log_error("ca: FRM_STORE_NOTICE with no payload — dropped loud");
        break;
      }
      ca_notice_t* c = (ca_notice_t*)get_clear_memory(sizeof(*c));
      c->server = server;
      c->np = np;
      if (_ca_closure_arm(server, c, _ca_notice_run) != 0) {
        frm_store_notice_destroy(np);
        free(c);
      }
      break;
    }
    default:
      /* Store messages LEAVE via the behaviors; anything else arriving at
         the server's actor is a routing bug — the standing loud drop. */
      log_error("ca: a foreign message (type %u) arrived at the session "
                "server's actor — dropped loud", (unsigned)msg->type);
      if (msg->payload != NULL && msg->payload_destroy != NULL) {
        msg->payload_destroy(msg->payload);
      }
      break;
  }
}

/* --- the closed hook's purge (loop thread) --------------------------------- */

static void _ca_conn_closed_purge(ca_session_server_t* server,
                                  ca_session_conn_t* iface) {
  /* The subscriptions: remove every entry of THIS connection; unwatch every
     sid whose LAST subscriber was one of them. */
  ca_session_sub_t** sp = &server->subs;
  while (*sp != NULL) {
    ca_session_sub_t* sub = *sp;
    if (sub->conn != iface->conn) {
      sp = &sub->next;
      continue;
    }
    *sp = sub->next;
    if (!_ca_sid_subscribed(server, sub->sid)) {
      _ca_post_unwatch(server, sub->sid);
    }
    _ca_sub_free(sub);
    _ca_conn_unref(iface);   /* the entry's ref */
  }
  /* The pending round trips: their replies route to nothing later — the
     entries die here (the late reply becomes the router's loud drop). */
  ca_session_pending_t** pp = &server->pending;
  while (*pp != NULL) {
    ca_session_pending_t* pe = *pp;
    if (pe->conn != iface->conn) {
      pp = &pe->next;
      continue;
    }
    *pp = pe->next;
    _ca_pending_free(pe);
  }
}

static void _ca_closed_run(void* raw) {
  ca_closed_t* c = (ca_closed_t*)raw;
  ca_session_server_t* server = c->server;

  if (ATOMIC_LOAD(&server->dying) == 0) {
    _ca_conn_closed_purge(server, c->iface);
  }
  _ca_conn_unref(c->iface);   /* the closure's own ref */
  _ca_closure_done(server);
  free(c);
}

/* --- the four request paths (loop thread, one per wire type) --------------- */

static void _ca_on_prompt(ca_session_server_t* server, void* payload_raw,
                          ca_session_conn_t* iface) {
  ca_prompt_request_t* req = (ca_prompt_request_t*)payload_raw;
  uint64_t req_id = req->req_id;

  if (req->text == NULL) {
    /* A hand-crafted frame's refusal (the wire's decode refuses an absent
       text before this — the handler repeats the rule: never trust the
       peer). */
    log_error("ca: a prompt request carries no text — refused loud");
    _ca_send_error(iface, req_id, 1, "the prompt carries no text");
    return;
  }
  if (req->sid == NULL) {
    /* CREATE + START (spec §3). The create is legal ON THE LOOP THREAD:
       the birth batch is frame_create's documented direct write (no store
       round trip, no await — WaveDB's sync writes are internally
       serialized), and frame_start queues ONE continuation onto the frame's
       pool-attached mailbox. */
    frame_t* f = frame_create(server->root, NULL, req->text, &server->cfg);
    if (f == NULL) {
      log_error("ca: the prompt goal refused to create a frame (the frame "
                "layer's log carries the reason)");
      _ca_send_error(iface, req_id, 1,
                     "the session refused to start (see the server's log)");
      return;
    }
    if (server->shared_backend != NULL) {
      frame_set_model_backend(f, server->shared_backend);
    }
    _ca_reg_add(server, strdup(frame_sid(f)), f);
    if (frame_start(f) != 0) {
      /* A fresh frame's start cannot refuse; when it does anyway, tear the
         half-made session down and refuse loud — never a half-answer. */
      char sid_copy[CA_WIRE_SID_MAX];
      snprintf(sid_copy, sizeof(sid_copy), "%s", frame_sid(f));
      frame_destroy(_ca_reg_take(server, sid_copy));
      _ca_send_error(iface, req_id, 1, "the session '%s' refused to start",
                     sid_copy);
      return;
    }
    ca_prompt_response_t* res =
        (ca_prompt_response_t*)get_clear_memory(sizeof(*res));
    res->req_id = req_id;
    res->status = 0;
    res->sid = strdup(frame_sid(f));
    (void)_ca_send(iface, CA_PROMPT_RESPONSE, res);
    return;
  }
  /* STEER: the standing FIFO over the wire — POSTED (see _frame_steer_post's
     contract): the response answers QUEUED, not committed; the store's FIFO
     commits it ahead of anything the frame posts after (durable input
     before model work is a store fact). */
  frame_t* f = _ca_reg_find(server, req->sid);
  if (f == NULL) {
    log_error("ca: a steer names unknown session '%s' — refused loud",
              req->sid);
    _ca_send_error(iface, req_id, 2, "session unknown '%s'", req->sid);
    return;
  }
  if (frame_is_done(f) != 0) {
    log_error("ca: a steer arrived for the done session '%s' — refused "
              "loud (a done frame accepts no steer)", req->sid);
    _ca_send_error(iface, req_id, 2, "the session '%s' is done", req->sid);
    return;
  }
  if (_frame_steer_post(f, "user", req->text) != 0) {
    _ca_send_error(iface, req_id, 1, "the steer refused to post");
    return;
  }
  ca_prompt_response_t* res =
      (ca_prompt_response_t*)get_clear_memory(sizeof(*res));
  res->req_id = req_id;
  res->status = 0;
  res->sid = (char*)get_memory(1);
  res->sid[0] = '\0';   /* "" — the steer's empty-sid shape (the wire's) */
  (void)_ca_send(iface, CA_PROMPT_RESPONSE, res);
}

static int _ca_post_events_scan(ca_session_server_t* server,
                                ca_session_pending_t* pe,
                                uint64_t from_seq) {
  /* The events range's ABSOLUTE ROOT-LEVEL bounds (frame.c's scan
     discipline — WaveDB's subtree bounded scans are broken in both
     directions): from_seq = RESUME-AT semantics, so the replay's first
     record is from_seq + 1 (the event keys are %-020 padded, so the
     lexicographic and numeric orders agree). The bounds' texts are consumed
     by the store's bound compose (the payload's OWNED members). */
  char start_key[24];
  frm_store_scan_payload_t* sp;

  if (from_seq == UINT64_MAX) {
    /* The cursor's +1 would wrap: refuse loud rather than replay from
       seq 0. */
    log_error("ca: an events cursor at UINT64_MAX cannot resume — refused "
              "loud");
    return -1;
  }
  snprintf(start_key, sizeof(start_key), "%020llu",
           (unsigned long long)(from_seq + 1));
  size_t start_len = strlen(pe->sid) + strlen("/events/") + strlen(start_key) + 1;
  size_t end_len = strlen(pe->sid) + strlen("/events0") + 1;
  char* start = (char*)get_memory(start_len);
  char* end = (char*)get_memory(end_len);
  snprintf(start, start_len, "%s/events/%s", pe->sid, start_key);
  snprintf(end, end_len, "%s/events0", pe->sid);

  sp = (frm_store_scan_payload_t*)get_clear_memory(sizeof(*sp));
  sp->start = start;
  sp->end = end;
  sp->limit = SA_FRAME_DEBUG_MAX_EVENTS;
  sp->reply_to = &server->actor;   /* the reply routes at the SERVER's actor */
  sp->corr = pe->corr;
  _frame_post(wave_db_store_actor(server->root), (uint32_t)FRM_STORE_SCAN, sp,
              frm_store_scan_payload_destroy, "events replay scan");
  return 0;
}

static void _ca_on_events(ca_session_server_t* server, void* payload_raw,
                          ca_session_conn_t* iface) {
  ca_events_request_t* req = (ca_events_request_t*)payload_raw;
  uint64_t req_id = req->req_id;

  if (req->sid == NULL) {
    /* CA2's observation pinned at the handler too: a NULL-sid events
       request cannot encode validly, but a hand-crafted one must STILL be
       refused — the wire never trusts its peer, and neither does the
       handler. */
    log_error("ca: an events request carries no session — refused loud");
    _ca_send_error(iface, req_id, 1, "the events request carries no session");
    return;
  }
  if (req->op == CA_EVENTS_UNSUBSCRIBE) {
    ca_session_sub_t** sp = &server->subs;
    uint8_t found = 0;
    while (*sp != NULL) {
      ca_session_sub_t* sub = *sp;
      if (sub->conn != iface->conn || strcmp(sub->sid, req->sid) != 0) {
        sp = &sub->next;
        continue;
      }
      *sp = sub->next;
      _ca_sub_free(sub);
      _ca_conn_unref(iface);   /* the entry's ref */
      found = 1;
    }
    if (found == 0) {
      /* The no-op unsub refuses loud (the tests pin it: an unsubscribe of
         nothing is the peer's mistake, never a silent success). */
      log_error("ca: an unsubscribe for '%s' matches no subscription — "
                "refused loud", req->sid);
      _ca_send_error(iface, req_id, 1,
                     "no events subscription to end for '%s'", req->sid);
      return;
    }
    if (!_ca_sid_subscribed(server, req->sid)) {
      _ca_post_unwatch(server, req->sid);   /* the LAST subscriber left */
    }
    /* The unsubscribe's terminal marker: seq 0 + the op echoed — the same
       signal shape the live-tail channels carry. */
    _ca_send_events_record(iface, req_id, CA_EVENTS_UNSUBSCRIBE, req->sid, 0,
                           NULL);
    return;
  }
  if (req->op != CA_EVENTS_REPLAY_THEN_LIVE && req->op != CA_EVENTS_LIVE_ONLY) {
    log_error("ca: an events request's op %u is outside the closed set — "
              "refused loud", (unsigned)req->op);
    _ca_send_error(iface, req_id, 1, "the events op %u is unknown",
                   (unsigned)req->op);
    return;
  }
  frame_t* f = _ca_reg_find(server, req->sid);
  if (f == NULL) {
    /* The registry is the session's in-memory truth (the recorded
       restart/reconcile limit): a store-only session is refused loud —
       the unknown-sid test's shape. */
    log_error("ca: an events request names unknown session '%s' — refused "
              "loud", req->sid);
    _ca_send_error(iface, req_id, 2, "session unknown '%s'", req->sid);
    return;
  }

  /* The watch joins FIRST — before the scan reaches the store's dispatch —
     so the store's FIFO anchors the delivery: a record committed after the
     watch joins but before the scan reads arrives as an in-flight notice
     (held, flushed post-replay) instead of falling into a delivery gap.
     The scan composes only after the watch is posted. */
  _ca_post_watch(server, req->sid);
  ca_session_sub_t* sub = (ca_session_sub_t*)get_clear_memory(sizeof(*sub));
  sub->req_id = req->req_id;
  sub->op = req->op;
  sub->sid = strdup(req->sid);
  sub->conn = iface->conn;
  sub->iface = iface;
  sub->live = (uint8_t)(req->op == CA_EVENTS_LIVE_ONLY);
  sub->last_seq = req->from_seq;
  sub->next = server->subs;
  server->subs = sub;
  _ca_conn_ref(iface);

  if (req->op == CA_EVENTS_LIVE_ONLY) {
    /* The live marker NOW (no replay precedes it): from_seq's resume rule
       rides last_seq — records at/below the cursor never forward. */
    _ca_send_events_record(iface, req->req_id, req->op, req->sid, 0, NULL);
    return;
  }
  ca_session_pending_t* pe =
      _ca_pending_add(server, CA_PENDING_EVENTS_SCAN, req->req_id, req->op,
                      req->sid, iface);
  if (_ca_post_events_scan(server, pe, req->from_seq) != 0) {
    /* The cursor was past its end: the pending entry dies here, the sub
       dies with it (a replay that can never leave is not a subscription). */
    for (ca_session_sub_t** sp = &server->subs; *sp != NULL; sp = &(*sp)->next) {
      if (*sp == sub) {
        *sp = sub->next;
        break;
      }
    }
    if (!_ca_sid_subscribed(server, sub->sid)) {
      _ca_post_unwatch(server, sub->sid);
    }
    _ca_sub_free(sub);
    _ca_conn_unref(iface);
    /* Remove the pending entry (its scan never posts). */
    for (ca_session_pending_t** pp = &server->pending; *pp != NULL;
         pp = &(*pp)->next) {
      if (*pp == pe) {
        *pp = pe->next;
        _ca_pending_free(pe);
        break;
      }
    }
    _ca_send_error(iface, req_id, 1, "the events cursor is past the end");
  }
}

static void _ca_on_interrupt(ca_session_server_t* server, void* payload_raw,
                             ca_session_conn_t* iface) {
  ca_interrupt_request_t* req = (ca_interrupt_request_t*)payload_raw;
  uint64_t req_id = req->req_id;

  if (req->sid == NULL) {
    log_error("ca: an interrupt request carries no session — refused loud");
    _ca_send_error(iface, req_id, 1, "the interrupt carries no session");
    return;
  }
  frame_t* f = _ca_reg_find(server, req->sid);
  if (f == NULL) {
    log_error("ca: an interrupt names unknown session '%s' — refused loud",
              req->sid);
    _ca_send_error(iface, req_id, 2, "session unknown '%s'", req->sid);
    return;
  }
  if (frame_is_done(f) != 0) {
    log_error("ca: an interrupt arrived for the done session '%s' — "
              "refused loud (a done frame has nothing to interrupt)",
              req->sid);
    _ca_send_error(iface, req_id, 2, "the session '%s' is done", req->sid);
    return;
  }
  /* frame_interrupt is the PUBLIC entry (the surface slice's contract:
     legal from any thread — ONE FRM_INT into the mailbox). */
  frame_interrupt(f);
  ca_interrupt_response_t* res =
      (ca_interrupt_response_t*)get_clear_memory(sizeof(*res));
  res->req_id = req_id;
  res->status = 0;   /* the interrupt POSTED into the mailbox (the contract) */
  (void)_ca_send(iface, CA_INTERRUPT_RESPONSE, res);
}

static void _ca_on_sessions(ca_session_server_t* server, void* payload_raw,
                            ca_session_conn_t* iface) {
  ca_sessions_request_t* req = (ca_sessions_request_t*)payload_raw;

  frm_store_sessions_payload_t* lp =
      (frm_store_sessions_payload_t*)get_clear_memory(sizeof(*lp));
  lp->corr = _ca_pending_add(server, CA_PENDING_SESSIONS, req->req_id, 0,
                             NULL, iface)->corr;
  lp->reply_to = &server->actor;   /* the reply's ROUTER is the server actor */
  _frame_post(wave_db_store_actor(server->root),
              (uint32_t)FRM_STORE_LIST_SESSIONS, lp,
              frm_store_sessions_payload_destroy, "sessions listing");
}

/* --- one work closure: the type switch (the wire's decode already ran at
   the transport; the transport never learns the types' semantics) ---------- */

static void _ca_work_run(void* raw) {
  ca_work_t* w = (ca_work_t*)raw;
  ca_session_server_t* server = w->server;

  if (ATOMIC_LOAD(&server->dying) != 0) {
    /* The server is going down: the request dies with it (its conn ref is
       the closure's own). */
    ca_wire_payload_destroy(w->type, w->payload);
    _ca_conn_unref(w->iface);
    _ca_closure_done(server);
    free(w);
    return;
  }
  switch (w->type) {
    case CA_PROMPT_REQUEST:
      _ca_on_prompt(server, w->payload, w->iface);
      break;
    case CA_EVENTS_REQUEST:
      _ca_on_events(server, w->payload, w->iface);
      break;
    case CA_INTERRUPT_REQUEST:
      _ca_on_interrupt(server, w->payload, w->iface);
      break;
    case CA_SESSIONS_REQUEST:
      _ca_on_sessions(server, w->payload, w->iface);
      break;
    default:
      /* The wire's decode refuses unknown types first; this is the closed
         vocabulary's defensive floor (the payload dies at this closure's
         tail like every other request's). */
      log_error("ca: a request of unknown type %llu arrived at the handler "
                "switch — dropped loud", (unsigned long long)w->type);
      break;
  }
  /* The request payload dies HERE (uniformly — the handlers copy the texts
     they compose from; the struct's heap fields and the struct itself are
     the closure's). */
  ca_wire_payload_destroy(w->type, w->payload);
  _ca_conn_unref(w->iface);
  _ca_closure_done(server);
  free(w);
}

/* --- the public surface ----------------------------------------------------- */

ca_session_server_t* ca_session_server_create(wave_database_root_t* root,
                                              scheduler_pool_t* pool,
                                              streams_loop_thread_t* loop,
                                              const frame_config_t* frame_cfg,
                                              model_backend_t* shared_backend) {
  if (root == NULL || pool == NULL || loop == NULL || frame_cfg == NULL) {
    log_error("ca_session_server_create: a NULL root/pool/loop/frame_cfg — "
              "refused loud");
    return NULL;
  }
  ca_session_server_t* server =
      (ca_session_server_t*)get_clear_memory(sizeof(*server));
  server->root = root;
  server->pool = pool;
  server->loop = loop;
  server->shared_backend = shared_backend;
  server->cfg = *frame_cfg;
  server->cfg.pool = pool;   /* the API's frames ride the API's pool: the
                                template's pool member is overwritten */
  ATOMIC_STORE(&server->dying, 0);
  ATOMIC_STORE(&server->closures_live, 0);
  /* The server's actor attaches to the pool: the store's replies and
     notices dispatch on pool workers, and their closures re-marshal onto
     the loop thread (handlers.h's threading model). */
  actor_init(&server->actor, server, _ca_server_behavior, pool);
  return server;
}

int ca_session_handle(ca_session_server_t* server, uint64_t type, void* payload,
                      ca_session_conn_t* connection) {
  if (server == NULL || connection == NULL || connection->send == NULL ||
      connection->conn == NULL) {
    ca_wire_payload_destroy(type, payload);
    log_error("ca_session_handle: a frame arrived on an unusable "
              "connection — refused loud, payload destroyed");
    return -1;
  }
  if (ATOMIC_LOAD(&server->dying) != 0) {
    ca_wire_payload_destroy(type, payload);
    log_error("ca_session_handle: the server is going down — the frame is "
              "refused loud");
    return -1;
  }
  /* The conn ref goes FIRST (the closure and every entry it may create keep
     the connection object alive), the dying recheck closes the race (the
     file head), and the enqueue is last. */
  _ca_conn_ref(connection);
  if (ATOMIC_LOAD(&server->dying) != 0) {
    _ca_conn_unref(connection);
    ca_wire_payload_destroy(type, payload);
    return -1;
  }
  ca_work_t* w = (ca_work_t*)get_clear_memory(sizeof(*w));
  w->server = server;
  w->type = type;
  w->payload = payload;   /* transferred */
  w->iface = connection;
  if (_ca_closure_arm(server, w, _ca_work_run) != 0) {
    /* The loop is gone (or the server began dying): the payload dies HERE
       and the caller drops the frame. */
    ca_wire_payload_destroy(type, payload);
    _ca_conn_unref(connection);
    free(w);
    log_error("ca_session_handle: the loop is gone — the frame is dropped "
              "loud");
    return -1;
  }
  return 0;   /* ACCEPTED: the responses post on the loop thread (async) */
}

void ca_session_conn_closed(ca_session_server_t* server,
                            ca_session_conn_t* connection) {
  if (server == NULL || connection == NULL) return;
  if (ATOMIC_LOAD(&server->dying) != 0) return;   /* the destroy purges all */
  _ca_conn_ref(connection);
  if (ATOMIC_LOAD(&server->dying) != 0) {
    _ca_conn_unref(connection);
    return;
  }
  ca_closed_t* c = (ca_closed_t*)get_clear_memory(sizeof(*c));
  c->server = server;
  c->iface = connection;
  if (_ca_closure_arm(server, c, _ca_closed_run) != 0) {
    _ca_conn_unref(connection);
    free(c);
  }
}

frame_t* ca_session_server_frame(ca_session_server_t* server,
                                 const char* sid) {
  if (server == NULL || sid == NULL) return NULL;
  return _ca_reg_find(server, sid);
}

void ca_session_server_destroy(ca_session_server_t* server) {
  if (server == NULL) return;
  ATOMIC_STORE(&server->dying, 1);
  /* The drain (bounded; loud on break): every enqueued closure must finish
     and every pending store round trip must receive its reply — the store
     keeps nothing the server awaits. A break means the loop died mid-flight
     (its own destroy logs the stranded FIFO); the recorded in-flight seam
     beyond that is the embedder's teardown order (handlers.h's note). */
  uint64_t start = platform_monotonic_ns();
  uint64_t deadline = start + (uint64_t)_CA_DRAIN_WAIT_MS * 1000000ULL;
  while ((ATOMIC_LOAD(&server->closures_live) != 0 || server->pending != NULL) &&
         platform_monotonic_ns() < deadline) {
    platform_sleep_ms(1);
  }
  if (ATOMIC_LOAD(&server->closures_live) != 0 || server->pending != NULL) {
    log_error("ca_session_server_destroy: the drain broke its %d ms bound "
              "(closures in flight or store round trips unanswered) — "
              "tearing down loud; any late deliverable refuses or drops",
              (int)_CA_DRAIN_WAIT_MS);
  }
  /* The subscriptions: each sid still watched unwatchs (the store's list
     survives this server — one unwatch per SUB, idempotent and a no-op when
     the store's watch is gone); every entry's conn ref releases (the
     connection objects the server pinned during its life must all go). */
  {
    ca_session_sub_t* sub = server->subs;
    while (sub != NULL) {
      ca_session_sub_t* next = sub->next;
      ca_session_conn_t* iface = sub->iface;   /* read BEFORE the free */
      _ca_post_unwatch(server, sub->sid);
      _ca_sub_free(sub);
      _ca_conn_unref(iface);
      sub = next;
    }
    server->subs = NULL;
  }
  /* The pending entries (a post-break drain's leftovers): their refs
     release; the store holds their replies' targets — the actor destroy
     below flags the mailbox, so a late reply's post refuses and frees
     itself (the standing dead-target shape). */
  {
    ca_session_pending_t* pe = server->pending;
    while (pe != NULL) {
      ca_session_pending_t* next = pe->next;
      _ca_pending_free(pe);
      pe = next;
    }
    server->pending = NULL;
  }
  /* The frames: the server OWNS every registry record (the recorded holding
     rule — a done frame's record stays until here too). The embedder
     quiesces its pool FIRST (the frame layer's documented teardown order:
     stop the pool, then frame_destroy) when frames may still be running. */
  {
    ca_session_reg_t* reg = server->regs;
    while (reg != NULL) {
      ca_session_reg_t* next = reg->next;
      frame_destroy(reg->f);
      free(reg->sid);
      free(reg);
      reg = next;
    }
    server->regs = NULL;
  }
  /* The server's actor dies LAST: its mailbox may still hold bounced
     replies/notices (their payloads die in the drain). */
  actor_destroy(&server->actor);
  free(server);
}

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */