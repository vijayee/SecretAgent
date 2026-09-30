//
// Created by victor on 5/23/25.
//

#ifndef OFFS_BCRYPT_H
#define OFFS_BCRYPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Check a plaintext key against a bcrypt hash ($2b$ prefix).
   Returns 0 on match, -1 on mismatch or error. */
int bcrypt_check(const char* key, const char* hash);

/* Generate a bcrypt hash ($2b$ prefix) for a plaintext key using the given
   cost (log2 work factor; must be in 4..31, default 12). The 16-byte salt is
   drawn from the platform CSPRNG (platform_random_bytes), so this is safe to
   call client-side without /dev/urandom. Writes a NUL-terminated "$2b$..."
   string into out, which must be at least 64 bytes. Returns 0 on success,
   -1 on error (bad args, RNG failure, or crypt_blowfish rejection). */
int bcrypt_generate(const char* key, int cost, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_BCRYPT_H */
