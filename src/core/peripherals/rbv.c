// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rbv.c
// RBV ("RAM-Based Video") chip for the Macintosh IIci.  See rbv.h for the
// public contract and HardwarePrivateEqu.a (lines 816-916) for the
// authoritative register/bit layout.
//
// The RBV register file is eight 8-bit registers reachable at small byte
// offsets from the chip base ($50F26000):
//
//   $000 RvDataB   control bits (cache/power/sound/parity)   — VIA2 buf-B equiv
//   $001 RvExp     expansion register                        — accept-and-log
//   $002 RvSInt    slot-interrupt status   (RvIRQ0..6)
//   $003 RvIFR     interrupt flag register (RvSCSIDRQ/AnySlot/SCSIRQ/SndIRQ)
//   $010 RvMonP    monitor parameters (depth + monitor-sense + video on/off)
//   $011 RvChpT    chip-test register                        — accept-and-log
//   $012 RvSEnb    slot-interrupt enable
//   $013 RvIER     interrupt enable register
//
// Apple's shared VIA2/RBV OS code additionally reaches the IFR and IER at
// the VIA-register-spaced aliases Rv2IFR = vIFR+RvIFR = $1A03 and
// Rv2IER = vIER+RvIER = $1C13 (the IER decode requires A4=1 — an RBV ASIC
// quirk documented in the mac68k headers and local rbv-byte-lane-findings).
// We decode both the native small offsets and those two aliases so code
// written either way reaches the same register.
//
// Interrupt model.  RBV's SCSI / slot / sound interrupts are level inputs;
// we recompute the aggregated IFR from the live source state on every
// change rather than latching edges.  The chip asserts a single combined
// interrupt (→ 68030 IPL 2, matching the VIA2 it replaces) whenever
// (IFR & IER & $7F) is non-zero.

#include "rbv.h"

#include "checkpoint.h"
#include "log.h"
#include "system.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("rbv");

// === Register offsets (native RBV byte offsets) =============================

#define RV_DATAB 0x000 // RvDataB
#define RV_EXP   0x001 // RvExp
#define RV_SINT  0x002 // RvSInt
#define RV_IFR   0x003 // RvIFR
#define RV_MONP  0x010 // RvMonP
#define RV_CHPT  0x011 // RvChpT
#define RV_SENB  0x012 // RvSEnb
#define RV_IER   0x013 // RvIER

// VIA2-spaced aliases the shared OS code uses (see file header).
#define RV_IFR_ALIAS 0x1A03 // Rv2IFR = vIFR($1A00) + RvIFR($003)
#define RV_IER_ALIAS 0x1C13 // Rv2IER = vIER($1C00) + RvIER($013)

// === RvDataB bits ===========================================================

#define RVDATAB_CDIS     (1u << 0) // RvCDis    external cache disable (1 = off)
#define RVDATAB_BUSLK    (1u << 1) // RvBusLk   bus lockout
#define RVDATAB_POWEROFF (1u << 2) // RvPowerOff soft power off (active-low)
#define RVDATAB_CFLUSH   (1u << 3) // RvCFlush  flush external cache (active-low)
#define RVDATAB_SNDEXT   (1u << 6) // RvSndExt  1 = internal speaker

// === RvIFR / RvIER bits =====================================================

#define RVIFR_SCSIDRQ (1u << 0) // RvSCSIDRQ
#define RVIFR_ANYSLOT (1u << 1) // RvAnySlot
#define RVIFR_EXPIRQ  (1u << 2) // RvExpIRQ (reserved)
#define RVIFR_SCSIRQ  (1u << 3) // RvSCSIRQ
#define RVIFR_SNDIRQ  (1u << 4) // RvSndIRQ
#define RVIFR_SETCLR  (1u << 7) // write: 1 = set named bits, 0 = clear them

// === RvMonP bits ============================================================

#define RVMONP_DEPTH_MASK  0x07u // RvColor1-3 (bits 0-2): depth code 0..3
#define RVMONP_SENSE_SHIFT 3 // RvMonID1-3 (bits 3-5): monitor sense
#define RVMONP_SENSE_MASK  0x38u
#define RVMONP_VIDOFF      (1u << 6) // RvVIDOff
#define RVMONP_VID3ST      (1u << 7) // RvVID3St (all outputs tri-stated)

// Map a logical slot number (0 = built-in video, 1-6 = NuBus) to its bit in
// RvSInt / RvSEnb.  Per the equates: RvIRQ1..6 = bits 0-5, RvIRQ0 = bit 6.
static inline uint8_t slot_bit(int slot) {
    if (slot == 0)
        return 1u << 6; // RvIRQ0 — built-in video
    if (slot >= 1 && slot <= 6)
        return (uint8_t)(1u << (slot - 1)); // RvIRQ1..6
    return 0;
}

