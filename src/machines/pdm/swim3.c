// SWIM3 floppy controller — PDM island $50F16000, 16 byte-wide registers
// at stride $200 (index = offset >> 9).  Phase-G scope: the register file
// with the readback / read-to-clear semantics the ROM .Sony driver's
// open, probe and idle-poll paths depend on, plus the Sony-drive sense
// protocol answering "internal SuperDrive present, no disk inserted,
// drive 2 absent".  No media datapath, no DMA engine, no interrupt
// sources — those are Phase H (the driver enables EnableInts at open but
// nothing here ever raises the FDC line).
//
// Contract references: the PDM SWIM3 register map and the .Sony driver's
// expectations follow Apple's SonySWIM3.a driver source and the SWIM3 ERS
// (register set §3, drive interface §5, presence probe §7.1, no-disk idle
// behavior §8.3 of the project's SWIM3 note).

#include "pdm.h"

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
#define R_GAP     12
#define R_SECTOR  13
#define R_NSECT   14
#define R_INTMASK 15

// Mode bits
#define M_ENABLE_INTS 0x01u
#define M_DRIVE1      0x02u
#define M_DRIVE2      0x04u
#define M_HEADSEL     0x20u // the drive SEL line (sense address bit 3)

// Handshake bits
#define H_RDDATA 0x04u // live drive RdData — the sense-read output

// The drive's sense response for the current {SEL,CA2,CA1,CA0} address.
// Drive 1 is the internal SuperDrive with no disk in place; drive 2 must
// sense absent (all-ones RdData, giving kind 1111 = no drive).
static uint8_t drive_sense(pdm_swim3_t *sw) {
    if ((sw->mode & M_DRIVE2) && !(sw->mode & M_DRIVE1))
        return 1; // no second drive: RdData floats high at every address
    uint32_t addr = ((sw->mode & M_HEADSEL) ? 8u : 0u) | (sw->phase & 7u);
    switch (addr) {
    case 0: // rDirPrev — current step-direction latch
        return sw->step_dir;
    case 1: // rStepOff — 1 = step complete (idle)
        return 1;
    case 2: // rMotorOff
        return sw->motor_on ? 0 : 1;
    case 3: // rEjectOn — eject button not pressed
        return 0;
    case 5: // rMFMDrive — 1 = SuperDrive
        return 1;
    case 6: // rDoubleSided
        return 1;
    case 7: // rNoDrive — 0 = drive present
        return 0;
    case 8: // rNoDiskInPl — 1 = NO disk in place (the Phase-G answer)
        return 1;
    case 9: // rNoWrProtect — moot with no disk
        return 1;
    case 10: // rNotTrack0 — drives auto-home at power-on
        return 0;
    case 13: // rMFMModeOn
        return sw->mfm_mode;
    case 14: // rNotReady — no disk: never ready
        return 1;
    case 15: // rRevised — with 7/6/5 this senses kind x011 = SuperDrive
        return 1;
    default: // 4/11/12: RdData0/1 routing and tach — no flux, no pulses
        return 0;
    }
}

// A control strobe: LSTRB (phase bit 3) rose with address {SEL,CA2,CA1,CA0}.
// CA2 carries the on/off data bit; only the motor / mode / direction latches
// matter with no disk present.
static void drive_strobe(pdm_swim3_t *sw) {
    uint32_t addr = ((sw->mode & M_HEADSEL) ? 8u : 0u) | (sw->phase & 7u);
    switch (addr) {
    case 0: // wDirNext — step inward
        sw->step_dir = 0;
        break;
    case 4: // wDirPrev — step outward
        sw->step_dir = 1;
        break;
    case 2: // wMotorOn
        sw->motor_on = 1;
        break;
    case 6: // wMotorOff — the drive forgets its GCR/MFM mode (§11.12)
        sw->motor_on = 0;
        sw->mfm_mode = 0;
        break;
    case 9: // wMFMModeOn
        sw->mfm_mode = 1;
        break;
    case 13: // wMFMModeOff (GCR mode)
        sw->mfm_mode = 0;
        break;
    default: // eject / disk-in-place latches: inert with no disk
        break;
    }
    LOG(3, "strobe addr %u (motor=%u mfm=%u)", addr, sw->motor_on, sw->mfm_mode);
}

uint8_t pdm_swim3_read(config_t *cfg, uint32_t off) {
    pdm_swim3_t *sw = &pdm_st(cfg)->amic.swim3;
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
    case R_ONES: // read = handshake: only the RdData sense line is live here
        return drive_sense(sw) ? H_RDDATA : 0;
    case R_INTR: // read-to-clear
        v = sw->intr;
        sw->intr = 0;
        return v;
    case R_STEP:
        return sw->step;
    case R_CTRACK:
        return sw->ctrack;
    case R_CSECT:
        return sw->csect;
    case R_GAP:
        return sw->gap;
    case R_SECTOR:
        return sw->sector;
    case R_NSECT:
        return sw->nsect;
    case R_INTMASK:
        return sw->intmask;
    default: // R_DATA: PIO FIFO unused on PDM (DMA machine is Phase H)
        return 0;
    }
}

void pdm_swim3_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_swim3_t *sw = &pdm_st(cfg)->amic.swim3;
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
            drive_strobe(sw);
        break;
    }
    case R_SETUP:
        if (value & 0x80u) {
            // SoftReset (self-clearing): registers return to reset state.
            pdm_swim3_t z = {0};
            *sw = z;
        } else {
            sw->setup = value;
        }
        break;
    case R_ZEROES: // clear the 1-bits in mode
        sw->mode &= (uint8_t)~value;
        break;
    case R_ONES: // set the 1-bits in mode
        sw->mode |= value;
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
        break;
    default: // R_DATA / R_ERROR / R_INTR: not writable in this model
        LOG(2, "write to reg %u = $%02X ignored", (unsigned)(off >> 9), value);
        break;
    }
}
