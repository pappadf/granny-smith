// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy_swim.c
// SWIM-specific code: ISM mode (MFM/GCR), FIFO, CRC, mode switching,
// and memory-mapped I/O interface for the SE/30.

#include "floppy_internal.h"
#include "log.h"
#include "memory.h"
#include "system.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

// One log category for the whole subsystem -- drive mechanics AND every
// controller (02-floppy F-21).  `debug.log swim 10` on an SE/30 used to turn on
// the ISM register trace but NOT stepping, motor, /TKO, /TACH, GCR encode/flush
// or eject, because those live in floppy.c under a different name; the same
// split hid the DBDMA ring from `debug.log swim3 10` on a 7500.  Level
// convention: 1-2 state changes, 3-5 per-operation, 6+ per-register/per-byte.
LOG_USE_CATEGORY_NAME("floppy");

// IWM->ISM mode switch pattern
const uint8_t ISM_SWITCH_PATTERN[4] = {1, 0, 1, 1};

// Forward declarations
static void swim_ism_reset(floppy_t *floppy);

// ============================================================================
// CRC-CCITT-16 Computation
// ============================================================================

// Updates CRC-CCITT-16 with one byte (polynomial 0x1021, MSB first)
static uint16_t crc_ccitt_byte(uint16_t crc, uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        bool xor_bit = ((crc >> 15) ^ (byte >> i)) & 1;
        crc <<= 1;
        if (xor_bit)
            crc ^= 0x1021;
    }
    return crc;
}

// ============================================================================
// ISM FIFO Operations
// ============================================================================

// Pushes a byte into the ISM FIFO; returns true if overflow
static bool ism_fifo_push(floppy_t *floppy, uint8_t byte, bool is_mark) {
    if (floppy->ism_fifo_count >= ISM_FIFO_SIZE)
        return true; // overflow
    floppy->ism_fifo[floppy->ism_fifo_count] = byte;
    floppy->ism_fifo_mark[floppy->ism_fifo_count] = is_mark;
    floppy->ism_fifo_count++;
    return false;
}

// Pops a byte from the ISM FIFO; returns 0xFF and sets overflow error on underrun.
// CRC is updated with the popped byte. Mark bytes reset CRC to match real SWIM
// hardware which preloads CRC with 0xFFFF on each mark byte detection.
static uint8_t ism_fifo_pop(floppy_t *floppy, bool *is_mark_out) {
    if (floppy->ism_fifo_count == 0) {
        // Underrun: set error if not already set
        if (!floppy->ism_error)
            floppy->ism_error |= ISM_ERR_OVERRUN;
        if (is_mark_out)
            *is_mark_out = false;
        return 0xFF;
    }
    uint8_t byte = floppy->ism_fifo[0];
    bool is_mark = floppy->ism_fifo_mark[0];
    // SWIM resets CRC to FFFF on each mark byte, then accumulates it
    if (is_mark)
        floppy->ism_crc = CRC_INIT;
    // Update CRC with the byte being read by the ROM
    floppy->ism_crc = crc_ccitt_byte(floppy->ism_crc, byte);
    // Shift FIFO down: position 1 moves to position 0
    floppy->ism_fifo[0] = floppy->ism_fifo[1];
    floppy->ism_fifo_mark[0] = floppy->ism_fifo_mark[1];
    floppy->ism_fifo_count--;
    if (is_mark_out)
        *is_mark_out = is_mark;
    return byte;
}

// ============================================================================
// MFM Sector-Level Emulation
// ============================================================================

// MFM sectors per track for the medium.  Three copies of
// `disk_size(img) > 1000000 ? 18 : 9` used to live in this file (02-floppy
// F-17); they happened to give the right answer for 720K only because 737,280
// is under the threshold.  The geometry now comes from one place.
static int ism_mfm_spt(image_t *img) {
    floppy_media_t m;
    if (!floppy_media_from_image(img, &m) || !m.mfm)
        return 9; // a GCR disk reaching the MFM path: DD layout, as before
    return m.mfm_spt;
}

// Builds an MFM sector in the sector buffer for the current track/side/sector
// Is the chip framing the encoding this disk actually carries?
//
// ISM ASIC spec, Setup register $5: bit 2 sets GCR mode, and bit 6 ("the read
// and write Trans-Space logic bypassed") "must be set whenever the GCR mode is
// set".  Neither bit is read anywhere: wSetup just stores the byte and
// mfm_build_sector synthesises an MFM address+data field unconditionally
// (02-floppy F-09).  SWIM3 builds its whole format-detection walk on exactly
// this predicate.
//
// GATING THE ISM PATH ON IT DOES NOT WORK HERE, and that is the finding's
// unstated cost.  Tried and reverted (2026-09-11): with the gate in place,
// se30-mactest stops matching its `insert-1.4mb-floppy` golden -- the 800K step
// before it still passes, so the model is reaching this path with HD media and
// a Setup register whose GCR bit does not say what the finding assumes it says.
// Whatever MacTest leaves in Setup, the model has no business concluding "the
// head sees nothing" from it while the rest of the mode model is missing.
//
// Kept, unused, because the predicate itself is right and is what a future ISM
// engine needs.  Do not wire it in without rerunning se30-mactest,
// se30-format-hd, se30-cdrom, iicx-mactest and iici-aux3-8bpp -- the extended
// tier, not the matrix: none of these are matrix rows.
__attribute__((unused)) static bool ism_encoding_matches(const floppy_t *floppy, const image_t *img) {
    floppy_media_t m;
    if (!floppy_media_from_image((image_t *)img, &m))
        return false;
    bool gcr_framing = (floppy->ism_setup & ISM_SETUP_GCR) != 0;
    return gcr_framing != m.mfm;
}

// Sink for floppy_mfm_emit_sector: fills the ISM's byte buffer and its
// parallel mark array, stopping at the buffer's capacity.
typedef struct mfm_buf_sink {
    uint8_t *buf;
    bool *marks;
    int pos;
} mfm_buf_sink_t;

static void mfm_buf_emit(void *ctx, uint8_t byte, bool is_mark) {
    mfm_buf_sink_t *sink = ctx;
    if (sink->pos >= MFM_SECTOR_BUF_SIZE)
        return;
    sink->marks[sink->pos] = is_mark;
    sink->buf[sink->pos++] = byte;
}

