// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// new_age.c
// The New Age floppy controller (NEC µPD72070, Apple mode) and its
// SuperDrive — see new_age.h for the model's scope and the board seam.
//
// The host interface is the 765 three-phase protocol, coordinated entirely
// by the main status register [µPD72070 §5.1.1]:
//
//   * idle: RQM=1, DIO=0, CB=0.
//   * command phase: each FIFO write takes one byte; CB sets with the
//     first.  The last parameter byte (the "launch byte") starts the
//     command.
//   * execution phase (the data commands only): RQM=0, CB=1 — "upon
//     receiving the last parameter, the FDC enters the execution phase
//     without setting the RQM bit".  The shipped driver's WriteToFDC waits
//     for RQM after every byte, so the launch byte's write returns when the
//     transfer is over and the driver's DMA-done poll starts against a
//     finished command.
//   * result phase: RQM=1, DIO=1, CB=1; each FIFO read pops one byte and
//     the first deasserts INT; after the last the chip is idle again.
//
// Drive commands (Seek, Recalibrate, Set Motor Control, Set Drive Mode)
// run in the background the way the 765's seek does: the chip returns to
// idle at once and interrupts when the drive handshake ends; the host
// collects the cause with Sense Interrupt Status.  Set Enable Control,
// Eject and the DPLL commands interrupt at once ("the FDC always informs the
// host of a normal termination").
//
// The interrupt is a LEVEL: command arrival and the first result read drop
// it [µPD72070 §5.1.1, "Reset INT, Set CB"], and Sense Interrupt Status
// collects the one pending cause.  The driver's SenseInterrupt waits for CB
// to be CLEAR after the flag appears and GetResult waits for it to be SET —
// which is exactly the idle / result-phase CB of the protocol above.
//
// Media are presented at the sector level (new_age.h).  Rotation is
// modelled — header positions follow emulated time at the drive's spindle
// speed — because the driver's whole-track read starts at the header it
// last saw (MFMTrack) and its MFM format probe walks headers until the
// sector number wraps (SetUpFDC).  Sectors are laid out without
// interleave; nothing the driver does observes the physical order.

#include "new_age.h"

#include "floppy.h"
#include "floppy_geometry.h"
#include "image.h"
#include "log.h"
#include "scheduler.h"

#include <math.h>
#include <string.h>

// One category for the whole floppy subsystem (see swim3.c).
LOG_USE_CATEGORY_NAME("floppy");

// === Timing =================================================================
//
// The bounds the specification puts on the drive handshakes, scaled where a
// real value would only slow the emulator down without changing what the
// driver sees.  Every wait in the driver is a TimeDBRA-scaled loop whose
// body is several instructions per poll, so its budget in emulated time is
// several times its nominal milliseconds; the values below sit well inside
// the nominal figure as well.

// Motor on to /Ready: 600 ms typical, 1 s max [µPD72070 §4.4.3, §5.2]; the
// shared module's spin-up time is used so every controller agrees.
#define NA_MOTOR_ON_NS (400.0e6)
// Drive mode change (GCR <-> MFM): 800 ms max [µPD72070 §4.4.1].
#define NA_MODE_NS (50.0e6)
// One head step plus its share of settling: 12 ms track-to-track max and
// 30 ms settle on the Apple drive [Apple 3.5-inch drive spec §2.3.4], with
// the /Ready window 18 ms per track [µPD72070 Table 4.4.1].
#define NA_STEP_NS   (3.0e6)
#define NA_SETTLE_NS (15.0e6)
// Eject to the /CSTIN change: up to 1.5 s [µPD72070 §5.2].
#define NA_EJECT_NS (500.0e6)
// GCR address/data mark search: 400 ms [µPD72070 §4.9.4].  MFM searches
// for two index pulses, i.e. two revolutions (na_search_ns).
#define NA_GCR_SEARCH_NS (400.0e6)
// A command that fails before touching the medium still takes the chip a
// moment to answer.
#define NA_SHORT_NS (20.0e3)

// The /CSTIN poller [µPD72070 Ch. 6 POLLING flowchart]: the idle loop scans
// both drives, and after raising INT for a change waits 100 ms before the
// next scan.  Polling only runs when the chip is idle; the quiet window
// keeps it out of the gaps between the bytes of a command sequence the
// host is in the middle of.
#define NA_POLL_NS       (10.0e6)
#define NA_POLL_HOLD_NS  (100.0e6)
#define NA_POLL_QUIET_NS (5.0e6)

// === Command set ============================================================

typedef enum {
    NA_IDLE = 0,
    NA_COMMAND, // accumulating command bytes
    NA_EXEC, // execution phase of a data command
    NA_RESULT, // result bytes waiting in the FIFO
} na_phase_t;

// Event kinds (the event's data word: kind << 8 | drive).
enum {
    NA_EV_XFER = 1, // the executing data command's next step
    NA_EV_SEEK, // a seek/recalibrate ended
    NA_EV_MOTOR, // the spindle reached speed (or never did)
    NA_EV_MODE, // a drive mode change ended
    NA_EV_EJECT, // the medium leaves the drive
};

// Total command length (opcode + parameters); 0 = invalid in Apple mode
// [µPD72070 §1.4.1; the Apple-mode command table].  Full-opcode match: bit
// 7 is the on/off selector on the Apple drive commands, bit 6 the GCR/MFM
// selector on the data commands; the MT/TB variants the driver never
// builds are not accepted.
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
        return 9; // Read Data, Write Data, Read A Track
    case 0x0D:
    case 0x4D:
        return 6; // Format A Track (the filler byte launches it)
    case 0x01:
    case 0x41:
        return 5; // Format/Write (the sync-group/GAP3 byte launches it)
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

