# L5 Persona — Store Records + the Full Onyx Assembly — Design

**Date:** 2026-10-06
**Status:** owner-agreed shape settled in session (store records + the hammer ships v1; the
FULL Onyx assembly pulled; the escalation half = its own follow-on slice). No open questions —
every detail is pinned.
**Atlas slice:** a new node (delivery; L5's persona half) — evidence: `test/test_persona.cpp`
(new, pure) + the derive's injection tests in `test/test_loop.cpp` + the falsifiability
data-layer check.
**Parity sources:** the ONYX codebase (Victor's direction: "we have to pull this from the
onyx codebase") — `onyx/backend/onyx/db/persona.py` (the Persona record: the system-prompt
skeleton + the tools + the above/below placement + sharing), `onyx/backend/onyx/chat/
prompt_utils.py` (`build_system_prompt`: the base prompt + the placeholder substitution + the
user-information/company-context section + the TOOL-CONDITIONAL guidance sections), `onyx/
backend/onyx/prompts/prompt_utils.py` (`apply_prompt_placeholders`: the `{{.}}` vocabulary +
the leave-visible-on-typo rule; the `{{user.<key>}}` catalog = recognized keys RESOLVE ("" when
unavailable — never a raw-token leak), unknown keys LEFT VISIBLE so the author spots the
mistake); the chat's msg 4148 (PERSONA SPEC v1 "the hammer" — the falsifiable persona; the
voice/context separation doctrine) + msg 4220 (episodic/semantic); matrix §5 (L5's mapping:
the persona pointer into the store; the cache-stable prefix rules; plan→ask→act is the
ESCALATION slice's) + rows 14/36 (the digest/prefix engineering the derive keeps honest).
**The escalation (the blocked-ask + the ladder + the client's ask UX) — OUT (follow-on slice;
its ask surface rides this slice's client).**

## Why this slice exists

The runtime's system prompt is HARDCODED in `model.c`/`loop.c` — the loop is amoral
(deliberately), but nothing makes the agent's MANNER loadable data. The matrix's L5 plan
(msg 4148/4198's loop-manner split) needs: the persona = portable voice as loadable data; the
context (who the user is) = separate, never welded in; the whole thing testable as data.
Onyx supplies the assembly's transposed shape; the hammer supplies the first REAL persona.

## Decisions carried in (settled with the owner, 2026-10-06)

| Decision | Value | Authority |
|---|---|---|
| Storage | persona = **WaveDB store records** at the ROOT's `personas/` subtree (portable voice = root-scoped); the record = keyed versioned JSON; the VOICE/CONTEXT separation = two records | owner |
| The first record | **the hammer ships v1** (msg 4148's markdown verbatim) with its checkable-behavior list as the record's test-spec; the falsifiability meta-rule enforced at the data layer | owner |
| The assembly | the **FULL Onyx assembly** transposed: the first-block injection + the `{{.}}` placeholder machinery (the leave-visible-on-typo rule) + the user-context section (the separate record) + the TOOL-CONDITIONAL guidance (per the frame's tool surface) + the optional below-placement | owner |
| Injection point | the derive's system prompt's FIRST block; the composed string is BYTE-STABLE per inputs (the cache-stable prefix rules, row 36) | owner |
| Unset persona | today's behavior byte-identical (the built-in minimal base; no persona record = no injection) | owner |
| Out of scope | the escalation (the blocked-ask/ladder/ask-UX — the follow-on slice); the Pondr client's persona UI; the digest/fingerprint machinery (rows 14/27's refine work); mobile/other; the persona CRUD over the client API (a later wire verb if Pondr demands) | owner |
| Architecture | approach A: the PURE module `src/Frame/persona.{h,c}` + the derive's integration; rejected: inline-in-loop.c and file-based personas | owner |

## 1. The records + storage

**The voice record** — `personas/<name>/record`:

```json
{
  "version": 1,
  "name": "hammer",
  "text": "<the persona's markdown — msg 4148's spec block VERBATIM, the meta-rule
           included: it IS the record's falsifiability contract (enforced at load)>",
  "placement": "first",              /* or "below" — Onyx's above/below */
  "guidance": [
    {"key": "execute", "text": "<tool-conditional guidance for the execute surface>"}
  ],
  "test_spec": [
    "Did it restate the user's actual intent?",
    "Did it flag uncertainty wherever it existed?",
    "Did it offer the unasked-but-needed follow-up?",
    "Did it refuse fake warmth and fake certainty?",
    "Did formatting reduce load rather than add it?",
    "Is it short enough to re-read in one pass?"
  ]
}
```

**The context record** — `personas/<name>/user-context`: a SEPARATE record (the voice/context
separation is structural, not convention): either a plain text (rendered as one block) or a
JSON object (its key/value pairs are the placeholder catalog's `{USER_<KEY>}` values — Onyx's
`{{user.<key>}}` transposition). `personas/<name>/meta` = `{created}`.

**The falsifiability enforcement (the meta-rule, mechanically)**: the persona module's data
layer refuses LOUD (the load-time validation) a record whose `text` carries a principle-like
paragraph that names neither a stated principle nor appears in `test_spec`... — the honest
mechanical form: the record's `principles` (a structured list, optional) is checked against
`test_spec` at LOAD: a principle with no testable statement = the load-refusal that names the
gap. The hammer's spec (7 principles + 6 checks) passes structurally. (The enforcement stops
at structure — a NLP judgment of "reducible" is a human's call; the data layer enforces the
RECORD SHAPE's completeness, loudly.)

## 2. The full assembly (the transposition)

`persona_compose(voice, context, tools) → the system-prompt string` — pure, in `persona.c`:

1. **The persona block** (the record's `text`), placement `first` = block 1; `below` = after
   the base instructions (Onyx's above/below).
2. **The `{USER_CONTEXT}` block**: the context record rendered — plain text verbatim; a JSON
   object rendered as `key: value` lines (stable ORDER: the keys' sorted order — byte-stability).
3. **The placeholders**: the composed string's `{CURRENT_DATETIME}` (present? → substituted
   with the render-time stamp — NOTE: a substituted datetime BREAKS byte-stability per turn —
   Onyx appends when absent; OUR rule: a persona that uses `{CURRENT_DATETIME}` opts OUT of
   the cache-stable prefix (documented; the derive's stability claim weakens ONLY for that
   record — the load-time validation WARNS loud on its presence, doesn't refuse)); the
   `{USER_NAME}`/`{USER_<KEY>}` (resolved from the context record's fields; recognized-but-
   missing = "" — never a raw-token leak; UNRECOGNIZED = left visible — the author's typo
   stays readable (Onyx's rule, carried verbatim in spirit)).
4. **The tool-conditional guidance**: for each `guidance[]` entry: attached IFF the frame's
   tool surface carries a tool id == key; today's surfaces: `execute` (the one tool). The
   missing-tool guidance never attaches. The sections' ATTACH ORDER = the record's list order
   (determinism).
5. The base instructions (today's model.c/loop.c prompt tail) ride AFTER (or below, when the
   placement says so) — the composed shape = persona block + context block + guidance sections
   + the base instructions.

The FALLBACK: a record's read refused/missing = the built-in base ONLY, logged loud (never a
half-persona render; never silent).

## 3. The derive integration + the frame surface

- `frame_config_t` gains `const char* persona_name;` (BORROWED; NULL/empty = the built-in
  base — every existing frame's behavior byte-identical). The spawn spec stays
  {goal, context}-only (the recorded DEV); a child inherits the parent's persona name with
  its config (the config's copy site — the SAME inheritance rule the watchdog's/config's
  fields follow).
- The derive (loop.c) loads `personas/<name>/{record,user-context}` RIDE the SAME derive
  scan (the events scan's bounds extended with the persona reads? NO — the persona records
  are STATE, not events: they ride the SNAPSHOT's scan shape — the derive's ctx-snapshot
  machinery reads state keys; the persona read = one more state read on the same trip (the
  store actor's round trips). The compose runs after the scan reply (the events + state both
  in hand); the injected system prompt = the composed string.
- `frame_config_t`'s persona at the SPAWN: a child's cfg inherits `persona_name` (a heap
  dup, the other fields' pattern).

## 4. Testing

| Suite | Adds |
|---|---|
| `test/test_persona.cpp` (NEW, pure) | the record load/validate (the shape's refusal rules incl. the falsifiability check + the datetime warning); the compose: placement both modes, the context block's text/JSON shapes, the placeholder substitution (recognized-missing→"", unrecognized→VISIBLE), the tool-conditional attach/skip, the byte-stability (compose twice ⇒ identical bytes), the missing-record fallback |
| `test/test_loop.cpp` | the derive's injection: a frame with a persona → the model request's system prompt carries the composed block FIRST; a frame without → byte-identical to today; the hammer's REAL record shipped in the store → composed verbatim |
| `test/test_persona.cpp` (the hammer fixture) | the hammer's record stored → loaded → composed (its markdown verbatim in the prompt); its test_spec's data-layer check passes |
| the config/spawn tests | the persona inheritance through frames (a child adopts the parent's persona record — its block present in the child's prompts) |

Verification bars as always (`setarch -R` ON+ASan, valgrind strip --strip-debug, the
no-locks grep).

## 5. Out of scope / recorded follow-ons

- The ESCALATION slice: the blocked-ask (publish-then-block on the client API + the frame's
  ask verb), plan→ask→act, the Pondr ask UX — next.
- The Pondr persona UI; the client-API persona verbs (CRUD over the wire — when Pondr
  demands); the digest machinery; a SECOND tool (the guidance's conditioning surface grows
  with it).