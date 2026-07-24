// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi_53c96.c
// NCR 53C96 chip model — see scsi_53c96.h.  Register semantics follow the
// NCR 53C94/95/96 Data Manual ch. 4 (register file) and ch. 5 (command set);
// section references below are to that manual.  Phase C implements the
// disconnected-state behavior the boot ROM exercises; the target-transfer
// machinery arrives with the bus attachment in Phase E.

#include "scsi_53c96.h"

#include "log.h"
#include "scheduler.h"
#include "system.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("scsi96");

// Register addresses (A3..A0)
#define R_XFER_LO   0x0 // r: transfer counter / w: transfer count
#define R_XFER_HI   0x1
#define R_FIFO      0x2
#define R_COMMAND   0x3
#define R_STATUS    0x4 // r; w: destination ID
#define R_INTERRUPT 0x5 // r; w: select/reselect time-out
#define R_SEQSTEP   0x6 // r; w: synchronous transfer period
#define R_FIFOFLAGS 0x7 // r; w: synchronous offset
#define R_CONFIG1   0x8
#define R_CLOCKCONV 0x9 // w
#define R_TEST      0xA // w (test mode)
#define R_CONFIG2   0xB
#define R_CONFIG3   0xC
#define R_CONFIG4   0xD // 53C96 only
#define R_TC_HIGH   0xE // 24-bit count extension (Config 2 feature)

// Status register bits (Figure 4-2)
#define ST_INT   0x80
#define ST_GE    0x40
#define ST_PE    0x20
#define ST_TC    0x10
#define ST_VGC   0x08
#define ST_PHASE 0x07

// Interrupt register bits (Figure 4-3)
#define IR_SCSI_RST      0x80
#define IR_ILL_CMD       0x40
#define IR_DISCONNECT    0x20
#define IR_BUS_SERVICE   0x10
#define IR_FUNC_COMPLETE 0x08
#define IR_RESELECTED    0x04
#define IR_SEL_ATN       0x02
#define IR_SELECTED      0x01

#define FIFO_DEPTH 16

struct scsi_53c96 {
    // Programmer-visible register file
    uint16_t xfer_count; // write side (reload value)
    uint16_t xfer_counter; // read side (live counter)
    uint8_t fifo[FIFO_DEPTH];
    uint8_t fifo_count;
    uint8_t fifo_rd; // read cursor (bottom of FIFO)
    uint8_t command; // last executed command
    uint8_t status;
    uint8_t dest_id;
    uint8_t intr;
    uint8_t timeout; // select/reselect time-out register
    uint8_t seq_step;
    uint8_t sync_period;
    uint8_t sync_offset;
    uint8_t config1;
    uint8_t config2;
    uint8_t config3;
    uint8_t config4;
    uint8_t clock_conv;

    bool int_line; // INT output level
    bool sel_enabled; // enable selection/reselection latch

    uint32_t clock_hz; // chip clock (time-out scaling)

    struct scheduler *sched;
    scsi_53c96_irq_cb irq_cb;
    void *irq_ctx;
};

// Drive the INT output (and Status bit 7 mirror).
static void set_int(scsi_53c96_t *c, bool active) {
    if (active)
        c->status |= ST_INT;
    else
        c->status &= (uint8_t)~ST_INT;
    if (c->int_line != active) {
        c->int_line = active;
        if (c->irq_cb)
            c->irq_cb(c->irq_ctx, active);
    }
}

// Post an interrupt cause: latch the bits and raise INT.
static void post_interrupt(scsi_53c96_t *c, uint8_t bits) {
    c->intr |= bits;
    set_int(c, true);
}

// Selection time-out event: no target responded to a select sequence.
// The chip disconnects and raises the Disconnect interrupt (ch. 5, select
// sequences: "if the target does not respond within the time-out period").
static void select_timeout_event(void *source, uint64_t data) {
    (void)data;
    scsi_53c96_t *c = (scsi_53c96_t *)source;
    c->seq_step = 0; // no progress through the selection algorithm
    post_interrupt(c, IR_DISCONNECT);
}

// Selection time-out in nanoseconds: RV * 8192 * clock-conversion / clock
// (write address 05).  A clock-conversion register value of 0 means 8.
static uint64_t select_timeout_ns(scsi_53c96_t *c) {
    uint32_t ccf = c->clock_conv & 7 ? (c->clock_conv & 7) : 8;
    uint64_t ticks = (uint64_t)c->timeout * 8192ull * ccf;
    if (c->clock_hz == 0)
        return 250000000ull; // defensive: ANSI-standard 250 ms
    return ticks * 1000000000ull / c->clock_hz;
}

