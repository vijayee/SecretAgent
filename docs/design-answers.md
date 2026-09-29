# Design Answers — extracted from docs/chat.json

Every substantive question raised in the founding conversation (the Onyx export,
"Frankenstein AI Agent"), with its settled answer. Code has been stripped per
instruction; `[msg NNNN]` = message id in chat.json. Where a later message
corrected an earlier answer, the **superseding answer** is stated and marked.

## 1. The project framing

**Q: Do we need to "jailbreak" the assistant out of the web UI?** [msg 4146]
No. The web interface is just a client. The "personality" you want to keep is
text — response instructions about reading intent and treating you well — and
text is portable. The one thing genuinely caged is *tool access*: "you don't want
to free me from the web interface; you want to give me hands." [msgs 4146, 4188]

**Q: What is each of the four harnesses, and what do we take from it?** [msg 4146]
- **Onyx** — persona + context grounding ("who speaks, what it knows"). Its
  personality is not in the codebase; it is the response instructions.
- **DeepSeek Harness (DSH)** — the diligence spine: event-sourced loop, "single
  source of truth" log, guarded tool pipeline (pre → execute → post → result).
- **Claude Code** — clarity + escalation: plan → ask → act; readable decisions.
  Proprietary, but the *pattern* is reimplementable (and Victor holds the source).
- **Prime Agent (PA)** — efficiency: persistent state, scoped subagent recursion,
  memory/skills CRUD, compaction (/refine).
These don't conflict, they compose: Onyx decides *who* speaks, DSH decides *how
the loop runs*, Claude Code decides *how it communicates and escalates*, PA
decides *how it remembers and gets faster*.

**Q: What was wrong with the DeepSeek harness ("rigorous but flat")?** [msg 4146]
Nothing in the loop encodes "read the person." Rigor about the task produces a
relationally flat surface. Fix: the loop stays amoral; personality is a
first-class, checkable text layer riding on top (L5), not a change to the loop.

## 2. Language and portability decisions

**Q: Why C? Performance?** [msgs 4149-4153]
Not performance — portability. A solid C library embeds into any language or
environment. C also *forces* the boundary: the loop can't accidentally smear the
voice (text) into compiled code. C is right for parts that never change (event
bus, loop); wrong for parts iterated weekly (persona, escalation rules) — those
are loadable text.

**Q: Can a DSH-style log and a Prime-Agent-style tree be one data structure?**
[msgs 4177-4178, 4180]
Yes — but "one" means one *log*, with the tree as a **derived projection** over
lineage pointers, exactly like DSH's `deriveMessages()` message projection and
surface projection. DSH itself deferred the tree ("pi-style entry tree — deferred
unless needed"); the project completes that deferred tree as a projection.
[Correction, msg 4192/4184:] with WaveDB as substrate, sessions live as
**subtrees** — the tree is containment + lineage, not a copy nor an MVCC-fork.

**Q: Is using a database (WaveDB) for everything over-engineering?** [msgs 4179-4191]
No. The access patterns split cleanly: the log's dominant operations are append +
sequential scan (WaveDB stream scan handles them), fork is a range scan /
subtree, compaction is a rewrite, and arbitrary lineage queries are exactly what
DSC's graph layer does. "Log is truth, views derive" is the pattern both DSH and
Pondr already use. The database is not a hammer shaped like a nail here — the
primitives line up.

## 3. The actor merge (DSH × Prime Agent)

**Q: Whose loop is the outer shell — Prime Agent dispatching DSH-style
subagents, or the reverse?** [msg 4176]
They are duals: Prime Agent = tree outer, loop inner (truth in running kernels,
silent); DSH = loop outer, tree emergent from the stream (truth in the log).
**Flip Prime Agent's orientation**: DSH's event loop is the outer shell; Prime
Agent's recursion lives inside it as the state model. One hard requirement falls
out: every state mutation must be expressible as an event, or replay breaks.

**Q: Does spawning a subagent spawn "a DSH-like subset," or is DSH the root
agent?** [msg 4178]
Neither. In DSH a subagent is a **full sibling session** with its own complete
loop, seeded with a one-time snapshot of the parent's completed turns, linked by
lineage metadata — not nestable frames. The tree in DSH is a derived projection,
not structure.

**Q: Does a child share live context (Prime Agent lexical scoping) or a frozen
snapshot (DSH fork-seed)?** [msgs 4178, 4191-4196]
Settled in three steps:
1. Snapshot fork (DSH-style): copy parent's events at fork. Simple, safe.
   [msg 4178]
2. HTML/CSS correction: inheritance can be live, *typed* (ctx vs local), live
   without copies, resolved by a cascade (shadow wins), with unset/revert
   controls; shadow DOM = report-not-transcript. [msg 4194]
3. [Final, msg 4196] **Neither copy nor cascade-walk is needed — the child gets
   an API over the parent's subtree.** The parent decides, in code, what the
   child can ask: the API projects only what's needed ("obscure what is
   needed"). Encapsulation is enforced by an interface boundary, not data
   layout. The `surface` (vtable of closures) becomes the most important field
   of the frame.
This also structurally fixes Prime Agent's real bug (#819: children blind to
parent memory because a hardcoded 6-entry alphabetical head slice + local-first
default hid global memories).

