// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// headless_main.c
// Command-line entry point for running the emulator without a GUI.

#include "platform.h"

#include "headless.h"

#include "api.h"
#include "appletalk.h"
#include "checkpoint_machine.h"
#include "core_init.h"
#include "cpu.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "laserwriter_job.h"
#include "log.h"
#include "machine.h"
#include "machine_config.h"
#include "memory.h"
#include "nubus.h"
#include "printer_sink.h"
#include "prom.h"
#include "rom.h"
#include "scheduler.h"
#include "script.h"
#include "scsi.h"
#include "shell.h"
#include "shell_var.h"
#include "system.h"
#include "vrom.h"
#include "event/gs_event.h"
#include "io/io_worker.h"
#include "job/job.h"
#include "mailbox/mailbox.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#endif

// Nothing to redraw: headless has no screen of its own (platform.h)
void platform_force_redraw(void) {}

// The host directory published as the default AppleShare volume, resolved at
// startup from --shared-dir or $GS_SHARED_DIR.  The path literal lives here in
// the platform layer, never in src/core (PR #69): core only ever executes the
// tree operation it is handed.  Empty means "no default volume".
static char g_shared_dir[PATH_MAX];

// Where the LaserWriter's documents land, from --print-dir or $GS_PRINT_DIR.
// Empty means "no directory": a finished job is logged and dropped.
static char g_print_dir[PATH_MAX];

// Platform sink for a finished LaserWriter job (weak default in
// laserwriter_job.c drops it): <print-dir>/<job>-<title>.pdf, the title
// reduced to filename-safe characters.  An error outcome keeps its
// document (the pages shown before the error are in it) and is reported
// on the console.
void laserwriter_sink_document(const laserwriter_document_t *doc) {
    const char *outcome = doc->ok ? "ok" : doc->budget_exceeded ? "execution budget spent" : doc->error_name;
    if (!g_print_dir[0]) {
        printf("laserwriter: job %u '%s' (%u pages, %s) discarded: no --print-dir\n", (unsigned)doc->job_id, doc->title,
               (unsigned)doc->pages, outcome);
        return;
    }
    if (mkdir(g_print_dir, 0755) != 0 && errno != EEXIST) {
        printf("laserwriter: cannot create print directory %s: %s\n", g_print_dir, strerror(errno));
        return;
    }
    // Filename-safe title: one '_' per run of anything outside [A-Za-z0-9._-]
    char safe[LASERWRITER_TITLE_MAX + 1];
    size_t n = 0;
    bool pending_sep = false;
    for (const char *p = doc->title; *p && n < LASERWRITER_TITLE_MAX; p++) {
        unsigned char c = (unsigned char)*p;
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-';
        if (keep) {
            if (pending_sep && n > 0)
                safe[n++] = '_';
            pending_sep = false;
            if (n < LASERWRITER_TITLE_MAX)
                safe[n++] = (char)c;
        } else {
            pending_sep = true;
        }
    }
    safe[n] = '\0';
    // Room for the directory plus the longest name this can form
    char path[PATH_MAX + LASERWRITER_TITLE_MAX + 32];
    snprintf(path, sizeof(path), "%s/%05u-%s.pdf", g_print_dir, (unsigned)doc->job_id, n ? safe : "untitled");
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("laserwriter: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    size_t wrote = fwrite(doc->pdf, 1, doc->pdf_len, f);
    fclose(f);
    if (wrote != doc->pdf_len)
        printf("laserwriter: short write to %s (%zu of %zu bytes)\n", path, wrote, doc->pdf_len);
    if (doc->ok)
        printf("laserwriter: job %u '%s': %u pages -> %s\n", (unsigned)doc->job_id, doc->title, (unsigned)doc->pages,
               path);
    else
        printf("laserwriter: job %u '%s': %u pages -> %s (error: %s in %s)\n", (unsigned)doc->job_id, doc->title,
               (unsigned)doc->pages, path, outcome, doc->offending);
}

// Platform sink for a job's captured PostScript (appletalk.printer.capture):
// <print-dir>/<job>.ps, the job number matching its PDF's.
void laserwriter_sink_capture(const laserwriter_capture_t *cap) {
    if (!g_print_dir[0]) {
        printf("laserwriter: job %u PostScript (%zu bytes) discarded: no --print-dir\n", (unsigned)cap->job_id,
               cap->ps_len);
        return;
    }
    if (mkdir(g_print_dir, 0755) != 0 && errno != EEXIST) {
        printf("laserwriter: cannot create print directory %s: %s\n", g_print_dir, strerror(errno));
        return;
    }
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/%05u.ps", g_print_dir, (unsigned)cap->job_id);
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("laserwriter: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    size_t wrote = fwrite(cap->ps, 1, cap->ps_len, f);
    fclose(f);
    if (wrote != cap->ps_len)
        printf("laserwriter: short write to %s (%zu of %zu bytes)\n", path, wrote, cap->ps_len);
    printf("laserwriter: job %u PostScript%s -> %s\n", (unsigned)cap->job_id, cap->complete ? "" : " (cut off)", path);
}

// Platform sink for a document from a printer the core rasterises itself
// (the ImageWriter, printer_sink.h): <print-dir>/<printer>-<job>-<title>.pdf,
// with the same safe-name reduction as the LaserWriter's.
void printer_sink_document(const printer_document_t *doc) {
    if (!g_print_dir[0]) {
        printf("%s: job %u '%s' (%u pages) discarded: no --print-dir\n", doc->slug, (unsigned)doc->job_id, doc->title,
               (unsigned)doc->pages);
        return;
    }
    if (mkdir(g_print_dir, 0755) != 0 && errno != EEXIST) {
        printf("%s: cannot create print directory %s: %s\n", doc->slug, g_print_dir, strerror(errno));
        return;
    }
    // Filename-safe title: one '_' per run of anything outside [A-Za-z0-9.-]
    char safe[PRINTER_TITLE_MAX + 1];
    size_t n = 0;
    bool pending_sep = false;
    for (const char *p = doc->title; *p && n < PRINTER_TITLE_MAX; p++) {
        unsigned char c = (unsigned char)*p;
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-';
        if (keep) {
            if (pending_sep && n > 0)
                safe[n++] = '_';
            pending_sep = false;
            if (n < PRINTER_TITLE_MAX)
                safe[n++] = (char)c;
        } else {
            pending_sep = true;
        }
    }
    safe[n] = '\0';
    char path[PATH_MAX + PRINTER_TITLE_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s-%05u-%s.pdf", g_print_dir, doc->slug, (unsigned)doc->job_id,
             n ? safe : "untitled");
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("%s: cannot write %s: %s\n", doc->slug, path, strerror(errno));
        return;
    }
    size_t wrote = fwrite(doc->pdf, 1, doc->pdf_len, f);
    fclose(f);
    if (wrote != doc->pdf_len)
        printf("%s: short write to %s (%zu of %zu bytes)\n", doc->slug, path, wrote, doc->pdf_len);
    if (doc->ok)
        printf("%s: job %u '%s': %u pages -> %s\n", doc->slug, (unsigned)doc->job_id, doc->title, (unsigned)doc->pages,
               path);
    else
        printf("%s: job %u '%s': %u pages -> %s (%s)\n", doc->slug, (unsigned)doc->job_id, doc->title,
               (unsigned)doc->pages, path, doc->detail);
}

// Platform sink for a printer job's raw input (the printer's `capture`):
// <print-dir>/<printer>-<job>.<ext>, beside its PDF.
void printer_sink_capture(const printer_capture_t *cap) {
    if (!g_print_dir[0]) {
        printf("%s: job %u input (%zu bytes) discarded: no --print-dir\n", cap->slug, (unsigned)cap->job_id, cap->len);
        return;
    }
    if (mkdir(g_print_dir, 0755) != 0 && errno != EEXIST) {
        printf("%s: cannot create print directory %s: %s\n", cap->slug, g_print_dir, strerror(errno));
        return;
    }
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s-%05u.%s", g_print_dir, cap->slug, (unsigned)cap->job_id, cap->ext);
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("%s: cannot write %s: %s\n", cap->slug, path, strerror(errno));
        return;
    }
    size_t wrote = fwrite(cap->data, 1, cap->len, f);
    fclose(f);
    if (wrote != cap->len)
        printf("%s: short write to %s (%zu of %zu bytes)\n", cap->slug, path, wrote, cap->len);
    printf("%s: job %u input -> %s\n", cap->slug, (unsigned)cap->job_id, path);
}

