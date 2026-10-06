//
// Created by victor on 9/29/26.
//

/* Direct decode tests for the model client (plan Task 9). These drive
   model_backend_t.complete through the REAL http path against a local fake
   HTTP server (test-side sockets are allowed; the runtime's model client
   rides the async streams client on the process loop). All canned
   OpenAI-shaped bodies; offline. */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include "../src/Frame/frame.h"
#include "../src/Frame/model.h"
#include "../src/Frame/model_internal.h"
#include "../src/Util/json.h"
}

/* Fake server: one-connection TCP thread that reads the request and replies
   with a canned status code + body. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int fake_server_listen(uint16_t* port_out) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, (struct sockaddr*)&addr, &len);
  *port_out = ntohs(addr.sin_port);
  return fd;
}

/* Reads the complete request (headers + Content-Length body) into seen_body,
   replies HTTP <code> with <body>, then closes. */
static void fake_server_run_canned(int listen_fd, int code, const char* body,
                                   std::string* seen_body,
                                   std::atomic<uint8_t>* seen) {
  int client_fd = accept(listen_fd, NULL, NULL);
  if (client_fd < 0) return;

  struct timeval tv = {3, 0};
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char buf[8192];
  size_t total = 0;
  size_t header_end = 0;
  while (header_end == 0) {
    if (total + 1 >= sizeof(buf)) break;
    ssize_t n = recv(client_fd, buf + total, sizeof(buf) - total - 1, 0);
    if (n <= 0) break;
    total += (size_t)n;
    buf[total] = '\0';
    for (size_t i = 0; i + 3 < total; i++) {
      if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
        header_end = i + 4;
        break;
      }
    }
  }
  if (header_end == 0) {
    close(client_fd);
    return;
  }

  /* Drain the request body so the client's send side never stalls. */
  {
    const char* cl = strstr(buf, "Content-Length:");
    if (cl != NULL) {
      size_t body_len = (size_t)strtoul(cl + 15, NULL, 10);
      size_t have = total - header_end;
      while (have < body_len) {
        ssize_t n = recv(client_fd, buf + total, sizeof(buf) - total - 1, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        have += (size_t)n;
      }
    }
  }

  /* Record AFTER the drain, so the request body is in the copy. */
  *seen_body = buf;

  size_t body_len = strlen(body);
  char head[256];
  int n = snprintf(head, sizeof(head),
    "HTTP/1.1 %d Reason\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n"
    "\r\n",
    code, body_len);
  ASSERT_GT(n, 0);
  ASSERT_LT((size_t)n, sizeof(head));
  size_t sent = 0;
  while (sent < (size_t)n) {
    ssize_t w = write(client_fd, head + sent, (size_t)n - sent);
    ASSERT_GT(w, 0);
    sent += (size_t)w;
  }
  sent = 0;
  while (sent < body_len) {
    ssize_t w = write(client_fd, body + sent, body_len - sent);
    ASSERT_GT(w, 0);
    sent += (size_t)w;
  }
  shutdown(client_fd, SHUT_WR);
  seen->store(1);
  close(client_fd);
}

/* The loop's message shape: a user turn in a JSON array. */
static json_value_t* make_messages(void) {
  json_value_t* msgs = json_new_array();
  json_value_t* m = json_new_object();
  json_object_set(m, "role", json_new_string("user"));
  json_object_set(m, "content", json_new_string("compute six times seven"));
  json_array_append(msgs, m);
  return msgs;
}

/* ---------------------------------------------------------------------- */
/* Submit-path tests (plan Task 4): REAL async submit over the loop thread */
/* ---------------------------------------------------------------------- */

/* model.h's sink contract recorded from the test's side: every delivery
   (body/error are OWNED by the sink — recorded then freed; the headers
   capture is BORROWED for the call — recorded then deinit+freed, since this
   sink is the final owner on the REAL http path). wait() blocks a bounded
   wall-clock on a real condition_variable, so a missed delivery fails as a
   timeout, never a hang. */
typedef struct sink_record_t {
  std::mutex m;
  std::condition_variable cv;
  int calls;
  std::thread::id thread_id;   /* which thread fired the sink */
  int status;
  bool has_body;
  std::string body;
  bool has_error;
  std::string error;
  size_t header_count;
} sink_record_t;

