//
// Created by victor on 10/04/26.
//

/* The typed C client's body (the client-api spec §2 / plan Task 7): the
 * offs_client io idiom transposed to our four ops + the events mux.
 *
 * THE STRUCTURE (matching the header's io-model note):
 *   - ONE socket + ONE reader thread. The reader recv-polls the nonblocking
 *     socket (2 ms between quiet reads — the poll-loop shape offs_client's
 *     pd_loop_run_once(poll_timeout) gives, minus the pd machinery: the
 *     client never links the frame or streams layer), feeds the framer, and
 *     routes each complete frame: a response whose req_id matches the ONE
 *     in-flight request's slot completes that round trip; the events
 *     subscription's stream (rides the SAME socket under the subscribe
 *     request's req_id) delivers to the events callback as an independent
 *     flow. Everything else drops loud.
 *   - The request ops run on the CALLER's thread: encode + send + a bounded
 *     condvar wait (request_timeout_ms); the reader hands the DECODED
 *     payload to the slot, the caller interprets it into its op callback. A
 *     timeout / a refusal / a drop completes the op callback with a failing
 *     status AND fires the config's error callback.
 *   - The events connection-drop path reconnects on a 1 s -> 8 s backoff
 *     (max_retries budgets the attempts, 0 = forever), re-authenticating
 *     (tcp) and re-subscribing from the last delivered seq.
 *   - THE HELD-PAYLOAD TABLE (offs_client's held_payload list verbatim):
 *     every pointer handed to a callback is a FRESH copy the library owns;
 *     release unlinks + frees it, destroy frees every unreleased one.
 *
 * THE LOCK note: this client takes ONE platform mutex (offs_client's
 * client->lock shape) protecting the request slot, the subscription state,
 * the req-id counter, and the held-payload list — and it is held across
 * every send (the caller's request frame and the reader's reconnect
 * re-subscribe must never interleave frame bytes). Callbacks fire OUTSIDE
 * it (offs_client's snapshot-under-lock discipline). */

#include "sa_client.h"

typedef int sa_client_tu_anchor_t;   /* ISO C: a non-empty TU when gated */

#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)

#include "../../ClientApi/client_api_wire.h"
#include "../../Network/stream_framer.h"
#include "../../Platform/platform.h"
#include "../../Util/allocator.h"
#include "../../Util/log.h"
#include <errno.h>
#include <string.h>

/* The reconnect backoff (the header's pinned 1 s -> 8 s doubling), the read
 * poll's quiet cadence, and the backoff's cancellation slice (destroy joins
 * the reader inside one slice). */
#define SA_RECONNECT_BASE_MS 1000u
#define SA_RECONNECT_MAX_MS  8000u
#define SA_READ_POLL_MS      2u
#define SA_BACKOFF_SLICE_MS  100u

/* The wire's req_id DUALITY pins req_id as EVERY request struct's first
 * member (client_api_wire.h's note) — the shared roundtrip stamps the
 * freshly-assigned id through that pin instead of a per-type setter. And a
 * response type of 0 in the pending slot is the client's LOCAL-failure
 * sentinel: the waiter interprets it instead of dereferencing a payload. */
static void _stamp_req_id(void* req, uint64_t rid) {
  memcpy(req, &rid, sizeof(rid));
}

typedef struct {
  int active;          /* a request owns the slot */
  int done;            /* the reader completed it (or a drop failed it) */
  uint64_t req_id;
  uint64_t want_type;  /* the response type that completes it */
  uint64_t resp_type;  /* 0 = a LOCAL failure (no payload rides) */
  uint8_t resp_status;
  void* payload;       /* the DECODED wire payload; the waiter owns it */
} sa_pending_t;

/* The events subscription (ONE active per client). All mutation sits under
   client->lock (the reader's frame routing and the ops' setup race the same
   lock); the callback pointers are SNAPSHOTTED under it before the reader
   delivers outside. */
typedef struct {
  int active;
  int live;            /* the live marker arrived */
  char* sid;           /* OUR copy */
  uint64_t req_id;     /* the subscribe request's id — the stream's filter */
  uint64_t last_seq;   /* the last delivered record's seq (the reconnect's
                          resume-at cursor) */
  sa_client_events_cb_t cb;
  void* ctx;
} sa_sub_t;

/* offs_client's held_payload node: a COPY handed to a callback, waiting for
 * the consumer's one release (or destroy's sweep). */
typedef struct sa_held_t {
  void* ptr;
  struct sa_held_t* next;
} sa_held_t;

struct sa_client_t {
  sa_client_config_t cfg;   /* the value copy; the three strings below are
                               OUR copies (the config's may die at call) */
  char* socket_path;
  char* host;
  char* api_key;            /* scrubbed by length at destroy (CA6) */
  size_t api_key_len;

  platform_mutex_t* lock;
  platform_condvar_t* wake;   /* request-slot + subscription completions */

  platform_socket_t* sock;
  stream_framer_t* framer;
  platform_thread_t* reader;
  volatile uint8_t running;    /* destroy's stop flag */
  volatile uint8_t connected;  /* under lock; off = no sends, no requests */

  uint64_t next_req_id;
  sa_pending_t pending;
  sa_sub_t sub;

  sa_held_t* held;   /* offs_client's list shape, under lock */
};

/* ---- small helpers --------------------------------------------------------- */

static char* _dup_string(const char* s) {
  size_t n;
  char* d;
  if (s == NULL) return NULL;
  n = strlen(s) + 1;
  d = get_memory(n);
  if (d != NULL) memcpy(d, s, n);
  return d;
}

/* The wire's api-key bound (the server's decode refuses an over-bound key —
   refuse it at the client, loud, before a byte is sent). */
static int _key_in_bounds(const sa_client_t* c) {
  return (c->api_key != NULL && c->api_key_len > 0 &&
          c->api_key_len <= CA_WIRE_KEY_MAX);
}

static uint64_t _now_ms(void) {
  return platform_monotonic_ns() / 1000000ull;
}

/* Clear the LOCK-HELD subscription state (all callers hold the lock): the
   unsubscribe's terminal marker, the reconnect's budget give-up, and the
   subscribe's failure paths share this shape. */
static void _clear_sub_locked(sa_client_t* c);

/* ---- the held-payload table (offs_client's _hold_payload verbatim) --------- */

