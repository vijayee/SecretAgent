//
// Created by victor on 10/1/26.
//
// test_refine.cpp — the parity mirror (spec §8): the ported refinement.test.ts
// intents (citations inline), run against pure data first (Task 1/2), the
// store round trips (Task 3), the real review call (Task 4), and the full
// refine_run/refine_rollback cycles (Task 5+7).
#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "../src/Util/allocator.h"
#include "../src/Frame/refine.h"
}

#include <string>

/* Task 1's tests build folds in memory, so the seed helpers live here: the
   parse function's own round trips ride the store-actor tests (Task 3+).
   mk_fold_seed plants one memory entry DIRECTLY through the public
   refine_entry_* API (the record array it would compose is Task 3's scan
   reply — not a Task 1 fixture). */

static const char* const kSkillReference =
    "{\"type\":\"python\",\"import\":\"agent_skills.example\",\"callable\":\"run\"}";
static const char* const kSkillArguments = "{\"prompt\":\"run the skill\"}";

/* Copies a plain string into the struct's malloc'd field (NULL passes null). */
static char* refi_dup(const char* s) {
  if (s == NULL) return NULL;
  char* copy = (char*) get_memory(strlen(s) + 1);
  strcpy(copy, s);
  return copy;
}

static void refi_edit_set(refine_edit_t* e, const char* action, const char* kind,
                          const char* id, const char* title, const char* content,
                          const char* path, const char* reference,
                          const char* arguments, unsigned expect_version,
                          uint64_t ev_first, uint64_t ev_last, const char* reason) {
  refine_edit_destroy(e);
  e->action = refi_dup(action);
  e->kind = refi_dup(kind);
  e->id = refi_dup(id);
  e->title = refi_dup(title);
  e->content = refi_dup(content);
  e->path = refi_dup(path);
  e->reference = refi_dup(reference);
  e->arguments = refi_dup(arguments);
  e->expect_version = expect_version;
  e->evidence_first = ev_first;
  e->evidence_last = ev_last;
  e->reason = refi_dup(reason);
}

static void refi_edit_clear(refine_edit_t* e) {
  refine_edit_destroy(e);
}

/* One seeded memory entry (kind/id/version per call; the fields the fold
   tests reason over). seq 1 = a put that rode record seq 1. */
static refine_entry_t* mk_fold_seed(refine_fold_t* fold, const char* kind,
                                    const char* id, unsigned version) {
  return refine_entry_put(fold, kind, id, "Memory title", "memory content",
                          "memory/path", "{}", "{}", version, 0, 1);
}

static void mk_fold_init(refine_fold_t* fold) {
  memset(fold, 0, sizeof(*fold));
}

/* The lifecycle test needs the seeded entries too. */
static void mk_fold_seed_kind(refine_fold_t* fold, const char* kind, unsigned version) {
  std::string id = std::string(kind) + "_entry";
  bool is_skill = strcmp(kind, "skill") == 0;
  refine_entry_t* put =
      refine_entry_put(fold, kind, id.c_str(), "Memory title", "memory content",
                       "memory/path", is_skill ? kSkillReference : "{}",
                       is_skill ? kSkillArguments : "{}", version, 0, 1);
  ASSERT_NE(put, nullptr);
}

/* port of refinement.test.ts:214+ (the it.each(kinds) lifecycle): every
   kind creates at version 1, updates bump, delete tombstones; delete on
   the entry leaves the FOLD without it but the record history intact
   (Task 5 tests the store side). */
