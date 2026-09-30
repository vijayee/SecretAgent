//
// Created by victor on 5/23/25.
//

#include "bcrypt.h"
#include <string.h>
#include "../Platform/platform_random.h"

/* Vendored Solar Designer crypt_blowfish (deps/bcrypt submodule, built as the
   bcrypt_os CMake target). _crypt_blowfish_rn parses the $2b$cost$salt prefix
   from the supplied hash, recomputes the digest, and writes the full hash into
   the caller buffer; it returns that buffer on success or NULL on malformed
   input. The prototype is declared here rather than including the vendored
   crypt_blowfish.h, so this file's own bcrypt.h stays the only bcrypt header on
   the include path and there is no header-name interaction. The 64-byte buffer
   matches OpenBSD libcrypt's BCRYPT_HASHSIZE. */
#define OFFS_BCRYPT_HASHSIZE 64
extern char* _crypt_blowfish_rn(const char* key, const char* setting,
                                char* output, int size);
/* _crypt_gensalt_blowfish_rn builds a "$2b$cost$salt" setting string from a
   caller-supplied 16-byte random salt (we draw it from platform_random_bytes
   so there is no dependency on /dev/urandom or getentropy). count is the log2
   work factor. */
extern char* _crypt_gensalt_blowfish_rn(const char* prefix, unsigned long count,
                                        const char* input, int size,
                                        char* output, int output_size);

int bcrypt_check(const char* key, const char* hash) {
  if (key == NULL || hash == NULL) return -1;
  /* Only $2a/$2b/$2y are valid bcrypt prefixes. Reject $2x (the legacy
     sign-extension-bug variant) and anything malformed before hashing, so a
     bogus setting is never handed to _crypt_blowfish_rn. */
  if (hash[0] != '$' || hash[1] != '2' ||
      (hash[2] != 'a' && hash[2] != 'b' && hash[2] != 'y') ||
      hash[3] != '$') {
    return -1;
  }
  char out[OFFS_BCRYPT_HASHSIZE];
  if (_crypt_blowfish_rn(key, hash, out, (int)sizeof(out)) == NULL) return -1;
  /* Constant-time compare. strcmp short-circuits on the first differing byte,
     leaking how much of the recomputed hash matches the stored one. bcrypt
     hashes are fixed-length ($2b$ + cost + 22 salt + 31 digest = 60 chars),
     so compare the full recomputed length without early exit. Reject a
     stored hash whose length differs from the recomputed one first so a
     truncated buffer can't match a prefix and so we never over-read hash. */
  size_t out_len = strlen(out);
  if (strlen(hash) != out_len) return -1;
  int diff = 0;
  for (size_t i = 0; i < out_len; i++) {
    diff |= (unsigned char)out[i] ^ (unsigned char)hash[i];
  }
  return (diff == 0) ? 0 : -1;
}

int bcrypt_generate(const char* key, int cost, char* out, size_t out_size) {
  if (key == NULL || out == NULL) return -1;
  /* bcrypt cost is the log2 work factor; the spec-valid range is 4..31. Values
     below 4 are insecurely fast, above 31 overflow the two-digit field. */
  if (cost < 4 || cost > 31) return -1;
  /* The full $2b$ hash (setting + digest) is 60 chars + NUL; require the
     OpenBSD-standard 64-byte buffer for both the setting and the final hash. */
  if (out_size < (size_t)OFFS_BCRYPT_HASHSIZE) return -1;

  /* 128-bit salt drawn from the platform CSPRNG. */
  uint8_t salt[16];
  if (platform_random_bytes(salt, sizeof(salt)) != 0) return -1;

  char setting[OFFS_BCRYPT_HASHSIZE];
  if (_crypt_gensalt_blowfish_rn("$2b$", (unsigned long)cost,
                                 (const char*)salt, (int)sizeof(salt),
                                 setting, (int)sizeof(setting)) == NULL) {
    return -1;
  }

  /* _crypt_blowfish_rn writes the full "$2b$cost$salt<digest>" hash into out. */
  if (_crypt_blowfish_rn(key, setting, out, (int)out_size) == NULL) return -1;

  /* Defensive: crypt_blowfish always NUL-terminates, but make it explicit. */
  out[out_size - 1] = '\0';
  return 0;
}
