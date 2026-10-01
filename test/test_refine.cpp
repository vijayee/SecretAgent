//
// Created by victor on 10/1/26.
//
// test_refine.cpp — the parity mirror (spec §8): the ported refinement.test.ts
// intents (citations inline), run against pure data first (Task 1/2), the
// store round trips (Task 3), the real review call (Task 4), and the full
// refine_run/refine_rollback cycles (Task 5+7).
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "../src/Util/allocator.h"
#include "../src/Frame/refine.h"
#include "../src/Frame/frame_internal.h"   /* Task 3's _frame_sync_* helpers */
#include "../src/Frame/model_internal.h"   /* Task 4's _model_request_body */
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

/* ------------------------------------------------------------------ */
/* Task 2: the fold's canonical views (fingerprint + bounded digest)   */
/* ------------------------------------------------------------------ */

/* The records scan reply is a JSON ARRAY of TEXT elements: each element is
   a JSON-string-wrapped record document (Task 3 hands the joint text). */
static std::string refi_json_quote(const std::string& raw) {
  std::string out = "\"";
  for (char c : raw) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

/* Parses a records reply composed of well-formed no-edit records (each
   composes its record line, no entries) plus an optional junk element, so
   the view tests exercise record lines and skip lines through the public
   parse. The seqs ride in ascending order — the fold's stored order. */
static void refi_parse_no_edit_records(refine_fold_t* fold, uint64_t first_seq,
                                       uint64_t count, int include_junk) {
  std::string records = "[";
  for (uint64_t seq = first_seq; seq < first_seq + count; seq++) {
    if (seq != first_seq) records += ",";
    std::string rec = "{\"seq\":" + std::to_string(seq) + ",\"id\":\"r" +
                      std::to_string(seq) + "\",\"trigger\":\"trigger " +
                      std::to_string(seq) + "\",\"edits\":[]}";
    records += refi_json_quote(rec);
  }
  if (include_junk) {
    if (count > 0) records += ",";
    records += "\"not a record document\"";
  }
  records += "]";
  ASSERT_EQ(refine_fold_parse(records.c_str(), fold), 0);
}

/* Both canonical views refuse a NULL fold loud and return NULL — the same
   posture as refine_fold_parse and refine_edit_apply. */
TEST(TestRefine, TestCanonicalViewsRefuseNullFold) {
  EXPECT_EQ(refine_fold_fingerprint(NULL), nullptr);
  EXPECT_EQ(refine_fold_digest(NULL), nullptr);
}

/* port of refinement.ts:777-843 (the covered-fields doc) + the delivery
   gate's precondition agent-session.ts:7238-7245: for log-derived folds
   the fingerprint's hashed set covers the digest's printed fields, so the
   gate's precondition inverts safely — equal fingerprint implies equal
   render, and render unequal only when content truly differs. Entry order
   is normalized to (kind,id) sort, so an equal content set planted in a
   different put order stays equal; a non-skill entry whose reference
   changed STAYS equal (the render never prints the contract off-skill); a
   metadata-only change is not possible in our fold (title/seq are not
   carried into a digest byte) — we assert the fold's fields ARE the
   fingerprint's coverage instead: unprinted fields move nothing, printed
   ones differ (the differs-test below). */
TEST(TestRefine, TestFingerprintStableForEqualFold) {
  const char* memory_reference = "{\"note\":\"not rendered off-skill\"}";
  const char* memory_arguments = "{\"also\":\"not rendered off-skill\"}";

  refine_fold_t fold_a;
  mk_fold_init(&fold_a);
  refine_entry_put(&fold_a, "memory", "mid", "Memory title", "memory content",
                   "mem/path", memory_reference, memory_arguments, 2, 0, 4);
  refine_entry_put(&fold_a, "skill", "sid", "Skill title", "skill content",
                   "skill/path", kSkillReference, kSkillArguments, 1, 0, 5);
  refine_entry_put(&fold_a, "prompt", "pid", "Prompt title", "prompt content",
                   "general", "{}", "{}", 3, 0, 6);
  ASSERT_EQ(fold_a.nentries, 3u);
  refi_parse_no_edit_records(&fold_a, 10, 2, 0);

  /* Same content set, REVERSED put order — the same digest, the same
     fingerprint. */
  refine_fold_t fold_b;
  mk_fold_init(&fold_b);
  refine_entry_put(&fold_b, "prompt", "pid", "Prompt title", "prompt content",
                   "general", "{}", "{}", 3, 0, 6);
  refine_entry_put(&fold_b, "skill", "sid", "Skill title", "skill content",
                   "skill/path", kSkillReference, kSkillArguments, 1, 0, 5);
  refine_entry_put(&fold_b, "memory", "mid", "Memory title", "memory content",
                   "mem/path", memory_reference, memory_arguments, 2, 0, 4);
  refi_parse_no_edit_records(&fold_b, 10, 2, 0);

  char* digest_a = refine_fold_digest(&fold_a);
  char* digest_b = refine_fold_digest(&fold_b);
  ASSERT_NE(digest_a, nullptr);
  ASSERT_NE(digest_b, nullptr);
  EXPECT_STREQ(digest_a, digest_b) << "equal content sets render equal digests";
  char* fp_a = refine_fold_fingerprint(&fold_a);
  char* fp_b = refine_fold_fingerprint(&fold_b);
  ASSERT_NE(fp_a, nullptr);
  ASSERT_NE(fp_b, nullptr);
  EXPECT_STREQ(fp_a, fp_b) << "equal renders fingerprint equal";
  EXPECT_EQ(strlen(fp_a), 16u) << "the fingerprint is 16 hex chars";
  free(digest_a);
  free(digest_b);

  /* A non-skill entry's call contract changed — the render never prints it,
     so the fingerprint STAYS equal. */
  refine_fold_t fold_c;
  mk_fold_init(&fold_c);
  refine_entry_put(&fold_c, "memory", "mid", "Memory title", "memory content",
                   "mem/path", "{\"changed\":true}", "{\"changed\":true}", 2, 0,
                   4);
  refine_entry_put(&fold_c, "skill", "sid", "Skill title", "skill content",
                   "skill/path", kSkillReference, kSkillArguments, 1, 0, 5);
  refine_entry_put(&fold_c, "prompt", "pid", "Prompt title", "prompt content",
                   "general", "{}", "{}", 3, 0, 6);
  refi_parse_no_edit_records(&fold_c, 10, 2, 0);
  char* fp_c = refine_fold_fingerprint(&fold_c);
  ASSERT_NE(fp_c, nullptr);
  EXPECT_STREQ(fp_a, fp_c) << "a non-skill contract change is unprinted";

  /* The unprinted entry fields (title, entry seq) move nothing either —
     our fold carries no separate metadata, so coverage IS the fold's
     printed fields. */
  refine_fold_t fold_d;
  mk_fold_init(&fold_d);
  refine_entry_put(&fold_d, "memory", "mid", "OTHER title", "memory content",
                   "mem/path", memory_reference, memory_arguments, 2, 0, 99);
  refine_entry_put(&fold_d, "skill", "sid", "Skill title", "skill content",
                   "skill/path", kSkillReference, kSkillArguments, 1, 0, 5);
  refine_entry_put(&fold_d, "prompt", "pid", "Prompt title", "prompt content",
                   "general", "{}", "{}", 3, 0, 6);
  refi_parse_no_edit_records(&fold_d, 10, 2, 0);
  char* fp_d = refine_fold_fingerprint(&fold_d);
  ASSERT_NE(fp_d, nullptr);
  EXPECT_STREQ(fp_a, fp_d) << "title/entry-seq changes are unprinted here";

  free(fp_a);
  free(fp_b);
  free(fp_c);
  free(fp_d);
  refine_fold_destroy(&fold_a);
  refine_fold_destroy(&fold_b);
  refine_fold_destroy(&fold_c);
  refine_fold_destroy(&fold_d);
}

/* The covered fields that DO render each move the fingerprint: content,
   version, path, the SKILL call contract, and the record material (a
   changed trigger, and reordered record lines — the refinements tail
   renders positionally, so record order is material). */
TEST(TestRefine, TestFingerprintDiffersOnContentAndVersion) {
  refine_fold_t base;
  mk_fold_init(&base);
  refine_entry_put(&base, "memory", "mid", "Memory title", "memory content",
                   "mem/path", "{}", "{}", 2, 0, 4);
  refine_entry_put(&base, "skill", "sid", "Skill title", "skill content",
                   "skill/path", kSkillReference, kSkillArguments, 1, 0, 5);
  refi_parse_no_edit_records(&base, 10, 1, 0);
  char* fp_base = refine_fold_fingerprint(&base);
  ASSERT_NE(fp_base, nullptr);

  for (const char* what : {"content", "version", "path", "skill_contract"}) {
    SCOPED_TRACE(what);
    bool skill_changed = strcmp(what, "skill_contract") == 0;
    refine_fold_t variant;
    mk_fold_init(&variant);
    refine_entry_put(&variant, "memory", "mid", "Memory title",
                     strcmp(what, "content") == 0 ? "CHANGED content"
                                                  : "memory content",
                     strcmp(what, "path") == 0 ? "other/path" : "mem/path",
                     "{}", "{}", strcmp(what, "version") == 0 ? 3u : 2u, 0, 4);
    refine_entry_put(&variant, "skill", "sid", "Skill title", "skill content",
                     "skill/path",
                     skill_changed
                         ? "{\"type\":\"python\",\"import\":\"other\","
                           "\"callable\":\"run\"}"
                         : kSkillReference,
                     skill_changed ? "{\"prompt\":\"changed\"}"
                                   : kSkillArguments,
                     1, 0, 5);
    refi_parse_no_edit_records(&variant, 10, 1, 0);
    char* fp_variant = refine_fold_fingerprint(&variant);
    ASSERT_NE(fp_variant, nullptr);
    EXPECT_STRNE(fp_base, fp_variant) << what << " is fingerprinted";
    free(fp_variant);
    refine_fold_destroy(&variant);
  }

  /* Record material: a different trigger. */
  refine_fold_t retime;
  mk_fold_init(&retime);
  refine_entry_put(&retime, "memory", "mid", "Memory title", "memory content",
                   "mem/path", "{}", "{}", 2, 0, 4);
  refine_entry_put(&retime, "skill", "sid", "Skill title", "skill content",
                   "skill/path", kSkillReference, kSkillArguments, 1, 0, 5);
  refi_parse_no_edit_records(&retime, 11, 1, 0);
  char* fp_retime = refine_fold_fingerprint(&retime);
  ASSERT_NE(fp_retime, nullptr);
  EXPECT_STRNE(fp_base, fp_retime) << "a changed trigger is fingerprinted";
  free(fp_retime);
  refine_fold_destroy(&retime);

  /* And reordered record lines with an equal content set: the digest's
     refinements tail renders positionally, so record order is material —
     equal fingerprints stay render-equal (refinement.ts:777+ "stored
     order"; an order-only change must not reuse the previous digest). */
  refine_fold_t reorder;
  mk_fold_init(&reorder);
  refine_entry_put(&reorder, "memory", "mid", "Memory title", "memory content",
                   "mem/path", "{}", "{}", 2, 0, 4);
  refine_entry_put(&reorder, "skill", "sid", "Skill title", "skill content",
                   "skill/path", kSkillReference, kSkillArguments, 1, 0, 5);
  refi_parse_no_edit_records(&reorder, 10, 2, 0);
  refine_fold_t reordered;
  mk_fold_init(&reordered);
  refine_entry_put(&reordered, "memory", "mid", "Memory title",
                   "memory content", "mem/path", "{}", "{}", 2, 0, 4);
  refine_entry_put(&reordered, "skill", "sid", "Skill title", "skill content",
                   "skill/path", kSkillReference, kSkillArguments, 1, 0, 5);
  refi_parse_no_edit_records(&reordered, 10, 2, 0);
  char* swap_line = reordered.record_lines[0];
  reordered.record_lines[0] = reordered.record_lines[1];
  reordered.record_lines[1] = swap_line;
  char* fp_reorder = refine_fold_fingerprint(&reorder);
  char* fp_reordered = refine_fold_fingerprint(&reordered);
  ASSERT_NE(fp_reorder, nullptr);
  ASSERT_NE(fp_reordered, nullptr);
  EXPECT_STRNE(fp_reorder, fp_reordered)
      << "record lines ride in STORED order (the tail renders positionally)";
  free(fp_reorder);
  free(fp_reordered);
  refine_fold_destroy(&reorder);
  refine_fold_destroy(&reordered);

  free(fp_base);
  refine_fold_destroy(&base);
}

/* port of refinement.ts:20-22's caps + the newest-first render: 7 memory
   entries render 6 plus a "+1 older" overflow line; content compacts
   whitespace and trims at 180; at most 5 newest refinement lines render;
   deleted entries render nothing; a malformed record renders its skip line;
   empty kinds print "0" with no entries. */
TEST(TestRefine, TestDigestRenderBounded) {
  /* The empty fold first: "harness: empty", every kind prints 0, and no
     refinements section. */
  refine_fold_t empty;
  mk_fold_init(&empty);
  char* empty_digest = refine_fold_digest(&empty);
  ASSERT_NE(empty_digest, nullptr);
  EXPECT_STREQ(empty_digest,
               "harness: empty\nprompt: 0\nmemory: 0\nskill: 0\nsubagent: 0")
      << "the empty fold's digest is the all-zero render";
  free(empty_digest);

  refine_fold_t fold;
  mk_fold_init(&fold);
  for (unsigned v = 1; v <= 7; v++) {
    std::string id = "m" + std::to_string(v);
    ASSERT_NE(refine_entry_put(&fold, "memory", id.c_str(), "t",
                               "memory content", "mem/path", "{}", "{}", v, 0,
                               v),
              nullptr);
  }
  /* Tombstones render nothing and count nothing. */
  ASSERT_NE(refine_entry_put(&fold, "memory", "tombstone", "t",
                             "ghost content", "mem/path", "{}", "{}", 1, 1, 8),
            nullptr);
  /* Compaction: whitespace runs collapse, ends trim. */
  ASSERT_NE(refine_entry_put(&fold, "prompt", "p1", "t", "  a\n\t b  c  ",
                             "general", "{}", "{}", 1, 0, 2),
            nullptr);
  /* The trim: a 240-char single-run content renders 180 (177 chars + ...). */
  ASSERT_NE(refine_entry_put(&fold, "skill", "s1", "t",
                             std::string(240, 'x').c_str(), "skill/path",
                             kSkillReference, kSkillArguments, 1, 0, 9),
            nullptr);
  ASSERT_NE(refine_entry_put(&fold, "skill", "gone", "t", "ghost", "skill/path",
                             "{}", "{}", 1, 1, 9),
            nullptr);
  /* 7 records + one malformed element (its skip line rides the tail). */
  refi_parse_no_edit_records(&fold, 1, 7, 1);

  char* digest = refine_fold_digest(&fold);
  ASSERT_NE(digest, nullptr);
  std::string rendered(digest);
  free(digest);

  /* The harness line + per-kind counts. Live entries: 7 memory + 1 prompt
     + 1 skill; tombstones render nothing and count nothing. */
  EXPECT_EQ(rendered.find("harness: 9"), 0u);
  EXPECT_NE(rendered.find("prompt: 1"), std::string::npos);
  EXPECT_NE(rendered.find("memory: 7"), std::string::npos);
  EXPECT_NE(rendered.find("skill: 1"), std::string::npos);
  EXPECT_NE(rendered.find("subagent: 0"), std::string::npos);

  /* 7 memory entries render the 6 NEWEST (m7..m2) + the overflow line. */
  EXPECT_NE(rendered.find("- m7 mem/path v7: memory content"),
            std::string::npos);
  EXPECT_NE(rendered.find("- m2 mem/path v2: memory content"),
            std::string::npos);
  EXPECT_EQ(rendered.find("- m1 "), std::string::npos)
      << "the oldest entry falls past the cap";
  EXPECT_EQ(rendered.find("ghost content"), std::string::npos)
      << "a tombstone renders nothing";
  EXPECT_NE(rendered.find("- +1 older memory entries"), std::string::npos);
  EXPECT_EQ(rendered.find("+2 older"), std::string::npos);

  /* Content compaction + the 180-char trim. */
  EXPECT_NE(rendered.find("- p1 general v1: a b c"), std::string::npos)
      << "whitespace compacts and ends trim";
  EXPECT_NE(rendered.find(std::string(177, 'x') + "..."), std::string::npos)
      << "long content trims to 180 with the ellipsis";
  EXPECT_EQ(rendered.find(std::string(178, 'x')), std::string::npos);

  /* The skill line prints only the digest line shape (no ref/args). */
  EXPECT_NE(rendered.find("- s1 skill/path v1: "), std::string::npos);
  EXPECT_EQ(rendered.find("ref="), std::string::npos);
  EXPECT_EQ(rendered.find("- gone"), std::string::npos)
      << "the deleted skill renders nothing";

  /* Refinements: at most the 5 NEWEST record lines render — 8 stored lines
     tail to seqs 4..7 PLUS the skip line (the newest slot), seqs 1..3 fall
     past the tail. */
  EXPECT_NE(rendered.find("refinements:"), std::string::npos);
  for (uint64_t seq = 4; seq <= 7; seq++) {
    std::string line = "- " + std::to_string(seq) + " trigger " +
                       std::to_string(seq);
    EXPECT_NE(rendered.find(line), std::string::npos)
        << "newest tail line " << line << " renders";
  }
  EXPECT_EQ(rendered.find("- 1 trigger 1"), std::string::npos);
  EXPECT_EQ(rendered.find("- 2 trigger 2"), std::string::npos);
  EXPECT_EQ(rendered.find("- 3 trigger 3"), std::string::npos);

  /* The malformed record renders its skip line (labeled by type, never by
     value — refinement.ts:479-483). */
  EXPECT_NE(rendered.find("harness: skipped malformed refinement record"),
            std::string::npos);
  EXPECT_EQ(rendered.find("not a record document"), std::string::npos);

  /* Exactly 5 refinement lines rendered: the 4 trigger lines + the skip
     line (count the "trigger " occurrences in the refinements tail). */
  size_t refinements_at = rendered.find("refinements:");
  ASSERT_NE(refinements_at, std::string::npos);
  size_t tail_triggers = 0;
  for (size_t at = rendered.find("trigger ", refinements_at);
       at != std::string::npos; at = rendered.find("trigger ", at + 1)) {
    tail_triggers++;
  }
  EXPECT_EQ(tail_triggers, 4u)
      << "the newest-5 tail holds 4 record lines + the skip line";

  /* The render is deterministic. */
  char* again = refine_fold_digest(&fold);
  ASSERT_NE(again, nullptr);
  EXPECT_STREQ(again, rendered.c_str());
  free(again);

  refine_fold_destroy(&fold);
}

/* "refine-fingerprint-..." material prefix: the salt version string rule
   (refinement.ts:25-30) — bumping the render contract's version string
   changes the hex over the same body, so fingerprints minted under
   different versions never compare equal. Nothing new is exposed publicly:
   the test pins the documented material by computing the FNV-1a-64 (basis
   0xcbf29ce484222325, prime 0x100000001b3) over BOTH salt versions itself
   and asserting the public fingerprint matches the "v1" one. */
TEST(TestRefine, TestFingerprintSaltedVersionString) {
  auto fnv64 = [](uint64_t hash, const char* s) -> uint64_t {
    for (size_t i = 0; s[i] != '\0'; i++) {
      hash ^= (uint64_t) (unsigned char) s[i];
      hash *= 0x100000001b3ULL;
    }
    return hash;
  };
  auto hex = [](uint64_t hash) -> std::string {
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) hash);
    return buf;
  };

  refine_fold_t fold;
  mk_fold_init(&fold);
  refine_entry_put(&fold, "memory", "mid", "t", "mc", "mp", "{}", "{}", 2, 0, 3);
  refine_entry_put(&fold, "skill", "sid", "t", "sc", "sp", kSkillReference,
                   kSkillArguments, 1, 0, 4);
  refi_parse_no_edit_records(&fold, 5, 1, 0);

  /* The documented material: entries sorted (kind,id) — memory before
     skill; skill entries alone continue with the call contract; the record
     line rides in stored order. */
  std::string body = std::string("e;memory;mid;2;mp;mc;") +
                     "e;skill;sid;1;sp;sc;r;" + kSkillReference + ";a;" +
                     kSkillArguments + ";l;5;trigger 5;;";
  std::string material_v1 = "refine-fingerprint-v1;" + body;
  std::string material_v2 = "refine-fingerprint-v2;" + body;

  char* fp = refine_fold_fingerprint(&fold);
  ASSERT_NE(fp, nullptr);
  std::string fingerprint(fp);
  free(fp);
  EXPECT_EQ(fingerprint.size(), 16u);
  for (char c : fingerprint) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
        << "the fingerprint is 16 LOWERCASE hex chars: " << fingerprint;
  }
  std::string hex_v1 = hex(fnv64(0xcbf29ce484222325ULL, material_v1.c_str()));
  std::string hex_v2 = hex(fnv64(0xcbf29ce484222325ULL, material_v2.c_str()));
  EXPECT_EQ(fingerprint, hex_v1)
      << "the fingerprint is FNV-1a-64 over the documented salted material";

  /* The bump rule: the same body under a different salt version string
     hashes to different hex — a contract change can never compare equal. */
  EXPECT_NE(hex_v1, hex_v2);
  EXPECT_NE(fingerprint, hex_v2);

  refine_fold_destroy(&fold);
}

