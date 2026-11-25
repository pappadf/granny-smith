// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mdu_io.c
// The MDU+RBV family's dispatch table — see mdu_io.h.  The decode itself runs
// on the shared board_io engine (board_io.c); this file just supplies
// the MDU window table, mirror mask, and device set.  The decode and per-window
// penalties are the IIci dispatcher verbatim (the IIsi's was byte-identical).

#include "mdu_io.h"

#include "m68k_board.h" // m68k_board_desc_t

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
static uint8_t mdu_vdac_peek(void *dev, uint32_t off) {
    return builtin_rbv_video_vdac_peek((nubus_card_t *)dev, off);
}
static void mdu_vdac_write(void *dev, uint32_t off, uint8_t val) {
    builtin_rbv_video_vdac_write((nubus_card_t *)dev, off, val);
}
static const memory_interface_t mdu_vdac_iface = {
    .read_uint8 = mdu_vdac_read,
    .write_uint8 = mdu_vdac_write,
    .peek_uint8 = mdu_vdac_peek,
};

// The canonical MDU $50Fxxxxx decode, expressed as data.  Like GLUE but: no
// VIA2 window (the RBV replaces it), and two extra windows — the VDAC ($24000)
// and the RBV control registers ($26000).  Gaps decode to nothing.
//
//   base     end      device            penalty              xform               rd  wr     name
const board_io_range_t mdu_io_ranges_tbl[] = {
    {0x00000, 0x02000, BOARD_DEV_VIA1, MDU_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x04000, 0x06000, BOARD_DEV_SCC, MDU_SCC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x06000, 0x08000, BOARD_DEV_SCSI, MDU_SCSI_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_drq"},
    {0x10000, 0x12000, BOARD_DEV_SCSI, MDU_SCSI_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scsi_reg"},
    {0x12000, 0x14000, BOARD_DEV_SCSI, MDU_SCSI_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_blind"},
    {0x14000, 0x16000, BOARD_DEV_ASC, MDU_ASC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "asc"},
    {0x16000, 0x18000, BOARD_DEV_FLOPPY, MDU_SWIM_IO_PENALTY, BOARD_IO_STRIDE_512, 0, 0, NULL, NULL, "swim"},
    {0x24000, 0x26000, BOARD_DEV_VDAC, MDU_VDAC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "vdac"},
    {0x26000, 0x28000, BOARD_DEV_RBV, MDU_RBV_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "rbv"},
    {0}, // sentinel: end == 0
};

const board_io_range_t *mdu_io_ranges(void) {
    return mdu_io_ranges_tbl;
}

void mdu_io_bind(mdu_io_t *io, config_t *cfg, const struct m68k_board_desc *desc, void *asc, void *floppy, void *rbv,
                 struct nubus_card *video_card) {
    m68k_board_io_install(io, cfg, desc);
    board_io_bind_dev(io, BOARD_DEV_VIA1, cfg->via1, via_get_memory_interface(cfg->via1));
    board_io_bind_dev(io, BOARD_DEV_SCC, cfg->scc, scc_get_memory_interface(cfg->scc));
    board_io_bind_dev(io, BOARD_DEV_SCSI, cfg->scsi, scsi_get_memory_interface(cfg->scsi));
    board_io_bind_dev(io, BOARD_DEV_ASC, asc, asc_get_memory_interface((asc_t *)asc));
    board_io_bind_dev(io, BOARD_DEV_FLOPPY, floppy, floppy_get_memory_interface((floppy_t *)floppy));
    board_io_bind_dev(io, BOARD_DEV_RBV, rbv, rbv_get_memory_interface((rbv_t *)rbv));
    board_io_bind_dev(io, BOARD_DEV_VDAC, video_card, &mdu_vdac_iface);
}
