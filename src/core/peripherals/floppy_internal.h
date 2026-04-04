// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy_internal.h
// Internal shared types, constants, and struct definition for the unified
// floppy subsystem (IWM + SWIM). This header is NOT part of the public API;
// include it only from floppy*.c files.

#ifndef FLOPPY_INTERNAL_H
#define FLOPPY_INTERNAL_H

#include "image.h"
#include "memory.h"
#include "scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Controller Type Constants
// ============================================================================

// IWM-only controller (Mac Plus)
#define FLOPPY_TYPE_IWM 0
// SWIM dual-mode controller (SE/30)
#define FLOPPY_TYPE_SWIM 1

// ============================================================================
// Drive and Track Geometry
// ============================================================================

#define NUM_DRIVES 2
#define NUM_TRACKS 80
#define NUM_SIDES  2

// Time in nanoseconds for motor spin-up (400 ms)
#define MOTOR_SPINUP_TIME_NS (400ULL * 1000000ULL)

// Step motor settle time in nanoseconds.  After each step command, /STEP
// reads as 0 (active) until a scheduler event fires after this period.
// On real hardware the settle is ~12 ms, but the emulator's averaged CPI
// model means the ROM's fixed-iteration poll timeout (~2800 instructions
// on Plus, ~3600 on SE/30) expires before 12 ms of emulated time at low
// CPI values.  10 µs is long enough for MacTest's post-step /STEP check
// to see 0 (step in progress) while settling well within the ROM's
// timeout at any CPI.
#define STEP_SETTLE_TIME_NS (10ULL * 1000ULL)

// Motor speed settle time after stepping across a speed-zone boundary.
// /READY deasserts until the motor reaches the new target RPM.
#define SPEED_SETTLE_TIME_NS (5ULL * 1000000ULL)

// ============================================================================
// IWM State Control Line Bits
// ============================================================================

#define IWM_LINE_CA0    0x01
#define IWM_LINE_CA1    0x02
#define IWM_LINE_CA2    0x04
#define IWM_LINE_LSTRB  0x08
#define IWM_LINE_ENABLE 0x10
#define IWM_LINE_SELECT 0x20
#define IWM_LINE_Q6     0x40
#define IWM_LINE_Q7     0x80

// Helper macros to access IWM lines by name
#define IWM_CA0(f)    (((f)->iwm_lines & IWM_LINE_CA0) != 0)
#define IWM_CA1(f)    (((f)->iwm_lines & IWM_LINE_CA1) != 0)
#define IWM_CA2(f)    (((f)->iwm_lines & IWM_LINE_CA2) != 0)
#define IWM_LSTRB(f)  (((f)->iwm_lines & IWM_LINE_LSTRB) != 0)
#define IWM_ENABLE(f) (((f)->iwm_lines & IWM_LINE_ENABLE) != 0)
#define IWM_SELECT(f) (((f)->iwm_lines & IWM_LINE_SELECT) != 0)
#define IWM_Q6(f)     (((f)->iwm_lines & IWM_LINE_Q6) != 0)
#define IWM_Q7(f)     (((f)->iwm_lines & IWM_LINE_Q7) != 0)

// Current drive index (0 or 1)
#define DRIVE_INDEX(f) (IWM_SELECT(f) ? 1 : 0)

// ============================================================================
// IWM Mode Register Bits
// ============================================================================

#define IWM_MODE_LATCH   0x01
#define IWM_MODE_ASYNC   0x02
#define IWM_MODE_NOTIMER 0x04
#define IWM_MODE_FAST    0x08
#define IWM_MODE_8MHZ    0x10
#define IWM_MODE_TEST    0x20

// ============================================================================
// IWM Status/Handshake Register Bits
// ============================================================================

#define IWM_STATUS_MODE   0x1F
#define IWM_STATUS_ENABLE 0x20
#define IWM_STATUS_SENSE  0x80

#define IWM_HDSHK_RES      0x3F
#define IWM_HDSHK_WRITE    0x40
#define IWM_HDSHK_WB_EMTPY 0x80

// ============================================================================
// ISM (SWIM-specific) Constants
// ============================================================================

// ISM mode register bits
#define ISM_MODE_CLEAR_FIFO 0x01
#define ISM_MODE_DRIVE1     0x02
#define ISM_MODE_DRIVE2     0x04
#define ISM_MODE_ACTION     0x08
#define ISM_MODE_WRITE      0x10
#define ISM_MODE_HDSEL      0x20
#define ISM_MODE_ISM_IWM    0x40
#define ISM_MODE_MOTOR_ON   0x80

