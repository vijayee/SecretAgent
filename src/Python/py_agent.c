//
// Created by victor on 9/29/26.
//

#include "py_agent.h"

#ifdef SA_HAS_PYTHON

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "../Actor/actor.h"
#include "../Frame/frame_bridge.h"
#include "../Frame/frame_messages.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include "../Util/budget.h"
#include "../Util/log.h"
#include "../Platform/platform.h"

#include "pyrt.h"
#include "pyrt_messages.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* WHY IT BLOCKS (the plan's documented reconciliation of the pyrt outbound
   rule with this bridge's synchronous-request shape):
   The bridge verbs post a corr-matched request into the frame actor's OWN
   inbox and then WAIT for the corr-matched reply. Waiting from a callback
   would violate the pyrt outbound rule ("callbacks must be non-blocking"),
   EXCEPT that the blocking happens on the PYTHON side of the pyrt WORKER
   thread — the cell's own thread, which pyrt's design explicitly permits to
   block on its own work queue. The pyrt thread IS the frame's dedicated
   python thread, so a queued cell can hold that thread for its whole
   duration anyway; while this thread waits, the GIL is RELEASED
   (Py_BEGIN_ALLOW_THREADS / Py_END_ALLOW_THREADS), so the interpreter (its
   own PEP-684 subinterpreter GIL included) is not held and sibling frames
   keep running. The reply is delivered on the frame's dispatch thread
   through frame_bridge's installed sink, which wakes this very thread via
   the per-request completion record below. The wait is BOUNDED
   (SA_PY_AGENT_WAIT_MS) and a timeout is answered as a failure — this is
   the PA synchronous-request round-trip shape (peer_book precedent:
   platform_condvar_timed_wait), never an unbounded block. */

/* The bounded wait: overridable so tests can shrink it if a suite must not
   tolerate the default window. Default 500 ms per verb call. */
#ifndef SA_PY_AGENT_WAIT_MS
#define SA_PY_AGENT_WAIT_MS 500
#endif

/* ----------------------- completion registry ---------------------------- */

/* Per-request wait record. It lives on the CALLER's (pyrt thread's) stack.
   The registry holds only POINTERS to these, so there is no registry-owned
   heap memory at all: a waiter registers itself (under the lock) BEFORE
   posting its request (so a reply racing the send cannot miss it), and
   unlinks itself (under the lock) after the wait — while the note_reply
   path only ever chases records that are still linked, no record is ever
   touched after its stack frame dies. One mutex + one condvar are shared;
   note_reply fills the corr-matched record, copies the BORROWED reply text
   into it (good-actors: the sink owns nothing long-term), and broadcasts. */
typedef struct py_agent_wait_t {
  uint64_t corr;
  uint8_t done;
  uint8_t status;
  char* text;                     /* heap copy of the reply text; waiter frees */
  struct py_agent_wait_t* next;
} py_agent_wait_t;

static platform_mutex_t* _py_agent_lock = NULL;
static platform_condvar_t* _py_agent_cv = NULL;
static py_agent_wait_t* _py_agent_waiters = NULL;
static ATOMIC(uint64_t) _py_agent_corr = 0;

/* Benign-race guard install (pyrt's pool idiom): every racing init caller
   makes a candidate mutex; the first CAS wins and keeps it, losers destroy
   theirs. Under the winning mutex the infra pair is created. */
static _Atomic(platform_mutex_t*) _py_agent_init_guard = NULL;

void py_agent_init(void) {
  platform_mutex_t* m = platform_mutex_create();
  platform_mutex_t* expected = NULL;
  if (!atomic_compare_exchange_strong(&_py_agent_init_guard, &expected, m)) {
    platform_mutex_destroy(m);
    m = atomic_load(&_py_agent_init_guard);
  }
  if (m == NULL) {
    log_error("py_agent: init guard missing — registry not mounted");
    return;
  }
  platform_mutex_lock(m);
  if (_py_agent_lock == NULL) {
    _py_agent_lock = platform_mutex_create();
    _py_agent_cv = platform_condvar_create();
  }
  /* (Re-)register always: py_agent_init is the mount point for the reply
     registry, and frame_bridge's tests demount the sink between suites. */
  frame_bridge_register_reply_sink(py_agent_note_reply);
  platform_mutex_unlock(m);
}

/* The reply path. Runs on the frame's DISPATCH thread: never blocks (lock +
   a small allocation for the text copy only) and never frees the borrowed
   text. */
