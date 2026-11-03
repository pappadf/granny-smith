// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Breadcrumb stubs for unit tests: the real ones (debug/breadcrumb.c) read
// the machine's CPU, which a suite linking the scheduler alone does not have

#include "breadcrumb.h"

static volatile uint32_t g_crumb[CRUMB_WORDS];

volatile uint32_t *crumb_block(void) {
    return g_crumb;
}

crumb_phase_t crumb_enter(crumb_phase_t phase) {
    (void)phase;
    return CRUMB_EVENT_LOOP;
}

void crumb_leave(crumb_phase_t previous) {
    (void)previous;
}

const char *crumb_phase_name(crumb_phase_t phase) {
    (void)phase;
    return "";
}

void crumb_tick_begin(uint32_t vbls, double host_us_per_vbl, double host_us_per_loop, uint32_t mode) {
    (void)vbls;
    (void)host_us_per_vbl;
    (void)host_us_per_loop;
    (void)mode;
}

void crumb_vbl_done(uint64_t instr, uint32_t pc, uint32_t r24) {
    (void)instr;
    (void)pc;
    (void)r24;
}

void crumb_after_vbl(struct config *m, uint64_t instr) {
    (void)m;
    (void)instr;
}

void crumb_set(int word, uint32_t value) {
    (void)word;
    (void)value;
}

void crumb_gpu_wait(double ms) {
    (void)ms;
}
