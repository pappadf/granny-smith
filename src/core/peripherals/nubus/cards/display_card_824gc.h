// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// display_card_824gc.h
// "Apple Macintosh Display Card 8•24 GC" ("Dolphin") — a NuBus display card
// whose display half is the JMFB family and whose Am29000 "GC" accelerator is
// *simulated, not emulated* (HLE, "option B").  See the proposal
// local/gs-docs/proposals/proposal-8-24gc-hle-acceleration.md and the
// reverse-engineering dossier under local/gs-docs/8-24GC/.
//
// Two halves:
//   * Display — the genuine v1.1 declaration ROM (`341-0266`, BoardId $2C) and
//     its `.Display_Video_Apple_MDCGC` video driver drive a linear framebuffer
//     + CLUT + VBL slot IRQ through the JMFB/Stopwatch/CLUT/Endeavor register
//     blocks at slot+$200000 (identical map to jmfb.c; proposal §3.13).
//   * Accelerator — the card model implements the GC-OS/IPC protocol surface
//     natively in C: the SRAM/DRAM windows, the accelerator control registers,
//     the shared-memory command block (CB) and the bring-up state machine that
//     makes the `.GraphAccel` driver believe a live Am29000 card booted.  The
//     firmware bytes are *stored, not executed*.  Stage 0/1 (this file) covers
//     identity, the register/RAM map, and the full bring-up ladder; drawing
//     funcs decline to the ROM path (proposal §4 safety net), so the desktop
//     renders correctly via QuickDraw's own fallback.  The DrawMultiObject
//     interpreter and rasterizers (stage 2+) are a follow-up under gcqd/.
//
// Address model (proposal §3.2, protocol §3): the card straddles two spaces.
// The display half + declaration ROM live in STANDARD slot space
// ($Fs000000).  The accelerator (SRAM, DRAM, control/comm regions) lives in
// SUPER-slot space ($s0000000), reached by the driver's card-local↔NuBus
// translation `NuBus(a) = (slot<<28) | (a & 0x0FFFFFFF)` for card-local
// addresses `0x0xxxxxxx..0x4xxxxxxx` (only $Fxxxxxxx passes through).

#ifndef NUBUS_CARDS_DISPLAY_CARD_824GC_H
#define NUBUS_CARDS_DISPLAY_CARD_824GC_H

#include "card.h"
#include <stdbool.h>
#include <stdint.h>

// === Declaration ROM ========================================================
// Ship v1.1 (341-0266) as default (richest monitor table; proposal §3.1).
// 64 KB chip, byteLanes $E1 (byte lane 0) → 256 KB bus-space footprint
// (chip ×4), placed at the top of standard slot space.
#define GC824_VROM_FILENAME      "Apple-341-0266.vrom"
#define GC824_DECLROM_CHIP_SIZE  0x010000u // 64 KB raw chip data (file size)
#define GC824_DECLROM_BUS_SIZE   0x040000u // 256 KB bus-space footprint (chip ×4)
#define GC824_DECLROM_BUS_OFFSET 0xFC0000u // slot top minus 256 KB
#define GC824_VROM_CRC           0xD722B053u // v1.1 Format-Block CRC (vrom.identify)

// === Display half (standard slot space) =====================================
#define GC824_VRAM_SIZE       0x200000u // 2 MB VRAM
#define GC824_FB_ALIAS_OFFSET 0x900000u // 24-bit Memory Manager ScrnBase mirror
// JMFB-family register blocks (verbatim from JMFBDepVideoEqu.a; see jmfb.h).
#define GC824_JMFB_BLOCK_OFFSET 0x200000u // JMFB / Stopwatch / CLUT / Endeavor
#define GC824_REGISTER_SIZE     0x000400u // covers all four 256-byte blocks
// In-block offsets (four 256-byte blocks: 0=JMFB, 1=Stopwatch, 2=CLUT, 3=Endeavor).
#define GC824_JMFBCSR       0x00u // Control & Status (sense lines at +2 bits 9-11)
#define GC824_JMFBVIDEOBASE 0x08u // Framebuffer base in VRAM (units of 32 bytes)
#define GC824_JMFBROWWORDS  0x0Cu // Stride encoding (depth-dependent)
#define GC824_SWICREG       0x3Cu // Stopwatch interrupt/control (bit1 = VINT disable)
#define GC824_SWCLRVINT     0x48u // Clear pending VBL interrupt
#define GC824_SWSTATUSREG   0xC0u // Stopwatch status (VBL toggle bit 2)
#define GC824_CLUTADDR      0x00u // RAMDAC palette index
#define GC824_CLUTDATA      0x04u // RAMDAC palette data (3 writes = R,G,B)
#define GC824_CLUTPBCR      0x08u // Pixel Bus Control (bits 3-4 = depth)
#define GC824_VINT_DISABLE  0x0002u // Stopwatch SWICReg bit 1 (active-high mask)
#define GC824_VRSTB         0x8000u // JMFBCSR master reset
#define GC824_MASK_SENSE    0xF1FFu // sense bits live in bits 9-11

