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
  char text_buf[] = "make the thing";
  req.req_id = 7;
  req.sid = NULL;                 /* absent = create+start */
  req.text = text_buf;
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  int rc = ca_wire_encode(CA_PROMPT_REQUEST, &req, &raw, &raw_len);
  ASSERT_EQ(rc, 0);
  ASSERT_NE(raw, nullptr);

  uint64_t type = 0;
  ca_prompt_request_t* back = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  rc = ca_wire_decode_bytes(raw, raw_len, &type, (void**)&back, &req_id,
                            &status);
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
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  uint8_t* raw = NULL;
  size_t raw_len = 0;

  /* the accepted start: every element round-trips */
  ca_prompt_response_t res = {0};
  char sid_buf[] = "sessions/abc123";
  res.req_id = 7;
  res.status = 0;
  res.sid = sid_buf;
  ASSERT_EQ(ca_wire_encode(CA_PROMPT_RESPONSE, &res, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_PROMPT_RESPONSE) << "the response = request + 1";
  EXPECT_EQ(req_id, 7u);
  EXPECT_EQ(status, 0u);
  ASSERT_NE(payload, nullptr);
  EXPECT_STREQ(((ca_prompt_response_t*)payload)->sid, "sessions/abc123");
  ca_wire_payload_destroy(CA_PROMPT_RESPONSE, payload);
  payload = NULL;
  free(raw);

  /* the refusal status (a server-side decline is still a well-formed
     response frame — it DECODES, the caller reads the refusal off status) */
  res.req_id = 9;
  res.status = 1;
  res.sid = (char*)"sessions/x";
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_PROMPT_RESPONSE, &res, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_PROMPT_RESPONSE);
  EXPECT_EQ(req_id, 9u);
  EXPECT_EQ(status, 1u);
  ASSERT_NE(payload, nullptr);
  EXPECT_STREQ(((ca_prompt_response_t*)payload)->sid, "sessions/x");
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
  req.text = big.data();   /* C++17: data() is a mutable char* */
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_PROMPT_REQUEST, &req, &raw, &raw_len), 0);
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  EXPECT_NE(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  free(raw);
}

TEST(TestClientApiWire, TestUnknownTypeAndGarbageAnswerError) {
  uint8_t garbage[] = {0xFFu, 0x01u, 0x02u, 0x03u};
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  EXPECT_NE(ca_wire_decode_bytes(garbage, sizeof(garbage), &type, &payload,
                                 &req_id, &status), 0)
      << "malformed CBOR is a decode refusal (the caller sends CA_ERROR)";
  ca_sessions_request_t sreq = {0};
  sreq.req_id = 2;
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_SESSIONS_REQUEST, &sreq, &raw, &raw_len), 0);
  /* decode a SESSIONS frame AS IF it were a PROMPT: the type switch owns the
     cast — decoding a type against the wrong payload struct is the caller's
     switch, not the wire's problem; pin the type IS readable though */
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_SESSIONS_REQUEST);
  ca_wire_payload_destroy(CA_SESSIONS_REQUEST, payload);
  free(raw);
}

