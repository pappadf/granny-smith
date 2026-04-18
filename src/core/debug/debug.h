// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug.h
// Public interface for debugging, breakpoints, and tracing.

#ifndef DEBUG_H
#define DEBUG_H

// === Includes ===
#include "addr_format.h"
#include "common.h"

#include <stdbool.h>
#include <stdint.h>

// === Forward Declarations ===
struct cpu;

struct breakpoint;
typedef struct breakpoint breakpoint_t;

struct logpoint;
typedef struct logpoint logpoint_t;

// Logpoint kind.  PC logpoints fire when the CPU executes at an address range
// (cheap; checked in the debug step hook).  Memory logpoints fire on reads or
// writes to a physical/logical address range (forces the page through the
// memory slow path).
enum logpoint_kind {
    LP_KIND_PC = 0, // fire on PC match
    LP_KIND_WRITE = 1, // fire on memory write
    LP_KIND_READ = 2, // fire on memory read
    LP_KIND_RW = 3, // fire on read or write
};

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

breakpoint_t *set_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space);

bool delete_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space);

void debugger_disasm_pc(char *buf);

int debugger_disasm(char *buf, uint32_t addr);

int debug_break_and_trace(void);

void debug_print_target_trace(void);

void debug_trace_capture_log(const char *line);

int debug_trace_is_active(void);

bool debug_active(debug_t *debug);

// Check if prompt/status line is enabled (IMP-308)
int debug_prompt_enabled(void);

// Set the default for prompt (used by --no-prompt CLI flag so that every
// subsequent shell connection inherits the setting without having to send
// "prompt off" first).
void debug_set_prompt_default(int enabled);

// Check and auto-delete temporary breakpoints (IMP-601)
void debug_check_tbreak(debug_t *debug, uint32_t pc);

// === Exception trace ring ===
// Records every CPU bus error / exception as a ring buffer entry.  The
// bus-error code paths in cpu_internal.h call exc_trace_record().  Enable
// streaming with `log exceptions 1`, dump the ring with `info exceptions`.
typedef struct exc_trace_entry {
    uint64_t ts; // CPU instruction count at the exception
    uint32_t faulting_pc; // cpu->instruction_pc when the fault occurred
    uint32_t saved_pc; // PC stacked (differs from faulting_pc on retry vs skip)
    uint32_t fault_addr; // faulting address written to stack frame
    uint32_t vbr;
    uint32_t vector; // exception vector (0x008 = bus error, etc.)
    uint16_t sr;
    uint16_t format_frame; // 0xA/0xB/0x0/0x2
    uint8_t rw; // 1=read, 0=write
    uint8_t double_fault_kind; // 0 = none, 1 = retry double-fault detected
} exc_trace_entry_t;

// Record one exception event (called from cpu_internal.h exception paths)
void exc_trace_record(uint32_t vector, uint32_t faulting_pc, uint32_t saved_pc, uint32_t fault_addr, uint32_t rw,
                      uint32_t vbr, uint16_t sr, uint16_t format_frame, int double_fault_kind);

#endif // DEBUG_H
