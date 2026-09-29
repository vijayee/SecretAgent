//
// Created by victor on 9/29/26.
//

#include "py_subprocess.h"

#ifdef SA_HAS_PYTHON

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../Util/allocator.h"

/* Per-stream hard cap (plan §Task 9): buffered output is truncated past
   this; the read end is STILL drained to EOF so the child can always finish
   and waitpid below cannot hang on a child blocked writing to a full pipe. */
#define _PYSUB_STREAM_CAP (1024u * 1024u)
#define _PYSUB_DISCARD_CHUNK 4096

uint8_t pyrt_run_cell_subprocess(const char* code, char** out_text, char** out_err) {
  *out_text = NULL;
  *out_err = NULL;
  if (code == NULL) {
    *out_text = strdup("pyrt: subprocess exited nonzero");
    return 1;
  }

  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0) {
    *out_text = strdup("pyrt: subprocess pipe failed");
    return 1;
  }
  if (pipe(err_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    *out_text = strdup("pyrt: subprocess pipe failed");
    return 1;
  }

  /* Buffers are reserved BEFORE the fork: a mid-setup allocation failure
     must never strand a child process (only the fork/unfork paths below can
     produce one). */
  char* out_buf = (char*)get_memory(_PYSUB_STREAM_CAP + 1);
  char* err_buf = (char*)get_memory(_PYSUB_STREAM_CAP + 1);
  char discard[_PYSUB_DISCARD_CHUNK];
  /* Defensive; get_memory aborts on OOM. */
  if (out_buf == NULL || err_buf == NULL) {
    free(out_buf);
    free(err_buf);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    *out_text = strdup("pyrt: subprocess buffer failed");
    return 1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    *out_text = strdup("pyrt: subprocess fork failed");
    return 1;
  }
  if (pid == 0) {
    /* Child: only the write ends may survive exec, rewired to fd 1/2. */
    close(out_pipe[0]);
    close(err_pipe[0]);
    if (dup2(out_pipe[1], STDOUT_FILENO) < 0 || dup2(err_pipe[1], STDERR_FILENO) < 0) {
      _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    execlp("python3", "python3", "-I", "-c", code, (char*)NULL);
    _exit(127); /* exec failed: the parent reads it back as a nonzero exit */
  }

  /* Parent: hand the write ends to the child's side of the pipe only; close
     our copies immediately so EOF is observable after the child exits. */
  close(out_pipe[1]);
  close(err_pipe[1]);

  size_t out_len = 0;
  size_t err_len = 0;
  int out_open = 1;
  int err_open = 1;

  /* Drain BOTH read ends to EOF under select (no timeout): the child's exit
     closes its write ends, so EOF is the drain-complete signal. EINTR is
     retried; any other select error marks both streams closed. */
  while (out_open || err_open) {
    fd_set readfds;
    FD_ZERO(&readfds);
    int maxfd = -1;
    if (out_open) {
      FD_SET(out_pipe[0], &readfds);
      if (out_pipe[0] > maxfd) {
        maxfd = out_pipe[0];
      }
    }
    if (err_open) {
      FD_SET(err_pipe[0], &readfds);
      if (err_pipe[0] > maxfd) {
        maxfd = err_pipe[0];
      }
    }
    if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (out_open && FD_ISSET(out_pipe[0], &readfds)) {
      /* Buffer only up to the cap (documented truncation); past it the bytes
         are discarded, but the stream keeps draining. */
      size_t room = _PYSUB_STREAM_CAP - out_len;
      char* target = out_buf + out_len;
      ssize_t n;
      do {
        n = read(out_pipe[0], room > 0 ? (void*)target : (void*)discard,
                 room > 0 ? room : sizeof(discard));
      } while (n < 0 && errno == EINTR);
      if (n <= 0) {
        out_open = 0; /* EOF (n == 0) or read error: stream is done */
      } else if (room > 0) {
        out_len += (size_t)n < room ? (size_t)n : room;
      }
    }
    if (err_open && FD_ISSET(err_pipe[0], &readfds)) {
      size_t room = _PYSUB_STREAM_CAP - err_len;
      char* target = err_buf + err_len;
      ssize_t n;
      do {
        n = read(err_pipe[0], room > 0 ? (void*)target : (void*)discard,
                 room > 0 ? room : sizeof(discard));
      } while (n < 0 && errno == EINTR);
      if (n <= 0) {
        err_open = 0; /* EOF (n == 0) or read error: stream is done */
      } else if (room > 0) {
        err_len += (size_t)n < room ? (size_t)n : room;
      }
    }
  }
  out_buf[out_len] = '\0';
  err_buf[err_len] = '\0';

  /* waitpid before any return: the child must be reaped on every path past
     the fork (both read ends have reached EOF, or been dropped on select
     error). EINTR is retried; any other waitpid failure counts as nonzero —
     an unreapable child must not be reported as a clean cell. */
  int wstatus = 0;
  int reaped = 0;
  while (!reaped) {
    if (waitpid(pid, &wstatus, 0) == pid) {
      reaped = 1;
    } else if (errno != EINTR) {
      break;
    }
  }
  close(out_pipe[0]);
  close(err_pipe[0]);

  int exitcode = (reaped && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : -1;
  uint8_t status = (exitcode == 0 && err_len == 0) ? (uint8_t)0 : (uint8_t)1;
  if (status == 0) {
    /* stdout IS the result per the envelope contract. */
    *out_text = out_buf;
    *out_err = NULL;
    free(err_buf);
  } else {
    /* Failure text: the stderr text, or the literal when stderr is empty. */
    *out_text = strdup(err_len > 0 ? err_buf : "pyrt: subprocess exited nonzero");
    *out_err = err_len > 0 ? strdup(err_buf) : NULL;
    free(out_buf);
    free(err_buf);
  }
  return status;
}

#endif /* SA_HAS_PYTHON */
