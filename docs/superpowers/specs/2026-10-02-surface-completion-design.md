# Surface Completion — Design

**Date:** 2026-10-02
**Status:** owner-agreed shape settled in session (six decisions, table below; approach A: frame-owned
seams + a per-pooled-cell watchdog thread). No open questions remain — every detail this design
touches is pinned. This slice RESOLVES two matrix owner escalations (Q4 pooled-cell watchdog; Q6
`inspect` pull-forward).
**Atlas slice:** a new node (delivery; parity feature matrix §6 slice rank #3, L2/L4) — evidence:
`test/test_budget.cpp` (new) + extended `test/test_pyrt.cpp` / `test/test_py_agent.cpp` /
`test/test_loop.cpp`.
**Parity sources:** `docs/parity-feature-matrix.md` §6 slice 3's sketch; §1 rows 29 (budget table),
25 (interrupt a running cell), 87 (the 6th half-built verb), and the known-pending print() gap from
the turn-lifecycle final review. The seven-verb audit (§2): this slice lands `write`'s durable half
(`emit`), pulls the smallest `inspect` member forward (`keys`), and leaves `read`/`inspect`'s full
semantics (events scan, child listing, path reads) deferred — recorded YAGNI unchanged (§7).

## Why this slice exists

The seven-verb surface is five verbs working plus two gaps the matrix pinned precisely:

1. **`emit` is dropped.** `agent.emit(text)` posts `PYRT_EMIT` to the frame (`py_agent.c:539` →
   `pyrt_post_text`) and `frame.c` has no `case PYRT_EMIT` — the payload falls into `default:` and
   vanishes. No durable output channel exists: a cell's deliberate artifacts (a finished report,
   streamed findings) survive only in the cell's own heap or as the single return value.
2. **The frame exposes no interrupt.** `pyrt_interrupt` is tested (`test_pyrt.cpp`) but the *frame*
   owns its pyrt privately (`tools/frame-demo/main.c:26-32`); a caller cannot reach a running
   pooled cell. The pooled engine's cell wait has no bound at all — a hung cell hangs the frame
   silently until destroy (matrix escalation Q4). The lifecycle slice reserved the reason-union
   members `aborted`/`blocked` for exactly this work (`lifecycle.h`).
3. **No budget table.** Caps exist but as scattered literals: the derive's three projection caps
   are `#define`s in `loop.c:146-161`, the transport caps live in `http_client.c:71-77`, and the
   tool-path / bridge / emit boundaries have NO cap at all. Truncation happens at *projection*
   time — the durable `cell.result` event stores the full text (matrix row 29's "PARTIAL").
4. **Exec-only cells lose print().** The embedded cell helper returns the last expression's repr
   (`pyrt.c:89-95`); `print()` output goes to the process's own stdout and is lost to the model.
   The subprocess backend treats stdout as the result (`py_subprocess.c:174`) — the two backends
   disagree on the envelope contract.

## Decisions carried in (settled with the owner, 2026-10-02)

| Decision | Value | Authority |
|---|---|---|
| Watchdog at deadline | **Interrupt + poison**: synthesize the corr-matched result, fire the interrupt, poison the runtime's frame; destroy-of-a-wedged-frame join is a documented cost | owner, per-question answer |
| `emit` durability | an **`emit` event kind** in the frame's own events stream (one stream, replay for free), source-capped, derive-rendered bounded lines | owner |
| `inspect` pull-forward | **`agent.keys(scope)` only** — own-subtree key listing; events scan / child listing / `read(path)` stay deferred | owner |
| Fail-loud at the source | **Truncate + marker**: the source actor cuts to the cap, appends an explicit truncation marker, delivers bounded text; durable events never carry unbounded text | owner |
| `print()` in embedded cells | **stdout captured, subprocess parity**: captured stdout IS the result; repr joins it when both exist | owner |
| `frame_interrupt` semantics | **Cut cell + abort turn**: interrupt synthesizes the failed result AND ends the open turn `aborted`; `FRM_STOP` (drain-at-boundary) stays separate; `blocked` stays reserved | owner |
| Architecture | Frame-owned seams: all policy in `frame.c` dispatch cases; pyrt/py_agent gain only source-side caps + stdout capture + the flag they already have; watchdog = a short-lived condvar thread per pooled cell. Two rejected alternates: a process-wide watchdog thread + deadline heap (global registry, violates the zero-locks discipline) and policy-inside-pyrt (the batch discipline lives in frame.c — pyrt would need the store) | owner |
| Out of scope | the `blocked` reason and the admission state machine (steering slice); a real kill path for the subprocess backend (SIGKILL seam) — the poison contract stands for both backends; `read(path)` full semantics; derive compaction; Windows | owner |

## 1. The `emit` event — the write verb's durable half

**Writer.** `frame.c` gains `case PYRT_EMIT`: compose a durable `emit` record in the frame's own
events stream at the frame's pre-allocated seq — ONE store batch, fire-and-post (corr 0,
reply_to NULL), the same discipline as `cell.result`'s event. Payload: `{"text": …}`. No lifecycle
riders — emit is a side effect, not a step boundary; it arrives during an open turn, between
turns, and in the demo-driver path, and every case just writes (the bridge verbs already prove a
store batch composes while a cell is pending). Store-refused emit: today's loud refusal contract
(log + drop; emit has no corr to answer).

