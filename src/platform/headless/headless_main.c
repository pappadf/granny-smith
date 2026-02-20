// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// headless_main.c
// Command-line entry point for running the emulator without a GUI.

#include "platform.h"

#include "cpu.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "machine.h"
#include "memory.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "system.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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
    printf("\n");
    printf("Examples:\n");
    printf("  %s rom=plus.rom\n", program);
    printf("  %s rom=plus.rom hd=disk.img\n", program);
    printf("  %s rom=plus.rom fd=system.dsk script=boot.sh\n", program);
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

// Check if stdin has data available (non-blocking)
static int stdin_has_data(void) {
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

// Shell prompt for headless mode
void print_prompt(void) {
    printf("gs> ");
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

    // Register headless-specific quit command
    register_cmd("quit", "General", "quit - exit the emulator", cmd_quit);

    setup_init();
    global_emulator = system_create(&machine_plus, NULL);

    if (!global_emulator) {
        fprintf(stderr, "Error: Failed to initialize emulator\n");
        return 1;
    }

    // Load ROM
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "load-rom %s", rom_file);
    shell_dispatch(cmd);

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

    // Check if quit was requested during script execution
    if (quit_requested) {
        g_running = 0;
    }

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
