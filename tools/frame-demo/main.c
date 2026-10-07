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
// CTRL-C on the direct mode: _demo_on_sigint sets ONE flag (a raw handler
// can post or allocate nothing), and a 50 ms watcher thread — armed only
// around frame_run_loop — picks the flag up and calls frame_interrupt, the
// surface slice's real interrupt entry point (posts the FRM_INT mailbox
// message; safely callable from another thread while the caller stays
// blocked in the loop run). On the paths without a running loop
// (refine/rollback) or in the serve/client modes the flag IS the shutdown:
// those paths poll it (the daemon's main thread) or fall back to the
// documented exit — a SIGINT on an unwatched path still exits the process,
// leaving committed state durable in the store. Over the wire the interrupt
// enters through the client-api's CA_INTERRUPT_REQUEST instead (the client
// mode's sa_client_interrupt); the same frame_interrupt posts it.
//
// REFINE (the refine slice): --refine/--refine-global review a run's
// trajectory after the loop and apply evidence-backed supplemental lessons;
// --refine-sid resumes a PAST session's subtree instead (no turn loop);
// --refine-rollback rolls one stored refinement back. The library never
// prints — every summary rides out of the demo here, verbatim.
//
// SERVE (the client-api slice): `frame-demo serve` IS the runtime's daemon —
// one scheduler pool (the store's AND the frames'), one streams loop, the
// client-api session server over the frames' store, and the transports:
// unix ALWAYS on --socket-path (the socket file's permission is the auth),
// TCP only when --tcp-port AND --api-key are given together (the api key's
// bcrypt hash rides the transport; the plaintext key never leaves the
// daemon). CTRL-C stops accepting and tears down in handlers.h's pinned
// order, exit 0.
//
// CLIENT (the client-api slice): `frame-demo client` rides
// src/ClientLibs/c/sa_client — prompt the goal as a NEW session (sid NULL),
// print the response's sid + status, subscribe the session's event channel,
// and stream every committed store record VERBATIM (one JSON line per
// record, the seq prefixed; the client never re-parses a record) until
// CTRL-C (or the console's "quit") tears the client down.
//
// ESCALATION (the escalation slice): `serve --escalation plan-ask-act`
// (or `bypass`; free is the default) sets the frames' ladder — every
// api-created frame inherits it from the serve's template (the whole-struct
// template copy carries the field; frame_create's copy sites landed the
// inheritance). The client's stream tail ALSO reads the console: an "ask"
// record renders as a dialog (the question + the numbered options + the
// plan as an indented block), the `answer [sid ask_id] <pick|reject> [text]`
// command rides sa_client_ask_reply (a pick = an option's label or its
// 1-based index, or free text; the bare token "reject" refuses, its
// optional text becomes the value), and "quit" tears the client down (the
// session and its records stay in the daemon's store). THE DIRECT MODE's
// run-loop rc 2 (a LIVE yield) is NOT a failure: on the ask park the
// frame's own dialog renders and the answer rides frame_ask_reply from the
// SAME console (an abandoned wait leaves the frame parked — its ask record
// stays in the events log; a resume re-presents it); a live-children yield
// is pumped again.

#include "../../src/Frame/frame.h"
#include "../../src/Frame/loop.h"
#include "../../src/Frame/refine.h"
#include "../../src/Platform/platform.h"
#include "../../src/Scheduler/scheduler.h"
#include "../../src/Util/allocator.h"
#include "../../src/Util/json.h"
#include "../../src/Util/log.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The client-api surface (the serve/client modes) rides BOTH gates — the
   frames/store (WaveDB) and the loop thread (streams). A build without
   either compiles nothing here (the handlers' empty-TU idiom carried up into
   the demo): the mode entry points below refuse loud and return 2. */
#if defined(SA_HAS_WDB) && defined(SA_HAS_STREAMS)
#define SA_DEMO_HAS_CLIENT_API 1
#include "../../src/ClientApi/client_api_wire.h"
#include "../../src/ClientApi/handlers.h"
#include "../../src/ClientApi/Tcp/tcp_transport.h"
#include "../../src/ClientApi/Unix/unix_transport.h"
#include "../../src/ClientLibs/c/sa_client.h"
#include "../../src/Platform/platform_socket.h"
#include "../../src/Streams/loop_thread.h"
#include "../../src/Util/bcrypt.h"
#include <stdint.h>
#else
#define SA_DEMO_HAS_CLIENT_API 0
#endif

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
    "  --escalation   the escalation ladder: free (execute free; default) | "
    "plan-ask-act (plan\n"
    "                   turns gate on ONE owner approval — the parked ask "
    "is answered from\n"
    "                   this console) | bypass (DANGEROUS: plan turns "
    "auto-approve)\n"
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

static const char SA_DEMO_SERVE_USAGE[] =
    "usage: frame-demo serve --socket-path <path> --model <tag>\n"
    "                    [--location <dir>] [--base-url <url>]\n"
    "                    [--tcp-port <port> --api-key <text>]\n"
    "  --location     WaveDB root directory (default ./sa-demo-db; created if "
    "absent)\n"
    "  --base-url     OpenAI-compatible endpoint (default "
    "http://127.0.0.1:11434)\n"
    "  --model        model tag, required (e.g. llama3)\n"
    "  --socket-path  the daemon's AF_UNIX listen socket, required\n"
    "  --escalation   the frames' escalation ladder: free (default; execute "
    "free) |\n"
    "                 plan-ask-act (plan turns park ONE ask; the clients' "
    "answers\n"
    "                 resume them) | bypass (DANGEROUS: plan turns "
    "auto-approve)\n"
    "  --tcp-port     ALSO listen on 127.0.0.1:<port> — REQUIRES --api-key\n"
    "  --api-key      the TCP listeners' api key (bcrypt-hashed here; the "
    "client passes\n"
    "                 the SAME plaintext) — REQUIRES --tcp-port\n"
    "CTRL-C stops accepting and shuts the daemon down (exit 0).\n";

static const char SA_DEMO_CLIENT_USAGE[] =
    "usage: frame-demo client (--socket-path <path> | --tcp-port <port> "
    "--tcp-host <host> --api-key <text>) --goal \"<text>\"\n"
    "  --socket-path  the daemon's AF_UNIX socket (the plain shape — the "
    "socket file's\n"
    "                 own permission is the auth)\n"
    "  --tcp-host/--tcp-port/--api-key  connect over TCP instead (ALL THREE "
    "required\n"
    "                 together; the key must match the daemon's --api-key)\n"
    "  --goal         goal text, required — posted as a NEW session\n"
    "Prints the session's sid + the streamed store records (one JSON line "
    "per record,\n"
    "the seq prefixed) live, until CTRL-C (or the console's quit) tears the "
    "client down.\n"
    "While the stream tails, the console reads ONE command per line:\n"
    "  answer [<sid> <ask_id>] <pick|reject> [text]  reply to a parked ask\n"
    "                 (the sid/ask_id prefix is OPTIONAL — the last ask "
    "this client saw\n"
    "                 rides; a pick is an option's index or its label, or "
    "free text; the\n"
    "                 bare token reject refuses — its optional text becomes "
    "the value)\n"
    "  quit           tear the client down (the session and its records "
    "stay in the\n"
    "                 daemon's store — a later resume re-presents it)\n";

/* Log-hook formatter: one line per event ("<level> <message>"), flushed so
   the stream is live even with stdout piped. */
static void _demo_log_line(log_Event* ev) {
  fprintf(stdout, "[%s] ", log_level_string(ev->level));
  if (ev->fmt != NULL) vfprintf(stdout, ev->fmt, ev->ap);
  fputc('\n', stdout);
  fflush(stdout);
}

