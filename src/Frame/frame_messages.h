//
// Created by victor on 9/29/26.
//

#ifndef SA_FRAME_MESSAGES_H
#define SA_FRAME_MESSAGES_H

#include <stddef.h>
#include <stdint.h>

/* The reply-target actor borrowed by the store vocabulary's payloads; the
   Actor/actor.h definition arrives with the frame.h include in every
   consumer of these types. */
typedef struct actor_t actor_t;

/* Frame-layer message types (carried in the generic message_t envelope).

   The numeric base keeps the frame vocabulary numerically DISJOINT from the
   pyrt vocabulary (pyrt_messages.h, PYRT_EXECUTE..PYRT_INTERRUPT = 0..5):
   these message types share ONE physical queue — the owning frame actor's
   mailbox receives both the pyrt thread's PYRT_* posts and its cells' FRM_*
   bridge requests — so equal numeric values would silently misroute
   dispatch. */
typedef enum frame_message_type_e {
  FRM_REMEMBER = 0x1000,  /* cell -> frame: durable state write */
  FRM_RECALL,             /* cell -> frame: resolve a key up the lineage chain */
  FRM_REPLY,              /* frame -> pyrt: corr-matched answer to a bridge request */
  FRM_CELL_EXECUTE,       /* loop -> frame: run this cell (from the model's tool call) */
  FRM_STOP,               /* control: end the frame when the queue drains */
  FRM_SPAWN,              /* cell -> frame: admission-only child spawn */
  FRM_REPORT,             /* cell -> frame: the cell's frame.report verb */
  FRM_TURN,               /* engine -> itself: the scheduled turn-step
                             continuation (payload NULL) — the loop's turn
                             loop, dissolved */
  FRM_MODEL_RESULT,       /* transport -> frame: the model completion arrived
                             (frm_model_payload_t; see model.c's relay) */
  FRM_CHILD_REPORT,       /* child -> parent: this child's engine is terminal
                             (frm_child_report_payload_t; the parent
                             re-schedules) */
  /* --- the store vocabulary (the root's store actor owns WaveDB; see
     frame.c's _store_behavior — the ONE serializer of writes AND reads) --- */
  FRM_STORE_BATCH,        /* any frame route -> store actor: one atomic op
                             list (frm_store_batch_payload_t) */
  FRM_STORE_SCAN,         /* -> store actor: bounded reverse range read
                             (frm_store_scan_payload_t) — the derive's store
                             trip */
  FRM_STORE_RECALL,       /* -> store actor: the lineage resolve walk
                             (frm_store_recall_payload_t) */
  FRM_STORE_KEYS,         /* -> store actor: the keys verb's bounded OWN-
                             subtree scan (frm_store_keys_payload_t) — the
                             reply carries KEY NAME tail segments, never a
                             value; see the surface-completion spec §3 */
  FRM_STORE_GET_NAMED,    /* -> store actor: a DIRECT read of NAMED absolute
                             root-level keys (frm_store_get_named_payload_t)
                             — the persona read (persona spec §3): the reply
                             carries ONE JSON array record with the values'
                             keys attached (no positional ambiguity); see
                             the payload's shape note below */
  FRM_STORE_LIST_SESSIONS, /* -> store actor: the sessions listing (own
                              payload; the reply-router at the SERVER's
                              actor answers — a frame actor receiving the
                              reply = the standing loud drop) */
  FRM_STORE_WATCH,        /* -> store actor: subscribe a subtree (the
                             events' live tail) */
  FRM_STORE_UNWATCH,      /* -> store actor: drop a subscription */
  FRM_STORE_NOTICE,       /* store actor -> watcher: a committed matching
                             event record's shape */
  FRM_STORE_REPLY,        /* store actor -> requester: corr-matched result
                             (frm_store_reply_payload_t) */
  FRM_REPORT_BIND,        /* child frame actor -> parent frame actor: compose
                             the cross-subtree report batch THERE (the
                             parent's actor pre-allocates the parent's seq;
                             frm_report_bind_payload_t) */
  FRM_INT,                /* interrupt (owner -> the frame's own mailbox): cut
                             the pending cell + close an open turn aborted —
                             the reason union's RESERVED `aborted` first
                             writer. No payload. */
  FRM_KEYS,               /* cell -> frame: the keys verb's OWN-subtree key
                             listing (frm_remember_payload_t; `key` carries
                             the CLOSED-SET scope "local" | "ctx" — anything
                             else is the fail-loud refusal before any post) */
  FRM_CELL_WATCHDOG,       /* the POOLED cell's deadline fired: the SAME
                             interrupt synthesis runs under the watchdog
                             wording (arm_cut = 0 — a deadline never arms a
                             cut). Payload = the frame.c-private watchdog
                             struct, handed off whole; its destroyer frees
                             it on the frame's thread — see frame.c */
  FRM_STEER,               /* wire/server -> the frame's OWN mailbox: compose
                             the durable msg.append fire-and-post in the
                             frame's dispatch (frm_steer_payload_t). The
                             POSTED shape of the steer: frame_append_msg is
                             a synchronous store API (pump-wait) and is
                             therefore inline-store-only — a caller NOT on
                             the frame's thread (the client-api handlers on
                             the loop thread) posts the compose instead.
                             frame.h's frame_interrupt is the posted-verb
                             precedent; like it, the post is legal from any
                             thread. */
  FRM_ASK,                 /* cell -> frame: the model's blocked-ask verb
                             (py_agent.c's agent.ask) — the FIRE-AND-POST
                             publish of the owner-surface question + options
                             (frm_ask_payload_t); nothing crosses back
                             through the bridge sink, and the ENGINE parks
                             the turn at the close batch (escalation spec
                             §1.2). Resolution = FRM_ASK_REPLY. */
  FRM_ASK_REPLY            /* owner surface -> engine input: a parked ask's
                             resolution (frm_ask_reply_payload_t) — validated
                             against the engine's pending_ask and consumed
                             EXACTLY ONCE; a stale ask_id is dropped loud
                             (escalation spec §1.4). */
} frame_message_type_e;

