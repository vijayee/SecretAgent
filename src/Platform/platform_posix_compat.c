/*
 * POSIX compat shim — function bodies for the static-inline shims declared in
 * platform_posix_compat.h. The inline declarations cover most callers; this
 * .c file provides real exported symbols for callers (e.g. tools/offs-ca) that
 * do not include the compat header.
 */

#if defined(_MSC_VER)

#include "platform_posix_compat.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* Functions MSVC lacks outright. The corresponding declarations are guarded
 * by #ifndef in platform_posix_compat.h so the UCRT-provided versions (e.g.
 * strnlen) are not redefined. */

#ifndef strndup
char* strndup(const char* s, size_t n) {
  size_t len = strnlen(s, n);
  char* p = (char*)malloc(len + 1);
  if (!p) return NULL;
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}
#endif

/* mkdtemp replacement: create a uniquely-named directory. */
#include <direct.h>
#include <process.h>
#include <time.h>
#include <windows.h>
char* mkdtemp(char* tmpl) {
  if (!tmpl) return NULL;
  size_t len = strlen(tmpl);
  if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
    return NULL;
  }
  /* Mix several entropy sources so back-to-back calls in the same
   * process produce different names. rand() is per-process state and
   * is not thread-safe; we also pull in the current PID, a high-
   * resolution counter, and the wall clock. */
  static int seed_initialized = 0;
  if (!seed_initialized) {
    srand((unsigned)(time(NULL) ^ (intptr_t)GetCurrentProcessId()));
    seed_initialized = 1;
  }
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  for (int attempt = 0; attempt < 100; attempt++) {
    unsigned int r = (unsigned)rand();
    unsigned int mix = (unsigned)qpc.LowPart ^ (unsigned)(qpc.LowPart >> 16);
    unsigned int id = (unsigned)(intptr_t)GetCurrentProcessId();
    unsigned int combined = r ^ (mix + attempt * 2654435761u) ^ (id * 0x9E3779B1u);
    snprintf(tmpl + len - 6, 7, "%06x", combined & 0xffffff);
    QueryPerformanceCounter(&qpc);
    if (_mkdir(tmpl) == 0) {
      return tmpl;
    }
  }
  return NULL;
}

char* realpath(const char* path, char* resolved_path) {
  return _fullpath(resolved_path, path, _MAX_PATH);
}

int asprintf(char** strp, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = _vscprintf(fmt, ap);
  va_end(ap);
  if (len < 0) return -1;
  *strp = (char*)malloc((size_t)len + 1);
  if (!*strp) return -1;
  va_start(ap, fmt);
  int written = vsnprintf(*strp, (size_t)len + 1, fmt, ap);
  va_end(ap);
  return written;
}

int vasprintf(char** strp, const char* fmt, va_list ap) {
  va_list ap_copy;
  va_copy(ap_copy, ap);
  int len = _vscprintf(fmt, ap_copy);
  va_end(ap_copy);
  if (len < 0) return -1;
  *strp = (char*)malloc((size_t)len + 1);
  if (!*strp) return -1;
  return vsnprintf(*strp, (size_t)len + 1, fmt, ap);
}

/* memmem replacement: locate the first occurrence of NEEDLE (size NEEDLELEN)
 * in HAYSTACK (size HAYSTACKLEN). Returns NULL if not found. Direct port of
 * the BSD/GNU algorithm — needed because off_routes.c uses it for multipart
 * boundary parsing and MSVC's CRT does not ship a memmem symbol. */
void* memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen) {
  if (needlelen == 0) return (void*)haystack;
  if (haystacklen < needlelen) return NULL;
  const unsigned char* h = (const unsigned char*)haystack;
  const unsigned char* n = (const unsigned char*)needle;
  size_t scan_max = haystacklen - needlelen;
  for (size_t i = 0; i <= scan_max; i++) {
    if (h[i] != n[0]) continue;
    if (memcmp(h + i, n, needlelen) == 0) {
      return (void*)(h + i);
    }
  }
  return NULL;
}

#endif /* _MSC_VER */
