// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// glue_io.h
// The GLUE family's tables for the I/O engine (board_io.h), installed through
// the board-descriptor face (m68k_board_io.h).

#ifndef GS_MACHINES_GLUE_GLUE_IO_H
#define GS_MACHINES_GLUE_GLUE_IO_H

#include "board_io.h"
#include "memory.h"
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

// The canonical GLUE $50Fxxxxx island repeats every 128 KB; the 6522s decode
// only A9..A12, so the live decode is the low 17 bits.  (MDU uses an 18-bit
// $40000 mirror — see mdu_io.c.)
#define GLUE_IO_MIRROR 0x0001FFFFUL

struct m68k_board_desc;

// --- The GLUE family's tables ----------------------------------------------

// Cache the GLUE device interfaces and install the board's window table +
// mirror + unmapped-read value.  Call after rtc/scc/via/scsi/asc/floppy are up,
// before installing the map.
void glue_io_bind(board_io_t *io, config_t *cfg, const struct m68k_board_desc *desc, void *asc, void *floppy);

// The GLUE family's ordered window table (sentinel-terminated).  Exposed for
// the address-map unit test and for GLUE machines' board descriptors.
extern const board_io_range_t glue_io_ranges_tbl[];
const board_io_range_t *glue_io_ranges(void);

#endif // GS_MACHINES_GLUE_GLUE_IO_H