// === Interrupt ==============================================================

static void na_set_int(new_age_t *na, bool level) {
    if (na->be.set_irq)
        na->be.set_irq(na->be.ctx, level);
}

// Record the cause Sense Interrupt Status will return, and raise INT.
static void na_interrupt(new_age_t *na, uint8_t st0, bool seek_end, uint8_t pcn) {
    na->int_pending = 1;
    na->int_seek_end = seek_end ? 1 : 0;
    na->int_st0 = st0;
    na->int_pcn = pcn;
    LOG(4, "interrupt ST0=$%02X%s", st0, seek_end ? " (seek end)" : "");
    na_set_int(na, true);
}

// Enter the result phase with `len` queued bytes; `irq` raises INT (every
// result phase but Sense Interrupt/Drive Status and Revision does).
static void na_enter_result(new_age_t *na, int len, bool irq) {
    na->phase = NA_RESULT;
    na->result_len = (uint8_t)len;
    na->result_idx = 0;
    if (irq)
        na_set_int(na, true);
}

// === Drive and media ========================================================

static int na_drive_count(const new_age_t *na) {
    return na->fd ? floppy_drive_count(na->fd) : 0;
}

static bool na_drive_exists(const new_age_t *na, int d) {
    return d >= 0 && d < na_drive_count(na);
}

static image_t *na_image(const new_age_t *na, int d) {
    return na_drive_exists(na, d) ? floppy_drive_image(na->fd, (unsigned)d) : NULL;
}

// /Ready: a medium in place and the spindle at speed.
static bool na_ready(const new_age_t *na, int d) {
    return na_image(na, d) && floppy_drive_motor_on(na->fd, (unsigned)d) && na->motor_ready[d];
}

// The medium's geometry and the format it carries now.
static bool na_media(const new_age_t *na, int d, floppy_media_t *m) {
    memset(m, 0, sizeof(*m));
    return na_image(na, d) && floppy_media_current(na->fd, (unsigned)d, m);
}

// One revolution, from the shared rotation model (floppy_geometry.h): the
// SuperDrive spins 1.44 MB at 300 rpm and, because New Age keeps its
// 500 kbps rate for both MFM densities, 720 KB at 600 rpm [µPD72070
// §1.3.2]; GCR at the zone's speed.
static double na_rev_ns(const floppy_media_t *m, int track) {
    return floppy_media_rev_ns(m, track, FLOPPY_MFM_DD_RPM_NA);
}

// How long one sector's worth of track takes to pass the head.
static double na_slot_ns(const floppy_media_t *m, int track) {
    return na_rev_ns(m, track) / (double)floppy_media_spt(m, track);
}

// The header-search timeout: 400 ms in GCR, two index pulses in MFM.
static double na_search_ns(const floppy_media_t *m, int track, bool mfm) {
    if (mfm && m && m->valid)
        return 2.0 * na_rev_ns(m, track);
    return mfm ? 400.0e6 : NA_GCR_SEARCH_NS;
}

// The next header to pass under the head: its slot index around the track,
// and how long until it arrives.
static int na_next_header(const new_age_t *na, const floppy_media_t *m, int track, double *delay_ns) {
    return floppy_media_next_header(m, track, na_rev_ns(m, track), scheduler_time_ns(na->sched), delay_ns);
}

// How long until slot `idx` has wholly passed under the head.
static double na_until_slot_end(const new_age_t *na, const floppy_media_t *m, int track, int idx) {
    double now = scheduler_time_ns(na->sched);
    double slot = na_slot_ns(m, track);
    int spt = floppy_media_spt(m, track);
    double n = floor(now / slot + 1e-3); // the slot under the head now
    int cur = (int)fmod(n, (double)spt);
    int ahead = (idx - cur + spt) % spt;
    if (ahead == 0)
        ahead = spt; // it is passing now: wait for it to come round again
    return (n + (double)ahead + 1.0) * slot - now;
}

// Sector numbering: MFM headers count from 1, GCR from 0 [µPD72070 §4.8.1].
static int na_first_sector(bool mfm) {
    return mfm ? 1 : 0;
}

// Can the head read this medium the way the chip is set up?  The opcode's
// bit 6, the drive's mode latch and the medium's recording must agree, and
// MFM additionally needs the 500 kbps rate and conventional (not 1 Mbps
// perpendicular) recording — the SuperDrive has no ED media.
static bool na_can_read(const new_age_t *na, int d, const floppy_media_t *m, bool mfm) {
    if (!m->valid || (na->drive_mfm[d] != 0) != mfm || m->mfm != mfm)
        return false;
    if (mfm && ((na->drr & 0x03) != 0 || na->perp == 3))
        return false;
    return true;
}