static void _hold_payload(sa_client_t* c, void* ptr) {
  sa_held_t* node;
  if (ptr == NULL || c == NULL) return;
  node = get_clear_memory(sizeof(*node));
  if (node == NULL) {
    free(ptr);   /* never leak: the consumer's later release is a no-op */
    return;
  }
  node->ptr = ptr;
  platform_mutex_lock(c->lock);
  node->next = c->held;
  c->held = node;
  platform_mutex_unlock(c->lock);
}

void sa_client_release_payload(sa_client_t* client, void* payload) {
  sa_held_t** link;
  sa_held_t* node;
  if (client == NULL || payload == NULL) return;
  platform_mutex_lock(client->lock);
  link = &client->held;
  while (*link != NULL) {
    if ((*link)->ptr == payload) break;
    link = &(*link)->next;
  }
  node = *link;
  if (node != NULL) *link = node->next;
  platform_mutex_unlock(client->lock);
  if (node == NULL) return;   /* unknown / already released: a safe no-op */
  free(node);
  free(payload);
}

/* Destroy's sweep: every unreleased copy goes here (a released-once pointer
   was already unlinked and freed — released exactly once, exactly one
   free). */
static void _release_all_payloads(sa_client_t* c) {
  sa_held_t* node;
  platform_mutex_lock(c->lock);
  node = c->held;
  c->held = NULL;
  platform_mutex_unlock(c->lock);
  while (node != NULL) {
    sa_held_t* next = node->next;
    free(node->ptr);
    free(node);
    node = next;
  }
}

/* ---- the error channel ------------------------------------------------------ */

/* Fires the config's error callback with a HELD copy of the text (the
   lifetime rule: the consumer owns it after the call returns). */
static void _error_local(sa_client_t* c, uint64_t rid, uint8_t status,
                         const char* text) {
  char* copy;
  if (c->cfg.error_cb == NULL) return;
  copy = _dup_string(text);
  if (copy == NULL) return;
  c->cfg.error_cb(c->cfg.error_ctx, rid, status, copy);
  _hold_payload(c, copy);
}

/* ---- channel io -------------------------------------------------------------- */

static int _send_framed(sa_client_t* c, uint64_t type, void* payload) {
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  uint8_t* framed;
  size_t flen;
  size_t off = 0;
  int rounds = 0;

  if (c->sock == NULL) return -1;
  if (ca_wire_encode(type, payload, &raw, &raw_len) != 0) return -1;
  framed = stream_frame_encode(raw, raw_len, &flen);
  free(raw);
  if (framed == NULL) return -1;
  /* send all: the socket is nonblocking (a quiet peer's EAGAIN waits in 2 ms
     slices; our frames are small — the round cap is a stuck-peer escape) */
  while (off < flen) {
    ssize_t n = platform_socket_send(c->sock, framed + off, flen - off);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
        ++rounds < 5000) {
      platform_sleep_ms(SA_READ_POLL_MS);
      continue;
    }
    free(framed);
    return -1;
  }
  free(framed);
  return 0;
}

static void _close_channel(sa_client_t* c) {
  if (c->framer != NULL) {
    stream_framer_destroy(c->framer);
    c->framer = NULL;
  }
  if (c->sock != NULL) {
    platform_socket_destroy(c->sock);
    c->sock = NULL;
  }
  platform_mutex_lock(c->lock);
  c->connected = 0;
  platform_mutex_unlock(c->lock);
}

/* The TCP auth exchange: the connection's FIRST frame pair (the header's
   rule). The wait is bounded by connect_timeout_ms. Runs before the reader
   thread exists (initial connect) or on the reader thread (a reconnect) —
   the socket/framer are exclusively ours here either way. The exchanged
   auth request's req_id rides the wire's first-member duality: it is
   STAMPED below before the send. */
static int _exchange_auth(sa_client_t* c) {
  ca_auth_request_t req;
  uint64_t deadline;
  void* payload = NULL;
  uint64_t rid = 0;

  if (!_key_in_bounds(c)) {
    log_error("sa_client: a tcp connection needs a bounded, non-empty "
              "api key");
    return -1;
  }
  memset(&req, 0, sizeof(req));
  platform_mutex_lock(c->lock);
  rid = ++c->next_req_id;
  _stamp_req_id(&req, rid);
  platform_mutex_unlock(c->lock);
  req.api_key = c->api_key;   /* BORROWED for the encode: the wire copies the
                                 key into the cbor; the client's copy sees no
                                 scrub here — destroy's scrub owns that */
  if (_send_framed(c, CA_AUTH_REQUEST, &req) != 0) {
    log_error("sa_client: the auth exchange's send failed");
    return -1;
  }
  deadline = _now_ms() + c->cfg.connect_timeout_ms;
  for (;;) {
    uint8_t buf[4096];
    uint8_t* fdata;
    size_t flen;
    int got = 0;
    ssize_t n;
    if (_now_ms() >= deadline) {
      log_error("sa_client: the auth exchange timed out");
      return -1;
    }
    n = platform_socket_recv(c->sock, buf, sizeof(buf));
    if (n > 0) {
      if (stream_framer_feed(c->framer, buf, (size_t)n) != 0) {
        log_error("sa_client: the framer refused the auth bytes");
        return -1;
      }
      while ((fdata = stream_framer_next(c->framer, &flen)) != NULL) {
        uint64_t type2 = 0, rid2 = 0;
        void* p2 = NULL;
        uint8_t st2 = 0;
        if (ca_wire_decode_bytes(fdata, flen, &type2, &p2, &rid2, &st2) != 0 ||
            type2 != CA_AUTH_RESPONSE || rid2 != rid) {
          log_error("sa_client: the auth exchange saw a frame that is not "
                    "its response");
          ca_wire_payload_destroy(type2, p2);
          free(fdata);
          return -1;
        }
        free(fdata);
        payload = p2;
        got = 1;
        break;
      }
      if (got) break;
      continue;
    }
    if (n == 0) {
      log_error("sa_client: the auth exchange saw the hangup");
      return -1;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      platform_sleep_ms(SA_READ_POLL_MS);
      continue;
    }
    log_error("sa_client: the auth exchange's read failed (errno=%d)", errno);
    return -1;
  }
  {
    ca_auth_response_t* res = (ca_auth_response_t*)payload;
    int ok = (res->status == 0);
    uint8_t st = res->status;
    ca_wire_payload_destroy(CA_AUTH_RESPONSE, res);
    if (!ok) {
      log_error("sa_client: the api key was refused (status=%u)", st);
      return -1;
    }
  }
  log_info("sa_client: the tcp connection authenticated");
  return 0;
}

