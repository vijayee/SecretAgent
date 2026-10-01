//
// Created by victor on 10/1/26.
//

#ifndef SA_REFINE_H
#define SA_REFINE_H

#include "../Util/json.h"

#ifdef SA_HAS_WDB
#include "frame.h"

/* --- caps (SA_* ifndef discipline — a build can override with -D) ---------
   The review's budget trio: PA's output caps are refuse-loud BOUNDS on our
   side (the http request contract in model.c stays frozen). */
#ifndef SA_REFINE_MAX_EDITS
#define SA_REFINE_MAX_EDITS 8            /* one proposal's edit budget */
#endif
#ifndef SA_REFINE_SCAN_EVENTS
#define SA_REFINE_SCAN_EVENTS 128        /* the trajectory's newest-events view */
#endif
#ifndef SA_REFINE_TRAJECTORY_CHARS
#define SA_REFINE_TRAJECTORY_CHARS 80000 /* the tail slice (refinement.ts:1258) */
#endif
/* The digest render caps (refinement.ts:20-22, verbatim numbers): */
#ifndef SA_REFINE_DIGEST_ENTRIES_PER_KIND
#define SA_REFINE_DIGEST_ENTRIES_PER_KIND 6
#endif
#ifndef SA_REFINE_DIGEST_REFINEMENTS
#define SA_REFINE_DIGEST_REFINEMENTS 5
#endif
#ifndef SA_REFINE_DIGEST_CONTENT_CHARS
#define SA_REFINE_DIGEST_CONTENT_CHARS 180
#endif
#ifndef SA_REFINE_LOG_WINDOW
#define SA_REFINE_LOG_WINDOW 512         /* the shared scan window max */
#endif
/* Refuse-loud batch budget (spec §5's math: <= 1 record + <= 8 entry puts +
   2 meta values = ~42 KB worst; this keeps every refine batch far under
   frame.c's mirrored 120 KB WAL cap — refusal happens at compose time). */
#define SA_REFINE_ENTRY_PUT_BYTES 4096
#define SA_REFINE_RECORD_BYTES 8192
#define SA_REFINE_BATCH_BYTES (96 * 1024)

/* Suffix kinds (RefinementKind, refinement.ts:35). */
#define REFINE_KINDS_COUNT 4
extern const char* const REFINE_KINDS[REFINE_KINDS_COUNT];   /* prompt/memory/skill/subagent */
extern const char REFINE_ID_BASE_PROMPT[];                    /*"base_system_prompt"*/

/* --- the fold (spec §2): the current state, folded from the log alone ----

   The log is truth; the entry materialization is a derived output this
   module recomposes in the same atomic batch as its log records. A stale or
   missing entry value can never resurrect a lesson — reads come from the
   fold, the fold comes from the log. */

/* One folded entry. All strings heap-owned by the fold. */
typedef struct refine_entry_t {
  char* kind;
  char* id;
  char* title;
  char* content;
  char* path;
  char* reference;    /* skill call contract serialized ("{}" when absent) */
  char* arguments;    /* skill call arguments serialized ("{}" absent) */
  unsigned version;   /* refinement.ts:1128's bump semantics */
  uint8_t deleted;    /* tombstone: the newest state action was a delete */
  uint64_t seq;       /* the newest harness-log seq that touched it */
} refine_entry_t;

/* The fold: entries + the log's record lines (the fingerprint's record
   material and the digest's refinement tail). */
typedef struct refine_fold_t {
  refine_entry_t* entries;    /* heap array */
  size_t nentries, capacity;
  char** record_lines;        /* "<seq>;<trigger-trim>;" per record, in stored
                                 (ascending) order; a malformed record renders
                                 its skip-line label instead */
  size_t nrecords, rcapacity;
} refine_fold_t;

/* Parse a JSON ARRAY of harness-log record TEXTS (the scan reply's joint
   text — Task 3 hands it over) and fold it. Every record's edits apply in
   order; malformed records are dropped with a loud log line and render as a
   skip line (refinement.ts:479-483's render-not-crash rule). Returns 0, or
   -1 with a loud log on OOM/array-level parse failure. */
int refine_fold_parse(const char* records_array_json, refine_fold_t* fold);

/* The fold's lifecycle (all entries' strings + the record lines). */
void refine_fold_destroy(refine_fold_t* fold);