/* --- CTRL-C machinery -------------------------------------------------------
 * ONE flag, set by the handler; nothing else is async-signal-safe here
 * (frame_interrupt posts and allocates — a raw handler may never call it).
 * Who polls the flag:
 *   - the DIRECT loop: _demo_interrupt_watchdog, a 50 ms thread armed around
 *     frame_run_loop (it calls frame_interrupt when the flag rises);
 *   - SERVE/CLIENT: the mode's own main-thread wait loop;
 *   - an UNWATCHED path (refine/rollback; interrupted before the watcher was
 *     armed): the legacy documented exit — notice + 130, committed state is
 *     already durable. */
static volatile sig_atomic_t g_demo_sigint;    /* the handler's ONLY write */
static volatile sig_atomic_t g_demo_sigint_watched;   /* set 1: somebody polls */

static void _demo_on_sigint(int sig) {
  (void)sig;
  static const char notice[] =
      "\nframe-demo: interrupt requested\n";
  ssize_t printed = write(STDERR_FILENO, notice, sizeof(notice) - 1);
  (void)printed;
  g_demo_sigint = 1;
  if (!g_demo_sigint_watched) {
    static const char exit_note[] =
        "frame-demo: nothing is polling interrupts on this path — exiting "
        "(committed state is already durable)\n";
    printed = write(STDERR_FILENO, exit_note, sizeof(exit_note) - 1);
    (void)printed;
    _exit(130);
  }
}

static frame_t* g_demo_watch_frame;   /* armed around the direct loop read */

/* The DIRECT loop's interrupt watcher: a 50 ms tick converts the flag into
   the real interrupt. The frame stays valid from the arm to the disarm's
   join (the audit dump and destroy happen strictly after it), so the one
   post it may make never races the frame's teardown; if the loop ended on
   its own before the flag rose, the disarm's g_demo_watch_stop makes it
   exit post-less. */
static volatile sig_atomic_t g_demo_watch_stop;

static void* _demo_interrupt_watchdog(void* arg) {
  (void)arg;
  while (!g_demo_sigint && !g_demo_watch_stop) usleep(50000);
  if (g_demo_sigint) frame_interrupt(g_demo_watch_frame);
  return NULL;
}

/* Disarm + join the DIRECT loop's watchdog before ANY frame teardown:
   g_demo_watch_stop releases a loop that ended on its own; a flag already
   risen still fires its one frame_interrupt — on the frame the loop was
   driving (valid until this join returned, and an extra mailbox post on an
   ended frame is harmless — destroy drains it). */
