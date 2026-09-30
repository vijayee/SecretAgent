#ifndef OFFS_PLATFORM_RANDOM_H
#define OFFS_PLATFORM_RANDOM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int platform_random_bytes(uint8_t* buf, size_t len);

/* Cryptographically secure random primitives for use in routing/storage
   decisions where predictability enables Eclipse/correlation attacks.
   These must NOT be used in hot loops where throughput matters more than
   unpredictability — frand/RAND_bytes is the right choice for bulk pad
   generation; these are for occasional per-decision draws. */
uint32_t platform_random_uint32(void);

/* Uniform float in [0.0, 1.0). Returns 0.0 on entropy failure (callers
   treat low values as the conservative branch, so this fails safe). */
float platform_random_uniform_float(void);

/* Uniform integer in [0, bound). Returns 0 on entropy failure or bound==0. */
size_t platform_random_uniform_index(size_t bound);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_RANDOM_H */