// ISM setup register bits
#define ISM_SETUP_HDSEL_EN   0x01
#define ISM_SETUP_SEL35      0x02
#define ISM_SETUP_GCR        0x04
#define ISM_SETUP_FCLK_DIV2  0x08
#define ISM_SETUP_ENABLE_ECM 0x10
#define ISM_SETUP_IBM_DRIVE  0x20
#define ISM_SETUP_TRANS_DIS  0x40
#define ISM_SETUP_MOTOR_TMO  0x80

// ISM error register bits
#define ISM_ERR_UNDERRUN     0x01
#define ISM_ERR_MARK_IN_DATA 0x02
#define ISM_ERR_OVERRUN      0x04

// ISM handshake register bits
#define ISM_HDSHK_MARK_BYTE 0x01
#define ISM_HDSHK_CRC_NZ    0x02
#define ISM_HDSHK_RDDATA    0x04
#define ISM_HDSHK_SENSE     0x08
#define ISM_HDSHK_MOTOR_ON  0x10
#define ISM_HDSHK_ERROR     0x20
#define ISM_HDSHK_DAT2BYTE  0x40
#define ISM_HDSHK_DAT1BYTE  0x80

// ISM FIFO capacity
#define ISM_FIFO_SIZE 2

// CRC-CCITT initial value
#define CRC_INIT 0xFFFF

// IWM->ISM mode switch: expected bit-6 pattern in 4 consecutive mode writes
extern const uint8_t ISM_SWITCH_PATTERN[4];

// MFM sector buffer size
#define MFM_SECTOR_BUF_SIZE 768

// Offset of the first data byte in the MFM sector buffer
#define MFM_DATA_START_OFFSET 60

// ============================================================================
// Shared Types
// ============================================================================

// Represents a single disk track with GCR-encoded data
typedef struct floppy_track {
    size_t size; // encoded track size
    bool modified; // true if track has been written to
    uint8_t *data; // pointer to encoded GCR data (excluded from checkpoint)
} floppy_track_t;

// Represents a physical floppy drive with head position and motor state
typedef struct floppy_drive {
    bool _dirtn; // step direction: 0=inward (higher tracks), 1=outward
    bool _motoron; // motor signal (active low: true=off, false=on)
    bool motor_spinning_up; // true during motor spin-up period
    bool speed_settling; // true while motor adjusts RPM after zone change
    int step_settle_count; // >0 while step is settling; cleared by scheduler event
    int track; // current head position (0-79)
    int offset; // byte offset within current track
    int data_side; // latched head side for data I/O
    floppy_track_t tracks[NUM_SIDES][NUM_TRACKS]; // GCR encoded track data
} floppy_drive_t;

// ============================================================================
// Unified Floppy Controller Struct
// ============================================================================

// Unified floppy disk controller state (IWM or SWIM)
typedef struct floppy floppy_t;
struct floppy {
    // === Plain data (checkpointed via single memcpy) ===

    // Controller type (FLOPPY_TYPE_IWM or FLOPPY_TYPE_SWIM)
    int type;

    // IWM controller state (shared drive interface)
    uint8_t iwm_lines; // packed IWM state lines (CA0-Q7)
    uint8_t mode; // IWM mode register
    bool sel; // VIA-driven SEL signal (head select)
    floppy_drive_t drives[NUM_DRIVES]; // drive state machines

    // SWIM-only fields (unused when type == FLOPPY_TYPE_IWM)
    int cstin_delay[NUM_DRIVES]; // disk insertion detection delay

    // SWIM mode tracking
    bool in_ism_mode; // true = ISM mode active
    uint8_t mode_switch_count; // 0-4: position in IWM->ISM 4-write sequence
    uint8_t iwm_write_latch; // SWIM echoes last data bus byte on read
    bool iwm_latch_valid; // true after write, cleared after read

    // ISM registers
    uint8_t ism_mode; // mode/status register
    uint8_t ism_setup; // setup register
    uint8_t ism_error; // error register (read-clears)
    uint8_t ism_phase; // phase register (reset: 0xF0)
    uint8_t ism_param[16]; // parameter RAM (16 bytes)
    uint8_t ism_param_idx; // parameter RAM pointer
    uint8_t ism_iwm_config; // IWM configuration register

