//
// Created by victor on 9/29/26.
//
// The turn engine — the piece that makes the stack model-driven. Since the
// orchestration slice (Task 3 of the frame-orchestration plan) the engine is
// a PHASE MACHINE carried on the frame, not a held-thread for(;;): every
// step composes, posts the next store round trip / message, and RETURNS
// (yields) — the step CONTINUES in the next arrival dispatch. The frame's
// own actor IS the engine's runner: its owner (a scheduler pool worker in
// production; the synchronous test driver's bounded pump) picks it up on
// every arrival:
//
//   frame_start ──▶ FRM_TURN ─┬─▶ checks + FRM_STORE_SCAN (the derive)
//                             │        └─▶ phase=STORE, yield
// (the store's FRM_STORE_REPLY routes back via frame.c's reply-router
//  step 4 into _frame_engine_store_reply):
//   FRAME_STORE_DERIVE ─▶ parse the raw records (µs); a persona-carrying
//                            frame PARKS the DOM + posts the persona's
//                            GET_NAMED read ─▶ phase=STORE, yield
//   FRAME_STORE_PERSONA ─▶ compose the persona GROUP (the prompt's FIRST
//                            block; a refused/missing read = the base
//                            alone, LOUD — a turn NEVER fails on a persona
//                            read) with the parked DOM ─▶ the model path:
//                            async backend: submit() ─▶ phase=MODEL, yield
//                            sync backend:  complete() INLINE (test driver)
//   FRM_MODEL_RESULT ─▶ decode (µs) ─▶ tool path / content path
//   FRAME_STORE_CELL_RUN ─▶ FRM_CELL_EXECUTE ─▶ phase=CELL, yield
//   PYRT_RESULT ─▶ slot completed ─▶ repost FRM_TURN ─▶ next turn
//   FRAME_STORE_FINISH ─▶ the turn's message + completion commit ─▶
//                            top with live children: phase=CHILDREN, yield
//                            top, none:      engine ends (status rode)
//                            child, none:    quiet-completion terminate
//                            (the content IS the outcome — reported to the
//                             parent; its bind reply posts FRM_CHILD_REPORT)
//   FRM_CHILD_REPORT ─▶ live_children-- + the folded frame.join ─▶
//                            repost FRM_TURN (the parent RESUMES)
//
// ONE TURN = at most four mailbox dispatches; every arrow above is a
// dispatch or a repost — never a wait. NO lock exists on the engine's path.
//
// THE TURN'S RULES, in the old for(;;)'s exact order (the restructure kept
// every rule byte-equivalent):
//   1. derive — the engine rebuilds the model's ENTIRE view from the frame's
//      stored event log on every turn (stateless; nothing accumulates in
//      hidden engine memory): msg.append events become {role, content}
//      messages; the ctx snapshot (state.remember events replayed as
//      newest-value-wins) and one-line child reports (frame.report events)
//      go into the system prompt; cell results since the newest msg.append
//      ride along as tool-result-equivalent user messages. The projection
//      reads the store as ONE bounded reverse scan (FRAME_STORE_DERIVE —
//      root-level, ABSOLUTE-bounds; database_subtree_scan_* is broken in
//      both directions and never touched here). Nothing materializes to
//      disk: the store IS the source.
//   2. complete — one model call. async backend (submit != NULL): the
//      derived array is handed over and destroyed; the completion arrives
//      later as FRM_MODEL_RESULT. Sync backend (submit == NULL — every
//      scripted test backend): complete() runs INLINE inside the derive
//      reply's dispatch — blocking the actor, which is acceptable ONLY on
//      the documented inline/test driver (§6), never on a pool worker in
//      production. tools = NULL: model.c's canned single `execute` tool.
//   3. tool path — the cell.run audit is the FRAME_STORE_CELL_RUN round
//      trip (the audit commit BEFORE any cell executes — no untracked cell
//      ever runs); the reply dispatches FRM_CELL_EXECUTE (the old tool path
//      verbatim: the unclaimed-payload check, model_reply_destroy) and a
//      pending cell yields in FRAME_PHASE_CELL. A SYNCHRONOUS refusal never
//      set the pending slot — the ENGINE writes its PAIRED status-1
//      cell.result right there (the audit-honesty fix: the old loop's
//      refusal paths left the cell.run line unpaired) and the next turn
//      re-derives from it. A FAILED cell does not end the turn: the next
//      derive carries its status-1 text (traceback) back as a user message
//      and the model self-corrects.
//   4. content path — msg.append (or the empty-turn control event) and, when
//      the turn ENDS the frame, the status->done put in ONE atomic
//      FRAME_STORE_FINISH batch (message + completion cannot half-apply).
//      The END rule (spec §1/§4): a content turn while LIVE CHILDREN are
//      pending yields — phase=FRAME_PHASE_CHILDREN, status stays "running",
//      frame_run_loop returns 2 — and each child's report resumes it; an
//      engine ending with live_children == 0 flips done. A CHILD with no
//      children of its own ends quiet-complete: its content IS its outcome,
//      reported to the parent through the engine-driven report bind (the
//      reply posts FRM_CHILD_REPORT). (REPORT SEMANTICS: a report marks the
//      REPORTING frame done at every depth; a child additionally binds the
//      report event into the parent's log, a top frame has no parent log to
//      bind into and reports into its OWN log.)
//
// TURN-LIFECYCLE ENVELOPE (Task 2, the turn-lifecycle slice): every engine
// action opens/closes the audit envelope — turn.start at the turn entry
// (after the cap check, before the model dispatch — the derive's scan reply
// dispatch, because the counter restore reads that scan), step.start riding
// the turn's first durable record batch (the cell.run audit / the finish
// batch), and the close [step.end + turn.end {reason}] riding the cell.result
// batch (tool cycles) or the finish batch (content turns). EVERY live-engine
// exit closes its turn (the DSH finally-discipline: _loop_fail carries
// turn.end {error + the control kind}) — the balance is a TESTED RULE
// (test_loop.cpp). The counter restores once per engine run through the log.
//
// MESSAGE-ARRAY CONSTRUCTION SIMPLIFICATION (documented): NO
// tool_call/tool-role history is reconstructed. Each turn sends a FRESH
// plain-roles array (system + projected msg.appends + trailing cell results
// as user messages) built from the event projection. Cell CODE is not
// re-quoted into context (cell.run events are skipped by the projection) —
// the result text carries the outcome, and history-by-projection is where
// token bloat would otherwise creep back. One helper builds the whole array.
//
// ERRORS: model errors retry per the GUARDS TABLE (guards spec §2; the
// cause-specific caps + backoffs — the retry is ONE fresh FRM_TURN repost,
// the re-derive is provably equivalent to the old loop's same-array retry
// because the derive is stateless from the store: a steering write that
// slipped in only ADDS context; wait-bearing retries post the SAME
// continuation through the delayed-post timer) and end the engine failed
// with control events ("model-error" per retry, then "model-error-final"
// carrying the class's name); the store round trips' refusals, a missing
// backend, a missing python runtime, and each awaited phase's deadline
// break each fail loud with their own control kind. The engine NEVER spins
// and never fails silently.
//
// The synchronous driver: frame_run_loop (below) is start-or-pump — a
// bounded pump of the frame's, its live ancestors', and the inline store's
// mailboxes over the SAME engine; the public event-driven entry is
// frame_start (frame.h; the pool owns pacing).
//
// Cap: frame_set_loop_turn_cap (runtime state on the frame; 0 = the
// SA_LOOP_MAX_TURNS default of 64) bounds model turns issued per run — fail
// loud with control "turn-limit" at the cap.

#include "loop.h"
#include "frame_internal.h"
#include "frame_messages.h"
#include "guards.h"     /* the retry table (guards spec §2): this file's reply
                           path is the table's SECOND integration point — the
                           cause lookup + the plan's cap + the backoff */
#include "lifecycle.h"
#include "model.h"
#include "model_internal.h"
#include "persona.h"    /* the persona GROUP's compose (persona spec §2): the
                           derive's second store trip reads the persona
                           records as state and composes here — the loop
                           owns no persona logic beyond the read + compose
                           + the first-block placement */
#include "../Actor/actor.h"
#include "../Platform/platform_time.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include "../Util/budget.h"
#include "../Util/log.h"

/* The model sink's Retry-After lookup (below). Streams-gated: the header
   module's .c is streams-compiled, and in a WDB-no-streams build no http
   backend exists — every sink delivery's headers is NULL — so the parse
   compiles out with the transport. */
#if defined(SA_HAS_STREAMS)
#include "../Streams/http_headers.h"
#endif

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

/* The projection caps are the budget table's names NOW (src/Util/budget.h,
   surface-completion spec §4): SA_BUDGET_LOOP_MSG_CAP, SA_BUDGET_LOOP_SNAPSHOT,
   SA_BUDGET_LOOP_REPORT, SA_BUDGET_LOOP_EMIT. The loop's LOCAL structure caps
   (how many keys / reports / trailing results ride along) stay here — they
   bound the projection's SHAPE, not any one text's bytes. The per-message /
   per-value / per-line byte caps live in the table alone. */

/* Bounded ctx snapshot: at most this many distinct keys reach the system
   prompt (NEW keys beyond the cap are dropped; already-present keys keep
   updating to their newest value). The snapshot comes from state.remember
   events, so it includes local/ writes too — the projection shows the model
   the effect history of BOTH remember layers (they share the state.remember
   event; the ctx/local split is a recall-shadowing detail the model does not
   need). */
#define SA_LOOP_SNAPSHOT_MAX_KEYS 24

/* One-line per-child report summaries: at most this many reports, each
   flattened to its first line. */
#define SA_LOOP_REPORTS_MAX 8

/* Trailing cell-result block: at most this many results ride along after the
   newest msg.append (a model that keeps calling tools without ever writing a
   message stays bounded; older results roll off). */
#define SA_LOOP_RESULTS_BLOCK_MAX 8

/* Bounded wait for the engine's FRAME_PHASE_CELL (the driver's per-phase
   deadline; a first-time interpreter boot lives well inside this). */
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

/* The PLAN turn's instruction (escalation spec §2.2; Task 5): ONE bounded
   constant — byte-stable per mode (the cache-stable prefix's rule) — riding
   the system prompt of every plan turn (after the persona group, or ahead
   of the base instruction when no persona rides). The plan turn is
   tools-null: the model's reply text IS the plan; the close composes the
   approval gate (the plan_gate ask, or the bypass's auto-approval). */
static const char SA_LOOP_PLAN_INSTRUCTION[] =
    "You are in PLAN mode: do not execute anything yet. Propose a concrete "
    "plan of steps for the goal instead; your reply IS the plan, and the "
    "owner approves or rejects it before any cell can run.\n";

/* The frame's tool-id surface, as persona_compose (persona spec §2 item 4)
   reads it: the tool-conditional guidance entries attach on these ids.
   Today's surface is the model layer's ONE canned `execute` tool (loop.c's
   instruction above says so; model.c serves it) — the id list pinned here,
   where the compose happens. A second tool grows this list with model.c's
   surface. */
