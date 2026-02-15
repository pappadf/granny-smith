// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug.h
// Public interface for debugging, breakpoints, and tracing.

#ifndef DEBUG_H
#define DEBUG_H

// === Includes ===
#include "common.h"

#include <stdbool.h>
#include <stdint.h>

// === Forward Declarations ===
struct cpu;

struct breakpoint;
typedef struct breakpoint breakpoint_t;

struct logpoint;
typedef struct logpoint logpoint_t;

struct config;
typedef struct config config_t;

// === Constants ===
#define TRACE_ENTRY_PC  0
#define TRACE_ENTRY_LOG 1

// === Type Definitions ===

// Single trace entry: either a PC value or a log message index
typedef struct trace_entry {
    uint8_t type; // TRACE_ENTRY_PC or TRACE_ENTRY_LOG
    uint32_t value; // PC address or log message index
} trace_entry_t;

// Log message stored in trace log buffer
typedef struct trace_log_msg {
    char *text; // Log message text (owned)
} trace_log_msg_t;

// Debug state (exposed for performance-critical access)
struct debug {
    bool active;
    int step;
    breakpoint_t *breakpoints;
    uint32_t last_breakpoint_pc; // Track last breakpoint PC hit to skip it once when resuming
    logpoint_t *logpoints;
    // Trace buffer for PC entries
    uint32_t *trace_buffer;
    uint32_t trace_buffer_size;
    int trace_head;
    int trace_tail;
    int trace_size;
    // Trace log message buffer
    trace_log_msg_t *trace_log_buffer;
    uint32_t trace_log_buffer_size;
    uint32_t trace_log_head;
    uint32_t trace_log_count;
    // Combined trace entries (PC + log references)
    trace_entry_t *trace_entries;
    uint32_t trace_entries_size;
    uint32_t trace_entries_head;
    uint32_t trace_entries_tail;
    // Platform-specific assertion callback (e.g., for test integration)
    void (*assertion_callback)(const char *expr, const char *file, int line, const char *func);
};

struct debug;
typedef struct debug debug_t;

// === Lifecycle (Constructor / Destructor) ===

debug_t *debug_init(void);
void debug_cleanup(debug_t *debug);

// === Operations ===

breakpoint_t *debugger_set_breakpoint(debug_t *debug, uint32_t addr);

void debugger_disasm_pc(char *buf);

int debugger_disasm(char *buf, uint32_t addr);

int debug_break_and_trace(void);

void debug_print_target_trace(void);

void debug_trace_capture_log(const char *line);

int debug_trace_is_active(void);

bool debug_active(debug_t *debug);

#endif // DEBUG_H
