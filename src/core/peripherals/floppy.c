// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy.c
// Unified floppy disk controller: lifecycle, shared IWM core logic (disk status,
// disk control, IWM read/write), and public API. Supports both IWM (Mac Plus)
// and SWIM (SE/30) controller types.

#include "floppy.h"
#include "floppy_internal.h"
#include "log.h"
#include "memory.h"
#include "object.h"
#include "platform.h"
#include "shell.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

// Forward declarations — class descriptors are at the bottom of the file but
// floppy_init / floppy_delete reference them.
extern const class_desc_t floppy_class;
extern const class_desc_t floppy_drive_class;
extern const class_desc_t floppy_drives_collection_class;

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

LOG_USE_CATEGORY_NAME("floppy");

// ============================================================================
// Shared IWM Core Functions (used by both IWM and SWIM code paths)
// ============================================================================

// Forward declarations for scheduler callbacks
static void floppy_step_settle_callback(void *src, uint64_t data);
static void floppy_speed_settle_callback(void *src, uint64_t data);

// Returns pointer to the currently selected drive based on IWM SELECT line
static floppy_drive_t *current_drive(floppy_t *floppy) {
    return &floppy->drives[IWM_SELECT(floppy) ? 1 : 0];
}

// Returns the current disk status based on IWM CA lines and SEL signal.
// The VIA-based SEL signal is part of the sense-line address, not drive
// selection.  The caller may pass a SEL-derived drive index, but the status
// should reflect the active drive (the one with a disk and motor on).
int floppy_disk_status(floppy_t *floppy, int drv) {
    floppy_drive_t *drive = &floppy->drives[drv];

    // Build status key from CA lines and VIA SEL signal
    int key = (IWM_CA0(floppy) ? 0x01 : 0) | (IWM_CA1(floppy) ? 0x02 : 0) | (IWM_CA2(floppy) ? 0x04 : 0) |
              (floppy->sel ? 0x08 : 0);

    int ret = 0;
    const char *desc = "unknown";

    switch (key) {
    case 0x00: // /DIRTN: active-low — returns 0 when stepping inward
        desc = "/DIRTN";
        ret = drive->_dirtn; // _dirtn: false=inward → 0, true=outward → 1
        break;
    case 0x01: // /STEP: zero during step settle period
        desc = "/STEP";
        // Event-based: step_settle_count is cleared by scheduler callback
        ret = (drive->step_settle_count > 0) ? 0 : 1;
        break;
    case 0x02: // /MOTORON: active-low — returns 0 when motor is ON
        desc = "/MOTORON";
        ret = drive->_motoron; // _motoron: false=on → 0, true=off → 1
        break;
    case 0x03: // EJECT
        desc = "EJECT";
        ret = 0;
        break;
    case 0x04: // RDDATA: data from side 0
    {
        // Return the current data bit from the read head.  On real hardware,
        // the SENSE line reflects raw GCR/MFM flux data as the disk spins
        // continuously.  Derive the byte position from scheduler time to
        // simulate disk rotation (the head sees different data over time).
        uint8_t *data = iwm_track_data(drive, floppy->disk[drv], 0, floppy->scheduler);
        if (data) {
            size_t trk_len = iwm_track_length(drive->track);
            double now_ns = scheduler_time_ns(floppy->scheduler);
            double ns_per_byte = 16340.0; // ~16.3µs per GCR byte at 489.6 kbit/s
            int byte_pos = (int)(now_ns / ns_per_byte) % (int)trk_len;
            int bit_idx = (int)(now_ns / 2040.0) & 7; // ~2µs per flux cell
            ret = (data[byte_pos] >> bit_idx) & 1;
        } else {
            // HD (MFM) disk: no GCR track data, simulate time-varying flux
            double now_ns = scheduler_time_ns(floppy->scheduler);
            int bit_idx = (int)(now_ns / 2040.0) & 7;
            ret = (bit_idx < 4) ? 1 : 0;
        }
        desc = "RDDATA side0";
        break;
    }
    case 0x05: // IWM: reserved; SWIM: mfmDrv (SuperDrive present)
        if (floppy->type == FLOPPY_TYPE_SWIM) {
            desc = "mfmDrv";
            ret = 1; // SWIM always has a SuperDrive (FDHD capable)
        } else {
            desc = "reserved (CA2+CA0)";
            ret = 0;
        }
        break;
    case 0x06: // /SIDES (IWM) or SIDES (SWIM)
        if (floppy->type == FLOPPY_TYPE_SWIM) {
            desc = "SIDES";
            ret = 1; // SuperDrive is always double-sided
        } else {
            desc = "/SIDES";
            ret = 1; // drive supports double-sided
        }
        break;
    case 0x07: // /DRVIN
        desc = "/DRVIN";
        ret = 0; // drive connected
        break;
    case 0x08: // /CSTIN: zero when disk in drive
        desc = "/CSTIN";
        if (floppy->type == FLOPPY_TYPE_SWIM && floppy->cstin_delay[drv] > 0) {
            floppy->cstin_delay[drv]--;
            ret = 1; // report no disk during insertion delay
        } else {
            ret = (floppy->disk[drv] == NULL);
        }
        break;
    case 0x09: // /WRTPRT: zero when write protected
        desc = "/WRTPRT";
        ret = (floppy->disk[drv] != NULL) ? floppy->disk[drv]->writable : 0;
        break;
    case 0x0A: // /TKO: zero when head on track 0
        desc = "/TKO";
        ret = (drive->track != 0);
        break;
    case 0x0B: // /TACH (GCR: 60 pulses/rev) or INDEX (ISM mode)
    {
        // In ISM mode, register 0111 switches from the high-frequency FG
        // tach signal (60 pulses/rev) to a low-frequency INDEX signal.
        //
        // INDEX pulse count depends on disk type:
        //   800K (GCR):  2 pulses/rev → 100ms cycle → MEASURE_SPEED ≈ 78,300 ticks
        //   1.4MB (HD):  1 pulse/rev  → 200ms cycle → HD_MEASURE_SPEED ≈ 157,000 ticks
        //
        // The INDEX pulse is a short HIGH spike (~2ms) from the inductive
        // sensor detecting the hub mark, followed by a long LOW phase.
        // The asymmetric duty cycle is critical: MacTest's MEASURE_SPEED
        // function uses VIA T2 overflow counting during the LOW phase.
        bool ism_mode =
            (floppy->type == FLOPPY_TYPE_SWIM && floppy->in_ism_mode && (floppy->ism_mode & ISM_MODE_MOTOR_ON));
        if (ism_mode) {
            double now_ns = scheduler_time_ns(floppy->scheduler);
            double ns_per_rev = (60.0 / 300) * 1e9; // 200ms at 300 RPM
            // HD disks: 1 INDEX pulse per revolution (200ms cycle)
            // 800K disks: 2 INDEX pulses per revolution (100ms cycle)
            image_t *img = floppy->disk[drv];
            bool is_hd = (img && img->type == image_fd_hd);
            double ns_per_cycle = is_hd ? ns_per_rev : (ns_per_rev / 2.0);
            double index_high_ns = 2.0 * 1e6; // 2ms HIGH pulse
            double pos_in_rev = fmod(now_ns, ns_per_rev);
            double pos_in_cycle = fmod(pos_in_rev, ns_per_cycle);
            ret = (pos_in_cycle < index_high_ns) ? 1 : 0;
            desc = is_hd ? "INDEX(HD)" : "INDEX(800K)";
        } else {
            const char *tach_reason = NULL;
            ret = iwm_tach_signal(floppy->scheduler, drive, &tach_reason);
            desc = "/TACH";
        }
        LOG(6, "Drive %d: Reading %s = %d (ism=%d ism_mode=0x%02X in_ism=%d track=%d)", drv, desc, ret, ism_mode,
            floppy->ism_mode, floppy->in_ism_mode, drive->track);
        return ret;
    }
    case 0x0C: // RDDATA: data from side 1
    {
        uint8_t *data = iwm_track_data(drive, floppy->disk[drv], 1, floppy->scheduler);
        if (data) {
            size_t trk_len = iwm_track_length(drive->track);
            double now_ns = scheduler_time_ns(floppy->scheduler);
            double ns_per_byte = 16340.0;
            int byte_pos = (int)(now_ns / ns_per_byte) % (int)trk_len;
            int bit_idx = (int)(now_ns / 2040.0) & 7;
            ret = (data[byte_pos] >> bit_idx) & 1;
        } else {
            // HD (MFM) disk: simulate time-varying flux
            double now_ns = scheduler_time_ns(floppy->scheduler);
            int bit_idx = (int)(now_ns / 2040.0) & 7;
            ret = (bit_idx < 4) ? 1 : 0;
        }
        desc = "RDDATA side1";
        break;
    }
    case 0x0D: // /DRVEXIST: 1 when physical drive present (ISM mode only)
        desc = "/DRVEXIST";
        // MacTest's CHECK_DRIVE_STATUS reads this twice: once with the SWIM
        // in ISM mode (expects 1 = drive exists) and once in IWM compat mode
        // (expects 0).  Return 1 only in ISM mode.
        ret = (floppy->type == FLOPPY_TYPE_SWIM && floppy->in_ism_mode) ? 1 : 0;
        break;
    case 0x0E: // /READY: zero when ready
        desc = "/READY";
        ret = (drive->motor_spinning_up || drive->speed_settling) ? 1 : 0;
        break;
    case 0x0F: // NEWINTF
        desc = "NEWINTF";
        if (floppy->type == FLOPPY_TYPE_SWIM) {
            // Report new interface (0) only for HD disks
            ret = (floppy->disk[drv] && floppy->disk[drv]->type == image_fd_hd) ? 0 : 1;
        } else {
            ret = 1; // 800K drive
        }
        break;
    default:
        ret = 0;
        break;
    }

    LOG(6, "Drive %d: Reading %s = %d", drv, desc, ret);
    LOG(8, "  detail: key=0x%02X ca0=%d ca1=%d ca2=%d sel=%d dirtn=%d motoron=%d track=%d", key, IWM_CA0(floppy),
        IWM_CA1(floppy), IWM_CA2(floppy), floppy->sel, drive->_dirtn ? 1 : 0, drive->_motoron ? 1 : 0, drive->track);

    return ret;
}

