# The Client API — Wire + Transports + Handlers + C Client — Design

**Date:** 2026-10-03
**Status:** owner-agreed shape settled in session (the ClientAPI pattern mirrored from liboffs at
liboffs's abstraction level, with ported transports; six decisions tabled below; approach A-with-
ported-transports). No open questions — every detail is pinned.
**Atlas slice:** a new node (delivery; L2/L4 + the surface row 44's first half) — evidence:
`test/test_client_api_*.cpp` (new) + `test/test_sa_client.cpp` (new) + the CLI's serve/client modes.
**Parity sources:** `docs/architecture-research.html`'s opencode section (the client half's
verdict; its client-ingest discipline + the refetch sin we avoid by construction) + `liboffs`'s
`src/ClientAPI` (THE pattern: one wire, a transport seam, per-domain handlers, ClientLibs) —
Victor's own library's client-architecture, mirrored at its level of abstraction. Matrix rows:
44 (REST routes/desktop integration — P; this slice lands the framed-wire half; an HTTP surface
is NOT in this slice), 16 (the framed typed-message bridge — DEV said "re-enters only with a
server-based embedder": this slice IS that embedder), 15 (admission — stays a later slice; the
wire's prompt carries the standing FIFO), 2 (pending-input durability — restart/reconcile later).
The Pondr UI is NOT in this slice: Pondr becomes a FLUTTER desktop app (the Figma-Make React
source as its 1:1 mockup base) — the next workstream, consuming this wire through a
`ClientLibs/dart` binding (a raw-socket consumer needs no HTTP hinge).

## Why this slice exists

The runtime is a library with seven verbs and no consumers beyond the demo CLI driving frames
directly in-process. The matrix has waited on three un-landable pieces whose prerequisite is a
wire: the desktop/REST integration (row 44), the steering/admission slice (row 4's second input
source), and the framed-bridge DEV row 16 (its defer was explicit: "a wire protocol re-enters
only with a server-based embedder"). opencode's section settled the SHAPE (a thin pure event
consumer over a live stream; replay from the store by cursor, never the refetch dance) and
liboffs already owns THE PATTERN — the client architecture question answers itself by mirroring
the layering the runtime inherits from: one wire protocol, a transport seam, per-domain
handlers, and typed client libraries.

## Decisions carried in (settled with the owner, 2026-10-03)

| Decision | Value | Authority |
|---|---|---|
| Client architecture | the ClientAPI pattern (wire → transports → handlers → ClientLibs), at liboffs's abstraction level | owner |
| Wire format | **CBOR over DEDICATED transports — HTTP does not carry the wire** (HTTP keeps the streams server's existing role; a JSON/SSE HTTP surface is the Pondr/UI slice's business WITH its browser consumer) | owner |
| Transports | BOTH unix-domain AND TCP in this slice (ported transport dirs, byte-except-includes; the stream_framer ported from liboffs Network) | owner |
| Wire verbs | the MINIMAL FOUR pairs: PROMPT / EVENTS / INTERRUPT / SESSIONS (+ CA_ERROR) | owner |
| First consumer | `src/ClientLibs/c` (the offs_client pattern: callbacks, payload-lifetime rule, config struct) + the demo CLI's serve/client modes on it | owner |
| Admission | the standing FIFO (`frame_append_msg`) over the wire; row 15's machine stays a later slice | owner |
| WebSockets | NO — recorded follow-on tied to a browser consumer | owner |
| Out of scope | TLS config (api-key authed TCP only); the admission machine (row 15); the durable session registry (restart/reconcile); ClientLibs/js + the HTTP/SSE surface (the Pondr Flutter slice); spawn/report wire verbs; the durable-cancel-fence question (slice 4) | owner |

## 1. The wire

`src/ClientApi/client_api_wire.{h,c}` — CBOR arrays, message type first element; RESPONSES are
the adjacent type (REQUEST+1), the compile-time pairing asserts carried verbatim
(`CLIENT_API_STATIC_ASSERT` shape). Every request carries a req_id the response echoes
(id-paired — row 16's closed vocabulary, id-pairing, bounded frames, fail-loud).

```c
#define CA_PROMPT_REQUEST     1   /* {req_id, sid?, text} — sid absent = create+start a top frame */
#define CA_PROMPT_RESPONSE    2
#define CA_EVENTS_REQUEST     3   /* {req_id, sid, op, from_seq} — op: 0 = replay then
                                     live tail; 1 = live tail only; 2 = unsubscribe */
#define CA_EVENTS_RESPONSE    4   /* repeated {req_id, sid, seq, record_json} frames;
                                     the transition to live tailing is SIGNALED by a
                                     final frame with seq = from_seq's sentinel value
                                     (seq 0 with the op echoed) — after it, records
                                     arrive as they commit */
#define CA_INTERRUPT_REQUEST  5   /* {req_id, sid} */
#define CA_INTERRUPT_RESPONSE 6
#define CA_SESSIONS_REQUEST   7   /* {req_id} */
#define CA_SESSIONS_RESPONSE  8
#define CA_ERROR              11  /* {req_id, status, text} — any pairing's failure */
```

(The final vocabulary enumerates EXACTLY the shapes above; unknown types, oversize strings,
and malformed encodes all answer CA_ERROR — the wire never trusts its peer.) Bounded-string
discipline in every decoder (the
`_decode_string` shape from liboffs's wire.c: absent ("" sentinel) vs empty, max lengths).
libcbor is the encode/decode dep — already in the ecosystem via WaveDB.

## 2. The framing + the two transports

`src/Network/stream_framer.{h,c}` PORTED from liboffs (the streams-port slice's proven
byte-except-includes idiom): `stream_frame_encode` `[4-byte big-endian length][data]`,
the accumulator (`feed`/`next`), `STREAM_FRAMER_MAX_FRAME_SIZE` 2 MB — the slow-drip
defense included. Two transport dirs, ported from liboffs's own dirs:

- `src/ClientApi/Unix/` — AF_UNIX accept + pipe shapes (liboffs's unix_transport/unix_connection).
- `src/ClientApi/Tcp/` — TCP with the api_key_hash auth (the streams server's proven
  machinery); TLS config NOT here.

Each transport: actor-first struct on its pd loop thread, the listen socket + watchers, the
connections vector, bounded connections, the destroy-node discipline — liboffs's shapes.
A complete frame → `session_handle(...)` — the WIRE LAYER CANNOT TELL which transport a
frame arrived on.

## 3. The handlers (the actor-glue)

`src/ClientApi/handlers.{h,c}` — transport-blind; both transports feed one entry point.

- **PROMPT**: no sid → create a top frame (goal = text) on the server's scheduler pool + start;
  sid → `frame_append_msg(frame, "user", text)` (the standing FIFO — the client's first real
  steering source). Response `{req_id, status, sid}`. The server keeps a sid → live `frame_t*`
  map (in-memory, the recorded restart/reconcile limit).
- **EVENTS**: `{req_id, sid, from_seq}` → the bounded store scan (absolute root-level bounds,
  the keys-listing discipline) streamed as one framed CBOR record per event, then the LIVE
  tail: the store actor gains `store_notify` — subscribers registered per the store actor's
  dispatch (its single writer), and every committed event batch pokes the list; a frame
  arriving with no subscription = the standing loud drop. Subscribe/unsubscribe are the two
  sub-shapes of the events request. A dropped connection unsubscribes (the connection's
  teardown).
- **INTERRUPT**: `{req_id, sid}` → `frame_interrupt(frame)` (cross-thread mailbox post — the
  pyrt-worker-established safe operation). Blocking frame APIs are NOT exposed — handlers
  stay µs-scale; only posted verbs cross.
- **SESSIONS**: bounded scan of the root's `sessions/` subtrees'
  `meta/{created,status,depth,goal}` → one response.

## 4. The C client + the CLI

`src/ClientLibs/c/sa_client.{h,c}` — the offs_client pattern transposed:

- `sa_client_config_t {transport (unix|tcp), socket_path, host, port, api_key,
  connect_timeout_ms, max_retries, retry_base_delay_ms}`; ONE in-flight request per
  connection (the CLI is sequential); the events channel muxes on the same connection.
- Callbacks on liboffs's payload-lifetime rule VERBATIM (renamed): pointers stay valid until
  `sa_client_release_payload` or disconnect; double-release/unreleased-at-destroy are safe
  no-ops/reclaimed.
- Surface: `sa_client_prompt / interrupt / list_sessions / subscribe_events / unsubscribe`
  (+ disconnect/destroy). Events arrive as pre-decoded records `{sid, seq,
  json_text}` — the client NEVER re-parses the record (consumers speak JSON natively);
  reconnect = a client-level bounded backoff; a dropped connection rejects pending requests
  via error callbacks (every pending request has a reject path).

`tools/frame-demo` gains two modes: **serve** (the runtime + the transports — the daemon) and
the CLIENT mode speaking `sa_client_*` over a local unix socket; the existing direct mode is
unchanged (266 tests keep their shape). In-repo integration tests wire the transports + client
in-process (loop threads + sockets on temp paths); the real CLI subprocess story stays
live-gate/manual evidence like the refine slice's.

## 5. Testing

| Suite | Adds |
|---|---|
| `test_client_api_wire.cpp` (NEW, plain gate) | every pair's encode→decode round-trip; the pairing asserts; bounded-string refusals; unknown/malformed → CA_ERROR; the record JSON hinge |
| `test_client_api_transports.cpp` (NEW) | both transports in-proc (unix temp paths, the tcp loopback); the framer's cap defense; the tcp unauthenticated refusal shape; bounded connections |
| `test_client_api_handlers.cpp` (NEW) | PROMPT start/steer on a real frame tree (the steering semantics over the wire); EVENTS cursor replay + live-tail via store_notify; INTERRUPT reaching a running pooled frame; SESSIONS listing a real tree |
| `test_sa_client.cpp` (NEW) | the callback/payload-lifetime contract; reconnect error paths; the events mux |
| existing suites | untouched: the CLI's direct mode keeps all 266 tests; the streams suite keeps its server core |

Verification bars as always: `setarch -R` on every suite run; valgrind on `strip --strip-debug`
copies (symtab kept); ASan for the loop-thread/transport paths; the no-locks grep (the
transports' own mutex pairs join the documented exception set — liboffs's transport destroy
disciplines carry their locks honestly; each gets a justification comment).

## 6. Out of scope / recorded follow-ons (deliberate)

- **The Pondr Flutter app** — the NEXT workstream (owner's direction): the Figma-Make React
  source (Pondr.zip in the repo root) converted to Flutter, 1:1 with the mockup's
  functionality (chat, sessions, settings/providers, the subconscious view, login/register),
  mock-driven first, consuming this wire via a Dart client; new repo, the one-way dependency
  arrow (pondr → agent-runtime) intact.
- The HTTP/JSON+SSE surface (browser clients; opencode's ingest discipline: 16 ms coalescing,
  cursor replay, heartbeat) — with its consumer.
- TLS config; the admission machine (row 15); the durable session registry; WS; the
  spawn/report wire verbs; ClientLibs/js.