//
// Created by victor on 9/29/26.
//

#ifndef SA_PY_AGENT_H
#define SA_PY_AGENT_H

#ifdef SA_HAS_PYTHON
#include <Python.h>
/* Appends the bridge verb methods to pyrt's base table (log/status/emit);
   called from pyrt's module creation. Returns the COMBINED table (heap;
   module-owned: built ONCE on the first call — before any interpreter can
   import the module — and returned by every later call; it lives for the
   process). NULL on OOM. */
PyMethodDef* py_agent_methods_combined(void);
/* Mounts the bridge reply registry: creates the completion infra and
   registers py_agent_note_reply with frame_bridge. Idempotent (the infra is
   built once and further calls just re-register the sink — tests demount
   frame_bridge's sink between suites and remount it here). Called ONCE from
   pyrt's first global boot (see pyrt.c), which is the runtime layer's
   one-way startup wire documented on frame_bridge.h. */
void py_agent_init(void);
/* The reply sink registered with frame_bridge at runtime init. Called on the
   frame's dispatch thread; must not block and must not free `text` (the text
   is BORROWED — this copy is the waiter's only copy). Corr-matched: wakes
   exactly the waiter that posted the request; a reply with no waiting
   waiter (e.g. the caller already timed out) is dropped loudly. */
void py_agent_note_reply(uint64_t corr, uint8_t status, const char* text);
#endif /* SA_HAS_PYTHON */

#endif // SA_PY_AGENT_H