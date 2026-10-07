//
// Created by victor on 10/03/26.
//

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>
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

/* A hand-shaped frame [type, req_id, tail...] — the config pair's GET form
   and the bad-size/bad-field refusal shapes need it (nullptr rides a CBOR
   null — the not-a-text field's refusal shape). */
static void _hand_frame(uint64_t type, uint64_t req_id,
                        const std::vector<const char*>& tail, uint8_t** out,
                        size_t* out_len) {
  std::vector<cbor_item_t*> elements;
  elements.push_back(cbor_build_uint64(type));
  elements.push_back(cbor_build_uint64(req_id));
  for (const char* s : tail) {
    elements.push_back(s == nullptr ? cbor_new_null() : cbor_build_string(s));
  }
  cbor_item_t* array = cbor_new_definite_array(elements.size());
  for (cbor_item_t* e : elements) {
    (void)cbor_array_push(array, e);
    cbor_decref(&e);
  }
  size_t n = cbor_serialize_alloc(array, (unsigned char**)out, out_len);
  cbor_decref(&array);
  ASSERT_NE(n, 0u) << "the hand-shaped frame must serialize";
  ASSERT_NE(*out, nullptr);
}

/* The CONFIG wire pair (the settings surface's pair): [14, req_id,
   base_url, api_key, model] and its [15, req_id, status, base_url,
   api_key, model] — "" is the ABSENT sentinel on every template field
   (all-absent = the GET), the api_key rides the AUTH pair's by-length
   scrub discipline, and the pairing assert joined the vocabulary at
   compile time (14/15 = the ERROR 11 / AUTH 12-13 numbers' next free
   pair). */