TEST(TestClientApiWire, TestEventsLiveMarkerEchoesOp) {
  /* the live marker: seq 0, the op echoed (in the struct AND the decode's
     status out), and no record */
  ca_events_response_t marker = {0};
  char sid_buf[] = "sessions/abc123";
  marker.req_id = 9;
  marker.sid = sid_buf;
  marker.seq = 0;                    /* the LIVE-TRANSITION signal */
  marker.op = CA_EVENTS_LIVE_ONLY;
  marker.record_json = NULL;
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_EVENTS_RESPONSE, &marker, &raw, &raw_len), 0);
  uint64_t type = 0;
  ca_events_response_t* back = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, (void**)&back, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_EVENTS_RESPONSE);
  EXPECT_EQ(req_id, 9u);
  EXPECT_EQ(back->seq, 0u);
  EXPECT_EQ(back->op, (uint8_t)CA_EVENTS_LIVE_ONLY);
  EXPECT_EQ(back->record_json, nullptr);
  EXPECT_EQ(status, (uint8_t)CA_EVENTS_LIVE_ONLY) << "the marker's op echo";
  ca_wire_payload_destroy(CA_EVENTS_RESPONSE, back);
  free(raw);

  /* a record frame carries its record and echoes its op the same way */
  char json_buf[] = "{\"cell\":\"remember\"}";
  ca_events_response_t rec = {0};
  rec.req_id = 10;
  rec.sid = sid_buf;
  rec.seq = 41;
  rec.op = CA_EVENTS_REPLAY_THEN_LIVE;
  rec.record_json = json_buf;
  ASSERT_EQ(ca_wire_encode(CA_EVENTS_RESPONSE, &rec, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, (void**)&back, &req_id,
                                 &status), 0);
  EXPECT_EQ(back->seq, 41u);
  EXPECT_STREQ(back->record_json, "{\"cell\":\"remember\"}");
  EXPECT_EQ(status, (uint8_t)CA_EVENTS_REPLAY_THEN_LIVE);
  ca_wire_payload_destroy(CA_EVENTS_RESPONSE, back);
  free(raw);

  /* seq 0 carrying a record is the marker shape violated — loud */
  rec.seq = 0;
  ASSERT_EQ(ca_wire_encode(CA_EVENTS_RESPONSE, &rec, &raw, &raw_len), 0);
  EXPECT_NE(ca_wire_decode_bytes(raw, raw_len, &type, (void**)&back, &req_id,
                                 &status), 0);
  free(raw);
}

TEST(TestClientApiWire, TestSessionsResponseRowsAndError) {
  /* the sessions response's rows are {sid, status, goal, created, depth} */
  ca_sessions_response_t res = {0};
  res.req_id = 12;
  res.nrecords = 1;
  res.records = (ca_sessions_record_t*)calloc(1, sizeof(ca_sessions_record_t));
  ASSERT_NE(res.records, nullptr);
  res.records[0].sid = strdup("sessions/abc123");
  res.records[0].status = strdup("running");
  res.records[0].goal = strdup("the goal");
  res.records[0].created = 42;
  res.records[0].depth = 3;
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_SESSIONS_RESPONSE, &res, &raw, &raw_len), 0);
  free(res.records[0].sid);
  free(res.records[0].status);
  free(res.records[0].goal);
  free(res.records);

  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_SESSIONS_RESPONSE);
  EXPECT_EQ(req_id, 12u);
  ca_sessions_response_t* back = (ca_sessions_response_t*)payload;
  ASSERT_EQ(back->nrecords, 1u);
  EXPECT_STREQ(back->records[0].sid, "sessions/abc123");
  EXPECT_STREQ(back->records[0].status, "running");
  EXPECT_STREQ(back->records[0].goal, "the goal");
  EXPECT_EQ(back->records[0].created, 42u);
  EXPECT_EQ(back->records[0].depth, 3u);
  EXPECT_EQ(status, 0u);
  ca_wire_payload_destroy(CA_SESSIONS_RESPONSE, payload);
  free(raw);

  /* the error frame: the standing refusal answer, its status + text echo */
  ca_error_t err = {0};
  char text_buf[] = "frame unknown";
  err.req_id = 13;
  err.status = 2;
  err.text = text_buf;
  ASSERT_EQ(ca_wire_encode(CA_ERROR, &err, &raw, &raw_len), 0);
  status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_ERROR);
  EXPECT_EQ(req_id, 13u);
  EXPECT_EQ(status, 2u);
  EXPECT_STREQ(((ca_error_t*)payload)->text, "frame unknown");
  ca_wire_payload_destroy(CA_ERROR, payload);
  free(raw);
}

