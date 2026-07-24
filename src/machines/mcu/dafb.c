// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dafb.c
// DAFB built-in video — Phase C register stub (see dafb.h).  Every access to
// the $F9800000 aperture is stored and logged (log-once per register) so the
// boot ROM's probe sequence becomes the RE artifact the proposal's Phase C
// gate asks for; nothing is interpreted yet.  Evidence labels follow the
// reference: register meanings at this aperture are [R]/[U] until the
// DAFBDriver.a cross-check in Phase D.

#include "dafb.h"

#include "log.h"
#include "scheduler.h"
#include "system.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("dafb");

// Swatch register offsets inside the aperture (Swatch block at +$100;
// reference §11.4/§11.10 [R])
#define SWATCH_INTR_STATUS  0x108u // bit 0 VBL pending, bit 2 cursor pending
#define SWATCH_CLEAR_CURSOR 0x10Cu // access clears cursor pending
#define SWATCH_CLEAR_VBL    0x114u // access clears VBL pending

// Phase C fixed frame period (60.15 Hz) — replaced by programmed Swatch
// timing in Phase D.
#define DAFB_FRAME_NS 16625000ull

struct dafb {
    uint32_t regs[DAFB_REG_COUNT]; // stored register file ($000-$3FF)
    uint8_t touched[DAFB_REG_COUNT]; // log-once bitmap
    uint8_t *vram; // dedicated VRAM buffer
    uint32_t vram_size; // installed capacity (512K/1M/2M)
    struct scheduler *sched; // frame-event source (NULL until attached)
};

// Frame event: raise the Swatch pending bits and re-arm.
static void dafb_frame_event(void *source, uint64_t data) {
    (void)data;
    dafb_t *dafb = (dafb_t *)source;
    dafb->regs[SWATCH_INTR_STATUS >> 2] |= 0x5u; // VBL (bit 0) + cursor (bit 2)
    scheduler_new_cpu_event(dafb->sched, dafb_frame_event, dafb, 0, 0, DAFB_FRAME_NS);
}

void dafb_attach_scheduler(dafb_t *dafb, struct scheduler *sched) {
    if (!dafb || !sched)
        return;
    dafb->sched = sched;
    scheduler_new_event_type(sched, "dafb", dafb, "swatch_frame", dafb_frame_event);
    if (!has_event(sched, dafb_frame_event))
        scheduler_new_cpu_event(sched, dafb_frame_event, dafb, 0, 0, DAFB_FRAME_NS);
}

dafb_t *dafb_init(uint32_t vram_size, checkpoint_t *cp) {
    dafb_t *dafb = (dafb_t *)calloc(1, sizeof(dafb_t));
    if (!dafb)
        return NULL;
    dafb->vram_size = vram_size;
    dafb->vram = (uint8_t *)calloc(1, vram_size);
    if (!dafb->vram) {
        free(dafb);
        return NULL;
    }
    if (cp) {
        system_read_checkpoint_data(cp, dafb->regs, sizeof(dafb->regs));
        system_read_checkpoint_data(cp, dafb->vram, vram_size);
    }
    return dafb;
}

void dafb_delete(dafb_t *dafb) {
    if (!dafb)
        return;
    free(dafb->vram);
    free(dafb);
}

void dafb_checkpoint(dafb_t *dafb, checkpoint_t *cp) {
    if (!dafb || !cp)
        return;
    system_write_checkpoint_data(cp, dafb->regs, sizeof(dafb->regs));
    system_write_checkpoint_data(cp, dafb->vram, dafb->vram_size);
}

uint8_t *dafb_vram(dafb_t *dafb) {
    return dafb ? dafb->vram : NULL;
}

uint32_t dafb_vram_size(dafb_t *dafb) {
    return dafb ? dafb->vram_size : 0;
}

void dafb_reset(dafb_t *dafb) {
    if (!dafb)
        return;
    memset(dafb->regs, 0, sizeof(dafb->regs));
    memset(dafb->touched, 0, sizeof(dafb->touched));
}

