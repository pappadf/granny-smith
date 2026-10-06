// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// new_age.c
// The AV Quadras' face of the shared New Age model
// (core/peripherals/new_age.c, which owns the chip, its command processor
// and its drive): the PSC island decodes the chip at $50F2A000, its DMA
// rides PSC channel 3 with the set's count as the chip's terminal count,
// and its INT is a level into PSC-VIA2 bit 5.  This file is the island
// decode, the channel-3 movers and the interrupt sink it is bound to; its
// API is declared in av.h (the chip's own header is new_age.h).

#include "av.h"
#include "psc.h"

#include "floppy.h"
#include "new_age.h"
#include "system.h"

#include <stddef.h>
#include <stdlib.h>

struct av_new_age {
    new_age_t chip; // checkpointed up to chip.fd
    config_t *cfg;
};

static inline av_new_age_t *fdc_of(config_t *cfg) {
    return ((av_state_t *)cfg->machine_context)->fdc;
}

static av_psc_t *psc_of_ctx(void *ctx) {
    av_state_t *st = (av_state_t *)((config_t *)ctx)->machine_context;
    return st ? st->psc : NULL;
}

// Device → memory, one byte.  The PSC terminates its count on the set's
// last byte, and that is the chip's TC: a whole-track read that programs
// both register sets (MFMTrack) sees TC at the end of the first command's
// range and the second set already armed for the next.
static int av_fd_dma_put(void *ctx, uint8_t value) {
    av_psc_t *psc = psc_of_ctx(ctx);
    if (!psc || av_psc_dma_dir(psc, AV_PSC_DMA_FDC) != 1)
        return NEW_AGE_DMA_NONE;
    uint32_t left = av_psc_dma_remaining(psc, AV_PSC_DMA_FDC);
    if (av_psc_dma_device_in(psc, AV_PSC_DMA_FDC, &value, 1) != 1)
        return NEW_AGE_DMA_NONE;
    return left == 1 ? NEW_AGE_DMA_TC : NEW_AGE_DMA_OK;
}

// Memory → device, one byte.
static int av_fd_dma_get(void *ctx, uint8_t *out) {
    av_psc_t *psc = psc_of_ctx(ctx);
    if (!psc || av_psc_dma_dir(psc, AV_PSC_DMA_FDC) != 0)
        return NEW_AGE_DMA_NONE;
    uint32_t left = av_psc_dma_remaining(psc, AV_PSC_DMA_FDC);
    if (av_psc_dma_device_out(psc, AV_PSC_DMA_FDC, out, 1) != 1)
        return NEW_AGE_DMA_NONE;
    return left == 1 ? NEW_AGE_DMA_TC : NEW_AGE_DMA_OK;
}

// The INT pin: a level into PSC-VIA2 bit 5.  The driver tests the flag
// (Btst #FDCInt,VIA2IFR) and never clears it; the chip drops it.
static void av_fd_set_irq(void *ctx, bool level) {
    av_psc_t *psc = psc_of_ctx(ctx);
    if (psc)
        av_psc_via2_source(psc, AV_PSC_VIA2_FDC, level);
}

static void av_new_age_bind(av_new_age_t *fdc) {
    const new_age_backend_t be = {
        .dma_put = av_fd_dma_put,
        .dma_get = av_fd_dma_get,
        .set_irq = av_fd_set_irq,
        .ctx = fdc->cfg,
    };
    new_age_bind(&fdc->chip, fdc->cfg->floppy, fdc->cfg->scheduler, &be);
}

// ============================================================
// Register handlers
// ============================================================

// Apple's decode puts the chip's A0 on byte-offset bit 6 with A2 tied high:
// $101 is STR/DRR and $141 the data register.  Nothing else answers.
static int av_fd_reg(uint32_t off) {
    if (off == 0x101)
        return NEW_AGE_REG_STATUS;
    if (off == 0x141)
        return NEW_AGE_REG_DATA;
    return -1;
}

uint8_t av_new_age_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    int reg = av_fd_reg(win_off);
    return reg < 0 ? 0xFF : new_age_read(&fdc_of(cfg)->chip, (unsigned)reg);
}

uint8_t av_new_age_peek(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    int reg = av_fd_reg(win_off);
    return reg < 0 ? 0xFF : new_age_peek(&fdc_of(cfg)->chip, (unsigned)reg);
}

void av_new_age_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    int reg = av_fd_reg(win_off);
    if (reg >= 0)
        new_age_write(&fdc_of(cfg)->chip, (unsigned)reg, value);
}

// ============================================================
// Lifecycle
// ============================================================

av_new_age_t *av_new_age_init(config_t *cfg, checkpoint_t *cp) {
    av_new_age_t *fdc = calloc(1, sizeof(*fdc));
    if (!fdc)
        return NULL;
    fdc->cfg = cfg;
    if (cp)
        system_read_checkpoint_data(cp, &fdc->chip, offsetof(new_age_t, fd));
    av_new_age_bind(fdc);
    if (!cp)
        new_age_reset(&fdc->chip); // power-on state
    new_age_register_events(&fdc->chip, cp != NULL);
    return fdc;
}

void av_new_age_reset(av_new_age_t *fdc) {
    if (fdc)
        new_age_reset(&fdc->chip);
}

void av_new_age_delete(av_new_age_t *fdc) {
    if (!fdc)
        return;
    if (fdc->chip.sched)
        scheduler_forget_source(fdc->chip.sched, &fdc->chip);
    free(fdc);
}

void av_new_age_checkpoint(av_new_age_t *fdc, checkpoint_t *cp) {
    if (!fdc || !cp)
        return;
    system_write_checkpoint_data(cp, &fdc->chip, offsetof(new_age_t, fd));
}