/* Event types (stored at sessions/<sid>/events/<seq>, JSON, %020d seq). ONLY
   msg.append produces context. */
typedef enum frame_event_type_e {
  EV_MSG_APPEND = 0,    /* payload {role, content} */
  EV_FRAME_SPAWN,       /* payload {child_sid, goal, depth} */
  EV_FRAME_REPORT,      /* payload {child_sid, text} — the one context-producing child event */
  EV_FRAME_JOIN,        /* payload {child_sid} */
  EV_CELL_RUN,          /* payload {code, corr} */
  EV_CELL_RESULT,       /* payload {corr, status, text} */
  EV_STATE_REMEMBER,    /* payload {key, value} */
  EV_CONTROL,           /* payload {kind, text} — interrupt/shutdown/error */
  EV_ASK,               /* payload {askId, question, options[]} — the
                           published blocked-ask record (escalation spec
                           §1.2; ordinary durable event: seq-ordered,
                           replayed, delivered through the events channel) */
  EV_ASK_REPLY          /* payload {askId, decision, value} — a parked ask's
                           resolution record (escalation spec §1.4); the
                           decision renders "answer" | "reject" */
} frame_event_type_e;

/* Bridge request payloads (ownership transfers with the message): */
typedef struct frm_remember_payload_t { uint64_t corr; char* key; char* json_value; } frm_remember_payload_t;
/* spawn: admission-only child request; context_json NULL = no handoff key. */
typedef struct frm_spawn_payload_t { uint64_t corr; char* goal; char* context_json; } frm_spawn_payload_t;
/* report: the cell's frame ends with this text. */
typedef struct frm_report_payload_t { uint64_t corr; char* text; } frm_report_payload_t;
/* reply: corr + status + heap text */
typedef struct frm_reply_payload_t { uint64_t corr; uint8_t status; char* text; } frm_reply_payload_t;
/* cell execute (loop -> frame): `corr` pairs the cell.run event with the
   cell.result event on the audit trail (the LOOP's corr space — pyrt's
   executor corr stays inside the runtime). Ownership of `code` transfers. */
