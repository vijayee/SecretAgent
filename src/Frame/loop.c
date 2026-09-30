//
// Created by victor on 9/29/26.
//
// The turn-loop engine (Task 10) — the piece that makes the stack
// model-driven. One frame, one loop, one thread: the threading model (the
// loop runs on the CALLER's thread; the frame actor is inline, NULL pool, and
// pumped by the loop itself; the frame's own pyrt worker is the only other
// thread in the picture and talks to the loop through the frame mailbox) is
// documented on frame_internal.h.
//
// THE TURN, in order:
//   1. derive — the loop rebuilds the model's ENTIRE view from the frame's
//      stored event log on every turn (stateless; nothing accumulates in
//      hidden loop memory): msg.append events become {role, content}
//      messages; the ctx snapshot (state.remember events replayed as
//      newest-value-wins) and one-line child reports (frame.report events)
//      go into the system prompt; cell results since the newest msg.append
//      ride along as tool-result-equivalent user messages. The projection
//      reads the store through frame_debug_events — already the bounded,
//      root-level, ABSOLUTE-bounds reverse scan this module requires
//      (database_subtree_scan_* is broken in both directions and never
//      touched here). Nothing materializes to disk: the store IS the source.
//   2. complete — one model call: the single `execute` tool (tools=NULL gets
//      model.c's canned tool), the backend's raw_out passed NULL (it is the
//      documented NULL-tolerant out-param; the loop does not want the raw
//      body).
//   3. tool path — EV_CELL_RUN event (one root batch), the code handed to
//      the frame actor as FRM_CELL_EXECUTE (frame_dispatch, synchronous on
//      the loop thread), then a bounded pump-and-wait for the PYRT_RESULT
//      that completes the frame's pending-cell slot; the frame's own
//      behavior writes the paired EV_CELL_RESULT event in the same dispatch
//      that completes the slot. A FAILED cell does not end the loop: the
//      next derive carries its status-1 text (traceback) back as a user
//      message and the model self-corrects.
//   4. content path — msg.append assistant event. TOP frames end here
//      (status flips to done); CHILD frames keep their "running" status —
//      REPORT SEMANTICS (the decided design): a report marks the REPORTING
//      frame done at every depth; a child additionally binds the report
//      event into the parent's log, a top frame has no parent log to bind
//      into and reports into its OWN log instead. Children therefore end
//      exclusively via the report verb (actor.report within a cell); a child
//      whose loop ends on a no-tool-call turn is simply left "running" —
//      dangling-join stragglers are the tree-slice's business, not the
//      loop's.
//
// MESSAGE-ARRAY CONSTRUCTION SIMPLIFICATION (documented): NO
// tool_call/tool-role history is reconstructed. Each turn sends a FRESH
// plain-roles array (system + projected msg.appends + trailing cell results
// as user messages) built from the event projection. Cell CODE is not
// re-quoted into context (cell.run events are skipped by the projection) —
// the result text carries the outcome, and history-by-projection is where
// token bloat would otherwise creep back. One helper builds the whole array.
//
// ERRORS: model errors retry ONCE (the same derived array — the store has
// not moved) and end the loop nonzero with EV_CONTROL events ("model-error",
// then "model-error-final"); derive failure, a missing backend, a missing
// python runtime, and a cell still in flight at the wait deadline each fail
// loud with their own control kind. The loop NEVER spins and never fails
// silently.
//
// Cap: frame_set_loop_turn_cap (runtime state on the frame; 0 = the
// SA_LOOP_MAX_TURNS default of 64) bounds model turns issued per run — fail
// loud with control "turn-limit" at the cap.

#include "loop.h"
#include "frame_internal.h"
#include "frame_messages.h"
#include "model.h"
#include "../Actor/actor.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include "../Util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SA_HAS_WDB

/* ---------------------------------------------------------------------------
 * Context caps (WHY HERE: the derive is the ONE place token bloat creeps in —
 * every cap is a byte bound the projection can never exceed, and each is
 * deliberately smaller than any sane model context so a runaway replay
 * truncates instead of bloating a request).
 * ------------------------------------------------------------------------- */

/* Per-message cap on ANY projected text (msg.append content, cell result
   text). 4000 chars is long enough for real prose, short enough that a
   512-record replay stays bounded no matter what the model wrote. */