static const char* const _LOOP_TOOL_IDS[] = {"execute"};
#define _LOOP_TOOL_IDS_N (sizeof(_LOOP_TOOL_IDS) / sizeof(_LOOP_TOOL_IDS[0]))

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
 * "frame","corr","at","cause","payload"} — the derive's DOM is the parsed
 * event array the FRM_STORE_SCAN reply carried (ascending seq, bounded to
 * the newest 512 records), and the projection runs two passes over that
 * parsed array: pass A fills the system-prompt material (snapshot +
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

/* The ladder's gating read (escalation spec §2.2/§2.3; Task 5): 1 = THIS
   turn runs PLAN-shaped — a tools-null model request, the plan-instruction
   block riding the system prompt, and the close-side gate composing in the
   content path. FREE never gates (the byte-identity proof: every standing
   pin's mode); PLAN_ASK_ACT/BYPASS gate every turn until the engine's
   ladder_act flips (the approval's consume or the bypass's auto-approval,
   re-learned from the log every derive — spec §2.4's consult). */
static uint8_t _loop_ladder_gating(frame_t* f, const frame_engine_state_t* e) {
  return (_frame_escalation_mode(f) != (unsigned)FRAME_ESCALATION_FREE &&
          e != NULL && e->ladder_act == 0)
             ? 1 : 0;
}

static char* _loop_system_content(frame_t* f, const loop_snap_t* snaps, size_t nsnaps,
                                  const loop_report_t* reports, size_t nreports,
                                  const char* persona_prefix,
                                  const char* plan_block) {
  loop_sb_t sb;
  _loop_sb_init(&sb);
  if (persona_prefix != NULL) {
    /* The persona GROUP (persona spec §2): the composed prefix IS the
       prompt's first block — the record carries its placement, and it was
       composed WITH SA_LOOP_INSTRUCTION as the base, so the instruction's
       bytes live INSIDE the prefix (placement "first" = persona block +
       context + guidance, then the instructions; "below" = instructions
       first, then the persona group). The sb replaces its own instruction
       put with the prefix verbatim and continues with the goal — the
       composed string is byte-stable per inputs (the cache-stable prefix,
       matrix row 36: the persona re-composes every derive, and a record
       that carries no {CURRENT_DATETIME} stamps nothing). */
    _loop_sb_puts(&sb, persona_prefix);
    if (plan_block != NULL) {
      _loop_sb_puts(&sb, plan_block);   /* the plan block rides AFTER the
                                           persona group */
    }
  } else {
    if (plan_block != NULL) {
      /* The ladder's plan turn without a persona: the plan block rides
         BEFORE the base instruction (escalation Task 5). */
      _loop_sb_puts(&sb, plan_block);
    }
    _loop_sb_puts(&sb, SA_LOOP_INSTRUCTION);   /* today's bytes, untouched */
  }

  const char* goal = _frame_goal(f);
  if (goal != NULL) {
    char* g = _loop_trunc(goal, SA_BUDGET_LOOP_MSG_CAP);
    _loop_sb_putf(&sb, "Goal: %s\n", g);
    free(g);
  }

  if (nsnaps > 0) {
    _loop_sb_puts(&sb, "\nFrame state (newest value wins; JSON verbatim):\n");
    for (size_t i = 0; i < nsnaps; i++) {
      char* v = json_serialize(snaps[i].value);
      char* vt = (v != NULL) ? _loop_trunc(v, SA_BUDGET_LOOP_SNAPSHOT) : NULL;
      _loop_sb_putf(&sb, "- %s = %s\n", snaps[i].key,
                    (vt != NULL && vt[0] != '\0') ? vt : "(unserializable)");
      free(v);
      free(vt);
    }
  }

  if (nreports > 0) {
    _loop_sb_puts(&sb, "\nChild reports:\n");
    for (size_t i = 0; i < nreports; i++) {
      char* line = _loop_one_line(reports[i].text, SA_BUDGET_LOOP_REPORT);
      _loop_sb_putf(&sb, "- %s: %s\n", reports[i].child_sid, line);
      free(line);
    }
  }
  return sb.s;   /* freed by the caller */
}

/* ONE helper builds the whole messages array (the projection, byte-
   equivalent to the old loop's derive): returns a fresh JSON array value the
   engine destroys after the model call; NULL on failure (the caller logs a
   control event). `events` = the derive's parsed DOM (consumed).
   `persona_prefix` = the composed persona GROUP (borrowed; NULL = none —
   every persona-less frame's prompts stay byte-identical).
   `plan_block` = the ladder's plan instruction (borrowed; NULL in every
   free-acting turn — the standing prompts stay byte-identical). */
static json_value_t* _loop_project(frame_t* f, json_value_t* events,
                                   const char* persona_prefix,
                                   const char* plan_block) {
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
  char* sys_text = _loop_system_content(f, snaps, nsnaps, reports, nreports,
                                        persona_prefix, plan_block);
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
        char* ct = _loop_trunc(json_as_string(content), SA_BUDGET_LOOP_MSG_CAP);
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
                             SA_BUDGET_LOOP_MSG_CAP);
      size_t line_len = strlen("cell result (status ): ") + strlen(status_buf) +
                        strlen(tt);
      char* line = get_memory(line_len + 1);
      snprintf(line, line_len + 1, "cell result (status %s): %s", status_buf, tt);
      free(tt);
      _loop_result_push(result_ring, &nresults, line);
    } else if (strcmp(type_name, "emit") == 0) {
      /* The write verb's projected half (spec §1): one line per emit,
         "emit: <text>", each capped at the table's emit line cap, event
         order — the model sees its own deliberate artifacts again at its
         next derive. Emits ride the same ring as cell results (only the
         block since the newest msg.append is projected). */
      json_value_t* text_v = json_get(payload, "text");
      const char* text = (text_v != NULL) ? json_as_string(text_v) : NULL;
      /* Only non-empty text renders a line — {"text": null} and "" render
         NOTHING (json_as_string on null is ""; guarding here keeps the
         branch's claim honest: no text renders nothing).
         The line cap is SA_BUDGET_LOOP_EMIT (300): a source-capped emit's
         marker ("\n[budget: truncated at N bytes]") sits PAST this cut's
         300-byte window and is not carried into the projection — accepted,
         the same accepted pattern as cell results' 32 KiB vs 4000. */
      if (text != NULL && text[0] != '\0') {
        char* tt = _loop_trunc(text, SA_BUDGET_LOOP_EMIT);
        size_t line_len = strlen("emit: ") + strlen(tt);
        char* line = get_memory(line_len + 1);
        snprintf(line, line_len + 1, "emit: %s", tt);
        free(tt);
        _loop_result_push(result_ring, &nresults, line);
      }
      /* An emit record without renderable text (no key, null, or empty)
         renders nothing (the render-not-crash rule — the writer's compose
         always carries text; a corrupt stored record is the fold's loud
         skip, not a crash here). */
    } else if (strcmp(type_name, LIFE_EVENT_REPAIR) == 0) {
      /* The crash-repair brief (spec §4): REPAIR events render as a user-
         role message, text verbatim — the model reads the full crash
         briefing in its next derive with the details upfront. The payload's
         text is capped at source (the derive's msg cap). The ring flushes
         FIRST: a repair between a cell.result and the stream's next
         msg.append must not reorder the result line behind the brief. */
      _loop_flush_results(out, result_ring, &nresults);
      json_value_t* text_v = json_get(payload, "text");
      if (text_v != NULL) {
        const char* text = json_as_string(text_v);
        char* tt = _loop_trunc((text != NULL) ? text : "", SA_BUDGET_LOOP_MSG_CAP);
        json_value_t* m = json_new_object();
        json_object_set(m, "role", json_new_string("user"));
        json_object_set(m, "content", json_new_string(tt));
        free(tt);
        json_array_append(out, m);
      }
      /* A repair record with no text renders nothing (the render-not-crash
         rule — the brief's compose always carries text; a corrupt stored
         record is the fold's loud skip, not a crash here). */
    }
    /* cell.run skipped (code is not re-quoted — see the header's
       construction note); state.remember / frame.report went into pass A;
       spawn/join/control records carry no model context — and the emit
       record is rendered above (the ring), while pyrt log/status ride the
       frame's consumption only and never reach the stream at all. turn.start /
       turn.end / step.start / step.end are log spine only (the envelope
       folds away; the `repair` brief is the ONE model-visible lifecycle
       type and rendered above). */
  }
  _loop_flush_results(out, result_ring, &nresults);

  json_value_destroy(events);
  return out;
}

/* ---------------------------------------------------------------------------
 * The engine. frame_internal.h's handlers + the frame-side helpers they
 * lean on (frame.c: _frame_post / _frame_store_actor / _frame_store_corr_next
 * / _frame_event_post / _frame_event_post_fire / _frame_seq_rollback /
 * _frame_engine_finish_post / _frame_pump).
 * ------------------------------------------------------------------------- */

/* The loop's own corr counter (pairs cell.run with cell.result on the audit
   trail; a PRIVATE space — py_agent's verb corrs travel through the bridge
   reply registry, pyrt's executor corrs stay inside the runtime). */
static ATOMIC(uint64_t) _loop_corr = 0;

static uint64_t _loop_next_corr(void) {
  return atomic_fetch_add(&_loop_corr, 1) + 1;
}

/* One control event {kind, text}; FIRE-AND-POST (§1): every engine path that
   can be OBSERVED externally ends through an awaited phase (FINISH/BIND
   reply), and the store's FIFO order puts every earlier control batch ahead
   of it — assertions riding post-commit state stay deterministic (the sync
   driver additionally drains the inline store to quiescence before its
   return). */
static void _loop_control(frame_t* f, const char* kind, const char* text) {
  json_value_t* payload = json_new_object();
  json_object_set(payload, "kind", json_new_string(kind));
  json_object_set(payload, "text",
                  (text != NULL) ? json_new_string(text) : json_new_null());
  if (_frame_event_post_fire(f, "control", payload) != 0) {
    log_error("loop: control event '%s' refused by the store at '%s'", kind,
              frame_sid(f));
  }
}

/* The turn counter's lazy restore (Task 2 rider 1): the FIRST entry of an
   engine run restores the current turn's number from the derive's scanned
   events — the NEWEST recorded lifecycle turn number + 1; no extra round
   trip (the scan already materialized them). The _frame_restore_seq
   discipline: monotonic restore, gaps LOGGED LOUD and continued, never a
   lock, never a second writer — the restore reads, and only the engine's
   entry writes. A log with no lifecycle records at all (a pre-lifecycle
   tail — the envelope's absence is not a truncation) starts the counter at
   1. Malformed lifecycle records skip (the render-not-crash rule; the fold
   already logged them). */
static uint64_t _loop_turn_counter_restore(frame_t* f,
                                           const json_value_t* events) {
  uint64_t newest = 0;
  uint64_t last = 0;
  uint8_t any = 0;
  size_t n = json_size(events);
  for (size_t i = 0; i < n; i++) {
    json_value_t* rec = json_at(events, i);
    if (rec == NULL) continue;
    json_value_t* type_v = json_get(rec, "type");
    json_value_t* payload = json_get(rec, "payload");
    const char* type_name = (type_v != NULL) ? json_as_string(type_v) : "";
    if (payload == NULL) continue;
    if (strcmp(type_name, LIFE_EVENT_TURN_START) != 0 &&
        strcmp(type_name, LIFE_EVENT_STEP_START) != 0 &&
        strcmp(type_name, LIFE_EVENT_STEP_END) != 0 &&
        strcmp(type_name, LIFE_EVENT_TURN_END) != 0) {
      continue;   /* only the envelope's four numbered types carry turns */
    }
    json_value_t* turn_v = json_get(payload, "turn");
    if (turn_v == NULL || json_type(turn_v) != JSON_INT ||
        json_as_int(turn_v) < 0) {
      continue;   /* malformed lifecycle payload: the fold's loud rule
                     already rendered it; the restore skips it */
    }
    uint64_t t = (uint64_t)json_as_int(turn_v);
    if (any != 0 && t > last && t - last > 1) {
      log_error("loop: the turn counter restore at '%s' found a gap "
                "(recorded turn %llu follows turn %llu) — continuing past "
                "it loud, never inventing a smaller number", frame_sid(f),
                (unsigned long long)t, (unsigned long long)last);
    }
    if (t > newest) newest = t;
    last = t;
    any = 1;
  }
  return (any != 0) ? newest + 1 : 1;
}

/* The turn-entry rider (Task 2 rider 1; spec §3): after the turn cap check
   passed (it gates the FRM_TURN that posted this derive) and before the
   model dispatch, the engine opens the turn — ONE turn.start record,
   fire-and-post (the control events' discipline: a refusal logs loud and
   the engine continues; the close still writes when it can, so the envelope
   pairs in the log whenever the store accepts). Runs on EVERY cycle
   including the first; a model-RETRY's re-derive finds the turn open and
   skips the entry (the retry is the SAME turn, never a renumber). */
static void _loop_turn_entry(frame_t* f, frame_engine_state_t* e) {
  if (e->turn_open) return;
  json_value_t* payload = lifecycle_turn_start_json(e->turn_counter);
  if (_frame_event_post_fire(f, LIFE_EVENT_TURN_START, payload) != 0) {
    log_error("loop: turn %llu's turn.start was refused by the store at "
              "'%s' — continuing loud",
              (unsigned long long)e->turn_counter, frame_sid(f));
  }
  e->turn_open = 1;
  e->step_open = 0;
}

/* The failure-path turn close (Task 2 riders 3/4; spec §3's terminal
   attribution): an ALIVE engine's open turn closes on EVERY exit — the
   failure's own batch carries turn.end {reason error, text: the control
   kind's wording verbatim} (a path with no control kind carries the plain
   detail text), step.end riding BEFORE it when a step durably started (DSH's
   order), all in ONE atomic fire-and-post batch with the control event when
   one exists. Never blocks the terminate: a refusal logs loud and the
   terminate still runs (never hang on a WAL failure). */
static void _loop_turn_close_fail(frame_t* f, frame_engine_state_t* e,
                                  const char* kind, const char* text) {
  if (e == NULL || !e->turn_open) return;
  const char* names[3];
  json_value_t* payloads[3];
  size_t n = 0;
  if (kind != NULL) {
    json_value_t* control = json_new_object();
    json_object_set(control, "kind", json_new_string(kind));
    json_object_set(control, "text",
                    (text != NULL) ? json_new_string(text) : json_new_null());
    names[n] = "control";
    payloads[n++] = control;
  }
  if (e->step_open) {
    names[n] = LIFE_EVENT_STEP_END;
    payloads[n] = lifecycle_step_json(e->turn_counter, 1);
    n++;
  }
  names[n] = LIFE_EVENT_TURN_END;
  payloads[n++] = lifecycle_turn_end_json(
      e->turn_counter, LIFE_REASON_ERROR, (kind != NULL) ? kind : text);
  int rc = _frame_event_batch_post_fire(f, names, payloads, n,
                                        "failure close");
  if (rc != 0) {
    log_error("loop: the failure close of turn %llu at '%s' was refused "
              "pre-post — the terminate still runs, never hang",
              (unsigned long long)e->turn_counter, frame_sid(f));
  }
  e->turn_open = 0;   /* the close ATTEMPTED — the fire-and-post refusal is
                         the store's recorded loud gap, never a spin */
  e->step_open = 0;
}

/* The terminal step (defined below the turn engine's handlers; declared
   early — the failure surfaces of this file run ahead of its
   definition). EXPORTED (frame_internal.h): frame.c's interrupt synthesis
   ends the engine with it on the frame's dispatch thread. */
void _frame_engine_terminate(frame_t* f, uint8_t ok, const char* text);

/* The failure surface (spec §4): the control event — kind + text, when the
   path carries one — stays in THIS frame's log (fire-and-post; the store's
   FIFO commits it ahead of the terminate's bind), and the engine terminates
   FAILED: a CHILD binds ONE failure report whose text is "<control kind>:
   <control text>" (the accountability surface — the parent's derive shows
   it), and the bind's confirmed commit resumes the parent; a TOP frame's
   engine just ends failed (the pinned cap-is-a-failure shape, no status
   change). kind NULL = a plain-text failure at a path that never carried a
   control kind (the OOM/lost-reply shapes — semantics kept byte-for-byte);
   text NULL = the kind alone. Never silent. */
