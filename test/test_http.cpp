//
// Created by victor on 9/29/26.
//

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "../src/Net/http.h"
}

/* Fake server: one-connection TCP thread that reads the request and replies
   with a canned HTTP/1.1 response. POSIX sockets used directly in test code
   (test-side socket code is allowed; the RUNTIME uses src/Net/http only). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static void _serve_once(int client_fd, const char* canned);
static void fake_server_run(int listen_fd, std::string* seen_body,
                            std::atomic<uint8_t>* seen, const char* canned);

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
  std::thread server(fake_server_run, listen_fd, &seen_body, &seen, nullptr);

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
   Content-Length. With a non-NULL canned reply, that full raw response is
   sent verbatim instead (chunked framing tests use this). _serve_once only
   ever gets a connected, request-answered descriptor from fake_server_run,
   so it never inspects the request. */
static void _serve_once(int client_fd, const char* canned) {
  const char* body = "{\"ok\":true}";
  size_t body_len = strlen(body);
  char reply[256];
  size_t sent;
  if (canned != NULL) {
    body = canned;
    body_len = strlen(body);
    sent = 0;
    while (sent < body_len) {
      ssize_t w = write(client_fd, body + sent, body_len - sent);
      ASSERT_GT(w, 0);
      sent += (size_t)w;
    }
    shutdown(client_fd, SHUT_WR);
    return;
  }
  int n = snprintf(reply, sizeof(reply),
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %zu\r\n"
    "Connection: close\r\n"
    "\r\n",
    body_len);
  ASSERT_GT(n, 0);
  ASSERT_LT((size_t)n, sizeof(reply));
  sent = 0;
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

static void fake_server_run(int listen_fd, std::string* seen_body,
                            std::atomic<uint8_t>* seen, const char* canned) {
  int client_fd = accept(listen_fd, NULL, NULL);
  ASSERT_GT(client_fd, 0);

  /* Bound the read: a client that fails past connect but before sending
     (the leak pins close the fd during header build) closes the connection
     with zero bytes read, and the server thread must still exit. */
  struct timeval tv = {3, 0};
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  /* Read the request: headers first, then exactly Content-Length more bytes. */
  char buf[4096];
  size_t total = 0;
  size_t header_end = 0;
  while (header_end == 0) {
    if (total + 1 >= sizeof(buf)) break;
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
  if (header_end == 0) {
    /* No parseable request ever arrived: nothing to record, close and exit
       so the test thread can join. */
    close(client_fd);
    return;
  }
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
  _serve_once(client_fd, canned);
  seen->store(1);

  close(client_fd);
}

TEST(TestHttp, TestLongUrlAndKeyDoNotLeak) {
  /* Pins the fix for the post-connect header-build failure branches: an
     oversized path (request-line overflow) and an oversized bearer key
     (auth-header overflow) must return a transport-error response with NO
     leak of the parsed path (run under valgrind in CI). Connect a real
     (fake) server so the header-build branches are actually reached. */
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run, listen_fd, &seen_body, &seen, nullptr);

  std::string giant_path(4096, 'a');
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/" + giant_path;
  http_response_t* r = http_post_json(url.c_str(), NULL, "{}", 1000);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->status, -1);      /* request-line overflow is a transport error */
  ASSERT_NE(r->error, nullptr);  /* the failure reason is set for the caller */
  http_response_destroy(r);

  std::string long_key(4050, 'k');
  /* The auth branch sits between connect and send on the SAME connection as
     the request-line check, so the key case needs a SHORT path to reach it. */
  std::string short_url = "http://127.0.0.1:" + std::to_string(port) + "/";
  r = http_post_json(short_url.c_str(), long_key.c_str(), "{}", 1000);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->status, -1);      /* auth-header overflow is a transport error */
  ASSERT_NE(r->error, nullptr);
  http_response_destroy(r);

  close(listen_fd);
  server.join();   /* fake_server_run tolerates never receiving a request:
                      bounded read, then it closes its accepted fd and exits */
}

/* -------- chunked responses (Transfer-Encoding: chunked decode) -------- */

/* Builds a canned chunked HTTP/1.1 response: each piece becomes one chunk
   (lower-case hex size, ";ext=1" parameter on the second), and with_trailer
   puts a trailer field between the zero-size chunk and the final CRLF. */
static std::string chunked_reply(const std::vector<std::string>& pieces,
                                 bool with_trailer) {
  std::string reply =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Transfer-Encoding: chunked\r\n"
    "Connection: close\r\n"
    "\r\n";
  for (size_t i = 0; i < pieces.size(); i++) {
    char head[32];
    snprintf(head, sizeof(head), "%zx%s\r\n", pieces[i].size(),
             (i == 1) ? ";ext=1" : "");
    reply += head;
    reply += pieces[i];
    reply += "\r\n";
  }
  reply += "0\r\n";
  if (with_trailer) reply += "X-Wave-Note: ignored-by-decode\r\n";
  reply += "\r\n";
  return reply;
}

/* One fake-server round trip against a canned raw reply; returns the client
   response to destroy in the caller's test. */
