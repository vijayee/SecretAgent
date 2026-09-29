---
# Backpressure, Mute-Park, and the Timer-Actor Stall

Companion to `concurrency-pass.md` (which covers the actor/deque/queue_state
invariants) and `ARCHITECTURE.md` ("Concurrency Model"). This document records
the 2026-09 large-PUT stall incident: what happened, the causal chain, the
three fixes that resolved it, the **contract** of the mute-park backpressure
paradigm going forward, its known caveats, and a playbook for diagnosing the
same class of failure downstream. Read this before adding an actor that emits
high-frequency timer traffic, and before touching `Scheduler/scheduler.c`,
`Timer/timer_actor.c`, or the poll-dancer timer backends.

Date: 2026-09-28. Verified: full `testliboffs` + `testoffs` suites green, and a
1.93 GB `offs put` completing in ~32 s end-to-end (rc 0, ORI returned) after
the fixes.

## 1. What happened

Importing a ~1.9 GB file through the CLI stalled repeatedly and unpredictably:
sometimes a hard spin on one core, sometimes a silent zero-CPU freeze lasting
minutes, sometimes (after the first fix) apparent progress followed by a
freeze. Small PUTs were never affected. The failure needed three contributing
defects acting together, which is why no single fix made the symptom go away
and why it had never surfaced on Linux.

## 2. The causal chain

Three layers, all required for the stall:

```
block save (every block)                    [the LOAD — by design]
  └─ timer_actor_debounce() sends TIMER_DEBOUNCE into the timer actor's
     mailbox; the debounce map coalesces to one OS timer per
     (target, completion_type) key, but each save still costs one
     mailbox message + one stop/re-arm of that timer.

(a) scheduler mute-spin                     [the AMPLIFIER]
    When a target's mailbox is over the pressure threshold, the scheduler
    muted the sender and immediately re-queued it → the sender spun in a
    tight loop, burning a core while making no progress. Every block save
    fed this loop.

(b) timer-actor lock-across-destroy         [the COLLAPSE]
    TIMER_DEBOUNCE dispatch held `loop_lock` across pd_timer_destroy.
    On Windows/IOCP, destroy → iocp_drain_sync() WAITS for the poll-dancer
    loop thread — which needs `loop_lock` in its completion dispatcher to
    make progress. Circular wait: every dispatch that replaced a timer
    paid a 5-second drain timeout instead of microseconds. Drain rate
    collapsed to ~5 s/message → the mailbox became an unbounded backlog →
    sustained muting → (a) spun forever or (b) froze.

(c) IOCP drain-timeout use-after-free       [the RESIDUAL BUG]
    Once (b) was fixed, destroy could return while a queued completion
    still existed whose lpOverlapped IS the pd_timer_t struct. The
    dispatcher dereferences timer->platform_data and td->destroyed on
    that struct. Freeing it on a timed-out drain was a use-after-free.
```

Why Linux never showed this class of bug: the message pattern is identical
(same debounce code runs everywhere), but `epoll_timer_destroy` disarms a
timerfd and closes the fd **synchronously on the caller's thread** — there is
no queued completion and nothing to drain, so destroy never waits on the loop
thread. `loop_lock` was only ever held across a fast operation, the mailbox
drained at full speed, muting never engaged, and (a) never found a sustained
full mailbox to spin on. The bug was Windows-specific by construction, not by
accident of testing.

## 3. What changed

### 3.1 Scheduler: mute-park instead of mute-spin (`src/Scheduler/scheduler.c`)

A muted actor now **parks**: it is taken off the run queue
(`queue_state=IDLE`, `SCHEDULED` cleared) instead of being re-queued to spin.
Wake-up paths:

- `backpressure_release` — when the target's mailbox drains below the
  pressure threshold, it unmutes its senders and re-injects them.
- A MUTED re-check on fall-through covers the lost-wakeup race where release
  lands after the park decision.
- `_scheduler_pool_reinject_stranded` deliberately **skips MUTED|DESTROY**
  actors — it recovers *stranded* actors, and re-injecting a muted one would
  re-park it. Parked-muted actors are only revived from the drain side.
- `actor_destroy` drains the destroyed actor's `pressured_senders`, so
  teardown cannot strand parked senders.

### 3.2 Timer actor: never hold `loop_lock` across a wait (`src/Timer/timer_actor.c`)

All dispatch paths (TIMER_DEBOUNCE, TIMER_DEBOUNCE_FLUSH, TIMER_COMPLETION)
and the synchronous cancel paths now untrack under the lock and perform
`pd_timer_stop`/`pd_timer_destroy`/free **after releasing it**, via the
`_timer_destroy_timer()` helper. The rule is structural: `loop_lock` serializes
pd_timer mutations across threads; it must never be held across anything that
can wait for the loop thread. Teardown (`_timer_actor_destroy_all_tracked`)
swaps the tracked array out under the lock and destroys outside it, and
destroys tracked timers *before* joining the loop thread so drains stay fast.

### 3.3 poll-dancer: drain-safe timer destroy (`deps/poll-dancer`)

The platform `timer_destroy` op now returns an int contract:

- **1** — no loop thread can still reach this timer; `pd_timer_destroy` may
  free the struct (epoll and kqueue always return 1: timers dispatch
  synchronously off the fd/kevent, no queued completion outlives destroy).
