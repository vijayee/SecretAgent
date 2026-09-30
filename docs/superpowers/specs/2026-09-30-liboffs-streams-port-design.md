# Liboffs Streams/HTTP Port — Design

**Date:** 2026-09-30
**Status:** agreed with owner in session (sections approved inline)
**Next slice queued behind this one:** frame orchestration (spawn → child loop → report → join as scheduled actor continuations); its Section-1 shape ("the frame is the actor, drives itself" + CPython as a frame-owned resource) was agreed 2026-09-30 and is parked, NOT redesigned here.

## Why this slice exists

The frame-tree slice shipped a deliberately minimal **synchronous** HTTP client
(`src/Net/http.c`: one POST, blocking recv, content-length + chunked decode,
connection per request). Two things changed since:

1. The orchestration slice needs the model call **async** — the turn loop must
   submit I/O and yield to the scheduler, never hold a worker for a model call
   (60–150 s measured against local Ollama). An async façade over the sync
   client was considered and rejected by the owner as throwaway scaffolding.
2. Downstream slices (run-agent-as-desktop) need an express-like **HTTP server**
   for REST APIs.

Liboffs already has both, production-hardened: the `poll-dancer` event loop
(epoll/kqueue/IOCP) and the express-like HTTP layer in `ClientAPI/HTTP`. The
owner's direction: port the whole stack in rather than build a second client.

## What moves

### Pinned dependencies (wavedb precedent)

