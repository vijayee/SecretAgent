# Frame Orchestration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Frame orchestration: spawned frames actually run, fully event-driven — the turn loop dissolves into scheduled turn-step continuations (derive → model submit → yield; model/cell/child-arrival → next step); spawn = admit + start; parents resume from child events and never block on anything; child failure fails loud into the parent's log. WaveDB is wrapped in a store actor, so all frame-layer store writes and reads are serialized WITHOUT a single lock.

**Architecture:** `src/Frame/frame.{h,c}` gains the pool-carried frame (`frame_config_t.pool`, inherited down the lineage), `frame_start`, the **store actor** (`wave_database_root_t` becomes an actor state whose behavior owns the WaveDB root: its mailbox executes batches/scans/recall-walks one at a time — owner amendment 2026-09-30, *"wrap wavedb in an actor therefore serializing writes and reads and eliminating the need for locks"*; NO platform mutex/rwlock/barrier is introduced anywhere in the frame layer), and three new mailbox behaviors' routing (`FRM_TURN` / `FRM_MODEL_RESULT` / `FRM_CHILD_REPORT`) plus the store vocabulary (`FRM_STORE_BATCH` / `FRM_STORE_SCAN` / `FRM_STORE_RECALL` / `FRM_STORE_REPLY` / `FRM_REPORT_BIND`). `src/Frame/loop.{h,c}` is rewritten from a held-thread `for(;;)` into the phase machine the same handlers run — the derive is a store round trip (`FRAME_PHASE_STORE`), the synchronous `frame_run_loop` stays as a test/demo driver over the SAME engine. `src/Frame/model.{h,c}` gains the optional async `submit` vtable member (the http backend's model call rides `http_client_submit` + a relay to the engine's sink). `src/Python/py_agent.c` untouched (its bounded bridge wait becomes the from-cells remember/recall round trip's wait; the store actor is the reply source). Atlas closes `remember-session-across-restarts` evidence-only and folds `subdivide-session-into-accountable-agents`' verification into this plan's acceptance criteria.

**Spec:** `docs/superpowers/specs/2026-09-30-frame-orchestration-design.md` (§5 = the store actor; supersedes the originally planned per-frame write lock).