typedef struct frm_cell_payload_t { uint64_t corr; char* code; } frm_cell_payload_t;
void frm_remember_payload_destroy(void* p);
void frm_spawn_payload_destroy(void* p);
void frm_report_payload_destroy(void* p);
void frm_reply_payload_destroy(void* p);
void frm_cell_payload_destroy(void* p);

/* Model completion (transport -> frame): the http body and error move in RAW
   (steal-slot, exactly model.c's completion record shape); the
   FRM_MODEL_RESULT behavior decodes via model_internal.h on the frame's own
   thread — the streams completion stays µs-scale. Ownership of body/error
   transfers with the message. */
typedef struct frm_model_payload_t {
  int status;       /* http status, or -1 on transport failure */
  char* body;       /* heap; steal-slot */
  size_t body_len;
  char* error;      /* heap transport reason on status -1 */
  /* The sink's parsed Retry-After, in SECONDS (0 = absent): a VALUE — the
     header capture itself died in the sink, so no heap member rides here
     and the destroyer stays untouched. The guards table's rate class reads
     it at the engine's reply branch. */
  unsigned retry_after_sec;
} frm_model_payload_t;
/* Child terminal: bookkeeping only — the parent-side report binding ALREADY
   happened in the child's ONE cross-subtree report-bind batch (composed by
   the parent's actor, executed by the store actor, and its corr-matched
   reply confirmed committed BEFORE this message is posted — see frame.c's
   FRM_REPORT_BIND); this message RESUMES the parent's live engine. failed =
   the child ended on a control event (the parent's thread logs the resume
   loudly for it). */
typedef struct frm_child_report_payload_t { char* child_sid; uint8_t failed; } frm_child_report_payload_t;
void frm_model_payload_destroy(void* p);
void frm_child_report_payload_destroy(void* p);

/* --- the store vocabulary payloads (all owned by the store round trip;
   ownership of heap fields transfers with the message) --------------------- */

/* One op. Ownership of key and value transfers with the payload; the store
   behavior frees them after it acts. is_delete = DELETE op (WaveDB
   raw_op_t.type 1): the store behavior REFUSES loud (a deletion never
   smuggles bytes) a delete op carrying a value or a length. The harness
   entry-withdrawal is the first user (the LOG never deletes — frame layer
   events stay puts). */
typedef struct frm_store_op_t { char* key; uint8_t* value; size_t value_len; uint8_t is_delete; } frm_store_op_t;

/* A batch = ONE atomic root database_batch_sync_raw across every touched
   subtree (the composed full-root-path keys — the existing spawn/report/
   shape, unchanged). op_name is a BORROWED label the store worker logs on
   refusal. reply_to NULL = fire-and-post (control/audit writes nothing
   waits on). corr 0 = fire-and-post likewise. */
typedef struct frm_store_batch_payload_t {
  frm_store_op_t* ops; size_t nops;
  const char* op_name;
  actor_t* reply_to;    /* BORROWED; NULL = fire-and-post */
  uint64_t corr;        /* the requester's round-trip key (0 = fire-and-post) */
} frm_store_batch_payload_t;

/* Bounded reverse range read: the bounds are absolute ROOT-LEVEL composed
   paths (WaveDB's subtree bounded scans are broken in both directions — the
   root-level absolute-bounds discipline, unchanged). */
typedef struct frm_store_scan_payload_t {
  char* start;          /* OWNED; e.g. "sessions/<sid>/events" */
  char* end;            /* OWNED; e.g. "sessions/<sid>/events0" */
  size_t limit;         /* newest-record cap; 0 or > the store's window max
                           clamps to the store's max window */
  actor_t* reply_to;    /* BORROWED; never NULL */
  uint64_t corr;
} frm_store_scan_payload_t;

/* The recall resolve walk (own local/ -> own ctx/ -> meta/parent hops' ctx/),
   relocated INTO the store actor: it is a pure store computation over the
   walk's subtree reads, and this keeps the frame's behavior lock-free. */
typedef struct frm_store_recall_payload_t {
  char* key;          /* OWNED; non-empty, free of '/' */
  char* sid_path;     /* OWNED; where the walk starts */
  unsigned max_hops;  /* the frame's depth budget (max_depth) */
  actor_t* reply_to;  /* BORROWED */
  uint64_t corr;
} frm_store_recall_payload_t;

