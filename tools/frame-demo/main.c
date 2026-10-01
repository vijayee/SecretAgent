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
//
// REFINE (the refine slice): --refine/--refine-global review a run's
// trajectory after the loop and apply evidence-backed supplemental lessons;
// --refine-sid resumes a PAST session's subtree instead (no turn loop);
// --refine-rollback rolls one stored refinement back. The library never
// prints — every summary rides out of the demo here, verbatim.

#include "../../src/Frame/frame.h"
#include "../../src/Frame/loop.h"
#include "../../src/Frame/refine.h"
#include "../../src/Util/json.h"
#include "../../src/Util/log.h"

#include <errno.h>
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
    "  --goal      goal text for the top frame, required for a loop run\n"
    "  --refine       after the frame run, review this run's trajectory and "
    "apply evidence-backed supplemental lessons (the /refine "
    "command; prints the summary; a no-op prints loudly too)\n"
    "  --refine-global  same, but into the SHARED root harness subtree (the "
    "session subtree stays read-only context on shared runs; "
    "the shared scope is read-only context on local runs)\n"
    "  --refine-sid <sid>  run refine against a PAST session's stored "
    "trajectory (a sessions/<hex> path — the frame-demo "
    "printout at create time carries it); NO turn loop runs\n"
    "  --refine-rollback <seq>  roll back the refinement recorded at that "
    "harness-log seq (append-only: a NEW rollback record)\n"
    "  --refine and --refine-rollback are mutually exclusive\n";

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

/* The refine summary print: the summary rides out VERBATIM (fputs, exact
   bytes — the library never prints; the demo is its printing surface) on a
   commit or a loud no-op (rr 0 or 1). On rr -1 the summary is NULL by the
   refine.h contract and the log hook already carried the failure — the demo
   mirrors its existing engine-failure convention (a stderr line, exit 1)
   and prints nothing. Returns 0 for rr >= 0, 1 for the failure. */
static int _demo_print_refine_summary(int rr, char** summary) {
  if (rr < 0) {
    fprintf(stderr,
            "frame-demo: refine failed (the log carries the failure)\n");
    return 1;
  }
  if (*summary != NULL) {
    fputs(*summary, stdout);
    free(*summary);
    *summary = NULL;
    fflush(stdout);
  }
  return 0;
}

/* The --refine-sid value's frame path: frame_resume opens the FULL subtree
   path ("sessions/<hex>" — what frame_sid prints at create time), so a BARE
   hex is framed into "sessions/<hex>" here; anything containing '/' rides
   through as the path it claims to be. *framed_out carries the heap copy to
   free when a bare hex was framed. Returns 0 (with *path_out NULL when no
   --refine-sid was given — a fresh top frame) or -1 on out of memory. */
static int _demo_refine_sid_path(const char* refine_sid, const char** path_out,
                                 char** framed_out) {
  *framed_out = NULL;
  *path_out = NULL;
  if (refine_sid == NULL) return 0;
  if (strchr(refine_sid, '/') != NULL) {
    *path_out = refine_sid;
    return 0;
  }
  size_t len = strlen("sessions/") + strlen(refine_sid) + 1;
  char* framed = malloc(len);
  if (framed == NULL) return -1;
  snprintf(framed, len, "sessions/%s", refine_sid);
  *framed_out = framed;
  *path_out = framed;
  return 0;
}

