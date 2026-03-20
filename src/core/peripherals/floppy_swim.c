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

LOG_USE_CATEGORY_NAME("swim");

// SWIM base address on SE/30 and mapped window size
#define SWIM_BASE_ADDR 0x50016000
#define SWIM_MAP_SIZE  0x2000 // 16 registers x 512-byte stride = 8 KB

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

// Builds an MFM sector in the sector buffer for the current track/side/sector
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

    // Determine sectors per track from disk size
    size_t disk_sz = disk_size(img);
    int sectors_per_track;
    if (disk_sz > 1000000) // > ~1MB = 1440K
        sectors_per_track = 18;
    else
        sectors_per_track = 9;

    if (sector < 1 || sector > sectors_per_track) {
        floppy->mfm_buf_len = 0;
        return;
    }

    // Calculate disk image offset: MFM uses side-interleaved layout
    // block = (track * 2 + side) * sectors_per_track + (sector - 1)
    size_t block = (size_t)(track * 2 + side) * sectors_per_track + (sector - 1);
    size_t offset = block * 512;
    if (offset + 512 > disk_sz) {
        floppy->mfm_buf_len = 0;
        return;
    }

    uint8_t sector_data[512];
    size_t read = disk_read_data(img, offset, sector_data, 512);
    if (read != 512) {
        floppy->mfm_buf_len = 0;
        return;
    }

    // Build the sector buffer: address field + gap + data field
    uint8_t *buf = floppy->mfm_sector_buf;
    bool *marks = floppy->mfm_sector_mark;
    int pos = 0;

    memset(marks, 0, MFM_SECTOR_BUF_SIZE);

    // Sync bytes (12 x $00)
    for (int i = 0; i < 12; i++)
        buf[pos++] = 0x00;

    // Address mark: 3x mark $A1 + $FE
    marks[pos] = true;
    buf[pos++] = 0xA1;
    marks[pos] = true;
    buf[pos++] = 0xA1;
    marks[pos] = true;
    buf[pos++] = 0xA1;
    buf[pos++] = 0xFE;

    // Address field: cylinder, side, sector, size code
    buf[pos++] = (uint8_t)track;
    buf[pos++] = (uint8_t)side;
    buf[pos++] = (uint8_t)sector;
    buf[pos++] = 0x02; // 512 bytes/sector

    // CRC over last mark byte + $FE + 4 address bytes
    uint16_t crc = CRC_INIT;
    crc = crc_ccitt_byte(crc, 0xA1);
    crc = crc_ccitt_byte(crc, 0xFE);
    crc = crc_ccitt_byte(crc, (uint8_t)track);
    crc = crc_ccitt_byte(crc, (uint8_t)side);
    crc = crc_ccitt_byte(crc, (uint8_t)sector);
    crc = crc_ccitt_byte(crc, 0x02);
    buf[pos++] = (uint8_t)(crc >> 8);
    buf[pos++] = (uint8_t)(crc & 0xFF);

    // Gap2 (22 x $4E)
    for (int i = 0; i < 22; i++)
        buf[pos++] = 0x4E;

    // Sync bytes (12 x $00)
    for (int i = 0; i < 12; i++)
        buf[pos++] = 0x00;

    // Data mark: 3x mark $A1 + $FB
    marks[pos] = true;
    buf[pos++] = 0xA1;
    marks[pos] = true;
    buf[pos++] = 0xA1;
    marks[pos] = true;
    buf[pos++] = 0xA1;
    buf[pos++] = 0xFB;

    // Sector data (512 bytes)
    memcpy(&buf[pos], sector_data, 512);
    pos += 512;

    // CRC over last data mark byte + sector data
    crc = CRC_INIT;
    crc = crc_ccitt_byte(crc, 0xA1);
    crc = crc_ccitt_byte(crc, 0xFB);
    for (int i = 0; i < 512; i++)
        crc = crc_ccitt_byte(crc, sector_data[i]);
    buf[pos++] = (uint8_t)(crc >> 8);
    buf[pos++] = (uint8_t)(crc & 0xFF);

    // Gap3 (inter-sector gap)
    int gap3_len = (sectors_per_track == 18) ? 101 : 80;
    for (int i = 0; i < gap3_len; i++)
        buf[pos++] = 0x4E;

    floppy->mfm_buf_len = (uint16_t)pos;
    floppy->mfm_buf_pos = 0;
    floppy->mfm_cur_track = (uint8_t)track;
    floppy->mfm_cur_side = (uint8_t)side;

    LOG(4, "SWIM ISM: Built MFM sector T=%d S=%d Sec=%d (%d bytes)", track, side, sector, pos);
}