#define SA_LOOP_MSG_CAP 4000

/* Bounded ctx snapshot: at most this many distinct keys reach the system
   prompt (NEW keys beyond the cap are dropped; already-present keys keep
   updating to their newest value), and each value renders at most this many
   chars. The snapshot comes from state.remember events, so it includes
   local/ writes too — the projection shows the model the effect history of
   BOTH remember layers (they share the state.remember event; the ctx/local
   split is a recall-shadowing detail the model does not need). */
#define SA_LOOP_SNAPSHOT_MAX_KEYS 24
#define SA_LOOP_SNAPSHOT_VALUE_CAP 500

/* One-line per-child report summaries: at most this many reports, each
   flattened to its first line, each line at most this many chars. */
#define SA_LOOP_REPORTS_MAX 8
#define SA_LOOP_REPORT_LINE_CAP 300

/* Trailing cell-result block: at most this many results ride along after the
   newest msg.append (a model that keeps calling tools without ever writing a
   message stays bounded; older results roll off). */
#define SA_LOOP_RESULTS_BLOCK_MAX 8

/* Bounded pump-and-wait for one cell (the loop's timeout while the pyrt
   worker runs it; a first-time interpreter boot lives well inside this). */
#ifndef SA_LOOP_CELL_WAIT_MS
#define SA_LOOP_CELL_WAIT_MS 60000
#endif

/* The one turn instruction (kept SHORT — it rides every turn). */
static const char SA_LOOP_INSTRUCTION[] =
    "You drive one frame of an agent session. Your only tool is `execute`: "
    "its `code` argument is ONE python cell run in this frame's interpreter "
    "(one shared namespace per frame). Inside cells the injected `actor` "
    "module provides the verbs: actor.remember(key, value), "
    "actor.recall(key), actor.spawn(goal, context=None), actor.report(value).\n"
    "Finish the frame by calling actor.report inside your last cell (a "
    "completion declaration), or simply by answering WITHOUT a tool call.\n";

/* ---------------------------------------------------------------------------
 * Small text helpers. get_memory aborts on OOM (style guide), so these
 * allocations carry no failure branches.
 * ------------------------------------------------------------------------- */