static void _demo_watchdog_disarm(platform_thread_t** watchdog) {
  if (*watchdog != NULL) {
    g_demo_watch_stop = 1;
    platform_thread_join(*watchdog);
    *watchdog = NULL;
    g_demo_watch_frame = NULL;
    g_demo_watch_stop = 0;
  }
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
   commit or a loud no-op (rr 0 or 1), with ONE demo-side rule: when the
   frozen text itself carries no trailing newline (an empty summary takes
   the same path) the demo adds a single one after the fputs, so the
   summary never fuses with the next shell line. On rr -1 the summary is
   NULL by the refine.h contract and the log hook already carried the
   failure — the demo mirrors its existing engine-failure convention (a
   stderr line, exit 1) and prints nothing. Returns 0 for rr >= 0, 1 for
   the failure. */
static int _demo_print_refine_summary(int rr, char** summary) {
  if (rr < 0) {
    fprintf(stderr,
            "frame-demo: refine failed (the log carries the failure)\n");
    return 1;
  }
  if (*summary != NULL) {
    size_t len = strlen(*summary);
    fputs(*summary, stdout);
    if (len == 0 || (*summary)[len - 1] != '\n') putchar('\n');
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

/* --- THE ESCALATION SURFACE (shared: the DIRECT mode's parked-ask console
 *     AND the CLIENT mode's answer command) ---------------------------------- */

/* The ladder's argv word → the frame_escalation_mode_e value; -1 = unknown
   (the caller prints its own usage). Absent = free at every call site. */
static int _demo_parse_escalation(const char* text, unsigned* out) {
  if (strcmp(text, "free") == 0) {
    *out = FRAME_ESCALATION_FREE;
    return 0;
  }
  if (strcmp(text, "plan-ask-act") == 0) {
    *out = FRAME_ESCALATION_PLAN_ASK_ACT;
    return 0;
  }
  if (strcmp(text, "bypass") == 0) {
    *out = FRAME_ESCALATION_BYPASS;
    return 0;
  }
  return -1;
}

static const char* _demo_escalation_name(unsigned mode) {
  switch (mode) {
    case FRAME_ESCALATION_PLAN_ASK_ACT:
      return "plan-ask-act";
    case FRAME_ESCALATION_BYPASS:
      return "bypass";
    default:
      return "free";
  }
}

/* The project allocator's strdup (the style law's wrappers; the demo keeps
   its raw-free on the SAME pointers — no mixed-class free exists here). */
static char* _demo_dup(const char* text) {
  size_t len = strlen(text) + 1;
  char* out = (char*)get_memory(len);
  memcpy(out, text, len);
  return out;
}

/* One parked ask, as the demo's console sees it: strings COPIED off an
   "ask" record's payload (the record's own tree is freed right after the
   line renders; a slot outlives it — the client's registry, the direct
   mode's parked scan). sid is the registry's key (NULL in the direct
   mode's scan — the render takes the sid as the call's argument instead). */
typedef struct {
  char* sid;
  char* ask_id;
  char* question;
  char** options;   /* the labels; owned */
  size_t noptions;
  char* plan;       /* NULL = the record's null plan */
} demo_ask_slot_t;

static void _demo_ask_slot_clear(demo_ask_slot_t* ask) {
  if (ask == NULL) return;
  free(ask->sid);
  free(ask->ask_id);
  free(ask->question);
  if (ask->options != NULL) {
    for (size_t i = 0; i < ask->noptions; i++) free(ask->options[i]);
    free(ask->options);
  }
  free(ask->plan);
  memset(ask, 0, sizeof(*ask));
}

/* A DEEP copy (the registry's read hands a local the caller frees without
   racing the registry's slot). */
static void _demo_ask_slot_copy(const demo_ask_slot_t* src,
                                demo_ask_slot_t* dst) {
  memset(dst, 0, sizeof(*dst));
  if (src == NULL || src->ask_id == NULL) return;
  dst->sid = (src->sid != NULL) ? _demo_dup(src->sid) : NULL;
  dst->ask_id = _demo_dup(src->ask_id);
  dst->question = (src->question != NULL) ? _demo_dup(src->question) : NULL;
  dst->noptions = src->noptions;
  if (src->noptions > 0 && src->options != NULL) {
    dst->options = (char**)get_clear_memory(src->noptions * sizeof(char*));
    for (size_t i = 0; i < src->noptions; i++) {
      dst->options[i] = _demo_dup((src->options[i] != NULL)
                                      ? src->options[i] : "");
    }
  }
  dst->plan = (src->plan != NULL) ? _demo_dup(src->plan) : NULL;
}

/* Fills the slot from an "ask" record's payload ({kind, askId, question,
   options[], plan} — frame.c's ONE composer's shape). 0 = filled; -1 = not
   the shape (the caller prints the raw record line instead). */
static int _demo_ask_slot_fill(demo_ask_slot_t* ask,
                               const json_value_t* payload) {
  const char* ask_id = json_as_string(json_get(payload, "askId"));
  const char* question = json_as_string(json_get(payload, "question"));
  json_value_t* options = json_get(payload, "options");
  if (ask_id == NULL || question == NULL ||
      (options != NULL && json_type(options) != JSON_ARRAY)) {
    return -1;
  }
  size_t noptions = (options != NULL) ? json_size(options) : 0;
  char** labels = NULL;
  if (noptions > 0) {
    labels = (char**)get_clear_memory(noptions * sizeof(char*));
    for (size_t i = 0; i < noptions; i++) {
      const char* label = json_as_string(json_at(options, i));
      labels[i] = _demo_dup((label != NULL) ? label : "");
    }
  }
  const char* plan = json_as_string(json_get(payload, "plan"));
  ask->ask_id = _demo_dup(ask_id);
  ask->question = _demo_dup(question);
  ask->options = labels;
  ask->noptions = noptions;
  ask->plan = (plan != NULL) ? _demo_dup(plan) : NULL;
  return 0;
}

/* The ask's DIALOG render (the spec §3.4's shape): the header, the
   question, the plan as an indented block, the numbered options, the
   console's hint line. */
static void _demo_ask_render(const demo_ask_slot_t* ask, const char* sid,
                             const char* hint) {
  printf("== ASK %s %s ==\n", (sid != NULL) ? sid : "?", ask->ask_id);
  printf("  %s\n", ask->question);
  if (ask->plan != NULL) {
    printf("  ");
    for (const char* p = ask->plan; *p != '\0'; p++) {
      if (*p == '\n') {
        if (p[1] == '\0') break;   /* the trailing newline rides unprinted */
        putchar('\n');
        printf("  ");
      } else {
        putchar(*p);
      }
    }
    putchar('\n');
  }
  for (size_t i = 0; i < ask->noptions; i++) {
    printf("   %zu. %s\n", i + 1, ask->options[i]);
  }
  printf("   (%s)\n", hint);
}

/* --- THE CONSOLE'S ANSWER GRAMMAR --------------------------------------------
 * `answer [<sid> <ask_id>] <pick|reject> [text]`
 *   The LONG form names the session and the ask outright. The SHORT form
 *   rides the LAST UNPARKED ASK the mode remembers (the client: the events
 *   registry; the direct mode: the parked scan — its run owns exactly ONE
 *   frame). The long form is honest there too: its sid/ask_id prefix is
 *   documentation, never a filter (the frame owns its ask).
 *   `reject` as the first value token makes the reply a REFUSAL (its
 *   optional rest is the refusal's value, verbatim). Otherwise the rest is
 *   the VALUE: an exact option label rides as the label, a decimal index
 *   maps to its label, anything else is free text. */

typedef struct {
  char* sid;      /* the long form's; NULL = the short form */
  char* ask_id;   /* the long form's; NULL = the short form */
  uint8_t reject; /* 1 = the bare reject token */
  char* value;    /* heap: the joined rest (unresolved); NULL = absent */
} demo_answer_line_t;

static void _demo_answer_line_clear(demo_answer_line_t* parts) {
  free(parts->sid);
  free(parts->ask_id);
  free(parts->value);
  memset(parts, 0, sizeof(*parts));
}

/* Returns 1 = an answer line (parts filled — clear them); 0 = NOT an
   answer line (the caller's other commands); -1 = malformed (the usage
   one-liner). */
static int _demo_answer_line_parse(const char* line,
                                   demo_answer_line_t* parts) {
  memset(parts, 0, sizeof(*parts));
  size_t len = strlen(line);
  char* work = (char*)get_memory(len + 1);
  memcpy(work, line, len + 1);
  char* save = NULL;
  const char* delims = " \t";
  char* cmd = strtok_r(work, delims, &save);
  if (cmd == NULL || strcmp(cmd, "answer") != 0) {
    free(work);
    return 0;   /* whitespace only, or another command */
  }
  char* tok = strtok_r(NULL, delims, &save);
  if (tok == NULL) {
    free(work);
    return -1;   /* a bare "answer" */
  }
  if (strchr(tok, '/') != NULL) {
    /* THE LONG FORM: the sid path carries the '/' (the free text never
       does); the ask_id is its next token — required. */
    parts->sid = _demo_dup(tok);
    tok = strtok_r(NULL, delims, &save);
    if (tok == NULL) {
      _demo_answer_line_clear(parts);
      free(work);
      return -1;
    }
    parts->ask_id = _demo_dup(tok);
    tok = strtok_r(NULL, delims, &save);
  }
  if (tok == NULL) {
    _demo_answer_line_clear(parts);
    free(work);
    return -1;
  }
  if (strcmp(tok, "reject") == 0) {
    parts->reject = 1;
    tok = strtok_r(NULL, delims, &save);
  }   /* else the value rides, tok first */
  /* The rest's join (single spaces; the free text never needs the original
     spacing in a console demo). */
  while (tok != NULL) {
    size_t tlen = strlen(tok);
    size_t vlen = (parts->value != NULL) ? strlen(parts->value) : 0;
    char* grown = (parts->value != NULL)
                      ? (char*)get_memory(vlen + tlen + 2)
                      : (char*)get_memory(tlen + 1);
    if (parts->value != NULL) {
      memcpy(grown, parts->value, vlen);
      grown[vlen] = ' ';
      free(parts->value);
    }
    memcpy(grown + vlen + (parts->value != NULL ? 1 : 0), tok, tlen + 1);
    parts->value = grown;
    tok = strtok_r(NULL, delims, &save);
  }
  free(work);
  return 1;
}

/* Resolves *value_ptr against the ask's options: the exact label wins, a
   1..noptions decimal index maps to its label, anything else rides free
   text. Frees and replaces the incoming buffer when the resolution mapped. */
static void _demo_answer_resolve_value(const demo_ask_slot_t* ask,
                                       char** value_ptr) {
  if (*value_ptr == NULL || ask == NULL) return;
  for (size_t i = 0; i < ask->noptions; i++) {
    if (strcmp(*value_ptr, ask->options[i]) == 0) return;   /* the exact
                                                               label */
  }
  size_t len = strlen(*value_ptr);
  size_t digits = 0;
  for (size_t i = 0; i < len; i++) {
    if (isdigit((unsigned char)(*value_ptr)[i]) == 0) break;
    digits++;
  }
  if (digits == len && len > 0) {
    errno = 0;
    unsigned long idx = strtoul(*value_ptr, NULL, 10);
    if (errno == 0 && idx >= 1 && idx <= ask->noptions) {
      free(*value_ptr);
      *value_ptr = _demo_dup(ask->options[idx - 1]);
    }
    return;   /* a number past the options rides free text */
  }
}

/* The DIRECT mode's parked-ask scan: the public surface holds no
   pending-ask accessor, so the demo reads the events log's truth — the
   LAST "ask" record with NO matching "ask.reply" after it (the engine
   keeps ONE park at a time, so that IS the parked one). Fills the
   caller's slot (cleared by the CALLER); 1 = parked, 0 = not. */
static int _demo_parked_ask_scan(frame_t* f, demo_ask_slot_t* slot) {
  memset(slot, 0, sizeof(*slot));
  char* json = frame_debug_events(f);
  if (json == NULL) return 0;
  char* err = NULL;
  json_value_t* events = json_parse(json, strlen(json), &err);
  if (err != NULL) free(err);
  free(json);
  if (events == NULL) return 0;
  int parked = 0;
  for (size_t i = 0; i < json_size(events); i++) {
    json_value_t* rec = json_at(events, i);
    const char* type = json_as_string(json_get(rec, "type"));
    if (type == NULL) continue;
    if (strcmp(type, "ask") == 0) {
      _demo_ask_slot_clear(slot);
      /* a refill's shape refusal clears the running park too — the slot
         the caller renders is only EVER a filled one */
      parked = (_demo_ask_slot_fill(slot, json_get(rec, "payload")) == 0)
                   ? 1 : 0;
    } else if (strcmp(type, "ask.reply") == 0) {
      const char* reply_id =
          json_as_string(json_get(json_get(rec, "payload"), "askId"));
      if (reply_id != NULL && slot->ask_id != NULL &&
          strcmp(reply_id, slot->ask_id) == 0) {
        _demo_ask_slot_clear(slot);
        parked = 0;
      }
    }
  }
  json_value_destroy(events);
  if (parked == 0) _demo_ask_slot_clear(slot);
  return parked;
}

/* --- THE CONSOLE'S STDIN PUMP -------------------------------------------------
 * poll(2) on fd 0 — the demo's OWN standing raw-POSIX idiom (the SIGINT
 * handler's write(2), the unistd includes; the platform layer has no
 * fd-poll surface — its poll-dancer watchers are socket watchers), so the
 * demo keeps the honest minimal tool. A partial line waits in the buffer;
 * bytes past a buffer's cap drop with a note (the demo never grows
 * unbounded); EOF after a partial line flushes the rest as the line. */
typedef struct {
  char pending[1024];
  size_t len;
  int eof;
} demo_stdin_buf_t;

/* Returns 1 = a full line at `line` ('\n' stripped, NUL-terminated),
   0 = nothing yet (a no-data timeout, an EINTR — the caller's wait state
   re-checks its own flag), -1 = EOF. timeout_ms < 0 blocks. */
static int _demo_stdin_line(demo_stdin_buf_t* b, int timeout_ms, char* line,
                            size_t cap) {
  if (b->eof != 0) return -1;
  char chunk[256];
  for (;;) {
    /* the pending tail's first full line (or none) */
    size_t nl = 0;
    while (nl < b->len && b->pending[nl] != '\n') nl++;
    if (nl < b->len) {
      size_t take = (nl < cap - 1) ? nl : cap - 1;
      memcpy(line, b->pending, take);
      line[take] = '\0';
      if (nl >= cap - 1) {
        fprintf(stderr, "frame-demo: a console line longer than %zu bytes "
                "was truncated\n", cap - 1);
      }
      memmove(b->pending, b->pending + nl + 1, b->len - nl - 1);
      b->len -= nl + 1;
      return 1;
    }
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return 0;
    ssize_t got = read(STDIN_FILENO, chunk, sizeof(chunk));
    if (got < 0) return 0;   /* EINTR (or a transient error) — the caller
                                re-checks its own wait state */
    if (got == 0) {
      b->eof = 1;
      if (b->len == 0) return -1;
      size_t take = (b->len < cap - 1) ? b->len : cap - 1;
      memcpy(line, b->pending, take);
      line[take] = '\0';
      b->len = 0;
      return 1;   /* the file's trailing partial rides as its last line;
                     the NEXT call returns the EOF */
    }
    for (ssize_t i = 0; i < got; i++) {
      if (b->len >= sizeof(b->pending)) {
        fprintf(stderr, "frame-demo: a console line overflowed the %zu byte "
                "buffer — its head rides, its tail was dropped\n",
                sizeof(b->pending) - 1);
        break;
      }
      b->pending[b->len++] = chunk[i];
    }
  }
}

/* The console's command one-liner (parse refusals print it). */
static const char SA_DEMO_ANSWER_USAGE_LINE[] =
    "commands: answer [<sid> <ask_id>] <pick|reject> [text] | quit";

/* The DIRECT mode's parked-ask console: the dialog renders, then lines read
   (BLOCKING — this thread owns stdin while the parked engine idles) until
   a parseable reply rides frame_ask_reply (returns 1 → the outer loop
   re-pumps the run — the reply's records land through the frame's own
   dispatch), or the wait is ABANDONED ('quit', stdin's EOF, an interrupt
   flag — returns 0; the frame stays parked and the demo exits: the ask
   record stays in the events log, a later resume re-presents it). */
static int _demo_direct_parked_wait(frame_t* f, const demo_ask_slot_t* ask,
                                    demo_stdin_buf_t* in) {
  char hint[128];
  snprintf(hint, sizeof(hint),
           "answer <pick|reject> [text] — 'quit' leaves the frame parked");
  _demo_ask_render(ask, frame_sid(f), hint);
  fflush(stdout);
  for (;;) {
    if (g_demo_sigint != 0) return 0;
    printf("frame-demo: answer? ");
    fflush(stdout);
    char line[1024];
    int have = _demo_stdin_line(in, -1, line, sizeof(line));
    if (have < 0) return 0;    /* stdin closed — an honest abandon */
    if (have == 0) continue;   /* EINTR: the flag drive re-checked above */
    if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) return 0;
    demo_answer_line_t parts;
    int parsed = _demo_answer_line_parse(line, &parts);
    if (parsed <= 0) {
      printf("frame-demo: %s\n", SA_DEMO_ANSWER_USAGE_LINE);
      fflush(stdout);
      continue;
    }
    _demo_answer_resolve_value(ask, &parts.value);
    if (parts.reject == 0 &&
        (parts.value == NULL || parts.value[0] == '\0')) {
      fprintf(stderr, "frame-demo: an answer carries its value (an option's "
              "label or index, or free text — 'reject' refuses)\n");
      _demo_answer_line_clear(&parts);
      continue;
    }
    int posted = frame_ask_reply(f, ask->ask_id, parts.reject, parts.value);
    _demo_answer_line_clear(&parts);
    if (posted != 0) {
      fprintf(stderr, "frame-demo: the reply was refused (the frame layer's "
              "log carries the reason)\n");
      continue;
    }
    printf("frame-demo: the reply was posted — the frame resumes (the "
           "reply's records ride the events log)\n");
    fflush(stdout);
    return 1;
  }
}

#if SA_DEMO_HAS_CLIENT_API

/* --- SERVE: the daemon (the client-api slice) ------------------------------- */

typedef struct {
  const char* socket_path;
  const char* tcp_port;   /* the TCP listener: set ONLY WITH tcp_key */
  const char* tcp_key;    /* (required together — refused loud otherwise) */
  const char* location;
  const char* base_url;
  const char* model;
  unsigned escalation;    /* frame_escalation_mode_e; free when the flag is
                             absent — every api-created frame inherits it
                             from the server's template */
} demo_serve_args_t;

/* Parses a uint16 port text (digits only, no ranges, ERANGE caught). */
static int _demo_parse_port(const char* text, uint16_t* out) {
  char* end = NULL;
  errno = 0;
  unsigned long port = strtoul(text, &end, 10);
  if (!isdigit((unsigned char)text[0]) || end == text || *end != '\0' ||
      errno == ERANGE || port > 65535u) {
    return -1;
  }
  *out = (uint16_t)port;
  return 0;
}

/* Returns 0 = parsed; 1 = a bad/incomplete argument printed (exit 2's
   path). *tcp_port_out carries the parsed port (0 when absent). */
static int _demo_parse_serve(int argc, char** argv, demo_serve_args_t* a,
                             uint16_t* tcp_port_out) {
  memset(a, 0, sizeof(*a));
  a->location = "sa-demo-db";
  a->base_url = "http://127.0.0.1:11434";
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
      a->socket_path = argv[++i];
    } else if (strcmp(argv[i], "--location") == 0 && i + 1 < argc) {
      a->location = argv[++i];
    } else if (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc) {
      a->base_url = argv[++i];
    } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
      a->model = argv[++i];
    } else if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
      a->tcp_key = argv[++i];
    } else if (strcmp(argv[i], "--tcp-port") == 0 && i + 1 < argc) {
      a->tcp_port = argv[++i];
    } else if (strcmp(argv[i], "--escalation") == 0 && i + 1 < argc) {
      if (_demo_parse_escalation(argv[i + 1], &a->escalation) != 0) {
        fprintf(stderr, "frame-demo serve: --escalation needs free, "
                "plan-ask-act, or bypass (got '%s')\n%s", argv[i + 1],
                SA_DEMO_SERVE_USAGE);
        return 1;
      }
      i++;
    } else {
      fprintf(stderr, "frame-demo: unknown or incomplete argument '%s'\n%s",
              argv[i], SA_DEMO_SERVE_USAGE);
      return 1;
    }
  }
  if (a->socket_path == NULL) {
    fprintf(stderr, "frame-demo serve: --socket-path is required\n%s",
            SA_DEMO_SERVE_USAGE);
    return 1;
  }
  /* TCP is opt-IN and needs the key: --tcp-port and --api-key are required
     together (a network listener never rides unauthenticated, and a key
     without a TCP port would ride silently ignored). Refused BEFORE the
     --model check — the specific refusal names the real problem. */
  if ((a->tcp_port != NULL) != (a->tcp_key != NULL)) {
    fprintf(stderr, "frame-demo serve: --tcp-port and --api-key are "
            "required together\n%s", SA_DEMO_SERVE_USAGE);
    return 1;
  }
  if (a->model == NULL) {
    fprintf(stderr, "frame-demo serve: --model is required\n%s",
            SA_DEMO_SERVE_USAGE);
    return 1;
  }
  if (a->tcp_port != NULL &&
      _demo_parse_port(a->tcp_port, tcp_port_out) != 0) {
    fprintf(stderr, "frame-demo serve: --tcp-port needs a numeric port "
            "0..65535 (got '%s')\n%s", a->tcp_port, SA_DEMO_SERVE_USAGE);
    return 1;
  }
  return 0;
}

