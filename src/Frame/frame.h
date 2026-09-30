//
// Created by victor on 9/29/26.
//

#ifndef SA_FRAME_H
#define SA_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef SA_HAS_WDB
#include "../Actor/actor.h"

typedef struct wave_database_root_t wave_database_root_t;   /* opaque; owns ONE root db */

/* Config (immutable after create): */
typedef struct frame_config_t {
  const char* model_base_url;    /* e.g. http://127.0.0.1:11434 (Ollama) */
  const char* model_api_key;     /* may be NULL/empty for Ollama */
  const char* model_name;        /* e.g. a local model tag */
  unsigned max_depth;            /* SA_FRAME_MAX_DEPTH equivalent (default 4) */
  unsigned model_timeout_ms;     /* one completion POST bound, ms; 0 = the
                                    built-in default (model.h's
                                    SA_MODEL_TIMEOUT_MS). Local models on
                                    big tool-calling turns can run minutes —
                                    the default 30 s is for cloud endpoints. */
} frame_config_t;

wave_database_root_t* wave_db_open(const char* location /* NULL = in-memory */);
void wave_db_close(wave_database_root_t* db);

typedef struct frame_t frame_t;

typedef struct model_backend_t model_backend_t;   /* defined by model.h (Task 9) */

/* parent NULL = top-level session (sid generated); goal may be NULL. */
frame_t* frame_create(wave_database_root_t* db, frame_t* parent,
                      const char* goal, const frame_config_t* cfg);
/* Boot-time restore of a frame whose subtree already exists on disk (the
   restart path): opens the subtree at `sid` (the full path, e.g.
   "sessions/<hex>"), refuses loudly WITHOUT writes when the birth record
   (meta/created) is missing, restores the seq counter past every persisted
   event, and reads meta/depth + meta/parent back into the frame. It writes
   NOTHING (no meta, no events — restart changes no durable state) and does
   NOT start the turn loop (loop.h's frame_run_loop does).
   `cfg` is the post-restart model config (copied in; NULL = none carried).
   The goal is not separately persisted and comes back NULL; the lineage
   parent is restored only as the parent PATH (meta/parent) — the live
   parent frame_t is a separate process object, so a resumed child reports
   loud refusal instead of a phantom link until the tree slice re-links it.
   Status is whatever the store holds (a resumed "done" frame stays done). */
frame_t* frame_resume(wave_database_root_t* db, const char* sid,
                      const frame_config_t* cfg);
const char* frame_sid(const frame_t* f);            /* full subtree path, e.g. sessions/<sid> */
uint8_t frame_is_done(const frame_t* f);
void frame_destroy(frame_t* f);

/* Model backend for frame_run_loop: INJECTED, BORROWED — the frame stores
   the pointer but does not own or destroy it (scripted test backends are
   stack objects). When set, the loop uses it exclusively and never builds
   the default. When NOT set, the first use constructs the default
   model_http_backend_create (from the frame's config) ONCE, and THAT
   instance is frame-owned (freed in frame_destroy). */
void frame_set_model_backend(frame_t* f, model_backend_t* backend);

/* Total model turns frame_run_loop may issue before failing loud with a
   control "turn-limit" event. 0 = the loop's built-in default (64). The cap
   is runtime state (not a compile-time constant) so a test can bound the
   always-tool-calling model to a handful of turns. */
void frame_set_loop_turn_cap(frame_t* f, unsigned cap);

/* Store operations (called by frame behaviors; ONE root batch per effect). */
int frame_remember_local(frame_t* f, const char* key, const char* json_value);   /* state/local/<key> */
int frame_remember_ctx(frame_t* f, const char* key, const char* json_value);     /* state/ctx/<key> */
/* Resolve: own local/ first, then ctx/ up the lineage chain (shadowing). */
char* frame_recall(frame_t* f, const char* key);        /* malloc'd JSON text; NULL if unresolvable */
int frame_append_msg(frame_t* f, const char* role, const char* content);   /* msg.append event */

/* Admission-only spawn (PA semantics): validates depth, creates the child
   subtree, ONE root batch: child's birth batch (meta + status) + parent's
   frame.spawn event + lineage triple ops. Returns the child immediately;
   fails (NULL + log_error) when depth is exceeded — never substitutes.
   context_json (nullable) is stored as the child's state/ctx/handoff key. */
frame_t* frame_spawn(frame_t* parent, const char* goal, const char* context_json);
/* Child-side: frame.report event in the child + one event bound into the
   parent's log; marks the child done. */
int frame_report(frame_t* child, const char* text);
int frame_join(frame_t* child);   /* frame.join event in the parent; child becomes joined */
/* test/debug accessor: malloc'd JSON array of the frame's raw event records
   (bounded to last 512, root-level absolute-bounds scan) */
char* frame_debug_events(frame_t* f);

/* Test/synchronous-entry point: run ONE behavior dispatch on `msg` exactly as
   the scheduler would run it. In production the embedded actor's mailbox
   routes through this same dispatch; tests call it directly to exercise a
   behavior in-line. The behavior CONSUMES the message's payload (msg->payload
   is NULL on return when the type was handled); never blocks beyond a µs
   batch. */
void frame_dispatch(frame_t* f, message_t* msg);

#endif /* SA_HAS_WDB */

#endif // SA_FRAME_H