// === State ==================================================================

struct rbv {
    // --- plain data (checkpointed up to the first pointer field) ---
    rbv_variant_t variant;

    uint8_t reg_datab; // RvDataB latched control bits
    uint8_t reg_exp; // RvExp
    uint8_t reg_monp; // RvMonP (depth + sense + video bits)
    uint8_t reg_chpt; // RvChpT
    uint8_t reg_ier; // RvIER (bits 0-6 enable; bit 7 is set/clr on write only)
    uint8_t reg_senb; // RvSEnb slot-interrupt enable (bits 0-6)

    uint8_t slot_pending; // active-high: bit = slot IRQ requested (slot_bit layout)
    bool scsi_irq; // live RvSCSIRQ source
    bool scsi_drq; // live RvSCSIDRQ source
    bool snd_irq; // live RvSndIRQ source

    bool irq_active; // last reported combined-interrupt state
    bool power_armed; // soft-power debounce (see RvDataB write handler)

    // --- pointers / callbacks (not checkpointed) ---
    memory_interface_t memory_interface;
    void (*irq_cb)(void *ctx, bool active);
    void *irq_ctx;
    void (*power_cb)(void *ctx);
    void *power_ctx;
    void (*mode_cb)(void *ctx, int depth_code);
    void *mode_ctx;
};

// === Interrupt aggregation ==================================================

// Compose the current RvIFR value (bits 0-6) from the live source state.
// RvAnySlot reflects only slots enabled in RvSEnb, matching the hardware.
static uint8_t rbv_compose_ifr(const rbv_t *rbv) {
    uint8_t ifr = 0;
    if (rbv->scsi_drq)
        ifr |= RVIFR_SCSIDRQ;
    if ((rbv->slot_pending & rbv->reg_senb & 0x7Fu) != 0)
        ifr |= RVIFR_ANYSLOT;
    if (rbv->scsi_irq)
        ifr |= RVIFR_SCSIRQ;
    if (rbv->snd_irq)
        ifr |= RVIFR_SNDIRQ;
    return ifr;
}

// Recompute the combined interrupt and notify the machine when it changes.
static void rbv_update_irq(rbv_t *rbv) {
    bool active = (rbv_compose_ifr(rbv) & rbv->reg_ier & 0x7Fu) != 0;
    if (active == rbv->irq_active)
        return;
    rbv->irq_active = active;
    if (rbv->irq_cb)
        rbv->irq_cb(rbv->irq_ctx, active);
}

// === Register read ==========================================================

// Translate a window offset (native or VIA-spaced alias) to a register id,
// or 0xFF if unmapped.
static uint16_t rbv_decode(uint32_t off) {
    switch (off) {
    case RV_DATAB:
    case RV_EXP:
    case RV_SINT:
    case RV_IFR:
    case RV_MONP:
    case RV_CHPT:
    case RV_SENB:
    case RV_IER:
        return (uint16_t)off;
    case RV_IFR_ALIAS:
        return RV_IFR;
    case RV_IER_ALIAS:
        return RV_IER;
    default:
        return 0xFFFF;
    }
}

static uint8_t rbv_read_byte(void *device, uint32_t addr) {
    rbv_t *rbv = (rbv_t *)device;
    uint16_t reg = rbv_decode(addr);
    switch (reg) {
    case RV_DATAB:
        return rbv->reg_datab;
    case RV_EXP:
        return rbv->reg_exp;
    case RV_SINT: {
        // RvSInt is active-low: a set bit means "no interrupt on that slot".
        uint8_t v = (uint8_t)(~rbv->slot_pending & 0x7Fu);
        LOG(4, "read RvSInt = $%02X", v);
        // The built-in-video VBL flag (RvIRQ0 = bit 6) is clear-on-read: a
        // vblank pulse is observed once per frame then deasserts.  NuBus slot
        // bits (0-5) are level and not cleared here.
        if (rbv->slot_pending & (1u << 6)) {
            rbv->slot_pending &= (uint8_t) ~(1u << 6);
            rbv_update_irq(rbv);
        }
        return v;
    }
    case RV_IFR: {
        uint8_t ifr = rbv_compose_ifr(rbv);
        // Bit 7 on read = "any enabled interrupt is pending".
        if ((ifr & rbv->reg_ier & 0x7Fu) != 0)
            ifr |= RVIFR_SETCLR;
        LOG(4, "read RvIFR = $%02X", ifr);
        return ifr;
    }
    case RV_MONP:
        LOG(4, "read RvMonP = $%02X", rbv->reg_monp);
        return rbv->reg_monp;
    case RV_CHPT:
        return rbv->reg_chpt;
    case RV_SENB:
        return rbv->reg_senb;
    case RV_IER:
        // Bit 7 always reads 0 (it is a write-only set/clr selector).
        return (uint8_t)(rbv->reg_ier & 0x7Fu);
    default:
        LOG(2, "read unmapped RBV offset $%04X", addr);
        return 0xFF;
    }
}

