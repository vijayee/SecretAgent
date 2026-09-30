# Frame Orchestration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Frame orchestration: spawned frames actually run, fully event-driven — the turn loop dissolves into scheduled turn-step continuations (derive → model submit → yield; model/cell/child-arrival → next step); spawn = admit + start; parents resume from child events and never block on anything; child failure fails loud into the parent's log.

**Architecture:** `src/Frame/frame.{h,c}` gains the pool-carried frame (`frame_config_t.pool`, inherited down the lineage), `frame_start`, the per-frame write lock, and three new mailbox behaviors' routing (`FRM_TURN` / `FRM_MODEL_RESULT` / `FRM_CHILD_REPORT`). `src/Frame/loop.{h,c}` is rewritten from a held-thread `for(;;)` into the phase machine the same handlers run — the synchronous `frame_run_loop` stays as a test/demo driver over the SAME engine. `src/Frame/model.{h,c}` gains the optional async `submit` vtable member (the http backend's model call rides `http_client_submit` + a relay to the engine's sink). `src/Python/py_agent.c` untouched. Atlas closes `remember-session-across-restarts` evidence-only and folds `subdivide-session-into-accountable-agents`' verification into this plan's acceptance criteria.

**Spec:** `docs/superpowers/specs/2026-09-30-frame-orchestration-design.md`.

