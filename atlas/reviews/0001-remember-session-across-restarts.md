# Review 0001 — remember-session-across-restarts

- **Node:** `remember-session-across-restarts` (leaf, delivery)
- **Review date:** 2026-10-01
- **Reviewer:** fresh gate agent — did NOT build this node's work; read-only review, this file is the only thing written
- **Scope:** remember-session-across-restarts: kill-and-replay verification over the WaveDB session-subtree schema
- **Review head at evidence write:** this review was produced at commit `f0067fc` (16 commits after `2630133`, the post-evidence-block refine slice included and checked, see finding F4)

## Evidence actually run / checked

### 1. The claim's test

- `cmake --build cmake-build-debug -j` → exit 0.
- `setarch -R ctest --test-dir cmake-build-debug -R "TestLoop.TestRestartReplayRestoresSeqAndContext" --output-on-failure` → `100% tests passed, 0 tests failed out of 1`.
- Same test under the ASan build (`cmake-build-asan`): `100% tests passed, 0 tests failed out of 1` — extra leak/correctness evidence for a test excluded from valgrind (see evidence item 4).
- Read the test body (`test/test_loop.cpp:797-894`). Judged against the claim:
  - **kill-reopen-resume:** writes "remember seven" + ctx `n=7` to a scratch-disk WaveDB, `frame_destroy` + `wave_db_close`, reopens, `frame_resume(db, sid, &cfg)` on the SAME sid with the assert `restart resumes an EXISTING frame`.
  - **seq restore strictly past pre-restart events:** the post-restart append ("more") is asserted (third session's view, lines 861-888) to be the strict maximum seq: `EXPECT_LT(seq, seq_of_more)` for every other event ("every other event predates the resumed app") plus `EXPECT_GT(seq_of_more, 2)` ("the pre-restart events really carried seqs").
  - **replayed context reaching the next turn:** the recording model vtable captures the derive input; asserts `remember seven` (the replayed msg.append) and `n = 7` (the replayed ctx snapshot) both reach the next turn's derived context (`test_loop.cpp:836-839`), and `frame_is_done == 1`.
  - **no silent data loss:** the birth record is a loud resume gate (verified in `src/Frame/frame.c:2798-2807` — `frame_resume` refuses without `meta/created`, no writes, loud error), and the strict-max seq assertion forbids silent overwrite/loss.
- Full suites (fresh runs, this review): ON build **189/189 green** (`setarch -R ctest --test-dir cmake-build-debug`), ASan build **189/189 green**, OFF build **62/62 green** (matches the node's "62 on the OFF build" exactly).

### 2. Schema evidence in the node description vs reality

Grep hits in `src/Frame/frame.c`:

- `frame.c:494-497` — `snprintf(evkey, ..., "events/%020llu", seq)` → events at zero-padded `.../events/<seq>` (node's `sessions/<sid>/events/<seq>`; `frame.c:259` documents `sid_path` as `sessions/<hex>`, `frame.c:2607` composes it).
- `frame.c:2729-2730` — birth batch writes `meta/created`; `frame.c:2741-2749` — `meta/depth` and `meta/parent` in the same ops array.
- `frame.c:2130-2142` — `_frame_events_bounds` composes absolute `<sid_path>/events` bounds; `frame.c:2157-2163` — reverse scan over the bounded events range to restore the largest seq ("Boot: continue the seq counter past any persisted events (restart-safe)", `frame.c:2685`).
- `frame.c:2798-2817` — resume gate reads `meta/created` loudly (birth record required), restores `meta/depth` / `meta/parent` lineage.

Grep hits in `docs/wavedb-exploration.md` (all as described in the node):

- Zero-padded seq keys: "seq keys must be zero-padded (e.g. `%020llu`)" (lines 94, 111).
- Batch atomicity: "root-level `database_batch_sync_raw` = one `WAL_BATCH` under one `txn_id` + one commit point; cross-subtree atomicity is the same transaction" (lines 38, 92, 111).
- The 128 KB cap: "a batch larger than `wal_config.max_file_size` (default **128 KB**) is rejected with -5" (lines 39, 92).
- Scan resume: "iterators seek (chunk-descent) with half-open bounds" (lines 42, 86, 94).
- Durability at ack as a `wal_sync_mode` choice (`immediate`/`debounced`) (line 72).
- Lineage as pointer + SPO/POS index ops riding the same atomic batch (lines 63, 111); "no clone/snapshot needed (fork = lineage pointer + new subtree)" (lines 90, 111); one WaveDB per process (line 111).

No claim in the node description could not be found in code or docs.

### 3. The Open finding (WaveDB CONCURRENT-mode visibility)

- **Honestly recorded both places:** in the node description ("Open finding from the verification, on the record: in WaveDB CONCURRENT mode a write committed after a reopen is durable (a fresh session sees and replays it) but INVISIBLE to the same session's own scans and point-gets") and verbatim in the test (`test/test_loop.cpp:843-851`, "NOTE (documented WaveDB defect): ... a minimal root-level probe against WaveDB's library reproduces this; even database_snapshot does not materialize it ... The seq continuation is therefore asserted from the THIRD session's view").
- **Blocking or not?** Non-blocking, substrate-level. The runtime reopens the database only on process restart; the post-restart scenario is exactly the one proven: a *fresh* session sees and replays committed writes (`again2` in the test reads the "more" record written by `again`). The caveat only bites a session scanning its own post-open commits, which the test works around and the design does not depend on. The workaround (third-session assertion) is legitimate and does not weaken the seq-continuation proof.
- **Post-evidence drift (git log 2630133..HEAD, 16 commits):** the refine slice (8fede6f store sync-scan and delete ops, 1492bec join-site slot reset + regression test at `test/test_frame.cpp:285-313`, plus refine module/docs commits) did NOT touch the restart/resume picture. `git diff 2630133..HEAD -- src/Frame/frame.c` shows only: the refine slice's additive `_frame_sync_scan`/`_frame_ops_destroy` helpers (hunk after `_frame_set_status_done`, +126 lines), a `_frame_sync_slot_reset` refactor replacing free/memset call sites, and a `memset` guard on `put_ops` — `frame_resume`, `_frame_events_bounds`, the boot seq restore, and the birth gate are hunks-untouched. The join-slot regression test is about frame_join reusing a leftover sync slot, not restart.

### 4. Honesty check

- **Completion deliberately held:** the lifecycle code exists as described — `atlas/kit/lifecycle.cjs` pushes `'no-linked-review'` (line 71) and `'parent-is-not-reviewer-authority'` (line 75), matching the verbatim refusal recorded in the node description.
- **Suite counts:** description says 153 ctest green at its writing (2026-10-01 frame-orchestration close). Now 189 (verified green ON and ASan) — the suite grew with the refine slice's tests; OFF stays 62 (verified). Stale-but-growing counts, honestly framed at write time.
- **Valgrind breadth wording:** verified — `ctest -N -R "TestFrame|TestStore|TestFrameTree|TestLoop|TestModelDecode"` now counts 42 (38 at evidence write; grew with the suite). The restart-test exclusion is genuinely recorded on node `S006` (= `drive-model-driven-frame-tree`, per `atlas/references.cjs` byId mapping): "TestLoop.TestRestartReplay is pathologically slow under valgrind emulation (killed at the 40 min cap mid-test, 4 prior tests clean)". ASan now covers this specific test in this review (evidence item 1), closing the residual gap further.
- **Live gate:** `test/test_live_loop.cpp` exists, is opt-in/env-gated against a real endpoint, model.c:270 handles the degraded `finish_reason:"load"` capture honestly. The 67.6 s Ollama pass is endpoint-dependent and not re-runnable in this review; accepted as an honestly-recorded opt-in claim.
- **No-lock grep:** ran the standing grep myself — `grep -nE "platform_mutex|rwlock|barrier|sem"` over `src/Frame/frame.c`, `src/Frame/frame_internal.h`, `src/Loop/loop.c`, `src/Frame/frame_messages.h`, `src/Frame/frame.h` returns only comment matches (e.g. frame.c:133-134 is the comment explaining the collision avoidance); no lock usage. Matches the recorded exception (model.c).
- **Nothing faked:** the description's evidence claims all matched reality; the self-recorded defects (CONCURRENT visibility, Windows pending) are presented as open, not silently absorbed.

## Findings

1. **noted** — F1: WaveDB CONCURRENT-mode visibility caveat (write committed after a reopen is durable+replayable but invisible to the committing session's own scans/point-gets) is honestly recorded in the node description and verbatim in the test comment (test/test_loop.cpp:843-851), tested-by-probe, worked around via a third-session assertion, and does not block the claim: the runtime only reopens on process restart, and the fresh-session-visibility case is exactly what the test proves. Documented substrate limitation — a WaveDB-side fix should be tracked separately.
2. **noted** — F2: Suite counts in the description (153 green, 38 valgrind-breadth tests) were accurate at writing and have since grown (189/189 ON and ASan, 62/62 OFF, 42 breadth suites — all verified green by this review). Stale-by-growth only; nothing regressed.
3. **noted** — F3: The refine slice (8fede6f/1492bec/f0067fc, 16 commits after the evidence block) left the restart/resume paths untouched (diff-verified: only additive sync-scan/delete helpers and a sync-slot-reset refactor in frame.c). However, `_frame_sync_scan` is a new same-session scan consumer; when the refine slice's live work begins, its scans should be checked against the F1 caveat (scans after the session's own commits) so the limitation does not surface there first.
4. **noted** — F4: Windows verification remains pending (honestly recorded in completionEvidence, not silently absorbed). Until then the persistence claim in this node is Linux-proven only.
5. **noted** — F5: The live gate's 67.6 s Ollama pass (and its finish_reason:"load" honest-capture behavior) is endpoint-dependent and reproducible only against a live endpoint at a later date; the gate correctly re-records degraded captures instead of passing on them, but the pass itself is a point-in-time observation, not a standing guarantee.

## Verdict

**rawVerdict: PASS_WITH_NOTES**

Rationale: the node's central claim — a session persists as a WaveDB subtree and reconstructs fully (events, lineage, context projection) after a mid-run process restart, with every state mutation an atomic batch — is directly carried by `TestLoop.TestRestartReplayRestoresSeqAndContext`, which this review ran green on both the debug and ASan builds and whose body genuinely asserts the four things it is cited for (kill-reopen-resume, strict-past seq restore asserted from a third session, replayed append + ctx snapshot reaching the next turn's derive, loud birth-record gate with no silent loss). Every schema/substrate fact in the description traces to real code and docs; the one genuine hole found (CONCURRENT-mode same-session visibility) is a substrate limitation the node itself put on the record, documented in the test, and worked around in a way that preserves the proof; the refine slice added since the evidence block does not touch the restart/resume paths. All remaining gaps (Windows, live-endpoint dependence, stale-by-growth suite counts) are honestly recorded notes, not faked or hidden holes.

## Gate disposition

This review is recorded by the reviewer; the gate disposition for a PASS_WITH_NOTES run is **"accepted"** — the completion lifecycle (atlas/kit/lifecycle.cjs, fresh-review-required) can now close node `remember-session-across-restarts` on this linked review record. (Per review protocol: no severity blocker/major-with-unaddressed findings are present, so nothing blocks.)