/* ------------------------------------------------------------------ */
/* Task 3: the store round trips the fold's log reads ride             */
/* ------------------------------------------------------------------ */

static frame_config_t refi_frame_config(void) {
  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));   /* additive fields default sensibly */
  cfg.model_base_url = NULL;
  cfg.model_api_key = NULL;
  cfg.model_name = "unused";
  cfg.max_depth = 4;
  return cfg;
}

TEST(TestRefine, TestSyncScanReturnsNewestAscendingAsJointArray) {
  /* The store-actor path the fold's parse rides: a scratch in-memory store
     is seeded with record texts under a COMPOSED ABSOLUTE range (never
     relative — the subtree-scan breakage), then _frame_sync_scan runs its
     FRM_STORE_SCAN round trip through the store actor (the single-flight
     sync slot pumps it — the frame.c family's one pump order). The reply is
     ONE malloc'd JSON-ARRAY text of the raw record texts, ascending, exactly
     the seeded count. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);

  /* Seed three refine record docs at <sid>/refine/records/<seq> — via the
     sync batch itself (the seed rides the OTHER new helper; the ops'
     ownership transfers into the round trip). */
  frm_store_op_t* ops = (frm_store_op_t*)get_clear_memory(3 * sizeof(frm_store_op_t));
  for (uint64_t s = 1; s <= 3; s++) {
    std::string rec = "{\"seq\":" + std::to_string(s) + ",\"id\":\"r" +
                      std::to_string(s) + "\",\"trigger\":\"trigger " +
                      std::to_string(s) + "\",\"edits\":[]}";
    ops[s - 1].key =
        strdup((sid + "/refine/records/" + std::to_string(s)).c_str());
    ops[s - 1].value = (uint8_t*)strdup(rec.c_str());
    ops[s - 1].value_len = rec.size();
  }
  EXPECT_EQ(_frame_sync_batch(f, ops, 3, "refine seed test"), 0)
      << "the seed batch committed atomically";

  std::string start = sid + "/refine/records";
  std::string end = sid + "/refine/records0";

  /* One round trip, one joint array text. */
  char* text = NULL;
  ASSERT_EQ(_frame_sync_scan(f, start.c_str(), end.c_str(), 0, &text), 0);
  ASSERT_NE(text, nullptr);
  char* err = NULL;
  json_value_t* arr = json_parse(text, strlen(text), &err);
  if (err != NULL) free(err);
  ASSERT_NE(arr, nullptr) << "raw: " << text;
  ASSERT_EQ(json_type(arr), JSON_ARRAY);
  ASSERT_EQ(json_size(arr), 3u) << "exactly the seeded count: " << text;

  /* Ascending: the seeded record texts ride oldest first, each as a quoted
     string element the fold's parse consumes whole. */
  for (size_t i = 0; i < 3; i++) {
    json_value_t* el = json_at(arr, i);
    ASSERT_NE(el, nullptr);
    ASSERT_EQ(json_type(el), JSON_STRING);
    const char* rec_text = json_as_string(el);
    ASSERT_NE(rec_text, nullptr);
    json_value_t* rec = json_parse(rec_text, strlen(rec_text), NULL);
    ASSERT_NE(rec, nullptr) << "element " << i << " is the raw record text";
    EXPECT_EQ(json_as_int(json_get(rec, "seq")), (int64_t)(1 + i));
    json_value_destroy(rec);
  }
  json_value_destroy(arr);

  /* And the joint text IS the fold's parse input (Task 5's path): */
  refine_fold_t fold;
  memset(&fold, 0, sizeof(fold));
  EXPECT_EQ(refine_fold_parse(text, &fold), 0);
  EXPECT_EQ(fold.nrecords, 3u);
  refine_fold_destroy(&fold);
  free(text);

  /* The newest-records cap: cap 2 returns the NEWEST two (seqs 2, 3), still
     ascending — never the oldest two. */
  char* newest = NULL;
  ASSERT_EQ(_frame_sync_scan(f, start.c_str(), end.c_str(), 2, &newest), 0);
  ASSERT_NE(newest, nullptr);
  err = NULL;
  arr = json_parse(newest, strlen(newest), &err);
  if (err != NULL) free(err);
  ASSERT_NE(arr, nullptr) << "raw: " << newest;
  ASSERT_EQ(json_type(arr), JSON_ARRAY);
  ASSERT_EQ(json_size(arr), 2u);
  for (size_t i = 0; i < 2; i++) {
    json_value_t* el = json_at(arr, i);
    ASSERT_EQ(json_type(el), JSON_STRING);
    json_value_t* rec = json_parse(json_as_string(el), strlen(json_as_string(el)), NULL);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(json_as_int(json_get(rec, "seq")), (int64_t)(2 + i));
    json_value_destroy(rec);
  }
  json_value_destroy(arr);
  free(newest);

  /* An EMPTY range is exactly "[]" (one array text, zero elements). */
  std::string none_start = sid + "/refine/none";
  std::string none_end = sid + "/refine/none0";
  char* empty = NULL;
  ASSERT_EQ(_frame_sync_scan(f, none_start.c_str(), none_end.c_str(), 8, &empty), 0);
  ASSERT_NE(empty, nullptr);
  EXPECT_STREQ(empty, "[]");
  free(empty);

  frame_destroy(f);
  wave_db_close(db);
}

