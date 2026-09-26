// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// headless_main.c
// Command-line entry point for running the emulator without a GUI.

#include "platform.h"

#include "appletalk.h"
#include "checkpoint_machine.h"
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
#include "prom.h"
#include "rom.h"
#include "scheduler.h"
#include "script.h"
#include "scsi.h"
#include "shell.h"
#include "shell_var.h"
#include "system.h"
#include "vrom.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#endif

// Platform stubs for functions defined in WASM but needed by core

// Video force redraw - no-op in headless
void frontend_force_redraw(void) {
    // No video in headless mode
}

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

// VBL is
// no longer a scheduler event armed per machine: the run loop injects it
// imperatively, one VBL pulse per frame-unit, via scheduler_run_frame() — the
// same path web2's scheduler_main_loop() takes.  See pump_scheduler_with_heartbeat
// / the main loop below, and docs/core/scheduler/scheduler.md §10.

// Signal handling for graceful shutdown
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_interrupted = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_stop(sched);
}

static void sigterm_handler(int sig) {
    (void)sig;
    g_running = 0;
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_stop(sched);
}

// PID file path for daemon mode
static char g_pid_path[256] = {0};

// Build PID file path for the given port
static void pid_file_path(int port, char *buf, size_t len) {
    snprintf(buf, len, "/tmp/gs-headless-%d.pid", port);
}