// === Register write =========================================================

static void rbv_write_byte(void *device, uint32_t addr, uint8_t value) {
    rbv_t *rbv = (rbv_t *)device;
    uint16_t reg = rbv_decode(addr);
    switch (reg) {
    case RV_DATAB: {
        // Soft power-off: RvPowerOff (bit 2) is active-low.  The boot ROM
        // touches RvDataB with the bit low during early init before the OS
        // has driven it, so (like the IIcx VIA2 PB2 detector) we arm on the
        // first observed bit-high and only then fire on the falling edge.
        bool new_pwr_high = (value & RVDATAB_POWEROFF) != 0;
        if (!rbv->power_armed && new_pwr_high) {
            rbv->power_armed = true;
        } else if (rbv->power_armed && !new_pwr_high) {
            LOG(1, "RBV soft power-off (RvPowerOff = 0)");
            if (rbv->power_cb)
                rbv->power_cb(rbv->power_ctx);
        }
        rbv->reg_datab = value;
        return;
    }
    case RV_EXP:
        rbv->reg_exp = value;
        return;
    case RV_SINT:
        // Slot-interrupt status is driven by the cards, not the CPU; writes
        // are accepted (the OS pokes it during self-test) but do not latch.
        LOG(3, "write RvSInt = $%02X (accept-and-log)", value);
        return;
    case RV_IFR:
        // IFR is composed live from source state.  The OS clears flags by
        // writing with bit 7 = 0; the underlying sources deassert on
        // service, so the recompute below reflects the result.  Accept the
        // write so diagnostic poke/peek sequences see no bus error.
        LOG(3, "write RvIFR = $%02X (set/clr=%d)", value, (value & RVIFR_SETCLR) ? 1 : 0);
        rbv_update_irq(rbv);
        return;
    case RV_MONP: {
        uint8_t old_depth = rbv->reg_monp & RVMONP_DEPTH_MASK;
        // Depth (bits 0-2) and video on/off (bits 6-7) are writable; the
        // monitor-sense field (bits 3-5) is read-only and preserved.
        rbv->reg_monp = (uint8_t)((value & ~RVMONP_SENSE_MASK) | (rbv->reg_monp & RVMONP_SENSE_MASK));
        uint8_t new_depth = rbv->reg_monp & RVMONP_DEPTH_MASK;
        LOG(3, "write RvMonP = $%02X (depth=%d vidoff=%d)", rbv->reg_monp, new_depth,
            (rbv->reg_monp & RVMONP_VIDOFF) ? 1 : 0);
        if (new_depth != old_depth && rbv->mode_cb)
            rbv->mode_cb(rbv->mode_ctx, new_depth);
        return;
    }
    case RV_CHPT:
        rbv->reg_chpt = value;
        return;
    case RV_SENB: {
        // RvSEnb uses the same set/clr-via-bit-7 convention as RvIER.
        uint8_t bits = value & 0x7Fu;
        if (value & 0x80u)
            rbv->reg_senb |= bits;
        else
            rbv->reg_senb &= (uint8_t)~bits;
        LOG(3, "write RvSEnb = $%02X -> $%02X", value, rbv->reg_senb);
        rbv_update_irq(rbv);
        return;
    }
    case RV_IER: {
        // Bit 7 selects set (1) vs clear (0) of the named enable bits.
        uint8_t bits = value & 0x7Fu;
        if (value & 0x80u)
            rbv->reg_ier |= bits;
        else
            rbv->reg_ier &= (uint8_t)~bits;
        LOG(3, "write RvIER = $%02X -> $%02X", value, rbv->reg_ier);
        rbv_update_irq(rbv);
        return;
    }
    default:
        LOG(2, "write unmapped RBV offset $%04X = $%02X", addr, value);
        return;
    }
}

// RBV is an 8-bit peripheral; compose wider accesses from byte ops so an
// occasional word/long touch from the OS doesn't fault.
static uint16_t rbv_read_word(void *device, uint32_t addr) {
    return (uint16_t)((rbv_read_byte(device, addr) << 8) | rbv_read_byte(device, addr + 1));
}

static uint32_t rbv_read_long(void *device, uint32_t addr) {
    return ((uint32_t)rbv_read_word(device, addr) << 16) | rbv_read_word(device, addr + 2);
}

