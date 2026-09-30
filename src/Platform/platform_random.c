#define _DEFAULT_SOURCE

#include "platform_random.h"
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #include <bcrypt.h>

  int platform_random_bytes(uint8_t* buf, size_t len) {
    /* BCryptGenRandom always uses the system-preferred RNG provider */
    return BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
  }
#else
  #include <unistd.h>

  int platform_random_bytes(uint8_t* buf, size_t len) {
    /* getentropy(2) on Linux/BSDs caps a single request at 256 bytes
       (returns -1, errno=EINVAL for larger buffers). Loop in chunks so
       callers can request block-sized buffers (up to 2 MB) safely. */
    size_t filled = 0;
    while (filled < len) {
      size_t chunk = len - filled;
      if (chunk > 256) chunk = 256;
      if (getentropy(buf + filled, chunk) != 0) {
        return -1;
      }
      filled += chunk;
    }
    return 0;
  }
#endif

uint32_t platform_random_uint32(void) {
  uint8_t bytes[4];
  if (platform_random_bytes(bytes, sizeof(bytes)) != 0) {
    return 0;
  }
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

float platform_random_uniform_float(void) {
  /* Map a 32-bit draw to [0, 1.0) by dividing by 2^32. RAND_max-equivalent
     but cryptographically secure and full 24-bit float precision. */
  return (float)(platform_random_uint32() / 4294967296.0);
}

size_t platform_random_uniform_index(size_t bound) {
  if (bound == 0) return 0;
  /* Lemire-style debiased reduction: reject the partial bucket so a small
     bound isn't biased by the remainder of the 2^32 space. */
  const uint32_t bound32 = (uint32_t)bound;
  const uint64_t range = (uint64_t)UINT32_MAX + 1ULL;
  const uint64_t bound64 = (uint64_t)bound32;
  const uint64_t limit = range - (range % bound64);
  uint32_t draw;
  do {
    draw = platform_random_uint32();
  } while ((uint64_t)draw >= limit);
  return (size_t)(draw % bound32);
}