// === Register aperture (accept-and-log with readback) =======================
// The aperture is decoded as longword slots; sub-longword accesses read and
// write the addressed bytes of the stored value (big-endian lanes).  Offsets
// beyond $3FF mirror into the low $400 [U — mirror extent unmeasured].

static inline uint32_t reg_index(uint32_t offset) {
    return (offset & 0x3FFu) >> 2;
}

static void log_touch(dafb_t *dafb, uint32_t offset, bool write, uint32_t value) {
    uint32_t idx = reg_index(offset);
    if (dafb->touched[idx] && !write)
        return; // reads of an already-seen register stay quiet
    dafb->touched[idx] = 1;
    LOG(2, "DAFB %s $%03X %s $%08X", write ? "write" : "read", offset & 0x3FFu, write ? "=" : "->", value);
}

// Forward declaration: reads of the Swatch clear registers also clear.
static void swatch_access_side_effects(dafb_t *dafb, uint32_t offset);

static uint8_t dafb_read8(void *ctx, uint32_t offset) {
    dafb_t *dafb = (dafb_t *)ctx;
    uint32_t v = dafb->regs[reg_index(offset)];
    log_touch(dafb, offset, false, v);
    swatch_access_side_effects(dafb, offset);
    return (uint8_t)(v >> (8 * (3 - (offset & 3))));
}

static uint16_t dafb_read16(void *ctx, uint32_t offset) {
    return (uint16_t)(((uint16_t)dafb_read8(ctx, offset) << 8) | dafb_read8(ctx, offset + 1));
}

static uint32_t dafb_read32(void *ctx, uint32_t offset) {
    dafb_t *dafb = (dafb_t *)ctx;
    uint32_t v = dafb->regs[reg_index(offset)];
    log_touch(dafb, offset, false, v);
    swatch_access_side_effects(dafb, offset);
    return v;
}

// Access side effects on the Swatch clear registers (reference: any access
// to +$10C / +$114 clears the corresponding pending bit).
static void swatch_access_side_effects(dafb_t *dafb, uint32_t offset) {
    uint32_t reg = offset & 0x3FCu;
    if (reg == SWATCH_CLEAR_CURSOR)
        dafb->regs[SWATCH_INTR_STATUS >> 2] &= ~0x4u;
    else if (reg == SWATCH_CLEAR_VBL)
        dafb->regs[SWATCH_INTR_STATUS >> 2] &= ~0x1u;
}

static void dafb_write8(void *ctx, uint32_t offset, uint8_t value) {
    dafb_t *dafb = (dafb_t *)ctx;
    uint32_t idx = reg_index(offset);
    uint32_t shift = 8 * (3 - (offset & 3));
    dafb->regs[idx] = (dafb->regs[idx] & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    log_touch(dafb, offset, true, dafb->regs[idx]);
    swatch_access_side_effects(dafb, offset);
}

static void dafb_write16(void *ctx, uint32_t offset, uint16_t value) {
    dafb_write8(ctx, offset, (uint8_t)(value >> 8));
    dafb_write8(ctx, offset + 1, (uint8_t)value);
}

static void dafb_write32(void *ctx, uint32_t offset, uint32_t value) {
    dafb_t *dafb = (dafb_t *)ctx;
    dafb->regs[reg_index(offset)] = value;
    log_touch(dafb, offset, true, value);
    swatch_access_side_effects(dafb, offset);
}

static const memory_interface_t dafb_reg_iface = {
    .read_uint8 = dafb_read8,
    .read_uint16 = dafb_read16,
    .read_uint32 = dafb_read32,
    .write_uint8 = dafb_write8,
    .write_uint16 = dafb_write16,
    .write_uint32 = dafb_write32,
};

const memory_interface_t *dafb_reg_interface(dafb_t *dafb) {
    (void)dafb;
    return &dafb_reg_iface;
}