// ST3: every bit is an active-low drive line, read through the multiplexed
// status protocol [µPD72070 §3.1.12, §4.4.6].  A missing drive floats every
// line high ($FF), which is the driver's drive-absent test (Open).  The
// SuperDrive is a 2 MB-class drive (bit 3) that is not a 4 MB Typhoon
// (bit 2 high); an empty drive reads write-protected and not ready.
static uint8_t na_st3(const new_age_t *na, int d) {
    if (!na_drive_exists(na, d))
        return 0xFF;
    image_t *img = na_image(na, d);
    floppy_media_t m;
    bool hd = na_media(na, d, &m) && m.hd;
    uint8_t st3 = 0x08 | 0x04 | 0x02; // 2 MB drive, /Mode ID, /Select Media (not ED)
    if (!hd)
        st3 |= 0x80; // /2MB-or-4MB media high: low-density medium (or none)
    if (img && image_is_writable(img))
        st3 |= 0x40; // /Write protect high: writable
    if (!na_ready(na, d))
        st3 |= 0x20; // /Ready
    if (floppy_drive_track(na->fd, (unsigned)d) != 0)
        st3 |= 0x10; // /TK0
    if (!na->drive_mfm[d])
        st3 |= 0x01; // /MFM mode high: GCR
    return st3;
}

// === Events =================================================================

static void na_event(void *source, uint64_t data);
static void na_poll_event(void *source, uint64_t data);

static void na_arm(new_age_t *na, int kind, int drive, double delay_ns) {
    if (delay_ns < 1000.0)
        delay_ns = 1000.0;
    scheduler_new_cpu_event(na->sched, na_event, na, (uint64_t)((kind << 8) | (drive & 0xFF)), 0,
                            (uint64_t)ceil(delay_ns));
}

// === Result assembly ========================================================

// The seven-byte result of the data commands: ST0-ST2 and the ID.
static void na_finish(new_age_t *na) {
    na->result[0] = na->x_st0;
    na->result[1] = na->x_st1;
    na->result[2] = na->x_st2;
    na->result[3] = na->x_c;
    na->result[4] = na->x_h;
    na->result[5] = na->x_r;
    na->result[6] = na->x_n;
    LOG(3, "command $%02X done: ST0=$%02X ST1=$%02X ST2=$%02X C=%u H=%u R=%u N=$%02X", na->x_op, na->x_st0, na->x_st1,
        na->x_st2, na->x_c, na->x_h, na->x_r, na->x_n);
    na_enter_result(na, 7, true);
}

// An abnormal termination with the given ST1/ST2 bits.
static void na_fail(new_age_t *na, uint8_t st1, uint8_t st2) {
    na->x_st0 = (uint8_t)(NEW_AGE_ST0_ABNORMAL | (na->x_st0 & 0x3F)); // IC = %01, the rest kept
    na->x_st1 |= st1;
    na->x_st2 |= st2;
    na_finish(na);
}

// The normal-termination ID update after the last sector transferred: R
// steps on below EOT and wraps to the first sector at it.
//
// The cylinder does NOT advance at EOT, although the specification's table
// says it does for MT = 0 [µPD72070 Table 4.8.1].  The shipped driver
// stores the result's C as the head's cylinder (GetResult → fCylinder) and
// skips the next seek when the target matches it; every one of its
// whole-track reads ends at EOT, so a C+1 there would leave it one cylinder
// out after each read and send the next transfer to the wrong cylinder,
// recovering only through 25 retries and a recalibrate.  The driver works
// on the real machine, so the real chip reports the cylinder it is on.
static void na_update_id(new_age_t *na, int last, bool mfm) {
    if (last < na->x_eot)
        na->x_r = (uint8_t)(last + 1);
    else
        na->x_r = (uint8_t)na_first_sector(mfm);
}

// === The data commands ======================================================

static bool na_op_mfm(uint8_t op) {
    return (op & 0x40) != 0;
}

// The opcode family, with the GCR/MFM bit folded out.
static uint8_t na_op_base(uint8_t op) {
    return (uint8_t)(op & ~0x40);
}