static void _loop_fail(frame_t* f, frame_engine_state_t* e, const char* kind,
                       const char* text) {
  /* The finally-discipline rider (Task 2 riders 3/4): an ALIVE engine's open
     turn closes error, riding the failure's OWN batch — the control event
     and the envelope's closers in ONE atomic fire-and-post batch, BEFORE the
     terminate (the store's FIFO commits the close ahead of a child's report
     bind). No close, control event: the plain pre-entry failures' paths
     (turn_open == 0 — the cap refusal NEVER opened the refused turn, so the
     `turn-limit` reason is emitted by NO writer — spec §5's pin). */
  if (e != NULL && e->turn_open) {
    _loop_turn_close_fail(f, e, kind, text);
  } else if (kind != NULL) {
    _loop_control(f, kind, text);
  }
  if (kind == NULL) {
    _frame_engine_terminate(f, 0,
                            (text != NULL) ? text : "engine failed");
    return;
  }
  if (text == NULL) {
    _frame_engine_terminate(f, 0, kind);
    return;
  }
  char* joined = get_memory(strlen(kind) + strlen(text) + 4);
  if (joined == NULL) {
    log_error("loop: out of memory composing the failure text at '%s' — "
              "the bound report carries the kind alone", frame_sid(f));
    _frame_engine_terminate(f, 0, kind);
    return;
  }
  snprintf(joined, strlen(kind) + strlen(text) + 4, "%s: %s", kind, text);
  _frame_engine_terminate(f, 0, joined);
  free(joined);   /* the terminate consumed the text synchronously (the bind
                     payload owns its own copy) */
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

/* End the live engine WITHOUT notifying anyone (the engine-side bookkeeping
   only). `_loop_fail`/`_frame_engine_terminate` are the failure and terminal
   surfaces (a child's outcome binds into the parent); this one serves the
   DESTROY-race shapes — the turn continuation refused by a dying mailbox,
   a queued turn arriving at a gone subtree — where no engine could notify
   anyone, and the clean stop/end paths. Keep a genuine failure OUT of it. */
static void _loop_engine_end(frame_t* f, frame_engine_state_t* e, uint8_t failed) {
  if (e == NULL) {
    log_error("loop: the engine ended at an unusable frame (already logged)");
    return;
  }
  e->engine_live = 0;
  e->phase = FRAME_PHASE_NONE;
  e->store_kind = (frame_store_kind_e)0;
  e->store_corr = 0;
  e->turn_cell_corr = 0;
  e->model_retry_step = 0;
  /* The ladder's ACT marker dies with the engine (the model_retry_step
     lifecycle: a DEAD engine never carries it — a restart's first derive
     re-consults the log's plan-approved control record, spec §2.4). */
  e->ladder_act = 0;
  /* The wake latch resets with the engine (a dying engine's queued
     continuation is either consumed or dead-queued — a resumed run must
     wake on its own inputs, never inherit a stale one). */
  atomic_store(&e->continuation_queued, 0);
  /* The envelope's compose-time facts die with the engine (the store's
     records stay the truth): a DEAD engine never carries the counter or an
     open-turn fact across a restart — the next run's first entry restores
     the counter through the log (Task 2 rider 1's restore discipline). */
  e->turn_counter = 0;
  e->turn_known = 0;
  e->turn_open = 0;
  e->step_open = 0;
  if (e->turn_reply != NULL) {
    model_reply_destroy(e->turn_reply);
    e->turn_reply = NULL;
  }
  free(e->finish_text);     /* the turn's outcome text dies with the engine
                               (the FINISH reply that consumed it ran already,
                               or the frame died mid-yield) */
  e->finish_text = NULL;
  /* The persona trip's window state dies with the engine too (mirror
     finish_text: the parked derive DOM and the composed GROUP are this
     derive's facts — a restarted engine re-derives and re-composes). */
  if (e->derive_events != NULL) {
    json_value_destroy(e->derive_events);
    e->derive_events = NULL;
  }
  free(e->persona_prefix);
  e->persona_prefix = NULL;
  /* The doom breaker's streak dies with the engine too (frame_internal.h's
     field contract; guards spec §1): a DEAD engine never carries a streak
     across a restart — the next run's re-derive relearns the input facts
     from the log and the fresh tool path refolds from zero. */
  free(e->doom_last_code);
  e->doom_last_code = NULL;
  e->doom_streak = 0;
  e->users_seen_seq = 0;
  e->cell_users_seq = 0;
  /* The parked ask dies with the engine (the same discipline: a DEAD engine
     never carries it — every engine end funnels here; the frame teardown's
     own _frame_engine_ask_clear covers a mid-park destroy). The parked
     publish flag follows (frame.c's helper clears the pyrt flag too). */
  _frame_engine_ask_clear(f);
  if (failed) e->engine_failed = 1;
}

/* The doom trip's close (guards spec §1; the guards plan task 6): ONE
   fire-and-post batch (the control's discipline) — the breaker's control
   record {kind, text} + turn.end {reason doom-loop}, control FIRST (the
   store's FIFO puts the model-visible control ahead of the close). No step
   records ever existed for this turn — the trip refused the call BEFORE the
   audit batch composed — so the envelope stays in the "opened but stepped
   nothing" shape the turn-limit refusal's pre-entry refusal uses
   (turn.start … turn.end{doom-loop}, spec §1). Refusal: loud log and the
   OPEN tail stays for resume-repair — the terminate still runs (the
   interrupt's refused-close discipline: never block, never spin on a WAL
   failure). */
static int _loop_doom_close(frame_t* f, frame_engine_state_t* e,
                            uint8_t streak) {
  char text[160];
  snprintf(text, sizeof(text),
           "doom-loop guard: the last %u cells were identical; the turn is "
           "refused — vary the approach", (unsigned)streak);
  json_value_t* control_payload = json_new_object();
  json_value_t* turn_end_payload = NULL;
  if (control_payload != NULL) {
    json_object_set(control_payload, "kind", json_new_string("doom-loop"));
    json_object_set(control_payload, "text", json_new_string(text));
    turn_end_payload = lifecycle_turn_end_json(e->turn_counter,
                                               LIFE_REASON_DOOM_LOOP, text);
  }
  if (control_payload == NULL || turn_end_payload == NULL) {
    json_value_destroy(control_payload);
    json_value_destroy(turn_end_payload);
    log_error("loop: out of memory composing the doom close at '%s'",
              frame_sid(f));
    return -1;
  }
  const char* names[2] = {"control", LIFE_EVENT_TURN_END};
  json_value_t* payloads[2] = {control_payload, turn_end_payload};
  int rc = _frame_event_batch_post_fire(f, names, payloads, 2, "doom close");
  if (rc == 0) {
    e->turn_open = 0;
    e->step_open = 0;
  } else {
    log_error("loop: the doom close of turn %llu at '%s' was refused "
              "pre-post — the open tail stays for resume-repair; the "
              "terminate still runs",
              (unsigned long long)e->turn_counter, frame_sid(f));
  }
  return rc;
}

/* Repost the turn continuation (engine -> itself; never a wait). The
   refusal here is the DESTROY flag only — actor_send answers
   delivered-vs-refused (busy included: a continuation pushed into a
   non-empty mailbox is still delivered; false is the only refusal, never
   re-judged from the return below).
   THE WAKE LATCH rides the continuation: between this queueing and the
   continuation's dispatch the transient phase-NONE gap lets a CONCURRENT
   steer's enqueue race ahead of the continuation — the steer's wake guard
   reads the latch and rides this continuation's derive instead of posting
   a duplicate FRM_TURN (the review's extra-turn-step leak). */
static int _loop_post_turn(frame_t* f) {
  actor_t* actor = _frame_actor(f);
  if (actor == NULL || (atomic_load(&actor->flags) & ACTOR_FLAG_DESTROY)) {
    log_error("loop: the turn continuation was refused at '%s' — the mailbox "
              "is gone; the engine ends failed",
              (f != NULL) ? frame_sid(f) : "?");
    _loop_engine_end(f, _frame_engine_state(f), 1);
    return -1;
  }
  frame_engine_state_t* e = _frame_engine_state(f);
  if (e != NULL) atomic_store(&e->continuation_queued, 1);
  message_t m;
  m.type = (uint32_t)FRM_TURN;
  m.payload = NULL;
  m.payload_destroy = NULL;
  (void)actor_send(actor, &m);
  return 0;
}

/* The engine's model sink (model.h's contract): whatever thread the backend
   completes on (the streams loop thread; a test backend synchronously within
   submit — µs-scale either way: field writes + one post, NO decode here),
   it moves body/error ownership into the FRM_MODEL_RESULT payload the
   frame's own dispatch decodes. The delivery's headers capture is BORROWED
   into this call and OWNED here — this sink is the final consumer, so it
   parses Retry-After (the rate class's pacing fact) and deinits + frees the
   capture before every exit below.

   The sink also OWNS one pending-submit slot (frame_internal.h's lifetime
   handoff: the engine's submit step acquired it for exactly this
   completion) — every exit from the sink releases it, and the release of a
   die-requested frame runs the deferred teardown (frame.c: the record dies
   exactly once, on whichever thread got there last). */
static void _loop_model_sink(void* ctx, int status, char* body,
                             size_t body_len, char* error,
                             http_headers_t* headers) {
  /* Retry-After FIRST — the headers' OWNERSHIP ends here (this sink is the
     final consumer; model.h's contract), so the value is parsed and the
     capture deinit+freed before ANY of the exits below — the headers die on
     every exit path, including the no-memory drop.
     Bounded parse (spec §2): seconds only — strcasecmp'd lookup by
     http_headers_get, strtol, digits (strtol also tolerates leading
     whitespace/sign — harmless: http-parser strips OWS and the guards' 5 s
     cap bounds the value), > 0 and <= 3600 accepted; the date form (leading
     non-digit, or any trailing bytes) answers 0 = absent, and the table's
     own cap (guards.c's 5 s) bounds the value downstream. µs-scale on the
     streams loop thread: one lookup, one strtol. */
#if defined(SA_HAS_STREAMS)
  unsigned retry_after_sec = 0;
  if (headers != NULL) {
    const char* ra = http_headers_get(headers, "retry-after");
    if (ra != NULL) {
      char* end = NULL;
      long v = strtol(ra, &end, 10);
      if (end != ra && *end == '\0' && v > 0 && v <= 3600) {
        retry_after_sec = (unsigned)v;
      }
    }
    http_headers_deinit(headers);
    free(headers);
  }
#else
  unsigned retry_after_sec = 0;   /* no streams: no headers ever arrive */
#endif
  frame_t* f = (frame_t*)ctx;
  if (f == NULL) {
    log_error("loop: the model completion arrived with no frame context — "
              "dropped loud (body/error die here)");
    free(body);
    free(error);
    return;
  }
  if (_frame_engine_die_requested(f) != 0) {
    /* The frame died under the in-flight submit — frame_destroy mid-turn
       marked die_requested and DEFERRED its teardown to this release: the
       record is alive (only its mailbox and the engine are dead), so this
       gate is safe to read, and past it NOTHING on the frame is touched.
       The completion is unusable now; the body/error die here. */
    log_error("loop: the model completion arrived after the frame '%s' died "
              "— dropped loud; the slot's release runs the deferred teardown",
              frame_sid(f));
    free(body);
    free(error);
    (void)_frame_engine_submit_release(f);   /* may free f inside */
    return;
  }
  frm_model_payload_t* p = get_clear_memory(sizeof(frm_model_payload_t));
  if (p == NULL) {
    log_error("loop: the model completion payload failed to allocate at '%s' — "
              "the completion is dropped loud", frame_sid(f));
    free(body);
    free(error);
    (void)_frame_engine_submit_release(f);
    return;
  }
  p->status = status;
  p->body = body;
  p->body_len = body_len;
  p->error = error;
  p->retry_after_sec = retry_after_sec;
  actor_t* mailbox = _frame_actor(f);
  if (mailbox == NULL) {
    /* Defensive only (the handoff keeps the record alive over the slot; the
       actor is embedded): if the mailbox is ever missing, the completion is
       unusable — drop loud and RELEASE (the slot's duty moved here). */
    log_error("loop: the model completion arrived after the frame '%s' died "
              "— dropped loud", frame_sid(f));
    frm_model_payload_destroy(p);
    (void)_frame_engine_submit_release(f);
    return;
  }
  /* The last-mile race is _frame_post's own DESTROY-flag gate: a destroy
     that lands right after the die check above has flagged the actor, and
     the post drops the payload loud instead of entering the dying queue. */
  _frame_post(mailbox, (uint32_t)FRM_MODEL_RESULT, p,
              frm_model_payload_destroy, "model result");
  (void)_frame_engine_submit_release(f);   /* the slot's owner lets go last */
}

/* The derive's store round trip (the turn step's yield): compose the events
   range's ABSOLUTE root-level bounds (pure key composition — the
   _frame_events_bounds string shape, no store access) and post the bounded
   reverse scan as FRM_STORE_SCAN; the reply continues inside
   _frame_engine_store_reply's FRAME_STORE_DERIVE branch. */
static void _loop_post_derive(frame_t* f, frame_engine_state_t* e) {
  const char* sid = frame_sid(f);
  size_t base = (sid != NULL) ? strlen(sid) : 0;
  char* lo = get_memory(base + strlen("/events") + 1);
  char* hi = get_memory(base + strlen("/events0") + 1);
  frm_store_scan_payload_t* sp =
      (frm_store_scan_payload_t*)get_clear_memory(sizeof(frm_store_scan_payload_t));
  if (sid == NULL || lo == NULL || hi == NULL || sp == NULL) {
    log_error("loop: out of memory composing the derive scan at '%s'",
              (sid != NULL) ? sid : "?");
    free(lo);
    free(hi);
    free(sp);
    _loop_fail(f, e, "derive-error", NULL);
    return;
  }
  snprintf(lo, base + strlen("/events") + 1, "%s/events", sid);
  snprintf(hi, base + strlen("/events0") + 1, "%s/events0", sid);
  sp->start = lo;                    /* OWNED by the store round trip */
  sp->end = hi;                      /* OWNED */
  sp->limit = SA_FRAME_DEBUG_MAX_EVENTS;
  sp->reply_to = _frame_actor(f);
  sp->corr = _frame_store_corr_next(f);
  /* The awaited round trip's routing is set BEFORE the post (this dispatch
     is the single runner; the reply routes in a LATER dispatch). */
  e->phase = FRAME_PHASE_STORE;
  e->store_kind = FRAME_STORE_DERIVE;
  e->store_corr = sp->corr;
  _frame_post(_frame_store_actor(f), (uint32_t)FRM_STORE_SCAN, sp,
              frm_store_scan_payload_destroy, "derive scan");
}

/* The tool path's cell.run audit = the FRAME_STORE_CELL_RUN round trip (the
   audit commit BEFORE any cell executes — no untracked cell ever runs); the
   reply dispatches the cell. */
static void _loop_post_cell_run(frame_t* f, frame_engine_state_t* e,
                                model_reply_t* reply) {
  uint64_t corr = _loop_next_corr();
  json_value_t* run_payload = json_new_object();
  if (run_payload == NULL) {
    log_error("loop: out of memory building the cell.run payload at '%s'",
              frame_sid(f));
    model_reply_destroy(reply);
    _loop_fail(f, e, NULL, "out of memory building the cell.run payload");
    return;
  }
  json_object_set(run_payload, "code", json_new_string(reply->tool_code));
  json_object_set(run_payload, "corr", json_new_int((int64_t)corr));

  e->store_corr = _frame_store_corr_next(f);
  e->turn_cell_corr = corr;
  e->phase = FRAME_PHASE_STORE;
  e->store_kind = FRAME_STORE_CELL_RUN;
  /* The turn envelope's step.start RIDES the audit batch (Task 2 rider 2):
     [step.start, cell.run] as ONE atomic awaited batch — the envelope's
     opener and the turn's first durable record commit together (the same
     seq pre-allocation + refusal rollback discipline as the single record;
     the payload compose order puts the envelope first). */
  const char* names[2] = {LIFE_EVENT_STEP_START, "cell.run"};
  json_value_t* payloads[2];
  payloads[0] = lifecycle_step_json(e->turn_counter, 1);
  payloads[1] = run_payload;   /* OWNED by the batch on every path */
  uint64_t first_seq = 0;
  int rc = _frame_event_batch_post(f, names, payloads, 2, e->store_corr,
                                   _frame_actor(f), &first_seq,
                                   "cell.run audit");
  if (rc != 0) {
    /* The audit line was refused (e.g. a huge cell, logged loud pre-post) —
       fail loud rather than execute an untracked cell. */
    if (first_seq != 0) {
      for (size_t i = 2; i > 0; i--) _frame_seq_rollback(f, first_seq + i - 1);
    }
    e->phase = FRAME_PHASE_NONE;
    e->store_kind = (frame_store_kind_e)0;
    e->store_corr = 0;
    log_error("loop: cell.run event refused at '%s'", frame_sid(f));
    model_reply_destroy(reply);
    /* The step.start never committed — step_open stays as the close's
       compose-time truth (0); the fail closes the TURN (error). */
    _loop_fail(f, e, "audit-error", "cell.run event refused");
    return;
  }
  /* The step was POSTED (the audit batch's commit is confirmed by the
     CELL_RUN reply — a store-stage refusal reverts step_open there); the
     model reply rides the engine state to the CELL_RUN reply (which
     dispatches the cell out of it and destroys it). */
  e->step_open = 1;
  e->turn_reply = reply;
}

/* The tool path (the model called `execute`): the doom guard's fold BEFORE
   the audit batch composes, then the audit round trip + yield. */
static void _loop_tool_path(frame_t* f, frame_engine_state_t* e,
                            model_reply_t* reply) {
  if (!_loop_python_ready()) {
    log_error("loop: tool call at '%s' but this build has no python "
              "runtime — no cell can execute", frame_sid(f));
    model_reply_destroy(reply);
    _loop_fail(f, e, "python-missing", "no python runtime in this build");
    return;
  }
  /* The DOOM GUARD (guards spec §1) runs before anything is audited: the
     threshold-th byte-identical call is refused — never dispatched, never
     audited (nothing pointless is ever written to the trail). fresh_input =
     a user message landed since the last dispatch (the derive's
     users_seen_seq scan tracked it; the first dispatch of a run always
     reads fresh — cell_users_seq starts at 0 and the streak folds from
     zero anyway). The copy of the incoming code happens HERE because the
     reply's lifetime ends on the trip path below and after the dispatch
     hand-off — this dispatch is the provable-owner point. */
  uint8_t identical = (e->doom_last_code != NULL &&
                       reply->tool_code != NULL &&
                       strcmp(e->doom_last_code, reply->tool_code) == 0)
                          ? 1 : 0;
  uint8_t fresh_input = (e->users_seen_seq != e->cell_users_seq) ? 1 : 0;
  uint8_t tripped = 0;
  uint8_t streak = guards_doom_next(e->doom_streak, identical, fresh_input,
                                    &tripped);
  if (tripped != 0) {
    /* THE TRIP: the close rides ONE batch (control + turn.end), the cell
       never runs, the engine ends failed — the frame stays resumable (the
       fold's answer carries the final streak count for the close's text). */
    (void)_loop_doom_close(f, e, streak);
    model_reply_destroy(reply);
    _frame_engine_terminate(f, 0,
                            "doom-loop: the model repeated the same cell "
                            "too many times");
    return;
  }
  e->doom_streak = streak;
  free(e->doom_last_code);
  e->doom_last_code = strdup(reply->tool_code);   /* dispatch-side memory:
                                                     the reply dies after
                                                     this path */
  if (e->doom_last_code == NULL) {
    /* The copy failed: the identity fact degrades to "never identical"
       (the streak resets to 1 on a NULL last_code next fold — never a
       FALSE trip) — log loud and go on (the cell is still run). */
    log_error("loop: out of memory copying the cell doom identity at '%s'",
              frame_sid(f));
  }
  e->cell_users_seq = e->users_seen_seq;
  _loop_post_cell_run(f, e, reply);
}

/* The engine's turn-finish main payload for a plan turn's close: the
   assistant msg.append (the plan text — the content path's ordinary main
   record), or the empty-turn control when the reply carried no content.
   Both mirrors of `_frame_engine_finish_post`'s main-record compose (the
   shape the derive projects exactly the same way). */
static json_value_t* _loop_plan_main_payload(const char* content) {
  if (content == NULL) {
    /* The empty-turn control event (the exact payload _loop_control
       composes; the standing finish shape). */
    json_value_t* c = json_new_object();
    if (c == NULL) return NULL;
    json_object_set(c, "kind", json_new_string("empty-turn"));
    json_object_set(c, "text", json_new_null());
    return c;
  }
  json_value_t* m = json_new_object();
  if (m == NULL) return NULL;
  json_object_set(m, "role", json_new_string("assistant"));
  json_object_set(m, "content", json_new_string(content));
  return m;
}

/* The ladder's control payload ({kind, auto} + the standing control shape's
   null-text key). kind "plan-requested" rides the PLAN gate's close
   (auto null — no decision happened); "plan-approved" rides the approval
   consume (auto false) and the BYPASS close (auto true) — the spec §2.2/
   §2.3 durable phase transitions. NULL on OOM — loud. */
static json_value_t* _loop_plan_control_payload(const char* kind, int auto_ok,
                                                uint8_t with_auto) {
  json_value_t* c = json_new_object();
  if (c == NULL) return NULL;
  json_object_set(c, "kind", json_new_string(kind));
  if (with_auto != 0) {
    json_object_set(c, "auto", json_new_bool(auto_ok));
  }
  return c;
}

/* The ladder's runtime-authored plan gate (escalation spec §2.2; Task 5):
   ONE fire-and-post batch — [step.start, control{plan-requested},
   msg.append(plan text), the "ask" record {question, options, plan:<the
   cap-capped text>}, step.end, turn.end{blocked}] — the FINISH batch's
   rider-group shape (this batch is the plan content turn's whole cycle) with
   the gate's records in it; the ask and its turn's close can never split
   across a crash boundary. The ask parks BEFORE the compose (the gate is
   runtime-authored: corr 0, plan_gate 1, the question/options the engine
   owns); on rc == 0 the engine rests in FRAME_PHASE_ASK and the pyrt park
   flag rides (a LIVING cell would be refused — one park at a time; a parked
   plan turn has none). A refusal leaves NOTHING behind (the batch's
   pre-allocation rolled back — the standing fire-and-post discipline) and
   the caller clears the half-boxed ask and fails the turn loud: the parked
   state's truth rides committed records ONLY.
   content = the plan turn's model text (borrowed; NULL = the empty-turn
   control, the finishing shape). Returns the pre-post rc (0 = posted). */
static int _loop_plan_gate_post(frame_t* f, frame_engine_state_t* e,
                                const char* content) {
  /* The park: the mint (root allocator — no collisions), then the OWNED
     strings (the FRM_ASK receipt's boxing shape; corr is 0 — no bridge
     reply sink authored this ask). */
  char minted[9];
  _frame_engine_ask_id_mint(f, minted);
  if (minted[0] != '\0') e->pending_ask.ask_id = strdup(minted);
  e->pending_ask.question = strdup("Approve this plan?");
  e->pending_ask.options =
      (char**)get_clear_memory(2 * sizeof(char*));
  e->pending_ask.noptions = 2;
  if (e->pending_ask.options != NULL) {
    e->pending_ask.options[0] = strdup("Approve");
    e->pending_ask.options[1] = strdup("Reject");
  }
  if (e->pending_ask.ask_id == NULL || e->pending_ask.question == NULL ||
      e->pending_ask.options == NULL || e->pending_ask.options[0] == NULL ||
      e->pending_ask.options[1] == NULL) {
    log_error("loop: out of memory boxing the plan gate's ask at '%s'",
              frame_sid(f));
    _frame_engine_ask_clear(f);
    return -1;
  }
  e->pending_ask.plan_gate = 1;

  /* The ask record's plan renders the content CAP-CAPPED (an OUTPUT render —
     the budget table's bridge value cap + the truncate-marker helper; an
     oversized plan asks in the marked shape, never silently). */
  char* plan_capped = NULL;
  uint8_t plan_trunc = 0;
  budget_truncate_with_marker((content != NULL) ? content : "",
                              SA_BUDGET_BRIDGE_VALUE_BYTES, &plan_capped,
                              &plan_trunc);
  if (plan_capped == NULL) {
    log_error("loop: out of memory rendering the plan gate's plan text at "
              "'%s'", frame_sid(f));
    _frame_engine_ask_clear(f);
    return -1;
  }

  json_value_t* control = _loop_plan_control_payload("plan-requested", 0, 0);
  json_value_t* main = _loop_plan_main_payload(content);
  json_value_t* ask = json_new_object();
  json_value_t* options = (ask != NULL) ? json_new_array() : NULL;
  json_value_t* step_start =
      lifecycle_step_json(e->turn_counter, 1);
  json_value_t* step_end =
      lifecycle_step_json(e->turn_counter, 1);
  json_value_t* turn_end =
      lifecycle_turn_end_json(e->turn_counter, LIFE_REASON_BLOCKED, NULL);
  if (ask != NULL) {
    if (options != NULL) {
      json_object_set(ask, "kind", json_new_string("ask"));
      json_object_set(ask, "askId",
                      json_new_string(e->pending_ask.ask_id));
      json_object_set(ask, "question",
                      json_new_string(e->pending_ask.question));
      for (size_t i = 0; i < e->pending_ask.noptions; i++) {
        json_array_append(options,
                          json_new_string(e->pending_ask.options[i]));
      }
      json_object_set(ask, "options", options);   /* takes the value */
      json_object_set(ask, "plan",
                      (plan_capped[0] != '\0')
                          ? json_new_string(plan_capped)
                          : json_new_null());
    } else {
      /* options OOM'd: the half-composed ask record is dropped; the failure
         below clears the park. */
      json_value_destroy(ask);
      ask = NULL;
    }
  }
  if (control == NULL || main == NULL || step_start == NULL ||
      step_end == NULL || turn_end == NULL || ask == NULL) {
    /* The destroyers are NULL-tolerant; the parked ask's OWNED strings (and
       any options array living inside `ask`) die with the records here. */
    json_value_destroy(control);
    json_value_destroy(main);
    json_value_destroy(ask);
    json_value_destroy(step_start);
    json_value_destroy(step_end);
    json_value_destroy(turn_end);
    free(plan_capped);
    log_error("loop: out of memory composing the plan gate's close at '%s'",
              frame_sid(f));
    _frame_engine_ask_clear(f);
    return -1;
  }
  const char* names[6] = {LIFE_EVENT_STEP_START, "control",
                          (content != NULL) ? "msg.append" : "control",
                          "ask", LIFE_EVENT_STEP_END, LIFE_EVENT_TURN_END};
  json_value_t* payloads[6] = {step_start, control, main, ask, step_end,
                               turn_end};
  int rc = _frame_event_batch_post_fire(f, names, payloads, 6, "plan gate");
  free(plan_capped);   /* the record owned its own copy */
  if (rc == 0) {
    /* The close POSTED — the compose-time facts follow and the engine PARKS
       (the same shape as FRAME_PHASE_MODEL): the driver breaks on the park;
       a reply's FRM_TURN reposts. The pyrt park flag rides post-commit (a
       living cell would be refused — no second park can exist). */
    e->turn_open = 0;
    e->step_open = 0;
    e->phase = FRAME_PHASE_ASK;
    _frame_engine_pyrt_park_set(f, 1);
  }
  return rc;
}

/* The BYPASS plan turn's auto-approval close (escalation spec §2.3; Task 5):
   the plan turn still runs and its plan text stays a durable audit artifact,
   but the gate AUTO-APPROVES — ONE fire-and-post batch [step.start,
   msg.append(plan text), control{kind "plan-approved", auto true}, step.end,
   turn.end{completed}], NEVER the finish trip (the status put and the END
   rule stay a later content turn's business — this close must NOT end the
   engine). A refusal leaves the open tail standing (the caller fails the
   turn loud). Returns the pre-post rc (0 = posted; the compose-time flags
   cleared). */
static int _loop_bypass_plan_close_post(frame_t* f, frame_engine_state_t* e,
                                        const char* content) {
  json_value_t* control = _loop_plan_control_payload("plan-approved", 1, 1);
  json_value_t* main = _loop_plan_main_payload(content);
  json_value_t* step_start = lifecycle_step_json(e->turn_counter, 1);
  json_value_t* step_end = lifecycle_step_json(e->turn_counter, 1);
  json_value_t* turn_end =
      lifecycle_turn_end_json(e->turn_counter, LIFE_REASON_COMPLETED, NULL);
  if (control == NULL || main == NULL || step_start == NULL ||
      step_end == NULL || turn_end == NULL) {
    json_value_destroy(control);
    json_value_destroy(main);
    json_value_destroy(step_start);
    json_value_destroy(step_end);
    json_value_destroy(turn_end);
    log_error("loop: out of memory composing the bypass plan's close at '%s'",
              frame_sid(f));
    return -1;
  }
  const char* names[5] = {LIFE_EVENT_STEP_START,
                          (content != NULL) ? "msg.append" : "control",
                          "control", LIFE_EVENT_STEP_END,
                          LIFE_EVENT_TURN_END};
  json_value_t* payloads[5] = {step_start, main, control, step_end, turn_end};
  int rc = _frame_event_batch_post_fire(f, names, payloads, 5,
                                        "bypass plan close");
  if (rc == 0) {
    e->turn_open = 0;
    e->step_open = 0;
  }
  return rc;
}

/* The content path (no tool call: the turn ends): msg.append — or, on an
   empty assistant turn, the empty-turn control event — and, when the turn
   ENDS the frame, the meta/status=done put in ONE atomic FRAME_STORE_FINISH
   batch. The reply takes the END rule: live children pending → the CHILDREN
   yield (status stays "running"); a child with none → the quiet-completion
   terminate; a top frame → the engine ends (done rode the batch).
   THE LADDER's fork (escalation spec §2.2/§2.3; Task 5): a plan turn
   (gating — PLAN_ASK_ACT/BYPASS pre-approval) never reaches the finish
   (which would END the frame): its close composes the PHASE TRANSITION
   instead — the runtime-authored gate ask (park) or the bypass's
   auto-approval (continue). */
static void _loop_content_path(frame_t* f, frame_engine_state_t* e,
                               model_reply_t* reply) {
  /* The content is read BEFORE the reply dies (the finish batch composes its
     own copies — the batch's value is json-serialized text, not the reply's
     pointer), and the reply is destroyed after the batch composing used it. */
  const char* content =
      (reply->content != NULL && reply->content[0] != '\0') ? reply->content : NULL;
  /* THE LADDER'S GATE (escalation spec §2.2/§2.3): a plan turn's close
     composes the PHASE TRANSITION, never the finish (which would END the
     frame — a plan approval must keep the engine for its act phase). */
  uint8_t gating = _loop_ladder_gating(f, e);
  if (gating != 0) {
    unsigned mode = _frame_escalation_mode(f);
    if (mode == (unsigned)FRAME_ESCALATION_BYPASS) {
      int brc = _loop_bypass_plan_close_post(f, e, content);
      model_reply_destroy(reply);
      if (brc != 0) {
        _loop_fail(f, e, "commit-error", "the bypass plan close was refused");
        return;
      }
      /* The auto-approval committed: the ladder moves to ACT and the engine
         CONTINUES — the FRM_TURN repost carries the continuation_queued
         latch (the parked-ask reply's idiom). */
      e->ladder_act = 1;
      (void)_loop_post_turn(f);
      return;
    }
    int grc = _loop_plan_gate_post(f, e, content);
    model_reply_destroy(reply);
    if (grc == 0) {
      return;   /* parked (FRAME_PHASE_ASK set post-commit by the composer) */
    }
    /* The close was refused pre-post (logged; the seq range rolled back —
       the records committed NOTHING): the half-boxed ask dies with it and
       the turn fails loud — no half-parked state (the park's truth rides
       committed records only, spec §5). */
    _frame_engine_ask_clear(f);
    _loop_fail(f, e, "commit-error", "the plan gate close was refused");
    return;
  }
  /* The standing finish path (free/act turns — the ordinary shape,
     byte-identical). */
  /* The status put rides the batch ONLY when this turn ends the frame: a
     content turn while live children are pending yields at the finish reply
     (status stays "running"; the completing end writes the put — this batch
     when nothing raced, the engine's own fix-up otherwise). */
  int write_status = (_frame_is_child(f) == 0 && e->live_children == 0) ? 1 : 0;
  uint64_t corr = _frame_store_corr_next(f);
  int frc = _frame_engine_finish_post(f, content, write_status,
                                      corr, _frame_actor(f));
  /* The outcome text rides the engine state to the finish reply's end rule
     (the CHILDREN yield, the quiet-completion bind, and the top end all
     consume or free it there). */
  free(e->finish_text);
  e->finish_text = (content != NULL) ? strdup(content) : NULL;
  model_reply_destroy(reply);
  if (frc != 0) {
    /* Pre-post refusal (already logged loud; the seq rolled back): the
       turn's message + completion never committed. */
    _loop_fail(f, e, "commit-error", "turn finish batch refused");
    return;
  }
  e->phase = FRAME_PHASE_STORE;
  e->store_kind = FRAME_STORE_FINISH;
  e->store_corr = corr;
  /* yield: the FINISH reply takes the END rule */
}

/* The retry table's class-name surface (the failure close's text carries
   the cause's name: "model-error-final: <class>: <detail>"). Borrowed
   literals; the guards_cause_e → text table. */
static const char* _loop_cause_name(guards_cause_e cause) {
  switch (cause) {
    case GUARDS_CAUSE_TRANSPORT: return "transport";
    case GUARDS_CAUSE_SERVER: return "server";
    case GUARDS_CAUSE_RATE: return "rate";
    case GUARDS_CAUSE_OVERLOAD: return "overload";
    case GUARDS_CAUSE_OVERFLOW: return "overflow";
    default: return "fallback";
  }
}

/* The reply processing, shared by the sync and the async arrival paths —
   "scripted backends drive the SAME code". The GUARDS TABLE (guards spec
   §2) drives the model-failure branch: the cause folds the failure's
   observed facts (http_status — the async arrival passes the payload's
   status, the sync drain passes 0 = a scripted sync backend speaks no
   HTTP — and a 2xx decode's finish_reason), the class's plan decides
   retry-vs-fail, and a retry's repost carries the class's backoff: 0 =
   the immediate FRM_TURN repost (today's shape, the re-derive provably
   equivalent — the derive is stateless from the store), > 0 = the
   one-shot delayed-post timer's same continuation. Retryable exhaustion
   or a non-retryable class ends the engine failed with control
   "model-error-final" carrying "<class>: <detail>". */
static void _frame_engine_reply(frame_t* f, frame_engine_state_t* e,
                                int rc, model_reply_t* reply, char* err,
                                int http_status, unsigned retry_after_sec) {
  if (rc != 0 || reply == NULL) {
    const char* detail =
        (err != NULL && err[0] != '\0') ? err : "backend returned no reply";
    /* The cause folds the failure's observed facts FIRST — a non-retryable
       class must never take the retry path. The finish_reason guard is
       belt-and-braces only: the decode NULLs the reply on every error
       shape (the overload shape DECODES fine and is the success path's
       guest below). */
    guards_cause_e cause = guards_retry_cause(
        http_status,
        (reply != NULL) ? reply->finish_reason : NULL);
    const guards_retry_plan_t* plan = guards_retry_plan(cause);
    if (plan->retryable != 0 && e->model_retries < plan->cap) {
      unsigned backoff = guards_retry_backoff_ms(plan, e->model_retries,
                                                 retry_after_sec);
      _loop_control(f, "model-error", detail);
      free(err);
      if (reply != NULL) model_reply_destroy(reply);
      e->model_retries = (uint8_t)(e->model_retries + 1);
      /* The repost: the SAME continuation, ONE fresh FRM_TURN (the model-
         RETRY machinery is untouched — model_retry_step = 1 skips the
         entry checks + the count; the turn is never renumbered). backoff
         0 = today's immediate repost (the standing re-derive equivalence);
         > 0 = the delayed post of the same continuation — the table's
         wait is an optimization layered on the SAME state machine. */
      e->model_retry_step = 1;
      if (backoff == 0) {
        (void)_loop_post_turn(f);
      } else if (_frame_delayed_post(f, backoff) != 0) {
        log_error("loop: the retry's delayed post refused at '%s' — the "
                  "retry reposts immediately instead (the bounded wait is "
                  "an optimization; never a correctness dependency)",
                  frame_sid(f));
        (void)_loop_post_turn(f);   /* the optimization degrades, never the
                                       correctness */
      }
      return;
    }
    /* Exhausted retries or a non-retryable class: the class's name rides
       the final control's text (the standing loud-wording rule). */
    char final_text[640];
    snprintf(final_text, sizeof(final_text), "%s: %s",
             _loop_cause_name(cause), detail);
    _loop_fail(f, e, "model-error-final", final_text);
    free(err);
    if (reply != NULL) model_reply_destroy(reply);
    return;
  }
  free(err);
  e->model_retries = 0;       /* fresh retry budget per successful call */

  /* The OVERLOAD shape (the load-stop decoded "fine": rc == 0, an empty
     reply — the success path's guest): never a content turn, never a
     retry — the frame-tree slice's flagged follow-up fails LOUD (spec §2;
     the old code turned a zero-token load-stop into empty-turn/done). */
  if (guards_retry_cause(http_status, reply->finish_reason) ==
      GUARDS_CAUSE_OVERLOAD) {
    _loop_fail(f, e, "model-error-final",
               "overload: the provider answered with a load-stop");
    model_reply_destroy(reply);
    return;
  }

  if (reply->tool_code != NULL) {
    /* THE LADDER's plan turn refuses tool-call replies (escalation spec
       §2.2; Task 5): the request ran tools-NULL (no tool surface was
       offered), so a tool-call reply is an unexpected shape at a plan turn.
       Loud — `_loop_fail` ends the turn error (the control's wording rides
       the failure close); a LATER run over this gate-free state re-enters
       plan (ladder_act never flipped — nothing happened). */
    if (_loop_ladder_gating(f, e) != 0) {
      _loop_fail(f, e, "plan-mode", "the model sent tool calls in a plan turn");
      model_reply_destroy(reply);
      return;
    }
    _loop_tool_path(f, e, reply);
    return;
  }
  _loop_content_path(f, e, reply);
}

/* The model path (the derive continuation's tail, shared by BOTH arrivals —
   the persona-less derive and the persona trip's reply): project with the
   engine's persona prefix (NULL = the built-in base alone, today's exact
   bytes) and take the submit/complete path. CONSUMES `events` on every
   path.
   THE LADDER's tools shape (escalation spec §2.2; Task 5): a PLAN turn's
   request is built tools-NULL — the tools argument is a JSON NULL VALUE
   (model.c's `_model_request_body` no-tools shape: both `tools` and
   `tool_choice` are OMITTED from the request document — the refine slice's
   no-tools contract, reused verbatim) and the plan instruction rides the
   system prompt. The NULL POINTER keeps the canned `execute` tool (every
   free/act turn's unchanged bytes). */
static void _loop_engine_model_path(frame_t* f, frame_engine_state_t* e,
                                    json_value_t* events) {
  uint8_t gating = _loop_ladder_gating(f, e);
  json_value_t* messages =
      _loop_project(f, events, e->persona_prefix,
                    (gating != 0) ? SA_LOOP_PLAN_INSTRUCTION : NULL);   /* consumes the DOM */
  if (messages == NULL) {
    log_error("loop: the projection failed at '%s'", frame_sid(f));
    _loop_fail(f, e, "derive-error", NULL);
    return;
  }

  model_backend_t* mb = _frame_backend_get(f);
  if (mb == NULL) {
    log_error("loop: '%s' has no usable model backend", frame_sid(f));
    _loop_fail(f, e, "model-missing", NULL);
    json_value_destroy(messages);
    return;
  }

  /* The request's tools: NULL POINTER = the canned execute tool (today);
     a JSON NULL VALUE = explicitly no tools at all (gating plan turns).
     The value dies right after the submit/complete — model.h's contract
     says the backend copies everything it needs before returning. */
  json_value_t* tools_arg = (gating != 0) ? json_new_null() : NULL;

  if (mb->submit != NULL) {
    /* The ASYNC shape (Task 4's http submit): rc 0 → the sink fires EXACTLY
       ONCE (FRM_MODEL_RESULT) and the engine yields in FRAME_PHASE_MODEL;
       rc != 0 = rejected before any I/O — the sink will NEVER fire. */
    /* The lifetime handoff's begin: ONE pending-submit slot held from
       BEFORE the submit until the sink's release (frame.c's claim protocol)
       — frame_destroy mid-turn then DEFERS its teardown to that sink's last
       release instead of freeing the record under the completion it still
       carries. The sync path (below) never acquires: only a real in-flight
       submit counts. `_frame_engine_submit_settle` right after the submit
       clears the in-flight marker and — a destroy having raced the call —
       runs the deferred teardown on this, the record's last-owner thread:
       a settle returning 1 means the record is GONE and nothing of `f`/`e`
       may follow that return. */
    _frame_engine_submit_begin(f);
    int src = mb->submit(mb, messages, tools_arg, _loop_model_sink, f);
    json_value_destroy(messages);
    json_value_destroy(tools_arg);
    if (_frame_engine_submit_settle(f) != 0) return;
    if (src != 0) {
      /* Rejected before any I/O: the sink will never fire, so THIS caller
         releases its slot. A concurrent destroy's deferral ends here too:
         a release that returns 1 means the record is GONE — the engine
         state died inside the release and nothing of `f` may follow. */
      if (_frame_engine_submit_release(f) != 0) return;
      log_error("loop: the model submit was rejected (the sink will never "
                "fire) at '%s'", frame_sid(f));
      _loop_fail(f, e, "submit-failed", NULL);
      return;
    }
    e->phase = FRAME_PHASE_MODEL;
    return;                    /* yield: the completion arrives as a message */
  }

  /* The SYNC shape (every scripted test backend): complete() runs INLINE
     inside this dispatch — blocking the actor, acceptable ONLY on the
     documented inline/test driver (§6), never on a pool worker in
     production (a production backend implements submit). raw_out = NULL:
     the documented NULL-tolerant body out-param — the engine keeps only the
     parsed reply. The derived array's lifetime ends here either way (the
     model-error retry re-derives; nothing retains it). */
  model_reply_t* reply = NULL;
  char* err = NULL;
  int crc = mb->complete(mb, messages, tools_arg, NULL, &reply, &err);
  json_value_destroy(messages);
  json_value_destroy(tools_arg);
  /* The sync drain has NO status facts: a scripted sync backend speaks no
     HTTP — complete()'s rc is its own contract, not a status code — so the
     arrival passes 0/0 = absent both ways. The guards table then reads the
     fallback class for a sync failure, which keeps the standing once-only
     retry rule byte-identical (Task 5). */
  _frame_engine_reply(f, e, crc, reply, err, 0, 0);
}

/* --- the derive's PERSONA TRIP (the persona slice, spec §3) ---------------
   A persona-carrying frame reads the persona's two state records with ONE
   second bounded store trip before the model path: FRM_STORE_GET_NAMED over
   the absolute root-level keys personas/<name>/record and
   personas/<name>/user-context. The reply (FRAME_STORE_PERSONA) composes
   the persona GROUP into e->persona_prefix — the fallback (a refused read,
   a missing record, a record failing the rules) answers the built-in base
   alone, LOUD: a persona is presentation, and a turn NEVER fails on a
   persona read. The parsed derive DOM parks on e->derive_events across the
   trip (the finish_text lifetime rule; a dead engine frees it). */

/* The persona record-key shape: ONE path segment (a '/' in the name would
   compose a foreign subtree; the bound keeps the snprintf honest). */
#define _LOOP_PERSONA_NAME_MAX 256

static int _loop_persona_key_ok(const char* persona_name) {
  return persona_name != NULL && persona_name[0] != '\0' &&
         strlen(persona_name) <= _LOOP_PERSONA_NAME_MAX &&
         strchr(persona_name, '/') == NULL;
}

/* One subtree key under the persona's name (the absolute root-level text
   "personas/<name>/<leaf>"): a heap string the caller frees. */
static char* _loop_persona_key(const char* persona_name, const char* leaf) {
  char* out = get_memory(strlen("personas/") + strlen(persona_name) +
                         strlen(leaf) + 2);
  snprintf(out, strlen("personas/") + strlen(persona_name) + strlen(leaf) + 2,
           "personas/%s/%s", persona_name, leaf);
  return out;
}

/* The persona trip's post (_loop_post_derive's second-trip sibling): the
   GET_NAMED read of the persona subtree's TWO keys, posted as a FRAME_
   STORE_PERSONA round trip. The parked DOM must already sit on
   e->derive_events. */
static void _loop_post_persona(frame_t* f, frame_engine_state_t* e) {
  const char* persona = (f != NULL) ? _frame_persona_name(f) : NULL;
  if (persona == NULL || !_loop_persona_key_ok(persona)) {
    /* Unreachable through the calling branch (it checks first) — a
       defensive loud fail, never a silent wrong trip. */
    log_error("loop: the persona trip at '%s' carries no valid persona name",
              (f != NULL) ? frame_sid(f) : "?");
    _loop_fail(f, (f != NULL) ? e : NULL, "derive-error", NULL);
    return;
  }
  char* record_key = _loop_persona_key(persona, "record");
  char* context_key = _loop_persona_key(persona, "user-context");
  frm_store_get_named_payload_t* gp =
      (frm_store_get_named_payload_t*)get_clear_memory(sizeof(*gp));
  if (record_key == NULL || context_key == NULL || gp == NULL) {
    log_error("loop: out of memory composing the persona read at '%s'",
              (f != NULL) ? frame_sid(f) : "?");
    free(record_key);
    free(context_key);
    free(gp);
    _loop_fail(f, e, "derive-error", NULL);
    return;
  }
  gp->keys = (char**)get_clear_memory(2 * sizeof(char*));
  if (gp->keys == NULL) {
    log_error("loop: out of memory composing the persona read at '%s'",
              frame_sid(f));
    free(record_key);
    free(context_key);
    free(gp);
    _loop_fail(f, e, "derive-error", NULL);
    return;
  }
  gp->keys[0] = record_key;
  gp->keys[1] = context_key;
  gp->nkeys = 2;
  gp->reply_to = _frame_actor(f);
  gp->corr = _frame_store_corr_next(f);
  e->phase = FRAME_PHASE_STORE;
  e->store_kind = FRAME_STORE_PERSONA;
  e->store_corr = gp->corr;
  _frame_post(_frame_store_actor(f), (uint32_t)FRM_STORE_GET_NAMED, gp,
              frm_store_get_named_payload_destroy, "persona read");
}

/* The reply array's named lookup: the value text of the entry whose "key"
   matches (borrowed, lives while the array does), NULL when the key is
   absent or its value is null — ABSENCE IS THE ANSWER (the store's GET_
   NAMED shape: no positional guessing). */
static const char* _loop_get_named_value(json_value_t* array,
                                         const char* key) {
  for (size_t i = 0; i < json_size(array); i++) {
    json_value_t* entry = json_at(array, i);
    if (entry == NULL) continue;
    json_value_t* k = json_get(entry, "key");
    if (k == NULL || strcmp(json_as_string(k), key) != 0) continue;
    json_value_t* v = json_get(entry, "value");
    if (v == NULL || json_type(v) == JSON_NULL) return NULL;
    return json_as_string(v);
  }
  return NULL;
}

/* The persona GROUP's compose from the fetched records: the voice record
   loads (a refusal is LOUD and answers NULL = the built-in base alone —
   the spec §2 fallback, never a half-persona render) and composes with the
   turn instruction as the base — placement "first" rides the group up
   front, "below" the base ahead; the compose's byte rules (persona spec
   §2) pin the rest. */
static char* _loop_persona_compose(frame_t* f, const char* record_json,
                                   const char* context_text) {
  persona_record_t* record = NULL;
  if (persona_record_load(record_json, &record) != 0) {
    log_error("loop: the persona '%s' at '%s' failed the record rules — the "
              "built-in base rides this derive (a persona is presentation; "
              "the turn NEVER fails on a persona read)",
              (f != NULL) ? _frame_persona_name(f) : "?",
              (f != NULL) ? frame_sid(f) : "?");
    return NULL;
  }
  char* composed = persona_compose(record, context_text, _LOOP_TOOL_IDS,
                                   _LOOP_TOOL_IDS_N, SA_LOOP_INSTRUCTION);
  persona_record_destroy(record);
  return composed;
}

/* The FRAME_STORE_PERSONA reply's continuation: the persona read's answer —
   ONE JSON array record with the values' keys attached — composes the
   engine's persona_prefix (the fallback = the base alone, loud), unparks
   the derive DOM, and continues to the model path. */
static void _loop_engine_on_persona(frame_t* f, frame_engine_state_t* e,
                                    frm_store_reply_payload_t* r) {
  json_value_t* events = e->derive_events;
  e->derive_events = NULL;
  if (events == NULL) {
    /* Defensive only (the trip posts the DOM before the reply can route):
       a parked DOM gone means incoherent engine state — loud fail. */
    log_error("loop: the persona reply at '%s' found no parked derive DOM",
              frame_sid(f));
    _loop_fail(f, e, "derive-error", NULL);
    return;
  }

  const char* persona = _frame_persona_name(f);
  char* prefix = NULL;
  json_value_t* array = NULL;
  char* perr = NULL;
  if (r->rc != 0 || r->n != 1 || r->records[0] == NULL) {
    log_error("loop: the persona read at '%s' was refused by the store (%d)"
              " — the built-in base rides this derive (a persona is "
              "presentation; the turn NEVER fails on a persona read)",
              frame_sid(f), r->rc);
  } else {
    array = json_parse(r->records[0], strlen(r->records[0]), &perr);
    if (perr != NULL) free(perr);
    if (array == NULL || json_type(array) != JSON_ARRAY) {
      log_error("loop: the persona read's reply at '%s' is not the GET_"
                "NAMED array — the built-in base rides this derive (a "
                "persona is presentation; the turn NEVER fails on a persona "
                "read)", frame_sid(f));
      if (array != NULL) {
        json_value_destroy(array);
        array = NULL;
      }
    }
  }
  if (array != NULL) {
    char* record_key = _loop_persona_key(persona, "record");
    char* context_key = _loop_persona_key(persona, "user-context");
    const char* record_json = (record_key != NULL)
        ? _loop_get_named_value(array, record_key) : NULL;
    const char* context_text = (context_key != NULL)
        ? _loop_get_named_value(array, context_key) : NULL;
    free(record_key);
    free(context_key);
    if (record_json == NULL) {
      /* NOT installed: the stable missing-persona state — the base alone,
         loud (spec §2: never silent, never a half-persona render). */
      log_error("loop: the persona '%s' carries no installed record (the "
                "store answered null) at '%s' — the built-in base rides "
                "this derive (a persona is presentation; the turn NEVER "
                "fails on a persona read)", persona, frame_sid(f));
    } else {
      prefix = _loop_persona_compose(f, record_json, context_text);
    }
  }
  json_value_destroy(array);
  e->persona_prefix = prefix;
  _loop_engine_model_path(f, e, events);
}

/* The FRAME_STORE_DERIVE reply's continuation: parse the materialized raw
   records into the DOM (µs, bounded 512, unparseable dropped loud — the old
   frame_debug_events tail's shape MOVED here: the store worker carried raw
   texts), run the UNCHANGED two-pass projection, and take the model path —
   with the persona trip between them for a persona-carrying frame (the
   spec §3 second store trip; a persona-less frame continues directly, no
   extra store traffic, byte-identical). */
static void _loop_engine_on_derive(frame_t* f, frame_engine_state_t* e,
                                   frm_store_reply_payload_t* r) {
  if (r->rc != 0) {
    log_error("loop: the derive scan at '%s' was refused by the store (%d) — "
              "nothing can be derived", frame_sid(f), r->rc);
    _loop_fail(f, e, "derive-error", NULL);
    return;
  }
  json_value_t* events = json_new_array();
  if (events == NULL) {
    log_error("loop: out of memory building the derive DOM at '%s'",
              frame_sid(f));
    _loop_fail(f, e, "derive-error", NULL);
    return;
  }
  for (size_t i = 0; i < r->n; i++) {
    char* perr = NULL;
    json_value_t* rec = json_parse(r->records[i], strlen(r->records[i]), &perr);
    if (perr != NULL) free(perr);
    if (rec == NULL) {
      log_error("loop: unparseable event record dropped at '%s'", frame_sid(f));
      continue;
    }
    json_array_append(events, rec);
  }

  /* --- the doom reset's input (guards spec §1) ---------------------------
     The newest USER-role msg.append seq this derive saw. RECORD SHAPE (the
     frozen contract, frame.c's _frame_event_json_full): every event record
     carries its OWN seq as a JSON int — so seq-based tracking is exact and
     the count fallback never entered the codebase. The scan takes the MAX
     (order-proof even though the DOM arrives ascending), skipping
     malformed records under the render-not-crash rule. */
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (rec == NULL) continue;
    json_value_t* type_v = json_get(rec, "type");
    if (type_v == NULL) continue;
    if (strcmp(json_as_string(type_v), "msg.append") != 0) {
      /* --- the ladder's approval consult (escalation spec §2.4; Task 5) --
         The derive is the ONE stateless re-deriver: a plan-approved control
         record in the scanned window flips the engine to ACT — the
         approval's lifetime is the record (durable), so a resumed run
         re-enters act without re-asking. A plan-requested control (a
         standing, UNapproved gate) does NOT set it. WINDOW NOTE (the
         recorded safe default): the derive is bounded to the newest
         SA_FRAME_DEBUG_MAX_EVENTS records — a frame whose approval scrolled
         past the window re-gates, and re-asking is the SAFE default. */
      if (strcmp(json_as_string(type_v), "control") == 0) {
        json_value_t* payload = json_get(rec, "payload");
        json_value_t* kind = (payload != NULL) ? json_get(payload, "kind")
                                               : NULL;
        if (kind != NULL &&
            strcmp(json_as_string(kind), "plan-approved") == 0) {
          e->ladder_act = 1;
        }
      }
      continue;
    }
    json_value_t* payload = json_get(rec, "payload");
    json_value_t* role = (payload != NULL) ? json_get(payload, "role") : NULL;
    if (role == NULL || strcmp(json_as_string(role), "user") != 0) {
      continue;   /* the assistant's own appends are never fresh input */
    }
    json_value_t* seq_v = json_get(rec, "seq");
    if (seq_v == NULL || json_type(seq_v) != JSON_INT ||
        json_as_int(seq_v) < 0) {
      continue;
    }
    uint64_t seq = (uint64_t)json_as_int(seq_v);
    if (seq > e->users_seen_seq) e->users_seen_seq = seq;
  }

  /* --- THE TURN ENTRY (Task 2 rider 1; spec §3) ---------------------------
     After the turn cap check passed (it gates the FRM_TURN that posted this
     derive — the check runs in _frame_engine_turn) and BEFORE the model
     dispatch, the engine opens the turn: ONE turn.start record, fire-and-
     post (the control events' discipline — a refusal logs loud and the
     engine continues). The entry runs INSIDE this reply dispatch because
     the counter's lazy restore READS the derive's scanned events (no extra
     round trip): the FIRST entry of an engine run restores the counter
     (newest recorded turn + 1 — the _frame_restore_seq discipline: gaps
     loud, never a lock, never a second writer), later entries +1. A model-
     RETRY's re-derive finds the turn open and skips the entry (the retry is
     the SAME turn — never a renumber). A DEAD engine never carries the
     counter across a restart (frame_start resets; the next run's first
     entry re-restores through the log). */
  if (!e->turn_open) {
    if (!e->turn_known) {
      e->turn_counter = _loop_turn_counter_restore(f, events);
      e->turn_known = 1;
    } else {
      e->turn_counter += 1;
    }
    _loop_turn_entry(f, e);
  }

  /* --- the persona trip (persona spec §3) --------------------------------
     A persona-carrying frame parks the parsed DOM and posts the SECOND
     bounded store trip (the named read of the persona subtree's records)
     BEFORE the model path; the trip's reply composes the persona GROUP
     into e->persona_prefix and continues to the model path. A persona-less
     frame (no name, the empty-none normalization, an impossible key) takes
     the model path DIRECTLY: no extra store traffic, byte-identical
     prompt. The prefix rebuilt per derive — the records are STATE that can
     change between turns; the old one dies here. */
  const char* persona = _frame_persona_name(f);
  if (_loop_persona_key_ok(persona)) {
    free(e->persona_prefix);
    e->persona_prefix = NULL;
    e->derive_events = events;   /* parked until the persona reply */
    _loop_post_persona(f, e);
    return;                      /* yield: the trip's reply continues */
  }
  if (persona != NULL && persona[0] != '\0') {
    /* A configured name that cannot form the key (interior '/', over the
       bound): the loud base-only fallback — the turn NEVER fails on a
       persona read. */
    log_error("loop: the persona name '%s' at '%s' is not a valid record "
              "key — the built-in base rides this derive (a persona is "
              "presentation; the turn NEVER fails on a persona read)",
              persona, frame_sid(f));
  }
  _loop_engine_model_path(f, e, events);
}