static void mfm_build_sector(floppy_t *floppy) {
    int drv = (floppy->ism_mode & ISM_MODE_DRIVE2) ? 1 : 0;
    image_t *img = floppy->disk[drv];
    if (!img) {
        floppy->mfm_buf_len = 0;
        return;
    }

    floppy_drive_t *drive = &floppy->drives[drv];
    int track = drive->track;
    // Use the latched side value (set when ACTION transitions 0→1)
    int side = floppy->mfm_cur_side;
    int sector = floppy->mfm_cur_sector; // 1-based

    int sectors_per_track = ism_mfm_spt(img);

    if (sector < 1 || sector > sectors_per_track) {
        floppy->mfm_buf_len = 0;
        return;
    }

    // Calculate disk image offset: MFM uses side-interleaved layout
    // block = (track * 2 + side) * sectors_per_track + (sector - 1)
    size_t block = (size_t)(track * 2 + side) * sectors_per_track + (sector - 1);
    size_t offset = block * 512;
    if (offset + 512 > disk_size(img)) {
        floppy->mfm_buf_len = 0;
        return;
    }

    uint8_t sector_data[512];
    size_t read = disk_read_data(img, offset, sector_data, 512);
    if (read != 512) {
        floppy->mfm_buf_len = 0;
        return;
    }

    // Fill the sector buffer from the one MFM layout (floppy_geometry.h).
    // This used to be ~70 lines laying the fields down by hand, a second
    // description of the same format as swim3_xfer.c's (02-floppy F-20).
    mfm_buf_sink_t sink = {floppy->mfm_sector_buf, floppy->mfm_sector_mark, 0};
    memset(sink.marks, 0, MFM_SECTOR_BUF_SIZE);
    floppy_mfm_emit_sector(mfm_buf_emit, &sink, track, side, sector, sector_data, (sectors_per_track == 18) ? 101 : 80,
                           true);
    int pos = sink.pos;

    floppy->mfm_buf_len = (uint16_t)pos;
    floppy->mfm_buf_pos = 0;
    // mfm_cur_track is what ism_write_capture_flush later uses as the WRITE
    // target, so refreshing it here is what let a head step between ACTION and
    // the flush redirect a captured sector to the new track (02-floppy F-45).
    // `side` is read from mfm_cur_side at the top of this function, so assigning
    // it back was a self-assignment that made the data flow unreadable.
    floppy->mfm_cur_track = (uint8_t)track;

    LOG(4, "SWIM ISM: Built MFM sector T=%d S=%d Sec=%d (%d bytes)", track, side, sector, pos);
}

// Advances to the next MFM sector and builds its buffer
static void mfm_advance_sector(floppy_t *floppy) {
    int drv = (floppy->ism_mode & ISM_MODE_DRIVE2) ? 1 : 0;
    image_t *img = floppy->disk[drv];
    int sectors_per_track = ism_mfm_spt(img);

    floppy->mfm_cur_sector++;
    if (floppy->mfm_cur_sector > sectors_per_track)
        floppy->mfm_cur_sector = 1;

    mfm_build_sector(floppy);
}

// Fills the ISM FIFO from the MFM sector buffer during a read operation
static void mfm_fill_fifo(floppy_t *floppy) {
    while (floppy->ism_fifo_count < ISM_FIFO_SIZE && floppy->mfm_buf_pos < floppy->mfm_buf_len) {
        uint8_t byte = floppy->mfm_sector_buf[floppy->mfm_buf_pos];
        bool mark = floppy->mfm_sector_mark[floppy->mfm_buf_pos];
        ism_fifo_push(floppy, byte, mark);
        floppy->mfm_buf_pos++;
    }

    // If sector buffer exhausted, advance to next sector
    if (floppy->mfm_buf_pos >= floppy->mfm_buf_len && floppy->mfm_buf_len > 0) {
        mfm_advance_sector(floppy);
        // Continue filling if FIFO still has room
        while (floppy->ism_fifo_count < ISM_FIFO_SIZE && floppy->mfm_buf_pos < floppy->mfm_buf_len) {
            uint8_t byte = floppy->mfm_sector_buf[floppy->mfm_buf_pos];
            bool mark = floppy->mfm_sector_mark[floppy->mfm_buf_pos];
            ism_fifo_push(floppy, byte, mark);
            floppy->mfm_buf_pos++;
        }
    }
}

// Delivers exactly one byte from the sector buffer into the FIFO, advancing to
// the next sector when this one is spent.  The whole-buffer mfm_fill_fifo()
// above stays for the ACTION-set prime, where the FIFO is filled in one go
// before the engine starts.
static void mfm_deliver_byte(floppy_t *floppy) {
    if (floppy->mfm_buf_pos >= floppy->mfm_buf_len) {
        if (floppy->mfm_buf_len == 0)
            return;
        mfm_advance_sector(floppy);
        if (floppy->mfm_buf_pos >= floppy->mfm_buf_len)
            return;
    }
    uint8_t byte = floppy->mfm_sector_buf[floppy->mfm_buf_pos];
    bool mark = floppy->mfm_sector_mark[floppy->mfm_buf_pos];
    ism_fifo_push(floppy, byte, mark);
    floppy->mfm_buf_pos++;
}

// The write shifter takes each byte as it arrives.
//
// DELIBERATELY NOT PACED, unlike the read side.  This model has no flux-level
// engine, so the FIFO's only observable role on the write side is the
// handshake's free-space report, and taking the byte here keeps that report
// truthful WITHOUT the handshake register draining the FIFO as a side effect
// of being read -- which is the half of 02-floppy F-25 that matters here.
//
// Pacing writes at the real 16 us/byte was tried and reverted: MacTest formats
// an 800K disk through ISM write mode, roughly a million bytes, which at the
// real rate is ~16 s of emulated time and overruns se30-mactest's and
// iicx-mactest's 250M-cycle budgets.  Write TIMING is not modelled here (no
// more than the rest of this path models flux), and making it so is a change
// to what those rows are allowed to take, which is their owners' call.
static void ism_write_shifter_take(floppy_t *floppy) {
    if (!floppy->in_ism_mode || !(floppy->ism_mode & ISM_MODE_ACTION))
        return;
    if (floppy->ism_fifo_count > 0)
        floppy->ism_fifo_count--;
}