    // ISM FIFO
    uint8_t ism_fifo[ISM_FIFO_SIZE]; // 2-byte FIFO
    uint8_t ism_fifo_count; // bytes currently in FIFO (0-2)
    bool ism_fifo_mark[ISM_FIFO_SIZE]; // mark flag per FIFO byte

    // ISM CRC state
    uint16_t ism_crc; // running CRC-CCITT-16

    // MFM sector-level emulation state
    uint8_t mfm_sector_buf[MFM_SECTOR_BUF_SIZE]; // pre-built sector data
    uint16_t mfm_buf_pos; // current read position
    uint16_t mfm_buf_len; // valid bytes in sector buffer
    uint8_t mfm_cur_sector; // current sector number (1-based)
    uint8_t mfm_cur_track; // current track for MFM read
    uint8_t mfm_cur_side; // current side for MFM read
    bool mfm_sector_mark[MFM_SECTOR_BUF_SIZE]; // per-byte mark flags

    // ISM write capture state (persists sector writes to disk image)
    uint8_t ism_write_buf[512]; // captured sector data payload
    uint16_t ism_write_pos; // bytes captured so far (0-512)
    uint8_t ism_write_state; // 0=scanning for data mark, 1=capturing data
    uint8_t ism_write_a1_count; // consecutive $A1 mark bytes seen

    // === Pointers (not checkpointed) ===
    image_t *disk[NUM_DRIVES]; // disk image pointers
    struct scheduler *scheduler; // scheduler for timed events
    memory_interface_t memory_interface; // memory interface
};

// Size of plain-data portion for checkpointing (excludes pointers at end)
#define FLOPPY_CHECKPOINT_SIZE offsetof(floppy_t, disk)

// ============================================================================
// Shared Utility Functions (parameterized, no struct dependency)
// ============================================================================

// Returns the approximate GCR-encoded length for a track
size_t iwm_track_length(int track);

// Returns the rotational speed (RPM) for a given track
int iwm_track_rpm(int track);

// Returns the number of sectors for a given track (varies by zone)
int iwm_sectors_per_track(int track);

// Calculates the byte offset into a disk image for a given track and side
size_t iwm_disk_image_offset(int track, int side, int num_sides);

// Determines the number of sides based on disk image type
int iwm_image_num_sides(image_t *img);

// Calculates TACH signal state (60 pulses per revolution)
int iwm_tach_signal(struct scheduler *scheduler, floppy_drive_t *drive, const char **reason);

// Returns a pointer to GCR data for the given drive/image/side, encoding on demand
uint8_t *iwm_track_data(floppy_drive_t *drive, image_t *img, int sel, struct scheduler *scheduler);

// Writes any modified GCR tracks back to the underlying disk image
void iwm_flush_modified_tracks(floppy_drive_t *drive, image_t *img, int drive_index);

// GCR codeword table (6-bit to 8-bit)
extern const uint8_t gcr_codewords[];

// ============================================================================
// Shared IWM Core Functions (defined in floppy.c, used by floppy_swim.c)
// ============================================================================

// Returns the current disk status based on IWM CA lines and SEL signal
int floppy_disk_status(floppy_t *floppy, int drv);

// Processes disk control commands when LSTRB is high
void floppy_disk_control(floppy_t *floppy);

// Motor spin-up callback (needed for scheduler event registration)
void floppy_motor_spinup_callback(void *source, uint64_t data);

// Updates IWM state lines from register offset (even=clear, odd=set)
void floppy_update_iwm_lines(floppy_t *floppy, int offset);

// Reads from the IWM register at the specified offset
uint8_t floppy_iwm_read(floppy_t *floppy, uint32_t offset);

// Writes a byte to the IWM register at the specified offset
void floppy_iwm_write(floppy_t *floppy, uint32_t offset, uint8_t byte);

// ============================================================================
// IWM-Specific Functions (defined in floppy_iwm.c)
// ============================================================================

// Sets up IWM memory interface callbacks and registers with memory map
void floppy_iwm_setup(floppy_t *floppy, memory_map_t *map);

// ============================================================================
// SWIM-Specific Functions (defined in floppy_swim.c)
// ============================================================================

// Sets up SWIM memory interface callbacks and ISM initial state
void floppy_swim_setup(floppy_t *floppy, memory_map_t *map);

// SWIM motor spin-up callback (separate for scheduler event identity)
void floppy_swim_motor_spinup_callback(void *source, uint64_t data);

#endif // FLOPPY_INTERNAL_H