/* The FRAME_STORE_CELL_RUN reply's continuation: rc != 0 → the old loop's
   audit rule at the reply; rc == 0 → dispatch FRM_CELL_EXECUTE (the old tool
   path verbatim: the unclaimed-payload check, model_reply_destroy) and
   yield in FRAME_PHASE_CELL on a pending cell — OR write the paired
   status-1 cell.result for a synchronous refusal (the audit-honesty fix)
   and resume the turn. */
static void _loop_engine_on_cell_run(frame_t* f, frame_engine_state_t* e, int rc) {
  if (rc != 0) {
    if (e->turn_reply != NULL) {
      model_reply_destroy(e->turn_reply);
      e->turn_reply = NULL;
    }
    e->step_open = 0;   /* the audit batch was refused — its step.start never
                           committed (the store commits nothing half of) */
    log_error("loop: cell.run event refused at '%s'", frame_sid(f));
    _loop_fail(f, e, "audit-error", "cell.run event refused");
    return;
  }
  if (e->turn_reply == NULL || e->turn_reply->tool_code == NULL) {
    log_error("loop: the cell.run round trip lost the model reply at '%s' — "
              "failing loud", frame_sid(f));
    _loop_fail(f, e, NULL, "the cell.run round trip lost the model reply");
    return;
  }

  frm_cell_payload_t* cp = get_clear_memory(sizeof(frm_cell_payload_t));
  if (cp == NULL) {
    log_error("loop: out of memory building the FRM_CELL_EXECUTE payload at "
              "'%s'", frame_sid(f));
    model_reply_destroy(e->turn_reply);
    e->turn_reply = NULL;
    _loop_fail(f, e, NULL,
               "out of memory building the cell execute payload");
    return;
  }
  cp->corr = e->turn_cell_corr;
  cp->code = strdup(e->turn_reply->tool_code);
  if (cp->code == NULL) {
    log_error("loop: out of memory copying the cell code at '%s'", frame_sid(f));
    frm_cell_payload_destroy(cp);
    model_reply_destroy(e->turn_reply);
    e->turn_reply = NULL;
    _loop_fail(f, e, NULL, "out of memory copying the cell code");
    return;
  }
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
  model_reply_destroy(e->turn_reply);
  e->turn_reply = NULL;

  if (_frame_cell_pending(f) != 0) {
    e->phase = FRAME_PHASE_CELL;   /* yield: PYRT_RESULT reposts the turn */
    return;
  }

  /* A synchronous refusal (pending never set — a second in-flight cell,
     a pyrt boot/execute refusal, corr 0): the ENGINE writes the PAIRED
     status-1 cell.result right here (the audit-honesty fix — the refused
     cell.run line's counterpart) with the envelope riders riding the SAME
     atomic batch (Task 2 rider 3 — the same composer the PYRT completion
     uses), then the next turn re-derives from it. */
  json_value_t* result_payload = json_new_object();
  if (result_payload == NULL) {
    log_error("loop: out of memory building the refused cell's paired "
              "cell.result at '%s'", frame_sid(f));
  } else {
    /* The refusal's wording: the generic refusal, or the poisoned runtime's
       own corr-matched failure data (frame.c's interrupt synthesis — the
       poison contract, spec §2). */
    const char* refusal_note = _frame_cell_refusal_text(f);
    json_object_set(result_payload, "corr",
                    json_new_int((int64_t)e->turn_cell_corr));
    json_object_set(result_payload, "status", json_new_int(1));
    json_object_set(result_payload, "text",
                    json_new_string((refusal_note != NULL) ? refusal_note
                                    : "cell refused before execution"));
    /* with_riders = the engine's compose-time facts: its open turn closes
       completed with the refusal's result (the cycle answered, the engine
       continues); the helper clears the flags when the batch posts. */
    _frame_engine_result_close_post(f, result_payload,
                                    (e->turn_open != 0) ? 1 : 0);
  }
  (void)_loop_post_turn(f);   /* resume */
}

