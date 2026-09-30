//
// Created by victor on 9/30/26.
//

#include <gtest/gtest.h>
#include <atomic>
#include <string>

extern "C" {
#include "../src/Streams/http_server.h"
#include "../src/Streams/http_request.h"
#include "../src/Streams/http_response.h"
#include "../src/Streams/cors.h"
#include "../src/Streams/auth_middleware.h"
#include "../src/Scheduler/scheduler.h"
#include "../src/Util/bcrypt.h"
}

/* Loopback proofs for the ported express-like server core: bind, route, parse
   a request, run middleware, write a response — against the real liboffs
   surface (http_server_create/listen/get/use + the ported cors and auth
   middlewares), driven by its own poll-dancer I/O thread and schedulers.

   Raw-socket clients talking plain HTTP/1.1, servers on 127.0.0.1 ephemeral
   ports (house idiom from test_http.cpp: POSIX sockets directly in test
   code; the RUNTIME uses the ported server only). Every request carries
   "Connection: close" so the server ends each exchange by closing — the
   client reads to EOF and gets a bounded, deterministic conversation. */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* The shared route handler: stamps 200 + "ok", bumps the dispatch counter so
   tests can prove a handler ran (or notably did not). */
extern "C" void _stream_test_ok_handler(http_request_t* request,
                                        http_response_t* response,
                                        void* user_data) {
  (void)request;
  if (user_data != NULL) {
    ((std::atomic<int>*)user_data)->fetch_add(1);
  }
  http_response_set_status(response, 200);
  http_response_set_header(response, "Content-Type", "text/plain");
  http_response_write(response, "ok", 2);
  http_response_end(response);
}

/* Signature shims for http_server_use's void(*)(void*) destroy slot: the
   ported middlewares' destructors take their concrete type (liboffs casts
   the pointer at the call site; a shim keeps the C++ call well-defined). */
static void _cors_config_destroy_vp(void* config) {
  cors_config_destroy((cors_config_t*)config);
}

static void _auth_middleware_destroy_vp(void* auth) {
  auth_middleware_destroy((auth_middleware_t*)auth);
}

/* Reads back the ephemeral port the server bound (create uses port 0). Both
   sockaddr_in and sockaddr_in6 carry the port in the same first two bytes of
   sa_data, but read it per-family to stay honest about the layout. */
static uint16_t _bound_port(const http_server_t* server) {
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  if (getsockname(platform_socket_fd(server->listen_sock),
                  (struct sockaddr*)&ss, &len) != 0) {
    return 0;
  }
  if (ss.ss_family == AF_INET6) {
    return ntohs(((struct sockaddr_in6*)&ss)->sin6_port);
  }
  return ntohs(((struct sockaddr_in*)&ss)->sin_port);
}

static int _client_connect(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

/* Sends one raw request, then reads to EOF (the server closes every
   "Connection: close" exchange; a parse-failure close also shows up as EOF).
   Returns the raw response bytes (possibly empty for a bare close). */
static std::string _round_trip(int fd, const std::string& request) {
  size_t sent = 0;
  while (sent < request.size()) {
    ssize_t w = send(fd, request.data() + sent, request.size() - sent, 0);
    if (w <= 0) break;
    sent += (size_t)w;
  }
  std::string reply;
  char buf[2048];
  while (reply.size() < 64 * 1024) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    reply.append(buf, (size_t)n);
  }
  return reply;
}

class TestStreamsServer : public ::testing::Test {
 protected:
  void SetUp() override {
    pool_ = scheduler_pool_create(2);
    ASSERT_NE(pool_, nullptr);
    scheduler_pool_start(pool_);
  }

  void TearDown() override {
    if (server_ != NULL) {
      http_server_stop(server_);
      http_server_destroy(server_);
      server_ = NULL;
    }
    if (pool_ != NULL) {
      scheduler_pool_stop(pool_);
      scheduler_pool_destroy(pool_);
      pool_ = NULL;
    }
  }

  /* Binds a fresh server to 127.0.0.1 with an ephemeral port and records the
     port. Routes/middlewares must be registered BEFORE ListenServer(). */
  void CreateServer() {
    ASSERT_EQ(server_, nullptr);
    server_ = http_server_create(pool_, "127.0.0.1", 0);
    ASSERT_NE(server_, nullptr);
    port_ = _bound_port(server_);
    ASSERT_NE(port_, 0);
  }

  void ListenServer() {
    ASSERT_NE(server_, nullptr);
    http_server_listen(server_);
  }

  /* Connect + one raw exchange + close. */
  std::string Exchange(const std::string& request) {
    int fd = _client_connect(port_);
    EXPECT_GE(fd, 0);
    if (fd < 0) return "";
    std::string reply = _round_trip(fd, request);
    close(fd);
    return reply;
  }

  scheduler_pool_t* pool_ = nullptr;
  http_server_t* server_ = nullptr;
  uint16_t port_ = 0;
  std::atomic<int> hits_{0};   /* pre-C++20 atomic default-ctor leaves it uninitialized */
};

/* Bind + route + request + response: GET /hello dispatches the handler and
   comes back with an honest 200 framing (status line, typed body, stamped
   Content-Length, close semantics). */