/* The serve teardown: handlers.h's PINNED order verbatim — the transports
   first (each transport's destroy closes every connection; each teardown
   calls ca_session_conn_closed — "cons closed"), then the server destroy
   while the pool still runs, then pool stop, db close, pool destroy, loop
   destroy. Runs on BOTH the natural path and SIGINT's (the flag's main-
   thread poll). */
static void _demo_serve_teardown(tcp_transport_t* tcp_transport,
                                 unix_transport_t* unix_transport,
                                 ca_session_server_t* server,
                                 scheduler_pool_t* pool,
                                 wave_database_root_t* db,
                                 streams_loop_thread_t* loop) {
  if (tcp_transport != NULL) tcp_transport_destroy(tcp_transport);
  if (unix_transport != NULL) unix_transport_destroy(unix_transport);
  if (server != NULL) ca_session_server_destroy(server);
  if (pool != NULL) scheduler_pool_stop(pool);
  if (db != NULL) wave_db_close(db);
  if (pool != NULL) scheduler_pool_destroy(pool);
  if (loop != NULL) streams_loop_destroy(loop);
}

/* The serve run: the runtime + the client-api stack wired the tests'
   fixture's shape — ONE scheduler pool (the store's AND every api-created
   frame's; a pooled frame requires a pooled store, handlers.h's note), ONE
   streams loop (the server's one marshal loop), the session server built
   with a NULL shared backend (every api-created frame builds its OWN
   model_http_backend_create from the template's wiring, exactly the direct
   mode's per-frame default). */