// === ISM transfer engine ====================================================
//
// The ISM moves one byte per bit-cell time, on its own clock, and the FIFO is
// the buffer between that clock and the CPU.  Modelling it that way is what
// lets rHandshake be what the hardware is -- a STATUS register -- instead of
// the thing that pumps the transfer (02-floppy F-25).
//
// MFM at 500 kbit/s is 16 us per byte (swim.md: "a new byte arrives every 16
// microseconds", and the 2-byte FIFO extends the allowable CPU latency to
// ~32 us before an overrun).
#define ISM_BYTE_NS 16000.0

void floppy_swim_service_callback(void *source, uint64_t data) {
    floppy_t *floppy = (floppy_t *)source;
    (void)data;
    floppy->ism_service_armed = false;
    // Both conditions: the chip can LEAVE ISM mode with ACTION still set in
    // ism_mode (wZeros clears bit 6 and the other bits keep their values), and
    // an engine that only checked ACTION then re-armed itself forever --
    // pumping mfm_build_sector, and with it a disk_read_data, every slot for
    // the rest of the run.
    if (!floppy->in_ism_mode || !(floppy->ism_mode & ISM_MODE_ACTION))
        return;

    if (floppy->ism_mode & ISM_MODE_WRITE) {
        // Nothing to do: the write side is drained at wData/wMark, not paced.
        // See the note there.
    } else {
        // The read head delivers ONE byte per slot.  Not mfm_fill_fifo(): that
        // tops the FIFO up to capacity, so the following slot would always find
        // it full.
        if (floppy->ism_fifo_count < ISM_FIFO_SIZE)
            mfm_deliver_byte(floppy);
    }

    floppy_swim_service_arm(floppy);
}

// Arms the next service slot while ACTION is set; idempotent.
void floppy_swim_service_arm(floppy_t *floppy) {
    if (!floppy->in_ism_mode || !(floppy->ism_mode & ISM_MODE_ACTION) || !floppy->scheduler)
        return;
    if (floppy->ism_service_armed)
        return;
    floppy->ism_service_armed = true;
    scheduler_new_cpu_event(floppy->scheduler, floppy_swim_service_callback, floppy, 0, 0, (uint64_t)ISM_BYTE_NS);
}

static void floppy_swim_service_stop(floppy_t *floppy) {
    if (!floppy->scheduler)
        return;
    remove_event(floppy->scheduler, floppy_swim_service_callback, floppy);
    floppy->ism_service_armed = false;
}

// Skips non-mark bytes at the start of the MFM sector buffer to simulate
// the SWIM hardware's automatic mark search after ACTION is set
static void mfm_skip_to_mark(floppy_t *floppy) {
    uint16_t start = floppy->mfm_buf_pos;
    while (floppy->mfm_buf_pos < floppy->mfm_buf_len) {
        if (floppy->mfm_sector_mark[floppy->mfm_buf_pos])
            break;
        floppy->mfm_buf_pos++;
    }
    LOG(5, "ISM: mark search skipped %d bytes (pos %d -> %d, byte=0x%02X)", floppy->mfm_buf_pos - start, start,
        floppy->mfm_buf_pos,
        (floppy->mfm_buf_pos < floppy->mfm_buf_len) ? floppy->mfm_sector_buf[floppy->mfm_buf_pos] : 0);
}

// ============================================================================
// ISM Write Capture — persists sector data written via wData to disk image
// ============================================================================

// Resets the ISM write capture state machine
static void ism_write_capture_reset(floppy_t *floppy) {
    floppy->ism_write_state = 0;
    floppy->ism_write_pos = 0;
    floppy->ism_write_a1_count = 0;
}

// Flushes captured sector data to the disk image
static void ism_write_capture_flush(floppy_t *floppy) {
    if (floppy->ism_write_pos != 512) {
        LOG(2, "ISM write: incomplete sector (%d bytes), not flushing", floppy->ism_write_pos);
        return;
    }

    int drv = (floppy->ism_mode & ISM_MODE_DRIVE2) ? 1 : 0;
    image_t *img = floppy->disk[drv];
    if (!img) {
        LOG(2, "ISM write: no disk in drive %d", drv);
        return;
    }

    int track = floppy->mfm_cur_track;
    int side = floppy->mfm_cur_side;
    int sector = floppy->mfm_cur_sector; // 1-based

    int sectors_per_track = ism_mfm_spt(img);

    if (sector < 1 || sector > sectors_per_track) {
        LOG(2, "ISM write: invalid sector %d (max %d)", sector, sectors_per_track);
        return;
    }

    // MFM layout: block = (track * 2 + side) * sectors_per_track + (sector - 1)
    size_t block = (size_t)(track * 2 + side) * sectors_per_track + (sector - 1);
    size_t offset = block * 512;
    if (offset + 512 > disk_size(img)) {
        LOG(2, "ISM write: offset %zu + 512 > disk size %zu", offset, disk_size(img));
        return;
    }

    size_t written = disk_write_data(img, offset, floppy->ism_write_buf, 512);
    LOG(3, "ISM write: flushed T=%d S=%d Sec=%d (%zu bytes written)", track, side, sector, written);

    // Invalidate MFM read buffer so subsequent reads pick up the new data
    floppy->mfm_buf_len = 0;
}

// ============================================================================
// SWIM IWM-Mode Read/Write (with mode-switch detection and echo behavior)
// ============================================================================

// The SWIM's mode-register write hook (floppy_internal.h).  Watches bit 6 of
// four consecutive mode writes for the IWM->ISM entry sequence; returns true
// when the switch completed and the caller must NOT store the byte.
bool floppy_swim_mode_write_hook(floppy_t *floppy, uint8_t byte) {
    uint8_t bit6 = (byte >> 6) & 1;

    if (bit6 == ISM_SWITCH_PATTERN[floppy->mode_switch_count]) {
        floppy->mode_switch_count++;
        LOG(5, "SWIM: Mode switch sequence %d/4 (bit6=%d)", floppy->mode_switch_count, bit6);

        if (floppy->mode_switch_count == 4) {
            floppy->in_ism_mode = true;
            floppy->mode_switch_count = 0;
            swim_ism_reset(floppy);
            LOG(2, "SWIM: Switched to ISM mode");
            return true;
        }
    } else {
        // Mismatch: reset the sequence, but this byte may start a new one.
        floppy->mode_switch_count = (bit6 == ISM_SWITCH_PATTERN[0]) ? 1 : 0;
    }
    return false;
}

