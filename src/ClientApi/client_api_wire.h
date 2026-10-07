//
// Created by victor on 10/03/26.
//

/* The agent surface's client-API wire (the client-api spec §1): CBOR arrays,
   message type first element; the RESPONSE type = REQUEST+1; every request
   carries a req_id the response echoes. liboffs's pairing-assert discipline
   verbatim. Bounded fields: the wire never trusts its peer — unknown types,
   oversize strings, and malformed encodes all answer CA_ERROR. */

#ifndef SA_CLIENT_API_WIRE_H
#define SA_CLIENT_API_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef SA_HAS_WDB

/* The wire's one library dep is libcbor — and in this ecosystem libcbor
   arrives through the wavedb gate (wavedb-ext builds deps/wavedb's vendored
   copy; the root CMakeLists.txt SA_ENABLE_WDB block carries its include dirs
   and the archive onto the secretagent target). A wavedb-free build has no
   wire, so the whole surface self-gates the same way frame.h's store surface
   does. (If libcbor ever gains a gate of its own, lift this one.) */
#include <cbor.h>

/* The vocabulary + each frame's PINNED field order:
   CA_PROMPT_REQUEST     1: [1, req_id, text] | [1, req_id, sid, text]
                             — sid absent = create+start a top frame
   CA_PROMPT_RESPONSE    2: [2, req_id, status, sid] — sid "" = a steer
   CA_EVENTS_REQUEST     3: [3, req_id, sid, op, from_seq]
   CA_EVENTS_RESPONSE    4: [4, req_id, sid, seq, op, record_json]
   CA_INTERRUPT_REQUEST  5: [5, req_id, sid]
   CA_INTERRUPT_RESPONSE 6: [6, req_id, status]
   CA_SESSIONS_REQUEST   7: [7, req_id]
   CA_SESSIONS_RESPONSE  8: [8, req_id, rows] — row = [sid, status, goal,
                             created, depth]
   CA_ERROR             11: [11, req_id, status, text] — any pairing's
                             failure
   CA_AUTH_REQUEST      12: [12, req_id, api_key] — the TCP transport's
                             auth exchange (the unix socket needs none);
                             number mirrors liboffs's CLIENT_API_AUTH
                             REQUEST (its client_api_wire.h:22); our
                             payload carries the SAME req_id duality every
                             other request carries (liboffs's auth request
                             had none, and its api_key rode a bytestring
                             ours keeps as a bound text string — every
                             other field on this wire is text)
   CA_AUTH_RESPONSE     13: [13, req_id, status] — 0 = the connection is
                             authenticated; 1 = bad key (the connection
                             CLOSES after a bad-key response)
   CA_CONFIG_REQUEST    14: [14, req_id, base_url, api_key, model] — the
                             frame-config template's get/set (the session
                             server's frame_create source; "" = absent =
                             keep on EVERY field, so the all-"" shape —
                             and the 2-element form [14, req_id] — is the
                             GET; any present field makes it a SET)
   CA_CONFIG_RESPONSE   15: [15, req_id, status, base_url, api_key, model]
                             — the GET's answer ("" = absent) and the SET's
                             status-0 echo of the post-set template
   CA_ASK_REPLY_REQUEST 16: [16, req_id, sid, ask_id, decision, value] —
                             the parked ask's resolution ride (decision 0 =
                             answer, 1 = reject; value = the answer text,
                             "" = absent)
   CA_ASK_REPLY_RESPONSE 17: [17, req_id, delivered] — 1 = the reply
                             entered the frame's mailbox (bind/post ONLY —
                             the engine's stale drop is events-stream
                             truth, the ack contract §3.1) */