static int _demo_serve_run(int argc, char** argv) {
  demo_serve_args_t a;
  uint16_t tcp_port = 0;
  if (_demo_parse_serve(argc, argv, &a, &tcp_port) == 1) return 2;

  /* The daemon's SIGINT: the flag rides this mode's main-thread wait loop
     (below); the handler itself still only sets the flag. */
  signal(SIGINT, _demo_on_sigint);
  g_demo_sigint_watched = 1;

  scheduler_pool_t* pool = NULL;
  wave_database_root_t* db = NULL;
  streams_loop_thread_t* loop = NULL;
  ca_session_server_t* server = NULL;
  unix_transport_t* unix_transport = NULL;
  tcp_transport_t* tcp_transport = NULL;

  /* The stderr surface goes quiet; the hook below is the stream surface. */
  log_set_quiet(true);
  if (log_add_callback(_demo_log_line, NULL, LOG_INFO) != 0) {
    fprintf(stderr,
            "frame-demo: log callback table is full — running degraded "
            "(loop progress will not stream)\n");
  }
  wavedb_log_set_quiet(true);
#ifdef SA_HAS_PYTHON
  py_agent_init();
#endif

  pool = scheduler_pool_create(2);
  if (pool == NULL) {
    fprintf(stderr, "frame-demo serve: cannot create the scheduler pool\n");
    return 1;
  }
  scheduler_pool_start(pool);
  wave_database_config_t sc;
  memset(&sc, 0, sizeof(sc));
  sc.location = a.location;   /* REAL disk (create-if-absent) — the daemon
                                  serves the SAME subtrees a restart re-opens
                                  (wave_db_open_config's POOLED shape; the
                                  engine's workers pace it) */
  sc.store_pool = pool;
  db = wave_db_open_config(&sc);
  if (db == NULL) {
    fprintf(stderr, "frame-demo serve: cannot open the root db at '%s'\n",
            a.location);
    _demo_serve_teardown(NULL, NULL, NULL, pool, db, loop);
    return 1;
  }
  loop = streams_loop_create();
  if (loop == NULL) {
    fprintf(stderr, "frame-demo serve: cannot create the streams loop\n");
    _demo_serve_teardown(NULL, NULL, NULL, pool, db, loop);
    return 1;
  }
  frame_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.model_base_url = a.base_url;
  cfg.model_name = a.model;
  cfg.max_depth = 4;
  cfg.escalation_mode = a.escalation;   /* the ladder rides the WHOLE-struct
                                           template copy (ca_session_
                                           server_create's server->cfg =
                                           *frame_cfg) into EVERY
                                           api-created frame — frames'
                                           children inherit it too */
  server = ca_session_server_create(db, pool, loop, &cfg, NULL);
  if (server == NULL) {
    fprintf(stderr, "frame-demo serve: cannot create the session server\n");
    _demo_serve_teardown(NULL, NULL, NULL, pool, db, loop);
    return 1;
  }
  unix_transport = unix_transport_create(pool, server, a.socket_path);
  if (unix_transport == NULL) {
    fprintf(stderr, "frame-demo serve: cannot listen on the unix socket "
            "'%s'\n", a.socket_path);
    _demo_serve_teardown(NULL, NULL, server, pool, db, loop);
    return 1;
  }
  unix_transport_start(unix_transport);
  printf("frame-demo serve: serving sessions over unix at %s\n"
         "                model %s at %s, store %s, escalation %s\n",
         a.socket_path, a.model, a.base_url, a.location,
         _demo_escalation_name(a.escalation));
  if (a.tcp_port != NULL) {
    /* The api key's BCRYPT HASH rides the transport (the wire auth's
       presented key verify = bcrypt_check against it — the same machinery
       auth_middleware.c drives); the plaintext stays in the daemon's argv
       only. bcrypt_generate's RNG draw is platform_random_bytes. */
    char hash[64];
    if (bcrypt_generate(a.tcp_key, 12, hash, sizeof(hash)) != 0) {
      fprintf(stderr, "frame-demo serve: cannot bcrypt the api key\n");
      _demo_serve_teardown(NULL, unix_transport, server, pool, db, loop);
      return 1;
    }
    platform_address_t bound;
    memset(&bound, 0, sizeof(bound));
    tcp_transport = tcp_transport_create(pool, server, "127.0.0.1", tcp_port,
                                         hash, &bound);
    memset(hash, 0, sizeof(hash));   /* the hash copy dies with the
                                        transport; scrub this stack copy */
    if (tcp_transport == NULL) {
      fprintf(stderr, "frame-demo serve: cannot listen on tcp 127.0.0.1:%u\n",
              (unsigned)tcp_port);
      _demo_serve_teardown(NULL, unix_transport, server, pool, db, loop);
      return 1;
    }
    tcp_transport_start(tcp_transport);
    /* The platform prefers a dual-stack bind (the listen helper): the real
       port sits in the family's own member (the inet6 member overlaps the
       union). */
    unsigned bound_port = (bound.family == PLATFORM_AF_INET6)
                              ? (unsigned)bound.inet6.port
                              : (unsigned)bound.inet.port;
    printf("frame-demo serve: also listening on tcp 127.0.0.1:%u "
           "(api-key authed)\n", bound_port);
  }
  printf("frame-demo serve: ready — Ctrl-C shuts down cleanly\n");
  fflush(stdout);

  /* The daemon's main thread does nothing but wait: the transports' accept
     threads, the pool workers and the server's loop thread carry the work.
     The 50 ms poll IS this mode's SIGINT path (no watcher thread needed —
     nothing here is blocked). */
  g_demo_sigint_watched = 1;
  while (!g_demo_sigint) usleep(50000);
  fprintf(stderr, "frame-demo serve: shutting down (the pinned teardown "
          "order: transports, then the server, then the pools)\n");

  _demo_serve_teardown(tcp_transport, unix_transport, server, pool, db, loop);
  fprintf(stderr, "frame-demo serve: down\n");
  return 0;
}

