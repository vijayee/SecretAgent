#define _DEFAULT_SOURCE

#include "platform_socket.h"
#include "platform_socket_internal.h"

#ifndef _WIN32
  #include <stdlib.h>
  #include <stdio.h>
  #include <string.h>
  #include <unistd.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <net/if.h>
  #include <ctype.h>
  #include <fcntl.h>
  #include <errno.h>

  static int _family_to_native(platform_address_family_e family) {
    switch (family) {
      case PLATFORM_AF_INET:  return AF_INET;
      case PLATFORM_AF_INET6: return AF_INET6;
      case PLATFORM_AF_LOCAL: return AF_LOCAL;
      default: return -1;
    }
  }

  platform_socket_t* platform_socket_create(platform_address_family_e family, int stream) {
    int native_family = _family_to_native(family);
    if (native_family < 0) {
      return NULL;
    }
    int type = stream ? SOCK_STREAM : SOCK_DGRAM;
    int fd = socket(native_family, type, 0);
    if (fd < 0) {
      return NULL;
    }
    platform_socket_t* sock = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (sock == NULL) {
      close(fd);
      return NULL;
    }
    sock->fd = fd;
    sock->family = family;
    return sock;
  }

  void platform_socket_destroy(platform_socket_t* sock) {
    if (sock == NULL) {
      return;
    }
    close(sock->fd);
    free(sock);
  }

  int platform_socket_fd(platform_socket_t* sock) {
    return sock != NULL ? sock->fd : -1;
  }

  int platform_socket_is_pipe(platform_socket_t* sock) {
    if (sock == NULL) return -1;
    /* POSIX builds never have a pipe-backed platform_socket_t; the
     * Windows-specific is_pipe field is always zero. */
    return 0;
  }

  void* platform_socket_handle(platform_socket_t* sock) {
    (void)sock;
    /* POSIX sockets are accessed via platform_socket_fd; this accessor
     * only matters on Windows. */
    return NULL;
  }

  static int _address_to_native(const platform_address_t* addr,
                                 struct sockaddr_storage* storage, socklen_t* addrlen) {
    memset(storage, 0, sizeof(*storage));
    switch (addr->family) {
      case PLATFORM_AF_INET: {
        struct sockaddr_in* sin = (struct sockaddr_in*)storage;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(addr->inet.port);
        sin->sin_addr.s_addr = addr->inet.addr;
        *addrlen = sizeof(*sin);
        return 0;
      }
      case PLATFORM_AF_INET6: {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)storage;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(addr->inet6.port);
        memcpy(&sin6->sin6_addr, addr->inet6.addr, 16);
        sin6->sin6_scope_id = addr->inet6.scope_id;
        *addrlen = sizeof(*sin6);
        return 0;
      }
      case PLATFORM_AF_LOCAL: {
        struct sockaddr_un* sun = (struct sockaddr_un*)storage;
        sun->sun_family = AF_LOCAL;
        strncpy(sun->sun_path, addr->local.path, sizeof(sun->sun_path) - 1);
        sun->sun_path[sizeof(sun->sun_path) - 1] = '\0';
        *addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                               + strlen(sun->sun_path) + 1);
        return 0;
      }
    }
    return -1;
  }

  int platform_socket_bind(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return bind(sock->fd, (struct sockaddr*)&storage, addrlen);
  }

  platform_socket_t* platform_listen_socket_create(const char* host, uint16_t port,
                                                   platform_address_t* out_addr) {
    platform_address_family_e sock_family = PLATFORM_AF_INET6;
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET6, 1);
    if (sock != NULL) {
      /* IPV6_V6ONLY=0 makes the socket dual-stack: it serves both IPv4
       * (as v4-mapped connections) and IPv6 on the one listener. */
      int off = 0;
      (void)setsockopt(sock->fd, IPPROTO_IPV6, IPV6_V6ONLY,
                       (const char*)&off, sizeof(off));
    } else {
      /* No IPv6 support on this host: bind a plain IPv4 listener. */
      sock_family = PLATFORM_AF_INET;
      sock = platform_socket_create(PLATFORM_AF_INET, 1);
    }
    if (sock != NULL) {
      /* Set before bind: SO_REUSEADDR only takes effect when applied
       * prior to bind(), so the helper owns it. */
      (void)platform_socket_set_reuseaddr(sock);
    }
    if (sock == NULL) {
      return NULL;
    }

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    if (platform_address_parse(&addr, host, port) != 0) {
      /* Unparsable host (or plain hostname): bind the wildcard. */
      memset(&addr, 0, sizeof(addr));
      addr.family = sock_family;
      if (sock_family == PLATFORM_AF_INET6) {
        addr.inet6.port = port;
      } else {
        addr.inet.port = port;
      }
    }
    if (addr.family == PLATFORM_AF_INET && sock_family == PLATFORM_AF_INET6) {
      /* v4 host on a dual-stack socket: bind the v4-mapped address so both
       * families are served on one socket. inet.addr already holds the
       * address bytes in network order, so copy them straight in. The
       * union overlap means inet.port lives inside inet6.addr[4..5], so
       * the whole byte array must be cleared first. */
      uint8_t v4_bytes[4];
      memcpy(v4_bytes, &addr.inet.addr, sizeof(v4_bytes));
      memset(addr.inet6.addr, 0, sizeof(addr.inet6.addr));
      addr.inet6.addr[10] = 0xFF;
      addr.inet6.addr[11] = 0xFF;
      memcpy(&addr.inet6.addr[12], v4_bytes, sizeof(v4_bytes));
      addr.inet6.port = port;
      addr.family = PLATFORM_AF_INET6;
    }
    if (addr.family != sock_family) {
      fprintf(stderr, "listen_socket: host '%s' requires an unsupported family\n",
              host);
      platform_socket_destroy(sock);
      return NULL;
    }
    if (platform_socket_bind(sock, &addr) < 0) {
      if (sock_family == PLATFORM_AF_INET6) {
        /* Host without IPv6 (or v6 bind refused): fall back to an IPv4
         * bind, once, with a warning. */
        fprintf(stderr, "listen_socket: IPv6 bind failed for %s:%u, "
                        "falling back to IPv4\n", host, (unsigned)port);
        platform_socket_destroy(sock);
        sock_family = PLATFORM_AF_INET;
        sock = platform_socket_create(PLATFORM_AF_INET, 1);
        if (sock == NULL) {
          return NULL;
        }
        (void)platform_socket_set_reuseaddr(sock);
        memset(&addr, 0, sizeof(addr));
        addr.family = PLATFORM_AF_INET;
        addr.inet.port = port;
        if (platform_address_parse(&addr, host, port) != 0) {
          addr.inet.addr = 0;
          addr.inet.port = port;
        }
        if (addr.family != PLATFORM_AF_INET ||
            platform_socket_bind(sock, &addr) < 0) {
          fprintf(stderr, "listen_socket: IPv4 fallback bind failed for %s:%u\n",
                  host, (unsigned)port);
          platform_socket_destroy(sock);
          return NULL;
        }
      } else {
        fprintf(stderr, "listen_socket: bind failed for %s:%u\n",
                host, (unsigned)port);
        platform_socket_destroy(sock);
        return NULL;
      }
    }
    if (out_addr != NULL) {
      *out_addr = addr;
    }
    return sock;
  }

  int platform_socket_listen(platform_socket_t* sock, int backlog) {
    return listen(sock->fd, backlog);
  }

  platform_socket_t* platform_socket_accept(platform_socket_t* sock, platform_address_t* remote) {
    struct sockaddr_storage storage;
    socklen_t addrlen = sizeof(storage);
    int client_fd = accept(sock->fd, (struct sockaddr*)&storage, &addrlen);
    if (client_fd < 0) {
      return NULL;
    }
    platform_socket_t* client = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (client == NULL) {
      close(client_fd);
      return NULL;
    }
    client->fd = client_fd;
    client->family = sock->family;
    if (remote != NULL) {
      memset(remote, 0, sizeof(*remote));
      remote->family = sock->family;
    }
    return client;
  }

  int platform_socket_connect(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return connect(sock->fd, (struct sockaddr*)&storage, addrlen);
  }

  int platform_socket_set_nonblocking(platform_socket_t* sock) {
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags < 0) {
      return -1;
    }
    return fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);
  }

  int platform_socket_set_reuseaddr(platform_socket_t* sock) {
    int opt = 1;
    return setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  }

  ssize_t platform_socket_send(platform_socket_t* sock, const void* buf, size_t len) {
  #ifdef __APPLE__
    return send(sock->fd, buf, len, 0);
  #else
    return send(sock->fd, buf, len, MSG_NOSIGNAL);
  #endif
  }

  ssize_t platform_socket_recv(platform_socket_t* sock, void* buf, size_t len) {
    return recv(sock->fd, buf, len, 0);
  }

  int platform_socket_shutdown(platform_socket_t* sock, int how) {
    int native_how;
    switch (how) {
      case PLATFORM_SHUT_RD:   native_how = SHUT_RD; break;
      case PLATFORM_SHUT_WR:   native_how = SHUT_WR; break;
      case PLATFORM_SHUT_RDWR: native_how = SHUT_RDWR; break;
      default: return -1;
    }
    return shutdown(sock->fd, native_how);
  }

  int platform_address_parse(platform_address_t* addr, const char* host, uint16_t port) {
    memset(addr, 0, sizeof(*addr));
    struct in_addr in4;
    if (inet_pton(AF_INET, host, &in4) == 1) {
      addr->family = PLATFORM_AF_INET;
      addr->inet.addr = in4.s_addr;
      addr->inet.port = port;
      return 0;
    }
    /* Optional RFC 6874-style zone on a v6 literal: %ifname or %index.
     * inet_pton rejects the suffix, so split it off before parsing. */
    const char* zone = strchr(host, '%');
    size_t literal_len = (zone != NULL) ? (size_t)(zone - host) : strlen(host);
    if (literal_len == 0 || literal_len >= 64) return -1;
    char literal[64];
    memcpy(literal, host, literal_len);
    literal[literal_len] = '\0';
    struct in6_addr in6;
    if (inet_pton(AF_INET6, literal, &in6) == 1) {
      addr->family = PLATFORM_AF_INET6;
      memcpy(addr->inet6.addr, &in6, 16);
      addr->inet6.port = port;
      if (zone != NULL) {
        const char* zone_text = zone + 1;
        if (*zone_text == '\0') return -1;
        int numeric = 1;
        for (const char* scan = zone_text; *scan != '\0'; scan++) {
          if (!isdigit((unsigned char)*scan)) {
            numeric = 0;
            break;
          }
        }
        if (numeric) {
          errno = 0;
          unsigned long index = strtoul(zone_text, NULL, 10);
          if (errno == ERANGE || index == 0 || index > 0xFFFFFFFFul) return -1;
          addr->inet6.scope_id = (uint32_t)index;
        } else {
          unsigned int index = if_nametoindex(zone_text);
          if (index == 0) return -1;
          addr->inet6.scope_id = (uint32_t)index;
        }
      }
      return 0;
    }
    return -1;
  }

  int platform_address_to_string(const platform_address_t* addr, char* buf, size_t len) {
    switch (addr->family) {
      case PLATFORM_AF_INET:
        if (inet_ntop(AF_INET, &addr->inet.addr, buf, (socklen_t)len) == NULL) {
          return -1;
        }
        return 0;
      case PLATFORM_AF_INET6: {
        char ip[64];
        if (inet_ntop(AF_INET6, addr->inet6.addr, ip, sizeof(ip)) == NULL) {
          return -1;
        }
        if (addr->inet6.scope_id == 0) {
          int written = snprintf(buf, len, "%s", ip);
          return (written > 0 && (size_t)written < len) ? 0 : -1;
        }
        char ifname[64];
        int written;
        if (if_indextoname(addr->inet6.scope_id, ifname) != NULL) {
          written = snprintf(buf, len, "%s%%%s", ip, ifname);
        } else {
          written = snprintf(buf, len, "%s%%%u", ip,
                             (unsigned)addr->inet6.scope_id);
        }
        return (written > 0 && (size_t)written < len) ? 0 : -1;
      }
      case PLATFORM_AF_LOCAL:
        strncpy(buf, addr->local.path, len);
        if (len > 0) {
          buf[len - 1] = '\0';
        }
        return 0;
    }
    return -1;
  }