// FIFO helpers.  The bottom element and flags clear on chip reset; contents
// otherwise persist (ch. 4, FIFO register).
static void fifo_flush(scsi_53c96_t *c) {
    c->fifo_count = 0;
    c->fifo_rd = 0;
    c->fifo[0] = 0;
}

static void fifo_push(scsi_53c96_t *c, uint8_t v) {
    if (c->fifo_count >= FIFO_DEPTH) {
        c->status |= ST_GE; // top of FIFO overwritten (gross error)
        return;
    }
    c->fifo[(c->fifo_rd + c->fifo_count) % FIFO_DEPTH] = v;
    c->fifo_count++;
}

static uint8_t fifo_pop(scsi_53c96_t *c) {
    uint8_t v = c->fifo[c->fifo_rd];
    if (c->fifo_count > 0) {
        c->fifo_count--;
        c->fifo_rd = (uint8_t)((c->fifo_rd + 1) % FIFO_DEPTH);
    }
    return v; // empty FIFO re-reads the bottom register
}

// Chip reset: same effect as hardware reset (ch. 5).  Time-out, transfer
// count, destination ID, and clock conversion survive per ch. 4.
static void chip_reset(scsi_53c96_t *c) {
    fifo_flush(c);
    c->status = 0;
    c->intr = 0;
    c->seq_step = 0;
    c->sync_period = 5; // defaults to 5 after reset (write addr 06)
    c->sync_offset = 0; // cleared by reset (write addr 07)
    c->config1 &= 0x07; // My Bus ID survives? [U] — keep the ID bits, clear modes
    c->config2 = 0;
    c->config3 = 0;
    c->sel_enabled = false;
    set_int(c, false);
    if (c->sched)
        remove_event(c->sched, select_timeout_event, c);
}

void scsi_53c96_reset(scsi_53c96_t *c) {
    if (!c)
        return;
    chip_reset(c);
}

// Execute a command that fell to the bottom of the command register.
static void execute_command(scsi_53c96_t *c, uint8_t cmd) {
    uint8_t code = cmd & 0x7F;
    bool dma = (cmd & 0x80) != 0;
    LOG(3, "53C96 command $%02X (dest=%u timeout=$%02X)", cmd, c->dest_id, c->timeout);

    switch (code) {
    case 0x00: // NOP
        if (dma) {
            c->xfer_counter = c->xfer_count; // DMA NOP loads the counter
            c->status &= (uint8_t)~ST_TC; // loading clears terminal count
        }
        break;
    case 0x01: // Flush FIFO
        fifo_flush(c);
        break;
    case 0x02: // Reset chip
        chip_reset(c);
        break;
    case 0x03: // Reset SCSI bus
        // No targets yet (Phase E); the reset itself is a no-op on the wire.
        // Interrupt only when reset reporting is enabled (Config 1 bit 6 = 0).
        if (!(c->config1 & 0x40))
            post_interrupt(c, IR_SCSI_RST);
        break;
    case 0x40: // Reselect sequence
    case 0x41: // Select without ATN
    case 0x42: // Select with ATN
    case 0x43: // Select with ATN and stop
    case 0x46: // Select with ATN3
        // Disconnected-state selects: with no attached targets every select
        // ends in a selection time-out (Phase E connects the bus).
        if (c->sched)
            scheduler_new_cpu_event(c->sched, select_timeout_event, c, 0, 0, select_timeout_ns(c));
        else
            select_timeout_event(c, 0);
        break;
    case 0x44: // Enable selection/reselection
        c->sel_enabled = true;
        break;
    case 0x45: // Disable selection/reselection
        c->sel_enabled = false;
        post_interrupt(c, IR_FUNC_COMPLETE);
        break;
    case 0x1A: // Set ATN
    case 0x1B: // Reset ATN
        break; // wire-level; no observable effect without a bus
    default:
        // Commands from a mode group the chip is not in (it is always
        // disconnected until Phase E) raise the illegal-command interrupt.
        post_interrupt(c, IR_ILL_CMD);
        LOG(2, "53C96 illegal/unimplemented command $%02X", cmd);
        break;
    }
}