TEST(TestClientApiWire, TestInterruptRequestResponseRoundTrip) {
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;

  /* the interrupt request: [5, req_id, sid] round-trips its id + sid */
  ca_interrupt_request_t req = {0};
  char sid_buf[] = "sessions/xyz";
  req.req_id = 77;
  req.sid = sid_buf;
  ASSERT_EQ(ca_wire_encode(CA_INTERRUPT_REQUEST, &req, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_INTERRUPT_REQUEST);
  EXPECT_EQ(req_id, 77u);
  EXPECT_EQ(status, 0u) << "a request decodes with status 0";
  ASSERT_NE(payload, nullptr);
  EXPECT_STREQ(((ca_interrupt_request_t*)payload)->sid, "sessions/xyz");
  ca_wire_payload_destroy(CA_INTERRUPT_REQUEST, payload);
  payload = NULL;
  free(raw);

  /* the interrupt response: [6, req_id, status] echoes the pairing id */
  ca_interrupt_response_t res = {0};
  res.req_id = 77;
  res.status = 0;
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_INTERRUPT_RESPONSE, &res, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_INTERRUPT_RESPONSE)
      << "the response = request + 1";
  EXPECT_EQ(req_id, 77u);
  EXPECT_EQ(status, 0u);
  ca_wire_payload_destroy(CA_INTERRUPT_RESPONSE, payload);
  free(raw);
}

TEST(TestClientApiWire, TestEventsRequestRoundTripAndSteerForm) {
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;

  /* the events request: [3, req_id, sid, op, from_seq] pins every element */
  ca_events_request_t req = {0};
  char sid_buf[] = "sessions/s";
  req.req_id = 5;
  req.sid = sid_buf;
  req.op = CA_EVENTS_LIVE_ONLY;
  req.from_seq = 42;
  ASSERT_EQ(ca_wire_encode(CA_EVENTS_REQUEST, &req, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_EVENTS_REQUEST);
  EXPECT_EQ(req_id, 5u);
  EXPECT_EQ(status, 0u) << "a request decodes with status 0";
  ASSERT_NE(payload, nullptr);
  ca_events_request_t* back = (ca_events_request_t*)payload;
  EXPECT_STREQ(back->sid, "sessions/s");
  EXPECT_EQ(back->op, (uint8_t)CA_EVENTS_LIVE_ONLY);
  EXPECT_EQ(back->from_seq, 42u);
  ca_wire_payload_destroy(CA_EVENTS_REQUEST, payload);
  payload = NULL;
  free(raw);

  /* the prompt steer form: the 4-element request with a PRESENT sid
     round-trips that sid */
  ca_prompt_request_t steer = {0};
  steer.req_id = 6;
  steer.sid = (char*)"sessions/s";
  steer.text = (char*)"steer the frame";
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_PROMPT_REQUEST, &steer, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  ASSERT_NE(payload, nullptr);
  EXPECT_STREQ(((ca_prompt_request_t*)payload)->sid, "sessions/s");
  ca_wire_payload_destroy(CA_PROMPT_REQUEST, payload);
  payload = NULL;
  free(raw);

  /* the ""-sid form: the wire collapses "" and absent FOR REQUESTS (the
     deliberate asymmetry — responses keep "" via _decode_string_keep_empty;
     _decode_sid_element maps the "" sentinel to NULL here), so a 4-element
     steer with an empty sid arrives as the 3-element create+start shape */
  steer.sid = (char*)"";
  steer.text = (char*)"start a top frame";
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_PROMPT_REQUEST, &steer, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_PROMPT_REQUEST);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(((ca_prompt_request_t*)payload)->sid, nullptr);
  EXPECT_STREQ(((ca_prompt_request_t*)payload)->text, "start a top frame");
  ca_wire_payload_destroy(CA_PROMPT_REQUEST, payload);
  free(raw);
}

/* Build a well-formed CBOR array whose FIRST element is an integer outside
   the closed vocabulary ([type, req_id, filler]) — the default branch's
   refusal shape. */
static void _encode_off_vocabulary_frame(uint64_t wire_type, uint8_t** out,
                                         size_t* out_len) {
  cbor_item_t* array = cbor_new_definite_array(3);
  cbor_item_t* elements[3] = {cbor_build_uint64(wire_type),
                              cbor_build_uint64(77),
                              cbor_build_uint64(1)};
  for (size_t i = 0; i < 3; i++) {
    (void)cbor_array_push(array, elements[i]);
    cbor_decref(&elements[i]);
  }
  size_t n = cbor_serialize_alloc(array, (unsigned char**)out, out_len);
  cbor_decref(&array);
  ASSERT_NE(n, 0u) << "the hand-shaped frame must serialize";
  ASSERT_NE(*out, nullptr);
}