## 4. The interpreter question

**Q: What is an IPython kernel, actually?** [msgs 4156, 4160]
Not an embedded interpreter — a long-lived out-of-process daemon speaking the
Jupyter protocol (shell/iopub/control/stdin/heartbeat) over ZMQ. Its namespace
survives because the process never exits. The evaluator is incidental; the
value is a persistent namespace + process isolation + interrupt channel.

**Q: Could any VM/interpreter serve the same role? Is the language the essence?**
[msgs 4160-4168]
Any language works *in principle*; that is itself the proof the VM is not the
essence. The essence is a **persistent, scoped, isolatable namespace (the heap)**.
[Correction msgs 4165-4168:] "a model must be trained on the environment" was
overstated — untrained models are demonstrably effective (the harness works
today with Ollama). The split that matters: **code-generation fluency** (training
data-sensitive; Python has an edge) vs. **tool use** (a transferable
meta-capability). Language barely matters for solving; what matters is the
*feedback loop* (tight error signal) and *package ecosystems* (PyPI's library
economy, not the syntax). [Late finding, msg 4172:] Prime Agent itself already
ripped out IPython/ZMQ — it converged to a minimal CPython REPL over stdio,
confirming "just an interpreter."

**Q: What does Prime Agent's Python execution actually look like (four axes)?**
[msg 4206]
1. Granularity = a "cell": a block of Python per tool call.
2. State = a live Python namespace persisting across cells (durability via
   best-effort `dill` snapshot; not cleanly serializable — its weakness).
3. Concurrency = sequential cells, top-level await, background tasks between
   turns; a blocked cell holds the turn.
4. Boundary = `host_request`: Python is a sandbox for *expressing intent*;
   authoritative state (goals, messages, compaction, child lifecycle) stays in
   the host.

**Q: Where does the interpreter belong in a C core — embed, subprocess, kernel
daemon?** [msgs 4204-4206]
Embed route (chosen [msg 4205]): `libpython` in-process on **CPython 3.12+
subinterpreters with per-interpreter GIL (PEP 684)** — one subinterpreter per
actor, so the tree-of-kernels scales without Prime Agent's #294 boot-starvation
(process-per-agent cap). Key resolution: **the live namespace is for *thinking*;
WaveDB is for *remembering*** — no dill-style namespace ownership. The
interpreter is compute; inputs/outputs are events in the store.

