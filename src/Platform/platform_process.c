#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #define _DEFAULT_SOURCE
  #include <unistd.h>
#endif

#include "platform_process.h"

#ifdef _WIN32
  int platform_getpid(void) {
    return (int)GetCurrentProcessId();
  }

  int platform_core_count(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
  }
#else
  int platform_getpid(void) {
    return (int)getpid();
  }

  int platform_core_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return (int)(count > 0 ? count : 1);
  }
#endif