/* ------------------------------------------------------------------ */
/* Task 4: the no-tools review call + the proposal decode (row 18)     */
/* ------------------------------------------------------------------ */

/* PA's TRUNCATED_JSON_ERROR (refinement.ts:198-199, verbatim): the output
   cap's refusal AND the incomplete-JSON diagnosis share the one wording. */
static const char* const kTruncatedJson =
    "the model stopped before completing its JSON object. This usually means "
    "the output budget was exhausted; retry with a smaller request.";

/* The scripted review backend (test_loop.cpp's scripted_complete idiom,
   narrowed to the review's needs): EVERY call captures the tools DOM's
   kind (-1 = a NULL POINTER, never seen here) and the serialized messages
   array, then answers with sm->content as the decoded reply's content —
   the review consumes ONLY content (no tool surface exists to carry a call). */
typedef struct scripted_review_model_t {
  model_backend_t base;
  std::string content;          /* the canned reply content per call */
  std::string captured_messages;
  int tools_kind;               /* the tools ARGUMENT's json_type_e value */
  int calls;
} scripted_review_model_t;

static int scripted_review_complete(void* self, json_value_t* messages,
                                    json_value_t* tools, char** raw_out,
                                    model_reply_t** reply_out, char** error_out) {
  (void)raw_out;
  (void)error_out;
  scripted_review_model_t* sm = (scripted_review_model_t*)self;
  sm->calls++;
  /* The whole point of row 18: the review's tools argument is a JSON-null
     DOM — the EXPLICIT no-tools shape the request builder then omits —
     never a NULL pointer (which would compose the execute tool). */
  sm->tools_kind = (tools == NULL) ? -1 : (int)json_type(tools);
  char* seen = json_serialize(messages);
  if (seen != NULL) {
    sm->captured_messages.assign(seen);
    free(seen);
  }
  model_reply_t* r = (model_reply_t*)get_clear_memory(sizeof(model_reply_t));
  r->content = refi_dup(sm->content.c_str());
  r->tool_code = NULL;
  r->finish_reason = refi_dup("stop");
  *reply_out = r;
  return 0;
}