**Q: Is the interpreter needed at all, and how can that be tested?** [msgs 4168-4170]
It supplies exactly three unique gifts: (1) unbounded composition vs fixed-arity
tools, (2) computable state ("load once, slice across turns"), (3) package
ecosystem reach. Everything else (network, subprocess, FS) is a side effect any
tool layer handles better. Prime Agent never ran the ablation (interpreter +
scoping + recursion all changed at once), so its leap was a belief, not an
empirical result. Test spec: same model/tasks/scaffold, toggle only the
interpreter arm (A) vs tool registry (B) vs hybrid (C); measure the *crossover
curve* — task type where computation outruns a registry.
**Victor's override [msg 4173]:** no sandbox, no allow-listing — "I trust the
model like I trust my own code," same as every harness he already runs. The
safety story is the **event log** (replay + audit), not a gate. Defer gating
until a multi-user/server threat model exists (the host owns the side-effect
path, so a gate is a one-file change later).

**Q: What does Prime Agent do about package/security problems?** [msg 4172]
Nothing, by design: docs state "not a security sandbox," packages run with full
system access, skills auto-install Python packages into the kernel venv. The only
network-isolated piece is Prime Intellect's *cloud* `prime sandbox` product —
external, not built into the local harness. (Victor declines to build this
containment anyway — see the override above.)

## 5. The actor model (the concurrency layer)

**Q: What is the right concurrency model?** [msg 4207-4210]
Pony's actor model, minus what C can't take: *keep* isolated actors + async
causal message passing; *drop* the reference-capability type system (a
compile-time guarantee — replaced by one runtime rule: only **immutable
data/objects** (transferred with the message) or **references to objects that
manage their own concurrent state** — i.e., actors — cross a boundary, and
atomics may be used freely where safe) and *drop* ORCA GC (frames have
explicit spawn→report→join lifecycles; WaveDB is persistent; CPython's own GC
handles subinterpreters). Pony's ORCA GC was a win to *not* need: lifecycle +
durable store replace it.

**Q: Can one process hold many Python runtimes — one per subagent?** [msg 4208]
Yes: CPython 3.12 subinterpreters with per-interpreter GIL (`Py_NewInterpreterFromConfig`,
thread-state swap). One process, N threads, each thread a subinterpreter = "one
actor per subagent," avoiding the #294 cost of booting 100 kernel *processes*.

