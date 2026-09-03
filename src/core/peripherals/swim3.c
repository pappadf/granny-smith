// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// SWIM3 floppy controller — sixteen byte-wide registers, addressed here by
// INDEX (the PDM decodes them on $200 centres at $50F16000, Grand Central
// on $10 centres at +$15000; each board translates).  This file is the register file,
// the interrupt contract, and the Sony drive-register protocol (sense
// reads and LSTRB strobes); the media transfer engine that reads, writes
// and formats sectors through the AMIC DMA channel is swim3_xfer.c, and
// the drive itself — head position, motor, media, the object tree — is
// the shared floppy module (sw->fd, FLOPPY_TYPE_SWIM3).
//
// The split follows the chip: SWIM3's registers each have tight, testable
// semantics (readback, read-to-clear, write-1s-to-set/clear ports), while
// the engine is a state machine with its own tables.  Keeping them apart
// preserves the property that made the register file easy to verify.
// What differs per board — the DMA byte movers and the interrupt sink —
// comes in through swim3_backend_t (swim3.h).
//
// Contract references: the register map and its semantics follow Apple's
// SWIM3 Engineering Requirements Specification v1.2 (3/24/93); the Sony
// drive-register protocol follows Apple, "Guide to the Macintosh Family
// Hardware", 2nd ed., and Apple, "Power Macintosh Computers" Developer
// Note (1994), Table 3-7.  What the ROM's .Sony driver expects of the chip
// was established by tracing the shipping driver's own register accesses
// on this emulator (`debug.log swim3 5`), not from its source.

#include "swim3.h"
#include "floppy.h"

#include "floppy.h"
#include "log.h"
#include "scheduler.h"

LOG_USE_CATEGORY_NAME("swim3");

// Register indices (offset >> 9)
#define R_DATA    0
#define R_TIMER   1
#define R_ERROR   2
#define R_PARAM   3
#define R_PHASE   4
#define R_SETUP   5
#define R_ZEROES  6 // write: clear mode bits;  read: mode
#define R_ONES    7 // write: set mode bits;    read: handshake
#define R_INTR    8 // read-to-clear
#define R_STEP    9
#define R_CTRACK  10
#define R_CSECT   11
#define R_GAP     12 // write: gap3;  read: the header's format byte
#define R_SECTOR  13
#define R_NSECT   14
#define R_INTMASK 15

// Register names for the `swim3` trace category, in Apple's own spellings,
// so a captured log reads against the specification.  Three indices read
// and write different things, hence two tables.
static const char *const REG_RD_NAMES[16] = {"Data",    "Timer",     "Error", "Param",  "Phase",    "Setup",
                                             "Mode",    "Hdshk",     "Intr",  "Step",   "CurTrack", "CurSect",
                                             "FmtByte", "FirstSect", "NSect", "IntMask"};
static const char *const REG_WR_NAMES[16] = {"Data",   "Timer",     "Error", "Param",  "Phase",    "Setup",
                                             "Zeroes", "Ones",      "Intr",  "Step",   "CurTrack", "CurSect",
                                             "Gap",    "FirstSect", "NSect", "IntMask"};

// Handshake bits (§3.3) — the three this model drives
#define H_INT_PENDING 0x02u // an enabled interrupt is pending
#define H_RDDATA      0x04u // bit 2 RDData: "direct read of drive data"
#define H_SENSE       0x08u // bit 3 Sense: "direct read of rddata input"
#define H_ERROR       0x20u // the error register is non-zero

// The internal drive is always drive 1; PDM has no second drive.
#define FD 0u

// True while the mode register selects the internal drive.  Drive 2 is
// probed by enabling it alone, and must sense absent at every address.
static bool drive1_selected(swim3_t *sw) {
    return !((sw->mode & SWIM3_M_DRIVE2) && !(sw->mode & SWIM3_M_DRIVE1));
}

// The {SEL,CA2,CA1,CA0} drive-register address currently addressed: SEL is
// mode bit 5 (HeadSelect), CA0-2 are Phase bits 0-2 (§5).
static uint32_t drive_addr(swim3_t *sw) {
    return ((sw->mode & SWIM3_M_HEADSEL) ? 8u : 0u) | (sw->phase & 7u);
}

