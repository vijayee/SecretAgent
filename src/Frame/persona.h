//
// Created by victor on 10/6/26.
//

#ifndef SA_PERSONA_H
#define SA_PERSONA_H

#include <stddef.h>

/* persona.h — L5's persona half (the persona slice, spec §1-§2): PURE.
   The persona = the agent's MANNER as LOADABLE DATA (store voice records),
   never welded into the loop's code. This module owns the record's SHAPE
   and VALIDATION (the load-time field refusals + the falsifiability
   meta-rule as data) and the full assembly's COMPOSE (the Onyx
   build_system_prompt transposed: the persona block + the context block +
   the tool-conditional guidance + the base's position by placement + the
   {…} placeholder catalog). No store, no model, no actor: the record
   arrives as JSON text, the compose answers bytes. The store reads/writes
   ride the CALLER — frame.c owns the records' install, the derive owns
   their read (the missing-record fallback = the base alone, Task 4). */

/* --- the voice record (spec §1) ------------------------------------------
   The record persona_record_load produces; every heap string is OWNED by
   the record (deep-copied at load, freed by persona_record_destroy). The
   module never mutates a record after load — compose borrows only. */
typedef struct persona_record_t {
  unsigned version;                 /* refused unless == 1 */
  char* name;                       /* the record's identity (non-empty) */
  char* text;                       /* the persona's markdown, VERBATIM */
  char* placement;                  /* "first" | "below" (Onyx's above/below) */
  struct persona_guidance_t {
    char* key;                      /* the tool id this section attaches on */
    char* text;                     /* the section's markdown body */
  }* guidance;                      /* tool-conditional sections (may be a
                                       zero-length array when none) */
  size_t nguidance;
  char** principles;                /* OPTIONAL: the persona's principle
                                       statements (the falsifiability
                                       check's input; NULL when absent) */
  size_t nprinciples;
  char** test_spec;                 /* the checkable-behavior statements */
  size_t ntest_spec;
} persona_record_t;

/* Parse + validate one voice record's JSON text (the store key
   `personas/<name>/record`'s body). The record shape's refusals are LOUD —
   rc -1 plus a log line naming the broken rule, *out stays NULL, never a
   half record:
     - the text must be a JSON object at all;
     - "version": REQUIRED JSON int == 1;
     - "name":    REQUIRED non-empty string;
     - "text":    REQUIRED non-empty string;
     - "placement": REQUIRED, exactly "first" or "below";
     - "guidance": OPTIONAL JSON array (absent = none); each element a JSON
       object carrying non-empty string "key" AND "text" — one bad element
       refuses the whole load;
     - "principles"/"test_spec": OPTIONAL arrays of strings (a non-string
       element refuses the load).
   The falsifiability meta-rule (persona_validate_falsifiable) runs at load
   too: a record whose principles cannot be checked refuses, the
   validator's err line in the loud log.
   A record whose text carries {CURRENT_DATETIME} LOADS but WARNs loud —
   that record opts out of the cache-stable prefix (compose stamps the
   render time into it). */
int persona_record_load(const char* record_json, persona_record_t** out);

/* The record's lifecycle (every owned heap string, the guidance array, the
   string arrays). NULL = a no-op. */
void persona_record_destroy(persona_record_t* record);

/* --- the falsifiability meta-rule, mechanically (spec §1) ----------------
   The data layer enforces the RECORD SHAPE's completeness, loudly: every
   principle's TEXT must appear as a substring of SOME test_spec entry
   (the checks restate the principles — the record's author makes that
   hold; a "reducible" NLP judgment stays a human's call).
   Returns 0 when the record passes or carries no principles at all (an
   optional list absent = no check runs). Returns -1 when a principle is
   uncheckable: *err_out (when non-NULL) gets the heap refusal line
     principle '<text>' has no checkable test — the meta-rule
   or, for a principle with NO text at all (empty or whitespace-only —
   its substring "match" would be strstr's trivial always-match),
     principle '<text>' has no text — the meta-rule
   (the caller frees). A NULL record = -1 + a loud log, err_out untouched. */
int persona_validate_falsifiable(const persona_record_t* record,
                                 char** err_out);

/* --- the compose (spec §2): the full assembly, pure and deterministic ------

   One pass builds the persona GROUP — the record's text, then the context
   block, then the attached guidance sections — joining its non-empty parts
   with "\n\n":
     - A guidance entry attaches IFF its key names a member of `tools`
       (the frame's tool-id surface, an array of borrowed id strings) and
       the ATTACH ORDER is the record's own list order (the determinism
       pin). A section renders "## <key>" + a newline + the text. The
       missing-tool guidance never attaches.
     - The context block: a plain text (not a JSON document) renders
       VERBATIM; a JSON OBJECT renders one "key: value" line per field in
       the keys' SORTED order (the byte-stability render; string values
       verbatim, other values in compact JSON); anything else parses as
       JSON but is not an object → verbatim text too.

   Then the base rides the placement — the group moves as ONE body, never
   split from its context/guidance:
     - "first" (or any other placement): [persona][context][guidance…][base]
     - "below":                          [base][persona][context][guidance…]
   A NULL/empty base renders absent; the composed parts still join "\n\n".

   Then the placeholder catalog, ONE pass over the ASSEMBLED string:
     - {CURRENT_DATETIME} → the UTC ISO-8601 render stamp (one stamp per
       compose — the documented cache-stability opt-out).
     - {USER_<REST>} → the context object's "<rest lowercased>" field (its
       value verbatim when a string, compact JSON otherwise). A recognized
       pattern that resolves to nothing (the field missing, or no JSON
       object context at all) substitutes "" — NEVER a raw-token leak.
     - Any other {TOKEN} (no CURRENT_DATETIME, no USER_ prefix) is left
       VISIBLE verbatim — the record author's typo stays readable. The
       scanner leaves an unrecognized '{' in place and rescans from just
       inside it (the plain-brace cases round-trip byte-identically).

   record NULL = the no-persona path: the base AS-IS (a heap copy). A NULL
   record AND a NULL base = refuse loud, NULL.
   Byte-stability: the same record/context/tools/base compose to IDENTICAL
   bytes — except a {CURRENT_DATETIME} record, whose stamp moves (the
   load-time warning named that opt-out). Returns heap (the caller frees);
   never NULL otherwise — allocations abort on OOM (the allocator's rule). */
char* persona_compose(const persona_record_t* record,
                      const char* context_json_or_text,
                      const char* const* tools, size_t ntools,
                      const char* base);

#endif /* defined(SA_PERSONA_H) */