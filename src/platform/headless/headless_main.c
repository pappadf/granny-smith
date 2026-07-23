// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// headless_main.c
// Command-line entry point for running the emulator without a GUI.

#include "platform.h"

#include "checkpoint_machine.h"
#include "cpu.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "machine.h"
#include "machine_config.h"
#include "memory.h"
#include "nubus.h"
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
#include <termios.h>
#include <unistd.h>

// Platform stubs for functions defined in WASM but needed by core

// Video force redraw - no-op in headless
void frontend_force_redraw(void) {
    // No video in headless mode
}

// Headless uses the default (no-op) system_post_create from system.c.  VBL is
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

// PID file path for daemon mode (IMP-103)
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

// Kill existing daemon on the given port (IMP-103)
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
    printf("  hd=<file>       Hard disk image file (optional, can specify multiple)\n");
    printf("  cdrom=<file>    CD-ROM image file (optional, SCSI ID 3+)\n");
    printf("  fd=<file>       Floppy disk image file (optional, can specify multiple)\n");
    printf("  fd0=<file>      Floppy disk image for drive 0 (internal)\n");
    printf("  fd1=<file>      Floppy disk image for drive 1 (external)\n");
    printf("  video_card=<id> NuBus video card for the configurable slot (e.g. 824gc);\n");
    printf("                  default: the machine's default card\n");
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

// Quit flag for headless mode - set by quit command
static volatile int quit_requested = 0;

// Platform impl of gs_quit (weak default in system.c is a no-op).
void gs_quit(void) {
    quit_requested = 1;
    // Stop scheduler to break out of any running emulation
    scheduler_t *sched = system_scheduler();
    if (sched) {
        scheduler_stop(sched);
    }
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

// Check if the current daemon client has disconnected or sent new data.
// Returns: 0 = still alive, nothing pending
//          1 = new data pending (new command issued — e.g. "stop")
//         -1 = client disconnected
static int daemon_client_poll(int client_fd) {
    if (client_fd < 0)
        return 0;
    struct pollfd pfd = {.fd = client_fd, .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 0) <= 0)
        return 0;
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
        return -1;
    if (pfd.revents & POLLIN) {
        // Peek without consuming — if it's EOF (recv returns 0), client closed
        char peek;
        ssize_t n = recv(client_fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0)
            return -1;
        if (n > 0)
            return 1;
    }
    return 0;
}

// Pump the scheduler until it stops, emitting periodic heartbeat lines (IMP-105).
// In daemon mode the heartbeat prevents nc -w timeouts; in script/stdin modes it
// lets callers follow progress. Emits once per second with instruction count.
// Also: if the daemon client disconnects mid-run, stop the scheduler — a dead
// client has no way to see the result, and letting it run billions more
// instructions is wasteful.  If the client sends any data mid-run (e.g. a
// "stop" command), stop immediately so the next dispatch reads it.
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
    // differs).  An instruction-budget `scheduler.run N` schedules a
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

        // Daemon mode: check if the client disconnected or sent new data
        if (g_daemon_mode && g_client_fd >= 0) {
            int s = daemon_client_poll(g_client_fd);
            if (s < 0) {
                // Client gone — cancel the run rather than execute billions more
                fprintf(stderr, "# client disconnected, stopping run\n");
                scheduler_stop(sched);
                break;
            }
            if (s > 0) {
                // New data arrived (likely "stop") — break out so the caller
                // reads and dispatches it.  We don't process the data here.
                scheduler_stop(sched);
                break;
            }
        }
    }
}