// Processes disk control commands when LSTRB is high
void floppy_disk_control(floppy_t *floppy) {
    floppy_drive_t *drive = current_drive(floppy);
    int drv = DRIVE_INDEX(floppy);

    // Determine the correct motor callback based on controller type
    event_callback_t spinup_cb =
        (floppy->type == FLOPPY_TYPE_SWIM) ? floppy_swim_motor_spinup_callback : floppy_motor_spinup_callback;

    // Commands only when SEL is low
    if (floppy->sel)
        return;

    if (IWM_CA0(floppy)) {
        if (IWM_CA1(floppy)) {
            // EJECT (CA0=1, CA1=1, CA2=1)
            if (IWM_CA2(floppy)) {
                LOG(1, "Drive %d: Eject requested", drv);
                iwm_flush_modified_tracks(drive, floppy->disk[drv], drv);
                memset(drive->tracks, 0, sizeof(drive->tracks));
                floppy->disk[drv] = NULL;
                LOG(1, "Drive %d: Ejected", drv);
            }
        } else {
            // STEP (CA0=1, CA1=0, CA2=0)
            if (!IWM_CA2(floppy)) {
                int old_rpm = iwm_track_rpm(drive->track);
                // _dirtn: 0=inward (higher tracks), 1=outward (lower tracks)
                if (drive->_dirtn) {
                    if (drive->track > 0)
                        drive->track--;
                } else {
                    if (drive->track < NUM_TRACKS - 1)
                        drive->track++;
                }
                drive->offset = 0;
                drive->step_settle_count = 1;
                remove_event_by_data(floppy->scheduler, floppy_step_settle_callback, floppy, (uint64_t)drv);
                scheduler_new_cpu_event(floppy->scheduler, floppy_step_settle_callback, floppy, (uint64_t)drv, 0,
                                        STEP_SETTLE_TIME_NS);
                // Zone-crossing step: motor must change RPM, /READY deasserts
                if (old_rpm != iwm_track_rpm(drive->track)) {
                    drive->speed_settling = true;
                    remove_event_by_data(floppy->scheduler, floppy_speed_settle_callback, floppy, (uint64_t)drv);
                    scheduler_new_cpu_event(floppy->scheduler, floppy_speed_settle_callback, floppy, (uint64_t)drv, 0,
                                            SPEED_SETTLE_TIME_NS);
                    LOG(1, "Drive %d: Step track %d→%d ZONE CHANGE (%d→%d RPM), speed_settling=1", drv,
                        drive->track + (drive->_dirtn ? 1 : -1), drive->track, old_rpm, iwm_track_rpm(drive->track));
                }
                LOG(3, "Drive %d: Step to track %d (%s)", drv, drive->track, drive->_dirtn ? "outward" : "inward");
            }
        }
    } else {
        if (IWM_CA1(floppy)) {
            // MOTORON (CA0=0, CA1=1): CA2 sets motor state
            bool was_off = drive->_motoron;
            drive->_motoron = IWM_CA2(floppy);
            bool now_on = !drive->_motoron;

            if (was_off && now_on) {
                drive->motor_spinning_up = true;
                remove_event(floppy->scheduler, spinup_cb, floppy);
                scheduler_new_cpu_event(floppy->scheduler, spinup_cb, floppy, (uint64_t)drv, 0, MOTOR_SPINUP_TIME_NS);
                LOG(2, "Drive %d: Motor ON (spinning up)", drv);
            } else if (!was_off && !now_on) {
                drive->motor_spinning_up = false;
                remove_event(floppy->scheduler, spinup_cb, floppy);
                LOG(2, "Drive %d: Motor OFF", drv);
            } else {
                LOG(4, "Drive %d: Motor %s (no change)", drv, drive->_motoron ? "off" : "on");
            }
        } else {
            // DIRTN (CA0=0, CA1=0): CA2 sets direction
            drive->_dirtn = IWM_CA2(floppy);
            LOG(4, "Drive %d: Direction = %s", drv, drive->_dirtn ? "outward" : "inward");
        }
    }
}