/* Open (or REopen — the reconnect's path) the channel: connect, own a fresh
   framer, run the tcp auth exchange FIRST. Success sets connected. */
static int _open_channel(sa_client_t* c) {
  platform_socket_t* sock = NULL;
  platform_address_t addr;

  memset(&addr, 0, sizeof(addr));
  if (c->cfg.transport == SA_CLIENT_TRANSPORT_UNIX) {
    if (c->socket_path == NULL) {
      log_error("sa_client: the unix transport needs a socket_path");
      return -1;
    }
    sock = platform_socket_create(PLATFORM_AF_LOCAL, 1);
    if (sock == NULL) {
      log_error("sa_client: the unix socket create failed");
      return -1;
    }
    addr.family = PLATFORM_AF_LOCAL;
    strncpy(addr.local.path, c->socket_path, sizeof(addr.local.path) - 1);
    if (platform_socket_connect(sock, &addr) != 0) {
      log_error("sa_client: the unix connect failed (errno=%d)", errno);
      platform_socket_destroy(sock);
      return -1;
    }
  } else {
    if (c->host == NULL ||
        platform_address_parse(&addr, c->host, c->cfg.port) != 0) {
      log_error("sa_client: the tcp endpoint needs a parseable host:port");
      return -1;
    }
    sock = platform_socket_create(addr.family, 1);
    if (sock == NULL) {
      log_error("sa_client: the tcp socket create failed");
      return -1;
    }
    if (platform_socket_connect(sock, &addr) != 0) {
      log_error("sa_client: the tcp connect failed (errno=%d)", errno);
      platform_socket_destroy(sock);
      return -1;
    }
  }
  platform_socket_set_nonblocking(sock);
  /* a fresh channel owns a fresh framer (the old one's partial frame died
     with the drop) */
  if (c->framer != NULL) stream_framer_destroy(c->framer);
  if (c->sock != NULL) platform_socket_destroy(c->sock);
  c->sock = sock;
  c->framer = stream_framer_create();
  if (c->framer == NULL) {
    c->sock = NULL;
    platform_socket_destroy(sock);
    log_error("sa_client: the framer create failed");
    return -1;
  }
  if (c->cfg.transport == SA_CLIENT_TRANSPORT_TCP && _exchange_auth(c) != 0) {
    _close_channel(c);
    return -1;
  }
  platform_mutex_lock(c->lock);
  c->connected = 1;
  platform_mutex_unlock(c->lock);
  return 0;
}

/* ---- the frame routing -------------------------------------------------------- */

/* Completes the pending slot under lock (the waiter owns the payload from
   here; resp_type 0 = the local-failure sentinel, no payload). */
static void _complete_pending_locked(sa_client_t* c, uint64_t type,
                                     uint8_t status, void* payload) {
  c->pending.done = 1;
  c->pending.resp_type = type;
  c->pending.resp_status = status;
  c->pending.payload = payload;
  platform_condvar_broadcast(c->wake);
}

/* Drains (and forgets) the pending slot's completion under lock; the facts
   return by out-params and the payload becomes the CALLER's to interpret. */
static void _take_pending_locked(sa_client_t* c, uint64_t* type_out,
                                 uint8_t* status_out, void** payload_out) {
  *type_out = c->pending.resp_type;
  *status_out = c->pending.resp_status;
  *payload_out = c->pending.payload;
  memset(&c->pending, 0, sizeof(c->pending));
  platform_condvar_broadcast(c->wake);
}

/* The daemon's refusal delivery (the shared ERROR-frame completion): the
   error callback carries the wire text; the status returns for the op
   callback's failure completion. The payload goes here. MUST run off the
   lock (it holds). */
static uint8_t _deliver_daemon_error(sa_client_t* c, void* payload) {
  ca_error_t* err = (ca_error_t*)payload;
  uint8_t st = err->status;
  uint64_t rid = err->req_id;
  char* text = _dup_string(err->text);
  if (text == NULL) {
    _error_local(c, rid, SA_CLIENT_STATUS_ALLOC,
                 "out of memory delivering an error");
  } else if (c->cfg.error_cb != NULL) {
    c->cfg.error_cb(c->cfg.error_ctx, rid, st, text);
    _hold_payload(c, text);
  } else {
    free(text);
  }
  ca_wire_payload_destroy(CA_ERROR, err);
  return st;
}

/* Delivers ONE events response (record or marker) to the subscription's
   callback (the reader thread; the cb/ctx snapshot rides the caller's
   locked section). sid + record_json arrive as FRESH copies handed to the
   callback — held per the lifetime rule. */
static void _deliver_event(sa_client_t* c, const char* sid, uint64_t seq,
                           uint8_t op, const char* record_json,
                           sa_client_events_cb_t cb, void* ctx) {
  char* sid_copy;
  char* rec_copy;

  sid_copy = _dup_string(sid);
  rec_copy = (record_json != NULL) ? _dup_string(record_json) : NULL;
  if ((sid != NULL && sid_copy == NULL) ||
      (record_json != NULL && rec_copy == NULL)) {
    free(sid_copy);
    free(rec_copy);
    _error_local(c, 0, SA_CLIENT_STATUS_ALLOC,
                 "out of memory delivering an event");
    return;
  }
  if (cb != NULL) cb(ctx, sid_copy, seq, op, rec_copy);
  _hold_payload(c, sid_copy);
  if (rec_copy != NULL) _hold_payload(c, rec_copy);
}

/* The reader's one decoded frame. Routes:
   1. The subscription's stream: CA_EVENTS_RESPONSE carrying the subscribe
      request's req_id — records advance last_seq, markers set live; a
      marker also completes a matching blocking subscribe.
   2. The pending slot: the response (or CA_ERROR, or the unsubscribe's
      terminal marker) whose req_id matches. The unsub marker ALSO ends the
      subscription and tells its callback.
   Everything else drops loud (the server sends nothing unsolicited). */