// Handle a single client connection: read commands, execute, write output, close
// Supports multiple newline-delimited commands per connection (IMP-104)
static void daemon_handle_client(int client_fd) {
    g_client_fd = client_fd;

    // Read all available data from client (may contain multiple commands)
    char buf[2048];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = read(client_fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0)
            break;
        total += n;
        // Stop if we've seen at least one newline and no more data pending
        if (memchr(buf, '\n', total)) {
            // Check if more data is available
            fd_set fds;
            struct timeval tv = {0, 1000}; // 1ms
            FD_ZERO(&fds);
            FD_SET(client_fd, &fds);
            if (select(client_fd + 1, &fds, NULL, NULL, &tv) <= 0)
                break;
        }
    }

    if (total <= 0) {
        close(client_fd);
        g_client_fd = -1;
        return;
    }

    buf[total] = '\0';

    // Redirect output to the client socket
    daemon_redirect_output(client_fd);

    // Process each newline-delimited command (IMP-104). Lines
    // accumulate while a multi-line block is open (unbalanced '{').
    char acc[4096];
    size_t acc_len = 0;
    char *line = buf;
    while (line && *line && !quit_requested) {
        // Find end of current command
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = '\0';

        // Strip trailing carriage return
        size_t len = strlen(line);
        while (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';

        // Accumulate into the continuation buffer.
        if (len > 0 && acc_len + len + 2 < sizeof(acc)) {
            memcpy(acc + acc_len, line, len);
            acc_len += len;
            acc[acc_len++] = '\n';
            acc[acc_len] = '\0';
        }

        // Skip empty buffers / open blocks
        if (acc_len > 0 && !script_needs_continuation(acc)) {
            // Execute the accumulated chunk
            shell_dispatch(acc);
            acc_len = 0;
            acc[0] = '\0';

            // If the scheduler is running after the command, pump until done
            // with periodic heartbeat to prevent TCP client timeouts (IMP-105)
            pump_scheduler_with_heartbeat();

            // Emit current instruction as a status line (IMP-308)
            if (system_is_initialized() && debug_prompt_enabled()) {
                char disasm_buf[160];
                debugger_disasm_pc(disasm_buf, sizeof(disasm_buf));
                if (disasm_buf[0] != '\0')
                    printf("%s\n", disasm_buf);
            }
        }

        // Move to next command
        line = eol ? eol + 1 : NULL;
    }

    // Flush and restore output before closing the connection
    daemon_restore_output();

    close(client_fd);
    g_client_fd = -1;
}

// Main daemon loop: accept connections and handle them one at a time
static void daemon_loop(void) {
    fprintf(stderr, "Daemon listening on 127.0.0.1:%d\n", g_daemon_port);
    fprintf(stderr, "Send commands with: echo \"command\" | nc localhost %d\n", g_daemon_port);

    // Emit READY signal so callers can block-read instead of sleeping (IMP-101)
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
// the scheduler to completion (heartbeat included, IMP-105) and report
// whether a quit was requested so the interpreter can stop cleanly.
static bool headless_script_pump(void) {
    pump_scheduler_with_heartbeat();
    return quit_requested != 0;
}

// Run a script file through the v2 interpreter. The whole file is
// parsed up front (multi-line blocks need the full source), then
// executed; the pump hook drives the scheduler between statements.
static int run_script_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Error: Cannot open script file: %s\n", filename);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0)
        size = 0;
    char *src = malloc((size_t)size + 1);
    if (!src) {
        fclose(f);
        fprintf(stderr, "Error: out of memory reading script: %s\n", filename);
        return -1;
    }
    size_t got = fread(src, 1, (size_t)size, f);
    src[got] = '\0';
    fclose(f);

    printf("> running %s\n", filename);
    int result = script_run_source(src);
    free(src);
    if (result != 0) {
        // v2 scripts abort on the first error (§3.9); a script that
        // aborted never reaches its `quit`, so exit here instead of
        // dropping into the interactive loop.
        g_script_exit_code = 1;
        quit_requested = 1;
    }
    return 0;
}