// Callback invoked when motor spin-up delay completes (IWM)
void floppy_motor_spinup_callback(void *source, uint64_t data) {
    floppy_t *floppy = (floppy_t *)source;
    int drive_index = (int)data;

    if (drive_index < 0 || drive_index >= NUM_DRIVES) {
        LOG(1, "Drive %d: Invalid drive in spin-up callback", drive_index);
        return;
    }

    floppy->drives[drive_index].motor_spinning_up = false;
    LOG(3, "Drive %d: Motor spin-up complete, now ready", drive_index);
}

// Callback invoked when step settle period completes
static void floppy_step_settle_callback(void *source, uint64_t data) {
    floppy_t *floppy = (floppy_t *)source;
    int drive_index = (int)data;

    if (drive_index < 0 || drive_index >= NUM_DRIVES) {
        LOG(1, "Drive %d: Invalid drive in step settle callback", drive_index);
        return;
    }

    // /STEP now returns 1 (settled)
    floppy->drives[drive_index].step_settle_count = 0;
    LOG(5, "Drive %d: Step settle complete", drive_index);
}

// Callback invoked when motor speed settle period completes after a zone change
static void floppy_speed_settle_callback(void *source, uint64_t data) {
    floppy_t *floppy = (floppy_t *)source;
    int drive_index = (int)data;

    if (drive_index < 0 || drive_index >= NUM_DRIVES)
        return;

    floppy->drives[drive_index].speed_settling = false;
    LOG(5, "Drive %d: Speed settle complete", drive_index);
}