/* The terminal step (spec §4): end the live engine; a CHILD (not already
   done, parent live) binds ONE frame.report with the outcome text — the
   engine-driven bind makes the child's router post FRM_CHILD_REPORT on the
   CONFIRMED commit — and an already-done child (the report verb bound
   everything; its engine ends at the next is_done check) posts just the
   resume. A TOP frame failure makes NO status change (the pinned
   cap-is-a-failure shape). A terminate re-entered on an already-ended engine
   is a loud no-op. Never silent. */
void _frame_engine_terminate(frame_t* f, uint8_t ok, const char* text) {
  frame_engine_state_t* e = _frame_engine_state(f);
  if (e == NULL || !e->engine_live) {
    log_error("loop: a terminal step at '%s' reached an already-ended engine "
              "— loud no-op", (f != NULL) ? frame_sid(f) : "?");
    return;
  }
  _loop_engine_end(f, e, ok ? 0 : 1);
  if (_frame_is_child(f) == 0) {
    return;   /* a top frame has no parent log to notify */
  }
  if (frame_is_done(f) == 0) {
    /* The bind: the child composes ITS report record (its pre-allocated
       seq) + the outcome text; the PARENT composes the whole cross-subtree
       batch (the child's record + the child's status=done + its bound
       report event) and the store executes it as ONE atomic commit; the
       reply routes back to THIS child, whose router posts FRM_CHILD_REPORT
       to the parent. */
    if (_frame_report_bind_post(f, 0, 1, ok ? 0 : 1,
                                (text != NULL) ? text : "") != 0) {
      /* The compose refused loud (nothing was posted) — the resume still
         fires: a parent must never hang because a WAL write failed. */
      log_error("loop: the terminal report bind for '%s' refused pre-post — "
                "the parent still resumes (never hang on a WAL failure)",
                frame_sid(f));
      _frame_child_notify_post(f, ok ? 0 : 1);
    }
    return;
  }
  /* Already done (the report verb's bind is confirmed committed): the resume
     is the only missing piece — post it directly. */
  _frame_child_notify_post(f, ok ? 0 : 1);
}

