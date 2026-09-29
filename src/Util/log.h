/**
 * Copyright (c) 2020 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
 *
 * Extensions: per-module log levels, structured key=value output, module enum.
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>

#define LOG_VERSION "0.2.0"

typedef enum {
  LOG_MODULE_DEFAULT = 0,
  LOG_MODULE_NETWORK,
  LOG_MODULE_CACHE,
  LOG_MODULE_HTTP,
  LOG_MODULE_CONFIG,
  LOG_MODULE_NODE,
  LOG_MODULE_UPDATE,
  LOG_MODULE_STREAM,
  LOG_MODULE_ACTOR,
  LOG_MODULE_COUNT
} log_module_t;

typedef struct {
  va_list ap;
  const char *fmt;
  const char *file;
  struct tm *time;
  void *udata;
  int line;
  int level;
  int module;
} log_Event;

typedef void (*log_LogFn)(log_Event *ev);
typedef void (*log_LockFn)(bool lock, void *udata);

enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL };

#define log_trace(...) log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#define log_structured(level, module, ...) \
  _log_structured(level, module, __FILE__, __LINE__, __VA_ARGS__, NULL)

const char* log_level_string(int level);
const char* log_module_string(log_module_t module);
int log_level_from_string(const char *name);

void log_set_lock(log_LockFn fn, void *udata);
void log_set_level(int level);
void log_set_module_level(log_module_t module, int level);
int log_get_module_level(log_module_t module);
void log_set_quiet(bool enable);
void log_set_structured(bool enable);
int log_add_callback(log_LogFn fn, void *udata, int level);
int log_add_fp(FILE *fp, int level);

void log_log(int level, const char *file, int line, const char *fmt, ...);
void _log_structured(int level, int module, const char *file, int line, ...);

#endif