// Write PID file for daemon mode
static void write_pid_file(int port) {
    pid_file_path(port, g_pid_path, sizeof(g_pid_path));
    FILE *f = fopen(g_pid_path, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

// Remove PID file on exit
static void remove_pid_file(void) {
    if (g_pid_path[0])
        unlink(g_pid_path);
}

// Kill existing daemon on the given port
static int kill_existing_daemon(int port) {
    char path[256];
    pid_file_path(port, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return 0; // no PID file, nothing to kill
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1 || pid <= 0) {
        fclose(f);
        unlink(path);
        return 0;
    }
    fclose(f);
    // Check if process exists
    if (kill(pid, 0) != 0) {
        unlink(path); // stale PID file
        return 0;
    }
    // Send SIGTERM
    fprintf(stderr, "Killing existing daemon (PID %d) on port %d\n", pid, port);
    kill(pid, SIGTERM);
    // Wait briefly for it to exit
    for (int i = 0; i < 20; i++) {
        usleep(100000); // 100ms
        if (kill(pid, 0) != 0)
            break;
    }
    unlink(path);
    return 1;
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
    printf("  fd1=<file>      Floppy disk image for drive 1 (external)\n");
    printf("  video_card=<id> NuBus video card for the configurable slot (e.g. 824gc);\n");
    printf("                  default: the machine's default card\n");
    printf("  monitor=<id>    monitor on the built-in video port ('none' = unconnected,\n");
    printf("                  which hands the screen to a NuBus card)\n");
    printf("  script=<file>   Shell script file to execute at startup (optional)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help, -h      Display this help message\n");
    printf("  --speed=MODE    Pacing mode: paced, accelerated, turbo (default: paced; legacy\n");
    printf("                  aliases realtime/hardware map to paced, max to turbo). Headless\n");
    printf("                  runs are budget-driven; only 'accelerated' changes execution\n");
    printf("                  (more instructions per frame-unit, scheduler.speed multiplier).\n");
    printf("  --cycles=N      Run for N CPU cycles then exit (for testing)\n");
    printf("  --quiet, -q     Suppress startup messages\n");
    printf("  --daemon        Start in daemon mode (TCP socket interface for AI agents)\n");
    printf("  --port=PORT     TCP port for daemon mode (default: 6800)\n");
    printf("  --kill          Kill existing daemon on same port before starting\n");
    printf("  --script-stdin  Read script commands from stdin instead of a file\n");
    printf("  --var NAME=VAL  Set a shell variable (can be repeated)\n");
    printf("  --no-prompt     Disable the prompt status line for all connections\n");
    printf("  --checkpoint-dir=DIR  Directory to host writable image deltas (default: alongside base image)\n");
    printf("  --shared-dir=DIR     Publish DIR as the default AppleShare volume \"Shared\"\n");
    printf("                       (also settable with $GS_SHARED_DIR; created if missing)\n");
    printf("  --print-dir=DIR      Write each LaserWriter job's PDF as DIR/<job>-<title>.pdf\n");
    printf("                       (also $GS_PRINT_DIR; created if missing; needs a PLATEN=1 build)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s rom=plus.rom\n", program);
    printf("  %s rom=plus.rom hd=disk.img\n", program);
    printf("  %s rom=plus.rom fd=system.dsk script=boot.sh\n", program);
    printf("  %s --daemon rom=iix-iicx-se30-97221136.rom\n", program);
    printf("  %s --daemon --port=7000 rom=iix-iicx-se30-97221136.rom hd=disk.img\n", program);
}

// Global script exit code (set by commands like screenshot match)
static int g_script_exit_code = 0;

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

// The exit status: the script's own, or 3 if any assertion failed.
static int headless_exit_code(void) {
    if (g_assert_failures) {
        fprintf(stderr, "headless: %u assertion failure(s) this run\n", g_assert_failures);
        if (g_script_exit_code == 0)
            return 3;
    }
    return g_script_exit_code;
}

// Quit flag for headless mode - set by quit command
static volatile int quit_requested = 0;

// Platform impl of gs_quit (the weak default in system.c says "not
// supported": the browser owns the page).
int gs_quit(void) {
    quit_requested = 1;
    // Stop scheduler to break out of any running emulation
    scheduler_t *sched = system_scheduler();
    if (sched) {
        scheduler_stop(sched);
    }
    return 0;
}

// Legacy shell `quit` — thin shim.
// Forward declaration for run_script_file (used by main script-flag path).
static int run_script_file(const char *filename);

// Forward decl — used by the daemon-mode loop.
static void pump_scheduler_with_heartbeat(void);

// ============================================================================
// Daemon mode: TCP socket interface for AI agents
// ============================================================================

// Default daemon port (Motorola 68xx heritage)
#define DAEMON_DEFAULT_PORT 6800

// Daemon mode state
static int g_daemon_mode = 0;
static int g_daemon_port = DAEMON_DEFAULT_PORT;
static int g_listen_fd = -1;
static int g_client_fd = -1;
static bool g_client_lost = false; // a write to the client failed: stop serving it
static int g_saved_stdout = -1;
static int g_saved_stderr = -1;

// Redirect stdout/stderr to the client socket so printf output goes to the agent
static void daemon_redirect_output(int client_fd) {
    g_saved_stdout = dup(STDOUT_FILENO);
    g_saved_stderr = dup(STDERR_FILENO);
    dup2(client_fd, STDOUT_FILENO);
    dup2(client_fd, STDERR_FILENO);
}

// Restore stdout/stderr to their original destinations
static void daemon_restore_output(void) {
    fflush(stdout);
    fflush(stderr);
    if (g_saved_stdout >= 0) {
        dup2(g_saved_stdout, STDOUT_FILENO);
        close(g_saved_stdout);
        g_saved_stdout = -1;
    }
    if (g_saved_stderr >= 0) {
        dup2(g_saved_stderr, STDERR_FILENO);
        close(g_saved_stderr);
        g_saved_stderr = -1;
    }
}

// Create and bind the listening socket for daemon mode
static int daemon_create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("daemon: socket");
        return -1;
    }

    // Allow port reuse for quick restarts
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // bind to 127.0.0.1 only
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: port %d already in use. Use --port=N to specify a different port.\n", port);
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        perror("daemon: listen");
        close(fd);
        return -1;
    }

    return fd;
}