void py_agent_note_reply(uint64_t corr, uint8_t status, const char* text) {
  if (_py_agent_lock == NULL) {
    log_error("py_agent: reply corr %llu before any registry mount — dropped",
              (unsigned long long)corr);
    return;
  }
  platform_mutex_lock(_py_agent_lock);
  for (py_agent_wait_t* w = _py_agent_waiters; w != NULL; w = w->next) {
    if (w->corr == corr && w->done == 0) {
      w->status = status;
      if (text != NULL) {
        w->text = strdup(text);   /* the copy outlives the borrowed argument */
        if (w->text == NULL) {
          /* OOM on the copy: answer the waiter as a failure instead of
             handing it a heap pointer it cannot have. */
          w->status = 1;
          log_error("py_agent: OOM copying reply text (corr %llu) — answered "
                    "status 1", (unsigned long long)corr);
        }
      }
      w->done = 1;
      platform_condvar_broadcast(_py_agent_cv);
      platform_mutex_unlock(_py_agent_lock);
      return;
    }
  }
  platform_mutex_unlock(_py_agent_lock);
  /* No waiter matched (the caller timed out and unlinked, or the request was
     never posted): loud, consistent with frame_bridge's drop path. */
  log_error("py_agent: no waiter for reply corr %llu status %u '%s' — "
            "dropped (the caller timed out or the request was never sent)",
            (unsigned long long)corr, (unsigned)status, text ? text : "(no text)");
}

/* The JSON encoding of a python value (heap C string): every remember value
   is JSON-encoded at the python boundary — the store contract is JSON
   documents, and a bare python string ('wave') would be refused by the
   frame's remember validation. The interpreter's own json.dumps encodes
   containers/bools/None correctly (repr's 'True' is not JSON); failures
   propagate as exceptions. Caller frees. */
static char* _py_agent_json_of(PyObject* obj) {
  PyObject* json_mod = PyImport_ImportModule("json");
  if (json_mod == NULL) return NULL;
  PyObject* dumped = PyObject_CallMethod(json_mod, "dumps", "O", obj);
  Py_DECREF(json_mod);
  if (dumped == NULL) return NULL;
  const char* utf = PyUnicode_AsUTF8(dumped);
  if (utf == NULL) {
    Py_DECREF(dumped);
    return NULL;
  }
  char* out = strdup(utf);   /* strdup precedes any DECREF: dumped owns the buffer */
  Py_DECREF(dumped);
  if (out == NULL) PyErr_NoMemory();
  return out;
}

/* The python value of a JSON document string (new reference): the mirror of
   _py_agent_json_of on the recall path. On a parse failure returns the raw
   text as a python str instead (defensive — the store only writes JSON, so
   this branch should never fire). */
static PyObject* _py_agent_json_load(const char* text) {
  PyObject* json_mod = PyImport_ImportModule("json");
  if (json_mod == NULL) return NULL;
  PyObject* loaded = PyObject_CallMethod(json_mod, "loads", "s", text);
  Py_DECREF(json_mod);
  if (loaded != NULL) return loaded;
  PyErr_Clear();
  return PyUnicode_FromString(text);
}

/* -------------------------- request round-trip --------------------------- */

typedef enum py_agent_wait_rc_e {
  PY_AGENT_OK = 0,          /* reply delivered; the record holds status + text */
  PY_AGENT_NO_OWNER = 1,    /* no frame owns this thread: answered as failure */
  PY_AGENT_SEND_FAILED = 2, /* actor_send refused (owner going away): failure */
  PY_AGENT_TIMEOUT = 3      /* bounded wait elapsed: failure */
} py_agent_wait_rc_e;

/* Post the corr-matched request to the calling pyrt thread's owning frame
   actor and wait for the corr-matched bridge reply. The GIL is released for
   the whole send+wait (see the header comment). On success the reply's
   status is copied to *status_out and *text_out receives the heap copy of
   the reply text (may be NULL; the caller frees it). */