/* The FRAME_STORE_FINISH reply's continuation: rc != 0 → control
   "commit-error" + engine end failed; rc == 0 → the END rule (§1/§4):
   live children pending → the CHILDREN yield (each child report resumes);
   a child with none → its quiet-completion terminate (the content IS its
   outcome, reported to the parent); a top frame → the engine ends with the
   frame done. */
static void _loop_engine_on_finish(frame_t* f, frame_engine_state_t* e, int rc) {
  if (rc != 0) {
    log_error("loop: the turn finish batch was refused (%d) at '%s'",
              rc, frame_sid(f));
    /* The batch was REFUSED — nothing committed (the store's atomic rule);
       the turn is still open and the failure close below writes its
       turn.end {error, commit-error}. */
    _loop_fail(f, e, "commit-error", "turn finish batch refused");
    return;
  }
  /* The finish batch COMMITTED — with it the envelope's content-turn group
     (step.start/step.end/turn.end {completed} when the turn was open): the
     compose-time facts follow the store (the records are the truth). */
  e->turn_open = 0;
  e->step_open = 0;
  /* The turn's outcome text (consumed by every branch below). */
  char* text = e->finish_text;
  e->finish_text = NULL;
  if (e->live_children > 0) {
    /* THE YIELD (Task 5): a content turn while live children are pending
       does NOT end the frame — the engine goes quiet in FRAME_PHASE_CHILDREN
       (status stays "running"; frame_run_loop returns 2) and each child's
       report reposts the turn. */
    e->phase = FRAME_PHASE_CHILDREN;
    free(text);
    return;
  }
  if (_frame_is_child(f) != 0) {
    /* The child's quiet completion (spec §4): the content IS its outcome —
       the engine reports it to the parent (the bind's reply posts
       FRM_CHILD_REPORT) and the engine ends. */
    _frame_engine_terminate(f, 1, (text != NULL) ? text : "");
    free(text);
    return;
  }
  /* TOP, no live children: the engine ends. The status put RODE this batch —
     unless the yield shape raced (the batch composed while children were
     pending and the last child's report landed before this reply): an engine
     ending with live_children == 0 is done either way, so fire the status
     put now when the batch did not write it (fire-and-post; the store's FIFO
     commits it ahead of anything this engine could post after). */
  if (frame_is_done(f) == 0) {
    _frame_status_post_fire(f);
  }
  free(text);
  _loop_engine_end(f, e, 0);
}