TEST(TestClientApiWire, TestUnknownWellFormedTypeRefuses) {
  uint64_t off_vocabulary[] = {9, 99};
  for (size_t i = 0; i < sizeof(off_vocabulary) / sizeof(off_vocabulary[0]);
       i++) {
    uint8_t* raw = NULL;
    size_t raw_len = 0;
    _encode_off_vocabulary_frame(off_vocabulary[i], &raw, &raw_len);

    /* the refusal, AND the contract that pays for it: the req_id element
       decoded BEFORE the type dispatch, so a refusal still leaves the
       request's id on the out-param — the CA_ERROR echo's raw material.
       type/status/payload are untouched (their sentinel pre-call values
       hold; a NULL-init'd payload stays NULL). */
    uint64_t type = 12345u;
    void* payload = (void*)(uintptr_t)0x1u;
    uint64_t req_id = 0;
    uint8_t status = 7;
    EXPECT_NE(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                   &status), 0)
        << "the closed vocabulary refuses an off-vocabulary type loud";
    EXPECT_EQ(type, 12345u);
    EXPECT_EQ(status, 7u);
    EXPECT_EQ(payload, (void*)(uintptr_t)0x1u);
    EXPECT_EQ(req_id, 77u);
    free(raw);
  }
}
/* The AUTH pair (Task 6 / the TCP transport's first exchange): [12, req_id,
   api_key] and its [13, req_id, status] response — the same req_id duality
   every other request carries, the same pairing rule (13 = 12 + 1, the
   compile-time assert), and bcrypt-key field bounds. */
TEST(TestClientApiWire, TestAuthPairRoundTrip) {
  /* the request: a presented key round-trips; a request's status out is 0 */
  ca_auth_request_t req = {0};
  char key_buf[] = "a-presented-key";
  req.req_id = 21;
  req.api_key = key_buf;
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_AUTH_REQUEST, &req, &raw, &raw_len), 0);

  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_AUTH_REQUEST);
  EXPECT_EQ(req_id, 21u);
  EXPECT_EQ(status, 0u) << "a request carries no status";
  ca_auth_request_t* back = (ca_auth_request_t*)payload;
  ASSERT_NE(back, nullptr);
  EXPECT_STREQ(back->api_key, "a-presented-key");
  ca_wire_payload_destroy(CA_AUTH_REQUEST, back);
  free(raw);

  /* the response: status 1 = bad key (the connection closes after it); the
     response's status rides the decode's status out like every response's */
  ca_auth_response_t res = {0};
  res.req_id = 21;
  res.status = 1;
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_AUTH_RESPONSE, &res, &raw, &raw_len), 0);
  type = 0;
  payload = NULL;
  req_id = 0;
  status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_AUTH_RESPONSE);
  EXPECT_EQ(req_id, 21u);
  EXPECT_EQ(status, 1u);
  ca_auth_response_t* res_back = (ca_auth_response_t*)payload;
  ASSERT_NE(res_back, nullptr);
  EXPECT_EQ(res_back->status, 1u);
  ca_wire_payload_destroy(CA_AUTH_RESPONSE, res_back);
  free(raw);

  /* the bounds: an EMPTY key refuses ("" is the absent sentinel on this
     wire and an auth request's key is never absent) */
  ca_auth_request_t absent = {0};
  absent.req_id = 4;
  char empty_buf[1] = "";
  absent.api_key = empty_buf;
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_AUTH_REQUEST, &absent, &raw, &raw_len), 0);
  type = 0;
  payload = NULL;
  req_id = 0;
  status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw);

  /* the bounds: an over-bound key refuses loud */
  std::string big(CA_WIRE_KEY_MAX + 1, 'k');
  ca_auth_request_t oversized = {0};
  oversized.req_id = 5;
  oversized.api_key = &big[0];
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_AUTH_REQUEST, &oversized, &raw, &raw_len), 0);
  type = 0;
  payload = NULL;
  req_id = 0;
  status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw);
}
