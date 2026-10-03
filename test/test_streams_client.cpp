//
// Created by victor on 9/30/26.
//

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "../src/Streams/loop_thread.h"
#include "../src/Streams/http_client.h"
#include "../src/Streams/http_headers.h"
}

/* Async HTTP client proofs: one POST, one response, on the real poll-dancer
   reactor thread. Fake servers reuse the retired sync-client suite's house
   idiom (bind 127.0.0.1 on an ephemeral port, one connection, one canned
   reply, one server thread per test); completion records are waited on with
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
  size_t body_len = 0;     /* the client's reported length (not body.size()) */
  bool body_null = true;
  bool error_null = true;
  std::string error;
  http_headers_t* headers = nullptr;  /* the completion's captured set, OWNED
                                         whole (deinit + frees on destruction) */
  bool headers_null = true;
  ~completion_record() {
    if (headers != nullptr) {
      http_headers_deinit(headers);
      free(headers);
      headers = nullptr;
    }
  }
};

static void completion_record_reset(completion_record* r) {
  std::lock_guard<std::mutex> lk(r->m);
  r->fired = false;
  r->fire_count = 0;
  r->status = 0;
  r->body.clear();
  r->body_len = 0;
  r->body_null = true;
  r->error_null = true;
  r->error.clear();
}

