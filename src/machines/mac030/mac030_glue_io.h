// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue_io.h
// The 68k board-descriptor face of the I/O engine (mac030_io.h) plus the GLUE
// family's tables.
//
// The engine walks an ordered, sentinel-terminated table of
// `mac030_io_range_t` windows -- the address map expressed as DATA rather than
// a hand-written if-ladder.  GLUE (SE/30, IIcx, IIx) and MDU+RBV (IIci, IIsi)
// both use it, each installing its own range table + mirror mask + device set
// into a `mac030_io_t` at bind time.  This makes the address map directly
// unit-testable and kills the formerly cloned ~600-line dispatchers.

#ifndef GS_MACHINES_MAC030_GLUE_IO_H
#define GS_MACHINES_MAC030_GLUE_IO_H

#include "mac030_io.h"
#include "memory.h"
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

// The canonical GLUE $50Fxxxxx island repeats every 128 KB; the 6522s decode
// only A9..A12, so the live decode is the low 17 bits.  (MDU uses an 18-bit
// $40000 mirror — see mdu_io.c.)
#define MAC030_GLUE_IO_MIRROR 0x0001FFFFUL

// Backwards-compatible alias: the GLUE state struct calls its field's type
// mac030_glue_io_t.  (The engine is family-neutral; the name is historical.)
typedef mac030_io_t mac030_glue_io_t;

struct mac030_board_desc;

// Install a board's window table, mirror mask, unmapped-read value and
// bus-error span (mac030_io_install_table plus the descriptor's window base
// and io_berr_*), clearing every device slot.  Every 68k family bind opens
// with this; the family then
// fills only the slots its own chips occupy.  Pass the shared base descriptor
// (`&desc->common` for the families that wrap it).
void mac030_io_install(mac030_io_t *io, struct config *cfg, const struct mac030_board_desc *desc);

// --- The GLUE family's tables ----------------------------------------------

// Cache the GLUE device interfaces and install the board's window table +
// mirror + unmapped-read value.  Call after rtc/scc/via/scsi/asc/floppy are up,
// before installing the map.
void mac030_glue_io_bind(mac030_io_t *io, config_t *cfg, const struct mac030_board_desc *desc, void *asc, void *floppy);

// The GLUE family's ordered window table (sentinel-terminated).  Exposed for
// the address-map unit test and for GLUE machines' board descriptors.
extern const mac030_io_range_t glue_io_ranges[];
const mac030_io_range_t *mac030_glue_io_ranges(void);

#endif // GS_MACHINES_MAC030_GLUE_IO_H
