#ifndef OFFS_PLATFORM_SOCKET_H
#define OFFS_PLATFORM_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "platform_posix_compat.h"
#include <poll-dancer/poll-dancer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* pd_loop_t, pd_watcher_t, pd_event_t, and pd_callback_t come from
 * poll-dancer's public header, included above. The dispatch helper
 * below takes a poll-dancer callback and a pd_event_t, so the types
 * must be visible here. */

typedef enum {
  PLATFORM_AF_INET,
  PLATFORM_AF_INET6,
  PLATFORM_AF_LOCAL
} platform_address_family_e;

typedef struct {
  platform_address_family_e family;
  union {
    struct { uint32_t addr; uint16_t port; } inet;
    struct { uint8_t addr[16]; uint16_t port; uint32_t scope_id; } inet6;
    struct { char path[256]; } local;
  };
} platform_address_t;

typedef struct platform_socket_t platform_socket_t;

#define PLATFORM_SHUT_RD    0
#define PLATFORM_SHUT_WR    1
#define PLATFORM_SHUT_RDWR  2

/* Lifecycle */
platform_socket_t* platform_socket_create(platform_address_family_e family, int stream);
void platform_socket_destroy(platform_socket_t* sock);
int platform_socket_fd(platform_socket_t* sock);

/* Returns 1 if this socket is backed by a Windows named pipe handle, 0
 * if it's a regular socket (POSIX file descriptor or Winsock SOCKET), -1
 * on invalid input. The pipe path is only meaningful on Windows; on POSIX
 * builds this always returns 0 for a valid socket. */
int platform_socket_is_pipe(platform_socket_t* sock);

/* On Windows, return the underlying HANDLE for a pipe-backed socket.
 * Returns INVALID_HANDLE_VALUE (cast to void*) for non-pipe sockets, and
 * NULL for an invalid input. On POSIX builds this always returns NULL
 * (POSIX sockets are accessed via platform_socket_fd instead). */
void* platform_socket_handle(platform_socket_t* sock);

/* Server */
int platform_socket_bind(platform_socket_t* sock, const platform_address_t* addr);

/* Create a listener socket bound to host:port. Prefers a dual-stack IPv6
 * socket (IPV6_V6ONLY=0, serves both families on one socket); falls back to
 * an IPv4 socket when IPv6 is unavailable or the v6 bind fails (logs one
 * warning). Unparsable hosts bind the wildcard address of the chosen
 * family. The helper enables SO_REUSEADDR on the socket before binding.
 * *out_addr (if non-NULL) receives the requested (possibly v4-mapped)
 * address. Returns NULL on failure. */
platform_socket_t* platform_listen_socket_create(const char* host, uint16_t port,
                                                 platform_address_t* out_addr);
int platform_socket_listen(platform_socket_t* sock, int backlog);
platform_socket_t* platform_socket_accept(platform_socket_t* sock, platform_address_t* remote);

/* Client */
int platform_socket_connect(platform_socket_t* sock, const platform_address_t* addr);

/* Configuration */
int platform_socket_set_nonblocking(platform_socket_t* sock);
int platform_socket_set_reuseaddr(platform_socket_t* sock);

/* I/O */
ssize_t platform_socket_send(platform_socket_t* sock, const void* buf, size_t len);
ssize_t platform_socket_recv(platform_socket_t* sock, void* buf, size_t len);
int platform_socket_shutdown(platform_socket_t* sock, int how);

/* Address helpers */
int platform_address_parse(platform_address_t* addr, const char* host, uint16_t port);
int platform_address_to_string(const platform_address_t* addr, char* buf, size_t len);

/* Watcher dispatch helper.
 *
 * Create a poll-dancer watcher for this socket. For regular sockets
 * (POSIX fd or Winsock SOCKET) this calls pd_watcher_create. For
 * pipe-backed sockets on Windows it calls pd_watcher_create_for_handle
 * so the watcher's HANDLE field is the pipe HANDLE itself (no
 * _get_osfhandle round-trip).
 *
 * Centralizing this here keeps the transport code free of
 * "is it a pipe?" branching. The returned watcher must be destroyed
 * with pd_watcher_destroy.
 */
pd_watcher_t* platform_socket_watcher_create(pd_loop_t* loop,
                                             platform_socket_t* sock,
                                             pd_event_t events,
                                             pd_callback_t callback,
                                             void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_SOCKET_H */