**References:** `src/Frame/frame.c` (today's store sites to recut, read before Task 2: `database_subtree_batch_sync_raw` at lines 393/482/977/1308, `database_batch_sync_raw` at 578/1722/1827/1892, `database_scan_start_reverse` at 933/1918, `wave_db_open` at 1088+, `frame_recall` at 1471+, `frame_spawn` at 1542+, `frame_report` at 1738+, `frame_join` at 1846+, `_frame_cell_wait`'s pump at 1048+; the py-agent bridge wait idiom to mirror: `src/Python/py_agent.c` `_py_agent_request` — a waiter registered before the post, a bounded condvar wait, an unlink under the reply lock), `src/Frame/model.c` (the completion record + process loop thread the submit path reuses), `src/Scheduler/scheduler.h` (pool create/start/stop/destroy/wait_for_idle, `scheduler_inject`), `src/Actor/actor.c` (`actor_init`'s pool registration, `actor_send`'s universal queue+inject path, `actor_destroy`'s pooled teardown waits), `docs/STYLE_GUIDE.md`.

**Frozen boundaries (hard rules):** never modify `liboffs/`, `WaveDB/`, `deepseek-harness/`, `prime-agent/`, `onyx/`, `claude-code-source-code/`, or anything under `deps/`. `src/Actor`, `src/Scheduler`, `src/RefCounter`, `src/Util`, `src/Streams`, `src/Buffer`, `src/Python` are frozen EXCEPT one line of `pyrt` glue if FRM_SPAWN's reply shape demands it (expected: none). All ctest runs: `setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3` (ASLR quirk). Valgrind needs `--strip-debug` copies AND verification the binary actually ran (`grep -c "OK ]"`); a "0 leaks" from an unstripped run is empty evidence. Conventional commits, no Co-Authored-By, never leave TODOs, atomics fine. Escalate on ambiguity you cannot resolve from the spec rather than inventing behavior.

## File structure

```
src/Frame/frame.h             (Tasks 1+2: frame_config_t.pool, frame_start, wave_database_config_t +
                              wave_db_open_config, the store-actor accessors)
src/Frame/frame_messages.h    (Tasks 1+2: FRM_TURN/FRM_MODEL_RESULT/FRM_CHILD_REPORT +
                              FRM_STORE_BATCH/FRM_STORE_SCAN/FRM_STORE_RECALL/FRM_STORE_REPLY/
                              FRM_REPORT_BIND + their payloads and destroyers)
src/Frame/frame_internal.h    (Tasks 1+3: the engine contract — phase enum incl. FRAME_PHASE_STORE,
                              store kinds, handlers, model/store wait deadlines, _frame_pump)
src/Frame/frame.c             (Tasks 1+2+3+5: pool plumbing, the store actor (_store_behavior, the
                              reply routers, seq pre-allocation), engine fields, FRM_SPAWN keeps
                              the child, frame_destroy's pooled branch, dispatch cases)
src/Frame/loop.h              (Task 3: frame_run_loop's 0/1/2 contract)
src/Frame/loop.c              (Task 3+5+6: the turn engine — FRM_TURN step, store round trips,
                              reply paths, PHASE_CHILDREN, terminate)
src/Frame/model.h             (Task 4: model_backend_t.submit + model_response_sink_fn)
src/Frame/model.c             (Task 4: _model_http_submit, the relay, deferred client destroy, _model_result_from_http)
src/Frame/model_internal.h    (Task 4: the loop↔engine decode boundary)
tools/frame-demo/main.c       (Task 1 Step 1: explicit zero-init of its config — additive-field safety;
                              Task 2: the demo's db rides wave_db_open_config with an inline store)
test/test_frame.cpp           (Tasks 1+2+5+6: pool plumbing, TestStore round trips, one-batch cross-
                              subtree binds, pooled tree orchestration, failure accounting)
test/test_loop.cpp            (Task 1 Step 1's zero-init; Task 3: async scripted backend + engine compat;
                              Task 6: child-failure tests)
test/test_model_decode.cpp    (Task 4: submit-path round-trip + transport-failure tests)
test/test_live_loop.cpp       (Task 7: the live gate rides the pump + wave_db_open_config if it grows a pool)
atlas/workflow.json           (Task 7: evidence + the evidence-only close)
```

Verification idioms used throughout:

```bash
# build
cmake --build cmake-build-debug -j
# suite (everything hereafter assumes this instead of bare ctest)
setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
# valgrind on a filter (stripped copy; VERIFY it ran: grep -c "OK ]" on output)
cp cmake-build-debug/test/testsecretagent /tmp/vg && strip --strip-debug /tmp/vg
setarch -R valgrind --leak-check=full --suppressions=test/vg-cpython.supp \
  --suppressions=test/vg-wavedb.supp /tmp/vg --gtest_filter='<filter>'
# ASan suite
setarch -R ctest --test-dir cmake-build-asan --output-on-failure 2>/dev/null | tail -3
# the no-lock rule (Task 2's standing check — run after EVERY store-touching edit)
{ grep -rn "platform_mutex\|platform_rwlock\|platform_barrier\|platform_sem" src/Frame/ \
  && echo "VIOLATION: a lock in the frame layer"; } || echo "no locks in the frame layer"
```

---

### Task 1: Pool-carried frames + the FRM_TURN vocabulary + `frame_start` — UNCHANGED

Owner's amendment check: **Task 1 is unchanged.** The store actor does not touch it —
the pool field, the FRM_TURN message, `frame_start`, and the config-site hardening are
exactly as committed below (its tests open in-memory dbs via `wave_db_open(NULL)`, which
Task 2 keeps as the inline-store wrapper of `wave_db_open_config`; every Task-1 call site
keeps compiling and passing verbatim).

**Files:**
- Modify: `src/Frame/frame.h`, `src/Frame/frame_messages.h`, `src/Frame/frame_internal.h`, `src/Frame/frame.c`, `test/test_frame.cpp`, `tools/frame-demo/main.c`

- [ ] **Step 1: Harden every frame_config_t constructor site to explicit zero-init FIRST** (the new field must default to NULL, and two existing sites construct by bare declaration / positional init)

`test/test_frame.cpp` + `test/test_loop.cpp`'s `test_config` helpers and the demo's config (tools/frame-demo/main.c:204) become explicit:

```cpp
static frame_config_t test_config(void) {
  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));   /* additive fields (pool, timeout) default sensibly */
  cfg.model_base_url = NULL;
  cfg.model_api_key = NULL;
  cfg.model_name = "unused";
  cfg.max_depth = 4;
  return cfg;
}
```

- [ ] **Step 2: Write the frozen contracts**

`src/Frame/frame.h` additions (inside the existing SA_HAS_WDB gate; `scheduler_pool_t` is already forward-declared by Actor/actor.h — no new include edge):

```c
/* Config gains ONE field (append at the END — positional initializers keep
 compiling with a zero default): the scheduler pool this frame's embedded
 actor attaches to. BORROWED — never owned/freed by the frame. NULL = the
 inline shape (tests/loop pump the mailbox by hand); a spawned child
 INHERITS the parent's pool, so a tree always sits on one pool. The engine
 (frame_start) schedules onto it; no pool means the driver pumps. */
  scheduler_pool_t* pool;       /* in frame_config_t, after model_timeout_ms */

/* Begin (or restart after an ended run) the event-driven turn engine: ONE
   turn-step continuation is queued on the frame's actor; from there the actor
   yields to its scheduler pool between turn phases and re-runs on every
   arrival (model completion, cell result, child report). Returns 0, or
   nonzero with a loud log_error when the frame is dead or an engine is
   already live on it (one engine per frame). */
int frame_start(frame_t* f);

/* Test/debug + embedding accessor: the pool the frame's actor is attached
   to (NULL = inline). */
scheduler_pool_t* frame_pool(const frame_t* f);
```

`src/Frame/frame_messages.h` additions (the FRM_* vocabulary lives here; payload structs + destroyers declared here, destroyers defined in frame.c):

```c
  FRM_TURN,          /* engine -> itself: the scheduled turn-step continuation
                        (payload NULL) — the loop's turn loop, dissolved */
  FRM_MODEL_RESULT,  /* transport -> frame: the model completion arrived
                        (frm_model_payload_t; see model.c's relay) */
  FRM_CHILD_REPORT   /* child -> parent: this child's engine is terminal
                        (frm_child_report_payload_t; the parent re-schedules) */

/* model completion (transport -> frame): the http body and error move in RAW
   (steal-slot, exactly model.c's completion record shape); the
   FRM_MODEL_RESULT behavior decodes via model_internal.h on the frame's own
   thread — the streams completion stays µs-scale. Ownership of body/error
   transfers with the message. */
typedef struct frm_model_payload_t {
  int status;       /* http status, or -1 on transport failure */
  char* body;       /* heap; steal-slot */
  size_t body_len;
  char* error;      /* heap transport reason on status -1 */
} frm_model_payload_t;
/* child terminal: bookkeeping only — the parent-side report binding ALREADY
   happened in the child's report batch (under the parent's write lock);
   this message RESUMES the parent's live engine. failed = the child ended on
   a control event (the parent's thread logs the resume loudly for it). */
typedef struct frm_child_report_payload_t { char* child_sid; uint8_t failed; } frm_child_report_payload_t;
void frm_model_payload_destroy(void* p);
void frm_child_report_payload_destroy(void* p);
```

(Task 2 revises the `frm_child_report_payload_t` comment above — "under the parent's
write lock" becomes the store-actor commit wording — but NO Task-1 code changes with it.)

- [ ] **Step 3: Write the failing tests (`test/test_frame.cpp` additions)**

```cpp
TEST(TestFrame, TestPoolAttachAndInheritance) {
  frame_config_t cfg = test_config();   /* zero-init'd helper from Step 1 */
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  cfg.pool = pool;
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* parent = frame_create(db, NULL, "tree root", &cfg);
  ASSERT_NE(parent, nullptr);
  EXPECT_EQ(frame_pool(parent), pool) << "frame_create attaches the config's pool";

  frame_t* child = frame_spawn(parent, "leaf", NULL);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(frame_pool(child), pool) << "spawned children inherit the parent's pool";

  /* An inline default is unchanged: a pool-less config means pool NULL. */
  frame_config_t plain = test_config();
  frame_t* inline_frame = frame_create(db, NULL, NULL, &plain);
  ASSERT_NE(inline_frame, nullptr);
  EXPECT_EQ(frame_pool(inline_frame), nullptr);

  frame_destroy(child);
  frame_destroy(inline_frame);
  frame_destroy(parent);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  wave_db_close(db);
}

TEST(TestFrame, TestFrameStartQueuesOneTurnContinuation) {
  frame_config_t plain = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* f = frame_create(db, NULL, NULL, &plain);
  ASSERT_NE(f, nullptr);

  EXPECT_EQ(frame_start(f), 0);
  EXPECT_NE(frame_start(f), 0) << "one live engine per frame — restart refused loudly";

  /* On an inline frame the continuation sits in the mailbox (the owner pumps):
     exactly what Task 3's FRM_TURN case will consume. Drain it by hand — the
     Task-1 handler is a loud late-drop, so after the drain the engine is
     still startable (task 3's engine changes that). */
  actor_run(&f->actor, ACTOR_BATCH_SIZE);
  EXPECT_EQ(frame_start(f), 0) << "the single continuation was consumed; engine restartable";

  frame_destroy(f);
  wave_db_close(db);
}
```

- [ ] **Step 4: Red → implement → green**

Implementation shape (frame.c):
- `frame_t` gains `scheduler_pool_t* pool;` (borrows the config's at alloc; inherited
  into children in `_frame_alloc`'s parent-branch, exactly like the model config).
- `_frame_alloc` and `frame_resume` call `actor_init(&f->actor, f, _frame_behavior, f->pool)`.
- `frame_start`: refuse on `_frame_is_live(f) == 0` or `engine_live != 0` (log_error,
  return -1); else set `engine_live = 1` (the remaining engine knobs arrive with
  Task 3's frame_t additions — this task carries only `engine_live`) and build
  `message_t{type = FRM_TURN, payload = NULL}` and `actor_send(&f->actor, &m)`.
- The FRM_TURN dispatch case (temporary, replaced by Task 3's engine): log_error the
  late-drop and return — no payload to destroy.
- `frame_pool(f)`: return `f->pool`.
- `frame_destroy`: pooled branch per the style guide's teardown family — the frame's
  actor teardown routes through `actor_destroy(&f->actor)` (its RUNNING/queue-state waits
  break out once `scheduler_pool_stop` set `stopped`) instead of the inline detach path.

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
```
Expected: the two new tests green; every existing test unchanged and green.

- [ ] **Step 5: Valgrind on TestFrame.* (stripped copy, verified-executing) — 0 leaks**
- [ ] **Step 6: Commit**

```bash
git add src/Frame/frame.h src/Frame/frame.c test/test_frame.cpp tools/frame-demo/main.c test/test_loop.cpp
git commit -m "feat: pool-carried frame actors with frame_start engine vocabulary"
```

---

### Task 2: The store actor (WaveDB behind one mailbox — no locks)

`wave_database_root_t` becomes an actor state: its behavior (`_store_behavior`) executes
every frame-layer store operation — atomic batches, the bounded events scan, the recall
resolve walk — ONE message at a time. Serialization comes from being one actor; the
per-frame write lock the spec originally proposed (§5, superseded) is GONE — **no
platform mutex/rwlock/barrier/semaphore may appear anywhere under `src/Frame/`** (the
standing grep above proves it).

Read first (in `src/Frame/frame.c`): every `database_subtree_batch_sync_raw` /
`database_batch_sync_raw` / `database_subtree_get_sync_raw` /
`database_scan_start_reverse` call site (lines 249, 393, 482, 578, 933, 977, 1308, 1426,
1500, 1722, 1827, 1892, 1918 — the grep in References lists them), `wave_db_open`
(1088+), `_frame_cell_wait`'s pump (1048+), and `src/Python/py_agent.c`'s
`_py_agent_request` (the bounded-bridge-wait idiom the from-cells verbs keep).

**Files:**
- Modify: `src/Frame/frame.h` (`wave_database_config_t` + `wave_db_open_config`, the store-actor accessors), `src/Frame/frame_messages.h` (the FRM_STORE_* / FRM_REPORT_BIND vocabulary), `src/Frame/frame_internal.h` (`_frame_pump`), `src/Frame/frame.c` (the root struct, `_store_behavior`, the reply routers, seq pre-allocation, the public-API pump-waits, teardown), `tools/frame-demo/main.c` (its db line rides `wave_db_open_config`)
- Test: `test/test_frame.cpp`

- [ ] **Step 1: Write the frozen contracts**

`src/Frame/frame.h` additions (after Task 1's block; `Scheduler/scheduler.h` needs NO
include — `scheduler_pool_t` is forward-declared in Actor/actor.h, which frame.h already
includes):

```c
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

/* Test/dual-driver accessors:
   - the store actor embedded in the root (actor_t-first state; BORROWED);
   - the inline pump: runs ONE actor_run batch over the store actor's
     mailbox; returns 0 after pumping. On a POOLED store it refuses loud
     (workers own pacing) and returns nonzero. */
actor_t* wave_db_store_actor(wave_database_root_t* db);
int wave_db_pump(wave_database_root_t* db);
```

`src/Frame/frame_internal.h` addition (the ONE definition of the pump order — used by
`_frame_cell_wait`, the sync-API pump-waits of this task, and Task 3's driver pump):

```c
/* ONE pump cycle over an inline frame's whole round-trip surface: the frame's
   mailbox, then each LIVE ANCESTOR's mailbox (child first — a bind request
   must reach the parent's actor and its composition), then — when the ROOT's
   store actor is inline — the store actor's mailbox. This is what makes every
   inline round trip (bridge verb -> store batch -> store reply -> answer)
   complete within one pump cycle, and it is the ONLY pump-order definition:
   _frame_cell_wait, the sync store API and the Task-3 driver pump all lean on
   it. Pooled surfaces (a pooled frame or a pooled store) are NEVER pumped
   here — scheduler workers own them; an inline frame's pooled ancestors (only
   possible via a mismatched frame_create cfg — the Task-2 guard refuses it)
   are skipped by the live-ancestor walk. */
void _frame_pump(frame_t* f);
```

`src/Frame/frame_messages.h` additions (the store vocabulary; payloads + destroyers
declared here, destroyers defined in frame.c):

```c
  FRM_STORE_BATCH,   /* any frame route -> store actor: one atomic op list
                        (frm_store_batch_payload_t) */
  FRM_STORE_SCAN,    /* -> store actor: bounded reverse range read
                        (frm_store_scan_payload_t) — the derive's store trip */
  FRM_STORE_RECALL,  /* -> store actor: the lineage resolve walk
                        (frm_store_recall_payload_t) */
  FRM_STORE_REPLY,   /* store actor -> requester: corr-matched result
                        (frm_store_reply_payload_t) */
  FRM_REPORT_BIND    /* child frame actor -> parent frame actor: compose the
                        cross-subtree report batch THERE (the parent's actor
                        pre-allocates the parent's seq; frm_report_bind_payload_t) */

/* One put op (all frame-layer writes are puts). Ownership of key and value
   transfers with the payload; the store behavior frees them after it acts. */
typedef struct frm_store_op_t { char* key; uint8_t* value; size_t value_len; } frm_store_op_t;

/* A batch = ONE atomic root database_batch_sync_raw across every touched
   subtree (the composed full-root-path keys — the existing spawn/report/shape,
   unchanged). op_name is a BORROWED label the store worker logs on refusal.
   reply_to NULL = fire-and-post (control/audit writes nothing waits on). */
typedef struct frm_store_batch_payload_t {
  frm_store_op_t* ops; size_t nops;
  const char* op_name;
  actor_t* reply_to;    /* BORROWED; NULL = fire-and-post */
  uint64_t corr;        /* the requester's round-trip key (0 = fire-and-post) */
} frm_store_batch_payload_t;

typedef struct frm_store_scan_payload_t {
  char* start; char* end;   /* OWNED absolute root-level bounds ("sessions/<sid>/events",
                               "sessions/<sid>/events0" — the _frame_events_bounds shape) */
  size_t limit;             /* newest-record cap (SA_FRAME_DEBUG_MAX_EVENTS) */
  actor_t* reply_to;        /* BORROWED; never NULL */
  uint64_t corr;
} frm_store_scan_payload_t;

/* The recall resolve walk (own local/ -> own ctx/ -> meta/parent hops' ctx/),
   relocated INTO the store actor: it is a pure store computation over the
   walk's subtree reads, and this keeps the frame's behavior lock-free. */
typedef struct frm_store_recall_payload_t {
  char* key;          /* OWNED; non-empty, free of '/' */
  char* sid_path;     /* OWNED; where the walk starts */
  unsigned max_hops;  /* the frame's depth budget (max_depth) */
  actor_t* reply_to;  /* BORROWED */
  uint64_t corr;
} frm_store_recall_payload_t;

/* The round-trip result. rc = 0 committed / the store's refusal code.
   `records` = the MATERIALIZED RAW record texts (heap, ascending seq order,
   OWNED) for scans; the recall's resolution is records[0] or rc != 0; batch
   replies carry n == 0 / records == NULL. PAYLOAD DECISION (owner asked to
   settle it from the code): the store worker does NOT parse JSON — raw texts
   are honest (the store deals in store values) and the cheap side of the
   trade (no serialize-then-reparse round trip); the requester's router parses
   each record (µs, bounded 512) where the projection already consumes a DOM.
   Unparseable raw records are dropped LOUD by the router. */
typedef struct frm_store_reply_payload_t {
  uint64_t corr; int rc; size_t n; char** records;
} frm_store_reply_payload_t;

/* child -> parent: the cross-subtree report binding composed at the parent's
   actor (ONE batch: the child's record at the child's PRE-ALLOCATED seq —
   already composed and carried here — + the child's meta/status=done + the
   parent's bound report event at the parent's own pre-allocated seq).
   engine_driven (the TERMINATE path) is what makes the child's router post
   FRM_CHILD_REPORT on the reply; a cell-verb report or a direct sync-API
   report only binds (the report-verb engine end posts its resume via its own
   terminate at the next is_done turn). */
typedef struct frm_report_bind_payload_t {
  actor_t* reply_to;        /* BORROWED: the CHILD's actor receives the store reply */
  uint64_t corr;            /* the child's own store round-trip key */
  uint64_t bridge_corr;     /* the cell's bridge corr, or 0 when not cell-side */
  uint8_t engine_driven;    /* 1 = post FRM_CHILD_REPORT on the store reply */
  char* child_sid;          /* OWNED */
  uint64_t child_seq;       /* the child's pre-allocated seq */
  char* child_event_text;   /* OWNED; the child's own frame.report record JSON */
  char* text;               /* OWNED; the report text (the parent re-composes its bound event) */
} frm_report_bind_payload_t;

void frm_store_batch_payload_destroy(void* p);
void frm_store_scan_payload_destroy(void* p);
void frm_store_recall_payload_destroy(void* p);
void frm_store_reply_payload_destroy(void* p);
void frm_report_bind_payload_destroy(void* p);
```

- [ ] **Step 2: Write the failing tests (`test/test_frame.cpp` additions + the existing bridge-verbatim tests recut to the two-pump shape)**

```cpp
TEST(TestStore, TestStoreActorBatchIsAtomicAndAnswered) {
  /* The store actor inline: a batch posted to ITS mailbox is executed as ONE
     root transaction, and its corr-matched reply lands in the requester's
     mailbox. The test plays the requester by posting a batch whose reply_to
     is a frame actor it drains by hand. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  actor_t* store = wave_db_store_actor(db);
  ASSERT_NE(store, nullptr);

  frame_config_t cfg = test_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);   /* inline frame = the reply target */
  ASSERT_NE(f, nullptr);

  frm_store_batch_payload_t* bp = get_clear_memory(sizeof(*bp));
  bp->ops = get_clear_memory(2 * sizeof(frm_store_op_t));
  bp->nops = 2;
  bp->ops[0].key = strdup("sessions/_probe/state/local/k1");
  bp->ops[0].value = (uint8_t*)strdup("\"one\"");
  bp->ops[0].value_len = strlen("\"one\"");
  bp->ops[1].key = strdup("sessions/_probe/state/ctx/k2");
  bp->ops[1].value = (uint8_t*)strdup("\"two\"");
  bp->ops[1].value_len = strlen("\"two\"");
  bp->op_name = "store-test";
  bp->reply_to = &f->actor;
  bp->corr = 4242;
  message_t m;
  m.type = (uint32_t)FRM_STORE_BATCH;
  m.payload = bp;
  m.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(store, &m));

  /* Nothing committed until the store actor runs; nothing answered until the
     frame's own pump routes the reply: */
  EXPECT_EQ(frame_recall(f, "k1"), nullptr) << "the store actor has not run yet";
  wave_db_pump(db);                        /* the store actor runs the batch */
  actor_run(&f->actor, ACTOR_BATCH_SIZE);  /* the reply dispatches on the frame actor */

  /* The batch committed atomically (both keys or neither), and the frame's
     OWN log is untouched by the unrelated store batch. */
  char* v = frame_recall(f, "k1");
  ASSERT_NE(v, nullptr);
  EXPECT_STREQ(v, "\"one\"");
  free(v);
  v = frame_recall(f, "k2");
  ASSERT_NE(v, nullptr);
  EXPECT_STREQ(v, "\"two\"");
  free(v);
  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  EXPECT_EQ(json_size(events), 0u);
  json_value_destroy(events);

  /* A store-level rejection: an oversized op posted straight to the store
     actor is refused loud and commits NOTHING (fail-loud stays the store's
     contract; the composers' cap checks are mirrored here). */
  frm_store_batch_payload_t* bad = get_clear_memory(sizeof(*bad));
  bad->ops = get_clear_memory(sizeof(frm_store_op_t));
  bad->nops = 1;
  bad->ops[0].key = strdup("sessions/_probe/state/local/big");
  std::string blob(200 * 1024, 'x');       /* > SA_FRAME_MAX_BATCH_BYTES */
  bad->ops[0].value = (uint8_t*)strdup(blob.c_str());
  bad->ops[0].value_len = blob.size();
  bad->op_name = "store-test-oversized";
  bad->reply_to = &f->actor;
  bad->corr = 4243;
  message_t bad_msg;
  bad_msg.type = (uint32_t)FRM_STORE_BATCH;
  bad_msg.payload = bad;
  bad_msg.payload_destroy = frm_store_batch_payload_destroy;
  ASSERT_TRUE(actor_send(store, &bad_msg));
  wave_db_pump(db);
  actor_run(&f->actor, ACTOR_BATCH_SIZE);   /* the refusal reply routes (dropped loud
                                               — an unmatched corr — but observable via
                                               the recall below) */
  EXPECT_EQ(frame_recall(f, "big"), nullptr) << "the oversized batch committed nothing";

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestStore, TestPooledStorePumpRefusesLoud) {
  /* The dual-driver rule's other half: a POOLED store's pacing belongs to its
     workers; wave_db_pump must refuse loud, not steal a mailbox run. */
  frame_config_t cfg = test_config();
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  cfg.pool = pool;
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  EXPECT_NE(wave_db_store_actor(db), nullptr);
  EXPECT_NE(wave_db_pump(db), 0) << "pooled store: the pump refuses loud, it never pumps";
  wave_db_close(db);            /* documented order: stop, close, destroy */
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
}
```

(Note: `frame_recall` above is the DIRECT sync API on an INLINE frame + INLINE store —
it composes, posts, and pump-waits internally, so these assertions exercise the same
round trip from the caller's side. `load_events` is test_frame.cpp's existing
`frame_debug_events` wrapper. The Task-2 no-lock grep runs with this test.)

```cpp
TEST(TestStore, TestStoreRecallWalkRidesTheStoreActor) {
  /* The resolve walk (own local/ -> own ctx/ -> ancestor ctx/) is now a
     store MESSAGE, not a synchronous frame-side read: a cell's actor.recall
     is answered from the FRM_STORE_REPLY router. Uses the test_frame
     bridge harness (bridge_completion_mount). */
  wave_database_root_t* db = wave_db_open(NULL);
  frame_config_t cfg = test_config();
  frame_t* parent = frame_create(db, NULL, NULL, &cfg);
  frame_t* child = frame_spawn(parent, NULL, NULL);
  frame_remember_ctx(parent, "inherited", "\"hello\"");   /* ancestor state */
  frame_remember_local(child, "local", "\"mine\"");

  bridge_completion_t completion;
  bridge_completion_mount(&completion);

  /* The cell-verb shape: FRM_RECALL dispatched at the child is now
     compose-and-post; the bridge corr answer waits for the store reply. */
  message_t req = bridge_request(FRM_RECALL, 91, "inherited", NULL);
  frame_dispatch(child, &req);      /* posts the store recall; no bridge reply YET */
  EXPECT_EQ(completion.corrs.size(), 0u)
      << "no synchronous bridge answer anymore — the store actor is the reply source";

  wave_db_pump(db);                 /* the store actor runs the walk */
  actor_run(&child->actor, ACTOR_BATCH_SIZE);   /* the router answers the corr */
  ASSERT_EQ(completion.corrs.size(), 1u);
  EXPECT_EQ(completion.corrs[0], 91u);
  EXPECT_EQ(completion.statuses[0], 0u) << "the walk resolved through the store actor";
  EXPECT_EQ(completion.texts[0], "\"hello\"");
  completion.corrs.clear(); completion.statuses.clear(); completion.texts.clear();

  /* local/ shadows ctx/ within one frame (unchanged semantics, same path): */
  message_t req2 = bridge_request(FRM_RECALL, 92, "local", NULL);
  frame_dispatch(child, &req2);
  wave_db_pump(db);
  actor_run(&child->actor, ACTOR_BATCH_SIZE);
  ASSERT_EQ(completion.corrs.size(), 1u);
  EXPECT_EQ(completion.corrs[0], 92u);
  EXPECT_EQ(completion.texts[0], "\"mine\"");

  bridge_completion_mount(NULL);
  frame_destroy(child);
  frame_destroy(parent);
  wave_db_close(db);
}
```

```cpp
TEST(TestFrame, TestReportBindIsOneCrossSubtreeBatch) {
  /* The cross-frame effect is ONE atomic batch, composed at the PARENT's
     actor with the parent's PRE-ALLOCATED seq; the child only ever allocated
     its own. Observable as exact per-log seq chains after the reply. */
  wave_database_root_t* db = wave_db_open(NULL);
  frame_config_t cfg = test_config();
  frame_t* parent = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(parent, nullptr);
  frame_t* child = frame_spawn(parent, "leaf", NULL);   /* spawn = parent seq 1 */
  ASSERT_NE(child, nullptr);

  EXPECT_EQ(frame_report(child, "leaf's verdict"), 0);  /* the bind (sync shape pumps) */

  /* Parent log: frame.spawn @ seq 1, bound frame.report @ seq 2 (the parent
     allocated 2 at ITS actor — the child could not touch it). */
  json_value_t* events = load_events(parent);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(json_size(events), 2u);
  EXPECT_EQ(event_is(json_at(events, 0), "frame.spawn"), true);
  EXPECT_EQ(json_as_int(json_get(json_at(events, 1), "seq")), 2);
  EXPECT_EQ(event_is(json_at(events, 1), "frame.report"), true);
  EXPECT_EQ(json_as_string(json_get(json_get(json_at(events, 1), "payload"), "child_sid")),
            frame_sid(child));
  json_value_destroy(events);

  /* Child log: exactly ONE frame.report @ seq 1, cause = spawn-free (null
     payload shape unchanged), status done. */
  json_value_t* cev = load_events(child);
  ASSERT_NE(cev, nullptr);
  ASSERT_EQ(json_size(cev), 1u);
  EXPECT_EQ(event_is(json_at(cev, 0), "frame.report"), true);
  EXPECT_EQ(json_as_int(json_get(json_at(cev, 0), "seq")), 1);
  json_value_destroy(cev);
  EXPECT_EQ(frame_is_done(child), 1);

  /* Refusal = NOTHING half-applies in EITHER log: an oversized report text is
     refused at compose; both logs keep their exact prior state. */
  std::string big(200 * 1024, 'x');        /* > SA_FRAME_MAX_BATCH_BYTES */
  EXPECT_NE(frame_report(child, big.c_str()), 0) << "refused loud, never truncated";
  json_value_t* p2 = load_events(parent);
  ASSERT_EQ(json_size(p2), 2u) << "the parent's log did not half-apply";
  json_value_destroy(p2);
  json_value_t* c2 = load_events(child);
  ASSERT_EQ(json_size(c2), 1u) << "the child's log did not half-apply";
  json_value_destroy(c2);

  frame_destroy(child);
  frame_destroy(parent);
  wave_db_close(db);
}
```

(Harness note: `load_events`/`event_is` — copy test_loop.cpp's small static helpers
verbatim into test_frame.cpp on first use, and the `bridge_request` /
`bridge_completion_mount` recording sink from its existing bridge block. Note:
`frame_recall` asserts above ride the direct sync API on an INLINE frame + INLINE store —
it composes, posts, and pump-waits internally, so the assertions exercise the store
round trip from the caller's side.)

(Also RECUT the three existing synchronous bridge tests — the comment at test_frame.cpp:188
"frame_dispatch delivers replies to it synchronously on this same thread" no longer holds:
a bridge verb's corr answer is routed by the FRM_STORE_REPLY router, so each
`frame_dispatch(f, &request)` is followed by `wave_db_pump(db)` + one more
`actor_run(&f->actor, ACTOR_BATCH_SIZE)` before the corr assertion. Same assertions,
same corrs, one extra pump line each — the reply RACE the old single dispatch hid is now
visible in the test, which is the point.)

- [ ] **Step 3: Implement (frame.c)**

- **The root struct** gains `actor_t store_actor;` as its FIRST member (house rule) plus
  the borrowed `scheduler_pool_t* store_pool` pointer (for the guard + logs). Build it in
  `wave_database_root_t`'s new construction (below) via `get_clear_memory`, then
  `actor_init(&root->store_actor, root, _store_behavior, cfg->store_pool)`.
- **`wave_db_open_config`**: the EXACT body of today's `wave_db_open` (config default →
  `database_config_set_sync_only(0)` → NULL location disables persist →
  `database_create_with_config` fail-loud → the lineage graph layer, identical comments)
  with the root construction moved under it + the actor_init tail. `wave_db_open(location)`
  becomes the wrapper: construct `{location, NULL}` and delegate.
- **`wave_db_close`**: FIRST `actor_destroy(&root->store_actor)` (inline = running-wait
  no-op; pooled = the stopped/IDLE waits — document the caller order: stop the pool, then
  close the db, then destroy the pool), then today's graph/subtree/db teardown unchanged.
- **`wave_db_store_actor`** returns `&root->store_actor`. **`wave_db_pump`** returns
  nonzero with a loud log when `root->store_pool != NULL` (workers own pacing),
  otherwise `actor_run(&root->store_actor, ACTOR_BATCH_SIZE)` and 0.
- **`_store_behavior(void* state, message_t* msg)`** — the store actor's dispatch:
  - `FRM_STORE_BATCH`: map the payload ops to a `raw_op_t` array (type 0 puts), check the
    per-op + total cap against `SA_FRAME_MAX_BATCH_BYTES` (refused loud with the op_name
    label; the reply still fires with the refusal rc), run ONE
    `database_batch_sync_raw(root->db, '/', ops, nops)`; FREE every op key/value +
    the payload; build `frm_store_reply_payload_t{corr, rc, 0, NULL}` and, when
    `reply_to != NULL`, `actor_send(reply_to, &m)` (an actor_send refusal is logged loud —
    a dying requester loses only a wake-up it could not have used). `reply_to == NULL` →
    fire-and-post: the store worker's own log carries commit/refusal.
  - `FRM_STORE_SCAN`: build the bounds with `path_create_from_raw` on the payload's
    absolute composed keys, `database_scan_start_reverse`, materialize newest-first (the
    `frame_debug_events` materialization loop, MOVED here, WITHOUT its parse/serialize
    tail), REVERSE to ascending, reply `{corr, rc, n, records}`. The store worker never
    parses JSON.
  - `FRM_STORE_RECALL`: today's `frame_recall` walk EXACTLY (own local/ → own ctx/ → the
    meta/parent hops' ctx/, the hop budget), relocated to run against the root with the
    payload's `sid_path`/`max_hops` as the state (it opens/closes ancestor subtrees per hop
    exactly as today; all inside this one dispatch = one serialized read walk). Reply:
    the resolved text as `records[0]` (rc 0) or `rc != 0` / records NULL.
  - `FRM_STORE_REPLY` / unknown: routing bug — loud drop, payload destroyed.
- **The frame's reply router** — `static void _frame_store_reply_route(frame_t* f,
  frm_store_reply_payload_t* r)` (the FRM_STORE_REPLY case of `_frame_behavior`; the
  payload's ownership is consumed by the router on every path):
  1. `spawn_pending` (this task: the DIRECT-API admission — see below) → fill it;
  2. `bind_pending` → the report bind's result (this task: the sync-API report — see
     below; Task 6's terminate reuses the slot);
  3. `f->sync.in_use && corr match` → fill the DIRECT-CALLER slot
     `{done, rc, char* text}` (single slot: direct sync calls are sequential on the pump
     owner's thread in the only shape that supports them — see the inline rule);
  4. `engine store_corr + FRAME_PHASE_STORE` → Task 3 adds this branch (its handler lives
     in loop.c);
  5. the registered cell-verb bridge corrs (`_frame_bridge_pending`, see below) → answer
     through `_frame_bridge_reply(corr, status, text-or-NULL)`;
  6. else → loud drop ("unmatched store reply corr %llu").
- **The cell-verb corr registry** — `_frame_bridge_pending` (a singly-linked
  `frm_bridge_pending_t{uint64_t corr; uint8_t is_recall; next}` list + a `uint64_t corr`
  allocator `f->store_corr_seq++`): every behavior-posted store message registers its corr
  BEFORE the post, from the FRAME's dispatch thread (every mutation of the list is on the
  frame's own dispatch thread — actor single-runner discipline; NO lock). The router's
  step 5 matches a corr, unlinks, answers, frees.
- **Seq pre-allocation** (the lock's replacement): a new local helper
  `_frame_seq_alloc(frame_t* f) → {seq = f->seq + 1; f->seq = seq;}` and its counterpart
  on refusal `_frame_seq_rollback(f, abandoned)` (`if (f->seq == abandoned) f->seq = abandoned - 1;`
  — exact when the frame was single-flight, restoring today's no-gap discipline; otherwise
  a loud gap log). Every write-path compose call sites this instead of the read-then-hope
  `f->seq + 1`.
- **The write paths recut** (each keeps today's validation, caps, and log texts — only
  WHERE the batch executes changes):
  - `_frame_event_write(f, type, payload)` → compose the record (unchanged) +
    `_frame_seq_alloc` + build the one-op `FRM_STORE_BATCH` + post. This task it stays
    SYNC-SEMANTICS for its old-loop callers: a new sync wrapper
    `_frame_event_write_sync` = post with a sync corr + `_frame_sync_wait(f, corr,
    SA_FRAME_STORE_WAIT_MS)` (bounded pure-drain pump-wait, the `_frame_cell_wait` shape:
    `_frame_pump(f)` + deadline + 1 ms sleeps). `_loop_control` / the old loop keep the
    sync wrapper; PYRT_RESULT's cell.result post becomes FIRE-AND-POST (`reply_to NULL`) —
    the slot completes in the same dispatch exactly as today, and the event's commitment
    is guaranteed FIFO-ahead of anything the frame posts after it (the next derive's scan).
  - `_frame_remember_variant` → `_frame_remember_post(f, key, json_value, prefix, corr,
    reply_to)` (compose + seq-alloc + post; on the sync path the public wrapper waits).
    Public `frame_remember_local/ctx` = post + `_frame_sync_wait`. The FRM_REMEMBER
    behavior = `_frame_remember_post(..., corr = the bridge corr, reply_to = &f->actor)` —
    NO synchronous bridge answer anymore; the router's step 5 answers the corr when the
    store reply lands.
  - `frame_recall` → compose + post `FRM_STORE_RECALL{key, f->sid_path, f->max_depth,
    reply_to = &f->actor, corr}` + `_frame_sync_wait` (public); the FRM_RECALL behavior
    posts the same and lets the router answer (`is_recall` text path).
  - `frame_append_msg` → one-op batch post + sync wait (public); Task 3's content path
    composes append + status into the FINISH batch instead.
  - `frame_spawn` → compose (today's whole admission composition is UNCHANGED — the
    parent's seq is its own pre-allocated seq, the child subtree is fresh so it has no
    other writer) + seq-alloc + post + sync-wait (public: the child is returned only
    after the admission COMMITS — today's contract, now confirmed by the store reply;
    the refusal path destroys the child exactly as today) — plus the new
    `spawn_pending` slot {in_use, corr, frame_t* child, bridge_corr} so the reply handler
    has the child object to either return (sync) or start (Task 5) or destroy loud
    (refusal). A cell-side FRM_SPAWN keeps today's corr-matched answer shape, now
    answered from the router (the waiter's bounded py-agent wait covers the extra hop).
  - `frame_report` → the CHILD composes ONLY its own record (child seq pre-allocated,
    child status op composed as full-root-path keys) and posts
    `FRM_REPORT_BIND{reply_to = &f->actor, corr = child's own store corr, bridge_corr,
    engine_driven = 0, child_sid, child_seq, child_event_text, text}` to the PARENT's
    actor. The PARENT's FRM_REPORT_BIND behavior allocates the parent's seq, composes the
    WHOLE three-op batch (the child's record + the child's meta/status=done + the parent's
    bound event), and posts ONE `FRM_STORE_BATCH{reply_to = the CHILD's actor (carried
    from the bind), corr = the bind's corr}`. The child's router (step 2, via
    `bind_pending{in_use, corr, bridge_corr, engine_driven, own_seq}`) releases the sync
    API's caller (its `_frame_sync_wait` polls the slot), answers a cell-side
    `bridge_corr` through the bridge sink, and rolls the child's seq back best-effort +
    logs loud on refusal. Task 6's engine terminate composes the SAME bind with
    `engine_driven = 1` (the FRM_CHILD_REPORT resume posts from the reply). The parent's
    seq rollback on ITS composed-batch refusal lives in the parent's own FRM_REPORT_BIND
    behavior (it receives no store reply — the reply goes to the child; the parent learns
    nothing from the store here, which is correct: its own seq was pre-allocated at
    compose and the store worker logs the refusal).
  - `frame_join` → parent-side single-op batch post + sync wait (public); Task 5 folds
    it into the resume path.
  - `_frame_set_status_done` → one-op batch post + sync wait (the old loop's caller).
- **The pool guard**: `frame_create`/`frame_resume` refuse loud (NULL + log_error) a
  non-NULL `cfg->pool` while the ROOT's store actor is inline
  (`f->root->store_pool == NULL`) — a pooled frame posting into an un-pumped store
  mailbox is a hang, and hangs lie.
- **`_frame_pump(f)`**: exactly the frame_internal.h contract —
  `actor_run(&f->actor, ACTOR_BATCH_SIZE)`, then for each live ancestor (while
  `a = a->parent != NULL`, inline only, bounded by depth) `actor_run(&a->actor, ...)`,
  then `if (f->root->store_pool == NULL) actor_run(&f->root->store_actor, ACTOR_BATCH_SIZE)`.
- **`_frame_cell_wait`'s pump** is replaced by `_frame_pump(f)` (wider order, same
  deadline discipline) — a cell's bridge verbs now round-trip the store inside one pump
  cycle on the inline shape.
- NO new `platform_mutex`/`rwlock`/`barrier`/`semaphore` anywhere. NO behavioral change to
  the inline/uncontended tests except the bridge verbs' two-pump shape (Step 2's recut).

- [ ] **Step 4: Red → green: the whole suite (every existing test's assertions unchanged) + the ASan config**

```bash
cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
cmake --build cmake-build-asan --target testsecretagent -j && setarch -R ctest --test-dir cmake-build-asan --output-on-failure 2>/dev/null | tail -3
```

- [ ] **Step 5: Prove the no-lock amendment (the standing grep from the idiom block — expect exactly "no locks in the frame layer")**

```bash
{ grep -rn "platform_mutex\|platform_rwlock\|platform_barrier\|platform_sem" src/Frame/ \
  && echo "VIOLATION: a lock in the frame layer"; } || echo "no locks in the frame layer"
```

- [ ] **Step 6: Valgrind on TestStore.* + TestFrame.* (stripped copy, verified-executing) — 0 leaks**
- [ ] **Step 7: Commit**

```bash
git add src/Frame/frame.h src/Frame/frame_messages.h src/Frame/frame_internal.h src/Frame/frame.c tools/frame-demo/main.c test/test_frame.cpp
git commit -m "feat: the store actor serializes frame writes and reads (no locks)"
```

---

### Task 3: The turn engine — frame_run_loop over scheduled continuations

The synchronous `for(;;)` of `frame_run_loop` becomes a phase machine driven by the
frame's own dispatch (spec §1). All existing TestLoop tests keep every assertion
byte-for-byte — the scripted sync backends drive the SAME reply-processing code inline,
now reached one store round trip later (the driver pumps; the assertions ride post-commit
state, which every awaited phase guarantees).

**Files:**
- Modify: `src/Frame/frame_internal.h`, `src/Frame/frame.c`, `src/Frame/loop.h`, `src/Frame/loop.c`, `test/test_loop.cpp`

- [ ] **Step 1: The frozen engine contract (frame_internal.h additions)**

```c
/* The turn engine's phases (spec §1): the frame yields between all of them. */
typedef enum frame_phase_e {
  FRAME_PHASE_NONE = 0,     /* between steps / engine not started */
  FRAME_PHASE_MODEL,        /* an async model submit is in flight */
  FRAME_PHASE_CELL,         /* the turn's one cell is in flight */
  FRAME_PHASE_STORE,        /* a store round trip is in flight (Task 2's vocabulary) */
  FRAME_PHASE_CHILDREN      /* yielded: live children pending (Task 5 fills this) */
} frame_phase_e;

/* What the pending FRAME_PHASE_STORE round trip is for (frame_t.store_kind):
   each kind's reply continues the turn inside the reply dispatch itself. */
typedef enum frame_store_kind_e {
  FRAME_STORE_DERIVE = 1,   /* the bounded events scan; the reply parses the records
                               and runs the projection + submit/reply processing */
  FRAME_STORE_CELL_RUN,     /* the cell.run audit commit; the reply dispatches
                               FRM_CELL_EXECUTE (no untracked cell ever runs) */
  FRAME_STORE_FINISH        /* msg.append (+ meta/status=done for a top frame) in ONE
                               batch; the reply ends the engine (top) or drives the
                               child's quiet-completion report bind (child) */
} frame_store_kind_e;

/* Engine handlers (loop.c implements, frame.c's _frame_behavior routes): */

/* Start the engine: refuses (-1, loud) on a dead frame or an engine already
   live; else resets the per-run knobs (turns_issued, model_retries,
   engine_failed), sets engine_live = 1, and posts ONE FRM_TURN continuation. */
int _frame_engine_start(frame_t* f);

/* The FRM_TURN behavior: ONE turn step (checks → the derive store round trip
   → on its reply the submit/complete + tool/content paths), then yields — it
   returns without blocking on anything. */
void _frame_engine_turn(frame_t* f);

/* The FRM_STORE_REPLY branch for engine round trips (frame.c routes here when
   phase == FRAME_PHASE_STORE and the corr matches store_corr): DERIVE → parse
   the reply's records into the DOM (µs, bounded 512, unparseable dropped loud)
   and run the projection + backend resolve; CELL_RUN → rc != 0 = control
   "audit-error" + terminate, rc == 0 = dispatch FRM_CELL_EXECUTE; FINISH →
   end the engine (top) / drive the child's report bind (child). */
void _frame_engine_store_reply(frame_t* f, frm_store_reply_payload_t* payload);

/* The FRM_MODEL_RESULT behavior: decode the raw completion (model_internal.h,
   Task 4) + process the reply; a decode/model error retries EXACTLY ONCE
   (model_retries) then terminates failed. */
void _frame_engine_model_arrived(frame_t* f, frm_model_payload_t* payload);

/* Called by frame.c's PYRT_RESULT handler AFTER the cell slot completed:
   engine_live && phase == CELL → repost the FRM_TURN continuation.
   (Synchronous cell refusals — pending never set — let the FRM_CELL_RUN reply
   step resume; see _frame_engine_store_reply's CELL_RUN path.) */
void _frame_engine_cell_done(frame_t* f);

/* The FRM_CHILD_REPORT behavior (Task 5 fills it; Task 3 declares the route):
   bookkeeping + resume-only-a-live-engine. */
void _frame_engine_child_report(frame_t* f, frm_child_report_payload_t* payload);

/* The sync driver's pump deadlines (the cell deadline SA_LOOP_CELL_WAIT_MS
   60000 already lives in loop.c): a config-slow model's own timeout + slack
   must fit under SA_LOOP_MODEL_WAIT_MS; a store round trip is µs–ms, so its
   deadline break is a loud stall, never a hang. */
#ifndef SA_LOOP_MODEL_WAIT_MS
#define SA_LOOP_MODEL_WAIT_MS 300000
#endif
#ifndef SA_LOOP_STORE_WAIT_MS
#define SA_LOOP_STORE_WAIT_MS 30000
#endif
```

`frame.c`'s `frame_t` gains the engine knobs with the store fields:
`engine_live`, `phase`, `store_kind`, `store_corr` (the frame's own round-trip key,
allocated from `_frame_bridge_pending`'s corr space — one counter, no collisions),
`turns_issued`, `model_retries`, `engine_failed` (Task 5 adds `live_children`).

- [ ] **Step 2: loop.c — the engine, keeping every derive/tool/content rule byte-equivalent**

The restructure, in the exact rule order the old `for(;;)` ran:

- `_frame_engine_turn(f)`: the loop checks become the step's checks IN THE SAME ORDER —
  `stop_requested` → engine end; `frame_is_done` → engine end (the advisory direct read,
  spec §5's carve-out); `turns_issued == cap` → `control turn-limit` (fire-and-post, the
  store worker commits it FIFO-ahead of the terminate's bind) + terminate-failed; backend
  resolve → `control model-missing`. Then the DERIVE becomes the store round trip:
  compose the bounds (`_frame_events_bounds` shape — pure key composition, no store
  access), post `FRM_STORE_SCAN{start, end, limit = SA_FRAME_DEBUG_MAX_EVENTS,
  reply_to = &f->actor, corr = the frame's next store corr}`, set
  `phase = FRAME_PHASE_STORE; store_kind = FRAME_STORE_DERIVE; store_corr = corr`,
  destroy nothing else, return — yield.
- `_frame_engine_store_reply(f, payload)` (the FRM_STORE_REPLY branch above):
  - `FRAME_STORE_DERIVE`, `rc != 0` → `control derive-error` + terminate-failed.
    `rc == 0` → parse each record (`json_parse`, µs; unparseable dropped loud), build the
    DOM exactly as today's `frame_debug_events` tail does (the materialization loop MOVED
    to the router side), run `_loop_project(f, events_dom)` — today's
    `_loop_derive_context` two-pass projection with its input changed from
    "frame_debug_events's string" to "the parsed DOM", EVERY OTHER RULE UNTOUCHED
    (pass A snapshot/reports, the result ring, pass B msg/cell results, the caps) —
    then the backend path: `mb == NULL` → `control model-missing` + terminate-failed;
    backend's `submit` present (Task 4) → `rc == 0` → `phase = FRAME_PHASE_MODEL`,
    destroy the derived messages, yield; `rc != 0` → `control submit-failed` +
    terminate-failed. Sync-only backend (`submit == NULL` — every existing scripted test
    backend): `complete()` runs INLINE inside this dispatch — blocking the actor,
    acceptable ONLY on the documented inline/test driver (spec §6), never on a pool
    worker in production — then `_frame_engine_reply`.
  - `FRAME_STORE_CELL_RUN`, `rc != 0` → `control audit-error` + terminate-failed (the old
    loop's rule, now at the reply). `rc == 0` → dispatch FRM_CELL_EXECUTE (today's
    tool path verbatim from here: the unclaimed-payload check, `model_reply_destroy`) →
    `_frame_cell_pending` → `phase = FRAME_PHASE_CELL`, yield — else (a synchronous
    refusal: second in-flight cell, pyrt boot failure, corr 0) the refusal paths get
    their PAIRED status-1 `cell.result` fire-and-post `FRM_STORE_BATCH` (today they were
    written by the slot completion; the refusals never set cell_pending, so the ENGINE
    writes the paired event itself right here — the audit-honesty fix, one place) and
    then repost FRM_TURN (resume).
  - `FRAME_STORE_FINISH`, `rc != 0` → `control commit-error` + terminate-failed.
    `rc == 0` → the END rule (Task 5 fills the children branch): top → engine ends
    (engine_live = 0, phase NONE); child → the quiet-completion terminate (Task 6's
    `_frame_engine_terminate(f, 1, reply content)`) instead of the old dangling
    "children are left running" note at the top of loop.c.
- `_frame_engine_reply(f, rc, reply, err)`: model error → `control model-error`
  (fire-and-post) + ONE retry: the old loop retried `complete()` with the SAME derived
  array and did NOT spend a turn on it; async the array is destroyed after submit — the
  retry is ONE fresh FRM_TURN repost (the derive is stateless from the store, so the
  re-derive is provably equivalent — a steering write that slipped in only ADDS context;
  the spec's §1 records this exact shape), `model_retries < 1` → repost, else
  `model-error-final` + terminate-failed. (One deliberate recut: the retry rides a store
  round trip instead of a retained array; semantics unchanged.)
- The tool path's pre-store tail: the python-missing gate stays BEFORE the cell.run
  round trip (`control python-missing` + terminate-failed); the `cell.run` event is the
  `FRAME_STORE_CELL_RUN` round trip (the audit commit BEFORE any cell executes — no
  untracked cell, the old rule held by the phase, not a blocking write).
- The content path: `msg.append`/`empty-turn`, then ONE `FRAME_STORE_FINISH` batch either
  way (one awaited step, one phase): a TOP frame with content composes the msg.append
  event + `meta/status=done` into one atomic batch (today two writes — the composition
  win: message + completion cannot half-apply); a TOP frame's EMPTY reply composes the
  `empty-turn` control (fire-and-post, FIFO-ahead) + `meta/status=done`; a CHILD's reply
  composes only the msg.append (or the empty-turn control; status stays "running") and
  the FINISH reply drives the quiet-completion terminate. Then
  `phase = FRAME_PHASE_STORE; store_kind = FRAME_STORE_FINISH` — yield.
- `PYRT_RESULT`'s frame.c tail: the cell.result event is today's fire-and-post batch
  (Task 2), the slot completes in-dispatch, then `_frame_engine_cell_done(f)` (repost
  FRM_TURN when the engine awaits the cell). Unmatched/late results keep dropping loud
  as today.
- Control events (`_loop_control`): FIRE-AND-POST (reply_to NULL, corr 0) — every engine
  path that can be OBSERVED externally ends through an awaited phase (FINISH/BIND reply),
  and the store's FIFO order puts every earlier control batch ahead of it, so assertions
  riding post-commit state stay deterministic. The sync DRIVER additionally drains the
  inline store to quiescence on its way back (below) — the belt to the FIFO's suspenders.
- `frame_run_loop(f)`: becomes start-or-pump + the bounded pump — when
  `engine_live == 0`, `frame_start(f)` (refused → rc 1; ALREADY-LIVE engines are fine:
  the driver PUMPS from where the engine is, which is how a spawned child's queued FRM_TURN
  gets drained by the synchronous driver) then `_frame_pump(f)` (`actor_run` over the
  frame, its inline ancestors, and the inline store — Task 2's pump) in a loop while
  `engine_live && phase != FRAME_PHASE_CHILDREN`, `platform_sleep_ms(1)` between pumps
  when the mailbox is quiet, deadline per phase (`SA_LOOP_CELL_WAIT_MS` for CELL,
  `SA_LOOP_MODEL_WAIT_MS` for MODEL — a deadline break is `control cell-timeout` /
  `control model-await` + engine end + rc 1; `SA_LOOP_STORE_WAIT_MS` for STORE — a break
  is `control store-timeout` + engine end + rc 1) — returning: `0` clean end (`1` when
  `engine_failed`), `2` on `FRAME_PHASE_CHILDREN`. loop.h's contract comment grows the 2.
  Before every return: if the frame's root store is inline, drain it to quiescence
  (`while (actor_run(&root->store_actor, ACTOR_BATCH_SIZE)) {}` bounded by the STORE wait
  deadline) so no fire-and-post outlives the driver's return.
  The pre-flight "dead frame" check and rc 1 stay.

- [ ] **Step 3: The failing test — an ASYNC scripted backend via frame_run_loop's pump**

```cpp
TEST(TestLoop, TestAsyncScriptedBackendDrivesTheSameEngine) {
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "async drive", &cfg);
  ASSERT_NE(f, nullptr);

  /* The async scripted backend: submit() stashes the serialized messages and
     POSTS an FRM_MODEL_RESULT straight back into the frame's mailbox (the
     shape a production backend's loop-thread completion lands in after the
     model.c relay forwards it) with ONE canned content-only completion. */
  async_model_t am;
  am.base.complete = NULL;                 /* async-only: the engine must take the submit path */
  am.base.submit = async_submit;
  am.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"async done"}}]})json");
  frame_set_model_backend(f, &am.base);

  /* The pump drives the SAME engine; the async step YIELDS at
     FRAME_PHASE_MODEL and the posted result resumes it; the derive itself
     rode the store actor's FRM_STORE_SCAN round trip before that. */
  EXPECT_EQ(frame_run_loop(f), 0);
  ASSERT_EQ(am.captured.size(), 1u);
  EXPECT_NE(am.captured[0].find("Goal: async drive"), std::string::npos);
  EXPECT_EQ(frame_is_done(f), 1);

  json_value_t* events = load_events(f);
  ASSERT_NE(events, nullptr);
  bool saw_assistant = false;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    if (event_is(rec, "msg.append") &&
        strcmp(json_as_string(json_get(json_get(rec, "payload"), "role")),
               "assistant") == 0 &&
        strcmp(json_as_string(json_get(json_get(rec, "payload"), "content")),
               "async done") == 0) {
      saw_assistant = true;
    }
  }
  EXPECT_TRUE(saw_assistant);
  json_value_destroy(events);

  frame_destroy(f);
  wave_db_close(db);
}
/* async_model_t / async_submit: submit copies what it needs (serialize
   messages into a heap buffer), builds the frm_model_payload_t{status=200,
   body=cheap_decode_buffer,…} with frm_model_payload_destroy as
   payload_destroy, actor_send's it to &f->actor, and returns 0. */
```
(The harness's shape mirrors scripted_model_t; async_submit may call the shared
`scripted_decode` on the canned body.)

- [ ] **Step 4: Red → green: the WHOLE TestLoop suite (scripted + steering + turn-limit + empty-turn + restart) with assertions unchanged; ASan config**
- [ ] **Step 5: Valgrind on TestLoop.* (stripped, verified) — 0 leaks**
- [ ] **Step 6: Re-run the no-lock grep (expect "no locks in the frame layer")**
- [ ] **Step 7: Commit**

```bash
git add src/Frame/frame_internal.h src/Frame/frame.c src/Frame/loop.h src/Frame/loop.c test/test_loop.cpp
git commit -m "feat: the turn loop dissolves into scheduled turn-step continuations"
```

---

### Task 4: The async model submit (model.c rides `http_client_submit`)

`model_backend_t` gains the optional `submit`; the http backend implements it over the
async transport, with decode moved OFF the reactor (spec §3). Store-actor note: the
engine calls `submit` from inside the derive-reply dispatch (`_frame_engine_store_reply`'s
FRAME_STORE_DERIVE path) — the call site moved from the FRM_TURN dispatch to the store
reply; everything below is unchanged. The store actor does not appear in this task.
**Files:**
- Create: `src/Frame/model_internal.h`
- Modify: `src/Frame/model.h`, `src/Frame/model.c`
- Test: `test/test_model_decode.cpp` (the fake-server fixture exists in test_streams_client.cpp; port the shape into a submit-path test)

- [ ] **Step 1: The frozen contract (model.h additions)**

```c
/* Asynchronous completion delivery (orchestration slice). After a rc==0
   submit, the sink fires EXACTLY ONCE — on the streams loop thread or
   synchronously within submit — and it takes OWNERSHIP of body and error
   (heap; free() or consume). A rc != 0 return means rejected before any I/O:
   the sink will NEVER fire for that call. submit must copy or serialize
   everything it needs from messages/tools before it returns. NULL = the
   backend is sync-only (scripted tests; the engine drains it inline). */
typedef void (*model_response_sink_fn)(void* ctx, int status, char* body,
                                       size_t body_len, char* error);

  /* model_backend_t gains, after complete: */
  int (*submit)(void* self, json_value_t* messages, json_value_t* tools,
                model_response_sink_fn on_done, void* on_done_ctx);
```

`src/Frame/model_internal.h` (the loop↔engine decode handoff):

```c
//
// Created by victor on 9/30/26.
//

#ifndef SA_MODEL_INTERNAL_H
#define SA_MODEL_INTERNAL_H

#include "../Frame/model.h"
#include <stddef.h>
#include "../Util/json.h"

#ifdef SA_HAS_WDB
/* status + raw body + transport error → decoded reply, with model.c's exact
   error surface ("model client: HTTP %d: <body excerpt | transport reason |
   (no body)>"). 0 ok (*reply_out owns the reply); nonzero (*error_out owns
   the reason). The single helper behind BOTH the synchronous complete() and
   the engine's FRM_MODEL_RESULT behavior — one error surface, two
   delivery modes. */
int _model_result_from_http(int status, const char* body, size_t body_len,
                            const char* transport_error,
                            model_reply_t** reply_out, char** error_out);
#endif /* SA_HAS_WDB */

#endif // SA_MODEL_INTERNAL_H
```

- [ ] **Step 2: The failing tests (test_model_decode.cpp — loopback, real loop thread)**

```cpp
TEST(TestModelDecode, TestSubmitRoundTripMatchesComplete) {
  /* canned-server fixture (the test_streams_client shape): one 200
     Content-Length body of a completions JSON with BOTH content and a tool
     call. Mount the http backend (model_http_backend_create over
     base_url=loopback), call submit(...) directly with a tiny sink that
     records (status, body) and posts completion of a test-side wait.
     Assert: rc == 0; exactly ONE sink call; status == 200;
     _model_result_from_http on the recorded body yields the SAME reply shape
     complete() yields on the same canned body (content + tool_code). */
}
TEST(TestModelDecode, TestSubmitTransportFailureReachesTheSink) {
  /* dead endpoint (port 1): rc == 0, then EXACTLY ONE sink call with
     status == -1 and a non-empty heap error (and NULL body). */
}
TEST(TestModelDecode, TestSubmitRejectedNeverCallsSink) {
  /* NULL body_json → rc != 0 and NO sink call within 50 ms (the http
     client's documented rejected-submit contract, inherited verbatim). */
}
```

(Each: wait bounded 3000 ms on a test-side condition; the fixture's fake server thread
joins before asserts. The http backend's process loop thread pins/unpins via
backend create/destroy as model.c already documents.)

- [ ] **Step 3: Implement `_model_http_submit`**

- Build `url` + serialized `request_text` from messages/tools FIRST
  (`_model_join_url` / `_model_request_text` unchanged — a build failure is a rc != 0
  return, sink never fired, per the contract above).
- Allocate the relay record `{ model_response_sink_fn fn; void* ctx; }` (heap; owns the
  sink pair) — a build/alloc failure after this point calls the sink SYNCHRONOUSLY with
  `status = -1` and a heap error, then returns 0 (accepted into the failure path — the
  "exactly once" invariant holds either way).
- `http_client_create(b->loop)`; its completion callback (loop thread, µs-scale:
  field-steal-free — it just forwards) does: `on_done->fn(on_done->ctx, status, body,
  body_len, error)` (ownership moves straight through), `free(relay)`, then defers the
  client teardown: `streams_loop_call(b->loop, _relay_destroy_client, client)` — the
  client contract forbids calling `http_client_destroy` from inside its own completion.
  (If `streams_loop_call` fails, the loop thread is dying — record the leak loudly; the
  loop's own fail-loud discipline has already aborted the process.)
- The SYNC side refactors to consume `_model_result_from_http` with its existing
  status/body/error handling — zero behavior change to `complete()`
  (test_model_decode's existing suites pin the text).
- `model_backend_destroy` unchanged (the relay/client die with their completion; the
  backend owns only its loop pin).

- [ ] **Step 4: Red → green: TestModelDecode + TestStreamsClient + the whole suite**
- [ ] **Step 5: Valgrind on TestModelDecode.* (stripped, verified) — 0 leaks**
- [ ] **Step 6: Commit**

```bash
git add src/Frame/model.h src/Frame/model.c src/Frame/model_internal.h test/test_model_decode.cpp
git commit -m "feat: the http model backend submits asynchronously to the frame's engine"
```

---

### Task 5: Spawn = admit + start; await children with event resume

Spawn's start rides the store actor's admission reply (Task 2's `spawn_pending`): the
child's engine is started when the admission is CONFIRMED committed, not when the batch
was handed to the store — stronger than "admission is durable at return", and the
event-driven flow needs nothing else. **Files:**
- Modify: `src/Frame/frame.c`, `src/Frame/frame_internal.h`, `src/Frame/frame.h` (comment additions only: frame_spawn documents the inherited borrowed backend + turn cap), `src/Frame/loop.c`
- Test: `test/test_frame.cpp`

- [ ] **Step 1: The failing tests (pooled tree, real workers, order-agnostic)**

```cpp
/* Shared harness: an inline-frame-tree test plus THE pooled orchestration
   test. The scripted backend keys its canned reply off the derived system
   prompt's Goal line (two different backends via two queues would race on
   pool scheduling order; goal-keying is order-proof). The db is
   wave_db_open_config with the SAME pool (a pooled frame requires a pooled
   store — Task 2's guard). */
TEST(TestFrameTree, TestPooledParentSpawnsChildAndResumesOnReport) {
  py_agent_init();
  frame_config_t cfg = test_config();
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  cfg.pool = pool;

  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;                    /* the store actor on the same pool */
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_t* parent = frame_create(db, NULL, "parent goal", &cfg);
  ASSERT_NE(parent, nullptr);

  /* Replies keyed by goal (see harness note): parent turn 1 = execute the
     spawn cell; child turn 1 = content-only "leaf done quietly"; any later
     parent turn = content-only "parent observed leaf". */
  goal_keyed_model_t gk;
  gk.base.complete = goal_keyed_complete;
  gk.spawn_cell =
      "import actor\nactor.spawn('leaf goal', context=None)\nprint('spawned')";
  frame_set_model_backend(parent, &gk.base);

  frame_start(parent);                     /* the event-driven entry */
  /* Poll for terminal (the pool runs everything; no joins, ever): */
  for (int i = 0; i < 6000 && !frame_is_done(parent); i++) platform_sleep_ms(10);
  EXPECT_TRUE(frame_is_done(parent))
      << "the parent resumed on the child's report and completed";

  /* The child's subtree on disk: spawn event in the parent + the child's
     frame.report (quiet completion) bound in + the folded frame.join. */
  json_value_t* events = load_events(parent);
  ASSERT_NE(events, nullptr);
  size_t n_report = 0, n_spawn = 0, n_join = 0;
  std::string child_sid;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* payload = json_get(rec, "payload");
    if (event_is(rec, "frame.spawn")) n_spawn++;
    if (event_is(rec, "frame.report")) { n_report++;
      if (child_sid.empty()) child_sid = json_as_string(json_get(payload, "child_sid")); }
    if (event_is(rec, "frame.join")) n_join++;
  }
  EXPECT_EQ(n_spawn, 1u);
  EXPECT_EQ(n_report, 1u) << "exactly ONE report per child, regardless of the "
                             "child's work volume (S004 accountability)";
  EXPECT_EQ(n_join, 1u) << "join folded into the resume path";
  EXPECT_FALSE(child_sid.empty());

  /* The child's own log, WITHOUT a live handle (the spawn happened inside
     the engine; the test never held the child): frame_resume(db, child_sid,
     &plain) with a POOL-LESS config opens its DONE subtree as an inline
     handle. A resumed done frame re-runs nothing (frame_start refuses for
     done frames); the handle's reads ride the documented DIRECT debug scan
     (frame_debug_events) — NO sync write API is available on it because the
     store here is POOLED (spec §5's inline-only rule: a sync write would
     refuse loud, which is exactly the shape this assertion documents). Assert
     its ONE frame.report and no dangling control event, then frame_destroy
     the handle. */
  {
    frame_config_t plain = test_config();   /* pool NULL: inline handle shape */
    frame_t* resumed = frame_resume(db, child_sid.c_str(), &plain);
    ASSERT_NE(resumed, nullptr);
    json_value_t* child_events = load_events(resumed);
    ASSERT_NE(child_events, nullptr);
    size_t n_child_report = 0;
    for (size_t i = 0; i < json_size(child_events); i++)
      if (event_is(json_at(child_events, i), "frame.report")) n_child_report++;
    EXPECT_EQ(n_child_report, 1u);
    json_value_destroy(child_events);
    frame_destroy(resumed);
  }
  json_value_destroy(events);

  /* The #819 scoping shape, same resolved-handle pattern: the parent's
     scripted cell calls actor.remember('inherited', '"hello"') (the ctx
     layer) BEFORE the spawn; the child's CELL calls actor.recall — a recall
     of "inherited" resolves through the store actor's walk (the pooled frame
     actor routes the reply); a recall of a parent LOCAL key returns NULL.
     Every assertion rides the store-actor round trip through the cells — the
     direct sync API is inline-only (spec §5), so a pooled test never calls
     frame_recall from the test thread. */

  frame_destroy(parent);   /* the resumed-child handle was destroyed above */
  scheduler_pool_stop(pool);      /* documented order: stop, then close, then destroy */
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}
```

```cpp
TEST(TestFrameTree, TestPooledTreeKeepsOneReportPerChildWithContiguousSeq) {
  /* The accountability storm (subdivide-session's assertion, now the
     natural product of seq pre-allocation + ONE store actor): N children
     spawned from one parent cell, each content-quits; the parent's log
     holds N bound reports in a CONTIGUOUS seq chain under concurrent
     scheduling — the property the old per-frame write lock test chased,
     proven without a lock. */
  py_agent_init();
  frame_config_t cfg = test_config();
  scheduler_pool_t* pool = scheduler_pool_create(4);
  scheduler_pool_start(pool);
  cfg.pool = pool;
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  frame_t* parent = frame_create(db, NULL, "storm root", &cfg);
  /* The parent's first (and only model) turn: ONE cell that spawns
     N = 8 children in a loop, then content-quits → FRAME_PHASE_CHILDREN. */
  goal_keyed_model_t gk;   /* as above; spawn_cell loops actor.spawn("leaf %d") 8 times */
  frame_set_model_backend(parent, &gk.base);
  frame_start(parent);
  for (int i = 0; i < 6000 && !frame_is_done(parent); i++) platform_sleep_ms(10);
  EXPECT_TRUE(frame_is_done(parent));

  json_value_t* events = load_events(parent);
  /* Assert: 1 spawn cell + 8 frame.spawn events + 8 bound frame.report events +
     8 frame.join events; every record's cause == the previous record's seq;
     seq runs 1..N_total contiguous (the store actor's serialization produced
     no gaps and no duplicates without any lock). */
  ...exact-count + contiguity loop as TestConcurrentReports did...
  json_value_destroy(events);
  frame_destroy(parent);
  scheduler_pool_stop(pool);
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}
```

```cpp
TEST(TestFrame, TestInlineParentYieldsAwaitingChildren) {
  /* NO pool: the inline shape. The parent's scripted model answers its FIRST
     turn with the spawn tool call (its cell admits + starts the child — the
     child's engine queues, nobody pumps an inline actor but its driver), and
     its SECOND turn with content-only text while the child is live:
     frame_run_loop returns 2 — yielded, live, awaiting children (the child's
     own loop is the test's business, as in the synchronous world today). */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* parent = frame_create(db, NULL, "inline await", &cfg);
  ASSERT_NE(parent, nullptr);
  spawn_then_content_model_t scm;
  scm.base.complete = scripted_complete;   /* the test_loop harness shape */
  scm.replies = {spawn_tool_body, content_body};   /* bodies as in TestLoop */
  frame_set_model_backend(parent, &scm.base);

  EXPECT_EQ(frame_run_loop(parent), 2) << "yielded awaiting children";
  EXPECT_EQ(frame_is_done(parent), 0) << "live children hold the frame running";

  json_value_t* events = load_events(parent);
  ...assert frame.spawn present; exactly one; the child sid named...
  json_value_destroy(events);
  frame_destroy(parent);
  wave_db_close(db);
}
```
(The `goal_keyed` harness: `goal_keyed_complete` inspects `json_get(json_at(messages, 0),
"content")` for the recorded goal keys and pops from the matching canned queue; full C++
listing shape follows scripted_model_t in test_loop.cpp. The pooled test's includes add
`../src/Platform/platform_time.h` (platform_sleep_ms) beside the frame headers. NOTE for
the implementer: test helpers cannot reach `frame_t` internals — every assertion goes
through the public API
(frame_debug_events/frame_recall/frame_is_done/frame_sid) exactly as existing suites do.
The `plain` config for `frame_resume` keeps the inline handle shape on a pooled store:
reads ride the documented direct debug scan, writes would refuse loud.)

- [ ] **Step 2: Implement**

- `frame_spawn`'s reply handler (Task 2's `spawn_pending` routing, START branch): inherit
  the parent's engine knobs (`loop_turn_cap`) AND the borrowed `backend` override
  (frame.h documents the borrowed inheritance: production leaves it NULL for everyone;
  tests set it once on the parent), then `_frame_engine_start(child)` — the child queues
  ONE FRM_TURN (inline) or schedules on the pool. ONLY after the admission reply confirms
  the commit.
- The FRM_SPAWN bridge behavior: STOP destroying the child — the reply's routed START
  (above) makes the child the admission's live product; the corr-matched bridge answer
  (the child's sid text) is still answered by the ROUTER at the admission reply (the
  sid is already known at compose — the wait covers the store hop).
- `frame_t` gains `size_t live_children;` — incremented by the spawn reply's START
  branch (counting exactly the children whose admission COMMITTED and whose engine
  started).
- `FRM_CHILD_REPORT` behavior (`_frame_engine_child_report`): `live_children--` (>= 0;
  a zero-count delivery logs loud) → fold the join: `frame_join` of that child as
  `frame_join_post` (the parent's own seq, compose + FIRE-AND-POST `FRM_STORE_BATCH` —
  the store's FIFO puts the join ahead of the resumed derive's scan, so the derive sees
  it; the store worker logs a refusal loud; on-failure-continue — the resume still
  happens) → if `engine_live` repost FRM_TURN (resume); if not, bookkeeping only (resume
  only a live engine — a direct-API caller's join bookkeeping still lands).
- Task 3's content-end rule now branches on `live_children > 0`: `phase =
  FRAME_PHASE_CHILDREN` (status stays "running"; frame_run_loop returns 2) instead of
  ending. Each later FRM_CHILD_REPORT resumes; `frame_is_done` only flips when the
  engine ends with `live_children == 0`.
- The quiet-completion end (Task 3) is already the child-side half of this flow; its
  report bind rides FRM_REPORT_BIND (Task 2) and the parent's resume arrives via
  FRM_CHILD_REPORT posted by the CHILD's bind-reply router (Task 6's route, already in
  Task 2's slot).
- The pooled store + pooled frames mix is now exercised for real (both tree tests); the
  no-lock grep runs again in verification.

- [ ] **Step 3: Red → green: TestFrameTree + TestFrame + TestLoop + TestLiveLoop(skipped-without-env) all green; ASan**
- [ ] **Step 4: Valgrind on TestFrameTree.* + TestLoop.* (stripped, verified) — 0 leaks**
- [ ] **Step 5: Commit**

```bash
git add src/Frame/frame.c src/Frame/frame_internal.h src/Frame/loop.c src/Frame/frame.h test/test_frame.cpp
git commit -m "feat: spawn admits and starts; parents resume from child report events"
```

---

### Task 6: Child failure fails loud into the parent

**Files:**
- Modify: `src/Frame/loop.c` (`_frame_engine_terminate` — Task 3 introduced it; Task 6 fills the child-notify branch), `test/test_loop.cpp`, `test/test_frame.cpp`

- [ ] **Step 1: The failing tests**

```cpp
TEST(TestLoop, TestChildTurnLimitFailsIntoTheParent) {
  /* The CHILD engine hits its own turn cap: control "turn-limit" in the
     child's log; the child's status done (no parent ever awaits a zombie);
     ONE frame.report with the failure text bound into the parent's log; a
     FRM_CHILD_REPORT{failed=1} posted to the parent. */
  py_agent_init();
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* parent = frame_create(db, NULL, "parent of failures", &cfg);
  frame_t* child = frame_spawn(parent, "spiral forever", NULL);
  ASSERT_NE(child, nullptr);
  /* The spawn ALREADY started the child's engine (Task 5) — its FRM_TURN is
     queued; frame_run_loop below PUMPS a live engine without re-starting it
     (Task 3's start-or-pump contract). */
  always_tool_model_t am;   /* the fallback scripted model from TestTurnLimitFailsLoud */
  am.base.complete = scripted_complete;   /* fallback = always a tool call */
  am.fallback = &always_tool_body;
  frame_set_model_backend(child, &am.base);   /* BEFORE the child's turn 1 fires */
  frame_set_loop_turn_cap(child, 1);

  EXPECT_EQ(frame_run_loop(child), 1) << "the engine failed, loudly";
  EXPECT_EQ(frame_is_done(child), 1) << "a failed CHILD is done — never a zombie a parent awaits";
  EXPECT_EQ(frame_is_done(parent), 0) << "the parent is a separate engine; unbuilt here";

  /* The binding rode the store: the terminate composed the bind, the parent's
     actor committed it, the reply routed back. Pump order (the
     Task-2 helper's documented order — frame, ancestors, inline store): */
  wave_db_pump(db);                       /* the bind batch commits */
  actor_run(&child->actor, ACTOR_BATCH_SIZE);    /* the bind reply routes; the
                                            FRM_CHILD_REPORT posts to the parent */
  actor_run(&parent->actor, ACTOR_BATCH_SIZE);   /* the parent's bookkeeping:
                                            live_children--, join post, resume check */
  wave_db_pump(db);                       /* the folded join commits */
  actor_run(&parent->actor, ACTOR_BATCH_SIZE);   /* the join reply (bookkeeping-only
                                            — the parent has no live engine) */

  /* The parent's log holds one frame.report whose text is the failure. */
  json_value_t* events = load_events(parent);
  ...assert exactly one frame.report; text contains "turn-limit"...
  /* And the child's own log carries the control kind. */
  json_value_t* child_events = load_events(child);
  ...assert one control event, kind "turn-limit"...

  /* The resume path is QUEUED on the parent (inline): engine-agnostic —
     the above pumps already drained it; assert the join event landed. */
  json_value_destroy(events); json_value_destroy(child_events);
  frame_destroy(child);
  frame_destroy(parent);
  wave_db_close(db);
}

TEST(TestFrameTree, TestPooledChildFailureResumesParentWithTheFailure) {
  /* The pooled shape end to end: parent turn 1 spawns; the child (cap
     inherited via frame_set_loop_turn_cap on the PARENT = 2, with an
     always-tool reply keyed to the child's goal) fails loud; the parent
     RESUMES and its next derive shows the failure line; the parent answers
     content and completes done. Assertions: child done; parent done; parent
     log has exactly ONE report for the child whose text carries the failure
     kind. All actor-driven (no pumps, no joins — the pool runs everything;
     poll frame_is_done). */
}
```

- [ ] **Step 2: Implement `_frame_engine_terminate(f, ok, text)` (spec §4)**

```c
/* Terminal step: end the engine; a CHILD (not already done, parent live)
   binds ONE frame.report with the outcome text (a failure = the control
   kind + reason; a quiet completion = the last assistant content) and posts
   FRM_CHILD_REPORT{failed} so the parent RESUMES. A TOP frame failure makes
   NO status change (the pinned cap-is-a-failure shape). Never silent. */
static void _frame_engine_terminate(frame_t* f, uint8_t ok, const char* text);
```

- engine side first: `engine_live = 0`, `phase = NONE`, `engine_failed` set — then the
  child's bind: the child pre-allocates ITS seq (compose-time), composes its OWN
  `frame.report` record + the failing text, posts `FRM_REPORT_BIND{reply_to = &f->actor,
  corr, bridge_corr = 0, child_sid, child_seq, child_event_text, text, engine_driven}`
  to the PARENT's actor and fills the child's single `bind_pending` slot. The store
  reply routes back (Task 2's route, step 2) → the child answers nothing (bridge_corr 0)
  and, because the bind was ENGINE-driven, posts `FRM_CHILD_REPORT{child_sid, failed = !ok}`
  to the parent — the parent resumes on a CONFIRMED-committed binding (spec §4's
  strengthening).
- failure text composition: `"<control kind>: <control text>"` (e.g. "turn-limit: model
  turn budget exhausted") — the bound report's text is the accountability surface, the
  child's own log keeps the control event with the exact kind (fire-and-post, FIFO-ahead
  of the bind).
- binding failure (the bind refused / the store refused): NOT a stop — log loud AND still
  post the `FRM_CHILD_REPORT` from the router's rc != 0 branch — a parent must never hang
  because a WAL write failed. (The child's seq stays pre-allocated/gapped on that path —
  spec §5's recorded consequence.)
- the retry/failure call sites from Task 3 route through this helper; assert (loud) that
  a terminate for an already-terminated engine is a log_error no-op.

- [ ] **Step 3: Red → green + full suite; ASan**
- [ ] **Step 4: Valgrind on TestLoop.* + TestFrame.* (stripped, verified)**
- [ ] **Step 5: Commit**

```bash
git add src/Frame/loop.c test/test_loop.cpp test/test_frame.cpp
git commit -m "feat: child failure fails loud into the parent with one report per child"
```

---

### Task 7: Live gate + full verification + Atlas evidence (both folds)

**Files:**
- Modify: `atlas/workflow.json` (+ rebuilt `atlas/atlas.html`), nothing else

- [ ] **Step 1: All three configs green**

```bash
cmake --build cmake-build-debug -j && cmake --build cmake-build-asan --target testsecretagent -j
setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3
setarch -R ctest --test-dir cmake-build-asan --output-on-failure 2>/dev/null | tail -3
cmake -S . -B cmake-build-off -DSA_ENABLE_PYTHON=OFF -DSA_ENABLE_WDB=OFF \
  -DSA_ENABLE_STREAMS=OFF -DSA_BUILD_TESTS=ON && cmake --build cmake-build-off -j && \
  setarch -R ctest --test-dir cmake-build-off --output-on-failure 2>/dev/null | tail -3
```
Expected: all green (TestLiveLoop SKIPS without the env var, never fails).

- [ ] **Step 2: Valgrind across the moved surfaces (stripped copies, verified-executing)**

Filters: `TestFrame.*:TestFrameTree.*:TestStore.*:TestLoop.*:TestModelDecode.*` (0
errors, 0 definitely-lost; CPython/WaveDB classes stay in the existing suppressions).
The store actor's teardown (actor_destroy then the db lifecycle) is in these windows —
a leaked batch payload or a stranded store reply shows here first.

- [ ] **Step 3: The no-lock amendment's standing grep**

```bash
{ grep -rn "platform_mutex\|platform_rwlock\|platform_barrier\|platform_sem" src/Frame/ \
  && echo "VIOLATION: a lock in the frame layer"; } || echo "no locks in the frame layer"
```
Expected: "no locks in the frame layer" — the store actor is the ONLY serializer. This
is the owner's amendment turned into evidence.

- [ ] **Step 4: The live gate rides the event-driven engine + the store actor**

```bash
SA_TEST_OLLAMA_URL=http://127.0.0.1:11434 SA_TEST_OLLAMA_MODEL=gemma4:latest \
  setarch -R ctest --test-dir cmake-build-debug -R TestLiveLoop --output-on-failure 2>/dev/null | tail -4
```
Expected: PASS. The gate's `frame_run_loop` now pumps the phase machine; the model call
arrives as FRM_MODEL_RESULT (through the http backend's submit) instead of blocking a
thread in `_model_http_complete`'s condvar, and every derive/recall in the gate is a
store-actor round trip on the inline store (the pump order covers it). If the model
narrates instead of acting, the gate's own fresh-frame reruns absorb it — rerun once
before concluding failure.

- [ ] **Step 5: Atlas evidence — close the restarts node evidence-only (b), fold the accountability assertions (a)**

- `remember-session-across-restarts` (evidence-only close): the description gains the
  kill-and-resume proof (TestLoop.TestRestartReplayRestoresSeqAndContext — scratch-disk
  WaveDB, close, reopen, frame_resume: loud birth-record gate, cross-restart recall
  (a store-actor round trip against the reopened root), the replayed msg.append + ctx
  snapshot reaching the next derive, the post-resume append at a seq strictly past every
  pre-restart event, asserted through a third session because of the recorded
  concurrent-mode same-session visibility defect) + this slice's event-driven proof (a
  resumed frame's engine re-runs off its persistent log, through the store actor's
  serialized reads). CompletionEvidence bullets get HONEST wording: Linux-proven shapes
  explicitly; the Windows bullet says "Windows verification pending (no Windows
  environment) — recorded on the slice's node, not silently absorbed". Try
  `status: "completed"`; if the atlas validator's completion lifecycle (atlas/validate.js
  + kit/lifecycle.cjs) demands a review record for it, KEEP `in-progress` with the
  evidence text and note the validator gate in the node description — never fake the
  review.
- `subdivide-session-into-accountable-agents`: the description gains the verification
  result ("folded into the frame-orchestration slice's acceptance: one report per child
  under concurrent scheduling — TestFrameTree's pooled multi-child test proves a
  contiguous seq chain over N spawned children's bound reports WITHOUT any lock —
  WaveDB's writes and reads serialize behind the root's store actor; lineage-derived
  tree; #819's scoping blind spot demonstrably absent (children resolve inherited ctx
  only via the resolve walk)").

```bash
cd atlas && node build.js && node build.js --check && node validate.js
```
Commit whatever the rebuild emits.

- [ ] **Step 6: Commit(s)**

```bash
git add atlas/workflow.json atlas/atlas.html
git commit -m "docs: frame orchestration evidence; restarts node closed evidence-only"
```

---

## Acceptance criteria (whole plan)

1. All configs green (ON / ASan under `setarch -R` / OFF with no python/wavedb/streams).
2. Valgrind clean on the restructured suites (verified-executing runs only).
3. The live gate passes against local Ollama riding the event-driven engine + the store
   actor — a worker thread never blocks on a model call or a store round trip;
   `frame_run_loop`'s pump never blocks beyond its documented 1 ms sleeps and phase
   deadlines.
4. Spawned children RUN: the pooled tree test proves a parent spawns, yields (return 2
   inline), resumes on the child's report, and completes; `frame_join` lands folded on
   the resume path; NO thread anywhere waits on a child or a model call.
5. Accountability invariants hold (subdivide-session folded): exactly ONE
   `frame.report` per child bound into the parent's log regardless of child work volume,
   under concurrent scheduling with a contiguous cause chain — with the store actor as
   the only serializer (cross-frame effects are ONE atomic batch; seq counters
   pre-allocated in each frame's own actor); the tree derives reconstructably from
   lineage pointers; children see inherited ctx only via the resolve walk (#819 absent).
6. Child failure fails loud: control event in the child's log, ONE failure report bound
   into the parent, the parent resumes and its derive shows the failure. No silent rot.
7. **NO lock in the frame layer** (the owner's amendment): the standing grep over
   `src/Frame/` finds no platform_mutex/rwlock/barrier/semaphore — the store actor is
   the only serializer of writes AND reads.
8. Atlas updated truthfully (evidence-only close for restarts; accountability fold);
   no TODOs anywhere touched; atomic conventional commits.

## Known pending (recorded, not built here)

- Windows verification (no Windows environment; recorded on the atlas node).
- The pooled engine has NO cell watchdog (a hung pyrt cell hangs that frame's turn
  indefinitely) — the sync driver keeps SA_LOOP_CELL_WAIT_MS; a frame-side timer facility
  is out of scope (escalated to the owner; the desktop slice's interrupt story covers
  hung cells).
- Restart of a frame with disk-"running" children: `live_children` is in-memory; the
  re-link belongs to the restart/reconcile work.
- A process-default scheduler pool (frames AND store): deliberately not built (escalated;
  the embedding caller owns its pools via `frame_config_t.pool` / `wave_db_open_config`).
- Seq GAPS in `events/` after a store rejection (the best-effort roll-back collapses the
  single-flight case; a wrong key is impossible) — spec §5's recorded consequence of
  pre-allocation; the restart/reconcile slice may compact gaps if it ever matters.
- The direct synchronous store API (`frame_recall`, `frame_spawn`, …) is inline-store-only
  (spec §5): on a pooled store it refuses loud — production calls reach these effects
  through the actor paths (the engine, the bridge verbs).