**References:** `src/Frame/model.c` (the completion record + process loop thread the submit path reuses), `src/Scheduler/scheduler.h` (pool create/start/stop/destroy/wait_for_idle, `scheduler_inject`), `src/Actor/actor.c` (`actor_init`'s pool registration, `actor_send`'s universal queue+inject path, `actor_destroy`'s pooled teardown waits), `docs/STYLE_GUIDE.md`.

**Frozen boundaries (hard rules):** never modify `liboffs/`, `WaveDB/`, `deepseek-harness/`, `prime-agent/`, `onyx/`, `claude-code-source-code/`, or anything under `deps/`. `src/Actor`, `src/Scheduler`, `src/RefCounter`, `src/Util`, `src/Streams`, `src/Buffer`, `src/Python` are frozen EXCEPT one line of `pyrt` glue if FRM_SPAWN's reply shape demands it (expected: none). All ctest runs: `setarch -R ctest --test-dir cmake-build-debug --output-on-failure 2>/dev/null | tail -3` (ASLR quirk). Valgrind needs `--strip-debug` copies AND verification the binary actually ran (`grep -c "OK ]"`); a "0 leaks" from an unstripped run is empty evidence. Conventional commits, no Co-Authored-By, never leave TODOs, atomics fine. Escalate on ambiguity you cannot resolve from the spec rather than inventing behavior.

## File structure

```
src/Frame/frame.h             (Tasks 1+2: frame_config_t.pool, frame_start, frame_pool, write-lock note)
src/Frame/frame_messages.h    (Tasks 1+5: FRM_TURN/FRM_MODEL_RESULT/FRM_CHILD_REPORT + their payloads)
src/Frame/frame_internal.h    (Tasks 1+5: the engine contract — phase enum, handlers, model wait deadline)
src/Frame/frame.c             (Tasks 1+2+3+5: pool plumbing, write lock, engine field storage, FRM_SPAWN keeps the child,
                               frame_destroy's pooled branch, dispatch cases)
src/Frame/loop.h              (Task 3: frame_run_loop's 0/1/2 contract)
src/Frame/loop.c              (Task 3+5+6: the turn engine — FRM_TURN step, reply paths, PHASE_CHILDREN, terminate)
src/Frame/model.h             (Task 4: model_backend_t.submit + model_response_sink_fn)
src/Frame/model.c             (Task 4: _model_http_submit, the relay, deferred client destroy, _model_result_from_http)
src/Frame/model_internal.h    (Task 4: the loop↔engine decode boundary)
tools/frame-demo/main.c       (Task 1 Step 1: explicit zero-init of its config — additive-field safety)
test/test_frame.cpp           (Tasks 1+2+5+6: pool plumbing, write-lock storm, pooled tree orchestration, failure accounting)
test/test_loop.cpp            (Task 1 Step 1's zero-init; Task 3: async scripted backend + engine compat; Task 6: child-failure tests)
test/test_model_decode.cpp    (Task 4: submit-path round-trip + transport-failure tests)
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
```

---

### Task 1: Pool-carried frames + the FRM_TURN vocabulary + `frame_start`

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
   happened in the child's own report batch (under the parent's write lock);
   this message RESUMES the parent's live engine. failed = the child ended on
   a control event (the parent's thread logs the resume loudly for it). */
typedef struct frm_child_report_payload_t { char* child_sid; uint8_t failed; } frm_child_report_payload_t;
void frm_model_payload_destroy(void* p);
void frm_child_report_payload_destroy(void* p);
```

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

### Task 2: One write lock per frame (the cross-thread seq discipline)

Pooled frames write each other's logs (a child's report batch bumps `parent->seq`);
WaveDB serializes the batches, but the in-memory seq counter and event-key uniqueness
are frame-local state. Every event-bearing write to frame X's log takes X's
`write_lock` (spec §5); lock order is `the log's frame, then lineage ancestors` —
acyclic on a tree, so no deadlock exists.

**Files:**
- Modify: `src/Frame/frame.c` (`_frame_event_write`, `_frame_remember_variant`, `frame_spawn`, `frame_report`, lock create/destroy in `_frame_alloc`/`frame_resume`/`frame_destroy`)
- Test: `test/test_frame.cpp`

- [ ] **Step 1: Write the failing test (a concurrent log storm with the contiguity assertion)**

```cpp
TEST(TestFrame, TestConcurrentReportsKeepCauseChainContiguous) {
  /* The failure mode the write lock exists for: two pool workers report
     children INTO ONE parent concurrently. Without the lock the parent's seq
     counter races — duplicated or gapped event keys / cause chains. With it,
     the parent's log is a contiguous chain, and its seq ends at exactly the
     number of effects. */
  frame_config_t cfg = test_config();
  scheduler_pool_t* pool = scheduler_pool_create(4);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  cfg.pool = pool;
  wave_database_root_t* db = wave_db_open(NULL);
  frame_t* parent = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(parent, nullptr);

  /* Admission-only children (their engines are queued; pool not yet running
     work for them — they are never dispatched) whose reports run from worker
     tasks. */
  enum { N = 16 };
  frame_t* children[N];
  for (int i = 0; i < N; i++) {
    children[i] = frame_spawn(parent, NULL, NULL);
    ASSERT_NE(children[i], nullptr);
  }
  /* The storm: every child reports from its OWN std::thread, against the
     parent's (and its own) write locks. */
  std::vector<std::thread> ts;
  for (int i = 0; i < N; i++) {
    ts.emplace_back([children, i]() {
      char buf[32];
      snprintf(buf, sizeof(buf), "report %d", i);
      frame_report(children[i], buf);
    });
  }
  for (auto& t : ts) t.join();

  /* Contiguity: parse the parent's events; every record's cause == the
     previous record's seq, and the count == N (spawn) + N (reports) + 0. */
  char* json = frame_debug_events(parent);
  ASSERT_NE(json, nullptr);
  char* err = NULL;
  json_value_t* events = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(json_type(events), JSON_ARRAY);
  ASSERT_EQ(json_size(events), 2u * N);
  size_t seq = 1;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* prev = (i > 0) ? json_at(events, i - 1) : NULL;
    if (prev != NULL) {
      EXPECT_EQ(json_as_int(json_get(rec, "cause")),
                json_as_int(json_get(prev, "seq")))
          << "the cause chain broke at index " << i;
    }
    EXPECT_EQ((size_t)json_as_int(json_get(rec, "seq")), seq++);
  }
  json_value_destroy(events);
  free(json);

  for (int i = 0; i < N; i++) frame_destroy(children[i]);
  frame_destroy(parent);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  wave_db_close(db);
}
```
(The test file gains `#include <thread>` + `#include "../src/Scheduler/scheduler.h"` in the extern "C" block.)

- [ ] **Step 2: Implement the lock**

- `frame_t` gains `platform_mutex_t* write_lock;` — created in `_frame_alloc` AND
  `frame_resume` (`f->write_lock = platform_mutex_create();` — fail the alloc loudly on
  NULL), freed in `frame_destroy` (`if (f->write_lock != NULL) platform_mutex_destroy(...)`).
  frame.c's include of `../Platform/platform.h` stays banned (its barrier prototypes
  collide — the file already notes this): `write_lock` needs only `platform_time.h`'s
  neighborhood, so include `../Platform/platform_mutex.h`/the actual primitive header the
  other mutex users include via platform.h — read `src/Platform/` and include the NARROW
  header (the file header comment at frame.c:70-74 documents the collision; extend it).