static void sink_record(void* ctx, int status, char* body, size_t body_len,
                        char* error, http_headers_t* headers) {
  sink_record_t* r = (sink_record_t*)ctx;
  std::unique_lock<std::mutex> lk(r->m);
  r->calls++;
  r->thread_id = std::this_thread::get_id();
  r->status = status;
  r->has_body = (body != NULL);
  if (body != NULL) r->body.assign(body, body_len);
  r->has_error = (error != NULL);
  if (error != NULL) r->error = error;
  r->header_count = (headers != NULL) ? http_headers_count(headers) : 0;
  lk.unlock();
  r->cv.notify_all();
  /* The sink owns the heap body/error (model.h's contract) and the headers
     capture's deinit/free (the final owner): record, then die — a
     recorded-then-freed delivery never leaks. */
  free(body);
  free(error);
  if (headers != NULL) {
    http_headers_deinit(headers);
    free(headers);
  }
}

/* Bounded (3000 ms per the plan) wait for the n'th delivery. */
static bool sink_wait_calls(sink_record_t* r, int n) {
  std::unique_lock<std::mutex> lk(r->m);
  return r->cv.wait_for(lk, std::chrono::milliseconds(3000), [&] {
           return r->calls >= n;
         });
}

TEST(TestModelDecode, TestToolCallStringArguments) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run_canned, listen_fd, 200,
    "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
    "\"message\":{\"role\":\"assistant\",\"content\":\"\","
    "\"tool_calls\":[{\"type\":\"function\",\"function\":{\"name\":\"execute\","
    "\"arguments\":\"{\\\"code\\\":\\\"print(6*7)\\\"}\"}}]}}]}",
    &seen_body, &seen);

  std::string base_url = "http://127.0.0.1:" + std::to_string(port);
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  json_value_t* msgs = make_messages();
  model_reply_t* reply = NULL;
  char* err = NULL;
  char* raw = NULL;
  int rc = mb->complete(mb, msgs, NULL, &raw, &reply, &err);
  ASSERT_EQ(rc, 0) << (err ? err : "(no error string)");
  ASSERT_NE(reply, nullptr);
  EXPECT_STREQ(reply->content, "");
  ASSERT_NE(reply->tool_code, nullptr);
  EXPECT_STREQ(reply->tool_code, "print(6*7)");
  EXPECT_STREQ(reply->finish_reason, "tool_calls");
  ASSERT_NE(raw, nullptr);
  EXPECT_NE(strstr(raw, "\"choices\""), nullptr);

  /* The request must carry the model, the canned execute tool, tool_choice,
     and the derived messages — on the /v1/chat/completions path. */
  for (int i = 0; i < 5000 && seen.load() == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(seen.load(), 1);
  EXPECT_NE(seen_body.find("POST /v1/chat/completions"), std::string::npos);
  EXPECT_NE(seen_body.find("\"test-model\""), std::string::npos);
  EXPECT_NE(seen_body.find("tool_choice\":\"auto"), std::string::npos);
  EXPECT_NE(seen_body.find("execute"), std::string::npos);
  EXPECT_NE(seen_body.find("compute six times seven"), std::string::npos);

  free(raw);
  free(err);
  model_reply_destroy(reply);
  model_backend_destroy(mb);
  json_value_destroy(msgs);
  server.join();
  close(listen_fd);
}

