//
// Created by victor on 10/03/26.
//

#include "client_api_wire.h"
#include "../Util/allocator.h"
#include <stdlib.h>
#include <string.h>

/* A wavedb-free build has neither libcbor nor a wire (the header's gate
   note): the body below compiles to nothing there. The anchor keeps the
   translation unit non-empty (ISO C). */
typedef int ca_wire_tu_anchor_t;

#ifdef SA_HAS_WDB

/* ---- shared helpers (liboffs's client_api_wire.c idiom) ---- */

/* Encode a text field: NULL writes the "" sentinel (absent on the wire). */
static cbor_item_t* _encode_string(const char* str) {
  return cbor_build_string(str == NULL ? "" : str);
}

/* Decode a CBOR text string; NULL = non-string, empty (the absent sentinel),
   or over the bound — the caller destroys the payload and refuses the
   frame. */
static char* _decode_string(cbor_item_t* item, size_t max_len) {
  size_t len;
  char* str;

  if (!cbor_isa_string(item)) return NULL;
  len = cbor_string_length(item);
  if (len == 0 || len > max_len) return NULL;
  str = get_memory(len + 1);
  memcpy(str, cbor_string_handle(item), len);
  str[len] = '\0';
  return str;
}

/* Same as _decode_string, but an empty string decodes as "" — the prompt
   response's sid contract puts the absent marker ON the wire as "" (the
   element is always present there). */
static char* _decode_string_keep_empty(cbor_item_t* item, size_t max_len) {
  size_t len;
  char* str;

  if (!cbor_isa_string(item)) return NULL;
  len = cbor_string_length(item);
  if (len > max_len) return NULL;
  str = get_memory(len + 1);
  memcpy(str, cbor_string_handle(item), len);
  str[len] = '\0';
  return str;
}

/* Decode a CBOR uint; 0 = ok, -1 = not a (non-negative) uint. */
static int _decode_u64(cbor_item_t* item, uint64_t* out) {
  if (!cbor_isa_uint(item)) return -1;
  *out = cbor_get_int(item);
  return 0;
}

/* Decode a CBOR uint bounded to uint8 width (status/opcode fields — a
   truncating cast of a peer's value would be a trust bug). */
static int _decode_u8(cbor_item_t* item, uint8_t* out) {
  uint64_t value = 0;

  if (_decode_u64(item, &value) != 0 || value > UINT8_MAX) return -1;
  *out = (uint8_t)value;
  return 0;
}

/* Array-push + release: the pushed reference is the array's. */
static void _push(cbor_item_t* array, cbor_item_t* item) {
  (void)cbor_array_push(array, item);
  cbor_decref(&item);
}

static void _push_u64(cbor_item_t* array, uint64_t value) {
  _push(array, cbor_build_uint64(value));
}

static void _push_u8(cbor_item_t* array, uint8_t value) {
  _push(array, cbor_build_uint8(value));
}

static void _push_string(cbor_item_t* array, const char* str) {
  _push(array, _encode_string(str));
}

/* ---- per-type encoders (the header's pinned orders) ---- */

/* [1, req_id, text] | [1, req_id, sid, text] — sid present only when set */
static cbor_item_t* _encode_prompt_request(const ca_prompt_request_t* req) {
  cbor_item_t* array = cbor_new_definite_array(req->sid != NULL ? 4 : 3);

  _push_u8(array, CA_PROMPT_REQUEST);
  _push_u64(array, req->req_id);
  if (req->sid != NULL) {
    _push_string(array, req->sid);
  }
  _push_string(array, req->text);
  return array;
}

/* [2, req_id, status, sid] — sid "" when absent */
static cbor_item_t* _encode_prompt_response(const ca_prompt_response_t* res) {
  cbor_item_t* array = cbor_new_definite_array(4);

  _push_u8(array, CA_PROMPT_RESPONSE);
  _push_u64(array, res->req_id);
  _push_u8(array, res->status);
  _push_string(array, res->sid);
  return array;
}

/* [3, req_id, sid, op, from_seq] */
static cbor_item_t* _encode_events_request(const ca_events_request_t* req) {
  cbor_item_t* array = cbor_new_definite_array(5);

  _push_u8(array, CA_EVENTS_REQUEST);
  _push_u64(array, req->req_id);
  _push_string(array, req->sid);
  _push_u8(array, req->op);
  _push_u64(array, req->from_seq);
  return array;
}

