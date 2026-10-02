# Guards — Doom-Loop Breaker + Runtime-Owned Retry Tables — Design

**Date:** 2026-10-02
**Status:** owner-agreed shape settled in session (five decisions; approach A: a pure guards
module + two thin loop integration points + a one-shot delayed-post timer). No open questions —
every detail is pinned.
**Atlas slice:** a new node (delivery; L3) — evidence: `test/test_guards.cpp` (new) + extended
`test/test_loop.cpp` + `test/test_model_decode.cpp`.
**Parity sources:** the opencode section of `docs/architecture-research.html` (2026-10-02
addition): the doom-loop breaker (processor.ts:29, :356-379), cause-specific retry policy
(retry.ts:30-88), and the loss-free-truncation/artifact + blocked-ask essences are RECORDED for
later slices, not this one. This is the engine-side guard slice between surface completion and
the desktop/persona layers.
**The reason union:** `turn.end` gains `doom-loop` as its union's new member (the
`blocked` reason stays RESERVED for the steering slice — untouched).

## Why this slice exists

The engine has two unguarded failure classes a unit test can hit in minutes:

1. **The doom loop:** a model that repeats one `execute` cell verbatim forever — the loop never
   trips; only the turn cap eventually stops it (64 turns of identical bytes). opencode's
   breaker (3 identical consecutive tool calls → interrupt the pattern) is the named runtime
   guard for exactly this; the doc's steal-line mandates the transposition: an **event to the
   runtime's controlling policy, never an ask that can auto-approve itself**.
2. **Cause-blind retrying:** model errors retry EXACTLY once with no backoff
   (`model_retries < 1` → fresh `FRM_TURN` repost, loop.c's `_frame_engine_reply`) — a transport
   failure and a non-retryable overflow get the same one repost, a 429 gets no pacing, and the
   Ollama `load`-stop (finish_reason on an overloaded provider — the flagged follow-up from the
   frame-tree slice: "a zero-token load-stop currently maps to empty-turn/done — fail loud")
   has NO policy at all. opencode owns retry cause-specifically at the runtime: pattern-matched
   retryability, retry-after-aware backoff, explicit non-retryable classes as status events.

## Decisions carried in (settled with the owner, 2026-10-02)

| Decision | Value | Authority |
|---|---|---|
| Doom-loop response | **Refuse cell + end run**: the tripped call never runs; `turn.end {doom-loop}` + the engine ends failed; the frame stays resumable | owner |
| Retry depth | **Table + timed backoff**: a cause table with per-class caps and a bounded timed repost (actors never block — the timer is a thread, the pattern the cell watchdog establishes) | owner |
| Doom identity | **Byte-identical cell code; reset on input**: any different cell OR any user message landing between resets the streak | owner |
| No config knobs | constants only, like the budget table (threshold 3; the caps/backoffs in the table) | owner, per the option's wording |
| Out of scope | loss-free truncation artifacts (budget v2, a later increment); the blocked-ask protocol + the escalation ladder (L5); the desktop/client surface (row 44); Windows | owner |
| Architecture | approach A: `src/Frame/guards.{h,c}` (pure) + the loop's two integration points + `frame.c`'s delayed-post timer. Rejected alternates: retry-inside-model.c (the retry budget is the loop's correctness domain; model.c is the documented lock-exception file — don't grow it) and all-in-loop.c (the streak/table logic becomes untestable in isolation — lifecycle's pure-module pattern exists for this) | owner |

## 1. The doom-loop breaker

**State.** The engine state gains: the last dispatched cell's code (heap copy, freed/replaced on
every tool path) and `doom_streak`. Constants in the guards module: `SA_GUARDS_DOOM_THRESHOLD`
(3).

**The fold (pure, in guards.c).** `guards_doom_fold(streak, incoming_code, last_code,
last_user_seq, this_derive_max_user_seq)` — callers pass WHAT they saw; the fold answers the
streak's next value and whether the threshold is now crossed:

- identical (strcmp == 0): streak + 1;
- different: streak = 1, the copy is replaced;
- **reset on input:** any NEW user-role `msg.append` seq observed in this turn's derive (the
  derive's Pass A already walks every record — the engine tracks the newest user msg.append seq
  it has seen) resets the streak to 0 BEFORE the identity check — a model re-deriving from new
  input is not looping.

**The trip — refused BEFORE the audit.** When the incoming call is the threshold-th identical:
the cell NEVER runs, and no `cell.run` audit is written (nothing pointless is ever audited).
The close is ONE fire-and-post batch: control `"doom-loop"` with the model-visible text
(`"doom-loop guard: the last 3 cells were identical; the turn is refused — vary the approach"`)
+ `turn.end {reason: doom-loop}`. The envelope stays balanced NATURALLY: the step starts only
with the cell.run audit that never happened, so the turn's shape is
`turn.start … turn.end{doom-loop}` with no step records — the "opened but stepped nothing"
shape the turn-limit refusal's pre-entry refusal already uses. The engine ends failed
(`_frame_engine_terminate`, the standing failure surface); the frame stays resumable and
resume-repair sees a balanced tail.