#define CA_PROMPT_REQUEST     1
#define CA_PROMPT_RESPONSE    2
#define CA_EVENTS_REQUEST     3
#define CA_EVENTS_RESPONSE    4
#define CA_INTERRUPT_REQUEST  5
#define CA_INTERRUPT_RESPONSE 6
#define CA_SESSIONS_REQUEST   7
#define CA_SESSIONS_RESPONSE  8
#define CA_ERROR              11
#define CA_AUTH_REQUEST       12
#define CA_AUTH_RESPONSE      13
#define CA_CONFIG_REQUEST     14
#define CA_CONFIG_RESPONSE    15
#define CA_ASK_REPLY_REQUEST  16
#define CA_ASK_REPLY_RESPONSE 17

#if defined(__cplusplus)
#define CA_STATIC_ASSERT static_assert
#else
#define CA_STATIC_ASSERT _Static_assert
#endif

/* liboffs's arithmetic pairing asserts (client_api_wire.h), renamed: a
   renumbering that breaks a pair must fail at compile time. (The messages
   are string literals — C11/C++ static_asserts take no bare identifiers;
   the plan sketch's identifier form does not compile.) */
CA_STATIC_ASSERT(CA_PROMPT_RESPONSE == CA_PROMPT_REQUEST + 1,
                 "prompt response must be prompt request + 1");
CA_STATIC_ASSERT(CA_EVENTS_RESPONSE == CA_EVENTS_REQUEST + 1,
                 "events response must be events request + 1");
CA_STATIC_ASSERT(CA_INTERRUPT_RESPONSE == CA_INTERRUPT_REQUEST + 1,
                 "interrupt response must be interrupt request + 1");
CA_STATIC_ASSERT(CA_SESSIONS_RESPONSE == CA_SESSIONS_REQUEST + 1,
                 "sessions response must be sessions request + 1");
CA_STATIC_ASSERT(CA_AUTH_RESPONSE == CA_AUTH_REQUEST + 1,
                 "auth response must be auth request + 1");
CA_STATIC_ASSERT(CA_CONFIG_RESPONSE == CA_CONFIG_REQUEST + 1,
                 "config response must be config request + 1");
CA_STATIC_ASSERT(CA_CONFIG_REQUEST == CA_AUTH_RESPONSE + 1,
                 "config request joins the vocabulary's adjacency — a "
                 "renumber that collides with the ERROR 11 / AUTH 12-13 "
                 "numbers fails here at compile time");
CA_STATIC_ASSERT(CA_ASK_REPLY_RESPONSE == CA_ASK_REPLY_REQUEST + 1,
                 "ask-reply response must be ask-reply request + 1");
CA_STATIC_ASSERT(CA_ASK_REPLY_REQUEST == CA_CONFIG_RESPONSE + 1,
                 "ask-reply request joins the vocabulary's adjacency — a "
                 "renumber that collides with the CONFIG 14-15 numbers "
                 "fails here at compile time");

/* The wire's field bounds (each decoder refuses over-bound strings loud —
   the caller answers CA_ERROR):
   TEXT_MAX bounds a prompt/steer's text; SID_MAX bounds a sid path (+
   headroom); STATUS_MAX bounds a sessions row's status word; RECORD caps a
   forwarded event-record JSON text (> that means the record is forwarded by
   REFERENCE — see events' contract in the spec); SESSIONS_MAX caps the
   sessions listing's rows. */
#define CA_WIRE_TEXT_MAX (64u * 1024u)
#define CA_WIRE_SID_MAX 128u
#define CA_WIRE_STATUS_MAX 32u
#define CA_WIRE_RECORD_MAX (128u * 1024u)
#define CA_WIRE_SESSIONS_MAX 256u
#define CA_WIRE_KEY_MAX 256u   /* an auth request's api_key (bcrypt keys are
                                  ~60 chars; 256 is generous headroom) — the
                                  CONFIG pair's api_key reuses this bound */
#define CA_WIRE_CONFIG_TEXT_MAX 512u   /* a CONFIG set's base_url */
#define CA_WIRE_CONFIG_TAG_MAX 128u    /* a CONFIG set's model tag */
#define CA_WIRE_ASK_ID_MAX 40u         /* an ask's minted id (the 8-hex
                                          shape ×5 headroom) */
#define CA_WIRE_REQ_ID_MAX UINT64_MAX

