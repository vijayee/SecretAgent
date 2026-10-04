//
// Created by victor on 10/03/26.
//
/* Ported from liboffs src/Platform/platform_socket_watcher.c (byte-except-
 * includes-with-adaptations — none needed; the platform_socket_t is_pipe field
 * our platform_socket.c carries is the same shape it branches on). The
 * transport's centralization helper: keeps "is it a pipe?" branching out of
 * the transport code, exactly as platform_socket.h's declaration promises. */

#include "platform_socket.h"
#include "platform_socket_internal.h"

#include <poll-dancer/poll-dancer.h>

pd_watcher_t* platform_socket_watcher_create(pd_loop_t* loop,
                                             platform_socket_t* sock,
                                             pd_event_t events,
                                             pd_callback_t callback,
                                             void* user_data) {
  if (loop == NULL || sock == NULL) {
    return NULL;
  }
#ifdef _WIN32
  if (sock->is_pipe) {
    HANDLE h = (HANDLE)platform_socket_handle(sock);
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
      return NULL;
    }
    return pd_watcher_create_for_handle(loop, (void*)h, events, callback, user_data);
  }
#endif
  return pd_watcher_create(loop, platform_socket_fd(sock), events, callback, user_data);
}