// Whether the daemon client is gone.  EOF is not that: a half-closing client
// (nc -N, nc -q, Python's shutdown(SHUT_WR)) sends a FIN and still reads the
// output, and a FIN cannot tell a half-close from a close -- the old check
// peeked recv() == 0 and cancelled such a client's own scheduler.run.  Only
// a failed write can tell, and it shows up as POLLERR/POLLHUP on the socket
// once a write has drawn the peer's RST (the once-a-second heartbeat writes,
// so a closed client is noticed within ~1-2 s), or as a failed fflush.
static bool daemon_client_gone(int client_fd) {
    if (client_fd < 0)
        return false;
    struct pollfd pfd = {.fd = client_fd, .events = 0, .revents = 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return true;
    return ferror(stdout) != 0;
}

// While a run is in flight the daemon is inside one client's dispatch, so a
// second connection waits in the listen backlog until that dispatch returns.
// For a bare `scheduler.run` that is a short wait; for a `scheduler.run`
// inside a shell `while` loop it is forever, and the daemon looks wedged with
// only kill -9 as a way out.  Answer such a connection out of band instead:
// the run is stoppable from it, and anything else is refused with a note
// rather than executed, since the shell is busy with the first client.
static void daemon_serve_control_connection(void) {
    struct pollfd pfd = {.fd = g_listen_fd, .events = POLLIN, .revents = 0};
    if (g_listen_fd < 0 || poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
        return;
    int fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0)
        return;

    // One line, and only what is already there: a control client that sends
    // nothing must not stall the run it came to stop.
    char line[256];
    ssize_t n = recv(fd, line, sizeof(line) - 1, MSG_DONTWAIT);
    if (n < 0)
        n = 0;
    line[n] = '\0';
    for (char *p = line; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = '\0';
    char *cmd = line;
    while (*cmd == ' ')
        cmd++;

    const char *reply;
    if (strcmp(cmd, "stop") == 0 || strcmp(cmd, "scheduler.stop") == 0 || strcmp(cmd, "shell.interrupt") == 0 ||
        strcmp(cmd, "shell.interrupt()") == 0) {
        // The two halves of the terminal's Ctrl-C: end the run in flight, and
        // cancel the script loop that would otherwise start the next one.
        scheduler_t *s = system_scheduler();
        if (s)
            scheduler_stop(s);
        script_interrupt();
        reply = "# run stopped by control connection\n";
    } else if (strcmp(cmd, "quit") == 0) {
        quit_requested = 1;
        reply = "# quit requested\n";
    } else {
        reply = "# busy: a run is in flight on another connection; "
                "this connection accepts only stop / shell.interrupt / quit\n";
    }
    (void)!write(fd, reply, strlen(reply));
    close(fd);
}

// Pump the scheduler until it stops, emitting periodic heartbeat lines.
// In daemon mode the heartbeat prevents nc -w timeouts; in script/stdin modes it
// lets callers follow progress. Emits once per second with instruction count.
// Also: if the daemon client is gone mid-run, stop the scheduler — a dead
// client has no way to see the result, and letting it run billions more
// instructions is wasteful.  Data arriving mid-run does NOT stop the run: it
// is the client's next statement, and it waits its turn.  Stopping a run is
// a second connection's job (daemon_serve_control_connection).
static void pump_scheduler_with_heartbeat(void) {
    scheduler_t *sched = system_scheduler();
    config_t *cfg = global_emulator;
    double last_heartbeat = host_time();
    uint64_t start_instr = cpu_instr_count();

    // Drive execution exactly like web2's RAF loop: one VBL frame-unit at a
    // time (trigger_vbl + one VBL-period run, via scheduler_run_frame), as fast
    // as the host allows.  Yielding between frame-units lets the heartbeat fire
    // and the daemon poll for disconnect/new-data.  The frame-unit is the same
    // deterministic step web2 runs, so headless reproduces the web2 boot
    // bit-for-bit (only the pacing — max speed here, host-clock there —
    // differs).  That includes the power-up PRAM: the machine is built with
    // it (rtc.h pram_defaults_t) on both, where on the PDM machines the page
    // used to seed its own and headless booted differently.  An
    // instruction-budget `scheduler.run N` schedules a
    // run_stop_event; scheduler_run_frame's inner scheduler_run clamps to it, so
    // the budget stops mid-frame at exactly N and the loop below exits.

    while (sched && cfg && scheduler_is_running(sched) && !quit_requested) {
        scheduler_run_frame(sched, cfg);

        // Heartbeat: once per second, print progress
        double now = host_time();
        if (now - last_heartbeat >= 1.0) {
            uint64_t current = cpu_instr_count();
            printf("# running... %llu instructions (+%llu since start)\n", (unsigned long long)current,
                   (unsigned long long)(current - start_instr));
            fflush(stdout);
            last_heartbeat = now;
        }

        // Daemon mode: is the client still there to read the result?
        if (g_daemon_mode && g_client_fd >= 0) {
            if (daemon_client_gone(g_client_fd)) {
                // Client gone — cancel the run rather than execute billions
                // more.  Cancel the script too: a shell `while` loop would
                // otherwise start the next `scheduler.run` immediately and
                // spin on forever with nobody left to read its output.
                g_client_lost = true;
                scheduler_stop(sched);
                script_interrupt();
                break;
            }
            // A second client with something to say about this run.
            daemon_serve_control_connection();
        }
    }
}

// === Statements from a stream ==============================================
//
// A statement is complete when it ends at a newline and is balanced
// (script_needs_continuation).  The daemon, stdin and the REPL all feed lines
// to one assembler and run each statement the moment it is complete.  The
// daemon used to guess where a request ended BEFORE running anything -- read
// until a newline plus 1 ms of silence, then run the lot -- and every one of
// its defects was that guess: a 2 KB cap, a fragment dispatched half-read, a
// second send lost, a block silently dropped.

#define STMT_MAX (1u << 20) // 1 MB

typedef struct stmt_asm {
    char *buf;
    size_t len, cap;
} stmt_asm_t;

// Feed one line (no newline).  Returns 1 when a.buf holds a complete statement
// (run it, then stmt_reset), 0 when more lines are needed, -1 when the
// statement would pass STMT_MAX (it has been discarded).  Blank lines outside
// a statement are skipped.
static int stmt_feed_line(stmt_asm_t *a, const char *line, size_t len) {
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

static void stmt_reset(stmt_asm_t *a) {
    a->len = 0;
    if (a->buf)
        a->buf[0] = '\0';
}

static void stmt_free(stmt_asm_t *a) {
    free(a->buf);
    *a = (stmt_asm_t){0};
}

// Run one complete daemon statement, pump the run it starts, and report the
// PC as a status line.
static void daemon_run_statement(char *stmt) {
    shell_dispatch(stmt);
    pump_scheduler_with_heartbeat();
    if (!g_client_lost && system_is_initialized() && debug_prompt_enabled()) {
        char disasm_buf[160];
        debugger_disasm_pc(disasm_buf, sizeof(disasm_buf));
        if (disasm_buf[0] != '\0')
            printf("%s\n", disasm_buf);
    }
    fflush(stdout);
}

// How long a client may stay silent, after its last statement has finished
// and its output is flushed, before the daemon hangs up.  Not a latency: a
// statement runs the moment it is complete.  `echo cmd | nc -w 2` keeps
// working (the close lands well inside nc's timeout); `nc -N` just closes
// sooner.
#define DAEMON_IDLE_CLOSE_MS 500

// Serve one client: run its statements as they complete, while reading on.
// Supports any number of newline-delimited statements, in any number of
// sends, of any size up to STMT_MAX.
static void daemon_handle_client(int client_fd) {
    g_client_fd = client_fd;
    g_client_lost = false;
    daemon_redirect_output(client_fd);

    stmt_asm_t stmt = {0};
    char *pending = NULL; // bytes of a line not yet ended by a newline
    size_t pending_len = 0, pending_cap = 0;
    bool skipping = false; // dropping the rest of a line past STMT_MAX
    bool eof = false;
    double idle_since = host_time_ms();
    char chunk[65536];

    while (!quit_requested && !g_client_lost) {
        // Run every statement the input already completes.
        char *nl;
        while (!quit_requested && !g_client_lost && pending_len && (nl = memchr(pending, '\n', pending_len)) != NULL) {
            size_t line_len = (size_t)(nl - pending);
            if (!skipping) {
                int r = stmt_feed_line(&stmt, pending, line_len);
                if (r < 0)
                    printf("error: statement too long (over %u bytes); discarded\n", STMT_MAX);
                else if (r > 0) {
                    daemon_run_statement(stmt.buf);
                    stmt_reset(&stmt);
                }
            }
            skipping = false;
            memmove(pending, nl + 1, pending_len - line_len - 1);
            pending_len -= line_len + 1;
            idle_since = host_time_ms();
        }
        if (quit_requested || g_client_lost)
            break;

        if (eof) {
            // The last line may lack its newline; it is still a line.
            if (pending_len && !skipping) {
                int r = stmt_feed_line(&stmt, pending, pending_len);
                pending_len = 0;
                if (r < 0)
                    printf("error: statement too long (over %u bytes); discarded\n", STMT_MAX);
                else if (r > 0) {
                    daemon_run_statement(stmt.buf);
                    stmt_reset(&stmt);
                }
            }
            break;
        }

        // Wait for more, until the client has been idle long enough.
        double idle = host_time_ms() - idle_since;
        if (idle >= DAEMON_IDLE_CLOSE_MS)
            break;
        struct pollfd pfd = {.fd = client_fd, .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, (int)(DAEMON_IDLE_CLOSE_MS - idle) + 1);
        if (ready < 0 && errno != EINTR)
            break;
        if (ready <= 0)
            continue;
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
        if (n == 0) {
            eof = true; // no more input -- not "client gone" (see daemon_client_gone)
            continue;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            g_client_lost = true;
            break;
        }
        idle_since = host_time_ms();
        if (pending_len + (size_t)n > STMT_MAX) {
            // A line longer than any statement may be: drop it up to its end.
            printf("error: statement too long (over %u bytes); discarded\n", STMT_MAX);
            stmt_reset(&stmt);
            char *end = memchr(chunk, '\n', (size_t)n);
            pending_len = 0;
            if (!end) {
                skipping = true;
                continue;
            }
            skipping = true;
            size_t rest = (size_t)n - (size_t)(end - chunk);
            memmove(chunk, end, rest);
            n = (ssize_t)rest;
        }
        if (pending_len + (size_t)n > pending_cap) {
            size_t cap = pending_cap ? pending_cap : 65536;
            while (cap < pending_len + (size_t)n)
                cap *= 2;
            char *grown = realloc(pending, cap);
            if (!grown) {
                printf("error: out of memory reading the request\n");
                break;
            }
            pending = grown;
            pending_cap = cap;
        }
        memcpy(pending + pending_len, chunk, (size_t)n);
        pending_len += (size_t)n;
    }

    // An open block at the end of input is reported, never dropped.
    if (!g_client_lost && (stmt.len || (pending_len && !skipping)))
        printf("error: incomplete block at end of input; not run\n");
    stmt_free(&stmt);
    free(pending);

    daemon_restore_output();
    // Drain what the client still sends before closing: closing a socket with
    // unread input sends an RST, which can destroy output the client has not
    // read yet.
    shutdown(client_fd, SHUT_WR);
    for (int i = 0; i < 50; i++) {
        struct pollfd pfd = {.fd = client_fd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 10) <= 0 || recv(client_fd, chunk, sizeof(chunk), 0) <= 0)
            break;
    }
    close(client_fd);
    g_client_fd = -1;
}

// Main daemon loop: accept connections and handle them one at a time
static void daemon_loop(void) {
    fprintf(stderr, "Daemon listening on 127.0.0.1:%d\n", g_daemon_port);
    fprintf(stderr, "Send commands with: echo \"command\" | nc localhost %d\n", g_daemon_port);

    // Emit READY signal so callers can block-read instead of sleeping
    printf("READY\n");
    fflush(stdout);

    while (g_running && !quit_requested) {
        // Use select() so we can check g_running periodically
        fd_set fds;
        struct timeval tv = {1, 0}; // 1 second timeout
        FD_ZERO(&fds);
        FD_SET(g_listen_fd, &fds);

        int ready = select(g_listen_fd + 1, &fds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("daemon: select");
            break;
        }
        if (ready == 0)
            continue; // timeout, check g_running again

        // Accept client connection
        int client_fd = accept(g_listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("daemon: accept");
            continue;
        }

        daemon_handle_client(client_fd);
    }

    close(g_listen_fd);
    g_listen_fd = -1;
}

// Parse key=value argument, returns value or NULL if key doesn't match
static const char *parse_arg(const char *arg, const char *key) {
    size_t key_len = strlen(key);
    if (strncmp(arg, key, key_len) == 0 && arg[key_len] == '=') {
        return arg + key_len + 1;
    }
    return NULL;
}

// Script-interpreter pump hook: after each executed statement, drive
// the scheduler to completion (heartbeat included) and report
// whether a quit was requested so the interpreter can stop cleanly.
static bool headless_script_pump(void) {
    pump_scheduler_with_heartbeat();
    return quit_requested != 0;
}

// Run a script file through the v2 interpreter. script_run_file keeps
// the include stack, so `include` paths resolve relative to the script
// and diagnostics carry the file name; the pump hook drives the
// scheduler between statements.
static int run_script_file(const char *filename) {
    printf("> running %s\n", filename);
    int result = script_run_file(filename);
    if (result != 0) {
        // v2 scripts abort on the first error; a script that
        // aborted never reaches its `quit`, so exit here instead of
        // dropping into the interactive loop.
        g_script_exit_code = 1;
        quit_requested = 1;
    }
    return 0;
}

// Run statements from stdin, each as soon as it is complete: a
// line at a time through the statement assembler, so a multi-line block
// runs when it closes.  getline, not a 1024-byte fgets: a longer line used to
// be split into two statements.
static int run_script_stdin(void) {
    stmt_asm_t stmt = {0};
    char *line = NULL;
    size_t line_cap = 0;
    char *last_cmd = NULL;
    ssize_t n;
    while (!quit_requested && (n = getline(&line, &line_cap, stdin)) >= 0) {
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
            g_script_exit_code = 1;
            continue;
        }
        if (r == 0)
            continue;

        printf("> %s\n", stmt.buf);
        int result = script_run_line(stmt.buf); // interactive: results print
        stmt_reset(&stmt);
        if (result != 0)
            g_script_exit_code = 1;
    }
    if (stmt.len) {
        printf("error: incomplete block at end of input; not run\n");
        g_script_exit_code = 1;
    }
    stmt_free(&stmt);
    free(line);
    free(last_cmd);
    return 0;
}

// Check if stdin has data available (non-blocking)
static int stdin_has_data(void) {
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

// Shell prompt — shared builder in shell.c (same text as the web shell)
void print_prompt(void) {
    if (g_daemon_mode)
        return;
    char buf[256];
    shell_build_prompt(buf, sizeof(buf));
    printf("%s", buf);
    fflush(stdout);
}

// Poll for shell input (called from main loop): a line at a time through the
// statement assembler, with the continuation prompt "... " while a block is
// open.  getline, so a long line is one line.
static stmt_asm_t g_repl;

int shell_poll(void) {
    if (!stdin_has_data())
        return 0;

    static char *line = NULL;
    static size_t line_cap = 0;
    ssize_t n = getline(&line, &line_cap, stdin);
    if (n < 0)
        return 0;
    size_t len = (size_t)n;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    int r = stmt_feed_line(&g_repl, line, len);
    if (r < 0) {
        printf("error: statement too long (over %u bytes); discarded\n", STMT_MAX);
        return 1;
    }
    if (r == 0) {
        if (g_repl.len && !g_daemon_mode) {
            printf("... ");
            fflush(stdout);
        }
        return 1;
    }

    shell_dispatch(g_repl.buf);
    stmt_reset(&g_repl);
    return 1;
}

// Offer the ROM file's sibling *.vrom and *.prom files to the core's
// content-addressed registries.  The platform owns the filesystem: it names
// the directory; core walks it (offer_registry_add_dir), identifies each file
// by content and never fabricates a path itself.  This is what makes sibling
// card ROMs (e.g. the integration harness's tests/data/roms, reached via the
// absolute rom= path) discoverable without core knowing any directory.
static void offer_sibling_card_roms(const char *rom_path) {
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

int main(int argc, char *argv[]) {
    debug_set_failure_hook(headless_failure_hook);

    // Configuration from arguments
    const char *rom_file = NULL;
    const char *hd_files[8] = {NULL};
    int hd_count = 0;
    const char *cdrom_files[8] = {NULL}; // cdrom=<file> arguments
    int cdrom_count = 0;
    const char *fd_files[FLOPPY_NUM_DRIVES] = {NULL};
    int fd_count = 0;
    const char *fd_explicit[FLOPPY_NUM_DRIVES] = {NULL}; // fd0= and fd1= explicit drive assignments
    const char *script_file = NULL;
    const char *speed_mode = "paced";
    enum schedule_mode speed = schedule_paced;
    uint64_t max_cycles = 0;
    uint32_t ram_kb = 0;
    const char *model_override = NULL;
    const char *video_card_arg = NULL;
    const char *monitor_arg = NULL;
    int quiet = 0;
    int script_stdin = 0;
    int kill_daemon = 0;
    int no_prompt = 0;
    const char *checkpoint_dir = NULL; // explicit --checkpoint-dir=
    const char *var_defs[64] = {NULL}; // --var NAME=VALUE definitions
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
            g_daemon_mode = 1;
            quiet = 1; // suppress banner in daemon mode
            continue;
        }

        if (strcmp(arg, "--script-stdin") == 0) {
            script_stdin = 1;
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
            g_daemon_port = atoi(arg + 7);
            if (g_daemon_port <= 0 || g_daemon_port > 65535) {
                fprintf(stderr, "Error: Invalid port number: %s\n", arg + 7);
                return 1;
            }
            continue;
        }

        if (strncmp(arg, "--speed=", 8) == 0) {
            speed_mode = arg + 8;
            // An unknown mode is an error, not a silent paced run.
            if (!scheduler_mode_from_string(speed_mode, &speed)) {
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
            if (var_count < 64) {
                var_defs[var_count++] = def;
            } else {
                fprintf(stderr, "Warning: Too many --var definitions, ignoring: %s\n", def);
            }
            continue;
        }

        if ((value = parse_arg(arg, "rom")) != NULL) {
            rom_file = value;
            continue;
        }

        if ((value = parse_arg(arg, "hd")) != NULL) {
            if (hd_count < 8) {
                hd_files[hd_count++] = value;
            } else {
                fprintf(stderr, "Warning: Too many HD images, ignoring: %s\n", value);
            }
            continue;
        }

        if ((value = parse_arg(arg, "cdrom")) != NULL) {
            if (cdrom_count < 8) {
                cdrom_files[cdrom_count++] = value;
            } else {
                fprintf(stderr, "Warning: Too many CD-ROM images, ignoring: %s\n", value);
            }
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
            if (fd_count < FLOPPY_NUM_DRIVES) {
                fd_files[fd_count++] = value;
            } else {
                fprintf(stderr, "Warning: Too many FD images, ignoring: %s\n", value);
            }
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

    // Check ROM file exists
    FILE *f = fopen(rom_file, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open ROM file: %s\n", rom_file);
        return 1;
    }
    fclose(f);

    // Line-buffer stdout when output is redirected or piped
    if (script_file || !isatty(STDOUT_FILENO)) {
        setvbuf(stdout, NULL, _IOLBF, 0);
    }

    // Setup signal handlers
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigterm_handler);
    // Ignore SIGPIPE: in daemon mode stdout/stderr are dup2'd to the
    // client socket, so any printf after the client disconnects would
    // otherwise kill the daemon.  With SIG_IGN, write() returns -1 /
    // EPIPE, and daemon_client_gone (in pump_scheduler_with_heartbeat)
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

    // Initialize shell and emulator
    shell_init();

    // Between-statement scheduler pump for the script interpreter.
    script_set_pump_hook(headless_script_pump);

    // Apply --var definitions (after shell_init which calls shell_var_init)
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
    system_set_default_share(g_shared_dir); // core publishes it after each machine build

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
            return 1;
        }
        checkpoint_machine_set_root(checkpoint_dir);
    }

    // Probe the ROM to find compatible machines, then explicitly boot one.
    // ROM identity does not pick the machine — multiple Mac models share the
    // same ROM (Universal IIx/IIcx/SE/30), so the user picks via --model.
    rom_file_info_t rom_fi = {0};
    if (rom_probe_file(rom_file, &rom_fi) != 0 || !rom_fi.info) {
        fprintf(stderr, "Error: ROM file %s could not be identified\n", rom_file);
        return 1;
    }

    // Resolve target machine: --model overrides; else use the first entry in
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
            fprintf(stderr, "Error: --model %s is not compatible with this ROM (%s).\n", target_model,
                    rom_fi.info->family_name);
            fprintf(stderr, "Compatible models:");
            for (const char *const *p = rom_fi.info->compatible; *p; p++)
                fprintf(stderr, " %s", *p);
            fprintf(stderr, "\n");
            return 1;
        }
    }

    const hw_profile_t *profile = machine_find(target_model);
    if (!profile) {
        fprintf(stderr, "Error: machine model '%s' is not registered\n", target_model);
        return 1;
    }

    // Offer the ROM's sibling *.vrom files to the content-addressed registry
    // BEFORE the boot so the card factories can match them during machine
    // bring-up (offers persist across machine.boot).
    offer_sibling_card_roms(rom_file);

    // Startup is the same boot-document path scripts use (machine.boot):
    // CLI args fill the document, machine_boot_apply validates and
    // constructs, and the built-from record lets a later machine.restart
    // power-cycle this configuration.
    boot_config_t boot_doc = {
        .model = target_model,
        .ram_kb = ram_kb,
        .rom = rom_file,
        .video_card = video_card_arg,
        .monitor = monitor_arg,
        .video_sense = -1,
    };
    value_t boot_err = machine_boot_apply(&boot_doc);
    if (val_is_error(&boot_err)) {
        fprintf(stderr, "Error: %s\n", boot_err.err ? boot_err.err : "boot failed");
        value_free(&boot_err);
        return 1;
    }

    if (!global_emulator) {
        fprintf(stderr, "Error: Failed to initialize emulator\n");
        return 1;
    }

    // In daemon mode, stop the scheduler that se30_init/plus_init auto-started.
    // The agent will explicitly send "run" or "s" commands to control execution.
    if (g_daemon_mode) {
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
            return 1;
        }
        if (system_media_attach_path(global_emulator, &bays[i], false, hd_files[i], attach_err, sizeof(attach_err)) !=
            0) {
            fprintf(stderr, "Error: hd=%s: %s\n", hd_files[i], attach_err);
            return 1;
        }
        if (!quiet)
            printf("Attached HD[%d]: %s (%s)\n", i, hd_files[i], bays[i].label);
    }

    // The CD, into the model's CD bay (profile_cdrom_bay: its cdrom_id -- 3 on
    // every Macintosh, 0 on the Network Servers), on a model that has one.
    // There is one bay, so one cdrom=.
    if (cdrom_count > 1) {
        fprintf(stderr, "Error: %s has one CD-ROM bay; got %d cdrom= arguments\n", active->name, cdrom_count);
        return 1;
    }
    if (cdrom_count == 1) {
        media_bay_t cd;
        if (!profile_cdrom_bay(active, &cd)) {
            fprintf(stderr, "Error: %s has no CD-ROM bay for cdrom=%s\n", active->name, cdrom_files[0]);
            return 1;
        }
        if (system_media_attach_path(global_emulator, &cd, true, cdrom_files[0], attach_err, sizeof(attach_err)) != 0) {
            fprintf(stderr, "Error: cdrom=%s: %s\n", cdrom_files[0], attach_err);
            return 1;
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

    // Set scheduler pacing mode (validated at parse time, the same names
    // scheduler.mode takes).  Headless execution is budget-driven and never
    // consults the *pacing*; this keeps the flag surface consistent with the
    // WASM target.  'accelerated' does change execution — frame-units retire
    // more instructions at the lowered effective CPI; scheduler.speed picks
    // the multiplier.
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_set_mode(sched, speed);

    // Run startup script if provided
    if (script_file) {
        if (!quiet)
            printf("Running script: %s\n", script_file);
        run_script_file(script_file);
    }

    // Run commands from stdin if --script-stdin
    if (script_stdin) {
        run_script_stdin();
    }

    // Check if quit was requested during script execution
    if (quit_requested) {
        g_running = 0;
    }

    // ====================================================================
    // Daemon mode: listen on TCP socket for commands from AI agents
    // ====================================================================
    if (g_daemon_mode) {
        // Kill existing daemon on same port if --kill was specified
        if (kill_daemon)
            kill_existing_daemon(g_daemon_port);

        g_listen_fd = daemon_create_listener(g_daemon_port);
        if (g_listen_fd < 0) {
            fprintf(stderr, "Error: Failed to create daemon listener on port %d\n", g_daemon_port);
            system_destroy(global_emulator);
            global_emulator = NULL;
            return 1;
        }

        // Write PID file and register cleanup
        write_pid_file(g_daemon_port);
        atexit(remove_pid_file);
        fprintf(stderr, "Daemon PID: %d\n", getpid());

        // In daemon mode, do NOT auto-start the scheduler.
        // The agent sends "run" or "s" commands to control execution.
        daemon_loop();

        system_destroy(global_emulator);
        global_emulator = NULL;
        return headless_exit_code();
    }

    // ====================================================================
    // Interactive mode: normal REPL on stdin/stdout
    // ====================================================================

    // Start the emulator (only if not quitting)
    if (g_running) {
        if (!quiet)
            printf("\nStarting emulation (Ctrl+C to stop)...\n\n");

        scheduler_t *s = system_scheduler();
        if (s)
            scheduler_start(s);
    }

    // Main loop
    uint64_t start_cycles = cpu_instr_count();

    while (g_running && !quit_requested) {
        // Check for max cycles limit
        if (max_cycles > 0) {
            uint64_t elapsed_cycles = cpu_instr_count() - start_cycles;
            if (elapsed_cycles >= max_cycles) {
                if (!quiet)
                    printf("\nReached cycle limit (%llu cycles)\n", (unsigned long long)max_cycles);
                break;
            }
        }

        // Run emulation if scheduler is active
        scheduler_t *loop_sched = system_scheduler();
        if (loop_sched && global_emulator && scheduler_is_running(loop_sched)) {
            // Headless: run one VBL frame-unit per iteration (trigger_vbl + one
            // VBL-period run), as fast as the host allows — the same step web2's
            // RAF loop runs, just unthrottled.  Looping one frame at a time keeps
            // the REPL responsive to Ctrl+C (g_interrupted) and the max-cycles
            // cap above; a run_stop_event / scheduler_stop ends the run.
            scheduler_run_frame(loop_sched, global_emulator);
        } else {
            // Poll for shell input when idle
            shell_poll();
            usleep(10000); // 10ms sleep when idle
        }

        // Handle interrupt (Ctrl+C stops emulation but doesn't exit)
        if (g_interrupted) {
            g_interrupted = 0;
            printf("\n[Interrupted]\n");
            print_prompt();
        }
    }

    // Cleanup
    if (!quiet)
        printf("\nShutting down...\n");

    system_destroy(global_emulator);
    global_emulator = NULL;

    return headless_exit_code();
}