// Advances to the next MFM sector and builds its buffer
static void mfm_advance_sector(floppy_t *floppy) {
    int drv = (floppy->ism_mode & ISM_MODE_DRIVE2) ? 1 : 0;
    image_t *img = floppy->disk[drv];
    size_t disk_sz = img ? disk_size(img) : 0;
    int sectors_per_track = (disk_sz > 1000000) ? 18 : 9;

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
// SWIM IWM-Mode Read/Write (with mode-switch detection and echo behavior)
// ============================================================================

// Reads from the IWM register (SWIM in IWM mode)
static uint8_t swim_iwm_read(floppy_t *floppy, uint32_t offset) {
    GS_ASSERT(offset < 16);

    floppy_update_iwm_lines(floppy, offset);

    int drv = DRIVE_INDEX(floppy);

    // Mode register is WRITE ONLY
    if (IWM_Q6(floppy) && IWM_Q7(floppy))
        GS_ASSERT(0);

    // Read status register: Q6=1, Q7=0
    if (IWM_Q6(floppy) && !IWM_Q7(floppy)) {
        uint8_t status = floppy->mode & IWM_STATUS_MODE;
        if (IWM_ENABLE(floppy))
            status |= IWM_STATUS_ENABLE;
        if (floppy_disk_status(floppy, drv))
            status |= IWM_STATUS_SENSE;
        LOG(8, "  status=0x%02X (enable=%d mode=0x%02X)", status, IWM_ENABLE(floppy) ? 1 : 0,
            floppy->mode & IWM_STATUS_MODE);
        return status;
    }

    // Read handshake register: Q6=0, Q7=1
    if (!IWM_Q6(floppy) && IWM_Q7(floppy)) {
        uint8_t hdshk = IWM_HDSHK_RES | IWM_HDSHK_WRITE | IWM_HDSHK_WB_EMTPY;
        LOG(6, "Drive %d: Reading handshake = 0x%02X", drv, hdshk);
        return hdshk;
    }

    // Read data register: Q6=0, Q7=0
    if (!IWM_Q6(floppy) && !IWM_Q7(floppy)) {
        if (!(floppy->mode & IWM_MODE_ASYNC)) {
            LOG(6, "Drive %d: Sync read mode not implemented = 0x00", drv);
            return 0;
        }

        if (!IWM_ENABLE(floppy)) {
            // SWIM echo: return the last written data bus byte if latch is valid
            uint8_t val = floppy->iwm_latch_valid ? floppy->iwm_write_latch : 0xFF;
            floppy->iwm_latch_valid = false;
            LOG(6, "Drive %d: Reading data (disabled) echo = 0x%02X", drv, val);
            return val;
        }

        if (floppy->disk[drv] == NULL) {
            LOG(6, "Drive %d: Reading data (no disk) = 0x00", drv);
            return 0x00;
        }

        floppy_drive_t *drive = &floppy->drives[drv];
        int side = floppy->sel ? 1 : 0;
        uint8_t *data = iwm_track_data(drive, floppy->disk[drv], side, floppy->scheduler);
        if (!data) {
            // HD/MFM disk in drive: return 0x00 so IWM GCR sync detection fails
            LOG(5, "Drive %d: No GCR track data (MFM disk)", drv);
            return 0x00;
        }

        size_t trk_len = iwm_track_length(drive->track);

        // IWM latch mode: only bytes with MSB=1 are latched (valid GCR bytes)
        if (floppy->mode & IWM_MODE_LATCH) {
            for (size_t i = 0; i < trk_len; i++) {
                uint8_t byte = data[drive->offset++];
                if (drive->offset >= (int)trk_len)
                    drive->offset = 0;
                if (byte & 0x80) {
                    LOG(8, "Drive %d: Reading data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, side,
                        drive->offset, byte);
                    return byte;
                }
            }
            LOG(6, "Drive %d: No valid GCR byte found on track", drv);
            return 0x00;
        }

        // Non-latch mode: return raw bytes
        uint8_t ret = data[drive->offset++];
        if (drive->offset >= (int)trk_len)
            drive->offset = 0;
        LOG(8, "Drive %d: Reading data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, side, drive->offset,
            ret);
        return ret;
    }

    GS_ASSERT(0);
    return 0;
}

// Writes a byte to the IWM register (SWIM in IWM mode), with mode-switch detection
static void swim_iwm_write(floppy_t *floppy, uint32_t offset, uint8_t byte) {
    floppy_update_iwm_lines(floppy, offset);

    // SWIM latches every data bus byte for echo detection (unlike plain IWM)
    floppy->iwm_write_latch = byte;
    floppy->iwm_latch_valid = true;

    if (!IWM_Q6(floppy) || !IWM_Q7(floppy))
        return;

    int drv = DRIVE_INDEX(floppy);

    if (IWM_ENABLE(floppy)) {
        // Write data to disk
        floppy_drive_t *drive = &floppy->drives[drv];
        int side = floppy->sel ? 1 : 0;
        uint8_t *data = iwm_track_data(drive, floppy->disk[drv], side, floppy->scheduler);
        if (!data) {
            LOG(1, "Drive %d: Write failed - no track data", drv);
            return;
        }

        data[drive->offset++] = byte;
        LOG(8, "Drive %d: Writing data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, side, drive->offset - 1,
            byte);

        floppy_track_t *trk = &drive->tracks[side][drive->track];
        if (!trk->modified) {
            trk->modified = true;
            LOG(5, "Drive %d: Track %d side %d marked dirty", drv, drive->track, side);
        }

        if (drive->offset == (int)iwm_track_length(drive->track))
            drive->offset = 0;
    } else {
        // Write IWM mode register — track bit 6 for ISM switch sequence
        uint8_t bit6 = (byte >> 6) & 1;

        if (bit6 == ISM_SWITCH_PATTERN[floppy->mode_switch_count]) {
            floppy->mode_switch_count++;
            LOG(5, "SWIM: Mode switch sequence %d/4 (bit6=%d)", floppy->mode_switch_count, bit6);

            if (floppy->mode_switch_count == 4) {
                // 4-write sequence complete: switch to ISM mode
                floppy->in_ism_mode = true;
                floppy->mode_switch_count = 0;
                swim_ism_reset(floppy);
                LOG(2, "SWIM: Switched to ISM mode");
                return;
            }
        } else {
            // Mismatch: reset sequence
            floppy->mode_switch_count = 0;
            // Check if this byte starts a new sequence
            if (bit6 == ISM_SWITCH_PATTERN[0])
                floppy->mode_switch_count = 1;
        }

        floppy->mode = byte;
        LOG(4, "SWIM IWM: Mode register = 0x%02X", byte);
    }
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

    if (new_lstrb && !old_lstrb) {
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
        if ((floppy->ism_mode & ISM_MODE_ACTION) && !(floppy->ism_mode & ISM_MODE_WRITE))
            mfm_fill_fifo(floppy);

        bool is_mark = false;
        uint8_t byte = ism_fifo_pop(floppy, &is_mark);
        if (is_mark && !floppy->ism_error)
            floppy->ism_error |= ISM_ERR_MARK_IN_DATA;
        LOG(7, "ISM rData: 0x%02X (mark=%d, fifo=%d)", byte, is_mark, floppy->ism_fifo_count);
        if (floppy->ism_mode & ISM_MODE_ACTION)
            LOG(2, "ISM rData: 0x%02X mark=%d crc=0x%04X err=0x%02X", byte, is_mark, floppy->ism_crc,
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
            LOG(2, "ISM rMark: 0x%02X mark=%d crc=0x%04X err=0x%02X", byte, is_mark, floppy->ism_crc,
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
        int drv = (floppy->ism_mode & ISM_MODE_DRIVE2) ? 1 : (floppy->ism_mode & ISM_MODE_DRIVE1) ? 0 : 0;

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

        // Bits 6-7: FIFO status
        if (floppy->ism_mode & ISM_MODE_WRITE) {
            // Write mode: drain FIFO when ACTION is active
            if (floppy->ism_mode & ISM_MODE_ACTION)
                floppy->ism_fifo_count = 0;
            int space = ISM_FIFO_SIZE - floppy->ism_fifo_count;
            if (space >= 1)
                hdshk |= ISM_HDSHK_DAT1BYTE;
            if (space >= 2)
                hdshk |= ISM_HDSHK_DAT2BYTE;
        } else {
            // Read mode: refill FIFO first if reading
            if (floppy->ism_mode & ISM_MODE_ACTION)
                mfm_fill_fifo(floppy);
            if (floppy->ism_fifo_count >= 1)
                hdshk |= ISM_HDSHK_DAT1BYTE;
            if (floppy->ism_fifo_count >= 2)
                hdshk |= ISM_HDSHK_DAT2BYTE;
        }

        LOG(7, "ISM rHandshake: 0x%02X (fifo=%d, err=0x%02X)", hdshk, floppy->ism_fifo_count, floppy->ism_error);
        if (floppy->ism_mode & ISM_MODE_ACTION)
            LOG(2, "ISM rHdshk: 0x%02X mark=%d crc_nz=%d err=%d fifo=%d pos=%d/%d", hdshk,
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
            mfm_fill_fifo(floppy);
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
        floppy->ism_crc = crc_ccitt_byte(floppy->ism_crc, byte);
        LOG(7, "ISM wData: 0x%02X (fifo=%d)", byte, floppy->ism_fifo_count);
        break;

    case 1: // wMark: push mark byte into FIFO
        if (ism_fifo_push(floppy, byte, true)) {
            if (!floppy->ism_error)
                floppy->ism_error |= ISM_ERR_OVERRUN;
        }
        floppy->ism_crc = crc_ccitt_byte(floppy->ism_crc, byte);
        LOG(7, "ISM wMark: 0x%02X (fifo=%d)", byte, floppy->ism_fifo_count);
        break;

    case 2: // wCRC (ACTION=1) or wIWMConfig (ACTION=0)
        if (floppy->ism_mode & ISM_MODE_ACTION) {
            uint8_t crc_hi = (uint8_t)(floppy->ism_crc >> 8);
            uint8_t crc_lo = (uint8_t)(floppy->ism_crc & 0xFF);
            ism_fifo_push(floppy, crc_hi, false);
            ism_fifo_push(floppy, crc_lo, false);
            LOG(6, "ISM wCRC: appended CRC 0x%04X", floppy->ism_crc);
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
        floppy->ism_param_idx = 0; // any access to addr 6 resets param counter
        LOG(2, "ISM wZeros: 0x%02X (mode: 0x%02X -> 0x%02X) pos=%d/%d", byte, old_mode, floppy->ism_mode,
            floppy->mfm_buf_pos, floppy->mfm_buf_len);

        // Log when ACTION is cleared
        if ((old_mode & ISM_MODE_ACTION) && !(floppy->ism_mode & ISM_MODE_ACTION))
            LOG(3, "ISM: ACTION cleared, buf consumed %d/%d sec=%d", floppy->mfm_buf_pos, floppy->mfm_buf_len,
                floppy->mfm_cur_sector);

        // If bit 6 was cleared: switch back to IWM mode
        if ((old_mode & ISM_MODE_ISM_IWM) && !(floppy->ism_mode & ISM_MODE_ISM_IWM)) {
            floppy->in_ism_mode = false;
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
        break;
    }
    case 7: { // wOnes: set specified bits in mode register
        uint8_t old_mode = floppy->ism_mode;
        floppy->ism_mode |= byte;
        LOG(2, "ISM wOnes: 0x%02X (mode: 0x%02X -> 0x%02X) pos=%d/%d", byte, old_mode, floppy->ism_mode,
            floppy->mfm_buf_pos, floppy->mfm_buf_len);

        swim_handle_fifo_clear(floppy, old_mode);
        swim_handle_action_set(floppy, old_mode);
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
static uint8_t swim_read(floppy_t *floppy, uint32_t offset) {
    GS_ASSERT(offset < 16);

    if (floppy->in_ism_mode) {
        if (offset < 8) {
            LOG(7, "ISM: Read from write-only address %d", offset);
            return 0;
        }
        return swim_ism_read(floppy, offset);
    } else {
        return swim_iwm_read(floppy, offset);
    }
}

// Writes to the SWIM at the given register offset (0-15)
static void swim_write(floppy_t *floppy, uint32_t offset, uint8_t byte) {
    GS_ASSERT(offset < 16);

    if (floppy->in_ism_mode) {
        if (offset >= 8) {
            LOG(7, "ISM: Write to read-only address %d = 0x%02X", offset, byte);
            return;
        }
        swim_ism_write(floppy, offset, byte);
    } else {
        swim_iwm_write(floppy, offset, byte);
    }
}

// ============================================================================
// SWIM Memory Interface (SE/30 address decoding)
// ============================================================================

// Memory interface handler for 8-bit reads
static uint8_t swim_read_uint8(void *ctx, uint32_t addr) {
    floppy_t *s = (floppy_t *)ctx;
    uint32_t offset = (addr >> 9) & 0x0F;
    return swim_read(s, offset);
}

// Memory interface handler for 16-bit reads (not supported)
static uint16_t swim_read_uint16(void *ctx, uint32_t addr) {
    (void)ctx;
    (void)addr;
    GS_ASSERT(0);
    return 0;
}

// Memory interface handler for 32-bit reads (not supported)
static uint32_t swim_read_uint32(void *ctx, uint32_t addr) {
    (void)ctx;
    (void)addr;
    GS_ASSERT(0);
    return 0;
}

// Memory interface handler for 8-bit writes
static void swim_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    floppy_t *s = (floppy_t *)ctx;
    uint32_t offset = (addr >> 9) & 0x0F;
    swim_write(s, offset, value);
}

// Memory interface handler for 16-bit writes (not supported)
static void swim_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    (void)ctx;
    (void)addr;
    (void)value;
    GS_ASSERT(0);
}

// Memory interface handler for 32-bit writes (not supported)
static void swim_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    (void)ctx;
    (void)addr;
    (void)value;
    GS_ASSERT(0);
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

// Sets up SWIM memory interface callbacks and ISM initial state
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

    // Register with memory map if provided (NULL = machine handles registration)
    if (map)
        memory_map_add(map, SWIM_BASE_ADDR, SWIM_MAP_SIZE, "swim", &floppy->memory_interface, floppy);
}
