//
// Created by victor on 9/29/26.
//

/* Direct decode tests for the model client (plan Task 9). These drive
   model_backend_t.complete through the REAL http path against a local fake
   HTTP server (same fixture pattern as test_http.cpp: test-side sockets are
   allowed; the runtime uses src/Net/http only). All canned OpenAI-shaped
   bodies; offline. */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

extern "C" {
#include "../src/Frame/frame.h"
#include "../src/Frame/model.h"
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
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", 4};
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
  frame_config_t cfg = {base_url.c_str(), "sk-test", "test-model", 4};
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
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", 4};
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
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", 4};
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
  frame_config_t cfg = {base_url.c_str(), NULL, "test-model", 4};
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
  frame_config_t cfg = {"http://127.0.0.1:1", NULL, "test-model", 4};
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
  EXPECT_EQ(reply, nullptr);
  EXPECT_EQ(raw, nullptr);

  free(err);
  model_backend_destroy(mb);
  json_value_destroy(msgs);
}