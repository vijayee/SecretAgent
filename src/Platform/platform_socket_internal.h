#ifndef OFFS_PLATFORM_SOCKET_INTERNAL_H
#define OFFS_PLATFORM_SOCKET_INTERNAL_H

#include "platform_socket.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The struct definition for platform_socket_t lives here so that
 * adjacent translation units (e.g. platform_local.c) can access the
 * fields when dispatching on transport type. The struct is opaque to
 * external callers — only the public API in platform_socket.h is
 * meant to be used. */
struct platform_socket_t {
#if defined(_WIN32)
  SOCKET fd;          /* Winsock SOCKET when is_pipe == 0 */
  HANDLE handle;      /* Named-pipe HANDLE when is_pipe == 1 */
  int family;         /* platform_address_family_e */
  int is_pipe;        /* 1 if backed by a Windows named pipe, else 0 */
  int owns_handle;    /* 1 if destroy should CloseHandle, 0 if borrowed */
  int nonblocking;    /* 1 after platform_socket_set_nonblocking; pipes keep
                       * their handle PIPE_WAIT and implement non-blocking
                       * send/recv from this flag themselves */
  char* pipe_name;    /* Pipe name (for listener rearm), NULL for client/accepted sockets */
#else
  int fd;
  int family; /* platform_address_family_e */
#endif
};

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_SOCKET_INTERNAL_H */