// VBL is
// no longer a scheduler event armed per machine: the run loop injects it
// imperatively, one VBL pulse per frame-unit, via scheduler_run_frame() — the
// same path web2's scheduler_main_loop() takes.  See hl_run_statement
// / the main loop below, and docs/internals/core/scheduler/scheduler.md §10.

// The loop's flags (headless.h).  The handlers only set a flag: nothing a
// signal handler may call (signal-safety(7)) stops a scheduler, so the loops
// act on them -- SIGTERM ends every loop and stops the machine, SIGINT is
// Ctrl-C (hl_interrupt).
volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_interrupted = 0;
volatile sig_atomic_t g_quit_requested = 0;

// SIGINT: Ctrl-C
static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1; // the loop acts: cancel stdin's job, stop its run
}

// SIGTERM: shut down
static void sigterm_handler(int sig) {
    (void)sig;
    g_running = 0; // every loop checks it; hl_run_statement stops the machine
}

// Install `handler` for `sig`.  SA_RESTART keeps the semantics signal() had
// here (glibc's BSD signal): a blocking getline on stdin survives a Ctrl-C;
// poll()-based waits return EINTR and re-check the flags.
static void install_signal(int sig, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
}

// The whole of a small text file, NUL-terminated, for the caller to free; NULL
// when it cannot be read.
static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, 4096, f);
        len += n;
        if (n < 4096)
            break;
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

// Print usage information
static void print_usage(const char *program) {
    printf("Usage: %s rom=<file> [hd=<file>] [cdrom=<file>] [fd=<file>] [script=<file>]\n", program);
    printf("\n");
    printf("Arguments:\n");
    printf("  rom=<file>      ROM image file (required)\n");
    printf("  ram=<kb>        RAM size in kilobytes (default: machine-specific)\n");
    printf("  model=<id>      Machine model (e.g. pm8500) when the ROM serves several;\n");
    printf("                  default: the first model the ROM identifies as\n");
    printf("  hd=<file>       Hard disk image (optional, repeatable): each goes into the model's\n");
    printf("                  next hard-disk bay, the boot bay first (the ProFile on a Lisa)\n");
    printf("  cdrom=<file>    CD-ROM image (optional, once): into the model's CD bay --\n");
    printf("                  SCSI ID 3 on a Macintosh, 0 on a Network Server\n");
    printf("  fd=<file>       Floppy disk image file (optional, can specify multiple)\n");
    printf("  fd0=<file>      Floppy disk image for drive 0 (internal)\n");
    printf("  fd1=<file>      Floppy disk image for drive 1 (the second position; it gets a drive)\n");
    printf("  config=<doc>    configuration document: a JSON file, or JSON inline\n");
    printf("                  (see catalog.default_config); the arguments here are shorthand\n");
    printf("  drive=<spec>    add a drive, bus:unit:type[:image] (type hd or cd; repeatable),\n");
    printf("                  e.g. drive=scsi:4:hd:data.img drive=scsi2:2:cd\n");
    printf("  video_card=<id> NuBus video card for the configurable slot (e.g. 824gc);\n");
    printf("                  default: the machine's default card\n");
    printf("  slots=<spec>    expansion-slot cards, 'SLOT=CARD[,key=value]*;...'\n");
    printf("                  (e.g. slots='$A=824gc,mode=gc_640x480_8bpp;$B=mdc_8_24,rom=substitute')\n");
    printf("  monitor=<id>    monitor on the built-in video port ('none' = unconnected,\n");
    printf("                  which hands the screen to a NuBus card)\n");
    printf("  script=<file>   Shell script file to execute at startup (optional)\n");
    printf("\n");
    printf("  Startup order: script= first, then --script-stdin, then the daemon or the\n");
    printf("  interactive prompt.  A `quit` in a script, or a script= that fails, ends the\n");
    printf("  run there (a daemon still prints READY, then exits).\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help, -h      Display this help message\n");
    printf("  --speed=MODE    Pacing mode: paced, accelerated, turbo (default: paced). Headless\n");
    printf("                  runs are budget-driven; only 'accelerated' changes execution\n");
    printf("                  (more instructions per frame-unit, scheduler.speed multiplier).\n");
    printf("  --cycles=N      Run for N CPU cycles then exit (for testing)\n");
    printf("  --quiet, -q     Suppress startup messages\n");
    printf("  --daemon        Start in daemon mode (TCP socket interface for AI agents).\n");
    printf("                  Listens on 127.0.0.1 with NO authentication: any local process\n");
    printf("                  can run any statement, host file access included, as this user\n");
    printf("  --port=PORT     TCP port for daemon mode (default: 6800)\n");
    printf("  --kill          Kill existing daemon on same port before starting (SIGTERM,\n");
    printf("                  SIGKILL after 5 s); PID file /tmp/gs-headless-<port>.pid\n");
    printf("  --script-stdin  Read script commands from stdin instead of a file\n");
    printf("  --var NAME=VAL  Set a shell variable (can be repeated)\n");
    printf("  --no-prompt     Disable the prompt status line for all connections\n");
    printf("  --checkpoint-dir=DIR  Directory to host writable image deltas (default: alongside base image)\n");
    printf("  --shared-dir=DIR     Publish DIR as the default AppleShare volume \"Shared\"\n");
    printf("                       (also settable with $GS_SHARED_DIR; created if missing)\n");
    printf("  --print-dir=DIR      Write each LaserWriter job's PDF as DIR/<job>-<title>.pdf\n");
    printf("                       (also $GS_PRINT_DIR; created if missing; needs a PLATEN=1 build)\n");
    printf("  --framed        Core events as `@event <kind> <json>` lines, a job's output as `@out <json>`,\n");
    printf("                  an I/O job's progress as `@progress <json>`, and `@end ok|error` after each\n");
    printf("                  statement (for a client that parses)\n");
    printf("  --io=sync       No I/O worker: file work runs on the emulator thread (a bisecting aid)\n");
    printf("  --jobs=inline   No job thread: scripts run on the emulator thread (a bisecting aid)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s rom=plus.rom\n", program);
    printf("  %s rom=plus.rom hd=disk.img\n", program);
    printf("  %s rom=plus.rom fd=system.dsk script=boot.sh\n", program);
    printf("  %s --daemon rom=iix-iicx-se30-97221136.rom\n", program);
    printf("  %s --daemon --port=7000 rom=iix-iicx-se30-97221136.rom hd=disk.img\n", program);
}

// Assertion failures seen this run.  A failed GS_ASSERT prints its
// diagnostics and pauses the machine (debug.c diagnose_and_halt) so it can be
// examined on the spot; it does not stop a script, which would otherwise run
// on and pass.  So the run's exit status carries it.
static unsigned g_assert_failures;

static void headless_failure_hook(const char *kind, const char *expr, const char *file, int line, const char *func) {
    (void)expr, (void)func;
    if (strcmp(kind, "assertion") != 0)
        return; // GS_UNIMPLEMENTED: a gap in the model, reported, not a failed invariant
    g_assert_failures++;
    fprintf(stderr, "headless: assertion %u failed at %s:%d -- this run will exit non-zero\n", g_assert_failures,
            file ? file : "<unknown>", line);
}