/* [4, req_id, sid, seq, op, record_json] — seq 0 = the LIVE-TRANSITION
   marker (op echoed, record_json NULL → the "" sentinel) */
static cbor_item_t* _encode_events_response(const ca_events_response_t* res) {
  cbor_item_t* array = cbor_new_definite_array(6);

  _push_u8(array, CA_EVENTS_RESPONSE);
  _push_u64(array, res->req_id);
  _push_string(array, res->sid);
  _push_u64(array, res->seq);
  _push_u8(array, res->op);
  _push_string(array, res->record_json);
  return array;
}

/* [5, req_id, sid] */
static cbor_item_t* _encode_interrupt_request(
    const ca_interrupt_request_t* req) {
  cbor_item_t* array = cbor_new_definite_array(3);

  _push_u8(array, CA_INTERRUPT_REQUEST);
  _push_u64(array, req->req_id);
  _push_string(array, req->sid);
  return array;
}

/* [6, req_id, status] */
static cbor_item_t* _encode_interrupt_response(
    const ca_interrupt_response_t* res) {
  cbor_item_t* array = cbor_new_definite_array(3);

  _push_u8(array, CA_INTERRUPT_RESPONSE);
  _push_u64(array, res->req_id);
  _push_u8(array, res->status);
  return array;
}

/* [7, req_id] */
static cbor_item_t* _encode_sessions_request(
    const ca_sessions_request_t* req) {
  cbor_item_t* array = cbor_new_definite_array(2);

  _push_u8(array, CA_SESSIONS_REQUEST);
  _push_u64(array, req->req_id);
  return array;
}

/* [8, req_id, rows] — row = [sid, status, goal, created, depth] */
static cbor_item_t* _encode_sessions_response(
    const ca_sessions_response_t* res) {
  cbor_item_t* array;
  cbor_item_t* rows;

  if (res->nrecords > CA_WIRE_SESSIONS_MAX) return NULL;
  array = cbor_new_definite_array(3);
  _push_u8(array, CA_SESSIONS_RESPONSE);
  _push_u64(array, res->req_id);
  rows = cbor_new_definite_array(res->nrecords);
  for (size_t i = 0; i < res->nrecords; i++) {
    const ca_sessions_record_t* rec = &res->records[i];
    cbor_item_t* row = cbor_new_definite_array(5);

    _push_string(row, rec->sid);
    _push_string(row, rec->status);
    _push_string(row, rec->goal);
    _push_u64(row, rec->created);
    _push_u64(row, (uint64_t)rec->depth);
    _push(rows, row);
  }
  _push(array, rows);
  return array;
}

/* [11, req_id, status, text] */
static cbor_item_t* _encode_error(const ca_error_t* err) {
  cbor_item_t* array = cbor_new_definite_array(4);

  _push_u8(array, CA_ERROR);
  _push_u64(array, err->req_id);
  _push_u8(array, err->status);
  _push_string(array, err->text);
  return array;
}

/* ---- per-type decoders: total-or-refusal — on ANY refusal the
   partially-built payload is destroyed here (never a partial payload, never
   a leak); *payload fills on success only ---- */

static int _decode_sid_element(cbor_item_t* frame, size_t index,
                               size_t max_len, char** out, int require) {
  cbor_item_t* item = cbor_array_get(frame, index);
  size_t len;

  if (item == NULL) return -1;
  if (!cbor_isa_string(item)) {
    cbor_decref(&item);
    return -1;
  }
  len = cbor_string_length(item);
  if (len > max_len) {
    cbor_decref(&item);
    return -1;
  }
  if (len == 0) {
    cbor_decref(&item);
    if (require) return -1;
    *out = NULL;   /* the "" sentinel decodes as absent */
    return 0;
  }
  *out = get_memory(len + 1);
  memcpy(*out, cbor_string_handle(item), len);
  (*out)[len] = '\0';
  cbor_decref(&item);
  return 0;
}

