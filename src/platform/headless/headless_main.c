// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// headless_main.c
// Command-line entry point for running the emulator without a GUI.

#include "platform.h"

#include "cpu.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "machine.h"
#include "memory.h"
#include "rom.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "system.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
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
    printf("Usage: %s rom=<file> [hd=<file>] [fd=<file>] [script=<file>]\n", program);
    printf("\n");
    printf("Arguments:\n");
    printf("  rom=<file>      ROM image file (required)\n");
    printf("  hd=<file>       Hard disk image file (optional, can specify multiple)\n");
    printf("  fd=<file>       Floppy disk image file (optional, can specify multiple)\n");
    printf("  script=<file>   Shell script file to execute at startup (optional)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help, -h      Display this help message\n");
    printf("  --speed=MODE    Scheduler mode: max, realtime, hardware (default: realtime)\n");
    printf("  --cycles=N      Run for N CPU cycles then exit (for testing)\n");
    printf("  --quiet, -q     Suppress startup messages\n");
    printf("  --daemon        Start in daemon mode (TCP socket interface for AI agents)\n");
    printf("  --port=PORT     TCP port for daemon mode (default: 6800)\n");
    printf("  --kill          Kill existing daemon on same port before starting\n");
    printf("  --script-stdin  Read script commands from stdin instead of a file\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s rom=plus.rom\n", program);
    printf("  %s rom=plus.rom hd=disk.img\n", program);
    printf("  %s rom=plus.rom fd=system.dsk script=boot.sh\n", program);
    printf("  %s --daemon rom=SE30.rom\n", program);
    printf("  %s --daemon --port=7000 rom=SE30.rom hd=disk.img\n", program);
}

// Global script exit code (set by commands like screenshot --match)
static int g_script_exit_code = 0;

// Quit flag for headless mode - set by quit command
static volatile int quit_requested = 0;

// Quit command - headless-specific
static uint64_t cmd_quit(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    quit_requested = 1;
    // Stop scheduler to break out of any running emulation
    scheduler_t *sched = system_scheduler();
    if (sched) {
        scheduler_stop(sched);
    }
    return 0;
}

// Forward declaration for run_script_file
static int run_script_file(const char *filename);

// Script command — execute a script file inline (IMP-802)
static uint64_t cmd_script(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: script <path>\n");
        return (uint64_t)-1;
    }
    return (uint64_t)run_script_file(argv[1]);
}

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

    // Process each newline-delimited command (IMP-104)
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

        // Skip empty lines
        if (len > 0) {
            // Execute the command
            shell_dispatch(line);

            // If the scheduler is running after the command, pump until done
            scheduler_t *sched = system_scheduler();
            while (sched && scheduler_is_running(sched) && !quit_requested) {
                double now = host_time();
                scheduler_main_loop(global_emulator, now * 1000.0);
            }

            // Emit current instruction as a status line (IMP-308)
            if (system_is_initialized() && debug_prompt_enabled()) {
                char disasm_buf[100];
                debugger_disasm_pc(disasm_buf);
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

// Run commands from a script file
static int run_script_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Error: Cannot open script file: %s\n", filename);
        return -1;
    }

    char line[1024];
    int line_num = 0;
    while (fgets(line, sizeof(line), f)) {
        line_num++;

        // Strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        // Skip empty lines and comments
        if (len == 0 || line[0] == '#')
            continue;

        // Handle include directive (IMP-804)
        if (strncmp(line, "include ", 8) == 0) {
            const char *inc_path = line + 8;
            // Skip leading whitespace
            while (*inc_path == ' ' || *inc_path == '\t')
                inc_path++;
            if (*inc_path) {
                printf("> include %s\n", inc_path);
                run_script_file(inc_path);
                if (quit_requested)
                    break;
                continue;
            }
        }

        // Execute the command and capture return code
        printf("> %s\n", line);
        int result = shell_dispatch(line);

        // Non-zero return code indicates error (e.g., screenshot --match mismatch)
        if (result != 0) {
            g_script_exit_code = result;
        }

        // If the command started the scheduler (e.g., "run"), pump the main loop
        // until execution stops. This mimics em_main.c's behavior.
        scheduler_t *sched = system_scheduler();
        while (sched && scheduler_is_running(sched) && !quit_requested) {
            double now = host_time();
            scheduler_main_loop(global_emulator, now * 1000.0); // Convert seconds to milliseconds
        }

        // Check if quit was requested
        if (quit_requested) {
            break;
        }
    }

    fclose(f);
    return 0;
}

