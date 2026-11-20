// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// glue_io.c
// The 68k board-descriptor install for the I/O engine (board_io.c) and the
// GLUE family's dispatch tables.  GLUE (here) and MDU+RBV (mdu_io.c) each
// install their own table + mirror + device set.  The decode, per-window bus
// penalties, and IRQ priority are the SE/30 / IIcx logic verbatim (those were
// byte- and behaviour-identical); only the dispatch is table-driven.

#include "glue_io.h"

#include "m68k_board.h" // machine_irq_route_t + M68K_BOARD_IRQ_* + m68k_board_desc_t

#include "asc.h"
#include "floppy.h"
#include "scc.h"
#include "scsi.h"
#include "via.h"

#include <stdbool.h>

// The descriptor form of board_io_install_table: the table, mirror and
// unmapped value, plus the window base and the bus-error span.
void m68k_board_io_install(board_io_t *io, config_t *cfg, const struct m68k_board_desc *desc) {
    board_io_install_table(io, cfg, desc->io_ranges, desc->io_mirror_mask, desc->io_unmapped_read);
    io->window_base = desc->rom_end;
    if (desc->io_berr_hi > desc->io_berr_lo) {
        io->berr_lo = desc->io_berr_lo;
        io->berr_hi = desc->io_berr_hi;
    }
}

// ============================================================
// GLUE family tables
// ============================================================

// Per-window access penalties (cycles): VIA syncs to the E-clock (~16),
// everything else costs ~2 on top of the ~4 baseline.
#define GLUE_VIA_IO_PENALTY  16
#define GLUE_SCC_IO_PENALTY  2
#define GLUE_SCSI_IO_PENALTY 2
#define GLUE_ASC_IO_PENALTY  2
#define GLUE_SWIM_IO_PENALTY 2

// The canonical GLUE $50Fxxxxx decode, expressed as data.  Windows are
// half-open [base, end); the gap [$08000,$10000) and anything past $18000 are
// unmapped (read 0 / write ignored).  SCSI {DRQ,BLIND} are the pseudo-DMA
// "blind" registers: every access hits one fixed NCR5380 register — reads pop
// the FIFO at reg 0, writes push at reg $201 — regardless of the byte offset
// within the window (BOARD_IO_FIXED).  The two VIA windows mask A0 because the
// 6522 rides lane 0 and ignores it (BOARD_IO_MASK_A0).
//
//   base     end      device            penalty               xform               rd  wr     name
const board_io_range_t glue_io_ranges_tbl[] = {
    {0x00000, 0x02000, BOARD_DEV_VIA1, GLUE_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x02000, 0x04000, BOARD_DEV_VIA2, GLUE_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via2", .esync = 1},
    {0x04000, 0x06000, BOARD_DEV_SCC, GLUE_SCC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x06000, 0x08000, BOARD_DEV_SCSI, GLUE_SCSI_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_drq"},
    {0x10000, 0x12000, BOARD_DEV_SCSI, GLUE_SCSI_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scsi_reg"},
    {0x12000, 0x14000, BOARD_DEV_SCSI, GLUE_SCSI_IO_PENALTY, BOARD_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_blind"},
    {0x14000, 0x16000, BOARD_DEV_ASC, GLUE_ASC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "asc"},
    {0x16000, 0x18000, BOARD_DEV_FLOPPY, GLUE_SWIM_IO_PENALTY, BOARD_IO_STRIDE_512, 0, 0, NULL, NULL, "swim"},
    {0}, // sentinel: end == 0
};

const board_io_range_t *glue_io_ranges(void) {
    return glue_io_ranges_tbl;
}

void glue_io_bind(board_io_t *io, config_t *cfg, const struct m68k_board_desc *desc, void *asc, void *floppy) {
    m68k_board_io_install(io, cfg, desc);
    board_io_bind_dev(io, BOARD_DEV_VIA1, cfg->via1, via_get_memory_interface(cfg->via1));
    board_io_bind_dev(io, BOARD_DEV_VIA2, cfg->via2, via_get_memory_interface(cfg->via2));
    board_io_bind_dev(io, BOARD_DEV_SCC, cfg->scc, scc_get_memory_interface(cfg->scc));
    board_io_bind_dev(io, BOARD_DEV_SCSI, cfg->scsi, scsi_get_memory_interface(cfg->scsi));
    board_io_bind_dev(io, BOARD_DEV_ASC, asc, asc_get_memory_interface((asc_t *)asc));
    board_io_bind_dev(io, BOARD_DEV_FLOPPY, floppy, floppy_get_memory_interface((floppy_t *)floppy));
}

// ============================================================
// IRQ→IPL routing (the GLUE family's second dispatch table)
// ============================================================

// Ordered highest-IPL-first: NMI→7, SCC→4, VIA2→2, VIA1→1.
static const machine_irq_route_t glue_irq_routes[] = {
    {M68K_BOARD_IRQ_NMI,  7},
    {M68K_BOARD_IRQ_SCC,  4},
    {M68K_BOARD_IRQ_VIA2, 2},
    {M68K_BOARD_IRQ_VIA1, 1},
    {0,                   0}, // sentinel
};

const machine_irq_route_t *m68k_board_irq_routes(void) {
    return glue_irq_routes;
}