int _frame_engine_start(frame_t* f) {
  if (!_frame_is_live(f)) {
    log_error("frame_start: dead frame");
    return -1;
  }
  frame_engine_state_t* e = _frame_engine_state(f);
  if (e == NULL || e->engine_live) {
    log_error("frame_start: an engine is already live at '%s' — ONE engine "
              "per frame",
              (f != NULL) ? frame_sid(f) : "?");
    return -1;
  }
  /* The per-run knobs (§1: turns_issued resets at frame_start — the cap
     stays a per-run bound, exactly like the old loop's; a full-run retry
     gets a fresh budget and fresh failed-knob state). live_children resets
     with the engine (spec's recorded restart limit: the count is THIS run's
     in-memory awaitables — children from a prior run are the restart/
     reconcile slice's business). */
  e->turns_issued = 0;
  e->model_retries = 0;
  e->model_retry_step = 0;
  e->ladder_act = 0;   /* the ladder re-consults the log's control records on
                          this run's FIRST derive (spec §2.4) — a restarted
                          engine never inherits the act phase in memory */
  atomic_store(&e->continuation_queued, 0);   /* the wake latch is a per-run
                                   knob: a restart wakes on its own inputs */
  e->engine_failed = 0;
  e->live_children = 0;
  e->finish_text = NULL;
  /* The persona trip's window state starts parked-free (the derive is the
     only writer of either field; a restart re-derives and re-composes). */
  e->derive_events = NULL;
  e->persona_prefix = NULL;
  /* The doom breaker's streak never survives a restart either (the
     _loop_engine_end clear owns the free — a stale copy here would mean a
     leak the freed-engine discipline never wrote). */
  e->doom_streak = 0;
  e->doom_last_code = NULL;
  e->users_seen_seq = 0;
  e->cell_users_seq = 0;
  /* The lifecycle envelope's state starts UNKNOWN (the first entry restores
     the counter through the log — the restore discipline; a DEAD engine
     never carries it across a restart, and a restart of a failed engine on
     the same record does not either). */
  e->turn_counter = 0;
  e->turn_known = 0;
  e->turn_open = 0;
  e->step_open = 0;
  e->phase = FRAME_PHASE_NONE;
  e->store_kind = (frame_store_kind_e)0;
  e->store_corr = 0;
  e->turn_cell_corr = 0;
  e->turn_reply = NULL;
  e->engine_live = 1;
  return _loop_post_turn(f);
}

void _frame_engine_turn(frame_t* f) {
  frame_engine_state_t* e = (f != NULL) ? _frame_engine_state(f) : NULL;
  if (e == NULL) {
    log_error("frame: FRM_TURN at an unusable frame — dropping loud");
    return;
  }
  if (!e->engine_live) {
    /* A late repost after a terminal step: loud, nothing to destroy (the
       continuation's payload is NULL). */
    log_error("frame: FRM_TURN late-dropped at '%s' — no turn engine is "
              "live", frame_sid(f));
    return;
  }
  if (_frame_is_live(f) == 0) {
    /* The subtree died under a queued continuation (a destroy raced the
       mailbox) — a dead frame cannot derive anything. */
    log_error("loop: the frame's subtree is gone on a queued turn at '%s' "
              "— the engine ends failed", frame_sid(f));
    _loop_engine_end(f, e, 1);
    return;
  }
  if (e->phase != FRAME_PHASE_NONE && e->phase != FRAME_PHASE_CHILDREN) {
    /* A duplicate/late continuation raced a step already in flight (two
       child reports each post their resume; the engine runs ONE turn at a
       time): drop loud. Nothing is lost durably — the in-flight turn's own
       continuation or the next resumed derive re-reads the store. (The
       CHILDREN phase itself IS the resume dispatch point: a continuation
       arriving there runs the step.) */
    log_error("frame: FRM_TURN late-dropped at '%s' — a turn step (phase %u) "
              "is already in flight", frame_sid(f), (unsigned)e->phase);
    return;
  }

  if (e->model_retry_step != 0) {
    /* A model-RETRY continuation: the old loop's retry did not spend a turn
       and re-ran no checks — go straight to the re-derive. */
    e->model_retry_step = 0;
  } else {
    /* Stop request drains before anything else (FRM_STOP is a control
       request, not an interruption: the running cell finishes). */
    if (_frame_stop_requested(f)) {
      log_info("loop: stop requested before turn %u of '%s'",
               (unsigned)(e->turns_issued + 1), frame_sid(f));
      _loop_engine_end(f, e, 0);
      return;
    }
    /* A report (from any earlier turn's cell) already ended this frame —
       clean completion (the advisory direct read, spec §5's carve-out). For
       a CHILD the terminal step posts its parent's resume (the report verb's
       bind is confirmed committed; the resume is the one missing piece); a
       TOP frame simply ends. */
    if (frame_is_done(f)) {
      _frame_engine_terminate(f, 1, NULL);
      return;
    }
    /* Fail loud at the cap: never an infinite loop. */
    unsigned cap = _frame_loop_turn_cap(f);
    if (e->turns_issued == cap) {
      log_error("loop: turn limit %u reached at '%s' — failing loud",
                cap, frame_sid(f));
      _loop_fail(f, e, "turn-limit", "model turn budget exhausted");
      return;
    }
    model_backend_t* mb = _frame_backend_get(f);
    if (mb == NULL) {
      log_error("loop: '%s' has no usable model backend", frame_sid(f));
      _loop_fail(f, e, "model-missing", NULL);
      return;
    }
    e->turns_issued++;
  }
  _loop_post_derive(f, e);   /* the derive = the store round trip; yield */
}

