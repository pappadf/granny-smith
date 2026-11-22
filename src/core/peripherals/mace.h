// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mace.h
// The MACE Ethernet core: AMD's Am79C940 as Apple's Curio ASIC embeds it,
// shared by every machine that carries one -- the AV Quadras (PSC channels
// 1 and 2 at $50F1C000), the TNT Power Macs (Grand Central DBDMA channels 2
// and 3) and, by the same silicon, the PDM boards.  Machine-independent like
// bmac.c: what differs between them is the DMA engine in front of the core
// and what (if anything) is on the cable, and the machine supplies both
// through mace_ops_t.
//
// Scope: the thirty-two byte-wide registers on $10 centres with their reset
// values; CHIPID $0940 (the "B0" Curio); IAC with PADR/LADRF (LS byte first,
// PHYADDR/LOGADDR auto-clearing after 6/8 accesses, ADDRCHG immediate); IR
// read-to-clear and INTR = IR & ~IMR; the read-to-clear, wrapping MPC/RNTPC/
// RCVCC counters; SWRST and XMTFWU/RCVFWU self-clearing; transmit with
// APADXMT padding and the FCS, and Curio's ONE-DEEP transmit status (a
// second frame is not taken until XMTFS of the first has been read, and
// reading XMTRC before XMTFS is the order the drivers keep); receive with
// physical/broadcast/LADRF-hash/promiscuous matching, the FCS appended,
// ASTRPRCV stripping, and the four-byte RCVFS; the three loopback modes of
// UTR (internal ones never reach the cable).  No timing: a frame completes
// when the DMA engine ends it, and is received when the cable offers it.
//
// Contract: docs/internals/core/peripherals/mace.md; the hardware is
// docs/reference/machines/av/mace.md.

#ifndef GS_CORE_PERIPHERALS_MACE_H
#define GS_CORE_PERIPHERALS_MACE_H

#include "checkpoint.h"
#include "irq_line.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct mace mace_t;

// Register numbers (the data manual's, offset = n * $10 on Curio).
#define MACE_RCVFIFO  0
#define MACE_XMTFIFO  1
#define MACE_XMTFC    2
#define MACE_XMTFS    3
#define MACE_XMTRC    4
#define MACE_RCVFC    5
#define MACE_RCVFS    6
#define MACE_FIFOFC   7
#define MACE_IR       8
#define MACE_IMR      9
#define MACE_PR       10
#define MACE_BIUCC    11
#define MACE_FIFOCC   12
#define MACE_MACCC    13
#define MACE_PLSCC    14
#define MACE_PHYCC    15
#define MACE_CHIPIDLO 16
#define MACE_CHIPIDHI 17
#define MACE_IAC      18
#define MACE_LADRF    20
#define MACE_PADR     21
#define MACE_MPC      24
#define MACE_RNTPC    26
#define MACE_RCVCC    27
#define MACE_UTR      29

// IR / IMR
#define MACE_IR_XMTINT 0x01u
#define MACE_IR_RCVINT 0x02u
#define MACE_IR_MPCO   0x04u
// XMTFS
#define MACE_XMTFS_XMTSV 0x80u
// MACCC
#define MACE_MACCC_ENRCV 0x01u
#define MACE_MACCC_ENXMT 0x02u
#define MACE_MACCC_PROM  0x80u
// UTR loopback (bits 2:1)
#define MACE_UTR_LOOP_MASK 0x06u
#define MACE_UTR_EXTLPB    0x02u
#define MACE_UTR_INTLPB    0x04u
#define MACE_UTR_MENDECLPB 0x06u

// The Curio B0 core's chip ID ($0941 is the earlier A2).
#define MACE_CHIPID 0x0940u

// The longest frame the FIFOs carry: 1518 bytes plus the 4-byte FCS, with
// room for a driver that hands over a few bytes more.
#define MACE_FRAME_MAX 1536
#define MACE_RXQ_DEPTH 8 // frames the receive side holds before MPC counts

