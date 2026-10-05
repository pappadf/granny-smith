// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dbdma.h
// DBDMA — Apple's descriptor-based DMA engine: a bank of identical
// channels inside the Mac I/O controller (eleven in Grand Central,
// thirteen in Heathrow), one engine programmed one way for every device
// datapath (SCSI, floppy, Ethernet, serial, audio, ATA).  Channel
// registers live at island +$8000 + n*$100; both the registers and the
// in-memory command descriptors are LITTLE-ENDIAN.
//
// Register truth: the shipping-silicon programming model as agreed by the
// two driver corpora written for and proven on these exact machines —
// Linux arch/powerpc/include/asm/dbdma.h (Paul Mackerras, 1996) and the
// OSF/NetBSD macppc dbdma.h (Open Software Foundation, 1991-1998) — and
// their canonical stop/reset/start sequences.  Apple's pre-release 1993
// DBDMA.h describes an EARLIER architecture revision (different command
// nibble positions, JUMP/WAIT/STOP opcodes, shifted status bits) that
// shipping Grand Central does not implement; it is used here only for
// the register-file layout and the descriptor commit rule.
//
// The interface is family-clean (no config_t): guest memory and the
// interrupt line are injected hooks, so the isolated unit suite drives
// the engine against array-backed memory.  What differs per I/O
// controller is data the family supplies: the channel count (dbdma_init)
// and the channel -> interrupt-source map, which lives in the family's
// irq hook (Grand Central: interrupt n == channel n; Heathrow: a table,
// e.g. ATA channels 11/12 interrupt on sources 2/3).

#ifndef GS_CORE_PERIPHERALS_DBDMA_H
#define GS_CORE_PERIPHERALS_DBDMA_H

#include "checkpoint.h"
#include "dma_mem.h"

#include <stdbool.h>
#include <stdint.h>

struct dbdma;
typedef struct dbdma dbdma_t;

// Upper bound on channels per engine; the live count is dbdma_init's.
#define DBDMA_MAX_CHANNELS           16
#define DBDMA_CHANNELS_GRAND_CENTRAL 11 // channels 0-10 = interrupts 0-10
#define DBDMA_CHANNELS_HEATHROW      13 // blocks 0-12 (10 unowned)

// Per-channel register offsets (each channel spans $100; only these three
// are load-bearing on Grand Central, plus the three condition-select
// registers this model keeps live — see dbdma.c).
#define DBDMA_REG_CONTROL  0x00u // write: mask (hi 16) / value (lo 16)
#define DBDMA_REG_STATUS   0x04u // read: RUN..BT + device status byte
#define DBDMA_REG_CMDPTRLO 0x0Cu // physical address of next descriptor
#define DBDMA_REG_INTRSEL  0x10u // interrupt condition select (mask/value)
#define DBDMA_REG_BRSEL    0x14u // branch condition select
#define DBDMA_REG_WAITSEL  0x18u // wait condition select

// channelStatus bits (shipping positions — Linux/OSF names).
#define DBDMA_RUN     0x8000u // channel enabled (host-set)
#define DBDMA_PAUSE   0x4000u // execution suspended at command boundary
#define DBDMA_FLUSH   0x2000u // flush request; self-clears when done
#define DBDMA_WAKE    0x1000u // restart a STOPped channel; self-clears
#define DBDMA_DEAD    0x0800u // channel died on an error (sticky)
#define DBDMA_ACTIVE  0x0400u // program in progress (engine-owned)
#define DBDMA_BT      0x0100u // last command's branch was taken
#define DBDMA_DEVSTAT 0x00FFu // device status bits s0-s7

// === Injected hooks =========================================================

// Guest-physical memory moves (descriptor fetch/write-back and data
// transfers; the CPU MMU is deliberately not in the path) go through the
// shared dma_mem_port_t.  This engine fills the port's BLOCK
// slots: it moves kilobytes per command, and tnt.c's implementation turns
// the RAM case into a memcpy -- which is also why it is not the same
// implementation as dma_mem_port_physical (that one resolves host memory
// only; this one falls through to the whole bus).

// Channel-completion interrupt; the family maps `chan` to its interrupt
// source.  Fired AFTER the descriptor's result write-back, per the house
// gotcha.
typedef void (*dbdma_irq_fn)(void *ctx, int chan);