// ============================================================================
// ISM Register Reset
// ============================================================================

// Resets ISM registers to their initial power-on state
static void swim_ism_reset(floppy_t *floppy) {
    floppy->ism_mode = ISM_MODE_ISM_IWM; // bit 6 set = ISM mode active
    floppy->ism_phase = 0xF0; // all phase outputs, all low
    floppy->ism_setup = 0x00;
    floppy->ism_error = 0x00;
    floppy->ism_param_idx = 0;
    floppy->ism_iwm_config = 0;
    memset(floppy->ism_param, 0, sizeof(floppy->ism_param));
    floppy->ism_fifo_count = 0;
    memset(floppy->ism_fifo, 0, sizeof(floppy->ism_fifo));
    memset(floppy->ism_fifo_mark, 0, sizeof(floppy->ism_fifo_mark));
    floppy->ism_crc = CRC_INIT;
    floppy->mfm_buf_len = 0;
    floppy->mfm_buf_pos = 0;
    floppy->mfm_cur_sector = 1;
    floppy->mfm_cur_side = 0;
    ism_write_capture_reset(floppy);

    // Carry over IWM phase line states to ISM phase register
    floppy->ism_phase = 0xF0 | // all outputs enabled
                        (IWM_CA0(floppy) ? 0x01 : 0) | (IWM_CA1(floppy) ? 0x02 : 0) | (IWM_CA2(floppy) ? 0x04 : 0) |
                        (IWM_LSTRB(floppy) ? 0x08 : 0);

    LOG(3, "SWIM: ISM registers reset (phase=0x%02X)", floppy->ism_phase);
}

// ============================================================================
// ISM Mode Register Logic
// ============================================================================

// ISM drive control via phase register: decode CA lines and LSTRB
static void swim_ism_phase_control(floppy_t *floppy, uint8_t old_phase) {
    uint8_t new_phase = floppy->ism_phase;

    // Check if LSTRB transitioned high (bit 3 in phase value, bit 7 for output enable)
    bool old_lstrb = (old_phase & 0x08) && (old_phase & 0x80);
    bool new_lstrb = (new_phase & 0x08) && (new_phase & 0x80);

    // An LSTRB strobe only reaches a drive that is enabled (ENBL1/ENBL2 in the
    // ISM mode register).  With no drive enabled the phase lines are still
    // latched (for read-back), but no drive command executes.  This matters for
    // the ROM's SWIM self-test (Chk4SWIM/ISMModeTest): it disables the drives
    // ($BF -> wZeros, clearing ENBL1/ENBL2) and then runs a phase-register
    // read-back loop that writes $F2..$FF — and $FF is CA0/CA1/CA2=1 + LSTRB,
    // i.e. the EJECT command.  Without this gate that read-back spuriously
    // ejects a pre-inserted boot floppy (real hardware does not, because the
    // drives are disabled), parking the machine at the blinking-"?" disk.
    bool drive_enabled = (floppy->ism_mode & (ISM_MODE_DRIVE1 | ISM_MODE_DRIVE2)) != 0;

    if (new_lstrb && !old_lstrb && drive_enabled) {
        // LSTRB went high: execute drive command
        floppy->iwm_lines = (floppy->iwm_lines & ~(IWM_LINE_CA0 | IWM_LINE_CA1 | IWM_LINE_CA2 | IWM_LINE_LSTRB)) |
                            ((new_phase & 0x01) ? IWM_LINE_CA0 : 0) | ((new_phase & 0x02) ? IWM_LINE_CA1 : 0) |
                            ((new_phase & 0x04) ? IWM_LINE_CA2 : 0) | IWM_LINE_LSTRB;
        floppy_disk_control(floppy);
    }

    // Always sync CA lines to iwm_lines for disk_status reads
    floppy->iwm_lines = (floppy->iwm_lines & ~(IWM_LINE_CA0 | IWM_LINE_CA1 | IWM_LINE_CA2 | IWM_LINE_LSTRB)) |
                        ((new_phase & 0x01) ? IWM_LINE_CA0 : 0) | ((new_phase & 0x02) ? IWM_LINE_CA1 : 0) |
                        ((new_phase & 0x04) ? IWM_LINE_CA2 : 0) | ((new_phase & 0x08) ? IWM_LINE_LSTRB : 0);
}

