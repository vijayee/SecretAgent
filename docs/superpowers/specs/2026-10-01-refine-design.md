# Refine (the Continual Harness) — Design

**Date:** 2026-10-01
**Status:** owner-agreed shape settled in session (supplemental state = a WaveDB subtree; refine = a USER-INVOKED command on the demo CLI, never a model tool; review through the store actor; history + rollback free from the DB-as-log). Every remaining open question from `docs/parity-feature-matrix.md` §3 (Q1–Q4) is resolved from the prime-agent code below and pinned as a decision — no TBDs. One deliberate divergence from PA is recorded (§4's evidence requirement).
**Atlas slice:** new `refine-continual-harness` node (delivery; the parity feature matrix ranked this slice #1, `docs/parity-feature-matrix.md` §6) — evidence: `test/test_refine.cpp` + the opt-in live refine gate.
**Parity source:** `docs/parity-feature-matrix.md` §3 (this design's base sketch + the OPERATIONS table ported from the code) and §6 row 1.

## Why this slice exists

SecretAgent has the whole frame-store machinery (the store actor, atomic root batches,
bounded reverse scans, the recall resolve walk) but NOTHING consolidated across a
session's turns: no memory a second session — or the same model's next run — can learn
from. PA's answer is the Continual Harness: `/refine` reviews the current trajectory with
an auxiliary model pass and applies **small, evidence-backed CRUD edits to supplemental
state** while the base system prompt stays immutable (`refinement.ts:129-180`, the
contract's own wording at :141: *"The base system prompt is immutable and MUST NOT be
rewritten"*), with recorded history snapshots that support rollback (PA README:58).

Ours collapses PA's scattered artifact files (`harness_state.json` + `refinements.jsonl`
+ per-file reload-to-avoid-clobber logic — the AR weakness row) into **WaveDB**: the
supplemental state is a subtree, every refinement is an appended record, and the
DB-as-log doctrine gives versions/dedupe/rollback without building any of the snapshot
file machinery (the whole DB holds the log; the WAL is durability machinery, not the log
— matrix §2 row 40 wording).

## Decisions carried in (already settled with the owner)

| Decision | Value | Authority |
|---|---|---|
| Supplemental state | a WaveDB subtree through the store actor — not scattered JSON files | owner, 2026-10-01 (matrix §3's proposed shape, owner-approved) |
| Refine surface | a demo-CLI command, user-invoked. NOT a model tool; auto-refine is a scheduled frame behavior LATER | owner, 2026-10-01 |
| Review | bounded trajectory review through the STORE ACTOR (bounded reverse scan, derive-style caps) | owner, 2026-10-01 |
| Writes | small evidence-backed writes via FRM_STORE_BATCH compositions | owner, 2026-10-01 |
| History/rollback | free from the DB-as-log: the whole DB holds the log; the WAL is durability machinery. Refinement history = appended store records | owner, 2026-10-01 |
| Slice rank | #1 in the recommended order (matrix §6) | matrix §6 |
| Out of scope | auto-refine scheduling, digest delivery into the derive (row 14's follow-up, shape decided in §3), ranking (§6 Q4), child-visibility of shared/global harness entries, Windows | owner, 2026-10-01 |

## 1. The supplemental subtree — path schema and scoping

WaveDB paths only (no files, no JSON documents to reopen). Two scopes, one composition
rule — every key is composed as a FULL root-level path (the store actor deals only in the
absolute-bounds discipline):

```
session-local (default)                shared (root-level, opt-in --refine-global)
sessions/<sid>/harness/log/<seq>       harness/log/<seq>
sessions/<sid>/harness/entry/<kind>/<id>   harness/entry/<kind>/<id>
sessions/<sid>/harness/meta/fingerprint    harness/meta/fingerprint
sessions/<sid>/harness/meta/digest         harness/meta/digest
```

- `log/<seq>` = the AUTHORITATIVE append-only refinement records (§5). Seq is the
  harness log's OWN counter (not the events counter): restored at every refine start by
  taking the largest seq found in the newest scan (`_frame_restore_seq`'s discipline,
  frame.c), monotonic, gaps allowed and recorded the same way.
- `entry/<kind>/<id>` = DERIVED materialization (the folded current entry; PA's
  `harness_state.json` role). Written in the same atomic batch as its log records. A
  lost/corrupt entry value is recoverable by re-folding the log (§2) — the log is truth.
- `<kind>` ∈ `prompt | memory | skill | subagent` (`RefinementKind`, refinement.ts:35).
  `<id>` = a slugged string (`slug` port, refinement.ts:396-405: trim, lowercase,
  non-alphanumerics → `_`, trim `_`, cap 80, fallback to the kind).
- Scanning bounds ride the proven composition: `<root>/log` .. `<root>/log0`
  (`/events`..`/events0`'s shape, frame.c `_frame_events_bounds`).
- A GLOBAL write composes absolute root-level keys exactly like the lineage graph ops
  do (`graph_triple_expand_ops` merges FULL root-database paths into cross-subtree
  batches) — no subtree handle is opened for the shared scope.
- **Scoping semantics** (QA2 answered, the matrix §3 question "root vs per-frame"):
  session-local writes stay inside the session subtree — a shared-scope invocation
  writes the root `harness/` subtree. During a local refinement the shared scope is
  READ-ONLY CONTEXT (PA's merge discipline, refinement.ts:380-407 and :473-483 verified
  in refinement.test.ts:471-501): an edit naming an entry that lives in the other scope
  is REJECTED loud (`"%s targets a %s-scope entry from a %s refinement"`). No cross-scope
  guessing, ever.
- Child frames do NOT see either scope in this slice: the #819 lesson says child
  visibility must be a recall-walk question and stays a recall-walk question — the
  scoped-visibility port is another slice (recorded in Known pending).

## 2. The fold: entries, versions, and the digest derive from the log alone

At refine start (and inside any test) the current state is FOLDED from the log records —
never read back from the entry materialization:

```
fold(log) = { kind -> id -> entry {title, content, path, reference, arguments,
                                   version, seq} }
```

- Each record's edits apply in order (oldest seq first): `create` inserts version 1;
  `update` bumps version (before.version + 1); `delete` removes the entry (the log's
  tombstone record; the entry put is DELETED from the materialization — §5).
- **Versions are free from the log**: an entry's version = the count of applied
  writes to it (PA's explicit version field is preserved in every record so the
  materialization stays byte-parity, refinement.ts:1128 — `version = before ? before.version + 1 : 1`).
- Malformed records (unparseable, non-object): dropped with a loud log line and rendered
  in the digest as a skip line (`refinement.ts:479-483`'s render-not-crash contract) —
  a single bad append never breaks a session's refine (PA test :826).
- The folded state's canonical render is the **digest** (§3) and its **fingerprint** (§4).

## 3. The `/refine` flow (user-invoked, bounded, store-actor only)

```
frame_run_loop (the user's session, done or any point after)     [demo CLI thread]
      │
      ▼
refine_run(f, instructions, shared_scope)          [src/Frame/refine.c; ALSO reachable
      │                                             on a PAST session via --refine-sid]
      ├─1─ refuse loud on a pooled store (the direct-sync rule, frame.c
      │    _frame_sync_store_refused — the whole direct store API is
      │    inline-store-only; refine adds one member, no exceptions)
      ├─2─ FRM_STORE_SCAN  the scope's log/<seq>   (newest 512 window max → the fold)
      ├─3─ FRM_STORE_SCAN  the frame's events/<seq> (bound: SA_REFINE_SCAN_EVENTS=128
      │    newest; every event serialized, then a 80,000-char TAIL slice —
      │    PA planRefinement's slice(-80_000), refinement.ts:1258)
      ├─4─ the review call: the frame's OWN model backend vtable complete()
      │    (§6) over [system: the review contract] + [user: current digest +
      │    prior-refinement tail + read-only-context digest (local run) +
      │    trajectory tail + instructions]; JSON-only proposal
      ├─5─ validate + apply the proposal against the fold (§4: per-edit
      │    validation, version guards, evidence gate; 0..N valid edits)
      │    ── zero valid edits ⇒ NO-OP: nothing commits, nothing changes,
      │       the caller is TOLD loudly (§4's evidence rule ⇒ no record without
      │       evidence, so an unevidenced review is a no-op BY CONSTRUCTION —
      │       the divergence from PA recorded below)
      └─6─ ONE atomic FRM_STORE_BATCH (≤11 ops, §5's composition = the 1 log
           record + every entry put/delete + meta/fingerprint + meta/digest),
           then the summary print: committed id, per-edit applied/rejected and
           why, the (possibly unchanged) fingerprint, the digest y/n line
```

- The trajectory read IS a store-actor round trip (FRM_STORE_SCAN with absolute
  composed bounds `<sid>/events`..<sid>/events0`, the derive's own scan shape;
  cap honors the store's window max — 0 or excess clamps, frame.c's FRM_STORE_SCAN).
  Evidence resolution needs NO recall walk: the review reads the trajectory records
  themselves (the citations point at scanned events), and the cross-scope digest read
  is a scan of the other scope's logged subtree — the recall walk's LINEAGE semantics
  do not apply to a global scope, and the fold never consults entry materializations.
- The flow runs end to end on the caller's thread in one bounded await chain per store
  hop (`_frame_slot_wait`'s pump idiom, SA_FRAME_STORE_WAIT_MS deadlines). No new
  threads, no new locks — the store actor remains the only serializer.
- `refine_rollback(f, seq, shared_scope)` rides the SAME machinery without a model call
  (§5).

## 4. The review: evidence, version guards, and the fingerprint

**The reviewer** (QA1 answered): the frame's own configured backend (`_frame_backend_get`
— the frame-injected override wins, else the once-built default from the frame's config,
frame.c). No second aux-model config is built; a same-config review is the parity
shape, the vtable injection makes every offline test deterministic (scripted backends
return canned proposals), and a later model-registry slice can pass a different
frame's config for a true aux model. The PA output caps are honored as REFUSE-LOUD
BOUNDS on our side (the http request contract in model.c stays frozen): a proposal
whose decoded edits exceed `SA_REFINE_MAX_EDITS` (8) rejects with PA's truncation
wording (`refinement.ts:199`: *"the model stopped before completing its JSON object"*);
a non-JSON reply (after a code-fence trim) rejects loud. The review call carries **NO
tools** (§7 — the model must never meet refine through the tool surface).

**Per-edit validation** (the `validateEdit` port, refinement.ts:1002-1052, error strings
preserved verbatim where the parity tests assert them):

- `"unsupported action %s"` / `"unsupported kind %s"` (:1004, :1007)
- `id == "base_system_prompt"` refuses at ALL actions — the immutable-base rule as a
  validation line, refinement.ts:1010 (test :318-341 asserted on the C side)
- `"create requires title and content"` (and update; :1016)
- skill edits carry the Python contract: `"skill-edit requires arguments"`,
  `"skill requires python reference"`, `"skill requires python import"`,
  `"skill requires callable or call_pattern"` (:1001-1043 shapes)
- **OURS (the deliberate strengthening):** every edit requires
  `evidence.seqs` — the trajectory event seqs (first..last) it rests on. An edit without
  any seq refuses: `"edit without evidence"`. PA's reviewer is trusted to ground itself;
  ours FAILS LOUD because the owner's shape is "small, EVIDENCE-BACKED writes" and the
  store can prove the citation (§5 records it structurally). Recorded as a divergence.

**Apply semantics** (the `applyRefinementProposal` port + gates, refinement.ts:1066-1147):

- version guards: an `update`/`delete` carries `expect.version`; a mismatch against the
  fold refuses `"stale target: entry version %u, review saw %u"` (PA's baseline stale
  rejection :1083-1100 — the same contract restated against versions, tested
  refinement.test.ts:152-179). Sequential edits to the same entry inside ONE proposal
  are ALLOWED — each edit checks the fold AS-OF the previous edit (test :181-195).
- `create` on an existing id refuses `"entry already exists"` (:1111); `update`/`delete`
  on a missing id refuse `"entry not found"` (:1102/:1115). This flat existing-id rule
  + the digest in the reviewer prompt (§3 step 4) is the duplicate-lesson gate.
- `delete` (the withdrawal action) on a missing entry refuses; its batch op is a real
  store DELETE op (§5).
- **The fingerprint gate** (QA3 answered precisely — parity with PA's mechanism, cited):
  PA's `harnessDigestFingerprint` (refinement.ts:777-843) hashes the material THE RENDER
  ACTUALLY PRINTS — per entry (sorted by scope,kind,id — order normalized away): scope,
  kind, id, title, path, version, content, and the skill's call contract ONLY (another
  kind may change reference/arguments without changing a digest byte), plus each
  refinement's printed fields in stored order, under a version salt
  (refinement.ts:25-30); it EXCLUDES the fields the render never prints (metadata,
  source, created_at/updated_at bookkeeping, query-relevance terms) — :803-814's doc
  comment. The DELIVERY gate consumes it at TURN COMMIT: the `[harness-digest]` custom
  message is injected only when the newest in-context digest's fingerprint differs from
  the current state's (`agent-session.ts:7238-7245`; cold-boundary stale-append
  :9639-9650; a fresh digest REMOVES the older in-context copies — newest-only,
  :9641-9649; parked-on-abort re-arm :7156-7160).
  **Ours:** the fingerprint is an FNV-1a-64 over the same covered-fields material
  rendered canonically from OUR fold (per entry sorted kind,id:
  `kind␀id␀version␀path␀content␀` + skill's reference/arguments only for skill entries
  + per malformed record its skip-line label; the material string starts with
  `"refine-fingerprint-v1;"`). It lives at `<scope>/meta/fingerprint` (16 lowercase hex),
  written in the SAME atomic batch as anything that changed it. It gates THREE things
  today:
  1. the *summary print* — "digest changed: yes/no" against the stored value;
  2. the *record skip*: a refine whose fold AND record set are unchanged by the proposal
     (all edits rejected, or none) writes NOTHING — the stored fingerprint gate means a
     repeated refine over an unchanged trajectory + state cannot stamp new records
     (the PA delivery gate's newest-only semantics, ported to refine-time);
  3. the FUTURE delivery slice's gate — row 14/27's turn-commit injection consumes this
     exact stored value (shape decided NOW: the digest rides the derive as a
     RE-ANCHORED block rendered fresh from the store each derive — never appended to
     msg.append history, so history stays projection-clean and the block rides the end
     of the derive's view; matrix §3's history-vs-re-anchor question ANSWERED as
     re-anchored, implementation deferred).
- **Digest value** stored at `<scope>/meta/digest`: the bounded render (§2, the
  cold-start print's read; also refine's print source). Render caps ride
  refinement.ts:20-22: 6 entries/kind, 5 newest refinements, 180 chars (port verbatim,
  tested).

**Refine's log record** (`<scope>/log/<seq>` value, JSON):

```json
{"seq":5,"id":"refine_20261001_183422","scope":"local",
 "trigger":"<summary>","rollbackOf":null,
 "evidence":{"session":"sessions/<sid>","first_seq":12,"last_seq":87,
             "summary":"<why the trajectory shows this>"},
 "edits":[{"action":"create","kind":"memory","id":"chunked_bodies_silently",
           "title":"Chunked bodies decode silently","content":"...">
           ,"path":"general","version":1,
           "reference":null,"arguments":null,
           "expect":{"version":0},
           "evidence":{"first_seq":12,"last_seq":14,
                       "summary":"cites the events this lesson rests on"}}],
 "at":"2026-10-01T18:34:22Z"}
```

A rollback's record carries `rollbackOf = <target seq>` and the inverse edits with their
own evidence ({"kind":"rollback","refineOf":N}).

## 5. History + rollback: the append-only log through the store actor

- **The harness log IS the history.** One record per refine invocation (up to 8 edits
  inside it) or per rollback. NOTHING is ever mutated, rewritten, or deleted in the log
  — a repair/withdrawal is a NEW record referencing the original (`rollbackOf`), and a
  record that later rolls back ANOTHER record is itself history (refinement history
  chains read as a plain ascending scan).
- **Entry materialization ops in the same batch:**
  - create/update → a PUT of the full entry value at `<root>/entry/<kind>/<id>`;
  - the WITHDRAW of an entry → a real DELETE op at `<root>/entry/<kind>/<id>`. This
    slice extends `frm_store_op_t` with `is_delete` (the store batch validator accepts
    the no-value shape for delete ops and maps them to WaveDB's `raw_op_t.type = 1`;
    the batch accounting counts key bytes only for deletes). The LOG never deletes.
  - The fold-on-restart rule makes deletes durable by construction: the state is folded
    from the log, so a stale entry value (or none at all) can never resurrect a lesson.
  - delete-on-rollback then re-create (PA :1163's inverse shape, ported refinement.test.ts
    :680-737): each inverse edit carries the BEFORE snapshot (`before` = the full entry
    JSON as it was at the original application) — the snapshot READ lives in the target
    record itself (read via the exact-key scan below); restore = the apply path with
    the snapshot's content and the CURRENT fold's version guard re-checked (a rollback
    after a later edit to the same entry gets its per-edit "stale target" rejection —
    never a silent overwrite).
- **Exact-record read for a rollback:** a bounded scan over
  `[<root>/log/<seq>, <root>/log/<seq+1>)` — exactly one record when it exists, none
  otherwise (log seqs are leaves; no children under a seq key). A missing target
  record refuses `"rollback: no refinement record at seq %llu"` (PA throws on a missing
  target; test :730-737).
- **Batch budget (refuse-loud math before the post):** ≤ 1 record (8 KB) + ≤ 8 entries
  (4 KB each) + 2 meta values → ~42 KB worst; the refine composer refuses any batch
  over `SA_REFINE_BATCH_BYTES` (96 KB) BEFORE posting and the store's own mirror caps
  (per-op and per-batch, SA_FRAME_MAX_BATCH_BYTES 120 KB) back it up — rejection is
  loud and early, never a truncation (frame.c's composer-mirror discipline).
- **Atomicity is free:** the whole commit is ONE `database_batch_sync_raw` root batch —
  either the record + its entries + meta all exist or nothing does (the store actor's
  batch discipline and the WAL cap already test this shape).

## 6. Records on disk (the parity mirror's data shapes) — recap

| Store key | Value | Written by |
|---|---|---|
| `<scope>/log/<seq>` | the refinement record JSON (§4) | refine_run / refine_rollback |
| `<scope>/entry/<kind>/<id>` | entry JSON, or a DELETE op on withdraw | the same batch |
| `<scope>/meta/fingerprint` | 16-hex FNV-1a-64 | the same batch |
| `<scope>/meta/digest` | the bounded digest text | the same batch |

No new frame event types; no events/msg.append writes at all (the derive's 512-event
window never sees harness traffic — §7).

## 7. What does NOT change (the freeze list)

- **The model never sees refine as a tool.** `/refine` is not in the execute surface;
  the review call carries NO tools at all (model.c's request build learns ONE shape:
  a `tools` argument that is a JSON `null` value means "no tools" — omit both `tools`
  and `tool_choice`; the NULL-POINTER behavior is unchanged).
- **The base persona / system prompt text is untouched** — and validation refuses
  `base_system_prompt` by id (§4; the PA rule as a validation line).
- **No new locks anywhere.** The refine flow is single-threaded (demo CLI thread)
  riding the store actor's existing serialization; the standing no-locks grep stays
  clean (model.c's recorded exceptions remain the only ones).
- **No changes to the events log, msg.append, compaction, spawn/join/report paths,
  the frame engine, or the turn loop.** The derive does not learn the digest in this
  slice (row 14's delivery is the follow-up; §4 pinned its shape).
- **No TODOs** anywhere the slice touches.

## 8. Test parity plan (`test/test_refine.cpp`, new file, WDB-gated)

Ported intents carry their refinement.test.ts / refinement.ts / agent-session.ts
citations inline in the test comments; the fixture drives inline stores with
`wave_db_pump` + `actor_run` (test_frame.cpp's pump order) and scripted model backends
(test_loop.cpp's scripted_complete idiom):

1. stale-target rejection on version mismatch — refinement.test.ts:152-179
2. sequential same-entry edits apply with version bumps — :181-195
3. create/update/delete lifecycle per kind — :214+
4. create-on-existing / update-on-missing / delete-on-missing refusals — apply site
   refusals :1102-1115
5. the evidence gate: an edit without seqs refuses; a proposal with zero valid edits
   commits NOTHING (the no-op rule, §3 step 5) — OURS
6. the skill contract validation — validateEdit :1002-1052
7. base_system_prompt refused at every action — :1010
8. fingerprint stability: equal folds ⇒ equal fingerprint; content change ⇒ differs;
   version-only bookkeeping with unchanged content ⇒ EQUAL (the excluded-fields rule)
   — refinement.ts:777-843
9. digest bounds: ≤6 entries/kind, ≤5 refinement lines, 180-char content trim —
   refinement.ts:20-22
10. rollback composes inverse edits (created→delete, updated→before restore,
    deleted→recreate) — refinement.test.ts:680-737
11. rollback of a missing target refuses — :730-737
12. rollback after a later edit → per-edit stale rejection — the version guard reused
13. the fingerprint gate: a second refine with no fold change commits no record —
    agent-session.ts:7238-7245's gate ported to refine-time
14. one-atomic-batch composition: a batch over budget refuses with nothing committed
    (the store's composer-mirror shape, test_frame.cpp:614's refusal idiom)
15. refine on a POOLED store refuses loud without hanging — the direct-sync rule
16. the trajectory scan's newest-128 cap holds (500 seeded events ⇒ 128 materialized)
17. the shared/global subtree separates from the session subtree; cross-scope edits
    refuse — refinement.ts:380-407, test :471-501
18. the no-tools review call: the scripted backend's captured request body carries no
    `tools`/`tool_choice` keys — the model.c JSON-null omission test
19. malformed log records: skipped loudly, digest renders a skip line, refine still
    runs — refinement.test.ts:826 + refinement.ts:479-483
20. the opt-in LIVE gate (`SA_TEST_OLLAMA_URL` + `SA_TEST_REFINE_LIVE=1`): a real model
    reviews a real seeded trajectory; asserts invariantly (outcome-independent):
    folded fingerprint + digest exist and match the store meta (or a loud no-op) —
    outcome-dependent lesson content is NOT asserted (models differ)

## 9. Acceptance criteria

1. All three build configs green (ON / ASan under `setarch -R` / OFF no-python-wavedb-
   streams) with `test_refine.cpp` registered under the WDB gate.
2. Valgrind clean over the refine suites (verified-executing stripped runs; the
   documented env-raise + disk-test exclusion idiom).
3. The no-locks grep stays clean over the frame layer (model.c's recorded exceptions).
4. Every refinement write is ONE atomic store batch through the store actor; the log is
   append-only; withdrawals are new records + materialization DELETE ops; versions and
   fingerprints fold from the log.
5. `frame-demo --refine` runs the whole flow against a real endpoint and prints the
   summary; `--refine-sid` refines a past session; `--refine-global` writes the shared
   scope. The no-op path prints loudly and commits nothing.
6. Atlas: a `refine-continual-harness` node with evidence (tests + the live gate's
   outcome-independent invariants), no TODOs touched, atomic conventional commits.

## 10. Known pending (recorded, not built here)

- Digest DELIVERY into the derive (row 14/27): shape decided (§4's re-anchored block),
  implementation deferred to the row-14 follow-up with the fingerprint already stored.
- Auto-refine: a scheduled frame behavior whose step calls refine_run is NOT here
  (PA's `reviewAutoRefine` :1331-1346 cadence maps to frame scheduling later).
- Child (and other-session) visibility of shared/global harness entries stays nothing —
  the recall-walk scoping port (#819 / matrix row 35) lands with the tree slices.
- Ranking: newest-first seq order in the digest, no term-IDF scorer (matrix §3 Q4
  answered: simple until size forces the port).
- A pooled-store refine (the engine-driven shape): the direct-sync rule refuses loud
  today, consistent with the whole direct store API family; a scheduled auto-refine
  slice revisits it from INSIDE a frame behavior.