/* The named direct read (the persona slice, spec §3): a LIST of absolute
   ROOT-LEVEL keys (the scan discipline — never relative bounds), each read
   DIRECTLY (database_get_sync_raw — the persona records are STATE reads,
   not walks, not scans). The reply is ONE record (records[0], rc 0): a JSON
   array with one {"key": <the requested key verbatim>, "value": <the raw
   text or null>} entry per REQUESTED key, IN THE REQUESTED ORDER — the
   store ATTACHES each fetched value to its key, so a missing key answers
   "value": null as a first-class fact (the persona fallback reads it) and
   no reply ever leaves the reader guessing which value came from where.
   Values are TEXT records read as C strings (the persona records' scope:
   utf8 text; a value's interior NUL never appears in a text record). */
typedef struct frm_store_get_named_payload_t {
  char** keys;          /* OWNED; nkeys absolute root-level key texts
                           (ownership of every string transfers with the
                           payload, on every path) */
  size_t nkeys;
  actor_t* reply_to;    /* BORROWED; never NULL (the persona trip awaits) */
  uint64_t corr;
} frm_store_get_named_payload_t;

/* The keys verb's bounded scan (surface-completion spec §3): the reverse
   range read over OWN sid_path/state/<scope> (absolute composed bounds —
   the root-level discipline); the reply's records[] carry the scanned keys'
   NAME tail segments — never a value crosses back (recall resolves values;
   keys never mixes them). A child lists its own keys only: the scan bounds
   carry the requesting frame's OWN sid_path, so the no-third-path rule
   needs no extra code. */
typedef struct frm_store_keys_payload_t {
  char* sid_path;     /* OWNED; the requesting frame's own subtree */
  char* scope;        /* OWNED; "local" or "ctx" (the closed set, re-validated
                         here — the frame dispatch checked it first) */
  actor_t* reply_to;  /* BORROWED */
  uint64_t corr;
} frm_store_keys_payload_t;

/* The round-trip result. rc = 0 committed / the store's refusal code.
   `records` = the MATERIALIZED RAW record texts (heap, ascending order,
   OWNED) for scans; the recall's resolution is records[0] or rc != 0; batch
   replies carry n == 0 / records == NULL. PAYLOAD DECISION (owner asked to
   settle it from the code): the store worker does NOT parse JSON — raw
   texts are honest (the store deals in store values) and the cheap side of
   the trade (no serialize-then-reparse round trip); the requester's router
   parses each record (µs, bounded 512) where the projection already
   consumes a DOM. Unparseable raw records are dropped LOUD by the router. */
typedef struct frm_store_reply_payload_t {
  uint64_t corr; int rc; size_t n; char** records;
} frm_store_reply_payload_t;

/* child -> parent: the cross-subtree report binding composed at the parent's
   actor (ONE batch: the child's record at the child's PRE-ALLOCATED seq —
   already composed and carried here — + the child's meta/status=done + the
   parent's bound report event at the parent's own pre-allocated seq).
   engine_driven (the TERMINATE path) is what makes the child's router post
   FRM_CHILD_REPORT on the reply; a cell-verb report or a direct sync-API
   report only binds (the report-verb engine end posts its resume via its
   own terminate at the next is_done turn). */
typedef struct frm_report_bind_payload_t {
  actor_t* reply_to;        /* BORROWED: the CHILD's actor receives the store reply */
  uint64_t corr;            /* the child's own store round-trip key */
  uint64_t bridge_corr;     /* the cell's bridge corr, or 0 when not cell-side */
  uint8_t engine_driven;    /* 1 = post FRM_CHILD_REPORT on the store reply */
  char* child_sid;          /* OWNED */
  uint64_t child_seq;       /* the child's pre-allocated seq */
  char* child_event_text;   /* OWNED; the child's own frame.report record JSON */
  char* text;               /* OWNED; the report text (the parent re-composes its bound event) */
} frm_report_bind_payload_t;

/* The posted steer (FRM_STEER): one durable msg.append composed by the
   frame's OWN dispatch (the seq's single writer is the frame's thread — the
   handler could never pre-allocate honestly from the loop thread). Role and
   text HEAP+OWNED, transfer with the message; NULL fields refuse loud at
   the dispatch (same validation as frame_append_msg). */
