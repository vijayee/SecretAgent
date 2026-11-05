# Incorporating the Onyx Study into the Defaults — Design

**Date:** 2026-11-05
**Status:** owner-agreed shape settled in session (all five findings PLUS the turn-limit knob as
the sixth; the limit trip = an ASK, never a kill; default unbounded). No open questions.
**Source:** docs/onyx-voice-and-reasoning-study.md (committed 9bded99) — Part 3's "worth taking"
list + the six-harness budget survey (DSH / PA / CC / opencode / Onyx; the four coding agents
default UNBOUNDED — the unanimous answer, our default follows).
**Atlas slice:** the defaults node (L3/L5).

## Why this slice exists

SecretAgent's DEFAULT frame (persona-less, free-mode) speaks in machinery: SA_LOOP_INSTRUCTION
(loop.c:201, rides every turn of every frame) says what `execute` does and nothing about how to
think; its verb list omits `agent.ask`/`actor.keys`/`actor.emit` entirely (the model discovers them
only by probing); no reminder channel exists; budget cuts say the cut's facts but never the honest
uncertainty. The Onyx study shows what a mature harness carries in those seams — and the budget
survey (six harnesses) settles the stopping doctrine: the loop defaults UNBOUNDED; a limit, when
the owner sets one, trips into an ASK. The loop (L3) stays amoral throughout: everything below is
constants + one config field + the trip's ask machinery (which the escalation slice landed).

## Decisions carried in (settled with the owner, 2026-11-05)

