// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dma_mem.c — the shared guest-physical port.  See dma_mem.h.

#include "dma_mem.h"

bool dma_mem_port_bound(const dma_mem_port_t *p) {
    return p && (p->read || p->read_block || p->write || p->write_block);
}

uint32_t dma_mem_read(const dma_mem_port_t *p, uint32_t phys, unsigned width) {
    if (!p)
        return 0;
    if (p->read)
        return p->read(p->ctx, phys, width);
    if (!p->read_block)
        return 0;
    uint8_t buf[4] = {0, 0, 0, 0};
    if (width > sizeof buf)
        width = sizeof buf;
    p->read_block(p->ctx, phys, buf, width);
    uint32_t v = 0;
    for (unsigned i = 0; i < width; i++)
        v = (v << 8) | buf[i]; // big-endian, like every bus in this machine
    return v;
}

void dma_mem_write(const dma_mem_port_t *p, uint32_t phys, uint32_t value, unsigned width) {
    if (!p)
        return;
    if (p->write) {
        p->write(p->ctx, phys, value, width);
        return;
    }
    if (!p->write_block)
        return;
    uint8_t buf[4];
    if (width > sizeof buf)
        width = sizeof buf;
    for (unsigned i = 0; i < width; i++)
        buf[i] = (uint8_t)(value >> ((width - 1 - i) * 8));
    p->write_block(p->ctx, phys, buf, width);
}

void dma_mem_read_block(const dma_mem_port_t *p, uint32_t phys, uint8_t *buf, uint32_t len) {
    if (!p || !buf)
        return;
    if (p->read_block) {
        p->read_block(p->ctx, phys, buf, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)dma_mem_read(p, phys + i, 1);
}

void dma_mem_write_block(const dma_mem_port_t *p, uint32_t phys, const uint8_t *buf, uint32_t len) {
    if (!p || !buf)
        return;
    if (p->write_block) {
        p->write_block(p->ctx, phys, buf, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        dma_mem_write(p, phys + i, buf[i], 1);
}