/* [1, req_id, text] | [1, req_id, sid, text]; sid "" decodes as absent */
static int _decode_prompt_request(cbor_item_t* frame, void** payload) {
  size_t size = cbor_array_size(frame);
  ca_prompt_request_t* req;
  cbor_item_t* item;
  int rc;

  if (size != 3 && size != 4) return -1;
  req = get_clear_memory(sizeof(*req));
  if (size == 4) {
    rc = _decode_sid_element(frame, 2, CA_WIRE_SID_MAX, &req->sid, 0);
    if (rc != 0) {
      ca_wire_payload_destroy(CA_PROMPT_REQUEST, req);
      return -1;
    }
  }
  item = cbor_array_get(frame, size == 3 ? 2 : 3);
  rc = item == NULL ? -1 : 0;
  if (rc == 0) req->text = _decode_string(item, CA_WIRE_TEXT_MAX);
  cbor_decref(&item);
  if (rc != 0 || req->text == NULL) {
    ca_wire_payload_destroy(CA_PROMPT_REQUEST, req);
    return -1;
  }
  *payload = req;
  return 0;
}

/* [2, req_id, status, sid] — sid "" = a steer */
static int _decode_prompt_response(cbor_item_t* frame, void** payload) {
  ca_prompt_response_t* res;
  cbor_item_t* item;
  int rc;

  if (cbor_array_size(frame) != 4) return -1;
  res = get_clear_memory(sizeof(*res));
  item = cbor_array_get(frame, 2);
  rc = item == NULL ? -1 : _decode_u8(item, &res->status);
  cbor_decref(&item);
  if (rc != 0) {
    ca_wire_payload_destroy(CA_PROMPT_RESPONSE, res);
    return -1;
  }
  item = cbor_array_get(frame, 3);
  rc = item == NULL ? -1 : 0;
  if (rc == 0) res->sid = _decode_string_keep_empty(item, CA_WIRE_SID_MAX);
  cbor_decref(&item);
  if (rc != 0 || res->sid == NULL) {
    ca_wire_payload_destroy(CA_PROMPT_RESPONSE, res);
    return -1;
  }
  *payload = res;
  return 0;
}

/* [3, req_id, sid, op, from_seq] */
static int _decode_events_request(cbor_item_t* frame, void** payload) {
  ca_events_request_t* req;
  cbor_item_t* item;
  int rc;

  if (cbor_array_size(frame) != 5) return -1;
  req = get_clear_memory(sizeof(*req));
  req->sid = NULL;
  rc = _decode_sid_element(frame, 2, CA_WIRE_SID_MAX, &req->sid, 1);
  if (rc != 0) {
    ca_wire_payload_destroy(CA_EVENTS_REQUEST, req);
    return -1;
  }
  item = cbor_array_get(frame, 3);
  rc = item == NULL ? -1 : _decode_u8(item, &req->op);
  cbor_decref(&item);
  if (rc != 0 || req->op > CA_EVENTS_UNSUBSCRIBE) {
    ca_wire_payload_destroy(CA_EVENTS_REQUEST, req);
    return -1;
  }
  item = cbor_array_get(frame, 4);
  rc = item == NULL ? -1 : _decode_u64(item, &req->from_seq);
  cbor_decref(&item);
  if (rc != 0) {
    ca_wire_payload_destroy(CA_EVENTS_REQUEST, req);
    return -1;
  }
  *payload = req;
  return 0;
}

/* [4, req_id, sid, seq, op, record_json] — the live marker (seq 0) carries
   no record; a record frame (seq != 0) MUST carry one */
static int _decode_events_response(cbor_item_t* frame, void** payload) {
  ca_events_response_t* res;
  cbor_item_t* item;
  int rc;

  if (cbor_array_size(frame) != 6) return -1;
  res = get_clear_memory(sizeof(*res));
  rc = _decode_sid_element(frame, 2, CA_WIRE_SID_MAX, &res->sid, 1);
  if (rc != 0) {
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, res);
    return -1;
  }
  item = cbor_array_get(frame, 3);
  rc = item == NULL ? -1 : _decode_u64(item, &res->seq);
  cbor_decref(&item);
  if (rc != 0) {
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, res);
    return -1;
  }
  item = cbor_array_get(frame, 4);
  rc = item == NULL ? -1 : _decode_u8(item, &res->op);
  cbor_decref(&item);
  if (rc != 0 || res->op > CA_EVENTS_UNSUBSCRIBE) {
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, res);
    return -1;
  }
  item = cbor_array_get(frame, 5);
  rc = item == NULL ? -1 : 0;
  if (rc == 0) res->record_json = _decode_string(item, CA_WIRE_RECORD_MAX);
  cbor_decref(&item);
  if (rc != 0 || ((res->seq == 0) != (res->record_json == NULL))) {
    ca_wire_payload_destroy(CA_EVENTS_RESPONSE, res);
    return -1;
  }
  *payload = res;
  return 0;
}

