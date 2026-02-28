// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// swim.c
// Implements the SWIM (Sander-Wozniak Integrated Machine) floppy disk
// controller for the SE/30. The SWIM contains two independent cores:
//   - IWM core: GCR-only, latch/state programming model (Mac Plus compatible)
//   - ISM core: GCR + MFM, 16-register interface with FIFO, CRC, parameter RAM
//
// Only one core is active at a time. The chip resets into IWM mode.
// IWM->ISM switching uses a 4-write handshake on the mode register.
// ISM->IWM switching writes zeros to the mode register with bit 6 set.
//
// Memory mapping: SE/30 maps SWIM at 0x50016000 with 512-byte register stride
// (same decode as Mac Plus IWM): register = (addr >> 9) & 0xF.

// ============================================================================
// Includes
// ============================================================================

#include "swim.h"
#include "iwm_swim.h"
#include "log.h"
#include "memory.h"
#include "system.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

// ============================================================================
// Constants
// ============================================================================

LOG_USE_CATEGORY_NAME("swim");

// SWIM base address on SE/30 and mapped window size
#define SWIM_BASE_ADDR 0x50016000
#define SWIM_MAP_SIZE  0x2000 // 16 registers x 512-byte stride = 8 KB

// ISM mode register bits
#define ISM_MODE_CLEAR_FIFO 0x01 // toggle 1->0 to clear FIFO and init CRC
#define ISM_MODE_DRIVE1     0x02 // enable internal drive
#define ISM_MODE_DRIVE2     0x04 // enable external drive
#define ISM_MODE_ACTION     0x08 // start read/write operation
#define ISM_MODE_WRITE      0x10 // 0=read, 1=write
#define ISM_MODE_HDSEL      0x20 // head select output (if Setup bit 0 = 1)
#define ISM_MODE_ISM_IWM    0x40 // clearing switches back to IWM mode
#define ISM_MODE_MOTOR_ON   0x80 // enable /ENBL outputs

// ISM setup register bits
#define ISM_SETUP_HDSEL_EN   0x01 // 0=Q3 input, 1=HDSEL output
#define ISM_SETUP_SEL35      0x02 // 3.5" select (inverted output)
#define ISM_SETUP_GCR        0x04 // 1=GCR mode, 0=MFM mode
#define ISM_SETUP_FCLK_DIV2  0x08 // divide FCLK by 2
#define ISM_SETUP_ENABLE_ECM 0x10 // enable error correction
#define ISM_SETUP_IBM_DRIVE  0x20 // 1=IBM drive (pulses)
#define ISM_SETUP_TRANS_DIS  0x40 // bypass Trans-Space Machine (for GCR)
#define ISM_SETUP_MOTOR_TMO  0x80 // motor timeout enable

// ISM error register bits (first-error-wins, read-clears)
#define ISM_ERR_UNDERRUN     0x01 // CPU not writing fast enough
#define ISM_ERR_MARK_IN_DATA 0x02 // mark byte read from data register
#define ISM_ERR_OVERRUN      0x04 // FIFO empty on read / full on write

// ISM handshake register bits
#define ISM_HDSHK_MARK_BYTE 0x01 // next FIFO byte is a mark
#define ISM_HDSHK_CRC_NZ    0x02 // CRC non-zero
#define ISM_HDSHK_RDDATA    0x04 // current RDDATA input state
#define ISM_HDSHK_SENSE     0x08 // drive SENSE output
#define ISM_HDSHK_MOTOR_ON  0x10 // motor on or timer active
#define ISM_HDSHK_ERROR     0x20 // any error bit set
#define ISM_HDSHK_DAT2BYTE  0x40 // FIFO has 2 bytes / space for 2
#define ISM_HDSHK_DAT1BYTE  0x80 // FIFO has >= 1 byte / space for >= 1

// ISM FIFO capacity
#define ISM_FIFO_SIZE 2

// CRC-CCITT initial value
#define CRC_INIT 0xFFFF

// IWM->ISM mode switch: expected bit-6 pattern in 4 consecutive mode writes
static const uint8_t ISM_SWITCH_PATTERN[4] = {1, 0, 1, 1};

// Size of plain-data portion for checkpointing (excludes pointers at end)
#define SWIM_CHECKPOINT_SIZE offsetof(swim_t, disk)

// MFM sector buffer: enough for gap + sync + mark + header + CRC + gap + sync + mark + 512 data + CRC
#define MFM_SECTOR_BUF_SIZE 768

// ============================================================================
// SWIM State
// ============================================================================

// Represents the SWIM floppy disk controller (dual-mode: IWM + ISM)
struct swim {
    // === Plain data (checkpointed via single memcpy) ===

    // IWM controller state (shared drive interface)
    uint8_t iwm_lines; // packed IWM state lines (CA0-Q7)
    uint8_t iwm_mode; // IWM mode register
    bool sel; // VIA-driven SEL signal (IWM head select)
    floppy_drive_t drives[NUM_DRIVES]; // drive state machines

    // SWIM mode tracking
    bool in_ism_mode; // true = ISM mode active
    uint8_t mode_switch_count; // 0-4: position in IWM->ISM 4-write sequence

    // ISM registers
    uint8_t ism_mode; // mode/status register (zeros/ones write)
    uint8_t ism_setup; // setup register
    uint8_t ism_error; // error register (read-clears, first-error-wins)
    uint8_t ism_phase; // phase register (reset: 0xF0)
    uint8_t ism_param[16]; // parameter RAM (16 bytes, auto-increment)
    uint8_t ism_param_idx; // parameter RAM pointer (resets on addr 6)
    uint8_t ism_iwm_config; // IWM configuration register

    // ISM FIFO
    uint8_t ism_fifo[ISM_FIFO_SIZE]; // 2-byte FIFO
    uint8_t ism_fifo_count; // bytes currently in FIFO (0-2)
    bool ism_fifo_mark[ISM_FIFO_SIZE]; // mark flag per FIFO byte

    // ISM CRC state
    uint16_t ism_crc; // running CRC-CCITT-16