TEST(TestRefine, TestCreateUpdateDeleteLifecycleAppliesPerKind) {
  for (int k = 0; k < REFINE_KINDS_COUNT; k++) {
    const char* kind = REFINE_KINDS[k];
    SCOPED_TRACE(kind);
    refine_fold_t fold;
    mk_fold_init(&fold);
    refine_edit_t e;
    memset(&e, 0, sizeof(e));

    std::string id = std::string(kind) + "_entry";
    std::string title = std::string(kind) + " title";
    std::string content = std::string(kind) + " content";
    std::string path = std::string(kind) + "/created";
    bool is_skill = strcmp(kind, "skill") == 0;

    refi_edit_set(&e, "create", kind, id.c_str(), title.c_str(), content.c_str(),
                  path.c_str(), is_skill ? kSkillReference : NULL,
                  is_skill ? kSkillArguments : NULL, 0, 5, 9, "seed lesson");
    char* err = refine_edit_apply(&fold, &e, 10);
    ASSERT_EQ(err, nullptr) << "create refused for " << kind;
    refine_entry_t* entry = refine_entry_find(&fold, kind, id.c_str());
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->version, 1u);
    EXPECT_STREQ(entry->title, title.c_str());
    EXPECT_STREQ(entry->content, content.c_str());
    EXPECT_STREQ(entry->path, path.c_str());
    if (is_skill) {
      EXPECT_STREQ(entry->reference, kSkillReference);
      EXPECT_STREQ(entry->arguments, kSkillArguments);
    }

    refi_edit_set(&e, "update", kind, id.c_str(), "Updated title",
                  "Updated content", NULL, is_skill ? kSkillReference : NULL,
                  is_skill ? kSkillArguments : NULL, 1, 5, 9,
                  "the second turn showed the lesson lands");
    err = refine_edit_apply(&fold, &e, 11);
    ASSERT_EQ(err, nullptr) << "update refused for " << kind;
    entry = refine_entry_find(&fold, kind, id.c_str());
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->version, 2u);
    EXPECT_STREQ(entry->title, "Updated title");
    EXPECT_STREQ(entry->content, "Updated content");
    EXPECT_STREQ(entry->path, path.c_str()) << "NULL path keeps the entry's path";

    size_t live_entries = fold.nentries;
    refi_edit_set(&e, "delete", kind, id.c_str(), NULL, NULL, NULL, NULL, NULL,
                  2, 5, 9, "the lesson was withdrawn");
    err = refine_edit_apply(&fold, &e, 12);
    ASSERT_EQ(err, nullptr) << "delete refused for " << kind;
    EXPECT_EQ(refine_entry_find(&fold, kind, id.c_str()), nullptr)
        << "the live fold lost the entry";
    EXPECT_EQ(fold.nentries, live_entries)
        << "the tombstone row stays in the fold (the log is truth)";

    refi_edit_clear(&e);
    refine_fold_destroy(&fold);
  }
}

/* port of refinement.test.ts:281-296 ("creates ids from titles and uses
   default path and metadata when omitted") + the plan's slug rule: a
   create with NO id derives its id from the slug (title
   "Chunked bodies decode silently!" -> id
   "chunked_bodies_decode_silently"; "Native Check!" -> "native_check"),
   defaults path "general" at version 1, and the skill contract survives. */
TEST(TestRefine, TestCreateDerivesSlugIdAndDefaults) {
  refine_fold_t fold;
  mk_fold_init(&fold);
  refine_edit_t e;
  memset(&e, 0, sizeof(e));

  refi_edit_set(&e, "create", "memory", NULL, "Chunked bodies decode silently!",
                "A chunked response body decodes in place", NULL, NULL, NULL, 0,
                3, 4, "the chunked-response turn proved it");
  char* err = refine_edit_apply(&fold, &e, 5);
  ASSERT_EQ(err, nullptr);
  refine_entry_t* entry =
      refine_entry_find(&fold, "memory", "chunked_bodies_decode_silently");
  ASSERT_NE(entry, nullptr);
  EXPECT_STREQ(entry->title, "Chunked bodies decode silently!");
  EXPECT_STREQ(entry->path, "general");
  EXPECT_EQ(entry->version, 1u);

  refi_edit_set(&e, "create", "skill", NULL, "Native Check!", "Run checks.",
                NULL, kSkillReference, kSkillArguments, 0, 6, 7, "why");
  err = refine_edit_apply(&fold, &e, 6);
  ASSERT_EQ(err, nullptr);
  entry = refine_entry_find(&fold, "skill", "native_check");
  ASSERT_NE(entry, nullptr);
  EXPECT_STREQ(entry->path, "general") << "path defaults to general on create";
  EXPECT_EQ(entry->version, 1u);
  EXPECT_STREQ(entry->reference, kSkillReference);
  EXPECT_STREQ(entry->arguments, kSkillArguments);

  refi_edit_clear(&e);
  refine_fold_destroy(&fold);
}

/* port of refinement.test.ts:152-179: an update whose expect_version is
   behind the fold's current version is refused loud, error text
   "stale target: entry version 2, review saw 1", and the fold's content
   is UNCHANGED (never a silent overwrite). */