/* The model.c JSON-null omission, through the exported test surface: a
   request built with tools = json_new_null() carries NEITHER a "tools"
   nor a "tool_choice" key, while a build with tools = NULL (the pointer,
   unchanged behavior) still carries the execute tool. The model/tool-free
   body must be a valid request otherwise (model key + messages array
   present, the tools-array shape carried verbatim with tool_choice auto). */
TEST(TestRefine, TestRequestBodyOmitsToolsOnJsonNull) {
  json_value_t* msgs = json_new_array();
  json_value_t* m = json_new_object();
  json_object_set(m, "role", json_new_string("user"));
  json_object_set(m, "content", json_new_string("review the trajectory"));
  json_array_append(msgs, m);
  ASSERT_EQ(json_size(msgs), 1u);

  /* The JSON-null tools: BOTH tool keys OMITTED from the document. */
  json_value_t* no_tools = json_new_null();
  char* body = NULL;
  char* err = NULL;
  ASSERT_EQ(_model_request_body("review-model", msgs, no_tools, &body, &err), 0)
      << (err != NULL ? err : "(no error string)");
  ASSERT_NE(body, nullptr);
  json_value_t* req = json_parse(body, strlen(body), &err);
  ASSERT_NE(req, nullptr) << "the tool-free body is a valid JSON request: " << body;
  ASSERT_EQ(json_type(req), JSON_OBJECT);
  json_value_t* model_v = json_get(req, "model");
  ASSERT_NE(model_v, nullptr);
  ASSERT_EQ(json_type(model_v), JSON_STRING);
  EXPECT_STREQ(json_as_string(model_v), "review-model");
  json_value_t* msgs_out = json_get(req, "messages");
  ASSERT_NE(msgs_out, nullptr);
  ASSERT_EQ(json_type(msgs_out), JSON_ARRAY);
  ASSERT_EQ(json_size(msgs_out), 1u);
  EXPECT_EQ(json_get(req, "tools"), nullptr)
      << "a JSON-null tools argument carries NO tools key";
  EXPECT_EQ(json_get(req, "tool_choice"), nullptr)
      << "a JSON-null tools argument carries NO tool_choice key";
  json_value_destroy(req);
  free(body);

  /* The NULL-POINTER shape is UNCHANGED (the turn loop's contract): the
     canned execute tool rides with tool_choice auto. */
  char* body2 = NULL;
  err = NULL;
  ASSERT_EQ(_model_request_body("review-model", msgs, NULL, &body2, &err), 0);
  ASSERT_NE(body2, nullptr);
  req = json_parse(body2, strlen(body2), &err);
  ASSERT_NE(req, nullptr) << "the execute-tool body is a valid request: " << body2;
  json_value_t* tools_out = json_get(req, "tools");
  ASSERT_NE(tools_out, nullptr);
  ASSERT_EQ(json_type(tools_out), JSON_ARRAY);
  ASSERT_EQ(json_size(tools_out), 1u);
  json_value_t* tool0 = json_at(tools_out, 0);
  ASSERT_NE(tool0, nullptr);
  json_value_t* fn = json_get(tool0, "function");
  ASSERT_NE(fn, nullptr);
  json_value_t* name = json_get(fn, "name");
  ASSERT_NE(name, nullptr);
  EXPECT_STREQ(json_as_string(name), "execute");
  json_value_t* choice = json_get(req, "tool_choice");
  ASSERT_NE(choice, nullptr);
  EXPECT_STREQ(json_as_string(choice), "auto");
  json_value_destroy(req);
  free(body2);

  /* The tools-ARRAY shape is unchanged too: the given array rides
     verbatim (model.c's documented superset shape). */
  json_value_t* scripted_tools = json_new_array();
  json_value_t* cell = json_new_object();
  json_object_set(cell, "type", json_new_string("function"));
  json_object_set(cell, "name", json_new_string("probe_tool"));
  json_array_append(scripted_tools, cell);
  char* body3 = NULL;
  err = NULL;
  ASSERT_EQ(_model_request_body("review-model", msgs, scripted_tools, &body3, &err), 0);
  ASSERT_NE(body3, nullptr);
  req = json_parse(body3, strlen(body3), &err);
  ASSERT_NE(req, nullptr);
  json_value_t* carried = json_get(req, "tools");
  ASSERT_NE(carried, nullptr);
  ASSERT_EQ(json_size(carried), 1u);
  json_value_t* carried_name = json_get(json_at(carried, 0), "name");
  ASSERT_NE(carried_name, nullptr);
  EXPECT_STREQ(json_as_string(carried_name), "probe_tool")
      << "the array tools ride verbatim";
  EXPECT_NE(json_get(req, "tool_choice"), nullptr);
  json_value_destroy(req);
  json_value_destroy(scripted_tools);
  free(body3);

  json_value_destroy(no_tools);
  json_value_destroy(msgs);
}

