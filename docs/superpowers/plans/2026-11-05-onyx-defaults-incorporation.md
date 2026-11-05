# Onyx Defaults Incorporation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the defaults study's five findings into the default frame's derive — the voice floor, the completed seven-verb list with the ask craft, the two-register rule, the truncation-honesty line, and the standing `<system-reminder>` block — as two byte-stable constants riding the cache-stable prefix.

**Architecture:** One constant (`SA_LOOP_INSTRUCTION`, src/Frame/loop.c) is rewritten to the spec §1.1 byte pin; a second constant (`SA_LOOP_REMINDER_BLOCK`) is added and appended in `_loop_system_content` so it ENDS the stable prefix in every frame (after the persona group AND its instruction, before the goal — the placement settled 2026-11-05 from the study's placement law; `persona_compose` is untouched). All standing derive byte pins retarget in test/test_loop.cpp via the `TEST_LOOP_INSTRUCTION` mirror + a new `TEST_LOOP_REMINDER_BLOCK` mirror.

**Tech Stack:** C (C11, get_memory/refcounter idiom), GoogleTest (C++17), CTest, WaveDB-backed frame derive. Gates: `setarch -R ctest` (ASan aborts in `__asan_init` on this machine — ALWAYS under `setarch -R`), ON+ASan + OFF + off-verify + valgrind.

**Source spec:** docs/superpowers/specs/2026-11-05-onyx-defaults-incorporation-design.md (§1/§1.1 the instruction's byte pin; §2/§2.1 the reminder block's byte pin; §4 the pins + matrix close-out; §5 the test set). The turn-limit knob is DROPPED (spec status line) — no max_turns work, no new writers for `LIFE_REASON_TURN_LIMIT`, the standing fail-loud cap (loop.c:123-124, 2407-2409) is untouched.

**Byte-pin authority:** the two constants' bytes come from spec §1.1/§2.1 EXACTLY — including the em-dashes (U+2014 `—`), the `<n>` placeholder literal in the honesty line (mirrors budget.h:42's `<original-length>` notation; the real marker is `\n[budget: truncated at %llu bytes]`, budget.c:12), and the trailing newline on every displayed line.

---

### Task 1: Spec reconciliation (docs)

**Files:**
- Modify: `docs/superpowers/specs/2026-11-05-onyx-defaults-incorporation-design.md`

The spec was written in one pass and carries (a) two unreconciled placement sentences (§2 vs the §5 test_persona parenthetical) and (b) two stale turn-limit bullets from the dropped §3. This task fixes the document BEFORE any code, so the plan's pins have one authority.

- [ ] **Step 1: Reconcile the §2 placement sentence**

Replace:

```
A SECOND stable constant (`SA_LOOP_REMINDER_BLOCK` — the same section, beside the instruction) in
the derive's system prompt, AFTER the instruction block (the stable prefix keeps one composition,
one order — the exact placement pinned at §2.1):
```

with:

```
A SECOND stable constant (`SA_LOOP_REMINDER_BLOCK` — the same section, beside the instruction) in
the derive's system prompt: the block ENDS the stable prefix in EVERY frame — after the persona
group AND its instruction, before the goal line. ONE composition rule, ONE order — settled
2026-11-05 from the study's placement law (§2.2): the rider governs the goal and the context, so
it rides closest to them. The exact bytes pinned at §2.1.
```

- [ ] **Step 2: Fix the §5 test_persona bullet's parenthetical**

Replace:

```
- **`test/test_persona.cpp`**: the hammer's compose untouched (the standing byte pins green) — the
  reminder block rides AFTER the persona group (below the persona, before the instruction's base
  text — the one ORDER pinned).
```

with:

```
- **`test/test_persona.cpp`**: the hammer's compose untouched (the standing byte pins green) — the
  reminder block rides AFTER the persona group: the block ends the stable prefix in every frame
  (the §2 ONE rule; the below placement reads [instruction][persona][reminder]).
```

- [ ] **Step 3: Fix the stale §5 test_lifecycle bullet**

Replace:

```
- **`test/test_lifecycle.cpp`**: the turn-limit reason's composer + the repair rule (a
  control{turn-limit}-only tail with no turn.end = OPEN — repair closers compose with... the trip
  tail ends turn.end{blocked} so the balance holds; pin the blocked balance again over the limit
  ask's shape).
```

with:

```
- **Standing pins that do NOT churn**: the fail-loud turn cap's control pins (test_loop.cpp:785-899)
  stay byte-identical — the stopping doctrine stands as designed (the knob is dropped); NO
  max_turns config, NO limit trip, NO `turn-limit` writer beyond the existing fail-loud one.
  test_frame.cpp/test_lifecycle.cpp pin no derive bytes (substring finds and model-authored text).
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-11-05-onyx-defaults-incorporation-design.md
git commit -m "docs: the incorporation spec's placement reconciled — the reminder ends the stable prefix, the stale turn-limit bullets land their drop"
```

---

### Task 2: The instruction's new bytes (spec §1.1)

**Files:**
- Modify: `src/Frame/loop.c:200-208` (the `SA_LOOP_INSTRUCTION` constant + its comment)
- Modify: `test/test_loop.cpp:3863-3870` (the `TEST_LOOP_INSTRUCTION` mirror — byte-for-byte)
- Test: `test/test_loop.cpp` (new `TestInstructionCarriesTheStudyFive`)

**TDD contract:** the test-side mirror updates FIRST — the suite goes red against today's runtime constant — then the C constant changes and the suite goes green. The four standing full-equality pins (TestNoPersonaIsByteIdentical, TestPersonaMissingRecordFallsBackLoud, TestPersonaPlaceholderCatalogResolvesLive, TestPersonaBelowPlacementRidesAfterTheBase) and the prefix pins (TestPersonaInjectedAsTheFirstBlock) all flow through the shared `TEST_LOOP_INSTRUCTION` — updating the ONE mirror retargets them all; NO pin-line edits in this task.

- [ ] **Step 1: Update `TEST_LOOP_INSTRUCTION` to the spec §1.1 bytes**

In test/test_loop.cpp, replace the constant at :3863-3870 (the ENTIRE literal, verbatim) with:

```cpp
/* The turn instruction's bytes, pinned here byte-for-byte (loop.c's
   SA_LOOP_INSTRUCTION — the no-persona pin below is that text's proof).
   The defaults slice's shape: the seven-verb list, the ask craft, the two
   registers, the voice floor, the truncation honesty (spec §1.1's pin). */
static const char TEST_LOOP_INSTRUCTION[] =
    "You drive one frame of an agent session. Your only tool is `execute`: its\n"
    "`code` argument is ONE python cell run in this frame's interpreter (one\n"
    "shared namespace per frame). Inside cells the injected `actor` module\n"
    "provides the verbs: actor.remember(key, value), actor.recall(key),\n"
    "actor.keys(scope), actor.emit(text), actor.spawn(goal, context=None),\n"
    "actor.report(value), and actor.ask(question, options=[]).\n"
    "Ask the user only when ambiguity is real or a decision is the owner's:\n"
    "one sharp open question, or five options at most, and say what the\n"
    "answer unblocks.\n"
    "Child reports face you as the parent agent — dense and unformatted.\n"
    "Your own replies to the user stay conversational and direct.\n"
    "Deeply understand the user's intent before answering; think step-by-step;\n"
    "offer the follow-up the user will need next.\n"
    "Content cut at \"[budget: truncated at <n> bytes]\" is not guessable:\n"
    "reconstruct it from the frame's state (actor.recall/actor.keys) or ask —\n"
    "never invent the middle.\n"
    "Finish the frame by calling actor.report inside your last cell (a\n"
    "completion declaration), or simply by answering WITHOUT a tool call.\n";
```

Byte-checks: every displayed line of the spec §1.1 block is one `\n`-terminated source line; the em-dashes in "— dense and unformatted." and "or ask —" are UTF-8 `—`; the `<n>` is a LITERAL (angle brackets ride the prompt).

- [ ] **Step 2: Add the per-finding presence test**

Insert immediately after `TestNoPersonaIsByteIdentical`'s closing brace (after :3956):