// Run commands from stdin (IMP-803)
static int run_script_stdin(void) {
    char line[1024];
    char last_cmd[1024] = {0};
    while (fgets(line, sizeof(line), stdin)) {
        // Strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        // Skip comments
        if (line[0] == '#')
            continue;

        // Empty line repeats last command (IMP-806)
        if (len == 0) {
            if (last_cmd[0] == '\0')
                continue;
            strncpy(line, last_cmd, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
        } else {
            strncpy(last_cmd, line, sizeof(last_cmd) - 1);
            last_cmd[sizeof(last_cmd) - 1] = '\0';
        }

        // Execute the command and capture return code
        printf("> %s\n", line);
        int result = shell_dispatch(line);

        // Non-zero return code indicates error
        if (result != 0) {
            g_script_exit_code = result;
        }

        // Pump scheduler if command started it
        scheduler_t *sched = system_scheduler();
        while (sched && scheduler_is_running(sched) && !quit_requested) {
            double now = host_time();
            scheduler_main_loop(global_emulator, now * 1000.0);
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
        char disasm_buf[100];
        debugger_disasm_pc(disasm_buf);
        if (disasm_buf[0] != '\0')
            printf("%s > ", disasm_buf);
        else
            printf("gs> ");
    } else {
        printf("gs> ");
    }
    fflush(stdout);
}

// Poll for shell input (called from main loop)
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

    if (len > 0) {
        shell_dispatch(line);
    }

    return 1;
}

int main(int argc, char *argv[]) {
    // Configuration from arguments
    const char *rom_file = NULL;
    const char *hd_files[8] = {NULL};
    int hd_count = 0;
    const char *fd_files[2] = {NULL};
    int fd_count = 0;
    const char *script_file = NULL;
    const char *speed_mode = "realtime";
    uint64_t max_cycles = 0;
    int quiet = 0;
    int script_stdin = 0;
    int kill_daemon = 0;

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

        if ((value = parse_arg(arg, "fd")) != NULL) {
            if (fd_count < 2) {
                fd_files[fd_count++] = value;
            } else {
                fprintf(stderr, "Warning: Too many FD images, ignoring: %s\n", value);
            }
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

    if (!quiet) {
        printf("Granny Smith - Headless Macintosh Emulator\n");
        printf("==========================================\n");
        printf("ROM:    %s\n", rom_file);
        for (int i = 0; i < hd_count; i++)
            printf("HD[%d]:  %s\n", i, hd_files[i]);
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

    // Register headless-specific commands
    register_cmd("quit", "General", "quit — exit the emulator", cmd_quit);
    register_cmd("script", "General", "script <path> — execute a script file", cmd_script);

    setup_init();

    // Use load-rom to identify the ROM and create the appropriate machine.
    // load-rom reads the ROM file, determines the machine type from the checksum,
    // calls system_create() internally, and loads the ROM into machine memory.
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "load-rom %s", rom_file);
    shell_dispatch(cmd);

    if (!global_emulator) {
        fprintf(stderr, "Error: Failed to initialize emulator (ROM identification failed)\n");
        return 1;
    }

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

    // Insert floppy disk images via shell command
    for (int i = 0; i < fd_count; i++) {
        snprintf(cmd, sizeof(cmd), "insert-disk %s", fd_files[i]);
        int rc = shell_dispatch(cmd);
        if (rc == 0) {
            if (!quiet)
                printf("Inserted FD[%d]: %s\n", i, fd_files[i]);
        } else {
            fprintf(stderr, "Warning: Cannot open floppy image: %s\n", fd_files[i]);
        }
    }

    // Set scheduler mode
    scheduler_t *sched = system_scheduler();
    if (sched) {
        if (strcmp(speed_mode, "max") == 0) {
            scheduler_set_mode(sched, schedule_max_speed);
        } else if (strcmp(speed_mode, "realtime") == 0 || strcmp(speed_mode, "real") == 0) {
            scheduler_set_mode(sched, schedule_real_time);
        } else if (strcmp(speed_mode, "hardware") == 0 || strcmp(speed_mode, "hw") == 0) {
            scheduler_set_mode(sched, schedule_hw_accuracy);
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
        if (loop_sched && scheduler_is_running(loop_sched)) {
            scheduler_main_loop(global_emulator, now * 1000.0); // Convert seconds to milliseconds
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