// The exit status: the scripts' own (`script_rc`, from script= and
// --script-stdin), or 3 if any assertion failed.
static int headless_exit_code(int script_rc) {
    if (g_assert_failures) {
        fprintf(stderr, "headless: %u assertion failure(s) this run\n", g_assert_failures);
        if (script_rc == 0)
            return 3;
    }
    return script_rc;
}

// Platform impl of gs_quit (the weak default in system.c says "not
// supported": the browser owns the page).
int gs_quit(void) {
    g_quit_requested = 1;
    // Stop scheduler to break out of any running emulation
    scheduler_t *sched = system_scheduler();
    if (sched) {
        scheduler_stop(sched);
    }
    return 0;
}

// ============================================================================
// The loop: frame, drain, and scripts as jobs
// ============================================================================
//
// Headless is a client of its own mailbox (mailbox.h, "an in-process
// client").  Every statement -- from the script file, stdin, the REPL or a
// daemon connection -- is posted as a REQ_SCRIPT and runs as a job on the
// job thread (job/job.h), exactly as a terminal line does in the browser;
// this thread meanwhile does what the browser's tick does: one frame-unit
// when the machine runs, then a drain, which serves the job's calls into
// the object tree and delivers its result.  `scheduler.run N` inside a
// script waits for N because the job waits for its mode; `scheduler.run`
// returns at once and the machine runs on between statements.

#define HL_MBX_RING (64u << 10)

static gs_mailbox_t g_mbx;
static uint8_t g_mbx_region[GS_MBX_ALIGN + GS_MBX_CTRL_WORDS * 4u + 2 * HL_MBX_RING];
static gs_mailbox_client_t g_cli;
static int g_framed = 0; // --framed: @event / @end lines on stdout
static int g_io_sync = 0; // --io=sync: no I/O worker
static int g_jobs_inline = 0; // --jobs=inline: no job thread; scripts run on the emulator thread
static uint32_t g_foreground_job = 0; // the statement in flight (its request id)
static uint32_t g_foreground_client = 0;

// Core events (gs_event.h): with --framed each one is a line a client can
// parse; otherwise they are silent here (the REPL prints its own state).
void gs_event_emit(gs_event_kind_t kind, const char *json) {
    if (!g_framed)
        return;
    printf("@event %u %s\n", (unsigned)kind, json);
    fflush(stdout);
}

uint32_t gs_current_client(void) {
    return gs_mailbox_current_client(&g_mbx);
}

static void hl_mailbox_init(void) {
    if (!gs_mailbox_init(&g_mbx, g_mbx_region, HL_MBX_RING, HL_MBX_RING, gs_eval)) {
        fprintf(stderr, "headless: mailbox init failed\n");
        exit(1);
    }
    gs_mailbox_client_init(&g_cli, &g_mbx);
    gs_mailbox_set_ready(&g_mbx);
}

// The daemon's pacing (--speed=, scheduler.mode / speed / max_speed): host
// state, so it outlives every machine and is never in a checkpoint.  Headless
// execution is budget-driven and never consults the *pacing*; 'accelerated'
// does change execution -- frame-units retire more instructions at the
// lowered effective CPI, at the pinned scheduler.speed.
static host_pacing_t s_pacing = HOST_PACING_DEFAULT;

host_pacing_t *platform_pacing(void) {
    return &s_pacing;
}

// A new machine is the active one: what the session counts of the previous
// machine is not compared with it (the --max-cycles count, main()).
static bool s_count_rebase;

void platform_machine_attached(void) {
    s_count_rebase = true;
}

// One turn of the loop: a frame-unit if the machine runs, then the drain.
// Returns whether anything happened (a frame ran or a request was served).
// One frame, when the machine runs.  Also what inline mode (job.h) calls
// while a script waits for the mode it started.
static bool hl_run_frame(void) {
    scheduler_t *sched = system_scheduler();
    if (sched && global_emulator && scheduler_is_running(sched)) {
        scheduler_run_frame(sched, global_emulator, &s_pacing);
        return true;
    }
    return false;
}

static void hl_inline_frame(void) {
    if (!hl_run_frame())
        usleep(1000);
}

bool hl_pump_once(void) {
    bool did = hl_run_frame();
    if (gs_mailbox_drain(&g_mbx, 0, NULL) > 0)
        did = true;
    if (job_layer_has_work())
        did = true;
    return did;
}

// A job's printed output arrives as EVT_LOG output records (mailbox.h);
// this prints the text -- or, framed, hands the record to the client as an
// `@out` line.
// A job's annotation records (value_begin / value / error, mailbox.h): to a
// framed client as `@value_begin <json>`, `@value <json>`, `@error <json>`
// lines among its `@out` lines.  Unframed output prints an error's lines to
// stderr (hl_print_error_lines) and ignores the rest.
static void hl_print_annotation(const char *json) {
    static const char *const kinds[] = {"value_begin", "value", "error"};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        char key[40];
        snprintf(key, sizeof key, "{\"event\":\"%s\",", kinds[i]);
        if (strncmp(json, key, strlen(key)) == 0) {
            printf("@%s %s\n", kinds[i], json);
            fflush(stdout);
            return;
        }
    }
}

// Writes the JSON string whose body starts at `t` (just past its opening
// quote) to `f`, unescaped.  Returns the position after the closing quote.
static const char *hl_put_json_string(FILE *f, const char *t) {
    while (*t && *t != '"') {
        if (*t == '\\' && t[1]) {
            t++;
            switch (*t) {
            case 'n':
                fputc('\n', f);
                break;
            case 't':
                fputc('\t', f);
                break;
            case 'u': {
                unsigned c = 0;
                if (sscanf(t + 1, "%4x", &c) == 1) {
                    fputc((int)c, f);
                    t += 4;
                }
                break;
            }
            default:
                fputc(*t, f);
            }
            t++;
        } else {
            fputc(*t++, f);
        }
    }
    return *t == '"' ? t + 1 : t;
}

static void hl_print_output_record(const char *json) {
    if (g_framed) {
        printf("@out %s\n", json);
        fflush(stdout);
        return;
    }
    const char *t = strstr(json, "\"text\":\"");
    if (!t)
        return;
    hl_put_json_string(stdout, t + 8);
    fflush(stdout);
}

// Unframed: an `error` record's lines, to stderr -- the core writes a job's
// error only as this record.  A `truncated` record's full text is already on
// stderr.
static void hl_print_error_lines(const char *json) {
    if (strstr(json, "\"truncated\":true"))
        return;
    const char *t = strstr(json, "\"lines\":[");
    if (!t)
        return;
    t += 9;
    fflush(stdout); // keep the job's text before its error
    while (*t == '"') {
        t = hl_put_json_string(stderr, t + 1);
        fputc('\n', stderr);
        if (*t == ',')
            t++;
    }
    fflush(stderr);
}

// Takes every event off the ring: a job's output is printed, the result
// of `id` (when non-zero) is reported through *rc.  Returns whether that
// result arrived.
static bool hl_take_events(uint32_t id, int *rc) {
    static uint8_t buf[128u << 10];
    uint32_t len, kind;
    bool got = false;
    while ((kind = gs_mailbox_client_take(&g_cli, buf, sizeof buf, &len)) != 0) {
        if (kind == GS_MBX_EVT_RESULT) {
            if (id && RD_LE32(buf + 4 * GS_MBX_RESULT_ID) == id) {
                *rc = RD_LE32(buf + 4 * GS_MBX_RESULT_OK) ? 0 : -1;
                got = true;
            }
        } else if (kind == GS_MBX_EVT_PROGRESS && g_framed && len < sizeof buf) {
            uint32_t n = RD_LE32(buf + 4 * GS_MBX_EVENT_JSON_LEN);
            char *json = (char *)buf + 4 * GS_MBX_EVENT_WORDS;
            if (4 * GS_MBX_EVENT_WORDS + n < sizeof buf) {
                json[n] = '\0';
                printf("@progress %s\n", json);
                fflush(stdout);
            }
        } else if (kind == GS_MBX_EVT_LOG && len < sizeof buf) {
            uint32_t n = RD_LE32(buf + 4 * GS_MBX_EVENT_JSON_LEN);
            char *json = (char *)buf + 4 * GS_MBX_EVENT_WORDS;
            if (4 * GS_MBX_EVENT_WORDS + n < sizeof buf) {
                json[n] = '\0';
                if (strstr(json, "\"event\":\"output\"") == json + 1)
                    hl_print_output_record(json);
                else if (g_framed)
                    hl_print_annotation(json);
                else if (strstr(json, "\"event\":\"error\"") == json + 1)
                    hl_print_error_lines(json);
            }
        }
    }
    return got;
}