/* The scripted review backend (test_loop.cpp's scripted_complete idiom) is
   invoked BY refine_review and ASSERTS it received a tools DOM whose type
   is JSON_NULL (not a NULL pointer) — the end-to-end shape guard (the
   model never meets refine as a tool, spec §7). */
TEST(TestRefine, TestReviewCallPassesJsonNullTools) {
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);

  /* Seed the trajectory the review will read: two msg.append events whose
     text the bounded view must carry. */
  ASSERT_EQ(frame_append_msg(f, "user", "the chunked body decoded in place"), 0);
  ASSERT_EQ(frame_append_msg(f, "assistant", "noted the chunked-body lesson"), 0);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;   /* the sync-scripted shape */
  sm.content =
      "{\"summary\":\"Chunked bodies decode in place\","
      "\"rationale\":\"The chunked-body turn proved decoding happens in place\","
      "\"edits\":[{\"action\":\"create\",\"kind\":\"memory\","
      "\"title\":\"Chunked bodies decode silently\","
      "\"content\":\"A chunked body decodes in place\","
      "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"the "
      "chunked-body turn\"},\"reason\":\"the chunked turn proved it\"}]}";
  frame_set_model_backend(f, &sm.base);

  refine_fold_t fold;
  mk_fold_init(&fold);
  ASSERT_NE(mk_fold_seed(&fold, "memory", "chunked_bodies", 1), nullptr);

  char* traj = NULL;
  refine_edit_t* edits = NULL;
  size_t nedits = 99;   /* sentinel: the call zeroes every out-slot first */
  char* summary = NULL;
  char* rationale = NULL;
  char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(),
                                     "focus on chunked bodies", &traj, &edits,
                                     &nedits, &summary, &rationale);
  ASSERT_EQ(refusal, nullptr) << "the scripted review decodes: "
                              << (refusal != NULL ? refusal : "(none)");
  ASSERT_EQ(sm.calls, 1);

  /* THE SHAPE GUARD: the backend received a JSON_NULL tools DOM. */
  EXPECT_EQ(sm.tools_kind, (int)JSON_NULL)
      << "tools is a JSON-null DOM, never a NULL pointer (-1) nor an array";

  /* The bounded trajectory view the review consumed rides back to the
     caller (produced HERE, freed by the CALLER). */
  ASSERT_NE(traj, nullptr);
  EXPECT_NE(strstr(traj, "the chunked body decoded in place"), nullptr)
      << "the trajectory view carries the seeded event text: " << traj;
  char* perr = NULL;
  json_value_t* traj_arr = json_parse(traj, strlen(traj), &perr);
  if (perr != NULL) free(perr);
  ASSERT_NE(traj_arr, nullptr) << "the trajectory view stays parseable JSON";
  ASSERT_EQ(json_type(traj_arr), JSON_ARRAY);
  ASSERT_GE(json_size(traj_arr), 2u) << "both seeded events ride: " << traj;
  json_value_destroy(traj_arr);

  /* The review prompt carried the spec §3 step-4 sections (the captured
     messages array). */
  const std::string& seen = sm.captured_messages;
  EXPECT_NE(seen.find("<current_harness_state>"), std::string::npos);
  EXPECT_NE(seen.find("- chunked_bodies"), std::string::npos)
      << "the current digest's entry line rides in the digest section";
  EXPECT_NE(seen.find("<refinement_history>"), std::string::npos);
  EXPECT_NE(seen.find("No prior refinement history."), std::string::npos)
      << "an empty fold renders the empty-history line";
  EXPECT_NE(seen.find("<trajectory>"), std::string::npos);
  EXPECT_NE(seen.find("the chunked body decoded in place"), std::string::npos);
  EXPECT_NE(seen.find("<user_refine_instructions>"), std::string::npos);
  EXPECT_NE(seen.find("focus on chunked bodies"), std::string::npos);
  EXPECT_EQ(seen.find("<shared_harness_context>"), std::string::npos)
      << "an empty shared scope adds no read-only-context section";

  /* The decoded proposal: one evidence-backed memory create. */
  ASSERT_EQ(nedits, 1u);
  ASSERT_NE(edits, nullptr);
  EXPECT_STREQ(edits[0].action, "create");
  EXPECT_STREQ(edits[0].kind, "memory");
  EXPECT_EQ(edits[0].id, nullptr)
      << "a create's id stays NULL — the slug derives at apply time "
         "(refine.h: id NULL on create = slug(title, kind))";
  EXPECT_STREQ(edits[0].title, "Chunked bodies decode silently");
  EXPECT_STREQ(edits[0].content, "A chunked body decodes in place");
  EXPECT_EQ(edits[0].expect_version, 0u);
  EXPECT_EQ(edits[0].evidence_first, 1u);
  EXPECT_EQ(edits[0].evidence_last, 2u);
  EXPECT_STREQ(edits[0].reason, "the chunked turn proved it")
      << "the edit's top-level reason feeds the record's evidence summary";
  EXPECT_NE(summary, nullptr);
  EXPECT_STREQ(summary, "Chunked bodies decode in place");
  EXPECT_NE(rationale, nullptr);
  EXPECT_STREQ(rationale, "The chunked-body turn proved decoding happens in place");

  for (size_t i = 0; i < nedits; i++) refine_edit_destroy(&edits[i]);
  free(edits);
  free(traj);
  free(summary);
  free(rationale);
  refine_fold_destroy(&fold);
  frame_destroy(f);
  wave_db_close(db);
}