/* FNV-1a-64 of the fold's canonical material, as 16 lowercase hex chars
   (malloc'd 17 bytes; caller frees). A NULL fold refuses loud (log_error)
   and returns NULL. Material (the spec §4's port of
   refinement.ts:777-843 — covered fields ONLY, order normalized):
   "refine-fingerprint-v1;" then per entry in (kind,id) sort order:
     "e;<kind>;<id>;<version>;<path>;<content>;" and ONLY the skill entries
   keep going with "r;<reference>;a;<arguments>;" — then per record line in
   stored order: "l;<record-line>;" (a malformed record's line IS its skip
   label, so equality implies identical renders). */
char* refine_fold_fingerprint(const refine_fold_t* fold);

/* The bounded digest render (malloc'd; caller frees). A NULL fold refuses
   loud (log_error) and returns NULL.

   harness: <entries in scope | "empty">
   <kind>: <count>
   - <id> <path> v<version>: <content whitespace-compacted, 180>
   ... (<= SA_REFINE_DIGEST_ENTRIES_PER_KIND per kind; overflow line
       "- +<n> older <kind> entries")
   refinements: <newest SA_REFINE_DIGEST_REFINEMENTS lines>
   - <seq> <trigger-trim-180>

   Deleted entries render NOTHING; a malformed record renders its skip
   line; empty kinds print "0" with no entries. */
char* refine_fold_digest(const refine_fold_t* fold);

/* --- the fold's direct entry construction (the refine_entry_* helpers) ----

   The fold-parse and the apply path build through these; the parity tests'
   seeds plant their entries with them (a seeded fold is a fold the log
   WOULD have folded — the struct fills through the public API). Entries are
   COPIED in from borrowed strings; tombstones live in the array (the log's
   delete actions stay visible to the fold's own bookkeeping) while
   refine_entry_find shows only LIVE entries. */
refine_entry_t* refine_entry_put(refine_fold_t* fold, const char* kind,
                                 const char* id, const char* title,
                                 const char* content, const char* path,
                                 const char* reference, const char* arguments,
                                 unsigned version, uint8_t deleted,
                                 uint64_t seq);

/* The LIVE entry for (kind,id) — a tombstone is invisible. NULL when the
   fold holds no such entry. */
refine_entry_t* refine_entry_find(const refine_fold_t* fold, const char* kind,
                                  const char* id);

/* --- the edit (one proposal's unit; the reviewer's JSON fields) ----------- */

typedef struct refine_edit_t {
  char* action;     /* "create" | "update" | "delete" */
  char* kind;       /* one of REFINE_KINDS */
  char* id;         /* NULL on create = slug(title, kind) (refinement.ts:396-405) */
  char* title;      /* create/update required */
  char* content;    /* create/update required */
  char* path;       /* "general" default on create; NULL = keep on update */
  char* reference;  /* skill create: the reference object serialized; NULL = keep */
  char* arguments;  /* skill create/update: serialized arguments; NULL = keep */
  unsigned expect_version;  /* update/delete: the version the review SAW */
  uint64_t evidence_first, evidence_last;  /* the trajectory event seqs this
                            lesson rests on (BOTH 0 = no evidence => refused) */
  char* reason;     /* the "why" — the record also carries it */
} refine_edit_t;

void refine_edit_destroy(refine_edit_t* e);

/* Field-only validation (RefinementKind contract + the immutable-base rule).
   NULL = valid; otherwise a malloc'd refusal string the caller frees. */
char* refine_edit_validate(const refine_edit_t* e);

/* The evidence gate (spec §4, OURS): NULL = both seqs set and sane
   (first <= last, both nonzero, first <= the frame's current seq is checked
   at record-compose time); "edit without evidence" malloc'd otherwise. */
char* refine_edit_evidence_check(const refine_edit_t* e);

/* Apply one VALIDATED edit to the fold IN MEMORY (no store). Returns NULL
   when applied (the fold mutated: create at version 1, update bumps,
   delete tombstones) — or a malloc'd refusal string and the fold unchanged.
   `at_seq` = the harness-log seq this application rides (the entry's seq
   field). Refusals (verbatim where parity-asserted): "entry already exists"
   / "entry not found" / "stale target: entry version %u, review saw %u". */
char* refine_edit_apply(refine_fold_t* fold, const refine_edit_t* e,
                        uint64_t at_seq);

/* Compose the record's edits array element for one edit (the refinement
   record's JSON shape, spec §4) — before/after snapshots ride the record;
   `before` is the full entry JSON the apply replaced (NULL on create). The
   returned DOM is owned by the caller. */
json_value_t* refine_record_edit_json(const refine_edit_t* e,
                                      const refine_entry_t* before,
                                      int applied, const char* error,
                                      unsigned after_version);

#endif /* SA_HAS_WDB */
#endif /* SA_REFINE_H */