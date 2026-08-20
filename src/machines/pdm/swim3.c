// SWIM3 floppy controller — PDM island $50F16000, 16 byte-wide registers
// at stride $200 (index = offset >> 9).  This file is the register file,
// the interrupt contract, and the Sony drive-register protocol (sense
// reads and LSTRB strobes); the media transfer engine that reads, writes
// and formats sectors through the AMIC DMA channel is swim3_xfer.c, and
// the drive itself — head position, motor, media, the object tree — is
// the shared floppy module (cfg->floppy, FLOPPY_TYPE_SWIM3).
//
// The split follows the chip: SWIM3's registers each have tight, testable
// semantics (readback, read-to-clear, write-1s-to-set/clear ports), while
// the engine is a state machine with its own tables.  Keeping them apart
// preserves the property that made the register file easy to verify.
//
// Contract references: the register map and its semantics follow Apple's
// SWIM3 Engineering Requirements Specification v1.2 (3/24/93); the Sony
// drive-register protocol follows Apple, "Guide to the Macintosh Family
// Hardware", 2nd ed., and Apple, "Power Macintosh Computers" Developer
// Note (1994), Table 3-7.  What the ROM's .Sony driver expects of the chip
// was established by tracing the shipping driver's own register accesses
// on this emulator (`debug.log swim3 5`), not from its source.

#include "pdm.h"

#include "floppy.h"
#include "log.h"

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
#define H_RDDATA      0x04u // live drive RdData — the sense-read output
#define H_ERROR       0x20u // the error register is non-zero

// The internal drive is always drive 1; PDM has no second drive.
#define FD 0u

// True while the mode register selects the internal drive.  Drive 2 is
// probed by enabling it alone, and must sense absent at every address.
static bool drive1_selected(pdm_swim3_t *sw) {
    return !((sw->mode & SWIM3_M_DRIVE2) && !(sw->mode & SWIM3_M_DRIVE1));
}

// The {SEL,CA2,CA1,CA0} drive-register address currently addressed: SEL is
// mode bit 5 (HeadSelect), CA0-2 are Phase bits 0-2 (§5).
static uint32_t drive_addr(pdm_swim3_t *sw) {
    return ((sw->mode & SWIM3_M_HEADSEL) ? 8u : 0u) | (sw->phase & 7u);
}

// The drive's sense response for the currently addressed register (§5.2).
// Reading a sense address also ROUTES a head: addresses 4 / 12 select head
// 0 / 1 for data transfer, and during a GCR format the driver uses 1 / 15
// instead, because those read back as 1 while it writes from the index.
static uint8_t drive_sense(config_t *cfg, pdm_swim3_t *sw) {
    if (!drive1_selected(sw))
        return 1; // no second drive: RdData floats high at every address

    uint32_t addr = drive_addr(sw);
    floppy_t *fd = cfg->floppy;
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
        return (uint8_t)pdm_swim3_index_pulse(cfg);
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
        return pdm_swim3_media_is_hd(cfg) ? 0 : 1;
    default:
        return 0;
    }
}

// A control strobe: LSTRB (phase bit 3) rose with address {SEL,CA2,CA1,CA0}
// (§5.1).  CA2 carries the on/off data bit, so each function has an "on"
// and an "off" address four apart.
static void drive_strobe(config_t *cfg, pdm_swim3_t *sw) {
    uint32_t addr = drive_addr(sw);
    floppy_t *fd = cfg->floppy;
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

void pdm_swim3_update_irq(config_t *cfg) {
    pdm_swim3_t *sw = &pdm_st(cfg)->amic.swim3;
    bool level = (sw->mode & SWIM3_M_ENABLE_INTS) && (sw->intr & sw->intmask);
    pdm_amic_set_fdc_irq(cfg, level);
}

void pdm_swim3_raise(config_t *cfg, uint8_t bits) {
    pdm_swim3_t *sw = &pdm_st(cfg)->amic.swim3;
    sw->intr |= bits; // a source sets its flag regardless of the mask
    LOG(4, "interrupt $%02X (intr=$%02X mask=$%02X)", bits, sw->intr, sw->intmask);
    pdm_swim3_update_irq(cfg);
}

// === Register file ==========================================================

static uint8_t swim3_read_reg(config_t *cfg, pdm_swim3_t *sw, uint32_t off) {
    uint8_t v;
    switch (off >> 9) {
    case R_TIMER:
        return sw->timer;
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
        v = drive_sense(cfg, sw) ? H_RDDATA : 0;
        if ((sw->mode & SWIM3_M_ENABLE_INTS) && (sw->intr & sw->intmask))
            v |= H_INT_PENDING;
        if (sw->error)
            v |= H_ERROR;
        return v;
    case R_INTR: // read-to-clear — and the IRQ line drops with it
        v = sw->intr;
        sw->intr = 0;
        pdm_swim3_update_irq(cfg);
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

uint8_t pdm_swim3_read(config_t *cfg, uint32_t off) {
    pdm_swim3_t *sw = &pdm_st(cfg)->amic.swim3;
    uint8_t v = swim3_read_reg(cfg, sw, off);
    LOG(5, "rd +$%04X %-9s = $%02X", off, REG_RD_NAMES[(off >> 9) & 15], v);
    return v;
}

void pdm_swim3_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_swim3_t *sw = &pdm_st(cfg)->amic.swim3;
    LOG(5, "wr +$%04X %-9s = $%02X", off, REG_WR_NAMES[(off >> 9) & 15], value);
    switch (off >> 9) {
    case R_TIMER:
        sw->timer = value;
        break;
    case R_PARAM:
        sw->param = value;
        break;
    case R_PHASE: {
        // LSTRB is bit 3; a rising edge strobes the addressed drive latch.
        uint8_t rose = (uint8_t)(value & ~sw->phase & 0x08u);
        sw->phase = value;
        if (rose)
            drive_strobe(cfg, sw);
        break;
    }
    case R_SETUP:
        if (value & 0x80u) {
            // SoftReset (self-clearing): registers return to their reset
            // state (§3.10) and any running engine stops with them.
            pdm_swim3_t z = {0};
            z.ctrack = 0xFF;
            z.csect = 0x7F;
            z.sector = 0xFF;
            *sw = z;
            pdm_swim3_engine_update(cfg);
            pdm_swim3_update_irq(cfg);
        } else {
            sw->setup = value;
        }
        break;
    case R_ZEROES: // clear the 1-bits in mode
        sw->mode &= (uint8_t)~value;
        pdm_swim3_engine_update(cfg);
        pdm_swim3_update_irq(cfg);
        break;
    case R_ONES: // set the 1-bits in mode
        sw->mode |= value;
        pdm_swim3_engine_update(cfg);
        pdm_swim3_update_irq(cfg);
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
        break;
    case R_NSECT:
        sw->nsect = value;
        break;
    case R_INTMASK:
        sw->intmask = value;
        pdm_swim3_update_irq(cfg);
        break;
    default: // R_DATA / R_ERROR / R_INTR: not writable in this model
        LOG(2, "write to reg %u = $%02X ignored", (unsigned)(off >> 9), value);
        break;
    }
}