static void _handle_frame(sa_client_t* c, const uint8_t* raw, size_t len) {
  uint64_t type = 0, rid = 0;
  void* payload = NULL;
  uint8_t status = 0;
  ca_events_response_t* ev = NULL;
  sa_client_events_cb_t ev_cb = NULL;
  void* ev_ctx = NULL;
  int deliver_event = 0;
  int unsub_marker = 0;

  if (ca_wire_decode_bytes(raw, len, &type, &payload, &rid, &status) != 0) {
    log_error("sa_client: a malformed wire frame dropped loud");
    return;
  }

  platform_mutex_lock(c->lock);
  if (type == CA_EVENTS_RESPONSE && c->sub.active && rid == c->sub.req_id) {
    ev = (ca_events_response_t*)payload;
    payload = NULL;
    deliver_event = 1;
    ev_cb = c->sub.cb;
    ev_ctx = c->sub.ctx;
    if (ev->seq > 0) {
      c->sub.last_seq = ev->seq;
    } else {
      c->sub.live = 1;
      if (c->pending.active && !c->pending.done &&
          c->pending.req_id == rid) {
        _complete_pending_locked(c, type, status, NULL);
      }
    }
  } else if (c->pending.active && !c->pending.done &&
             rid == c->pending.req_id) {
    if (type == CA_EVENTS_RESPONSE &&
        c->pending.want_type == CA_EVENTS_RESPONSE) {
      /* the UNSUBSCRIBE's terminal marker: the subscription ends here —
         tell the consumer with the same marker shape, the waiter releases
         with success */
      ev = (ca_events_response_t*)payload;
      payload = NULL;
      unsub_marker = 1;
      ev_cb = c->sub.cb;
      ev_ctx = c->sub.ctx;
      c->sub.active = 0;
      free(c->sub.sid);
      c->sub.sid = NULL;
      _complete_pending_locked(c, type, status, NULL);
    } else {
      _complete_pending_locked(c, type, status, payload);
      payload = NULL;
    }
  } else {
    log_error("sa_client: an unsolicited frame dropped loud (type=%llu "
              "req_id=%llu)", (unsigned long long)type,
              (unsigned long long)rid);
  }
  platform_mutex_unlock(c->lock);

  if (payload != NULL) ca_wire_payload_destroy(type, payload);
  if (deliver_event || unsub_marker) {
    _deliver_event(c, ev->sid, ev->seq, ev->op, ev->record_json, ev_cb,
                   ev_ctx);
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, ev);
  }
}

/* ---- the request roundtrip ------------------------------------------------------ */

/* The shared blocking roundtrip. Returns:
   0 = a response arrived (the DECODED wire payload rides *resp_out — the
       CALLER owns it); rid_out/rtype_out/rstatus_out filled;
   1 = failed: the error callback FIRED, *fail_out carries the status — the
       op callback completes with it and no payload;
  -1 = the call was refused outright (the busy slot case fires the error
       callback with req_id 0) — the op callback does NOT run.
  The caller's req payload is STAMPED with the assigned req_id (the wire's
  first-member duality) and is the caller's to destroy after the call.
  THE SEND HOLDS THE LOCK: the caller's request frame and the reader's
  reconnect re-subscribe can never interleave bytes. */
static int _roundtrip(sa_client_t* c, uint64_t want_type, void* req_payload,
                      uint64_t req_type, uint64_t* rid_out,
                      uint64_t* rtype_out, uint8_t* rstatus_out,
                      void** resp_out, uint8_t* fail_out) {
  uint64_t deadline;
  int done;

  platform_mutex_lock(c->lock);
  if (!c->connected) {
    platform_mutex_unlock(c->lock);
    _error_local(c, 0, SA_CLIENT_STATUS_DISCONNECTED, "not connected");
    *fail_out = SA_CLIENT_STATUS_DISCONNECTED;
    return 1;
  }
  if (c->pending.active) {
    platform_mutex_unlock(c->lock);
    _error_local(c, 0, SA_CLIENT_STATUS_BUSY,
                 "a request is already in flight");
    *fail_out = SA_CLIENT_STATUS_BUSY;
    return 1;
  }
  *rid_out = ++c->next_req_id;
  _stamp_req_id(req_payload, *rid_out);
  memset(&c->pending, 0, sizeof(c->pending));
  c->pending.active = 1;
  c->pending.req_id = *rid_out;
  c->pending.want_type = want_type;
  if (_send_framed(c, req_type, req_payload) != 0) {
    c->pending.active = 0;
    platform_mutex_unlock(c->lock);
    _error_local(c, *rid_out, SA_CLIENT_STATUS_DISCONNECTED,
                 "the connection dropped on send");
    *fail_out = SA_CLIENT_STATUS_DISCONNECTED;
    return 1;
  }
  platform_mutex_unlock(c->lock);

  deadline = _now_ms() + c->cfg.request_timeout_ms;
  platform_mutex_lock(c->lock);
  while (!c->pending.done) {
    uint64_t now = _now_ms();
    if (now >= deadline) break;
    platform_condvar_timed_wait(c->wake, c->lock,
                                (uint32_t)(deadline - now));
  }
  done = c->pending.done;
  if (done) {
    _take_pending_locked(c, rtype_out, rstatus_out, resp_out);
  } else {
    /* cleared whatever the outcome (a timeout's late response lands loud
       in the drop branch) */
    *rtype_out = 0;
    *rstatus_out = SA_CLIENT_STATUS_TIMEOUT;
    *resp_out = NULL;
    memset(&c->pending, 0, sizeof(c->pending));
  }
  platform_mutex_unlock(c->lock);

  if (!done) {
    _error_local(c, *rid_out, SA_CLIENT_STATUS_TIMEOUT,
                 "the request timed out");
    *fail_out = SA_CLIENT_STATUS_TIMEOUT;
    return 1;
  }
  if (*rtype_out == 0) {
    /* the reader's local-failure sentinel (the channel dropped mid-wait) */
    _error_local(c, *rid_out, *rstatus_out, "the connection was lost");
    *fail_out = *rstatus_out;
    return 1;
  }
  if (*rtype_out == CA_ERROR) {
    /* the daemon's refusal: the error callback carries the wire text; the
       op callback completes with the wire's status */
    *fail_out = _deliver_daemon_error(c, *resp_out);
    *resp_out = NULL;
    return 1;
  }
  return 0;
}

/* Clear the subscription taking the lock (a subscribe/unsubscribe's failure
   path on the caller thread). */
static void _clear_sub(sa_client_t* c) {
  platform_mutex_lock(c->lock);
  _clear_sub_locked(c);
  platform_mutex_unlock(c->lock);
}

/* ---- the ops ---------------------------------------------------------------------- */