// Start a data command: decode its parameters and arm the first step.
static void na_start_data(new_age_t *na) {
    uint8_t op = na->cmd[0];
    uint8_t base = na_op_base(op);
    bool mfm = na_op_mfm(op);
    int d = na->cmd[1] & 0x03;
    na->x_op = op;
    na->x_drive = (uint8_t)d;
    na->x_head = (uint8_t)((na->cmd[1] >> 2) & 1);
    na->x_st0 = (uint8_t)((na->x_head ? NEW_AGE_ST0_HD : 0) | d);
    na->x_st1 = na->x_st2 = 0;
    na->x_tc = 0;
    na->x_count = 0;
    na->phase = NA_EXEC;

    floppy_media_t m;
    bool have = na_media(na, d, &m);
    int track = na_drive_exists(na, d) ? floppy_drive_track(na->fd, (unsigned)d) : 0;
    na->x_c = (uint8_t)track;
    na->x_h = na->x_head;
    na->x_r = 0;
    na->x_n = 0;

    if (base == 0x0A) { // Read ID
        na->x_n = m.fmt_byte;
        if (!na_ready(na, d)) {
            na->x_st0 |= NEW_AGE_ST0_NR;
            na_arm(na, NA_EV_XFER, d, NA_SHORT_NS);
            return;
        }
        if (!have || !na_can_read(na, d, &m, mfm) || na->x_head >= m.sides) {
            // Nothing the chip can frame passes under the head.
            na->x_st1 = NEW_AGE_ST1_MA;
            na_arm(na, NA_EV_XFER, d, na_search_ns(&m, track, mfm));
            return;
        }
        double delay = 0;
        int idx = na_next_header(na, &m, track, &delay);
        na->x_r = (uint8_t)(idx + na_first_sector(mfm));
        na_arm(na, NA_EV_XFER, d, delay);
        return;
    }

    if (base == 0x06 || base == 0x05 || base == 0x02) { // Read Data, Write Data, Read A Track
        na->x_c = na->cmd[2];
        na->x_h = na->cmd[3];
        na->x_r = na->cmd[4];
        na->x_n = na->cmd[5];
        na->x_eot = na->cmd[6];
    } else if (base == 0x0D || base == 0x01) { // Format A Track, Format/Write
        na->x_n = na->cmd[2];
        na->x_sc = na->cmd[3];
    }

    if (!na_ready(na, d)) {
        na->x_st0 |= NEW_AGE_ST0_NR;
        na_arm(na, NA_EV_XFER, d, NA_SHORT_NS);
        return;
    }
    if (base == 0x1E) { // Raw Dump: the nibble path is not modelled
        na->x_st1 = NEW_AGE_ST1_MA;
        na_arm(na, NA_EV_XFER, d, na_search_ns(&m, track, mfm));
        return;
    }
    bool writes = base == 0x05 || base == 0x0D || base == 0x01;
    if (writes && !(na_image(na, d) && image_is_writable(na_image(na, d)))) {
        na->x_st1 = NEW_AGE_ST1_NW;
        na_arm(na, NA_EV_XFER, d, NA_SHORT_NS);
        return;
    }
    if (base == 0x0D || base == 0x01) {
        // Formatting lays the track down from scratch: what was on it does
        // not matter, only that the drive is in the mode the opcode frames.
        // The write starts at the index in MFM and anywhere in GCR
        // [µPD72070 §4.7]; either way one revolution passes.
        if ((na->drive_mfm[d] != 0) != mfm || !have) {
            na->x_st0 |= NEW_AGE_ST0_EC;
            na_arm(na, NA_EV_XFER, d, NA_SHORT_NS);
            return;
        }
        na_arm(na, NA_EV_XFER, d, na_rev_ns(&m, track));
        return;
    }
    if (!have || !na_can_read(na, d, &m, mfm) || na->x_head >= m.sides) {
        na->x_st1 = NEW_AGE_ST1_MA;
        na_arm(na, NA_EV_XFER, d, na_search_ns(&m, track, mfm));
        return;
    }
    if (base == 0x02) {
        // Read A Track reads sectors in the order they pass and so cannot
        // read interleaved media; every Mac format is interleaved, so it
        // ends with ND [µPD72070 §5.1.7].
        na->x_st1 = NEW_AGE_ST1_ND;
        na_arm(na, NA_EV_XFER, d, na_rev_ns(&m, track));
        return;
    }
    if (na->x_c != track) {
        // Headers name another cylinder: not found, wrong cylinder.
        na->x_st1 = NEW_AGE_ST1_ND;
        na->x_st2 = NEW_AGE_ST2_NC;
        na_arm(na, NA_EV_XFER, d, na_search_ns(&m, track, mfm));
        return;
    }
    int idx = (int)na->x_r - na_first_sector(mfm);
    if (idx < 0 || idx >= floppy_media_spt(&m, track)) {
        na->x_st1 = NEW_AGE_ST1_ND;
        na_arm(na, NA_EV_XFER, d, na_search_ns(&m, track, mfm));
        return;
    }
    // The first sector's slot has to come round and pass under the head.
    na_arm(na, NA_EV_XFER, d, na_until_slot_end(na, &m, track, idx));
}

// One sector of Read Data: the sector has just passed under the head.
static void na_step_read(new_age_t *na, const floppy_media_t *m, int track, bool mfm) {
    int idx = (int)na->x_r - na_first_sector(mfm);
    uint8_t buf[FLOPPY_SECTOR_BYTES];
    if (!floppy_media_read_sector(m, track, na->x_head, idx, buf)) {
        // A sector the medium holds whose data cannot be read is a data
        // field CRC error (ST1 DE, ST2 DD [µPD765 family status registers]),
        // what a damaged sector gives; one the medium does not hold is not
        // found.
        bool held = idx >= 0 && idx < floppy_media_spt(m, track) && na->x_head < m->sides &&
                    floppy_media_sector_offset(m, track, na->x_head, idx) + FLOPPY_SECTOR_BYTES <= disk_size(m->img);
        if (held)
            na_fail(na, NEW_AGE_ST1_DE, NEW_AGE_ST2_DD);
        else
            na_fail(na, NEW_AGE_ST1_ND, 0);
        return;
    }
    for (int i = 0; i < FLOPPY_SECTOR_BYTES; i++) {
        int r = na->be.dma_put ? na->be.dma_put(na->be.ctx, buf[i]) : NEW_AGE_DMA_NONE;
        if (r == NEW_AGE_DMA_NONE) {
            na_fail(na, NEW_AGE_ST1_OR, 0); // the FIFO overran
            return;
        }
        if (r == NEW_AGE_DMA_TC) {
            na->x_tc = 1; // the rest of the field is read and checked, not sent
            break;
        }
    }
    LOG(4, "read C%d H%d R%d", track, na->x_head, na->x_r);
    na->x_count++;
}

