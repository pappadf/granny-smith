// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// new_age.c
// New Age (µPD72070) floppy controller stub — see new_age.h.
//
// The state machine is the datasheet's Apple-mode PIO handshake
// (new-age.md §5), reduced to "no drive present":
//
//   * idle: RQM=1, DIO=0, CB=0; DxI bits 2/3 set (no drive installed).
//   * command phase: the first FIFO byte selects the command and its total
//     length; RQM stays 1, DIO stays 0, CB sets with the first byte.
//   * execution is instantaneous.  Interrupting commands latch PSC-VIA2
//     IFR bit 5 and record the ST0 SenseInterrupt will return; the <SM23>
//     deviation keeps CB SET until the pending status is collected.
//   * result phase: RQM=1, DIO=1, CB=1; each FIFO read pops one byte;
//     after the last byte the chip returns to idle (RQM=1, DIO=0, CB=0).
//   * Sense Drive Status returns ST3 = $FF and does not interrupt;
//     Sense Interrupt Status returns the recorded ST0 (+PCN after a
//     seek-family end), or ST0 = $80 when nothing is pending.

#include "new_age.h"

#include "av.h"
#include "psc.h"

#include "cpu.h"
#include "log.h"
#include "system.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("new_age");

// MSR bits (µPD72070, Apple mode).
#define NA_MSR_RQM 0x80 // request for master
#define NA_MSR_DIO 0x40 // 1 = FDC → host (result phase)
#define NA_MSR_CB  0x10 // command busy
#define NA_MSR_D1I 0x08 // drive 1 not installed
#define NA_MSR_D0I 0x04 // drive 0 not installed

#define NA_ST0_INVALID 0x80 // invalid command / nothing pending
#define NA_ST3_NODRIVE 0xFF // "no drive" — the documented quick path

typedef enum {
    NA_IDLE = 0,
    NA_COMMAND, // accumulating command bytes
    NA_RESULT, // result bytes waiting in the FIFO
} na_phase_t;

struct av_new_age {
    // --- plain data (checkpointed up to the first pointer field) ---
    na_phase_t phase;
    uint8_t cmd[16]; // command bytes accumulated
    int cmd_len;
    int cmd_expect; // total bytes this command carries
    uint8_t result[8]; // result bytes queued
    int result_len;
    int result_idx;
    bool int_pending; // an interrupt cause awaits Sense Interrupt Status
    bool int_seek_end; // pending cause is seek-family (2-byte result)
    uint8_t int_st0; // ST0 for the pending cause
    uint8_t int_pcn; // PCN for a seek-family cause
    uint8_t drr; // data-rate register latch ($101 write)

    // --- pointers (not checkpointed) ---
    config_t *cfg;
};

static inline av_new_age_t *na_of(config_t *cfg) {
    return ((av_state_t *)cfg->machine_context)->fdc;
}

// Total command length (opcode + parameters); 0 = invalid in Apple mode.
// Full-opcode match — bit 7 is the on/off selector on the Apple drive
// commands and bit 6 the GCR/MFM selector on the data commands
// (new-age.md §3).
static int na_cmd_len(uint8_t op) {
    switch (op) {
    case 0x13:
        return 4; // Configure
    case 0x03:
        return 3; // Specify
    case 0x32:
        return 2; // Select Drive Type
    case 0x12:
        return 2; // Perpendicular Mode
    case 0x07:
        return 2; // Recalibrate
    case 0x0F:
        return 3; // Seek
    case 0x08:
        return 1; // Sense Interrupt Status
    case 0x04:
        return 2; // Sense Drive Status
    case 0x0A:
    case 0x4A:
        return 2; // Read ID (GCR/MFM)
    case 0x06:
    case 0x46:
    case 0x05:
    case 0x45:
    case 0x02:
    case 0x42:
        return 9; // Read/Write Data, Read A Track
    case 0x0D:
    case 0x4D:
        return 6; // Format A Track
    case 0x01:
    case 0x41:
        return 5; // Format/Write
    case 0x1E:
    case 0x5E:
        return 8; // Raw Dump
    case 0x1B:
    case 0x9B:
        return 2; // Set Enable Control
    case 0x1A:
    case 0x9A:
        return 2; // Set Motor Control
    case 0x1C:
    case 0x5C:
        return 2; // Set Drive Mode
    case 0x52:
        return 2; // Eject Disk
    case 0x0B:
    case 0x8B:
        return 2; // Disable/Enable DPLL
    case 0x20:
        return 1; // Revision
    default:
        return 0; // invalid / illegal in Apple mode
    }
}

