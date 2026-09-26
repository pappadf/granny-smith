// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// platform.h
// The one platform header: what core code may assume of any host.
//
// Every target -- the browser (wasm), headless, the tools and the unit tier
// -- includes this, and each supplies only its clock, as `platform_clock.h`
// on its include path.  There used to be a copy per target, and they had
// drifted: the wasm copy hand-rolled the bit scans the headless one got from
// the compiler, both carried a mutex, a thread id, drive-activity enums and a
// tick counter nothing used, and the unit tier and the tools each kept a
// third and fourth.

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Byte swapping.
#define BE16(x) ((((x) & 0xff00) >> 8) | (((x) & 0x00ff) << 8))
#define BE32(x)                                                                                                        \
    ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >> 8) | (((x) & 0x0000ff00) << 8) | (((x) & 0x000000ff) << 24))

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// Count trailing zeros in a 32-bit value (32 for zero).
static inline int platform_ntz32(uint32_t mask) {
    return mask ? __builtin_ctz(mask) : 32;
}

// The highest set bit's position (bit scan reverse); -1 for zero.
static inline int platform_bsr32(uint32_t value) {
    return value ? 31 - __builtin_clz(value) : -1;
}

// === Host audio (em_audio.c; no-ops on headless) ===
// One parameterized stream for all machines: open sets the source rate and
// channel count, push feeds interleaved int16 frames (vol_0_7 is the guest's
// 3-bit volume), set_rate restarts the stream at a new source rate.
void platform_audio_open(uint32_t src_rate_hz, int channels);
void platform_audio_push(const int16_t *frames, int nframes, int vol_0_7);
void platform_audio_set_rate(uint32_t src_rate_hz);
// The host ring's fill against its target depth (0.0 empty, 1.0 at target),
// or < 0 when there is no fresh signal (no stream, no host audio) -- optional
// feedback for the accelerated-mode governor.
double platform_audio_ring_fill(void);

// Print the host's callstack, for the failure handler.
void platform_print_host_callstack(void);

// A ROM at `rom_path` is about to boot (machine_boot_apply, after the
// document validated): the host's chance to offer the card ROMs it keeps
// beside it (vrom_offer_dir / prom_offer_dir).  Core builds no search path
// of its own (docs/core/memory/rom.md).  Headless walks the ROM's directory
// for *.vrom / *.prom, as it does for the CLI's rom= at startup; the browser
// has nothing to do, its card ROMs live in OPFS and are offered on upload.
void platform_offer_sibling_card_roms(const char *rom_path);

// The target's clock: host_time() (seconds) and host_time_ms().
#include "platform_clock.h"

#endif // PLATFORM_H