int main(int argc, char** argv) {
  const char* location = "sa-demo-db";
  const char* base_url = "http://127.0.0.1:11434";
  const char* model = NULL;
  const char* goal = NULL;
  int want_refine = 0;          /* --refine */
  uint8_t refine_global = 0;    /* --refine-global (the SHARED harness root) */
  const char* refine_sid = NULL;  /* --refine-sid: the past session's subtree */
  int has_rollback = 0;         /* --refine-rollback */
  uint64_t rollback_seq = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--location") == 0 && i + 1 < argc) {
      location = argv[++i];
    } else if (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc) {
      base_url = argv[++i];
    } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
      model = argv[++i];
    } else if (strcmp(argv[i], "--goal") == 0 && i + 1 < argc) {
      goal = argv[++i];
    } else if (strcmp(argv[i], "--refine") == 0) {
      want_refine = 1;
    } else if (strcmp(argv[i], "--refine-global") == 0) {
      refine_global = 1;
    } else if (strcmp(argv[i], "--refine-sid") == 0 && i + 1 < argc) {
      refine_sid = argv[++i];
    } else if (strcmp(argv[i], "--refine-rollback") == 0 && i + 1 < argc) {
      const char* text = argv[++i];
      char* end = NULL;
      errno = 0;
      unsigned long long seq = strtoull(text, &end, 10);
      if (text[0] == '\0' || text[0] == '-' || end == text || *end != '\0' ||
          errno == ERANGE) {
        fprintf(stderr, "frame-demo: --refine-rollback needs a nonnegative "
                "harness-log seq (got '%s')\n%s", text, SA_DEMO_USAGE);
        return 2;
      }
      has_rollback = 1;
      rollback_seq = (uint64_t)seq;
    } else {
      fprintf(stderr, "frame-demo: unknown or incomplete argument '%s'\n%s",
              argv[i], SA_DEMO_USAGE);
      return 2;
    }
  }
  /* The two refine ACTIONS are mutually exclusive (stated in the usage
     text): --refine is a review-and-apply of a fresh run's trajectory,
     --refine-rollback un-does one recorded refinement. */
  if (want_refine && has_rollback) {
    fprintf(stderr, "frame-demo: --refine and --refine-rollback are mutually "
            "exclusive\n%s", SA_DEMO_USAGE);
    return 2;
  }
  if (model == NULL) {
    fprintf(stderr, "frame-demo: --model is required\n%s", SA_DEMO_USAGE);
    return 2;
  }
  /* --goal is optional only when the refine action supplies the session
     (a --refine-sid resume, or a --refine-rollback vehicle frame —
     frame_create's NULL goal is legal, frame.h's contract). A --refine
     without --goal and without --refine-sid has no session to review. */
  if (goal == NULL && refine_sid == NULL && !has_rollback) {
    fprintf(stderr, "frame-demo: --goal is required (or supply --refine-sid "
            "<sid>/<--refine-rollback <seq>'s session)\n%s", SA_DEMO_USAGE);
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

  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = location;   /* the demo's store stays INLINE — the engine's
                               driver pumps it (wave_db_open_config's NULL
                               store_pool shape) */
  wave_database_root_t* db = wave_db_open_config(&sc);
  if (db == NULL) {
    fprintf(stderr, "frame-demo: cannot open the root db at '%s'\n", location);
    return 1;
  }

  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));   /* additive fields (pool) default sensibly */
  cfg.model_base_url = base_url;  /* api key — none for Ollama */
  cfg.model_name = model;
  cfg.max_depth = 4;

  /* The refine session: a --refine-sid resume (framed to the subtree path),
     or a fresh top frame (goal may be NULL). */
  char* framed_sid = NULL;
  const char* resume_path = NULL;
  if (_demo_refine_sid_path(refine_sid, &resume_path, &framed_sid) != 0) {
    fprintf(stderr, "frame-demo: out of memory framing the --refine-sid "
            "session path\n");
    wave_db_close(db);
    return 1;
  }
  frame_t* f = NULL;
  if (resume_path != NULL) {
    f = frame_resume(db, resume_path, &cfg);
    if (f == NULL) {
      fprintf(stderr, "frame-demo: cannot resume the '%s' subtree "
              "(the frame layer's log carries the reason)\n", resume_path);
      free(framed_sid);
      wave_db_close(db);
      return 1;
    }
  } else {
    f = frame_create(db, NULL, goal, &cfg);
    if (f == NULL) {
      fprintf(stderr, "frame-demo: cannot create the top frame\n");
      wave_db_close(db);
      return 1;
    }
  }
  if (goal != NULL) {
    printf("frame-demo: %s goal '%s' -> model %s at %s\n", frame_sid(f), goal,
           model, base_url);
  } else {
    printf("frame-demo: %s (no goal) -> model %s at %s\n", frame_sid(f), model,
           base_url);
  }
  fflush(stdout);

  /* ROLLBACK (--refine-rollback, with the session from --refine-sid or the
     fresh frame above): no turn loop — the exact-record scan rides the
     stored refinement record alone. */
  if (has_rollback) {
    char* summary = NULL;
    int rr = refine_rollback(f, rollback_seq, refine_global, &summary);
    int rc = _demo_print_refine_summary(rr, &summary);
    frame_destroy(f);
    wave_db_close(db);
    free(framed_sid);
    return rc;
  }

  /* REFINE AGAINST A PAST SESSION (--refine-sid): no turn loop — refine's
     review rides the same configured endpoint over the stored trajectory. */
  if (refine_sid != NULL) {
    char* summary = NULL;
    int rr = refine_run(f, NULL, refine_global, &summary);
    int rc = _demo_print_refine_summary(rr, &summary);
    frame_destroy(f);
    wave_db_close(db);
    free(framed_sid);
    return rc;
  }

  int loop_rc = frame_run_loop(f);
  int outcome_rc = _demo_print_outcome(f);
  int rc = (loop_rc == 0 && outcome_rc == 0) ? 0 : 1;

  /* --refine: review THIS run's trajectory (instructions NULL = the
     built-in review contract) after the loop, whatever the loop's exit. */
  if (want_refine) {
    char* summary = NULL;
    int rr = refine_run(f, NULL, refine_global, &summary);
    if (_demo_print_refine_summary(rr, &summary) != 0) rc = 1;
  }

  frame_destroy(f);
  wave_db_close(db);
  return rc;
}