// Per-channel device port.  OUTPUT_* commands offer bytes to `out`,
// INPUT_* commands request bytes from `in`; each returns the count
// actually moved — returning less than `len` stalls the channel (still
// ACTIVE) until the device calls dbdma_kick().  `s_bits` (optional)
// is the device's live s0-s7 status byte, consulted by the branch/wait/
// interrupt condition selectors.  A channel with no port stalls on its
// first data command (and logs) — the honest "device not modeled yet".
typedef struct dbdma_port {
    int (*out)(void *ctx, const uint8_t *buf, int len); // memory -> device
    int (*in)(void *ctx, uint8_t *buf, int len); // device -> memory
    uint8_t (*s_bits)(void *ctx); // live device status bits (may be NULL)
    void *ctx;
    // Bytes this port will move per activation before the channel yields,
    // or 0 for "as many as the device offers".
    //
    // A port whose device is itself rate-limited -- the AWACS half-buffer,
    // the SWIM3 byte ring, an SCC ring -- needs nothing here: it returns
    // short on its own and the channel stalls until the device kicks it.
    // MESH does not: it pops straight off the SCSI bus, so a data command
    // ran to completion inside the guest's control-register store.  A
    // non-zero burst makes the channel yield mid-command with its cursor
    // intact, exactly as a short device return does, and the device's own
    // scheduler pump kicks it again on the bus's cadence.
    //
    // Set it only together with something that will kick, or the channel
    // parks for good.  Last in the struct so positional initialisers stay
    // valid, like mac030_io_range_t.esync.
    int burst;
    // Packet boundaries, for a device that moves frames rather than a byte
    // stream (BMAC).  Both optional.
    //
    // `out_last` is called when an OUTPUT_LAST command has delivered its
    // last byte, before the branch decision and the result write-back: the
    // frame the device assembled from OUTPUT_MORE/OUTPUT_LAST pieces is
    // complete, and whatever s-bits sending it changes are the ones the
    // command's status sees.
    //
    // `in_end` answers whether the device's input stopped at the end of a
    // frame, and consumes that boundary.  The engine asks when `in` comes
    // back short — true completes the command early, its residual the
    // unused remainder of reqCount — and again when a command fills
    // exactly, so a frame that ends on a descriptor's last byte does not
    // also end the next one.  `in` returns 0 while a boundary is pending.
    void (*out_last)(void *ctx);
    bool (*in_end)(void *ctx);
} dbdma_port_t;

// === Lifecycle ==============================================================

// Build an engine of `nchan` channels (1..DBDMA_MAX_CHANNELS) and, with
// `cp`, restore its channel state.  The stream holds exactly `nchan`
// channel records, so an 11-channel engine checkpoints as before.
dbdma_t *dbdma_init(checkpoint_t *cp, int nchan);
void dbdma_delete(dbdma_t *d);
void dbdma_checkpoint(dbdma_t *d, checkpoint_t *cp);
void dbdma_reset(dbdma_t *d); // power-on: all channels idle

void dbdma_set_memory_port(dbdma_t *d, const dma_mem_port_t *port); // copied; NULL unbinds
void dbdma_set_irq_hook(dbdma_t *d, dbdma_irq_fn fn, void *ctx);
void dbdma_set_port(dbdma_t *d, int chan, const dbdma_port_t *port); // port copied; NULL detaches

// === Register file (values in little-endian register domain) ===============

// The island dispatcher byte-swaps at the bus edge (TNT_LE32 / GOS_LE32); these take
// and return the little-endian register VALUES the guest composed with
// stwbrx / recovers with lwbrx.  Control writes take effect synchronously
// — the status a guest reads back next reflects them (the #1 way DBDMA
// models hang drivers is deferring this).
uint32_t dbdma_reg_read(dbdma_t *d, int chan, uint32_t offset);
void dbdma_reg_write(dbdma_t *d, int chan, uint32_t offset, uint32_t value);

// === Device-side pacing =====================================================

// Re-run a channel whose data command stalled on the device (data/space
// became available, or the device's s-bits changed and a WAIT may now
// pass).  Safe to call on an idle channel.
void dbdma_kick(dbdma_t *d, int chan);

// True when the channel is enabled and mid-program (a device pump uses
// this to know whether polling its side is worthwhile).
bool dbdma_active(dbdma_t *d, int chan);

#endif // GS_CORE_PERIPHERALS_DBDMA_H
