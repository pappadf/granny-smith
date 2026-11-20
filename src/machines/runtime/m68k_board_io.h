// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// m68k_board_io.h
// The 68k board-descriptor face of the I/O engine (board_io.h).
//
// The engine walks an ordered, sentinel-terminated table of
// `board_io_range_t` windows -- the address map expressed as DATA rather than
// a hand-written if-ladder.  Every 68k II/Quadra family (GLUE, MDU+RBV, OSS,
// MCU, AV) installs its own range table + mirror mask + device set into a
// `board_io_t` at bind time, through the board descriptor (m68k_board.h).
// This makes the address map directly unit-testable and kills the formerly
// cloned ~600-line dispatchers.

#ifndef GS_MACHINES_RUNTIME_M68K_BOARD_IO_H
#define GS_MACHINES_RUNTIME_M68K_BOARD_IO_H

#include "board_io.h"
#include "system_internal.h"

struct m68k_board_desc;

// Install a board's window table, mirror mask, unmapped-read value and
// bus-error span (board_io_install_table plus the descriptor's window base
// and io_berr_*), clearing every device slot.  Every 68k family bind opens
// with this; the family then
// fills only the slots its own chips occupy.  Pass the shared base descriptor
// (`&desc->common` for the families that wrap it).
void m68k_board_io_install(board_io_t *io, struct config *cfg, const struct m68k_board_desc *desc);

#endif // GS_MACHINES_RUNTIME_M68K_BOARD_IO_H