- `_frame_event_write(f, …)` and `_frame_remember_variant(f, …)`: lock → seq read +
  batch + seq bump → unlock.
- `frame_spawn`: parent's write_lock held across ITS OWN seq read + the whole
  `database_batch_sync_raw` (the child's birth meta is a fresh subtree — no lock needed
  there; document why at the batch).
- `frame_report`: child's write_lock FIRST, then the parent's (lock order, documented at
  the function), both held across the one root batch.
- NO behavioral change is intended anywhere: the inline (uncontended) path is one
  non-contended mutex per effect. All existing tests keep their exact assertions.

- [ ] **Step 3: Red → green + valgrind on TestFrame.* + ASan suite**
- [ ] **Step 4: Commit**

```bash
git add src/Frame/frame.c test/test_frame.cpp
git commit -m "fix: per-frame write lock keeps cross-frame event batches race-free on pooled trees"
```

---

### Task 3: The turn engine — frame_run_loop over scheduled continuations

The synchronous `for(;;)` of `frame_run_loop` becomes a phase machine driven by the
frame's own dispatch (spec §1). All existing TestLoop tests keep every assertion
byte-for-byte — the scripted sync backends drive the SAME reply-processing code inline.

**Files:**
- Modify: `src/Frame/frame_internal.h`, `src/Frame/frame.c`, `src/Frame/loop.h`, `src/Frame/loop.c`, `test/test_loop.cpp`

- [ ] **Step 1: The frozen engine contract (frame_internal.h additions)**

```c
/* The turn engine's phases (spec §1): the frame yields between all of them. */
typedef enum frame_phase_e {
  FRAME_PHASE_NONE = 0,     /* between steps / engine not started */
  FRAME_PHASE_MODEL,        /* an async model submit is in flight */
  FRAME_PHASE_CELL,         /* the turn's one cell is in flight */
  FRAME_PHASE_CHILDREN      /* yielded: live children pending (Task 5 fills this) */
} frame_phase_e;

/* Engine handlers (loop.c implements, frame.c's _frame_behavior routes): */

/* Start the engine: refuses (-1, loud) on a dead frame or an engine already
   live; else resets the per-run knobs (turns_issued, model_retries,
   engine_failed), sets engine_live = 1, and posts ONE FRM_TURN continuation. */
int _frame_engine_start(frame_t* f);

/* The FRM_TURN behavior: ONE turn step (derive → submit/complete → tool or
   content path), then yields — it returns without blocking on anything. */
void _frame_engine_turn(frame_t* f);

/* The FRM_MODEL_RESULT behavior: decode the raw completion (model_internal.h,
   Task 4) + process the reply; a decode/model error retries EXACTLY ONCE
   (model_retries) then terminates failed. */
void _frame_engine_model_arrived(frame_t* f, frm_model_payload_t* payload);

/* Called by frame.c's PYRT_RESULT handler AFTER the cell slot completed:
   engine_live && phase == CELL → repost the FRM_TURN continuation.
   (Synchronous cell refusals — pending never set — let the FRM_TURN step
   itself resume; see loop.c's post-dispatch check.) */
void _frame_engine_cell_done(frame_t* f);

/* The FRM_CHILD_REPORT behavior (Task 5 fills it; Task 3 declares the route):
   bookkeeping + resume-only-a-live-engine. */
void _frame_engine_child_report(frame_t* f, frm_child_report_payload_t* payload);

/* The model-await deadline of the SYNC driver's pump (the cell deadline
   SA_LOOP_CELL_WAIT_MS 60000 already lives in loop.c): a config-slow model's
   own timeout + slack must fit under this, or the driver fails loud. */
#ifndef SA_LOOP_MODEL_WAIT_MS
#define SA_LOOP_MODEL_WAIT_MS 300000
#endif
```

