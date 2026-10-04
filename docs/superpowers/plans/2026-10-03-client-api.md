# Client API Implementation Plan (wire + transports + handlers + C client + CLI)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the runtime its consumer surface — the liboffs ClientAPI pattern transposed to SecretAgent: one CBOR wire, TWO ported transports (unix + TCP), transport-blind handlers over the store/frame actors, a typed C client, and the demo CLI's serve/client modes on it.

**Architecture:** `src/Network/stream_framer.{h,c}` ported (byte-except-includes); `src/ClientApi/` = wire + handlers + `Unix/` + `Tcp/` dirs (liboffs's transport dirs ported with our handler dispatch substituted); the store actor gains the subscription/notify machinery (store_notify) the live event tail rides; `src/ClientLibs/c/sa_client.{h,c}` transposes offs_client's client idiom; the CLI gains serve/client modes while its direct mode (and all 266 tests) stay untouched.

**Tech Stack:** C11; libcbor (already in the ecosystem via WaveDB); poll-dancer + src/Streams' loop thread; GoogleTest; WaveDB store actor.

**Spec:** docs/superpowers/specs/2026-10-03-client-api-design.md — READ FIRST.
**Port sources (READ-ONLY, never modified):** `/home/victor/Workspace/src/github.com/vijayee/liboffs/src/Network/stream_framer.{h,c}`, `/home/victor/Workspace/src/github.com/vijayee/liboffs/src/ClientAPI/Unix/{unix_transport,unix_connection}.{h,c}`, `/home/victor/Workspace/src/github.com/vijayee/liboffs/src/ClientAPI/TCP/{tcp_transport,tcp_connection}.{h,c}`, `ClientLibs/c/offs_client.{h,c}` (pattern only), `ClientAPI/client_api_wire.{h,c}` (idiom only — the vocabulary is OURS).

**Standing bars (every task):** `setarch -R cmake --build cmake-build-debug -j && setarch -R ctest --test-dir cmake-build-debug --output-on-failure` (266/266 at fecd2e5); valgrind on `strip --strip-debug` copies (symtab KEPT); ASan for loop-thread paths; no TODOs; no Co-Authored-By; conventional commits on master; STYLE_GUIDE.md first; the read-only reference dirs stay untouched.

---

### Task 1: The framer port

**Files:**
- Create: `src/Network/stream_framer.h`, `src/Network/stream_framer.c` (ported from liboffs `src/Network/stream_framer.{h,c}` — 146 lines)
- Create: `test/test_stream_framer.cpp`
- Modify: `test/CMakeLists.txt` (the plain gate — the framer is pure)

- [ ] **Step 1: Port** — copy liboffs's two files; adapt includes to OUR tree paths (`../Util/allocator.h` etc. — liboffs's layout matches ours for Util/Actor/Platform; verify against liboffs's includes in the .c and fix paths). The API: `stream_frame_encode`, `stream_framer_create/destroy/feed/next`, `STREAM_FRAMER_MAX_FRAME_SIZE` 2 MB. Keep liboffs's comments (audit #3's slow-drip note).
- [ ] **Step 2: The failing test — write it FIRST, then port (TDD reversed here means: test compiles against the real API; port immediately after so the red phase is only the build):**

```cpp
//
// Created by victor on 10/03/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
extern "C" {
#include "../src/Network/stream_framer.h"
}

TEST(TestStreamFramer, TestEncodeDecodeRoundTrip) {
  const char* msg = "hello framer";
  size_t framed_len = 0;
  uint8_t* framed = stream_frame_encode((const uint8_t*)msg, strlen(msg),
                                        &framed_len);
  ASSERT_NE(framed, nullptr);
  EXPECT_EQ(framed_len, strlen(msg) + 4);
  EXPECT_EQ(framed[0], 0) << "big-endian length prefix: <64KiB message";
  EXPECT_EQ(framed[1], 0);

  stream_framer_t* fr = stream_framer_create();
  ASSERT_NE(fr, nullptr);
  /* feed ONE byte at a time — the accumulator must reassemble */
  for (size_t i = 0; i < framed_len; i++) {
    EXPECT_EQ(stream_framer_feed(fr, framed + i, 1), 0);
  }
  size_t out_len = 0;
  uint8_t* out = stream_framer_next(fr, &out_len);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out_len, strlen(msg));
  EXPECT_EQ(memcmp(out, msg, out_len), 0);
  free(out);
  EXPECT_EQ(stream_framer_next(fr, &out_len), nullptr) << "no spurious frames";
  stream_framer_destroy(fr);
  free(framed);
}

TEST(TestStreamFramer, TestTwoFramesInOneFeed) {
  size_t a_len = 0, b_len = 0;
  uint8_t* a = stream_frame_encode((const uint8_t*)"A", 1, &a_len);
  uint8_t* b = stream_frame_encode((const uint8_t*)"BBB", 3, &b_len);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  std::vector<uint8_t> both(a, a + a_len);
  both.insert(both.end(), b, b + b_len);
  free(a);
  free(b);

  stream_framer_t* fr = stream_framer_create();
  ASSERT_NE(fr, nullptr);
  ASSERT_EQ(stream_framer_feed(fr, both.data(), both.size()), 0);
  size_t out_len = 0;
  uint8_t* out = stream_framer_next(fr, &out_len);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out_len, 1u);
  EXPECT_EQ(out[0], 'A');
  free(out);
  out = stream_framer_next(fr, &out_len);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out_len, 3u);
  EXPECT_EQ(out[0], 'B');
  free(out);
  stream_framer_destroy(fr);
}

TEST(TestStreamFramer, TestOversizedDeclaredLengthRefuses) {
  /* a peer advertising a length over the cap: feed() refuses loud (the
     slow-drip defense — liboffs's audit #3) */
  stream_framer_t* fr = stream_framer_create();
  ASSERT_NE(fr, nullptr);
  uint8_t header[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};   /* way over 2 MB */
  EXPECT_EQ(stream_framer_feed(fr, header, 4), -1);
  stream_framer_destroy(fr);
}

TEST(TestStreamFramer, TestCapIsTwoMB) {
  EXPECT_EQ(STREAM_FRAMER_MAX_FRAME_SIZE, (size_t)(2 * 1024 * 1024));
}
```

- [ ] **Step 3: Build + run green** (the suite +4 = 270/270), valgrind on the stripped copy.
- [ ] **Step 4: Commit**

```bash
git add src/Network/stream_framer.h src/Network/stream_framer.c test/test_stream_framer.cpp test/CMakeLists.txt
git commit -m "feat: the stream framer (ported from liboffs Network; the 2MB slow-drip defense)"
```

---

### Task 2: The wire vocabulary

**Files:**
- Create: `src/ClientApi/client_api_wire.h`, `src/ClientApi/client_api_wire.c`
- Create: `test/test_client_api_wire.cpp`
- Modify: `test/CMakeLists.txt` (the plain gate)

- [ ] **Step 1: The failing tests** (mirror test_guards.cpp's registration; the CBOR dep's include path — CHECK how WaveDB reaches <cbor.h>: grep the deps' CMake for the cbor include dir and mirror it here; if it's WaveDB-internal, the wire's CMake target needs the same include dir + a link against the libcbor artifact WaveDB already vendors — READ deps/wavedb/CMakeLists.txt's cbor handling first and replicate; report the mechanism you found):

```cpp
//
// Created by victor on 10/03/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <string>
extern "C" {
#include "../src/ClientApi/client_api_wire.h"
}

TEST(TestClientApiWire, TestPromptRequestRoundTrip) {
  ca_prompt_request_t req = {0};
  req.req_id = 7;
  req.sid = NULL;                 /* absent = create+start */
  req.text = "make the thing";
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  int rc = ca_wire_encode(CA_PROMPT_REQUEST, &req, &raw, &raw_len);
  ASSERT_EQ(rc, 0);
  ASSERT_NE(raw, nullptr);

  uint64_t type = 0;
  ca_prompt_request_t* back = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  rc = ca_wire_decode(raw, raw_len, &type, (void**)&back, &req_id, &status);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(type, (uint64_t)CA_PROMPT_REQUEST);
  EXPECT_EQ(req_id, 7u);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->sid, nullptr);
  EXPECT_STREQ(back->text, "make the thing");
  ca_wire_payload_destroy(CA_PROMPT_REQUEST, back);
  free(raw);
}

TEST(TestClientApiWire, TestPromptResponsePairing) {
  ca_prompt_response_t res = {0};
  res.req_id = 7;
  res.status = 0;
  res.sid = "sessions/abc123";
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_PROMPT_RESPONSE, &res, &raw, &raw_len), 0);
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  ASSERT_EQ(ca_wire_decode(raw, raw_len, &type, &payload, &req_id, &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_PROMPT_RESPONSE) << "the response = request + 1";
  ca_wire_payload_destroy(CA_PROMPT_RESPONSE, payload);
  free(raw);
}

TEST(TestClientApiWire, TestBoundedStringsRefuse) {
  /* a wire payload with an oversize text is REFUSED at decode — the wire
     never trusts its peer */
  std::string big(CA_WIRE_TEXT_MAX + 1, 'x');
  ca_prompt_request_t req = {0};
  req.req_id = 1;
  req.sid = NULL;
  req.text = big.c_str();
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_PROMPT_REQUEST, &req, &raw, &raw_len), 0);
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  EXPECT_NE(ca_wire_decode(raw, raw_len, &type, &payload, &req_id, &status), 0);
  free(raw);
}

TEST(TestClientApiWire, TestUnknownTypeAndGarbageAnswerError) {
  uint8_t garbage[] = {0xFFu, 0x01u, 0x02u, 0x03u};
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  EXPECT_NE(ca_wire_decode(garbage, sizeof(garbage), &type, &payload, &req_id,
                           &status), 0)
      << "malformed CBOR is a decode refusal (the caller sends CA_ERROR)";
  ca_sessions_request_t sreq = {0};
  sreq.req_id = 2;
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_SESSIONS_REQUEST, &sreq, &raw, &raw_len), 0);
  /* decode a SESSIONS frame AS IF it were a PROMPT: the type switch owns the
     cast — decoding a type against the wrong payload struct is the caller's
     switch, not the wire's problem; pin the type IS readable though */
  ASSERT_EQ(ca_wire_decode(raw, raw_len, &type, &payload, &req_id, &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_SESSIONS_REQUEST);
  ca_wire_payload_destroy(CA_SESSIONS_REQUEST, payload);
  free(raw);
}
```

- [ ] **Step 2: Write `client_api_wire.h`** — the vocabulary + pairing asserts + decode bounds:

```c
//
// Created by victim on 10/03/26.
//

/* The agent surface's client-API wire (the client-api spec §1): CBOR arrays,
   message type first element; the RESPONSE type = REQUEST+1; every request
   carries a req_id the response echoes. liboffs's pairing-assert discipline
   verbatim. Bounded fields: the wire never trusts its peer. */

#ifndef SA_CLIENT_API_WIRE_H
#define SA_CLIENT_API_WIRE_H

#include <cbor.h>
#include <stddef.h>
#include <stdint.h>

#define CA_PROMPT_REQUEST     1
#define CA_PROMPT_RESPONSE    2
#define CA_EVENTS_REQUEST     3
#define CA_EVENTS_RESPONSE    4
#define CA_INTERRUPT_REQUEST  5
#define CA_INTERRUPT_RESPONSE 6
#define CA_SESSIONS_REQUEST   7
#define CA_SESSIONS_RESPONSE  8
#define CA_ERROR              11

#if defined(__cplusplus)
#define CA_STATIC_ASSERT static_assert
#else
#define CA_STATIC_ASSERT _Static_assert
#endif

/* liboffs's arithmetic pairing asserts (client_api_wire.h), renamed: a
   renumbering that breaks a pair must fail at compile time. */
CA_STATIC_ASSERT(CA_PROMPT_RESPONSE == CA_PROMPT_REQUEST + 1,
                 ca_prompt_pairing);
CA_STATIC_ASSERT(CA_EVENTS_RESPONSE == CA_EVENTS_REQUEST + 1,
                 ca_events_pairing);
CA_STATIC_ASSERT(CA_INTERRUPT_RESPONSE == CA_INTERRUPT_REQUEST + 1,
                 ca_interrupt_pairing);
CA_STATIC_ASSERT(CA_SESSIONS_RESPONSE == CA_SESSIONS_REQUEST + 1,
                 ca_sessions_pairing);

/* The wire's field bounds (each decoder refuses over-bound strings loud —
   the caller answers CA_ERROR):
   TEXT_MAX bounds a prompt/steer's text; SID_MAX bounds a sid path (+
   headroom); RECORD caps a forwarded event-record JSON text (> that means
   the record is forwarded by REFERENCE — see events' contract in the
   spec); the sessions listing caps its rows. */
#define CA_WIRE_TEXT_MAX (64u * 1024u)
#define CA_WIRE_SID_MAX 128u
#define CA_WIRE_RECORD_MAX (128u * 1024u)
#define CA_WIRE_SESSIONS_MAX 256u
#define CA_WIRE_REQ_ID_MAX UINT64_MAX

/* --- the payload types (plain C structs; the destroy frees their heap
   fields) --------------------------------------------------------------- */
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
                           re-parsed by the client) */
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

int ca_wire_encode(uint64_t type, void* payload, uint8_t** out, size_t* out_len);
int ca_wire_decode(const uint8_t* raw, size_t raw_len, uint64_t* type,
                   void** payload, uint64_t* req_id, uint8_t* status);
void ca_wire_payload_destroy(uint64_t type, void* payload);

#endif /* SA_CLIENT_API_WIRE_H */
```

- [ ] **Step 3: Write `client_api_wire.c`** — liboffs's helper shapes (`_encode_string`/`_decode_string` with the max-len param + the "" sentinel for absent sid fields, the size guard `_decode_u64`): encode per type (an ARRAY whose first item is the type; fields in a PINNED order — document each type's layout in a comment; req_id FIRST FIELD after the type); decode: cbor_deserialize type check → per-type parse into the struct (every over-bound/malformed field = nonzero rc, the caller answers CA_ERROR); the destroy per type. The events response's live marker: seq == 0 && op echoed (the doc's signal). The sessions response encodes `record_json`-free rows ({sid,status,goal,created,depth}).
- [ ] **Step 4: Build + run green** (suite +4), valgrind.
- [ ] **Step 5: Commit**

