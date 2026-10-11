// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// headless.h
// What headless_main.c (the loop, the REPL, startup) and headless_daemon.c
// (the TCP daemon) share.  Internal to the headless platform.

#ifndef HEADLESS_H
#define HEADLESS_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The mailbox clients this process posts statements as.
#define HL_CLIENT_STDIN  3u // the script file, --script-stdin and the REPL
#define HL_CLIENT_DAEMON 4u // a daemon connection

// The loop's flags.  Signal handlers only set g_running / g_interrupted; the
// loops act on them.  g_quit_requested is set by `quit` (platform_quit) and by the
// daemon's control connection.
extern volatile sig_atomic_t g_running; // cleared by SIGTERM: shut down
extern volatile sig_atomic_t g_interrupted; // set by SIGINT: Ctrl-C
extern volatile sig_atomic_t g_quit_requested; // `quit`

// One turn of the loop: a frame-unit if the machine runs, then the drain.
// Returns whether anything happened.
bool hl_pump_once(void);

// Runs one statement as a job for `client` and waits for its result, driving
// the loop meanwhile.  Returns 0 (ok), -1 (the script failed or was cancelled).
int hl_run_statement(uint32_t client, const char *src);

// === Statement assembler ===
// A statement is complete when it ends at a newline and is balanced
// (script_needs_continuation).  The daemon, stdin and the REPL all feed lines
// to one assembler and run each statement the moment it is complete.

#define STMT_MAX (1u << 20) // 1 MB

typedef struct stmt_asm {
    char *buf;
    size_t len, cap;
} stmt_asm_t;

// Feed one line (no newline).  1: a.buf holds a complete statement (run it,
// then stmt_reset); 0: more lines are needed; -1: the statement would pass
// STMT_MAX (it has been discarded).
int stmt_feed_line(stmt_asm_t *a, const char *line, size_t len);
void stmt_reset(stmt_asm_t *a);
void stmt_free(stmt_asm_t *a);

// === The daemon (headless_daemon.c) ===

#define DAEMON_DEFAULT_PORT 6800 // Motorola 68xx heritage

// Serve the shell on 127.0.0.1:`port` until quit or SIGTERM; `kill_existing`
// first stops a daemon already on that port.  Returns 0, or 1 when the
// listener cannot be created.
int daemon_run(int port, bool kill_existing);

// Called by hl_run_statement while a statement waits: notices a vanished
// client (and cancels its work) and answers control connections during a
// run.  A no-op unless a daemon client is connected.
void daemon_watch_statement(uint32_t client, bool running);

#endif // HEADLESS_H
