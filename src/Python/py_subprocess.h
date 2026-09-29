//
// Created by victor on 9/29/26.
//

#ifndef SA_PY_SUBPROCESS_H
#define SA_PY_SUBPROCESS_H

#include <stdint.h>

#ifdef SA_HAS_PYTHON
/* Runs one stateless cell in a `python3 -I -c` subprocess. Returns exit
   status (0 ok) and fills *out_text (stdout) and *out_err (stderr or
   runtime error text); both returned strings are heap-allocated and free()d
   by the caller.
   Per-stream output is capped at 1 MiB; bytes past the cap are discarded
   (the pipe is still drained to EOF so waitpid cannot hang), so a successful
   cell's *out_text may be truncated at the cap.
   Documented limitation: no live namespace across subprocess cells;
   POSIX-only in this milestone (Windows spawn ships with the PCBuild
   completion task for the slice). */
uint8_t pyrt_run_cell_subprocess(const char* code, char** out_text, char** out_err);
#endif /* SA_HAS_PYTHON */

#endif // SA_PY_SUBPROCESS_H