/* The trajectory's bounded tail slice (PA planRefinement's .slice(-80_000),
   refinement.ts:1258): an over-cap joined scan text drops WHOLE OLDEST
   records — never a mid-document cut — so the view stays a parseable array
   holding a CONTIGUOUS newest run, never exceeds
   SA_REFINE_TRAJECTORY_CHARS, and the newest record always survives. */
TEST(TestRefine, TestReviewTrajectoryTailSliceDropsWholeOldestRecords) {
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);

  /* Seed 24 bulk event records (~4 KB each, ~98 KB joined — past the
     80,000-char cap but inside the frame's 120 KB seed batch), the oldest
     carrying marker_0, the newest marker_23. The keys mirror frame.c's
     REAL event-key shape ("events/%020llu") so the scan's ascending order
     is the numeric seq order. */
  frm_store_op_t* ops =
      (frm_store_op_t*)get_clear_memory(24 * sizeof(frm_store_op_t));
  char zkey[32];
  for (uint64_t s = 0; s < 24; s++) {
    std::string rec =
        "{\"seq\":" + std::to_string(s) + ",\"type\":\"probe.bulk\","
        "\"payload\":{\"blob\":\"" + std::string(4000, 'x') +
        "\",\"marker\":\"marker_" + std::to_string(s) + "\"}}";
    snprintf(zkey, sizeof(zkey), "%020llu", (unsigned long long)s);
    ops[s].key = strdup((sid + "/events/" + zkey).c_str());
    ops[s].value = (uint8_t*)strdup(rec.c_str());
    ops[s].value_len = rec.size();
  }
  EXPECT_EQ(_frame_sync_batch(f, ops, 24, "bulk event seed"), 0);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.content =
      "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":[]}";
  frame_set_model_backend(f, &sm.base);

  refine_fold_t fold;
  mk_fold_init(&fold);
  char* traj = NULL;
  refine_edit_t* edits = NULL;
  size_t nedits = 0;
  char* summary = NULL;
  char* rationale = NULL;
  char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                     &traj, &edits, &nedits, &summary,
                                     &rationale);
  ASSERT_EQ(refusal, nullptr);
  ASSERT_NE(traj, nullptr);
  ASSERT_LE(strlen(traj), (size_t)SA_REFINE_TRAJECTORY_CHARS)
      << "the bounded view never exceeds the cap";
  ASSERT_GE(strlen(traj), 2u);

  /* Parseable array, a contiguous NEWEST run: kept seqs are exactly
     (24 - size)..23 in ascending order. */
  char* perr = NULL;
  json_value_t* arr = json_parse(traj, strlen(traj), &perr);
  if (perr != NULL) free(perr);
  ASSERT_NE(arr, nullptr) << "the sliced view stays parseable: " << traj;
  ASSERT_EQ(json_type(arr), JSON_ARRAY);
  size_t kept = json_size(arr);
  ASSERT_GE(kept, 1u);
  ASSERT_LT(kept, 24u) << "the cap DROPPED oldest records (the seed overflows)";
  for (size_t i = 0; i < kept; i++) {
    json_value_t* rec =
        json_parse(json_as_string(json_at(arr, i)), strlen(json_as_string(json_at(arr, i))), NULL);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(json_as_int(json_get(rec, "seq")), (int64_t)(24 - (kept - i)))
        << "the kept run is the contiguous newest block";
    json_value_destroy(rec);
  }
  json_value_destroy(arr);

  /* The newest record survives; the oldest records fall past the cap. */
  EXPECT_NE(strstr(traj, "marker_23"), nullptr);
  EXPECT_EQ(strstr(traj, "marker_0"), nullptr);

  /* The review consumed exactly the bounded view (the prompt section). */
  EXPECT_NE(sm.captured_messages.find("marker_23"), std::string::npos);
  EXPECT_EQ(sm.captured_messages.find("marker_0"), std::string::npos);

  free(traj);
  free(edits);   /* a zero-edit proposal sets none to free */
  free(summary);
  free(rationale);
  refine_fold_destroy(&fold);
  frame_destroy(f);
  wave_db_close(db);
}