#else
  /* Windows implementation — Winsock2 */
  #include <winsock2.h>
  #include <windows.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #include <afunix.h>

  #pragma comment(lib, "iphlpapi.lib")
  #include <stdlib.h>
  #include <stdio.h>
  #include <string.h>
  #include <ctype.h>
  #include <errno.h>

  #include "platform_internal.h"

  static int _family_to_native(platform_address_family_e family) {
    switch (family) {
      case PLATFORM_AF_INET:  return AF_INET;
      case PLATFORM_AF_INET6: return AF_INET6;
      case PLATFORM_AF_LOCAL: return _platform_has_af_unix() ? AF_UNIX : -1;
      default: return -1;
    }
  }

  platform_socket_t* platform_socket_create(platform_address_family_e family, int stream) {
    if (_platform_winsock_init() != 0) {
      return NULL;
    }
    int native_family = _family_to_native(family);
    if (native_family < 0) {
      return NULL;
    }
    int type = stream ? SOCK_STREAM : SOCK_DGRAM;
    SOCKET fd = socket(native_family, type, 0);
    if (fd == INVALID_SOCKET) {
      return NULL;
    }
    platform_socket_t* sock = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (sock == NULL) {
      closesocket(fd);
      return NULL;
    }
    sock->fd = fd;
    sock->handle = NULL;
    sock->family = family;
    sock->is_pipe = 0;
    sock->owns_handle = 0;
    return sock;
  }

  void platform_socket_destroy(platform_socket_t* sock) {
    if (sock == NULL) {
      return;
    }
    if (sock->is_pipe) {
      /* For pipe-backed sockets, only close the handle if we own it.
       * The accept path returns a wrapper around the listener's
       * HANDLE; the listener keeps the HANDLE alive. */
      if (sock->owns_handle &&
          sock->handle != NULL &&
          sock->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(sock->handle);
      }
    } else {
      closesocket(sock->fd);
    }
    if (sock->pipe_name != NULL) {
      free(sock->pipe_name);
    }
    free(sock);
  }

  int platform_socket_fd(platform_socket_t* sock) {
    if (sock == NULL) return -1;
    /* For pipe-backed sockets, return an opaque non-negative tag derived
     * from the handle pointer. The value is meaningful only to callers
     * that pair it with platform_socket_is_pipe to choose the right
     * watcher API (pd_watcher_create vs pd_watcher_create_for_handle). */
    if (sock->is_pipe) {
      return (int)(intptr_t)sock->handle;
    }
    return (int)(intptr_t)sock->fd;
  }

  int platform_socket_is_pipe(platform_socket_t* sock) {
    if (sock == NULL) return -1;
    return sock->is_pipe ? 1 : 0;
  }

  void* platform_socket_handle(platform_socket_t* sock) {
    if (sock == NULL) return NULL;
    /* Return the HANDLE for pipe-backed sockets. For non-pipe sockets
     * this returns INVALID_HANDLE_VALUE so callers can detect the
     * mismatch; they should use platform_socket_fd for non-pipe paths. */
    if (!sock->is_pipe) {
      return (void*)INVALID_HANDLE_VALUE;
    }
    return (void*)sock->handle;
  }

  static int _address_to_native(const platform_address_t* addr,
                                 struct sockaddr_storage* storage, socklen_t* addrlen) {
    memset(storage, 0, sizeof(*storage));
    switch (addr->family) {
      case PLATFORM_AF_INET: {
        struct sockaddr_in* sin = (struct sockaddr_in*)storage;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(addr->inet.port);
        sin->sin_addr.s_addr = addr->inet.addr;
        *addrlen = sizeof(*sin);
        return 0;
      }
      case PLATFORM_AF_INET6: {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)storage;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(addr->inet6.port);
        memcpy(&sin6->sin6_addr, addr->inet6.addr, 16);
        sin6->sin6_scope_id = addr->inet6.scope_id;
        *addrlen = sizeof(*sin6);
        return 0;
      }
      case PLATFORM_AF_LOCAL: {
        if (!_platform_has_af_unix()) {
          return -1;
        }
        struct sockaddr_un* sun = (struct sockaddr_un*)storage;
        sun->sun_family = AF_UNIX;
        strncpy(sun->sun_path, addr->local.path, sizeof(sun->sun_path) - 1);
        sun->sun_path[sizeof(sun->sun_path) - 1] = '\0';
        *addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                               + strlen(sun->sun_path) + 1);
        return 0;
      }
    }
    return -1;
  }

  int platform_socket_bind(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return bind(sock->fd, (struct sockaddr*)&storage, addrlen) == 0 ? 0 : -1;
  }

  platform_socket_t* platform_listen_socket_create(const char* host, uint16_t port,
                                                   platform_address_t* out_addr) {
    platform_address_family_e sock_family = PLATFORM_AF_INET6;
    platform_socket_t* sock = platform_socket_create(PLATFORM_AF_INET6, 1);
    if (sock != NULL) {
      /* IPV6_V6ONLY=0 makes the socket dual-stack: it serves both IPv4
       * (as v4-mapped connections) and IPv6 on the one listener. */
      DWORD off = 0;
      (void)setsockopt(sock->fd, IPPROTO_IPV6, IPV6_V6ONLY,
                       (const char*)&off, sizeof(off));
    } else {
      /* No IPv6 support on this host: bind a plain IPv4 listener. */
      sock_family = PLATFORM_AF_INET;
      sock = platform_socket_create(PLATFORM_AF_INET, 1);
    }
    if (sock != NULL) {
      /* Set before bind: SO_REUSEADDR only takes effect when applied
       * prior to bind(), so the helper owns it. */
      (void)platform_socket_set_reuseaddr(sock);
    }
    if (sock == NULL) {
      return NULL;
    }

    platform_address_t addr;
    memset(&addr, 0, sizeof(addr));
    if (platform_address_parse(&addr, host, port) != 0) {
      /* Unparsable host (or plain hostname): bind the wildcard. */
      memset(&addr, 0, sizeof(addr));
      addr.family = sock_family;
      if (sock_family == PLATFORM_AF_INET6) {
        addr.inet6.port = port;
      } else {
        addr.inet.port = port;
      }
    }
    if (addr.family == PLATFORM_AF_INET && sock_family == PLATFORM_AF_INET6) {
      /* v4 host on a dual-stack socket: bind the v4-mapped address so both
       * families are served on one socket. inet.addr already holds the
       * address bytes in network order, so copy them straight in. The
       * union overlap means inet.port lives inside inet6.addr[4..5], so
       * the whole byte array must be cleared first. */
      uint8_t v4_bytes[4];
      memcpy(v4_bytes, &addr.inet.addr, sizeof(v4_bytes));
      memset(addr.inet6.addr, 0, sizeof(addr.inet6.addr));
      addr.inet6.addr[10] = 0xFF;
      addr.inet6.addr[11] = 0xFF;
      memcpy(&addr.inet6.addr[12], v4_bytes, sizeof(v4_bytes));
      addr.inet6.port = port;
      addr.family = PLATFORM_AF_INET6;
    }
    if (addr.family != sock_family) {
      fprintf(stderr, "listen_socket: host '%s' requires an unsupported family\n",
              host);
      platform_socket_destroy(sock);
      return NULL;
    }
    if (platform_socket_bind(sock, &addr) < 0) {
      if (sock_family == PLATFORM_AF_INET6) {
        /* Host without IPv6 (or v6 bind refused): fall back to an IPv4
         * bind, once, with a warning. */
        fprintf(stderr, "listen_socket: IPv6 bind failed for %s:%u, "
                        "falling back to IPv4\n", host, (unsigned)port);
        platform_socket_destroy(sock);
        sock_family = PLATFORM_AF_INET;
        sock = platform_socket_create(PLATFORM_AF_INET, 1);
        if (sock == NULL) {
          return NULL;
        }
        (void)platform_socket_set_reuseaddr(sock);
        memset(&addr, 0, sizeof(addr));
        addr.family = PLATFORM_AF_INET;
        addr.inet.port = port;
        if (platform_address_parse(&addr, host, port) != 0) {
          addr.inet.addr = 0;
          addr.inet.port = port;
        }
        if (addr.family != PLATFORM_AF_INET ||
            platform_socket_bind(sock, &addr) < 0) {
          fprintf(stderr, "listen_socket: IPv4 fallback bind failed for %s:%u\n",
                  host, (unsigned)port);
          platform_socket_destroy(sock);
          return NULL;
        }
      } else {
        fprintf(stderr, "listen_socket: bind failed for %s:%u\n",
                host, (unsigned)port);
        platform_socket_destroy(sock);
        return NULL;
      }
    }
    if (out_addr != NULL) {
      *out_addr = addr;
    }
    return sock;
  }

  int platform_socket_listen(platform_socket_t* sock, int backlog) {
    return listen(sock->fd, backlog) == 0 ? 0 : -1;
  }

  platform_socket_t* platform_socket_accept(platform_socket_t* sock, platform_address_t* remote) {
    struct sockaddr_storage storage;
    socklen_t addrlen = sizeof(storage);
    SOCKET client_fd = accept(sock->fd, (struct sockaddr*)&storage, &addrlen);
    if (client_fd == INVALID_SOCKET) {
      return NULL;
    }
    platform_socket_t* client = (platform_socket_t*)calloc(1, sizeof(platform_socket_t));
    if (client == NULL) {
      closesocket(client_fd);
      return NULL;
    }
    client->fd = client_fd;
    client->family = sock->family;
    if (remote != NULL) {
      memset(remote, 0, sizeof(*remote));
      remote->family = sock->family;
    }
    return client;
  }

  int platform_socket_connect(platform_socket_t* sock, const platform_address_t* addr) {
    struct sockaddr_storage storage;
    socklen_t addrlen;
    if (_address_to_native(addr, &storage, &addrlen) != 0) {
      return -1;
    }
    return connect(sock->fd, (struct sockaddr*)&storage, addrlen) == 0 ? 0 : -1;
  }

  int platform_socket_set_nonblocking(platform_socket_t* sock) {
    /* For Winsock SOCKETs, switch to non-blocking so recv() returns
     * WSAEWOULDBLOCK instead of blocking. For named pipes, the handle
     * stays PIPE_WAIT + FILE_FLAG_OVERLAPPED (the overlapped watcher and
     * handshake paths depend on it); send/recv implement non-blocking
     * semantics themselves from this flag, so just record it. */
    if (sock == NULL) return -1;
    if (sock->is_pipe) {
      sock->nonblocking = 1;
      return 0;
    }
    unsigned long mode = 1;
    return ioctlsocket(sock->fd, FIONBIO, &mode) == 0 ? 0 : -1;
  }

  int platform_socket_set_reuseaddr(platform_socket_t* sock) {
    if (sock == NULL || sock->is_pipe) return -1;
    int opt = 1;
    return setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR,
                      (const char*)&opt, sizeof(opt)) == 0 ? 0 : -1;
  }

  /* Map Winsock WSAGetLastError() codes to POSIX errno values. Winsock does
   * NOT set C errno on failure — it reports errors through WSAGetLastError()
   * — so callers that check errno (e.g. http_connection's write path testing
   * `errno != EAGAIN`) would otherwise see stale errno and treat a transient
   * WSAEWOULDBLOCK as a fatal error, closing the connection. Map the codes
   * the send/recv paths can actually surface. */
  static int _wsaerr_to_errno(int wsaerr) {
    switch (wsaerr) {
      case WSAEWOULDBLOCK:        return EAGAIN; /* == EWOULDBLOCK on Windows */
      case WSAEINTR:              return EINTR;
      case WSAECONNRESET:         return ECONNRESET;
      case WSAECONNABORTED:       return ECONNABORTED;
      case WSAESHUTDOWN:          return EPIPE;
      case WSAENETRESET:          return ENETRESET;
      case WSAENOTCONN:            return ENOTCONN;
      case WSAENOTSOCK:           return EBADF;
      case WSAEMSGSIZE:           return EMSGSIZE;
      case WSAEHOSTUNREACH:       return EHOSTUNREACH;
      case WSAENETUNREACH:        return ENETUNREACH;
      case WSAETIMEDOUT:          return ETIMEDOUT;
      case WSAECONNREFUSED:       return ECONNREFUSED;
      default:                    return EIO;
    }
  }

  /* Map a small set of common Windows error codes to POSIX errno values.
   * The full mapping would be a 100+ entry table; this covers the errors
   * we actually surface from WriteFile/ReadFile/CreateNamedPipe paths. */
  static int _winerr_to_errno(DWORD err) {
    switch (err) {
      case ERROR_BROKEN_PIPE:        return EPIPE;
      case ERROR_PIPE_NOT_CONNECTED: return EPIPE;
      case ERROR_NO_DATA:            return EAGAIN;
      case ERROR_PIPE_BUSY:          return EAGAIN;
      case ERROR_INVALID_HANDLE:     return EBADF;
      case ERROR_NOT_ENOUGH_MEMORY:  return ENOMEM;
      case ERROR_ACCESS_DENIED:      return EACCES;
      case ERROR_INVALID_PARAMETER:  return EINVAL;
      case ERROR_HANDLE_EOF:         return 0;
      case ERROR_IO_INCOMPLETE:      return EIO;
      case ERROR_IO_PENDING:         return EAGAIN;
      default:                       return EIO;
    }
  }

  /* Named-pipe handles are always created with FILE_FLAG_OVERLAPPED, so a
   * synchronous send/recv must ride a local OVERLAPPED — the
   * sync-over-async pattern. Passing a NULL lpOverlapped to WriteFile/
   * ReadFile on an overlapped handle is undefined behaviour: it can report
   * success without the bytes ever reaching the pipe. The local OVERLAPPED
   * is safe even when the handle is bound to an IOCP port (completions are
   * demuxed by the OVERLAPPED pointer), and CancelIoEx cancels exactly this
   * transfer, never a watcher's pending read on the same handle.
   *
   * Returns the transferred byte count (possibly a partial transfer, like
   * POSIX send/recv), 0 on graceful peer close, or -1 with errno set
   * (EAGAIN on a non-blocking read when no data was queued; writes block
   * until complete because a cancelled pipe write loses its byte count). */
  static ssize_t _pipe_transfer(platform_socket_t* sock, int is_write,
                                void* buf, size_t len) {
    HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (event == NULL) {
      errno = _winerr_to_errno(GetLastError());
      return -1;
    }

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = event;

    DWORD transferred = 0;
    BOOL ok = is_write ? WriteFile(sock->handle, buf, (DWORD)len, NULL, &ov)
                       : ReadFile(sock->handle, buf, (DWORD)len, NULL, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      /* Only reads honor nonblocking: PeekNamedPipe gates them above, and
       * a cancelled read reports its real byte count through
       * GetOverlappedResult. A WRITE must never be cancelled: the kernel
       * accepts a prefix into the pipe buffer but GetOverlappedResult
       * reports 0 transferred after the abort (observed: 2993 cancels each
       * injected ~4096 bytes yet all reported aborted=0), so the caller
       * cannot know how much to skip and would resend from offset 0,
       * duplicating the stream. The write's completion fires once every
       * byte is written or the pipe errors, mirroring a POSIX blocking
       * send(). */
      DWORD wait_ms = (!is_write && sock->nonblocking) ? 0 : INFINITE;
      if (WaitForSingleObject(event, wait_ms) == WAIT_TIMEOUT) {
        /* Read-only path: writes never time out (see above). The cancelled
         * read may have already pulled bytes out of the pipe before the
         * cancel landed. Those bytes are real stream data: reporting EAGAIN
         * here makes the caller retry and lose them. GetOverlappedResult
         * retrieves the final count of the aborted operation; return it as
         * a partial transfer, or EAGAIN when nothing moved. */
        CancelIoEx(sock->handle, &ov);
        DWORD aborted_bytes = 0;
        GetOverlappedResult(sock->handle, &ov, &aborted_bytes, TRUE);
        CloseHandle(event);
        if (aborted_bytes > 0) {
          return (ssize_t)aborted_bytes;
        }
        errno = EAGAIN;
        return -1;
      }
      ok = GetOverlappedResult(sock->handle, &ov, &transferred, TRUE);
    } else if (ok) {
      /* Synchronous completion still reports the count through the
       * OVERLAPPED. */
      ok = GetOverlappedResult(sock->handle, &ov, &transferred, TRUE);
    }
    CloseHandle(event);

    if (!ok) {
      DWORD err = GetLastError();
      if (err == ERROR_NO_DATA || err == ERROR_PIPE_BUSY) {
        errno = EAGAIN;
        return -1;
      }
      if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_CONNECTED) {
        /* Graceful close from peer; treat as zero bytes transferred. */
        return 0;
      }
      errno = _winerr_to_errno(err);
      return -1;
    }
    if (!is_write && transferred == 0) {
      /* Zero-byte read on a connected pipe: peer closed cleanly. */
      return 0;
    }
    return (ssize_t)transferred;
  }

  ssize_t platform_socket_send(platform_socket_t* sock, const void* buf, size_t len) {
    if (sock == NULL) return -1;
    if (sock->is_pipe) {
      return _pipe_transfer(sock, 1, (void*)buf, len);
    }
    int result = send(sock->fd, (const char*)buf, (int)len, 0);
    if (result == SOCKET_ERROR) {
      errno = _wsaerr_to_errno(WSAGetLastError());
      return -1;
    }
    return (ssize_t)result;
  }

  ssize_t platform_socket_recv(platform_socket_t* sock, void* buf, size_t len) {
    if (sock == NULL) return -1;
    if (sock->is_pipe) {
      if (sock->nonblocking) {
        /* Peek first so a drained pipe yields EAGAIN instead of a blocking
         * wait that would stall a caller's poll/timeout loop (the pipe
         * handle itself stays PIPE_WAIT). */
        DWORD available = 0;
        if (!PeekNamedPipe(sock->handle, NULL, 0, NULL, &available, NULL)) {
          DWORD err = GetLastError();
          if (err == ERROR_BROKEN_PIPE) {
            return 0;
          }
          if (err == ERROR_NO_DATA || err == ERROR_PIPE_BUSY) {
            errno = EAGAIN;
            return -1;
          }
          errno = _winerr_to_errno(err);
          return -1;
        }
        if (available == 0) {
          errno = EAGAIN;
          return -1;
        }
      }
      return _pipe_transfer(sock, 0, buf, len);
    }
    int result = recv(sock->fd, (char*)buf, (int)len, 0);
    if (result == SOCKET_ERROR) {
      errno = _wsaerr_to_errno(WSAGetLastError());
      return -1;
    }
    return (ssize_t)result;
  }

  int platform_socket_shutdown(platform_socket_t* sock, int how) {
    if (sock == NULL) return -1;
    if (sock->is_pipe) {
      /* Named pipes have no real half-close: the kernel only signals
       * EOF on the server side when the client HANDLE is closed. To
       * honour the caller's intent ("I'm done writing; the peer
       * should see EOF on the next recv") we close the client end
       * entirely. Callers that need full-duplex use must call
       * shutdown only at end-of-life. */
      if (sock->handle != NULL && sock->handle != INVALID_HANDLE_VALUE) {
        if (sock->owns_handle) {
          CloseHandle(sock->handle);
          sock->handle = INVALID_HANDLE_VALUE;
        }
      }
      return 0;
    }
    int native_how;
    switch (how) {
      case PLATFORM_SHUT_RD:   native_how = SD_RECEIVE; break;
      case PLATFORM_SHUT_WR:   native_how = SD_SEND; break;
      case PLATFORM_SHUT_RDWR: native_how = SD_BOTH; break;
      default: return -1;
    }
    return shutdown(sock->fd, native_how) == 0 ? 0 : -1;
  }

  int platform_address_parse(platform_address_t* addr, const char* host, uint16_t port) {
    memset(addr, 0, sizeof(*addr));
    struct in_addr in4;
    if (inet_pton(AF_INET, host, &in4) == 1) {
      addr->family = PLATFORM_AF_INET;
      addr->inet.addr = in4.s_addr;
      addr->inet.port = port;
      return 0;
    }
    /* Optional RFC 6874-style zone on a v6 literal: %ifname or %index.
     * inet_pton rejects the suffix, so split it off before parsing. */
    const char* zone = strchr(host, '%');
    size_t literal_len = (zone != NULL) ? (size_t)(zone - host) : strlen(host);
    if (literal_len == 0 || literal_len >= 64) return -1;
    char literal[64];
    memcpy(literal, host, literal_len);
    literal[literal_len] = '\0';
    struct in6_addr in6;
    if (inet_pton(AF_INET6, literal, &in6) == 1) {
      addr->family = PLATFORM_AF_INET6;
      memcpy(addr->inet6.addr, &in6, 16);
      addr->inet6.port = port;
      if (zone != NULL) {
        const char* zone_text = zone + 1;
        if (*zone_text == '\0') return -1;
        int numeric = 1;
        for (const char* scan = zone_text; *scan != '\0'; scan++) {
          if (!isdigit((unsigned char)*scan)) {
            numeric = 0;
            break;
          }
        }
        if (numeric) {
          errno = 0;
          unsigned long index = strtoul(zone_text, NULL, 10);
          if (errno == ERANGE || index == 0 || index > 0xFFFFFFFFul) return -1;
          addr->inet6.scope_id = (uint32_t)index;
        } else {
          /* if_nametoindex/if_indextoname/IF_NAMESIZE come from netioapi.h,
           * which is auto-included by iphlpapi.h (not ws2tcpip.h). The MSVC
           * SDK declares only the lowercase names (the MinGW header adds the
           * uppercase aliases as macros, which do not exist here). */
          unsigned long index = (unsigned long)if_nametoindex(zone_text);
          if (index == 0) return -1;
          addr->inet6.scope_id = (uint32_t)index;
        }
      }
      return 0;
    }
    return -1;
  }

  int platform_address_to_string(const platform_address_t* addr, char* buf, size_t len) {
    switch (addr->family) {
      case PLATFORM_AF_INET:
        if (inet_ntop(AF_INET, &addr->inet.addr, buf, (socklen_t)len) == NULL) {
          return -1;
        }
        return 0;
      case PLATFORM_AF_INET6: {
        char ip[64];
        if (inet_ntop(AF_INET6, addr->inet6.addr, ip, sizeof(ip)) == NULL) {
          return -1;
        }
        if (addr->inet6.scope_id == 0) {
          int written = snprintf(buf, len, "%s", ip);
          return (written > 0 && (size_t)written < len) ? 0 : -1;
        }
        /* IF_NAMESIZE is 256 on Windows (16 on POSIX); the buffer must be
         * large enough or if_indextoname fails and we fall back to numeric. */
        char ifname[IF_NAMESIZE];
        int written;
        if (if_indextoname(addr->inet6.scope_id, ifname) != NULL) {
          written = snprintf(buf, len, "%s%%%s", ip, ifname);
        } else {
          written = snprintf(buf, len, "%s%%%u", ip,
                             (unsigned)addr->inet6.scope_id);
        }
        return (written > 0 && (size_t)written < len) ? 0 : -1;
      }
      case PLATFORM_AF_LOCAL:
        strncpy(buf, addr->local.path, len);
        if (len > 0) {
          buf[len - 1] = '\0';
        }
        return 0;
    }
    return -1;
  }
#endif
