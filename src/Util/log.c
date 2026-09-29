/*
 * Copyright (c) 2020 rxi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Extensions: per-module log levels, structured key=value output,
 *             log_level_from_string, log_module_string.
 */

#include "log.h"
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
  #include "../Platform/platform_posix_compat.h"
#endif

#define MAX_CALLBACKS 32

typedef struct {
  log_LogFn fn;
  void *udata;
  int level;
} Callback;

static struct {
  void *udata;
  log_LockFn lock;
  int level;
  bool quiet;
  bool structured;
  int module_levels[LOG_MODULE_COUNT];
  Callback callbacks[MAX_CALLBACKS];
} L;

static const char *level_strings[] = {
  "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static const char *module_strings[] = {
  "DEFAULT", "NETWORK", "CACHE", "HTTP",
  "CONFIG", "NODE", "UPDATE", "STREAM", "ACTOR"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
  "\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"
};
#endif

static int _module_level(log_module_t module) {
  if (module < 0 || module >= LOG_MODULE_COUNT) return L.module_levels[LOG_MODULE_DEFAULT];
  return L.module_levels[module];
}

static void stdout_callback(log_Event *ev) {
  char buf[16];
  buf[strftime(buf, sizeof(buf), "%H:%M:%S", ev->time)] = '\0';
#ifdef LOG_USE_COLOR
  fprintf(
    ev->udata, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
    buf, level_colors[ev->level], level_strings[ev->level],
    ev->file, ev->line);
#else
  fprintf(
    ev->udata, "%s %-5s %s:%d: ",
    buf, level_strings[ev->level], ev->file, ev->line);
#endif
  if (ev->fmt != NULL) {
    vfprintf(ev->udata, ev->fmt, ev->ap);
  }
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}

