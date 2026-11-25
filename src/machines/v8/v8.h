// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// v8.h
// The V8 family: Apple's low-cost 68030 boards built around one custom
// "VISA"-lineage decoder that absorbs the MDU, the RBV, the ASC and the
// video timing of a Macintosh IIci into a single gate array.  Three chips of
// the lineage are modelled here, each a model profile over one substrate:
//
//   V8    Macintosh LC II       external monitor, VRAM SIMM, Ariel CLUT
//   Eagle Macintosh Classic II  built-in 512x342 1-bit CRT, frame buffer in DRAM
//   Spice Macintosh Color Classic built-in 512x384 colour CRT, VRAM, Ariel CLUT
//
// The register file at $50F26000 is the RBV's (the V8 variant of rbv.c);
// the per-model deltas -- the soldered RAM, the machine-ID strap on VIA1
// port A, the frame-buffer source, the monitor -- are a v8_board_t named by
// hw_profile_t.board.  See docs/internals/machines/v8/v8.md.
//
// The Sonora-based boards (LC III, LC III+, LC 520, LC 550, Color Classic
// II; sonora.c) are a second chip kind of the same substrate: the same I/O
// island and RBV-style pseudo-VIA2, but a 32-bit-only decoder with ROM at
// $40800000, VRAM at $60B00000, a bank-stitching RAM decoder, a CPU ID
// register and its own video control registers.

#ifndef GS_MACHINES_V8_V8_H
#define GS_MACHINES_V8_V8_H

#include "m68k_board.h" // m68k_board_desc_t
#include "machine_profile.h" // machine_substrate_t, builtin_video_desc_t
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

// Where the built-in video scans its frame buffer from.
typedef enum v8_fb_source {
    V8_FB_VRAM = 0, // VRAM in the I/O space at $50F40000 (V8, Spice)
    V8_FB_DRAM = 1, // a fixed page of main RAM (Eagle's compact 1-bit screen)
} v8_fb_source_t;

// The decoder generation a board is built around.
typedef enum v8_chip_kind {
    V8_KIND_V8 = 0, // V8, Eagle, Spice: the 10 MB RAM space, the 24-bit windows
    V8_KIND_SONORA = 1, // Sonora and its Ardbeg variant (sonora.c)
} v8_chip_kind_t;

// One model of the family: the hardware data the substrate is built from.
typedef struct v8_board {
    const char *chip; // "V8" / "Eagle" / "Spice" / "Sonora" / "Ardbeg" (tracing)
    v8_chip_kind_t kind;
    m68k_board_desc_t desc; // ROM window, I/O table, bus-error window

    // VIA1 port A machine-ID strap: the levels of PA6, PA4, PA2 and PA1
    // (bit n of this byte is PAn), as the ROM's machine-identification tables test them.
    uint8_t id_strap;

    uint32_t pcb_ram; // RAM soldered to the logic board, in bytes (2 or 4 MB)

    // Built-in video.
    v8_fb_source_t fb_source;
    uint32_t vram_size; // VRAM fitted (V8_FB_VRAM), in bytes
    uint32_t dram_fb; // physical address of the DRAM frame buffer (V8_FB_DRAM)
    // A built-in CRT is strapped to one raster: its width/height and the
    // sense code its cable presents (0 width = an external monitor port,
    // whose sense comes from the configured monitor).
    uint16_t crt_width, crt_height;
    uint8_t crt_sense;
    bool crt_mono; // the CRT shows one gun (the compact 1-bit screen)

    uint8_t asc_version; // the sound block's ASC version byte

    // The ADB/clock/power microcontroller on VIA1's shift register: Egret
    // (LC II, Classic II) or Cuda (Color Classic -- its ROM opens with Cuda's
    // TIP/ByteAck/TReq handshake).
    bool cuda;

    // Sonora only: the CPU ID register at $5FFFFFFC (signature $A55A in the
    // high half, the board ID in the low half).
    uint32_t cpu_id;
} v8_board_t;

// The one substrate every V8-family model binds.
extern const machine_substrate_t v8_substrate;

// The V8 family's I/O island ($50F00000, 256 KB) is the MDU's window table
// (mdu_io_ranges_tbl); the Sonora island is the same rows plus Sonora's own
// registers (sonora.c).
extern const board_io_range_t sonora_io_ranges_tbl[];

// Fixed addresses shared by every chip of the lineage.
#define V8_ROM_START     0x40000000UL // ROM space (ROMBase $40A00000 is one mirror)
#define V8_ROM_END       0x50000000UL
#define V8_IO_MIRROR     0x0003FFFFUL // 256 KB island
#define V8_ISLAND_BASE   0x50F00000UL // the I/O island proper
#define V8_ISLAND_SIZE   0x00040000UL
#define V8_VRAM_BASE     0x50F40000UL // VRAM window ($50F40000-$50FBFFFF)
#define V8_VRAM_WINDOW   0x00080000UL // 512 KB decoded
#define V8_LOW_ROM_START 0x00A00000UL // the 24-bit ROM window, decoded in both modes
#define V8_LOW_ROM_END   0x00E00000UL
#define V8_LOW_IO_BASE   0x00F00000UL // the 24-bit I/O window
#define V8_LOW_IO_SIZE   0x00100000UL
#define V8_RAM_SPACE     0x00A00000UL // 10 MB of RAM address space
#define V8_RAM_IMAGE     0x00800000UL // $800000-$9FFFFF: the top 2 MB of soldered RAM, always

// The Sonora decoder's fixed addresses (sonora.c).
#define SONORA_VRAM_BASE   0x60B00000UL // the frame buffer (two 512 KB VRAM banks)
#define SONORA_VRAM_WINDOW 0x00100000UL
#define SONORA_CPUID_ADDR  0x5FFFFFFCUL // the CPU ID register
#define SONORA_RAM_BANK    0x01000000UL // a bank's span while the ROM sizes RAM
#define SONORA_RAM_SPACE   0x05000000UL // five banks (built-in + two SIMMs of two)

#endif // GS_MACHINES_V8_V8_H