TEST_F(TestStreamsServer, TestHelloRouteRoundTrip) {
  CreateServer();
  http_server_get(server_, "^/hello$", _stream_test_ok_handler, &hits_);
  ListenServer();

  std::string reply =
      Exchange("GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");

  EXPECT_EQ(hits_.load(), 1);
  EXPECT_NE(reply.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(reply.find("Content-Type: text/plain"), std::string::npos);
  EXPECT_NE(reply.find("Content-Length: 2"), std::string::npos);
  EXPECT_NE(reply.find("Connection: close"), std::string::npos);
  size_t body_pos = reply.find("\r\n\r\n");
  ASSERT_NE(body_pos, std::string::npos);
  EXPECT_EQ(reply.substr(body_pos + 4), "ok");
}

/* No route matches the path: the dispatch layer answers 404 itself, without
   ever reaching a handler. */
TEST_F(TestStreamsServer, TestUnknownRouteReturns404) {
  CreateServer();
  http_server_get(server_, "^/hello$", _stream_test_ok_handler, &hits_);
  ListenServer();

  std::string reply =
      Exchange("GET /goodbye HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");

  EXPECT_EQ(hits_.load(), 0);
  EXPECT_NE(reply.find("HTTP/1.1 404 Not Found"), std::string::npos);
}

/* Malformed request line: the connection-layer parse fails and the ported
   (liboffs-real) semantic is a bare connection close — no response bytes, no
   dispatch. The client sees EOF with nothing that parses. */
TEST_F(TestStreamsServer, TestMalformedRequestClosesConnection) {
  CreateServer();
  http_server_get(server_, "^/hello$", _stream_test_ok_handler, &hits_);
  ListenServer();

  std::string reply =
      Exchange("\x7f\x7fgarbage\r\n\r\n");

  EXPECT_EQ(hits_.load(), 0);
  EXPECT_EQ(reply.find("HTTP/1.1"), std::string::npos);
}

/* cors middleware: OPTIONS preflight is answered at the middleware layer
   (204 + the allow headers, chain stopped, handler never runs) and the CORS
   origin header rides every other response too. */
TEST_F(TestStreamsServer, TestCorsPreflightAndHeader) {
  CreateServer();
  cors_config_t* cors = cors_config_default();
  http_server_use(server_, cors_middleware, cors, _cors_config_destroy_vp);
  http_server_get(server_, "^/hello$", _stream_test_ok_handler, &hits_);
  ListenServer();

  std::string preflight =
      Exchange("OPTIONS /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
  EXPECT_EQ(hits_.load(), 0);
  EXPECT_NE(preflight.find("HTTP/1.1 204 No Content"), std::string::npos);
  EXPECT_NE(preflight.find("Access-Control-Allow-Origin: *"), std::string::npos);
  EXPECT_NE(preflight.find("Access-Control-Allow-Methods"), std::string::npos);
  EXPECT_NE(preflight.find("Access-Control-Max-Age"), std::string::npos);

  std::string reply =
      Exchange("GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
  EXPECT_EQ(hits_.load(), 1);
  EXPECT_NE(reply.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(reply.find("Access-Control-Allow-Origin: *"), std::string::npos);
  EXPECT_EQ(reply.substr(reply.find("\r\n\r\n") + 4), "ok");
}

/* auth middleware, opt-out case: allow_local_no_auth on a loopback binding
   lets a bearer-less request through to the route. */
TEST_F(TestStreamsServer, TestAuthLoopbackOptOut) {
  char hash[64];
  ASSERT_EQ(bcrypt_generate("secret-token", 4, hash, sizeof(hash)), 0);
  CreateServer();
  auth_middleware_t* auth = auth_middleware_create(hash, true, server_);
  ASSERT_NE(auth, nullptr);
  http_server_use(server_, auth_middleware_handler(), auth,
                  _auth_middleware_destroy_vp);
  http_server_get(server_, "^/hello$", _stream_test_ok_handler, &hits_);
  ListenServer();

  std::string reply =
      Exchange("GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");

  EXPECT_EQ(hits_.load(), 1);
  EXPECT_NE(reply.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_EQ(reply.substr(reply.find("\r\n\r\n") + 4), "ok");
}

/* auth middleware, bearer enforced: a missing Authorization is 401 with the
   Bearer challenge, a wrong token fails the bcrypt check as 403, and the
   matching Bearer reaches the route for 200. */
TEST_F(TestStreamsServer, TestAuthBearerEnforced) {
  char hash[64];
  ASSERT_EQ(bcrypt_generate("secret-token", 4, hash, sizeof(hash)), 0);
  CreateServer();
  auth_middleware_t* auth = auth_middleware_create(hash, false, server_);
  ASSERT_NE(auth, nullptr);
  http_server_use(server_, auth_middleware_handler(), auth,
                  _auth_middleware_destroy_vp);
  http_server_get(server_, "^/hello$", _stream_test_ok_handler, &hits_);
  ListenServer();

  std::string missing =
      Exchange("GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
  EXPECT_EQ(hits_.load(), 0);
  EXPECT_NE(missing.find("HTTP/1.1 401 Unauthorized"), std::string::npos);
  EXPECT_NE(missing.find("WWW-Authenticate: Bearer"), std::string::npos);

  std::string wrong =
      Exchange("GET /hello HTTP/1.1\r\nHost: test\r\n"
               "Authorization: Bearer not-the-token\r\nConnection: close\r\n\r\n");
  EXPECT_EQ(hits_.load(), 0);
  EXPECT_NE(wrong.find("HTTP/1.1 403 Forbidden"), std::string::npos);

  std::string good =
      Exchange("GET /hello HTTP/1.1\r\nHost: test\r\n"
               "Authorization: Bearer secret-token\r\nConnection: close\r\n\r\n");
  EXPECT_EQ(hits_.load(), 1);
  EXPECT_NE(good.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_EQ(good.substr(good.find("\r\n\r\n") + 4), "ok");
}