// Ctrl-C, exactly (the same rule as the browser terminal): cancel the
// statement in flight if there is one, else stop a run stdin started, else
// stop whatever runs -- headless has no toolbar, so an unowned run (the
// boot) is the user's to stop too.
static void hl_interrupt(uint32_t client) {
    scheduler_t *s = system_scheduler();
    if (g_foreground_job && g_foreground_client == client) {
        job_cancel(client, g_foreground_job);
        return;
    }
    if (s && !scheduler_stop_owned(s, client))
        scheduler_stop_owned(s, 0);
}

// SIGTERM arrived (g_running cleared): stop the machine, once, so a run in
// flight ends and every loop can unwind.  The handler only sets the flag.
static void hl_honour_sigterm(void) {
    static bool done;
    if (g_running || done)
        return;
    done = true;
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_stop(sched);
}

// Runs one statement as a job and waits for its result (headless.h).
int hl_run_statement(uint32_t client, const char *src) {
    uint32_t id = gs_mailbox_client_script(&g_cli, client, src, strlen(src));
    if (!id) {
        printf("error: statement too long for the mailbox\n");
        return -1;
    }
    g_foreground_job = id;
    g_foreground_client = client;
    double last_heartbeat = host_time();
    uint64_t start_instr = cpu_instr_count();
    bool cancelled_for_quit = false; // `quit` or SIGTERM: the job was cancelled
    int rc = -1;
    for (;;) {
        bool did = hl_pump_once();
        // Results: ours ends the wait; another client's (none today) is
        // dropped.  The job's output is printed as it arrives.
        if (hl_take_events(id, &rc))
            break;
        if (g_interrupted) {
            g_interrupted = 0;
            hl_interrupt(HL_CLIENT_STDIN);
            printf("\n[Interrupted]\n");
        }
        hl_honour_sigterm();
        if ((g_quit_requested || !g_running) && !cancelled_for_quit) {
            // `quit` inside a script, or SIGTERM: the machine is stopped;
            // the script must not start the next run.
            cancelled_for_quit = true;
            job_cancel(client, id);
        }
        scheduler_t *sched = system_scheduler();
        bool running = sched && scheduler_is_running(sched);
        if (running) {
            double now = host_time();
            if (now - last_heartbeat >= 1.0) {
                uint64_t current = cpu_instr_count();
                printf("# running... %llu instructions (+%llu since start)\n", (unsigned long long)current,
                       (unsigned long long)(current - start_instr));
                fflush(stdout);
                last_heartbeat = now;
            }
        }
        daemon_watch_statement(client, running); // a no-op without a daemon client
        if (!did && !running)
            usleep(1000);
    }
    g_foreground_job = 0;
    if (g_framed) {
        printf("@end %s\n", rc == 0 ? "ok" : "error");
        fflush(stdout);
    }
    return rc;
}

// === Statements from a stream (headless.h) =================================
//
// The daemon (headless_daemon.c), stdin and the REPL all feed lines to this
// one assembler and run each statement the moment it is complete.

// Blank lines outside a statement are skipped.
int stmt_feed_line(stmt_asm_t *a, const char *line, size_t len) {
    while (len > 0 && line[len - 1] == '\r')
        len--;
    if (len == 0 && a->len == 0)
        return 0;
    if (a->len + len + 2 > STMT_MAX) {
        a->len = 0;
        return -1;
    }
    if (a->len + len + 2 > a->cap) {
        size_t cap = a->cap ? a->cap : 4096;
        while (cap < a->len + len + 2)
            cap *= 2;
        char *grown = realloc(a->buf, cap);
        if (!grown) {
            a->len = 0;
            return -1;
        }
        a->buf = grown;
        a->cap = cap;
    }
    memcpy(a->buf + a->len, line, len);
    a->len += len;
    a->buf[a->len++] = '\n';
    a->buf[a->len] = '\0';
    return script_needs_continuation(a->buf) ? 0 : 1;
}

// Empty the assembler for the next statement (keeps its buffer)
void stmt_reset(stmt_asm_t *a) {
    a->len = 0;
    if (a->buf)
        a->buf[0] = '\0';
}

// Release the assembler's buffer
void stmt_free(stmt_asm_t *a) {
    free(a->buf);
    *a = (stmt_asm_t){0};
}

// Parse key=value argument, returns value or NULL if key doesn't match
static const char *parse_arg(const char *arg, const char *key) {
    size_t key_len = strlen(key);
    if (strncmp(arg, key, key_len) == 0 && arg[key_len] == '=') {
        return arg + key_len + 1;
    }
    return NULL;
}

// Run a script file: one `include` statement as a job, so `include` paths
// inside it resolve relative to the file and diagnostics carry its name.
// Returns the script's exit status: 0, or 1 when it failed.
static int run_script_file(const char *filename) {
    printf("> running %s\n", filename);
    char stmt[4096];
    size_t o = 0;
    o += (size_t)snprintf(stmt + o, sizeof stmt - o, "include \"");
    for (const char *c = filename; *c && o + 3 < sizeof stmt; c++) {
        if (*c == '"' || *c == '\\')
            stmt[o++] = '\\';
        stmt[o++] = *c;
    }
    snprintf(stmt + o, sizeof stmt - o, "\"");
    int result = hl_run_statement(HL_CLIENT_STDIN, stmt);
    if (result != 0) {
        // v2 scripts abort on the first error; a script that
        // aborted never reaches its `quit`, so exit here instead of
        // dropping into the interactive loop.
        g_quit_requested = 1;
        return 1;
    }
    return 0;
}

// Run statements from stdin, each as soon as it is complete: a
// line at a time through the statement assembler, so a multi-line block
// runs when it closes.  getline, not a 1024-byte fgets: a longer line used to
// be split into two statements.  Returns the exit status: 1 if any statement
// failed, else 0.
static int run_script_stdin(void) {
    int exit_code = 0;
    stmt_asm_t stmt = {0};
    char *line = NULL;
    size_t line_cap = 0;
    char *last_cmd = NULL;
    ssize_t n;
    while (g_running && !g_quit_requested && (n = getline(&line, &line_cap, stdin)) >= 0) {
        size_t len = (size_t)n;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        // Skip comments
        if (line[0] == '#')
            continue;

        // An empty line repeats the last command -- only outside a
        // continuation.
        if (len == 0 && stmt.len == 0) {
            if (!last_cmd)
                continue;
            free(line);
            line = strdup(last_cmd);
            line_cap = line ? strlen(line) + 1 : 0;
            if (!line)
                break;
            len = strlen(line);
        } else if (stmt.len == 0) {
            free(last_cmd);
            last_cmd = strdup(line);
        }

        int r = stmt_feed_line(&stmt, line, len);
        if (r < 0) {
            printf("error: statement too long (over %u bytes); discarded\n", STMT_MAX);
            exit_code = 1;
            continue;
        }
        if (r == 0)
            continue;

        printf("> %s\n", stmt.buf);
        int result = hl_run_statement(HL_CLIENT_STDIN, stmt.buf); // interactive: results print
        stmt_reset(&stmt);
        if (result != 0)
            exit_code = 1;
    }
    if (stmt.len) {
        printf("error: incomplete block at end of input; not run\n");
        exit_code = 1;
    }
    stmt_free(&stmt);
    free(line);
    free(last_cmd);
    return exit_code;
}

