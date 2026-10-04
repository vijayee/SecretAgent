//
// Created by victor on 10/04/26.
//

#ifndef SA_CLIENT_H
#define SA_CLIENT_H

/* The typed C client for the agent surface's client API (the client-api spec
 * §2): four blocking ops + the events mux, carried by the ClientApi wire over
 * the Network framer and the client's OWN socket io. liboffs's
 * offs_client.{h,c} is the transposed IDIOM here — the config shape, the
 * payload-lifetime rule (that header's :58-67 doc block, carried verbatim
 * below renamed), the own-io reader thread, and the bounded request wait —
 * NOT a byte-port of its ~30-op plumbing.
 * MODULE BOUNDARY: this module links the ClientApi wire + the Network framer
 * + Platform + its own socket io only — it NEVER includes src/Frame. A
 * server for it lives in src/ClientApi (the transports + the handlers).
 * The events channel's json_text rides the wire as bytes; the client never
 * re-parses a store record. */

/* The client rides BOTH gates: the wire's libcbor arrives through the wavedb
 * gate, and the client targets the transports' server stack (the loop
 * thread's machinery). A build without either has nothing to talk to — the
 * module compiles to nothing there (handlers.h's empty-TU anchor idiom),
 * so no CMake regex is needed for the OFF build. */
#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SA_CLIENT_TRANSPORT_UNIX = 0,
  SA_CLIENT_TRANSPORT_TCP = 1,
} sa_transport_e;

/* Statuses handed to callbacks: 0 = OK; the wire's daemon statuses ride
 * through unchanged (1 = refused, 2 = the frame/session unknown — see
 * client_api_wire.h); the client's OWN failure modes fill the 10+ band. */
#define SA_CLIENT_STATUS_OK            0
#define SA_CLIENT_STATUS_TIMEOUT       10
#define SA_CLIENT_STATUS_DISCONNECTED  11
#define SA_CLIENT_STATUS_ALLOC         12
#define SA_CLIENT_STATUS_BUSY          13
#define SA_CLIENT_STATUS_LOCAL         14   /* the request never left: an
                                             encode or send failed before the
                                             wire saw it (the channel's fate
                                             is unknown — the reader sorts
                                             the drop out) */
#define SA_CLIENT_STATUS_REENTRANT     15   /* a blocking op called FROM an
                                             events callback (the reader
                                             cannot route its response):
                                             refused outright — the op
                                             returns -1 and no callback
                                             fires; this status names the
                                             refusal */

/* Callback types.
 *
 * Payload ownership for every callback below: pointer arguments handed to a
 * callback (sid strings, event-record JSON text, the sessions rows array and
 * its strings, error texts) remain VALID until the consumer calls
 * sa_client_release_payload on each pointer exactly once (after copying
 * whatever it wants to keep), or until sa_client_destroy tears the client
 * down. The library does not free the payload when the callback returns, so
 * the pointer stays valid after the callback returns — cross-thread
 * consumers may copy it later. Releasing a payload twice, releasing an
 * unknown pointer, or releasing after destroy is a safe no-op. Unreleased
 * payloads are reclaimed at destroy. The client delivers ONLY pointers it
 * tracks: a payload the client could not hold (out of memory) never reaches a
 * callback — the failure routes through the error channel instead, and the
 * OOM path's texts may be static literals (a release on an untracked pointer
 * is the safe no-op above). */

/* The prompt response. status 0 = the frame started (sid = the new frame's
 * "sessions/..." path) or the steer queued (sid = ""). sid is held; NULL
 * sid = the request FAILED (see the request ops' contract below) — nothing
 * to release then. */
typedef void (*sa_client_prompt_cb_t)(void* ctx, uint8_t status,
                                      const char* sid);

/* The interrupt response: status 0 = the interrupt posted into the frame's
 * mailbox. No payload. */
typedef void (*sa_client_interrupt_cb_t)(void* ctx, uint8_t status);

/* One sessions listing row — the wire's [sid, status, goal, created, depth]
 * shape. status is "running"/"done"/... or NULL when the meta read came up
 * empty; goal is the session's goal text or NULL. EVERY pointer in the rows
 * handed to the callback is held separately (the rows array itself, plus
 * each row's non-NULL string — NULL fields need no release). */