**Q: Are actor messages durable data to be stored?** [Final, msgs 4211-4212]
No. **Messages are verbs, not nouns** — async function calls that instantiate a
behavior and evaporate. The durable truth splits three ways: *messages* (transient
control, in-memory), *effects* (the writes/consequences — durable in WaveDB's
WAL), *transcript* (model turns — durable). No message store is built; the WAL
already is the event log. [Supersedes msg 4208's "message backbone is WaveDB."]

**Q: Does the actor block on Python?** [msgs 4208, 4214]
Never. The actor's message loop and the Python execution loop are two threads
with a message handoff: EXECUTE is enqueued and the actor returns; the python
thread boots its subinterpreter lazily (only on first EXECUTE — frames that
never run code pay nothing), runs, and posts RESULT back. Preemption and
interrupt become messages, which is the Pony-correct place to put them.

**Q: How does Python communicate *out* (fixing Prime Agent's black box)?**
[msg 4216]
An injected module whose methods are C callbacks posting messages to the
owning actor's inbox: `log`/`status` (ephemeral narration, streams up) vs
`emit`/`report` (durable effects). Callback rules: non-blocking (may take the
inbox mutex only), never re-enter Python, ownership of strings transfers
(strdup out of the Python heap). Routing/persistence stays in C behaviors.

## 6. Memory architecture

**Q: With WaveDB attached, does the agent already "have memory" — good or bad?**
[msg 4220]
It has **episodic** memory (this run: events, frames, replayable trace). That is
good precisely because it frees Pondr to be what it always meant to be: the
**semantic** store (consolidated, cross-session, associative — the "artificial
subconscious"). Working memory = context window + live namespace; episodic =
WaveDB; semantic = Pondr. Danger to guard: *querying raw history is not
consolidation* — a durable store answers "what did I do," never "what do I know
without re-reading everything." Consolidation runs in the background, never on
the access path (GeoSTM doctrine).

**Q: Do Pondr and the agent merge into one project?** [msg 4218]
No — three layers: Pondr (application) → agent runtime (library) → WaveDB
(substrate). Dependency arrows strictly one-way (`pondr → agent-runtime →
wavedb`); the agent never includes Pondr. Integration is a **schema** (both are
`sessions/<id>/events/<seq>` over the same store), not a codebase merge. Pondr
is the library's *first client*, not its home.

## 7. The agent model (L3/L4/L5)

**Q: Is this a "coding agent"? Where is the coding/non-coding line?** [msgs 4199-4202]
The line is not in the model — it is (1) the tool surface and (2) the feedback
loop (can it *see the consequence* of its acts). The architecture erases it:
every agent is born with the full surface and **specialization is subtraction**:
`read, inspect, write, execute, spawn, report, remember`. Subtract `execute` →
assistant; subtract `remember` → stateless function; etc. Two settled decisions:
`execute` is free by default (log is the containment, per msg 4173's override);
`remember()` writes only the frame's own subtree — sharing happens solely via
`report()` up and parent-API projections down. No third path.

**Q: Whose behavior is the "loop" and whose the "manner"?** [msgs 4148, 4198]
The loop (L3) is amoral and tool-agnostic; the voice and escalation rules (L5)
are loadable text, versionable and testable. "The hammer" persona spec — warmth
as *fit*: truth first, read intent, anticipate, signal density, no performative
warmth, every promised behavior checkable — is a separate, portable, user-agnostic
module (chat.json msg 4148). The per-user project memory is a separate context
layer, never welded into the persona.

## 8. Naming (the long thread)

**Q: What is this project's name?** [msgs 4221-4246]
Still open — deliberately. Every candidate was graded and most rejected:
Confluence/Estuary (Atlassian searchability), Meeseeks/Misiques (Warner
trademark; misspellings don't dodge it), Golem (runs-amok folklore + Golem
Network collision), Doozer (Henson character + doozerd datastore), Concierge /
Serf / Friday (generic; Marvel's Friday is trademarked *for exactly this
class*; "serf" is the moral opposite of a portable tool), Zombies and Robotnik
(no). Keepers: **Homunculus** (Atomsk's constructed task-being) for the frame
concept, **Athanor/Alembic** for the runtime-if-you-name-the-vessel... and
"Frankenstein" is *Victor* (the creator), not the creature. Working title the
session itself produced: **Secret Agent** — literally "the one whose name you
don't know yet" — until it tells Victor its name. The assistant (the persona
instance) is separate and as-yet unnamed.

## 9. The actionable milestone

**Q: What is the first build step, concretely?** [msgs 4213-4214, 4247-4248]
Build the actor-model runtime + CPython integration (completed here as the
liboffs extraction + 58 passing tests), then prove the concurrency spine
milestone: one actor receives EXECUTE, stays responsive (can still take
INTERRUPT) while its Python thread runs a slow script, streams
`actor.log`/`status` output in real time, returns a corr-matched RESULT —
plus lazy interpreter boot, swappable backend, injected outbound `actor`
module with non-blocking callbacks. Verification of the footguns first
(GIL teardown from main, `Py_Initialize` once, subinterpreter API churn
PEP 684→734 against real headers), not the happy path.

**Open questions left running** (raised, answered with a recommendation, but
not yet locked by Victor): the exact default surface API set for subagents
[msg 4196, answer: four — get_context/report/spawn/request_tool — "yours to
answer"]; whether WaveDB's scan cursor / subtree copy / lineage query costs are
as cheap as needed [msg 4192: settled *by the READMEs' documented API* — range
scan, zero-copy subtree, graph triples]; the runtime's own name [§8 above].