// Shell prompt -- shared builder in shell.c (same text as the web shell)
static void print_prompt(void) {
    char buf[256];
    shell_build_prompt(buf, sizeof(buf));
    printf("%s", buf);
    fflush(stdout);
}

// The REPL's stdin, read with read() into our own buffer.  A FILE* would
// read ahead: paste three lines and getline returns the first while the
// other two sit in stdio's buffer, where poll() on the descriptor cannot see
// them -- they ran only when more input arrived.  Here the buffer is ours,
// so a complete line in it is taken before the descriptor is waited on.
static char *g_stdin_buf;
static size_t g_stdin_len, g_stdin_cap;
static bool g_stdin_eof;

// Take the next line out of g_stdin_buf into `line` (NUL-terminated, without
// its newline; the caller frees it), waiting up to `timeout_ms` for input when
// none is buffered.  At end of input the unterminated remainder is the last
// line.  Returns false when there is no line yet (or no more input).
static bool stdin_take_line(int timeout_ms, char **line, size_t *len) {
    char *nl = g_stdin_len ? memchr(g_stdin_buf, '\n', g_stdin_len) : NULL;
    if (!nl && !g_stdin_eof) {
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, timeout_ms) > 0) {
            if (g_stdin_cap - g_stdin_len < 4096) {
                size_t cap = g_stdin_cap ? g_stdin_cap * 2 : 8192;
                char *grown = realloc(g_stdin_buf, cap);
                if (!grown)
                    return false;
                g_stdin_buf = grown;
                g_stdin_cap = cap;
            }
            ssize_t n = read(STDIN_FILENO, g_stdin_buf + g_stdin_len, g_stdin_cap - g_stdin_len);
            if (n > 0)
                g_stdin_len += (size_t)n;
            else if (n == 0 || (errno != EINTR && errno != EAGAIN))
                g_stdin_eof = true; // end of input (or a dead descriptor): stop polling it
            nl = g_stdin_len ? memchr(g_stdin_buf, '\n', g_stdin_len) : NULL;
        }
    } else if (!nl && timeout_ms > 0) {
        usleep((useconds_t)timeout_ms * 1000); // no input will come: the caller's idle wait
    }
    size_t line_len, used;
    if (nl) {
        line_len = (size_t)(nl - g_stdin_buf);
        used = line_len + 1;
    } else if (g_stdin_eof && g_stdin_len) {
        line_len = used = g_stdin_len;
    } else {
        return false;
    }
    *line = malloc(line_len + 1);
    if (!*line)
        return false;
    memcpy(*line, g_stdin_buf, line_len);
    (*line)[line_len] = '\0';
    *len = line_len;
    memmove(g_stdin_buf, g_stdin_buf + used, g_stdin_len - used);
    g_stdin_len -= used;
    return true;
}

// Poll for shell input (called from the main loop, which passes how long it
// may wait): a line at a time through the statement assembler, with the
// continuation prompt "... " while a block is open.  Returns whether a line
// was taken.
static stmt_asm_t g_repl;

static bool repl_poll(int timeout_ms) {
    char *line;
    size_t len;
    if (!stdin_take_line(timeout_ms, &line, &len))
        return false;
    while (len > 0 && line[len - 1] == '\r')
        line[--len] = '\0';

    int r = stmt_feed_line(&g_repl, line, len);
    free(line);
    if (r < 0) {
        printf("error: statement too long (over %u bytes); discarded\n", STMT_MAX);
        return true;
    }
    if (r == 0) {
        if (g_repl.len) {
            printf("... ");
            fflush(stdout);
        }
        return true;
    }

    hl_run_statement(HL_CLIENT_STDIN, g_repl.buf);
    stmt_reset(&g_repl);
    return true;
}

// Offer the ROM file's sibling *.vrom and *.prom files to the core's
// content-addressed registries.  The platform owns the filesystem: it names
// the directory; core walks it (offer_registry_add_dir), identifies each file
// by content and never fabricates a path itself.  This is what makes sibling
// card ROMs (e.g. the integration harness's tests/data/roms, reached via the
// absolute rom= path) discoverable without core knowing any directory.
static void offer_sibling_card_roms(const char *rom_path) {
    if (!rom_path || !*rom_path)
        return;
    const char *slash = strrchr(rom_path, '/');
    char dir[1024];
    if (slash) {
        size_t dir_len = (size_t)(slash - rom_path);
        if (dir_len == 0)
            dir_len = 1; // ROM at filesystem root → "/"
        if (dir_len >= sizeof(dir))
            return;
        memcpy(dir, rom_path, dir_len);
        dir[dir_len] = '\0';
    } else {
        strcpy(dir, "."); // bare filename → the current directory
    }
    vrom_offer_dir(dir, ".vrom");
    prom_offer_dir(dir, ".prom");
}

// === The platform contract (src/platform/platform.h) =======================

// A script's `machine.boot rom=<elsewhere>` gets the same walk the CLI's
// rom= got at startup; without it a ROM booted from another directory found
// none of the card ROMs beside it (#187).
void platform_offer_sibling_card_roms(const char *rom_path) {
    offer_sibling_card_roms(rom_path);
}

// The host callstack, for the failure handler: glibc's backtrace where there
// is one (this lived in em_main.c's never-compiled native branch).
void platform_print_host_callstack(void) {
    printf("\n=== Host callstack ===\n");
#if defined(__linux__) || defined(__APPLE__)
    void *frames[64];
    int n = backtrace(frames, 64);
    char **syms = backtrace_symbols(frames, n);
    if (syms) {
        for (int i = 0; i < n; i++)
            printf("%s\n", syms[i]);
        free(syms);
        return;
    }
#endif
    printf("(unavailable)\n");
}

// No host audio sink headless: deterministic capture for golden-WAV tests
// lives core-side in audio_out.c, ahead of this boundary.
void platform_audio_open(uint32_t src_rate_hz, int channels) {
    (void)src_rate_hz;
    (void)channels;
}

void platform_audio_push(const int16_t *frames, int nframes, int vol_0_7) {
    (void)frames;
    (void)nframes;
    (void)vol_0_7;
}

void platform_audio_set_rate(uint32_t src_rate_hz) {
    (void)src_rate_hz;
}

// No host audio ring: the governor's audio signal is simply absent (and the
// governor itself never runs on the budget-driven headless path).
double platform_audio_ring_fill(void) {
    return -1.0;
}

// Limits on the repeatable arguments.  Going past one is an error, not a
// warning: a run must not go on without media it was asked for (and in
// daemon mode a warning would reach no client).
#define HL_MAX_HD     8
#define HL_MAX_CDROM  8
#define HL_MAX_DRIVES 16
#define HL_MAX_VARS   64

// Report a repeatable argument given more than `max` times.  Returns main's
// exit status.
static int too_many(const char *program, const char *what, int max, const char *value) {
    fprintf(stderr, "Error: too many %s (at most %d): %s\n", what, max, value);
    fprintf(stderr, "Run %s --help for usage.\n", program);
    return 1;
}