/* [5, req_id, sid] */
static int _decode_interrupt_request(cbor_item_t* frame, void** payload) {
  ca_interrupt_request_t* req;
  int rc;

  if (cbor_array_size(frame) != 3) return -1;
  req = get_clear_memory(sizeof(*req));
  req->sid = NULL;
  rc = _decode_sid_element(frame, 2, CA_WIRE_SID_MAX, &req->sid, 1);
  if (rc != 0) {
    ca_wire_payload_destroy(CA_INTERRUPT_REQUEST, req);
    return -1;
  }
  *payload = req;
  return 0;
}

/* [6, req_id, status] */
static int _decode_interrupt_response(cbor_item_t* frame, void** payload) {
  ca_interrupt_response_t* res;
  cbor_item_t* item;
  int rc;

  if (cbor_array_size(frame) != 3) return -1;
  res = get_clear_memory(sizeof(*res));
  item = cbor_array_get(frame, 2);
  rc = item == NULL ? -1 : _decode_u8(item, &res->status);
  cbor_decref(&item);
  if (rc != 0) {
    ca_wire_payload_destroy(CA_INTERRUPT_RESPONSE, res);
    return -1;
  }
  *payload = res;
  return 0;
}

/* [7, req_id] */
static int _decode_sessions_request(cbor_item_t* frame, void** payload) {
  ca_sessions_request_t* req;

  if (cbor_array_size(frame) != 2) return -1;
  req = get_clear_memory(sizeof(*req));
  *payload = req;
  return 0;
}

/* [8, req_id, rows] — row = [sid, status, goal, created, depth] */
static int _decode_sessions_response(cbor_item_t* frame, void** payload) {
  ca_sessions_response_t* res;
  cbor_item_t* rows;
  size_t nrows;

  if (cbor_array_size(frame) != 3) return -1;
  rows = cbor_array_get(frame, 2);
  if (rows == NULL || !cbor_isa_array(rows)) {
    cbor_decref(&rows);
    return -1;
  }
  nrows = cbor_array_size(rows);
  if (nrows > CA_WIRE_SESSIONS_MAX) {
    cbor_decref(&rows);
    return -1;
  }
  res = get_clear_memory(sizeof(*res));
  res->nrecords = nrows;
  if (nrows > 0) {
    res->records = get_clear_memory(sizeof(ca_sessions_record_t) * nrows);
  }
  for (size_t i = 0; i < nrows; i++) {
    ca_sessions_record_t* rec = &res->records[i];
    cbor_item_t* row = cbor_array_get(rows, i);
    cbor_item_t* item;
    uint64_t created = 0;
    uint64_t depth = 0;
    int rc;

    if (row == NULL || !cbor_isa_array(row) || cbor_array_size(row) != 5) {
      cbor_decref(&row);
      cbor_decref(&rows);
      ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, res);
      return -1;
    }
    rc = _decode_sid_element(row, 0, CA_WIRE_SID_MAX, &rec->sid, 1);
    if (rc != 0) {
      cbor_decref(&row);
      cbor_decref(&rows);
      ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, res);
      return -1;
    }
    item = cbor_array_get(row, 1);
    rc = item == NULL ? -1 : 0;
    if (rc == 0) rec->status = _decode_string(item, CA_WIRE_STATUS_MAX);
    cbor_decref(&item);
    item = cbor_array_get(row, 2);
    rc = rc != 0 ? rc : (item == NULL ? -1 : 0);
    if (rc == 0) rec->goal = _decode_string(item, CA_WIRE_TEXT_MAX);
    cbor_decref(&item);
    item = cbor_array_get(row, 3);
    rc = rc != 0 ? rc : (item == NULL ? -1 : _decode_u64(item, &created));
    cbor_decref(&item);
    item = cbor_array_get(row, 4);
    rc = rc != 0 ? rc : (item == NULL ? -1 : _decode_u64(item, &depth));
    cbor_decref(&item);
    cbor_decref(&row);
    if (rc != 0 || depth > SIZE_MAX) {
      cbor_decref(&rows);
      ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, res);
      return -1;
    }
    rec->created = created;
    rec->depth = (size_t)depth;
  }
  cbor_decref(&rows);
  *payload = res;
  return 0;
}