- [ ] **Step 2: loop.c — the engine, keeping every derive/tool/content rule byte-equivalent**

The restructure, in the exact rule order the old `for(;;)` ran:

- `_frame_engine_turn(f)`: the loop checks become the step's checks IN THE SAME ORDER —
  `stop_requested` → engine end; `frame_is_done` → engine end; `turns_issued == cap` →
  `control turn-limit` + terminate-failed; derive → `control derive-error`; backend →
  `control model-missing`. Then: backend's `submit` (may be NULL until Task 4 — the
  sync-only branch IS `complete()`) → reply processing.
- `_frame_engine_reply(f, rc, reply, err)`: model error → `control model-error` + one
  repost (`model_retries < 1`) else `model-error-final` + terminate-failed. Tool path =
  TODAY's tool path verbatim: python-missing gate, `cell.run` event
  (`control audit-error` + terminate on refusal), FRM_CELL_EXECUTE dispatch — then ONE
  new audit-honesty fix: the refusal paths that never set `cell_pending` (a second
  in-flight cell, pyrt boot failure, corr 0) get their PAIRED status-1 `cell.result`
  event written by the engine right after the dispatch (today the trail ends at cell.run —
  a silent hole; the write uses the slot's own `cell_status`). Then: `_frame_cell_pending`
  → `phase = FRAME_PHASE_CELL`, yield — else resume directly (repost FRM_TURN). Content
  path = TODAY's content path verbatim: `frame_append_msg("assistant", …)` /
  `control empty-turn`, then the END rule of Task 5 (this task: no live children exist
  yet, so top → `_frame_set_status_done` + end; child → quiet-completion + terminate —
  `frame_report(f, reply->content)` + post FRM_CHILD_REPORT + end; this replaces the old
  dangling "children are left running" note at the top of loop.c — see spec §4).
- `PYRT_RESULT`'s frame.c tail: `_frame_engine_cell_done(f)` (repost FRM_TURN when the
  engine awaits the cell). Unmatched/late results keep dropping loud as today.
- `frame_run_loop(f)`: becomes start-or-pump + the bounded pump — when
  `engine_live == 0`, `frame_start(f)` (refused → rc 1; ALREADY-LIVE engines are fine:
  the driver PUMPS from where the engine is, which is how a spawned child's queued FRM_TURN
  gets drained by the synchronous driver) then `actor_run` in a loop
  while `engine_live && phase != FRAME_PHASE_CHILDREN`, `platform_sleep_ms(1)` between
  pumps when the mailbox is quiet, deadline per phase (`SA_LOOP_CELL_WAIT_MS` for CELL,
  `SA_LOOP_MODEL_WAIT_MS` for MODEL — a deadline break is `control cell-timeout` /
  `control model-await` + engine end + rc 1) — returning: `0` clean end, `1` when
  `engine_failed`, `2` on `FRAME_PHASE_CHILDREN`. loop.h's contract comment grows the 2.
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
     FRAME_PHASE_MODEL and the posted result resumes it. */
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
- [ ] **Step 6: Commit**

```bash
git add src/Frame/frame_internal.h src/Frame/frame.c src/Frame/loop.h src/Frame/loop.c test/test_loop.cpp
git commit -m "feat: the turn loop dissolves into scheduled turn-step continuations"
```

---

### Task 4: The async model submit (model.c rides `http_client_submit`)

`model_backend_t` gains the optional `submit`; the http backend implements it over the
async transport, with decode moved OFF the reactor (spec §3).

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

**Files:**
- Modify: `src/Frame/frame.c`, `src/Frame/frame_internal.h`, `src/Frame/frame.h` (comment additions only: frame_spawn documents the inherited borrowed backend + turn cap), `src/Frame/loop.c`
- Test: `test/test_frame.cpp`

- [ ] **Step 1: The failing tests (pooled tree, real workers, order-agnostic)**

