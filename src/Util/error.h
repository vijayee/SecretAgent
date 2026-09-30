//
// Created by victor on 4/7/25.
//

#ifndef OFFS_ERROR_H
#define OFFS_ERROR_H

// The Windows SDK <wingdi.h> defines `ERROR` as the integer 0, which would
// break a function-style ERROR(...) macro. We expose the canonical name as
// OFFS_ERROR and keep ERROR as an alias for convenience, but the alias is
// only emitted if the Windows SDK has not already defined ERROR. This avoids
// surprises when a TU includes <windows.h> before this header.
#include "../RefCounter/refcounter.h"
typedef struct {
  refcounter_t refcounter;
  char* message;
  char* file;
  char* function;
  int line;
} async_error_t;

async_error_t* error_create(char* message, char* file, char* function, int line);
void error_destroy(async_error_t* error);

#define OFFS_ERROR(MESSAGE) error_create((MESSAGE), (char*)__FILE__, (char*)__func__, __LINE__)

/* OFFS_ERROR creates a fresh error (count=1, yield=0). OFFS_ERROR_TRANSFER
   does the same AND yields it (yield=1), signalling that the caller is
   transferring ownership of the single reference to whoever receives it
   (e.g. a stream_notify payload). The receiver adopts the yielded reference
   rather than allocating a new one, so the reference is moved, not copied.
   Use OFFS_ERROR_TRANSFER whenever the freshly-made error is handed off
   wholesale and the caller will not retain it; use OFFS_ERROR only when the
   caller keeps the reference. */
#define OFFS_ERROR_TRANSFER(MESSAGE) offs_error_transfer((MESSAGE), (char*)__FILE__, (char*)__func__, __LINE__)
async_error_t* offs_error_transfer(char* message, char* file, char* function, int line);

// Only define ERROR(...) if the Windows SDK hasn't already taken the name.
// On non-Windows builds (or after carefully undef'ing first) this gives us
// the short form; otherwise callers must use OFFS_ERROR.
#ifndef _WIN32
  #define ERROR(MESSAGE) OFFS_ERROR(MESSAGE)
#endif

#endif //OFFS_ERROR_H