TEST(TestClientApiWire, TestConfigRoundTrips) {
  /* GET: {req_id 9} → the response carries base_url/api_key/model strings
     (absent = the "" sentinel, per the wire's bounded-string discipline:
     CA_WIRE_CONFIG_* bounds — TEXT 512 for base_url, KEY 256 for api_key
     mirroring CA_WIRE_KEY_MAX, TAG 128 for the model tag). */
  /* SET: {req_id 9, base_url "http://127.0.0.1:11434", api_key "", tag
     "gemma4:latest"} = the absent-fields-unchanged shape ("" = absent
     everywhere in CONFIG's set) → decodes equal. */
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;

  /* the SET request's encode→decode equality (the "" sentinel rides as the
     empty string and decodes back to the absent NULL) */
  ca_config_request_t req = {0};
  req.req_id = 9;
  req.base_url = (char*)"http://127.0.0.1:11434";
  req.api_key = NULL;   /* absent — the encoder writes the "" sentinel */
  req.model = (char*)"gemma4:latest";
  ASSERT_EQ(ca_wire_encode(CA_CONFIG_REQUEST, &req, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_CONFIG_REQUEST);
  EXPECT_EQ(req_id, 9u);
  EXPECT_EQ(status, 0u) << "a request decodes with status 0";
  ASSERT_NE(payload, nullptr);
  {
    ca_config_request_t* back = (ca_config_request_t*)payload;
    EXPECT_STREQ(back->base_url, "http://127.0.0.1:11434");
    EXPECT_EQ(back->api_key, nullptr) << "the absent sentinel kept absent";
    EXPECT_STREQ(back->model, "gemma4:latest");
    ca_wire_payload_destroy(CA_CONFIG_REQUEST, back);
  }
  free(raw);

  /* the GET by the wire's own encoder: the all-NULL fields ride as all ""
     and decode all-absent */
  ca_config_request_t get = {0};
  get.req_id = 9;
  raw = NULL;
  raw_len = 0;
  payload = NULL;
  ASSERT_EQ(ca_wire_encode(CA_CONFIG_REQUEST, &get, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_CONFIG_REQUEST);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(((ca_config_request_t*)payload)->base_url, nullptr);
  EXPECT_EQ(((ca_config_request_t*)payload)->api_key, nullptr);
  EXPECT_EQ(((ca_config_request_t*)payload)->model, nullptr);
  ca_wire_payload_destroy(CA_CONFIG_REQUEST, payload);
  free(raw);

  /* the GET's 2-element form [14, req_id]: the spec's "{req_id}" shape */
  raw = NULL;
  raw_len = 0;
  payload = NULL;
  _hand_frame(CA_CONFIG_REQUEST, 4, {}, &raw, &raw_len);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_CONFIG_REQUEST);
  EXPECT_EQ(req_id, 4u);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(((ca_config_request_t*)payload)->base_url, nullptr);
  ca_wire_payload_destroy(CA_CONFIG_REQUEST, payload);
  free(raw);

  /* the response: the GET's answer round-trips every template field, the
     key's DECODED length rides for the destroy's scrub, and the response's
     type IS request + 1 (the pairing assert's runtime face) */
  ca_config_response_t res = {0};
  res.req_id = 9;
  res.status = 0;
  res.base_url = (char*)"http://127.0.0.1:11434";
  res.api_key = (char*)"sk-secret";
  res.model = (char*)"gemma4:latest";
  raw = NULL;
  raw_len = 0;
  payload = NULL;
  ASSERT_EQ(ca_wire_encode(CA_CONFIG_RESPONSE, &res, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_CONFIG_RESPONSE)
      << "the response = request + 1";
  EXPECT_EQ(req_id, 9u);
  EXPECT_EQ(status, 0u);
  ASSERT_NE(payload, nullptr);
  {
    ca_config_response_t* back = (ca_config_response_t*)payload;
    EXPECT_EQ(back->status, 0u);
    EXPECT_STREQ(back->base_url, "http://127.0.0.1:11434");
    ASSERT_NE(back->api_key, nullptr);
    EXPECT_STREQ(back->api_key, "sk-secret");
    EXPECT_EQ(back->key_len, strlen("sk-secret")) << "the scrub's count";
    EXPECT_STREQ(back->model, "gemma4:latest");
    ca_wire_payload_destroy(CA_CONFIG_RESPONSE, back);
  }
  free(raw);

  /* the response's absent shape: the all-"" answer decodes all-absent —
     the template-defaults-empty truth a fresh daemon's GET carries */
  ca_config_response_t empty = {0};
  empty.req_id = 15;
  empty.status = 0;
  raw = NULL;
  raw_len = 0;
  payload = NULL;
  ASSERT_EQ(ca_wire_encode(CA_CONFIG_RESPONSE, &empty, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(status, 0u);
  EXPECT_EQ(((ca_config_response_t*)payload)->base_url, nullptr);
  EXPECT_EQ(((ca_config_response_t*)payload)->api_key, nullptr);
  EXPECT_EQ(((ca_config_response_t*)payload)->model, nullptr);
  ca_wire_payload_destroy(CA_CONFIG_RESPONSE, payload);
  free(raw);

  /* the bounds: every template field refuses over its bound loud */
  std::string big_url(CA_WIRE_CONFIG_TEXT_MAX + 1, 'u');
  ca_config_request_t over_url = {0};
  over_url.req_id = 5;
  over_url.base_url = &big_url[0];   /* C++17: data() is a mutable char* */
  raw = NULL;
  raw_len = 0;
  payload = NULL;
  ASSERT_EQ(ca_wire_encode(CA_CONFIG_REQUEST, &over_url, &raw, &raw_len), 0);
  EXPECT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw);

  std::string big_key(CA_WIRE_KEY_MAX + 1, 'k');
  ca_config_request_t over_key = {0};
  over_key.req_id = 6;
  over_key.api_key = &big_key[0];
  raw = NULL;
  raw_len = 0;
  payload = NULL;
  ASSERT_EQ(ca_wire_encode(CA_CONFIG_REQUEST, &over_key, &raw, &raw_len), 0);
  EXPECT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw);

  std::string big_tag(CA_WIRE_CONFIG_TAG_MAX + 1, 't');
  ca_config_request_t over_tag = {0};
  over_tag.req_id = 7;
  over_tag.model = &big_tag[0];
  raw = NULL;
  raw_len = 0;
  payload = NULL;
  ASSERT_EQ(ca_wire_encode(CA_CONFIG_REQUEST, &over_tag, &raw, &raw_len), 0);
  EXPECT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw);

  /* the hand shapes: a 3-element partial-field form refuses, and a CBOR
     null field (not-a-text) refuses — the wire never trusts its peer */
  raw = NULL;
  raw_len = 0;
  payload = NULL;
  _hand_frame(CA_CONFIG_REQUEST, 8, {"only-a-base-url"}, &raw, &raw_len);
  EXPECT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw);

  uint8_t* raw2 = NULL;   /* the ASSERTs inside _hand_frame force two vars */
  size_t raw2_len = 0;
  payload = NULL;
  _hand_frame(CA_CONFIG_REQUEST, 8, {"http://ok", nullptr, "tag"}, &raw2,
              &raw2_len);
  EXPECT_EQ(ca_wire_decode_bytes(raw2, raw2_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw2);
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

/* The ASK_REPLY pair (the escalation slice's reply verb): [16, req_id, sid,
   ask_id, decision, value] and its [17, req_id, delivered] response.
   decision 0 = answer / 1 = reject; the value's "" sentinel decodes absent
   (the reject-with-no-text shape); the ack's delivered flag carries the
   bind/post truth ONLY — the engine's stale-ask drop is events-stream truth
   (the spec §3.1's pinned ack contract). */
TEST(TestClientApiWire, TestAskReplyRoundTrip) {
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;

  /* the answer: every element round-trips */
  ca_ask_reply_request_t req = {0};
  char sid_buf[] = "sessions/abc123";
  req.req_id = 31;
  req.sid = sid_buf;
  req.ask_id = (char*)"a1b2c3d4";
  req.decision = 0;
  req.value = (char*)"approve option 2";
  ASSERT_EQ(ca_wire_encode(CA_ASK_REPLY_REQUEST, &req, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_ASK_REPLY_REQUEST);
  EXPECT_EQ(req_id, 31u);
  EXPECT_EQ(status, 0u) << "a request decodes with status 0";
  ASSERT_NE(payload, nullptr);
  {
    ca_ask_reply_request_t* back = (ca_ask_reply_request_t*)payload;
    EXPECT_STREQ(back->sid, "sessions/abc123");
    EXPECT_STREQ(back->ask_id, "a1b2c3d4");
    EXPECT_EQ(back->decision, 0u);
    EXPECT_STREQ(back->value, "approve option 2");
    ca_wire_payload_destroy(CA_ASK_REPLY_REQUEST, back);
  }
  payload = NULL;
  free(raw);

  /* the reject with no value: value NULL rides the "" sentinel and decodes
     back absent */
  ca_ask_reply_request_t reject = {0};
  reject.req_id = 32;
  reject.sid = (char*)"sessions/abc123";
  reject.ask_id = (char*)"e5f6a7b8";
  reject.decision = 1;
  reject.value = NULL;   /* the encoder writes the "" sentinel */
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_ASK_REPLY_REQUEST, &reject, &raw, &raw_len), 0);
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(((ca_ask_reply_request_t*)payload)->decision, 1u);
  EXPECT_EQ(((ca_ask_reply_request_t*)payload)->value, nullptr)
      << "the empty value decodes absent";
  ca_wire_payload_destroy(CA_ASK_REPLY_REQUEST, payload);
  payload = NULL;
  free(raw);

  /* the bounds: an over-bound ask_id refuses (the minted ids are the 8-hex
     shape; 40 chars is 5x that headroom) */
  std::string big_ask(CA_WIRE_ASK_ID_MAX + 1, 'a');
  ca_ask_reply_request_t over_ask = {0};
  over_ask.req_id = 33;
  over_ask.sid = (char*)"sessions/abc123";
  over_ask.ask_id = &big_ask[0];   /* C++17: data() is a mutable char* */
  over_ask.decision = 0;
  over_ask.value = (char*)"x";
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_ASK_REPLY_REQUEST, &over_ask, &raw, &raw_len), 0);
  status = 0;
  EXPECT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw);

  /* the bounds: a decision > 1 refuses at DECODE (the encoder is the
     trusted side — the out-of-range decision encodes permissively and the
     decode refuses it loud, the same encode→decode-refusal shape every
     bounded field runs) */
  ca_ask_reply_request_t bad_decision = {0};
  bad_decision.req_id = 34;
  bad_decision.sid = (char*)"sessions/abc123";
  bad_decision.ask_id = (char*)"a1b2c3d4";
  bad_decision.decision = 2;
  bad_decision.value = (char*)"x";
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_ASK_REPLY_REQUEST, &bad_decision, &raw,
                           &raw_len), 0);
  payload = NULL;
  EXPECT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw);

  /* the bounds: a value over the wire's text cap refuses at decode — the
     ask's fields refuse oversized INPUT, never silently truncate */
  std::string big_value(CA_WIRE_TEXT_MAX + 1, 'v');
  ca_ask_reply_request_t over_value = {0};
  over_value.req_id = 35;
  over_value.sid = (char*)"sessions/abc123";
  over_value.ask_id = (char*)"a1b2c3d4";
  over_value.decision = 0;
  over_value.value = &big_value[0];
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_ASK_REPLY_REQUEST, &over_value, &raw, &raw_len),
            0);
  payload = NULL;
  EXPECT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw);

  /* the response: [17, req_id, delivered] round-trips the delivered bool
     (1 = the reply entered the frame's mailbox) and the flag rides the
     decode's status out */
  ca_ask_reply_response_t res = {0};
  res.req_id = 31;
  res.delivered = 1;
  raw = NULL;
  raw_len = 0;
  payload = NULL;
  ASSERT_EQ(ca_wire_encode(CA_ASK_REPLY_RESPONSE, &res, &raw, &raw_len), 0);
  status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_ASK_REPLY_RESPONSE)
      << "the response = request + 1";
  EXPECT_EQ(req_id, 31u);
  EXPECT_EQ(status, 1u) << "the delivered flag rides the status out";
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(((ca_ask_reply_response_t*)payload)->delivered, 1u);
  ca_wire_payload_destroy(CA_ASK_REPLY_RESPONSE, payload);
  payload = NULL;
  free(raw);

  res.req_id = 40;
  res.delivered = 0;   /* the bind/post refused — §3.1's ack contract */
  raw = NULL;
  raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_ASK_REPLY_RESPONSE, &res, &raw, &raw_len), 0);
  status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(status, 0u);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(((ca_ask_reply_response_t*)payload)->delivered, 0u);
  ca_wire_payload_destroy(CA_ASK_REPLY_RESPONSE, payload);
  payload = NULL;
  free(raw);

  /* the response-side refusal: a malformed element count refuses loud */
  uint8_t* raw2 = NULL;
  size_t raw2_len = 0;
  payload = NULL;
  _hand_frame(CA_ASK_REPLY_RESPONSE, 3, {"extra"}, &raw2, &raw2_len);
  EXPECT_EQ(ca_wire_decode_bytes(raw2, raw2_len, &type, &payload, &req_id,
                                 &status), -1);
  EXPECT_EQ(payload, nullptr);
  free(raw2);
}

TEST(TestClientApiWire, TestAskReplyPairAssertsExtend) {
  /* the pairing asserts are COMPILE-TIME (the CA_STATIC_ASSERT chain):
     16 = 15 + 1 extends the vocabulary's adjacency and 17 = 16 + 1 pairs
     the response — a renumber that breaks either fails the build. This
     runtime face exercises the pair so the numbers stay honest in the
     binary too. */
  ca_ask_reply_response_t res = {0};
  res.req_id = 3;
  res.delivered = 1;
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_ASK_REPLY_RESPONSE, &res, &raw, &raw_len), 0);
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
  EXPECT_EQ(type, (uint64_t)CA_ASK_REPLY_RESPONSE) << "the response = request + 1";
  EXPECT_EQ(req_id, 3u);
  EXPECT_EQ(status, 1u);
  ASSERT_NE(payload, nullptr);
  ca_wire_payload_destroy(CA_ASK_REPLY_RESPONSE, payload);
  free(raw);
}
