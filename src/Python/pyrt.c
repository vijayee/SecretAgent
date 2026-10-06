//
// Created by victor on 9/29/26.
//

#include "pyrt.h"
#include "py_subprocess.h"
#include "py_agent.h"

#ifdef SA_HAS_PYTHON

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "../Actor/actor.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
#include "../Util/budget.h"
#include "../Util/log.h"
#include "../Platform/platform.h"

#include <stdlib.h>
#include <string.h>

/* Work queue node: the runtime's private pending-cell entry. The queue is
   single-threaded (one pyrt OS thread per runtime pops it), so head/tail are
   maintained under py->lock and the popped node is handed off to the worker
   after an unlock. The node is NOT a pyrt_execute_payload_t; it is freed with
   plain free() once its payload has been extracted. */
typedef struct pyrt_work_node_t {
  pyrt_execute_payload_t* exec;
  struct pyrt_work_node_t* next;
} pyrt_work_node_t;

struct pyrt_t {
  actor_t* owner;
  pyrt_backend_e backend;
  platform_mutex_t* lock;
  platform_condvar_t* condition;
  pyrt_work_node_t* head;
  pyrt_work_node_t* tail;
  ATOMIC(uint8_t) shutdown;
  /* Externally visible view of the thread-local interp_live. */
  ATOMIC(uint8_t) active;
  ATOMIC(uint8_t) interrupt_req;
  /* The blocked-ask's one-park-at-a-time flag (escalation spec §1.1): set by
     the frame when it consumes a FRM_ASK, read by the agent.ask verb before
     publishing, cleared on the reply's consumption. Cleared at create —
     get_clear_memory zeroes it. */
  ATOMIC(uint8_t) ask_parked;
  ATOMIC(uint64_t) corr_counter;
  platform_thread_t* thread;
  PyThreadState* tstate;
  PyObject* main_dict;
  unsigned idle_evict_ms;
};

/* Forward declaration: _pyrt_interp_teardown (defined before the thread body
   calls it) returns its slot to the pool, so the pool pair is needed both
   before and after the backend helpers. */
static void _pyrt_slot_release(void);

/* Outbound channel: read by the injected actor module's callbacks on the
   pyrt thread (the only thread that re-enters Python here). Known milestone
   limitation: stdlib `threading` threads spawned by a cell do NOT inherit
   this TLS, so log/status/emit posts made from them are silently dropped
   (no routing, no error). */
static _Thread_local pyrt_t* _tls_pyrt = NULL;

/* Process-wide runtime state. _pyrt_global_guard is installed with the
   benign-race pattern from liboffs pool.c: every racing thread creates its
   own candidate mutex, the first CAS wins and keeps it, losers destroy
   theirs — nobody can hold a discarded mutex, so the destroy is safe. */
static _Atomic(platform_mutex_t*) _pyrt_global_guard = NULL;
static ATOMIC(uint8_t) _pyrt_global_ready = 0;
static ATOMIC(uint8_t) _pyrt_pool_ready = 0;
static size_t _pyrt_cap = 0;
static platform_mutex_t* _pyrt_slots_lock = NULL;
static platform_condvar_t* _pyrt_slots_cond = NULL;
static size_t _pyrt_live = 0;

/* Slot-wait granularity: the queued-worker wait is a timed wait so it can
   re-observe the runtime's shutdown flag between passes (the queued-destroy
   fix; see _pyrt_slot_acquire). */
#define _PYRT_SLOT_WAIT_MS 100

/* Runs once per subinterpreter; defines the cell executor the thread calls.
   `import actor` binds the injected module for the interp so a never-importing
   cell still speaks the bridge — live models emit bare `actor.*` calls.
   Printed stdout is captured per cell and IS the result (the subprocess
   envelope's parity — spec §5); the last expression's repr joins it as a
   closing "=> repr" line when both exist; a crashing cell keeps its partial
   stdout ahead of the traceback. */