extern "C" void completion_record_on(void* ctx, int status, char* body,
                                     size_t body_len, char* error,
                                     http_headers_t* headers) {
  completion_record* r = (completion_record*)ctx;
  {
    std::lock_guard<std::mutex> lk(r->m);
    r->status = status;
    r->body_null = (body == NULL);
    r->body_len = body_len;
    if (body != NULL) r->body.assign(body, body_len);
    r->error_null = (error == NULL);
    if (error != NULL) r->error.assign(error);
    /* The headers move INTO the record whole: the callback owns the capture,
       the record is its owner from here (freed on destruction). */
    r->headers = headers;
    r->headers_null = (headers == NULL);
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
  EXPECT_TRUE(rec.headers_null);    /* a failed transport delivers no headers */
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

/* Both framings the retired sync client's rules survive for, live against a
   canned server: an HTTP/1.0 close-delimited response (no Content-Length, no
   Transfer-Encoding — the body runs to connection close) still completes as
   a 200 SUCCESS with the full body, not a "short body" transport failure. */
TEST(TestStreamsClient, TestChunkedAndHttp10BothLive) {
  streams_loop_thread_t* lt = streams_loop_create();
  ASSERT_NE(lt, nullptr);
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  const char* canned =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "close-delimited success";
  std::thread server(fake_server_run, listen_fd, &seen_body, &seen, canned);

  http_client_t* c = http_client_create(lt);
  ASSERT_NE(c, nullptr);
  completion_record rec;
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/x";
  int rc = http_client_submit(c, url.c_str(), NULL, "{}", 3000,
                              completion_record_on, &rec);
  ASSERT_EQ(rc, 0);
  ASSERT_TRUE(wait_completion(&rec, 5000));

  EXPECT_EQ(rec.status, 200);            /* read-to-close is a success */
  EXPECT_FALSE(rec.body_null);
  EXPECT_EQ(rec.body, "close-delimited success");
  EXPECT_TRUE(rec.error_null);
  EXPECT_EQ(rec.fire_count, 1);

  server.join();
  http_client_destroy(c);
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

/* ------------------------------------------------------------------ */
/* The framing battery, re-pinned from the retired sync client         */
/* ------------------------------------------------------------------ */

/* The Task-8-era framing battery lived in test/test_http.cpp and retired
   with the sync client; these pins carry its rules over to the async
   client, where http-parser (the pinned lib) now owns framing. What the
   move changed, and how each pin was adjusted honestly:

   - A Content-Length together with chunked is now a TRANSPORT FAILURE: the
     strict parse flags the RFC 7230 3.3.3 smuggling shape
     (HPE_UNEXPECTED_CONTENT_LENGTH) instead of the sync client's lenient
     "chunked wins". The repeated-TE pin therefore drops its Content-Length
     line, and the TE+CL combo gets its own failure pin below.
   - A malformed size line surfaces through the parser's dialect verdict —
     the client's "parse error (...) HPE_*" wording — so those pins assert
     the transport-failure shape and the stable "parse error" framing
     rather than a synced-on phrase.
   - An absurd CHUNK-SIZE claim is not caught AT the claim (the pinned
     parser accepts any hex size); the close/cap disciplines catch it as
     soon as real evidence arrives. Those pins assert the failure shape
     ("chunked" ... connection closed), not where the reject fires.
   - A never-ending raw stream is caught well before the 64 MiB read cap —
     by the parser's own ~8 KB per-header-block bound on a header flood —
     so the raw-flood pin asserts the rejection shape, not which bound won. */

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

/* One canned exchange through the full client stack: server thread, loop
   thread, submit, completion wait. Returns true when the completion fired;
   `rec` carries the completion's outcome either way. */
static bool canned_roundtrip(const std::string& canned, completion_record& rec) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  EXPECT_GE(listen_fd, 0);
  if (listen_fd < 0) return false;
  std::string seen_body;
  std::atomic<uint8_t> seen;
  seen.store(0);
  std::thread server(fake_server_run, listen_fd, &seen_body, &seen,
                     canned.c_str());

  streams_loop_thread_t* lt = streams_loop_create();
  EXPECT_NE(lt, nullptr);
  http_client_t* c = http_client_create(lt);
  EXPECT_NE(c, nullptr);
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/x";
  int rc = http_client_submit(c, url.c_str(), NULL, "{}", 10000,
                              completion_record_on, &rec);
  EXPECT_EQ(rc, 0);
  bool fired = wait_completion(&rec, 10000);
  EXPECT_TRUE(fired);

  server.join();
  close(listen_fd);
  http_client_destroy(c);
  streams_loop_destroy(lt);
  return fired;
}

TEST(TestStreamsClient, TestChunkedMultiChunkBodyDecodes) {
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

  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(chunked_reply(pieces, true), rec));
  EXPECT_EQ(rec.status, 200);
  EXPECT_TRUE(rec.error_null);
  EXPECT_FALSE(rec.body_null);
  EXPECT_EQ(rec.body_len, original.size()); /* no trailing CRLF noise */
  EXPECT_EQ(rec.body, original);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestChunkedSizeLineDialects) {
  /* Dialects the wire carries: the header VALUE capitalised ("Chunked"),
     uppercase hex sizes, and a parameter on every size line. Decode must
     stay byte-exact across all of it (the parser matches case-insensitively
     and ignores chunk parameters). */
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

  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(canned, rec));
  EXPECT_EQ(rec.status, 200);
  EXPECT_TRUE(rec.error_null);
  ASSERT_FALSE(rec.body_null);
  EXPECT_EQ(rec.body_len, original.size());
  EXPECT_EQ(rec.body, original);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestChunkedHeaderValueDialects) {
  /* Header-VALUE dialects around the coding LIST: value capitalised, OWS
     around the list comma (chunked declared last, as RFC 7230 requires on
     the chunked-wins rule). Decode must stay byte-exact. */
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

  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(canned, rec));
  EXPECT_EQ(rec.status, 200);
  EXPECT_TRUE(rec.error_null);
  ASSERT_FALSE(rec.body_null);
  EXPECT_EQ(rec.body_len, original.size());
  EXPECT_EQ(rec.body, original);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestChunkedDeclaredOnRepeatedTransferEncodingLines) {
  /* RFC 7230 section 3.2.2: repeated Transfer-Encoding field-lines are ONE
     field value list — every later field-line still contributes its codings.
     chunked declared on a line AFTER a chunkless line must still win (here
     over the close-delimited reading an earlier chunkless TE would imply).
     The sync-client-era pin also carried a Content-Length alongside; the
     pinned strict parser now rejects that combination outright (RFC 7230
     3.3.3's smuggling rule — see the failure pin below), so the length lie
     was dropped from this dialect pin. */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: gzip\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "c\r\n{\"split\":tr}\r\n0\r\n\r\n", rec));
  EXPECT_EQ(rec.status, 200);
  EXPECT_TRUE(rec.error_null);
  ASSERT_FALSE(rec.body_null);
  EXPECT_EQ(rec.body_len, 12u);
  EXPECT_EQ(rec.body, "{\"split\":tr}");
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestContentLengthWithChunkedIsTransportFailure) {
  /* Both framings on one response: the pinned strict parser rejects the
     smuggling shape (HPE_UNEXPECTED_CONTENT_LENGTH) instead of the sync
     client's lenient let-chunked-win — an honest semantic move to the
     parser's verdict. The pin is the failure shape; never a success. */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 12\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "c\r\n{\"split\":tr}\r\n0\r\n\r\n", rec));
  EXPECT_EQ(rec.status, -1);
  EXPECT_TRUE(rec.body_null);
  EXPECT_FALSE(rec.error_null);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestChunkedMalformedSizeLineFailsClean) {
  /* "zz" is not a hex size: the client reports a transport failure with the
     documented shape (status -1, error set, no body) and not a crash. The
     reason travels through the pinned parser's verdict ("parse error ...
     HPE_INVALID_CHUNK_SIZE"), so the pin is the shape plus the "parse
     error" framing, not the retired client's own "chunked" phrase. */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n"
    "\r\nzz\r\n{\"a\":1}\r\n0\r\n\r\n", rec));
  EXPECT_EQ(rec.status, -1);
  ASSERT_FALSE(rec.error_null);
  EXPECT_NE(rec.error.find("parse error"), std::string::npos);
  EXPECT_TRUE(rec.body_null);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestChunkedTruncatedChunkFailsClean) {
  /* Chunk size promises 5 bytes of data, the connection closes after 3:
     unexpected EOF before the terminator is a transport failure, never a
     silently truncated success. */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n"
    "\r\n5\r\nabc", rec));
  EXPECT_EQ(rec.status, -1);
  ASSERT_FALSE(rec.error_null);
  EXPECT_NE(rec.error.find("chunked"), std::string::npos);
  EXPECT_TRUE(rec.body_null);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestChunkedAbsurdSizeClaimRejected) {
  /* A size claim over the 64 MiB cap with only a fragment delivered. The
     pinned parser accepts the size as a number — the reject fires on real
     evidence (the fragment feeds the buffer, the close makes the frame
     unfinishable). Pinned as the failure shape ("chunked", no body), not
     as an up-front claim check that the parser does not make. */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n"
    "\r\n40000001\r\njunk", rec));
  EXPECT_EQ(rec.status, -1);
  ASSERT_FALSE(rec.error_null);
  EXPECT_NE(rec.error.find("chunked"), std::string::npos);
  EXPECT_TRUE(rec.body_null);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestChunkedMissingFinalCrlfFailsClean) {
  /* The last-chunk terminator arrives ("0\r\n") and then the connection
     closes without the final CRLF that ends the empty trailer section: the
     frame is incomplete, so this is a transport failure — never a silently
     truncated success. */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n"
    "\r\n5\r\nhello\r\n0\r\n", rec));
  EXPECT_EQ(rec.status, -1);
  ASSERT_FALSE(rec.error_null);
  EXPECT_NE(rec.error.find("chunked"), std::string::npos);
  EXPECT_TRUE(rec.body_null);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestChunkedEmptyBodyZeroLengthNotNull) {
  /* An empty completion comes back as headers + "0\r\n\r\n" — zero data
     chunks. The reply is still a success: status 200 and a zero-length
     (non-NULL, NUL-terminated) body, never a NULL store or a crash. */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(chunked_reply({}, false), rec));
  EXPECT_EQ(rec.status, 200);
  EXPECT_TRUE(rec.error_null);
  EXPECT_FALSE(rec.body_null);
  EXPECT_EQ(rec.body_len, 0u);
  EXPECT_EQ(rec.body, "");
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestChunkedEmptyBodyWithTrailerZeroLengthNotNull) {
  /* Same empty-body reply, but with a trailer field between the zero-size
     chunk and the final CRLF: trailers must not change the success shape. */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(chunked_reply({}, true), rec));
  EXPECT_EQ(rec.status, 200);
  EXPECT_TRUE(rec.error_null);
  EXPECT_FALSE(rec.body_null);
  EXPECT_EQ(rec.body_len, 0u);
  EXPECT_EQ(rec.body, "");
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestContentLengthZeroIsNullBody) {
  /* Content-Length: 0 is the CLOSE-delimited rule's twin: the body store
     stays NULL (an empty chunked body is the only non-NULL empty). */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", rec));
  EXPECT_EQ(rec.status, 200);
  EXPECT_TRUE(rec.error_null);
  EXPECT_TRUE(rec.body_null);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestContentLengthOverBodyCapRejected) {
  /* A promised Content-Length over the 64 MiB cap is a transport failure
     before the read loop can grow toward the lie (1 GiB claimed here; the
     delivered fragment + close would otherwise resolve to a plain
     short-body error, so the message naming the cap pins the reject). */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 200 OK\r\nContent-Length: 1073741824\r\nConnection: close\r\n"
    "\r\n{\"tiny\":1}", rec));
  EXPECT_EQ(rec.status, -1);
  ASSERT_FALSE(rec.error_null);
  EXPECT_NE(rec.error.find("cap"), std::string::npos);
  EXPECT_TRUE(rec.body_null);
  EXPECT_EQ(rec.fire_count, 1);
}

