// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// escc_dbdma.h
// The MacRISC ESCC face shared by Grand Central (TNT) and Heathrow
// (Gossamer): the +$13000 ESCC aperture's decode onto the shared SCC cell,
// and the four DBDMA channels (a transmit and a receive per SCC channel)
// that move serial bytes without the CPU.
//
// The aperture: B ctl +$00 / B data +$10 / A ctl +$20 / A data +$30, mapped
// onto the cell's classic address pins (A/B on bit 1, D/C on bit 2 -- the
// legacy-aperture layout the shared model decodes natively).  Only those
// four registers (+$00..+$3F) reach the cell; the aperture has others past
// them.  The Open Transport LocalTalk driver writes $00 then $01 to +$A0
// before the frames it sends, and decoding that as A control (bits 5:4 = 2)
// left channel A's register pointer at 1: the driver's next "select RR3"
// write on channel A then landed in WR1 as $03, turning on channel A's Tx
// and External/Status interrupts behind its back.
//
// The DBDMA ports: an OUTPUT command hands the port a run of bytes, which
// go to the channel's data register one at a time -- the same path as a
// CPU store to it, so local loopback (WR14 LOOP) and the cross-channel
// cable behave as they do under PIO; an INPUT command asks for what the
// channel has received, which the port serves from the receive queue and
// kicks again whenever a data register is written.  (Bytes arriving from
// the host side -- `machine.scc.a.receive` -- reach an INPUT program at its
// next kick; the Network Server Diagnostic Utility's DMA tests are
// loopbacks and never wait on that.)  Fitted to that utility's serial DMA
// tests (SccDMA: channel 4 OUTPUT of an 80-byte pattern, channel 5 INPUT of
// the loopback, byte-for-byte compare).
//
// What differs per controller is data: the DBDMA channel numbers of SCC
// channel A's transmit and receive (B's are two above each).

#ifndef GS_MACHINES_RUNTIME_ESCC_DBDMA_H
#define GS_MACHINES_RUNTIME_ESCC_DBDMA_H

#include <stdbool.h>
#include <stdint.h>

struct config;
struct dbdma;
struct scc;

// === Type Definitions =======================================================

struct escc_dbdma;

// One SCC channel's DBDMA port context.
typedef struct escc_dbdma_port {
    struct escc_dbdma *owner;
    unsigned ch; // 0 = A, 1 = B
} escc_dbdma_port_t;

// The ESCC's DBDMA wiring.  Rebuilt by escc_dbdma_init at construction;
// not checkpointed.  Its address is the ports' context, so it lives in the
// family state for the machine's lifetime.
typedef struct escc_dbdma {
    struct config *cfg; // the SCC cell is cfg->scc
    struct dbdma *dbdma; // NULL until init: the aperture then kicks nothing
    int tx_base; // channel A transmit; B is tx_base + 2
    int rx_base; // channel A receive; B is rx_base + 2
    escc_dbdma_port_t port[2];
} escc_dbdma_t;

// === Lifecycle ==============================================================

// Attach the four DBDMA ports (after dbdma_init and the SCC).
void escc_dbdma_init(escc_dbdma_t *e, struct config *cfg, struct dbdma *dbdma, int tx_base, int rx_base);

// === Operations =============================================================

// True when an aperture offset is one of the four channel registers.
bool escc_is_channel_reg(uint32_t off);

// One aperture byte (offset from +$13000): the guest's read, or an
// inspection (`peek`).  Offsets past the channel registers read 0.
uint8_t escc_aperture_read(struct scc *scc, uint32_t off, bool peek);

// One aperture store; the receive channels are kicked, as a data-register
// write may loop back.  Returns false (and touches nothing) for an offset
// past the channel registers, so the caller can log it.
bool escc_aperture_write(escc_dbdma_t *e, struct scc *scc, uint32_t off, uint8_t value);

#endif // GS_MACHINES_RUNTIME_ESCC_DBDMA_H