/* The proposal decode: a fenced reply ("```json ... ```") parses after the
   trim; > SA_REFINE_MAX_EDITS edits refuse with the PA truncation wording
   (kTruncatedJson — refinement.ts:199's shape; the cap is the refuse-loud
   stand-in for the output reserve, spec §4); a non-JSON reply refuses loud;
   a closer-heavy reply names the valid-JSON cause, never the truncation one;
   edits missing required fields refuse with the validateEdit strings while
   edits missing/wrong-typing the decode's own fields refuse with the decode
   shape's own line; non-object top-level fields fall to the defaults. */
TEST(TestRefine, TestProposalDecodeCapsAndFences) {
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  frame_set_model_backend(f, &sm.base);

  refine_fold_t fold;
  mk_fold_init(&fold);

  /* 1. The fence: a prose-wrapped "```json ... ```" reply parses after the
        trim (the decode's fence-trim; the JSON-only contract tolerated at
        the fence the models actually emit). */
  sm.content =
      "Here is my proposal:\n```json\n"
      "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":["
      "{\"action\":\"create\",\"kind\":\"memory\",\"title\":\"Fenced lesson\","
      "\"content\":\"the fenced lesson content\","
      "\"evidence\":{\"first_seq\":1,\"last_seq\":2}}]}\n```\nGood luck.";
  {
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_EQ(refusal, nullptr) << (refusal != NULL ? refusal : "(none)");
    ASSERT_EQ(nedits, 1u);
    ASSERT_NE(edits, nullptr);
    EXPECT_STREQ(edits[0].title, "Fenced lesson");
    EXPECT_EQ(edits[0].id, nullptr) << "the create's slug derives at apply";
    EXPECT_STREQ(edits[0].content, "the fenced lesson content");
    EXPECT_EQ(edits[0].evidence_first, 1u);
    EXPECT_EQ(edits[0].evidence_last, 2u);
    EXPECT_STREQ(summary, "s");
    EXPECT_STREQ(rationale, "r");
    for (size_t i = 0; i < nedits; i++) refine_edit_destroy(&edits[i]);
    free(edits);
    free(traj);
    free(summary);
    free(rationale);
  }

  /* 2. The cap: a proposal with MORE than SA_REFINE_MAX_EDITS edits
        refuses with PA's truncation wording — refuse-loud, never a silent
        truncation to the first 8. */
  {
    std::string proposal =
        "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":[";
    for (int i = 0; i <= SA_REFINE_MAX_EDITS; i++) {
      if (i > 0) proposal += ",";
      proposal += "{\"action\":\"create\",\"kind\":\"memory\",\"title\":"
                  "\"cap edit " + std::to_string(i) + "\",\"content\":\"c\","
                  "\"evidence\":{\"first_seq\":1,\"last_seq\":2}}";
    }
    proposal += "]}";
    sm.content = proposal;
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_NE(refusal, nullptr) << "the over-cap proposal refuses loud";
    EXPECT_STREQ(refusal, kTruncatedJson);
    free(refusal);
    EXPECT_EQ(nedits, 0u) << "no partial decode on the cap refusal";
    EXPECT_EQ(edits, nullptr);
    EXPECT_EQ(traj, nullptr) << "a refused review leaves NO outs set";
    free(summary);
    free(rationale);
  }

  /* 3. A non-JSON reply refuses loud: a brace-free prose reply carries PA's
        fall-through wording (refinement.ts:963's "Refiner did not return a
        JSON object"); a brace-bearing but malformed one carries the
        "the model did not return valid JSON: <reason>" line. */
  {
    sm.content =
        "I reviewed the trajectory and found nothing worth persisting.";
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_NE(refusal, nullptr);
    EXPECT_STREQ(refusal, "Refiner did not return a JSON object");
    free(refusal);
    EXPECT_EQ(traj, nullptr);
    EXPECT_EQ(edits, nullptr);
    EXPECT_EQ(summary, nullptr);
    EXPECT_EQ(rationale, nullptr);
  }

  /* 3b. A brace-bearing but syntactically malformed reply refuses with the
         PA valid-JSON line (balanced braces, so NOT the truncation
         diagnosis; the json reason rides). */
  {
    sm.content = "{\"summary\":\"s\",\"edits\":[]}{invalid json}";
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_NE(refusal, nullptr);
    EXPECT_NE(strstr(refusal, "the model did not return valid JSON: "), nullptr)
        << refusal;
    free(refusal);
    EXPECT_EQ(traj, nullptr);
    EXPECT_EQ(edits, nullptr);
    EXPECT_EQ(summary, nullptr);
    EXPECT_EQ(rationale, nullptr);
  }

  /* 3c. A closer-heavy reply (more closers than openers) is never
         misdiagnosed as truncated: the scan's depth floor keeps the
         balanced-shape answer, and the valid-JSON cause rides. */
  {
    sm.content =
        "{\"summary\":\"s\",\"edits\":[]}]}";
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_NE(refusal, nullptr);
    EXPECT_NE(strstr(refusal, "the model did not return valid JSON: "), nullptr)
        << refusal;
    EXPECT_EQ(strstr(refusal, kTruncatedJson), nullptr)
        << "a closer-heavy reply is NOT named truncated";
    free(refusal);
    EXPECT_EQ(traj, nullptr);
    EXPECT_EQ(edits, nullptr);
    EXPECT_EQ(summary, nullptr);
    EXPECT_EQ(rationale, nullptr);
  }

  /* 4. A truncated fence — no closing ``` — whose JSON never completes
        names the PA truncation cause (the incomplete-JSON diagnosis). */
  {
    sm.content =
        "```json\n{\"summary\":\"s\",\"edits\":[{\"action\":\"cr";
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_NE(refusal, nullptr);
    EXPECT_STREQ(refusal, kTruncatedJson);
    free(refusal);
    EXPECT_EQ(traj, nullptr);
  }

  /* 5. Field-shape refusals carry the validateEdit strings (the DECODE
        refuses loud — no partial decode). */
  const struct {
    const char* what;
    const char* edit_json;
    const char* expect_refusal;
  } fields[] = {
      {"update without title/content",
       "{\"action\":\"update\",\"kind\":\"memory\",\"id\":\"mid\","
       "\"evidence\":{\"first_seq\":1,\"last_seq\":2}}",
       "update requires title and content"},
      {"delete without id",
       "{\"action\":\"delete\",\"kind\":\"memory\","
       "\"evidence\":{\"first_seq\":1,\"last_seq\":2}}",
       "delete requires id"},
      {"unsupported kind",
       "{\"action\":\"create\",\"kind\":\"note\",\"title\":\"t\",\"content\":\"c\","
       "\"evidence\":{\"first_seq\":1,\"last_seq\":2}}",
       "unsupported kind note"},
      {"unsupported action",
       "{\"action\":\"patch\",\"kind\":\"memory\",\"title\":\"t\",\"content\":\"c\","
       "\"evidence\":{\"first_seq\":1,\"last_seq\":2}}",
       "unsupported action patch"},
      {"the base prompt id",
       "{\"action\":\"update\",\"kind\":\"prompt\",\"id\":\"base_system_prompt\","
       "\"title\":\"t\",\"content\":\"c\","
       "\"evidence\":{\"first_seq\":1,\"last_seq\":2}}",
       "base system prompt is not editable"},
  };
  for (const auto& field : fields) {
    SCOPED_TRACE(field.what);
    std::string proposal =
        std::string("{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":[") +
        field.edit_json + "]}";
    sm.content = proposal;
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_NE(refusal, nullptr) << "a field-shape refusal refuses the decode";
    EXPECT_STREQ(refusal, field.expect_refusal);
    free(refusal);
    EXPECT_EQ(edits, nullptr);
    EXPECT_EQ(traj, nullptr);
    free(summary);
    free(rationale);
  }

  /* 5b. THE DECODE'S OWN SHAPE REFUSALS (missing or wrong-typed action /
         kind) refuse loud with the decode's own line — never the implicit
         fall-through to refine_edit_validate's internal "(null)" catch. */
  const struct {
    const char* what;
    const char* edit_json;
  } shapes[] = {
      {"missing action",
       "{\"kind\":\"memory\",\"title\":\"t\",\"content\":\"c\","
       "\"evidence\":{\"first_seq\":1,\"last_seq\":2}}"},
      {"wrong-typed kind",
       "{\"action\":\"create\",\"kind\":5,\"title\":\"t\",\"content\":\"c\","
       "\"evidence\":{\"first_seq\":1,\"last_seq\":2}}"},
  };
  for (const auto& shape : shapes) {
    SCOPED_TRACE(shape.what);
    std::string proposal =
        std::string("{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":[") +
        shape.edit_json + "]}";
    sm.content = proposal;
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_NE(refusal, nullptr) << "a decode-shape refusal refuses the decode";
    EXPECT_STREQ(refusal, "edit element is malformed");
    free(refusal);
    EXPECT_EQ(edits, nullptr);
    EXPECT_EQ(traj, nullptr);
    free(summary);
    free(rationale);
  }

  /* 6. The EMPTY edits proposal is a VALID decode (zero edits — the no-op
        proposal PA's contract asks for; Task 5's no-op rule consumes it). */
  {
    sm.content =
        "{\"summary\":\"nothing to persist\",\"rationale\":\"no "
        "evidence-backed lessons in the trajectory\",\"edits\":[]}";
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_EQ(refusal, nullptr) << "an empty edits proposal is a valid decode";
    EXPECT_EQ(nedits, 0u);
    EXPECT_EQ(edits, nullptr);
    EXPECT_STREQ(summary, "nothing to persist");
    EXPECT_STREQ(rationale, "no evidence-backed lessons in the trajectory");
    free(traj);
    free(summary);
    free(rationale);
  }

  /* 7. The decode defaults: a NON-OBJECT summary and rationale fall to PA's
        normalize defaults, and a JSON-NULL edits field is the valid
        zero-edit proposal (the same shape a missing edits field carries). */
  {
    sm.content = "{\"summary\":5,\"rationale\":[\"no\"],\"edits\":null}";
    char* traj = NULL;
    refine_edit_t* edits = NULL;
    size_t nedits = 0;
    char* summary = NULL;
    char* rationale = NULL;
    char* refusal = refine_review_call(f, &fold, (sid + "/harness").c_str(), NULL,
                                       &traj, &edits, &nedits, &summary,
                                       &rationale);
    ASSERT_EQ(refusal, nullptr)
        << "a null edits proposal is a valid decode: "
        << (refusal != NULL ? refusal : "(none)");
    EXPECT_EQ(nedits, 0u);
    EXPECT_EQ(edits, nullptr);
    EXPECT_STREQ(summary, "Refined continual harness state")
        << "a non-object summary falls to the PA default";
    EXPECT_STREQ(rationale, "")
        << "a non-object rationale falls to the PA default";
    free(traj);
    free(summary);
    free(rationale);
  }

  refine_fold_destroy(&fold);
  frame_destroy(f);
  wave_db_close(db);
}