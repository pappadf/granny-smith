// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// breadcrumb.h
// Where the emulator thread is, in words another thread can read while it
// is stuck.  A small block of 32-bit words in shared memory: the thread
// stores what it is doing (the tick phase, the frame-unit of the tick it
// is running, the instruction count and PC after each frame-unit, a GPU
// wait), and the page reads the block when the thread stops answering --
// the one view of a wedged core that does not need the core's help.
// Plain stores, no locks: a reader may see a phase change half done, which
// costs nothing but a stale word.

#ifndef BREADCRUMB_H
#define BREADCRUMB_H

#include <stdint.h>

// The block's words (the page reads them by these indices: keep
// app/web2/src/bus/diagnostics.ts in step)
enum {
    CRUMB_MAGIC, // CRUMB_MAGIC_VALUE
    CRUMB_VERSION, // CRUMB_VERSION_VALUE
    CRUMB_SEQ, // bumped on every phase change
    CRUMB_PHASE, // crumb_phase_t
    CRUMB_PHASE_MS, // wall-clock time (ms, low 32 bits) the phase began
    CRUMB_OUTER, // the phase the current one interrupted (a GPU wait inside "run")
    CRUMB_TICKS, // ticks begun
    CRUMB_TICK_MS, // wall-clock time the current (or last) tick began
    CRUMB_TICK_VBLS, // frame-units the current tick was given
    CRUMB_TICK_VBL, // frame-units of it run so far
    CRUMB_INSTR_LO, // instructions retired, after the last frame-unit
    CRUMB_INSTR_HI,
    CRUMB_PC, // the CPU's PC after the last frame-unit
    CRUMB_R24, // PowerPC r24 (the 68K emulator's PC), or 0
    CRUMB_VBL_MS, // wall-clock time the last frame-unit ended
    CRUMB_HOST_US_PER_VBL, // the scheduler's estimate of a frame-unit's host cost
    CRUMB_HOST_US_PER_LOOP, // ...and of the host loop period
    CRUMB_MODE, // scheduler mode id
    CRUMB_JIT_PENDING, // asynchronous compiles in flight
    CRUMB_GPU_WAITS, // GPU waits begun (any kind)
    CRUMB_GPU_WAIT_MS, // host ms spent in GPU waits (total, low 32 bits)
    CRUMB_WORDS = 32
};

#define CRUMB_MAGIC_VALUE   0x47534442u // "GSDB"
#define CRUMB_VERSION_VALUE 1u

// What the thread is doing (CRUMB_PHASE)
typedef enum {
    CRUMB_EVENT_LOOP, // between ticks: the worker's JavaScript event loop
    CRUMB_TICK, // a tick's bookkeeping (perf, checkpoint timer)
    CRUMB_RUN, // the scheduler running frame-units (guest code)
    CRUMB_VIDEO, // the display update after them
    CRUMB_REQUESTS, // serving the page's requests
    CRUMB_IDLE, // a stopped machine waiting for a request
    CRUMB_GPU_RING, // waiting for room in a GPU worker's op ring
    CRUMB_GPU_ACK, // waiting for a GPU worker's acknowledgement (a readback, a fence)
    CRUMB_GPU_PACE, // waiting for a GPU worker to finish a frame
    CRUMB_JIT_INSTALL, // installing a compiled translation (between ticks)
    CRUMB_CHECKPOINT, // saving or loading a checkpoint
    CRUMB_WATCH, // the watch: status and perf lines
    CRUMB_PHASES
} crumb_phase_t;

// The block (32 words), for the platform to hand to the page
volatile uint32_t *crumb_block(void);

// Enters `phase`; returns the phase it replaces, for crumb_leave
crumb_phase_t crumb_enter(crumb_phase_t phase);
void crumb_leave(crumb_phase_t previous);

// The phase's name ("run", "gpu-ack", ...)
const char *crumb_phase_name(crumb_phase_t phase);

// The scheduler: a tick begins with `vbls` frame-units, one has run
void crumb_tick_begin(uint32_t vbls, double host_us_per_vbl, double host_us_per_loop, uint32_t mode);
void crumb_vbl_done(uint64_t instr, uint32_t pc, uint32_t r24);
struct config;
void crumb_after_vbl(struct config *m, uint64_t instr); // the machine's CPU's PC

// Counters the page reads with the block
void crumb_set(int word, uint32_t value);
void crumb_gpu_wait(double ms);

#endif // BREADCRUMB_H