// Updates IWM state lines based on address offset (even=clear, odd=set)
void floppy_update_iwm_lines(floppy_t *floppy, int offset) {
    // Map offset pair to bit mask: offset/2 gives line index (0-7)
    static const uint8_t line_masks[] = {IWM_LINE_CA0,    IWM_LINE_CA1,    IWM_LINE_CA2, IWM_LINE_LSTRB,
                                         IWM_LINE_ENABLE, IWM_LINE_SELECT, IWM_LINE_Q6,  IWM_LINE_Q7};

    uint8_t prev = floppy->iwm_lines;
    uint8_t mask = line_masks[offset >> 1];
    if (offset & 1)
        floppy->iwm_lines |= mask; // odd offset = set line
    else
        floppy->iwm_lines &= ~mask; // even offset = clear line

    LOG(7, "IWM line: offset=%d prev=0x%02X now=0x%02X", offset, prev, floppy->iwm_lines);

    // Fire disk control only on LSTRB rising edge (0→1 transition).
    // On real IWM hardware, commands are latched on the LSTRB strobe,
    // not when CA lines change while LSTRB is already high.  This
    // prevents status reads (which set CA lines) from re-triggering
    // the command that happens to share the same CA pattern.
    bool lstrb_rose = IWM_LSTRB(floppy) && !(prev & IWM_LINE_LSTRB);
    if (lstrb_rose)
        floppy_disk_control(floppy);
}

// Reads from the IWM register at the specified offset
uint8_t floppy_iwm_read(floppy_t *floppy, uint32_t offset) {
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
            LOG(6, "Drive %d: Reading data (disabled) = 0xFF", drv);
            return 0xFF;
        }

        if (floppy->disk[drv] == NULL) {
            LOG(6, "Drive %d: Reading data (no disk) = 0x00", drv);
            return 0x00;
        }

        floppy_drive_t *drive = current_drive(floppy);
        uint8_t *data = iwm_track_data(drive, floppy->disk[drv], floppy->sel, floppy->scheduler);
        if (!data) {
            LOG(1, "Drive %d: Read failed - no track data", drv);
            return 0xFF;
        }

        size_t trk_len = iwm_track_length(drive->track);

        // IWM latch mode: only bytes with MSB=1 are latched (valid GCR bytes)
        if (floppy->mode & IWM_MODE_LATCH) {
            for (size_t i = 0; i < trk_len; i++) {
                uint8_t byte = data[drive->offset++];
                if (drive->offset >= (int)trk_len)
                    drive->offset = 0;
                if (byte & 0x80) {
                    LOG(8, "Drive %d: Reading data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, floppy->sel,
                        drive->offset, byte);
                    return byte;
                }
                LOG(9, "Drive %d: Skipping MSB=0 byte 0x%02X at offset %d", drv, byte, drive->offset - 1);
            }
            LOG(6, "Drive %d: No valid GCR byte found on track", drv);
            return 0x00;
        }

        // Non-latch mode: return raw bytes
        uint8_t ret = data[drive->offset++];
        if (drive->offset >= (int)trk_len)
            drive->offset = 0;
        LOG(8, "Drive %d: Reading data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, floppy->sel,
            drive->offset, ret);
        return ret;
    }

    GS_ASSERT(0);
    return 0;
}

// Writes a byte to the IWM register at the specified offset
void floppy_iwm_write(floppy_t *floppy, uint32_t offset, uint8_t byte) {
    floppy_update_iwm_lines(floppy, offset);

    if (!IWM_Q6(floppy) || !IWM_Q7(floppy))
        return;

    int drv = DRIVE_INDEX(floppy);

    if (IWM_ENABLE(floppy)) {
        // Write data to disk
        floppy_drive_t *drive = current_drive(floppy);

        uint8_t *data = iwm_track_data(drive, floppy->disk[drv], floppy->sel, floppy->scheduler);
        if (!data) {
            LOG(1, "Drive %d: Write failed - no track data", drv);
            return;
        }

        data[drive->offset++] = byte;
        LOG(8, "Drive %d: Writing data track=%d side=%d offset=%d = 0x%02X", drv, drive->track, floppy->sel,
            drive->offset - 1, byte);

        floppy_track_t *trk = &drive->tracks[floppy->sel][drive->track];
        if (!trk->modified) {
            trk->modified = true;
            LOG(5, "Drive %d: Track %d side %d marked dirty", drv, drive->track, floppy->sel);
        }

        if (drive->offset == (int)iwm_track_length(drive->track))
            drive->offset = 0;
    } else {
        // Write mode register
        floppy->mode = byte;
        LOG(4, "IWM: Mode register = 0x%02X", byte);
    }
}

// ============================================================================
// Public API
// ============================================================================