/* Heap copy truncated to `cap` bytes, truncation-safe (the caller frees). */
static char* _loop_trunc(const char* s, size_t cap) {
  size_t n = s ? strlen(s) : 0;
  if (n > cap) n = cap;
  char* out = get_memory(n + 1);
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

/* The FIRST line of s (up to \r/\n), truncated to cap — the one-line shape
   child summaries keep. */
static char* _loop_one_line(const char* s, size_t cap) {
  if (s == NULL) s = "";
  size_t n = strcspn(s, "\r\n");
  if (n > cap) n = cap;
  char* out = get_memory(n + 1);
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

/* Minimal growable string buffer for the system prompt. */
typedef struct loop_sb_t {
  char* s;
  size_t len;
  size_t cap;
} loop_sb_t;

static void _loop_sb_init(loop_sb_t* b) {
  b->cap = 256;
  b->len = 0;
  b->s = get_memory(b->cap);
  b->s[0] = '\0';
}

static void _loop_sb_reserve(loop_sb_t* b, size_t extra) {
  size_t need = b->len + extra + 1;
  if (need <= b->cap) return;
  size_t ncap = b->cap;
  while (ncap < need) ncap *= 2;
  char* ns = get_memory(ncap);
  memcpy(ns, b->s, b->len + 1);
  free(b->s);
  b->s = ns;
  b->cap = ncap;
}

static void _loop_sb_puts(loop_sb_t* b, const char* s) {
  size_t n = strlen(s);
  _loop_sb_reserve(b, n);
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = '\0';
}

static void _loop_sb_putf(loop_sb_t* b, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int need = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (need < 0) return;
  _loop_sb_reserve(b, (size_t)need);
  va_start(args, fmt);
  vsnprintf(b->s + b->len, (size_t)need + 1, fmt, args);
  va_end(args);
  b->len += (size_t)need;
}

/* ---------------------------------------------------------------------------
 * Derivation. The event record shape (frame_messages.h): {"seq","type",
 * "frame","corr","at","cause","payload"} as ONE root-level scan bounded to
 * the newest 512 records, ascending — the projection runs two passes over
 * that parsed array: pass A fills the system-prompt material (snapshot +
 * report lines), pass B projects the message stream in order.
 * ------------------------------------------------------------------------- */

typedef struct loop_snap_t {
  const char* key;        /* borrowed from the events DOM */
  const json_value_t* value;   /* borrowed payload value (any JSON type) */
} loop_snap_t;

typedef struct loop_report_t {
  const char* child_sid;  /* borrowed */
  const char* text;       /* borrowed */
} loop_report_t;

typedef struct loop_result_t {
  const char* text;       /* borrowed (NULL allowed) */
  int status;
} loop_result_t;

/* The ctx snapshot: state.remember events replayed NEWEST-VALUE-WINS into a
   fixed bounded table. (state/local/ and state/ctx/ writes both emit this
   event — see frame.c's _frame_remember_variant.) */
static void _loop_snap_apply(loop_snap_t* snaps, size_t* nsnaps,
                             const char* key, const json_value_t* value) {
  for (size_t i = 0; i < *nsnaps; i++) {
    if (strcmp(snaps[i].key, key) == 0) {
      snaps[i].value = value;
      return;
    }
  }
  if (*nsnaps < SA_LOOP_SNAPSHOT_MAX_KEYS) {
    snaps[*nsnaps].key = key;
    snaps[*nsnaps].value = value;
    (*nsnaps)++;
  }
  /* Full table: new keys are DROPPED (bounded; existing keys keep updating). */
}

/* Rolling ring: oldest dropped (freed) when full. Lines are heap copies —
   unlike the borrowed snapshot/reports, the result lines are COMPOSED
   strings, freed on flush/drop. */
static void _loop_result_push(char** ring, size_t* n, char* line) {
  if (*n == SA_LOOP_RESULTS_BLOCK_MAX) {
    free(ring[0]);
    for (size_t i = 1; i < SA_LOOP_RESULTS_BLOCK_MAX; i++) ring[i - 1] = ring[i];
    (*n)--;
  }
  ring[(*n)++] = line;
}

/* Flush the pending result block as tool-result-equivalent user messages. */
static void _loop_flush_results(json_value_t* out, char** ring, size_t* n) {
  for (size_t i = 0; i < *n; i++) {
    json_value_t* um = json_new_object();
    json_object_set(um, "role", json_new_string("user"));
    json_object_set(um, "content", json_new_string(ring[i]));
    free(ring[i]);
    ring[i] = NULL;
    json_array_append(out, um);
  }
  *n = 0;
}

static char* _loop_system_content(frame_t* f, const loop_snap_t* snaps, size_t nsnaps,
                                  const loop_report_t* reports, size_t nreports) {
  loop_sb_t sb;
  _loop_sb_init(&sb);
  _loop_sb_puts(&sb, SA_LOOP_INSTRUCTION);

  const char* goal = _frame_goal(f);
  if (goal != NULL) {
    char* g = _loop_trunc(goal, SA_LOOP_MSG_CAP);
    _loop_sb_putf(&sb, "Goal: %s\n", g);
    free(g);
  }

  if (nsnaps > 0) {
    _loop_sb_puts(&sb, "\nFrame state (newest value wins; JSON verbatim):\n");
    for (size_t i = 0; i < nsnaps; i++) {
      char* v = json_serialize(snaps[i].value);
      char* vt = (v != NULL) ? _loop_trunc(v, SA_LOOP_SNAPSHOT_VALUE_CAP) : NULL;
      _loop_sb_putf(&sb, "- %s = %s\n", snaps[i].key,
                    (vt != NULL && vt[0] != '\0') ? vt : "(unserializable)");
      free(v);
      free(vt);
    }
  }

  if (nreports > 0) {
    _loop_sb_puts(&sb, "\nChild reports:\n");
    for (size_t i = 0; i < nreports; i++) {
      char* line = _loop_one_line(reports[i].text, SA_LOOP_REPORT_LINE_CAP);
      _loop_sb_putf(&sb, "- %s: %s\n", reports[i].child_sid, line);
      free(line);
    }
  }
  return sb.s;   /* freed by the caller */
}

/* ONE helper builds the whole messages array (loop.c's documented message
   construction). Returns a fresh JSON array value the loop destroys after
   the model call; NULL on derivation failure (the caller logs a control
   event). */
static json_value_t* _loop_derive_context(frame_t* f) {
  char* events_json = frame_debug_events(f);
  if (events_json == NULL) return NULL;
  char* perr = NULL;
  json_value_t* events = json_parse(events_json, strlen(events_json), &perr);
  free(events_json);
  if (perr != NULL) free(perr);
  if (events == NULL || json_type(events) != JSON_ARRAY) {
    json_value_destroy(events);
    return NULL;
  }

  loop_snap_t snaps[SA_LOOP_SNAPSHOT_MAX_KEYS];
  size_t nsnaps = 0;
  loop_report_t reports[SA_LOOP_REPORTS_MAX];
  size_t nreports = 0;
  char* result_ring[SA_LOOP_RESULTS_BLOCK_MAX];
  size_t nresults = 0;
  for (size_t i = 0; i < SA_LOOP_RESULTS_BLOCK_MAX; i++) result_ring[i] = NULL;

  /* Pass A: system-prompt material. */
  size_t n_events = json_size(events);
  for (size_t i = 0; i < n_events; i++) {
    json_value_t* rec = json_at(events, i);
    if (rec == NULL) continue;
    json_value_t* type_v = json_get(rec, "type");
    json_value_t* payload = json_get(rec, "payload");
    const char* type_name = (type_v != NULL) ? json_as_string(type_v) : "";
    if (payload == NULL) continue;
    if (strcmp(type_name, "state.remember") == 0) {
      json_value_t* key = json_get(payload, "key");
      json_value_t* value = json_get(payload, "value");
      if (key != NULL && value != NULL) {
        _loop_snap_apply(snaps, &nsnaps, json_as_string(key), value);
      }
    } else if (strcmp(type_name, "frame.report") == 0) {
      if (nreports < SA_LOOP_REPORTS_MAX) {
        json_value_t* sid = json_get(payload, "child_sid");
        json_value_t* text = json_get(payload, "text");
        reports[nreports].child_sid = (sid != NULL) ? json_as_string(sid) : "?";
        reports[nreports].text = (text != NULL) ? json_as_string(text) : "";
        nreports++;
      }
    }
  }

  json_value_t* out = json_new_array();
  if (out == NULL) {
    json_value_destroy(events);
    return NULL;
  }
  char* sys_text = _loop_system_content(f, snaps, nsnaps, reports, nreports);
  json_value_t* sys = json_new_object();
  json_object_set(sys, "role", json_new_string("system"));
  json_object_set(sys, "content", json_new_string(sys_text));
  free(sys_text);
  json_array_append(out, sys);

  /* Pass B: the message stream, in event order. */
  for (size_t i = 0; i < n_events; i++) {
    json_value_t* rec = json_at(events, i);
    if (rec == NULL) continue;
    json_value_t* type_v = json_get(rec, "type");
    json_value_t* payload = json_get(rec, "payload");
    const char* type_name = (type_v != NULL) ? json_as_string(type_v) : "";
    if (payload == NULL) continue;
    if (strcmp(type_name, "msg.append") == 0) {
      _loop_flush_results(out, result_ring, &nresults);
      json_value_t* role = json_get(payload, "role");
      json_value_t* content = json_get(payload, "content");
      if (role != NULL && content != NULL) {
        char* ct = _loop_trunc(json_as_string(content), SA_LOOP_MSG_CAP);
        json_value_t* m = json_new_object();
        json_object_set(m, "role", json_new_string(json_as_string(role)));
        json_object_set(m, "content", json_new_string(ct));
        free(ct);
        json_array_append(out, m);
      }
    } else if (strcmp(type_name, "cell.result") == 0) {
      /* Tool-result equivalent: only results since the newest msg.append are
         projected (the ring holds the block; a msg.append flushes it). */
      json_value_t* status_v = json_get(payload, "status");
      json_value_t* text_v = json_get(payload, "text");
      char status_buf[24];
      int status = (status_v != NULL) ? (int)json_as_int(status_v) : 0;
      snprintf(status_buf, sizeof(status_buf), "%d", status);
      const char* text = (text_v != NULL) ? json_as_string(text_v) : NULL;
      char* tt = _loop_trunc((text != NULL && text[0] != '\0') ? text
                                                              : "(no output)",
                             SA_LOOP_MSG_CAP);
      size_t line_len = strlen("cell result (status ): ") + strlen(status_buf) +
                        strlen(tt);
      char* line = get_memory(line_len + 1);
      snprintf(line, line_len + 1, "cell result (status %s): %s", status_buf, tt);
      free(tt);
      _loop_result_push(result_ring, &nresults, line);
    }
    /* cell.run skipped (code is not re-quoted — see the header's
       construction note); state.remember / frame.report went into pass A;
       spawn/join/control records carry no model context. */
  }
  _loop_flush_results(out, result_ring, &nresults);

  json_value_destroy(events);
  return out;
}

/* ---------------------------------------------------------------------------
 * The loop proper.
 * ------------------------------------------------------------------------- */

/* The loop's own corr counter (pairs cell.run with cell.result on the audit
   trail; a PRIVATE space — py_agent's verb corrs travel through the bridge
   reply registry, pyrt's executor corrs stay inside the runtime). */
static ATOMIC(uint64_t) _loop_corr = 0;

static uint64_t _loop_next_corr(void) {
  return atomic_fetch_add(&_loop_corr, 1) + 1;
}

/* One control event {kind, text}; never fatal on its own failure (the loop's
   caller decides what a missed audit line means — the log carries it). */
static void _loop_control(frame_t* f, const char* kind, const char* text) {
  json_value_t* payload = json_new_object();
  json_object_set(payload, "kind", json_new_string(kind));
  json_object_set(payload, "text",
                  (text != NULL) ? json_new_string(text) : json_new_null());
  if (_frame_event_write(f, "control", payload) != 0) {
    log_error("loop: control event '%s' refused by the store at '%s'", kind,
              frame_sid(f));
  }
}

/* The python availability gate: a WDB-only build has no runtime, so a tool
   call can NEVER execute. The check is lazy (at the first tool call) rather
   than at run_loop entry — a content-only scripted loop stays runnable, and
   a tool call fails loud with control "python-missing" instead of burning
   the turn budget on status-1 cells. */
#ifndef SA_HAS_PYTHON
#define _loop_python_ready() 0
#else
#define _loop_python_ready() 1
#endif

int frame_run_loop(frame_t* f) {
  if (_frame_is_live(f) == 0) {
    log_error("frame_run_loop: dead frame");
    return 1;
  }

  unsigned cap = _frame_loop_turn_cap(f);
  unsigned turn = 0;                       /* model turns ISSUED */

  for (;;) {
    /* Stop request drains before anything else (FRM_STOP is a control
       request, not an interruption: the running cell finishes). */
    if (_frame_stop_requested(f)) {
      log_info("loop: stop requested before turn %u of '%s'", turn + 1,
               frame_sid(f));
      return 0;
    }
    /* A report (from any earlier turn's cell) already ended this frame —
       clean completion whether it is now top or child. */
    if (frame_is_done(f)) return 0;
    /* Fail loud at the cap: never an infinite loop. */
    if (turn == cap) {
      log_error("loop: turn limit %u reached at '%s' — failing loud",
                cap, frame_sid(f));
      _loop_control(f, "turn-limit", "model turn budget exhausted");
      return 1;
    }
    turn++;

    json_value_t* messages = _loop_derive_context(f);
    if (messages == NULL) {
      log_error("loop: derivation failed at '%s'", frame_sid(f));
      _loop_control(f, "derive-error", NULL);
      return 1;
    }
    model_backend_t* mb = _frame_backend_get(f);
    if (mb == NULL) {
      log_error("loop: '%s' has no usable model backend", frame_sid(f));
      _loop_control(f, "model-missing", NULL);
      json_value_destroy(messages);
      return 1;
    }

    model_reply_t* reply = NULL;
    char* err = NULL;
    /* raw_out = NULL: the documented NULL-tolerant body out-param of
       model.h's complete() — the loop keeps only the parsed reply. */
    int rc = mb->complete(mb, messages, NULL, NULL, &reply, &err);
    if (rc != 0 || reply == NULL) {
      /* Retry ONCE with the same derived array (the store has not moved);
         then fail loud. */
      _loop_control(f, "model-error", (err != NULL) ? err : "backend returned no reply");
      free(err);
      err = NULL;
      reply = NULL;
      rc = mb->complete(mb, messages, NULL, NULL, &reply, &err);
      if (rc != 0 || reply == NULL) {
        _loop_control(f, "model-error-final",
                      (err != NULL) ? err : "backend returned no reply");
        free(err);
        json_value_destroy(messages);
        return 1;
      }
    }
    free(err);

    if (reply->tool_code != NULL) {
      /* --- the tool path ------------------------------------------------ */
      if (!_loop_python_ready()) {
        log_error("loop: tool call at '%s' but this build has no python "
                  "runtime — no cell can execute", frame_sid(f));
        _loop_control(f, "python-missing", "no python runtime in this build");
        model_reply_destroy(reply);
        json_value_destroy(messages);
        return 1;
      }
      uint64_t corr = _loop_next_corr();
      json_value_t* run_payload = json_new_object();
      json_object_set(run_payload, "code", json_new_string(reply->tool_code));
      json_object_set(run_payload, "corr", json_new_int((int64_t)corr));
      if (_frame_event_write(f, "cell.run", run_payload) != 0) {
        /* The audit line was refused (e.g. a huge cell) — fail loud rather
           than execute an untracked cell. */
        log_error("loop: cell.run event refused at '%s'", frame_sid(f));
        _loop_control(f, "audit-error", "cell.run event refused");
        model_reply_destroy(reply);
        json_value_destroy(messages);
        return 1;
      }

      frm_cell_payload_t* cp = get_clear_memory(sizeof(frm_cell_payload_t));
      cp->corr = corr;
      cp->code = strdup(reply->tool_code);
      message_t m;
      m.type = (uint32_t)FRM_CELL_EXECUTE;
      m.payload = cp;
      m.payload_destroy = frm_cell_payload_destroy;
      /* The frame behavior consumes the payload on every handled path, so a
         non-NULL leftover means it did not claim the message. */
      frame_dispatch(f, &m);
      if (m.payload != NULL) {
        log_error("loop: FRM_CELL_EXECUTE unclaimed at '%s'", frame_sid(f));
        frm_cell_payload_destroy((frm_cell_payload_t*)m.payload);
        m.payload = NULL;
      }
      model_reply_destroy(reply);
      reply = NULL;

      uint8_t cell_status = 1;
      if (_frame_cell_wait(f, SA_LOOP_CELL_WAIT_MS, &cell_status) != 0) {
        /* The cell is still running at the deadline; its eventual result
           still lands (the frame slot completes it) but the loop stops
           waiting. */
        log_error("loop: cell corr %llu in flight past the %d ms wait at '%s'",
                  (unsigned long long)corr, (int)SA_LOOP_CELL_WAIT_MS,
                  frame_sid(f));
        _loop_control(f, "cell-timeout", NULL);
        json_value_destroy(messages);
        return 1;
      }
      /* A status-1 cell does NOT end the loop: the traceback rides the next
         derive as a status-1 user message and the model self-corrects. */
      log_info("loop: cell corr %llu of '%s' completed with status %u",
               (unsigned long long)corr, frame_sid(f), cell_status);
      json_value_destroy(messages);
      continue;
    }

    /* --- the content path (no tool call: the turn ends) ---------------- */
    if (reply->content != NULL && reply->content[0] != '\0') {
      if (frame_append_msg(f, "assistant", reply->content) != 0) {
        log_error("loop: assistant msg.append refused at '%s'", frame_sid(f));
        _loop_control(f, "commit-error", "assistant msg.append refused");
        model_reply_destroy(reply);
        json_value_destroy(messages);
        return 1;
      }
    } else {
      /* An empty stop (no tool call, no content) still lands in the audit
         trail as a control event — a turn that writes nothing must never
         vanish from the record. The model chose to stop: the frame ends. */
      _loop_control(f, "empty-turn", NULL);
    }
    model_reply_destroy(reply);
    json_value_destroy(messages);

    /* TOP frames end here; children left running end via report only. */
    if (!_frame_is_child(f)) {
      if (_frame_set_status_done(f) != 0) {
        log_error("loop: status->done refused at '%s'", frame_sid(f));
        return 1;
      }
    }
    return 0;
  }
}

#endif /* SA_HAS_WDB */