```bash
git add src/ClientApi/client_api_wire.h src/ClientApi/client_api_wire.c test/test_client_api_wire.cpp test/CMakeLists.txt
git commit -m "feat: the client-api CBOR wire (four pairs + error; bounded, id-paired)"
```

---

### Task 3: The store side — sessions listing + the subscription machinery

**Files:**
- Modify: `src/Frame/frame_messages.h` (`FRM_STORE_LIST_SESSIONS` + `FRM_STORE_WATCH`/`FRM_STORE_UNWATCH` + `frm_store_notice_t`)
- Modify: `src/Frame/frame_internal.h` (the handlers' private contract: the watch/notice API + the sessions scan)
- Modify: `src/Frame/frame.c` (the store behavior's three cases + the notify fan-out at the batch commit point `frame.c:1324`)
- Modify: `test/test_frame.cpp`

- [ ] **Step 1: Failing tests** (test_frame.cpp — the harness's real frame/store shapes; three tests):
  - `TestStoreListsSessionsAndTheirMeta`: create two frames (one with a pool; one inline), remember nothing, then LIST (via the new `_frame_store_list_sessions_sync(f-root-ish)` — the harness drives the store the test_frame way); assert the two sids + meta/status ("running"/"done") + goal + depth; cap honored (a 300-session listing clamps — SKIP the 300 shape: bounded at CA_WIRE_SESSIONS_MAX with the loud-truncation log).
  - `TestStoreNotifyWatchesASubtree`: watch `sessions/<sid>` → dispatch a store batch that touches ANOTHER sid's events → no notice; a batch that touches the WATCHED sid's `events/<seq>` → the notice arrives on the watcher's actor with the right {sid, seq, record text}; then unwatch → silent.
  - `TestStoreNoticeCarriesTheCommittedRecord`: the notice's record_json is the committed record's exact text (the batch's op value).
- [ ] **Step 2: The vocabulary** (frame_messages.h):

```c
  FRM_STORE_LIST_SESSIONS, /* -> store actor: the sessions listing (its own
                               payload: frm_store_sessions_payload_t — the
                               reply's ROUTER answers with the records) */
  FRM_STORE_WATCH,         /* -> store actor: subscribe a subtree (the events'
                               live tail rides this) */
  FRM_STORE_UNWATCH,       /* -> store actor: drop a subscription */
  FRM_STORE_NOTICE         /* store actor -> watcher: {sid_path, seq, record_json}
                               for one committed matching record */
```
  + payload types (`frm_store_notice_t {char* sid_path; uint64_t seq; char* record_json;}` + destroyer; the watch payload `{char* sid_path; actor_t* watcher;}` — the WATCHER's actor pointer (the transport's connection actor borrows it)).
- [ ] **Step 3: The store behavior**:
  - `FRM_STORE_LIST_SESSIONS`: the bounded enumeration of the root's `sessions/` first-level entries + their `meta/{created,status,depth,goal}` reads — inside the store's dispatch (the single serializer; use `database_scan_sync_raw`-family per the API list at deps/wavedb/src/Database/database.h:369-373; bound the walk with the same scan-window rules; collect DISTINCT sid segments from the composed root-level bounds).
  - The WATCH/UNWATCH list: a small linked list on the store actor's state (the dispatch-thread domain — no locks); `_store_actor_watch/unwatch` compose + post; on each SUCCESSFUL `database_batch_sync_raw` at frame.c:1324: for each committed op key, for each watcher whose sid_path is a PREFIX of the op's key AND the key sits under that sid's `events/`: compose ONE notice per (sid, seq) per watcher (the record's committed VALUE text rides — the op's value bytes, freed by the reply machinery as usual... CAREFUL: the ops' values transfer to the store's own teardown — the notice STRDUPs the bytes before the reply machinery reclaims) → post `FRM_STORE_NOTICE` to the watcher's actor.
  - The router: a notice arriving at a frame actor (not the transport) = the standing loud drop (only transports/consumers watch).
- [ ] **Step 4: Green + valgrind (the notice's strdup ownership on both the delivered and unwatched-in-flight paths)** + ASan cheap. Commit:

```bash
git add src/Frame/frame_messages.h src/Frame/frame_internal.h src/Frame/frame.c test/test_frame.cpp
git commit -m "feat: the store side of the client api — sessions listing + the events' subscription fan-out"
```

---

### Task 4: The handlers (the actor-glue)

**Files:**
- Create: `src/ClientApi/handlers.h`, `src/ClientApi/handlers.c`
- Create: `test/test_client_api_handlers.cpp`
- Modify: `test/CMakeLists.txt` (the WDB gate — the handlers need the store)

- [ ] **Step 1: The failing tests** (the in-proc wiring: a real wave_database_root + one frame tree + a CONNECTION-actor test double that plays the transport: its dispatch receives the frames the handlers post back; the harness is a small actor struct with a frame queue — mirroring test_frame's store tests):

```cpp
TEST(TestClientApiHandlers, TestPromptStartsATopFrameAndAnswersItsSid) {
  /* handlers_prompt(no sid, "goal text") → the response frame on the double:
     {req_id, status 0, sid "sessions/..."}; the frame EXISTS in the
     root's sessions subtree (the store scan proves it) */
}

TEST(TestClientApiHandlers, TestPromptSteerAppendsAUserMessage) {
  /* a created frame; handlers_prompt(sid, "the steer") → status 0; the
     frame's events carry the msg.append user record after the steer (the
     standing FIFO) */
}

TEST(TestClientApiHandlers, TestEventsReplaysTheCursorAndSignalsLive) {
  /* a frame with 3 events committed; subscribe from seq 1 → the double
     receives: the records for seq 2..3 (the 1-record's exclusion rule: from_seq
     = RESUME-AT semantics — records with seq > from_seq), then the live
     marker; then a NEW event commits (via a cell/remember) → its record
     arrives as a live tail frame */
}

TEST(TestClientApiHandlers, TestInterruptReachesTheFrame) {
  /* a pooled frame mid-cell (the test_loop pooled idiom); handlers_interrupt →
     the synthesis lands (the guards slice's shape over the wire); status 0 */
}

TEST(TestClientApiHandlers, TestUnknownSidAnswersError) {
  /* interrupt/prompt/events on a sid the registry doesn't know → ONE
     CA_ERROR frame with the req_id echo; the wire's closed vocabulary */
}
```

- [ ] **Step 2: The implementation** — `handlers.h` exposes the SERVER's session context:

```c
typedef struct ca_session_server_t ca_session_server_t;
/* cfg carries the borrowed wave_database_root_t + the server's scheduler
   pool (frames schedule onto it) + the model backend (borrowed or NULL) +
   the frame_config_t template (the server's model wiring). */
ca_session_server_t* ca_session_server_create(wave_database_root_t* root,
                                              scheduler_pool_t* pool,
                                              streams_loop_thread_t* loop);
void ca_session_server_destroy(ca_session_server_t* server);
/* The transport calls this with a DECODED payload: the type switch runs
   HERE (the wire layer never learns the types). Returns what it sends:
   0 = a response/error frame posted to the connection; nonzero = the
   connection is gone (the caller drops). */
int ca_session_handle(void* server, uint64_t type, void* payload,
                      void* connection);
```

  Internals:
  - **The registry**: a sid → `frame_t*` map (a small list; the dispatch-thread
    domain is THE TRANSPORT's dispatch — handlers run per-connection-event; MULTIPLE
    transports share one server → the registry needs its OWN lock or an owning actor.
    DECISION: `ca_session_server` is an ACTOR (actor-first struct): all handler calls
    marshal over `streams_loop_call` onto the loop thread; the registry + frame calls
    happen THERE (the dispatch-thread discipline holds); the connection actors receive
    response frames by mailbox post. This is the cleanest honest ownership — document it.)
  - **PROMPT**: sid NULL → `_frame_create + frame_start` (the frame owns a copy of the
    goal; the response's sid via `frame_sid`); else → the registry lookup + `frame_append_msg`
    (the FIFO — durable-input-before-model-work is a store fact).
  - **EVENTS**: op REPLAY_THEN_LIVE/LIVE_ONLY → compose a `FRM_STORE_SCAN` (absolute bounds
    from from_seq: start = `sessions/<sid>/events/<from_seq+1 zero-padded>` — the scan's
    ASCENDING reply already exists; the reply route: the HANDLER server's actor is the
    scan reply's target (reply_to = the ca_session_server's actor — a store message with
    an actor reply is the standing shape) → the reply's dispatch streams the records as
    CA_EVENTS_RESPONSE frames to the connection + composes the live marker + REGISTERS
    the watcher (`FRM_STORE_WATCH` with the connection-actor + sid prefix).
  - **INTERRUPT**: registry lookup → the PUBLIC frame_interrupt (already cross-thread
    safe) → response.
  - **SESSIONS**: `FRM_STORE_LIST_SESSIONS` → the reply streams the records.
  - **Unknown type / unknown sid / dead frames**: exactly one `CA_ERROR` (the wire's
    closed vocabulary).
- [ ] **Step 3: Green (suite +5), ASan (the loop thread + the registry's frame lifetimes — a frame DESTROYED by its engine while the registry holds it: the engine's teardown claims the frame; the registry must DROP the entry: subscribe the server's actor to nothing... the honest mechanism: the server's actor watches nothing; the FRAMES' done-status drives a small sweep at each handler dispatch (the registry's entries whose frame_is_done → kept (a client may still steer?? no — a done frame accepts no steer; the entry STAYS for the events channel replay... decide: the registry HOLDS the frame_t* (an owned runtime object); a done frame's runtime object lives until the server's teardown — the server is the frames' owner, not the engine's. VERIFY frame lifetime machinery (the deferred-destroy claim — the surface slice's ownership story) and PIN the registry's holding rule in a comment).** valgrind. Commit:

```bash
git add src/ClientApi/handlers.h src/ClientApi/handlers.c test/test_client_api_handlers.cpp test/CMakeLists.txt
git commit -m "feat: the client-api handlers — the actor-glue over frames and the store"
```

---

### Task 5: The UNIX transport (ported)

**Files:**
- Create: `src/ClientApi/Unix/unix_transport.{h,c}`, `src/ClientApi/Unix/unix_connection.{h,c}`
- Create: `test/test_client_api_unix.cpp`
- Modify: `test/CMakeLists.txt`

PORT INSTRUCTIONS (byte-except-includes, the streams-port slice's idiom — with the EXPLICIT adaptation list):
- From liboffs `src/ClientAPI/Unix/unix_transport.{h,c}` (503 lines): the transport struct (actor-first, the pd loop/thread, listen socket + watcher, connections vec, max_connections, the destroy-node list, socket_path) + create/destroy/start/stop + the accept machinery. ADAPT: strip the daemon-only params (block_cache/ofd_cache/tuple_cache/ssl/api_key_hash — an AF_UNIX socket needs NO key auth: the socket's own file permission is the auth — keep `socket_path` + `max_connections` only); `health_ctx`/`peer_node` gone.
- From `unix_connection.{h,c}` (1562 lines): the CONNECTION machinery only — the struct (refcounter, actor, sock, watcher, streams' framer), the read callback → framer → complete-frame extraction, `_send_frame`, `_send_error,` the watcher update/close, the destroy. ADAPT: REPLACE liboffs's dispatch body (its ~40 op handlers: put/get/load/rep/auth...) with OUR single line:

CONSEQUENCE for Task 2 (decided here, applies there): the framer extracts raw BYTES; the
connection loads the CBOR ONCE (`cbor_load`) when a complete frame appears, then passes the
LOADED item into the wire. So the wire exposes BOTH shapes:

```c
/* The bytes form: cbor_load + decode, the cbor item destroyed inside —
   the transports' convenience. */
int ca_wire_decode_bytes(const uint8_t* raw, size_t raw_len, uint64_t* type,
                         void** payload, uint64_t* req_id, uint8_t* status);
/* The item form: a PRE-LOADED item the caller still owns. */
int ca_wire_decode(cbor_item_t* frame, uint64_t* type, void** payload,
                   uint64_t* req_id, uint8_t* status);
void ca_wire_payload_destroy(uint64_t type, void* payload);
```

(Task 2's test bodies above use `ca_wire_decode` with raw bytes — rename those first-form
calls to `ca_wire_decode_bytes`; the pinned shapes are unchanged.)

And OUR dispatch replaces liboffs's ~40-op handler switch:

```c
static void _unix_dispatch_frame(unix_connection_t* conn, cbor_item_t* frame) {
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  if (ca_wire_decode(frame, &type, &payload, &req_id, &status) != 0) {
    req_id = 0;   /* an undecodable frame has no req_id to echo */
    _unix_connection_send_error(conn, req_id, 1, "malformed wire frame");
    return;
  }
  if (ca_session_handle(conn->server, type, payload, conn) != 0) {
    /* the handler posted its response frames on the success path; nonzero
       = the connection is gone (nothing more can be sent) */
    log_error("unix: the handler refused a frame on a dead connection");
  }
}
```
   and the AUTH: NONE on unix (skip `_handle_auth`).
- The `ca_session_server_t*` rides the transport (created by the embedder — the test/CLI — and BORROWED by the transport; the server's own actor + loop wiring: the server actor marshals on WHICH loop? Task 4's answer: the server's actor + a loop — for TESTS the loop is a `streams_loop_thread_t` shared with the TRANSPORTS: the design gives `ca_session_server_create(root, pool, loop_thread)` — the loop_thread BORROWED).
- [ ] **Step 1: Failing test** (test_client_api_unix.cpp): a temp-path socket; a transport + server on shared loop threads; a raw AF_UNIX CLIENT socket (a test double — NOT sa_client yet) sends a PROMPT frame; asserts the response frame on the socket + a real frame in a real store; an EVENTS subscription streams a record + the live marker; an unknown sid → CA_ERROR. (The test client sends via `stream_frame_encode(cbor_bytes)`.)
- [ ] **Step 2: Port + adapt; build; suite green (+1-3); valgrind + ASan (the transport's destroy + mid-subscription teardown paths are the leak race surface).**
- [ ] **Step 3: Commit**

```bash
git add src/ClientApi/Unix/ test/test_client_api_unix.cpp test/CMakeLists.txt
git commit -m "feat: the unix transport (ported from liboffs; the wire's first carrier)"
```

---

### Task 6: The TCP transport (ported)

**Files:**
- Create: `src/ClientApi/Tcp/tcp_transport.{h,c}`, `src/ClientApi/Tcp/tcp_connection.{h,c}`
- Modify: `test/test_client_api_transports_tcp.cpp` (NEW — or extend the unix suite file; ONE file for both transports keeps the gate simple: ADD to `test/test_client_api_unix.cpp` and RENAME it `test_client_api_transports.cpp`)

PORT: liboffs's `src/ClientAPI/TCP/{tcp_transport,tcp_connection}.{h,c}` (414 + 1827) with the SAME adaptation list as Task 5 PLUS: the api-key auth (TCP REQUIRES auth — the transport carries the BCRYPT hash via the streams server's `auth_middleware` machinery shape — the api_key_hash param comes back; an unauthenticated connection's FIRST frame prompts the auth exchange: the wire gains `CA_AUTH_REQUEST/RESPONSE` types (12/13 — the liboffs numbers match its AUTH_REQUEST!) with a challenge/answer shape; a connection that sends non-AUTH before authenticating gets CA_ERROR loud and closes). NOTE: check liboffs's ws/tcp auth exchange shape for the pattern; port it honestly.
- [ ] **Failing test**: an authenticated prompt round-trip over loopback TCP; an unauthenticated connection refused loud; the auth exchange's wrong-key refusal.
- [ ] **Commit**:

```bash
git add src/ClientApi/Tcp/ test/test_client_api_transports.cpp
git commit -m "feat: the tcp transport (ported from liboffs; api-key authed)"
```

---

### Task 7: `src/ClientLibs/c` — the typed C client

**Files:**
- Create: `src/ClientLibs/c/sa_client.h`, `src/ClientLibs/c/sa_client.c`
- Create: `test/test_sa_client.cpp`

PATTERN SOURCE: liboffs's `offs_client.{h,c}` (the callbacks + payload-lifetime + config idiom) — TRANSPOSED (a small client; ours is ~4 ops + the events mux, not a byte-port of 3,389 lines):

```c
/* The client config (offs_client_config_t's shape): transport + endpoints +
   the api key (tcp) + the timeouts/retries. */
typedef struct {
  sa_transport_e transport;
  const char* socket_path;
  const char* host;
  uint16_t port;
  const char* api_key;
  uint32_t connect_timeout_ms;
  uint32_t request_timeout_ms;   /* ONE in-flight request per connection */
} sa_client_config_t;

/* The payload-lifetime rule VERBATIM (renamed; see offs_client.h:64-77):
   callback payloads stay valid until sa_client_release_payload on each
   pointer exactly once, or until destroy; double-release = safe no-op. */
```
- The io model: like offs_client — its OWN threads per the real offs_client.c's shape (READ it: its poll/recv threads; transpose the same blocking-wait-for-request + the events read-loop shape; a client connection = ONE socket + a reader thread + the request/response round trip with a timeout).
- [ ] **Failing tests** (test_sa_client.cpp): prompt round-trip against an in-proc transport+server wiring (Task 5/6's test server); the events subscription's callback records; the release-payload discipline (double-release no-op); a dropped connection's error path; the request-timeout's error callback.
- [ ] **Commit**:

```bash
git add src/ClientLibs/c/ test/test_sa_client.cpp test/CMakeLists.txt
git commit -m "feat: the typed C client (transposed from offs_client; payload-lifetime discipline)"
```

---

### Task 8: The CLI's serve/client modes + docs + the final sweep

**Files:**
- Modify: `tools/frame-demo/main.c` (+ a `serve` mode: the runtime + transports + the server; a `client` mode: sa_client over a unix socket posting the goal text + printing streamed events' JSON; the existing direct mode untouched; note: the file's staled CTRL-C comment (lines 30-33: "frame_t exposes NO interrupt entry point") is now FALSE — frame_interrupt landed in the surface slice; update it: the direct mode gains a real SIGINT → frame_interrupt wiring)
- Modify: `docs/parity-feature-matrix.md` (row 44: the framed-wire half LANDED (the unix/tcp transports + the wire + handlers + the C client + the CLI serve mode; the HTTP/SSE surface remains the Pondr slice's); row 16: the DEV note updates — "the wire protocol re-entered: the client-api slice (2026-10-03), the wire is CBOR over dedicated transports, not the stdio JSON"; new row for store_notify/the client surface? one row suffices — add row 54: **The client API (wire + transports + handlers + typed client)**, L)
- The memory file: the controller's job — SKIP.

- [ ] **Step 1: Build the modes; verify manually**: run `frame-demo serve --socket-path /tmp/sa-demo.sock` on a temp store + `frame-demo client --socket-path /tmp/sa-demo.sock --goal "test"` → the client prints the response + the streamed events. (Manual, not ctest — the subprocess story is live-gate evidence, the refine slice's precedent.)
- [ ] **Step 2: The sweep**: full ON + ASan + OFF (cmake-build-off: the client-api tree — does the OFF build exclude ClientApi? DECIDE: OFF python/streams stays sans ClientApi/ClientLibs (they need the store + the loop); the CMake excludes per the streams gate's exact pattern — `src/(Streams|Buffer)` regex extended or a new `SA_ENABLE_CLIENT_API` option defaulting tied to WDB+streams? SIMPLEST: the ClientApi/ClientLibs/Network dirs compile under `SA_ENABLE_STREAMS`'s gate shape (they need the loop thread) — extend the existing streams-gate regex to include `src/(ClientApi|ClientLibs|Network)`. Run the OFF suites + verify.) Valgrind on the new suites. The no-locks grep (the transports' destroy pairs justify themselves).
- [ ] **Step 3: Commit**

```bash
git add tools/frame-demo/main.c docs/parity-feature-matrix.md
git commit -m "feat: the demo CLI's serve/client modes; docs: the client api lands — matrix row 44's wire half + row 16's un-defer"
```