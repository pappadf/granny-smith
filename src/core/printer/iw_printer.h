// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iw_printer.h
// The virtual ImageWriter: one dot-matrix printer per machine,
// `machine.imagewriter`, connected to a serial port (the guest's own
// ImageWriter driver talks to it through the SCC) or, as an ImageWriter II
// with the LocalTalk Option card, to the AppleTalk network (PAP).  Whatever
// feeds it, the bytes go to one interpreter (iw_interp.h); a job ends when
// the input goes quiet (serial), at the PAP EOF (AppleTalk) or on eject(),
// and becomes one PDF handed to the platform's printer sink.

#ifndef IW_PRINTER_H
#define IW_PRINTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "checkpoint.h"
#include "common.h"
#include "iw_interp.h"
#include "scc.h"

struct scheduler;
typedef struct iw_printer iw_printer_t;

// Where the printer is plugged in
typedef enum {
    IW_CONN_NONE = 0,
    IW_CONN_SERIAL_A, // SCC channel A: the Mac modem port, the Lisa's Serial A
    IW_CONN_SERIAL_B, // SCC channel B: the Mac printer port, the Lisa's Serial B
    IW_CONN_LOCALTALK, // the LocalTalk Option card (ImageWriter II only)
    IW_CONN_COUNT
} iw_connection_t;

// How the machine wires a printer's ready line (its DTR) into each SCC
// channel: which input, and the level it shows when the printer is ready.
typedef struct {
    bool wired;
    scc_pin_t pin;
    bool ready_level;
} iw_port_wiring_t;

// Where a job's title comes from: the name of the guest's foreground
// application as UTF-8 in `out` (`cap` bytes), false when there is none.
typedef bool (*iw_title_fn)(char *out, size_t cap);

// Build the machine's printer.  `wiring[0]`/`[1]` describe SCC channels A
// and B.  With `checkpoint`, the printer restores its state from it.
iw_printer_t *iw_printer_new(struct scheduler *scheduler, scc_t *scc, const iw_port_wiring_t wiring[2],
                             checkpoint_t *checkpoint);

// The machine's title source (a Macintosh reads CurApName); jobs without
// one are titled "Print".
void iw_printer_set_title_source(iw_printer_t *p, iw_title_fn fn);

// The machine configuration's printer, at a fresh boot: `model` powered on and
// plugged into `conn`.  False when the combination is impossible (the
// LocalTalk card on an ImageWriter, or another printer already on LocalTalk).
bool iw_printer_install(iw_printer_t *p, iw_model_t model, iw_connection_t conn);

void iw_printer_delete(iw_printer_t *p);

void iw_printer_checkpoint(iw_printer_t *p, checkpoint_t *checkpoint);

// Bytes for the printer from the AppleTalk front end: interpreted now.
// A job starts with the first byte.
void iw_printer_feed(iw_printer_t *p, const uint8_t *data, size_t len);

// End the current job now (PAP EOF, eject()): the document goes to the sink.
void iw_printer_end_job(iw_printer_t *p);

// The machine's printer while it is connected to LocalTalk, else NULL: the
// PAP server's ImageWriter personality prints on it.
iw_printer_t *iw_printer_localtalk(void);

// Status for the PAP status word and the UI
bool iw_printer_paper_out(const iw_printer_t *p);
bool iw_printer_selected(const iw_printer_t *p);
bool iw_printer_sheet_feeder(const iw_printer_t *p);
bool iw_printer_busy(const iw_printer_t *p); // a job is in progress
const char *iw_printer_status(const iw_printer_t *p);
const char *iw_printer_name(const iw_printer_t *p);

#endif // IW_PRINTER_H
