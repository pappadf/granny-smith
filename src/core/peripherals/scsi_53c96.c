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
#include "scsi.h"
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

    // Active DMA transfer through the pseudo-DMA aperture
    uint8_t xfer_mode; // XFER_* below
    uint32_t counter_live; // live byte counter (0 write = 65536)
    uint32_t xfer_residual; // bytes held back in the FIFO at INT time (odd = 1)
    uint8_t xfer_int_done; // completion INT already posted for this transfer

    struct scheduler *sched;
    struct scsi *bus; // bus/target model (NULL until attached)
    scsi_53c96_irq_cb irq_cb;
    void *irq_ctx;
};

// Pseudo-DMA transfer modes
#define XFER_IDLE     0
#define XFER_CMD_OUT  1 // CDB continues via the aperture (DMA select)
#define XFER_DATA_IN  2
#define XFER_DATA_OUT 3

// Map the bus model's phase to the 53C96 status-register phase field
// (MSG/CD/IO wire encoding; Figure 4-2).
static uint8_t phase_bits(int p) {
    switch (p) {
    case scsi_data_out:
        return 0x0;
    case scsi_data_in:
        return 0x1;
    case scsi_command:
        return 0x2;
    case scsi_status:
        return 0x3;
    case scsi_message_out:
        return 0x6;
    case scsi_message_in:
        return 0x7;
    default:
        return 0x0;
    }
}

// Refresh the status-register phase field from the live bus.
static void refresh_phase(scsi_53c96_t *c) {
    if (!c->bus)
        return;
    c->status = (uint8_t)((c->status & ~ST_PHASE) | phase_bits(scsi_get_bus_phase(c->bus)));
}

static void pdma_out_byte(scsi_53c96_t *c, uint8_t value);
static uint8_t pdma_in_byte(scsi_53c96_t *c);

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
    LOG(3, "post int $%02X (intr=$%02X)", bits, c->intr);
    set_int(c, true);
}