```cpp
TEST(TestLoop, TestInstructionCarriesTheStudyFive) {
  /* The defaults slice's five findings ride the LIVE prompt: the byte pins
     cover the exact composition; this names each finding so a regression
     answers WHICH one moved. */
  frame_config_t cfg = test_config();
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_t* f = frame_create(db, NULL, "find each", &cfg);
  ASSERT_NE(f, nullptr);

  recording_model_t rm = {};
  rm.base.complete = recording_complete;
  rm.replies.push_back(
      R"json({"choices":[{"message":{"role":"assistant","content":"found"}}]})json");
  frame_set_model_backend(f, &rm.base);

  EXPECT_EQ(frame_run_loop(f), 0);
  ASSERT_EQ(rm.captured.size(), 1u);
  std::string content = test_system_content(rm.captured[0]);

  /* The completed verb list (the ask/keys/emit entries join). */
  EXPECT_NE(content.find("actor.keys(scope)"), std::string::npos)
      << "the verb list's keys entry";
  EXPECT_NE(content.find("actor.emit(text)"), std::string::npos)
      << "the verb list's emit entry";
  EXPECT_NE(content.find("actor.ask(question, options=[])"), std::string::npos)
      << "the verb list's ask entry";
  /* The ask craft (study §1.5). */
  EXPECT_NE(content.find("one sharp open question, or five options at most"),
            std::string::npos) << "the ask craft's cap";
  /* The two registers (study §2.4). */
  EXPECT_NE(content.find("Child reports face you as the parent agent"),
            std::string::npos) << "the child register";
  EXPECT_NE(content.find("Your own replies to the user stay conversational"),
            std::string::npos) << "the user register";
  /* The voice floor. */
  EXPECT_NE(content.find("Deeply understand the user's intent before answering"),
            std::string::npos) << "the voice floor";
  /* The truncation honesty (the forgotten-files doctrine). */
  EXPECT_NE(content.find("never invent the middle"), std::string::npos)
      << "the honesty line";

  frame_destroy(f);
  wave_db_close(db);
}
```

- [ ] **Step 3: Run — verify RED**

```bash
setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure -R TestLoop
```