/* --- the payload types (plain C structs; the destroy frees their heap
   fields). THE req_id DUALITY: element 1 lands BOTH on the caller's
   *req_id out-param AND in the decoded payload's own req_id field (every
   struct's first member — the transports' bridges route on the out-param,
   the handlers read the field). ------------------------------- */
typedef struct ca_prompt_request_t {
  uint64_t req_id;
  char* sid;     /* heap or NULL */
  char* text;    /* heap */
} ca_prompt_request_t;

typedef struct ca_prompt_response_t {
  uint64_t req_id;
  uint8_t status;   /* 0 = accepted (the frame started/steered) */
  char* sid;        /* heap; the frame's sid on a start ("" when absent) */
} ca_prompt_response_t;

typedef enum ca_events_op_e {
  CA_EVENTS_REPLAY_THEN_LIVE = 0,   /* from_seq .. newest, then the live tail */
  CA_EVENTS_LIVE_ONLY = 1,          /* from the commit moment onward */
  CA_EVENTS_UNSUBSCRIBE = 2
} ca_events_op_e;

typedef struct ca_events_request_t {
  uint64_t req_id;
  char* sid;       /* heap */
  uint8_t op;      /* ca_events_op_e */
  uint64_t from_seq;
} ca_events_request_t;

typedef struct ca_events_response_t {
  uint64_t req_id;
  char* sid;            /* heap */
  uint64_t seq;         /* the record's seq; the LIVE-TRANSITION signal =
                           0 with op echoed in status */
  uint8_t op;           /* the echoing: the live marker carries it */
  char* record_json;    /* heap: the store record's JSON VERBATIM (never
                           re-parsed by the client); NULL = no record —
                           only the live marker may carry none */
} ca_events_response_t;

typedef struct ca_interrupt_request_t {
  uint64_t req_id;
  char* sid;
} ca_interrupt_request_t;

typedef struct ca_interrupt_response_t {
  uint64_t req_id;
  uint8_t status;   /* 0 = the interrupt posted into the frame's mailbox */
} ca_interrupt_response_t;

typedef struct ca_sessions_request_t {
  uint64_t req_id;
} ca_sessions_request_t;

typedef struct ca_sessions_record_t {
  char* sid;     /* heap */
  char* status;  /* heap ("running"/"done"/...) — or NULL */
  char* goal;    /* heap or NULL */
  uint64_t created;   /* meta/created's value, 0 when absent */
  size_t depth;
} ca_sessions_record_t;

typedef struct ca_sessions_response_t {
  uint64_t req_id;
  ca_sessions_record_t* records;   /* heap array, CA_WIRE_SESSIONS_MAX-bounded */
  size_t nrecords;
} ca_sessions_response_t;

typedef struct ca_error_t {
  uint64_t req_id;
  uint8_t status;   /* 1 = the request was refused; 2 = the frame unknown... */
  char* text;       /* heap */
} ca_error_t;

typedef struct ca_auth_request_t {
  uint64_t req_id;
  char* api_key;    /* heap; the presented key (bcrypt-checked against the
                       transport's hash) */
  size_t key_len;   /* the key's DECODED byte length (a CBOR string may
                       carry embedded NULs — strlen cannot see past the
                       first one; the destroy scrubs by this length) */
} ca_auth_request_t;

typedef struct ca_auth_response_t {
  uint64_t req_id;
  uint8_t status;   /* 0 = authenticated; 1 = bad key (the connection
                       closes after this frame) */
} ca_auth_response_t;

/* The CONFIG pair (the settings surface): the session server's frame-config
   template {base_url, api_key, model} travels the wire. The ALL-ABSENT
   request (every field NULL — the 2-element [14, req_id] form, or the
   5-element form with every "" sentinel) is the GET; any present field
   makes it a SET that mutates only the present ones (absent = unchanged;
   the template answers status 0 either way). The api_key rides the AUTH
   pair's scrub discipline: the decode records the string's byte length and
   the destroy scrubs by it before the free (strlen would stop at the first
   embedded NUL a CBOR string carried). */
