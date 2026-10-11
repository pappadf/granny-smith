// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// new_age.h
// The "New Age" floppy controller — the NEC µPD72070 strapped into Apple
// mode — and the SuperDrive behind it (new_age.c).  One chip model behind a
// board backend, the way swim3.h is built: the AV Quadras decode it at
// $50F2A000, feed it from PSC DMA channel 3 and take its INT into the
// PSC's pseudo-VIA2 bit 5 (src/machines/av/new_age.c); everything the chip
// owns is here.
//
// Drive and media state (head position, motor, media, the object tree)
// lives in the shared floppy module (floppy.h, FLOPPY_TYPE_NEW_AGE); the
// chip only keeps what its registers, its command processor and its drive
// command latches own.
//
// Level of the model — sector, not flux.  In Apple mode the chip does the
// GCR nibblizing, the MFM framing, the sync groups, the checksums and the
// tag bytes in hardware, and the host only ever sees 512-byte sectors on
// the DMA channel (Apple's driver never sets the TB bit, so tags never
// cross the bus).  Raw Dump, the one path that would expose nibbles, is not
// modelled.
//
// Contract references: NEC, µPD72070 Advanced Floppy Disk Controller
// specification (the "New Age FDC" document); Apple, Macintosh Quadra
// 840AV and Centris 660AV Developer Note, Ch. 12; and the shipped driver
// (NewAgeDrvr.a, cited by routine name).  See
// docs/reference/machines/av/new-age.md and
// docs/internals/core/peripherals/new_age.md.

#ifndef GS_CORE_PERIPHERALS_NEW_AGE_H
#define GS_CORE_PERIPHERALS_NEW_AGE_H

#include <stdbool.h>
#include <stdint.h>

struct floppy;
struct scheduler;

// === What the board provides =================================================

// One DMA byte, as the chip's DMARQ/DMAAK/TC pins see it.
#define NEW_AGE_DMA_NONE 0 // no byte moved: the channel is closed or paused
#define NEW_AGE_DMA_OK   1 // one byte moved
#define NEW_AGE_DMA_TC   2 // one byte moved, and the board asserted terminal count with it

// `dma_put` streams a read byte toward memory, `dma_get` fetches a byte to
// write; both answer NEW_AGE_DMA_*.  Terminal count is the board's: on the
// AV it is the PSC set's count reaching zero on that byte.  `set_irq`
// drives the chip's INT pin level into the board's interrupt fabric.
typedef struct new_age_backend {
    int (*dma_put)(void *ctx, uint8_t value);
    int (*dma_get)(void *ctx, uint8_t *out);
    void (*set_irq)(void *ctx, bool level);
    void *ctx;
} new_age_backend_t;

// === Chip state ==============================================================

// Plain data first — the board checkpoints the struct positionally up to
// `fd` and re-binds the pointer tail with new_age_bind() after a restore.
typedef struct new_age {
    // --- host interface (command / execution / result phases) ---
    uint8_t phase; // na_phase_t (new_age.c)
    uint8_t cmd[16]; // command bytes accumulated
    uint8_t cmd_len;
    uint8_t cmd_expect; // total bytes this command carries
    uint8_t result[8]; // result bytes queued
    uint8_t result_len;
    uint8_t result_idx;
    // --- the interrupt cause Sense Interrupt Status reports ---
    uint8_t int_pending;
    uint8_t int_seek_end; // pending cause is seek-family (ST0 + PCN)
    uint8_t int_st0;
    uint8_t int_pcn;
    // --- configuration latches (reset by DRR bit 7) ---
    uint8_t drr; // data-rate register: PCS2:0 in 4:2, DRATE in 1:0
    uint8_t drive_type; // Select Drive Type (3 = Apple FDD)
    uint8_t perp; // Perpendicular Mode D1:D0
    uint8_t non_dma; // Specify ND
    // --- per-drive state the chip drives through ENBL/LSTRB ---
    uint8_t enabled; // ENBL0_b/ENBL1_b asserted (bit per drive)
    uint8_t drive_mfm[2]; // the drive's GCR/MFM mode latch (Set Drive Mode)
    uint8_t motor_ready[2]; // spindle up to speed: /Ready can assert
    uint8_t pcn[2]; // present cylinder number
    uint8_t seeking[2]; // a seek/recalibrate is in flight (D*B)
    uint8_t cstin_seen[2]; // the /CSTIN level the poller last reported
    double poll_quiet_ns; // no /CSTIN report before this instant
    // --- execution phase (the command in flight) ---
    uint8_t x_op; // opcode of the executing command
    uint8_t x_drive, x_head;
    uint8_t x_c, x_h, x_r, x_n, x_eot; // ID / range parameters
    uint8_t x_sc; // format: sectors per track
    uint8_t x_st0, x_st1, x_st2; // result status being assembled
    uint8_t x_tc; // terminal count seen
    uint8_t x_count; // sectors completed
    uint8_t eject_pending[2]; // Eject strobed; the medium leaves at the event

    // --- pointers / callbacks (not checkpointed; new_age_bind) ---
    struct floppy *fd;
    struct scheduler *sched;
    new_age_backend_t be;
} new_age_t;

