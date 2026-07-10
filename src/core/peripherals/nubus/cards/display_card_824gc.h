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
//     firmware bytes are *stored, not executed*.  Stage 0/1 covers identity,
//     the register/RAM map, and the full bring-up ladder; stage 2 adds the
//     DrawMultiObject interpreter + 1-bpp rasterizers and the func-$15 blit.
//     Anything outside the accept envelope declines to the ROM path (proposal
//     §4 safety net), and gc.force_decline declines everything — the
//     differential test oracle.
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
// Loaded by CONTENT via declrom_load_vrom_card (vrom.c catalog keyed on the
// Format-Block CRC) — any of the three known GC ROMs works: v1.1 (341-0266,
// 64 KB, richest monitor table; proposal §3.1), v1.0, or the alpha (32 KB).
// All are byteLanes $E1 (byte lane 0) → chip ×4 bus-space footprint laid into
// the tail of a 256 KB window at the top of standard slot space.
#define GC824_DECLROM_BUS_SIZE   0x040000u // 256 KB bus window (64 KB chip ×4)
#define GC824_DECLROM_BUS_OFFSET 0xFC0000u // slot top minus 256 KB
#define GC824_VROM_CRC           0xD722B053u // v1.1 Format-Block CRC (vrom.identify)

// === Display half ===========================================================
#define GC824_VRAM_SIZE       0x200000u // 2 MB standard-slot VRAM (early boot FB)
#define GC824_FB_ALIAS_OFFSET 0x900000u // 24-bit Memory Manager ScrnBase mirror
// The active framebuffer lives in the card's super-slot DRAM aperture, NOT the
// standard-slot VRAM.  System 6.0.8 has 32-bit QuickDraw, so the GC decl-ROM's
// SecondaryInit (0x2690) swaps the video sResource to its 32-bit variant, moving
// ScrnBase to the MajorBase (super-slot) aperture: ScrnBase ($0824) = WMgrPort
// baseAddr = NuBus 0x9C011400 = DRAM aperture (super-slot 0x0C000000) + 0x11400,
// stride 1024, 1 bpp (System 6 boot default).  QuickDraw / the Finder draw the
// whole desktop there (verified: dumping 0x9C011400 → full menu bar + windows).
// The standard-slot VRAM only holds the *early* "Welcome to Macintosh" startup
// screen, drawn before SecondaryInit swapped the base — a red herring.  So the
// host display surfaces p->dram + GC824_FB_OFFSET.
#define GC824_FB_OFFSET 0x011400u
// (legacy, unused) offsets kept for reference.
#define GC824_FB_DRAM_OFFSET 0x010000u
#define GC824_FB_VRAM_OFFSET 0x011400u
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

// GCQD "gcp" command-block window (STANDARD-slot space).  The QuickDraw
// marshaller (GCQD, gc24 -4048) does NOT reach the command block through the
// super-slot DRAM aperture — it uses the driver's "gcp" pointer, computed in
// .GraphAccel Open (driver doc §5.1 step 5 / §7) as
//     card+$16C = $F0000000 | (slot<<24) | (slot<<20)   (= std base + slot*1 MB)
//     gcp       = card+$C = card+$16C + $8C00
// Init29K's arm/CB-read+arm path is gated OFF for the normal config ($168 bit
// 11 stays set — .GraphAccel sub_2A54 sets base $800 and only $128 bit 8 clears
// it, and $128 = config-A $60 at that point), so the driver never reads
// PublicIn+$40C nor writes the arm magic; GCQD drives the whole CB (doorbell,
// status, heartbeat, args, queue-carve) through this fixed, slot-derived
// STANDARD-slot window instead.  We alias it onto the SAME DRAM CB the
// super-slot exposes at GC824_DRAM_CB, so the fields the marshaller polls hit
// the live CB engine (and CB-internal pointers are published gcp-relative so
// GCQD's `(gcp & $FFF00000) | (ptr & $FFFFF)` address reconstruction lands in
// this window).  See debug/2026-07-09-*-gcp-window.md.
#define GC824_GCP_OFFSET 0x8C00u // gcp offset within card+$16C
#define GC824_GCP_WINDOW 0x2000u // CB header + carved queue (GCQD G+$268 = 4 KB)

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
// ACDC RAMDAC palette ports (card-local, super-slot).  A byte write to the ADDR
// port sets the palette index and resets the R/G/B phase; three successive byte
// writes to the DATA port are R, G, B (8-bit each), after which the index
// auto-increments.  (decl-ROM SetEntries $2C86-$2CA6 / GetEntries $31E8-$3208.)
#define GC824_REG_ACDC_ADDR 0x6C00000u
#define GC824_REG_ACDC_DATA 0x6C00004u
// Video "heartbeat" register in Am29000/RISC space at card-local 0x4C00000.
// The decl-ROM video driver's cardSync primitive ($33C8) polls bit 31 of THIS
// cell (via the delayTouch helper $3420, whose last read lands in D0) waiting
// for a full set→clear→set cycle; the $44001C0 read in that loop is a discarded
// bus side-effect.  The HLE synthesizes a toggling bit 31 here (see gc_read).
#define GC824_REG_SYNC_HB 0x4C00000u