static py_agent_wait_rc_e _py_agent_request(uint32_t msg_type, void* payload,
                                            void (*destroy)(void*), uint64_t corr,
                                            uint8_t* status_out, char** text_out) {
  actor_t* owner = pyrt_thread_owner();
  if (_py_agent_lock == NULL) {
    /* Unmountable path: py_agent_init() runs at pyrt's first global boot,
       before any thread can run a verb — this is a loud defensive guard,
       never a live path. */
    if (destroy != NULL && payload != NULL) {
      destroy(payload);
    }
    log_error("py_agent: no registry mounted (uninitialized runtime layer) — "
              "answering the call as failure (corr %llu)",
              (unsigned long long)corr);
    return PY_AGENT_NO_OWNER;
  }
  if (owner == NULL) {
    /* Cells calling agent.* while no frame owns their runtime (or from a
       stdlib thread without the TLS): answer immediately as failure, loud. */
    if (destroy != NULL && payload != NULL) {
      destroy(payload);
    }
    log_error("py_agent: verb outside a frame-owned runtime thread — "
              "answering the call as failure (corr %llu)",
              (unsigned long long)corr);
    return PY_AGENT_NO_OWNER;
  }

  /* The waiter is registered BEFORE the post so a reply racing the send
     cannot miss the record. */
  py_agent_wait_t w;
  memset(&w, 0, sizeof(w));
  w.corr = corr;
  platform_mutex_lock(_py_agent_lock);
  w.next = _py_agent_waiters;
  _py_agent_waiters = &w;
  platform_mutex_unlock(_py_agent_lock);

  message_t msg;
  msg.type = msg_type;
  msg.payload = payload;
  msg.payload_destroy = destroy;

  uint8_t sent = 0;
  char* reply_text = NULL;
  uint8_t reply_status = 0;
  uint8_t delivered = 0;
  Py_BEGIN_ALLOW_THREADS
  sent = actor_send(owner, &msg) ? 1 : 0;
  if (sent) {
    /* actor_send already freed the payload if it refused; wait only then. */
    uint64_t deadline =
        platform_monotonic_ns() + (uint64_t)SA_PY_AGENT_WAIT_MS * 1000000ULL;
    platform_mutex_lock(_py_agent_lock);
    uint64_t now = platform_monotonic_ns();
    while (w.done == 0 && now < deadline) {
      uint64_t remaining_ms = (deadline - now) / 1000000ULL + 1;
      platform_condvar_timed_wait(_py_agent_cv, _py_agent_lock,
                                  (uint32_t)remaining_ms);
      now = platform_monotonic_ns();
    }
    /* Unlink the record under the same lock the reply path holds: after
       this, no note_reply call can reach the stack record again. */
    py_agent_wait_t** link = &_py_agent_waiters;
    while (*link != NULL && *link != &w) {
      link = &(*link)->next;
    }
    if (*link != NULL) {
      *link = w.next;
    }
    delivered = w.done;
    reply_status = w.status;
    reply_text = w.text;
    platform_mutex_unlock(_py_agent_lock);
  }
  Py_END_ALLOW_THREADS

  if (sent == 0) {
    platform_mutex_lock(_py_agent_lock);
    py_agent_wait_t** link = &_py_agent_waiters;
    while (*link != NULL && *link != &w) {
      link = &(*link)->next;
    }
    if (*link != NULL) {
      *link = w.next;
    }
    platform_mutex_unlock(_py_agent_lock);
    free(reply_text);
    log_error("py_agent: send to the owning frame actor failed (corr %llu) — "
              "answered as failure", (unsigned long long)corr);
    return PY_AGENT_SEND_FAILED;
  }

  if (delivered == 0) {
    free(reply_text);
    return PY_AGENT_TIMEOUT;
  }
  if (status_out != NULL) {
    *status_out = reply_status;
  }
  if (text_out != NULL) {
    *text_out = reply_text;
  } else {
    free(reply_text);
  }
  return PY_AGENT_OK;
}

/* ------------------------- text coercion helpers ------------------------- */

/* One corr per verb call, process-wide (distinct from pyrt's executor corr
   space: those travel through PYRT_RESULT mailbox payloads, these through
   the bridge sink, so the two spaces never meet). */
static uint64_t _py_agent_next_corr(void) {
  return atomic_fetch_add(&_py_agent_corr, 1) + 1;
}

/* str → heap copy verbatim; any other object → heap copy of its repr (valid
   JSON text for scalars and strings; containers that repr to non-JSON are
   refused downstream, loud). Returns NULL with a python exception set on
   conversion/OOM failure. The string is OUT of Python's heap before any
   borrowed pointer can die. */
static char* _py_agent_text_of(PyObject* obj) {
  if (PyUnicode_Check(obj)) {
    const char* utf = PyUnicode_AsUTF8(obj);
    if (utf == NULL) return NULL;
    char* out = strdup(utf);
    if (out == NULL) PyErr_NoMemory();
    return out;
  }
  PyObject* r = PyObject_Repr(obj);
  if (r == NULL) return NULL;
  const char* utf = PyUnicode_AsUTF8(r);
  if (utf == NULL) {
    Py_DECREF(r);
    return NULL;
  }
  char* out = strdup(utf);   /* strdup precedes any DECREF: r owns the buffer */
  Py_DECREF(r);
  if (out == NULL) PyErr_NoMemory();
  return out;
}

/* ------------------------------ the verbs -------------------------------- */