void _frame_engine_store_reply(frame_t* f, frm_store_reply_payload_t* r) {
  frame_engine_state_t* e = (f != NULL) ? _frame_engine_state(f) : NULL;
  if (e == NULL || !e->engine_live || e->phase != FRAME_PHASE_STORE ||
      e->store_corr == 0 || r == NULL || r->corr != e->store_corr) {
    log_error("loop: an unmatched engine store reply (corr %llu) at '%s' — "
              "dropped loud",
              (unsigned long long)((r != NULL) ? r->corr : 0),
              (f != NULL) ? frame_sid(f) : "?");
    if (r != NULL) frm_store_reply_payload_destroy(r);
    return;
  }
  int rc = r->rc;
  frame_store_kind_e kind = e->store_kind;
  e->phase = FRAME_PHASE_NONE;
  e->store_corr = 0;
  switch (kind) {
    case FRAME_STORE_DERIVE:
      _loop_engine_on_derive(f, e, r);
      break;
    case FRAME_STORE_CELL_RUN:
      _loop_engine_on_cell_run(f, e, rc);
      break;
    case FRAME_STORE_FINISH:
      _loop_engine_on_finish(f, e, rc);
      break;
    case FRAME_STORE_PERSONA:
      _loop_engine_on_persona(f, e, r);
      break;
    default:
      break;
  }
  frm_store_reply_payload_destroy(r);   /* the raw records die here */
}

void _frame_engine_model_arrived(frame_t* f, frm_model_payload_t* payload) {
  frame_engine_state_t* e = (f != NULL) ? _frame_engine_state(f) : NULL;
  if (e == NULL || payload == NULL || !e->engine_live ||
      e->phase != FRAME_PHASE_MODEL) {
    /* A late/duplicated arrival (after a terminal step, or outside the
       await): a loud drop — the payload's ownership dies here. */
    log_error("loop: an unmatched model completion at '%s' — dropped loud",
              (f != NULL) ? frame_sid(f) : "?");
    if (payload != NULL) frm_model_payload_destroy(payload);
    return;
  }
  if (_frame_engine_die_requested(f) != 0) {
    /* A completion dispatched into a dying frame (a destroy raced the
       mailbox): loud drop, and — the die rule — NO FRM_TURN repost and NO
       engine step into a record whose teardown owns everything. The
       payload's ownership dies here. */
    log_error("loop: a model completion dispatched into the dying frame "
              "'%s' — dropped loud (no turn continuation is reposted into "
              "the dead mailbox)", frame_sid(f));
    frm_model_payload_destroy(payload);
    return;
  }
  e->phase = FRAME_PHASE_NONE;
  model_reply_t* reply = NULL;
  char* err = NULL;
  /* The payload's facts outlive its record: read BEFORE the destroy (the
     sink parsed the header capture, the decode carries the retry value
     byte-identically, the reply branch consults the table with both). */
  int model_status = payload->status;
  unsigned retry_after_sec = payload->retry_after_sec;
  int rc = _model_result_from_http(model_status, payload->body,
                                   payload->body_len, payload->error,
                                   retry_after_sec, &reply, &err);
  frm_model_payload_destroy(payload);   /* the raw body/error die here */
  _frame_engine_reply(f, e, rc, reply, err, model_status, retry_after_sec);
}

void _frame_engine_cell_done(frame_t* f) {
  frame_engine_state_t* e = (f != NULL) ? _frame_engine_state(f) : NULL;
  if (e == NULL || !e->engine_live || e->phase != FRAME_PHASE_CELL) {
    return;   /* not the engine's cell (a direct-API/driver-driven cell) */
  }
  e->phase = FRAME_PHASE_NONE;
  (void)_loop_post_turn(f);   /* the turn continues after the cell's result */
}

void _frame_engine_child_report(frame_t* f, frm_child_report_payload_t* payload) {
  /* Task 5's parent-resume behavior. The binding ALREADY happened (the
     child's engine-driven bind batch was confirmed committed before the
     FRM_CHILD_REPORT was posted) — this is bookkeeping + the resume. The
     pending-children counter is the liveness shape (the Task-4
     pending-submits precedent; the ponyc pointer's "pending-message
     accounting" made honest): decrement, fold the join, repost the turn. */
  const char* child_sid = (payload != NULL && payload->child_sid != NULL)
                              ? payload->child_sid : "?";
  uint8_t failed = (payload != NULL) ? payload->failed : 0;
  if (f == NULL) {
    log_error("loop: FRM_CHILD_REPORT for '%s' with no frame — dropped loud",
              child_sid);
    if (payload != NULL) frm_child_report_payload_destroy(payload);
    return;
  }
  if (failed != 0) {
    /* The parent's thread logs a failed child's resume loudly (spec's
       contract); the failure text itself is in the bound report event. */
    log_error("loop: child '%s' ended FAILED under '%s' — its failure report "
              "is bound in this frame's log; the engine resumes",
              child_sid, frame_sid(f));
  } else {
    log_info("loop: child '%s' reported under '%s' — the engine resumes",
             child_sid, frame_sid(f));
  }
  /* The liveness counter: children admitted-and-started, not yet resumed. A
     zero-count delivery is loud (nothing counted it) — the join fold and
     the resume decision below still take their normal paths. */
  frame_engine_state_t* e = _frame_engine_state(f);
  if (e != NULL && e->live_children > 0) {
    e->live_children--;
  } else {
    log_error("loop: a child report for '%s' arrived at '%s' with NO child "
              "counted (in-memory bookkeeping of this process's spawns; a "
              "restarted engine resumes without one)", child_sid, frame_sid(f));
  }
  /* Fold the join (§4): ONE frame.join in this frame's log, fire-and-post —
     the store's FIFO commits it ahead of anything this dispatch posts after
     (the resumed derive's scan sees it). On-failure-continue. */
  _frame_join_post(f, (payload != NULL) ? payload->child_sid : NULL);
  if (payload != NULL) frm_child_report_payload_destroy(payload);
  /* Resume ONLY a live engine (a direct-API caller's join bookkeeping still
     landed above): the reposted turn re-derives — the bound report event is
     in the store ahead of the scan, because the report message posts only
     after the bind's commit reply. */
  if (e == NULL || !e->engine_live) {
    return;
  }
  (void)_loop_post_turn(f);   /* the parent RESUMES (the turn-step guard keeps
                                 ONE turn in flight when two reports race) */
}

/* ---------------------------------------------------------------------------
 * The synchronous driver (§6): start-or-pump over the SAME engine.
 * ------------------------------------------------------------------------- */

/* The deadline per awaited phase (frame_internal.h's contract); NONE — the
   gap between a terminal step and the next dispatch — gets the store
   deadline. Mostly a one-pump transient, but a retry's backoff parks the
   phase in NONE for up to 5 s (the guards table's backoff cap) — still
   safe under the 30 s SA_LOOP_STORE_WAIT_MS deadline. */
static unsigned _loop_phase_deadline_ms(frame_phase_e phase) {
  switch (phase) {
    case FRAME_PHASE_CELL:
      return SA_LOOP_CELL_WAIT_MS;
    case FRAME_PHASE_MODEL:
      return SA_LOOP_MODEL_WAIT_MS;
    default:
      return SA_LOOP_STORE_WAIT_MS;
  }
}

/* Before every return: drain the INLINE store to quiescence so no
   fire-and-post outlives the driver's return (the belt to the store FIFO's
   suspenders). A pooled store is never pumped here (workers own pacing). */
static void _loop_driver_drain_store(frame_t* f) {
  if (_frame_store_pooled(f) != 0) {
    return;
  }
  actor_t* store = _frame_store_actor(f);
  if (store == NULL) return;
  uint64_t deadline =
      platform_monotonic_ns() + (uint64_t)SA_LOOP_STORE_WAIT_MS * 1000000ULL;
  while (actor_run(store, ACTOR_BATCH_SIZE)) {
    if (platform_monotonic_ns() >= deadline) {
      log_error("loop: the inline store did not reach quiescence within %d ms "
                "at '%s' — giving the driver's drain up (the store keeps the "
                "queue; its own log carries the stall)",
                (int)SA_LOOP_STORE_WAIT_MS, frame_sid(f));
      break;
    }
  }
}

int frame_run_loop(frame_t* f) {
  if (_frame_is_live(f) == 0) {
    log_error("frame_run_loop: dead frame");
    return 1;
  }
  frame_engine_state_t* e = _frame_engine_state(f);
  if (e == NULL) {
    log_error("frame_run_loop: unusable frame state");
    return 1;
  }

  /* Start-or-pump (§6): an engine that is NOT live is started (refused →
     rc 1); an ALREADY-LIVE one is fine — the driver pumps from where the
     engine is, which is how a spawned child's queued FRM_TURN gets drained
     by the synchronous driver. */
  if (e->engine_live == 0) {
    if (frame_start(f) != 0) return 1;
  }

  /* The bounded pump: the deadline is PER PHASE (the cell deadline
     SA_LOOP_CELL_WAIT_MS, SA_LOOP_MODEL_WAIT_MS, SA_LOOP_STORE_WAIT_MS); a
     breaking deadline is a loud stall, never a hang. */
  frame_phase_e seen = FRAME_PHASE_NONE;
  uint64_t seen_trip = 0;      /* the STORE phase re-enters once per awaited
                                  round trip — the deadline resets per TRIP
                                  (the store wait is µs–ms per round trip,
                                  per frame_internal.h's deadline contract) */
  int seen_valid = 0;
  uint64_t phase_started = 0;
  for (;;) {
    /* ONE pump cycle over the frame's whole round-trip surface (the frame
       mailbox, its live ancestors', the inline store — frame_internal.h's
       ONE pump order); everything the engine awaits lands within cycles. */
    _frame_pump(f);
    if (e->engine_live == 0) break;
    if (e->phase == FRAME_PHASE_CHILDREN) break;   /* Task 5's yield */
    if (e->phase == FRAME_PHASE_ASK) break;   /* the ask park (escalation
                                                 spec §1.3): the engine rests
                                                 until the owner's reply —
                                                 frame_ask_reply's posted
                                                 FRM_ASK_REPLY clears the park
                                                 and reposts the turn; the
                                                 NEXT run loop pumps that */
    if (!seen_valid || e->phase != seen ||
        (e->phase == FRAME_PHASE_STORE && e->store_corr != seen_trip)) {
      seen = e->phase;
      seen_trip = e->store_corr;
      seen_valid = 1;
      phase_started = platform_monotonic_ns();
    }
    if (platform_monotonic_ns() - phase_started >=
        (uint64_t)_loop_phase_deadline_ms(e->phase) * 1000000ULL) {
      /* The deadline break: control per phase + engine end failed — the
         old loop's cell-timeout rule, per the awaited phase. The stall's
         own message is the diagnosis (the awaited reply routes whenever it
         lands; late engine replies drop loud). */
      const char* kind = "store-timeout";
      if (e->phase == FRAME_PHASE_CELL) {
        /* The unification (surface-completion spec §2): the inline driver's
           cell deadline runs the SAME interrupt synthesis the pooled
           watchdog posts — the corr-matched close + poison — instead of
           giving up with the cell slot left pending. arm_cut = 0: a
           deadline never arms a cut. The apply ENDS THE ENGINE ITSELF on
           every batch path — BOTH the posted close and the refused
           pre-post call _frame_engine_terminate internally — so when the
           apply returns, engine_live == 0 is the normal shape. The
           discriminator below separates "the apply already ended the
           engine" from "the apply returned without ending it" (its n==0
           shapes: the benign already-complete-cell no-op and the OOM
           corner). The compose-time facts follow the post: a POSTED close
           leaves turn_open 0 with the engine ended. */
        _frame_interrupt_apply(f, 0,
                               "aborted: cell exceeded the watchdog deadline");
        if (e->engine_live == 0) {
          /* The apply ended the engine (the posted close AND the refused
             batch alike — the terminate already ran inside it, and
             re-entering it here would log the loud already-ended no-op on
             EVERY routine cell deadline). Only the cell-timeout control
             wording remains. */
          _loop_control(f, "cell-timeout", NULL);
          break;
        }
        /* The apply returned WITHOUT ending the engine: the n==0 shapes
           only (a deadline at an already-complete cell, or the OOM corner
           where the cell slot closed inline without its durable record).
           Both leave turn_open 0 — the riders exist only under an open
           turn — so _loop_fail's rider branch is a no-op and this is its
           plain shape: ONE cell-timeout control event + the failed
           terminate THAT ends the engine here (its first end, not a
           re-entry). */
        _loop_fail(f, e, "cell-timeout", NULL);
        break;
      }
      if (e->phase == FRAME_PHASE_MODEL) kind = "model-await";
      log_error("loop: the engine at '%s' awaited phase %u past its %u ms "
                "deadline — the engine ends failed loud", frame_sid(f),
                (unsigned)e->phase, _loop_phase_deadline_ms(e->phase));
      _loop_fail(f, e, kind, NULL);
      break;
    }
    platform_sleep_ms(1);
  }

  _loop_driver_drain_store(f);
  /* 2 on the LIVE yields (the CHILDREN yield — Task 5's branch — or the ask
     park, whose reply resumes the engine live), else 0 clean
     / 1 failed loud by the terminal step's engine_failed. */
  if (e->phase == FRAME_PHASE_CHILDREN || e->phase == FRAME_PHASE_ASK) {
    return 2;
  }
  return (e->engine_failed != 0) ? 1 : 0;
}

#endif /* SA_HAS_WDB */