// === Accelerator (super-slot space; offsets are card-local & 0x0FFFFFFF) =====
#define GC824_SRAM_OFFSET        0x0000000u // SRAM window
#define GC824_SRAM_SIZE          0x0020000u // 128 KB (driver pattern-sizes it)
#define GC824_SRAM_IALIAS_OFFSET 0x2000000u // SRAM-as-instruction-space alias
#define GC824_CTLREG_PAGE        0x4000000u // accelerator control-register page
#define GC824_CONFIG_OFFSET      0x6000000u // second config/ID register
#define GC824_DRAM_OFFSET        0xC000000u // DRAM bank 0 (holds the comm regions)
#define GC824_DRAM_SIZE          0x0200000u // 2 MB default
#define GC824_DRAM_MIRROR_OFFSET 0xD000000u // bank-0 mirror (incomplete decode)
// MFB / config / ACDC register space (RISC 0x44000000/0x46000000/0x46C00000
// aliased to host 0x04xxxxxx/0x06xxxxxx).  Backed as RAM for write/read-back
// register probes; the semantic registers below are intercepted.
#define GC824_REGS_OFFSET 0x4000000u // card-local base of the register space
#define GC824_REGS_SIZE   0x4000000u // 64 MB (covers 0x04000000..0x07FFFFFF)

// Accelerator control registers (card-local low 28 bits).
#define GC824_REG_ATTACH  0x4000028u // W: 0 detach / -1 attach (never read)
#define GC824_REG_ALIVE_C 0x4000044u // R bit31 = alive flag C
#define GC824_REG_ALIVE_B 0x4000048u // R bit31 = alive flag B
#define GC824_REG_ALIVE_A 0x400004Cu // R bit31 = alive flag A
#define GC824_REG_KICK    0x4000050u // W -1: firmware start/restart doorbell
#define GC824_REG_MEMCFG  0x400009Cu // R bit31: memory-config discriminator
// MFB sync/heartbeat register (RISC memory/interrupt controller).  The
// driver's card-sync primitive polls bit 31 waiting for it to cycle
// set→clear→set, confirming the RISC is alive and running; the HLE toggles
// it on each read (a free-running heartbeat), exactly like the JMFB VBL-sync
// toggle.  Reverse-engineered from the System 6.0.x driver at $52C0-$52E2.
#define GC824_REG_MFB_SYNC 0x44001C0u
// ACDC (RAMDAC/CLUT) ID register at card-local 0x46C00008.  Read-only; returns
// the ACDC identifier (bits 24-27 = 6).  The decl-ROM video driver's ACDC probe
// requires (read & 0x0F000000) == 0x06000000 to detect the ACDC (decl ROM
// $4488).  0x46C00008 & 0x0FFFFFFF = 0x06C00008 (host card-local offset).
#define GC824_REG_ACDC_ID 0x6C00008u

// Comm regions — fixed card-local addresses inside DRAM bank 0 (protocol §4).
// Expressed as DRAM-buffer offsets (card-local − 0x0C000000).
#define GC824_DRAM_PUBLICIN 0x006400u // host→card boot block
#define GC824_DRAM_PUBLICOU 0x006800u // card→host status block
#define GC824_DRAM_CB       0x007000u // HifComm command block (the RPC surface)
#define GC824_DRAM_VIDCOMM  0x007300u // display-mode-change protocol
#define GC824_DRAM_INPARGS  0x007800u // host launch args

// PublicIn field offsets (relative to PublicIn base).
#define GC824_PI_MAGIC    0x000u // boot magic 0x42005300 (card clears it)
#define GC824_PI_ENTRY    0x004u // firmware entry (0xF000)
#define GC824_PI_MEMLIMIT 0x008u // NuBus(top-of-DRAM)
#define GC824_PI_FREEMEM  0x014u // free-mem base for PQD structures
#define GC824_PI_FLAGS    0x018u // feature flags (→ kernel gr64)
#define GC824_PI_CBADDR   0x40Cu // card publishes NuBus(CB) here