TEST(TestModelDecode, TestToolCallObjectArguments) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run_canned, listen_fd, 200,
    "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\",\"message\":"
    "{\"role\":\"assistant\",\"tool_calls\":[{\"type\":\"function\",\"functio"
    "n\":{\"name\":\"execute\",\"arguments\":{\"code\":\"import actor\"}}}]}}"
    "]}",
    &seen_body, &seen);

  /* Trailing slash must be trimmed by the URL join. */
  std::string base_url = "http://127.0.0.1:" + std::to_string(port) + "/";
  frame_config_t cfg = {base_url.c_str(), "sk-test", "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  json_value_t* msgs = make_messages();
  model_reply_t* reply = NULL;
  char* err = NULL;
  char* raw = NULL;
  int rc = mb->complete(mb, msgs, NULL, &raw, &reply, &err);
  ASSERT_EQ(rc, 0) << (err ? err : "(no error string)");
  ASSERT_NE(reply, nullptr);
  EXPECT_STREQ(reply->content, "");
  ASSERT_NE(reply->tool_code, nullptr);
  EXPECT_STREQ(reply->tool_code, "import actor");
  EXPECT_STREQ(reply->finish_reason, "tool_calls");

  for (int i = 0; i < 5000 && seen.load() == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(seen.load(), 1);
  /* No doubled slash after trimming; the bearer key reaches the wire. */
  EXPECT_NE(seen_body.find("POST /v1/chat/completions"), std::string::npos);
  EXPECT_EQ(seen_body.find("//"), std::string::npos);
  EXPECT_NE(seen_body.find("Authorization: Bearer sk-test"), std::string::npos);

  free(raw);
  free(err);
  model_reply_destroy(reply);
  model_backend_destroy(mb);
  json_value_destroy(msgs);
  server.join();
  close(listen_fd);
}

TEST(TestModelDecode, TestNoToolCallContentOnly) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run_canned, listen_fd, 200,
    "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
    "\"message\":{\"role\":\"assistant\",\"content\":\"all done\"}}]}",
    &seen_body, &seen);

  std::string base_url = "http://127.0.0.1:" + std::to_string(port);
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  json_value_t* msgs = make_messages();
  model_reply_t* reply = NULL;
  char* err = NULL;
  char* raw = NULL;
  int rc = mb->complete(mb, msgs, NULL, &raw, &reply, &err);
  ASSERT_EQ(rc, 0) << (err ? err : "(no error string)");
  ASSERT_NE(reply, nullptr);
  EXPECT_STREQ(reply->content, "all done");
  EXPECT_EQ(reply->tool_code, nullptr);
  EXPECT_STREQ(reply->finish_reason, "stop");

  free(raw);
  free(err);
  model_reply_destroy(reply);
  model_backend_destroy(mb);
  json_value_destroy(msgs);
  server.join();
  close(listen_fd);
}

TEST(TestModelDecode, TestNon2xxCarriesCodeAndExcerpt) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run_canned, listen_fd, 500,
    "{\"error\":\"boom\"}", &seen_body, &seen);

  std::string base_url = "http://127.0.0.1:" + std::to_string(port);
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  json_value_t* msgs = make_messages();
  model_reply_t* reply = NULL;
  char* err = NULL;
  char* raw = NULL;
  int rc = mb->complete(mb, msgs, NULL, &raw, &reply, &err);
  EXPECT_NE(rc, 0);
  ASSERT_NE(err, nullptr);
  EXPECT_NE(strstr(err, "500"), nullptr);
  EXPECT_NE(strstr(err, "boom"), nullptr);
  EXPECT_EQ(reply, nullptr);
  EXPECT_EQ(raw, nullptr);

  free(err);
  model_backend_destroy(mb);
  json_value_destroy(msgs);
  server.join();
  close(listen_fd);
}

TEST(TestModelDecode, TestMalformedBodyIsDecodeError) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run_canned, listen_fd, 200,
    "not json at all", &seen_body, &seen);

  std::string base_url = "http://127.0.0.1:" + std::to_string(port);
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  json_value_t* msgs = make_messages();
  model_reply_t* reply = NULL;
  char* err = NULL;
  char* raw = NULL;
  int rc = mb->complete(mb, msgs, NULL, &raw, &reply, &err);
  EXPECT_NE(rc, 0);
  ASSERT_NE(err, nullptr);
  EXPECT_NE(strstr(err, "decode"), nullptr);
  EXPECT_EQ(reply, nullptr);

  free(err);
  model_backend_destroy(mb);
  json_value_destroy(msgs);
  server.join();
  close(listen_fd);
}