// What the machine puts around the core.  Every hook may be NULL.
typedef struct mace_ops {
    // A frame the transmitter put on the cable (no FCS); not called for the
    // internal loopback modes.
    void (*wire_tx)(void *ctx, const uint8_t *frame, int len);
    // A frame is waiting in the receive FIFO for the DMA engine.
    void (*rx_ready)(void *ctx);
    // The transmitter can take another frame (the status of the last one was
    // read, or ENXMT came on with a frame held).
    void (*tx_ready)(void *ctx);
} mace_ops_t;

// A core in its reset state, hooks unbound.
mace_t *mace_init(void);
void mace_delete(mace_t *m);
// Save / restore the core's state (its MACE_CP_FIELDS list in mace.c).  The
// hooks are untouched: the machine binds them on the fresh core.
void mace_checkpoint(const mace_t *m, checkpoint_t *cp);
void mace_restore(mace_t *m, checkpoint_t *cp);

// The core as an entry of a board's CP_FIELD list (checkpoint.h): OP is
// system_write_checkpoint_data or system_read_checkpoint_data, and picks
// mace_checkpoint or mace_restore.
#define MACE_CP(OP, cp, m)                          MACE_CP_##OP((cp), (m))
#define MACE_CP_system_write_checkpoint_data(cp, m) mace_checkpoint((m), (cp))
#define MACE_CP_system_read_checkpoint_data(cp, m)  mace_restore((m), (cp))

// Hardware reset (and BIUCC.SWRST): every register to its reset value, the
// FIFOs emptied, INTR dropped.  The station address RAM is kept.
void mace_reset(mace_t *m);

// INTR (level): IR & ~IMR != 0.
void mace_set_irq(mace_t *m, irq_fn fn, void *ctx);
void mace_set_ops(mace_t *m, const mace_ops_t *ops, void *ctx); // copied

// Register file: `reg` is the register number (offset >> 4 on Curio).
uint8_t mace_read(mace_t *m, unsigned reg);
// The same register without the read's side effects.
uint8_t mace_peek(const mace_t *m, unsigned reg);
void mace_write(mace_t *m, unsigned reg, uint8_t value);

// --- The DMA engine's side ----------------------------------------------

// TDTREQ: the transmit FIFO takes a frame (no status is pending -- Curio's
// one-deep status buffer -- and no frame is held for ENXMT).
bool mace_tx_ready(const mace_t *m);
// FIFO-direct transmit writes: bytes of the frame in flight, then its EOF.
// At EOF the frame is padded (APADXMT), given its FCS and sent: on the
// cable, back through the receiver (loopback), or both (EXTLPB).
void mace_tx_write(mace_t *m, const uint8_t *buf, int len);
void mace_tx_eof(mace_t *m);

// RDTREQ: length of the frame at the head of the receive FIFO (FCS
// included unless stripped), or 0 when none waits.
int mace_rx_pending(const mace_t *m);
// FIFO-direct receive: take the head frame into `buf` (at most `max` bytes)
// and its four status bytes RFS0-RFS3 into `rfs`; RCVINT is raised.
// Returns the frame length, 0 when nothing waits.
int mace_rx_take(mace_t *m, uint8_t *buf, int max, uint8_t rfs[4]);

// --- The cable's side ---------------------------------------------------

// A frame arrives on the cable (destination address first, no FCS).  It
// passes the receiver's enable, the loopback state and the address filter
// or is dropped; true when it was queued.
bool mace_wire_rx(mace_t *m, const uint8_t *frame, int len);

// The station address loaded through IAC/PADR (byte 0 first on the wire).
void mace_get_padr(const mace_t *m, uint8_t mac[6]);

// Counters for tests and diagnostics.
uint32_t mace_tx_frames(const mace_t *m); // frames transmitted (any mode)
uint32_t mace_rx_frames(const mace_t *m); // frames handed to the DMA engine

#endif // GS_CORE_PERIPHERALS_MACE_H
