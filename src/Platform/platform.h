//
// Created by victor on 9/29/26.
//

#ifndef SA_PLATFORM_H
#define SA_PLATFORM_H

#include "platform_compiler.h"
#include "platform_posix_compat.h"
#include "platform_time.h"
#include "platform_thread.h"
#include "platform_process.h"
#include "platform_file.h"
/* platform_socket.h reaches <poll-dancer/poll-dancer.h>, which exists only
   under the streams gate — same OFF-build guard shape as the wavedb one. */
#ifdef SA_HAS_STREAMS
#include "platform_socket.h"
#endif

#endif // SA_PLATFORM_H