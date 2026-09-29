#ifndef SA_PLATFORM_COMPILER_H
#define SA_PLATFORM_COMPILER_H

#if defined(_MSC_VER)
  /* MSVC */
  #define PLATFORM_NORETURN       __declspec(noreturn)
  #define PLATFORM_THREAD_LOCAL   __declspec(thread)
  #define PLATFORM_UNUSED         /* MSVC: suppress C4100 via /wd4100, no per-symbol equivalent */
  #define PLATFORM_STRUCT_PACKED  __declspec(align(1))
  #define PLATFORM_ALIGNED(n)     __declspec(align(n))
  #define PLATFORM_PRINTF(f, a)   /* no MSVC equivalent */
  #include <intrin.h>
  #pragma intrinsic(_BitScanForward)
  static inline int _platform_ffs_msvc(unsigned long x) {
    unsigned long index;
    return _BitScanForward(&index, x) ? (int)(index + 1) : 0;
  }
  #define PLATFORM_FFS(x) _platform_ffs_msvc((unsigned long)(x))
  static inline int _platform_clz_msvc(unsigned long x) {
    unsigned long index;
    return _BitScanReverse(&index, x) ? (int)(31 - index) : 32;
  }
  #define PLATFORM_CLZ(x) _platform_clz_msvc((unsigned long)(x))
  static inline int _platform_popcount_msvc(unsigned long x) {
    return (int)__popcnt(x);
  }
  #define PLATFORM_POPCOUNT(x) _platform_popcount_msvc((unsigned long)(x))

  #define PLATFORM_DIAGNOSTIC_PUSH         __pragma(warning(push))
  #define PLATFORM_DIAGNOSTIC_POP          __pragma(warning(pop))
  #define PLATFORM_DIAGNOSTIC_IGNORE(w)    __pragma(warning(disable: w))
#else
  /* GCC / Clang */
  #define PLATFORM_NORETURN       __attribute__((noreturn))
  #define PLATFORM_THREAD_LOCAL   __thread
  #define PLATFORM_UNUSED         __attribute__((unused))
  #define PLATFORM_STRUCT_PACKED  __attribute__((packed))
  #define PLATFORM_ALIGNED(n)     __attribute__((aligned(n)))
  #define PLATFORM_PRINTF(f, a)   __attribute__((format(printf, f, a)))
  #define PLATFORM_FFS(x)         __builtin_ffs(x)
  #define PLATFORM_CLZ(x)         __builtin_clz(x)
  #define PLATFORM_POPCOUNT(x)    __builtin_popcount(x)

  #define PLATFORM_DIAGNOSTIC_PUSH         _Pragma("GCC diagnostic push")
  #define PLATFORM_DIAGNOSTIC_POP          _Pragma("GCC diagnostic pop")
  #define PLATFORM_DIAGNOSTIC_STRINGIFY(x) #x
  #define PLATFORM_DIAGNOSTIC_IGNORE_MSG(msg) \
    _Pragma(PLATFORM_DIAGNOSTIC_STRINGIFY(GCC diagnostic ignored msg))
  #define PLATFORM_DIAGNOSTIC_IGNORE(w) \
    PLATFORM_DIAGNOSTIC_IGNORE_MSG(PLATFORM_DIAGNOSTIC_STRINGIFY(w))
#endif

#endif /* SA_PLATFORM_COMPILER_H */