uint8_t scsi_53c96_read(scsi_53c96_t *c, uint32_t reg) {
    if (!c)
        return 0;
    switch (reg & 0xF) {
    case R_XFER_LO:
        return (uint8_t)c->xfer_counter;
    case R_XFER_HI:
        return (uint8_t)(c->xfer_counter >> 8);
    case R_FIFO:
        return fifo_pop(c);
    case R_COMMAND:
        return c->command;
    case R_STATUS:
        return c->status;
    case R_INTERRUPT: {
        // Reading the interrupt register releases INT and clears the
        // status latches + sequence step (ch. 4, interrupt register).
        uint8_t v = c->intr;
        if (c->int_line) {
            c->intr = 0;
            c->seq_step = 0;
            c->status &= (uint8_t) ~(ST_GE | ST_PE | ST_VGC);
            set_int(c, false);
        }
        return v;
    }
    case R_SEQSTEP:
        return (uint8_t)(c->seq_step | 0x08); // SOM idle (active-low, not at max)
    case R_FIFOFLAGS:
        return (uint8_t)(((c->seq_step & 7) << 5) | (c->fifo_count & 0x1F));
    case R_CONFIG1:
        return c->config1;
    case R_CONFIG2:
        return c->config2;
    case R_CONFIG3:
        return c->config3;
    case R_CONFIG4:
        return c->config4;
    default:
        return 0;
    }
}

void scsi_53c96_write(scsi_53c96_t *c, uint32_t reg, uint8_t value) {
    if (!c)
        return;
    switch (reg & 0xF) {
    case R_XFER_LO:
        c->xfer_count = (uint16_t)((c->xfer_count & 0xFF00) | value);
        break;
    case R_XFER_HI:
        c->xfer_count = (uint16_t)((c->xfer_count & 0x00FF) | ((uint16_t)value << 8));
        break;
    case R_FIFO:
        fifo_push(c, value);
        break;
    case R_COMMAND:
        // Two-deep in hardware; immediate execution suffices while every
        // implemented command completes synchronously or via one event.
        c->command = value;
        execute_command(c, value);
        break;
    case R_STATUS: // write: destination ID
        c->dest_id = value & 0x07;
        break;
    case R_INTERRUPT: // write: select/reselect time-out
        c->timeout = value;
        break;
    case R_SEQSTEP: // write: synchronous transfer period
        c->sync_period = value & 0x1F;
        break;
    case R_FIFOFLAGS: // write: synchronous offset
        c->sync_offset = value & 0x0F;
        break;
    case R_CONFIG1:
        c->config1 = value;
        break;
    case R_CLOCKCONV:
        c->clock_conv = value & 0x07;
        break;
    case R_TEST:
        LOG(2, "53C96 test-mode write $%02X ignored", value);
        break;
    case R_CONFIG2:
        c->config2 = value;
        break;
    case R_CONFIG3:
        c->config3 = value;
        break;
    case R_CONFIG4:
        c->config4 = value;
        break;
    default:
        break;
    }
}

scsi_53c96_t *scsi_53c96_init(struct scheduler *sched, uint32_t clock_hz, checkpoint_t *cp) {
    scsi_53c96_t *c = (scsi_53c96_t *)calloc(1, sizeof(scsi_53c96_t));
    if (!c)
        return NULL;
    c->sched = sched;
    c->clock_hz = clock_hz;
    chip_reset(c);
    if (cp) {
        // Restore the plain-data prefix; pointers/callbacks re-bind after.
        scsi_53c96_t saved;
        system_read_checkpoint_data(cp, &saved, sizeof(saved));
        saved.sched = sched;
        saved.clock_hz = clock_hz;
        saved.irq_cb = NULL;
        saved.irq_ctx = NULL;
        *c = saved;
    }
    if (sched)
        scheduler_new_event_type(sched, "scsi96", c, "select_timeout", select_timeout_event);
    return c;
}

void scsi_53c96_delete(scsi_53c96_t *c) {
    if (!c)
        return;
    if (c->sched)
        remove_event(c->sched, select_timeout_event, c);
    free(c);
}

void scsi_53c96_checkpoint(scsi_53c96_t *c, checkpoint_t *cp) {
    if (!c || !cp)
        return;
    system_write_checkpoint_data(cp, c, sizeof(*c));
}

void scsi_53c96_set_irq_callback(scsi_53c96_t *c, scsi_53c96_irq_cb cb, void *context) {
    if (!c)
        return;
    c->irq_cb = cb;
    c->irq_ctx = context;
    // Re-drive the current level so a restore re-establishes the VIA input.
    if (cb)
        cb(context, c->int_line);
}