**Source cap.** `_py_agent_emit` applies `SA_BUDGET_EMIT_BYTES` through the shared marker helper
(§4) BEFORE `pyrt_post_text`. One cap at one boundary — the frame trusts bounded text from its
runtime, per the budget doctrine.

**The silent-drop smell closes.** `PYRT_LOG` and `PYRT_STATUS` fall into `frame.c`'s `default:`
today — the frame drops them without a word. They get an explicit consumption: `log_info` lines
and a comment saying WHY they are deliberately not store records (messages are verbs, not nouns;
consumed on receipt). The derive does not render them — `log`/`status` stay narration.

**The derive.** Emitted records render in the model projection as one bounded line group (the
existing report-line group's shape): one line per emit, `"emit: <text>"`, in event order (the
projection's one ordering), each
capped at `SA_BUDGET_LOOP_EMIT` (300, table §4). This makes the verb real in both directions: the
model can emit durable output and will see it again in its own context.

## 2. `frame_interrupt` and the pooled-cell watchdog

**The entrypoint.** New public API in `frame.h`:

```c
void frame_interrupt(frame_t* f);   /* cut the pending cell; abort an open turn */
```

It composes a `FRM_INT` message (new `frame_messages.h` constant) and posts it into the frame's
OWN actor mailbox. Delivery is ordinary mailbox injection: the scheduler reaches a pooled frame
mid-turn — including while the engine awaits `PYRT_RESULT` — with zero new locks.

**`FRM_INT` dispatch — three cases, one synthesis helper (`_frame_interrupt_apply`, shared with
the watchdog):**

1. **Cell pending.** Record the interrupted corr (`f->cell_interrupted_corr`), synthesize the
   corr-matched `cell.result` status 1, text `"pyrt: interrupted"`, through the lifecycle slice's
   multi-record fire. When the turn is open the batch carries
   `[cell.result, step.end, turn.end {reason: aborted}]` atomically — the turn-lifecycle's rider
   mechanism unchanged; `aborted` is the RESERVED lifecycle member and this is its first writer.
   Then `cell_pending = 0`.
2. **Turn open, no cell pending** (the engine awaits a model completion, or sits between
   continuations). Same rider batch minus `cell.result` — the turn still ends `aborted`.
3. **Nothing open.** Arm the pyrt interrupt flag only (a cell crossing the next start boundary
   cuts — pyrt's idle-boundary refusal, already tested); no store write.

After cases 1–2: `f->pyrt_poisoned = 1`, then `pyrt_interrupt(f->pyrt)` (harmless if the running
cell finishes concurrently). The `reason` text differs by cause — frame-initiated
`"aborted: interrupted at the frame's request"` vs watchdog-initiated
`"aborted: cell exceeded the watchdog deadline"` — two wordings, ONE mechanism (`_frame_interrupt_apply`
takes the text).

**Poison semantics.** An in-memory, frame-owned flag. Every subsequent `FRM_CELL_EXECUTE` on this
frame refuses corr-matched status 1 `"pyrt: runtime poisoned by an interrupted cell"` — the fail-loud
refusal contract, testable without pyrt changes. WHY poison: the interrupted cell's interpreter
thread keeps running its code (cooperative-only reality on the pinned CPython 3.12.13), so its
real `PYRT_RESULT` must never be claimed. The frame keeps `cell_interrupted_corr` and drops a
LATER `PYRT_RESULT` that matches it QUIETLY (an expected shape, distinct from the existing loud
unclaimed-corr error, which stays for genuinely unknown corr). The wedge's cost is documented:
`pyrt_destroy` joins the thread, so destroying a wedged frame waits for the hung cell. Poison does
not survive a process restart — resume builds a fresh runtime; the durable log already carries the
`aborted` turn, and resume-repair stays quiet (the synthesizer made the tail balanced).

**The watchdog.** Since the scheduler has NO timed dispatch (`scheduler.h`: inject is immediate),
the deadline is a real thread, created per pooled cell:

- New `frame_config_t` field `cell_watchdog_ms`; 0 disables; default from
  `SA_FRAME_CELL_WATCHDOG_MS` (300000) in budget.h (§4).
- At `FRM_CELL_EXECUTE` dispatch, when `cfg.pool != NULL`: spawn a watchdog thread that waits on
  a condvar with the deadline. The `PYRT_RESULT` completion sets a done-flag and wakes it — the
  thread exits without posting. At the deadline it posts `FRM_CELL_WATCHDOG` (a sibling of FRM_INT)
  into the frame's own mailbox; its dispatch runs `_frame_interrupt_apply` with the watchdog
  wording. The thread is frame-owned and joined in `frame_destroy` (normally already exited; the
  wait is bounded by the deadline it lives under).
- **Inline driver unification (behavior change, deliberate):** today `_frame_cell_wait`'s deadline
  gives up and returns -1, leaving the cell NOT abandoned (frame.c:2663). The inline driver's
  deadline now invokes `_frame_interrupt_apply` with the watchdog wording first — both waiters run
  one policy. `loop.c`'s `cell-timeout` control wording stays as its surface text.

**What interrupt does NOT touch.** `FRM_STOP` keeps its contract (drain-at-boundary control,
never cuts, never stores); `blocked` stays reserved for the steering slice; nothing about the loop's
exit rules changes.

## 3. `agent.keys(scope)` — the pulled-forward `inspect` member

**Verb shape** (mirrors `recall` exactly): `_py_agent_keys` parses `agent.keys(scope)` (scope is a
string), posts `FRM_KEYS` `{scope, corr}`; the frame's dispatch performs a bounded scan of its OWN
subtree under `state/local/` or `state/ctx/` and answers through the existing bridge reply
machinery with a JSON array of **key names only** — never values (`recall` resolves values; keys
never mixes them).

- Scope is a closed set: `local` / `ctx`. Anything else is the standard fail-loud corr-matched
  refusal (`agent.keys: unknown scope`).
- The scan is bounded by `SA_BUDGET_KEYS_MAX` (256), lexicographic first-N; when truncated the
  array closes with the marker string `"[budget: keys truncated]"`. Own subtree only — a child
  lists its own keys, never the parent's or a sibling's: the no-third-path rule needs no code.
- Same-mode behavior as `recall`: answered from the store actor through the frame's sync slot; a
  store refusal answers corr-matched failure. An empty subtree answers `[]`.

**Deferred, recorded YAGNI unchanged:** events-scan queries, lineage/child listing, path reads —
real sessions will say if `inspect`'s remaining half is wanted (matrix §2 verdict stays 6.5 verbs).

## 4. One budget table per boundary

**New module: `src/Util/budget.{h,c}`** (bottom layer — the boundaries span Frame and Python code),
one header of named caps plus the shared marker function.

| Boundary | Constant | Applied at | Default |
|---|---|---|---|
| Cell result text | `SA_BUDGET_CELL_RESULT_BYTES` | `_pyrt_post_result` (both backends) | 32 KiB |
| Emit payload | `SA_BUDGET_EMIT_BYTES` | `_py_agent_emit` | 16 KiB |
| Bridge values (`remember` value, `spawn` context, `report` text, `keys` reply array) | `SA_BUDGET_BRIDGE_VALUE_BYTES` | `_py_agent_*` call sites | 16 KiB |
| Keys listing entries | `SA_BUDGET_KEYS_MAX` | the `FRM_KEYS` handler | 256 |
| Watchdog deadline | `SA_FRAME_CELL_WATCHDOG_MS` | `frame.c` (default for `cell_watchdog_ms`) | 300 000 |
| Projected message | `SA_BUDGET_LOOP_MSG_CAP` | derive, `loop.c` | 4000 |
| Projected snapshot value / report line / emit line | `SA_BUDGET_LOOP_SNAPSHOT`, `SA_BUDGET_LOOP_REPORT`, `SA_BUDGET_LOOP_EMIT` | derive | 500 / 300 / 300 |
| Transport | `_HTTP_BODY_MAX` / `_HTTP_READ_MAX` | stays in `http_client.c` — cross-referenced in budget.h as the transport boundary; not policy | 64 MiB |

**The marker, one shape, tested once:**

```c
/* In budget.c: truncate a UTF-8 text to cap bytes and append the marker.
   Returns a malloc'd string; NULL = refusal (cap 0, NULL input). */
void budget_truncate_with_marker(const char* text, size_t cap, char** out_text, uint8_t* out_truncated);
```

The marker text is `"\n[budget: truncated at N bytes]"` — never silently silent: the model sees
the head AND knows it was cut, and the durable record carries the marker, so the audit is honest
forever. When `strlen(text) <= cap` the helper copies verbatim with `out_truncated = 0` (no
marker on clean text). Every capped call site uses this helper — no ad-hoc `strncpy` truncation
anywhere.

**Migration:** `loop.c`'s three `#define`s move to the table (`loop.c` includes budget.h; values
unchanged — 4000/500/300). Grep gate: no cap-magnitude `#define` remains in `loop.c`, `pyrt.c`,
`py_agent.c` — every boundary names the table.

## 5. stdout capture — embedded backend parity

The cell helper script in `pyrt.c` wraps execution in a per-cell `sys.stdout` redirect to a
`StringIO`, restored on EVERY exit path (exception included). The result-text rule, matching the
subprocess envelope:

- **Non-empty stdout**: the STDOUT TEXT is the result; when the last expression's repr is not
  `None`, the repr joins as a closing line (`<stdout>…\n=> <repr>`).
- **No stdout**: today's behavior stands — the repr of the last expression (or empty when `None`).
- **Exception**: unchanged today's path — status 1 + traceback; partial stdout rides the failure
  text's head so nothing the model did before the crash is lost.

One subinterpreter note: `sys.stdout` at the subinterpreter level was NOT previously redirected —
the subprocess backend captures its child's pipes; `py_subprocess.c` gains ONLY the source cap
(§4), its result rule untouched.

## 6. Testing

| Suite | Adds |
|---|---|
| `test/test_budget.cpp` (NEW) | the marker helper's shapes (clean copy, exact-boundary, truncation + marker text verbatim, NULL/cap-0 refusal); the table's default constants pinned |
| `test/test_pyrt.cpp` | stdout capture (print-only, print+return `=>` shape, empty print then repr, exception with partial stdout); embedded result source cap on a generated 100 KiB string; interrupt-at-frame-level via `frame_interrupt` (inline): corr-matched synthesis, poison refusal of the next cell, late real result dropped quietly |
| `test/test_py_agent.cpp` | emit durability (record content + seq contiguity in the frame's events); emit and bridge value caps; `agent.keys` per scope (local/ctx/bad-scope refusal/empty); keys truncation marker |
| `test/test_loop.cpp` | POOLED interrupt mid-cell: `[cell.result, step.end, turn.end {aborted}]` one batch, contiguity vs the turn-lifecycle's pinned shapes; the late real result after synthesis; the watchdog fires at a short deadline (real sleep) through the same synthesis path; emit lines in the derive projection; the engine resumes cleanly on a fresh turn after poison refusal |
| grep gate | no cap-magnitude `#define`s left in loop.c/pyrt.c/py_agent.c; `PYRT_EMIT`/`PYRT_LOG`/`PYRT_STATUS` all have explicit frame cases |

Verification: `setarch -R ctest --test-dir cmake-build-debug --output-on-failure` (+ the ASan dir),
valgrind on the stripped test binaries, no-locks grep — the standing bars.

## 7. Out of scope / recorded limits (all deliberate, none hidden)

- The `blocked` reason stays RESERVED (steering slice); no cancel fence / wake latching here.
- No subprocess kill path: the poison contract stands for both backends; a SIGKILL seam is a
  documented candidate for a later slice IF real hangs appear (the poison text records the wedge,
  so its cost is visible in every audit that matters).
- `inspect`'s remaining half (events scan, lineage/child listing, `read(path)`) — YAGNI unchanged.
- Matrix rows 25/29/87 and escalations Q4/Q6 get their LAND updates in the implementation slice's
  docs commit (same pattern as previous slices).