**Never an ask.** No ask surface, no self-approval (the doc's steal-line, kept whole).

## 2. The retry table

**The classification chain.**

1. **`src/Streams/http_client.c`**: the async client's parser gains the two header callbacks
   (today NULL) capturing response headers into an `http_headers_t` ON THE COMPLETION RECORD —
   the PORTED header module (src/Streams/http_headers.{h,c}) doing its normal job, bounded at
   16 entries. NOT a Retry-After-only probe: the desktop slice (row 44) wants client-side
   headers for SSE/auth later; this slice's table reads
   `http_headers_get(&completion->headers, "retry-after")`. Header value bytes beyond a
   bounded length truncate at the probe line (the transport cap discipline).
2. **`model.c`**: the response sink's signature gains ONE additive parameter
   (`unsigned retry_after_msec`, 0 = absent) — every sink implementation updated, plus the
   async test backend; `_model_result_from_http(status, body, ...)` gains the same param and
   records the http status so the ERROR TEXT carries the class's facts (the standing
   loud-wording rule). The Ollama `load`-stop becomes the OVERLOAD class — non-retryable, fail
   loud (`"model-error: provider overload"`) — closing the frame-tree slice's flagged follow-up.
3. **`src/Frame/guards.c`**: the table, as pure data (constants; no config knobs):

| Class | Match | Retryable | Cap | Backoff |
|---|---|---|---|---|
| `transport` | http status -1 | yes | 5 | 0/250/500/1000/1000 ms |
| `server` | 5xx | yes | 5 | the same progression |
| `rate` | 429 | yes | 5 | Retry-After when present (capped 5 s), else the table's |
| `overload` | the `load` shape | no | — | non-retryable → fail loud |
| `overflow` | provider context-overflow | no | — | fail loud |
| `fallback` | anything else | yes | 1 (today's rule) | 0 ms |

The cap column counts RETRIES AFTER the original attempt (cap 5 = up to 6 calls; the
fallback's 1 = today's single-retry rule). The retry budget resets per successful call
(today's `model_retries` discipline, generalized);
a retry is still ONE fresh `FRM_TURN` repost (the re-derive-equivalence argument unchanged;
the turn is never renumbered); a NON-retryable class (or an exhausted cap) ends the engine
failed through the standing control wordings, with the class named in the text
(`"model-error-final: <class>: <detail>"`).

## 3. The delayed-post timer

`frame.c` gains a ONE-SHOT delayed-post primitive in the cell-watchdog thread pattern, but
SIMPLER by design (never a refactor of the hardened watchdog): a small struct + one short-lived
thread per pending backoff — the thread sleeps the (bounded ≥ 1 ms) delay, posts the request
into the frame's OWN mailbox, and quits. NO disarm protocol: the retry continuation arriving on
an ended engine is the existing loud no-op; frame_destroy with a timer in flight joins it
(the timer's delay is bounded — the table's max, 5 s). The failure backoff and the cell
watchdog share the PATTERN but keep separate, purpose-built mechanisms: the watchdog's protocol
was hardened for its destroy-race; the timer's post is always safe or a loud refusal.

## 4. Testing

| Suite | Adds |
|---|---|
| `test/test_guards.cpp` (NEW, pure — the lifecycle pattern) | the doom fold: identical/different/reset-on-input/threshold-crossing shapes; the retry table: every class's match + cap + backoff lookup + the Retry-After cap + the fallback's once-only shape |
| `test/test_model_decode.cpp` | the decode's status/fact-carrying error surface + the retry-after param |
| `test/test_streams_client.cpp` | the client's header capture (a server that answers headers → the completion record's `http_headers_t` carries them; bounded at 16; the Retry-After field reachable) |
| `test/test_loop.cpp` | three identical execute cells → the refusal close (turn.end{doom-loop}, NO third cell.run, no leftover step records); a steering message between identical cells RESETS (no trip); different cells don't trip; a scripted 4× 5xx failure → the reposts arrive through the timed path; a `load` failure → immediate fail loud with the overload wording |

Verification bars as always: `setarch -R` ON + ASan suites, valgrind on stripped copies
(`strip --strip-debug` keeps the suppression frames matchable), the no-locks grep
(the timer's mutex pair joins the watchdog quartet's documented exception block).

## 5. Out of scope / recorded limits (deliberate)

- No loss-free truncation artifacts (the budget cuts stay cut; opencode's artifact-with-retention
  generalization is a recorded later increment on the budget table).
- No blocked-ask/permission surface (L5); the breaker never asks.
- No desktop/REST routes (row 44); the client's header CAPTURE is its prerequisite, not its
  deliverable.
- The `blocked` reason stays reserved (steering slice). No config knobs land — constants only.