    // MFM sector-level emulation state
    uint8_t mfm_sector_buf[MFM_SECTOR_BUF_SIZE]; // pre-built sector data
    uint16_t mfm_buf_pos; // current read position in sector buffer
    uint16_t mfm_buf_len; // valid bytes in sector buffer
    uint8_t mfm_cur_sector; // current sector number (1-based)
    uint8_t mfm_cur_track; // current track for MFM read
    uint8_t mfm_cur_side; // current side for MFM read
    bool mfm_sector_mark[MFM_SECTOR_BUF_SIZE]; // per-byte mark flags in sector buffer

    // === Pointers (not checkpointed) ===
    image_t *disk[NUM_DRIVES]; // disk image pointers
    struct scheduler *scheduler; // scheduler for timed events
    memory_interface_t memory_interface; // SWIM memory interface
};

// ============================================================================
// Forward Declarations
// ============================================================================

static void swim_motor_spinup_callback(void *source, uint64_t data);

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
static bool ism_fifo_push(swim_t *swim, uint8_t byte, bool is_mark) {
    if (swim->ism_fifo_count >= ISM_FIFO_SIZE)
        return true; // overflow
    swim->ism_fifo[swim->ism_fifo_count] = byte;
    swim->ism_fifo_mark[swim->ism_fifo_count] = is_mark;
    swim->ism_fifo_count++;
    return false;
}

// Pops a byte from the ISM FIFO; returns 0xFF and sets overflow error on underrun
static uint8_t ism_fifo_pop(swim_t *swim, bool *is_mark_out) {
    if (swim->ism_fifo_count == 0) {
        // Underrun: set error if not already set
        if (!swim->ism_error)
            swim->ism_error |= ISM_ERR_OVERRUN;
        if (is_mark_out)
            *is_mark_out = false;
        return 0xFF;
    }
    uint8_t byte = swim->ism_fifo[0];
    bool is_mark = swim->ism_fifo_mark[0];
    // Shift FIFO down
    swim->ism_fifo[0] = swim->ism_fifo[1];
    swim->ism_fifo_mark[0] = swim->ism_fifo_mark[1];
    swim->ism_fifo_count--;
    if (is_mark_out)
        *is_mark_out = is_mark;
    return byte;
}

// ============================================================================
// MFM Sector-Level Emulation
// ============================================================================

// Builds an MFM sector in the sector buffer for the current track/side/sector
static void mfm_build_sector(swim_t *swim) {
    int drv = (swim->ism_mode & ISM_MODE_DRIVE2) ? 1 : 0;
    image_t *img = swim->disk[drv];
    if (!img) {
        swim->mfm_buf_len = 0;
        return;
    }

    floppy_drive_t *drive = &swim->drives[drv];
    int track = drive->track;
    // ISM mode: head select from mode register bit 5
    int side = (swim->ism_mode & ISM_MODE_HDSEL) ? 1 : 0;
    int sector = swim->mfm_cur_sector; // 1-based

    // Determine sectors per track from disk size
    size_t disk_sz = disk_size(img);
    int sectors_per_track;
    if (disk_sz > 1000000) // > ~1MB = 1440K
        sectors_per_track = 18;
    else
        sectors_per_track = 9;

    if (sector < 1 || sector > sectors_per_track) {
        swim->mfm_buf_len = 0;
        return;
    }

    // Calculate disk image offset: MFM uses side-interleaved layout
    // block = (track * 2 + side) * sectors_per_track + (sector - 1)
    size_t block = (size_t)(track * 2 + side) * sectors_per_track + (sector - 1);
    size_t offset = block * 512;
    if (offset + 512 > disk_sz) {
        swim->mfm_buf_len = 0;
        return;
    }

    uint8_t sector_data[512];
    size_t read = disk_read_data(img, offset, sector_data, 512);
    if (read != 512) {
        swim->mfm_buf_len = 0;
        return;
    }

    // Build the sector buffer: address field + gap + data field
    uint8_t *buf = swim->mfm_sector_buf;
    bool *marks = swim->mfm_sector_mark;
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

    // CRC over address mark ($A1x3) + $FE + 4 address bytes
    uint16_t crc = CRC_INIT;
    crc = crc_ccitt_byte(crc, 0xA1);
    crc = crc_ccitt_byte(crc, 0xA1);
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

    // CRC over data mark + sector data
    crc = CRC_INIT;
    crc = crc_ccitt_byte(crc, 0xA1);
    crc = crc_ccitt_byte(crc, 0xA1);
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

    swim->mfm_buf_len = (uint16_t)pos;
    swim->mfm_buf_pos = 0;
    swim->mfm_cur_track = (uint8_t)track;
    swim->mfm_cur_side = (uint8_t)side;

    LOG(4, "SWIM ISM: Built MFM sector T=%d S=%d Sec=%d (%d bytes)", track, side, sector, pos);
}

// Advances to the next MFM sector and builds its buffer
static void mfm_advance_sector(swim_t *swim) {
    int drv = (swim->ism_mode & ISM_MODE_DRIVE2) ? 1 : 0;
    image_t *img = swim->disk[drv];
    size_t disk_sz = img ? disk_size(img) : 0;
    int sectors_per_track = (disk_sz > 1000000) ? 18 : 9;

    swim->mfm_cur_sector++;
    if (swim->mfm_cur_sector > sectors_per_track)
        swim->mfm_cur_sector = 1; // wrap to sector 1

    mfm_build_sector(swim);
}

// Fills the ISM FIFO from the MFM sector buffer during a read operation
static void mfm_fill_fifo(swim_t *swim) {
    while (swim->ism_fifo_count < ISM_FIFO_SIZE && swim->mfm_buf_pos < swim->mfm_buf_len) {
        uint8_t byte = swim->mfm_sector_buf[swim->mfm_buf_pos];
        bool mark = swim->mfm_sector_mark[swim->mfm_buf_pos];
        ism_fifo_push(swim, byte, mark);
        swim->mfm_buf_pos++;
    }

    // If sector buffer exhausted, advance to next sector
    if (swim->mfm_buf_pos >= swim->mfm_buf_len && swim->mfm_buf_len > 0) {
        mfm_advance_sector(swim);
        // Continue filling if FIFO still has room
        while (swim->ism_fifo_count < ISM_FIFO_SIZE && swim->mfm_buf_pos < swim->mfm_buf_len) {
            uint8_t byte = swim->mfm_sector_buf[swim->mfm_buf_pos];
            bool mark = swim->mfm_sector_mark[swim->mfm_buf_pos];
            ism_fifo_push(swim, byte, mark);
            swim->mfm_buf_pos++;
        }
    }
}

