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
#include "platform.h"
#include "system.h"

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

// Returns pointer to the currently selected drive based on IWM SELECT line
static floppy_drive_t *current_drive(floppy_t *floppy) {
    return &floppy->drives[IWM_SELECT(floppy) ? 1 : 0];
}

// Returns the current disk status based on IWM CA lines and SEL signal
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
        if (drive->step_settle_count > 0) {
            drive->step_settle_count--;
            ret = 0; // step in progress
        } else {
            ret = 1; // step complete
        }
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
        desc = "RDDATA side0";
        ret = 1;
        break;
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
    case 0x0B: // /TACH: 60 pulses per revolution
    {
        const char *tach_reason = NULL;
        ret = iwm_tach_signal(floppy->scheduler, drive, &tach_reason);
        LOG(6, "Drive %d: Reading /TACH = %d (%s, track=%d)", drv, ret, tach_reason, drive->track);
        return ret;
    }
    case 0x0C: // RDDATA: data from side 1
        desc = "RDDATA side1";
        ret = 1;
        break;
    case 0x0D: // reserved (switch to GCR mode)
        desc = "reserved (SEL+CA2+CA0)";
        ret = 0;
        break;
    case 0x0E: // /READY: zero when ready
        desc = "/READY";
        ret = drive->motor_spinning_up ? 1 : 0;
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
                // _dirtn: 0=inward (higher tracks), 1=outward (lower tracks)
                if (drive->_dirtn) {
                    if (drive->track > 0)
                        drive->track--;
                } else {
                    if (drive->track < NUM_TRACKS - 1)
                        drive->track++;
                }
                drive->offset = 0;
                // /STEP reads as 0 (active) for the next few reads, then
                // returns to 1 (settled).  This models the ~12ms step motor
                // settle pulse that MacTest checks to verify the drive works.
                drive->step_settle_count = STEP_SETTLE_READS;
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

// Updates IWM state lines based on address offset (even=clear, odd=set)
void floppy_update_iwm_lines(floppy_t *floppy, int offset) {
    // Map offset pair to bit mask: offset/2 gives line index (0-7)
    static const uint8_t line_masks[] = {IWM_LINE_CA0,    IWM_LINE_CA1,    IWM_LINE_CA2, IWM_LINE_LSTRB,
                                         IWM_LINE_ENABLE, IWM_LINE_SELECT, IWM_LINE_Q6,  IWM_LINE_Q7};

    uint8_t mask = line_masks[offset >> 1];
    if (offset & 1)
        floppy->iwm_lines |= mask; // odd offset = set line
    else
        floppy->iwm_lines &= ~mask; // even offset = clear line

    LOG(7, "IWM line: offset=%d", offset);

    if (IWM_LSTRB(floppy))
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

    return floppy;
}

// Frees all resources associated with the floppy controller
void floppy_delete(floppy_t *floppy) {
    if (!floppy)
        return;
    LOG(2, "Floppy: Deleting controller");

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
