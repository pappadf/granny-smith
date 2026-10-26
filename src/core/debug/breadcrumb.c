// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// breadcrumb.c
// The emulator thread's breadcrumbs (breadcrumb.h).

#include "breadcrumb.h"

#include "cpu.h" // cpu_get_pc
#include "platform_clock.h" // host_time_ms
#include "ppc.h" // ppc_get_pc / ppc_get_gpr
#include "system_internal.h" // config_t::cpu / ppc

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h> // emscripten_date_now
#endif

static volatile uint32_t g_crumb[CRUMB_WORDS] = {
    [CRUMB_MAGIC] = CRUMB_MAGIC_VALUE, [CRUMB_VERSION] = CRUMB_VERSION_VALUE};
static double g_wait_ms; // CRUMB_GPU_WAIT_MS, unrounded

static const char *const k_names[CRUMB_PHASES] = {
    [CRUMB_EVENT_LOOP] = "event-loop",
    [CRUMB_TICK] = "tick",
    [CRUMB_RUN] = "run",
    [CRUMB_VIDEO] = "video",
    [CRUMB_REQUESTS] = "requests",
    [CRUMB_IDLE] = "idle",
    [CRUMB_GPU_RING] = "gpu-ring",
    [CRUMB_GPU_ACK] = "gpu-ack",
    [CRUMB_GPU_PACE] = "gpu-pace",
    [CRUMB_JIT_INSTALL] = "jit-install",
    [CRUMB_CHECKPOINT] = "checkpoint",
    [CRUMB_WATCH] = "watch",
};

// The block's times are wall-clock milliseconds (low 32 bits): another
// thread reads them against its own clock, and Date.now() is the one clock
// the page and the emulator's worker share
static inline uint32_t now32(void) {
#ifdef __EMSCRIPTEN__
    return (uint32_t)(uint64_t)emscripten_date_now();
#else
    return (uint32_t)(uint64_t)host_time_ms();
#endif
}

volatile uint32_t *crumb_block(void) {
    return g_crumb;
}

const char *crumb_phase_name(crumb_phase_t phase) {
    return phase < CRUMB_PHASES && k_names[phase] ? k_names[phase] : "?";
}

crumb_phase_t crumb_enter(crumb_phase_t phase) {
    crumb_phase_t previous = (crumb_phase_t)g_crumb[CRUMB_PHASE];
    g_crumb[CRUMB_OUTER] = previous;
    g_crumb[CRUMB_PHASE_MS] = now32();
    g_crumb[CRUMB_PHASE] = phase;
    g_crumb[CRUMB_SEQ]++;
    return previous;
}

void crumb_leave(crumb_phase_t previous) {
    g_crumb[CRUMB_OUTER] = CRUMB_EVENT_LOOP;
    g_crumb[CRUMB_PHASE_MS] = now32();
    g_crumb[CRUMB_PHASE] = previous;
    g_crumb[CRUMB_SEQ]++;
}

void crumb_tick_begin(uint32_t vbls, double host_us_per_vbl, double host_us_per_loop, uint32_t mode) {
    g_crumb[CRUMB_TICKS]++;
    g_crumb[CRUMB_TICK_MS] = now32();
    g_crumb[CRUMB_TICK_VBLS] = vbls;
    g_crumb[CRUMB_TICK_VBL] = 0;
    g_crumb[CRUMB_HOST_US_PER_VBL] = host_us_per_vbl > 0 && host_us_per_vbl < 4e9 ? (uint32_t)host_us_per_vbl : 0;
    g_crumb[CRUMB_HOST_US_PER_LOOP] = host_us_per_loop > 0 && host_us_per_loop < 4e9 ? (uint32_t)host_us_per_loop : 0;
    g_crumb[CRUMB_MODE] = mode;
}

void crumb_vbl_done(uint64_t instr, uint32_t pc, uint32_t r24) {
    g_crumb[CRUMB_TICK_VBL]++;
    g_crumb[CRUMB_INSTR_LO] = (uint32_t)instr;
    g_crumb[CRUMB_INSTR_HI] = (uint32_t)(instr >> 32);
    g_crumb[CRUMB_PC] = pc;
    g_crumb[CRUMB_R24] = r24;
    g_crumb[CRUMB_VBL_MS] = now32();
}

void crumb_set(int word, uint32_t value) {
    if (word >= 0 && word < CRUMB_WORDS)
        g_crumb[word] = value;
}

void crumb_gpu_wait(double ms) {
    g_crumb[CRUMB_GPU_WAITS]++;
    g_wait_ms += ms;
    g_crumb[CRUMB_GPU_WAIT_MS] = (uint32_t)(uint64_t)g_wait_ms;
}

void crumb_after_vbl(struct config *m, uint64_t instr) {
    uint32_t pc = 0, r24 = 0;
    if (m && m->ppc) {
        pc = ppc_get_pc(m->ppc);
        r24 = ppc_get_gpr(m->ppc, 24);
    } else if (m && m->cpu)
        pc = cpu_get_pc(m->cpu);
    crumb_vbl_done(instr, pc, r24);
}
