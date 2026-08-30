// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// swim3.h
// The SWIM3 floppy controller — register file, Sony drive-register
// protocol, and the media transfer engine (swim3.c + swim3_xfer.c).  One
// model behind two boards: the PDM decodes it through the AMIC island and
// feeds it from the AMIC floppy DMA channel; the TNT family (and the
// Network Servers) puts it at Grand Central +$15000 on $10 centres and
// feeds it from DBDMA channel 1.  What differs between the boards — the
// register stride, the DMA byte movers, the interrupt sink — is the
// backend below; everything the chip owns is here.
//
// Drive and media state (head position, motor, media, the object tree)
// lives in the shared floppy module (floppy.h, FLOPPY_TYPE_SWIM3); the
// chip only keeps what its registers and engine own.
//
// Contract references: Apple, SWIM3 Engineering Requirements
// Specification v1.2 (3/24/93); Apple, "Guide to the Macintosh Family
// Hardware", 2nd ed.; Apple, "Power Macintosh Computers" Developer Note
// (1994), Table 3-7.  See docs/core/peripherals/swim3.md.

#ifndef GS_CORE_PERIPHERALS_SWIM3_H
#define GS_CORE_PERIPHERALS_SWIM3_H

#include <stdbool.h>
#include <stdint.h>

struct floppy;
struct scheduler;

// === What the board provides =================================================

// The DMA channel as the engine uses it: RUN/direction state and one byte
// in either direction (address advance, count and terminal-count interrupt
// are the mover's business).  `put` streams a read byte toward memory and
// `get` fetches a byte to write; either returns false when the channel is
// closed or cannot move the byte — the engine treats that as an over/under-
// run, exactly as the chip's FIFO would.  `set_irq` drives the chip's IRQ
// pin level into the board's interrupt fabric.
typedef struct swim3_backend {
    bool (*dma_running)(void *ctx);
    bool (*dma_get)(void *ctx, uint8_t *out);
    bool (*dma_put)(void *ctx, uint8_t value);
    void (*set_irq)(void *ctx, bool level);
    void *ctx;
} swim3_backend_t;

// === Chip state ==============================================================

// Plain data first — a board checkpoints the struct positionally as part
// of its own state and re-binds the pointer tail with swim3_bind() after a
// restore (the av/cuda pattern).
typedef struct swim3 {
    // reg 1 Timer: a 1 us countdown (SWIM3-ERS:76).  `timer` is the loaded
    // value, `timer_start_ns` the scheduler time of the load; the running
    // count reads back live and TIMER_DONE fires at zero (swim3.c).  The
    // 7.5 .Sony driver never touches it; Copland's floppy plugin is built
    // on it (SwimIIISmallWait polls it — gs-docs/projects/copland).
    uint8_t timer;
    uint8_t timer_running;
    uint64_t timer_start_ns;
    uint8_t param; // reg 3 ParamData
    uint8_t phase; // reg 4 CA0-2/LSTRB lines (probe loopback readback)
    uint8_t setup; // reg 5 (bit 7 SoftReset self-clears)
    uint8_t mode; // reg 6 read; written via Zeroes (reg 6) / Ones (reg 7)
    uint8_t intr; // reg 8, read-to-clear
    uint8_t step, ctrack, csect, gap, sector, nsect; // regs 9-14 storage
    uint8_t intmask; // reg 15, R/W
    uint8_t error; // reg 2, read-to-clear
    uint8_t motor_on; // drive-1 spindle latch (strobe-controlled)
    uint8_t mfm_mode; // drive mode latch; forgotten at motor-off (§11.12)
    uint8_t step_dir; // step-direction latch (sense addr 0); 1 = outward
    uint8_t fmt_byte; // reg 12 read side: 4th address-field byte
    // --- transfer engine (swim3_xfer.c) ---
    uint8_t engine_running; // an engine event is armed (GO seen)
    uint8_t xfer_side; // head the sense address last routed (0/1)
    uint8_t eject_pending; // wEjectOn strobed; the media goes at the timeout
    uint32_t fmt_sectors; // sectors the last format stream declared
    uint8_t xfer_any; // FirstSector matched; the rest of SectorsToXfer are
                      // "accessed continuously" (ERS reg $E): every header
                      // that passes is the next one, no match required

    // --- pointers / callbacks (not checkpointed; swim3_bind) ---
    struct floppy *fd;
    struct scheduler *sched;
    swim3_backend_t be;
} swim3_t;

// === Lifecycle ===============================================================

// Attach the drive module, the scheduler and the board's backend.  Call at
// board init and again after a checkpoint restore has overwritten the
// plain-data part.  Does not touch the register state.
void swim3_bind(swim3_t *sw, struct floppy *fd, struct scheduler *sched, const swim3_backend_t *be);

// Register the chip's scheduler event types — before scheduler_start
// (the timer in swim3.c, the transfer engine in swim3_xfer.c).
void swim3_register_events(swim3_t *sw);
void swim3_xfer_register_events(swim3_t *sw);

// === Register file ===========================================================
// `reg` is the register INDEX 0..15 (the board decodes its own stride:
// $200 on the PDM, $10 behind Grand Central).

uint8_t swim3_read(swim3_t *sw, unsigned reg);
void swim3_write(swim3_t *sw, unsigned reg, uint8_t value);

// The IRQ pin follows ENABLE_INTS & (intr & intmask); the interrupt sources
// set their flag regardless of the mask.
void swim3_update_irq(swim3_t *sw);
void swim3_raise(swim3_t *sw, uint8_t bits);

// Interrupt register bits (§7.9)
#define SWIM3_INT_TIMER 0x01u
#define SWIM3_INT_STEP  0x02u
#define SWIM3_INT_ID    0x04u
#define SWIM3_INT_DONE  0x08u
#define SWIM3_INT_SENSE 0x10u

// Mode register bits (§7.7)
#define SWIM3_M_ENABLE_INTS 0x01u
#define SWIM3_M_DRIVE1      0x02u
#define SWIM3_M_DRIVE2      0x04u
#define SWIM3_M_ACTION      0x08u // GO
#define SWIM3_M_WRITE       0x10u
#define SWIM3_M_HEADSEL     0x20u
#define SWIM3_M_FORMAT      0x40u
#define SWIM3_M_GOSTEP      0x80u

// Setup register bits (§7.6)
#define SWIM3_S_COPYPROT   0x02u
#define SWIM3_S_GCR        0x04u
#define SWIM3_S_DISGCRCONV 0x10u
#define SWIM3_S_IBMDRIVE   0x20u

// Error register bits (§7.3)
#define SWIM3_E_UNDERRUN 0x01u
#define SWIM3_E_OVERRUN  0x04u
#define SWIM3_E_CRC_ADDR 0x40u
#define SWIM3_E_CRC_DATA 0x80u

// === Transfer engine (swim3_xfer.c) =========================================

// Mode-register edges: GO or GoStep just became set / cleared.
void swim3_engine_update(swim3_t *sw);
// Media facts the register file asks the engine for.
bool swim3_media_is_hd(swim3_t *sw);
int swim3_index_pulse(swim3_t *sw);

#endif // GS_CORE_PERIPHERALS_SWIM3_H