// Selection time-out event: no target responded to a select sequence.
// The chip disconnects and raises the Disconnect interrupt (ch. 5, select
// sequences: "if the target does not respond within the time-out period").
static void select_timeout_event(void *source, uint64_t data) {
    (void)data;
    scsi_53c96_t *c = (scsi_53c96_t *)source;
    LOG(3, "select timeout fires (dest=%u)", c->dest_id);
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
    c->xfer_mode = XFER_IDLE;
    if (c->bus)
        scsi_external_release(c->bus);
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
    LOG(3, "53C96 command $%02X (dest=%u)", cmd, c->dest_id);

    switch (code) {
    case 0x00: // NOP
        if (dma) {
            c->xfer_counter = c->xfer_count; // DMA NOP loads the counter
            c->status &= (uint8_t)~ST_TC; // loading clears terminal count
        }
        break;
    case 0x01: // Flush FIFO
        // Clears the FIFO only: "initializes the FIFO to the empty condition
        // by resetting the FIFO flags" (ch. 5, Flush FIFO). [D]  It must NOT
        // abandon a paused DMA-select sequence: the System 7.1 HD driver
        // flushes the FIFO between the DMA select and pushing the CDB through
        // the pseudo-DMA aperture; clearing xfer_mode here dropped those CDB
        // bytes, the target never left COMMAND phase, and every SCSI Manager
        // READ(10) timed out (ioErr) after the "Welcome" splash.
        fifo_flush(c);
        break;
    case 0x02: // Reset chip
        chip_reset(c);
        break;
    case 0x03: // Reset SCSI bus
        c->xfer_mode = XFER_IDLE;
        if (c->bus)
            scsi_external_release(c->bus);
        // Interrupt only when reset reporting is enabled (Config 1 bit 6 = 0).
        if (!(c->config1 & 0x40))
            post_interrupt(c, IR_SCSI_RST);
        break;
    case 0x41: // Select without ATN
    case 0x42: // Select with ATN
    case 0x43: // Select with ATN and stop
    case 0x46: // Select with ATN3
    {
        if (!c->bus || !scsi_external_select(c->bus, c->dest_id)) {
            // No device at the destination ID: selection time-out.
            if (c->sched)
                scheduler_new_cpu_event(c->sched, select_timeout_event, c, 0, 0, select_timeout_ns(c));
            else
                select_timeout_event(c, 0);
            break;
        }
        // Message byte(s) first for the ATN variants (IDENTIFY etc.) —
        // informational to the v1 target model; consumed from the FIFO.
        int msg_bytes = (code == 0x41) ? 0 : (code == 0x46) ? 3 : 1;
        for (int i = 0; i < msg_bytes && c->fifo_count > 0; i++)
            (void)fifo_pop(c);
        if (code == 0x43) {
            // Select-with-ATN-and-stop: halt after the message byte.
            c->seq_step = 1;
            refresh_phase(c);
            post_interrupt(c, IR_FUNC_COMPLETE | IR_BUS_SERVICE);
            break;
        }
        // CDB from the FIFO; run_cmd dispatches on the full CDB and moves
        // the bus out of COMMAND phase.
        while (c->fifo_count > 0 && scsi_get_bus_phase(c->bus) == scsi_command)
            scsi_push_data_out_byte(c->bus, fifo_pop(c));
        if (scsi_get_bus_phase(c->bus) != scsi_command) {
            c->seq_step = 4; // completed the whole select sequence
            if (dma) {
                c->counter_live = c->xfer_count ? c->xfer_count : 65536u;
                c->xfer_counter = (uint16_t)c->counter_live;
                c->status &= (uint8_t)~ST_TC;
            }
            refresh_phase(c);
            post_interrupt(c, IR_FUNC_COMPLETE | IR_BUS_SERVICE);
        } else {
            // Target selected, now in command phase, but the CDB has not
            // been supplied yet (the boot ROM flushes the FIFO before the
            // DMA select and feeds the CDB through the FIFO register /
            // pseudo-DMA port afterward).  Arm the command aperture at
            // sequence step 2.
            c->seq_step = 2;
            c->xfer_mode = XFER_CMD_OUT;
            refresh_phase(c);
            // A DMA select is still *executing* here: the chip has won the
            // bus and the target has entered the phase the sequence expects,
            // so it raises DREQ for the remaining command bytes and stays
            // silent.  An interrupt at this point means "selected, but the
            // target went somewhere unexpected", and SCSI Manager 4.3's
            // DoSelect reacts by clearing its NeedCmdSent flag — after which
            // DoCommand refuses to send the CDB at all (HALc96.a DoSelect
            // @waitLoop / @doneWithSel).  Non-DMA selects have no DREQ to
            // wait on, so they still get the interrupt.
            if (!dma)
                post_interrupt(c, IR_FUNC_COMPLETE | IR_BUS_SERVICE);
        }
        break;
    }
    case 0x40: // Reselect sequence (target role — not modeled)
        post_interrupt(c, IR_ILL_CMD);
        break;
    case 0x10: // Transfer information
    case 0x11: // Initiator command complete sequence
    case 0x12: // Message accepted
        if (!c->bus) {
            post_interrupt(c, IR_ILL_CMD);
            break;
        }
        if (code == 0x10) {
            int ph = scsi_get_bus_phase(c->bus);
            if (dma) {
                c->counter_live = c->xfer_count ? c->xfer_count : 65536u;
                c->xfer_counter = (uint16_t)c->counter_live;
                c->status &= (uint8_t)~ST_TC;
            }
            if (ph == scsi_data_in) {
                if (dma) {
                    // Pseudo-DMA read: arm the transfer and let the CPU pull
                    // the counted bytes through the aperture — each aperture
                    // read decrements the counter, exactly as a real DACK
                    // does on the DMA-side of the chip's FIFO.  DRQ is
                    // asserted (Terminal Count is reported up front because
                    // the count is latched and the FIFO is ready) so the
                    // ROM's polled drain loop, which checks TC BEFORE its
                    // MOVE.W drain, proceeds.  The completion interrupt is
                    // posted from pdma_in_byte when the counter reaches the
                    // FIFO residual: the 53C96 reserves the trailing odd byte
                    // in its FIFO, so for an odd-length transfer the completion
                    // interrupt fires once the CPU has drained (count-1) bytes
                    // through the aperture, and the driver then reads that last
                    // byte from the FIFO register.  For an even count the
                    // residual is 0 and the interrupt fires when the whole
                    // count has drained.  BOTH ROM drain shapes rely on this:
                    // the fixed 8-word chunk loop uses even counts, while the
                    // Duff's-device blind drain reads (count-1) words then waits
                    // for this interrupt before pulling the odd byte.
                    c->xfer_mode = XFER_DATA_IN;
                    c->xfer_residual = c->counter_live & 1u;
                    c->xfer_int_done = 0;
                    c->status |= ST_TC;
                } else {
                    uint8_t b;
                    if (scsi_pop_data_in_byte(c->bus, &b))
                        fifo_push(c, b);
                    scsi_external_data_in_complete(c->bus);
                    refresh_phase(c);
                    post_interrupt(c, IR_BUS_SERVICE);
                }
            } else if (ph == scsi_data_out) {
                if (dma) {
                    c->xfer_mode = XFER_DATA_OUT; // aperture writes push the payload
                } else {
                    while (c->fifo_count > 0 && scsi_get_bus_phase(c->bus) == scsi_data_out)
                        scsi_push_data_out_byte(c->bus, fifo_pop(c));
                    refresh_phase(c);
                    post_interrupt(c, IR_BUS_SERVICE);
                }
            } else if (ph == scsi_status) {
                int st = scsi_external_status_byte(c->bus);
                if (st >= 0)
                    fifo_push(c, (uint8_t)st);
                refresh_phase(c);
                post_interrupt(c, IR_BUS_SERVICE);
            } else if (ph == scsi_message_in) {
                int msg = scsi_external_message_byte(c->bus);
                if (msg >= 0)
                    fifo_push(c, (uint8_t)msg);
                refresh_phase(c);
                post_interrupt(c, IR_FUNC_COMPLETE);
            } else if (ph == scsi_command) {
                // The System's SCSI Manager selects without ATN/CDB and then
                // issues a separate Transfer Information to feed the command
                // block.  In DMA mode the CDB streams through the aperture; in
                // non-DMA mode the driver pre-loads the CDB into the FIFO, so
                // drain it to the target here and report completion once the
                // target advances out of command phase.
                if (dma) {
                    c->xfer_mode = XFER_CMD_OUT;
                } else {
                    while (c->fifo_count > 0 && scsi_get_bus_phase(c->bus) == scsi_command)
                        scsi_push_data_out_byte(c->bus, fifo_pop(c));
                    refresh_phase(c);
                    post_interrupt(c, IR_FUNC_COMPLETE | IR_BUS_SERVICE);
                }
            } else {
                LOG(2, "53C96 transfer info in unhandled phase %d", ph);
                post_interrupt(c, IR_ILL_CMD);
            }
        } else if (code == 0x11) {
            // ICCS: status byte then COMMAND COMPLETE message into the FIFO.
            int st = scsi_external_status_byte(c->bus);
            int msg = (st >= 0) ? scsi_external_message_byte(c->bus) : -1;
            if (st >= 0)
                fifo_push(c, (uint8_t)st);
            if (msg >= 0)
                fifo_push(c, (uint8_t)msg);
            refresh_phase(c);
            post_interrupt(c, IR_FUNC_COMPLETE);
        } else { // 0x12: message accepted → target disconnects
            scsi_external_release(c->bus);
            refresh_phase(c);
            post_interrupt(c, IR_DISCONNECT);
        }
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

// Register-read body (traced by the public wrapper below).
static uint8_t reg_read_body(scsi_53c96_t *c, uint32_t reg) {
    switch (reg & 0xF) {
    case R_XFER_LO:
        return (uint8_t)c->xfer_counter;
    case R_XFER_HI:
        return (uint8_t)(c->xfer_counter >> 8);
    case R_FIFO:
        // During an active pseudo-DMA read the chip's FIFO holds the transfer
        // data, so a FIFO-register read pulls the next payload byte (and
        // advances the DMA counter).  The ROM uses this for the trailing odd
        // byte of an odd-length transfer — it drains whole words through the
        // pseudo-DMA aperture, then reads the final byte here.  Without this,
        // the counter never reaches zero and the completion interrupt never
        // fires.
        if (c->xfer_mode == XFER_DATA_IN)
            return pdma_in_byte(c);
        return fifo_pop(c);
    case R_COMMAND:
        return c->command;
    case R_STATUS:
        // The status register's low three bits are combinational from the live
        // bus phase lines (/MSG //C/D /I/O), so a driver that polls status in a
        // tight loop — as the System's SCSI Manager does while waiting for the
        // target to enter COMMAND phase after a select — sees the phase change
        // without issuing another chip command.  Refresh from the bus on read.
        refresh_phase(c);
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
    case R_FIFOFLAGS: {
        // Bytes available to the CPU: the residual DMA counter while a
        // pseudo-DMA read is armed (capped at the 16-byte FIFO width), else
        // the command/status FIFO count.  Upper 3 bits duplicate seq step.
        uint32_t avail = (c->xfer_mode == XFER_DATA_IN) ? (c->counter_live < FIFO_DEPTH ? c->counter_live : FIFO_DEPTH)
                                                        : c->fifo_count;
        return (uint8_t)(((c->seq_step & 7) << 5) | (avail & 0x1F));
    }
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

uint8_t scsi_53c96_read(scsi_53c96_t *c, uint32_t reg) {
    if (!c)
        return 0;
    uint8_t v = reg_read_body(c, reg);
    LOG(5, "rd reg %X -> %02X", reg & 0xF, v);
    return v;
}

void scsi_53c96_write(scsi_53c96_t *c, uint32_t reg, uint8_t value) {
    if (!c)
        return;
    LOG(5, "wr reg %X = %02X", reg & 0xF, value);
    switch (reg & 0xF) {
    case R_XFER_LO:
        c->xfer_count = (uint16_t)((c->xfer_count & 0xFF00) | value);
        break;
    case R_XFER_HI:
        c->xfer_count = (uint16_t)((c->xfer_count & 0x00FF) | ((uint16_t)value << 8));
        break;
    case R_FIFO:
        if (c->xfer_mode == XFER_CMD_OUT && c->bus) {
            // A select sequence is paused awaiting CDB bytes: FIFO writes
            // feed the command phase directly (the ROM pushes the CDB
            // prefix here, then the tail via the pseudo-DMA port).
            pdma_out_byte(c, value);
            break;
        }
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
        saved.bus = NULL; // re-attached by the machine after restore
        saved.xfer_mode = XFER_IDLE; // mid-transfer restore lands in Phase I
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

void scsi_53c96_attach_bus(scsi_53c96_t *c, struct scsi *bus) {
    if (c)
        c->bus = bus;
}

// ============================================================================
// TurboSCSI pseudo-DMA aperture
// ============================================================================
// The CPU moves the payload for an armed DMA Transfer Information command.
// A finished transfer (counter exhausted, or the target changed phase)
// terminates the command: Terminal Count when the counter reached zero,
// then a Bus Service interrupt for the phase change.

// Terminate the active data-out transfer with the phase-change interrupt.
static void pdma_finish(scsi_53c96_t *c) {
    if (c->xfer_mode == XFER_IDLE)
        return;
    if (c->counter_live == 0)
        c->status |= ST_TC;
    c->xfer_mode = XFER_IDLE;
    refresh_phase(c);
    post_interrupt(c, IR_BUS_SERVICE);
}

// One payload byte through the aperture, read side — pulls straight from
// the target as the CPU reads, decrementing the transfer counter (one DACK
// per byte).  When the counter reaches zero, or the target runs out first,
// the transfer completes: advance the target to STATUS if its buffer is now
// empty (so the driver's completion read sees the phase change) and post the
// Bus Service interrupt the ROM's drain loop waits for.
static uint8_t pdma_in_byte(scsi_53c96_t *c) {
    uint8_t b = 0;
    if (!c || !c->bus || c->xfer_mode != XFER_DATA_IN)
        return 0;
    if (!scsi_pop_data_in_byte(c->bus, &b)) {
        // Target ran out before the count — short transfer.
        LOG(3, "pdma-in short transfer (counter=%u)", c->counter_live);
        scsi_external_data_in_complete(c->bus);
        refresh_phase(c);
        c->xfer_mode = XFER_IDLE;
        post_interrupt(c, IR_BUS_SERVICE);
        return 0;
    }
    if (c->counter_live > 0)
        c->counter_live--;
    c->xfer_counter = (uint16_t)c->counter_live;
    if (!c->xfer_int_done && c->counter_live == c->xfer_residual) {
        // The counted transfer has moved all but the reserved FIFO residual:
        // post the completion interrupt the ROM's drain loop waits for.  With
        // a zero residual (even count) the transfer is fully done here; with a
        // residual of one (odd count) the final byte stays live for the FIFO-
        // register read that the driver issues after seeing this interrupt.
        c->xfer_int_done = 1;
        LOG(3, "pdma-in count done (residual=%u)", c->xfer_residual);
        post_interrupt(c, IR_BUS_SERVICE);
        if (c->xfer_residual == 0) {
            scsi_external_data_in_complete(c->bus);
            refresh_phase(c);
            c->xfer_mode = XFER_IDLE;
        }
    } else if (c->counter_live == 0) {
        // Residual odd byte consumed (via the FIFO register read): finish the
        // transfer without a second interrupt.
        scsi_external_data_in_complete(c->bus);
        refresh_phase(c);
        c->xfer_mode = XFER_IDLE;
    }
    return b;
}

// One payload byte through the aperture, write side (data-out or the CDB
// tail of a DMA select).
static void pdma_out_byte(scsi_53c96_t *c, uint8_t value) {
    if (!c->bus)
        return;
    if (c->xfer_mode == XFER_CMD_OUT) {
        scsi_push_data_out_byte(c->bus, value);
        if (scsi_get_bus_phase(c->bus) != scsi_command) {
            // Full CDB dispatched: the select sequence completes.
            c->xfer_mode = XFER_IDLE;
            c->seq_step = 4;
            refresh_phase(c);
            post_interrupt(c, IR_FUNC_COMPLETE | IR_BUS_SERVICE);
        }
        return;
    }
    if (c->xfer_mode != XFER_DATA_OUT)
        return;
    scsi_push_data_out_byte(c->bus, value);
    if (c->counter_live > 0)
        c->counter_live--;
    c->xfer_counter = (uint16_t)c->counter_live;
    if (c->counter_live == 0 || scsi_get_bus_phase(c->bus) != scsi_data_out)
        pdma_finish(c);
}

uint16_t scsi_53c96_pdma_read16(scsi_53c96_t *c) {
    if (!c)
        return 0;
    uint16_t hi = pdma_in_byte(c);
    uint16_t lo = pdma_in_byte(c);
    return (uint16_t)((hi << 8) | lo);
}

void scsi_53c96_pdma_write16(scsi_53c96_t *c, uint16_t value) {
    if (!c)
        return;
    pdma_out_byte(c, (uint8_t)(value >> 8));
    pdma_out_byte(c, (uint8_t)value);
}

uint8_t scsi_53c96_pdma_read8(scsi_53c96_t *c) {
    return c ? pdma_in_byte(c) : 0;
}

void scsi_53c96_pdma_write8(scsi_53c96_t *c, uint8_t value) {
    if (c)
        pdma_out_byte(c, value);
}

bool scsi_53c96_dreq(scsi_53c96_t *c) {
    if (!c)
        return false;
    return c->xfer_mode != XFER_IDLE; // a transfer is armed and the FIFO is ready
}