TEST(TestRefine, TestStaleTargetRejectedOnVersionMismatch) {
  refine_fold_t fold;
  mk_fold_init(&fold);
  refine_edit_t e;
  memset(&e, 0, sizeof(e));

  refine_entry_t* seeded = mk_fold_seed(&fold, "memory", "memory_entry", 2);
  ASSERT_NE(seeded, nullptr);

  refi_edit_set(&e, "update", "memory", "memory_entry", "Planned title",
                "stale planned content", NULL, NULL, NULL, 1, 5, 9,
                "planned against the pre-bump state");
  char* err = refine_edit_apply(&fold, &e, 2);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "stale target: entry version 2, review saw 1");
  free(err);

  /* The fold is UNCHANGED — never a silent overwrite. */
  refine_entry_t* after = refine_entry_find(&fold, "memory", "memory_entry");
  ASSERT_NE(after, nullptr);
  EXPECT_STREQ(after->content, "memory content");
  EXPECT_STREQ(after->title, "Memory title");
  EXPECT_STREQ(after->path, "memory/path");
  EXPECT_EQ(after->version, 2u);
  EXPECT_EQ(after->seq, 1u);
  EXPECT_EQ(fold.nrecords, 0u) << "a refused edit composes no record";

  refi_edit_clear(&e);
  refine_fold_destroy(&fold);
}

/* port of refinement.test.ts:181-195: two updates to the same entry IN
   ONE PROPOSAL both apply — the second's expect_version matches the fold
   as-of the first edit; final version 3; final content from the second. */
TEST(TestRefine, TestSequentialEditsToSameEntryApply) {
  refine_fold_t fold;
  mk_fold_init(&fold);
  refine_edit_t e;
  memset(&e, 0, sizeof(e));
  ASSERT_NE(mk_fold_seed(&fold, "memory", "memory_entry", 1), nullptr);

  refi_edit_set(&e, "update", "memory", "memory_entry", "First", "first", NULL,
                NULL, NULL, 1, 5, 9, "first edit");
  char* err = refine_edit_apply(&fold, &e, 2);
  ASSERT_EQ(err, nullptr);
  refine_entry_t* entry = refine_entry_find(&fold, "memory", "memory_entry");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->version, 2u);
  EXPECT_STREQ(entry->content, "first");

  /* The second edit's expect_version matches the fold AS-OF the first. */
  refi_edit_set(&e, "update", "memory", "memory_entry", "Second", "second", NULL,
                NULL, NULL, 2, 5, 9, "second edit");
  err = refine_edit_apply(&fold, &e, 2);
  ASSERT_EQ(err, nullptr);
  entry = refine_entry_find(&fold, "memory", "memory_entry");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->version, 3u);
  EXPECT_STREQ(entry->content, "second");
  EXPECT_STREQ(entry->title, "Second");

  refi_edit_clear(&e);
  refine_fold_destroy(&fold);
}

/* the apply-site refusals: "entry already exists" / "entry not found"
   (refinement.ts:1102-1115, test.ts:351-368). */
TEST(TestRefine, TestCreateOnExistingUpdateAndDeleteOnMissingRefuse) {
  for (int k = 0; k < REFINE_KINDS_COUNT; k++) {
    const char* kind = REFINE_KINDS[k];
    SCOPED_TRACE(kind);
    refine_fold_t fold;
    mk_fold_init(&fold);
    refine_edit_t e;
    memset(&e, 0, sizeof(e));
    mk_fold_seed_kind(&fold, kind, 1);

    std::string id = std::string(kind) + "_entry";
    std::string missing = std::string(kind) + "_missing";
    bool is_skill = strcmp(kind, "skill") == 0;
    const char* reference = is_skill ? kSkillReference : NULL;
    const char* arguments = is_skill ? kSkillArguments : NULL;

    /* create on an existing entry refuses. */
    refi_edit_set(&e, "create", kind, id.c_str(), "t", "c", NULL, reference,
                  arguments, 0, 5, 9, "duplicate lesson");
    char* err = refine_edit_apply(&fold, &e, 2);
    ASSERT_NE(err, nullptr);
    EXPECT_STREQ(err, "entry already exists");
    free(err);

    /* update of a missing entry refuses. */
    refi_edit_set(&e, "update", kind, missing.c_str(), "t", "c", NULL, reference,
                  arguments, 1, 5, 9, "lesson went missing?");
    err = refine_edit_apply(&fold, &e, 2);
    ASSERT_NE(err, nullptr);
    EXPECT_STREQ(err, "entry not found");
    free(err);

    /* delete of a missing entry refuses. */
    refi_edit_set(&e, "delete", kind, missing.c_str(), NULL, NULL, NULL, NULL,
                  NULL, 1, 5, 9, "lesson went missing?");
    err = refine_edit_apply(&fold, &e, 2);
    ASSERT_NE(err, nullptr);
    EXPECT_STREQ(err, "entry not found");
    free(err);

    /* The seeded fold is untouched by the refusals. */
    ASSERT_NE(refine_entry_find(&fold, kind, id.c_str()), nullptr);
    EXPECT_EQ(refine_entry_find(&fold, kind, missing.c_str()), nullptr);

    refi_edit_clear(&e);
    refine_fold_destroy(&fold);
  }
}