// One sector of Write Data.  Terminal count mid-field zero-fills the rest
// [µPD72070 §4.8.6].
static void na_step_write(new_age_t *na, const floppy_media_t *m, int track, bool mfm) {
    int idx = (int)na->x_r - na_first_sector(mfm);
    uint8_t buf[FLOPPY_SECTOR_BYTES];
    memset(buf, 0, sizeof buf);
    for (int i = 0; i < FLOPPY_SECTOR_BYTES; i++) {
        int r = na->be.dma_get ? na->be.dma_get(na->be.ctx, &buf[i]) : NEW_AGE_DMA_NONE;
        if (r == NEW_AGE_DMA_NONE) {
            na_fail(na, NEW_AGE_ST1_OR, 0); // the FIFO underran
            return;
        }
        if (r == NEW_AGE_DMA_TC) {
            na->x_tc = 1;
            break;
        }
    }
    if (!floppy_media_write_sector(m, track, na->x_head, idx, buf)) {
        na_fail(na, NEW_AGE_ST1_ND, 0);
        return;
    }
    LOG(4, "write C%d H%d R%d", track, na->x_head, na->x_r);
    na->x_count++;
}

// The format the track's command lays down: the GCR format byte names the
// recording ($12 400K one-sided, $22/$24 800K) and in MFM the sector count
// does (9 = 720K, 18 = 1.44M).
static bool na_format_of(const new_age_t *na, bool mfm, floppy_format_t *out) {
    if (mfm) {
        if (na->x_sc == 9)
            *out = FLOPPY_FMT_MFM_720K;
        else if (na->x_sc == 18)
            *out = FLOPPY_FMT_MFM_1440K;
        else
            return false;
        return true;
    }
    *out = (na->x_n & 0xF0) == 0x10 ? FLOPPY_FMT_GCR_400K : FLOPPY_FMT_GCR_800K;
    return true;
}

// Format A Track / Format/Write: one revolution has passed.  Each sector
// costs four DMA bytes (C, H, R, N) [µPD72070 §4.7], Format/Write a further
// 512 of data [µPD72070 §4.7.2]; Format A Track fills the data field with
// the filler byte.  Sector numbers outside the track — Apple's <LW7> erase
// pass sends $3F for every one — lay down nothing readable.
static void na_step_format(new_age_t *na, int track, bool mfm) {
    int d = na->x_drive;
    bool with_data = na_op_base(na->x_op) == 0x01;
    floppy_format_t fmt;
    floppy_media_t m;
    bool known = na_format_of(na, mfm, &fmt);
    if (known && na_media(na, d, &m) && (fmt == FLOPPY_FMT_MFM_1440K) == m.hd)
        floppy_media_set_format(na->fd, (unsigned)d, fmt); // what the medium carries from now on
    else
        known = false; // a recording this medium cannot take: nothing readable results
    na_media(na, d, &m);

    uint8_t data[FLOPPY_SECTOR_BYTES];
    uint8_t filler = with_data ? 0 : na->cmd[5];
    for (int s = 0; s < na->x_sc; s++) {
        uint8_t id[4];
        for (int i = 0; i < 4; i++) {
            int r = na->be.dma_get ? na->be.dma_get(na->be.ctx, &id[i]) : NEW_AGE_DMA_NONE;
            if (r == NEW_AGE_DMA_NONE) {
                na_fail(na, NEW_AGE_ST1_OR, 0);
                return;
            }
        }
        memset(data, filler, sizeof data);
        if (with_data) {
            for (int i = 0; i < FLOPPY_SECTOR_BYTES; i++) {
                if (!na->be.dma_get || na->be.dma_get(na->be.ctx, &data[i]) == NEW_AGE_DMA_NONE) {
                    na_fail(na, NEW_AGE_ST1_OR, 0);
                    return;
                }
            }
        }
        na->x_c = id[0];
        na->x_h = id[1];
        na->x_r = id[2];
        int idx = (int)id[2] - na_first_sector(mfm);
        if (known && id[0] == track)
            floppy_media_write_sector(&m, track, na->x_head, idx, data); // out-of-track numbers fail quietly
    }
    LOG(3, "format C%d H%d: %u sectors, format byte $%02X", track, na->x_head, na->x_sc, na->x_n);
    na_finish(na);
}

// The executing data command's next step.
static void na_xfer_step(new_age_t *na) {
    uint8_t base = na_op_base(na->x_op);
    bool mfm = na_op_mfm(na->x_op);
    int d = na->x_drive;

    // Anything decided at the start (not ready, no mark, wrong cylinder,
    // write protect, Read A Track's ND) is reported now.
    if (na->x_st0 & (NEW_AGE_ST0_NR | NEW_AGE_ST0_EC) || na->x_st1 || na->x_st2) {
        na_fail(na, 0, 0);
        return;
    }
    floppy_media_t m;
    if (!na_ready(na, d) || !na_media(na, d, &m)) {
        // The medium left, or the spindle stopped, mid-command.
        na->x_st0 |= NEW_AGE_ST0_NR;
        na_fail(na, 0, 0);
        return;
    }
    int track = floppy_drive_track(na->fd, (unsigned)d);

    if (base == 0x0A) { // Read ID: the header just passed
        na->x_c = (uint8_t)track;
        na->x_h = na->x_head;
        na->x_n = m.fmt_byte;
        LOG(4, "read ID C%u H%u R%u N$%02X", na->x_c, na->x_h, na->x_r, na->x_n);
        na_finish(na);
        return;
    }
    if (base == 0x0D || base == 0x01) {
        na_step_format(na, track, mfm);
        return;
    }

    if (base == 0x06)
        na_step_read(na, &m, track, mfm);
    else
        na_step_write(na, &m, track, mfm);
    if (na->phase != NA_EXEC)
        return; // the step failed and has already reported

    int last = na->x_r;
    if (na->x_tc) {
        na_update_id(na, last, mfm); // terminal count: normal termination
        na_finish(na);
        return;
    }
    if (last >= na->x_eot) {
        // The range ran out with the channel still asking for bytes: "If TC
        // is not used ... Abnormal Termination will be set (ST0 = 40) and End
        // of Cylinder will be set (ST1 = 80)" [µPD72070 §2.3].
        na_update_id(na, last, mfm);
        na_fail(na, NEW_AGE_ST1_EN, 0);
        return;
    }
    // The next sector follows directly (no interleave on the model's track).
    na->x_r = (uint8_t)(last + 1);
    int idx = (int)na->x_r - na_first_sector(mfm);
    if (idx >= floppy_media_spt(&m, track)) {
        na_fail(na, NEW_AGE_ST1_ND, 0);
        return;
    }
    na_arm(na, NA_EV_XFER, d, na_slot_ns(&m, track));
}

