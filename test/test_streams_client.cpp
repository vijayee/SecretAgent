//
// Created by victor on 9/30/26.
//

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include "../src/Streams/loop_thread.h"
#include "../src/Streams/http_client.h"
}

/* Async HTTP client proofs: one POST, one response, on the real poll-dancer
   reactor thread. Fake servers reuse the house idiom from test_http.cpp
   (bind 127.0.0.1 on an ephemeral port, one connection, one canned reply,
   one server thread per test); completion records are waited on with
   mutex+cv — no sleeps-as-timing. */

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

/* Hang server: accepts one connection, holds it open for hold_ms without
   replying, then closes — the timeout and in-flight-cancel tests never get
   a response to race against. */
static void hang_server_run(int listen_fd, int hold_ms) {
  int client_fd = accept(listen_fd, NULL, NULL);
  if (client_fd >= 0) {
    /* Read nothing; hold the connection open without replying. */
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    close(client_fd);
  }
  close(listen_fd);
}

/* The single completion record every test waits on: copies the completion's
   heap strings, counts fires, and signals a cv — the client's completion
   contract is µs-scale on the loop thread, so this stays that cheap. */
struct completion_record {
  std::mutex m;
  std::condition_variable cv;
  bool fired = false;
  int fire_count = 0;
  int status = 0;
  std::string body;
  bool body_null = true;
  bool error_null = true;
  std::string error;
};

static void completion_record_reset(completion_record* r) {
  std::lock_guard<std::mutex> lk(r->m);
  r->fired = false;
  r->fire_count = 0;
  r->status = 0;
  r->body.clear();
  r->body_null = true;
  r->error_null = true;
  r->error.clear();
}

extern "C" void completion_record_on(void* ctx, int status, char* body,
                                     size_t body_len, char* error) {
  completion_record* r = (completion_record*)ctx;
  {
    std::lock_guard<std::mutex> lk(r->m);
    r->status = status;
    r->body_null = (body == NULL);
    if (body != NULL) r->body.assign(body, body_len);
    r->error_null = (error == NULL);
    if (error != NULL) r->error.assign(error);
    r->fired = true;
    r->fire_count++;
  }
  free(body);
  free(error);
  r->cv.notify_all();
}

static bool wait_completion(completion_record* r, int timeout_ms) {
  using std::chrono::steady_clock;
  std::unique_lock<std::mutex> lk(r->m);
  return r->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [&] { return r->fired; });
}

TEST(TestStreamsClient, TestSubmitCompletesRoundTrip) {
  streams_loop_thread_t* lt = streams_loop_create();
  ASSERT_NE(lt, nullptr);
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run, listen_fd, &seen_body, &seen, nullptr);

  http_client_t* c = http_client_create(lt);
  ASSERT_NE(c, nullptr);
  completion_record rec;
  std::string url =
    "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  int rc = http_client_submit(c, url.c_str(), NULL,
                              "{\"model\":\"m\",\"messages\":[]}", 3000,
                              completion_record_on, &rec);
  ASSERT_EQ(rc, 0);  /* accepted before any I/O; completion fires on the loop thread */
  ASSERT_TRUE(wait_completion(&rec, 5000));

  EXPECT_EQ(rec.status, 200);
  EXPECT_NE(rec.body.find("\"ok\":true"), std::string::npos);
  EXPECT_FALSE(rec.body_null);
  EXPECT_TRUE(rec.error_null);
  EXPECT_EQ(rec.fire_count, 1);

  server.join();
  /* The server saw exactly the advertised POST: path, length, content type. */
  EXPECT_NE(seen_body.find("POST /v1/chat/completions"), std::string::npos);
  EXPECT_NE(seen_body.find("Content-Length"), std::string::npos);
  EXPECT_NE(seen_body.find("Content-Type: application/json"), std::string::npos);
  EXPECT_NE(seen_body.find("{\"model\":\"m\",\"messages\":[]}"), std::string::npos);

  http_client_destroy(c);
  streams_loop_destroy(lt);
}

TEST(TestStreamsClient, TestDeadEndpointReportsTransport) {
  streams_loop_thread_t* lt = streams_loop_create();
  ASSERT_NE(lt, nullptr);
  http_client_t* c = http_client_create(lt);
  ASSERT_NE(c, nullptr);
  completion_record rec;
  int rc = http_client_submit(c, "http://127.0.0.1:1/x", NULL, "{}", 1000,
                              completion_record_on, &rec);
  ASSERT_EQ(rc, 0);
  ASSERT_TRUE(wait_completion(&rec, 5000));

  EXPECT_EQ(rec.status, -1);        /* transport failure, not an HTTP code */
  EXPECT_TRUE(rec.body_null);       /* no body on a transport error */
  EXPECT_FALSE(rec.error_null);     /* heap reason present */
  EXPECT_FALSE(rec.error.empty());
  EXPECT_EQ(rec.fire_count, 1);

  http_client_destroy(c);
  streams_loop_destroy(lt);
}