// === Lifecycle ===============================================================

// Attach the drive module, the scheduler and the board's backend.  Call at
// board init and again after a checkpoint restore has overwritten the
// plain-data part.  Does not touch the register state.
void new_age_bind(new_age_t *na, struct floppy *fd, struct scheduler *sched, const new_age_backend_t *be);

// Register the chip's scheduler event types and start the /CSTIN poller
// (`restoring` = the poller's periodic event comes back with the
// checkpoint, so it is not armed again).  Once, at construction.
void new_age_register_events(new_age_t *na, bool restoring);

// Hardware /RESET (and the DRR's self-clearing software reset): idle, the
// configuration latches back to their defaults, INT low, every drive
// enable dropped, and the /CSTIN memory forgotten so a medium already in a
// drive is reported again once polling resumes.  The drive's own latches
// (motor, mode, head) and the media are the drive's, and survive.
void new_age_reset(new_age_t *na);

// === Register file ===========================================================
// Apple mode decodes three registers [µPD72070 Table 3.1].

#define NEW_AGE_REG_STATUS 0 // read STR (MSR) / write DRR
#define NEW_AGE_REG_DATA   1 // the data register (command/result FIFO)

uint8_t new_age_read(new_age_t *na, unsigned reg);
// The same register without the read's side effects (no result byte popped).
uint8_t new_age_peek(new_age_t *na, unsigned reg);
void new_age_write(new_age_t *na, unsigned reg, uint8_t value);

// MSR bits [µPD72070 §3.1.1].  D0I/D1I follow the firmware flowchart
// ("If drive is not installed, set DxI").
#define NEW_AGE_MSR_RQM 0x80u
#define NEW_AGE_MSR_DIO 0x40u
#define NEW_AGE_MSR_EXM 0x20u
#define NEW_AGE_MSR_CB  0x10u
#define NEW_AGE_MSR_D1I 0x08u
#define NEW_AGE_MSR_D0I 0x04u
#define NEW_AGE_MSR_D1B 0x02u
#define NEW_AGE_MSR_D0B 0x01u

// ST0 [µPD72070 §3.1.9]
#define NEW_AGE_ST0_ABNORMAL 0x40u
#define NEW_AGE_ST0_INVALID  0x80u
#define NEW_AGE_ST0_CSTIN    0xC0u // /CSTIN state change
#define NEW_AGE_ST0_SE       0x20u
#define NEW_AGE_ST0_EC       0x10u
#define NEW_AGE_ST0_NR       0x08u
#define NEW_AGE_ST0_HD       0x04u
#define NEW_AGE_ST0_FIN      0x02u // /CSTIN high: no medium
// ST1
#define NEW_AGE_ST1_EN 0x80u
#define NEW_AGE_ST1_DE 0x20u
#define NEW_AGE_ST1_OR 0x10u
#define NEW_AGE_ST1_ND 0x04u
#define NEW_AGE_ST1_NW 0x02u
#define NEW_AGE_ST1_MA 0x01u
// ST2
#define NEW_AGE_ST2_DD 0x20u
#define NEW_AGE_ST2_NC 0x10u
#define NEW_AGE_ST2_MD 0x01u

#endif // GS_CORE_PERIPHERALS_NEW_AGE_H