static void na_event(void *source, uint64_t data) {
    new_age_t *na = (new_age_t *)source;
    int kind = (int)(data >> 8);
    int d = (int)(data & 0xFF) & 1;
    switch (kind) {
    case NA_EV_XFER:
        if (na->phase == NA_EXEC)
            na_xfer_step(na);
        return;
    case NA_EV_SEEK: {
        na->seeking[d] = 0;
        uint8_t st0 = (uint8_t)(NEW_AGE_ST0_SE | d);
        if (!na_ready(na, d))
            st0 |= NEW_AGE_ST0_ABNORMAL | NEW_AGE_ST0_EC | NEW_AGE_ST0_NR;
        LOG(3, "drive %d seek end: PCN %u%s", d, na->pcn[d], (st0 & NEW_AGE_ST0_ABNORMAL) ? " (not ready)" : "");
        na_interrupt(na, st0, true, na->pcn[d]);
        return;
    }
    case NA_EV_MOTOR:
        if (na_image(na, d) && floppy_drive_motor_on(na->fd, (unsigned)d)) {
            na->motor_ready[d] = 1;
            na_interrupt(na, (uint8_t)d, false, 0);
        } else {
            // /Ready never came: abort with EC [µPD72070 §4.4.3].
            na_interrupt(na, (uint8_t)(NEW_AGE_ST0_ABNORMAL | NEW_AGE_ST0_EC | NEW_AGE_ST0_NR | d), false, 0);
        }
        return;
    case NA_EV_MODE:
        if (na_drive_exists(na, d))
            na_interrupt(na, (uint8_t)d, false, 0);
        else
            na_interrupt(na, (uint8_t)(NEW_AGE_ST0_ABNORMAL | NEW_AGE_ST0_EC | d), false, 0);
        return;
    case NA_EV_EJECT:
        na->eject_pending[d] = 0;
        na->motor_ready[d] = 0;
        if (na->fd && floppy_drive_eject(na->fd, (unsigned)d))
            LOG(1, "drive %d: disk ejected", d);
        return;
    default:
        return;
    }
}

// === The /CSTIN poller ======================================================

// The idle loop's drive scan: compare each drive's /CSTIN with the level
// last reported, and on a change raise INT with IC = %11 [µPD72070 §3.1.9]
// and hold off for 100 ms.  Suspended while any drive is enabled — "the
// FDC can not poll the status for the two FDDs while one ENBL_B pin is
// active" [µPD72070 §4.4.2] — and while an interrupt waits to be collected.
static void na_poll_event(void *source, uint64_t data) {
    (void)data;
    new_age_t *na = (new_age_t *)source;
    if (na->phase != NA_IDLE || na->int_pending || na->enabled || !na->fd)
        return;
    if (scheduler_time_ns(na->sched) < na->poll_quiet_ns)
        return;
    int n = na_drive_count(na);
    for (int d = 0; d < n && d < 2; d++) {
        bool present = floppy_drive_image(na->fd, (unsigned)d) != NULL;
        if (present == (na->cstin_seen[d] != 0))
            continue;
        na->cstin_seen[d] = present ? 1 : 0;
        LOG(2, "drive %d: /CSTIN change, medium %s", d, present ? "inserted" : "removed");
        na_interrupt(na, (uint8_t)(NEW_AGE_ST0_CSTIN | (present ? 0 : NEW_AGE_ST0_FIN) | d), false, 0);
        na->poll_quiet_ns = scheduler_time_ns(na->sched) + NA_POLL_HOLD_NS;
        return;
    }
}

// === Command processor ======================================================

