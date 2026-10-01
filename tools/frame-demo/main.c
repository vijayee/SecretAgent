//
// Created by victor on 9/30/26.
//
// frame-demo (Task 11): the demo CLI. ONE top-level frame, ONE loop run ON
// THE CALLER'S THREAD against an OpenAI-compatible endpoint (local Ollama by
// default) — no daemon, no RPC. The store is REAL disk (create-if-absent).
//
// STREAMING (scoped): the loop is a single blocking call (frame_run_loop),
// so the public surface offers no turn-by-turn polling seam. The lines that
// DO stream live are the loop/cell progress lines ("cell corr ... completed
// with status", errors, stop requests), which ride Util/log.h's callback
// hook as they arrive. Control events (e.g. model-error) carry NO log line
// on their way in — they exist only as stored event records, so they first
// surface in the post-loop audit dump below, alongside the final assistant
// content.
//
// CTRL-C: pyrt_interrupt exists for python cells, but the frame layer owns
// its pyrt privately — frame_t exposes NO interrupt entry point, and a
// signal handler cannot safely post FRM_STOP through frame_dispatch (it
// allocates and takes locks). The plan's sanctioned path (print a notice,
// exit cleanly) applies; committed state is durable in the store.

#include "../../src/Frame/frame.h"
#include "../../src/Frame/loop.h"
#include "../../src/Util/json.h"
#include "../../src/Util/log.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The aliased WaveDB logger (see the family-shared symbol aliasing block in
   the root CMakeLists.txt) carries its OWN quiet flag — the demo's
   log_set_quiet below only touches SecretAgent's logger, so WaveDB's INFO
   lines around open/close would flood stderr without this. Only defined
   under SA_ENABLE_WDB, which is the only configuration the demo builds in. */
extern void wavedb_log_set_quiet(bool enable);

#ifdef SA_HAS_PYTHON
/* Reached through the bare extern (the test_loop.cpp idiom): py_agent.h
   would drag <Python.h> here. Idempotent; python boots lazily inside the
   frame machinery, but this call re-mounts the bridge reply sink. */
extern void py_agent_init(void);
#endif

static const char SA_DEMO_USAGE[] =
    "usage: frame-demo [--location <dir>] [--base-url <url>] --model <tag> "
    "--goal \"<text>\"\n"
    "  --location  WaveDB root directory (default ./sa-demo-db; created if "
    "absent)\n"
    "  --base-url  OpenAI-compatible endpoint (default "
    "http://127.0.0.1:11434)\n"
    "  --model     model tag, required (e.g. llama3)\n"
    "  --goal      goal text for the top frame, required\n";

/* Log-hook formatter: one line per event ("<level> <message>"), flushed so
   the stream is live even with stdout piped. */
static void _demo_log_line(log_Event* ev) {
  fprintf(stdout, "[%s] ", log_level_string(ev->level));
  if (ev->fmt != NULL) vfprintf(stdout, ev->fmt, ev->ap);
  fputc('\n', stdout);
  fflush(stdout);
}

static void _demo_on_sigint(int sig) {
  (void)sig;
  static const char notice[] =
      "\nframe-demo: interrupt requested — the frame layer has no interrupt "
      "entry point; exiting (committed state is already durable)\n";
  ssize_t printed = write(STDERR_FILENO, notice, sizeof(notice) - 1);
  (void)printed;
  _exit(130);
}

/* The audit dump: prints every stored event line, then the OUTCOME — the
   last msg.append with role "assistant" is the final assistant content
   (the turn that ended the loop); a top frame that ended via actor.report
   shows the frame.report text instead. Returns 0 when the frame ended done,
   nonzero when it is incomplete (error path). */