// The REPL: frame-units while the machine runs, a statement from stdin as
// soon as it is complete, until quit, SIGTERM or --cycles.  --cycles counts
// instructions run, across every machine the session runs: a machine.boot or
// checkpoint.load replaces the one being counted, and the count carries on
// from the new one's start (platform_machine_attached) rather than
// subtracting across them.
static void interactive_run(uint64_t max_cycles, int quiet) {
    if (!quiet)
        printf("\nStarting emulation (Ctrl+C to stop)...\n\n");

    uint64_t spent_cycles = 0;
    uint64_t last_cycles = cpu_instr_count();
    s_count_rebase = false;

    while (g_running && !g_quit_requested) {
        // Check for max cycles limit
        if (max_cycles > 0) {
            uint64_t now = cpu_instr_count();
            if (s_count_rebase) {
                s_count_rebase = false;
                last_cycles = now;
            }
            if (now > last_cycles)
                spent_cycles += now - last_cycles;
            last_cycles = now;
            if (spent_cycles >= max_cycles) {
                if (!quiet)
                    printf("\nReached cycle limit (%llu cycles)\n", (unsigned long long)max_cycles);
                break;
            }
        }

        // One frame-unit per iteration while the machine runs (the same
        // step web2's tick runs, unthrottled), then the drain; a REPL line
        // runs as a job, the loop pumping inside its wait.  Idle, the wait
        // for stdin is the loop's sleep (1 ms, so the drain keeps serving a
        // job's calls), and typed input ends it at once.
        bool busy = hl_pump_once();
        repl_poll(busy ? 0 : 1);
        hl_honour_sigterm();

        // Handle interrupt (Ctrl+C stops emulation but doesn't exit)
        if (g_interrupted) {
            g_interrupted = 0;
            hl_interrupt(HL_CLIENT_STDIN);
            printf("\n[Interrupted]\n");
            print_prompt();
        }
    }
}