typedef struct ca_config_request_t {
  uint64_t req_id;
  char* base_url;   /* heap or NULL (absent = keep); CONFIG_TEXT bound */
  char* api_key;    /* heap or NULL (absent = keep); KEY bound */
  size_t key_len;   /* the key's DECODED byte length (the destroy's scrub
                       count — set when api_key decoded, never by strlen) */
  char* model;      /* heap or NULL (absent = keep); TAG bound */
} ca_config_request_t;

typedef struct ca_config_response_t {
  uint64_t req_id;
  uint8_t status;   /* 0 = the fields below are the template's truth */
  char* base_url;   /* heap or NULL — the absent member rides the wire as "" */
  char* api_key;    /* heap or NULL; the destroy scrubs by key_len */
  size_t key_len;   /* the key's DECODED byte length (the scrub count) */
  char* model;      /* heap or NULL */
} ca_config_response_t;

/* The ASK_REPLY pair (the escalation slice's reply verb): the owner's
   resolution of a parked ask. The encoder is the trusted side — a decision
   > 1 encodes permissively and the DECODE refuses it loud (the same
   encode-permissive/decode-refusing split every bounded field runs). The
   response's delivered flag reflects ONLY the bind/post — the engine's
   stale-ask drop is events-stream truth (the spec §3.1's pinned ack
   contract). */
typedef struct ca_ask_reply_request_t {
  uint64_t req_id;
  char* sid;        /* heap; required */
  char* ask_id;     /* heap; required, <= 40 chars */
  uint8_t decision; /* 0 = answer, 1 = reject */
  char* value;      /* heap; may be empty → NULL by the "" rule */
} ca_ask_reply_request_t;
typedef struct ca_ask_reply_response_t {
  uint64_t req_id;
  uint8_t delivered; /* 1 = the reply entered the frame's mailbox (bind/post
                        ONLY — the engine's stale drop is events-stream
                        truth) */
} ca_ask_reply_response_t;

/* Encode one frame's payload into fresh CBOR bytes (cbor_serialize_alloc's
   buffer — free() it). The encoder is the TRUSTED side: field content is
   permissive there (the over-bound-text encode→decode-refusal shape is the
   plan's pinned test), while every decoder refuses malformed/over-bound
   shapes loud. payload is the type's own struct (the cast is the caller's
   switch); an unknown type refuses. */
int ca_wire_encode(uint64_t type, void* payload, uint8_t** out, size_t* out_len);

/* req_id/status outs fill only on success: req_id = element 1 (the echoed
   id); status = a response's status field, CA_ERROR's status, or — for
   CA_EVENTS_RESPONSE — its op (the live marker's echo is read off this
   out-param: seq == 0 + the echoed op = the transition to live tailing); a
   request decodes with status 0.
   On refusal: *type, *status, and *payload stay untouched (a NULL-init'd
   payload stays NULL), but req_id IS filled whenever the frame's req_id
   element decoded — an unknown type or a refusing PAYLOAD leaves it set
   (so an error frame can echo the request's req_id); only a not-an-array
   frame or a malformed element 0/1 leaves req_id untouched. */

/* The bytes form (the transports' convenience): cbor_load + decode, the
   loaded cbor destroyed inside; a malformed payload is a refusal. */
int ca_wire_decode_bytes(const uint8_t* raw, size_t raw_len, uint64_t* type,
                         void** payload, uint64_t* req_id, uint8_t* status);

/* The item form: a PRE-LOADED frame the caller still owns (a connection
   loads a frame once and routes it); never decref'd here. Same refusal
   rules. */
int ca_wire_decode(cbor_item_t* frame, uint64_t* type, void** payload,
                   uint64_t* req_id, uint8_t* status);

/* Free a decoded payload's heap fields + the struct itself. type tells the
   destroy the cast (the same switch that decoded it). */
void ca_wire_payload_destroy(uint64_t type, void* payload);

#endif /* SA_HAS_WDB */

#endif /* SA_CLIENT_API_WIRE_H */