typedef struct {
  const char* sid;
  const char* status;   /* NULL = unknown */
  const char* goal;     /* NULL = absent */
  uint64_t created;     /* meta/created's value, 0 when absent */
  size_t depth;
} sa_client_session_row_t;

typedef void (*sa_client_sessions_cb_t)(void* ctx, uint8_t status,
                                        const sa_client_session_row_t* rows,
                                        size_t nrows);
/* rows is NULL when the request FAILED. */

/* ONE delivered event: a record (seq > 0: record_json = the store record's
 * JSON VERBATIM — the client never re-parses it) or a MARKER (seq == 0 +
 * record_json NULL: op echoes WHICH transition — the subscription's replay
 * closed and live tailing began, or CA_EVENTS_UNSUBSCRIBE's terminal ack
 * after sa_client_unsubscribe_events). sid and record_json are HELD (two
 * releases per record; the marker's record_json is NULL — one release).
 * Fires on the reader thread; a reconnect may deliver a NEW live marker and
 * replay the gap the outage swallowed (see the reconnect note below). A
 * BLOCKING OP called from an events callback is refused immediately
 * (SA_CLIENT_STATUS_REENTRANT — the reader cannot route its response). */
typedef void (*sa_client_events_cb_t)(void* ctx, const char* sid,
                                      uint64_t seq, uint8_t op,
                                      const char* record_json);

/* The failure channel (registered on the config): EVERY request failure
 * fires here — a daemon refusal (CA_ERROR: the daemon's status + its text),
 * a timeout, a lost connection, a local refusal — with the request's req_id
 * (0 when the failure predated a request). text is held; NULL text never
 * happens. The request op's own callback ALSO completes with the failing
 * status (NULL payload) — the consumer picks either channel, neither
 * hangs. */
typedef void (*sa_client_error_cb_t)(void* ctx, uint64_t req_id,
                                     uint8_t status, const char* text);

/* The client config (offs_client_config_t's shape): the transport + endpoint
 * + the api key (tcp) + the timeouts/retries. Strings are copied at connect
 * (the api key copy is scrubbed by length at destroy). */
typedef struct {
  sa_transport_e transport;
  const char* socket_path;      /* UNIX: the served socket file */
  const char* host;             /* TCP */
  uint16_t port;                /* TCP */
  /* TCP REQUIRES the api key: the auth exchange is the connection's FIRST
   * frame pair (a NULL/empty key refuses sa_client_connect). UNIX ignores
   * it — the socket file's own permission is the auth. */
  const char* api_key;
  /* Bounds the TCP auth exchange (and each reconnect attempt's exchange) —
   * the raw OS connect itself is a blocking platform call outside it. Both
   * loopback transports connect in microseconds; the exchange is where a
   * dead daemon shows. */
  uint32_t connect_timeout_ms;
  /* ONE in-flight request per connection: the request ops block their
   * caller this long, then fail through the error callback + a failing op
   * completion. */
  uint32_t request_timeout_ms;
  /* The events channel's reconnect budget: the reader thread retries on a
   * 1 s → 8 s (doubling) backoff; max_retries bounds the attempts, 0 = retry
   * FOREVER (the daemon restarts are exactly what a session watcher wants to
   * ride out). When the budget exhausts the error callback fires and the
   * subscription ends. */
  uint32_t max_retries;
  sa_client_error_cb_t error_cb;   /* may be NULL (failures then complete
                                      only the op callbacks) */
  void* error_ctx;
} sa_client_config_t;

sa_client_config_t sa_client_config_default(void);

typedef struct sa_client_t sa_client_t;