int sa_client_prompt(sa_client_t* client, const char* sid, const char* text,
                     sa_client_prompt_cb_t callback, void* ctx) {
  ca_prompt_request_t* req;
  uint64_t rid = 0, rtype = 0;
  uint8_t rstatus = 0, fail = 0;
  void* resp = NULL;
  int rc;

  if (client == NULL || text == NULL) return -1;
  req = get_clear_memory(sizeof(*req));
  if (req == NULL) {
    _error_local(client, 0, SA_CLIENT_STATUS_ALLOC,
                 "out of memory building the prompt");
    return 0;
  }
  req->sid = _dup_string(sid);
  req->text = _dup_string(text);
  if (req->text == NULL || (sid != NULL && req->sid == NULL)) {
    _error_local(client, 0, SA_CLIENT_STATUS_ALLOC,
                 "out of memory building the prompt");
    ca_wire_payload_destroy(CA_PROMPT_REQUEST, req);
    return 0;
  }
  rc = _roundtrip(client, CA_PROMPT_RESPONSE, req, CA_PROMPT_REQUEST, &rid,
                  &rtype, &rstatus, &resp, &fail);
  ca_wire_payload_destroy(CA_PROMPT_REQUEST, req);
  if (rc < 0) return -1;
  if (rc == 1) {
    if (callback != NULL) callback(ctx, fail, NULL);
    return 0;
  }
  if (rtype != CA_PROMPT_RESPONSE) {
    /* unreachable (the slot filters by the pairing) — drop loud, move on */
    ca_wire_payload_destroy(rtype, resp);
    return 0;
  }
  {
    ca_prompt_response_t* res = (ca_prompt_response_t*)resp;
    uint8_t st = res->status;
    char* sid_copy = _dup_string(res->sid);
    ca_wire_payload_destroy(CA_PROMPT_RESPONSE, res);
    if (sid_copy == NULL) {
      _error_local(client, rid, SA_CLIENT_STATUS_ALLOC,
                   "out of memory delivering the prompt response");
      if (callback != NULL) callback(ctx, SA_CLIENT_STATUS_ALLOC, NULL);
      return 0;
    }
    if (callback != NULL) callback(ctx, st, sid_copy);
    _hold_payload(client, sid_copy);
  }
  return 0;
}

int sa_client_interrupt(sa_client_t* client, const char* sid,
                        sa_client_interrupt_cb_t callback, void* ctx) {
  ca_interrupt_request_t* req;
  uint64_t rid = 0, rtype = 0;
  uint8_t rstatus = 0, fail = 0;
  void* resp = NULL;
  int rc;

  if (client == NULL || sid == NULL) return -1;
  req = get_clear_memory(sizeof(*req));
  if (req == NULL) {
    _error_local(client, 0, SA_CLIENT_STATUS_ALLOC,
                 "out of memory building the interrupt");
    return 0;
  }
  req->sid = _dup_string(sid);
  if (req->sid == NULL) {
    _error_local(client, 0, SA_CLIENT_STATUS_ALLOC,
                 "out of memory building the interrupt");
    ca_wire_payload_destroy(CA_INTERRUPT_REQUEST, req);
    return 0;
  }
  rc = _roundtrip(client, CA_INTERRUPT_RESPONSE, req, CA_INTERRUPT_REQUEST,
                  &rid, &rtype, &rstatus, &resp, &fail);
  ca_wire_payload_destroy(CA_INTERRUPT_REQUEST, req);
  if (rc < 0) return -1;
  if (rc == 1) {
    if (callback != NULL) callback(ctx, fail);
    return 0;
  }
  if (rtype != CA_INTERRUPT_RESPONSE) {
    ca_wire_payload_destroy(rtype, resp);
    return 0;
  }
  {
    ca_interrupt_response_t* res = (ca_interrupt_response_t*)resp;
    uint8_t st = res->status;
    ca_wire_payload_destroy(CA_INTERRUPT_RESPONSE, res);
    if (callback != NULL) callback(ctx, st);
  }
  return 0;
}

/* Builds the rows' HELD copies from the decoded listing: the rows array
   itself plus each row's non-NULL string are separate held pointers. On a
   copy failure the PARTIAL build unwinds (freeing what it made), the error
   callback fires, *oom_out = 1. nrecords == 0 returns NULL with *oom_out 0
   (an empty store is a legit empty listing). */
static sa_client_session_row_t* _build_rows(sa_client_t* c,
                                            const ca_sessions_record_t* recs,
                                            size_t nrecords, uint64_t rid,
                                            int* oom_out) {
  sa_client_session_row_t* rows;
  size_t made;

  *oom_out = 0;
  if (nrecords == 0) return NULL;
  rows = get_memory(nrecords * sizeof(*rows));
  if (rows == NULL) {
    _error_local(c, rid, SA_CLIENT_STATUS_ALLOC,
                 "out of memory delivering the listing");
    *oom_out = 1;
    return NULL;
  }
  memset(rows, 0, nrecords * sizeof(*rows));
  for (made = 0; made < nrecords; made++) {
    rows[made].sid = _dup_string(recs[made].sid);
    rows[made].status = _dup_string(recs[made].status);
    rows[made].goal = _dup_string(recs[made].goal);
    rows[made].created = recs[made].created;
    rows[made].depth = recs[made].depth;
    if ((recs[made].sid != NULL && rows[made].sid == NULL) ||
        (recs[made].status != NULL && rows[made].status == NULL) ||
        (recs[made].goal != NULL && rows[made].goal == NULL)) {
      for (size_t i = 0; i <= made; i++) {
        free((void*)rows[i].sid);
        free((void*)rows[i].status);
        free((void*)rows[i].goal);
      }
      free(rows);
      _error_local(c, rid, SA_CLIENT_STATUS_ALLOC,
                   "out of memory delivering the listing");
      *oom_out = 1;
      return NULL;
    }
  }
  return rows;
}