// Sets the VIA-driven SEL signal for head selection
void floppy_set_sel_signal(floppy_t *floppy, bool sel) {
    if (!floppy) {
        LOG(4, "SEL signal -> %s (deferred, no floppy)", sel ? "high" : "low");
        return;
    }

    if (floppy->type == FLOPPY_TYPE_SWIM) {
        // SWIM has additional logging and side-latching behavior
        if (sel != floppy->sel)
            LOG(3, "SEL signal changed: %s -> %s (ISM=%d Setup=0x%02X Mode=0x%02X)", floppy->sel ? "high" : "low",
                sel ? "high" : "low", floppy->in_ism_mode, floppy->ism_setup, floppy->ism_mode);
        else
            LOG(6, "SEL signal: %s (no change)", sel ? "high" : "low");
        floppy->sel = sel;
        // Only update the current drive's data side when not actively reading/writing
        if (!IWM_ENABLE(floppy))
            floppy->drives[DRIVE_INDEX(floppy)].data_side = sel;
    } else {
        LOG(6, "SEL signal: %s -> %s", floppy->sel ? "high" : "low", sel ? "high" : "low");
        floppy->sel = sel;
    }
}

// Inserts a disk image into the specified drive
int floppy_insert(floppy_t *floppy, int drive, image_t *disk) {
    GS_ASSERT(drive < NUM_DRIVES);

    if (floppy->disk[drive] != NULL) {
        LOG(2, "Drive %d: Insert failed - disk already present", drive);
        return -1;
    }

    floppy->disk[drive] = disk;

    // SWIM: simulate disk insertion delay
    if (floppy->type == FLOPPY_TYPE_SWIM)
        floppy->cstin_delay[drive] = 0;

    current_drive(floppy)->offset = 0;
    const char *name = disk ? image_get_filename(disk) : NULL;
    LOG(1, "Drive %d: Inserted disk '%s' (writable=%d)", drive, name ? name : "<unnamed>", disk ? disk->writable : 0);

    return 0;
}

// Returns whether a disk is currently inserted in the specified drive
bool floppy_is_inserted(floppy_t *floppy, int drive) {
    GS_ASSERT(drive < NUM_DRIVES);

    return floppy->disk[drive] != NULL;
}

// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *floppy_get_memory_interface(floppy_t *floppy) {
    return &floppy->memory_interface;
}

// === M7e — read-only views for the object model =============================

int floppy_get_type(const floppy_t *floppy) {
    return floppy ? floppy->type : 0;
}
bool floppy_get_sel(const floppy_t *floppy) {
    return floppy ? floppy->sel : false;
}
int floppy_drive_track(const floppy_t *floppy, unsigned drive) {
    if (!floppy || drive >= NUM_DRIVES)
        return 0;
    return floppy->drives[drive].track;
}
int floppy_drive_side(const floppy_t *floppy, unsigned drive) {
    if (!floppy || drive >= NUM_DRIVES)
        return 0;
    return floppy->drives[drive].data_side;
}
bool floppy_drive_motor_on(const floppy_t *floppy, unsigned drive) {
    if (!floppy || drive >= NUM_DRIVES)
        return false;
    // _motoron is active-low: false = motor running.
    return !floppy->drives[drive]._motoron;
}
const char *floppy_drive_disk_path(const floppy_t *floppy, unsigned drive) {
    if (!floppy || drive >= NUM_DRIVES || !floppy->disk[drive])
        return NULL;
    return image_path(floppy->disk[drive]);
}

image_t *floppy_drive_image(const floppy_t *floppy, unsigned drive) {
    if (!floppy || drive >= NUM_DRIVES)
        return NULL;
    return floppy->disk[drive];
}

bool floppy_drive_eject(floppy_t *floppy, unsigned drive) {
    if (!floppy || drive >= NUM_DRIVES || !floppy->disk[drive])
        return false;
    // Mirror the in-controller eject flow (see the IWM CA0/1/2=1 path
    // around line 240): flush modified tracks first while the image is
    // still valid, drop the cached GCR buffers, then null the slot.
    // The image_t* itself is owned by cfg->images and freed at system
    // teardown; calling image_close here would double-free.
    iwm_flush_modified_tracks(&floppy->drives[drive], floppy->disk[drive], (int)drive);
    memset(floppy->drives[drive].tracks, 0, sizeof(floppy->drives[drive].tracks));
    floppy->disk[drive] = NULL;
    return true;
}

// ============================================================================
// Lifecycle (Init / Delete / Checkpoint)
// ============================================================================