/* [11, req_id, status, text] */
static int _decode_error(cbor_item_t* frame, void** payload) {
  ca_error_t* err;
  cbor_item_t* item;
  int rc;

  if (cbor_array_size(frame) != 4) return -1;
  err = get_clear_memory(sizeof(*err));
  item = cbor_array_get(frame, 2);
  rc = item == NULL ? -1 : _decode_u8(item, &err->status);
  cbor_decref(&item);
  if (rc != 0) {
    ca_wire_payload_destroy(CA_ERROR, err);
    return -1;
  }
  item = cbor_array_get(frame, 3);
  rc = item == NULL ? -1 : 0;
  if (rc == 0) err->text = _decode_string(item, CA_WIRE_TEXT_MAX);
  cbor_decref(&item);
  if (rc != 0 || err->text == NULL) {
    ca_wire_payload_destroy(CA_ERROR, err);
    return -1;
  }
  *payload = err;
  return 0;
}

static int _decode_frame(cbor_item_t* frame, uint64_t* type, void** payload,
                         uint64_t* req_id, uint8_t* status) {
  cbor_item_t* item;
  uint64_t wire_type = 0;
  int rc;

  if (!cbor_isa_array(frame) || cbor_array_size(frame) < 2) return -1;

  item = cbor_array_get(frame, 0);
  rc = item == NULL ? -1 : _decode_u64(item, &wire_type);
  cbor_decref(&item);
  if (rc != 0) return -1;

  item = cbor_array_get(frame, 1);
  rc = item == NULL ? -1 : _decode_u64(item, req_id);
  cbor_decref(&item);
  if (rc != 0) return -1;

  switch (wire_type) {
    case CA_PROMPT_REQUEST:
      rc = _decode_prompt_request(frame, payload);
      if (rc == 0) *status = 0;   /* a request carries no status */
      break;
    case CA_PROMPT_RESPONSE:
      rc = _decode_prompt_response(frame, payload);
      if (rc == 0) *status = ((ca_prompt_response_t*)*payload)->status;
      break;
    case CA_EVENTS_REQUEST:
      rc = _decode_events_request(frame, payload);
      if (rc == 0) *status = 0;
      break;
    case CA_EVENTS_RESPONSE:
      rc = _decode_events_response(frame, payload);
      if (rc == 0) *status = ((ca_events_response_t*)*payload)->op;
      break;
    case CA_INTERRUPT_REQUEST:
      rc = _decode_interrupt_request(frame, payload);
      if (rc == 0) *status = 0;
      break;
    case CA_INTERRUPT_RESPONSE:
      rc = _decode_interrupt_response(frame, payload);
      if (rc == 0) *status = ((ca_interrupt_response_t*)*payload)->status;
      break;
    case CA_SESSIONS_REQUEST:
      rc = _decode_sessions_request(frame, payload);
      if (rc == 0) *status = 0;
      break;
    case CA_SESSIONS_RESPONSE:
      rc = _decode_sessions_response(frame, payload);
      if (rc == 0) *status = 0;
      break;
    case CA_ERROR:
      rc = _decode_error(frame, payload);
      if (rc == 0) *status = ((ca_error_t*)*payload)->status;
      break;
    default:
      return -1;   /* the closed vocabulary: an unknown type refuses loud */
  }
  if (rc != 0) return -1;
  *type = wire_type;
  return 0;
}

/* ---- the surface ---- */