int sa_client_list_sessions(sa_client_t* client,
                            sa_client_sessions_cb_t callback, void* ctx) {
  ca_sessions_request_t* req;
  uint64_t rid = 0, rtype = 0;
  uint8_t rstatus = 0, fail = 0;
  void* resp = NULL;
  int rc;

  if (client == NULL) return -1;
  req = get_clear_memory(sizeof(*req));
  if (req == NULL) {
    _error_local(client, 0, SA_CLIENT_STATUS_ALLOC,
                 "out of memory building the listing");
    return 0;
  }
  rc = _roundtrip(client, CA_SESSIONS_RESPONSE, req, CA_SESSIONS_REQUEST,
                  &rid, &rtype, &rstatus, &resp, &fail);
  ca_wire_payload_destroy(CA_SESSIONS_REQUEST, req);
  if (rc < 0) return -1;
  if (rc == 1) {
    if (callback != NULL) callback(ctx, fail, NULL, 0);
    return 0;
  }
  if (rtype != CA_SESSIONS_RESPONSE) {
    ca_wire_payload_destroy(rtype, resp);
    return 0;
  }
  {
    ca_sessions_response_t* res = (ca_sessions_response_t*)resp;
    /* the rows build COPIES the decoded fields while the response's own
       memory still lives; the destroy releases the wire's struct after */
    ca_sessions_record_t* recs = res->records;
    size_t nrecords = res->nrecords;
    sa_client_session_row_t* rows;
    int oom = 0;
    if (rstatus != 0) {
      /* the wire's sessions response carries NO status field (the decode
         fills the out-param 0) — a nonzero here is unreachable; drop loud */
      if (callback != NULL) callback(ctx, rstatus, NULL, 0);
      ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, res);
      return 0;
    }
    rows = _build_rows(client, recs, nrecords, rid, &oom);
    ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, res);
    if (oom) {
      if (callback != NULL) callback(ctx, SA_CLIENT_STATUS_ALLOC, NULL, 0);
      return 0;
    }
    /* the lifetime rule: the rows array AND every non-NULL string in it are
       each held separately (one release each; NULL fields no-op) */
    _hold_payload(client, rows);
    for (size_t i = 0; i < nrecords; i++) {
      _hold_payload(client, (void*)rows[i].sid);
      if (rows[i].status != NULL) {
        _hold_payload(client, (void*)rows[i].status);
      }
      if (rows[i].goal != NULL) {
        _hold_payload(client, (void*)rows[i].goal);
      }
    }
    if (callback != NULL) callback(ctx, rstatus, rows, nrecords);
  }
  return 0;
}

int sa_client_subscribe_events(sa_client_t* client, const char* sid,
                               sa_client_events_cb_t callback, void* ctx) {
  ca_events_request_t req;
  uint64_t deadline;
  uint64_t rtype = 0;
  uint8_t st = 0;
  void* payload = NULL;
  int rc = -1;

  if (client == NULL || sid == NULL || callback == NULL) return -1;
  platform_mutex_lock(client->lock);
  if (!client->connected) {
    platform_mutex_unlock(client->lock);
    _error_local(client, 0, SA_CLIENT_STATUS_DISCONNECTED, "not connected");
    return -1;
  }
  if (client->sub.active) {
    platform_mutex_unlock(client->lock);
    _error_local(client, 0, SA_CLIENT_STATUS_BUSY,
                 "an events subscription is already active");
    return -1;
  }
  if (client->pending.active) {
    platform_mutex_unlock(client->lock);
    _error_local(client, 0, SA_CLIENT_STATUS_BUSY,
                 "a request is already in flight");
    return -1;
  }
  memset(&req, 0, sizeof(req));
  req.req_id = ++client->next_req_id;
  client->sub.active = 1;
  client->sub.live = 0;
  client->sub.last_seq = 0;
  client->sub.req_id = req.req_id;
  client->sub.cb = callback;
  client->sub.ctx = ctx;
  client->sub.sid = _dup_string(sid);
  if (client->sub.sid == NULL) {
    client->sub.active = 0;
    platform_mutex_unlock(client->lock);
    _error_local(client, 0, SA_CLIENT_STATUS_ALLOC,
                 "out of memory building the subscription");
    return 0;
  }
  /* the request encodes from OUR sub copy (borrowed for the encode; the
     wire serializes it before any of this can end) */
  req.sid = client->sub.sid;
  req.op = CA_EVENTS_REPLAY_THEN_LIVE;
  req.from_seq = 0;
  memset(&client->pending, 0, sizeof(client->pending));
  client->pending.active = 1;
  client->pending.req_id = req.req_id;
  client->pending.want_type = CA_EVENTS_RESPONSE;
  if (_send_framed(client, CA_EVENTS_REQUEST, &req) != 0) {
    client->pending.active = 0;
    platform_mutex_unlock(client->lock);
    _clear_sub(client);
    _error_local(client, req.req_id, SA_CLIENT_STATUS_DISCONNECTED,
                 "the connection dropped on send");
    return -1;
  }
  platform_mutex_unlock(client->lock);

  /* the bounded wait: the replay's records ride through to the callback,
     the subscription ends LIVE when the marker lands (or the refusal /
     timeout ends it through the error callback) */
  deadline = _now_ms() + client->cfg.request_timeout_ms;
  platform_mutex_lock(client->lock);
  for (;;) {
    uint64_t now = _now_ms();
    if (client->sub.live) {
      _take_pending_locked(client, &rtype, &st, &payload);
      rc = 0;
      break;
    }
    if (!client->sub.active) {
      /* the give-up path: the reconnect's budget exhausted mid-subscribe
         (its own error callback already ran); the pending's sentinel
         drains here */
      if (client->pending.done) {
        _take_pending_locked(client, &rtype, &st, &payload);
        ca_wire_payload_destroy(rtype, payload);
      }
      rc = -1;
      break;
    }
    if (client->pending.done) {
      _take_pending_locked(client, &rtype, &st, &payload);
      platform_mutex_unlock(client->lock);
      if (rtype == CA_ERROR) {
        /* the daemon refused the subscribe: the error callback carries the
           wire text (the shared delivery below), the sub dies */
        _deliver_daemon_error(client, payload);
        _clear_sub(client);
        rc = 0;   /* the failure was delivered through the callbacks */
      } else if (rtype == 0) {
        /* the reader's local-failure sentinel (the channel dropped
           mid-subscribe): the error callback fires, but the SUB STAYS —
           the subscription the caller registered rides the reader's
           reconnect (a give-up clears it there); only this call fails */
        _error_local(client, req.req_id, st, "the connection was lost");
        rc = -1;
      } else {
        /* a marker-shaped completion whose delivery already ran — the live
           flag was set before the completion */
        rc = 0;
      }
      return rc;
    }
    if (now >= deadline) {
      client->pending.active = 0;
      memset(&client->pending, 0, sizeof(client->pending));
      platform_mutex_unlock(client->lock);
      _clear_sub(client);
      _error_local(client, req.req_id, SA_CLIENT_STATUS_TIMEOUT,
                   "the events subscribe timed out");
      return -1;
    }
    platform_condvar_timed_wait(client->wake, client->lock,
                                (uint32_t)(deadline - now));
  }
  platform_mutex_unlock(client->lock);
  return rc;
}