// Initializes a floppy controller of the given type and maps it to memory
floppy_t *floppy_init(int type, memory_map_t *map, struct scheduler *scheduler, checkpoint_t *checkpoint) {
    floppy_t *floppy = malloc(sizeof(floppy_t));
    if (!floppy) {
        LOG(1, "Floppy: Allocation failed");
        return NULL;
    }

    memset(floppy, 0, sizeof(floppy_t));
    floppy->type = type;
    LOG(2, "Floppy: Controller created (type=%s)", type == FLOPPY_TYPE_SWIM ? "SWIM" : "IWM");

    floppy->scheduler = scheduler;

    if (type == FLOPPY_TYPE_SWIM) {
        scheduler_new_event_type(scheduler, "swim", floppy, "motor_spinup", &floppy_swim_motor_spinup_callback);
        floppy_swim_setup(floppy, map);
    } else {
        scheduler_new_event_type(scheduler, "floppy", floppy, "motor_spinup", &floppy_motor_spinup_callback);
        floppy_iwm_setup(floppy, map);
    }
    scheduler_new_event_type(scheduler, "floppy", floppy, "step_settle", &floppy_step_settle_callback);
    scheduler_new_event_type(scheduler, "floppy", floppy, "speed_settle", &floppy_speed_settle_callback);

    if (checkpoint) {
        LOG(3, "Floppy: Restoring from checkpoint");

        // Clear track data pointers before restoring (they will be overwritten)
        for (int d = 0; d < NUM_DRIVES; d++)
            for (int s = 0; s < NUM_SIDES; s++)
                for (int t = 0; t < NUM_TRACKS; t++)
                    floppy->drives[d].tracks[s][t].data = NULL;

        // Read plain-data portion
        system_read_checkpoint_data(checkpoint, floppy, FLOPPY_CHECKPOINT_SIZE);

        // Restore disk images by filename
        for (int i = 0; i < NUM_DRIVES; i++) {
            floppy->disk[i] = NULL;
            uint32_t len = 0;
            system_read_checkpoint_data(checkpoint, &len, sizeof(len));
            if (len > 0) {
                char *name = malloc(len);
                if (name) {
                    system_read_checkpoint_data(checkpoint, name, len);
                    floppy->disk[i] = setup_get_image_by_filename(name);
                    free(name);
                } else {
                    // Consume bytes to keep stream aligned
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
                    floppy_track_t *trk = &floppy->drives[d].tracks[s][t];
                    uint8_t has_data = 0;
                    system_read_checkpoint_data(checkpoint, &has_data, 1);
                    if (has_data && trk->size > 0) {
                        trk->data = malloc(trk->size);
                        if (trk->data)
                            system_read_checkpoint_data(checkpoint, trk->data, trk->size);
                        else {
                            // Consume bytes to keep stream aligned
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

    // Object-tree binding — instance_data is the floppy itself; the
    // drives collection child and per-drive entries follow.
    floppy->object = object_new(&floppy_class, floppy, "floppy");
    if (floppy->object) {
        object_attach(object_root(), floppy->object);
        floppy->drives_object = object_new(&floppy_drives_collection_class, floppy, "drives");
        if (floppy->drives_object)
            object_attach(floppy->object, floppy->drives_object);
        for (int i = 0; i < NUM_DRIVES; i++) {
            floppy->drive_links[i].floppy = floppy;
            floppy->drive_links[i].slot = i;
            floppy->drive_objects[i] = object_new(&floppy_drive_class, &floppy->drive_links[i], NULL);
        }
    }

    return floppy;
}

// Frees all resources associated with the floppy controller
void floppy_delete(floppy_t *floppy) {
    if (!floppy)
        return;
    LOG(2, "Floppy: Deleting controller");

    // Tear down per-drive entry objects (never attached), then the
    // drives collection, then the top-level node.
    for (int i = 0; i < NUM_DRIVES; i++) {
        if (floppy->drive_objects[i]) {
            object_delete(floppy->drive_objects[i]);
            floppy->drive_objects[i] = NULL;
        }
    }
    if (floppy->drives_object) {
        object_detach(floppy->drives_object);
        object_delete(floppy->drives_object);
        floppy->drives_object = NULL;
    }
    if (floppy->object) {
        object_detach(floppy->object);
        object_delete(floppy->object);
        floppy->object = NULL;
    }

    // Flush and free track data for both drives
    for (int d = 0; d < NUM_DRIVES; d++) {
        iwm_flush_modified_tracks(&floppy->drives[d], floppy->disk[d], d);
        for (int s = 0; s < NUM_SIDES; s++) {
            for (int t = 0; t < NUM_TRACKS; t++) {
                free(floppy->drives[d].tracks[s][t].data);
            }
        }
    }
    free(floppy);
}

// Saves the floppy controller state to a checkpoint
void floppy_checkpoint(floppy_t *restrict floppy, checkpoint_t *checkpoint) {
    if (!floppy || !checkpoint)
        return;
    LOG(13, "Floppy: Checkpointing controller");

    // Write plain-data portion
    system_write_checkpoint_data(checkpoint, floppy, FLOPPY_CHECKPOINT_SIZE);

    // Write disk filenames for each drive
    for (int i = 0; i < NUM_DRIVES; i++) {
        const char *name = floppy->disk[i] ? image_get_filename(floppy->disk[i]) : NULL;
        uint32_t len = (name && *name) ? (uint32_t)strlen(name) + 1 : 0;
        system_write_checkpoint_data(checkpoint, &len, sizeof(len));
        if (len)
            system_write_checkpoint_data(checkpoint, name, len);
    }

    // Save GCR track buffers (preserves in-flight write data)
    for (int d = 0; d < NUM_DRIVES; d++) {
        for (int s = 0; s < NUM_SIDES; s++) {
            for (int t = 0; t < NUM_TRACKS; t++) {
                floppy_track_t *trk = &floppy->drives[d].tracks[s][t];
                uint8_t has_data = (trk->data != NULL);
                system_write_checkpoint_data(checkpoint, &has_data, 1);
                if (has_data)
                    system_write_checkpoint_data(checkpoint, trk->data, trk->size);
            }
        }
    }
}

// === Object-model class descriptors =========================================
//
// `floppy` is the unified IWM/SWIM controller. Both Plus (IWM-only)
// and SE/30 (SWIM dual-mode) attach a floppy_t at machine init, so
// the object is always present when cfg->floppy is non-NULL.
//
// Drive layout matches the proposal: `floppy.drives[0]` is the
// internal drive, `floppy.drives[1]` is external. Indexed children
// are dense (always exactly 2 slots) — index sparseness only matters
// for collections that grow (breakpoints, scsi.devices); the floppy
// drive count is a hardware constant.
//
// instance_data on the floppy / drives nodes is the floppy_t* itself;
// per-drive entry objects carry a pointer to floppy->drive_links[i].

static floppy_t *floppy_self_from(struct object *self) {
    return (floppy_t *)object_data(self);
}

static const char *const FLOPPY_TYPE_NAMES[] = {"iwm", "swim"};

static value_t floppy_attr_type(struct object *self, const member_t *m) {
    (void)m;
    floppy_t *floppy = floppy_self_from(self);
    int t = floppy ? floppy_get_type(floppy) : 0;
    if (t < 0 || t > 1)
        t = 0;
    return val_enum(t, FLOPPY_TYPE_NAMES, 2);
}

static value_t floppy_attr_sel(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(floppy_get_sel(floppy_self_from(self)));
}

// `floppy.identify(path)` — return density string for a recognised floppy
// image ("400K" / "800K" / "1.4MB"), or empty string otherwise. Empty is
// falsy under the predicate-truthy rule, so callers can do
//   `assert ${floppy.identify(path)} "not a floppy"`.
static value_t floppy_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    image_t *img = image_open_readonly(argv[0].s);
    if (!img)
        return val_str("");
    const char *density = "";
    switch (img->type) {
    case image_fd_ss:
        density = "400K";
        break;
    case image_fd_ds:
        density = "800K";
        break;
    case image_fd_hd:
        density = "1.4MB";
        break;
    default:
        density = "";
        break;
    }
    image_close(img);
    return val_str(density);
}

// `floppy.create(path, [hd])` — create a blank floppy image and auto-mount
// it. `hd` is the optional density flag: "hd" / true → 1.44 MB, anything
// else → 800 KB. The legacy `--hd` string spelling is also accepted.
static value_t floppy_method_create(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    bool high_density = false;
    int preferred = -1;
    // hd is V_NONE-kind: body discriminates string / bool / integer.
    if (argc >= 2) {
        if (argv[1].kind == V_STRING && argv[1].s) {
            if (strcmp(argv[1].s, "hd") == 0 || strcmp(argv[1].s, "--hd") == 0) {
                high_density = true;
            } else if (argv[1].s[0] >= '0' && argv[1].s[0] <= '1' && argv[1].s[1] == '\0') {
                preferred = argv[1].s[0] - '0';
            } else if (*argv[1].s) {
                return val_err("floppy.create: second arg must be \"hd\" or drive index 0/1");
            }
        } else if (argv[1].kind == V_BOOL) {
            high_density = argv[1].b;
        } else if (argv[1].kind == V_INT || argv[1].kind == V_UINT) {
            int64_t d = (argv[1].kind == V_INT) ? argv[1].i : (int64_t)argv[1].u;
            if (d != 0 && d != 1)
                return val_err("floppy.create: drive index must be 0 or 1");
            preferred = (int)d;
        }
    }
    int rc = system_create_floppy(argv[0].s, high_density, preferred);
    return val_bool(rc == 0);
}

static const arg_decl_t floppy_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "Floppy image path"},
};

static const arg_decl_t floppy_create_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Output path"},
    {.name = "hd",
     .kind = V_NONE,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "\"hd\" / true for 1.44 MB; drive index 0/1 to pick a slot"},
};

static const member_t floppy_members[] = {
    {.kind = M_ATTR,
     .name = "type",
     .doc = "Controller type: iwm (Plus) or swim (SE/30)",
     .flags = VAL_RO,
     .attr = {.type = V_ENUM, .get = floppy_attr_type, .set = NULL}},
    {.kind = M_ATTR,
     .name = "sel",
     .doc = "VIA-driven head-select signal",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = floppy_attr_sel, .set = NULL}},
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "Return floppy density (\"400K\" / \"800K\" / \"1.4MB\") or empty if not a floppy",
     .method = {.args = floppy_path_arg, .nargs = 1, .result = V_STRING, .fn = floppy_method_identify}},
    {.kind = M_METHOD,
     .name = "create",
     .doc = "Create a blank floppy image and auto-mount it",
     .method = {.args = floppy_create_args, .nargs = 2, .result = V_BOOL, .fn = floppy_method_create}},
};

