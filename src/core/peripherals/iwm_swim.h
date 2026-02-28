// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iwm_swim.h
// Internal shared types, constants, and function declarations for the IWM
// (floppy.c) and SWIM (swim.c) floppy disk controllers.
// This header is NOT part of the public API; include it only from floppy.c
// and swim.c.

#ifndef IWM_SWIM_H
#define IWM_SWIM_H

#include "image.h"
#include "scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Constants
// ============================================================================

// Drive and track geometry
#define NUM_DRIVES 2
#define NUM_TRACKS 80
#define NUM_SIDES  2

// Time in nanoseconds that the drive reports /READY=1 (not ready) after motor on
#define MOTOR_SPINUP_TIME_NS (400ULL * 1000000ULL) // 400 ms

// IWM state control line bits - packed into single byte for efficient checkpointing
#define IWM_LINE_CA0    0x01
#define IWM_LINE_CA1    0x02
#define IWM_LINE_CA2    0x04
#define IWM_LINE_LSTRB  0x08
#define IWM_LINE_ENABLE 0x10
#define IWM_LINE_SELECT 0x20
#define IWM_LINE_Q6     0x40
#define IWM_LINE_Q7     0x80

// Helper macros to access IWM lines by name (works with any struct having iwm_lines field)
#define IWM_CA0(f)    (((f)->iwm_lines & IWM_LINE_CA0) != 0)
#define IWM_CA1(f)    (((f)->iwm_lines & IWM_LINE_CA1) != 0)
#define IWM_CA2(f)    (((f)->iwm_lines & IWM_LINE_CA2) != 0)
#define IWM_LSTRB(f)  (((f)->iwm_lines & IWM_LINE_LSTRB) != 0)
#define IWM_ENABLE(f) (((f)->iwm_lines & IWM_LINE_ENABLE) != 0)
#define IWM_SELECT(f) (((f)->iwm_lines & IWM_LINE_SELECT) != 0)
#define IWM_Q6(f)     (((f)->iwm_lines & IWM_LINE_Q6) != 0)
#define IWM_Q7(f)     (((f)->iwm_lines & IWM_LINE_Q7) != 0)

// Helper to get current drive index (0 or 1)
#define DRIVE_INDEX(f) (IWM_SELECT(f) ? 1 : 0)

// IWM mode register bits
#define IWM_MODE_LATCH   0x01 // 1 = latch mode enabled
#define IWM_MODE_ASYNC   0x02 // 1 = asynchronous handshake
#define IWM_MODE_NOTIMER 0x04 // 1 = disable 1-second motor-off delay
#define IWM_MODE_FAST    0x08 // 1 = fast mode (2us/bit for 3.5")
#define IWM_MODE_8MHZ    0x10 // 1 = 8 MHz clock (vs 7 MHz)
#define IWM_MODE_TEST    0x20 // 1 = test mode

// IWM status register bits
#define IWM_STATUS_MODE   0x1F
#define IWM_STATUS_ENABLE 0x20
#define IWM_STATUS_SENSE  0x80

// IWM handshake register bits
#define IWM_HDSHK_RES      0x3F
#define IWM_HDSHK_WRITE    0x40
#define IWM_HDSHK_WB_EMTPY 0x80

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
    int track; // current head position (0-79)
    int offset; // byte offset within current track
    floppy_track_t tracks[NUM_SIDES][NUM_TRACKS]; // GCR encoded track data
} floppy_drive_t;

// ============================================================================
// Shared Utility Functions (parameterized, no floppy_t/swim_t dependency)
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

// ============================================================================
// GCR Encoding/Decoding (defined in floppy.c, already non-static)
// ============================================================================

// GCR codeword table (6-bit to 8-bit)
extern const uint8_t gcr_codewords[];

// Converts a GCR codeword to its 6-bit value
uint8_t decode_gcr(uint8_t gcr_codeword);

// Encodes a sector to GCR format with header and data fields
uint8_t *encode_sector(uint8_t *dst, const uint8_t *tag, const uint8_t *data, int track, int sector, int side);

// Encodes an entire track with interleaved sectors to GCR format
void encode_track(uint8_t *dst, size_t track_length, int track, int side, const uint8_t *data);

#endif // IWM_SWIM_H
