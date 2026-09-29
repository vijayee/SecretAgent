#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #define _POSIX_C_SOURCE 199309L
  #include <time.h>
#endif

#include "platform_time.h"

#ifdef _WIN32
  void platform_sleep_ms(unsigned int ms) {
    Sleep(ms);
  }

  uint64_t platform_monotonic_ns(void) {
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000000ULL / freq.QuadPart);
  }
#else

  void platform_sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
  }

  uint64_t platform_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  }
#endif
