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
  ca_prompt_response_t res = {0};
  char sid_buf[] = "sessions/abc123";
  res.req_id = 7;
  res.status = 0;
  res.sid = sid_buf;
  uint8_t* raw = NULL;
  size_t raw_len = 0;
  ASSERT_EQ(ca_wire_encode(CA_PROMPT_RESPONSE, &res, &raw, &raw_len), 0);
  uint64_t type = 0;
  void* payload = NULL;
  uint64_t req_id = 0;
  uint8_t status = 0;
  ASSERT_EQ(ca_wire_decode_bytes(raw, raw_len, &type, &payload, &req_id,
                                 &status), 0);
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