/* THE IO MODEL (offs_client's shape, minimal): ONE socket + the client's OWN
 * reader thread. sa_client_connect opens the channel (unix socket / tcp +
 * the api-key auth exchange FIRST), then starts the reader; the request ops
 * encode (ca_wire_encode + stream_frame_encode), send, and block on a
 * condvar bounded by request_timeout_ms while the reader routes the frames —
 * a request's response to its slot, the events subscription's stream as an
 * INDEPENDENT callback flow muxed on the same socket. The request callbacks
 * fire on the CALLER thread (the bounded wait hands the decoded response
 * over); the events callback fires on the reader thread.
 *
 * THE RECONNECT: when the channel drops, any in-flight request fails
 * immediately (op completion + error callback, SA_CLIENT_STATUS_DISCONNECTED)
 * and — while a subscription is active — the reader thread reconnects on the
 * backoff (re-authenticating for tcp) and RE-SUBSCRIBES from the last
 * delivered seq: the outage's gap replays, then a NEW live marker, then live
 * tailing. Requests issued while disconnected fail fast
 * (SA_CLIENT_STATUS_DISCONNECTED); a request alone never re-opens the
 * channel. No subscription active = the reader exits; the client stays
 * disconnected until destroyed.
 *
 * THE RE-ENTRY: the events callback runs on the reader thread — a blocking
 * op from one is refused immediately (the op returns -1, no callback; the
 * reader cannot route its response, so the request would stall to a lying
 * TIMEOUT over a daemon that actually committed).
 *
 * THE IDLE COST: a connected client with an ACTIVE SUBSCRIPTION polls at the
 * 2 ms read cadence even with no traffic — an idle subscribed client costs
 * ~500 wakeups/s (the honest price of the simple reader; liboffs's pd timers
 * would remove it — a recorded candidate). */

sa_client_t* sa_client_connect(const sa_client_config_t* config);

/* Final teardown (offs_client's disconnect/destroy pair COLLAPSED — the
 * client has no detach-and-linger consumer in this slice): destroy JOINS the
 * reader thread and reclaims the payloads; a blocking op ON ANOTHER THREAD
 * at destroy time is UNDEFINED BEHAVIOR (the offs_client contract, carried
 * over) — disconnect your callers first. Stops the reader, closes the
 * channel, reclaims every unreleased payload, scrubs the api-key copy by
 * length, frees the client. Callbacks already delivered (payloads not yet
 * released) are freed HERE — pointers handed to callbacks are INVALID after
 * destroy returns. */
void sa_client_destroy(sa_client_t* client);

/* Releases a payload previously passed to a callback. The consumer calls
 * this for every payload pointer it received once it has copied the data.
 * An unknown or already-released pointer is a no-op; a payload released
 * twice, or after destroy, is also a no-op. */
void sa_client_release_payload(sa_client_t* client, void* payload);

/* Send the goal text (sid NULL: create + start a top frame; the response's
 * sid names it) or the steer (sid set). BLOCKS until the response, the
 * timeout, or the drop; returns 0 when the prompt callback ran (success or
 * failure-delivery), -1 when the call was refused outright (NULL args; a
 * call FROM an events callback — refused immediately, NO callback fires at
 * all; the error callback still fired for the local refusals —
 * busy/not-connected — but no prompt callback ran). */
int sa_client_prompt(sa_client_t* client, const char* sid, const char* text,
                     sa_client_prompt_cb_t callback, void* ctx);

/* Interrupt the session's open turn. Same blocking + return contract
 * (including the events-callback re-entry refusal: -1, no callback). */
int sa_client_interrupt(sa_client_t* client, const char* sid,
                        sa_client_interrupt_cb_t callback, void* ctx);

/* List the store's sessions. Same blocking + return contract (including the
 * events-callback re-entry refusal: -1, no callback). */
int sa_client_list_sessions(sa_client_t* client,
                            sa_client_sessions_cb_t callback, void* ctx);

/* Subscribe the events channel for ONE sid (one active subscription per
 * client): REPLAY_THEN_LIVE from seq 0 — the whole log replays to the events
 * callback, then the live marker arrives, then live tailing. BLOCKS until
 * the live marker (or the refusal/timeout); returns 0 when the
 * subscription is live or the failure was delivered through callbacks, -1
 * when refused outright (NULL args, a subscription already active, not
 * connected, a call FROM an events callback — refused immediately, no
 * callback — the error callback fired for the rest). */
int sa_client_subscribe_events(sa_client_t* client, const char* sid,
                               sa_client_events_cb_t callback, void* ctx);

/* End the active subscription: sends the unsubscribe request and blocks
 * until its terminal marker was delivered to the events callback (op =
 * CA_EVENTS_UNSUBSCRIBE) — or the timeout fires (the sub stays active then;
 * -1 + the error callback). Returns 0 when delivered, -1 refused outright
 * (no active subscription / not connected / a call FROM an events callback
 * — refused immediately, no callback). */
int sa_client_unsubscribe_events(sa_client_t* client);

#ifdef __cplusplus
}
#endif

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */

#endif /* SA_CLIENT_H */