int ca_wire_encode(uint64_t type, void* payload, uint8_t** out,
                   size_t* out_len) {
  cbor_item_t* frame = NULL;
  unsigned char* buf = NULL;
  size_t buf_len = 0;
  size_t n;

  if (out == NULL || out_len == NULL || payload == NULL) return -1;
  switch (type) {
    case CA_PROMPT_REQUEST:
      frame = _encode_prompt_request((const ca_prompt_request_t*)payload);
      break;
    case CA_PROMPT_RESPONSE:
      frame = _encode_prompt_response((const ca_prompt_response_t*)payload);
      break;
    case CA_EVENTS_REQUEST:
      frame = _encode_events_request((const ca_events_request_t*)payload);
      break;
    case CA_EVENTS_RESPONSE:
      frame = _encode_events_response((const ca_events_response_t*)payload);
      break;
    case CA_INTERRUPT_REQUEST:
      frame =
          _encode_interrupt_request((const ca_interrupt_request_t*)payload);
      break;
    case CA_INTERRUPT_RESPONSE:
      frame =
          _encode_interrupt_response((const ca_interrupt_response_t*)payload);
      break;
    case CA_SESSIONS_REQUEST:
      frame = _encode_sessions_request((const ca_sessions_request_t*)payload);
      break;
    case CA_SESSIONS_RESPONSE:
      frame =
          _encode_sessions_response((const ca_sessions_response_t*)payload);
      break;
    case CA_ERROR:
      frame = _encode_error((const ca_error_t*)payload);
      break;
    default:
      return -1;   /* the closed vocabulary: an unknown type refuses loud */
  }
  if (frame == NULL) return -1;
  n = cbor_serialize_alloc(frame, &buf, &buf_len);
  cbor_decref(&frame);
  if (n == 0 || buf == NULL || buf_len == 0) {
    free(buf);
    return -1;
  }
  *out = (uint8_t*)buf;
  *out_len = n;
  return 0;
}

int ca_wire_decode_bytes(const uint8_t* raw, size_t raw_len, uint64_t* type,
                         void** payload, uint64_t* req_id, uint8_t* status) {
  struct cbor_load_result load_result;
  cbor_item_t* frame;
  int rc;

  if (raw == NULL || raw_len == 0 || type == NULL || payload == NULL ||
      req_id == NULL || status == NULL) {
    return -1;
  }
  frame = cbor_load(raw, raw_len, &load_result);
  if (frame == NULL || load_result.error.code != CBOR_ERR_NONE) {
    /* a malformed payload: cbor_load failure IS the refusal (this vendored
       cbor_decref has no NULL guard — liboffs's error path guards too) */
    if (frame != NULL) cbor_decref(&frame);
    return -1;
  }
  rc = _decode_frame(frame, type, payload, req_id, status);
  cbor_decref(&frame);
  return rc;
}

int ca_wire_decode(cbor_item_t* frame, uint64_t* type, void** payload,
                   uint64_t* req_id, uint8_t* status) {
  if (frame == NULL || type == NULL || payload == NULL || req_id == NULL ||
      status == NULL) {
    return -1;
  }
  return _decode_frame(frame, type, payload, req_id, status);
}

void ca_wire_payload_destroy(uint64_t type, void* payload) {
  if (payload == NULL) return;
  switch (type) {
    case CA_PROMPT_REQUEST: {
      ca_prompt_request_t* req = (ca_prompt_request_t*)payload;
      free(req->sid);
      free(req->text);
      free(req);
      break;
    }
    case CA_PROMPT_RESPONSE: {
      ca_prompt_response_t* res = (ca_prompt_response_t*)payload;
      free(res->sid);
      free(res);
      break;
    }
    case CA_EVENTS_REQUEST: {
      ca_events_request_t* req = (ca_events_request_t*)payload;
      free(req->sid);
      free(req);
      break;
    }
    case CA_EVENTS_RESPONSE: {
      ca_events_response_t* res = (ca_events_response_t*)payload;
      free(res->sid);
      free(res->record_json);
      free(res);
      break;
    }
    case CA_INTERRUPT_REQUEST: {
      ca_interrupt_request_t* req = (ca_interrupt_request_t*)payload;
      free(req->sid);
      free(req);
      break;
    }
    case CA_INTERRUPT_RESPONSE: {
      free(payload);
      break;
    }
    case CA_SESSIONS_REQUEST: {
      free(payload);
      break;
    }
    case CA_SESSIONS_RESPONSE: {
      ca_sessions_response_t* res = (ca_sessions_response_t*)payload;
      for (size_t i = 0; i < res->nrecords; i++) {
        free(res->records[i].sid);
        free(res->records[i].status);
        free(res->records[i].goal);
      }
      free(res->records);
      free(res);
      break;
    }
    case CA_ERROR: {
      ca_error_t* err = (ca_error_t*)payload;
      free(err->text);
      free(err);
      break;
    }
    default:
      break;   /* an unknown type's payload is unknown memory — not ours */
  }
}

#endif /* SA_HAS_WDB */