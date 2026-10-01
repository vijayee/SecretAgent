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
#include "../src/Scheduler/scheduler.h"    /* the pooled-store refusal's pool */
}

#include <string>
#include <vector>

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
/* ------------------------------------------------------------------ */
/* Task 5: the whole cycles (refine_run + refine_rollback, the parity   */
/* rows 10-17) — every test drives the REAL round trips through the     */
/* store actor (the sync helpers pump themselves; no hand pumping).     */
/* ------------------------------------------------------------------ */

/* A bounded scan's reply as the raw record VALUE texts, ascending. Empty
   vector when the range is empty (or the scan refused — the tests assert
   the contents, never silently pass on an empty reply). */
static std::vector<std::string> refi_scan_values(frame_t* f,
                                                 const std::string& start,
                                                 const std::string& end,
                                                 size_t cap) {
  std::vector<std::string> out;
  char* raw = NULL;
  if (_frame_sync_scan(f, start.c_str(), end.c_str(), cap, &raw) != 0) return out;
  if (raw == NULL) return out;
  char* err = NULL;
  json_value_t* arr = json_parse(raw, strlen(raw), &err);
  if (err != NULL) free(err);
  free(raw);
  if (arr == NULL || json_type(arr) != JSON_ARRAY) {
    json_value_destroy(arr);
    return out;
  }
  for (size_t i = 0; i < json_size(arr); i++) {
    json_value_t* el = json_at(arr, i);
    if (el != NULL && json_type(el) == JSON_STRING) {
      out.emplace_back(json_as_string(el));
    }
  }
  json_value_destroy(arr);
  return out;
}

/* A meta key's stored text ("" = absent). */
static std::string refi_stored_meta(frame_t* f, const std::string& scope_root,
                                    const char* meta) {
  std::string key = scope_root + "/meta/" + meta;
  auto vals = refi_scan_values(f, key, key + "0", 1);
  return vals.empty() ? std::string() : vals[0];
}

/* The harness log's key (the events discipline's zero pad — the SAME
   composition refine.c's _refine_log_key rides; the test composes it to
   read the store, never to write around it). */
static std::string refi_log_key(const std::string& root, uint64_t seq) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s/log/%020llu", root.c_str(),
           (unsigned long long) seq);
  return buf;
}

/* One JSON field: the ""-default string form. */
static std::string refi_str(json_value_t* obj, const char* key) {
  json_value_t* v = json_get(obj, key);
  return (v != NULL && json_type(v) == JSON_STRING) ? json_as_string(v) : "";
}

/* The parsed record DOM helper (the caller's to destroy). */
static json_value_t* refi_parse_record(const std::string& text) {
  return json_parse(text.c_str(), text.size(), NULL);
}

/* A seed record's text as the fold WOULD have composed from one applied
   edit — the harness log's seed for the store-side tests (a REAL record
   shape, not a fixture shortcut: the fold re-applies its edits through
   refine_edit_apply). Plain string fields only (no escaping needed). */
static std::string refi_seed_record(uint64_t seq, const char* id,
                                    const char* trigger,
                                    const char* edits_json) {
  std::string rec = "{\"seq\":" + std::to_string(seq) + ",\"id\":\"" + id +
                    "\",\"trigger\":\"" + trigger + "\",\"rollbackOf\":null,"
                    "\"evidence\":{\"session\":\"s\",\"first_seq\":1,"
                    "\"last_seq\":2,\"summary\":\"the seed\"},\"edits\":[" +
                    (edits_json != NULL ? edits_json : "") +
                    "],\"at\":\"2026-10-01T12:00:00Z\"}";
  return rec;
}

/* One APPLIED edit element inside a seed record (expect + evidence + the
   before snapshot explicit). before_json may be NULL = "null". */
static std::string refi_seed_edit(const char* action, const char* kind,
                                  const char* id, const char* title,
                                  const char* content, const char* path,
                                  unsigned expect_version,
                                  uint64_t ev_first, uint64_t ev_last,
                                  const char* reason, const char* before_json) {
  std::string el = "{\"action\":\"" + std::string(action) + "\",\"kind\":\"" +
                   kind + "\",\"id\":\"" + id + "\",";
  if (title != NULL) el += std::string("\"title\":\"") + title + "\",";
  if (content != NULL) el += std::string("\"content\":\"") + content + "\",";
  if (path != NULL) el += std::string("\"path\":\"") + path + "\",";
  if (strcmp(action, "create") != 0) {
    el += "\"expect\":{\"version\":" + std::to_string(expect_version) + "},";
  }
  el += "\"evidence\":{\"first_seq\":" + std::to_string(ev_first) +
        ",\"last_seq\":" + std::to_string(ev_last) + ",\"summary\":\"" +
        reason + "\"},\"before\":" +
        (before_json != NULL ? before_json : "null") +
        ",\"applied\":true,\"error\":null}";
  return el;
}

/* The create lesson many Task 5 tests re-use (the chunked-body id). */
static const char* const kChunkedCreateProposal =
    "{\"summary\":\"Chunked bodies decode in place\","
    "\"rationale\":\"The chunked-body turn proved decoding happens in "
    "place\",\"edits\":[{\"action\":\"create\",\"kind\":\"memory\","
    "\"title\":\"Chunked bodies decode silently\","
    "\"content\":\"A chunked body decodes in place\","
    "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"the "
    "chunked-body turn\"},\"reason\":\"the chunked turn proved it\"}]}";