// Drive the FDC INT line (a LEVEL into PSC-VIA2 bit 5; the host clears it
// by collecting the interrupt status, never by writing the IFR).
static void na_set_int(av_new_age_t *fdc, bool active) {
    av_state_t *st = (av_state_t *)fdc->cfg->machine_context;
    if (st && st->psc)
        av_psc_via2_source(st->psc, AV_PSC_VIA2_FDC, active);
}

// Raise the FDC interrupt and record the ST0 Sense Interrupt Status returns.
static void na_interrupt(av_new_age_t *fdc, uint8_t st0, bool seek_end, uint8_t pcn) {
    fdc->int_pending = true;
    fdc->int_seek_end = seek_end;
    fdc->int_st0 = st0;
    fdc->int_pcn = pcn;
    na_set_int(fdc, true);
}

// Enter the result phase with the queued bytes.
static void na_enter_result(av_new_age_t *fdc, int len) {
    fdc->phase = NA_RESULT;
    fdc->result_len = len;
    fdc->result_idx = 0;
}

// Execute a completed command (instantaneous; no drives exist).
static void na_execute(av_new_age_t *fdc) {
    uint8_t op = fdc->cmd[0];
    LOG(2, "command $%02X len=%d (pc=%08X)", op, fdc->cmd_len, cpu_get_pc(fdc->cfg->cpu));

    switch (op) {
    case 0x08: { // Sense Interrupt Status — returns the pending cause
        if (fdc->int_pending) {
            fdc->result[0] = fdc->int_st0;
            if (fdc->int_seek_end) {
                fdc->result[1] = fdc->int_pcn;
                na_enter_result(fdc, 2);
            } else {
                na_enter_result(fdc, 1);
            }
            fdc->int_pending = false;
        } else {
            fdc->result[0] = NA_ST0_INVALID; // nothing pending
            na_enter_result(fdc, 1);
        }
        return;
    }
    case 0x04: // Sense Drive Status → ST3; does NOT interrupt
        fdc->result[0] = NA_ST3_NODRIVE;
        na_enter_result(fdc, 1);
        return;
    case 0x20: // Revision: firmware rev, hardware rev
        fdc->result[0] = 0x01;
        fdc->result[1] = 0x01;
        na_enter_result(fdc, 2);
        return;
    case 0x13: // Configure
    case 0x03: // Specify
    case 0x32: // Select Drive Type
    case 0x12: // Perpendicular Mode
        fdc->phase = NA_IDLE; // no result, no interrupt
        return;
    case 0x07: // Recalibrate — seek-family end interrupt
    case 0x0F: { // Seek
        uint8_t drive = (uint8_t)(fdc->cmd[1] & 3);
        fdc->phase = NA_IDLE;
        // No drive: abnormal termination + equipment check.
        na_interrupt(fdc, (uint8_t)(0x40 | 0x10 | 0x20 | drive), true, 0);
        return;
    }
    case 0x1B:
    case 0x9B: // Set Enable Control — always normal termination, interrupts
    case 0x1A:
    case 0x9A: // Set Motor Control
    case 0x1C:
    case 0x5C: // Set Drive Mode
    case 0x0B:
    case 0x8B: // Disable/Enable DPLL
    case 0x52: { // Eject
        uint8_t drive = (uint8_t)(fdc->cmd[1] & 3);
        fdc->phase = NA_IDLE;
        na_interrupt(fdc, drive, false, 0);
        return;
    }
    default:
        // Data-path commands cannot run with no drive; treat like the
        // datasheet's invalid path — interrupt with ST0 = $80.
        fdc->result[0] = NA_ST0_INVALID;
        na_enter_result(fdc, 1);
        na_interrupt(fdc, NA_ST0_INVALID, false, 0);
        return;
    }
}