static void structured_callback(log_Event *ev) {
  char buf[64];
  buf[strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", ev->time)] = '\0';
  /* time= level= module= file= line= ... */
  fprintf(ev->udata, "time=%s level=%s module=%s file=%s line=%d",
          buf, level_strings[ev->level],
          (ev->module >= 0 && ev->module < LOG_MODULE_COUNT)
            ? module_strings[ev->module] : "UNKNOWN",
          ev->file, ev->line);
  if (ev->fmt != NULL) {
    fprintf(ev->udata, " ");
    vfprintf(ev->udata, ev->fmt, ev->ap);
  }
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}

static void file_callback(log_Event *ev) {
  char buf[64];
  buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';
  fprintf(
    ev->udata, "%s %-5s %s:%d: ",
    buf, level_strings[ev->level], ev->file, ev->line);
  if (ev->fmt != NULL) {
    vfprintf(ev->udata, ev->fmt, ev->ap);
  }
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}

static void lock(void)   {
  if (L.lock) { L.lock(true, L.udata); }
}

static void unlock(void) {
  if (L.lock) { L.lock(false, L.udata); }
}

const char* log_level_string(int level) {
  return level_strings[level];
}

const char* log_module_string(log_module_t module) {
  if (module < 0 || module >= LOG_MODULE_COUNT) return "UNKNOWN";
  return module_strings[module];
}

int log_level_from_string(const char *name) {
  if (name == NULL) return LOG_INFO;
  for (int i = 0; i < 6; i++) {
    if (strcasecmp(name, level_strings[i]) == 0) return i;
  }
  /* Support numeric levels */
  char *endptr = NULL;
  long val = strtol(name, &endptr, 10);
  if (endptr != name && *endptr == '\0' && val >= LOG_TRACE && val <= LOG_FATAL) {
    return (int)val;
  }
  return LOG_INFO; /* default */
}

void log_set_lock(log_LockFn fn, void *udata) {
  L.lock = fn;
  L.udata = udata;
}

void log_set_level(int level) {
  L.level = level;
}

void log_set_module_level(log_module_t module, int level) {
  if (module >= 0 && module < LOG_MODULE_COUNT) {
    L.module_levels[module] = level;
  }
}

int log_get_module_level(log_module_t module) {
  if (module >= 0 && module < LOG_MODULE_COUNT) {
    return L.module_levels[module];
  }
  return L.level;
}

void log_set_quiet(bool enable) {
  L.quiet = enable;
}

void log_set_structured(bool enable) {
  L.structured = enable;
}

int log_add_callback(log_LogFn fn, void *udata, int level) {
  for (int index = 0; index < MAX_CALLBACKS; index++) {
    if (!L.callbacks[index].fn) {
      L.callbacks[index] = (Callback) { fn, udata, level };
      return 0;
    }
  }
  return -1;
}

int log_add_fp(FILE *fp, int level) {
  return log_add_callback(file_callback, fp, level);
}

static void init_event(log_Event *ev, void *udata) {
  if (!ev->time) {
    time_t t = time(NULL);
    ev->time = localtime(&t);
  }
  ev->udata = udata;
}

void log_log(int level, const char *file, int line, const char *fmt, ...) {
  log_Event ev = {
    .fmt   = fmt,
    .file  = file,
    .line  = line,
    .level = level,
    .module = LOG_MODULE_DEFAULT,
  };

  int effective_level = L.level;
  if (_module_level(LOG_MODULE_DEFAULT) > effective_level) {
    effective_level = _module_level(LOG_MODULE_DEFAULT);
  }

  lock();

  if (!L.quiet && level >= effective_level) {
    init_event(&ev, stderr);
    va_start(ev.ap, fmt);
    if (L.structured) {
      structured_callback(&ev);
    } else {
      stdout_callback(&ev);
    }
    va_end(ev.ap);
  }

  for (int index = 0; index < MAX_CALLBACKS && L.callbacks[index].fn; index++) {
    Callback *cb = &L.callbacks[index];
    if (level >= cb->level) {
      init_event(&ev, cb->udata);
      va_start(ev.ap, fmt);
      cb->fn(&ev);
      va_end(ev.ap);
    }
  }

  unlock();
}

void _log_structured(int level, int module, const char *file, int line, ...) {
  int effective_level = L.level;
  int mod_level = _module_level((log_module_t)module);
  if (mod_level > effective_level) effective_level = mod_level;

  log_Event ev = {
    .fmt    = NULL,
    .file   = file,
    .line   = line,
    .level  = level,
    .module = module,
  };

  lock();

  /* Build the key=value format string from varargs */
  char structured_fmt[1024];
  int pos = 0;
  va_list build_ap;
  va_start(build_ap, line);

  const char *key;
  while ((key = va_arg(build_ap, const char *)) != NULL) {
    const char *val = va_arg(build_ap, const char *);
    if (val == NULL) val = "(null)";
    /* Quote values containing spaces or special chars */
    int needs_quote = 0;
    for (const char *scan = val; *scan; scan++) {
      if (*scan == ' ' || *scan == '=' || *scan == '"' || *scan == '\t') {
        needs_quote = 1;
        break;
      }
    }
    if (needs_quote) {
      pos += snprintf(structured_fmt + pos, sizeof(structured_fmt) - (size_t)pos,
                      "%s=\"%s\" ", key, val);
    } else {
      pos += snprintf(structured_fmt + pos, sizeof(structured_fmt) - (size_t)pos,
                      "%s=%s ", key, val);
    }
    if (pos >= (int)sizeof(structured_fmt) - 2) break;
  }
  va_end(build_ap);

  /* Remove trailing space */
  if (pos > 0 && structured_fmt[pos - 1] == ' ') {
    structured_fmt[pos - 1] = '\0';
  }

  if (!L.quiet && level >= effective_level) {
    init_event(&ev, stderr);
    /* Use the built format string as fmt so it renders inline */
    ev.fmt = structured_fmt;
    /* We need an ap for the callback; since structured_fmt has no
     * format specifiers, we pass an empty va_list so vfprintf just
     * prints the string as-is. */
    va_start(ev.ap, line);
    structured_callback(&ev);
    va_end(ev.ap);
  }

  for (int index = 0; index < MAX_CALLBACKS && L.callbacks[index].fn; index++) {
    Callback *cb = &L.callbacks[index];
    if (level >= cb->level) {
      init_event(&ev, cb->udata);
      if (L.structured) {
        cb->fn(&ev);
      }
    }
  }

  unlock();
}