- **0** — a queued completion may still be pending (Windows/IOCP drain timed
  out); `pd_timer_destroy` skips the `free()` and the struct plus its
  `platform_data` **leak deliberately**. A leak is recoverable; a
  use-after-free is not. The dispatcher reads `td->destroyed` and skips the
  dead timer either way.

The non-CLI parts of the incident (flags-before-file parsing, 1 MiB PUT_DATA
chunks under the 2 MB wire-framer cap, a real `errno` instead of "No error")
are OFFS-side fixes in `offs` `src/offs/commands/put.c` and
`src/offs/client.c`; see that repo for their documentation.

## 4. The mute-park contract

For anyone writing or reviewing actor code, the paradigm is:

- **Muting is sender-side only.** A full target mailbox mutes *senders* into
  that target; the target itself never stops running because of its own
  mailbox. A slow consumer therefore always keeps draining, and parked
  producers are revived from the drain side. This is the self-healing
  direction, and the one every current topology uses.
- **Parked ≠ lost.** Park is cheap (no queue, no CPU). Revival is
  event-driven (release) or scan-driven (for non-muted stranded actors).
- **Mute state is checked, not trusted.** Dispatches re-check MUTED to cover
  lost-wakeup races; nothing assumes a muted actor saw its own wake-up.

### 4.1 Caveat (open, escalated): the mutual-mute cycle

If A sends to B and B sends to A and **both** mailboxes are over the
pressure threshold simultaneously, A parks (muted by B) and B parks (muted by
A). Neither drains, neither unmutes the other — a permanent whole-program
stall. No current guard breaks this: the stranded scan skips MUTED actors,
and `backpressure_release` only fires from a draining side, which is frozen.
This is theoretical today (no current actor pair is heavily bidirectional),
but any future actor that both sends to and receives heavily from another
actor has the exposure. Candidate guards, not yet chosen: a bounded park
timeout with message deferral; cycle detection in the stranded scan; or a
directional rule that never mutes a sender whose target is itself muted.
Pick by trade-off (degradation vs polling cost) before shipping a workload
with heavy bidirectional actor traffic.

### 4.2 Other caveats to know

- **The debounce map has 16 slots** (`MAX_DEBOUNCE_KEYS`). Saturation is
  silent. Debounce traffic is coalesced per `(target, completion_type)` pair,
  which is why a flood of block saves produces thousands of *messages* but
  only a handful of *timers*.
- **The flood itself is not eliminated.** Every block save still costs one
  mailbox message plus one timer re-arm. Mute-park makes the churn cheap and
  non-deadlocking; it does not make the timer actor a wide pipe. If a
  workload needs that, the structural answer is coalescing saves or a
  timing-wheel/min-heap timer actor — a durability-adjacent design change.
- **Leak-on-drain-timeout is deliberate** (poll-dancer). Do not "fix" the
  leaked timer struct by freeing it earlier.

## 5. Downstream playbook

Symptoms of this class, and what they mean:

| Symptom | Likely layer | Confirm by |
|---|---|---|
| One core pegged, no progress | sender-side mute **spin** (pre-3.1 code) or a run-queue loop | sampling profiler shows scheduler re-queue path |
| Zero CPU, silent freeze | **park** holding, or lock-across-destroy drain | full-stack dump: a parked actor whose muted-sender release never fired, or a thread in `iocp_drain_sync` |
| Progress at ~5 s/message steps | timer dispatch paying drain timeouts | daemon logs show long gaps between timer completions |
| Crash / corruption after a stall resolved | IOCP drain-timeout UAF (pre-3.3 code) | use-after-free on a `pd_timer_t` in the loop dispatcher |

Rules for code that touches these paths:

1. **Never hold `loop_lock` (or any timer-actor lock) across a call that can
   wait for the poll-dancer loop thread.** The IOCP destroy path drains and
   waits. Untrack under the lock; stop/destroy/free outside it.
2. **Never free a `pd_timer_t` that the IOCP loop may still reference.** Use
   the `timer_destroy` return contract (3.3) instead of reasoning about drain
   success yourself.
3. **When adding an actor that emits high-frequency timers or debounce
   traffic**, expect mailbox pressure by design. Make sure the consumer actor
   is a pure drain (never blocked on a resource the senders hold), so the
   mute-park cycle stays one-directional. Avoid heavy bidirectional message
   pairs between two actors (caveat 4.1) until a cycle guard exists.
4. **When adding debounce keys**, remember the 16-slot map cap; a silent
   saturation manifests as debounce timers that appear to never fire.
5. **Do not raise the mute/park thresholds to "fix" a stall** — the stall
   under this paradigm is a drain-side problem (a blocked consumer or a
   lock-across-wait), and raising thresholds converts it into memory growth
   and mute-spin.

## 6. Related records

- `docs/concurrency-pass.md` — actor/deque/queue_state invariants (F4/F5
  cover adjacent teardown races this incident's fixes do not change).
- poll-dancer `src/internal/platform.h` (`timer_destroy` contract comment)
  and `src/platform/iocp.c` (`iocp_timer_destroy`) — the leak-vs-UAF trade.
- `timer_actor.h` `timer_actor_cancel_target()` — the pointer-value-safe
  teardown path for actors that hold debounce keys.