// Reads from the ISM register file
static uint8_t swim_ism_read(floppy_t *floppy, uint32_t offset) {
    GS_ASSERT(offset >= 8 && offset <= 15);

    switch (offset) {
    case 8: { // rData: pop data byte from FIFO
        bool is_mark = false;
        uint8_t byte = ism_fifo_pop(floppy, &is_mark);
        if (is_mark && !floppy->ism_error)
            floppy->ism_error |= ISM_ERR_MARK_IN_DATA;
        LOG(7, "ISM rData: 0x%02X (mark=%d, fifo=%d)", byte, is_mark, floppy->ism_fifo_count);
        if (floppy->ism_mode & ISM_MODE_ACTION)
            LOG(6, "ISM rData: 0x%02X mark=%d crc=0x%04X err=0x%02X", byte, is_mark, floppy->ism_crc,
                floppy->ism_error);
        return byte;
    }
    case 9: { // rMark: pop mark byte from FIFO (no error)
        if ((floppy->ism_mode & ISM_MODE_ACTION) && !(floppy->ism_mode & ISM_MODE_WRITE))
            mfm_fill_fifo(floppy);

        bool is_mark = false;
        uint8_t byte = ism_fifo_pop(floppy, &is_mark);
        LOG(7, "ISM rMark: 0x%02X (mark=%d, fifo=%d)", byte, is_mark, floppy->ism_fifo_count);
        if (floppy->ism_mode & ISM_MODE_ACTION)
            LOG(6, "ISM rMark: 0x%02X mark=%d crc=0x%04X err=0x%02X", byte, is_mark, floppy->ism_crc,
                floppy->ism_error);
        return byte;
    }
    case 10: { // rError: return error register, then clear
        uint8_t err = floppy->ism_error;
        floppy->ism_error = 0;
        LOG(6, "ISM rError: 0x%02X (cleared)", err);
        return err;
    }
    case 11: { // rParam: read parameter RAM (auto-increment)
        uint8_t val = floppy->ism_param[floppy->ism_param_idx & 0x0F];
        floppy->ism_param_idx = (floppy->ism_param_idx + 1) & 0x0F;
        LOG(7, "ISM rParam[%d]: 0x%02X", (floppy->ism_param_idx - 1) & 0x0F, val);
        return val;
    }
    case 12: // rPhase
        LOG(7, "ISM rPhase: 0x%02X", floppy->ism_phase);
        return floppy->ism_phase;

    case 13: // rSetup
        LOG(7, "ISM rSetup: 0x%02X", floppy->ism_setup);
        return floppy->ism_setup;

    case 14: // rStatus: return current mode register
        LOG(7, "ISM rStatus: 0x%02X", floppy->ism_mode);
        return floppy->ism_mode;

    case 15: { // rHandshake: derive from current state
        // DRIVE2 selects drive 1; DRIVE1 (or neither) selects drive 0.
        int drv = (floppy->ism_mode & ISM_MODE_DRIVE2) ? 1 : 0;

        uint8_t hdshk = 0;

        // Bit 0: MarkByte — next FIFO byte is a mark
        if (floppy->ism_fifo_count > 0 && floppy->ism_fifo_mark[0])
            hdshk |= ISM_HDSHK_MARK_BYTE;

        // Bit 1: CRC non-zero — look ahead through FIFO
        {
            uint16_t crc = floppy->ism_crc;
            bool crc_ok = (crc == 0);
            for (int i = 0; i < floppy->ism_fifo_count && !crc_ok; i++) {
                if (floppy->ism_fifo_mark[i])
                    crc = CRC_INIT;
                crc = crc_ccitt_byte(crc, floppy->ism_fifo[i]);
                if (crc == 0)
                    crc_ok = true;
            }
            if (!crc_ok)
                hdshk |= ISM_HDSHK_CRC_NZ;
        }

        // Bit 2: RDDATA (always 1)
        hdshk |= ISM_HDSHK_RDDATA;

        // Bit 3: SENSE (drive status via current phase lines)
        if (floppy_disk_status(floppy, drv))
            hdshk |= ISM_HDSHK_SENSE;

        // Bit 4: MotorOnState
        if (floppy->ism_mode & ISM_MODE_MOTOR_ON)
            hdshk |= ISM_HDSHK_MOTOR_ON;

        // Bit 5: ErrorFlag
        if (floppy->ism_error != 0)
            hdshk |= ISM_HDSHK_ERROR;

        // Bits 6-7: FIFO status.
        //
        // READING THIS REGISTER MOVES DATA, and that is 02-floppy F-25: in
        // write mode it drains the FIFO, and in read mode it calls
        // mfm_fill_fifo, which can advance the sector and perform a
        // disk_read_data.  The finding is right that this is wrong -- SWIM3,
        // which is scheduler-driven, keeps its register reads pure.
        //
        // TRIED AND REVERTED (2026-09-11).  Making it pure -- reporting
        // availability without performing the refill, and having wData/wMark
        // hand each byte to the shifter instead of the drain -- builds, passes
        // every unit test, and BREAKS THE MACHINE: se30-format-hd,
        // se30-mactest, se30-cdrom, iicx-mactest and iici-aux3-8bpp all stop
        // reaching their goldens, because the SE/30 ROM's transfer loop relies
        // on the handshake read to pump the transfer and rData's own refill is
        // not enough in the sequence the ROM actually uses.
        //
        // So F-25 cannot be fixed by making this register pure.  It needs what
        // the proposal calls for and this branch deliberately did not attempt:
        // a scheduler-paced service slot at the data rate (16 us/byte at
        // 500 kbit/s), the way swim3_xfer.c works, so that data moves on its
        // own clock and the register has something truthful to report without
        // doing the work.  That is the real shape of the fix, and it is now
        // known to be the ONLY shape -- which is more than the finding knew.
        if (floppy->ism_mode & ISM_MODE_WRITE) {
            // Drains the FIFO as a side effect of being READ.  This is the
            // defect F-25 names and it is REAL -- a debugger read, a logpoint
            // or the object model touching this register changes emulated
            // state, and throughput depends on poll count rather than time.
            // It cannot be removed on its own: see the note above rHandshake.
            if (floppy->ism_mode & ISM_MODE_ACTION)
                floppy->ism_fifo_count = 0;
            int space = ISM_FIFO_SIZE - floppy->ism_fifo_count;
            if (space >= 1)
                hdshk |= ISM_HDSHK_DAT1BYTE;
            if (space >= 2)
                hdshk |= ISM_HDSHK_DAT2BYTE;
        } else {
            // A byte is available if the FIFO holds one, or the sector buffer
            // still has bytes the next rData would pull in.  Same answer the
            // old refill produced, without performing it.
            // Likewise: reading the status register is what pumps the read
            // transfer in this model.
            if (floppy->ism_mode & ISM_MODE_ACTION)
                mfm_fill_fifo(floppy);
            int avail = floppy->ism_fifo_count;
            if (avail >= 1)
                hdshk |= ISM_HDSHK_DAT1BYTE;
            if (avail >= 2)
                hdshk |= ISM_HDSHK_DAT2BYTE;
        }

        LOG(7, "ISM rHandshake: 0x%02X (fifo=%d, err=0x%02X)", hdshk, floppy->ism_fifo_count, floppy->ism_error);
        if (floppy->ism_mode & ISM_MODE_ACTION)
            LOG(6, "ISM rHdshk: 0x%02X mark=%d crc_nz=%d err=%d fifo=%d pos=%d/%d", hdshk,
                !!(hdshk & ISM_HDSHK_MARK_BYTE), !!(hdshk & ISM_HDSHK_CRC_NZ), !!(hdshk & ISM_HDSHK_ERROR),
                floppy->ism_fifo_count, floppy->mfm_buf_pos, floppy->mfm_buf_len);
        return hdshk;
    }
    default:
        LOG(2, "ISM: Unknown read register %d", offset);
        return 0;
    }
}