// The drive's sense response for the currently addressed register (§5.2).
// Reading a sense address also ROUTES a head: addresses 4 / 12 select head
// 0 / 1 for data transfer, and during a GCR format the driver uses 1 / 15
// instead, because those read back as 1 while it writes from the index.
static uint8_t drive_sense(swim3_t *sw) {
    if (!drive1_selected(sw))
        return 1; // no second drive: RdData floats high at every address

    uint32_t addr = drive_addr(sw);
    floppy_t *fd = sw->fd;
    image_t *disk = fd ? floppy_drive_image(fd, FD) : NULL;
    bool present = disk != NULL;

    switch (addr) {
    case 0: // rDirPrev — current step-direction latch
        return sw->step_dir;
    case 1: // rStepOff — 1 = step complete (seeks retire on their own event)
        if (sw->mode & SWIM3_M_FORMAT)
            sw->xfer_side = 0; // GCRFmtSelDecode routes head 0 here
        return 1;
    case 2: // rMotorOff
        return (fd && floppy_drive_motor_on(fd, FD)) ? 0 : 1;
    case 3: // rEjectOn — the emulated drive has no eject button
        return 0;
    case 4: // rRdData0 — route head 0
        sw->xfer_side = 0;
        return 0;
    case 5: // rMFMDrive — 1 = SuperDrive
        return 1;
    case 6: // rDoubleSided
        return 1;
    case 7: // rNoDrive — 0 = drive present
        return 0;
    case 8: // rNoDiskInPl — 1 = NO disk in place
        return present ? 0 : 1;
    case 9: // rNoWrProtect — 1 = NOT write-protected
        return (present && disk->writable) ? 1 : 0;
    case 10: // rNotTrack0 — 1 = head is not over track 0
        return (fd && floppy_drive_track(fd, FD) != 0) ? 1 : 0;
    case 11: // rNoTachPulse (GCR) / rIndexPulse (MFM)
        return (uint8_t)swim3_index_pulse(sw);
    case 12: // rRdData1 — route head 1
        sw->xfer_side = 1;
        return 0;
    case 13: // rMFMModeOn
        return sw->mfm_mode;
    case 14: // rNotReady — 0 = ready: media present and the spindle turning
        return (present && fd && floppy_drive_motor_on(fd, FD)) ? 0 : 1;
    case 15: // rNotRevised (no disk) / r1MegMedia: 1 = DD media, 0 = HD
        if (sw->mode & SWIM3_M_FORMAT)
            sw->xfer_side = 1; // GCRFmtSelDecode routes head 1 here
        return swim3_media_is_hd(sw) ? 0 : 1;
    default:
        return 0;
    }
}

// The head the drive puts on RD.  The RdData0 / RdData1 sense addresses (4
// and 12) are ALSO the head-select: whatever the SEL/CA lines address at
// transfer time is the head whose data the chip decodes, whether or not
// the driver ever reads the handshake register to look at it.  The Mac OS
// .Sony driver reads the sense (drive_sense routes it there too); Open
// Firmware's swim3 package just sets HeadSel and CA0-2 and starts the
// transfer — a model that routed only on a sense READ read side 0 for
// every side-1 block the firmware asked for.
static void route_head(swim3_t *sw) {
    uint32_t addr = drive_addr(sw);
    if (addr == 4)
        sw->xfer_side = 0;
    else if (addr == 12)
        sw->xfer_side = 1;
}

// A control strobe: LSTRB (phase bit 3) rose with address {SEL,CA2,CA1,CA0}
// (§5.1).  CA2 carries the on/off data bit, so each function has an "on"
// and an "off" address four apart.
static void drive_strobe(swim3_t *sw) {
    uint32_t addr = drive_addr(sw);
    floppy_t *fd = sw->fd;
    switch (addr) {
    case 0: // wDirNext — step inward
        sw->step_dir = 0;
        break;
    case 4: // wDirPrev — step outward
        sw->step_dir = 1;
        break;
    case 2: // wMotorOn
        sw->motor_on = 1;
        floppy_swim3_set_motor(fd, FD, true);
        break;
    case 6: // wMotorOff — the drive forgets its GCR/MFM mode (§11.12)
        sw->motor_on = 0;
        sw->mfm_mode = 0;
        floppy_swim3_set_motor(fd, FD, false);
        break;
    case 7: // wEjectOn — the mechanism takes up to 1.5 s, which the driver
        // spends waiting for rNoDiskInPl to read 1; the media leaves now.
        if (fd && floppy_drive_eject(fd, FD))
            LOG(1, "eject: media removed");
        break;
    case 9: // wMFMModeOn
        sw->mfm_mode = 1;
        break;
    case 13: // wMFMModeOff (GCR mode)
        sw->mfm_mode = 0;
        break;
    default: // wEjectOff / wDiskInPl / wNoDiskInPl: latch resets, inert here
        break;
    }
    LOG(3, "strobe addr %u (motor=%u mfm=%u)", addr, sw->motor_on, sw->mfm_mode);
}