static http_response_t* post_against_canned(const std::string& canned) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  EXPECT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run, listen_fd, &seen_body, &seen,
                     canned.c_str());
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  http_response_t* r = http_post_json(url.c_str(), NULL, "{}", 3000);
  EXPECT_NE(r, nullptr);
  server.join();
  close(listen_fd);
  return r;
}

TEST(TestHttp, TestChunkedMultiChunkBodyDecodes) {
  /* Three chunks whose boundaries deliberately cut mid-JSON (no alignment
     with token or CRLF boundaries); the last carries an odd hex size. */
  const std::string original =
    "{\"choices\":[{\"message\":{\"content\":\"wave wave wave\"}}],"
    "\"usage\":{\"total_tokens\":42}}";
  std::vector<std::string> pieces = {original.substr(0, 19),
                                     original.substr(19, 37),
                                     original.substr(56)};
  ASSERT_EQ(pieces[0].size() + pieces[1].size() + pieces[2].size(),
            original.size());
  std::string reply = chunked_reply(pieces, true);

  http_response_t* r = post_against_canned(reply);
  ASSERT_NE(r, nullptr);
  ASSERT_NE(r->body, nullptr);
  EXPECT_EQ(r->status, 200);
  EXPECT_EQ(r->error, nullptr);
  EXPECT_EQ(r->body_len, original.size());          /* no trailing CRLF noise */
  EXPECT_STREQ(r->body, original.c_str());          /* decoded + NUL-terminated */
  http_response_destroy(r);
}

TEST(TestHttp, TestChunkedSizeLineDialects) {
  /* Dialect field servers may send: the header VALUE capitalised
     ("Chunked"), uppercase hex sizes, and a parameter on every size line.
     The decode must still be byte-exact across all of it. */
  const std::string original = "{\"wave\":[1,2,3,{\"note\":\"deepest\"}]}";
  std::vector<std::string> pieces = {original.substr(0, 11),
                                     original.substr(11, 14),
                                     original.substr(25)};
  ASSERT_EQ(pieces[0].size() + pieces[1].size() + pieces[2].size(),
            original.size());

  std::string canned =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
    "Transfer-Encoding: Chunked\r\nConnection: close\r\n\r\n";
  for (size_t i = 0; i < pieces.size(); i++) {
    char head[32];
    snprintf(head, sizeof(head), "%X;wave=go\r\n", (unsigned)pieces[i].size());
    canned += head;
    canned += pieces[i];
    canned += "\r\n";
  }
  canned += "0\r\n\r\n";

  http_response_t* r = post_against_canned(canned);
  ASSERT_NE(r, nullptr);
  ASSERT_NE(r->body, nullptr);
  EXPECT_EQ(r->status, 200);
  EXPECT_EQ(r->error, nullptr);
  EXPECT_EQ(r->body_len, original.size());
  EXPECT_STREQ(r->body, original.c_str());
  http_response_destroy(r);
}

TEST(TestHttp, TestChunkedHeaderValueDialects) {
  /* Header-VALUE dialects around the coding LIST: value capitalised, OWS
     around the list comma (chunked declared last, as RFC 7230 requires on
     the chunked-wins rule). Decode must still be byte-exact. */
  const std::string original = "{\"note\":\"list dialect\"}";
  std::vector<std::string> pieces = {original.substr(0, 12),
                                     original.substr(12)};
  std::string canned =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
    "Transfer-Encoding: gzip , Chunked\r\nConnection: close\r\n\r\n";
  for (size_t i = 0; i < pieces.size(); i++) {
    char head[32];
    snprintf(head, sizeof(head), "%zX\r\n", pieces[i].size());
    canned += head;
    canned += pieces[i];
    canned += "\r\n";
  }
  canned += "0\r\n\r\n";

  http_response_t* r = post_against_canned(canned);
  ASSERT_NE(r, nullptr);
  ASSERT_NE(r->body, nullptr);
  EXPECT_EQ(r->status, 200);
  EXPECT_EQ(r->error, nullptr);
  EXPECT_EQ(r->body_len, original.size());
  EXPECT_STREQ(r->body, original.c_str());
  http_response_destroy(r);
}

TEST(TestHttp, TestChunkedMalformedSizeLineFailsClean) {
  /* "zz" is not a hex size: the client must report a transport failure with
     the documented shape (status -1, error set, no body) and not crash. */
  http_response_t* r = post_against_canned(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n"
    "\r\nzz\r\n{\"a\":1}\r\n0\r\n\r\n");
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->status, -1);
  ASSERT_NE(r->error, nullptr);
  EXPECT_NE(strstr(r->error, "chunked"), nullptr);
  EXPECT_EQ(r->body, nullptr);
  EXPECT_EQ(r->body_len, 0);
  http_response_destroy(r);
}

TEST(TestHttp, TestChunkedTruncatedChunkFailsClean) {
  /* Chunk size promises 5 bytes of data, the connection closes after 3:
     unexpected EOF before the terminator is a transport failure, never a
     silently truncated success. */
  http_response_t* r = post_against_canned(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n"
    "\r\n5\r\nabc");
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->status, -1);
  ASSERT_NE(r->error, nullptr);
  EXPECT_EQ(r->body, nullptr);
  EXPECT_EQ(r->body_len, 0);
  http_response_destroy(r);
}