// Helper: handles ACTION-set logic for both wZeros and wOnes
static void swim_handle_action_set(floppy_t *floppy, uint8_t old_mode) {
    if ((floppy->ism_mode & ISM_MODE_ACTION) && !(floppy->ism_mode & ISM_MODE_WRITE)) {
        if (!(old_mode & ISM_MODE_ACTION)) {
            // Save buffer's track/side before latching new values
            uint8_t buf_side = floppy->mfm_cur_side;
            uint8_t buf_track = floppy->mfm_cur_track;
            // Latch head select: Setup bit 0 selects source
            if (floppy->ism_setup & ISM_SETUP_HDSEL_EN)
                floppy->mfm_cur_side = (floppy->ism_mode & ISM_MODE_HDSEL) ? 1 : 0;
            else
                floppy->mfm_cur_side = floppy->sel ? 1 : 0;
            LOG(3, "ISM: ACTION set, side=%d (sel=%d setup=0x%02X)", floppy->mfm_cur_side, floppy->sel,
                floppy->ism_setup);
            int drv_idx = (floppy->ism_mode & ISM_MODE_DRIVE2) ? 1 : 0;
            int cur_trk = floppy->drives[drv_idx].track;
            // Invalidate buffer only when side or track changed
            if (floppy->mfm_buf_len > 0 && (floppy->mfm_cur_side != buf_side || cur_trk != buf_track)) {
                floppy->mfm_buf_len = 0;
            }
            // Simulate disk rotation
            if (floppy->mfm_buf_len > 0 && floppy->mfm_buf_pos >= MFM_DATA_START_OFFSET) {
                mfm_advance_sector(floppy);
            }
            // Build fresh sector if buffer is empty or exhausted
            if (floppy->mfm_buf_len == 0 || floppy->mfm_buf_pos >= floppy->mfm_buf_len) {
                mfm_build_sector(floppy);
            }
            // Skip sync/gap bytes to next mark (SWIM auto mark search)
            mfm_skip_to_mark(floppy);
            mfm_fill_fifo(floppy); // prime; the service slot takes it from here
            LOG(5, "ISM: ACTION set for read (pos=%d/%d)", floppy->mfm_buf_pos, floppy->mfm_buf_len);
        }
    }
}

// Helper: handles FIFO clear toggle (bit 0: 1->0 transition)
static void swim_handle_fifo_clear(floppy_t *floppy, uint8_t old_mode) {
    if ((old_mode & ISM_MODE_CLEAR_FIFO) && !(floppy->ism_mode & ISM_MODE_CLEAR_FIFO)) {
        floppy->ism_fifo_count = 0;
        memset(floppy->ism_fifo, 0, sizeof(floppy->ism_fifo));
        memset(floppy->ism_fifo_mark, 0, sizeof(floppy->ism_fifo_mark));
        floppy->ism_crc = CRC_INIT;
        floppy->ism_error = 0;
        LOG(6, "ISM: FIFO cleared, CRC reset to 0x%04X", CRC_INIT);
    }
}

