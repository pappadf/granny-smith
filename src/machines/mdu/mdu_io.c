// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu_io.c
// Shared MDU+RBV I/O dispatcher — see mdu_io.h.  Logic is the IIci dispatcher
// verbatim (the IIsi's was byte-identical); only the state access is now via
// mdu_io_t instead of each machine's struct.

#include "mdu_io.h"

#include "asc.h"
#include "builtin_rbv_video.h"
#include "floppy.h"
#include "rbv.h"
#include "scc.h"
#include "scsi.h"
#include "via.h"

// Mirror mask + window offsets (the MDU $50Fxxxxx island; no VIA2 window).
#define MDU_IO_MIRROR     0x0003FFFFUL
#define IO_VIA1           0x00000
#define IO_VIA1_END       0x02000
#define IO_SCC            0x04000
#define IO_SCC_END        0x06000
#define IO_SCSI_DRQ       0x06000
#define IO_SCSI_DRQ_END   0x08000
#define IO_SCSI_REG       0x10000
#define IO_SCSI_REG_END   0x12000
#define IO_SCSI_BLIND     0x12000
#define IO_SCSI_BLIND_END 0x14000
#define IO_ASC            0x14000
#define IO_ASC_END        0x16000
#define IO_SWIM           0x16000
#define IO_SWIM_END       0x18000
#define IO_VDAC           0x24000
#define IO_VDAC_END       0x26000
#define IO_RBV            0x26000
#define IO_RBV_END        0x28000

#define MDU_VIA_IO_PENALTY  16
#define MDU_SCC_IO_PENALTY  2
#define MDU_SCSI_IO_PENALTY 2
#define MDU_ASC_IO_PENALTY  2
#define MDU_SWIM_IO_PENALTY 2
#define MDU_RBV_IO_PENALTY  2
#define MDU_VDAC_IO_PENALTY 2

// Cache the device interfaces for the dispatcher.
void mdu_io_bind(mdu_io_t *io, config_t *cfg, void *asc, void *floppy, void *rbv, struct nubus_card *video_card) {
    io->cfg = cfg;
    io->via1_iface = via_get_memory_interface(cfg->via1);
    io->scc_iface = scc_get_memory_interface(cfg->scc);
    io->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    io->asc_iface = asc_get_memory_interface((asc_t *)asc);
    io->floppy_iface = floppy_get_memory_interface((floppy_t *)floppy);
    io->rbv_iface = rbv_get_memory_interface((rbv_t *)rbv);
    io->asc = asc;
    io->floppy = floppy;
    io->rbv = rbv;
    io->video_card = video_card;
}

uint8_t mdu_io_read_uint8(void *ctx, uint32_t addr) {
    mdu_io_t *io = (mdu_io_t *)ctx;
    config_t *cfg = io->cfg;
    uint32_t offset = addr & MDU_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(MDU_VIA_IO_PENALTY);
        return io->via1_iface->read_uint8(cfg->via1, (offset - IO_VIA1) & ~1u);
    }
    if (offset >= IO_SCC && offset < IO_SCC_END) {
        memory_io_penalty(MDU_SCC_IO_PENALTY);
        return io->scc_iface->read_uint8(cfg->scc, offset - IO_SCC);
    }
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY);
        return io->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY);
        return io->scsi_iface->read_uint8(cfg->scsi, offset - IO_SCSI_REG);
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY);
        return io->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(MDU_ASC_IO_PENALTY);
        return io->asc_iface->read_uint8(io->asc, offset - IO_ASC);
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(MDU_SWIM_IO_PENALTY);
        return io->floppy_iface->read_uint8(io->floppy, offset - IO_SWIM);
    }
    if (offset >= IO_VDAC && offset < IO_VDAC_END) {
        memory_io_penalty(MDU_VDAC_IO_PENALTY);
        return builtin_rbv_video_vdac_read(io->video_card, offset - IO_VDAC);
    }
    if (offset >= IO_RBV && offset < IO_RBV_END) {
        memory_io_penalty(MDU_RBV_IO_PENALTY);
        return io->rbv_iface->read_uint8(io->rbv, offset - IO_RBV);
    }
    return 0;
}

uint16_t mdu_io_read_uint16(void *ctx, uint32_t addr) {
    return ((uint16_t)mdu_io_read_uint8(ctx, addr) << 8) | mdu_io_read_uint8(ctx, addr + 1);
}

uint32_t mdu_io_read_uint32(void *ctx, uint32_t addr) {
    mdu_io_t *io = (mdu_io_t *)ctx;
    config_t *cfg = io->cfg;
    uint32_t offset = addr & MDU_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY * 4);
        uint8_t b0 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY * 4);
        uint8_t b0 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    return ((uint32_t)mdu_io_read_uint16(ctx, addr) << 16) | mdu_io_read_uint16(ctx, addr + 2);
}

void mdu_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    mdu_io_t *io = (mdu_io_t *)ctx;
    config_t *cfg = io->cfg;
    uint32_t offset = addr & MDU_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(MDU_VIA_IO_PENALTY);
        io->via1_iface->write_uint8(cfg->via1, (offset - IO_VIA1) & ~1u, value);
        return;
    }
    if (offset >= IO_SCC && offset < IO_SCC_END) {
        memory_io_penalty(MDU_SCC_IO_PENALTY);
        io->scc_iface->write_uint8(cfg->scc, offset - IO_SCC, value);
        return;
    }
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY);
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY);
        io->scsi_iface->write_uint8(cfg->scsi, offset - IO_SCSI_REG, value);
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY);
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(MDU_ASC_IO_PENALTY);
        io->asc_iface->write_uint8(io->asc, offset - IO_ASC, value);
        return;
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(MDU_SWIM_IO_PENALTY);
        io->floppy_iface->write_uint8(io->floppy, offset - IO_SWIM, value);
        return;
    }
    if (offset >= IO_VDAC && offset < IO_VDAC_END) {
        memory_io_penalty(MDU_VDAC_IO_PENALTY);
        builtin_rbv_video_vdac_write(io->video_card, offset - IO_VDAC, value);
        return;
    }
    if (offset >= IO_RBV && offset < IO_RBV_END) {
        memory_io_penalty(MDU_RBV_IO_PENALTY);
        io->rbv_iface->write_uint8(io->rbv, offset - IO_RBV, value);
        return;
    }
}

void mdu_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    mdu_io_write_uint8(ctx, addr, (uint8_t)(value >> 8));
    mdu_io_write_uint8(ctx, addr + 1, (uint8_t)(value & 0xFF));
}

void mdu_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    mdu_io_t *io = (mdu_io_t *)ctx;
    config_t *cfg = io->cfg;
    uint32_t offset = addr & MDU_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY * 4);
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(MDU_SCSI_IO_PENALTY * 4);
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    mdu_io_write_uint16(ctx, addr, (uint16_t)(value >> 16));
    mdu_io_write_uint16(ctx, addr + 2, (uint16_t)(value & 0xFFFF));
}

void mdu_io_fill_interface(memory_interface_t *iface) {
    iface->read_uint8 = mdu_io_read_uint8;
    iface->read_uint16 = mdu_io_read_uint16;
    iface->read_uint32 = mdu_io_read_uint32;
    iface->write_uint8 = mdu_io_write_uint8;
    iface->write_uint16 = mdu_io_write_uint16;
    iface->write_uint32 = mdu_io_write_uint32;
}