- `deps/poll-dancer` — submodule pinned at the SHA liboffs records (`a05b9e1`
  at read time, the IOCP drain-timeout destroy-UAF fix; matches
  docs/backpressure-and-timer-stall.md's verified state). Built via
  ExternalProject with `-DCMAKE_POSITION_INDEPENDENT_CODE=ON`; all three
  platform backends compile (epoll, kqueue, iocp) — Windows is a real build,
  not aspirational.
- `deps/http-parser` — submodule pinned at the SHA liboffs records.

If a pin's URL is unreachable offline, copy from the local checkout instead and
record the deviation, exactly as the wavedb task allowed.

### Ported source (copies — liboffs/ stays frozen, never modified)

New module `src/Streams/`:

| File (from liboffs/src/ClientAPI/HTTP unless noted) | Role |
|---|---|
| `http_server.{h,c}` | bind/accept, route table, middleware chain (`http_server_use`) |
| `http_connection.{h,c}` | one client connection: parse → handler → response |
| `http_route.{h,c}` | route matching + dispatch |
| `http_request.{h,c}`, `http_response.{h,c}` | message objects |
| `http_headers.{h,c}` | header set access |
| `cors.{h,c}` | the one generic middleware |
| `auth_middleware.{h,c}` | Bearer-vs-bcrypt-hash auth with loopback opt-out (owner: port — downstream REST wants it) |
| `../Util port: bcrypt.{h,c}` | 111-line dep of auth_middleware; randomness from our `platform_random_bytes` (same family primitive) |
| `http_client.{h,c}` | **NEW code** — async request client on poll-dancer watchers |
| `loop_thread.{h,c}` | **NEW code** — the loop's own thread + submit/completion glue |

### The client contract (what replaces `http_post_json`)

Same signature vocabulary as today's frozen API, split in two:

```c
/* Submit: returns immediately. Completion is delivered via the callback on
   the loop thread (see loop_thread for the mailbox glue). */
int http_client_submit(const char* url, const char* api_key,
                       const char* body_json, uint32_t timeout_ms,
                       http_client_completion_fn on_done, void* ctx);

typedef void (*http_client_completion_fn)(void* ctx, int status,
                                          char* body, size_t body_len,
                                          char* error /* heap, NULL on success,
                                                         caller frees */);
```

- Non-blocking connect/send/recv on poll-dancer watchers; the timeout is a
  loop timer, not a socket timeout.
- `http_client_submit` takes no locks it can't drop in µs, never blocks; the
  completion callback must also stay µs-scale (post a message, return).
- The completion callback posting into the OWNING frame actor's mailbox is
  loop_thread's job and the slice's key glue — corr-matched message like every
  other frame message (FRM_MODEL_RESULT family naming decided in the plan).

### What is retired

- `src/Net/http.{h,c}` — deleted outright (owner decision; one transport story
  per runtime).
- `test/test_http.cpp` rewritten against `http_client_submit`: the framing
  hardening survives test-for-test — chunked decode (now via http-parser's
  parser), absurd-size claims, repeated Transfer-Encoding lines, empty-body
  non-NULL rule, body cap — all re-pinned as async tests.
- `model.c` consumes the new contract; the live gate (`SA_TEST_OLLAMA_URL`)
  is the transport's live proof.

### Deliberately left behind

- All `*_routes.c` / off_routes (offs-identity + BlockCache-coupled — the
  desktop slice re-derives routes from the ported core).
- `src/Network` (QUIC/msquic, peer_book, gossip, relay) — nothing REST needs.
- Everything else in `ClientAPI/` beyond the HTTP dir listed above.

## Concurrency model

- **One poll-dancer loop thread per process** — the runtime's IO reactor,
  separate from the scheduler that runs actors.
- Client submit from an actor worker; the worker yields immediately. The loop
  dispatches fd completions and loop_thread posts the completion message into
  the owner actor's mailbox. Shared state between the loop thread and actor
  world = the mailbox mutex only — same discipline as the pyrt handoff.
- Server connections live on the loop thread; handlers run inline (µs-scale
  routing/assembly). WaveDB stays on the actor side. Server handlers are
  pure-drain workers → the mute-park doctrine (docs/backpressure-and-timer-stall.md)
  applies unchanged.
- Loop thread death is process-loud: fail loud, never limp (house style).

## Failure semantics

- Client fd/timeout failures: the completion carries `status=-1` + a heap
  reason string — no new error vocabulary for callers.
- Server: parse failures answer 400 from the connection layer; handlers return
  statuses that map to responses (C has no exceptions to launder).
- Batch/body caps: the Task-8-era `_HTTP_BODY_MAX`/`_HTTP_READ_MAX` discipline
  carries over to the async client (fail loud, never silent truncation).

## Alias machinery

The ported code is family code (get_memory, log_*, platform primitives). The
root CMake's objcopy alias step extends to the new archives: probe which
symbols each pinned library actually defines that collide with
`libsecretagent.a`, and redefine with a per-archive prefix (e.g. `pdx_` for
poll-dancer if needed). Same negative-control proof as the wavedb alias task:
link a test binary WITHOUT the aliases and show the failure, then with them.

## Evidence bar (slice completion)

1. **Live model turn through the ported async client** — the opt-in
   integration gate (`TestLiveLoop`) passes against local Ollama with the loop
   submitting the completion and yielding.
2. **Server loopback test** — ported core binds, one route matches, request
   parses, response writes; cors middleware demonstrably engages.
3. **Framing hardening survives the port** — chunked/malformed/size-cap/
   repeated-TE/empty-body tests all green on the async client (the fake-server
   harness ports).
4. **Windows build** compiles the full port and its tests via the IOCP backend.
5. ASan + valgrind clean on the new module's suites (the
   `--strip-debug`-copy discipline; WaveDB TLS suppressions unchanged), all
   three build configs green (ON / ASan / OFF — OFF stays sans streams, behind
   a gate symmetrical to `SA_ENABLE_WDB`).

## Explicit non-goals in this slice

- The frame orchestration slice itself (parked, Section-1 approved).
- REST route/biz content — routes are the desktop slice's work.
- QUIC/relay/peer networking; msquic.
- Incremental derive caching (documented known cost; separate upgrade).

## Open items for the implementation plan to settle

- Whether `http_server`'s threaded/accept model from liboffs needs adaptation
  to one-loop-thread (expected: liboffs server already runs on the loop).
- Exact `FRM_*` message name for the completion (plan decision).
- Whether poll-dancer/http-parser prefer their own CMake targets consumed as
  ExternalProjects or a combined build (mirror wavedb's choice, adapt if their
  option names differ).