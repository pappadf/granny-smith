// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iisi_internal.h
// Private state and equates for the Macintosh IIsi ("Erickson"/"Elsie")
// machine.  The IIsi is architecturally the IIci — same MDU-decoded I/O island
// at $50F0xxxx, same RBV chip ($50F26000, here in its V8/VISA variant), same
// built-in video reading from a slot aperture — with one defining addition:
// the Egret companion chip ($50F00000 VIA1 shift register) replaces the
// classic VIA1 transceiver/RTC path for ADB, the real-time clock, parameter
// RAM, the 1-second tick, and soft power-off.  Mirrors iici_internal.h in
// shape; the framebuffer lives in the slot-$E aperture ($FEE08000) rather than
// the IIci's slot-$B ($FBB08000).

#ifndef IISI_INTERNAL_H
#define IISI_INTERNAL_H

#include "common.h"
#include "mdu_io.h"
#include "memory.h"
#include "mmu.h"
#include "system_config.h" // for config_t.machine_context
#include <stdbool.h>
#include <stdint.h>

struct adb;
struct asc;
struct egret;
struct floppy;
struct rbv;
struct nubus_card;

// Per-machine state.
typedef struct iisi_state {
    struct adb *adb;
    struct asc *asc;
    struct egret *egret; // Egret companion (ADB/RTC/PRAM/tick/power via VIA1 SR)
    struct floppy *floppy;
    struct rbv *rbv; // RBV chip in V8 variant (VIA2 replacement + video control)
    struct nubus_card *video_card; // built-in V8 video pseudo-card (slot $E aperture)

    bool rom_overlay;
    mmu_state_t *mmu;

    mdu_io_t mdu_io; // device handles for the shared MDU dispatcher

    uint8_t last_port_a; // floppy/overlay filtering on VIA1 PA

    memory_interface_t io_interface;
} iisi_state_t;

static inline iisi_state_t *iisi_state(config_t *cfg) {
    return (iisi_state_t *)cfg->machine_context;
}

// IRQ source bit assignments (identical to the IIci: RBV at level 2 carries
// the combined slot/SCSI/sound IFR; Egret rides VIA1's shift-register
// interrupt at level 1, so it needs no separate source).
#define IISI_IRQ_VIA1 (1 << 0)
#define IISI_IRQ_RBV  (1 << 1)
#define IISI_IRQ_SCC  (1 << 2)
#define IISI_IRQ_NMI  (1 << 3)

// (I/O penalties + window offsets live with the shared dispatcher, mdu_io.c.)

// Address-space constants.  Shared MDUtable layout: ROM at $40800000, I/O
// island at $50F0xxxx mirrored across $50000000 with a $3FFFF mask so RBV
// ($26000) and VDAC ($24000) decode distinctly from the SCSI windows.
#define IISI_ROM_START 0x40800000UL
#define IISI_ROM_END   0x41000000UL
#define IISI_IO_BASE   0x50000000UL
#define IISI_IO_SIZE   0x10000000UL
#define IISI_IO_MIRROR 0x0003FFFFUL

// On-board video frame buffer placement (Macintosh IIsi Developer Note §8.2 /
// MMUTables.a `isVideo` "at physical zero for RBV"): the V8 reads the frame
// buffer from the BOTTOM of Bank A — physical address $00000000.  VideoInfoMacIIsi
// records a screen physical base of 0; the slot-$E screen base ($FEE08000 32-bit
// / $00E08000 24-bit) is mapped by the OS's PMMU onto physical 0.  So the card
// renders straight out of Bank A at offset 0 — there is no $8000 wrap offset on
// the IIsi (unlike the IIci's separate-buffer card).
#define IISI_VRAM_BASE        0xFEE00000UL // slot-$E aligned 32-bit aperture base (reference)
#define IISI_FB_PHYS_OFFSET   0x00000000UL // frame buffer at physical 0 (Bank A bottom)
#define IISI_FB_SCREEN_OFFSET 0x0UL // active screen sits at the frame-buffer start

// Two physical RAM banks (Developer Note §3.2/§3.3/§6.1):
//   Bank A: soldered 1 MB at physical $00000000 (holds the video frame buffer
//           at its bottom), mirroring within its 64 MB window.
//   Bank B: SIMM expansion at physical $04000000 (the OS makes this system "low
//           memory"), mirroring its installed size within its 64 MB window.
#define IISI_BANK_A_SIZE 0x00100000UL // 1 MB soldered Bank A
#define IISI_BANK_B_PHYS 0x04000000UL // Bank B physical base
#define IISI_BANK_WINDOW 0x04000000UL // 64 MB per-bank mirror window

#endif // IISI_INTERNAL_H