```cpp
/* Shared harness: an inline-frame-tree test plus THE pooled orchestration
   test. The scripted backend keys its canned reply off the derived system
   prompt's Goal line (two different backends via two queues would race on
   pool scheduling order; goal-keying is order-proof). */
TEST(TestFrameTree, TestPooledParentSpawnsChildAndResumesOnReport) {
  py_agent_init();
  frame_config_t cfg = test_config();
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  cfg.pool = pool;

  wave_database_root_t* db = wave_db_open(NULL);
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
     &cfg) opens its DONE subtree — a resumed done frame re-runs nothing
     (frame_start refuses for done frames) — then frame_debug_events reads
     it. Assert its ONE frame.report and no dangling control event, then
     frame_destroy the handle. */
  {
    frame_t* resumed = frame_resume(db, child_sid.c_str(), &cfg);
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
     layer) BEFORE the spawn; the resumed child's frame_recall of "inherited"
     resolves through the walk; a recall of a parent LOCAL key returns NULL.
     Every assertion rides the public API — no frame_t internals. */

  frame_destroy(parent);   /* the resumed-child handle was destroyed above */
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);
  wave_db_close(db);
}

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
(frame_debug_events/frame_recall/frame_is_done/frame_sid) exactly as existing suites do.)

- [ ] **Step 2: Implement**

- `frame_spawn` (its tail, after the admission batch commits): inherit the parent's
  engine knobs (`loop_turn_cap`) AND the borrowed `backend` override (frame.h documents
  the borrowed inheritance: production leaves it NULL for everyone; tests set it once on
  the parent), then `_frame_engine_start(child)` — the child queues ONE FRM_TURN (inline)
  or schedules on the pool.
- The FRM_SPAWN bridge behavior: STOP destroying the child — the started child IS the
  admission's live product now; the corr-matched reply stays the child's sid text. The
  handler also counts the bookkeeping (below).
- `frame_t` gains `size_t live_children;` — incremented by `frame_spawn` itself (parent's
  process bookkeeping; the FRM_SPAWN handler rides in through the same call).
- `FRM_CHILD_REPORT` behavior (`_frame_engine_child_report`): `live_children--` (>= 0;
  a zero-count delivery logs loud) → fold the join: `frame_join` of that child (one
  batch; on failure log loud and continue — the resume still happens) → if
  `engine_live` repost FRM_TURN (resume); if not, bookkeeping only (resume only a live
  engine — a direct-API caller's join bookkeeping still lands).
- Task 3's content-end rule now branches on `live_children > 0`: `phase =
  FRAME_PHASE_CHILDREN` (status stays "running"; frame_run_loop returns 2) instead of
  ending. Each later FRM_CHILD_REPORT resumes; `frame_is_done` only flips when the
  engine ends with `live_children == 0`.
- The quiet-completion end (Task 3) is already the child-side half of this flow.

- [ ] **Step 3: Red → green: TestFrameTree + TestFrame + TestLoop + TestLiveLoop(skipped-without-env) all green; ASan**
- [ ] **Step 4: Valgrind on TestFrameTree.* + TestLoop.* (stripped, verified) — 0 leaks**
- [ ] **Step 5: Commit**

```bash
git add src/Frame/frame.c src/Frame/frame_internal.h src/Frame/loop.c test/test_frame.cpp
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

  /* The parent's log holds one frame.report whose text is the failure. */
  json_value_t* events = load_events(parent);
  ...assert exactly one frame.report; text contains "turn-limit"...
  /* And the child's own log carries the control kind. */
  json_value_t* child_events = load_events(child);
  ...assert one control event, kind "turn-limit"...

  /* The resume message is QUEUED on the parent (inline): drain it and prove
     the bookkeeping is engine-agnostic (no live engine → bookkeeping only). */
  actor_run(&parent->actor, ACTOR_BATCH_SIZE);
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
     kind. */
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