TEST(TestStreamsClient, TestTimeoutFiresOnce) {
  streams_loop_thread_t* lt = streams_loop_create();
  ASSERT_NE(lt, nullptr);
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::thread server(hang_server_run, listen_fd, 3000);

  http_client_t* c = http_client_create(lt);
  ASSERT_NE(c, nullptr);
  completion_record rec;
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/x";
  auto t0 = std::chrono::steady_clock::now();
  int rc = http_client_submit(c, url.c_str(), NULL, "{}", 300,
                              completion_record_on, &rec);
  ASSERT_EQ(rc, 0);
  ASSERT_TRUE(wait_completion(&rec, 3000));
  auto elapsed_ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();

  EXPECT_EQ(rec.status, -1);
  EXPECT_TRUE(rec.body_null);
  EXPECT_FALSE(rec.error_null);
  EXPECT_EQ(rec.fire_count, 1);     /* exactly once: the timer is one-shot */
  EXPECT_GE(elapsed_ms, 250);       /* near the 300ms timer, not instant */
  EXPECT_LE(elapsed_ms, 2500);      /* and nowhere near the 3s server hold */

  server.join();
  http_client_destroy(c);
  streams_loop_destroy(lt);
}

TEST(TestStreamsClient, TestSubmitCopiesArguments) {
  streams_loop_thread_t* lt = streams_loop_create();
  ASSERT_NE(lt, nullptr);
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run, listen_fd, &seen_body, &seen, nullptr);

  http_client_t* c = http_client_create(lt);
  ASSERT_NE(c, nullptr);
  completion_record rec;

  char url_buf[96];
  snprintf(url_buf, sizeof(url_buf),
           "http://127.0.0.1:%u/v1/chat/completions", (unsigned)port);
  char body_buf[32];
  snprintf(body_buf, sizeof(body_buf), "%s", "{\"msg\":\"original\"}");

  int rc = http_client_submit(c, url_buf, NULL, body_buf, 3000,
                              completion_record_on, &rec);
  ASSERT_EQ(rc, 0);
  /* Scribble over the caller's buffers immediately after submit returns:
     the client owns COPIES, so the request on the wire must be untouched. */
  memset(url_buf, 'a', sizeof(url_buf) - 1);
  memset(body_buf, 'q', sizeof(body_buf) - 1);

  ASSERT_TRUE(wait_completion(&rec, 5000));
  EXPECT_EQ(rec.status, 200);

  server.join();
  EXPECT_NE(seen_body.find("POST /v1/chat/completions"), std::string::npos);
  EXPECT_NE(seen_body.find("{\"msg\":\"original\"}"), std::string::npos);
  EXPECT_NE(seen_body.find("Content-Length: 18"), std::string::npos);
  /* The scribbled bytes never reached the wire. */
  EXPECT_EQ(seen_body.find("aaaa"), std::string::npos);
  EXPECT_EQ(seen_body.find("qqqq"), std::string::npos);

  http_client_destroy(c);
  streams_loop_destroy(lt);
}

TEST(TestStreamsClient, TestRejectedSubmitNeverCallsBack) {
  streams_loop_thread_t* lt = streams_loop_create();
  ASSERT_NE(lt, nullptr);
  http_client_t* c = http_client_create(lt);
  ASSERT_NE(c, nullptr);
  completion_record rec;

  int rc = http_client_submit(c, "http://127.0.0.1:1/x", NULL, NULL, 1000,
                              completion_record_on, &rec);
  EXPECT_NE(rc, 0);                          /* rejected before any I/O */
  EXPECT_FALSE(wait_completion(&rec, 50));   /* no callback within 50ms */

  /* Cancelling the only (never-started) request must not resurrect it. */
  http_client_destroy(c);
  EXPECT_FALSE(wait_completion(&rec, 100));
  streams_loop_destroy(lt);
}

TEST(TestStreamsClient, TestDestroyCancelsInflight) {
  streams_loop_thread_t* lt = streams_loop_create();
  ASSERT_NE(lt, nullptr);
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::thread server(hang_server_run, listen_fd, 1500);

  http_client_t* c = http_client_create(lt);
  ASSERT_NE(c, nullptr);
  completion_record rec;
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/x";
  int rc = http_client_submit(c, url.c_str(), NULL, "{}", 5000,
                              completion_record_on, &rec);
  ASSERT_EQ(rc, 0);

  /* Let the request go in-flight (watcher on the loop, timer armed), then
     tear the client down mid-flight. */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto t0 = std::chrono::steady_clock::now();
  http_client_destroy(c);
  auto destroy_ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  EXPECT_LE(destroy_ms, 1000);               /* destroy is prompt, not a hang */

  EXPECT_FALSE(wait_completion(&rec, 2000)); /* cancelled: never fires */
  EXPECT_EQ(rec.fire_count, 0);

  server.join();
  streams_loop_destroy(lt);
}

/* -------- helpers -------- */

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
     closes the connection with zero bytes read, and the server thread must
     still exit. */
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