// === Interrupts (§7.9) ======================================================

void swim3_update_irq(swim3_t *sw) {
    bool level = (sw->mode & SWIM3_M_ENABLE_INTS) && (sw->intr & sw->intmask);
    sw->be.set_irq(sw->be.ctx, level);
}

void swim3_raise(swim3_t *sw, uint8_t bits) {
    sw->intr |= bits; // a source sets its flag regardless of the mask
    LOG(4, "interrupt $%02X (intr=$%02X mask=$%02X)", bits, sw->intr, sw->intmask);
    swim3_update_irq(sw);
}

// === Timer (reg 1) ==========================================================
//
// A 1 us countdown (SWIM3-ERS:76): a loaded value decrements at 1 MHz,
// independent of ClockDiv2, and TIMER_DONE (interrupt bit 0) is generated
// when the count reaches zero.  The ERS leaves the read-back of a running
// count and write-0-to-stop unstated; both are modelled, because Copland's
// floppy plugin POLLS the running count (SwimIIISmallWait loads N+1 and
// spins until the register reads zero — measured, see
// gs-docs/projects/copland re/bsfloppypdm.dis.txt), which is only
// meaningful if the live count reads back.  The 7.5 .Sony driver never
// touches the register (it uses the Time Manager), so this path is
// exercised by Copland alone.

static void swim3_timer_event(void *source, uint64_t data) {
    (void)data;
    swim3_t *sw = (swim3_t *)source;
    sw->timer = 0;
    sw->timer_running = 0;
    swim3_raise(sw, SWIM3_INT_TIMER);
}

static void swim3_timer_stop(swim3_t *sw) {
    remove_event(sw->sched, swim3_timer_event, sw);
    sw->timer = 0;
    sw->timer_running = 0;
}

static void swim3_timer_load(swim3_t *sw, uint8_t value) {
    remove_event(sw->sched, swim3_timer_event, sw);
    sw->timer = value;
    if (value == 0) { // write-0-to-stop
        sw->timer_running = 0;
        return;
    }
    sw->timer_running = 1;
    sw->timer_start_ns = (uint64_t)scheduler_time_ns(sw->sched);
    scheduler_new_cpu_event(sw->sched, swim3_timer_event, sw, 0, 0, (uint64_t)value * 1000u);
}

static uint8_t swim3_timer_read(swim3_t *sw) {
    if (!sw->timer_running)
        return sw->timer; // 0 after done/stop/reset
    uint64_t elapsed_us = ((uint64_t)scheduler_time_ns(sw->sched) - sw->timer_start_ns) / 1000u;
    if (elapsed_us >= sw->timer)
        return 0; // the event will retire it; the count already reads 0
    return (uint8_t)(sw->timer - elapsed_us);
}

void swim3_bind(swim3_t *sw, struct floppy *fd, struct scheduler *sched, const swim3_backend_t *be) {
    sw->fd = fd;
    sw->sched = sched;
    sw->be = *be;
}

void swim3_register_events(swim3_t *sw) {
    scheduler_new_event_type(sw->sched, "swim3", sw, "timer", swim3_timer_event);
}

// === Register file ==========================================================