/* --- CLIENT: sa_client over the served wire --------------------------------- */

typedef struct {
  const char* socket_path;
  const char* tcp_host;   /* default 127.0.0.1; requires tcp_port + api_key */
  const char* tcp_port;
  const char* api_key;
  const char* goal;
} demo_client_args_t;

/* Returns 0 = parsed; 1 = a bad/incomplete argument printed. The transport
   choice is an honest XOR: a socket path OR the tcp trio (--tcp-port and
   --api-key required together; --tcp-host defaults to loopback) — both sets
   at once is refused loud. */
static int _demo_parse_client(int argc, char** argv, demo_client_args_t* a,
                              uint16_t* tcp_port_out) {
  memset(a, 0, sizeof(*a));
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
      a->socket_path = argv[++i];
    } else if (strcmp(argv[i], "--tcp-host") == 0 && i + 1 < argc) {
      a->tcp_host = argv[++i];
    } else if (strcmp(argv[i], "--tcp-port") == 0 && i + 1 < argc) {
      a->tcp_port = argv[++i];
    } else if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
      a->api_key = argv[++i];
    } else if (strcmp(argv[i], "--goal") == 0 && i + 1 < argc) {
      a->goal = argv[++i];
    } else if (strcmp(argv[i], "--location") == 0 && i + 1 < argc) {
      /* the client holds NO store (the daemon owns it) — refused loud so the
         flag never rides silently ignored */
      fprintf(stderr, "frame-demo client: --location is the daemon's flag "
              "(the client opens no store)\n%s", SA_DEMO_CLIENT_USAGE);
      return 1;
    } else {
      fprintf(stderr, "frame-demo: unknown or incomplete argument '%s'\n%s",
              argv[i], SA_DEMO_CLIENT_USAGE);
      return 1;
    }
  }
  if (a->goal == NULL) {
    fprintf(stderr, "frame-demo client: --goal is required\n%s",
            SA_DEMO_CLIENT_USAGE);
    return 1;
  }
  if (a->socket_path != NULL && a->tcp_port != NULL) {
    fprintf(stderr, "frame-demo client: --socket-path and --tcp-port are "
            "mutually exclusive (one transport per connection)\n%s",
            SA_DEMO_CLIENT_USAGE);
    return 1;
  }
  if (a->socket_path == NULL && (a->tcp_port == NULL || a->api_key == NULL)) {
    fprintf(stderr, "frame-demo client: --socket-path, or --tcp-port with "
            "--api-key (and --tcp-host), is required\n%s",
            SA_DEMO_CLIENT_USAGE);
    return 1;
  }
  if (a->tcp_port != NULL) {
    if (_demo_parse_port(a->tcp_port, tcp_port_out) != 0) {
      fprintf(stderr, "frame-demo client: --tcp-port needs a numeric port "
              "0..65535 (got '%s')\n%s", a->tcp_port, SA_DEMO_CLIENT_USAGE);
      return 1;
    }
    if (a->tcp_host == NULL) a->tcp_host = "127.0.0.1";
  }
  return 0;
}

typedef struct {
  uint8_t status;
  char sid[CA_WIRE_SID_MAX + 1];
  int called;
  sa_client_t* client;   /* release_payload's target */
  /* THE PARKED-ASK REGISTRY (the escalation slice; the LAST unmarched ask
     per seen sid — the asks clear when their ask.reply record lands): the
     events callback (the reader thread) writes it, the console dispatcher
     (the main thread) reads it — one platform mutex orders the two. A NULL
     lock runs the registry degraded (the long-form answer still rides —
     only the short form and the label/index resolution need it). */
  platform_mutex_t* asks_lock;
  demo_ask_slot_t asks[4];
} demo_prompt_result_t;

static void _demo_prompt_cb(void* ctx, uint8_t status, const char* sid) {
  demo_prompt_result_t* r = (demo_prompt_result_t*)ctx;
  r->called = 1;
  r->status = status;
  if (sid != NULL) {
    snprintf(r->sid, sizeof(r->sid), "%s", sid);   /* copied BEFORE the
                                                      release below */
    sa_client_release_payload(r->client, (void*)sid);
  }
}

/* --- the parked-ask registry (the events callback's thread vs the console
   dispatcher's thread — the lock orders them) ----------------------------- */

/* STEALS the slot's strings on EVERY path (the caller's slot is left
   empty): the sid's slot REPLACES (the LAST ask per sid stands — a
   superseded ask's entry dies here). */
static void _demo_asks_set(demo_prompt_result_t* r, const char* sid,
                           demo_ask_slot_t* ask) {
  if (ask->ask_id == NULL) {
    _demo_ask_slot_clear(ask);
    return;
  }
  if (r->asks_lock == NULL || sid == NULL) {
    _demo_ask_slot_clear(ask);
    return;
  }
  platform_mutex_lock(r->asks_lock);
  size_t matched = 4;
  size_t free_slot = 4;
  for (size_t i = 0; i < 4; i++) {
    if (r->asks[i].sid != NULL && strcmp(r->asks[i].sid, sid) == 0) {
      matched = i;
      break;
    }
    if (free_slot == 4 && r->asks[i].ask_id == NULL) free_slot = i;
  }
  demo_ask_slot_t* target = (matched < 4)
                                ? &r->asks[matched]
                                : ((free_slot < 4) ? &r->asks[free_slot]
                                                   : &r->asks[0]);
  _demo_ask_slot_clear(target);
  *target = *ask;          /* the strings transfer */
  target->sid = _demo_dup(sid);
  memset(ask, 0, sizeof(*ask));
  platform_mutex_unlock(r->asks_lock);
}

/* An ask.reply's record lands: its ask's entry clears (the registry holds
   only UNPARKED asks). */
static void _demo_asks_clear_reply(demo_prompt_result_t* r,
                                   const char* ask_id) {
  if (r->asks_lock == NULL || ask_id == NULL) return;
  platform_mutex_lock(r->asks_lock);
  for (size_t i = 0; i < 4; i++) {
    if (r->asks[i].ask_id != NULL &&
        strcmp(r->asks[i].ask_id, ask_id) == 0) {
      _demo_ask_slot_clear(&r->asks[i]);
    }
  }
  platform_mutex_unlock(r->asks_lock);
}

static void _demo_asks_teardown(demo_prompt_result_t* r) {
  for (size_t i = 0; i < 4; i++) _demo_ask_slot_clear(&r->asks[i]);
  if (r->asks_lock != NULL) {
    platform_mutex_destroy(r->asks_lock);
    r->asks_lock = NULL;
  }
}

/* The registry's READ (the main thread, before a dispatch): a DEEP copy
   into *out (an empty slot when untracked, the lock absent, or the sid
   unknown). */
static void _demo_asks_find(demo_prompt_result_t* r, const char* sid,
                            demo_ask_slot_t* out) {
  memset(out, 0, sizeof(*out));
  if (r->asks_lock == NULL || sid == NULL) return;
  platform_mutex_lock(r->asks_lock);
  for (size_t i = 0; i < 4; i++) {
    if (r->asks[i].ask_id != NULL && r->asks[i].sid != NULL &&
        strcmp(r->asks[i].sid, sid) == 0) {
      _demo_ask_slot_copy(&r->asks[i], out);
      break;
    }
  }
  platform_mutex_unlock(r->asks_lock);
}

/* The ask-reply's console outcome: the callback sees the ACK's status only
   (sa_client_ask_reply's contract carries the delivered meanings). */