Expected: `TestInstructionCarriesTheStudyFive`, `TestNoPersonaIsByteIdentical`, `TestPersonaMissingRecordFallsBackLoud`, `TestPersonaPlaceholderCatalogResolvesLive`, `TestPersonaBelowPlacementRidesAfterTheBase`, `TestPersonaInjectedAsTheFirstBlock` FAIL (expected bytes ≠ today's constant); the substring-find plan tests (TestFreeModeAsksOnlyWhenTheModelAsks, TestPlanAskActApprovedActRunsRealCells) stay green.

- [ ] **Step 4: Implement — rewrite `SA_LOOP_INSTRUCTION`**

In src/Frame/loop.c, replace the comment at :200 AND the constant at :201-208 with:

```c
/* The one turn instruction (byte-stable per inputs — the cache-stable
   prefix's rule; it rides every turn). Carries the defaults slice's five
   study findings (spec §1.1's pin): the seven-verb list, the ask craft,
   the two registers, the voice floor, the truncation honesty. */
static const char SA_LOOP_INSTRUCTION[] =
    "You drive one frame of an agent session. Your only tool is `execute`: its\n"
    "`code` argument is ONE python cell run in this frame's interpreter (one\n"
    "shared namespace per frame). Inside cells the injected `actor` module\n"
    "provides the verbs: actor.remember(key, value), actor.recall(key),\n"
    "actor.keys(scope), actor.emit(text), actor.spawn(goal, context=None),\n"
    "actor.report(value), and actor.ask(question, options=[]).\n"
    "Ask the user only when ambiguity is real or a decision is the owner's:\n"
    "one sharp open question, or five options at most, and say what the\n"
    "answer unblocks.\n"
    "Child reports face you as the parent agent — dense and unformatted.\n"
    "Your own replies to the user stay conversational and direct.\n"
    "Deeply understand the user's intent before answering; think step-by-step;\n"
    "offer the follow-up the user will need next.\n"
    "Content cut at \"[budget: truncated at <n> bytes]\" is not guessable:\n"
    "reconstruct it from the frame's state (actor.recall/actor.keys) or ask —\n"
    "never invent the middle.\n"
    "Finish the frame by calling actor.report inside your last cell (a\n"
    "completion declaration), or simply by answering WITHOUT a tool call.\n";
```

No OTHER source change: the verb entries are TEXT — `actor.ask`/`actor.keys`/`actor.emit` already exist in the interpreter's injected module (the escalation/actor slices landed them); `SA_LOOP_PLAN_INSTRUCTION` (:216-219) is untouched.

- [ ] **Step 5: Run — verify GREEN**

```bash
setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure -R TestLoop
```

Expected: ALL TestLoop tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/Frame/loop.c test/test_loop.cpp
git commit -m "feat: the default instruction carries the study's five — the seven-verb list, the ask craft, the two registers, the floor, the truncation honesty"
```

---

### Task 3: The standing reminder block (spec §2.1)

**Files:**
- Modify: `src/Frame/loop.c` (new `SA_LOOP_REMINDER_BLOCK` constant + ONE insertion in `_loop_system_content`)
- Test: `test/test_loop.cpp` (new `TEST_LOOP_REMINDER_BLOCK` mirror; the four full-equality pins retarget; order asserts in the no-persona pin)

**TDD contract:** the test-side mirror + retargeted expectations land FIRST (red — the block not yet ridden), then the constant + insertion (green).

- [ ] **Step 1: Add the test-side mirror**

Insert immediately after `TEST_LOOP_INSTRUCTION`'s closing `;`:

```cpp
/* The standing reminder block's bytes, pinned here byte-for-byte (the
   mirror of loop.c's SA_LOOP_REMINDER_BLOCK — "the reminder ends the
   stable prefix" ordering proof). */
static const char TEST_LOOP_REMINDER_BLOCK[] =
    "<system-reminder>\n"
    "These tags are added by the system, never the user. Behave in accordance\n"
    "with relevance and continue normally otherwise.\n"
    "</system-reminder>\n"
    "<system-reminder>\n"
    "Address every part of the goal explicitly; values, names, and steps the\n"
    "frame's context carries ride verbatim rather than summarized away. When\n"
    "the context does not contain what is needed, say so directly rather than\n"
    "answering with adjacent information.\n"
    "</system-reminder>\n";
```

- [ ] **Step 2: Retarget the four full-equality pins**

(a) TestNoPersonaIsByteIdentical (:3950-3951) — replace:

```cpp
  std::string expected = std::string(TEST_LOOP_INSTRUCTION) + "Goal: pin the shape\n";
  EXPECT_EQ(content, expected) << "the persona-less system block moved";
```

with:

```cpp
  std::string expected =
      std::string(TEST_LOOP_INSTRUCTION) + TEST_LOOP_REMINDER_BLOCK +
      "Goal: pin the shape\n";
  EXPECT_EQ(content, expected) << "the persona-less system block moved";

  size_t instr_pos = content.find("You drive one frame");
  size_t reminder_pos = content.find("<system-reminder>");
  size_t goal_pos = content.find("Goal: pin the shape\n");
  ASSERT_NE(instr_pos, std::string::npos);
  ASSERT_NE(reminder_pos, std::string::npos);
  ASSERT_NE(goal_pos, std::string::npos);
  EXPECT_LT(instr_pos, reminder_pos) << "instruction then reminder";
  EXPECT_LT(reminder_pos, goal_pos) << "the reminder ends the stable prefix";
```

(b) TestPersonaMissingRecordFallsBackLoud (:3979-3980) — replace:

```cpp
  std::string expected =
      std::string(TEST_LOOP_INSTRUCTION) + "Goal: fallback turns\n";
```

with:

```cpp
  std::string expected =
      std::string(TEST_LOOP_INSTRUCTION) + TEST_LOOP_REMINDER_BLOCK +
      "Goal: fallback turns\n";
```

(c) TestPersonaPlaceholderCatalogResolvesLive (:4022-4024) — replace:

```cpp
  std::string expected =
      "Hello Victor, dear , keep {WHATEVER} visible.\n\nname: Victor\n\n" +
      std::string(TEST_LOOP_INSTRUCTION) + "Goal: greet me\n";
```

with:

```cpp
  std::string expected =
      "Hello Victor, dear , keep {WHATEVER} visible.\n\nname: Victor\n\n" +
      std::string(TEST_LOOP_INSTRUCTION) + TEST_LOOP_REMINDER_BLOCK +
      "Goal: greet me\n";
```

(d) TestPersonaBelowPlacementRidesAfterTheBase (:4124) — replace:

```cpp
  std::string expected = std::string(composed) + "Goal: below rides\n";
```

with:

```cpp
  std::string expected =
      std::string(composed) + TEST_LOOP_REMINDER_BLOCK + "Goal: below rides\n";
```

`TestPersonaInheritsIntoTheSpawnedChild` and `TestPersonaInjectedAsTheFirstBlock` carry prefix/find pins ONLY — no edit (the append keeps their bytes intact).

- [ ] **Step 3: Run — verify RED**

```bash
setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure -R TestLoop
```

Expected: the four retargeted tests FAIL (the block is not in the captured prompt yet); the presence test from Task 2 still passes.

- [ ] **Step 4: Implement the constant**

In src/Frame/loop.c, insert after `SA_LOOP_PLAN_INSTRUCTION`'s closing `;` (after :219):

```c
/* The standing reminder block (defaults spec §2): machinery text for every
   frame's derive — the NEVER-CHANGING reminder belongs in the stable
   prefix, not a per-turn message (a future CHANGEABLE reminder gets the
   per-turn back-slot, NOT this constant). The placement is ONE rule
   (settled 2026-11-05): appended after the composed prefix — the persona
   group AND its instruction — before the goal line, in every frame. */
static const char SA_LOOP_REMINDER_BLOCK[] =
    "<system-reminder>\n"
    "These tags are added by the system, never the user. Behave in accordance\n"
    "with relevance and continue normally otherwise.\n"
    "</system-reminder>\n"
    "<system-reminder>\n"
    "Address every part of the goal explicitly; values, names, and steps the\n"
    "frame's context carries ride verbatim rather than summarized away. When\n"
    "the context does not contain what is needed, say so directly rather than\n"
    "answering with adjacent information.\n"
    "</system-reminder>\n";
```

- [ ] **Step 5: Implement the insertion**

In `_loop_system_content` (src/Frame/loop.c), insert between the if/else that puts the persona prefix OR the plan-block/base-instruction (ends at :414, the `_loop_sb_puts(&sb, SA_LOOP_INSTRUCTION)` inside `else`) and the `const char* goal = _frame_goal(f);` line (:416):

```c
  _loop_sb_puts(&sb, SA_LOOP_REMINDER_BLOCK);   /* the standing reminder ends
                                                   the stable prefix — the
                                                   placement's ONE rule */
```

The resulting order for every shape:
- persona-less, free turn: `[SA_LOOP_INSTRUCTION][SA_LOOP_REMINDER_BLOCK][Goal...]`
- persona-less, plan turn: `[SA_LOOP_PLAN_INSTRUCTION][SA_LOOP_INSTRUCTION][SA_LOOP_REMINDER_BLOCK][Goal...]`
- placement-"first", free turn: `[persona group incl. instruction][SA_LOOP_REMINDER_BLOCK][Goal...]`
- placement-"below", free turn: `[persona group incl. instruction][SA_LOOP_REMINDER_BLOCK][Goal...]`
- persona + plan turn: `[persona group incl. instruction][SA_LOOP_PLAN_INSTRUCTION][SA_LOOP_REMINDER_BLOCK][Goal...]`

The plan-turn shapes ride the same one rule (no plan-turn pin churn: the plan tests use substring finds on "PLAN mode" only).

- [ ] **Step 6: Run — verify GREEN (whole suite)**

```bash
setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: 377/377 (376 standing + the one new test).

- [ ] **Step 7: Commit**

```bash
git add src/Frame/loop.c test/test_loop.cpp
git commit -m "feat: the standing reminder block ends the stable prefix — the tags' contract and the goal's completeness rider in every frame's derive"
```

---

### Task 4: Matrix close-out (docs)

**Files:**
- Modify: `docs/parity-feature-matrix.md`

- [ ] **Step 1: Locate the rows**

`grep -nE '^\| (4|7|11|34|35|37) ' docs/parity-feature-matrix.md` — confirm each row's title matches spec §4's list (4: Turn/step lifecycle envelope; 7: Wake latching + sticky turn outcomes; 11: Budgeted moving-context layout; 34: Durable input before any model work; 35: Dynamic context as in-band tagged messages; 37: Permission gate allow/deny/ask + plan mode).

- [ ] **Step 2: Append the incorporation notes**

Append to each row's Test-parity candidate cell (keeping the table's format; a sentence, not a new column):