- failure text composition: `"<control kind>: <control text>"` (e.g. "turn-limit: model
  turn budget exhausted") — the bound report's text is the accountability surface, the
  child's own log keeps the control event with the exact kind.
- binding failure (the report batch refused) is NOT a stop: log loud AND still post the
  FRM_CHILD_REPORT — a parent must never hang because a WAL write failed.
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

Filters: `TestFrame.*:TestFrameTree.*:TestLoop.*:TestModelDecode.*` (0 errors, 0
definitely-lost; CPython/WaveDB classes stay in the existing suppressions).

- [ ] **Step 3: The live gate rides the event-driven engine**

```bash
SA_TEST_OLLAMA_URL=http://127.0.0.1:11434 SA_TEST_OLLAMA_MODEL=gemma4:latest \
  setarch -R ctest --test-dir cmake-build-debug -R TestLiveLoop --output-on-failure 2>/dev/null | tail -4
```
Expected: PASS. The gate's `frame_run_loop` now pumps the phase machine; the model call
arrives as FRM_MODEL_RESULT (through the http backend's submit) instead of blocking a
thread in `_model_http_complete`'s condvar. If the model narrates instead of acting, the
gate's own fresh-frame reruns absorb it — rerun once before concluding failure.

- [ ] **Step 4: Atlas evidence — close the restarts node evidence-only (b), fold the accountability assertions (a)**

- `remember-session-across-restarts` (evidence-only close): the description gains the
  kill-and-resume proof (TestLoop.TestRestartReplayRestoresSeqAndContext — scratch-disk
  WaveDB, close, reopen, frame_resume: loud birth-record gate, cross-restart recall, the
  replayed msg.append + ctx snapshot reaching the next derive, the post-resume append at
  a seq strictly past every pre-restart event, asserted through a third session because
  of the recorded concurrent-mode same-session visibility defect) + this slice's
  event-driven proof (a resumed frame's engine re-runs off its persistent log).
  CompletionEvidence bullets get HONEST wording: Linux-proven shapes explicitly; the
  Windows bullet says "Windows verification pending (no Windows environment) — recorded
  on the slice's node, not silently absorbed". Try `status: "completed"`; if the atlas
  validator's completion lifecycle (atlas/validate.js + kit/lifecycle.cjs) demands a
  review record for it, KEEP `in-progress` with the evidence text and note the validator
  gate in the node description — never fake the review.
- `subdivide-session-into-accountable-agents`: the description gains the verification
  result ("folded into the frame-orchestration slice's acceptance: one report per child
  under concurrent scheduling — TestFrame.TestConcurrentReportsKeepCauseChainContiguous
  and TestFrameTree's pooled assertions; lineage-derived tree; #819's scoping blind spot
  demonstrably absent (children resolve inherited ctx only via the resolve walk)").

```bash
cd atlas && node build.js && node build.js --check && node validate.js
```
Commit whatever the rebuild emits.

- [ ] **Step 5: Commit(s)**

```bash
git add atlas/workflow.json atlas/atlas.html
git commit -m "docs: frame orchestration evidence; restarts node closed evidence-only"
```

---

## Acceptance criteria (whole plan)

1. All configs green (ON / ASan under `setarch -R` / OFF with no python/wavedb/streams).
2. Valgrind clean on the restructured suites (verified-executing runs only).
3. The live gate passes against local Ollama riding the event-driven engine — a worker
   thread never blocks on a model call; `frame_run_loop`'s pump never blocks beyond its
   documented 1 ms sleeps and phase deadlines.
4. Spawned children RUN: the pooled tree test proves a parent spawns, yields (return 2
   inline), resumes on the child's report, and completes; `frame_join` lands folded on
   the resume path; NO thread anywhere waits on a child or a model call.
5. Accountability invariants hold (subdivide-session folded): exactly ONE
   `frame.report` per child bound into the parent's log regardless of child work volume,
   under concurrent scheduling with a contiguous cause chain; the tree derives
   reconstructably from lineage pointers; children see inherited ctx only via the
   resolve walk (#819 absent).
6. Child failure fails loud: control event in the child's log, ONE failure report bound
   into the parent, the parent resumes and its derive shows the failure. No silent rot.
7. Atlas updated truthfully (evidence-only close for restarts; accountability fold);
   no TODOs anywhere touched; atomic conventional commits.

## Known pending (recorded, not built here)

- Windows verification (no Windows environment; recorded on the atlas node).
- The pooled engine has NO cell watchdog (a hung pyrt cell hangs that frame's turn
  indefinitely) — the sync driver keeps SA_LOOP_CELL_WAIT_MS; a frame-side timer facility
  is out of scope (escalated to the owner; the desktop slice's interrupt story covers
  hung cells).
- Restart of a frame with disk-"running" children: `live_children` is in-memory; the
  re-link belongs to the restart/reconcile work.
- A process-default scheduler pool: deliberately not built (escalated; the embedding
  caller owns its pool).