const class_desc_t floppy_class = {
    .name = "floppy",
    .members = floppy_members,
    .n_members = sizeof(floppy_members) / sizeof(floppy_members[0]),
};

// --- Per-drive entry class -------------------------------------------------

static floppy_t *floppy_drive_floppy(struct object *self, unsigned *slot_out) {
    floppy_drive_link_t *link = (floppy_drive_link_t *)object_data(self);
    if (!link) {
        *slot_out = 0;
        return NULL;
    }
    *slot_out = (unsigned)link->slot;
    return link->floppy;
}

static value_t floppy_drive_attr_index(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    (void)floppy_drive_floppy(self, &slot);
    return val_int((int)slot);
}

static value_t floppy_drive_attr_present(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    return val_bool(floppy && floppy_is_inserted(floppy, (int)slot));
}

static value_t floppy_drive_attr_track(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    return val_int(floppy_drive_track(floppy, slot));
}

static value_t floppy_drive_attr_side(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    return val_int(floppy_drive_side(floppy, slot));
}

static value_t floppy_drive_attr_motor_on(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    return val_bool(floppy_drive_motor_on(floppy, slot));
}

static value_t floppy_drive_attr_disk(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    const char *p = floppy_drive_disk_path(floppy, slot);
    return val_str(p ? p : "");
}