static void rbv_write_word(void *device, uint32_t addr, uint16_t value) {
    rbv_write_byte(device, addr, (uint8_t)(value >> 8));
    rbv_write_byte(device, addr + 1, (uint8_t)value);
}

static void rbv_write_long(void *device, uint32_t addr, uint32_t value) {
    rbv_write_word(device, addr, (uint16_t)(value >> 16));
    rbv_write_word(device, addr + 2, (uint16_t)value);
}

// === Lifecycle ==============================================================

rbv_t *rbv_init(rbv_variant_t variant, checkpoint_t *cp) {
    rbv_t *rbv = (rbv_t *)calloc(1, sizeof(*rbv));
    if (!rbv)
        return NULL;
    rbv->variant = variant;

    // Reset defaults: cache enabled, power on, sound to internal speaker.
    // RvPowerOff (bit 2) idles high (= powered on); the detector arms on it.
    rbv->reg_datab = RVDATAB_POWEROFF | RVDATAB_CFLUSH | RVDATAB_SNDEXT;
    // Default monitor sense = 6 (binary 110 = Macintosh II 13" RGB), depth 0.
    rbv->reg_monp = (uint8_t)((6u << RVMONP_SENSE_SHIFT) & RVMONP_SENSE_MASK);

    rbv->memory_interface = (memory_interface_t){
        .read_uint8 = rbv_read_byte,
        .read_uint16 = rbv_read_word,
        .read_uint32 = rbv_read_long,
        .write_uint8 = rbv_write_byte,
        .write_uint16 = rbv_write_word,
        .write_uint32 = rbv_write_long,
    };

    if (cp) {
        size_t data_size = offsetof(rbv_t, memory_interface);
        system_read_checkpoint_data(cp, rbv, data_size);
    }

    return rbv;
}

void rbv_delete(rbv_t *rbv) {
    free(rbv);
}

void rbv_checkpoint(rbv_t *rbv, checkpoint_t *cp) {
    if (!rbv || !cp)
        return;
    size_t data_size = offsetof(rbv_t, memory_interface);
    system_write_checkpoint_data(cp, rbv, data_size);
}

// === Wiring =================================================================

const memory_interface_t *rbv_get_memory_interface(rbv_t *rbv) {
    return &rbv->memory_interface;
}

void rbv_set_irq_callback(rbv_t *rbv, void (*cb)(void *ctx, bool active), void *ctx) {
    rbv->irq_cb = cb;
    rbv->irq_ctx = ctx;
}

void rbv_set_power_off_callback(rbv_t *rbv, void (*cb)(void *ctx), void *ctx) {
    rbv->power_cb = cb;
    rbv->power_ctx = ctx;
}

void rbv_set_mode_callback(rbv_t *rbv, void (*cb)(void *ctx, int depth_code), void *ctx) {
    rbv->mode_cb = cb;
    rbv->mode_ctx = ctx;
}

// === Interrupt sources ======================================================

void rbv_assert_slot_irq(rbv_t *rbv, int slot) {
    uint8_t bit = slot_bit(slot);
    if (!bit)
        return;
    if (rbv->slot_pending & bit)
        return;
    rbv->slot_pending |= bit;
    rbv_update_irq(rbv);
}

void rbv_clear_slot_irq(rbv_t *rbv, int slot) {
    uint8_t bit = slot_bit(slot);
    if (!bit)
        return;
    if (!(rbv->slot_pending & bit))
        return;
    rbv->slot_pending &= (uint8_t)~bit;
    rbv_update_irq(rbv);
}

void rbv_set_scsi_irq(rbv_t *rbv, bool active) {
    if (rbv->scsi_irq == active)
        return;
    rbv->scsi_irq = active;
    rbv_update_irq(rbv);
}

void rbv_set_scsi_drq(rbv_t *rbv, bool active) {
    if (rbv->scsi_drq == active)
        return;
    rbv->scsi_drq = active;
    rbv_update_irq(rbv);
}

void rbv_set_snd_irq(rbv_t *rbv, bool active) {
    if (rbv->snd_irq == active)
        return;
    rbv->snd_irq = active;
    rbv_update_irq(rbv);
}

// === Configuration ==========================================================

void rbv_set_monitor_sense(rbv_t *rbv, uint8_t sense3) {
    rbv->reg_monp = (uint8_t)((rbv->reg_monp & ~RVMONP_SENSE_MASK) |
                              (((uint32_t)sense3 << RVMONP_SENSE_SHIFT) & RVMONP_SENSE_MASK));
}

int rbv_current_depth(rbv_t *rbv) {
    return rbv->reg_monp & RVMONP_DEPTH_MASK;
}
