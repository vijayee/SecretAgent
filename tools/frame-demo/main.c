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
// CTRL-C tears the client down.

#include "../../src/Frame/frame.h"
#include "../../src/Frame/loop.h"
#include "../../src/Frame/refine.h"
#include "../../src/Platform/platform.h"
#include "../../src/Scheduler/scheduler.h"
#include "../../src/Util/json.h"
#include "../../src/Util/log.h"

#include <ctype.h>
#include <errno.h>
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
    "the seq prefixed) live, until CTRL-C tears the client down.\n";

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

#if SA_DEMO_HAS_CLIENT_API

/* --- SERVE: the daemon (the client-api slice) ------------------------------- */

typedef struct {
  const char* socket_path;
  const char* tcp_port;   /* the TCP listener: set ONLY WITH tcp_key */
  const char* tcp_key;    /* (required together — refused loud otherwise) */
  const char* location;
  const char* base_url;
  const char* model;
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
         "                model %s at %s, store %s\n",
         a.socket_path, a.model, a.base_url, a.location);
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

/* The event stream: ONE record per stdout line — the seq prefixed, the
   record's store JSON VERBATIM (the client never re-parses a record; this
   is the plain surface the daemon's events ride). A marker line (seq 0, no
   record) names the transition. Payloads release immediately after the
   line (the stream is unbounded; destroy only catches the races). */
static void _demo_events_cb(void* ctx, const char* sid, uint64_t seq,
                            uint8_t op, const char* record_json) {
  demo_prompt_result_t* r = (demo_prompt_result_t*)ctx;
  (void)sid;
  if (record_json != NULL) {
    printf("%llu: %s\n", (unsigned long long)seq, record_json);
  } else {
    printf("[live] the replay closed (op %u) — tailing live\n", (unsigned)op);
  }
  fflush(stdout);
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
  int prompt_rc = sa_client_prompt(client, NULL, a.goal, _demo_prompt_cb, &pr);
  if (prompt_rc != 0 || !pr.called) {
    fprintf(stderr, "frame-demo client: the prompt was refused outright "
            "(rc %d)\n", prompt_rc);
    sa_client_destroy(client);
    return 1;
  }
  if (pr.status != 0 || pr.sid[0] == '\0') {
    fprintf(stderr, "frame-demo client: the daemon refused or the session "
            "carried no sid (status %u)\n", (unsigned)pr.status);
    sa_client_destroy(client);
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
    return 1;
  }
  fprintf(stderr, "frame-demo client: live — Ctrl-C tears the client down\n");

  while (!g_demo_sigint) usleep(50000);
  fprintf(stderr, "frame-demo client: tearing down (records stay committed "
          "in the daemon's store)\n");
  sa_client_destroy(client);
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
     cannot post/allocate — see _demo_on_sigint). Armed HERE, around the
     single frame_run_loop call; refine/rollback above stay on the
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
  int loop_rc = frame_run_loop(f);

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