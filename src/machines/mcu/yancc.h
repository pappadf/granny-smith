// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// yancc.h
// The NuBus controller's register file of the 68040 Quadras: the Quadra
// 700/900/950's YANCC and the MEMC boards' Kiwi, which Apple made
// software-compatible with it ("Kiwi IC", Centris 610/650/Quadra 800
// Developer Note).  A board-independent chip piece, like the IOSB's
// (memc/iosb.h): each board embeds a yancc_t and routes its island window
// ($28000) to yancc_read / yancc_write.
//
// The register address is Apple-documented, its width and most bits are
// not, so the file is latch-and-readback under a log: 64 longword slots,
// every first touch logged, so the ROM's and the drivers' access sequence is
// recoverable as an RE artifact.  Actual NuBus transactions run through the
// memory map and the nubus core directly.
//
// The control/status bits are one per longword: bit n is bit 0 of the long
// at offset n*4 (MacTest Pro's NuBus Controller test packs and unpacks the
// file that way).  Two of them form the write-buffer error interrupt: bit 7
// is the error flag and bit 10 its enable.  With both set the controller
// requests level 7, the NMI line it shares ("NMI/YANCC err", Quadra 700
// Developer Note Table 2-7); clearing either drops the request.  The test
// arms bit 10, sets bit 7 25 times and expects 25 level-7 interrupts, its
// handler clearing bit 7 each time.  Nothing on the modelled NuBus sets the
// flag on its own: the write buffer never reports an error.

#ifndef GS_MACHINES_MCU_YANCC_H
#define GS_MACHINES_MCU_YANCC_H

#include <stdbool.h>
#include <stdint.h>

#define YANCC_REG_COUNT  64 // longword slots ($28000-$280FF, repeating through the window)
#define YANCC_BIT_ERR    7 // write-buffer error flag
#define YANCC_BIT_ERR_EN 10 // its interrupt enable

// The error interrupt's sink: `active` is the level-7 request.
typedef void (*yancc_irq_fn)(void *context, bool active);

// One controller's register file and its interrupt wire.
typedef struct yancc {
    uint32_t regs[YANCC_REG_COUNT]; // the latched longwords (checkpointed)
    uint64_t touched; // log-once bitmap, one bit per slot (checkpointed with regs)
    const char *name; // log prefix: "YANCC" or "Kiwi"
    yancc_irq_fn err_irq; // the level-7 request's sink
    void *irq_ctx;
} yancc_t;

// Wire the controller: its log name and the error interrupt's sink.  The
// register file keeps its contents (zero in a freshly allocated state).
void yancc_init(yancc_t *y, const char *name, yancc_irq_fn err_irq, void *context);

// Clear the register file and drop a pending error request (/RESET).
void yancc_reset(yancc_t *y);

// True while the error flag and its enable are both set.
bool yancc_err_irq(const yancc_t *y);

// One byte of the window at `off` (window-relative); `peek` reads without
// the first-touch log.  `pc` is the CPU's, for the log.
uint8_t yancc_read(yancc_t *y, uint32_t off, bool peek, uint32_t pc);

// A byte write at `off`; re-drives the error request when it changes.
void yancc_write(yancc_t *y, uint32_t off, uint8_t value, uint32_t pc);

#endif // GS_MACHINES_MCU_YANCC_H