static value_t floppy_drive_method_eject(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    unsigned slot = 0;
    floppy_t *floppy = floppy_drive_floppy(self, &slot);
    if (!floppy)
        return val_err("floppy not available");
    if (!floppy_drive_eject(floppy, slot))
        return val_err("drive %u: no disk inserted", slot);
    return val_none();
}

// `floppy.drives[N].insert(path, [writable])` — mount an image into this
// specific drive. Routes through shell_fd_argv so persistence / VFS
// resolution / drive-occupancy bookkeeping all stay in one place.
static value_t floppy_drive_method_insert(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    unsigned slot = 0;
    if (!floppy_drive_floppy(self, &slot))
        return val_err("floppy.drives.N.insert: floppy controller not available");
    bool writable = (argc >= 2) ? argv[1].b : false;
    char line[1024];
    int n = snprintf(line, sizeof(line), "fd insert \"%s\" %u %s", argv[0].s, slot, writable ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("floppy.drives.N.insert: path too long");
    char *targv[16];
    int targc = tokenize(line, targv, 16);
    if (targc <= 0)
        return val_err("floppy.drives.N.insert: tokenisation failed");
    return val_bool(shell_fd_argv(targc, targv) == 0);
}

static const arg_decl_t floppy_drive_insert_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Host path or storage URI of the image to mount"},
    {.name = "writable", .kind = V_BOOL, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Mount writable (default false)"},
};

static const member_t floppy_drive_members[] = {
    {.kind = M_ATTR,
     .name = "index",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = floppy_drive_attr_index, .set = NULL}},
    {.kind = M_ATTR,
     .name = "present",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = floppy_drive_attr_present, .set = NULL}},
    {.kind = M_ATTR,
     .name = "track",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = floppy_drive_attr_track, .set = NULL}},
    {.kind = M_ATTR,
     .name = "side",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = floppy_drive_attr_side, .set = NULL}},
    {.kind = M_ATTR,
     .name = "motor_on",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = floppy_drive_attr_motor_on, .set = NULL}},
    {.kind = M_ATTR,
     .name = "disk",
     .doc = "Path to currently inserted image (empty when no disk)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = floppy_drive_attr_disk, .set = NULL}},
    {.kind = M_METHOD,
     .name = "eject",
     .doc = "Remove the inserted disk",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = floppy_drive_method_eject}},
    {.kind = M_METHOD,
     .name = "insert",
     .doc = "Mount a disk image into this drive",
     .method = {.args = floppy_drive_insert_args, .nargs = 2, .result = V_BOOL, .fn = floppy_drive_method_insert}},
};

const class_desc_t floppy_drive_class = {
    .name = "floppy_drive",
    .members = floppy_drive_members,
    .n_members = sizeof(floppy_drive_members) / sizeof(floppy_drive_members[0]),
};

// --- Drives collection: indexed children -----------------------------------

static struct object *floppy_drives_get(struct object *self, int index) {
    floppy_t *floppy = (floppy_t *)object_data(self);
    if (!floppy || index < 0 || index >= NUM_DRIVES)
        return NULL;
    return floppy->drive_objects[index];
}
static int floppy_drives_count(struct object *self) {
    floppy_t *floppy = (floppy_t *)object_data(self);
    return floppy ? NUM_DRIVES : 0;
}
static int floppy_drives_next(struct object *self, int prev_index) {
    floppy_t *floppy = (floppy_t *)object_data(self);
    if (!floppy)
        return -1;
    int next = prev_index + 1;
    return next < NUM_DRIVES ? next : -1;
}

static const member_t floppy_drives_collection_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &floppy_drive_class,
               .indexed = true,
               .get = floppy_drives_get,
               .count = floppy_drives_count,
               .next = floppy_drives_next,
               .lookup = NULL}},
};
const class_desc_t floppy_drives_collection_class = {
    .name = "floppy_drives",
    .members = floppy_drives_collection_members,
    .n_members = 1,
};