// ============================================================================
// Drive Status and Control (SWIM-specific, adapted from floppy.c)
// ============================================================================

// Returns the current disk status based on CA lines, SEL, and drive state
static int swim_disk_status(swim_t *swim, int drv) {
    floppy_drive_t *drive = &swim->drives[drv];

    // Build status key from CA lines and SEL signal
    int key =
        (IWM_CA0(swim) ? 0x01 : 0) | (IWM_CA1(swim) ? 0x02 : 0) | (IWM_CA2(swim) ? 0x04 : 0) | (swim->sel ? 0x08 : 0);

    int ret = 0;
    const char *desc = "unknown";

    switch (key) {
    case 0x00: // /DIRTN
        desc = "/DIRTN";
        ret = !drive->_dirtn;
        break;
    case 0x01: // /STEP
        desc = "/STEP";
        ret = 1;
        break;
    case 0x02: // /MOTORON
        desc = "/MOTORON";
        ret = !drive->_motoron;
        break;
    case 0x03: // EJECT
        desc = "EJECT";
        ret = 0;
        break;
    case 0x04: // RDDATA side 0
        desc = "RDDATA side0";
        ret = 1;
        break;
    case 0x05: // mfmDrv: SuperDrive present
        desc = "mfmDrv";
        ret = 1; // SWIM always has a SuperDrive (FDHD capable)
        break;
    case 0x06: // /SIDES
        desc = "/SIDES";
        ret = 1; // double-sided drive
        break;
    case 0x07: // /DRVIN
        desc = "/DRVIN";
        ret = 0; // drive connected
        break;
    case 0x08: // /CSTIN: zero when disk in drive
        desc = "/CSTIN";
        ret = (swim->disk[drv] == NULL);
        break;
    case 0x09: // /WRTPRT: zero when write protected
        desc = "/WRTPRT";
        ret = (swim->disk[drv] != NULL) ? swim->disk[drv]->writable : 0;
        break;
    case 0x0A: // /TK0: zero when head on track 0
        desc = "/TK0";
        ret = (drive->track != 0);
        break;
    case 0x0B: // /TACH: 60 pulses per revolution
    {
        const char *tach_reason = NULL;
        ret = iwm_tach_signal(swim->scheduler, drive, &tach_reason);
        LOG(6, "Drive %d: Reading /TACH = %d (%s, track=%d)", drv, ret, tach_reason, drive->track);
        return ret;
    }
    case 0x0C: // RDDATA side 1
        desc = "RDDATA side1";
        ret = 1;
        break;
    case 0x0D: // reserved
        desc = "reserved (SEL+CA2+CA0)";
        ret = 0;
        break;
    case 0x0E: // /READY: zero when ready
        desc = "/READY";
        ret = drive->motor_spinning_up ? 1 : 0;
        break;
    case 0x0F: // NEWINTF / twoMeg: media type detection
        desc = "NEWINTF/twoMeg";
        // Return 1 for HD media (1.44MB disk), 0 for DD media
        if (swim->disk[drv] != NULL) {
            size_t sz = disk_size(swim->disk[drv]);
            ret = (sz > 1000000) ? 1 : 0; // HD media if > ~1 MB
        } else {
            ret = 0;
        }
        break;
    default:
        ret = 0;
        break;
    }

    LOG(6, "Drive %d: Reading %s = %d", drv, desc, ret);

    return ret;
}

// Processes disk control commands when LSTRB is high
static void swim_disk_control(swim_t *swim) {
    int drv = DRIVE_INDEX(swim);
    floppy_drive_t *drive = &swim->drives[drv];

    // Commands only when SEL is low
    if (swim->sel)
        return;

    if (IWM_CA0(swim)) {
        if (IWM_CA1(swim)) {
            // EJECT (CA0=1, CA1=1, CA2=1)
            if (IWM_CA2(swim)) {
                LOG(1, "Drive %d: Eject requested", drv);
                iwm_flush_modified_tracks(drive, swim->disk[drv], drv);
                memset(drive->tracks, 0, sizeof(drive->tracks));
                swim->disk[drv] = NULL;
                LOG(1, "Drive %d: Ejected", drv);
            }
        } else {
            // STEP (CA0=1, CA1=0, CA2=0)
            if (!IWM_CA2(swim)) {
                if (drive->_dirtn) {
                    if (drive->track > 0)
                        drive->track--;
                } else {
                    if (drive->track < NUM_TRACKS - 1)
                        drive->track++;
                }
                drive->offset = 0;
                LOG(3, "Drive %d: Step to track %d (%s)", drv, drive->track, drive->_dirtn ? "outward" : "inward");
            }
        }
    } else {
        if (IWM_CA1(swim)) {
            // MOTORON (CA0=0, CA1=1): CA2 sets motor state
            bool was_off = drive->_motoron;
            drive->_motoron = IWM_CA2(swim);
            bool now_on = !drive->_motoron;

            if (was_off && now_on) {
                drive->motor_spinning_up = true;
                remove_event(swim->scheduler, swim_motor_spinup_callback, swim);
                scheduler_new_cpu_event(swim->scheduler, swim_motor_spinup_callback, swim, (uint64_t)drv, 0,
                                        MOTOR_SPINUP_TIME_NS);
                LOG(2, "Drive %d: Motor ON (spinning up)", drv);
            } else if (!was_off && !now_on) {
                drive->motor_spinning_up = false;
                remove_event(swim->scheduler, swim_motor_spinup_callback, swim);
                LOG(2, "Drive %d: Motor OFF", drv);
            } else {
                LOG(4, "Drive %d: Motor %s (no change)", drv, drive->_motoron ? "off" : "on");
            }
        } else {
            // DIRTN (CA0=0, CA1=0): CA2 sets direction
            drive->_dirtn = IWM_CA2(swim);
            LOG(4, "Drive %d: Direction = %s", drv, drive->_dirtn ? "outward" : "inward");
        }
    }
}

