//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
extern "C" {
#include "../src/Net/http.h"
}

/* Fake server: one-connection TCP thread that reads the request and replies
   with a canned HTTP/1.1 response. POSIX sockets used directly in test code
   (test-side socket code is allowed; the RUNTIME uses src/Net/http only). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static void _serve_once(int client_fd);
static void fake_server_run(int listen_fd, std::string* seen_body, std::atomic<uint8_t>* seen);

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

TEST(TestHttp, TestPostJsonRoundTrip) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run, listen_fd, &seen_body, &seen);

  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  http_response_t* r = http_post_json(url.c_str(), NULL, "{\"model\":\"m\",\"messages\":[]}", 3000);
  ASSERT_NE(r, nullptr);
  for (int i = 0; i < 5000 && seen.load() == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(seen.load(), 1);
  EXPECT_EQ(r->status, 200);
  EXPECT_EQ(r->error, nullptr);
  EXPECT_NE(strstr(r->body, "\"ok\":true"), nullptr);
  EXPECT_NE(seen_body.find("/v1/chat/completions"), std::string::npos);
  EXPECT_NE(seen_body.find("chat/completions"), std::string::npos);   /* the path reached the server */
  EXPECT_NE(seen_body.find("Content-Length"), std::string::npos);

  http_response_destroy(r);
  server.join();
}

TEST(TestHttp, TestTransportErrorIsReported) {
  http_response_t* r = http_post_json("http://127.0.0.1:1/x", NULL, "{}", 300);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->status, -1);
  ASSERT_NE(r->error, nullptr);
  http_response_destroy(r);
}

/* Fills in the canned 200 reply: a body containing "ok":true with an honest
   Content-Length. _serve_once only ever gets a connected, request-answered
   descriptor from fake_server_run, so it never inspects the request. */
static void _serve_once(int client_fd) {
  const char* body = "{\"ok\":true}";
  size_t body_len = strlen(body);
  char reply[256];
  int n = snprintf(reply, sizeof(reply),
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n"
    "\r\n",
    body_len);
  ASSERT_GT(n, 0);
  ASSERT_LT((size_t)n, sizeof(reply));
  size_t sent = 0;
  while (sent < (size_t)n) {
    ssize_t w = write(client_fd, reply + sent, (size_t)n - sent);
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
}

static void fake_server_run(int listen_fd, std::string* seen_body, std::atomic<uint8_t>* seen) {
  int client_fd = accept(listen_fd, NULL, NULL);
  ASSERT_GT(client_fd, 0);

  /* Read the request: headers first, then exactly Content-Length more bytes. */
  char buf[4096];
  size_t total = 0;
  size_t header_end = 0;
  while (header_end == 0) {
    ssize_t n = recv(client_fd, buf + total, sizeof(buf) - total - 1, 0);
    if (n <= 0) break;
    total += (size_t)n;
    buf[total] = '\0';
    /* Header block ends at CRLF CRLF. */
    for (size_t i = 0; i + 3 < total; i++) {
      if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
        header_end = i + 4;
        break;
      }
    }
  }
  ASSERT_GT(total, (size_t)0);
  *seen_body = buf;

  /* Drain the request body (Content-Length in seen_body), so the client's
     send side never stalls. */
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

  /* Reply once the full request arrived, so the client's response read
     begins on a clean, well-formed exchange. */
  _serve_once(client_fd);
  seen->store(1);

  close(client_fd);
}