static int _demo_print_outcome(frame_t* f) {
  char* json = frame_debug_events(f);
  if (json == NULL) {
    fprintf(stderr, "frame-demo: cannot read '%s' events\n", frame_sid(f));
    return 1;
  }
  char* err = NULL;
  json_value_t* events = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  free(json);
  if (events == NULL || json_type(events) != JSON_ARRAY) {
    json_value_destroy(events);
    fprintf(stderr, "frame-demo: cannot parse '%s' events\n", frame_sid(f));
    return 1;
  }

  /* final_assistant/final_report stay pointed INTO the parsed tree, so the
     tree is destroyed only at the single exit below — the status/final
     prints happen while the strings are still alive. */
  const char* final_assistant = NULL;
  const char* final_report = NULL;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    json_value_t* payload = json_get(rec, "payload");
    if (payload == NULL) continue;
    const char* type_name = json_as_string(json_get(rec, "type"));
    if (type_name == NULL) continue;
    if (strcmp(type_name, "msg.append") == 0) {
      const char* role = json_as_string(json_get(payload, "role"));
      const char* content = json_as_string(json_get(payload, "content"));
      if (role == NULL) {
        printf("[event] msg.append <no role>: %s\n",
               (content != NULL) ? content : "");
      } else {
        printf("[event] msg.append %s: %s\n", role,
               (content != NULL) ? content : "");
        if (strcmp(role, "assistant") == 0 && content != NULL) {
          final_assistant = content;
        }
      }
    } else if (strcmp(type_name, "frame.report") == 0) {
      const char* text = json_as_string(json_get(payload, "text"));
      printf("[event] report: %s\n", (text != NULL) ? text : "");
      if (text != NULL) final_report = text;
    } else if (strcmp(type_name, "cell.result") == 0) {
      const char* text = json_as_string(json_get(payload, "text"));
      printf("[event] cell result (status %lld): %s\n",
             (long long)json_as_int(json_get(payload, "status")),
             (text != NULL) ? text : "");
    } else if (strcmp(type_name, "control") == 0) {
      const char* kind = json_as_string(json_get(payload, "kind"));
      const char* text = json_as_string(json_get(payload, "text"));
      printf("[event] control %s: %s\n", (kind != NULL) ? kind : "?",
             (text != NULL) ? text : "");
    }
    /* state.remember / cell.run / spawn / join: shape-only on the audit
       trail; their effects already stream through the log hook. */
  }

  const char* final_text = (final_assistant != NULL) ? final_assistant
                                                     : final_report;
  int rc;
  printf("status: %s\n", (frame_is_done(f) == 1) ? "done" : "incomplete");
  if (frame_is_done(f) != 1 || final_text == NULL || final_text[0] == '\0') {
    printf("final: (the frame ended without assistant content)\n");
    rc = (frame_is_done(f) == 1) ? 0 : 1;
  } else {
    printf("final: %s\n", final_text);
    rc = 0;
  }
  fflush(stdout);
  json_value_destroy(events);
  return rc;
}

int main(int argc, char** argv) {
  const char* location = "sa-demo-db";
  const char* base_url = "http://127.0.0.1:11434";
  const char* model = NULL;
  const char* goal = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--location") == 0 && i + 1 < argc) {
      location = argv[++i];
    } else if (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc) {
      base_url = argv[++i];
    } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
      model = argv[++i];
    } else if (strcmp(argv[i], "--goal") == 0 && i + 1 < argc) {
      goal = argv[++i];
    } else {
      fprintf(stderr, "frame-demo: unknown or incomplete argument '%s'\n%s",
              argv[i], SA_DEMO_USAGE);
      return 2;
    }
  }
  if (model == NULL || goal == NULL) {
    fprintf(stderr, "frame-demo: --model and --goal are required\n%s",
            SA_DEMO_USAGE);
    return 2;
  }

  signal(SIGINT, _demo_on_sigint);
  /* The stderr surface goes quiet; the hook below is the stream surface. */
  log_set_quiet(true);
  if (log_add_callback(_demo_log_line, NULL, LOG_INFO) != 0) {
    fprintf(stderr,
            "frame-demo: log callback table is full — running degraded "
            "(loop progress will not stream, the audit dump still prints)\n");
  }
  wavedb_log_set_quiet(true);
#ifdef SA_HAS_PYTHON
  py_agent_init();
#endif

  wave_database_root_t* db = wave_db_open(location);
  if (db == NULL) {
    fprintf(stderr, "frame-demo: cannot open the root db at '%s'\n", location);
    return 1;
  }

  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));   /* additive fields (pool) default sensibly */
  cfg.model_base_url = base_url;  /* api key — none for Ollama */
  cfg.model_name = model;
  cfg.max_depth = 4;
  frame_t* f = frame_create(db, NULL, goal, &cfg);
  if (f == NULL) {
    fprintf(stderr, "frame-demo: cannot create the top frame\n");
    wave_db_close(db);
    return 1;
  }
  printf("frame-demo: %s goal '%s' -> model %s at %s\n", frame_sid(f), goal,
         model, base_url);
  fflush(stdout);

  int loop_rc = frame_run_loop(f);
  int outcome_rc = _demo_print_outcome(f);

  frame_destroy(f);
  wave_db_close(db);
  return (loop_rc == 0 && outcome_rc == 0) ? 0 : 1;
}