// Callback invoked when motor spin-up delay completes
static void swim_motor_spinup_callback(void *source, uint64_t data) {
    swim_t *swim = (swim_t *)source;
    int drive_index = (int)data;

    if (drive_index < 0 || drive_index >= NUM_DRIVES) {
        LOG(1, "Drive %d: Invalid drive in spin-up callback", drive_index);
        return;
    }

    swim->drives[drive_index].motor_spinning_up = false;
    LOG(3, "Drive %d: Motor spin-up complete, now ready", drive_index);
}

// ============================================================================
// IWM Mode Register Logic (adapted from floppy.c with mode-switch tracking)
// ============================================================================

// Updates IWM state lines based on address offset (even=clear, odd=set)
static void swim_update_iwm_lines(swim_t *swim, int offset) {
    static const uint8_t line_masks[] = {IWM_LINE_CA0,    IWM_LINE_CA1,    IWM_LINE_CA2, IWM_LINE_LSTRB,
                                         IWM_LINE_ENABLE, IWM_LINE_SELECT, IWM_LINE_Q6,  IWM_LINE_Q7};

    uint8_t mask = line_masks[offset >> 1];
    if (offset & 1)
        swim->iwm_lines |= mask;
    else
        swim->iwm_lines &= ~mask;

    LOG(7, "IWM line: offset=%d", offset);

    if (IWM_LSTRB(swim))
        swim_disk_control(swim);
}

// Resets ISM registers to their initial power-on state
static void swim_ism_reset(swim_t *swim) {
    swim->ism_mode = ISM_MODE_ISM_IWM; // bit 6 set = ISM mode active
    swim->ism_phase = 0xF0; // all phase outputs, all low
    swim->ism_setup = 0x00;
    swim->ism_error = 0x00;
    swim->ism_param_idx = 0;
    swim->ism_iwm_config = 0;
    memset(swim->ism_param, 0, sizeof(swim->ism_param));
    swim->ism_fifo_count = 0;
    memset(swim->ism_fifo, 0, sizeof(swim->ism_fifo));
    memset(swim->ism_fifo_mark, 0, sizeof(swim->ism_fifo_mark));
    swim->ism_crc = CRC_INIT;
    swim->mfm_buf_len = 0;
    swim->mfm_buf_pos = 0;
    swim->mfm_cur_sector = 1;

    // Carry over IWM phase line states to ISM phase register
    swim->ism_phase = 0xF0 | // all outputs enabled
                      (IWM_CA0(swim) ? 0x01 : 0) | (IWM_CA1(swim) ? 0x02 : 0) | (IWM_CA2(swim) ? 0x04 : 0) |
                      (IWM_LSTRB(swim) ? 0x08 : 0);

    LOG(3, "SWIM: ISM registers reset (phase=0x%02X)", swim->ism_phase);
}

