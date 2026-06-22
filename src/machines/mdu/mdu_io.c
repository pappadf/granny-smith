// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu_io.c
// The MDU+RBV family's dispatch table — see mdu_io.h.  The decode itself runs
// on the shared mac030 I/O engine (mac030_glue_io.c); this file just supplies
// the MDU window table, mirror mask, and device set.  The decode and per-window
// penalties are the IIci dispatcher verbatim (the IIsi's was byte-identical).

#include "mdu_io.h"

#include "mac030_glue.h" // mac030_board_desc_t

#include "asc.h"
#include "builtin_rbv_video.h"
#include "floppy.h"
#include "rbv.h"
#include "scc.h"
#include "scsi.h"
#include "via.h"

// The MDU $50Fxxxxx island uses an 18-bit ($40000) mirror.
#define MDU_IO_MIRROR 0x0003FFFFUL

#define MDU_VIA_IO_PENALTY  16
#define MDU_SCC_IO_PENALTY  2
#define MDU_SCSI_IO_PENALTY 2
#define MDU_ASC_IO_PENALTY  2
#define MDU_SWIM_IO_PENALTY 2
#define MDU_RBV_IO_PENALTY  2
#define MDU_VDAC_IO_PENALTY 2

// The Bt450 VDAC ($24000) is fronted by direct accessors on the built-in RBV
// video card, not a memory_interface_t — wrap them so the generic engine can
// route to it like any other device (the engine only ever calls read/write
// _uint8; 16/32-bit accesses byte-decompose).
static uint8_t mdu_vdac_read(void *dev, uint32_t off) {
    return builtin_rbv_video_vdac_read((nubus_card_t *)dev, off);
}
static void mdu_vdac_write(void *dev, uint32_t off, uint8_t val) {
    builtin_rbv_video_vdac_write((nubus_card_t *)dev, off, val);
}
static const memory_interface_t mdu_vdac_iface = {
    .read_uint8 = mdu_vdac_read,
    .write_uint8 = mdu_vdac_write,
};

// The canonical MDU $50Fxxxxx decode, expressed as data.  Like GLUE but: no
// VIA2 window (the RBV replaces it), and two extra windows — the VDAC ($24000)
// and the RBV control registers ($26000).  Gaps decode to nothing.
//
//   base     end      device            penalty              xform               rd  wr     name
const mac030_io_range_t mdu_io_ranges_tbl[] = {
    {0x00000, 0x02000, MAC030_DEV_VIA1, MDU_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via1"},
    {0x04000, 0x06000, MAC030_DEV_SCC, MDU_SCC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x06000, 0x08000, MAC030_DEV_SCSI, MDU_SCSI_IO_PENALTY, MAC030_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_drq"},
    {0x10000, 0x12000, MAC030_DEV_SCSI, MDU_SCSI_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scsi_reg"},
    {0x12000, 0x14000, MAC030_DEV_SCSI, MDU_SCSI_IO_PENALTY, MAC030_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_blind"},
    {0x14000, 0x16000, MAC030_DEV_ASC, MDU_ASC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "asc"},
    {0x16000, 0x18000, MAC030_DEV_FLOPPY, MDU_SWIM_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "swim"},
    {0x24000, 0x26000, MAC030_DEV_VDAC, MDU_VDAC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "vdac"},
    {0x26000, 0x28000, MAC030_DEV_RBV, MDU_RBV_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "rbv"},
    {0}, // sentinel: end == 0
};

const mac030_io_range_t *mdu_io_ranges(void) {
    return mdu_io_ranges_tbl;
}

void mdu_io_bind(mdu_io_t *io, config_t *cfg, const struct mac030_board_desc *desc, void *asc, void *floppy, void *rbv,
                 struct nubus_card *video_card) {
    for (int i = 0; i < MAC030_DEV_COUNT; i++) {
        io->handle[i] = NULL;
        io->iface[i] = NULL;
    }
    io->handle[MAC030_DEV_VIA1] = cfg->via1;
    io->handle[MAC030_DEV_SCC] = cfg->scc;
    io->handle[MAC030_DEV_SCSI] = cfg->scsi;
    io->handle[MAC030_DEV_ASC] = asc;
    io->handle[MAC030_DEV_FLOPPY] = floppy;
    io->handle[MAC030_DEV_RBV] = rbv;
    io->handle[MAC030_DEV_VDAC] = video_card;

    io->iface[MAC030_DEV_VIA1] = via_get_memory_interface(cfg->via1);
    io->iface[MAC030_DEV_SCC] = scc_get_memory_interface(cfg->scc);
    io->iface[MAC030_DEV_SCSI] = scsi_get_memory_interface(cfg->scsi);
    io->iface[MAC030_DEV_ASC] = asc_get_memory_interface((asc_t *)asc);
    io->iface[MAC030_DEV_FLOPPY] = floppy_get_memory_interface((floppy_t *)floppy);
    io->iface[MAC030_DEV_RBV] = rbv_get_memory_interface((rbv_t *)rbv);
    io->iface[MAC030_DEV_VDAC] = &mdu_vdac_iface;

    io->ranges = desc->io_ranges;
    io->mirror_mask = desc->io_mirror_mask;
    io->cfg = cfg;
    io->unmapped_read = desc->io_unmapped_read;
}
