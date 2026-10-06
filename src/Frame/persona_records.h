//
// Created by victor on 10/6/26.
//

#ifndef SA_PERSONA_RECORDS_H
#define SA_PERSONA_RECORDS_H

/* persona_records.h — the persona records that SHIP in-repo (the persona
   slice, spec §1). The embedder's boot installs them into the store as
   STATE — frame.h's persona_records_install owns the put (idempotency is
   the installer's decision: a put overwrites, so the doc line is "call at
   boot"); this header owns only the record ASSETS, pure data with no
   store, no model, no actor — persona.c's discipline, kept. */

/* The hammer's voice record (spec §1) composed into the store's record
   JSON: {version 1, name "hammer", placement "first", the founding
   thread's (docs/chat.json) msg 4148 ```markdown block VERBATIM as the
   text — meta-rule included, it IS the record's falsifiability contract —
   plus the execute guidance and the principles/test_spec arrangement
   pinned in persona_records.c}. Serialized the json util's ONE canonical
   way, so every call answers byte-identical JSON. Returns a heap string
   the caller frees; NULL only on a compose refusal (logged loud). */
char* persona_records_hammer_record(void);

#endif /* SA_PERSONA_RECORDS_H */