int main(int argc, char *argv[]) {
    debug_set_failure_hook(headless_failure_hook);

    // Configuration from arguments
    const char *rom_file = NULL;
    const char *hd_files[HL_MAX_HD] = {NULL};
    int hd_count = 0;
    const char *cdrom_files[HL_MAX_CDROM] = {NULL}; // cdrom=<file> arguments
    int cdrom_count = 0;
    const char *fd_files[FLOPPY_NUM_DRIVES] = {NULL};
    int fd_count = 0;
    const char *fd_explicit[FLOPPY_NUM_DRIVES] = {NULL}; // fd0= and fd1= explicit drive assignments
    const char *script_file = NULL;
    const char *speed_mode = "paced";
    uint64_t max_cycles = 0;
    uint32_t ram_kb = 0;
    const char *model_override = NULL;
    const char *video_card_arg = NULL;
    const char *slots_arg = NULL;
    const char *monitor_arg = NULL;
    const char *config_arg = NULL; // config=: a JSON document, or a file holding one
    const char *drive_args[HL_MAX_DRIVES] = {NULL}; // drive=bus:unit:type[:image]
    int drive_count = 0;
    int quiet = 0;
    int script_stdin = 0;
    int kill_daemon = 0;
    int daemon_mode = 0; // --daemon
    int daemon_port = DAEMON_DEFAULT_PORT;
    int script_rc = 0; // the exit status script= and --script-stdin leave
    int rc = 1; // main's own exit status on the failure paths below
    int no_prompt = 0;
    const char *checkpoint_dir = NULL; // explicit --checkpoint-dir=
    const char *var_defs[HL_MAX_VARS] = {NULL}; // --var NAME=VALUE definitions
    int var_count = 0;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "-q") == 0) {
            quiet = 1;
            continue;
        }

        if (strcmp(arg, "--daemon") == 0) {
            daemon_mode = 1;
            quiet = 1; // suppress banner in daemon mode
            continue;
        }

        if (strcmp(arg, "--script-stdin") == 0) {
            script_stdin = 1;
            continue;
        }

        if (strcmp(arg, "--io=sync") == 0) {
            g_io_sync = 1; // no I/O worker: writes run inline, byte-identical (a bisecting aid)
            continue;
        }

        if (strcmp(arg, "--jobs=inline") == 0) {
            g_jobs_inline = 1; // no job thread: scripts run on the emulator thread (a bisecting aid)
            continue;
        }

        if (strcmp(arg, "--framed") == 0) {
            g_framed = 1; // core events as `@event` lines, `@end` after each statement
            continue;
        }

        if (strcmp(arg, "--kill") == 0) {
            kill_daemon = 1;
            continue;
        }

        if (strcmp(arg, "--no-prompt") == 0) {
            no_prompt = 1;
            continue;
        }

        if (strncmp(arg, "--port=", 7) == 0) {
            daemon_port = atoi(arg + 7);
            if (daemon_port <= 0 || daemon_port > 65535) {
                fprintf(stderr, "Error: Invalid port number: %s\n", arg + 7);
                return 1;
            }
            continue;
        }

        if (strncmp(arg, "--speed=", 8) == 0) {
            speed_mode = arg + 8;
            // An unknown mode is an error, not a silent paced run.
            // The daemon's pacing is host state: every machine it builds or
            // restores runs under it (platform_pacing).
            if (!scheduler_mode_from_string(speed_mode, &s_pacing.mode)) {
                fprintf(stderr, "Error: unknown --speed '%s' (paced, accelerated or turbo)\n", speed_mode);
                return 1;
            }
            continue;
        }

        if (strncmp(arg, "--cycles=", 9) == 0) {
            max_cycles = strtoull(arg + 9, NULL, 10);
            continue;
        }

        if (strncmp(arg, "--shared-dir=", 13) == 0) {
            snprintf(g_shared_dir, sizeof(g_shared_dir), "%s", arg + 13);
            continue;
        }
        if (strncmp(arg, "--print-dir=", 12) == 0) {
            snprintf(g_print_dir, sizeof(g_print_dir), "%s", arg + 12);
            continue;
        }
        if (strncmp(arg, "--checkpoint-dir=", 17) == 0) {
            checkpoint_dir = arg + 17;
            continue;
        }

        // --var NAME=VALUE: set a shell variable before script execution
        if (strncmp(arg, "--var", 5) == 0) {
            const char *def = NULL;
            if (arg[5] == '=') {
                def = arg + 6;
            } else if (arg[5] == '\0' && i + 1 < argc) {
                def = argv[++i];
            }
            if (!def || !strchr(def, '=')) {
                fprintf(stderr, "Error: --var requires NAME=VALUE\n");
                return 1;
            }
            if (var_count >= HL_MAX_VARS)
                return too_many(argv[0], "--var definitions", HL_MAX_VARS, def);
            var_defs[var_count++] = def;
            continue;
        }

        if ((value = parse_arg(arg, "rom")) != NULL) {
            rom_file = value;
            continue;
        }

        if ((value = parse_arg(arg, "hd")) != NULL) {
            if (hd_count >= HL_MAX_HD)
                return too_many(argv[0], "hd= images", HL_MAX_HD, value);
            hd_files[hd_count++] = value;
            continue;
        }

        if ((value = parse_arg(arg, "cdrom")) != NULL) {
            if (cdrom_count >= HL_MAX_CDROM)
                return too_many(argv[0], "cdrom= images", HL_MAX_CDROM, value);
            cdrom_files[cdrom_count++] = value;
            continue;
        }

        if ((value = parse_arg(arg, "fd0")) != NULL) {
            fd_explicit[0] = value;
            continue;
        }

        if ((value = parse_arg(arg, "fd1")) != NULL) {
            fd_explicit[1] = value;
            continue;
        }

        if ((value = parse_arg(arg, "fd")) != NULL) {
            if (fd_count >= FLOPPY_NUM_DRIVES)
                return too_many(argv[0], "fd= images", FLOPPY_NUM_DRIVES, value);
            fd_files[fd_count++] = value;
            continue;
        }

        if ((value = parse_arg(arg, "ram")) != NULL) {
            ram_kb = (uint32_t)strtoul(value, NULL, 10);
            continue;
        }

        if ((value = parse_arg(arg, "model")) != NULL) {
            model_override = value;
            continue;
        }

        if ((value = parse_arg(arg, "video_card")) != NULL) {
            video_card_arg = value;
            continue;
        }

        if ((value = parse_arg(arg, "slots")) != NULL) {
            slots_arg = value;
            continue;
        }

        if ((value = parse_arg(arg, "config")) != NULL) {
            config_arg = value;
            continue;
        }
        if ((value = parse_arg(arg, "drive")) != NULL) {
            if (drive_count >= HL_MAX_DRIVES)
                return too_many(argv[0], "drive= arguments", HL_MAX_DRIVES, value);
            drive_args[drive_count++] = value;
            continue;
        }
        if ((value = parse_arg(arg, "monitor")) != NULL) {
            monitor_arg = value;
            continue;
        }

        if ((value = parse_arg(arg, "script")) != NULL) {
            script_file = value;
            continue;
        }

        fprintf(stderr, "Unknown argument: %s\n", arg);
        print_usage(argv[0]);
        return 1;
    }

    // Validate required arguments
    if (!rom_file) {
        fprintf(stderr, "Error: ROM file is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // The ROM path is not checked here: it may run through an image or an
    // archive (roms.zip/Plus.rom), which only the core's VFS resolves.  The
    // loader reports a path that does not open.

    // Line-buffer stdout when output is redirected or piped
    if (script_file || !isatty(STDOUT_FILENO)) {
        setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    }

    // Setup signal handlers
    install_signal(SIGINT, sigint_handler);
    install_signal(SIGTERM, sigterm_handler);
    // Ignore SIGPIPE: in daemon mode stdout/stderr are dup2'd to the
    // client socket, so any printf after the client disconnects would
    // otherwise kill the daemon.  With SIG_IGN, write() returns -1 /
    // EPIPE, and daemon_client_gone (in hl_run_statement)
    // notices the failed write and cancels the run.
    signal(SIGPIPE, SIG_IGN);

    if (!quiet) {
        printf("Granny Smith - Headless Macintosh Emulator\n");
        printf("==========================================\n");
        printf("ROM:    %s\n", rom_file);
        if (ram_kb > 0)
            printf("RAM:    %u KB\n", ram_kb);
        for (int i = 0; i < hd_count; i++)
            printf("HD[%d]:  %s\n", i, hd_files[i]);
        for (int i = 0; i < cdrom_count; i++)
            printf("CD[%d]:  %s\n", i, cdrom_files[i]);
        for (int i = 0; i < fd_count; i++)
            printf("FD[%d]:  %s\n", i, fd_files[i]);
        if (script_file)
            printf("Script: %s\n", script_file);
        printf("Speed:  %s\n", speed_mode);
        if (max_cycles > 0)
            printf("Cycles: %llu\n", (unsigned long long)max_cycles);
        printf("\n");
    }

    // Initialize the process core (shell, object root, singletons)
    if (core_init() != 0) {
        fprintf(stderr, "Error: core initialisation failed\n");
        return 1;
    }

    // The mailbox this process is a client of, and the job thread that runs
    // its scripts (falls back to inline scripts if the thread cannot start).
    hl_mailbox_init();
    if (g_jobs_inline)
        job_inline_enable(hl_inline_frame);
    else if (!job_thread_start(512u << 10))
        fprintf(stderr, "headless: job thread could not be started; scripts run inline\n");
    if (!g_io_sync && !io_worker_start(256u << 10))
        fprintf(stderr, "headless: I/O worker could not be started; writes run inline\n");

    // Apply --var definitions (after core_init, whose shell_init calls shell_var_init)
    for (int i = 0; i < var_count; i++) {
        // Split NAME=VALUE at first '='
        char buf[256];
        strncpy(buf, var_defs[i], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *eq = strchr(buf, '=');
        if (eq) {
            *eq = '\0';
            shell_var_set(buf, eq + 1);
        }
    }

    setup_init();

    // Apply --no-prompt default so every client connection inherits it
    if (no_prompt)
        debug_set_prompt_default(false);

    // $GS_SHARED_DIR is the fallback for --shared-dir, so a CI job can turn the
    // default volume on without touching every invocation.
    if (!g_shared_dir[0]) {
        const char *env_dir = getenv("GS_SHARED_DIR");
        if (env_dir && *env_dir)
            snprintf(g_shared_dir, sizeof(g_shared_dir), "%s", env_dir);
    }
    system_set_default_share(g_shared_dir); // the network publishes it now, for every machine

    // $GS_PRINT_DIR is the fallback for --print-dir.  A directory without the
    // interpreter linked would never receive anything; say so up front.
    if (!g_print_dir[0]) {
        const char *env_dir = getenv("GS_PRINT_DIR");
        if (env_dir && *env_dir)
            snprintf(g_print_dir, sizeof(g_print_dir), "%s", env_dir);
    }
    if (g_print_dir[0] && !atalk_printer_has_interpreter())
        fprintf(stderr,
                "warning: --print-dir set but this build has no PostScript interpreter (build with PLATEN=1)\n");

    // If a --checkpoint-dir was given, point the machine layer at it
    // verbatim so writable image deltas and quick checkpoints land there.  No
    // id/timestamp suffix — headless callers manage the directory themselves.
    // It is also the root a machine.register'ed identity nests under, as
    // /opfs/checkpoints is in the browser (the default root does not exist
    // on a host).
    if (checkpoint_dir && *checkpoint_dir) {
        if (checkpoint_machine_set_dir(checkpoint_dir) != 0) {
            fprintf(stderr, "Error: cannot create --checkpoint-dir %s: %s\n", checkpoint_dir, strerror(errno));
            goto out;
        }
        checkpoint_machine_set_root(checkpoint_dir);
    }

    // Probe the ROM to find compatible machines, then explicitly boot one.
    // ROM identity does not pick the machine — multiple Mac models share the
    // same ROM (Universal IIx/IIcx/SE/30), so the user picks via model=.
    rom_file_info_t rom_fi = {0};
    if (rom_probe_file(rom_file, &rom_fi) != 0 || !rom_fi.info) {
        fprintf(stderr, "Error: ROM file %s could not be identified\n", rom_file);
        goto out;
    }
    // A known ROM for a machine that is not emulated has no model to boot.
    if (!rom_is_supported(rom_fi.info)) {
        fprintf(stderr, "Error: ROM file %s is the %s, for a machine Granny Smith does not emulate\n", rom_file,
                rom_fi.info->family_name);
        goto out;
    }

    // Resolve target machine: model= overrides; else use the first entry in
    // the ROM's compatible list (the family default, e.g. SE/30 for Universal).
    const char *target_model = model_override;
    if (!target_model) {
        target_model = rom_fi.info->compatible[0];
    } else {
        // Validate that the override is in the compatibility list.
        bool ok = false;
        for (const char *const *p = rom_fi.info->compatible; *p; p++) {
            if (strcmp(*p, target_model) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            fprintf(stderr, "Error: model=%s is not compatible with this ROM (%s).\n", target_model,
                    rom_fi.info->family_name);
            fprintf(stderr, "Compatible models:");
            for (const char *const *p = rom_fi.info->compatible; *p; p++)
                fprintf(stderr, " %s", *p);
            fprintf(stderr, "\n");
            goto out;
        }
    }

    const hw_profile_t *profile = machine_find(target_model);
    if (!profile) {
        fprintf(stderr, "Error: machine model '%s' is not registered\n", target_model);
        goto out;
    }

    // Offer the ROM's sibling *.vrom files to the content-addressed registry
    // BEFORE the boot so the card factories can match them during machine
    // bring-up (offers persist across machine.boot).
    offer_sibling_card_roms(rom_file);

    // Startup is the same boot-document path scripts use (machine.boot):
    // CLI args fill the document, and machine_boot_apply validates and
    // constructs.
    //
    // config= is the whole document (a file, or JSON inline); drive= adds to
    // its storage, and fd1= / two fd= ask for the second floppy position.
    char *config_text = NULL;
    if (config_arg && *config_arg && *config_arg != '{') {
        config_text = read_text_file(config_arg);
        if (!config_text) {
            fprintf(stderr, "Error: cannot read config=%s\n", config_arg);
            goto out;
        }
    }
    char drives_spec[512] = "";
    for (int i = 0; i < drive_count; i++) {
        // bus:unit:type -- the image, a fourth field, is attached below.
        char one[128];
        snprintf(one, sizeof one, "%s", drive_args[i]);
        char *c1 = strchr(one, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL, *c3 = c2 ? strchr(c2 + 1, ':') : NULL;
        if (c3)
            *c3 = '\0';
        size_t at = strlen(drives_spec);
        snprintf(drives_spec + at, sizeof drives_spec - at, "%s%s", at ? ";" : "", one);
    }
    boot_config_t boot_doc = {
        .model = target_model,
        .ram_kb = ram_kb,
        .rom = rom_file,
        .video_card = video_card_arg,
        .slots = slots_arg,
        .monitor = monitor_arg,
        .video_sense = -1,
        .config = config_text ? config_text : config_arg,
        .drives = drives_spec,
        .floppies_wanted = (fd_explicit[1] || fd_count > 1) ? 2 : 0,
    };
    value_t boot_err = machine_boot_apply(&boot_doc);
    free(config_text);
    if (val_is_error(&boot_err)) {
        fprintf(stderr, "Error: %s\n", boot_err.err ? boot_err.err : "boot failed");
        value_free(&boot_err);
        goto out;
    }

    // machine_boot_apply swapped the new machine in: global_emulator is it.

    // In daemon mode, stop the scheduler that se30_init/plus_init auto-started.
    // The agent will explicitly send "run" or "s" commands to control execution.
    if (daemon_mode) {
        scheduler_t *s = system_scheduler();
        if (s)
            scheduler_stop(s);
    }

    // Hard disks, one per hd=, into the model's hard-disk bays in attach
    // order -- the boot bay first, then the rest as declared -- on whatever
    // bus each bay is on (machine.scsi, machine.scsi2, or the Lisa's ProFile).
    // The web frontend attaches through the same bays (machine.attach_hd), so
    // the two front ends put a disk in the same place.  This used to put hd=N
    // at SCSI id N on the first bus whatever the model: on a Lisa that handed
    // a NULL bus to the SCSI layer and crashed, on a Network Server it put the
    // first disk beside the boot bay, and a fourth disk on a Mac landed on the
    // CD bay.  A disk that cannot be placed stops the
    // run: a test must not go on against a machine it did not ask for.
    const hw_profile_t *active = profile; // the model machine_boot_apply just built
    media_bay_t bays[MEDIA_HD_BAYS_MAX];
    int n_bays = profile_hd_bays(active, bays, MEDIA_HD_BAYS_MAX);
    char attach_err[256];
    for (int i = 0; i < hd_count; i++) {
        if (i >= n_bays) {
            fprintf(stderr, "Error: %s has %d hard-disk bay(s); none left for hd=%s\n", active->name, n_bays,
                    hd_files[i]);
            goto out;
        }
        if (system_media_attach_path(global_emulator, &bays[i], false, hd_files[i], attach_err, sizeof(attach_err)) !=
            0) {
            fprintf(stderr, "Error: hd=%s: %s\n", hd_files[i], attach_err);
            goto out;
        }
        if (!quiet)
            printf("Attached HD[%d]: %s (%s)\n", i, hd_files[i], bays[i].label);
    }

    // drive=bus:unit:type:image -- the image into the drive the document now
    // has at that position.
    for (int i = 0; i < drive_count; i++) {
        char one[512];
        snprintf(one, sizeof one, "%s", drive_args[i]);
        char *bus = one, *unit = strchr(one, ':'), *type = unit ? strchr(unit + 1, ':') : NULL;
        char *image = type ? strchr(type + 1, ':') : NULL;
        if (!image)
            continue;
        *unit++ = '\0';
        *type++ = '\0';
        *image++ = '\0';
        media_bay_t bay;
        if (!machine_storage_media_bay(active, bus, (int)strtol(unit, NULL, 10), &bay) ||
            system_media_attach_path(global_emulator, &bay, strcmp(type, "cd") == 0, image, attach_err,
                                     sizeof(attach_err)) != 0) {
            fprintf(stderr, "Error: drive=%s: %s\n", drive_args[i], attach_err);
            goto out;
        }
        if (!quiet)
            printf("Attached %s: %s (%s unit %s)\n", strcmp(type, "cd") == 0 ? "CD-ROM" : "HD", image, bus, unit);
    }

    // The CD, into the model's CD bay (profile_cdrom_bay: its cdrom_id -- 3 on
    // every Macintosh, 0 on the Network Servers), on a model that has one.
    // There is one bay, so one cdrom=.
    if (cdrom_count > 1) {
        fprintf(stderr, "Error: %s has one CD-ROM bay; got %d cdrom= arguments\n", active->name, cdrom_count);
        goto out;
    }
    if (cdrom_count == 1) {
        media_bay_t cd;
        if (!profile_cdrom_bay(active, &cd)) {
            fprintf(stderr, "Error: %s has no CD-ROM bay for cdrom=%s\n", active->name, cdrom_files[0]);
            goto out;
        }
        if (system_media_attach_path(global_emulator, &cd, true, cdrom_files[0], attach_err, sizeof(attach_err)) != 0) {
            fprintf(stderr, "Error: cdrom=%s: %s\n", cdrom_files[0], attach_err);
            goto out;
        }
        if (!quiet)
            printf("Attached CD-ROM: %s (SCSI ID %d)\n", cdrom_files[0], cd.unit);
    }

    // Insert explicit fd0=/fd1= floppy images into their designated drives
    for (int i = 0; i < FLOPPY_NUM_DRIVES; i++) {
        if (!fd_explicit[i])
            continue;
        int rc = system_fd_insert(fd_explicit[i], i, true);
        if (rc == 0) {
            if (!quiet)
                printf("Inserted FD[%d]: %s\n", i, fd_explicit[i]);
        } else {
            fprintf(stderr, "Warning: Cannot open floppy image for drive %d: %s\n", i, fd_explicit[i]);
        }
    }

    // Insert sequential fd= floppy images into first available drives
    // (writable by default, matching the legacy `fd insert` default).
    for (int i = 0; i < fd_count; i++) {
        int rc = system_fd_insert(fd_files[i], -1, true);
        if (rc == 0) {
            if (!quiet)
                printf("Inserted FD[%d]: %s\n", i, fd_files[i]);
        } else {
            fprintf(stderr, "Warning: Cannot open floppy image: %s\n", fd_files[i]);
        }
    }

    // Startup order: script= runs first, then --script-stdin, then the
    // daemon or the REPL.  A `quit` in either script, or a script= that
    // fails, ends the process there: neither the daemon nor the REPL starts.
    if (script_file) {
        if (!quiet)
            printf("Running script: %s\n", script_file);
        script_rc = run_script_file(script_file);
    }

    // Run commands from stdin if --script-stdin
    if (script_stdin && !g_quit_requested) {
        int stdin_rc = run_script_stdin();
        if (stdin_rc)
            script_rc = stdin_rc;
    }

    if (daemon_mode) {
        // Daemon mode: the shell on a TCP socket (headless_daemon.c); the
        // scheduler stays stopped until a client runs it.  After a `quit`
        // above it prints READY and returns at once.
        if (daemon_run(daemon_port, kill_daemon) != 0)
            goto out;
    } else {
        // Interactive mode: the REPL on stdin/stdout
        if (!g_quit_requested)
            interactive_run(max_cycles, quiet);
        if (!quiet)
            printf("\nShutting down...\n");
    }
    rc = headless_exit_code(script_rc);

out:
    if (global_emulator) {
        system_destroy(global_emulator);
        global_emulator = NULL;
    }
    return rc;
}