int sa_client_unsubscribe_events(sa_client_t* client) {
  ca_events_request_t req;
  uint64_t deadline;
  uint64_t rid = 0;
  int rc = -1;

  if (client == NULL) return -1;
  platform_mutex_lock(client->lock);
  if (!client->connected) {
    platform_mutex_unlock(client->lock);
    _error_local(client, 0, SA_CLIENT_STATUS_DISCONNECTED, "not connected");
    return -1;
  }
  if (!client->sub.active) {
    platform_mutex_unlock(client->lock);
    _error_local(client, 0, SA_CLIENT_STATUS_BUSY,
                 "no active subscription");
    return -1;
  }
  if (client->pending.active) {
    platform_mutex_unlock(client->lock);
    _error_local(client, 0, SA_CLIENT_STATUS_BUSY,
                 "a request is already in flight");
    return -1;
  }
  memset(&req, 0, sizeof(req));
  rid = ++client->next_req_id;
  req.req_id = rid;
  req.sid = client->sub.sid;   /* BORROWED for the encode */
  req.op = CA_EVENTS_UNSUBSCRIBE;
  req.from_seq = 0;
  memset(&client->pending, 0, sizeof(client->pending));
  client->pending.active = 1;
  client->pending.req_id = rid;
  client->pending.want_type = CA_EVENTS_RESPONSE;
  if (_send_framed(client, CA_EVENTS_REQUEST, &req) != 0) {
    client->pending.active = 0;
    platform_mutex_unlock(client->lock);
    _error_local(client, rid, SA_CLIENT_STATUS_DISCONNECTED,
                 "the connection dropped on send");
    return -1;
  }
  platform_mutex_unlock(client->lock);

  deadline = _now_ms() + client->cfg.request_timeout_ms;
  platform_mutex_lock(client->lock);
  for (;;) {
    uint64_t now = _now_ms();
    uint64_t rtype = 0;
    void* payload = NULL;
    uint8_t st = 0;
    if (client->pending.done) {
      _take_pending_locked(client, &rtype, &st, &payload);
      platform_mutex_unlock(client->lock);
      if (rtype == CA_ERROR) {
        /* the refusal was delivered through the callbacks; the subscription
           stays active (the daemon did not ack) */
        _deliver_daemon_error(client, payload);
        rc = 0;
      } else if (rtype == 0) {
        /* the reader's local-failure sentinel: the channel dropped
           mid-unsubscribe (its reconnect owns the recovery; the
           subscription survives it) */
        _error_local(client, rid, st, "the connection was lost");
        rc = -1;
      } else {
        /* the unsub marker: the reader ended the subscription and its
           terminal-markers delivery already ran */
        rc = 0;
      }
      return rc;
    }
    if (now >= deadline) {
      client->pending.active = 0;
      memset(&client->pending, 0, sizeof(client->pending));
      platform_mutex_unlock(client->lock);
      _error_local(client, rid, SA_CLIENT_STATUS_TIMEOUT,
                   "the unsubscribe timed out");
      return -1;
    }
    platform_condvar_timed_wait(client->wake, client->lock,
                                (uint32_t)(deadline - now));
  }
}

/* ---- the reconnect ------------------------------------------------------------ */

/* The backoff's cancellation-slicing sleep: 100 ms steps so destroy's join
   never waits longer than one slice. 0 = stopped mid-wait. */
static int _backoff_sleep(sa_client_t* c, uint32_t total_ms) {
  uint32_t slept = 0;
  while (slept < total_ms) {
    if (!c->running) return 0;
    platform_sleep_ms(SA_BACKOFF_SLICE_MS);
    slept += SA_BACKOFF_SLICE_MS;
  }
  return c->running;
}

/* The events channel's reconnect: the 1 s -> 8 s doubling backoff,
   max_retries attempts (0 = forever), then the re-auth (tcp) and the
   re-subscribe from the resume-at cursor. Returns 1 = reconnected (the
   reader loops back to its reads), 0 = give up / stopped. */
static int _reconnect_loop(sa_client_t* c) {
  uint32_t attempt = 1;
  uint32_t delay = SA_RECONNECT_BASE_MS;

  for (;;) {
    if (!_backoff_sleep(c, delay)) return 0;
    if (c->cfg.max_retries != 0 && attempt > c->cfg.max_retries) {
      uint64_t rid;
      log_error("sa_client: the events channel's reconnect budget "
                "(max_retries=%u) is exhausted", c->cfg.max_retries);
      platform_mutex_lock(c->lock);
      rid = c->sub.req_id;
      _clear_sub_locked(c);
      platform_mutex_unlock(c->lock);
      _error_local(c, rid, SA_CLIENT_STATUS_DISCONNECTED,
                   "the events channel's reconnect budget is exhausted");
      return 0;
    }
    if (_open_channel(c) != 0) {
      log_warn("sa_client: reconnect attempt %u failed; the next one waits "
               "%u ms", attempt, delay * 2 > SA_RECONNECT_MAX_MS
                                    ? SA_RECONNECT_MAX_MS : delay * 2);
      attempt++;
      if (delay < SA_RECONNECT_MAX_MS) {
        delay *= 2;
        if (delay > SA_RECONNECT_MAX_MS) delay = SA_RECONNECT_MAX_MS;
      }
      continue;
    }
    /* re-subscribe: the channel filters on the subscribe request's req_id —
       a FRESH one — from the resume-at cursor (the outage's gap replays,
       then a NEW live marker) */
    {
      ca_events_request_t req;
      int send_rc;
      memset(&req, 0, sizeof(req));
      platform_mutex_lock(c->lock);
      req.req_id = ++c->next_req_id;
      c->sub.req_id = req.req_id;
      c->sub.live = 0;
      req.sid = c->sub.sid;   /* BORROWED for the encode */
      req.op = CA_EVENTS_REPLAY_THEN_LIVE;
      req.from_seq = c->sub.last_seq;
      send_rc = _send_framed(c, CA_EVENTS_REQUEST, &req);
      platform_mutex_unlock(c->lock);
      if (send_rc != 0) {
        log_error("sa_client: the reconnected channel refused the "
                  "re-subscribe; the backoff restarts");
        _close_channel(c);
        attempt++;
        continue;
      }
    }
    log_warn("sa_client: the events channel reconnected after %u attempt(s); "
             "the subscription resumes from seq %llu", attempt,
             (unsigned long long)c->sub.last_seq);
    return 1;
  }
}