static uint8_t swim3_read_reg(swim3_t *sw, unsigned reg) {
    uint8_t v;
    switch (reg) {
    case R_TIMER:
        return swim3_timer_read(sw);
    case R_ERROR: // read-to-clear
        v = sw->error;
        sw->error = 0;
        return v;
    case R_PARAM:
        return sw->param;
    case R_PHASE: // probe loopback: reads back the written value
        return sw->phase;
    case R_SETUP:
        return sw->setup;
    case R_ZEROES: // read = current mode
        return sw->mode;
    case R_ONES: // read = handshake
        // The drive's RD line answers a sense address on BOTH handshake
        // bits the ERS lists for it: bit 2 (RDData) is what the Mac OS
        // .Sony driver reads, bit 3 (Sense) is what Open Firmware's swim3
        // package and Linux's driver read (`stat & DATA`, DATA = 0x08).  A
        // model that drove only bit 2 answered every Open Firmware sense
        // with 0 — "drive present, disk in" by luck, "single-sided" by the
        // same luck, and the firmware's open ended in BAD DISK.
        v = drive_sense(sw) ? (H_RDDATA | H_SENSE) : 0;
        if ((sw->mode & SWIM3_M_ENABLE_INTS) && (sw->intr & sw->intmask))
            v |= H_INT_PENDING;
        if (sw->error)
            v |= H_ERROR;
        return v;
    case R_INTR: // read-to-clear — and the IRQ line drops with it
        v = sw->intr;
        sw->intr = 0;
        swim3_update_irq(sw);
        return v;
    case R_STEP:
        return sw->step;
    case R_CTRACK:
        return sw->ctrack;
    case R_CSECT:
        return sw->csect;
    case R_GAP: // read side: the last address header's format byte
        return sw->fmt_byte;
    case R_SECTOR:
        return sw->sector;
    case R_NSECT:
        return sw->nsect;
    case R_INTMASK:
        return sw->intmask;
    default: // R_DATA: the PIO FIFO, which PDM never addresses (DMA only)
        return 0;
    }
}

uint8_t swim3_read(swim3_t *sw, unsigned reg) {
    uint8_t v = swim3_read_reg(sw, reg);
    LOG(5, "rd reg %2u %-9s = $%02X", reg, REG_RD_NAMES[reg & 15], v);
    return v;
}

void swim3_write(swim3_t *sw, unsigned reg, uint8_t value) {
    LOG(5, "wr reg %2u %-9s = $%02X", reg, REG_WR_NAMES[reg & 15], value);
    switch (reg) {
    case R_TIMER:
        swim3_timer_load(sw, value);
        break;
    case R_PARAM:
        sw->param = value;
        break;
    case R_PHASE: {
        // LSTRB is bit 3; a rising edge strobes the addressed drive latch.
        uint8_t rose = (uint8_t)(value & ~sw->phase & 0x08u);
        sw->phase = value;
        route_head(sw);
        if (rose)
            drive_strobe(sw);
        break;
    }
    case R_SETUP:
        if (value & 0x80u) {
            // SoftReset (self-clearing): registers return to their reset
            // state (§3.10) and any running engine stops with them.
            swim3_t z = {0};
            z.ctrack = 0xFF;
            z.csect = 0x7F;
            z.sector = 0xFF;
            z.fd = sw->fd;
            z.sched = sw->sched;
            z.be = sw->be;
            swim3_timer_stop(sw);
            *sw = z;
            swim3_engine_update(sw);
            swim3_update_irq(sw);
        } else {
            sw->setup = value;
        }
        break;
    case R_ZEROES: // clear the 1-bits in mode
        sw->mode &= (uint8_t)~value;
        route_head(sw);
        swim3_engine_update(sw);
        swim3_update_irq(sw);
        break;
    case R_ONES: // set the 1-bits in mode
        sw->mode |= value;
        route_head(sw);
        swim3_engine_update(sw);
        swim3_update_irq(sw);
        break;
    case R_STEP:
        sw->step = value;
        break;
    case R_CTRACK:
        sw->ctrack = value;
        break;
    case R_CSECT:
        sw->csect = value;
        break;
    case R_GAP:
        sw->gap = value;
        break;
    case R_SECTOR:
        sw->sector = value;
        sw->xfer_any = 0; // a new FirstSector: match it before continuing
        break;
    case R_NSECT:
        sw->nsect = value;
        sw->xfer_any = 0;
        break;
    case R_INTMASK:
        sw->intmask = value;
        swim3_update_irq(sw);
        break;
    default: // R_DATA / R_ERROR / R_INTR: not writable in this model
        LOG(2, "write to reg %u = $%02X ignored", reg, value);
        break;
    }
}
