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
                            lesson rests on (BOTH 0 = no evidence => refused).
                            A rollback record's inverse edits carry the
                            rollback evidence shape
                            {"kind":"rollback","refineOf":<seq>} (spec §4);
                            the record decode folds it to BOTH = refineOf —
                            the inverse edit's evidence IS the target. */
  char* reason;     /* the "why" — the record also carries it */
} refine_edit_t;

void refine_edit_destroy(refine_edit_t* e);

/* Field-only validation (RefinementKind contract + the immutable-base rule).
   NULL = valid; otherwise a malloc'd refusal string the caller frees. */
char* refine_edit_validate(const refine_edit_t* e);

/* The evidence gate (spec §4, OURS): NULL = both seqs set and sane
   (first <= last, both nonzero); "edit without evidence" malloc'd
   otherwise. The compose-time UPPER bound (first <= the frame's current
   seq) is the RUNNERS' check — refine_run reads the frame's current seq
   as the trajectory's scanned newest record (the only seq view a store
   round trip exposes), so a citation past the scanned view refuses with
   the gate's one string; an inverse (rollback) edit's evidence IS its
   target record seq and never runs the bound. */
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

/* --- the review call (spec §3 step 4): compose, call, decode only --------- */

/* The review pass (spec §3 step 4): builds
   [system = REFINE_REVIEW_SYSTEM] + [user = <current digest> +
   <prior-refinement tail from the fold> + <read-only-context digest for a
   local run against the shared scope> + "[trajectory]" + <instructions?>]
   and calls the frame's OWN backend (the frame-injected test backend wins;
   the shared/sync complete() vtable) with tools = json_new_null() — NO tool
   surface (spec §7).

   The trajectory's newest SA_REFINE_SCAN_EVENTS event records are read HERE
   (a bounded FRM_STORE_SCAN round trip through the store actor, the derive's
   own scan shape) and joined into a bounded tail slice of
   <= SA_REFINE_TRAJECTORY_CHARS: the slice drops WHOLE OLDEST records —
   never a mid-document cut — so the view stays parseable and
   evidence-citable; it rides back in *trajectory_json (heap, the caller's
   to free), the bounded view the review consumed. On a local run (scope_root
   != "harness") the shared scope's log is ALSO scanned read-only and its
   digest marks the read-only-context section; no write, no apply, no meta
   happens here — decode only.

   Returns NULL with the proposal decoded: *edits (a malloc'd refine_edit_t
   array the caller destroys per element and frees), *nedits (0 = the valid
   empty proposal), *summary and *rationale (heap, the caller's). Non-NULL
   return = a malloc'd refusal string (validateEdit strings, the decode's own
   malformation line, the output-cap truncation wording, the model error) and
   EVERY out-slot NULL/0 — the decode refuses loud, no partial decode. */
char* refine_review_call(frame_t* f, const refine_fold_t* fold,
                         const char* scope_root, const char* instructions,
                         char** trajectory_json,
                         refine_edit_t** edits, size_t* nedits,
                         char** summary, char** rationale);

/* --- the runners (spec §3 + §5): the whole refine cycles, on the caller's
   thread. The store actor stays the ONLY serializer: every write action is
   ONE atomic FRM_STORE_BATCH through it (1 log record + one entry op per
   applied edit — a full-entry put, or a real DELETE op on a withdrawal —
   plus meta/fingerprint + meta/digest), and the harness log is APPEND-ONLY
   (a rollback is a NEW record; an original record's text is never mutated).

   The scope roots (spec §1): the LOCAL scope composes under the session
   subtree ("<frame_sid>/harness/...", written by shared_scope = 0), the
   SHARED scope is the ROOT "harness/" subtree (shared_scope != 0, like the
   lineage graph ops' absolute root-level keys — no subtree handle). A
   shared run's writes never touch the session subtree and vice versa; a
   LOCAL run that names (update/delete) an entry living in the shared scope
   refuses loud per edit ("%s targets a %s-scope entry from a %s
   refinement") — the shared scope is READ-ONLY CONTEXT to a local run (its
   entries render into the review prompt's marked shared_harness_context
   section), and a local CREATE over a shared id is a session-local
   override (PA's merge namespacing), never a cross-scope write.

   The harness-log keys ride the events discipline ("log/<%020llu seq>",
   frame.c's _frame_event_key pad): the fixed-width zero pad keeps the
   keys' lexicographic order the seq order, so the fold's range scan
   [<root>/log, <root>/log0) IS the ascending log and the rollback's
   exact-record bounds [<root>/log/<seq>, <root>/log/<seq+1>) hit exactly
   one stored key (spec §5 — log seqs are leaves).

   The log's counter is restored ONCE per call from the fold (the newest
   scanned record's seq; the same monotonic restore as frame.c's
   _frame_restore_seq, gaps recorded identically) and allocated +1 per
   record the call composes. The record's "evidence" object is
   {"session",first_seq,last_seq,summary} for a normal refinement and
   {"kind":"rollback","refineOf":<target seq>} for a rollback record (the
   record text is what the store holds; the fold's parse accepts BOTH
   shapes). The batch is cap-checked against the per-op entry/record caps
   and SA_REFINE_BATCH_BYTES BEFORE the post — a refusal commits NOTHING.

   THE LIBRARY NEVER PRINTS (the summary is the caller's): rc
     0 = a refinement (or rollback) committed — *summary_out is the
         frozen report (the per-edit applied/refused lines + the
         "digest changed: <yes|no>" line against the STORED meta/digest)
     1 = the store was untouched (a no-op or a refusal) — *summary_out:
         refine: the frozen no-op banner ("no refinement committed (no
         evidence-backed edits)"; a rollback with nothing to apply takes
         it too — the fold would be unchanged, spec §4's record-skip gate)
         rollback: the missing-target banner ("rollback: no refinement
         record at seq <N>")
    -1 = failed loud (the log carries it; *summary_out NULL) */

/* ONE refine cycle (spec §3): fold the scope's log, review the trajectory
   with the frame's OWN backend (the no-tools review call), validate +
   evidence-gate + apply the proposal per edit, and commit the survivors in
   ONE atomic batch. */
int refine_run(frame_t* f, const char* instructions, uint8_t shared_scope,
               char** summary_out);

/* The rollback (spec §5): the exact-record scan over the target's log key
   range, the inverses recomposed from the record's APPLIED edit elements
   (a create inverts to a delete, an update/deletion inverts to the before
   snapshot's create/update) applied with the CURRENT fold's version guards
   re-checked (a later edit to the same entry gets its per-edit stale
   rejection while the OTHER inverses still apply), then its own ONE batch:
   the rollback record ("rollbackOf":<target seq>, the rollback evidence
   shape on record and edits) + the entry ops + the meta pair. */
int refine_rollback(frame_t* f, uint64_t seq, uint8_t shared_scope,
                    char** summary_out);

#endif /* SA_HAS_WDB */
#endif /* SA_REFINE_H */