- Row 4: ` — the incorporation 2026-11-05: the turn envelope unchanged; the default instruction now names the full seven-verb surface (actor.ask/keys/emit ride the list, loop.c)`
- Row 7: ` — the incorporation 2026-11-05: the stop stays behavioral; the budget survey settled unbounded as the default and no count-cap was grafted`
- Row 11: ` — the incorporation 2026-11-05: cut content is declared non-guessable AT THE INSTRUCTION (reconstruct from frame state or ask — never invent the middle)`
- Row 34: ` — the incorporation 2026-11-05: the standing <system-reminder> contract rides the stable prefix (SA_LOOP_REMINDER_BLOCK)`
- Row 35: ` — the incorporation 2026-11-05: the reminder tags' contract line tells the model the tags are system-authored (Onyx prompts/constants.py adaptation)`
- Row 37: ` — the incorporation 2026-11-05: the ask craft rides the default instruction (one sharp open question or ≤5 options, say what the answer unblocks)`

- [ ] **Step 3: Add the §5 escalation-craft line**

In "Section 5 — Escalation (L5) + persona" (~:179), append one paragraph after the escalation section's existing body:

```
The ask CRAFT (defaults incorporation 2026-11-05): the ask verb's discipline rides the DEFAULT
instruction, not just the ladder's gate asks — ask only when ambiguity is real or a decision is
the owner's; one sharp open question or at most five options; say what the answer unblocks. The
ladder's gate asks (the plan gate) keep their runtime-authored shape; this is the model-side
half — Claude Code's escalation machinery plus Onyx's conversational-clarification voice.
```