TEST(TestRefine, TestRefineRunCommitsOneAtomicBatch) {
  /* A scripted review backend returns a canned proposal; one refine_run
     call; the store then holds the record + entry + meta trio in ONE
     batch — the seqs line up: the record's edits == the materialized
     entries, and the stored meta derives EXACTLY from the log. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  std::string root = sid + "/harness";

  ASSERT_EQ(frame_append_msg(f, "user", "the chunked body decoded in place"), 0);
  ASSERT_EQ(frame_append_msg(f, "assistant", "noted the chunked-body lesson"), 0);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  sm.content = kChunkedCreateProposal;
  frame_set_model_backend(f, &sm.base);

  char* summary = NULL;
  int rc = refine_run(f, "focus on chunked bodies", 0, &summary);
  ASSERT_EQ(rc, 0) << "the commit path returns 0";
  ASSERT_NE(summary, nullptr);
  std::string sum(summary);
  free(summary);

  /* The frozen summary shape (the record id is the minted one — extract
     it and pin the record to the SAME id). */
  std::string committed_marker = " committed (1/1 edits applied)";
  size_t id_at = sum.find("refine: refine_");
  ASSERT_EQ(id_at, 0u) << "the header line: " << sum;
  size_t committed_at = sum.find(committed_marker);
  ASSERT_NE(committed_at, std::string::npos) << sum;
  size_t id_begin = strlen("refine: ");
  std::string record_id = sum.substr(id_begin, committed_at - id_begin);
  EXPECT_EQ(record_id.find("refine_"), 0u) << "the mint: " << record_id;
  EXPECT_NE(sum.find("  apply memory local:chunked_bodies_decode_silently "
                     "v1: the chunked turn proved it"), std::string::npos)
      << "the apply line: " << sum;
  EXPECT_NE(sum.find("digest changed: yes"), std::string::npos) << sum;
  EXPECT_EQ(sum.find("refuse "), std::string::npos) << sum;

  /* The log record (one record; the trio's seqs line up): */
  auto records = refi_scan_values(f, root + "/log", root + "/log0", 0);
  ASSERT_EQ(records.size(), 1u);
  json_value_t* rec = refi_parse_record(records[0]);
  ASSERT_NE(rec, nullptr) << records[0];
  EXPECT_EQ(json_as_int(json_get(rec, "seq")), (int64_t)1);
  EXPECT_EQ(refi_str(rec, "id"), record_id);
  EXPECT_EQ(refi_str(rec, "scope"), "local");
  EXPECT_EQ(refi_str(rec, "trigger"), "Chunked bodies decode in place");
  EXPECT_EQ(json_type(json_get(rec, "rollbackOf")), JSON_NULL);
  json_value_t* evidence = json_get(rec, "evidence");
  ASSERT_NE(evidence, nullptr);
  EXPECT_EQ(refi_str(evidence, "session"), sid);
  EXPECT_EQ(json_as_int(json_get(evidence, "first_seq")), (int64_t)1);
  EXPECT_EQ(json_as_int(json_get(evidence, "last_seq")), (int64_t)2);
  EXPECT_EQ(refi_str(evidence, "summary"),
            "The chunked-body turn proved decoding happens in place");
  json_value_t* edits = json_get(rec, "edits");
  ASSERT_NE(edits, nullptr);
  ASSERT_EQ(json_size(edits), 1u);
  json_value_t* el = json_at(edits, 0);
  EXPECT_EQ(refi_str(el, "action"), "create");
  EXPECT_EQ(refi_str(el, "kind"), "memory");
  EXPECT_EQ(refi_str(el, "id"), "chunked_bodies_decode_silently");
  EXPECT_EQ(refi_str(el, "path"), "general");
  EXPECT_EQ(json_as_int(json_get(el, "version")), (int64_t)1);
  EXPECT_EQ(json_as_bool(json_get(el, "applied")), 1);
  ASSERT_EQ(json_type(json_get(el, "error")), JSON_NULL);
  ASSERT_EQ(json_type(json_get(el, "before")), JSON_NULL);
  json_value_t* el_ev = json_get(el, "evidence");
  ASSERT_NE(el_ev, nullptr);
  EXPECT_EQ(json_as_int(json_get(el_ev, "first_seq")), (int64_t)1);
  EXPECT_EQ(json_as_int(json_get(el_ev, "last_seq")), (int64_t)2);
  EXPECT_EQ(refi_str(el_ev, "summary"), "the chunked turn proved it");
  json_value_destroy(rec);

  /* The entry materialization (the full entry value, version 1, seq 1): */
  auto entries = refi_scan_values(f, root + "/entry/memory",
                                  root + "/entry/memory0", 0);
  ASSERT_EQ(entries.size(), 1u);
  json_value_t* entry = refi_parse_record(entries[0]);
  ASSERT_NE(entry, nullptr) << entries[0];
  EXPECT_EQ(refi_str(entry, "kind"), "memory");
  EXPECT_EQ(refi_str(entry, "id"), "chunked_bodies_decode_silently");
  EXPECT_EQ(refi_str(entry, "title"), "Chunked bodies decode silently");
  EXPECT_EQ(refi_str(entry, "content"), "A chunked body decodes in place");
  EXPECT_EQ(refi_str(entry, "path"), "general");
  EXPECT_EQ(json_as_int(json_get(entry, "version")), (int64_t)1);
  EXPECT_EQ(json_as_int(json_get(entry, "seq")), (int64_t)1);
  json_value_destroy(entry);

  /* The meta pair derives EXACTLY from the log (the fold re-read is the
     truth): fold the log again and compare both meta values. */
  refine_fold_t fold;
  mk_fold_init(&fold);
  std::string joint = "[";
  for (size_t i = 0; i < records.size(); i++) {
    if (i > 0) joint += ",";
    joint += refi_json_quote(records[i]);
  }
  joint += "]";
  ASSERT_EQ(refine_fold_parse(joint.c_str(), &fold), 0);
  char* fp = refine_fold_fingerprint(&fold);
  ASSERT_NE(fp, nullptr);
  std::string stored_fp = refi_stored_meta(f, root, "fingerprint");
  EXPECT_EQ(stored_fp.size(), 16u) << "the fingerprint is 16 hex chars";
  EXPECT_EQ(stored_fp, std::string(fp)) << "the stored fingerprint folds";
  free(fp);
  char* digest = refine_fold_digest(&fold);
  ASSERT_NE(digest, nullptr);
  EXPECT_EQ(refi_stored_meta(f, root, "digest"), std::string(digest))
      << "the stored digest folds";
  free(digest);
  refine_fold_destroy(&fold);

  /* ONE batch = ONE store trip: the record's edits == the materialized
     entries + meta both present (a non-atomic shape would be observable
     whenever ANY member is missing). */
  EXPECT_EQ(sm.calls, 1) << "the review ran once";

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestRefine, TestRefineRunNoopCommitsNothing) {
  /* The no-op rule end to end (spec §3 step 5): a proposal whose edits all
     refuse (no evidence) commits NOTHING — no record, no entry put, no
     meta write; refine_run returns 1 and the summary is the frozen
     banner. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  std::string root = sid + "/harness";

  ASSERT_EQ(frame_append_msg(f, "user", "turn one text"), 0);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  sm.content =
      "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":["
      "{\"action\":\"create\",\"kind\":\"memory\",\"title\":\"Unevidenced\","
      "\"content\":\"nothing evidences this\",\"reason\":\"why\"}]}";
  frame_set_model_backend(f, &sm.base);

  char* summary = NULL;
  int rc = refine_run(f, NULL, 0, &summary);
  ASSERT_EQ(rc, 1) << "the clean no-op returns 1";
  ASSERT_NE(summary, nullptr);
  EXPECT_STREQ(summary,
               "refine: no refinement committed (no evidence-backed edits)");
  free(summary);

  /* NOTHING reached the store: the log, the entries, and the meta are all
     empty — and the ROOT (shared) harness is untouched too. */
  EXPECT_EQ(refi_scan_values(f, root + "/log", root + "/log0", 0).size(), 0u);
  for (int k = 0; k < REFINE_KINDS_COUNT; k++) {
    std::string base = std::string(root) + "/entry/" + REFINE_KINDS[k];
    EXPECT_EQ(refi_scan_values(f, base, base + "0", 0).size(), 0u) << REFINE_KINDS[k];
  }
  EXPECT_EQ(refi_stored_meta(f, root, "fingerprint"), "");
  EXPECT_EQ(refi_stored_meta(f, root, "digest"), "");
  EXPECT_EQ(refi_scan_values(f, "harness/log", "harness/log0", 0).size(), 0u);
  EXPECT_EQ(sm.calls, 1) << "the review still ran (the caller learns "
                            "nothing was kept)";

  /* The record-compose evidence bound (refine.h's contract, spec §4): a
     citation PAST the trajectory's scanned newest event is unprovable —
     the gate's one string, nothing commits. */
  sm.content =
      "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":["
      "{\"action\":\"create\",\"kind\":\"memory\",\"id\":\"future_lesson\","
      "\"title\":\"Future\",\"content\":\"cites unscanned events\","
      "\"evidence\":{\"first_seq\":50,\"last_seq\":60,\"summary\":\"why\"},"
      "\"reason\":\"unprovable\"}]}";
  char* future_summary = NULL;
  int future_rc = refine_run(f, NULL, 0, &future_summary);
  EXPECT_EQ(future_rc, 1) << "the unprovable citation is the same no-op";
  EXPECT_NE(future_summary, nullptr);
  EXPECT_STREQ(future_summary,
               "refine: no refinement committed (no evidence-backed edits)");
  free(future_summary);
  EXPECT_EQ(refi_scan_values(f, root + "/log", root + "/log0", 0).size(), 0u);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestRefine, TestRepeatedRefineWithNoChangeCommitsNothing) {
  /* The fingerprint gate at refine time (spec §4's gate list item 2; the
     PA delivery gate's agent-session.ts:7238-7245 semantics ported): a
     SECOND refine over an unchanged fold + trajectory still reviews, but
     with the same create rejected as a duplicate nothing commits — the
     stored meta/fingerprint is BYTE-STABLE. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string root = frame_sid(f) + std::string("/harness");

  ASSERT_EQ(frame_append_msg(f, "user", "turn one text"), 0);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  sm.content = kChunkedCreateProposal;   /* the SAME proposal both times */
  frame_set_model_backend(f, &sm.base);

  char* summary = NULL;
  ASSERT_EQ(refine_run(f, NULL, 0, &summary), 0);
  free(summary);
  ASSERT_EQ(sm.calls, 1);
  std::string fp1 = refi_stored_meta(f, root, "fingerprint");
  std::string digest1 = refi_stored_meta(f, root, "digest");
  ASSERT_EQ(fp1.size(), 16u);
  auto records1 = refi_scan_values(f, root + "/log", root + "/log0", 0);
  ASSERT_EQ(records1.size(), 1u);

  /* The SAME proposal again: the create refuses "entry already exists",
     nothing applies, nothing commits — the stored meta is byte-stable and
     the log gains no record (the record-skip gate). */
  int rc2 = refine_run(f, NULL, 0, &summary);
  ASSERT_EQ(rc2, 1) << "the record-skip no-op returns 1";
  ASSERT_NE(summary, nullptr);
  EXPECT_STREQ(summary,
               "refine: no refinement committed (no evidence-backed edits)");
  free(summary);
  EXPECT_EQ(sm.calls, 2) << "the gate does not skip the REVIEW — it skips "
                            "the RECORD";
  EXPECT_EQ(refi_scan_values(f, root + "/log", root + "/log0", 0).size(), 1u)
      << "no second record: the fold is unchanged";
  EXPECT_EQ(refi_stored_meta(f, root, "fingerprint"), fp1)
      << "the stored meta/fingerprint is byte-stable";
  EXPECT_EQ(refi_stored_meta(f, root, "digest"), digest1);
  auto entries = refi_scan_values(f, root + "/entry/memory",
                                  root + "/entry/memory0", 0);
  ASSERT_EQ(entries.size(), 1u);
  json_value_t* entry = refi_parse_record(entries[0]);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(json_as_int(json_get(entry, "version")), (int64_t)1)
      << "the version never moved";
  json_value_destroy(entry);

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestRefine, TestTrajectoryScanNewestCapHolds) {
  /* The review's bounded view (spec §3 step 3): 300 seeded msg.append
     events, one refine_run; the store-side scan handed the review EXACTLY
     the newest SA_REFINE_SCAN_EVENTS records asked for and NO more. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);

  for (int i = 1; i <= 300; i++) {
    std::string content = "turn " + std::to_string(i) + " text";
    ASSERT_EQ(frame_append_msg(f, "user", content.c_str()), 0);
  }

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  sm.content = "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":[]}";
  frame_set_model_backend(f, &sm.base);

  char* summary = NULL;
  int rc = refine_run(f, NULL, 0, &summary);
  ASSERT_EQ(rc, 1) << "the empty proposal is the clean no-op";
  free(summary);
  ASSERT_EQ(sm.calls, 1);

  /* The captured user text's <trajectory> section rides EXACTLY the
     newest-capped event records (each record text once). */
  char* err = NULL;
  json_value_t* seen = json_parse(sm.captured_messages.c_str(),
                                  sm.captured_messages.size(), &err);
  if (err != NULL) free(err);
  ASSERT_NE(seen, nullptr) << sm.captured_messages;
  ASSERT_EQ(json_size(seen), 2u);
  const char* user_text = json_as_string(json_get(json_at(seen, 1), "content"));
  ASSERT_NE(user_text, nullptr);
  std::string user(user_text);
  json_value_destroy(seen);

  size_t traj_open = user.find("<trajectory>");
  size_t traj_close = user.find("</trajectory>");
  ASSERT_NE(traj_open, std::string::npos);
  ASSERT_NE(traj_close, std::string::npos);
  ASSERT_LT(traj_open, traj_close);
  std::string traj = user.substr(traj_open, traj_close - traj_open);

  size_t events = 0;
  for (size_t at = traj.find("msg.append"); at != std::string::npos;
       at = traj.find("msg.append", at + 1)) {
    events++;
  }
  EXPECT_EQ(events, (size_t) SA_REFINE_SCAN_EVENTS)
      << "EXACTLY the newest cap, no more";
  EXPECT_NE(traj.find("turn 300 text"), std::string::npos)
      << "the newest event rides";
  EXPECT_NE(traj.find("turn 173 text"), std::string::npos)
      << "the block's OLDEST kept event rides (300 - 127)";
  EXPECT_EQ(traj.find("turn 172 text"), std::string::npos)
      << "the FIRST event past the cap does not ride";

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestRefine, TestSharedScopeSeparateAndReadOnlyFromLocal) {
  /* The scopes (spec §1; refinement.ts:380-407's merge discipline): a
     local run composes sessions/<sid>/harness/... and reads the ROOT
     harness/ entries as READ-ONLY context; an edit naming a SHARED entry
     from a LOCAL run refuses loud; a shared run writes the ROOT subtree
     and never the session subtree. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  std::string root = sid + "/harness";

  ASSERT_EQ(frame_append_msg(f, "user", "turn one text"), 0);

  /* Seed the SHARED log: one applied memory create (a REAL record shape
     the fold re-applies; the record composes the whole entry contract). */
  {
    std::string shared_record = refi_seed_record(
        1, "refine_shared_seed", "the shared memory seed",
        refi_seed_edit("create", "memory", "shared_note", "Shared note",
                       "Shared content", "general", 0, 1, 2, "the shared "
                       "seed", NULL)
            .c_str());
    frm_store_op_t* ops =
        (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
    ops[0].key = strdup(refi_log_key("harness", 1).c_str());
    ops[0].value = (uint8_t*)strdup(shared_record.c_str());
    ops[0].value_len = shared_record.size();
    EXPECT_EQ(_frame_sync_batch(f, ops, 1, "shared log seed"), 0);
  }

  /* (a) THE CROSS-SCOPE REFUSAL: a shared entry is never an edit target
         from a local run — the edit refuses loud, nothing commits. */
  {
    scripted_review_model_t sm = {};
    sm.base.complete = scripted_review_complete;
    sm.base.submit = NULL;
    sm.content =
        "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":["
        "{\"action\":\"update\",\"kind\":\"memory\",\"id\":\"shared_note\","
        "\"title\":\"Hijack\",\"content\":\"stolen\","
        "\"expect\":{\"version\":1},"
        "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"why\"},"
        "\"reason\":\"the shared note should not be touched\"}]}";
    frame_set_model_backend(f, &sm.base);
    char* summary = NULL;
    int rc = refine_run(f, NULL, 0, &summary);
    ASSERT_EQ(rc, 1) << "every edit refused: nothing commits";
    ASSERT_NE(summary, nullptr);
    EXPECT_STREQ(summary,
                 "refine: no refinement committed (no evidence-backed edits)");
    free(summary);
    EXPECT_EQ(refi_scan_values(f, root + "/log", root + "/log0", 0).size(), 0u)
        << "the local log stays empty";
    EXPECT_EQ(refi_scan_values(f, "harness/log", "harness/log0", 0).size(), 1u)
        << "the shared log is untouched";
    auto shared_records = refi_scan_values(f, "harness/log", "harness/log0", 0);
    ASSERT_EQ(shared_records.size(), 1u);
    EXPECT_EQ(shared_records[0], refi_seed_record(
                    1, "refine_shared_seed", "the shared memory seed",
                    refi_seed_edit("create", "memory", "shared_note",
                                   "Shared note", "Shared content", "general",
                                   0, 1, 2, "the shared seed", NULL).c_str()))
        << "the shared record is BYTE-IDENTICAL after the refused read-only "
           "context";
  }

  /* (b) THE LOCAL COMMIT: sessions/<sid>/harness/... only. */
  {
    scripted_review_model_t sm = {};
    sm.base.complete = scripted_review_complete;
    sm.base.submit = NULL;
    sm.content =
        "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":["
        "{\"action\":\"create\",\"kind\":\"memory\",\"id\":\"local_lesson\","
        "\"title\":\"Local lesson\",\"content\":\"the local lesson\","
        "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"why\"},"
        "\"reason\":\"a session-specific lesson\"}]}";
    frame_set_model_backend(f, &sm.base);
    char* summary = NULL;
    int rc = refine_run(f, NULL, 0, &summary);
    ASSERT_EQ(rc, 0) << "the local commit returns 0";
    ASSERT_NE(summary, nullptr);
    free(summary);
    auto local_records = refi_scan_values(f, root + "/log", root + "/log0", 0);
    ASSERT_EQ(local_records.size(), 1u);
    json_value_t* rec = refi_parse_record(local_records[0]);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(refi_str(rec, "scope"), "local") << "the record says local";
    EXPECT_EQ(json_as_int(json_get(rec, "seq")), (int64_t)1);
    json_value_destroy(rec);
    auto local_entries =
        refi_scan_values(f, root + "/entry/memory", root + "/entry/memory0", 0);
    ASSERT_EQ(local_entries.size(), 1u);
    EXPECT_EQ(refi_stored_meta(f, root, "fingerprint").size(), 16u);
    /* The shared scope is untouched by the local commit: */
    EXPECT_EQ(refi_scan_values(f, "harness/log", "harness/log0", 0).size(), 1u);
    EXPECT_EQ(refi_stored_meta(f, "harness", "fingerprint"), "")
        << "the local run writes NO shared meta";
  }

  /* (c) THE SHARED RUN writes the ROOT subtree and never the local one. */
  {
    scripted_review_model_t sm = {};
    sm.base.complete = scripted_review_complete;
    sm.base.submit = NULL;
    sm.content =
        "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":["
        "{\"action\":\"update\",\"kind\":\"memory\",\"id\":\"shared_note\","
        "\"title\":\"Shared note\",\"content\":\"Shared content v2\","
        "\"expect\":{\"version\":1},"
        "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"why\"},"
        "\"reason\":\"the shared note matures\"}]}";
    frame_set_model_backend(f, &sm.base);
    char* summary = NULL;
    int rc = refine_run(f, NULL, 1, &summary);
    ASSERT_EQ(rc, 0);
    std::string sum(summary);
    free(summary);
    EXPECT_NE(sum.find(" committed (1/1 edits applied)"), std::string::npos)
        << sum;
    EXPECT_NE(sum.find("  apply memory shared:shared_note v2: the shared "
                       "note matures"), std::string::npos)
        << "the shared run's apply line carries the SHARED scope: " << sum;
    EXPECT_NE(sum.find("digest changed: yes"), std::string::npos) << sum;

    auto shared_records = refi_scan_values(f, "harness/log", "harness/log0", 0);
    ASSERT_EQ(shared_records.size(), 2u) << "the shared log gained the run";
    json_value_t* rec = refi_parse_record(shared_records[1]);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(refi_str(rec, "scope"), "shared");
    EXPECT_EQ(json_as_int(json_get(rec, "seq")), (int64_t)2)
        << "the shared log's own counter restored from the newest record";
    json_value_destroy(rec);
    auto shared_entries =
        refi_scan_values(f, "harness/entry/memory", "harness/entry/memory0", 0);
    ASSERT_EQ(shared_entries.size(), 1u);
    json_value_t* entry = refi_parse_record(shared_entries[0]);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(json_as_int(json_get(entry, "version")), (int64_t)2);
    EXPECT_EQ(refi_str(entry, "content"), "Shared content v2");
    json_value_destroy(entry);
    EXPECT_EQ(refi_stored_meta(f, "harness", "fingerprint").size(), 16u);

    /* The session subtree's harness traffic is UNTOUCHED by the shared
       run: the log still holds its one local record, the shared commit
       wrote no local meta. */
    auto local_records = refi_scan_values(f, root + "/log", root + "/log0", 0);
    ASSERT_EQ(local_records.size(), 1u) << "the shared run wrote no local record";
    EXPECT_EQ(refi_stored_meta(f, root, "fingerprint").size(), 16u)
        << "the local meta is still the local run's one";
  }

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestRefine, TestLocalRefineRendersSeededSharedContext) {
  /* THE DEFERRED (from Task 4) positive branch: a local run over a
     NON-EMPTY shared harness log renders the shared entries into the
     digest's MARKED read-only section (spec §3 step 4's read-only-context
     digest; refinement.ts:380-407's merge renders the other scope). */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);

  ASSERT_EQ(frame_append_msg(f, "user", "turn one text"), 0);

  std::string shared_record = refi_seed_record(
      3, "refine_shared_seed", "the shared memory seed",
      refi_seed_edit("create", "memory", "shared_note", "Shared note",
                     "Shared content", "shared/path", 0, 1, 2, "the shared "
                     "seed", NULL)
          .c_str());
  frm_store_op_t* ops = (frm_store_op_t*)get_clear_memory(sizeof(frm_store_op_t));
  ops[0].key = strdup(refi_log_key("harness", 3).c_str());
  ops[0].value = (uint8_t*)strdup(shared_record.c_str());
  ops[0].value_len = shared_record.size();
  EXPECT_EQ(_frame_sync_batch(f, ops, 1, "shared log seed"), 0);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  sm.content = kChunkedCreateProposal;
  frame_set_model_backend(f, &sm.base);

  char* summary = NULL;
  ASSERT_EQ(refine_run(f, NULL, 0, &summary), 0);
  char* perr = NULL;
  json_value_t* seen = json_parse(sm.captured_messages.c_str(),
                                  sm.captured_messages.size(), &perr);
  if (perr != NULL) free(perr);
  ASSERT_NE(seen, nullptr);
  const char* user_text = json_as_string(json_get(json_at(seen, 1), "content"));
  ASSERT_NE(user_text, nullptr);
  std::string user(user_text);
  json_value_destroy(seen);
  size_t ctx_open = user.find("<shared_harness_context>");
  ASSERT_NE(ctx_open, std::string::npos)
      << "the marked read-only section rides for a seeded shared log";
  size_t ctx_close = user.find("</shared_harness_context>");
  ASSERT_NE(ctx_close, std::string::npos);
  std::string section = user.substr(ctx_open, ctx_close - ctx_open);
  EXPECT_NE(section.find("read-only context: shared-scope entries are never "
                         "edit targets from a local refinement"),
            std::string::npos)
      << "the marking prefix: " << section;
  /* The shared entries render into the section (the digest's entry line). */
  EXPECT_NE(section.find("memory: 1"), std::string::npos) << section;
  EXPECT_NE(section.find("- shared_note shared/path v1: Shared content"),
            std::string::npos)
      << "the shared entry's line rides inside the marked section: "
      << section;
  free(summary);

  frame_destroy(f);
  wave_db_close(db);
}

/* The rollback triple's fixture: refines create the memory + skill entries
   (run 1), then create the prompt + update the memory + delete the skill
   (run 2 — the row 10 target), leaving the harness log at 2 records. The
   target record is the SECOND (seq 2). */
static const std::string kRollbackCreateProposal =
    "{\"summary\":\"Seed entries\","
    "\"rationale\":\"seed two entries\",\"edits\":["
    "{\"action\":\"create\",\"kind\":\"memory\",\"id\":\"kept_memory\","
    "\"title\":\"Kept memory\",\"content\":\"memory content\","
    "\"path\":\"memory/path\","
    "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"why\"},"
    "\"reason\":\"seed\"},"
    "{\"action\":\"create\",\"kind\":\"skill\",\"id\":\"deleted_skill\","
    "\"title\":\"Skill title\",\"content\":\"skill content\","
    "\"path\":\"skill/path\",\"reference\":" + std::string(kSkillReference) +
    ",\"arguments\":" + std::string(kSkillArguments) + ","
    "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"why\"},"
    "\"reason\":\"seed\"}]}";

static const char* const kRollbackTargetProposal =
    "{\"summary\":\"Target refinement\","
    "\"rationale\":\"three edits\",\"edits\":["
    "{\"action\":\"create\",\"kind\":\"prompt\",\"id\":\"created_prompt\","
    "\"title\":\"Created\",\"content\":\"Created content\","
    "\"evidence\":{\"first_seq\":3,\"last_seq\":4,\"summary\":\"why\"},"
    "\"reason\":\"create\"},"
    "{\"action\":\"update\",\"kind\":\"memory\",\"id\":\"kept_memory\","
    "\"title\":\"Updated memory\",\"content\":\"Updated memory content\","
    "\"path\":\"updated/path\",\"expect\":{\"version\":1},"
    "\"evidence\":{\"first_seq\":3,\"last_seq\":4,\"summary\":\"why\"},"
    "\"reason\":\"update\"},"
    "{\"action\":\"delete\",\"kind\":\"skill\",\"id\":\"deleted_skill\","
    "\"expect\":{\"version\":1},"
    "\"evidence\":{\"first_seq\":3,\"last_seq\":4,\"summary\":\"why\"},"
    "\"reason\":\"withdraw\"}]}";

/* Seeds the two records above through refine_run (the REAL cycle) and
   returns the log's record texts (2). The review never runs (each call's
   backend is re-set before the run). */
static void refi_seed_rollback_log(frame_t* f, scripted_review_model_t* sm,
                                   char** summary_out1, char** summary_out2) {
  sm->content = kRollbackCreateProposal;
  ASSERT_EQ(refine_run(f, NULL, 0, summary_out1), 0);
  ASSERT_NE(*summary_out1, nullptr) << "run 1 committed";
  sm->content = kRollbackTargetProposal;
  ASSERT_EQ(refine_run(f, NULL, 0, summary_out2), 0);
  ASSERT_NE(*summary_out2, nullptr) << "run 2 committed";
  free(*summary_out1);
  free(*summary_out2);
}

TEST(TestRefine, TestRollbackComposesTheInverseFromTheRecord) {
  /* Row 10 (refinement.test.ts:680-737's rollback): a full created/updated/
     deleted refinement rolled back — the entry leaves the fold (the log
     gains the rollbackOf record), the originals are BYTE-IDENTICAL after
     (append-only — nothing mutated). */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string sid = frame_sid(f);
  std::string root = sid + "/harness";

  ASSERT_EQ(frame_append_msg(f, "user", "turn one text"), 0);
  ASSERT_EQ(frame_append_msg(f, "assistant", "turn two text"), 0);
  ASSERT_EQ(frame_append_msg(f, "user", "turn three text"), 0);
  ASSERT_EQ(frame_append_msg(f, "assistant", "turn four text"), 0);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  frame_set_model_backend(f, &sm.base);
  char* s1 = NULL;
  char* s2 = NULL;
  refi_seed_rollback_log(f, &sm, &s1, &s2);

  /* The target record (seq 2) BYTE-COPY before the rollback. */
  std::string tlo = root + "/log";
  auto before_records = refi_scan_values(f, tlo, tlo + "0", 0);
  ASSERT_EQ(before_records.size(), 2u);
  std::string record2_before = before_records[1];
  json_value_t* target = refi_parse_record(record2_before);
  ASSERT_NE(target, nullptr);
  std::string target_id = refi_str(target, "id");
  json_value_destroy(target);
  EXPECT_EQ(target_id.find("refine_"), 0u)
      << "sanity: the target id is a refine_ mint";

  /* The pre-rollback materialization (the update applied, the skill
     withdrawn): */
  {
    auto entries =
        refi_scan_values(f, root + "/entry/memory", root + "/entry/memory0", 0);
    ASSERT_EQ(entries.size(), 1u);
    json_value_t* entry = refi_parse_record(entries[0]);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(json_as_int(json_get(entry, "version")), (int64_t)2);
    EXPECT_EQ(refi_str(entry, "content"), "Updated memory content");
    json_value_destroy(entry);
    auto prompts =
        refi_scan_values(f, root + "/entry/prompt", root + "/entry/prompt0", 0);
    ASSERT_EQ(prompts.size(), 1u);
    auto skills =
        refi_scan_values(f, root + "/entry/skill", root + "/entry/skill0", 0);
    ASSERT_EQ(skills.size(), 0u) << "the withdrawn skill's materialization "
                                    "is gone (the DELETE op)";
  }

  char* summary = NULL;
  int rc = refine_rollback(f, 2, 0, &summary);
  ASSERT_EQ(rc, 0) << "the rollback commits a record: " << summary
                   << (summary != NULL ? "" : "(null: check the log)");
  ASSERT_NE(summary, nullptr);
  std::string sum(summary);
  free(summary);

  /* The inverse order is REVERSED (PA's [.. target].reverse): create skill,
     update memory, delete prompt — versions/final states pinned. */
  std::string rollback_reason = "Rollback " + target_id;
  EXPECT_NE(sum.find(" committed (3/3 edits applied)"), std::string::npos)
      << sum;
  EXPECT_NE(sum.find("  apply skill local:deleted_skill v1: " +
                     rollback_reason), std::string::npos) << sum;
  EXPECT_NE(sum.find("  apply memory local:kept_memory v3: " + rollback_reason),
            std::string::npos) << sum;
  EXPECT_NE(sum.find("  apply prompt local:created_prompt v1: " +
                     rollback_reason), std::string::npos) << sum;
  EXPECT_NE(sum.find("digest changed: yes"), std::string::npos) << sum;

  /* The materialization after: the created prompt GONE (a real DELETE op),
     the memory restored to the before snapshot at v3 (its restore edit
     applied on top — PA's own expectation), the skill RECREATED at v1 with
     the whole call contract. */
  {
    auto prompts =
        refi_scan_values(f, root + "/entry/prompt", root + "/entry/prompt0", 0);
    ASSERT_EQ(prompts.size(), 0u) << "the created_prompt is GONE";
    auto memories =
        refi_scan_values(f, root + "/entry/memory", root + "/entry/memory0", 0);
    ASSERT_EQ(memories.size(), 1u);
    json_value_t* entry = refi_parse_record(memories[0]);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(json_as_int(json_get(entry, "version")), (int64_t)3);
    EXPECT_EQ(refi_str(entry, "content"), "memory content");
    EXPECT_EQ(refi_str(entry, "title"), "Kept memory");
    EXPECT_EQ(refi_str(entry, "path"), "memory/path");
    json_value_destroy(entry);
    auto skills =
        refi_scan_values(f, root + "/entry/skill", root + "/entry/skill0", 0);
    ASSERT_EQ(skills.size(), 1u) << "the skill is RECREATED";
    json_value_t* skill = refi_parse_record(skills[0]);
    ASSERT_NE(skill, nullptr);
    EXPECT_EQ(json_as_int(json_get(skill, "version")), (int64_t)1);
    EXPECT_EQ(refi_str(skill, "content"), "skill content");
    /* The materialization stores reference/arguments as JSON OBJECTs
       (the entry shape's parsed contract) — compare their serializations. */
    char* ref_seen = json_serialize(json_get(skill, "reference"));
    char* args_seen = json_serialize(json_get(skill, "arguments"));
    ASSERT_NE(ref_seen, nullptr);
    ASSERT_NE(args_seen, nullptr);
    EXPECT_STREQ(ref_seen, kSkillReference);
    EXPECT_STREQ(args_seen, kSkillArguments);
    free(ref_seen);
    free(args_seen);
    json_value_destroy(skill);
  }

  /* The log holds 3 records; the rollback record carries the relation and
     the reversed inverse edits, and the ORIGINAL record is BYTE-IDENTICAL
     after (append-only — nothing mutated). */
  {
    auto after_records = refi_scan_values(f, tlo, tlo + "0", 0);
    ASSERT_EQ(after_records.size(), 3u);
    EXPECT_EQ(after_records[1], record2_before)
        << "the original record's text is byte-identical after";
    EXPECT_EQ(after_records[0], before_records[0]);
    json_value_t* rb = refi_parse_record(after_records[2]);
    ASSERT_NE(rb, nullptr) << after_records[2];
    EXPECT_EQ(json_as_int(json_get(rb, "seq")), (int64_t)3);
    EXPECT_EQ(refi_str(rb, "scope"), "local");
    EXPECT_EQ(refi_str(rb, "trigger"), "Rollback refinement " + target_id);
    json_value_t* rb_of = json_get(rb, "rollbackOf");
    ASSERT_NE(rb_of, nullptr);
    ASSERT_EQ(json_type(rb_of), JSON_INT);
    EXPECT_EQ(json_as_int(rb_of), (int64_t)2);
    json_value_t* rb_ev = json_get(rb, "evidence");
    ASSERT_NE(rb_ev, nullptr);
    EXPECT_EQ(refi_str(rb_ev, "kind"), "rollback");
    EXPECT_EQ(json_as_int(json_get(rb_ev, "refineOf")), (int64_t)2);
    json_value_t* rb_edits = json_get(rb, "edits");
    ASSERT_NE(rb_edits, nullptr);
    ASSERT_EQ(json_size(rb_edits), 3u);
    /* [create skill, update memory, delete prompt] — the reversed order. */
    json_value_t* e0 = json_at(rb_edits, 0);
    EXPECT_EQ(refi_str(e0, "action"), "create");
    EXPECT_EQ(refi_str(e0, "kind"), "skill");
    EXPECT_EQ(refi_str(e0, "id"), "deleted_skill");
    json_value_t* e1 = json_at(rb_edits, 1);
    EXPECT_EQ(refi_str(e1, "action"), "update");
    EXPECT_EQ(refi_str(e1, "kind"), "memory");
    EXPECT_EQ(refi_str(e1, "id"), "kept_memory");
    EXPECT_EQ(json_as_int(json_get(json_get(e1, "expect"), "version")),
              (int64_t)2)
        << "the restore's guard = the version the target left";
    json_value_t* e2 = json_at(rb_edits, 2);
    EXPECT_EQ(refi_str(e2, "action"), "delete");
    EXPECT_EQ(refi_str(e2, "kind"), "prompt");
    EXPECT_EQ(refi_str(e2, "id"), "created_prompt");
    for (size_t i = 0; i < 3; i++) {
      json_value_t* el = json_at(rb_edits, i);
      ASSERT_EQ(json_as_bool(json_get(el, "applied")), 1);
      json_value_t* el_ev = json_get(el, "evidence");
      ASSERT_NE(el_ev, nullptr);
      EXPECT_EQ(refi_str(el_ev, "kind"), "rollback")
          << "the inverse edits carry the rollback evidence shape";
      EXPECT_EQ(json_as_int(json_get(el_ev, "refineOf")), (int64_t)2);
      EXPECT_EQ(refi_str(el, "reason"), rollback_reason);
    }
    json_value_destroy(rb);
  }

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestRefine, TestRollbackMissingTargetRefused) {
  /* Row 11 (refinement.test.ts:730-737's missing-target throw): the
     rollback of a missing seq refuses — "rollback: no refinement record at
     seq 5" — with the store untouched. */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string root = frame_sid(f) + std::string("/harness");

  ASSERT_EQ(frame_append_msg(f, "user", "turn one text"), 0);

  char* summary = NULL;
  int rc = refine_rollback(f, 5, 0, &summary);
  ASSERT_EQ(rc, 1) << "the missing target refuses through the summary";
  ASSERT_NE(summary, nullptr);
  EXPECT_STREQ(summary, "rollback: no refinement record at seq 5");
  free(summary);
  EXPECT_EQ(refi_scan_values(f, root + "/log", root + "/log0", 0).size(), 0u)
      << "the store is untouched";
  EXPECT_EQ(refi_stored_meta(f, root, "fingerprint"), "");

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestRefine, TestRollbackAfterLaterEditRejectsStaleInverse) {
  /* Row 12: a rollback AFTER a later edit to the same entry gets the
     per-edit STALE rejection ("stale target: ...") on the inverse's report
     line, and the roll-back record still commits the OTHER inverse edits
     (refinement.test.ts:680-737's per-edit applied/rejected shape). */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string root = frame_sid(f) + std::string("/harness");

  for (int i = 1; i <= 4; i++) {
    std::string content = "turn " + std::to_string(i) + " text";
    ASSERT_EQ(frame_append_msg(f, "user", content.c_str()), 0);
  }

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  frame_set_model_backend(f, &sm.base);

  /* Run 1: create the memory (v1). Run 2: update it (-> v2) + create the
     prompt. Run 3: update it AGAIN (-> v3). The rollback of run 2 then
     hits the stale guard. */
  const char* run1 =
      "{\"summary\":\"s1\",\"rationale\":\"r1\",\"edits\":["
      "{\"action\":\"create\",\"kind\":\"memory\",\"id\":\"kept_memory\","
      "\"title\":\"Kept\",\"content\":\"one\","
      "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"why\"},"
      "\"reason\":\"create\"}]}";
  const char* run2 =
      "{\"summary\":\"s2\",\"rationale\":\"r2\",\"edits\":["
      "{\"action\":\"update\",\"kind\":\"memory\",\"id\":\"kept_memory\","
      "\"title\":\"Kept\",\"content\":\"two\","
      "\"expect\":{\"version\":1},"
      "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"why\"},"
      "\"reason\":\"first update\"},"
      "{\"action\":\"create\",\"kind\":\"prompt\",\"id\":\"other_prompt\","
      "\"title\":\"Other\",\"content\":\"the other lesson\","
      "\"evidence\":{\"first_seq\":3,\"last_seq\":4,\"summary\":\"why\"},"
      "\"reason\":\"create\"}]}";
  const char* run3 =
      "{\"summary\":\"s3\",\"rationale\":\"r3\",\"edits\":["
      "{\"action\":\"update\",\"kind\":\"memory\",\"id\":\"kept_memory\","
      "\"title\":\"Kept\",\"content\":\"third\","
      "\"expect\":{\"version\":2},"
      "\"evidence\":{\"first_seq\":3,\"last_seq\":4,\"summary\":\"why\"},"
      "\"reason\":\"later update\"}]}";
  char* summary = NULL;
  sm.content = run1;
  ASSERT_EQ(refine_run(f, NULL, 0, &summary), 0);
  free(summary);
  sm.content = run2;
  ASSERT_EQ(refine_run(f, NULL, 0, &summary), 0);
  free(summary);
  sm.content = run3;
  ASSERT_EQ(refine_run(f, NULL, 0, &summary), 0);
  free(summary);

  auto records = refi_scan_values(f, root + "/log", root + "/log0", 0);
  ASSERT_EQ(records.size(), 3u) << "three refinement records";

  int rc = refine_rollback(f, 2, 0, &summary);
  ASSERT_EQ(rc, 0) << "the rollback still commits its OTHER inverse";
  ASSERT_NE(summary, nullptr);
  std::string sum(summary);
  free(summary);
  EXPECT_NE(sum.find(" committed (1/2 edits applied)"), std::string::npos)
      << sum;
  /* THE STALE REJECTION on the restore line (the version the target left
     was 2; the fold moved to 3): */
  EXPECT_NE(sum.find("  refuse memory kept_memory: stale target: entry "
                     "version 3, review saw 2"), std::string::npos) << sum;
  EXPECT_NE(sum.find("  apply prompt local:other_prompt"), std::string::npos)
      << sum;
  EXPECT_NE(sum.find("digest changed: yes"), std::string::npos) << sum;

  /* The other inverse APPLIED (the prompt's materialization gone), the
     stale target's content UNTOUCHED at v3, and the record's per-edit
     shape carries both. */
  {
    auto prompts =
        refi_scan_values(f, root + "/entry/prompt", root + "/entry/prompt0", 0);
    ASSERT_EQ(prompts.size(), 0u) << "the other inverse applied";
    auto memories =
        refi_scan_values(f, root + "/entry/memory", root + "/entry/memory0", 0);
    ASSERT_EQ(memories.size(), 1u);
    json_value_t* entry = refi_parse_record(memories[0]);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(json_as_int(json_get(entry, "version")), (int64_t)3);
    EXPECT_EQ(refi_str(entry, "content"), "third")
        << "the stale inverse never touched the entry";
    json_value_destroy(entry);
  }
  {
    auto after = refi_scan_values(f, root + "/log", root + "/log0", 0);
    ASSERT_EQ(after.size(), 4u);
    json_value_t* rb = refi_parse_record(after[3]);
    ASSERT_NE(rb, nullptr);
    json_value_t* rb_edits = json_get(rb, "edits");
    ASSERT_NE(rb_edits, nullptr);
    ASSERT_EQ(json_size(rb_edits), 2u);
    json_value_t* e0 = json_at(rb_edits, 0);   /* reversed: the prompt's */
    EXPECT_EQ(json_as_bool(json_get(e0, "applied")), 1);
    json_value_t* e1 = json_at(rb_edits, 1);   /* the stale restore */
    EXPECT_EQ(json_as_bool(json_get(e1, "applied")), 0);
    EXPECT_EQ(refi_str(e1, "error"),
              "stale target: entry version 3, review saw 2");
    json_value_destroy(rb);
    /* The originals stayed byte-identical (append-only). */
    EXPECT_EQ(after[0], records[0]);
    EXPECT_EQ(after[1], records[1]);
    EXPECT_EQ(after[2], records[2]);
  }

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestRefine, TestRefineBatchOverBudgetRefusesNothingCommitted) {
  /* The refuse-loud batch math (spec §5): a proposal whose entry value
     exceeds the per-entry cap refuses BEFORE the post — nothing is
     committed (the compose-time cap discipline). */
  wave_database_root_t* db = wave_db_open(NULL);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);
  std::string root = frame_sid(f) + std::string("/harness");

  ASSERT_EQ(frame_append_msg(f, "user", "turn one text"), 0);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  sm.content =
      "{\"summary\":\"s\",\"rationale\":\"r\",\"edits\":["
      "{\"action\":\"create\",\"kind\":\"memory\",\"id\":\"big_lesson\","
      "\"title\":\"Big\",\"content\":\"" + std::string(5000, 'x') + "\","
      "\"evidence\":{\"first_seq\":1,\"last_seq\":2,\"summary\":\"why\"},"
      "\"reason\":\"big\"}]}";
  frame_set_model_backend(f, &sm.base);

  char* summary = NULL;
  int rc = refine_run(f, NULL, 0, &summary);
  ASSERT_EQ(rc, -1) << "the over-budget batch refuses loud";
  EXPECT_EQ(summary, nullptr) << "the failure path logs only";

  /* NOTHING committed: no record, no entry, no meta. */
  EXPECT_EQ(refi_scan_values(f, root + "/log", root + "/log0", 0).size(), 0u);
  EXPECT_EQ(refi_scan_values(f, root + "/entry/memory", root + "/entry/memory0", 0)
                .size(), 0u);
  EXPECT_EQ(refi_stored_meta(f, root, "fingerprint"), "");
  EXPECT_EQ(refi_stored_meta(f, root, "digest"), "");

  frame_destroy(f);
  wave_db_close(db);
}

TEST(TestRefine, TestRefinesOnPooledStoreRefuseLoud) {
  /* Row 15: refine on a POOLED store refuses loud without hanging (both
     runners — the direct sync APIs' inline-only rule; the store actor's
     pacing belongs to its scheduler workers). */
  scheduler_pool_t* pool = scheduler_pool_create(2);
  ASSERT_NE(pool, nullptr);
  scheduler_pool_start(pool);
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = NULL;
  sc.store_pool = pool;
  wave_database_root_t* db = wave_db_open_config(&sc);
  ASSERT_NE(db, nullptr);
  frame_config_t cfg = refi_frame_config();
  frame_t* f = frame_create(db, NULL, NULL, &cfg);
  ASSERT_NE(f, nullptr);

  scripted_review_model_t sm = {};
  sm.base.complete = scripted_review_complete;
  sm.base.submit = NULL;
  sm.content = kChunkedCreateProposal;
  frame_set_model_backend(f, &sm.base);

  char* summary = NULL;
  int rc = refine_run(f, NULL, 0, &summary);
  EXPECT_EQ(rc, -1) << "refine_run refuses loud on a pooled store";
  EXPECT_EQ(summary, nullptr);
  rc = refine_rollback(f, 1, 0, &summary);
  EXPECT_EQ(rc, -1) << "refine_rollback refuses loud on a pooled store";
  EXPECT_EQ(summary, nullptr);
  EXPECT_EQ(sm.calls, 0) << "the refusal precedes ANY review";

  frame_destroy(f);
  scheduler_pool_stop(pool);
  wave_db_close(db);
  scheduler_pool_destroy(pool);
}

TEST(TestRefine, TestFoldParsesRollbackEvidenceShape) {
  /* THE T1 SHAPE (refine.h's evidence contract): the fold's record decode
     accepts the record's rollback evidence {"kind":"rollback","refineOf":N}
     beside the plain shape — the inverse edits fold (their evidence IS the
     target), a rollback record renders its log line, and a malformed
     refineOf stays refused loud. */
  refine_fold_t fold;
  mk_fold_init(&fold);
  std::string target_edit =
      refi_seed_edit("create", "memory", "evidence_target", "Target",
                     "the target lesson", "general", 0, 11, 12, "why", NULL);
  std::string rollback_record =
      "{\"seq\":7,\"id\":\"refine_rollback_rec\","
      "\"trigger\":\"Rollback refinement refine_target_rec\",\"rollbackOf\":5,"
      "\"evidence\":{\"kind\":\"rollback\",\"refineOf\":5},\"edits\":["
      "{\"action\":\"delete\",\"kind\":\"memory\","
      "\"id\":\"evidence_target\",\"expect\":{\"version\":1},"
      "\"evidence\":{\"kind\":\"rollback\",\"refineOf\":5},"
      "\"before\":null,\"applied\":true,\"error\":null,"
      "\"reason\":\"Rollback refine_target_rec\"}],"
      "\"at\":\"2026-10-01T13:00:00Z\"}";
  std::string records = "[" + refi_json_quote(refi_seed_record(
                                  5, "refine_target_rec", "the target", target_edit.c_str())) +
                        "," + refi_json_quote(rollback_record) + "]";
  ASSERT_EQ(refine_fold_parse(records.c_str(), &fold), 0);
  EXPECT_EQ(refine_entry_find(&fold, "memory", "evidence_target"), nullptr)
      << "the inverse delete folded (the rollback evidence IS the target)";
  EXPECT_EQ(fold.nrecords, 2u) << "the rollback record renders its log line";
  char* digest = refine_fold_digest(&fold);
  ASSERT_NE(digest, nullptr);
  std::string rendered(digest);
  free(digest);
  EXPECT_NE(rendered.find("- 7 Rollback refinement refine_target_rec"),
            std::string::npos)
      << "the rollback record's line rides: " << rendered;

  /* A malformed refineOf (wrong type) leaves the evidence empty — the
     gate refuses the inverse loudly and the fold keeps the entry. */
  refine_edit_t edit;
  memset(&edit, 0, sizeof(edit));
  edit.action = refi_dup("delete");
  edit.kind = refi_dup("memory");
  edit.id = refi_dup("ghost");
  edit.expect_version = 1;
  edit.evidence_first = 1;
  edit.evidence_last = 1;
  refine_fold_t fold2;
  mk_fold_init(&fold2);
  ASSERT_NE(refine_entry_put(&fold2, "memory", "ghost", "t", "c", "p", "{}",
                             "{}", 1, 0, 1), nullptr);
  EXPECT_EQ(refine_edit_apply(&fold2, &edit, 9), nullptr)
      << "sanity: a well-formed inverse deletes";
  refine_edit_destroy(&edit);
  refine_fold_destroy(&fold2);

  refine_fold_destroy(&fold);
}

/* The malformed-refineOf variant through the RECORD decode: the fold's
   record parser leaves evidence 0/0 when refineOf is not an int — the
   edit refuses "edit without evidence" and the entry survives (a corrupt
   rollback never silently deletes). */
TEST(TestRefine, TestFoldRefusesMalformedRollbackEvidence) {
  std::string create_edit =
      refi_seed_edit("create", "memory", "ghost", "t", "c", "general", 0, 11,
                     12, "why", NULL);
  std::string bad_rollback =
      "{\"seq\":8,\"id\":\"refine_bad\",\"trigger\":\"Rollback refine_x\","
      "\"rollbackOf\":7,"
      "\"evidence\":{\"kind\":\"rollback\",\"refineOf\":\"seven\"},"
      "\"edits\":[{\"action\":\"delete\",\"kind\":\"memory\",\"id\":\"ghost\","
      "\"expect\":{\"version\":1},"
      "\"evidence\":{\"kind\":\"rollback\",\"refineOf\":\"seven\"},"
      "\"before\":null,\"applied\":true,\"error\":null}],"
      "\"at\":\"2026-10-01T13:00:00Z\"}";
  std::string records = "[" +
                        refi_json_quote(refi_seed_record(7, "refine_x", "t",
                                                         create_edit.c_str())) +
                        "," + refi_json_quote(bad_rollback) + "]";
  refine_fold_t fold;
  mk_fold_init(&fold);
  ASSERT_EQ(refine_fold_parse(records.c_str(), &fold), 0);
  EXPECT_EQ(fold.nrecords, 2u) << "the record folds; the edit refuses";
  refine_entry_t* ghost = refine_entry_find(&fold, "memory", "ghost");
  ASSERT_NE(ghost, nullptr) << "the malformed evidence gate kept the entry";
  EXPECT_EQ(ghost->version, 1u);
  refine_fold_destroy(&fold);
}