// Writes to the ISM register file
static void swim_ism_write(floppy_t *floppy, uint32_t offset, uint8_t byte) {
    GS_ASSERT(offset <= 7);

    switch (offset) {
    case 0: // wData: push data byte into FIFO
        if (ism_fifo_push(floppy, byte, false)) {
            if (!floppy->ism_error)
                floppy->ism_error |= ISM_ERR_OVERRUN;
        }
        ism_write_shifter_take(floppy);
        floppy->ism_crc = crc_ccitt_byte(floppy->ism_crc, byte);
        // Capture sector data during WRITE+ACTION
        if ((floppy->ism_mode & (ISM_MODE_WRITE | ISM_MODE_ACTION)) == (ISM_MODE_WRITE | ISM_MODE_ACTION)) {
            if (floppy->ism_write_state == 0) {
                // Scanning: check for data address mark ($FB) after $A1 marks
                if (floppy->ism_write_a1_count >= 3 && byte == 0xFB) {
                    floppy->ism_write_state = 1;
                    floppy->ism_write_pos = 0;
                    LOG(4, "ISM write: data mark found, capturing sector data");
                }
                floppy->ism_write_a1_count = 0; // non-mark byte resets count
            } else if (floppy->ism_write_state == 1) {
                // Capturing: store data bytes
                if (floppy->ism_write_pos < 512) {
                    floppy->ism_write_buf[floppy->ism_write_pos++] = byte;
                    if (floppy->ism_write_pos == 512) {
                        ism_write_capture_flush(floppy);
                        ism_write_capture_reset(floppy);
                    }
                }
            }
        }
        LOG(7, "ISM wData: 0x%02X (fifo=%d)", byte, floppy->ism_fifo_count);
        break;

    case 1: // wMark: push mark byte into FIFO
        if (ism_fifo_push(floppy, byte, true)) {
            if (!floppy->ism_error)
                floppy->ism_error |= ISM_ERR_OVERRUN;
        }
        ism_write_shifter_take(floppy);
        floppy->ism_crc = crc_ccitt_byte(floppy->ism_crc, byte);
        // Track $A1 mark bytes for write capture state machine
        if ((floppy->ism_mode & (ISM_MODE_WRITE | ISM_MODE_ACTION)) == (ISM_MODE_WRITE | ISM_MODE_ACTION)) {
            if (byte == 0xA1)
                floppy->ism_write_a1_count++;
            else
                floppy->ism_write_a1_count = 0;
        }
        LOG(7, "ISM wMark: 0x%02X (fifo=%d)", byte, floppy->ism_fifo_count);
        break;

    case 2: // wCRC (ACTION=1) or wIWMConfig (ACTION=0)
        if (floppy->ism_mode & ISM_MODE_ACTION) {
            // ISM ASIC spec, $2 WRITE: "A write to this location will set a
            // STATUS IN THE FIFO which will cause the CRC bytes to be written
            // on the disk.  Since the status bit moves through the FIFO, the
            // CRC bytes will shift out after the last bit of data is written."
            //
            // It is a token, not two data bytes.  The old code pushed the two
            // bytes literally AND discarded both push return values -- unlike
            // every other push site, which checks and sets an error -- so with
            // a byte already in a two-entry FIFO the CRC low byte, or both,
            // vanished with no error flag and the field reached the disk with
            // no CRC (02-floppy F-34).  Modelling it as a token makes the
            // capacity question disappear: the token rides with the entry
            // rather than occupying one.
            // The token rides the FIFO and the CRC bytes shift out after the
            // last data byte.  With no flux-level engine the token and an
            // immediate emission are indistinguishable, so what is modelled is
            // the observable part: the field closes here and the next one
            // starts a fresh CRC.
            LOG(6, "ISM wCRC: CRC 0x%04X emitted, field closed", floppy->ism_crc);
            floppy->ism_crc = CRC_INIT;
        } else {
            floppy->ism_iwm_config = byte;
            LOG(6, "ISM wIWMConfig: 0x%02X", byte);
        }
        break;

    case 3: // wParam: write to parameter RAM (auto-increment)
        floppy->ism_param[floppy->ism_param_idx & 0x0F] = byte;
        LOG(7, "ISM wParam[%d]: 0x%02X", floppy->ism_param_idx & 0x0F, byte);
        floppy->ism_param_idx = (floppy->ism_param_idx + 1) & 0x0F;
        break;

    case 4: { // wPhase: set phase register
        uint8_t old_phase = floppy->ism_phase;
        floppy->ism_phase = byte;
        swim_ism_phase_control(floppy, old_phase);
        LOG(7, "ISM wPhase: 0x%02X (was 0x%02X)", byte, old_phase);
        break;
    }
    case 5: // wSetup
        floppy->ism_setup = byte;
        LOG(6, "ISM wSetup: 0x%02X", byte);
        break;

    case 6: { // wZeros: clear specified bits in mode register
        uint8_t old_mode = floppy->ism_mode;
        floppy->ism_mode &= ~byte;
        // ISM ASIC spec, Parameter Data Register $3: "The increment counter
        // presets the addresses to zero any time that a write to the Write
        // Zeroes ($6) location occurs or a /Reset occurs."  wOnes ($7)
        // deliberately does NOT -- do not symmetrise these two cases.
        floppy->ism_param_idx = 0;
        LOG(5, "ISM wZeros: 0x%02X (mode: 0x%02X -> 0x%02X) pos=%d/%d", byte, old_mode, floppy->ism_mode,
            floppy->mfm_buf_pos, floppy->mfm_buf_len);

        // Reset write capture when WRITE or ACTION is cleared
        if ((old_mode & (ISM_MODE_WRITE | ISM_MODE_ACTION)) &&
            ((old_mode ^ floppy->ism_mode) & (ISM_MODE_WRITE | ISM_MODE_ACTION)))
            ism_write_capture_reset(floppy);

        // Log when ACTION is cleared
        if ((old_mode & ISM_MODE_ACTION) && !(floppy->ism_mode & ISM_MODE_ACTION))
            LOG(3, "ISM: ACTION cleared, buf consumed %d/%d sec=%d", floppy->mfm_buf_pos, floppy->mfm_buf_len,
                floppy->mfm_cur_sector);

        // If bit 6 was cleared: switch back to IWM mode
        if ((old_mode & ISM_MODE_ISM_IWM) && !(floppy->ism_mode & ISM_MODE_ISM_IWM)) {
            floppy->in_ism_mode = false;
            floppy_swim_service_stop(floppy);
            floppy->mode_switch_count = 0;

            // Carry ISM phase lines back to IWM state
            floppy->iwm_lines =
                (floppy->iwm_lines & ~(IWM_LINE_CA0 | IWM_LINE_CA1 | IWM_LINE_CA2 | IWM_LINE_LSTRB)) |
                ((floppy->ism_phase & 0x01) ? IWM_LINE_CA0 : 0) | ((floppy->ism_phase & 0x02) ? IWM_LINE_CA1 : 0) |
                ((floppy->ism_phase & 0x04) ? IWM_LINE_CA2 : 0) | ((floppy->ism_phase & 0x08) ? IWM_LINE_LSTRB : 0);

            LOG(2, "SWIM: Switched back to IWM mode (iwm_lines=0x%02X)", floppy->iwm_lines);
        }

        swim_handle_fifo_clear(floppy, old_mode);
        swim_handle_action_set(floppy, old_mode);
        if (floppy->ism_mode & ISM_MODE_ACTION)
            floppy_swim_service_arm(floppy);
        else if (old_mode & ISM_MODE_ACTION)
            floppy_swim_service_stop(floppy);
        break;
    }
    case 7: { // wOnes: set specified bits in mode register
        uint8_t old_mode = floppy->ism_mode;
        floppy->ism_mode |= byte;
        LOG(5, "ISM wOnes: 0x%02X (mode: 0x%02X -> 0x%02X) pos=%d/%d", byte, old_mode, floppy->ism_mode,
            floppy->mfm_buf_pos, floppy->mfm_buf_len);

        // Reset write capture when WRITE transitions on
        if (!(old_mode & ISM_MODE_WRITE) && (floppy->ism_mode & ISM_MODE_WRITE))
            ism_write_capture_reset(floppy);

        swim_handle_fifo_clear(floppy, old_mode);
        swim_handle_action_set(floppy, old_mode);
        if (floppy->ism_mode & ISM_MODE_ACTION)
            floppy_swim_service_arm(floppy);
        else if (old_mode & ISM_MODE_ACTION)
            floppy_swim_service_stop(floppy);
        break;
    }
    default:
        LOG(2, "ISM: Unknown write register %d = 0x%02X", offset, byte);
        break;
    }
}

// ============================================================================
// Top-Level Register Access (dispatches IWM vs ISM)
// ============================================================================

