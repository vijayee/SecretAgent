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
  /* The scheduler pool this frame's embedded actor attaches to. BORROWED —
     never owned/freed by the frame. NULL = the inline shape (tests/loop pump
     the mailbox by hand); a spawned child INHERITS the parent's pool, so a
     tree always sits on one pool. The engine (frame_start) schedules onto
     it; no pool means the driver pumps. */
  scheduler_pool_t* pool;
} frame_config_t;

/* Store-actor configuration (the root's OWN pool): NULL = the inline shape
   (tests/demos pump the store actor by hand via wave_db_pump; wave_db_open
   below is this with NULL). A POOLED FRAME REQUIRES a POOLED STORE —
   frame_create/frame_resume refuse loud otherwise (a pooled frame posting
   into an un-pumped inline store mailbox would hang, not fail). */
typedef struct wave_database_config_t {
  const char* location;          /* NULL = in-memory */
  scheduler_pool_t* store_pool;  /* BORROWED; never owned/freed by the root */
} wave_database_config_t;

/* Open (or boot-restore onto) a root database with the store actor attached.
   SAME database lifecycle as wave_db_open (database_create_with_config,
   sync_only=0, fail-loud on failure) — the store actor just OWNS the
   single-serializer role now. */
wave_database_root_t* wave_db_open_config(const wave_database_config_t* cfg);

/* Open (or boot-restore onto) a root database: the inline-store shape of
   wave_db_open_config (a NULL store pool — tests/demos pump via wave_db_pump). */
wave_database_root_t* wave_db_open(const char* location /* NULL = in-memory */);
void wave_db_close(wave_database_root_t* db);

/* Test/dual-driver accessors:
   - the store actor embedded in the root (actor_t-first state; BORROWED);
   - the inline pump: runs ONE actor_run batch over the store actor's
     mailbox; returns 0 after pumping. On a POOLED store it refuses loud
     (workers own pacing) and returns nonzero. */
actor_t* wave_db_store_actor(wave_database_root_t* db);
int wave_db_pump(wave_database_root_t* db);

typedef struct frame_t frame_t;

typedef struct model_backend_t model_backend_t;   /* defined by model.h (Task 9) */

/* parent NULL = top-level session (sid generated); goal may be NULL. */
frame_t* frame_create(wave_database_root_t* db, frame_t* parent,
                      const char* goal, const frame_config_t* cfg);
/* Boot-time restore of a frame whose subtree already exists on disk (the
   restart path): opens the subtree at `sid` (the full path, e.g.
   "sessions/<hex>"), refuses loudly WITHOUT writes when the birth record
   (meta/created) is missing, restores the seq counter past every persisted
   event, and reads meta/depth + meta/parent back into the frame. The ONE
   durable write a resume may make is the CRASH-REPAIR PASS on a NOT-done
   frame (the turn-lifecycle slice): an open tail is closed by ONE atomic
   batch of synthesized lifecycle closer records (turn-lifecycle spec §4) —
   balanced/pre-lifecycle tails compose nothing and stay untouched, and a
   DONE subtree skips the pass entirely (the read-only handle shape). It does
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

/* Begin (or restart after an ended run) the event-driven turn engine: ONE
   turn-step continuation is queued on the frame's actor; from there the
   actor yields to its scheduler pool between turn phases and re-runs on
   every arrival (model completion, cell result, child report). Returns 0,
   or nonzero with a loud log_error when the frame is dead or an engine is
   already live on it (one engine per frame). Starting an ALREADY-DONE frame
   also returns 0: the done check lives in the first turn dispatch, which
   then terminates the engine cleanly (a CHILD posts its parent's resume; a
   top frame just ends — an ended frame re-runs nothing). */
int frame_start(frame_t* f);

/* Test/debug + embedding accessor: the pool the frame's actor is attached
   to (NULL = inline). */
scheduler_pool_t* frame_pool(const frame_t* f);

/* Store operations (called by frame behaviors; ONE root batch per effect). */
int frame_remember_local(frame_t* f, const char* key, const char* json_value);   /* state/local/<key> */
int frame_remember_ctx(frame_t* f, const char* key, const char* json_value);     /* state/ctx/<key> */
/* Resolve: own local/ first, then ctx/ up the lineage chain (shadowing). */
char* frame_recall(frame_t* f, const char* key);        /* malloc'd JSON text; NULL if unresolvable */
int frame_append_msg(frame_t* f, const char* role, const char* content);   /* msg.append event */

/* Admission-only spawn (PA semantics): validates depth, creates the child
   subtree, ONE root batch: child's birth batch (meta + status) + parent's
   frame.spawn event + lineage triple ops. Returns the child (only after the
   admission COMMITS — the store reply confirms it); fails (NULL + log_error)
   when depth is exceeded — never substitutes. context_json (nullable) is
   stored as the child's state/ctx/handoff key.
   SPAWN = ADMIT + START (the orchestration slice): the child INHERITS the
   parent's engine knobs (loop_turn_cap) and the parent's BORROWED backend
   override (frame_set_model_backend on the parent; production leaves it NULL
   for everyone — the child's default backend builds from its own config),
   and when a turn engine is LIVE on the parent the admission commit's reply
   STARTS the child's own engine (spawn = admit + start; the child rides the
   parent's pool) and counts it in the parent's live children — the parent
   yields at FRAME_PHASE_CHILDREN and resumes on the child reports. An
   engine-less caller gets the admission-only shape unchanged and drives the
   child itself (frame_start / frame_run_loop's start-or-pump); the caller
   ADOPTS the returned record either way (its teardown is the caller's). */
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