// Execute a completed command.
static void na_execute(new_age_t *na) {
    uint8_t op = na->cmd[0];
    int d = na->cmd_len > 1 ? (na->cmd[1] & 0x03) : 0;
    int dd = d & 1; // the per-drive latches: Apple mode has two drive enables
    LOG(3, "command $%02X len=%d", op, na->cmd_len);

    switch (op) {
    case 0x08: // Sense Interrupt Status — returns the pending cause
        if (na->int_pending) {
            // HD reads 0 here [µPD72070 §3.1.9]; a seek-family end adds PCN.
            na->result[0] = (uint8_t)(na->int_st0 & ~NEW_AGE_ST0_HD);
            int len = 1;
            if (na->int_seek_end) {
                na->result[1] = na->int_pcn;
                len = 2;
            }
            na->int_pending = 0;
            na->int_seek_end = 0;
            na_enter_result(na, len, false);
        } else {
            na->result[0] = NEW_AGE_ST0_INVALID; // nothing pending
            na_enter_result(na, 1, false);
        }
        return;
    case 0x04: // Sense Drive Status → ST3; does NOT interrupt [§5.1.1]
        na->result[0] = na_st3(na, d);
        LOG(3, "drive %d ST3=$%02X", d, na->result[0]);
        na_enter_result(na, 1, false);
        return;
    case 0x20: // Revision: firmware rev, hardware rev
        na->result[0] = 0x01;
        na->result[1] = 0x01;
        na_enter_result(na, 2, false);
        return;
    case 0x13: // Configure (FIFO and implied-seek setup; nothing to model)
        na->phase = NA_IDLE;
        return;
    case 0x03: // Specify: only ND matters in Apple mode [§4.4.10]
        na->non_dma = na->cmd[2] & 1;
        na->phase = NA_IDLE;
        return;
    case 0x32: // Select Drive Type (latches: the driver issues it once per reset)
        na->drive_type = na->cmd[1] & 3;
        na->phase = NA_IDLE;
        return;
    case 0x12: // Perpendicular Mode
        na->perp = na->cmd[1] & 3;
        na->phase = NA_IDLE;
        return;

    case 0x07: // Recalibrate
    case 0x0F: { // Seek — fully handshaked, no step rate [§5.1.7]
        na->phase = NA_IDLE;
        int target = op == 0x07 ? 0 : na->cmd[2];
        if (target >= FLOPPY_NUM_TRACKS)
            target = FLOPPY_NUM_TRACKS - 1;
        int steps = 0;
        if (na_ready(na, dd)) {
            int cur = floppy_drive_track(na->fd, (unsigned)dd);
            steps = target - cur;
            if (steps != 0)
                floppy_mech_step(na->fd, (unsigned)dd, steps < 0, steps < 0 ? -steps : steps);
            na->pcn[dd] = (uint8_t)floppy_drive_track(na->fd, (unsigned)dd);
            if (steps < 0)
                steps = -steps;
        }
        na->seeking[dd] = 1;
        LOG(3, "drive %d %s to %d (%d steps)", dd, op == 0x07 ? "recalibrate" : "seek", target, steps);
        remove_event_by_data(na->sched, na_event, na, (uint64_t)((NA_EV_SEEK << 8) | dd));
        na_arm(na, NA_EV_SEEK, dd, steps ? (double)steps * NA_STEP_NS + NA_SETTLE_NS : NA_SHORT_NS);
        return;
    }

    case 0x9B: // Set Enable Control on
    case 0x1B: // ... off — always a normal termination [§4.4.2]
        if (op & 0x80)
            na->enabled |= (uint8_t)(1u << dd);
        else
            na->enabled &= (uint8_t) ~(1u << dd);
        na->phase = NA_IDLE;
        na_interrupt(na, (uint8_t)d, false, 0);
        return;
    case 0x9A: // Set Motor Control on: the /Ready handshake [§4.4.3]
        na->phase = NA_IDLE;
        if (na_drive_exists(na, dd))
            floppy_mech_set_motor(na->fd, (unsigned)dd, true);
        remove_event_by_data(na->sched, na_event, na, (uint64_t)((NA_EV_MOTOR << 8) | dd));
        na_arm(na, NA_EV_MOTOR, dd, na->motor_ready[dd] ? NA_SHORT_NS : NA_MOTOR_ON_NS);
        return;
    case 0x1A: // ... off
        na->phase = NA_IDLE;
        if (na_drive_exists(na, dd))
            floppy_mech_set_motor(na->fd, (unsigned)dd, false);
        na->motor_ready[dd] = 0;
        remove_event_by_data(na->sched, na_event, na, (uint64_t)((NA_EV_MOTOR << 8) | dd));
        na_interrupt(na, (uint8_t)d, false, 0);
        return;
    case 0x1C: // Set Drive Mode GCR
    case 0x5C: // ... MFM — reconfigures the DRIVE [§4.4.1]
        na->phase = NA_IDLE;
        na->drive_mfm[dd] = op == 0x5C;
        remove_event_by_data(na->sched, na_event, na, (uint64_t)((NA_EV_MODE << 8) | dd));
        na_arm(na, NA_EV_MODE, dd, NA_MODE_NS);
        return;
    case 0x52: // Eject: an immediate normal termination, the medium later
        na->phase = NA_IDLE;
        if (na_image(na, dd) && !na->eject_pending[dd]) {
            na->eject_pending[dd] = 1;
            floppy_mech_set_motor(na->fd, (unsigned)dd, false);
            na->motor_ready[dd] = 0;
            na_arm(na, NA_EV_EJECT, dd, NA_EJECT_NS);
        }
        na_interrupt(na, (uint8_t)d, false, 0);
        return;
    case 0x0B: // Disable/Enable DPLL — the drive's own data separator
    case 0x8B:
        na->phase = NA_IDLE;
        na_interrupt(na, (uint8_t)d, false, 0);
        return;

    default:
        na_start_data(na); // the data commands (na_cmd_len accepted it)
        return;
    }
}

// === Register handlers ======================================================