// Reads from the IWM register (SWIM in IWM mode)
static uint8_t swim_iwm_read(swim_t *swim, uint32_t offset) {
    GS_ASSERT(offset < 16);
    swim_update_iwm_lines(swim, offset);

    int drv = DRIVE_INDEX(swim);

    // Mode register is WRITE ONLY
    if (IWM_Q6(swim) && IWM_Q7(swim))
        GS_ASSERT(0);

    // Read status register: Q6=1, Q7=0
    if (IWM_Q6(swim) && !IWM_Q7(swim)) {
        uint8_t status = swim->iwm_mode & IWM_STATUS_MODE;
        if (IWM_ENABLE(swim))
            status |= IWM_STATUS_ENABLE;
        if (swim_disk_status(swim, drv))
            status |= IWM_STATUS_SENSE;
        LOG(8, "  status=0x%02X (enable=%d mode=0x%02X)", status, IWM_ENABLE(swim) ? 1 : 0,
            swim->iwm_mode & IWM_STATUS_MODE);
        return status;
    }

    // Read handshake register: Q6=0, Q7=1
    if (!IWM_Q6(swim) && IWM_Q7(swim)) {
        uint8_t hdshk = IWM_HDSHK_RES | IWM_HDSHK_WRITE | IWM_HDSHK_WB_EMTPY;
        LOG(6, "Drive %d: Reading handshake = 0x%02X", drv, hdshk);
        return hdshk;
    }

    // Read data register: Q6=0, Q7=0
    if (!IWM_Q6(swim) && !IWM_Q7(swim)) {
        if (!(swim->iwm_mode & IWM_MODE_ASYNC)) {
            LOG(6, "Drive %d: Sync read mode not implemented = 0x00", drv);
            return 0;
        }

        if (!IWM_ENABLE(swim)) {
            LOG(6, "Drive %d: Reading data (disabled) = 0xFF", drv);
            return 0xFF;
        }

        if (swim->disk[drv] == NULL) {
            LOG(6, "Drive %d: Reading data (no disk) = 0x00", drv);
            return 0x00;
        }

        floppy_drive_t *drive = &swim->drives[drv];
        uint8_t *data = iwm_track_data(drive, swim->disk[drv], swim->sel, swim->scheduler);
        if (!data) {
            LOG(1, "Drive %d: Read failed - no track data", drv);
            return 0xFF;
        }

        size_t trk_len = iwm_track_length(drive->track);

        // IWM latch mode: only bytes with MSB=1 are latched (valid GCR bytes)
        if (swim->iwm_mode & IWM_MODE_LATCH) {
            for (size_t i = 0; i < trk_len; i++) {
                uint8_t byte = data[drive->offset++];
                if (drive->offset >= (int)trk_len)
                    drive->offset = 0;
                if (byte & 0x80) {
                    LOG(8, "Drive %d: Reading data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, swim->sel,
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
        LOG(8, "Drive %d: Reading data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, swim->sel,
            drive->offset, ret);
        return ret;
    }

    GS_ASSERT(0);
    return 0;
}

// Writes a byte to the IWM register (SWIM in IWM mode), with mode-switch detection
static void swim_iwm_write(swim_t *swim, uint32_t offset, uint8_t byte) {
    swim_update_iwm_lines(swim, offset);

    if (!IWM_Q6(swim) || !IWM_Q7(swim))
        return;

    int drv = DRIVE_INDEX(swim);

    if (IWM_ENABLE(swim)) {
        // Write data to disk
        floppy_drive_t *drive = &swim->drives[drv];
        uint8_t *data = iwm_track_data(drive, swim->disk[drv], swim->sel, swim->scheduler);
        if (!data) {
            LOG(1, "Drive %d: Write failed - no track data", drv);
            return;
        }

        data[drive->offset++] = byte;
        LOG(8, "Drive %d: Writing data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, swim->sel,
            drive->offset - 1, byte);

        floppy_track_t *trk = &drive->tracks[swim->sel][drive->track];
        if (!trk->modified) {
            trk->modified = true;
            LOG(5, "Drive %d: Track %d side %d marked dirty", drv, drive->track, swim->sel);
        }

        if (drive->offset == (int)iwm_track_length(drive->track))
            drive->offset = 0;
    } else {
        // Write IWM mode register — track bit 6 for ISM switch sequence
        uint8_t bit6 = (byte >> 6) & 1;

        if (bit6 == ISM_SWITCH_PATTERN[swim->mode_switch_count]) {
            swim->mode_switch_count++;
            LOG(5, "SWIM: Mode switch sequence %d/4 (bit6=%d)", swim->mode_switch_count, bit6);

            if (swim->mode_switch_count == 4) {
                // 4-write sequence complete: switch to ISM mode
                swim->in_ism_mode = true;
                swim->mode_switch_count = 0;
                swim_ism_reset(swim);
                LOG(2, "SWIM: Switched to ISM mode");
                return;
            }
        } else {
            // Mismatch: reset sequence
            swim->mode_switch_count = 0;
            // Check if this byte starts a new sequence
            if (bit6 == ISM_SWITCH_PATTERN[0])
                swim->mode_switch_count = 1;
        }

        swim->iwm_mode = byte;
        LOG(4, "SWIM IWM: Mode register = 0x%02X", byte);
    }
}

// ============================================================================
// ISM Mode Register Logic
// ============================================================================

// ISM drive control via phase register: decode CA lines and LSTRB
static void swim_ism_phase_control(swim_t *swim, uint8_t old_phase) {
    uint8_t new_phase = swim->ism_phase;

    // Check if LSTRB transitioned high (bit 3 in phase value, bit 7 for output enable)
    bool old_lstrb = (old_phase & 0x08) && (old_phase & 0x80);
    bool new_lstrb = (new_phase & 0x08) && (new_phase & 0x80);

    if (new_lstrb && !old_lstrb) {
        // LSTRB went high: execute drive command
        // Set IWM line bits from phase register for disk_status/disk_control compatibility
        swim->iwm_lines = (swim->iwm_lines & ~(IWM_LINE_CA0 | IWM_LINE_CA1 | IWM_LINE_CA2 | IWM_LINE_LSTRB)) |
                          ((new_phase & 0x01) ? IWM_LINE_CA0 : 0) | ((new_phase & 0x02) ? IWM_LINE_CA1 : 0) |
                          ((new_phase & 0x04) ? IWM_LINE_CA2 : 0) | IWM_LINE_LSTRB;
        swim_disk_control(swim);
    }

    // Always sync CA lines to iwm_lines for disk_status reads
    swim->iwm_lines = (swim->iwm_lines & ~(IWM_LINE_CA0 | IWM_LINE_CA1 | IWM_LINE_CA2 | IWM_LINE_LSTRB)) |
                      ((new_phase & 0x01) ? IWM_LINE_CA0 : 0) | ((new_phase & 0x02) ? IWM_LINE_CA1 : 0) |
                      ((new_phase & 0x04) ? IWM_LINE_CA2 : 0) | ((new_phase & 0x08) ? IWM_LINE_LSTRB : 0);
}

// Reads from the ISM register file
static uint8_t swim_ism_read(swim_t *swim, uint32_t offset) {
    GS_ASSERT(offset >= 8 && offset <= 15);

    switch (offset) {
    case 8: { // rData: pop data byte from FIFO
        // If ISM is reading, refill FIFO from MFM buffer first
        if ((swim->ism_mode & ISM_MODE_ACTION) && !(swim->ism_mode & ISM_MODE_WRITE))
            mfm_fill_fifo(swim);

        bool is_mark = false;
        uint8_t byte = ism_fifo_pop(swim, &is_mark);
        if (is_mark && !swim->ism_error)
            swim->ism_error |= ISM_ERR_MARK_IN_DATA; // mark read from data register
        // Update CRC with byte
        swim->ism_crc = crc_ccitt_byte(swim->ism_crc, byte);
        LOG(7, "ISM rData: 0x%02X (mark=%d, fifo=%d)", byte, is_mark, swim->ism_fifo_count);
        return byte;
    }
    case 9: { // rMark: pop mark byte from FIFO (no error)
        if ((swim->ism_mode & ISM_MODE_ACTION) && !(swim->ism_mode & ISM_MODE_WRITE))
            mfm_fill_fifo(swim);

        bool is_mark = false;
        uint8_t byte = ism_fifo_pop(swim, &is_mark);
        swim->ism_crc = crc_ccitt_byte(swim->ism_crc, byte);
        LOG(7, "ISM rMark: 0x%02X (mark=%d, fifo=%d)", byte, is_mark, swim->ism_fifo_count);
        return byte;
    }
    case 10: { // rError: return error register, then clear
        uint8_t err = swim->ism_error;
        swim->ism_error = 0;
        LOG(6, "ISM rError: 0x%02X (cleared)", err);
        return err;
    }
    case 11: { // rParam: read parameter RAM (auto-increment)
        uint8_t val = swim->ism_param[swim->ism_param_idx & 0x0F];
        swim->ism_param_idx = (swim->ism_param_idx + 1) & 0x0F;
        LOG(7, "ISM rParam[%d]: 0x%02X", (swim->ism_param_idx - 1) & 0x0F, val);
        return val;
    }
    case 12: // rPhase
        LOG(7, "ISM rPhase: 0x%02X", swim->ism_phase);
        return swim->ism_phase;

    case 13: // rSetup
        LOG(7, "ISM rSetup: 0x%02X", swim->ism_setup);
        return swim->ism_setup;

    case 14: // rStatus: return current mode register
        LOG(7, "ISM rStatus: 0x%02X", swim->ism_mode);
        return swim->ism_mode;

    case 15: { // rHandshake: derive from current state
        int drv = (swim->ism_mode & ISM_MODE_DRIVE2) ? 1 : (swim->ism_mode & ISM_MODE_DRIVE1) ? 0 : 0;

        uint8_t hdshk = 0;

        // Bit 0: MarkByte — next FIFO byte is a mark
        if (swim->ism_fifo_count > 0 && swim->ism_fifo_mark[0])
            hdshk |= ISM_HDSHK_MARK_BYTE;

        // Bit 1: CRC non-zero
        if (swim->ism_crc != 0)
            hdshk |= ISM_HDSHK_CRC_NZ;

        // Bit 2: RDDATA (always 1 — no physical signal emulation)
        hdshk |= ISM_HDSHK_RDDATA;

        // Bit 3: SENSE (drive status via current phase lines)
        if (swim_disk_status(swim, drv))
            hdshk |= ISM_HDSHK_SENSE;

        // Bit 4: MotorOnState
        if (swim->ism_mode & ISM_MODE_MOTOR_ON)
            hdshk |= ISM_HDSHK_MOTOR_ON;

        // Bit 5: ErrorFlag
        if (swim->ism_error != 0)
            hdshk |= ISM_HDSHK_ERROR;

        // Bits 6-7: FIFO status
        if (swim->ism_mode & ISM_MODE_WRITE) {
            // Write mode: report space available
            int space = ISM_FIFO_SIZE - swim->ism_fifo_count;
            if (space >= 1)
                hdshk |= ISM_HDSHK_DAT1BYTE;
            if (space >= 2)
                hdshk |= ISM_HDSHK_DAT2BYTE;
        } else {
            // Read mode: report bytes available
            // Refill FIFO first if we're reading
            if (swim->ism_mode & ISM_MODE_ACTION)
                mfm_fill_fifo(swim);
            if (swim->ism_fifo_count >= 1)
                hdshk |= ISM_HDSHK_DAT1BYTE;
            if (swim->ism_fifo_count >= 2)
                hdshk |= ISM_HDSHK_DAT2BYTE;
        }

        LOG(7, "ISM rHandshake: 0x%02X (fifo=%d, err=0x%02X)", hdshk, swim->ism_fifo_count, swim->ism_error);
        return hdshk;
    }
    default:
        LOG(2, "ISM: Unknown read register %d", offset);
        return 0;
    }
}

// Writes to the ISM register file
static void swim_ism_write(swim_t *swim, uint32_t offset, uint8_t byte) {
    GS_ASSERT(offset <= 7);

    switch (offset) {
    case 0: // wData: push data byte into FIFO
        if (ism_fifo_push(swim, byte, false)) {
            if (!swim->ism_error)
                swim->ism_error |= ISM_ERR_OVERRUN;
        }
        swim->ism_crc = crc_ccitt_byte(swim->ism_crc, byte);
        LOG(7, "ISM wData: 0x%02X (fifo=%d)", byte, swim->ism_fifo_count);
        break;

    case 1: // wMark: push mark byte into FIFO
        if (ism_fifo_push(swim, byte, true)) {
            if (!swim->ism_error)
                swim->ism_error |= ISM_ERR_OVERRUN;
        }
        swim->ism_crc = crc_ccitt_byte(swim->ism_crc, byte);
        LOG(7, "ISM wMark: 0x%02X (fifo=%d)", byte, swim->ism_fifo_count);
        break;

    case 2: // wCRC (ACTION=1) or wIWMConfig (ACTION=0)
        if (swim->ism_mode & ISM_MODE_ACTION) {
            // Trigger CRC append: push CRC high/low bytes as a FIFO token
            uint8_t crc_hi = (uint8_t)(swim->ism_crc >> 8);
            uint8_t crc_lo = (uint8_t)(swim->ism_crc & 0xFF);
            ism_fifo_push(swim, crc_hi, false);
            ism_fifo_push(swim, crc_lo, false);
            LOG(6, "ISM wCRC: appended CRC 0x%04X", swim->ism_crc);
        } else {
            swim->ism_iwm_config = byte;
            LOG(6, "ISM wIWMConfig: 0x%02X", byte);
        }
        break;

    case 3: // wParam: write to parameter RAM (auto-increment)
        swim->ism_param[swim->ism_param_idx & 0x0F] = byte;
        LOG(7, "ISM wParam[%d]: 0x%02X", swim->ism_param_idx & 0x0F, byte);
        swim->ism_param_idx = (swim->ism_param_idx + 1) & 0x0F;
        break;

    case 4: { // wPhase: set phase register
        uint8_t old_phase = swim->ism_phase;
        swim->ism_phase = byte;
        swim_ism_phase_control(swim, old_phase);
        LOG(7, "ISM wPhase: 0x%02X (was 0x%02X)", byte, old_phase);
        break;
    }
    case 5: // wSetup
        swim->ism_setup = byte;
        LOG(6, "ISM wSetup: 0x%02X", byte);
        break;

    case 6: { // wZeros: clear specified bits in mode register
        uint8_t old_mode = swim->ism_mode;
        swim->ism_mode &= ~byte;
        swim->ism_param_idx = 0; // any access to addr 6 resets param counter
        LOG(6, "ISM wZeros: 0x%02X (mode: 0x%02X -> 0x%02X)", byte, old_mode, swim->ism_mode);

        // If bit 6 was cleared: switch back to IWM mode
        if ((old_mode & ISM_MODE_ISM_IWM) && !(swim->ism_mode & ISM_MODE_ISM_IWM)) {
            swim->in_ism_mode = false;
            swim->mode_switch_count = 0;

            // Carry ISM phase lines back to IWM state
            swim->iwm_lines =
                (swim->iwm_lines & ~(IWM_LINE_CA0 | IWM_LINE_CA1 | IWM_LINE_CA2 | IWM_LINE_LSTRB)) |
                ((swim->ism_phase & 0x01) ? IWM_LINE_CA0 : 0) | ((swim->ism_phase & 0x02) ? IWM_LINE_CA1 : 0) |
                ((swim->ism_phase & 0x04) ? IWM_LINE_CA2 : 0) | ((swim->ism_phase & 0x08) ? IWM_LINE_LSTRB : 0);

            LOG(2, "SWIM: Switched back to IWM mode (iwm_lines=0x%02X)", swim->iwm_lines);
        }

        // Handle Clear FIFO toggle (bit 0: 1->0 transition)
        if ((old_mode & ISM_MODE_CLEAR_FIFO) && !(swim->ism_mode & ISM_MODE_CLEAR_FIFO)) {
            swim->ism_fifo_count = 0;
            memset(swim->ism_fifo, 0, sizeof(swim->ism_fifo));
            memset(swim->ism_fifo_mark, 0, sizeof(swim->ism_fifo_mark));
            swim->ism_crc = CRC_INIT;
            swim->ism_error = 0;
            LOG(6, "ISM: FIFO cleared, CRC reset to 0x%04X", CRC_INIT);
        }

        // If ACTION was just set for read mode, start MFM sector read
        if ((swim->ism_mode & ISM_MODE_ACTION) && !(swim->ism_mode & ISM_MODE_WRITE)) {
            if (!(old_mode & ISM_MODE_ACTION)) {
                swim->mfm_cur_sector = 1;
                mfm_build_sector(swim);
                mfm_fill_fifo(swim);
                LOG(5, "ISM: ACTION set for read, started MFM sector fill");
            }
        }
        break;
    }
    case 7: { // wOnes: set specified bits in mode register
        uint8_t old_mode = swim->ism_mode;
        swim->ism_mode |= byte;
        LOG(6, "ISM wOnes: 0x%02X (mode: 0x%02X -> 0x%02X)", byte, old_mode, swim->ism_mode);

        // Handle Clear FIFO toggle (bit 0: 1->0 transition)
        if ((old_mode & ISM_MODE_CLEAR_FIFO) && !(swim->ism_mode & ISM_MODE_CLEAR_FIFO)) {
            swim->ism_fifo_count = 0;
            memset(swim->ism_fifo, 0, sizeof(swim->ism_fifo));
            memset(swim->ism_fifo_mark, 0, sizeof(swim->ism_fifo_mark));
            swim->ism_crc = CRC_INIT;
            swim->ism_error = 0;
            LOG(6, "ISM: FIFO cleared, CRC reset");
        }

        // If ACTION was just set for read mode, start MFM sector read
        if ((swim->ism_mode & ISM_MODE_ACTION) && !(swim->ism_mode & ISM_MODE_WRITE)) {
            if (!(old_mode & ISM_MODE_ACTION)) {
                swim->mfm_cur_sector = 1;
                mfm_build_sector(swim);
                mfm_fill_fifo(swim);
                LOG(5, "ISM: ACTION set for read, started MFM sector fill");
            }
        }
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
static uint8_t swim_read(swim_t *swim, uint32_t offset) {
    GS_ASSERT(offset < 16);

    if (swim->in_ism_mode) {
        if (offset < 8) {
            // ISM write-only addresses: reads return 0
            LOG(7, "ISM: Read from write-only address %d", offset);
            return 0;
        }
        return swim_ism_read(swim, offset);
    } else {
        return swim_iwm_read(swim, offset);
    }
}

// Writes to the SWIM at the given register offset (0-15)
static void swim_write(swim_t *swim, uint32_t offset, uint8_t byte) {
    GS_ASSERT(offset < 16);

    if (swim->in_ism_mode) {
        if (offset >= 8) {
            // ISM read-only addresses: writes ignored
            LOG(7, "ISM: Write to read-only address %d = 0x%02X", offset, byte);
            return;
        }
        swim_ism_write(swim, offset, byte);
    } else {
        swim_iwm_write(swim, offset, byte);
    }
}

// ============================================================================
// Memory Interface (SE/30 address decoding)
// ============================================================================

// Memory interface handler for 8-bit reads
static uint8_t swim_read_uint8(void *ctx, uint32_t addr) {
    swim_t *s = (swim_t *)ctx;
    // SE/30: same 512-byte stride as Mac Plus (addr >> 9) & 0xF
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
    swim_t *s = (swim_t *)ctx;
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
// Public API
// ============================================================================

// Sets the VIA-driven SEL signal for head selection
void swim_set_sel_signal(swim_t *swim, bool sel) {
    if (!swim) {
        LOG(4, "SEL signal -> %s (deferred, no swim)", sel ? "high" : "low");
        return;
    }
    LOG(6, "SEL signal: %s -> %s", swim->sel ? "high" : "low", sel ? "high" : "low");
    swim->sel = sel;
}

// Inserts a disk image into the specified drive
int swim_insert(swim_t *swim, int drive, image_t *disk) {
    GS_ASSERT(drive < NUM_DRIVES);

    if (swim->disk[drive] != NULL) {
        LOG(2, "Drive %d: Insert failed - disk already present", drive);
        return -1;
    }

    swim->disk[drive] = disk;

    swim->drives[DRIVE_INDEX(swim)].offset = 0;
    const char *name = disk ? image_get_filename(disk) : NULL;
    LOG(1, "Drive %d: Inserted disk '%s' (writable=%d)", drive, name ? name : "<unnamed>", disk ? disk->writable : 0);

    return 0;
}

// Returns whether a disk is currently inserted in the specified drive
bool swim_is_inserted(swim_t *swim, int drive) {
    GS_ASSERT(drive < NUM_DRIVES);
    return swim->disk[drive] != NULL;
}

// ============================================================================
// Lifecycle (Init / Delete / Checkpoint)
// ============================================================================

// Initializes the SWIM controller and maps it to memory
swim_t *swim_init(memory_map_t *map, struct scheduler *scheduler, checkpoint_t *checkpoint) {
    swim_t *swim = malloc(sizeof(swim_t));
    if (!swim) {
        LOG(1, "SWIM: Allocation failed");
        return NULL;
    }

    memset(swim, 0, sizeof(swim_t));
    LOG(2, "SWIM: Controller created");

    swim->scheduler = scheduler;
    scheduler_new_event_type(scheduler, "swim", swim, "motor_spinup", &swim_motor_spinup_callback);

    // ISM initial state (chip powers up in IWM mode)
    swim->in_ism_mode = false;
    swim->ism_phase = 0xF0;
    swim->ism_crc = CRC_INIT;
    swim->mfm_cur_sector = 1;

    // Set up memory interface
    swim->memory_interface.read_uint8 = &swim_read_uint8;
    swim->memory_interface.read_uint16 = &swim_read_uint16;
    swim->memory_interface.read_uint32 = &swim_read_uint32;
    swim->memory_interface.write_uint8 = &swim_write_uint8;
    swim->memory_interface.write_uint16 = &swim_write_uint16;
    swim->memory_interface.write_uint32 = &swim_write_uint32;

    // Register with memory map if provided (NULL = machine handles registration)
    if (map)
        memory_map_add(map, SWIM_BASE_ADDR, SWIM_MAP_SIZE, "swim", &swim->memory_interface, swim);

    if (checkpoint) {
        LOG(3, "SWIM: Restoring from checkpoint");

        // Clear track data pointers before restoring
        for (int d = 0; d < NUM_DRIVES; d++)
            for (int s = 0; s < NUM_SIDES; s++)
                for (int t = 0; t < NUM_TRACKS; t++)
                    swim->drives[d].tracks[s][t].data = NULL;

        // Read plain-data portion
        system_read_checkpoint_data(checkpoint, swim, SWIM_CHECKPOINT_SIZE);

        // Restore disk images by filename
        for (int i = 0; i < NUM_DRIVES; i++) {
            swim->disk[i] = NULL;
            uint32_t len = 0;
            system_read_checkpoint_data(checkpoint, &len, sizeof(len));
            if (len > 0) {
                char *name = malloc(len);
                if (name) {
                    system_read_checkpoint_data(checkpoint, name, len);
                    swim->disk[i] = setup_get_image_by_filename(name);
                    free(name);
                } else {
                    for (uint32_t k = 0; k < len; k++) {
                        char tmp;
                        system_read_checkpoint_data(checkpoint, &tmp, 1);
                    }
                }
            }
        }

        // Restore GCR track buffers
        for (int d = 0; d < NUM_DRIVES; d++) {
            for (int s = 0; s < NUM_SIDES; s++) {
                for (int t = 0; t < NUM_TRACKS; t++) {
                    floppy_track_t *trk = &swim->drives[d].tracks[s][t];
                    uint8_t has_data = 0;
                    system_read_checkpoint_data(checkpoint, &has_data, 1);
                    if (has_data && trk->size > 0) {
                        trk->data = malloc(trk->size);
                        if (trk->data)
                            system_read_checkpoint_data(checkpoint, trk->data, trk->size);
                        else {
                            for (size_t k = 0; k < trk->size; k++) {
                                uint8_t tmp;
                                system_read_checkpoint_data(checkpoint, &tmp, 1);
                            }
                            trk->size = 0;
                            trk->modified = false;
                        }
                    }
                }
            }
        }
    }

    return swim;
}

// Return the SWIM memory-mapped I/O interface for machine-level address decode
const memory_interface_t *swim_get_memory_interface(swim_t *swim) {
    return &swim->memory_interface;
}

// Frees all resources associated with the SWIM controller
void swim_delete(swim_t *swim) {
    if (!swim)
        return;
    LOG(2, "SWIM: Deleting controller");

    // Flush and free track data for both drives
    for (int d = 0; d < NUM_DRIVES; d++) {
        iwm_flush_modified_tracks(&swim->drives[d], swim->disk[d], d);
        for (int s = 0; s < NUM_SIDES; s++) {
            for (int t = 0; t < NUM_TRACKS; t++) {
                free(swim->drives[d].tracks[s][t].data);
            }
        }
    }
    free(swim);
}

// Saves the SWIM controller state to a checkpoint
void swim_checkpoint(swim_t *restrict swim, checkpoint_t *checkpoint) {
    if (!swim || !checkpoint)
        return;
    LOG(13, "SWIM: Checkpointing controller");

    // Write plain-data portion
    system_write_checkpoint_data(checkpoint, swim, SWIM_CHECKPOINT_SIZE);

    // Write disk filenames for each drive
    for (int i = 0; i < NUM_DRIVES; i++) {
        const char *name = swim->disk[i] ? image_get_filename(swim->disk[i]) : NULL;
        uint32_t len = (name && *name) ? (uint32_t)strlen(name) + 1 : 0;
        system_write_checkpoint_data(checkpoint, &len, sizeof(len));
        if (len)
            system_write_checkpoint_data(checkpoint, name, len);
    }

    // Save GCR track buffers
    for (int d = 0; d < NUM_DRIVES; d++) {
        for (int s = 0; s < NUM_SIDES; s++) {
            for (int t = 0; t < NUM_TRACKS; t++) {
                floppy_track_t *trk = &swim->drives[d].tracks[s][t];
                uint8_t has_data = (trk->data != NULL);
                system_write_checkpoint_data(checkpoint, &has_data, 1);
                if (has_data)
                    system_write_checkpoint_data(checkpoint, trk->data, trk->size);
            }
        }
    }
}
