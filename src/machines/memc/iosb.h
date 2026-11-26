// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iosb.h
// IOSB, the I/O subsystem and buffer of the MEMC boards (Centris 610/650,
// Quadra 610/650/800) -- written as a board-independent piece so the later
// boards built around the same I/O chip can reuse it.
//
// What the chip holds, and where this model keeps each part:
//
//   * VIA1 and VIA2 -- the shared 6522 model (via.c), built by the board.
//     The IOSB adds one wire of its own: the 53C96's DREQ is presented in
//     VIA2's interrupt-flag register, bit 0 (the CA2 position), which is
//     where the SCSI Manager polls it during pseudo-DMA.  iosb_via2_* below
//     front VIA2's IFR for that.
//   * the SWIM II -- the shared SWIM model (floppy_swim.c).
//   * the sound block -- an EASC-compatible ASC (asc.c).
//   * I/O address decode and dynamic bus sizing -- the shared board_io
//     engine decomposes wide accesses to 8- and 16-bit devices into byte
//     lanes, which is the IOSB's byte steering.
//   * its own configuration registers (this file), at island $18000, one
//     byte register per $100: bus-clock select ($000), a second config
//     ($100), SONIC/SCSI pseudo-DMA timing ($200), the chip's revision and
//     pseudo-DMA control ($300), the latched SCSI residual ($400),
//     brightness/contrast ($500/$501) and a watchdog timeout ($600).
//     The bits the ROM writes latch and read back; the revision field reads
//     as the part the board carries.

#ifndef GS_MACHINES_MEMC_IOSB_H
#define GS_MACHINES_MEMC_IOSB_H

#include "checkpoint.h"

#include <stdbool.h>
#include <stdint.h>

struct config;
struct via;

#define IOSB_REG_COUNT  8 // one register per $100 of the $2000 window
#define IOSB_REG_CONFIG 0 // bit 0: bus clock 20/25 MHz (1) or 33/40 MHz (0)
#define IOSB_REG_REV    3 // bits 7-5: revision; bits 2-0: pseudo-DMA control

// VIA2's interrupt-flag register within the VIA2 window (register 13 on the
// $200 register spacing).
#define IOSB_VIA2_IFR_OFF 0x1A00u

typedef bool (*iosb_dreq_fn)(void *context);

typedef struct iosb {
    uint8_t regs[IOSB_REG_COUNT][2]; // two byte lanes per register ($n00, $n01)
    // The 53C96 DREQ source that VIA2 IFR bit 0 presents.
    iosb_dreq_fn dreq;
    void *dreq_ctx;
} iosb_t;

// Power-on state: `bus_33mhz` selects the bus-clock bit's reset value.
void iosb_init(iosb_t *iosb, bool bus_33mhz);

// Wire the SCSI DREQ that VIA2 IFR bit 0 shows.
void iosb_set_dreq(iosb_t *iosb, iosb_dreq_fn fn, void *context);

// Configuration registers, as board_io handler rows (window offset 0-$1FFF).
uint8_t iosb_reg_read(iosb_t *iosb, uint32_t win_off);
void iosb_reg_write(iosb_t *iosb, uint32_t win_off, uint8_t value);

// VIA2's IFR with the DREQ bit: `ifr` is what the 6522 itself returns.
uint8_t iosb_via2_ifr(const iosb_t *iosb, uint8_t ifr);

// Checkpoint the register file (the DREQ wiring is rebuilt by the board).
void iosb_checkpoint(const iosb_t *iosb, checkpoint_t *cp);
void iosb_restore(iosb_t *iosb, checkpoint_t *cp);

#endif // GS_MACHINES_MEMC_IOSB_H
