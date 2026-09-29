//
// Created by victor on 9/29/26.
//

#include "pyrt.h"

#ifdef SA_HAS_PYTHON

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "../Actor/actor.h"
#include "../Util/allocator.h"
#include "../Util/atomic_compat.h"
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
  uint8_t shutdown;
  ATOMIC(uint8_t) active;
  ATOMIC(uint8_t) interrupt_req;
  ATOMIC(uint64_t) corr_counter;
  platform_thread_t* thread;
  unsigned long thread_id;
  PyThreadState* tstate;
  PyObject* main_dict;
  unsigned idle_evict_ms;
};

/* Forward declaration: _pyrt_interp_teardown (defined before the thread body
   calls it) returns its slot to the pool, so the pool pair is needed both
   before and after the backend helpers. */
static void _pyrt_slot_release(void);

/* Outbound channel: read by the injected actor module's callbacks on the
   pyrt thread (the only thread that re-enters Python here). */
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

/* Runs once per subinterpreter; defines the cell executor the thread calls. */
static const char _PYRT_HELPERS[] =
    "def __sa_exec_cell(code):\n"
    "    import traceback\n"
    "    try:\n"
    "        try:\n"
    "            result = eval(compile(code, '<cell>', 'eval'), globals())\n"
    "        except SyntaxError:\n"
    "            exec(compile(code, '<cell>', 'exec'), globals())\n"
    "            result = None\n"
    "        if result is not None:\n"
    "            globals()['_'] = result\n"
    "            return 0, repr(result)\n"
    "        return 0, ''\n"
    "    except BaseException:\n"
    "        return 1, traceback.format_exc()\n";

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
/* ------------------------------------------------------------------ */

static PyObject* _py_actor_log(PyObject* self, PyObject* args) {
  const char* text = NULL;
  if (!PyArg_ParseTuple(args, "s", &text)) {
    return NULL;
  }
  _pyrt_post_text(_tls_pyrt, PYRT_LOG, text);
  Py_RETURN_NONE;
}

static PyObject* _py_actor_status(PyObject* self, PyObject* args) {
  const char* text = NULL;
  if (!PyArg_ParseTuple(args, "s", &text)) {
    return NULL;
  }
  _pyrt_post_text(_tls_pyrt, PYRT_STATUS, text);
  Py_RETURN_NONE;
}

static PyObject* _py_actor_emit(PyObject* self, PyObject* args) {
  const char* text = NULL;
  if (!PyArg_ParseTuple(args, "s", &text)) {
    return NULL;
  }
  _pyrt_post_text(_tls_pyrt, PYRT_EMIT, text);
  Py_RETURN_NONE;
}

static PyMethodDef _py_actor_methods[] = {
    {"log", _py_actor_log, METH_VARARGS, "Stream live narration to the owning actor."},
    {"status", _py_actor_status, METH_VARARGS, "Stream the current status to the owning actor."},
    {"emit", _py_actor_emit, METH_VARARGS, "Post a durable-payload candidate to the owning actor."},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef _py_actor_moduledef = {
    PyModuleDef_HEAD_INIT, "actor", NULL, -1, _py_actor_methods, NULL, NULL, NULL, NULL};

static PyObject* _py_actor_create(void) {
  return PyModule_Create(&_py_actor_moduledef);
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
    Py_DECREF(result);
    *out_text = strdup("pyrt: bad cell result");
    return 1;
  }
  Py_DECREF(result);
  *out_text = strdup(text != NULL ? text : "");
  return status != 0;
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

/* Never called while holding another lock. */
static void _pyrt_slot_acquire(void) {
  platform_mutex_lock(_pyrt_slots_lock);
  while (_pyrt_live >= _pyrt_cap) {
    platform_condvar_wait(_pyrt_slots_cond, _pyrt_slots_lock);
  }
  _pyrt_live += 1;
  platform_condvar_broadcast(_pyrt_slots_cond);
  platform_mutex_unlock(_pyrt_slots_lock);
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
  py->thread_id = (unsigned long)platform_thread_self();

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
    while (py->head == NULL && !py->shutdown) {
      if (py->idle_evict_ms != 0 && interp_live) {
        int timedout = platform_condvar_timed_wait(py->condition, py->lock, py->idle_evict_ms);
        if (timedout == -1 && py->head == NULL && !py->shutdown) {
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
    if (py->shutdown && py->head == NULL) {
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
    uint8_t had_shutdown = py->shutdown;
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
      /* Task 9 replaces this with the real spawn path; queued subprocess
         cells are freed, never leaked. */
      pyrt_execute_payload_destroy(exec);
      continue;
    }

    /* Stale interrupt: the flagged cell already finished. */
    if (ATOMIC_LOAD(&py->interrupt_req) == 1 && !interp_live) {
      ATOMIC_STORE(&py->interrupt_req, 0);
      pyrt_execute_payload_destroy(exec);
      continue;
    }

    if (!interp_live) {
      _pyrt_slot_acquire(); /* may block; nothing else is held */
      if (!_pyrt_interp_boot(py)) {
        _pyrt_post_result(py, exec->corr, 1, "pyrt: subinterpreter boot failed");
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
    ATOMIC_STORE(&py->interrupt_req, 0);
    _pyrt_post_result(py, exec->corr, status, text);
    pyrt_execute_payload_destroy(exec);
    if (had_shutdown) {
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
  /* This task: flag only. Task 6 implements the verified mechanism. */
  ATOMIC_STORE(&pyrt->interrupt_req, 1);
}

uint8_t pyrt_isactive(const pyrt_t* pyrt) {
  if (pyrt == NULL) return 0;
  return (uint8_t)ATOMIC_LOAD(&pyrt->active);
}

void pyrt_destroy(pyrt_t* pyrt) {
  if (pyrt == NULL) return;
  platform_mutex_lock(pyrt->lock);
  pyrt->shutdown = 1;
  platform_condvar_broadcast(pyrt->condition);
  platform_mutex_unlock(pyrt->lock);
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