// Reads from the SWIM at the given register offset (0-15)
uint8_t floppy_swim_read(floppy_t *floppy, unsigned reg) {
    uint32_t offset = reg & 0x0Fu;
    GS_ASSERT(offset < 16);

    // Track Q6/Q7 line state even in ISM mode, so the IWM status register
    // compatibility check below can detect Q6=1, Q7=0.  Only update Q6/Q7
    // (offsets 12-15) to avoid side effects from CA/ENABLE/SELECT changes.
    if (floppy->in_ism_mode && offset >= 12) {
        // Offsets 12-15 map to Q6_OFF/Q6_ON/Q7_OFF/Q7_ON
        static const uint8_t masks[] = {IWM_LINE_Q6, IWM_LINE_Q6, IWM_LINE_Q7, IWM_LINE_Q7};
        uint8_t mask = masks[offset - 12];
        if (offset & 1)
            floppy->iwm_lines |= mask;
        else
            floppy->iwm_lines &= ~mask;
    }

    if (floppy->in_ism_mode) {
        // The SWIM's IWM status register (Q6=1, Q7=0) remains accessible
        // even in ISM mode for backward compatibility.  MacTest's CHECK_SENSE
        // reads the IWM status register to poll TACH/sense lines while the
        // SWIM is in ISM mode.  Without this, the read returns the ISM mode
        // register (rStatus) instead of the actual SENSE bit, breaking
        // speed measurement and other sense-line diagnostics.
        if (IWM_Q6(floppy) && !IWM_Q7(floppy)) {
            return floppy_iwm_read(floppy, offset);
        }
        if (offset < 8) {
            LOG(7, "ISM: Read from write-only address %d", offset);
            return 0;
        }
        return swim_ism_read(floppy, offset);
    } else {
        return floppy_iwm_read(floppy, offset);
    }
}

// Writes to the SWIM at the given register offset (0-15)
void floppy_swim_write(floppy_t *floppy, unsigned reg, uint8_t byte) {
    uint32_t offset = reg & 0x0Fu;
    GS_ASSERT(offset < 16);

    if (floppy->in_ism_mode) {
        if (offset >= 8) {
            LOG(7, "ISM: Write to read-only address %d = 0x%02X", offset, byte);
            return;
        }
        swim_ism_write(floppy, offset, byte);
    } else {
        floppy_iwm_write(floppy, offset, byte);
    }
}

// ============================================================================
// SWIM Memory Interface -- INDEX-ADDRESSED
// ============================================================================
//
// `addr` here is a REGISTER INDEX (0-15), not a bus address.  Whoever owns the
// window maps addresses onto it: the GLUE/MDU/MCU I/O tables via
// MAC030_IO_STRIDE_512 (the chip's A0-A3 are wired to A9-A12), and the IIfx /
// Q900 IOP via swim_bypass_addr() (2-byte centres from +$20).  This file used
// to bake the SE/30's `(addr >> 9) & 0x0F` in, which is why every register
// aliased to index 0 through the IOP bypass (02-floppy F-03).

// Memory interface handler for 8-bit reads
static uint8_t swim_read_uint8(void *ctx, uint32_t addr) {
    return floppy_swim_read((floppy_t *)ctx, addr);
}

// The chip is on one byte of the data bus, so a wide access reaches nothing.
// These used to GS_ASSERT(0) -- which prints and PAUSES THE SCHEDULER rather
// than aborting, so any guest executing `move.w $D80000,d0`, buggy or hostile,
// halted the emulator and surfaced in CI as an unexplained hang (02-floppy
// F-32).  Log it and return open bus, as grand_central.c does.
static uint16_t swim_read_uint16(void *ctx, uint32_t addr) {
    (void)ctx;
    LOG(1, "%s: 16-bit access at 0x%08X is not decoded; reading open bus", "''' + name + r'''", addr);
    return 0xFFFF;
}

static uint32_t swim_read_uint32(void *ctx, uint32_t addr) {
    (void)ctx;
    LOG(1, "%s: 32-bit access at 0x%08X is not decoded; reading open bus", "''' + name + r'''", addr);
    return 0xFFFFFFFFu;
}

// Memory interface handler for 8-bit writes
static void swim_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    floppy_swim_write((floppy_t *)ctx, addr, value);
}

static void swim_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    (void)ctx;
    (void)value;
    LOG(1, "%s: 16-bit write at 0x%08X is not decoded; dropped", "''' + name + r'''", addr);
}

static void swim_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    (void)ctx;
    (void)value;
    LOG(1, "%s: 32-bit write at 0x%08X is not decoded; dropped", "''' + name + r'''", addr);
}

// ============================================================================
// SWIM Setup and Motor Callback
// ============================================================================

// SWIM motor spin-up callback (separate identity for scheduler)
void floppy_swim_motor_spinup_callback(void *source, uint64_t data) {
    floppy_t *floppy = (floppy_t *)source;
    int drive_index = (int)data;

    if (drive_index < 0 || drive_index >= NUM_DRIVES) {
        LOG(1, "Drive %d: Invalid drive in spin-up callback", drive_index);
        return;
    }

    floppy->drives[drive_index].motor_spinning_up = false;
    LOG(3, "Drive %d: Motor spin-up complete, now ready", drive_index);
}

// Sets up SWIM memory interface callbacks and ISM initial state.
// `map` is always NULL: every SWIM machine decodes its own window and reaches
// the chip through floppy_get_memory_interface().  The parameter stays for
// signature symmetry with floppy_iwm_setup.
void floppy_swim_setup(floppy_t *floppy, memory_map_t *map) {
    // ISM initial state (chip powers up in IWM mode)
    floppy->in_ism_mode = false;
    floppy->ism_phase = 0xF0;
    floppy->ism_crc = CRC_INIT;
    floppy->mfm_cur_sector = 1;
    floppy->mfm_cur_side = 0;

    // Set up memory interface
    floppy->memory_interface.read_uint8 = &swim_read_uint8;
    floppy->memory_interface.read_uint16 = &swim_read_uint16;
    floppy->memory_interface.read_uint32 = &swim_read_uint32;
    floppy->memory_interface.write_uint8 = &swim_write_uint8;
    floppy->memory_interface.write_uint16 = &swim_write_uint16;
    floppy->memory_interface.write_uint32 = &swim_write_uint32;

    (void)map;
}