/* The lock-held clear (see the forward declaration). */
static void _clear_sub_locked(sa_client_t* c) {
  c->sub.active = 0;
  free(c->sub.sid);
  c->sub.sid = NULL;
}

/* ---- the reader thread --------------------------------------------------------- */

/* The read loop: recv-poll the nonblocking socket, feed the framer, route
   each complete frame. Returns 0 = stopped (destroy), -1 = the channel
   dropped. */
static int _read_until_drop(sa_client_t* c, uint8_t* buf, size_t buf_size) {
  while (c->running) {
    ssize_t n = platform_socket_recv(c->sock, buf, buf_size);
    if (n > 0) {
      uint8_t* fdata;
      size_t flen;
      if (stream_framer_feed(c->framer, buf, (size_t)n) != 0) {
        log_error("sa_client: the framer refused the byte stream");
        return -1;
      }
      while ((fdata = stream_framer_next(c->framer, &flen)) != NULL) {
        _handle_frame(c, fdata, flen);
        free(fdata);
      }
      continue;
    }
    if (n == 0) return -1;   /* the hangup */
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      platform_sleep_ms(SA_READ_POLL_MS);
      continue;
    }
    log_error("sa_client: the socket read failed (errno=%d)", errno);
    return -1;
  }
  return 0;
}

static void* _reader_thread(void* arg) {
  sa_client_t* c = (sa_client_t*)arg;
  uint8_t buf[16384];

  while (c->running) {
    int rc = _read_until_drop(c, buf, sizeof(buf));
    if (rc == 0) break;   /* destroy stopped the reader */
    if (!c->running) break;
    /* THE DROP: fail the in-flight request (its waiter wakes with the
       disconnect sentinel), then reconnect only for an active
       subscription — a request alone never re-opens the channel. */
    log_warn("sa_client: the channel dropped");
    platform_mutex_lock(c->lock);
    if (c->pending.active && !c->pending.done) {
      _complete_pending_locked(c, 0, SA_CLIENT_STATUS_DISCONNECTED, NULL);
    }
    int has_sub = c->sub.active;
    platform_mutex_unlock(c->lock);
    if (!has_sub) break;
    if (_reconnect_loop(c) <= 0) break;   /* give-up or stopped: exit */
  }
  platform_mutex_lock(c->lock);
  c->connected = 0;
  platform_mutex_unlock(c->lock);
  /* the reader's exit still owes the waiter a completion if the give-up
     raced a request */
  platform_mutex_lock(c->lock);
  if (c->pending.active && !c->pending.done) {
    _complete_pending_locked(c, 0, SA_CLIENT_STATUS_DISCONNECTED, NULL);
  }
  platform_mutex_unlock(c->lock);
  return NULL;
}

/* ---- the surface ---------------------------------------------------------------- */

sa_client_config_t sa_client_config_default(void) {
  sa_client_config_t c;
  memset(&c, 0, sizeof(c));
  c.transport = SA_CLIENT_TRANSPORT_UNIX;
  c.connect_timeout_ms = 5000u;
  c.request_timeout_ms = 10000u;
  c.max_retries = 0u;   /* the events channel retries forever */
  return c;
}

sa_client_t* sa_client_connect(const sa_client_config_t* config) {
  sa_client_t* c;

  if (config == NULL) return NULL;
  if (config->transport == SA_CLIENT_TRANSPORT_UNIX) {
    if (config->socket_path == NULL || config->socket_path[0] == '\0') {
      log_error("sa_client_connect: the unix transport needs a socket_path");
      return NULL;
    }
  } else if (config->transport == SA_CLIENT_TRANSPORT_TCP) {
    if (config->host == NULL || config->host[0] == '\0' ||
        config->api_key == NULL || config->api_key[0] == '\0') {
      log_error("sa_client_connect: the tcp transport needs a host and a "
                "non-empty api key (the auth exchange opens every "
                "connection)");
      return NULL;
    }
  } else {
    log_error("sa_client_connect: an unknown transport (%d)",
              (int)config->transport);
    return NULL;
  }

  c = get_clear_memory(sizeof(*c));
  if (c == NULL) return NULL;
  c->cfg = *config;
  c->socket_path = _dup_string(config->socket_path);
  c->host = _dup_string(config->host);
  c->api_key = _dup_string(config->api_key);
  c->api_key_len = (config->api_key != NULL) ? strlen(config->api_key) + 1 : 0;
  c->lock = platform_mutex_create();
  c->wake = platform_condvar_create();
  c->running = 1;
  c->next_req_id = 0;
  if (c->socket_path == NULL || c->lock == NULL || c->wake == NULL ||
      (config->api_key != NULL && c->api_key == NULL) ||
      (config->transport == SA_CLIENT_TRANSPORT_TCP && !_key_in_bounds(c))) {
    log_error("sa_client_connect: the client's own setup failed");
    sa_client_destroy(c);
    return NULL;
  }
  if (_open_channel(c) != 0) {
    log_error("sa_client_connect: the channel never opened");
    sa_client_destroy(c);
    return NULL;
  }
  c->reader = platform_thread_create(_reader_thread, c);
  if (c->reader == NULL) {
    log_error("sa_client_connect: the reader thread never started");
    sa_client_destroy(c);
    return NULL;
  }
  return c;
}

void sa_client_destroy(sa_client_t* client) {
  if (client == NULL) return;
  client->running = 0;
  /* the reader exits inside one backoff slice (its sleeps slice on
     running); join BEFORE the channel dies — no send/recv races the free */
  if (client->reader != NULL) platform_thread_join(client->reader);
  _close_channel(client);
  _release_all_payloads(client);
  free(client->sub.sid);
  /* the api-key copy scrubs by its captured LENGTH (CA6's discipline: an
     embedded NUL's tail scrubs too — strlen cannot see past the first) */
  if (client->api_key != NULL) {
    memset(client->api_key, 0, client->api_key_len);
    free(client->api_key);
  }
  free(client->socket_path);
  free(client->host);
  free(client->pending.payload);
  platform_mutex_destroy(client->lock);
  platform_condvar_destroy(client->wake);
  free(client);
}

#endif /* SA_HAS_WDB && SA_HAS_STREAMS */