TEST(TestModelDecode, TestTransportErrorReportsReason) {
  /* Port 1: refusals return instantly (no server listening). */
  frame_config_t cfg = {"http://127.0.0.1:1", NULL, "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  json_value_t* msgs = make_messages();
  model_reply_t* reply = NULL;
  char* err = NULL;
  char* raw = NULL;
  int rc = mb->complete(mb, msgs, NULL, &raw, &reply, &err);
  EXPECT_NE(rc, 0);
  ASSERT_NE(err, nullptr);
  EXPECT_NE(strstr(err, "model client"), nullptr);
  /* The http layer's transport reason is surfaced, not hidden behind the
     body placeholder (status -1 means no body, but error is set). The async
     client's connect failure carries its own reason shape. */
  EXPECT_NE(strstr(err, "http client: connect"), nullptr);
  EXPECT_EQ(strstr(err, "(no body)"), nullptr);
  EXPECT_EQ(reply, nullptr);
  EXPECT_EQ(raw, nullptr);

  free(err);
  model_backend_destroy(mb);
  json_value_destroy(msgs);
}

TEST(TestModelDecode, TestReasoningOnlyReplySurfacesContent) {
  /* Reasoning models (Ollama gemma4-class) legitimately answer with
     content:"" + a non-empty `reasoning` field and NO tool call — the
     assistant text lives in reasoning and the caller must see it, or the
     turn ends silently with an empty transcript and zero audit events. */
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run_canned, listen_fd, 200,
    "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
    "\"message\":{\"role\":\"assistant\",\"content\":\"\","
    "\"reasoning\":\"thought it out\"}}]}",
    &seen_body, &seen);

  std::string base_url = "http://127.0.0.1:" + std::to_string(port);
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  json_value_t* msgs = make_messages();
  model_reply_t* reply = NULL;
  char* err = NULL;
  char* raw = NULL;
  int rc = mb->complete(mb, msgs, NULL, &raw, &reply, &err);
  ASSERT_EQ(rc, 0) << (err ? err : "(no error string)");
  ASSERT_NE(reply, nullptr);
  EXPECT_STREQ(reply->content, "thought it out");
  EXPECT_EQ(reply->tool_code, nullptr);
  EXPECT_STREQ(reply->finish_reason, "stop");

  free(raw);

  model_reply_destroy(reply);
  model_backend_destroy(mb);
  json_value_destroy(msgs);
  server.join();
  close(listen_fd);
}

/* Accepts the request, then holds the connection open WITHOUT ever replying
   (a hanging model endpoint): the client's only escape is its timeout. */
static void fake_server_run_hang(int listen_fd, std::atomic<uint8_t>* seen) {
  int client_fd = accept(listen_fd, NULL, NULL);
  if (client_fd < 0) return;

  struct timeval tv = {3, 0};
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  char buf[8192];
  recv(client_fd, buf, sizeof(buf) - 1, 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(3000));
  close(client_fd);
  seen->store(1);
}

TEST(TestModelDecode, TestTimeoutResolveDerivation) {
  /* 0 = the built-in default; anything else passes through unchanged. */
  EXPECT_EQ(model_timeout_ms_resolve(0), 30000u);
  EXPECT_EQ(model_timeout_ms_resolve(1u), 1u);
  EXPECT_EQ(model_timeout_ms_resolve(30000u), 30000u);
  EXPECT_EQ(model_timeout_ms_resolve(180000u), 180000u);
}

TEST(TestModelDecode, TestConfigTimeoutReachesTransport) {
  /* The frame config's model_timeout_ms must actually bound the POST: a
     server that never answers fails in the configured window, not the
     built-in default (30 s) and not the server's own 3 s hold. The http
     layer reports a recv timeout and a peer close with one reason shape
     ("no complete header block from ..."), so the ELAPSED time is the
     proof the field reached the socket — 0.3 s configured vs 3 s hold vs
     30 s default. */
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run_hang, listen_fd, &seen);

  std::string base_url = "http://127.0.0.1:" + std::to_string(port);
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", NULL, 0, 4, 300};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  json_value_t* msgs = make_messages();
  model_reply_t* reply = NULL;
  char* err = NULL;
  char* raw = NULL;
  auto start = std::chrono::steady_clock::now();
  int rc = mb->complete(mb, msgs, NULL, &raw, &reply, &err);
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();

  EXPECT_NE(rc, 0);
  ASSERT_NE(err, nullptr);
  EXPECT_NE(strstr(err, "model client"), nullptr)
      << "err: " << (err ? err : "(null)");
  EXPECT_GE(elapsed_ms, 250) << "err: " << (err ? err : "(null)");
  EXPECT_LT(elapsed_ms, 2500) << "err: " << (err ? err : "(null)");
  EXPECT_EQ(reply, nullptr);
  EXPECT_EQ(raw, nullptr);

  free(err);
  model_backend_destroy(mb);
  json_value_destroy(msgs);
  server.join();
  close(listen_fd);
}