/* remember(key, value) -> bool.

   `value` is encoded with the interpreter's json.dumps at the boundary (a
   bare python str 'wave' stores as the JSON string "wave"; containers and
   bools/None encode the same way) — the python surface speaks python, the
   store contract is JSON. The encoded value is cut at the bridge value
   budget (SA_BUDGET_BRIDGE_VALUE_BYTES, spec §4) before it crosses. Bridges
   to FRM_REMEMBER; the frame's durable-ctx
   remember runs on the frame's dispatch thread and answers corr-matched
   through the sink. */
static PyObject* _py_agent_remember(PyObject* self, PyObject* args) {
  (void)self;
  PyObject* key_o = NULL;
  PyObject* val_o = NULL;
  if (!PyArg_ParseTuple(args, "OO:remember", &key_o, &val_o)) {
    return NULL;
  }
  char* key = _py_agent_text_of(key_o);
  if (key == NULL) return NULL;
  char* value = _py_agent_json_of(val_o);
  if (value == NULL) {
    free(key);
    return NULL;
  }

  /* The bridge value budget (budget table, spec §4): remember's VALUE is cut
     HERE, at the source — the store never holds unbounded text. The helper's
     marker travels with the value (the model learns the cut at its next
     recall); a REAL cut also posts a log line naming it. */
  char* capped = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(value, SA_BUDGET_BRIDGE_VALUE_BYTES, &capped,
                              &truncated);
  if (capped == NULL) {
    /* The helper refused (OOM / NULL text): the loud refusal contract. */
    free(key);
    free(value);
    PyErr_NoMemory();
    return NULL;
  }
  if (truncated != 0) {
    char cut_line[128];
    snprintf(cut_line, sizeof(cut_line),
             "remember: the %zu-byte value exceeded the %u-byte bridge budget "
             "and was cut with the truncation marker",
             strlen(value), (unsigned)SA_BUDGET_BRIDGE_VALUE_BYTES);
    pyrt_post_text(PYRT_LOG, cut_line);
  }
  free(value);
  value = capped;

  frm_remember_payload_t* rp = get_clear_memory(sizeof(frm_remember_payload_t));
  if (rp == NULL) {
    free(key);
    free(value);
    PyErr_NoMemory();
    return NULL;
  }
  rp->key = key;
  rp->json_value = value;   /* payload owns both (destroyer frees them) */

  uint8_t status = 0;
  char* text = NULL;
  uint64_t corr = _py_agent_next_corr();
  rp->corr = corr;
  py_agent_wait_rc_e rc =
      _py_agent_request((uint32_t)FRM_REMEMBER, rp, frm_remember_payload_destroy,
                        corr, &status, &text);
  free(text);
  if (rc == PY_AGENT_OK && status == 0) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

/* recall(key) -> value | None.

   Bridges to FRM_RECALL; a delivered success carries the resolved raw JSON
   text, decoded back to the python value it was remembered as (the mirror
   of remember's dumps — a bare str round trips as bare str). Unresolvable
   key (status 1), send failure, and the bounded-wait timeout all answer
   None — a failed lookup is never distinguished from a lost reply by the
   return value (the loud log on the reply path carries that story). */
static PyObject* _py_agent_recall(PyObject* self, PyObject* args) {
  (void)self;
  PyObject* key_o = NULL;
  if (!PyArg_ParseTuple(args, "O:recall", &key_o)) {
    return NULL;
  }
  char* key = _py_agent_text_of(key_o);
  if (key == NULL) return NULL;

  frm_remember_payload_t* rp = get_clear_memory(sizeof(frm_remember_payload_t));
  if (rp == NULL) {
    free(key);
    PyErr_NoMemory();
    return NULL;
  }
  rp->key = key;
  rp->json_value = NULL;   /* FRM_RECALL carries no value */

  uint8_t status = 0;
  char* text = NULL;
  uint64_t corr = _py_agent_next_corr();
  rp->corr = corr;
  py_agent_wait_rc_e rc =
      _py_agent_request((uint32_t)FRM_RECALL, rp, frm_remember_payload_destroy,
                        corr, &status, &text);
  if (rc == PY_AGENT_OK && status == 0 && text != NULL) {
    PyObject* out = _py_agent_json_load(text);
    free(text);
    return out;   /* NULL (exception set) propagates the OOM verbatim */
  }
  free(text);
  Py_RETURN_NONE;
}

/* keys(scope) -> list | None.

   Bridges to FRM_KEYS (the pulled-forward inspect member, spec §3): the
   frame's dispatch validates the CLOSED-SET scope ('local'/'ctx') and
   composes the bounded OWN-subtree listing in the store actor, answering a
   JSON array of KEY NAMES ONLY through the same bridge machinery (never a
   value — recall resolves values; keys never mixes them). The array decodes
   back to the python list it models; the empty subtree answers []
   (a real empty list, not None). A refusal (unknown scope), send failure,
   and the bounded-wait timeout all answer None — recall's documented
   failure shape, mirrored exactly. */
static PyObject* _py_agent_keys(PyObject* self, PyObject* args) {
  (void)self;
  PyObject* scope_o = NULL;
  if (!PyArg_ParseTuple(args, "O:keys", &scope_o)) {
    return NULL;
  }
  char* scope = _py_agent_text_of(scope_o);
  if (scope == NULL) return NULL;

  frm_remember_payload_t* rp = get_clear_memory(sizeof(frm_remember_payload_t));
  if (rp == NULL) {
    free(scope);
    PyErr_NoMemory();
    return NULL;
  }
  rp->key = scope;         /* the frame dispatch reads the scope from here */
  rp->json_value = NULL;   /* FRM_KEYS carries no value */

  uint8_t status = 0;
  char* text = NULL;
  uint64_t corr = _py_agent_next_corr();
  rp->corr = corr;
  py_agent_wait_rc_e rc =
      _py_agent_request((uint32_t)FRM_KEYS, rp, frm_remember_payload_destroy,
                        corr, &status, &text);
  if (rc == PY_AGENT_OK && status == 0 && text != NULL) {
    PyObject* out = _py_agent_json_load(text);
    free(text);
    return out;   /* NULL (exception set) propagates the OOM verbatim */
  }
  free(text);
  Py_RETURN_NONE;
}

/* spawn(goal, context=None) -> str | None.

   Bridges to FRM_SPAWN (admission-only child spawn on the frame); a
   delivered success carries the child sid path, returned as a python
   string. context=None posts NO handoff key (the py_agent contract: spawn
   without handoff context); a context string is stored verbatim and cut at
   the bridge value budget (SA_BUDGET_BRIDGE_VALUE_BYTES, spec §4) first — the
   frame
   validates it as JSON and refuses (loud, status 1 → None) otherwise. */
static PyObject* _py_agent_spawn(PyObject* self, PyObject* args) {
  (void)self;
  PyObject* goal_o = NULL;
  PyObject* ctx_o = NULL;
  if (!PyArg_ParseTuple(args, "O|O:spawn", &goal_o, &ctx_o)) {
    return NULL;
  }
  char* goal = _py_agent_text_of(goal_o);
  if (goal == NULL) return NULL;
  char* context = NULL;
  if (ctx_o != NULL && ctx_o != Py_None) {
    context = _py_agent_text_of(ctx_o);
    if (context == NULL) {
      free(goal);
      return NULL;
    }
    /* The bridge value budget (budget table, spec §4): the handoff CONTEXT
       is cut HERE, at the source — the child never reads unbounded handoff
       text. A context=None spawn posts no key (the legitimate case above);
       only PRESENT context text is capped. A REAL cut also posts a log line
       naming it. */
    char* capped = NULL;
    uint8_t truncated = 0;
    budget_truncate_with_marker(context, SA_BUDGET_BRIDGE_VALUE_BYTES, &capped,
                                &truncated);
    if (capped == NULL) {
      /* The helper refused (OOM / NULL text): the loud refusal contract. */
      free(context);
      free(goal);
      PyErr_NoMemory();
      return NULL;
    }
    if (truncated != 0) {
      char cut_line[128];
      snprintf(cut_line, sizeof(cut_line),
               "spawn: the %zu-byte context exceeded the %u-byte bridge "
               "budget and was cut with the truncation marker",
               strlen(context), (unsigned)SA_BUDGET_BRIDGE_VALUE_BYTES);
      pyrt_post_text(PYRT_LOG, cut_line);
    }
    free(context);
    context = capped;
  }

  frm_spawn_payload_t* sp = get_clear_memory(sizeof(frm_spawn_payload_t));
  if (sp == NULL) {
    free(goal);
    free(context);
    PyErr_NoMemory();
    return NULL;
  }
  sp->goal = goal;
  sp->context_json = context;   /* NULL is the legitimate no-handoff case */

  uint8_t status = 0;
  char* text = NULL;
  uint64_t corr = _py_agent_next_corr();
  sp->corr = corr;
  py_agent_wait_rc_e rc =
      _py_agent_request((uint32_t)FRM_SPAWN, sp, frm_spawn_payload_destroy,
                        corr, &status, &text);
  if (rc == PY_AGENT_OK && status == 0 && text != NULL) {
    PyObject* out = PyUnicode_FromString(text);
    free(text);
    return out;
  }
  free(text);
  Py_RETURN_NONE;
}

/* report(value) -> bool.

   Bridges to FRM_REPORT: the report text is the value (str verbatim, any
   other object coerced through repr — the loop-slice callers hand strings),
   cut at the bridge value budget (SA_BUDGET_BRIDGE_VALUE_BYTES, spec §4)
   before it crosses. A delivered success is True; refusal, send failure, and
   timeout are False. */
static PyObject* _py_agent_report(PyObject* self, PyObject* args) {
  (void)self;
  PyObject* val_o = NULL;
  if (!PyArg_ParseTuple(args, "O:report", &val_o)) {
    return NULL;
  }
  char* text = _py_agent_text_of(val_o);
  if (text == NULL) return NULL;

  /* The bridge value budget (budget table, spec §4): the report TEXT is cut
     HERE, at the source — the loop's projection (SA_BUDGET_LOOP_REPORT) then
     sees bounded durable text. The helper's marker travels with the text (the
     next derive learns the cut); a REAL cut also posts a log line naming it. */
  char* capped = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(text, SA_BUDGET_BRIDGE_VALUE_BYTES, &capped,
                              &truncated);
  if (capped == NULL) {
    /* The helper refused (OOM / NULL text): the loud refusal contract. */
    free(text);
    PyErr_NoMemory();
    return NULL;
  }
  if (truncated != 0) {
    char cut_line[128];
    snprintf(cut_line, sizeof(cut_line),
             "report: the %zu-byte text exceeded the %u-byte bridge budget "
             "and was cut with the truncation marker",
             strlen(text), (unsigned)SA_BUDGET_BRIDGE_VALUE_BYTES);
    pyrt_post_text(PYRT_LOG, cut_line);
  }
  free(text);
  text = capped;

  frm_report_payload_t* rp = get_clear_memory(sizeof(frm_report_payload_t));
  if (rp == NULL) {
    free(text);
    PyErr_NoMemory();
    return NULL;
  }
  rp->text = text;

  uint8_t status = 0;
  char* reply_text = NULL;
  uint64_t corr = _py_agent_next_corr();
  rp->corr = corr;
  py_agent_wait_rc_e rc =
      _py_agent_request((uint32_t)FRM_REPORT, rp, frm_report_payload_destroy,
                        corr, &status, &reply_text);
  free(reply_text);
  if (rc == PY_AGENT_OK && status == 0) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

/* --------------------- the blocked-ask verb ------------------------------ */

/* More ask options than this = the refusal (spec §1.1). */
#define _PY_AGENT_ASK_OPTIONS_MAX 8

/* A text that carries no non-whitespace byte (the ask boundary's
   non-empty-after-strip rule). */
static uint8_t _py_agent_text_isblank(const char* text) {
  for (const char* p = text; *p != '\0'; p++) {
    if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\v' &&
        *p != '\f') {
      return 0;
    }
  }
  return 1;
}

/* ask(question, options=None) -> str.

   The blocked-ask's verb (escalation spec §1.1): FIRE-AND-POST, the emit
   pattern verbatim — the FRM_ASK is posted to the owning frame actor and
   the marker string is returned immediately; NOTHING crosses back through
   the bridge sink (the resolution is FRM_ASK_REPLY and the model reads it
   in the next turn's derive). The park pre-check runs FIRST: one ask per
   frame at a time — while a park stands the verb refuses as data and posts
   NOTHING. Refusals are ALWAYS the return value (row 13's law: data, never
   an exception): the empty question, the oversized question (the budget
   table's cap — oversized ask INPUT is refused, never silently truncated),
   the non-list options argument, more than 8 options, an empty option.
   Absent/None options = no options; a non-str item is coerced through repr
   like report does. */
static PyObject* _py_agent_ask(PyObject* self, PyObject* args) {
  (void)self;
  PyObject* question_o = NULL;
  PyObject* options_o = NULL;
  if (!PyArg_ParseTuple(args, "O|O:ask", &question_o, &options_o)) {
    return NULL;
  }

  /* The parked pre-check FIRST (one ask per frame at a time): the py pointer
     comes from pyrt.c's TLS — the SAME accessor the emit path's owner lookup
     rides, never a duplicated TLS read. NULL (a non-pyrt thread) means no
     park can stand. */
  if (pyrt_ask_parked(pyrt_thread_pyrt()) != 0) {
    return PyUnicode_FromString("ask already parked — reply pending");
  }

  char* question = _py_agent_text_of(question_o);
  if (question == NULL) return NULL;

  /* The closed option list (spec §1.1): absent/None = no options; a non-list
     REFUSES. Each item is coerced through _py_agent_text_of (repr for a
     non-str, report's shape); blank and oversized items refuse. */
  char** options = NULL;
  size_t noptions = 0;
  if (options_o != NULL && options_o != Py_None) {
    if (!PyList_Check(options_o)) {
      free(question);
      return PyUnicode_FromString("ask: options must be a list");
    }
    Py_ssize_t n = PyList_Size(options_o);
    if ((size_t)n > _PY_AGENT_ASK_OPTIONS_MAX) {
      free(question);
      return PyUnicode_FromString("ask: too many options (8 max)");
    }
    options = get_clear_memory(sizeof(char*) * (size_t)(n > 0 ? n : 1));
    if (options == NULL) {
      free(question);
      PyErr_NoMemory();
      return NULL;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
      PyObject* item = PyList_GetItem(options_o, i);   /* borrowed */
      char* text = item != NULL ? _py_agent_text_of(item) : NULL;
      if (text == NULL) {
        /* Conversion/OOM: free the copy-in so far (the payload owns nothing
           yet) and let the pending exception propagate verbatim. */
        for (size_t j = 0; j < (size_t)i; j++) {
          free(options[j]);
        }
        free(options);
        free(question);
        return NULL;
      }
      if (_py_agent_text_isblank(text)) {
        free(text);
        for (size_t j = 0; j < (size_t)i; j++) {
          free(options[j]);
        }
        free(options);
        free(question);
        return PyUnicode_FromString("ask: an option is empty");
      }
      if (strlen(text) > SA_BUDGET_BRIDGE_VALUE_BYTES) {
        char line[128];
        snprintf(line, sizeof(line),
                 "ask: an option exceeds the bridge budget (%u bytes)",
                 (unsigned)SA_BUDGET_BRIDGE_VALUE_BYTES);
        free(text);
        for (size_t j = 0; j < (size_t)i; j++) {
          free(options[j]);
        }
        free(options);
        free(question);
        return PyUnicode_FromString(line);
      }
      options[noptions++] = text;
    }
  }

  /* The question's validations: non-empty AFTER STRIP, then the budget
     cap — oversized input is REFUSED (never silently truncated). */
  if (_py_agent_text_isblank(question)) {
    free(question);
    for (size_t j = 0; j < noptions; j++) {
      free(options[j]);
    }
    free(options);
    return PyUnicode_FromString("ask: the question is empty");
  }
  if (strlen(question) > SA_BUDGET_BRIDGE_VALUE_BYTES) {
    char line[128];
    snprintf(line, sizeof(line),
             "ask: the question exceeds the bridge budget (%u bytes)",
             (unsigned)SA_BUDGET_BRIDGE_VALUE_BYTES);
    free(question);
    for (size_t j = 0; j < noptions; j++) {
      free(options[j]);
    }
    free(options);
    return PyUnicode_FromString(line);
  }

  /* The publish target: without a frame-owned runtime there is no owner to
     receive the ask — refused as data (loud log), never posted into the
     void as a success. */
  actor_t* owner = pyrt_thread_owner();
  if (owner == NULL) {
    log_error("py_agent: agent.ask outside a frame-owned runtime thread — "
              "refused (no publish)");
    free(question);
    for (size_t j = 0; j < noptions; j++) {
      free(options[j]);
    }
    free(options);
    return PyUnicode_FromString("ask: no owner for this runtime — cannot "
                                "publish");
  }

  /* Box the payload (ownership of every heap field transfers with the
     message) and POST — the emit shape: no wait, no reply, no lock held
     beyond what actor_send needs. The GIL stays held for the µs-scale
     send, exactly as pyrt_post_text's posts do. */
  frm_ask_payload_t* ap = get_clear_memory(sizeof(frm_ask_payload_t));
  if (ap == NULL) {
    free(question);
    for (size_t j = 0; j < noptions; j++) {
      free(options[j]);
    }
    free(options);
    PyErr_NoMemory();
    return NULL;
  }
  ap->corr = _py_agent_next_corr();
  ap->question = question;
  ap->options = options;
  ap->noptions = noptions;

  message_t msg;
  msg.type = (uint32_t)FRM_ASK;
  msg.payload = ap;
  msg.payload_destroy = frm_ask_payload_destroy;
  if (!actor_send(owner, &msg)) {
    /* actor_send already destroyed the payload (destroyed actor / full
       queue — good-actors): the refusal is data, loud on the log side. */
    log_error("py_agent: the ask publish was refused by the owning frame "
              "actor — answered as data");
    return PyUnicode_FromString(
        "ask: the publish was refused by the frame — try again later");
  }
  return PyUnicode_FromString("asked");
}

/* --------------------- the injected module's method table ---------------- */

/* The base stream verbs (pyrt.c owns the routing; these are the callbacks
   the injected module has always exposed). */
static PyObject* _py_agent_log(PyObject* self, PyObject* args) {
  const char* text = NULL;
  if (!PyArg_ParseTuple(args, "s", &text)) {
    return NULL;
  }
  pyrt_post_text(PYRT_LOG, text);
  Py_RETURN_NONE;
}

static PyObject* _py_agent_status(PyObject* self, PyObject* args) {
  const char* text = NULL;
  if (!PyArg_ParseTuple(args, "s", &text)) {
    return NULL;
  }
  pyrt_post_text(PYRT_STATUS, text);
  Py_RETURN_NONE;
}

static PyObject* _py_agent_emit(PyObject* self, PyObject* args) {
  const char* text = NULL;
  if (!PyArg_ParseTuple(args, "s:emit", &text)) {
    return NULL;
  }
  /* The ONE source cap (budget table, spec §4): emit's text is cut HERE,
     at the boundary — the frame trusts bounded text from its runtime, and
     the durable emit record never carries unbounded text. The helper's
     marker travels with the text (the model learns the cut at its next
     derive); a REAL cut also posts a log line naming it (the marker alone
     is a cut shape, not an announcement). */
  char* capped = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(text, SA_BUDGET_EMIT_BYTES, &capped,
                              &truncated);
  if (capped == NULL) {
    /* The helper refused (OOM / NULL text): the loud refusal contract. */
    PyErr_NoMemory();
    return NULL;
  }
  if (truncated != 0) {
    char cut_line[128];
    snprintf(cut_line, sizeof(cut_line),
             "emit: the %zu-byte text exceeded the %u-byte emit budget and "
             "was cut with the truncation marker",
             strlen(text), (unsigned)SA_BUDGET_EMIT_BYTES);
    pyrt_post_text(PYRT_LOG, cut_line);
  }
  pyrt_post_text(PYRT_EMIT, capped);
  free(capped);   /* pyrt_post_text copies the text out; the cap buffer retires */
  Py_RETURN_NONE;
}

static PyMethodDef _py_agent_base_methods[] = {
    {"log", _py_agent_log, METH_VARARGS, "Stream live narration to the owning actor."},
    {"status", _py_agent_status, METH_VARARGS, "Stream the current status to the owning actor."},
    {"emit", _py_agent_emit, METH_VARARGS, "Post a durable-payload candidate to the owning actor."},
    {NULL, NULL, 0, NULL}};

static PyMethodDef _py_agent_verb_methods[] = {
    {"remember", _py_agent_remember, METH_VARARGS, "Durable shared state write; returns True or False."},
    {"recall", _py_agent_recall, METH_VARARGS, "Resolve a key up the frame lineage; returns the JSON text or None."},
    {"keys", _py_agent_keys, METH_VARARGS, "List this frame's OWN state keys (local|ctx); returns a list or None."},
    {"spawn", _py_agent_spawn, METH_VARARGS, "Admission-only child spawn; returns the child sid or None."},
    {"report", _py_agent_report, METH_VARARGS, "End this frame with a report; returns True or False."},
    {"ask", _py_agent_ask, METH_VARARGS, "Publish an owner-surface ask (fire-and-post); returns 'asked' or the refusal text."},
    {NULL, NULL, 0, NULL}};

#define _PY_AGENT_TABLE_SIZE ((sizeof(_py_agent_base_methods) + sizeof(_py_agent_verb_methods)) / sizeof(PyMethodDef))

/* Combined table: allocated ONCE (benign-race CAS idiom — pyrt's global boot
   path calls this under its guard, but own-safe anyway), returned by every
   later call, freed by nobody: pyrt's module def references it for the life
   of the process. */
PyMethodDef* py_agent_methods_combined(void) {
  static _Atomic(PyMethodDef*) _py_agent_table = NULL;
  PyMethodDef* existing = atomic_load(&_py_agent_table);
  if (existing != NULL) {
    return existing;
  }
  PyMethodDef* candidate = get_clear_memory(sizeof(PyMethodDef) * _PY_AGENT_TABLE_SIZE);
  if (candidate == NULL) {
    return NULL;
  }
  size_t n = 0;
  for (size_t i = 0; _py_agent_base_methods[i].ml_name != NULL; i++) {
    candidate[n++] = _py_agent_base_methods[i];
  }
  for (size_t i = 0; _py_agent_verb_methods[i].ml_name != NULL; i++) {
    candidate[n++] = _py_agent_verb_methods[i];
  }
  PyMethodDef* expected = NULL;
  if (!atomic_compare_exchange_strong(&_py_agent_table, &expected, candidate)) {
    /* Lost the race: the winner's table is authoritative; drop the copy. */
    free(candidate);
    return atomic_load(&_py_agent_table);
  }
  return candidate;
}

#endif /* SA_HAS_PYTHON */