| Decision | Value | Authority |
|---|---|---|
| Scope | **All five study findings + the turn-limit knob as the sixth** | owner |
| The stopping default | **Unbounded** (`max_turns = 0`) — the four coding agents' unanimous stance; depth never gimped | owner |
| The limit trip | **An ASK, never a kill**: "the AI thinks it can go deeper → it asks the user if it should" — the trip closes the turn with a control record, authors a Continue/Stop ask, parks; Continue = a durable consent control + a fresh budget; Stop = `turn.end{turn-limit}` (the reason's first writer), resumable | owner |

## 1. The instruction (one constant's new shape)

`SA_LOOP_INSTRUCTION` (loop.c:201, byte-stable, the cache-stable prefix's rule) carries five
additions — the full composed text is spec §1.1's pin (the implementer writes it from the pin):

- **The voice floor** (for persona-less frames; a loaded persona raises the ceiling): deeply
  understand the user's intent before answering; think step-by-step; offer the follow-up that will
  be needed next.
- **The completed verb list**: `actor.ask(question, options=[])` — ask only when ambiguity is real
  or a decision is the owner's; ONE sharp open question, or ≤5 options; say what the answer
  unblocks (the ask craft from the study §1.5); plus `actor.keys(scope)` + `actor.emit(text)`
  named (they exist but never rode the instruction).
- **The two-register rule** (study §2.4): child reports face you as the parent agent — dense,
  unformatted; your replies to the user stay conversational and direct.
- **The truncation honesty line** (the `forgotten-files` doctrine minus the unreadable-tool case):
  content cut at `[budget: truncated at N bytes]` is not guessable — reconstruct from state
  (`actor.recall`/`actor.keys`) or ask; never invent the middle.

### 1.1 THE BYTE PIN (the implementer's text source)

```text
You drive one frame of an agent session. Your only tool is `execute`: its
`code` argument is ONE python cell run in this frame's interpreter (one
shared namespace per frame). Inside cells the injected `actor` module
provides the verbs: actor.remember(key, value), actor.recall(key),
actor.keys(scope), actor.emit(text), actor.spawn(goal, context=None),
actor.report(value), and actor.ask(question, options=[]).
Ask the user only when ambiguity is real or a decision is the owner's:
one sharp open question, or five options at most, and say what the
answer unblocks.
Child reports face you as the parent agent — dense and unformatted.
Your own replies to the user stay conversational and direct.
Deeply understand the user's intent before answering; think step-by-step;
offer the follow-up the user will need next.
Content cut at "[budget: truncated at <n> bytes]" is not guessable:
reconstruct it from the frame's state (actor.recall/actor.keys) or ask —
never invent the middle.
Finish the frame by calling actor.report inside your last cell (a
completion declaration), or simply by answering WITHOUT a tool call.
```

(The tail's completion-declaration sentence rides byte-identical to today's. The ask/keys/emit
ENTRIES join the list; the craft + the registers + the floor + the honesty ride as shown.)

## 2. The reminder block

A SECOND stable constant (`SA_LOOP_REMINDER_BLOCK` — the same section, beside the instruction) in
the derive's system prompt, AFTER the instruction block (the stable prefix keeps one composition,
one order — the exact placement pinned at §2.1):

### 2.1 THE BYTE PIN

```text
<system-reminder>
These tags are added by the system, never the user. Behave in accordance
with relevance and continue normally otherwise.
</system-reminder>
<system-reminder>
Address every part of the goal explicitly; values, names, and steps the
frame's context carries ride verbatim rather than summarized away. When
the context does not contain what is needed, say so directly rather than
answering with adjacent information.
</system-reminder>
```

(The contract reminder = Onyx's `prompts/constants.py:8-11` line adapted; the rider = the
completeness reminder (`chat_prompts.py:64-66`) minus the documents-specific sentence — our
context is the frame's derive, not a document pack. The block rides in the system prompt — a
NEVER-CHANGING reminder belongs in the stable prefix, not a per-turn message; a future CHANGEABLE
reminder gets the per-turn back-slot then, not this slice.)

## 3. The turn-limit knob (`max_turns`, finding six)

`frame.h`: `unsigned max_turns;  /* 0 = unlimited (default) */` on frame_config_t + the FOURTH
three-way-inheritance copy site (the escalation_mode pattern; per agent like turn caps).
The guard lives at the TURN BOUNDARY (the turn-entry check, the doom trip's seam class — the
config read is the guard, the loop stays amoral):

- `e->turn_counter >= cfg->max_turns` (when max_turns > 0) at the NEXT turn's entry. THE TRIP'S
  THREE BATCHES (the order pinned):
  1. **The trip batch** (ONE atomic close): the turn's closing riders + `control {kind:
     "turn-limit", text:"budget N reached"}` + the `EV_ASK` event + `turn.end{reason blocked}` —
     an unanswered ask must NOT have closed the run durably: the lifecycle truth at trip is
     BLOCKED (the ask is pending); `turn-limit` has NOT written yet. NO kill, NO terminate.
  2. **The parked ask** — runtime-authored: `{question: "Turn budget (N) reached. Continue or
     stop?", options: ["Continue", "Stop"], plan: <a bounded last-turn summary — the frame's
     newest assistant text, budget-capped>}` via `e->pending_ask {ask_id, corr=0, limit_gate=1}`
     + `FRAME_PHASE_ASK` + the pyrt publish flag — the plan gate's mechanism verbatim (the
     `plan_gate`/`limit_gate` fields distinguish; a frame can trip both across its life, never
     simultaneously). The owner answers (or the wake latches supersede/interrupt — unchanged).
  3. **The answer's batch** (the reply machinery's [ask.reply + msg.append] batch):
     **Continue** adds the FOURTH record `control {kind:"limit-continued", count:k}` (k = the
     continuation ordinal, durable consent) and the engine resets the consumed count — a fresh N
     turns; **Stop** adds `turn.end{LIFE_REASON_TURN_LIMIT}` as the FOURTH record — **the
     reason's first writer** (the KNOWN list already accepts it) — engine ends, frame resumable.
- **Recovery consult**: the derive's scan recognizes `limit-continued` records → the consumed
  count's reset restores in-memory after restart (the approval-consult's sibling; window-scroll =
  an extra ask, never a silent skip — the recorded safe default).

## 4. Pins and the honest cost

- Every derive byte-identity pin retargets: the instruction + the reminder block are now part of
  the expected bytes (the persona slice's TestNoPersona* shape pins, TestFreeMode* pins, the loop
  suite's captured-request byte pins). NOTHING is deleted; the standing suite stays green with
  new expected bytes.
- The hammer's record: BYTE-UNTOUCHED (its tests pin the record; the derive's composition around
  it grows).
- `model-decode`'s positional inits bump AGAIN (the 5th field insertion — `max_turns` lands where
  frame_config_t field order puts it; the 11 inits re-bump).
- Matrix close-out: row 4's turn union + row 7's max-tokens note + row 11's moving-blocks/budget
  design + row 34/35's reminder convention + row 37's ask craft gain the incorporation notes; a
  new §5 escalation-craft line.

## 5. Tests (the falsifiable set)

- **`test/test_loop.cpp`**: the instruction's + the block's composed bytes (the byte pins); the
  verb-list entries present; ask-craft/registers/floor/honesty lines present (byte pins — the
  prompt capture).
- **`test/test_frame.cpp`**: `max_turns` inheritance (the escalation-mode test's shape); the trip:
  a frame with max_turns=2 + scripted turns → trip → control{turn-limit} + the ask + blocked;
  Continue → limit-continued(count 1) → two fresh turns → trip again → ask again; Stop →
  turn.end{turn-limit} first-writer + engine end + resumable; the parked wake branches work on a
  limit ask; recovery across restart (limit-continued in the log → the count restores); unlimited
  default = zero trips.
- **`test/test_lifecycle.cpp`**: the turn-limit reason's composer + the repair rule (a
  control{turn-limit}-only tail with no turn.end = OPEN — repair closers compose with... the trip
  tail ends turn.end{blocked} so the balance holds; pin the blocked balance again over the limit
  ask's shape).
- **`test/test_persona.cpp`**: the hammer's compose untouched (the standing byte pins green) — the
  reminder block rides AFTER the persona group (below the persona, before the instruction's base
  text — the one ORDER pinned).
- Gates: `setarch -R ctest` ON+ASan + OFF + off-verify + valgrind (the standing bar).

## 6. Non-goals (recorded)

- Per-turn CHANGEABLE reminders (the back-slot mechanism) — the channel is named; the moving half
  of row 11 is a later slice.
- Citation instructions (no quoted-doc surface exists).
- The feasibility anti-refusal counter (no planner).
- max_tokens-as-turn-reason stickiness (row 7's residual — no writer).
- The auto-refine/auto-limit interactions (a later seam).