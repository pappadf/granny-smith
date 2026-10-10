// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// log_context.c
// The logger's view of the running machine, supplied through
// log_set_context_hooks so that log.c itself depends on nothing but libc.
// The decorations describe the running machine, which a build -- a log line
// from a constructor -- leaves as it is (system_running).

#include "log_context.h"

#include "cpu.h" // cpu_get_pc
#include "debug.h" // debug_trace_is_active / debug_trace_capture_log
#include "log.h"
#include "ppc.h" // PowerPC pc / r24 for the PC decoration
#include "scheduler.h" // scheduler_instr_count
#include "system.h" // system_running
#include "system_internal.h" // config_t::cpu / ppc / scheduler

#include <stdio.h>

// The running machine's executed-instruction count (0 when none is running)
static unsigned long long ctx_instr_count(void) {
    config_t *running = system_running();
    return (unsigned long long)scheduler_instr_count(running ? running->scheduler : NULL);
}

// Writes the PC decoration.  On a PowerPC machine the interesting "PC" for
// driver-level logs is usually the emulated 68k one, which the ROM's
// emulator keeps in r24 while 68k code runs -- show both.
static void ctx_format_pc(char *buf, size_t size) {
    config_t *running = system_running();
    if (running && running->ppc) {
        snprintf(buf, size, "PC=%08x r24=%08x", (unsigned)ppc_get_pc(running->ppc),
                 (unsigned)ppc_get_gpr(running->ppc, 24));
        return;
    }
    uint32_t pc = (running && running->cpu) ? cpu_get_pc(running->cpu) : 0;
    snprintf(buf, size, "PC=%08x", (unsigned)pc);
}

// Hands each emitted line to the debug trace while it is recording
static void ctx_observe_line(const char *line) {
    if (debug_trace_is_active())
        debug_trace_capture_log(line);
}

// The hook table (static storage: the logger keeps the pointer)
static const log_context_hooks_t k_hooks = {
    .instr_count = ctx_instr_count,
    .format_pc = ctx_format_pc,
    .observe_line = ctx_observe_line,
};

void log_context_install(void) {
    log_set_context_hooks(&k_hooks);
}