typedef struct frm_steer_payload_t { char* role; char* text; } frm_steer_payload_t;

/* The blocked-ask publish (FRM_ASK): the model's question + closed options,
   posted FIRE-AND-POST by the agent.ask verb (py_agent.c's emit shape — no
   bridge-sink reply ever crosses back; the resolution is FRM_ASK_REPLY and
   the model reads it in the next turn's derive). corr rides the bridge corr
   (the reply-sink space) for the audit pairing; the engine mints the durable
   ask_id at receipt. Heap+OWNED fields transfer with the message
   (frm_ask_payload_destroy frees question + each option + the array). */
typedef struct frm_ask_payload_t {
  uint64_t corr;            /* the bridge corr (the reply-sink space) */
  char* question;
  char** options;           /* owned array of owned strings; NULL = no options */
  size_t noptions;
} frm_ask_payload_t;

/* The parked ask's resolution (FRM_ASK_REPLY): the engine consumes it
   against its pending_ask EXACTLY ONCE; a stale ask_id is dropped loud.
   ask_id carries the engine-minted "%08x" key (the sid allocator's shape);
   decision 0 = answer, 1 = reject; value = the answer text or the refusal
   text — may be empty. */
typedef struct frm_ask_reply_payload_t {
  char* ask_id;             /* the "%08x"-shaped key — the sid allocator's */
  uint8_t decision;         /* 0 = answer, 1 = reject */
  char* value;              /* the answer/refusal text — may be empty */
} frm_ask_reply_payload_t;

/* The client-API sessions listing (spec §3): the store enumerates the
   root's sessions/ first-level entries + each entry's meta/{created,
   status,depth,goal} and answers via the round-trip reply — records[] =
   ONE heap JSON row per session, {"sid","status","goal","created","depth"}
   (goal is the empty sentinel: no composer writes a meta/goal key today).
   The reply's ROUTER lives at the SERVER's actor (Task 4's
   ca_session_server); reply_to NULL = refused loud (the listing exists to
   be answered). */
typedef struct frm_store_sessions_payload_t { uint64_t corr; actor_t* reply_to; } frm_store_sessions_payload_t;

/* One events subscription (the live tail): the store's dispatch thread
   owns the subscription list (no locks); `watcher` is BORROWED into every
   notice. RECORDED SHAPE: an unwatch is the ONLY removal — a watcher
   destroyed WITHOUT unwatching (a connection torn down before its teardown
   sent the unwatch) keeps its notices posting, and each post refuses with
   the dead-target loud drop, until the unwatch arrives. The transports'
   teardown discipline (unwatch-before-destroy) is the contract. */
typedef struct frm_store_watch_t { char* sid_path; actor_t* watcher; } frm_store_watch_t;

/* The notice: ONE committed event record under a watched subtree. The
   store composes it on its own dispatch thread with SELF-OWNED copies
   (sid_path copied from the key, record_json strduplicated out of the
   batch's op value BEFORE the reply machinery reclaims it), so the payload
   survives the batch entirely. */
typedef struct frm_store_notice_t { char* sid_path; uint64_t seq; char* record_json; } frm_store_notice_t;

void frm_store_batch_payload_destroy(void* p);
void frm_store_sessions_payload_destroy(void* p);
void frm_store_watch_destroy(void* p);
void frm_store_notice_destroy(void* p);
void frm_store_scan_payload_destroy(void* p);
void frm_store_recall_payload_destroy(void* p);
void frm_store_keys_payload_destroy(void* p);
void frm_store_get_named_payload_destroy(void* p);
void frm_store_reply_payload_destroy(void* p);
void frm_report_bind_payload_destroy(void* p);
void frm_steer_payload_destroy(void* p);
void frm_ask_payload_destroy(void* p);
void frm_ask_reply_payload_destroy(void* p);

/* JSON event record shape (authoritative):
   {"seq":<int>,"type":"<event-name>","frame":"<sid-path>","corr":<int|null>,
    "at":"<iso>","cause":<int|null>,"payload":{...}}  */

#endif // SA_FRAME_MESSAGES_H