// PublicOu field offsets.
#define GC824_PO_SIG      0x000u // signature 0x32100456 (comes from stored ACEF bytes)
#define GC824_PO_VERSION  0x004u // build version word
#define GC824_PO_MSTICKS  0x030u // ms-tick counter (advanced from VBL)
#define GC824_PO_FBCFG    0x01Cu // framebuffer base/config word
#define GC824_PO_DEPTH    0x020u // pixel depth (1/2/4/8/24)
#define GC824_PO_ROWBYTES 0x024u // rowBytes
#define GC824_PO_VSIZE    0x028u // vertical size
#define GC824_PO_SENSE    0x508u // monitor sense code

// CB (command block) field offsets (relative to CB base).
#define GC824_CB_STATUS    0x000u // arm magic in / status+error bits
#define GC824_CB_MODE      0x004u // comm-mode word (seeded from HELLO param)
#define GC824_CB_MAILBOX   0x00Cu // host reply-mailbox physical address
#define GC824_CB_FREEPTR   0x010u // free-area pointer (queue carve)
#define GC824_CB_DOORBELL  0x014u // -1 = ring (RPC request)
#define GC824_CB_STATUSW   0x018u // completion: 3 = OK, 0xB = error
#define GC824_CB_RESULT    0x01Cu // func result longword
#define GC824_CB_STATUSMIR 0x020u // *[here] = host addr to mirror status into
#define GC824_CB_CODE      0x028u // callback code (reverse RPC)
#define GC824_CB_FUNC      0x048u // func code (cmd+4)
#define GC824_CB_SEQ       0x04Cu // sequence word (cmd+8)
#define GC824_CB_ARGCOUNT  0x050u // arg count (cmd+0xC)
#define GC824_CB_ARGSOFF   0x054u // args-area offset (HELLO returns default)
#define GC824_CB_QUEUE_PUB 0x1C0u // Transport B: bytes published by host
#define GC824_CB_QUEUE_ACK 0x1C8u // Transport B: bytes consumed by card
#define GC824_CB_HEARTBEAT 0x1C4u // card heartbeat counter (ticked per VBL)
#define GC824_CB_ARGSAREA  0x64Cu // default command args area
#define GC824_CB_FREEAREA  0x6CCu // default free-list base

// Protocol magic values.
#define GC824_BOOT_MAGIC   0x42005300u // PublicIn+0: 'B\0S\0'
#define GC824_ARM_MAGIC    0x73333337u // CB+0: host-owns/armed
#define GC824_REPLY_MAGIC  0x96666669u // card→host reply-mailbox notify
#define GC824_PUBLICOU_SIG 0x32100456u // PublicOu+0 signature
#define GC824_FW_ENTRY     0x0000F000u // firmware entry point
#define GC824_CB_INIT      0x01010000u // CB+0 init/idle status
#define GC824_STATUSW_OK   0x00000003u // CB+0x18 completion OK
#define GC824_STATUSW_ERR  0x0000000Bu // CB+0x18 completion error

// Per-card kind descriptor — registered in nubus.c's g_card_registry.
extern const nubus_card_kind_t display_card_824gc_kind;

// === Video-mode selection (machine.nubus.video_mode) ========================
// A pending "<monitor>_<N>bpp" id consumed by the next card_init (mirrors the
// jmfb / 24AC equivalents): sets the monitor sense + depth and seeds PRAM.
void display_card_824gc_pending_video_mode_set(const char *id);
const char *display_card_824gc_pending_video_mode_get(void);
bool display_card_824gc_video_mode_lookup(const char *id, const nubus_monitor_t **out_monitor, int *out_depth_bpp);

// === Accelerator introspection (object model — slot[N].card.gc) =============
// True iff `card` is a display_card_824gc.  The getters return 0/false for any
// other card kind so the object-model layer can call them uniformly.
bool display_card_824gc_is_card(const nubus_card_t *card);
// Bring-up state name ("reset", "downloading", "booted", "armed", "gc-on", "error").
const char *display_card_824gc_state(const nubus_card_t *card);
uint32_t display_card_824gc_cb_addr(const nubus_card_t *card); // published NuBus(CB), 0 if not booted
uint32_t display_card_824gc_seq(const nubus_card_t *card); // last accepted RPC sequence
uint32_t display_card_824gc_lastfunc(const nubus_card_t *card); // last dispatched func code
uint64_t display_card_824gc_rpc_count(const nubus_card_t *card); // total RPCs serviced
uint64_t display_card_824gc_queue_bytes(const nubus_card_t *card); // total Transport-B bytes drained
bool display_card_824gc_gc_on(const nubus_card_t *card); // acceleration turned ON (Control $0D)
int32_t display_card_824gc_error(const nubus_card_t *card); // last posted error (Control $14), 0 = none

#endif // NUBUS_CARDS_DISPLAY_CARD_824GC_H