- [ ] **Step 4: Commit**

```bash
git add docs/parity-feature-matrix.md
git commit -m "docs: the parity matrix gains the incorporation notes — the seven-verb surface, the standing reminder contract, the ask craft"
```

---

### Task 5: The standing bar (final sweep)

**Files:** none modified (verification only), unless a failure forces a fix (fix → re-run the whole task).

- [ ] **Step 1: Full ON suite (the default build)**

```bash
setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: 377/377, no leaks in the suite's own accounting.

- [ ] **Step 2: ASan build**

```bash
setarch -R cmake --build cmake-build-asan -j && setarch -R ctest --test-dir cmake-build-asan --output-on-failure
```

Expected: 377/377. (MUST run under `setarch -R` — the kernel-ASLR ASan abort.)

- [ ] **Step 3: OFF + off-verify builds**

```bash
cmake --build cmake-build-off -j && setarch -R ctest --test-dir cmake-build-off --output-on-failure
cmake --build cmake-build-off-verify -j && setarch -R ctest --test-dir cmake-build-off-verify --output-on-failure
```

Expected: OFF 109/109, off-verify 222/222 (test_loop.cpp is WDB-gated — OFF counts unchanged by this slice).

- [ ] **Step 4: Valgrind over the touched suite**

```bash
cmake --build cmake-build-vg -j
TESTBIN=$(find cmake-build-vg -name 'test_*' -type f | grep -E 'test_(frame|loop)$' | head -2)
for b in $TESTBIN; do strip --strip-debug "$b"; done
setarch -R valgrind --leak-check=full --error-exitcode=1 ctest --test-dir cmake-build-vg --output-on-failure -R 'TestLoop.TestNoPersonaIsByteIdentical|TestLoop.TestInstructionCarriesTheStudyFive|TestLoop.TestPersonaBelowPlacementRidesAfterTheBase'
```

Expected: 0 new leaks (the lsan-cpython/vg suppressions carry the standing CPython-class entries — no new suppression should be needed; if one is, investigate before adding).

- [ ] **Step 5: TODO sweep + style check**

```bash
grep -rn "TODO\|FIXME\|HACK\|XXX" src/Frame/loop.c test/test_loop.cpp
```

Expected: empty. Confirm STYLE_GUIDE.md conformance of the edited regions (indent discipline, comment voice) and that no `Co-Authored-By` lines ride the commits (this plan never creates commits with them).

- [ ] **Step 6: Update the memory ledger** (the project's known-pending discipline)

Append to the SecretAgent project memory's slice line: the defaults incorporation slice COMPLETE (the five findings, the reminder ends the stable prefix; the turn-limit knob dropped by design — revisit conditions live at the loop-budget note). Suite counts per Steps 1-3's actual results.

---

## Self-review notes (already applied)

- Spec coverage: §1/§1.1 → Task 2; §2/§2.1 → Task 3; §4 (pins retarget → Tasks 2-3; hammer untouched — no persona.c change; matrix → Task 4); §5 tests → Tasks 2-3 + the standing-bars task; §6 non-goals honored (no back-slot machinery, no citation text, no planner, no max_tokens writer — nothing in any task touches these).
- The spec's stale §5 bullets were fixed in the spec itself (Task 1) rather than silently ignored.
- Placement: settled with the owner (2026-11-05, "after the prefix") — one rule for all frames; recorded in Task 1's spec edit and Task 3's implementation comment.
- Byte-consistency: `TEST_LOOP_INSTRUCTION` (Task 2) and `TEST_LOOP_REMINDER_BLOCK` (Task 3) are byte-identical twins of the src constants — the four full-equality pins compose their expected strings FROM these mirrors, so no pin can drift.
- Em-dashes and the `<n>` literal are the highest-risk bytes; both tasks repeat the byte-check note.