/* ---------------------------------------------------------------------- */
/* Submit-path tests (plan Task 4): REAL async submit over the loop thread */
/* ---------------------------------------------------------------------- */

/* The canned body carries BOTH content and a tool call — the same shape the
   standing complete() tests pin (content decoded verbatim, the first tool
   call's `code` argument decoded from its JSON-string form). */
static const char* CONTENT_AND_TOOL_BODY =
    "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
    "\"message\":{\"role\":\"assistant\",\"content\":\"thinking about it\","
    "\"tool_calls\":[{\"type\":\"function\",\"function\":{\"name\":\"execute\","
    "\"arguments\":\"{\\\"code\\\":\\\"print(6*7)\\\"}\"}}]}}]}";

TEST(TestModelDecode, TestSubmitRoundTripMatchesComplete) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run_canned, listen_fd, 200,
                     CONTENT_AND_TOOL_BODY, &seen_body, &seen);

  std::string base_url = "http://127.0.0.1:" + std::to_string(port);
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);
  ASSERT_NE(mb->submit, nullptr);   /* the http backend is NOT sync-only */

  json_value_t* msgs = make_messages();
  sink_record_t rec = {};   /* zero-init: C++ members value-init */
  int rc = mb->submit(mb, msgs, NULL, sink_record, &rec);
  EXPECT_EQ(rc, 0);
  ASSERT_TRUE(sink_wait_calls(&rec, 1));

  /* Exactly ONE delivery, on the loop thread (NOT the submitting test
     thread); sink_record consumed the heap body/error. */
  {
    std::unique_lock<std::mutex> lk(rec.m);
    EXPECT_EQ(rec.calls, 1);
    EXPECT_EQ(rec.status, 200);
    EXPECT_TRUE(rec.has_body);
    EXPECT_STREQ(CONTENT_AND_TOOL_BODY, rec.body.c_str());
    EXPECT_FALSE(rec.has_error);
    EXPECT_NE(rec.thread_id, std::this_thread::get_id())
        << "the sink fires on the loop thread, never the submitter";
    /* The canned server's three headers ride the delivery: the recorded
       count pins the relay's pass-through (the sink parses Retry-After from
       this capture on the real path). */
    EXPECT_GT(rec.header_count, 0u)
        << "the relay passes the captured headers through to the sink";
  }
  /* A second delivery must not follow the first (at-most-one). */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  {
    std::unique_lock<std::mutex> lk(rec.m);
    EXPECT_EQ(rec.calls, 1);
  }

  /* The SAME completion surface complete() yields on the same canned body —
     through the shared decode helper (model_internal.h): content + tool
     code + finish reason, matching the standing complete() pins. */
  model_reply_t* reply = NULL;
  char* err = NULL;
  int drc = _model_result_from_http(rec.status, rec.body.c_str(),
                                    rec.body.size(), NULL, 0, &reply, &err);
  EXPECT_EQ(drc, 0) << (err ? err : "(no error string)");
  ASSERT_NE(reply, nullptr);
  EXPECT_STREQ(reply->content, "thinking about it");
  ASSERT_NE(reply->tool_code, nullptr);
  EXPECT_STREQ(reply->tool_code, "print(6*7)");
  EXPECT_STREQ(reply->finish_reason, "tool_calls");

  free(err);
  model_reply_destroy(reply);
  /* The deferred client teardown is a QUEUED loop op (its 100 ms tick
     ceiling); giving it the tick keeps this fixture from ever stranding it
     at the backend destroy's loop unmount. */
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  model_backend_destroy(mb);
  json_value_destroy(msgs);
  server.join();
  close(listen_fd);
}