static void _demo_answer_cb(void* ctx, uint8_t status) {
  demo_prompt_result_t* r = (demo_prompt_result_t*)ctx;
  (void)r;
  if (status == 0) {
    printf("frame-demo: the answer was delivered (the reply's records ride "
           "the events stream)\n");
  } else if (status == 1) {
    fprintf(stderr, "frame-demo: the answer was NOT delivered (the daemon "
            "refused: an unknown sid, a done frame, an empty answer — the "
            "events stream keeps the truth)\n");
  } else {
    fprintf(stderr, "frame-demo: the answer's ride failed (status %u)\n",
            (unsigned)status);
  }
  fflush(stdout);
  fflush(stderr);
}

/* One console line, the main thread: 1 = keep looping; 0 = quit (the read
   loop breaks and tears the client down). */
static int _demo_client_dispatch_line(demo_prompt_result_t* r,
                                      const char* line) {
  if (line[0] == '\0') return 1;
  if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) return 0;
  demo_answer_line_t parts;
  int parsed = _demo_answer_line_parse(line, &parts);
  if (parsed <= 0) {   /* another command's word — the usage one-liner */
    printf("frame-demo: %s\n", SA_DEMO_ANSWER_USAGE_LINE);
    fflush(stdout);
    return 1;
  }
  const char* sid = (parts.sid != NULL) ? parts.sid : r->sid;
  demo_ask_slot_t ask;
  _demo_asks_find(r, sid, &ask);   /* the short form's last unparked ask (a
                                      deep copy — the registry keeps its
                                      own) */
  const char* ask_id = (parts.ask_id != NULL) ? parts.ask_id : ask.ask_id;
  if (ask_id == NULL) {
    fprintf(stderr, "frame-demo: no ask is being tracked on %s — the events "
            "stream publishes the ask records; name the ask outright\n", sid);
    _demo_answer_line_clear(&parts);
    _demo_ask_slot_clear(&ask);
    return 1;
  }
  if (parts.reject == 0) {
    if (ask.ask_id != NULL && (parts.ask_id == NULL ||
                               strcmp(parts.ask_id, ask.ask_id) == 0)) {
      _demo_answer_resolve_value(&ask, &parts.value);
    }
    if (parts.value == NULL || parts.value[0] == '\0') {
      fprintf(stderr, "frame-demo: an answer carries its value (an option's "
              "label or index, or free text — the bare token reject "
              "refuses)\n");
      _demo_answer_line_clear(&parts);
      _demo_ask_slot_clear(&ask);
      return 1;
    }
  }
  int rc = sa_client_ask_reply(r->client, sid, ask_id, parts.reject,
                               (parts.value != NULL) ? parts.value : "",
                               _demo_answer_cb, r);
  _demo_answer_line_clear(&parts);
  _demo_ask_slot_clear(&ask);
  if (rc != 0) {
    fprintf(stderr, "frame-demo: the answer was refused outright (rc %d)\n",
            rc);
  }
  return 1;
}

/* The event stream: ONE record per stdout line — the seq prefixed, the
   record's store JSON VERBATIM (the client never re-parses a record; this
   is the plain surface the daemon's events ride) — with the escalation
   slice's TWO renders: an "ask" record renders as a DIALOG (the question +
   the numbered options + the plan as an indented block) and its entry
   fills the parked-ask registry; an "ask.reply" record renders one line
   and clears the registry's entry. A marker line (seq 0, no record) names
   the transition. Payloads release immediately after the line (the stream
   is unbounded; destroy only catches the races). */
static void _demo_events_cb(void* ctx, const char* sid, uint64_t seq,
                            uint8_t op, const char* record_json) {
  demo_prompt_result_t* r = (demo_prompt_result_t*)ctx;
  if (record_json == NULL) {
    printf("[live] the replay closed (op %u) — tailing live\n", (unsigned)op);
    fflush(stdout);
  } else {
    json_value_t* rec = json_parse(record_json, strlen(record_json), NULL);
    const char* type = (rec != NULL) ? json_as_string(json_get(rec, "type"))
                                     : NULL;
    json_value_t* payload = (rec != NULL) ? json_get(rec, "payload") : NULL;
    int handled = 0;
    if (type != NULL && payload != NULL) {
      if (strcmp(type, "ask") == 0) {
        demo_ask_slot_t ask;
        memset(&ask, 0, sizeof(ask));
        if (_demo_ask_slot_fill(&ask, payload) == 0) {
          _demo_ask_render(&ask, sid, "answer <sid> <ask_id> <pick|reject> "
                           "[text]");
          _demo_asks_set(r, sid, &ask);   /* STEALS the slot's strings */
          handled = 1;
        } else {
          _demo_ask_slot_clear(&ask);
        }
      } else if (strcmp(type, "ask.reply") == 0) {
        const char* decision = json_as_string(json_get(payload, "decision"));
        const char* value = json_as_string(json_get(payload, "value"));
        printf("%llu: %s: %s\n", (unsigned long long)seq,
               (decision != NULL && strcmp(decision, "reject") == 0)
                   ? "rejected" : "answered",
               (value != NULL && value[0] != '\0') ? value : "(no value)");
        fflush(stdout);
        _demo_asks_clear_reply(r, json_as_string(json_get(payload, "askId")));
        handled = 1;
      }
    }
    if (handled == 0) {
      printf("%llu: %s\n", (unsigned long long)seq, record_json);
    }
    fflush(stdout);
    json_value_destroy(rec);
  }
  sa_client_release_payload(r->client, (void*)sid);
  if (record_json != NULL) {
    sa_client_release_payload(r->client, (void*)record_json);
  }
}

static void _demo_client_error_cb(void* ctx, uint64_t req_id, uint8_t status,
                                  const char* text) {
  (void)ctx;
  fprintf(stderr, "frame-demo client: error (req %llu, status %u): %s\n",
          (unsigned long long)req_id, (unsigned)status,
          (text != NULL) ? text : "");
  fflush(stderr);
}