// ============================================================
// Register handlers
// ============================================================

uint8_t av_new_age_read(config_t *cfg, uint32_t addr) {
    av_new_age_t *fdc = na_of(cfg);
    uint32_t off = (addr & 0x3FFFFu) - 0x2A000u;
    switch (off) {
    case 0x101: { // MSR
        uint8_t msr = (uint8_t)(NA_MSR_RQM | NA_MSR_D0I | NA_MSR_D1I);
        if (fdc->phase == NA_RESULT)
            msr |= NA_MSR_DIO | NA_MSR_CB;
        else if (fdc->phase == NA_COMMAND)
            msr |= NA_MSR_CB;
        return msr;
    }
    case 0x141: // FIFO — pop one result byte
        if (fdc->phase == NA_RESULT) {
            uint8_t v = fdc->result[fdc->result_idx];
            na_set_int(fdc, false); // a status read deasserts INT
            if (fdc->result_idx + 1 < fdc->result_len)
                fdc->result_idx++;
            else
                fdc->phase = NA_IDLE; // last byte read → back to idle
            return v;
        }
        return 0xFF;
    default:
        return 0xFF;
    }
}

void av_new_age_write(config_t *cfg, uint32_t addr, uint8_t value) {
    av_new_age_t *fdc = na_of(cfg);
    uint32_t off = (addr & 0x3FFFFu) - 0x2A000u;
    switch (off) {
    case 0x101: // DRR (data-rate register)
        fdc->drr = value;
        if (value & 0x80) { // fReset: soft-reset the chip
            fdc->phase = NA_IDLE;
            fdc->int_pending = false;
            na_set_int(fdc, false);
            LOG(2, "DRR reset $%02X (pc=%08X)", value, cpu_get_pc(cfg->cpu));
        }
        return;
    case 0x141: // FIFO — command byte
        if (fdc->phase == NA_RESULT)
            return; // protocol violation; drop
        if (fdc->phase == NA_IDLE) {
            na_set_int(fdc, false); // "Reset INT, Set CB" on command arrival
            fdc->phase = NA_COMMAND;
            fdc->cmd_len = 0;
            fdc->cmd_expect = na_cmd_len(value);
            if (fdc->cmd_expect == 0) {
                // Invalid/illegal-in-Apple-mode opcode: ST0=$80 result.
                LOG(2, "invalid command $%02X (pc=%08X)", value, cpu_get_pc(cfg->cpu));
                fdc->cmd[0] = value;
                fdc->result[0] = NA_ST0_INVALID;
                na_enter_result(fdc, 1);
                na_interrupt(fdc, NA_ST0_INVALID, false, 0);
                return;
            }
        }
        if (fdc->cmd_len < (int)sizeof(fdc->cmd))
            fdc->cmd[fdc->cmd_len++] = value;
        if (fdc->cmd_len >= fdc->cmd_expect)
            na_execute(fdc);
        return;
    default:
        return;
    }
}

// ============================================================
// Lifecycle
// ============================================================

av_new_age_t *av_new_age_init(config_t *cfg, checkpoint_t *cp) {
    av_new_age_t *fdc = calloc(1, sizeof(*fdc));
    if (!fdc)
        return NULL;
    fdc->cfg = cfg;
    if (cp) {
        size_t data_size = offsetof(av_new_age_t, cfg);
        system_read_checkpoint_data(cp, fdc, data_size);
    }
    return fdc;
}

void av_new_age_delete(av_new_age_t *fdc) {
    free(fdc);
}

void av_new_age_checkpoint(av_new_age_t *fdc, checkpoint_t *cp) {
    if (!fdc || !cp)
        return;
    size_t data_size = offsetof(av_new_age_t, cfg);
    system_write_checkpoint_data(cp, fdc, data_size);
}