TEST(TestStreamsClient, TestContentLengthOverBodyCapRejectedBeforeAnyBody) {
  /* Same lie with NO body bytes at all: the reject must not depend on body
     evidence either — the headers alone complete, and the cap marked there
     still fails the response instead of a 200 with a NULL body. */
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 200 OK\r\nContent-Length: 1073741824\r\nConnection: close\r\n"
    "\r\n", rec));
  EXPECT_EQ(rec.status, -1);
  ASSERT_FALSE(rec.error_null);
  EXPECT_NE(rec.error.find("cap"), std::string::npos);
  EXPECT_TRUE(rec.body_null);
  EXPECT_EQ(rec.fire_count, 1);
}

/* The completion carries the response headers (the ported http_headers
   module): a 429 with its Retry-After plus an arbitrary extra field arrive
   on EVERY success-shaped completion. Lookup is case-INSENSITIVE (the
   ported module pairs names with strcasecmp) — the lookups below use a
   different case than the server delivered to pin that.

   X-Wave-Empty carries NO value (http-parser still fires the value callback
   with length 0): the empty pair commits as a skip, but the accumulator MUST
   reset — without the reset the Retry-After fragments append onto the
   mangled "X-Wave-EmptyRetry-After" name. */
TEST(TestStreamsClient, TestCompletionCarriesHeaders) {
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(
    "HTTP/1.1 429 Too Many Requests\r\n"
    "X-Wave-Empty:\r\n"
    "Retry-After: 3\r\n"
    "X-Wave-Test-Header: wave-wave\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{}", rec));
  EXPECT_EQ(rec.status, 429);
  ASSERT_FALSE(rec.headers_null);
  ASSERT_NE(rec.headers, nullptr);
  const char* ra = http_headers_get(rec.headers, "retry-after");
  ASSERT_NE(ra, nullptr);
  EXPECT_STREQ(ra, "3");
  /* the empty-valued pair vanished entirely (empty values commit as a skip)
     and NO mangled merge of its name into the next field is present; the
     count is the strongest pin — the wire carried 5 headers, one skipped */
  EXPECT_EQ(http_headers_get(rec.headers, "x-wave-empty"), nullptr);
  EXPECT_EQ(http_headers_get(rec.headers, "x-wave-emptyretry-after"), nullptr);
  EXPECT_EQ(http_headers_count(rec.headers), 4);
  /* the SECOND, arbitrary header arrived too — the capture is general, not
     a Retry-After whitelist (neither name matched the wire's case) */
  const char* other = http_headers_get(rec.headers, "X-WAVE-TEST-HEADER");
  ASSERT_NE(other, nullptr);
  EXPECT_STREQ(other, "wave-wave");
  /* the transport framing headers ride along as well */
  EXPECT_NE(http_headers_get(rec.headers, "connection"), nullptr);
  EXPECT_TRUE(rec.error_null);      /* 429 is a pass-through, not an error */
  EXPECT_EQ(rec.fire_count, 1);
  /* rec.headers dies with the record (deinit + frees) — one owner per
     delivery, none left behind */
}

