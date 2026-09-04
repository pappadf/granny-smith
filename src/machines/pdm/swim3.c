// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// swim3.c
// The PDM's face of the shared SWIM3 model (core/peripherals/swim3.c):
// the AMIC island decodes the sixteen registers on $200 centres at
// $50F16000 (index = offset >> 9), the engine streams through the AMIC
// floppy DMA channel, and the IRQ pin lands in the pseudo-VIA2 device
// bank.  Everything the chip owns lives in the shared module; this file
// is the three movers and the interrupt sink the module is bound to.

#include "pdm.h"

#include "swim3.h"

static bool pdm_fd_dma_running(void *ctx) {
    return pdm_amic_fd_dma_running((config_t *)ctx);
}

static bool pdm_fd_dma_get(void *ctx, uint8_t *out) {
    return pdm_amic_fd_dma_get((config_t *)ctx, out);
}

static bool pdm_fd_dma_put(void *ctx, uint8_t value) {
    return pdm_amic_fd_dma_put((config_t *)ctx, value);
}

static void pdm_fd_set_irq(void *ctx, bool level) {
    pdm_amic_set_fdc_irq((config_t *)ctx, level);
}

void pdm_swim3_bind(config_t *cfg) {
    const swim3_backend_t be = {
        .dma_running = pdm_fd_dma_running,
        .dma_get = pdm_fd_dma_get,
        .dma_put = pdm_fd_dma_put,
        .set_irq = pdm_fd_set_irq,
        .ctx = cfg,
    };
    swim3_bind(&pdm_st(cfg)->swim3, cfg->floppy, cfg->scheduler, &be);
}

void pdm_swim3_register_events(config_t *cfg) {
    swim3_register_events(&pdm_st(cfg)->swim3);
}

void pdm_swim3_xfer_register_events(config_t *cfg) {
    swim3_xfer_register_events(&pdm_st(cfg)->swim3);
}

uint8_t pdm_swim3_read(config_t *cfg, uint32_t off) {
    return swim3_read(&pdm_st(cfg)->swim3, (off >> 9) & 15u);
}

void pdm_swim3_write(config_t *cfg, uint32_t off, uint8_t value) {
    swim3_write(&pdm_st(cfg)->swim3, (off >> 9) & 15u, value);
}
