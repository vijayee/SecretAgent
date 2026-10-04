//
// Created by victor on 10/04/26.
//

#ifndef SA_CLIENT_API_HANDLERS_H
#define SA_CLIENT_API_HANDLERS_H

#include <stddef.h>
#include <stdint.h>

/* The handlers ride BOTH gates: the frame/store surface (SA_HAS_WDB — the
   frames, the store actor's posts, the wire's libcbor) and the loop thread
   (SA_HAS_STREAMS — streams_loop_call's marshal). A build without either
   has nothing for handlers to glue together (the OFF build excludes the
   ClientApi tree the same way it excludes Streams). */
#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include "../Actor/actor.h"
#include "../Frame/frame.h"
#include "../Frame/model.h"
#include "../Scheduler/scheduler.h"
#include "../Streams/loop_thread.h"

/* The handlers = the actor-glue over the frames and the store (client-api
   spec §3): transport-blind, one entry point fed by every transport's
   dispatch. Each request arrives DECODED (the wire owns the CBOR); the
   type switch runs here; the responses ride back through the connection
   interface below. */

/* ---- the connection interface (the transports implement it; the wire
   layer never learns a concrete connection type) -------------------------- */

/* send(): ENCODE (ca_wire_encode) + framer-frame + QUEUE the write (the
   transport owns its write machinery).
   - Returns 0 = queued; nonzero = this connection can take no more (it is
     tearing down or its write queue rejected back-pressure).
   - PAYLOAD OWNERSHIP: on 0 the send CONSUMES the payload (it encodes
     immediately on the caller's stack, then must destroy it); on nonzero
     the payload stays the SENDER's (the handlers destroy it via
     ca_wire_payload_destroy).
   - THREADED: called ON THE LOOP THREAD ONLY. The handlers marshal every
     send there (the store's replies and notices bounce back onto the loop
     thread through streams_loop_call closures), so the transport's send
     machinery stays single-threaded — the same discipline the frame layer
     keeps for its dispatches. */
typedef int (*ca_send_fn)(void* conn, uint64_t type, void* payload);

typedef struct ca_session_conn_t {
  void* conn;                  /* the transport's own connection object */
  ca_send_fn send;             /* required */
  void (*retain)(void* conn);  /* the transport connection's refcounter
                                  pair: the server holds ONE ref for every
                                  internal entry (a subscription, a pending
                                  store round trip, an in-flight closure)
                                  that still names the connection, so a
                                  tearing-down transport connection stays
                                  alive until the last handler touch. NULL
                                  pair = a caller-owned connection that
                                  must outlive the server — tests' doubles
                                  only; production transports carry
                                  refcounters. */
  void (*release)(void* conn);
} ca_session_conn_t;

/* ---- the session server -------------------------------------------------- */

/* The server is an ACTOR (actor_t first member) whose embedded actor is the
   store's replies' and notices' bounce point. THREADED MODEL (the whole
   module's contract):
   - THE LOOP THREAD single-writes ALL server state: the registry, the
     subscriptions, the pending store round trips. Every entry — the
     handler calls via ca_session_handle (from any thread; the transports'
     dispatches usually ARE on the loop thread — streams_loop_call is an
     async ENQUEUE, so a same-thread call simply defers to the next tick,
     never an inline nested run, never a deadlock) and every store
     reply/notice (the store actor posts at the embedded actor; its dispatch
     runs on the server's scheduler pool and re-marshals back onto the loop
     thread) — reaches the state as a loop-thread closure.
   - The SERVER OWNS its frames: a registry frame's record lives until
     ca_session_server_destroy (including past frame_is_done — the events
     channel must keep replaying a done session's log; a done frame accepts
     no STEER or INTERRUPT, refused loud at the handler).
   - The teardown duty: the embedder closes its connections BEFORE
     ca_session_server_destroy (each teardown calls ca_session_conn_closed);
     destroy drains the in-flight closures and store round trips bounded
     before freeing. The recorded in-flight seam the frame layer also
     carries (a store batch queued but never dispatched at destroy time) is
     the embedder's teardown order: destroy the server while the store
     actor is reachable, then wave_db_close. */
typedef struct ca_session_server_t ca_session_server_t;

/* All borrowed: the root (frames open subtrees on it), the pool (the
   server's actor + every api-created frame attach — a POOLED frame requires
   a POOLED store: open the root with wave_db_open_config on the SAME pool),
   the loop (the transports + the server share ONE loop thread).
   frame_cfg = the created frames' template, copied by value; its TEXT
   members are borrowed from the caller (frame_create dups its own per
   frame), and its `pool` member is OVERWRITTEN with the server's pool
   (frames the API creates ride the API's pool, whatever the template said).
   shared_backend = BORROWED, may be NULL: when set, EVERY api-created frame
   is injected with it BEFORE frame_start (deterministic injection — a test
   or embedder that needs one engine wiring across sessions; NULL keeps the
   frames' own config-built default backends). It must tolerate concurrent
   frames' calls (the http backends do; a stateful test double does not). */
ca_session_server_t* ca_session_server_create(wave_database_root_t* root,
                                              scheduler_pool_t* pool,
                                              streams_loop_thread_t* loop,
                                              const frame_config_t* frame_cfg,
                                              model_backend_t* shared_backend);
void ca_session_server_destroy(ca_session_server_t* server);

/* The transport's dispatch calls this with a DECODED payload (the wire owns
   the CBOR; the type switch's cast is this call's). ALWAYS MARSHALS: the
   work (registry update, frame create/steer, store posts, response sends)
   runs as a loop-thread closure. Returns 0 = ACCEPTED (queued — the
   response frames post later; one call may send many: the events scan's
   records + the live marker), nonzero = refused (dying, or the loop is
   gone) — the payload destroyed HERE; the caller drops the frame. */
int ca_session_handle(ca_session_server_t* server, uint64_t type, void* payload,
                      ca_session_conn_t* connection);

/* The connection's teardown hook (the transports call it when a connection
   closes — from ANY thread; it marshals like handle): drops the
   connection's event subscriptions (unwatching a sid whose LAST subscriber
   left) and its pending store round trips (their late replies drop loud).
   The connection object stays alive for the sends still in flight (its
   refcounter; the server's held refs); every send on a torn-down
   connection refuses, and the handlers drop those loudly. */
void ca_session_conn_closed(ca_session_server_t* server,
                            ca_session_conn_t* connection);

/* Test/debug accessor (the frame layer's debug-accessor shape): the
   registry frame for a session path — BORROWED (the server owns the
   record) — NULL when the sid is unknown. */
frame_t* ca_session_server_frame(ca_session_server_t* server, const char* sid);

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */

#endif /* SA_CLIENT_API_HANDLERS_H */