static uint8_t na_msr(const new_age_t *na) {
    uint8_t msr = 0;
    switch (na->phase) {
    case NA_IDLE:
        msr = NEW_AGE_MSR_RQM;
        break;
    case NA_COMMAND:
        msr = NEW_AGE_MSR_RQM | NEW_AGE_MSR_CB;
        break;
    case NA_EXEC:
        msr = NEW_AGE_MSR_CB; // RQM low until the result phase
        break;
    case NA_RESULT:
        msr = NEW_AGE_MSR_RQM | NEW_AGE_MSR_DIO | NEW_AGE_MSR_CB;
        break;
    }
    int n = na_drive_count(na);
    if (n < 1)
        msr |= NEW_AGE_MSR_D0I;
    if (n < 2)
        msr |= NEW_AGE_MSR_D1I;
    // D*B: a seek in flight, or its end not yet collected [§3.1.2].
    for (int d = 0; d < 2; d++) {
        bool pending_end = na->int_pending && na->int_seek_end && (na->int_st0 & 1) == d;
        if (na->seeking[d] || pending_end)
            msr |= (uint8_t)(d ? NEW_AGE_MSR_D1B : NEW_AGE_MSR_D0B);
    }
    return msr;
}

static uint8_t na_read_access(new_age_t *na, unsigned reg, bool peek) {
    if (reg == NEW_AGE_REG_STATUS)
        return na_msr(na);
    // The data register: one result byte per read.
    if (na->phase != NA_RESULT)
        return 0xFF;
    uint8_t v = na->result[na->result_idx];
    if (peek)
        return v;
    if (na->result_idx == 0)
        na_set_int(na, false); // reading the result deasserts INT
    if (na->result_idx + 1 < na->result_len) {
        na->result_idx++;
    } else {
        na->phase = NA_IDLE; // the last byte: back to idle
        na->poll_quiet_ns = fmax(na->poll_quiet_ns, scheduler_time_ns(na->sched) + NA_POLL_QUIET_NS);
    }
    return v;
}

uint8_t new_age_read(new_age_t *na, unsigned reg) {
    return na_read_access(na, reg, false);
}

uint8_t new_age_peek(new_age_t *na, unsigned reg) {
    return na_read_access(na, reg, true);
}

void new_age_write(new_age_t *na, unsigned reg, uint8_t value) {
    if (reg == NEW_AGE_REG_STATUS) { // DRR
        if (value & 0x80) {
            // S/W RST, self-clearing [§3.1.3]: the chip reinitialises and
            // the rest of the byte is what the register then holds.
            LOG(2, "DRR software reset ($%02X)", value);
            new_age_reset(na);
        }
        na->drr = (uint8_t)(value & 0x7F);
        return;
    }
    // The data register: one command byte.
    if (na->phase == NA_RESULT || na->phase == NA_EXEC)
        return; // the FIFO is not taking command bytes
    if (na->sched)
        na->poll_quiet_ns = fmax(na->poll_quiet_ns, scheduler_time_ns(na->sched) + NA_POLL_QUIET_NS);
    if (na->phase == NA_IDLE) {
        na_set_int(na, false); // "Reset INT, Set CB" on command arrival
        na->phase = NA_COMMAND;
        na->cmd_len = 0;
        na->cmd_expect = (uint8_t)na_cmd_len(value);
        if (na->cmd_expect == 0) {
            // An undefined or Apple-mode-illegal opcode: straight to a
            // one-byte result, ST0 = $80 [§3.3.2].
            LOG(2, "invalid command $%02X", value);
            na->cmd[0] = value;
            na->result[0] = NEW_AGE_ST0_INVALID;
            na_enter_result(na, 1, true);
            return;
        }
    }
    if (na->cmd_len < sizeof(na->cmd))
        na->cmd[na->cmd_len++] = value;
    if (na->cmd_len >= na->cmd_expect)
        na_execute(na);
}

// === Lifecycle ==============================================================

void new_age_bind(new_age_t *na, struct floppy *fd, struct scheduler *sched, const new_age_backend_t *be) {
    na->fd = fd;
    na->sched = sched;
    if (be)
        na->be = *be;
    else
        memset(&na->be, 0, sizeof(na->be));
}

void new_age_register_events(new_age_t *na, bool restoring) {
    scheduler_new_event_type(na->sched, "new_age", na, "engine", na_event);
    scheduler_new_event_type(na->sched, "new_age", na, "poll", na_poll_event);
    if (!restoring)
        scheduler_new_cpu_event(na->sched, na_poll_event, na, 0, 0, (uint64_t)NA_POLL_NS, true);
}

void new_age_reset(new_age_t *na) {
    // A command in flight dies with the reset; the drive's own timers
    // (spin-up, mode change, an eject already strobed) keep running.
    if (na->sched)
        remove_event_by_data(na->sched, na_event, na, (uint64_t)(NA_EV_XFER << 8) | na->x_drive);
    na->phase = NA_IDLE;
    na->cmd_len = na->cmd_expect = 0;
    na->result_len = na->result_idx = 0;
    na->int_pending = 0;
    na->int_seek_end = 0;
    na->drr = 0x02; // 250 kbps, the reset default [§3.1.3]
    na->drive_type = 0; // conventional FDD
    na->perp = 0;
    na->non_dma = 0;
    na->enabled = 0; // the ENBL pins float inactive
    // The firmware starts over at "Initialize FDC": the poller's memory of
    // each drive's /CSTIN is gone, so a medium already in place is reported
    // as an insertion once polling resumes.  This is what tells the driver
    // about a disk that was in the drive before it opened.
    na->cstin_seen[0] = na->cstin_seen[1] = 0;
    na_set_int(na, false);
}