static const char _PYRT_HELPERS[] =
    "import actor\n"
    "def __sa_exec_cell(code):\n"
    "    import traceback, io, sys\n"
    "    buf = io.StringIO()\n"
    "    old = sys.stdout\n"
    "    sys.stdout = buf\n"
    "    try:\n"
    "        try:\n"
    "            result = eval(compile(code, '<cell>', 'eval'), globals())\n"
    "        except SyntaxError:\n"
    "            exec(compile(code, '<cell>', 'exec'), globals())\n"
    "            result = None\n"
    "        result_txt = None if result is None else repr(result)\n"
    "        if result is not None:\n"
    "            globals()['_'] = result\n"
    "        sys.stdout = old\n"
    "        out = buf.getvalue()\n"
    "        if out.endswith('\\n'):\n"
    "            out = out[:-1]\n"
    "        if out:\n"
    "            if result_txt is not None:\n"
    "                return 0, out + '\\n=> ' + result_txt\n"
    "            return 0, out\n"
    "        return 0, result_txt if result_txt is not None else ''\n"
    "    except BaseException:\n"
    "        status, text = 1, traceback.format_exc()\n"
    "        try:\n"
    "            sys.stdout = old\n"
    "            out = buf.getvalue()\n"
    "            if out.endswith('\\n'):\n"
    "                out = out[:-1]\n"
    "            if out:\n"
    "                text = out + '\\n' + text\n"
    "        except BaseException:\n"
    "            sys.stdout = old\n"
    "        return status, text\n";

/* ------------------------------------------------------------------ */
/* Payload destroyers (pyrt_messages.h). NULL-safe, plain free().      */
/* ------------------------------------------------------------------ */

void pyrt_execute_payload_destroy(void* payload) {
  if (payload == NULL) return;
  pyrt_execute_payload_t* exec = (pyrt_execute_payload_t*)payload;
  free(exec->code);
  free(exec);
}

void pyrt_result_payload_destroy(void* payload) {
  if (payload == NULL) return;
  pyrt_result_payload_t* result = (pyrt_result_payload_t*)payload;
  free(result->text);
  free(result);
}

void pyrt_text_payload_destroy(void* payload) {
  if (payload == NULL) return;
  pyrt_text_payload_t* text = (pyrt_text_payload_t*)payload;
  free(text->text);
  free(text);
}

/* ------------------------------------------------------------------ */
/* Outbound post: never re-enters Python, takes no locks besides what  */
/* actor_send needs, and copies the text OUT of Python's heap first.   */
/* ------------------------------------------------------------------ */

static void _pyrt_post_text(pyrt_t* py, uint32_t type, const char* text) {
  if (py == NULL || py->owner == NULL || text == NULL) return;
  pyrt_text_payload_t* payload = get_clear_memory(sizeof(pyrt_text_payload_t));
  payload->text = strdup(text);
  message_t msg;
  msg.type = type;
  msg.payload = payload;
  msg.payload_destroy = pyrt_text_payload_destroy;
  actor_send(py->owner, &msg);
}

/* Result routing: text ownership transfers; NULL text means "nothing
   happened" and is never routed. */
static void _pyrt_post_result(pyrt_t* py, uint64_t corr, uint8_t status, char* text) {
  if (text == NULL) return;
  /* The ONE source cap (budget table, spec §4): both backends post through
     here, so the routed result text is bounded in one place. The marker
     travels with the text — the model learns the cut at its next derive.
     This function OWNS text: the cap exchange frees the old buffer. */
  char* capped = NULL;
  uint8_t truncated = 0;
  budget_truncate_with_marker(text, SA_BUDGET_CELL_RESULT_BYTES, &capped,
                              &truncated);
  free(text);
  if (capped == NULL) {
    /* The budget helper refused (OOM/cap 0): the corr-matched shape survives
       the cap's refusal (unless the fallback copy itself fails) — post the
       loud literal instead. */
    capped = strdup("pyrt: the result budget refused the text");
    if (capped == NULL) return;
  }
  (void)truncated;
  text = capped;
  if (py == NULL || py->owner == NULL) {
    free(text);
    return;
  }
  pyrt_result_payload_t* payload = get_clear_memory(sizeof(pyrt_result_payload_t));
  payload->corr = corr;
  payload->status = status;
  payload->text = text;
  message_t msg;
  msg.type = PYRT_RESULT;
  msg.payload = payload;
  msg.payload_destroy = pyrt_result_payload_destroy;
  actor_send(py->owner, &msg);
}

/* ------------------------------------------------------------------ */
/* Injected `actor` module (inittab-registered before any Py_Initialize). */
/* The method callbacks — log/status/emit AND the four bridge verbs — live */
/* in py_agent.c; this file owns the module DEF and the thread-local state */
/* they route through.                                                     */
/* ------------------------------------------------------------------ */