/* OURS (the spec §4 divergence, recorded there): an edit with no
   evidence seqs refuses "edit without evidence"; a proposal where every
   edit is refused composes NO record — the caller learns the no-op. */
TEST(TestRefine, TestEditWithoutEvidenceRefusedAndZeroEditsAreNoop) {
  refine_fold_t fold;
  mk_fold_init(&fold);
  refine_edit_t e;
  memset(&e, 0, sizeof(e));

  /* The evidence gate bounds: both unset, either unset, and an unsane
     (reversed) span all refuse with the one loud string. */
  refi_edit_set(&e, "create", "memory", "no_evidence", "t", "c", NULL, NULL, NULL,
                0, 0, 0, "why");
  char* err = refine_edit_evidence_check(&e);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "edit without evidence");
  free(err);

  refi_edit_set(&e, "create", "memory", "half_evidence", "t", "c", NULL, NULL,
                NULL, 0, 12, 0, "why");
  err = refine_edit_evidence_check(&e);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "edit without evidence");
  free(err);

  refi_edit_set(&e, "create", "memory", "reversed", "t", "c", NULL, NULL, NULL, 0,
                14, 12, "why");
  err = refine_edit_evidence_check(&e);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "edit without evidence");
  free(err);

  refi_edit_set(&e, "create", "memory", "evidenced", "t", "c", NULL, NULL, NULL, 0,
                12, 14, "why");
  err = refine_edit_evidence_check(&e);
  EXPECT_EQ(err, nullptr);

  /* The gate holds at the apply site too: an evidence-bearing edit applies,
     and the fold gained exactly the evidence-carrying lesson. */
  ASSERT_EQ(refine_edit_apply(&fold, &e, 1), nullptr);
  ASSERT_NE(refine_entry_find(&fold, "memory", "evidenced"), nullptr)
      << "sanity: the evidence-carrying version applied";

  refine_edit_destroy(&e);
  refine_fold_destroy(&fold);   /* the second scenario reseeds from zero */
  mk_fold_init(&fold);
  memset(&e, 0, sizeof(e));
  mk_fold_seed(&fold, "memory", "memory_entry", 1);

  /* A proposal where every edit is refused (two stale targets: the seeded
     version is 1, the review saw nothing) composes NO record — the fold
     gains neither entries nor record lines, the caller's compose list is
     empty, and the whole call is a loud no-op. */
  refi_edit_set(&e, "update", "memory", "memory_entry", "First", "one", NULL,
                NULL, NULL, 0, 5, 9, "refused edit one");
  err = refine_edit_apply(&fold, &e, 2);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "stale target: entry version 1, review saw 0");
  free(err);

  refi_edit_set(&e, "update", "memory", "memory_other", "Second", "two", NULL,
                NULL, NULL, 0, 5, 9, "refused edit two");
  err = refine_edit_apply(&fold, &e, 2);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "entry not found");
  free(err);

  EXPECT_EQ(fold.nentries, 1u);
  EXPECT_EQ(fold.nrecords, 0u) << "zero valid edits compose no record";
  refine_entry_t* seeded = refine_entry_find(&fold, "memory", "memory_entry");
  ASSERT_NE(seeded, nullptr);
  EXPECT_EQ(seeded->version, 1u) << "the fold is unchanged";

  refi_edit_clear(&e);
  refine_fold_destroy(&fold);
}

/* port of refinement.ts:1010: id == "base_system_prompt" refuses at
   create/update/delete with the verbatim string. */
TEST(TestRefine, TestBaseSystemPromptIdRefusedEveryAction) {
  const char* const actions[] = {"create", "update", "delete"};
  refine_edit_t e;
  memset(&e, 0, sizeof(e));
  for (const char* action : actions) {
    SCOPED_TRACE(action);
    refi_edit_set(&e, action, "prompt", REFINE_ID_BASE_PROMPT, "t", "c", NULL,
                  NULL, NULL, 1, 5, 9, "the base rule");
    char* err = refine_edit_validate(&e);
    ASSERT_NE(err, nullptr);
    EXPECT_STREQ(err, "base system prompt is not editable");
    free(err);
  }

  /* A create with NO id whose title slugs to the base id refuses too
     (refinement.test.ts: the create whose title derives the base id). */
  refi_edit_set(&e, "create", "prompt", NULL, "Base System Prompt", "c", NULL,
                NULL, NULL, 0, 5, 9, "the base rule");
  char* err = refine_edit_validate(&e);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "base system prompt is not editable");
  free(err);

  /* The apply site refuses loud as well — nothing can ever seed the base. */
  refine_fold_t fold;
  mk_fold_init(&fold);
  char* apply_err = refine_edit_apply(&fold, &e, 1);
  ASSERT_NE(apply_err, nullptr);
  free(apply_err);
  EXPECT_EQ(fold.nentries, 0u);

  refi_edit_clear(&e);
  refine_fold_destroy(&fold);
}