// Run commands from stdin (IMP-803). Lines accumulate while a
// multi-line block is open (unbalanced '{'), then submit as one chunk.
static int run_script_stdin(void) {
    char line[1024];
    char buf[8192];
    size_t buf_len = 0;
    char last_cmd[1024] = {0};
    while (fgets(line, sizeof(line), stdin)) {
        // Strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        // Skip comments
        if (line[0] == '#')
            continue;

        // Empty line repeats last command (IMP-806) — only outside a
        // continuation.
        if (len == 0 && buf_len == 0) {
            if (last_cmd[0] == '\0')
                continue;
            strncpy(line, last_cmd, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            len = strlen(line);
        } else if (buf_len == 0) {
            strncpy(last_cmd, line, sizeof(last_cmd) - 1);
            last_cmd[sizeof(last_cmd) - 1] = '\0';
        }

        // Accumulate into the continuation buffer.
        if (buf_len + len + 2 < sizeof(buf)) {
            memcpy(buf + buf_len, line, len);
            buf_len += len;
            buf[buf_len++] = '\n';
            buf[buf_len] = '\0';
        }
        if (script_needs_continuation(buf))
            continue;

        printf("> %s\n", buf);
        int result = script_run_line(buf); // interactive: results print
        buf_len = 0;
        buf[0] = '\0';

        // Non-zero return code indicates error
        if (result != 0) {
            g_script_exit_code = 1;
        }

        // Check if quit was requested
        if (quit_requested)
            break;
    }
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

// Shell prompt — shows disassembled current instruction (like the web shell)
void print_prompt(void) {
    if (g_daemon_mode)
        return;
    if (system_is_initialized()) {
        char disasm_buf[160];
        debugger_disasm_pc(disasm_buf, sizeof(disasm_buf));
        if (disasm_buf[0] != '\0')
            printf("%s > ", disasm_buf);
        else
            printf("gs> ");
    } else {
        printf("gs> ");
    }
    fflush(stdout);
}

// Poll for shell input (called from main loop). Lines accumulate while
// a multi-line block is open (continuation prompt "... ").
static char g_repl_buf[8192];
static size_t g_repl_len = 0;

bool shell_repl_continuing(void) {
    return g_repl_len > 0;
}

int shell_poll(void) {
    if (!stdin_has_data())
        return 0;

    char line[1024];
    if (fgets(line, sizeof(line), stdin) == NULL)
        return 0;

    // Strip trailing newline
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    if (len == 0 && g_repl_len == 0)
        return 1;

    if (g_repl_len + len + 2 < sizeof(g_repl_buf)) {
        memcpy(g_repl_buf + g_repl_len, line, len);
        g_repl_len += len;
        g_repl_buf[g_repl_len++] = '\n';
        g_repl_buf[g_repl_len] = '\0';
    }
    if (script_needs_continuation(g_repl_buf)) {
        if (!g_daemon_mode) {
            printf("... ");
            fflush(stdout);
        }
        return 1;
    }

    shell_dispatch(g_repl_buf);
    g_repl_len = 0;
    g_repl_buf[0] = '\0';

    return 1;
}

// Offer every *.vrom file in the ROM file's directory to the core's
// content-addressed vROM registry.  The platform owns the filesystem: it
// enumerates candidates and offers their paths; core identifies each by
// content and never fabricates a path itself.  This is what makes sibling
// vROM files (e.g. the integration harness's tests/data/roms, reached via
// the absolute rom= path) discoverable without core knowing any directory.
static void offer_sibling_vroms(const char *rom_path) {
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
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *entry;
    char path[1200];
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        // Only *.vrom candidates — the offer itself content-verifies them.
        if (len < 6 || strcmp(name + len - 5, ".vrom") != 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
            continue;
        vrom_offer(path);
    }
    closedir(d);
}

int main(int argc, char *argv[]) {
    // Configuration from arguments
    const char *rom_file = NULL;
    const char *hd_files[8] = {NULL};
    int hd_count = 0;
    const char *cdrom_files[8] = {NULL}; // cdrom=<file> arguments
    int cdrom_count = 0;
    const char *fd_files[2] = {NULL};
    int fd_count = 0;
    const char *fd_explicit[2] = {NULL}; // fd0= and fd1= explicit drive assignments
    const char *script_file = NULL;
    const char *speed_mode = "paced";
    uint64_t max_cycles = 0;
    uint32_t ram_kb = 0;
    const char *model_override = NULL;
    const char *video_card_arg = NULL;
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
            continue;
        }

        if (strncmp(arg, "--cycles=", 9) == 0) {
            max_cycles = strtoull(arg + 9, NULL, 10);
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
            if (fd_count < 2) {
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

    // Line-buffer stdout when output is redirected or piped (IMP-107)
    if (script_file || !isatty(STDOUT_FILENO)) {
        setvbuf(stdout, NULL, _IOLBF, 0);
    }

    // Setup signal handlers
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigterm_handler);
    // Ignore SIGPIPE: in daemon mode stdout/stderr are dup2'd to the
    // client socket, so any printf after the client disconnects would
    // otherwise kill the daemon.  With SIG_IGN, write() returns -1 /
    // EPIPE and the existing daemon_client_poll disconnect detection
    // (in pump_scheduler_with_heartbeat) handles the rest gracefully.
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

    // If a --checkpoint-dir was given, point the machine layer at it
    // verbatim so writable image deltas land there.  No id/timestamp
    // suffix — headless callers manage the directory themselves.
    if (checkpoint_dir && *checkpoint_dir) {
        if (checkpoint_machine_set_dir(checkpoint_dir) != 0) {
            fprintf(stderr, "Error: cannot create --checkpoint-dir %s: %s\n", checkpoint_dir, strerror(errno));
            return 1;
        }
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
    offer_sibling_vroms(rom_file);

    // Startup is the same boot-document path scripts use (machine.boot):
    // CLI args fill the document, machine_boot_apply validates and
    // constructs, and the built-from record makes later argument-less
    // `machine.boot` reboots inherit this configuration.
    boot_config_t boot_doc = {
        .model = target_model,
        .ram_kb = ram_kb,
        .rom = rom_file,
        .video_card = video_card_arg,
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

    // VBL was already armed by the system_post_create override above —
    // it ran from inside system_create, which the rom_load path or the
    // earlier system_create call has already kicked.

    // In daemon mode, stop the scheduler that se30_init/plus_init auto-started.
    // The agent will explicitly send "run" or "s" commands to control execution.
    if (g_daemon_mode) {
        scheduler_t *s = system_scheduler();
        if (s)
            scheduler_stop(s);
    }

    // Attach hard disk images
    for (int i = 0; i < hd_count; i++) {
        add_scsi_drive(global_emulator, hd_files[i], i);
        if (!quiet)
            printf("Attached HD[%d]: %s\n", i, hd_files[i]);
    }

    // Attach CD-ROM images (default SCSI ID starts at 3)
    for (int i = 0; i < cdrom_count; i++) {
        int cdrom_id = 3 + i; // default SCSI IDs 3, 4, 5, ...
        add_scsi_cdrom(global_emulator, cdrom_files[i], cdrom_id);
        if (!quiet)
            printf("Attached CD-ROM[%d]: %s (SCSI ID %d)\n", i, cdrom_files[i], cdrom_id);
    }

    // Insert explicit fd0=/fd1= floppy images into their designated drives
    for (int i = 0; i < 2; i++) {
        if (!fd_explicit[i])
            continue;
        char drive_str[8];
        snprintf(drive_str, sizeof(drive_str), "%d", i);
        char *fake_argv[] = {"fd", "insert", (char *)fd_explicit[i], drive_str, "true"};
        int rc = shell_fd_argv(5, fake_argv);
        if (rc == 0) {
            if (!quiet)
                printf("Inserted FD[%d]: %s\n", i, fd_explicit[i]);
        } else {
            fprintf(stderr, "Warning: Cannot open floppy image for drive %d: %s\n", i, fd_explicit[i]);
        }
    }

    // Insert sequential fd= floppy images into first available drives
    for (int i = 0; i < fd_count; i++) {
        char *fake_argv[] = {"fd", "insert", (char *)fd_files[i]};
        int rc = shell_fd_argv(3, fake_argv);
        if (rc == 0) {
            if (!quiet)
                printf("Inserted FD[%d]: %s\n", i, fd_files[i]);
        } else {
            fprintf(stderr, "Warning: Cannot open floppy image: %s\n", fd_files[i]);
        }
    }

    // Set scheduler pacing mode. Legacy three-mode names stay accepted:
    // realtime/hardware → paced, max → turbo. (Headless execution is
    // budget-driven and never consults the *pacing*; this keeps the flag
    // surface consistent with the WASM target. 'accelerated' does change
    // execution — frame-units retire more instructions at the lowered
    // effective CPI; scheduler.speed picks the multiplier.)
    scheduler_t *sched = system_scheduler();
    if (sched) {
        if (strcmp(speed_mode, "turbo") == 0 || strcmp(speed_mode, "max") == 0) {
            scheduler_set_mode(sched, schedule_unthrottled);
        } else if (strcmp(speed_mode, "accelerated") == 0 || strcmp(speed_mode, "accel") == 0) {
            scheduler_set_mode(sched, schedule_accelerated);
        } else if (strcmp(speed_mode, "paced") == 0 || strcmp(speed_mode, "realtime") == 0 ||
                   strcmp(speed_mode, "real") == 0 || strcmp(speed_mode, "hardware") == 0 ||
                   strcmp(speed_mode, "hw") == 0) {
            scheduler_set_mode(sched, schedule_paced);
        }
    }

    // Run startup script if provided
    if (script_file) {
        if (!quiet)
            printf("Running script: %s\n", script_file);
        run_script_file(script_file);
    }

    // Run commands from stdin if --script-stdin (IMP-803)
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
        // Kill existing daemon on same port if --kill was specified (IMP-103)
        if (kill_daemon)
            kill_existing_daemon(g_daemon_port);

        g_listen_fd = daemon_create_listener(g_daemon_port);
        if (g_listen_fd < 0) {
            fprintf(stderr, "Error: Failed to create daemon listener on port %d\n", g_daemon_port);
            system_destroy(global_emulator);
            global_emulator = NULL;
            return 1;
        }

        // Write PID file and register cleanup (IMP-103)
        write_pid_file(g_daemon_port);
        atexit(remove_pid_file);
        fprintf(stderr, "Daemon PID: %d\n", getpid());

        // In daemon mode, do NOT auto-start the scheduler.
        // The agent sends "run" or "s" commands to control execution.
        daemon_loop();

        system_destroy(global_emulator);
        global_emulator = NULL;
        return g_script_exit_code;
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
    double last_time = host_time();
    uint64_t start_cycles = cpu_instr_count();

    while (g_running && !quit_requested) {
        double now = host_time();

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
            (void)now;
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

        last_time = now;
    }

    // Cleanup
    if (!quiet)
        printf("\nShutting down...\n");

    system_destroy(global_emulator);
    global_emulator = NULL;

    return g_script_exit_code;
}