/* A value past the 512-byte capture cap truncates loud-but-continuing (the
   one-shot truncation log fires here — hdr_warned idiom): the pair still
   commits its truncated value, and the request completes normally. */
TEST(TestStreamsClient, TestOversizedHeaderValueTruncatesGracefully) {
  std::string canned =
    "HTTP/1.1 200 OK\r\n"
    "X-Wave-Big: " + std::string(600, 'x') + "\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{}";
  completion_record rec;
  ASSERT_TRUE(canned_roundtrip(canned, rec));
  EXPECT_EQ(rec.status, 200);
  ASSERT_FALSE(rec.headers_null);
  ASSERT_NE(rec.headers, nullptr);
  const char* big_val = http_headers_get(rec.headers, "x-wave-big");
  ASSERT_NE(big_val, nullptr);
  EXPECT_EQ(strlen(big_val), (size_t)512);
  EXPECT_EQ(http_headers_count(rec.headers), 3);
  EXPECT_TRUE(rec.error_null);
  EXPECT_EQ(rec.fire_count, 1);
}

/* A header value that never ends: the wire keeps coming long past any sane
   header block. http-parser's own header-bound catches the flood (HPE_HEADER
_OVERFLOW) long before the 64 MiB read cap — the pin is the REJECTION (a
   transport failure, no body), not which bound fired or at what byte. */
