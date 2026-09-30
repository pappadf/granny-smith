// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// davbus.h
// The Mac I/O controllers' DAVbus sound cell — Grand Central's AWACS face
// (TNT, island +$14000) and Heathrow's Screamer face (the beige G3,
// +$14000): five 32-bit little-endian registers on $10 centres (sound
// control, codec control, codec status, clip count, byte swap), the codec's
// expanded-command shadows behind the codec-control port, and the output
// datapath — one DBDMA channel pulling a descriptor program into the shared
// host audio stream at the selected frame rate.
//
// The cell is family-clean: the family hands it the scheduler, the DBDMA
// engine and channel, and the CPU clock the pacing is rationalised
// against, and forwards its island's register accesses.  The codec is
// AWACS (revision 2 in the status word) or Screamer (revision 3, Crystal
// manufacturer 1, registers 5-7 and the register-7 read-back) — the one
// variant flag.
//
// Register truth: the OS driver corpus for these machines (Apple's
// AppleOWScreamerAudio/PPCAwacs and awacs_OWhw.h, Linux sound/ppc awacs,
// NetBSD awacs.c) and the ITT ASCO 2300 codec datasheet via awacs.h.

#ifndef GS_CORE_PERIPHERALS_DAVBUS_H
#define GS_CORE_PERIPHERALS_DAVBUS_H

#include "awacs.h" // shared ASCO codec semantics

#include <stdbool.h>
#include <stdint.h>

struct scheduler;
struct dbdma;
struct object;

// The cell's software-visible state and its channel pacing: plain data,
// checkpointed whole by the family (the layout the TNT stream has always
// carried as its AWACS block).
typedef struct davbus {
    uint32_t sound_ctrl; // +$00: subframe selects, rate field (bits 10:8)
    uint32_t codec_ctrl; // +$10: last command (NEWECMD reads back clear)
    uint32_t byte_swap; // +$40: bit 0 = sample data is little-endian
    uint16_t codec[AWACS_CODEC_REGS]; // expanded-command shadows
    // Output-channel pacing (exact-rational: frames = cycles*rate/freq)
    uint64_t tick_cycles; // cycle stamp of the last credit grant
    uint64_t tick_frac; // running remainder of (elapsed*rate) mod freq
    uint32_t credit; // frames the port may consume before the next grant
    uint8_t tick_armed; // the pacing event is pending (mirrors scheduler)
    uint8_t partial[4]; // sub-frame byte assembly across port calls
    uint32_t partial_len;
    // Diagnostics (machine.sound)
    uint64_t frames_pushed; // frames rendered into the host stream
    int32_t peak; // loudest |sample| pushed since power-on
} davbus_t;

// The wiring the family supplies (not checkpointed; rebuilt at init).
typedef struct davbus_host {
    davbus_t *regs; // the family's checkpointed register block
    struct scheduler *sched;
    struct dbdma *dbdma;
    int out_chan; // the output DBDMA channel (8 on both controllers)
    uint32_t cpu_hz; // scheduler cycle rate the pacing is exact against
    bool screamer; // Screamer codec (revision 3) instead of AWACS
    int16_t *stage; // gain-applied staging frames for audio_out_push
    struct object *snd_object; // machine.sound
} davbus_host_t;

// The event type (before scheduler_start replays a restore).
void davbus_register_events(davbus_host_t *h);
// Open the host stream, attach the output channel's port, build
// machine.sound.
void davbus_init(davbus_host_t *h);
void davbus_reset(davbus_host_t *h); // power-on register state
void davbus_teardown(davbus_host_t *h);

// Island access for the +$14000 block (little-endian register domain).
uint32_t davbus_read32(davbus_host_t *h, uint32_t offset);
void davbus_write32(davbus_host_t *h, uint32_t offset, uint32_t value);

#endif // GS_CORE_PERIPHERALS_DAVBUS_H