TEST(TestModelDecode, TestSubmitTransportFailureReachesTheSink) {
  /* Port 1: refusals return instantly (no server listening) — the http
     client fires a transport-error completion (status -1, reason, NULL
     body), the model submit has ALREADY returned 0, and the sink carries
     the failure to the caller. */
  frame_config_t cfg = {"http://127.0.0.1:1", NULL, "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  json_value_t* msgs = make_messages();
  sink_record_t rec = {};
  int rc = mb->submit(mb, msgs, NULL, sink_record, &rec);
  EXPECT_EQ(rc, 0);   /* accepted: the failure arrives AT the sink */
  ASSERT_TRUE(sink_wait_calls(&rec, 1));

  {
    std::unique_lock<std::mutex> lk(rec.m);
    EXPECT_EQ(rec.calls, 1);
    EXPECT_NE(rec.thread_id, std::this_thread::get_id());
    EXPECT_EQ(rec.status, -1);
    EXPECT_FALSE(rec.has_body);
    EXPECT_TRUE(rec.has_error);
    EXPECT_NE(rec.error.find("http client: connect"), std::string::npos)
        << "err: " << rec.error;
  }

  /* Same settle as the round-trip test: the deferred teardown op's tick. */
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  model_backend_destroy(mb);
  json_value_destroy(msgs);
}

TEST(TestModelDecode, TestSubmitRejectedNeverCallsSink) {
  /* NULL messages (http body unbuildable) → rejected BEFORE any I/O:
     rc != 0 and the sink NEVER fires — the http client's rejected-submit
     contract, inherited verbatim by the model backend. */
  frame_config_t cfg = {"http://127.0.0.1:1", NULL, "test-model", NULL, 0, 4};
  model_backend_t* mb = model_http_backend_create(&cfg);
  ASSERT_NE(mb, nullptr);

  sink_record_t rec = {};
  int rc = mb->submit(mb, NULL, NULL, sink_record, &rec);
  EXPECT_NE(rc, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  {
    std::unique_lock<std::mutex> lk(rec.m);
    EXPECT_EQ(rec.calls, 0);
  }

  model_backend_destroy(mb);
}

TEST(TestModelDecode, TestRetryAfterRidesTheSurface) {
  /* The decode's retry_after_sec param is a PASS-THROUGH fact for the
     loop's retry branch (model_internal.h): the value rides the surface and
     the decode treats every value byte-identically — a 0 (absent) and a
     present value decode the SAME body to the SAME reply, and a failure
     surfaces the SAME standing error text. The value's CONSUMER is
     `_frame_engine_reply`'s retry branch (guards slice Task 5); nothing in
     the decode may change shape because a Retry-After arrived. */
  model_reply_t* r_absent = NULL;
  model_reply_t* r_present = NULL;
  char* e_absent = NULL;
  char* e_present = NULL;

  /* A 2xx body decodes identically with the value present and absent. */
  ASSERT_EQ(_model_result_from_http(200, CONTENT_AND_TOOL_BODY,
                                    strlen(CONTENT_AND_TOOL_BODY), NULL, 0,
                                    &r_absent, &e_absent), 0);
  ASSERT_EQ(_model_result_from_http(200, CONTENT_AND_TOOL_BODY,
                                    strlen(CONTENT_AND_TOOL_BODY), NULL, 3,
                                    &r_present, &e_present), 0);
  ASSERT_NE(r_absent, nullptr);
  ASSERT_NE(r_present, nullptr);
  EXPECT_STREQ(r_absent->content, r_present->content);
  EXPECT_STREQ(r_absent->tool_code, r_present->tool_code);
  EXPECT_STREQ(r_absent->finish_reason, r_present->finish_reason);
  EXPECT_EQ(e_absent, nullptr);
  EXPECT_EQ(e_present, nullptr);
  model_reply_destroy(r_absent);
  model_reply_destroy(r_present);

  /* A non-2xx keeps the standing error surface, byte-identical either way
     (the status/class text is the decode's; the retry VALUE is not in it). */
  ASSERT_EQ(_model_result_from_http(429, NULL, 0, NULL, 0,
                                    &r_absent, &e_absent), -1);
  ASSERT_EQ(_model_result_from_http(429, NULL, 0, NULL, 90,
                                    &r_present, &e_present), -1);
  EXPECT_EQ(r_absent, nullptr);
  EXPECT_EQ(r_present, nullptr);
  ASSERT_NE(e_absent, nullptr);
  ASSERT_NE(e_present, nullptr);
  EXPECT_STREQ(e_absent, e_present)
      << "the standing surface is value-blind";
  EXPECT_NE(strstr(e_absent, "429"), nullptr) << "err: " << e_absent;
  free(e_absent);
  free(e_present);
}