static void flood_server_run(int listen_fd) {
  signal(SIGPIPE, SIG_IGN);   /* the client closes mid-flood; that is the test */
  int client_fd = accept(listen_fd, NULL, NULL);
  if (client_fd < 0) return;
  const char* head = "HTTP/1.1 200 OK\r\nX-Flood: ";
  size_t head_len = strlen(head);
  size_t head_sent = 0;
  while (head_sent < head_len) {
    ssize_t w = send(client_fd, head + head_sent, head_len - head_sent,
                     MSG_NOSIGNAL);
    if (w <= 0) break;
    head_sent += (size_t)w;
  }
  std::string blob(65536, 'a');
  while (true) {
    ssize_t w = send(client_fd, blob.data(), blob.size(), MSG_NOSIGNAL);
    if (w <= 0) break;   /* the client's rejection closed the socket */
  }
  close(client_fd);
}

TEST(TestStreamsClient, TestNeverEndingRawStreamIsRejected) {
  uint16_t port = 0;
  int listen_fd = fake_server_listen(&port);
  ASSERT_GE(listen_fd, 0);
  std::thread server(flood_server_run, listen_fd);

  streams_loop_thread_t* lt = streams_loop_create();
  ASSERT_NE(lt, nullptr);
  http_client_t* c = http_client_create(lt);
  ASSERT_NE(c, nullptr);
  completion_record rec;
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/x";
  int rc = http_client_submit(c, url.c_str(), NULL, "{}", 15000,
                              completion_record_on, &rec);
  ASSERT_EQ(rc, 0);
  ASSERT_TRUE(wait_completion(&rec, 30000));

  EXPECT_EQ(rec.status, -1);          /* a flood is a transport failure */
  EXPECT_TRUE(rec.body_null);         /* nothing ever assembles */
  EXPECT_FALSE(rec.error_null);
  EXPECT_FALSE(rec.error.empty());
  EXPECT_EQ(rec.fire_count, 1);       /* exactly one completion, then done */

  server.join();
  http_client_destroy(c);
  streams_loop_destroy(lt);
}