static int _demo_client_run(int argc, char** argv) {
  demo_client_args_t a;
  uint16_t tcp_port = 0;
  if (_demo_parse_client(argc, argv, &a, &tcp_port) == 1) return 2;

  /* The client's SIGINT: the flag rides this mode's main-thread wait loop;
     installed HERE (not just main's direct path — a backgrounded client
     inherits SIGINT's SIG_IGN disposition from the shell, so the explicit
     catch is what re-arms it). */
  signal(SIGINT, _demo_on_sigint);
  g_demo_sigint_watched = 1;   /* the SIGINT flag is THIS mode's shutdown */

  sa_client_config_t c = sa_client_config_default();
  if (a.socket_path != NULL) {
    c.transport = SA_CLIENT_TRANSPORT_UNIX;
    c.socket_path = a.socket_path;
  } else {
    c.transport = SA_CLIENT_TRANSPORT_TCP;
    c.host = a.tcp_host;
    c.port = tcp_port;
    c.api_key = a.api_key;
  }
  sa_client_t* client = sa_client_connect(&c);
  if (client == NULL) {
    fprintf(stderr, "frame-demo client: cannot connect (the daemon is not "
            "listening there, or the tcp auth exchange failed)\n");
    return 1;
  }

  demo_prompt_result_t pr;
  memset(&pr, 0, sizeof(pr));
  pr.client = client;
  pr.asks_lock = platform_mutex_create();
  if (pr.asks_lock == NULL) {
    fprintf(stderr, "frame-demo client: the ask registry's lock is not "
            "created — the console runs degraded (the long-form answer "
            "still rides; only the short form and the label/index "
            "resolution need the registry)\n");
  }
  int prompt_rc = sa_client_prompt(client, NULL, a.goal, _demo_prompt_cb, &pr);
  if (prompt_rc != 0 || !pr.called) {
    fprintf(stderr, "frame-demo client: the prompt was refused outright "
            "(rc %d)\n", prompt_rc);
    sa_client_destroy(client);
    _demo_asks_teardown(&pr);
    return 1;
  }
  if (pr.status != 0 || pr.sid[0] == '\0') {
    fprintf(stderr, "frame-demo client: the daemon refused or the session "
            "carried no sid (status %u)\n", (unsigned)pr.status);
    sa_client_destroy(client);
    _demo_asks_teardown(&pr);
    return 1;
  }
  printf("frame-demo client: session %s started (status 0) — streaming the "
         "event records:\n", pr.sid);
  fflush(stdout);

  /* Subscribe: the WHOLE log replays (from seq 0), then the live marker,
     then tailing. BLOCKS until the marker (or the refusal — delivered
     through the error callback, rc 0). */
  if (sa_client_subscribe_events(client, pr.sid, _demo_events_cb, &pr) != 0) {
    fprintf(stderr, "frame-demo client: cannot subscribe the events (the "
            "error channel carries the reason)\n");
    sa_client_destroy(client);
    _demo_asks_teardown(&pr);
    return 1;
  }
  fprintf(stderr, "frame-demo client: live — the console: %s (Ctrl-C also "
          "tears the client down)\n", SA_DEMO_ANSWER_USAGE_LINE);

  /* The READ LOOP (the escalation slice): the events callback (the reader
     thread) streams the records AND fills the parked-ask registry; this
     thread splits its wait between the stream and the console — the stdin
     pump's poll rides the 20 ms cadence's top (the standing usleeper with
     the console window added around it). */
  demo_stdin_buf_t in;
  memset(&in, 0, sizeof(in));
  char line[1024];
  while (!g_demo_sigint) {
    int have = _demo_stdin_line(&in, 50, line, sizeof(line));
    if (have < 0) {
      fprintf(stderr, "frame-demo client: stdin closed — tearing down (the "
              "session runs on in the daemon's store)\n");
      break;
    }
    if (have == 1) {
      if (_demo_client_dispatch_line(&pr, line) == 0) break;   /* quit */
      continue;
    }
    usleep(20000);
  }
  fprintf(stderr, "frame-demo client: tearing down (records stay committed "
          "in the daemon's store)\n");
  sa_client_destroy(client);
  _demo_asks_teardown(&pr);
  return 0;
}

/* The gated modes' entry (the parse errors above print their own usages). */
static int _demo_mode_entry(const char* mode, int argc, char** argv) {
  if (strcmp(mode, "serve") == 0) return _demo_serve_run(argc, argv);
  return _demo_client_run(argc, argv);
}

#else   /* SA_DEMO_HAS_CLIENT_API */

/* A build without the client-api surface (no streams / no libcbor): the
   modes refuse loud — nothing to talk to was compiled in. */
static int _demo_mode_entry(const char* mode, int argc, char** argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr,
          "frame-demo: this build has no client-api surface (streams or "
          "wavedb off) — serve/client modes are unavailable\n");
  return 2;
}

#endif  /* SA_DEMO_HAS_CLIENT_API */

int main(int argc, char** argv) {
  /* The mode keyword (argv[1] ONLY — the direct mode never sees it, so its
     parse below stays byte-identical: --goal "serve" still rides today).
     The modes skip the direct mode's flag validation entirely. */
  if (argc > 1 && strcmp(argv[1], "serve") == 0) {
    /* argv+1: the sub-parses loop from i=1, which is the first real flag */
    return _demo_mode_entry("serve", argc - 1, argv + 1);
  }
  if (argc > 1 && strcmp(argv[1], "client") == 0) {
    return _demo_mode_entry("client", argc - 1, argv + 1);
  }

  const char* location = "sa-demo-db";
  const char* base_url = "http://127.0.0.1:11434";
  const char* model = NULL;
  const char* goal = NULL;
  unsigned escalation = FRAME_ESCALATION_FREE;   /* --escalation */
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
    } else if (strcmp(argv[i], "--escalation") == 0 && i + 1 < argc) {
      if (_demo_parse_escalation(argv[i + 1], &escalation) != 0) {
        fprintf(stderr, "frame-demo: --escalation needs free, "
                "plan-ask-act, or bypass (got '%s')\n%s", argv[i + 1],
                SA_DEMO_USAGE);
        return 2;
      }
      i++;
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
      /* The single LEADING-DIGIT check subsumes empty, a leading minus, and
         whitespace-prefixed negatives (" -5": strtoull skips the space,
         negates, and lands on seq - 1 with no error) — seq 0 is folded in
         too (the log keys allocate from seq >= 1, refine.h's counter
         restore). */
      if (!isdigit((unsigned char)text[0]) || end == text || *end != '\0' ||
          errno == ERANGE || seq == 0) {
        fprintf(stderr, "frame-demo: --refine-rollback needs a positive "
                "harness-log seq >= 1 (got '%s')\n%s", text, SA_DEMO_USAGE);
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
  /* --refine-global only widens the scope of a refine action; with no refine
     action requested it would ride silently ignored. */
  if (refine_global && !want_refine && refine_sid == NULL && !has_rollback) {
    fprintf(stderr, "frame-demo: --refine-global requires one of --refine, "
            "--refine-sid, or --refine-rollback\n%s", SA_DEMO_USAGE);
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
  cfg.escalation_mode = escalation;   /* the direct mode's console answers
                                         the ladder's parked asks */

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

  /* The turn loop is ONE blocking call on this thread; SIGINT reaches it
     through the interrupt watcher below (the handler only sets the flag and
     cannot post/allocate — see _demo_on_sigint). Armed per RUN (the parked
     ask's console wait disarms it — the flag-only handler holds there, the
     console loop polls the flag); refine/rollback above stay on the
     unwatched-path handler contract. */
  g_demo_sigint_watched = 1;
  g_demo_watch_frame = f;
  platform_thread_t* watchdog = platform_thread_create(
      _demo_interrupt_watchdog, NULL);
  if (watchdog == NULL) {
    /* No watcher can convert the flag into the interrupt: fall back to the
       unwatched-path contract (the handler's own exit) rather than promise
       an interrupt that cannot reach the frame. */
    g_demo_sigint_watched = 0;
    fprintf(stderr, "frame-demo: cannot arm the interrupt watcher — SIGINT "
            "exits instead\n");
  }

  /* THE LIVE YIELDS are NOT failures: run_loop returns 2 on the ask park
     (the console answers, the run re-pumps — the process STAYS ALIVE on
     the park) and on a live-children yield (pumped again). One stdin
     buffer feeds every console wait this run takes. */
  demo_stdin_buf_t in;
  memset(&in, 0, sizeof(in));
  demo_ask_slot_t ask;
  memset(&ask, 0, sizeof(ask));
  int loop_rc = 0;
  for (;;) {
    loop_rc = frame_run_loop(f);
    if (loop_rc != 2) break;   /* 0 clean / 1 failed loud */
    if (frame_is_done(f) == 1) break;   /* belt: a done frame runs nothing */
    if (_demo_parked_ask_scan(f, &ask) == 1) {
      /* THE ASK PARK: the demo's console takes the reply. */
      _demo_watchdog_disarm(&watchdog);   /* the console owns Ctrl-C here */
      int answered = _demo_direct_parked_wait(f, &ask, &in);
      _demo_ask_slot_clear(&ask);
      if (answered == 0) break;   /* the park stands; the demo ends here */
      watchdog = platform_thread_create(_demo_interrupt_watchdog, NULL);
      if (watchdog == NULL) {
        fprintf(stderr, "frame-demo: cannot re-arm the interrupt watcher — "
                "the resume run rides unwatched (the flag is polled at the "
                "next console wait)\n");
      }
    } else {
      printf("frame-demo: the frame yielded to live children — pumping\n");
      fflush(stdout);
    }
  }

  int outcome_rc = _demo_print_outcome(f);
  int rc = (loop_rc == 0 && outcome_rc == 0) ? 0 : 1;
  _demo_watchdog_disarm(&watchdog);   /* joined BEFORE any frame teardown */

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