/* Multi-phase init is REQUIRED, not optional: the pinned 3.12.13 caches a
   single-phase (m_size < 0) built-in def in the process-wide extension table
   and then raises "module actor does not support loading in subinterpreters"
   on the second subinterpreter (import.c create_builtin/fix_up_extension).
   The exec slot runs per interpreter; m_size 0 means per-interpreter module
   state (the def carries no static mutable state — its callbacks route
   through _tls_pyrt only). Py_mod_multiple_interpreters is declared
   consistent with that: the module is trivially per-interpreter-GIL safe. */
static int _py_actor_exec(PyObject* module) {
  (void)module;
  return 0;
}

static struct PyModuleDef_Slot _py_actor_slots[] = {
    {Py_mod_exec, _py_actor_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {0, NULL}};

/* Field order matches struct PyModuleDef: m_base, m_name, m_doc, m_size,
   m_methods, m_slots, m_traverse, m_clear, m_free. m_size >= 0 selects
   multi-phase. m_methods is NULL here and pointed at py_agent's COMBINED
   table (the base log/status/emit verbs + the bridge verbs) at the first
   global boot, before the inittab entry can ever be imported. */
static struct PyModuleDef _py_actor_moduledef = {
    PyModuleDef_HEAD_INIT, "actor", NULL, 0, NULL,
    _py_actor_slots, NULL, NULL, NULL};

/* Inittab entry point: returning the def (PyModuleDef_Type) makes
   import.c create_builtin take the PyModule_FromDefAndSpec path, which
   re-initializes the module (exec slot) for every interpreter that imports
   it — exactly what multi-phase init provides. */
static PyObject* _py_actor_create(void) {
  return PyModuleDef_Init(&_py_actor_moduledef);
}

/* ------------------------------------------------------------------ */
/* Subinterpreter backend (pyrt thread only).                          */
/* ------------------------------------------------------------------ */

/* Boot one PEP-684 subinterpreter (own GIL, no main-obmalloc sharing) and
   install the cell helpers. On success the new tstate is CURRENT on this
   thread (new_interpreter swaps it in) and its GIL is held. */
static uint8_t _pyrt_interp_boot(pyrt_t* py) {
  PyThreadState* tstate = NULL;
  PyInterpreterConfig config;
  memset(&config, 0, sizeof(config));
  config.use_main_obmalloc = 0;
  /* Contract amendment: the plan set check_multi_interp_extensions = 0, but
     the pinned 3.12.13 init_interp_settings rejects that combination
     ("per-interpreter obmalloc does not support single-phase init extension
     modules"); with per-interpreter obmalloc, extension checking must be ON.
     The pinned source wins per the plan's verification rule. */
  config.check_multi_interp_extensions = 1;
  config.gil = PyInterpreterConfig_OWN_GIL;

  PyStatus st = Py_NewInterpreterFromConfig(&tstate, &config);
  if (PyStatus_Exception(st) || tstate == NULL) {
    log_error("pyrt: subinterpreter boot failed");
    return 0;
  }
  py->tstate = tstate;

  PyObject* main_module = PyImport_AddModule("__main__");
  if (main_module == NULL) {
    PyErr_Clear();
    log_error("pyrt: subinterpreter __main__ missing");
    Py_EndInterpreter(py->tstate);
    py->tstate = NULL;
    return 0;
  }
  py->main_dict = PyModule_GetDict(main_module); /* borrowed */

  PyObject* rc = PyRun_String(_PYRT_HELPERS, Py_file_input, py->main_dict, py->main_dict);
  if (rc == NULL) {
    PyErr_Clear();
    log_error("pyrt: helper injection failed");
    Py_EndInterpreter(py->tstate);
    py->tstate = NULL;
    py->main_dict = NULL;
    return 0;
  }
  Py_DECREF(rc);
  ATOMIC_STORE(&py->active, 1);
  return 1;
}

/* On the pyrt thread with its tstate current. */
static void _pyrt_interp_teardown(pyrt_t* py) {
  if (py->tstate != NULL) {
    Py_EndInterpreter(py->tstate);
  }
  py->tstate = NULL;
  py->main_dict = NULL;
  ATOMIC_STORE(&py->active, 0);
  _pyrt_slot_release();
}

/* Run one cell through the injected helper. Returns 1 on error. *out_text
   always receives a strdup'd string (caller frees). */
static uint8_t _pyrt_run_cell_sub(pyrt_t* py, const char* code, char** out_text) {
  PyObject* fn = PyDict_GetItemString(py->main_dict, "__sa_exec_cell");
  if (fn == NULL) {
    PyErr_Clear();
    *out_text = strdup("pyrt: helpers missing");
    return 1;
  }
  PyObject* result = PyObject_CallFunction(fn, "s", code);
  if (result == NULL) {
    PyErr_Clear();
    *out_text = strdup("pyrt: cell failed to run");
    return 1;
  }
  int status = 0;
  const char* text = NULL;
  if (!PyArg_ParseTuple(result, "is", &status, &text)) {
    PyErr_Clear();
    /* Parsing failed, so no borrowed `text` pointer survived to escape: the
       replacement literal is strdup'd OUT of Python's heap. */
    *out_text = strdup("pyrt: bad cell result");
    Py_DECREF(result);
    return 1;
  }
  /* The tuple owns the string; strdup must precede any DECREF (otherwise a
     failing cell's traceback text loses its last owner at the DECREF and
     the copy below reads freed memory). */
  *out_text = strdup(text != NULL ? text : "");
  Py_DECREF(result);
  return (uint8_t)(status != 0);
}

/* ------------------------------------------------------------------ */
/* Global CPython boot + interpreter pool.                             */
/* ------------------------------------------------------------------ */

static uint8_t _pyrt_global_init(void) {
  if (ATOMIC_LOAD(&_pyrt_global_ready)) {
    return 1;
  }
  /* Benign-race guard install (liboffs pool.c idiom): every racing caller
     makes a candidate mutex; the first CAS wins and keeps it, losers destroy
     their own candidate — no one can hold a discarded mutex. */
  platform_mutex_t* m = platform_mutex_create();
  platform_mutex_t* expected = NULL;
  if (!atomic_compare_exchange_strong(&_pyrt_global_guard, &expected, m)) {
    platform_mutex_destroy(m);
  }
  platform_mutex_t* guard = atomic_load(&_pyrt_global_guard);
  if (guard != NULL) {
    platform_mutex_lock(guard);
  }
  if (!ATOMIC_LOAD(&_pyrt_global_ready)) {
    /* The injected module's method table (py_agent.c): the base stream verbs
       plus the bridge verbs, in ONE heap table. Built first — a NULL table
       fails the boot loudly before the inittab entry can ever be imported. */
    PyMethodDef* methods = py_agent_methods_combined();
    if (methods == NULL) {
      log_error("pyrt: building the combined 'actor' method table failed");
      if (guard != NULL) {
        platform_mutex_unlock(guard);
      }
      return 0;
    }
    _py_actor_moduledef.m_methods = methods;
    /* The bridge reply registry mounts HERE, on the first global boot,
       serialized under the same guard that serializes Py_Initialize —
       py_agent's reply sink (py_agent_note_reply) is thereby installed
       before any frame can dispatch and before any cell can run. This is
       the runtime layer's one-way startup wire documented on
       frame_bridge.h (frame.c: register -> behaviors answer corr-matched
       -> py_agent_wait_t records wake). */
    py_agent_init();
    if (PyImport_AppendInittab("actor", _py_actor_create) != 0) {
      log_error("pyrt: PyImport_AppendInittab('actor') failed");
      if (guard != NULL) {
        platform_mutex_unlock(guard);
      }
      return 0;
    }
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.install_signal_handlers = 0;
#ifdef SA_PYTHON_EXECUTABLE
    /* No anchor for stdlib discovery otherwise: embedding a static
       libpython gives getpath no executable, so the build-directory
       landmark (pybuilddir.txt) is missed. Pointing config.executable at
       the built `python` pins the paths to this build's source Lib dir and
       compiled modules dir deterministically. */
    wchar_t* exe = Py_DecodeLocale(SA_PYTHON_EXECUTABLE, NULL);
    if (exe != NULL) {
      PyStatus st = PyConfig_SetString(&config, &config.executable, exe);
      PyMem_RawFree(exe);
      if (PyStatus_Exception(st)) {
        log_error("pyrt: PyConfig_SetString(executable) failed");
        PyConfig_Clear(&config);
        if (guard != NULL) {
          platform_mutex_unlock(guard);
        }
        return 0;
      }
    } else {
      /* Continue unpinned: surface the failure now instead of masking it
         until a later, symptom-level boot error. */
      log_error("pyrt: SA_PYTHON_EXECUTABLE decode failed");
    }
#endif
    PyStatus st = Py_InitializeFromConfig(&config);
    if (PyStatus_Exception(st)) {
      PyConfig_Clear(&config);
      log_error("pyrt: Py_InitializeFromConfig failed");
      if (guard != NULL) {
        platform_mutex_unlock(guard);
      }
      return 0;
    }
    PyConfig_Clear(&config);
    /* The init thread never holds the main GIL again. */
    PyEval_SaveThread();
    ATOMIC_STORE(&_pyrt_global_ready, 1);
  }
  if (guard != NULL) {
    platform_mutex_unlock(guard);
  }
  return ATOMIC_LOAD(&_pyrt_global_ready);
}

/* Idempotent; the FIRST created runtime fixes the cap. Guarded by the same
   benign-race mutex _pyrt_global_init installed. */
static void _pyrt_pool_init(size_t cap) {
  if (ATOMIC_LOAD(&_pyrt_pool_ready)) {
    return;
  }
  platform_mutex_t* guard = atomic_load(&_pyrt_global_guard);
  if (guard != NULL) {
    platform_mutex_lock(guard);
  }
  if (!ATOMIC_LOAD(&_pyrt_pool_ready)) {
    _pyrt_cap = cap != 0 ? cap : (size_t)platform_core_count() * 2;
    _pyrt_slots_lock = platform_mutex_create();
    _pyrt_slots_cond = platform_condvar_create();
    ATOMIC_STORE(&_pyrt_pool_ready, 1);
  }
  if (guard != NULL) {
    platform_mutex_unlock(guard);
  }
}

/* Never called while holding another lock. Returns 1 with a slot held; 0 when
   the runtime shut down while queued for a slot (nothing acquired — the caller
   must NOT release and must settle its cell via the drain rule). Shutdown
   awareness is REQUIRED, not stylistic: a worker queued behind a full pool
   used to block on the global condvar with no wake on pyrt_destroy, so
   destroying the queued runtime would hang pyrt_destroy's join for the whole
   remaining hold of every slot. Each pass is a timed wait, so the shutdown
   flag is re-observed within _PYRT_SLOT_WAIT_MS and the join is bounded. */
static uint8_t _pyrt_slot_acquire(pyrt_t* py) {
  platform_mutex_lock(_pyrt_slots_lock);
  while (ATOMIC_LOAD(&py->shutdown) == 0 && _pyrt_live >= _pyrt_cap) {
    platform_condvar_timed_wait(_pyrt_slots_cond, _pyrt_slots_lock,
                                _PYRT_SLOT_WAIT_MS);
  }
  uint8_t acquired = (uint8_t)(ATOMIC_LOAD(&py->shutdown) == 0 ? 1 : 0);
  if (acquired) {
    _pyrt_live += 1;
    platform_condvar_broadcast(_pyrt_slots_cond);
  }
  platform_mutex_unlock(_pyrt_slots_lock);
  return acquired;
}

static void _pyrt_slot_release(void) {
  platform_mutex_lock(_pyrt_slots_lock);
  if (_pyrt_live > 0) {
    _pyrt_live -= 1;
  }
  platform_condvar_broadcast(_pyrt_slots_cond);
  platform_mutex_unlock(_pyrt_slots_lock);
}

/* ------------------------------------------------------------------ */
/* The pyrt thread.                                                    */
/* ------------------------------------------------------------------ */

static void* _pyrt_thread(void* arg) {
  pyrt_t* py = (pyrt_t*)arg;
  _tls_pyrt = py;

  uint8_t interp_live = 0;
  if (!_pyrt_global_init()) {
    /* Boot failures never strand an execute silently: the owner learns via
       its own timeout. */
    _tls_pyrt = NULL;
    return 0;
  }

  if (py->backend == SA_PYRT_BACKEND_SUBPROCESS) {
    /* Thread is the resource; cells are stateless per-call. */
    ATOMIC_STORE(&py->active, 1);
  }

  for (;;) {
    platform_mutex_lock(py->lock);
    /* Wait for work, honing the idle-eviction window while an interpreter
       is live (eviction only makes sense with something to evict). */
    while (py->head == NULL && ATOMIC_LOAD(&py->shutdown) == 0) {
      if (py->idle_evict_ms != 0 && interp_live) {
        int timedout = platform_condvar_timed_wait(py->condition, py->lock, py->idle_evict_ms);
        if (timedout == -1 && py->head == NULL && ATOMIC_LOAD(&py->shutdown) == 0) {
          platform_mutex_unlock(py->lock);
          _pyrt_interp_teardown(py);
          interp_live = 0;
          platform_mutex_lock(py->lock);
          continue;
        }
        continue;
      }
      platform_condvar_wait(py->condition, py->lock);
    }
    if (ATOMIC_LOAD(&py->shutdown) && py->head == NULL) {
      /* Shutdown does not abandon queued cells: anything queued before
         pyrt_destroy() must still get a corr-matched RESULT. The queue was
         just observed empty, so nothing is owed and the thread may exit;
         if a non-empty queue is seen below, the drain block after the cell
         re-checks and only exits once the queue is fully drained. */
      platform_mutex_unlock(py->lock);
      break;
    }
    pyrt_work_node_t* node = py->head;
    if (node != NULL) {
      py->head = node->next;
      if (py->head == NULL) {
        py->tail = NULL;
      }
    }
    uint8_t had_shutdown = ATOMIC_LOAD(&py->shutdown);
    /* Unlock before ANY dispatch work (slot acquire, GIL use, subprocess
       spawn): interrupt() and destroy() take this lock. */
    platform_mutex_unlock(py->lock);

    if (node == NULL) {
      continue;
    }
    pyrt_execute_payload_t* exec = node->exec;
    /* The work node is not a pyrt_execute_payload_t; plain free() after
       extracting the payload (plan-contract correction). */
    free(node);

    if (py->backend == SA_PYRT_BACKEND_SUBPROCESS) {
      /* Stateless per cell: the slot is held for the duration of this cell's
         run (spawn through waitpid), so the pool cap bounds concurrent
         subprocesses exactly like interpreters.
         We are already outside py->lock here (unlocked before dispatch). */
      if (!_pyrt_slot_acquire(py)) {
        /* Drain rule (queued-destroy fix): shutdown observed while queued for
           a slot — the stranded cell gets its corr-matched status-1 refusal
           and no slot is released (none was held). */
        _pyrt_post_result(py, exec->corr, 1,
                          strdup("pyrt: destroyed while queued"));
        pyrt_execute_payload_destroy(exec);
        continue;
      }
      char* proc_text = NULL;
      char* proc_err = NULL;
      uint8_t status = pyrt_run_cell_subprocess(exec->code, &proc_text, &proc_err);
      if (proc_err != NULL) {
        _pyrt_post_text(py, PYRT_STATUS, proc_err);
        free(proc_err);
      }
      _pyrt_post_result(py, exec->corr, status, proc_text);
      _pyrt_slot_release();
      pyrt_execute_payload_destroy(exec);
      continue;
    }

    /* Interrupt-touched dispatch (Task 6; cooperative-only, see
       pyrt_interrupt): a flagged request with NO interpreter live can only
       refer to a cell that already crossed a boundary (finished and took
       the flag with it via the per-cell clear below) or one that never
       reached a boot — the pending cell is cut at its start boundary. The
       ABA-trap fold: the dropped cell always posts its corr-matched status
       1 RESULT here, so the owner is never left hanging to timeout. */
    /* Idle-state asymmetry (shipped, documented): the cut fires only while
       NO interpreter is live; a request armed in the idle gap with the
       interpreter live is not honored for the next queued cell — the cell
       runs and the per-cell clear below consumes the flag. */
    if (ATOMIC_LOAD(&py->interrupt_req) == 1 && !interp_live) {
      ATOMIC_STORE(&py->interrupt_req, 0);
      /* _pyrt_post_result takes ownership of the text, so the literal must
         be copied out before it can be routed (freed at payload destroy). */
      _pyrt_post_result(py, exec->corr, 1,
                        strdup("pyrt: interrupted at boundary"));
      pyrt_execute_payload_destroy(exec);
      continue;
    }

    if (!interp_live) {
      if (!_pyrt_slot_acquire(py)) { /* may block on a full pool; nothing else is held */
        /* Same drain rule as the subprocess branch: shutdown observed while
           queued for a slot — corr-matched refusal, no slot to release. */
        _pyrt_post_result(py, exec->corr, 1,
                          strdup("pyrt: destroyed while queued"));
        pyrt_execute_payload_destroy(exec);
        continue;
      }
      if (!_pyrt_interp_boot(py)) {
        /* Same ownership rule: the result text must be heap-owned. This
           call site predates the interrupt refusal above and had the same
           latent free-a-literal corruption; fixed alongside it. */
        _pyrt_post_result(py, exec->corr, 1,
                          strdup("pyrt: subinterpreter boot failed"));
        pyrt_execute_payload_destroy(exec);
        /* No interpreter is live after a failed boot: return the slot so
           the pool does not leak it across retries. */
        _pyrt_slot_release();
        continue;
      }
      interp_live = 1;
    }

    char* text = NULL;
    uint8_t status = _pyrt_run_cell_sub(py, exec->code, &text);
    /* Flag-vs-cell semantics (Task 6): the interrupt flag is per-runtime,
       never per-corr — it is consumed exactly at each boundary the thread
       crosses. A request that arrives while a cell is already running is
       too late to act on that cell (cooperative delivery cannot preempt
       it), so the finishing cell takes the flag with it and posts its
       NORMAL corr-matched result; clearing here keeps that late request
       from leaking onto the next queued cell (the stale-dispatch refusal
       above only fires when no interpreter is live). */
    ATOMIC_STORE(&py->interrupt_req, 0);
    _pyrt_post_result(py, exec->corr, status, text);
    pyrt_execute_payload_destroy(exec);
    if (had_shutdown) {
      /* Drain rule (see the shutdown-break comment in the wait loop): a
         cell queued before shutdown still owed a RESULT, which was just
         posted — break only once the queue has been verified empty, so
         every pre-shutdown cell is dispatched and answered. */
      platform_mutex_lock(py->lock);
      uint8_t drained = (py->head == NULL);
      platform_mutex_unlock(py->lock);
      if (drained) {
        break;
      }
    }
  }

  if (interp_live) {
    _pyrt_interp_teardown(py);
  }
  _tls_pyrt = NULL;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Public API (frozen signatures from pyrt.h).                         */
/* ------------------------------------------------------------------ */

/* py_agent.c reads the TLS state through these two (the struct and the TLS
   itself stay private to pyrt.c). Both are meaningful ONLY on the pyrt
   thread of a runtime — exactly where the injected module's callbacks run. */

actor_t* pyrt_thread_owner(void) {
  if (_tls_pyrt == NULL) return NULL;
  return _tls_pyrt->owner;
}

pyrt_t* pyrt_thread_pyrt(void) {
  return _tls_pyrt;
}

void pyrt_ask_parked_set(pyrt_t* pyrt, uint8_t parked) {
  if (pyrt == NULL) return;
  ATOMIC_STORE(&pyrt->ask_parked, parked);
}

uint8_t pyrt_ask_parked(const pyrt_t* pyrt) {
  if (pyrt == NULL) return 0;
  return (uint8_t)ATOMIC_LOAD(&pyrt->ask_parked);
}

void pyrt_post_text(uint32_t type, const char* text) {
  _pyrt_post_text(_tls_pyrt, type, text);
}

pyrt_t* pyrt_create(actor_t* owner, const pyrt_config_t* cfg) {
  if (!_pyrt_global_init()) {
    return NULL;
  }
  _pyrt_pool_init(cfg != NULL ? cfg->pool_cap : 0);
  pyrt_t* py = get_clear_memory(sizeof(pyrt_t));
  py->owner = owner;
  py->backend = cfg != NULL ? cfg->backend : SA_PYRT_BACKEND_SUBINTERPRETER;
  py->idle_evict_ms = cfg != NULL ? cfg->idle_evict_ms : 0;
  py->lock = platform_mutex_create();
  py->condition = platform_condvar_create();
  return py;
}

uint64_t pyrt_execute(pyrt_t* pyrt, char* code) {
  if (pyrt == NULL || code == NULL) {
    free(code);
    return 0;
  }
  pyrt_execute_payload_t* exec = get_clear_memory(sizeof(pyrt_execute_payload_t));
  exec->code = code; /* ownership transferred */
  exec->corr = ATOMIC_FETCH_ADD(&pyrt->corr_counter, 1) + 1;

  platform_mutex_lock(pyrt->lock);
  /* Late EXECUTEs during teardown are rejected, never queued. */
  if (ATOMIC_LOAD(&pyrt->shutdown)) {
    platform_mutex_unlock(pyrt->lock);
    pyrt_execute_payload_destroy(exec);
    return 0;
  }
  /* Lazy boot on the first EXECUTE. Under the lock: a racing second
     execute cannot double-create the thread, and the freshly started
     thread cannot observe a queue state other than the one we install
     before the broadcast (it blocks on this lock until we unlock). */
  if (pyrt->thread == NULL) {
    pyrt->thread = platform_thread_create(_pyrt_thread, pyrt);
    if (pyrt->thread == NULL) {
      platform_mutex_unlock(pyrt->lock);
      log_error("pyrt: failed to start the pyrt thread");
      pyrt_execute_payload_destroy(exec);
      return 0;
    }
  }
  pyrt_work_node_t* node = get_clear_memory(sizeof(pyrt_work_node_t));
  node->exec = exec;
  if (pyrt->tail != NULL) {
    pyrt->tail->next = node;
  } else {
    pyrt->head = node;
  }
  pyrt->tail = node;
  platform_condvar_broadcast(pyrt->condition);
  platform_mutex_unlock(pyrt->lock);
  return exec->corr;
}

void pyrt_interrupt(pyrt_t* pyrt) {
  if (pyrt == NULL) return;
  /* Interrupt delivery is COOPERATIVE by verification, pinning 3.12.13:
     the FORCED paths cannot reach a PEP-684 subinterpreter from this thread.
     (1) PyThreadState_SetAsyncExc (pystate.c:1808) must be called with the
     GIL held and scans interp->threads.head of the CALLING thread's current
     interpreter (_PyInterpreterState_GET()); the embedder thread can only
     obtain the MAIN interpreter's GIL (PyGILState_Ensure makes a main-interp
     tstate), so the worker thread is not in the scanned list and the
     call returns 0 — probed empirically: rc=0 while a busy subinterpreter
     cell completed normally, status 0, no KeyboardInterrupt. (2)
     PyErr_SetInterruptEx (Modules/signalmodule.c:1886) drives process-global
     signal machinery: with install_signal_handlers=0 no handler is
     installed, so get_handler returns the default handler and the call is a
     no-op (probe: rc=0, cell unaffected) — and even tripped, it raises only
     on threads that run PyErr_CheckSignals under the main GIL. Shipped
     cooperative-only: the flag is honored at cell boundaries by the thread
     loop (see the _pyrt_thread interrupt handling). */
  ATOMIC_STORE(&pyrt->interrupt_req, 1);
}

uint8_t pyrt_isactive(const pyrt_t* pyrt) {
  if (pyrt == NULL) return 0;
  return (uint8_t)ATOMIC_LOAD(&pyrt->active);
}

void pyrt_destroy(pyrt_t* pyrt) {
  if (pyrt == NULL) return;
  platform_mutex_lock(pyrt->lock);
  ATOMIC_STORE(&pyrt->shutdown, 1);
  platform_condvar_broadcast(pyrt->condition);
  platform_mutex_unlock(pyrt->lock);
  /* Queued-destroy fix: a worker holding NO slot can be parked on the GLOBAL
     pool condvar inside _pyrt_slot_acquire; only py->condition's broadcast
     never reaches it and the join below would hang for the whole remaining
     hold of every pool slot. Shutdown is observable for that waiter too, so
     it must also see a wake. Liboffs condvar discipline: broadcast while
     holding the condvar's paired lock. */
  platform_mutex_lock(_pyrt_slots_lock);
  platform_condvar_broadcast(_pyrt_slots_cond);
  platform_mutex_unlock(_pyrt_slots_lock);
  if (pyrt->thread != NULL) {
    platform_thread_join(pyrt->thread); /* the thread tears down its backend */
  }
  platform_mutex_destroy(pyrt->lock);
  platform_condvar_destroy(pyrt->condition);
  /* Shutdown drains the queue before the thread exits; nodes abandoned
     mid-pop are freed on their own dispatch. */
  free(pyrt);
}

#endif /* SA_HAS_PYTHON */