/* port of refinement.ts:1001-1043's skill lines: a skill create without
   arguments refuses; a reference that is not an object or carries no
   python import/callable refuses; a WELL-formed skill edit applies and
   its contract survives the fold. */
TEST(TestRefine, TestSkillContractCarriesPythonReferenceAndArguments) {
  refine_fold_t fold;
  mk_fold_init(&fold);
  refine_edit_t e;
  memset(&e, 0, sizeof(e));

  /* A skill create without arguments refuses. */
  refi_edit_set(&e, "create", "skill", "argumentless", "t", "c", NULL,
                kSkillReference, NULL, 0, 5, 9, "why");
  char* err = refine_edit_validate(&e);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "create skill requires arguments");
  free(err);

  /* A skill create with no reference refuses. */
  refi_edit_set(&e, "create", "skill", "unbacked", "t", "c", NULL, NULL,
                kSkillArguments, 0, 5, 9, "why");
  err = refine_edit_validate(&e);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "create skill requires python reference");
  free(err);

  /* A reference that is not a JSON object refuses. */
  refi_edit_set(&e, "create", "skill", "list_reference", "t", "c", NULL,
                "[\"bad\"]", kSkillArguments, 0, 5, 9, "why");
  err = refine_edit_validate(&e);
  ASSERT_NE(err, nullptr);
  /* The provided-object check runs BEFORE the skill-specific lines
     (refinement.test.ts:443-450 asserts the generic wording for skills). */
  EXPECT_STREQ(err, "create requires reference to be an object when provided");
  free(err);

  /* A non-python reference type refuses (refinement.test.ts:404). */
  refi_edit_set(&e, "create", "skill", "shell_skill", "t", "c", NULL,
                "{\"type\":\"shell\",\"command\":\"edit\"}", kSkillArguments, 0, 5,
                9, "why");
  err = refine_edit_validate(&e);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "create skill reference.type must be python");
  free(err);

  /* A python reference with no import refuses. */
  refi_edit_set(&e, "create", "skill", "unimported", "t", "c", NULL,
                "{\"type\":\"python\",\"callable\":\"run\"}", kSkillArguments,
                0, 5, 9, "why");
  err = refine_edit_validate(&e);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "create skill requires python import");
  free(err);

  /* A python reference with no callable/call_pattern refuses. */
  refi_edit_set(&e, "create", "skill", "uncallable", "t", "c", NULL,
                "{\"type\":\"python\",\"import\":\"x\"}", kSkillArguments, 0, 5,
                9, "why");
  err = refine_edit_validate(&e);
  ASSERT_NE(err, nullptr);
  EXPECT_STREQ(err, "create skill requires callable or call_pattern");
  free(err);

  /* A well-formed skill create applies — the contract survives the fold. */
  refi_edit_set(&e, "create", "skill", "chunked_bodies_decode_silently",
                "Chunked bodies decode silently!", "Decode the chunked shape",
                NULL, kSkillReference, kSkillArguments, 0, 12, 14,
                "the chunked-response turn proved it");
  err = refine_edit_apply(&fold, &e, 12);
  ASSERT_EQ(err, nullptr);
  refine_entry_t* entry =
      refine_entry_find(&fold, "skill", "chunked_bodies_decode_silently");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->version, 1u);
  EXPECT_STREQ(entry->reference, kSkillReference);
  EXPECT_STREQ(entry->arguments, kSkillArguments);

  /* A skill UPDATE must carry its arguments too — and may keep the
     reference (refinement.ts:1042: the arguments line fires on updates). */
  refi_edit_set(&e, "update", "skill", "chunked_bodies_decode_silently",
                "Updated", "Updated content", NULL, NULL, kSkillArguments, 1, 12,
                14, "why");
  err = refine_edit_apply(&fold, &e, 13);
  ASSERT_EQ(err, nullptr);
  entry = refine_entry_find(&fold, "skill", "chunked_bodies_decode_silently");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->version, 2u);
  EXPECT_STREQ(entry->reference, kSkillReference) << "NULL keeps the reference";

  refi_edit_clear(&e);
  refine_fold_destroy(&fold);
}