// Comm regions — fixed card-local addresses inside DRAM bank 0 (protocol §4).
// Expressed as DRAM-buffer offsets (card-local − 0x0C000000).
#define GC824_DRAM_PUBLICIN 0x006400u // host→card boot block
#define GC824_DRAM_PUBLICOU 0x006800u // card→host status block
#define GC824_DRAM_CB       0x007000u // HifComm command block (the RPC surface)
#define GC824_DRAM_VIDCOMM  0x007300u // display-mode-change protocol
#define GC824_DRAM_INPARGS  0x007800u // host launch args

// GCQD per-context scratch (the "ctx" token func $17 publishes at CB+$608).
// GCQD threads this pointer through its QuickDraw bottlenecks and builds
// per-context structures off it — notably the offscreen-GWorld list lock at
// ctx+$C0 (walked by GCQD sub_492E in 32-bit MMU mode).  It MUST NOT overlap
// the CB window: the CB's own blit scratch lives at CB+$58.. and would corrupt
// the lock's peer word (ctx+$C4), spinning GCQD forever.  Give it a private,
// zero-initialized region high in DRAM, reached via the super-slot (32-bit).
#define GC824_DRAM_GCTX 0x180000u // GCQD context object (private scratch)
#define GC824_GCTX_SIZE 0x001000u // 4 KB — ample for GCQD's context fields

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
// Cursor-protocol fields (gc-cursor-protocol.md §3): the driver deposits the
// HOST addresses of the low-memory cursor globals so the card can bus-master-
// read them; the card raises the two status flags, which the host cursor stubs
// and GACursorTask consume (clear-on-read) to keep the ROM's software-cursor
// state coherent with card drawing.
#define GC824_CB_CRSRHID   0x5F0u // card→host: "card hid the cursor" → CrsrVis=0
#define GC824_CB_CRSRNEW   0x5F4u // card→host: "cursor changed" → CrsrNew=1
#define GC824_CB_CRSRRECTP 0x5F8u // host→card: host addr of CrsrRect ($083C)
#define GC824_CB_CRSRVISP  0x5FCu // host→card: host addr of CrsrVis ($08CC)

// VidComm — the display-mode-change protocol block (decl-ROM programMode
// $3436): once the firmware stamps GC824_VC_MAGIC at +$18, every cscSetMode /
// cscSetDefaultMode publishes the new geometry here, raises `go`, and rings
// the $0400_0050 doorbell; the card applies the mode and CLEARS the ack byte
// (+$04 byte 0), which the host polls before programming the ACDC/CRTC itself
// and releasing with ack = -1.  Offsets relative to GC824_DRAM_VIDCOMM.
#define GC824_VC_GO        0x00u // host: $80000000 = geometry valid
#define GC824_VC_ACK       0x04u // card clears byte 0; host releases with -1
#define GC824_VC_FBBASE    0x08u // NuBus framebuffer base for the new mode
#define GC824_VC_ROWBYTES  0x0Cu // rowBytes
#define GC824_VC_BPP       0x10u // pixel depth (1/2/4/8/16/24)
#define GC824_VC_SCANLINES 0x14u // scan-line count
#define GC824_VC_MAGIC_OFF 0x18u // firmware presence tag (host tests this)
#define GC824_VC_PAGEFLIP  0x1Cu // page-flip/interlace state (16/32-bpp crossings)
#define GC824_VC_MAGIC     0x57593160u // 'WY1`' — RISC video engine present

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
// Differential-test-oracle switch (proposal §4.1): when set, the drawing funcs
// ($2D SetPort / $15 StretchBits / $30 FontDownload) decline, so the identical
// guest scene renders via QuickDraw's ROM path — the accel-vs-ROM pixel oracle.
bool display_card_824gc_force_decline(const nubus_card_t *card);
void display_card_824gc_set_force_decline(nubus_card_t *card, bool v);

#endif // NUBUS_CARDS_DISPLAY_CARD_824GC_H
