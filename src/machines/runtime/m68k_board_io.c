// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// m68k_board_io.c
// The 68k board-descriptor install for the I/O engine (board_io.c) and the
// shared IRQ→IPL routing table.  Data and one install; no device code, so
// the address-map unit test links it beside the engine and the family tables.

#include "m68k_board_io.h"

#include "m68k_board.h" // machine_irq_route_t + M68K_BOARD_IRQ_* + m68k_board_desc_t

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
// IRQ→IPL routing (the GLUE routing, shared by the MDU and MCU)
// ============================================================

// Ordered highest-IPL-first: NMI→7, SCC→4, VIA2→2, VIA1→1.
static const machine_irq_route_t board_irq_routes[] = {
    {M68K_BOARD_IRQ_NMI,  7},
    {M68K_BOARD_IRQ_SCC,  4},
    {M68K_BOARD_IRQ_VIA2, 2},
    {M68K_BOARD_IRQ_VIA1, 1},
    {0,                   0}, // sentinel
};

const machine_irq_route_t *m68k_board_irq_routes(void) {
    return board_irq_routes;
}
