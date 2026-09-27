// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// imagewriter.h
// An Apple ImageWriter-class dot-matrix printer on a serial port: the far
// end of the cable from one Z8530 SCC channel.  It holds the port's "ready"
// handshake line asserted (a printer that is on, selected, with paper),
// collects every byte the guest transmits in asynchronous mode, and ends a
// job when the line has been quiet for a while -- the ImageWriter protocol
// has no end-of-job marker, so a gap is all a printer ever sees.  Each job
// goes to a platform sink as the raw ImageWriter command stream.
//
// Machines fit it as an on-board device: the Lisa wires one to Serial A.
// See docs/core/peripherals/imagewriter.md.

#ifndef IMAGEWRITER_H
#define IMAGEWRITER_H

// === Includes ===
#include "common.h"
#include "scc.h"

// === Forward Declarations ===
struct scheduler;

// === Type Definitions ===
struct imagewriter;
typedef struct imagewriter imagewriter_t;

// A finished job handed to the platform sink.  The bytes are valid only for
// the duration of the call; the sink copies what it keeps.
typedef struct {
    uint32_t job_id; // 1, 2, ... per machine
    const uint8_t *data; // the ImageWriter command stream as the guest sent it
    size_t len;
    bool truncated; // the job outgrew IMAGEWRITER_JOB_MAX and was cut there
} imagewriter_job_t;

// === Constants ===

// Guest time without a byte after which the current job is over.  A driver
// streams a page at line speed and pauses only to render the next band, so
// a gap this long means the document is done.
#define IMAGEWRITER_IDLE_NS (5ull * 1000000000ull)

// Largest job kept; a longer one is handed over at this size and the rest
// starts a new job (a runaway stream must not grow without bound).
#define IMAGEWRITER_JOB_MAX (8u << 20)

// === Lifecycle (Constructor / Destructor) ===

// Plugs a printer into SCC channel `ch` (0 = A, 1 = B).  `ready_pin` is the
// channel input the port's cable wires to the printer's ready line and
// `ready_asserted` the level that means "ready" to the guest's driver; the
// printer drives it at once and keeps it there.  The job timer is
// registered on `scheduler` so a checkpointed queue finds it.  Creates the
// `printer` object under the machine node.
imagewriter_t *imagewriter_init(scc_t *scc, unsigned int ch, scc_pin_t ready_pin, bool ready_asserted,
                                struct scheduler *scheduler);

// Unplugs the printer (a job in progress is handed over first) and frees it.
void imagewriter_delete(imagewriter_t *iw);

// === Operations ===

// Ends the current job now, as if the line had gone quiet.  No-op without one.
void imagewriter_flush(imagewriter_t *iw);

// Jobs handed to the sink so far.
uint32_t imagewriter_jobs(const imagewriter_t *iw);

// Bytes received since the printer was plugged in.
uint64_t imagewriter_bytes(const imagewriter_t *iw);

// Bytes of the job in progress (0 between jobs).
size_t imagewriter_pending(const imagewriter_t *iw);

// Platform sink for a finished job.  The weak default in imagewriter.c logs
// and drops it; headless_main.c writes <print-dir>/iw-<job>.iw.
void imagewriter_sink_job(const imagewriter_job_t *job);

#endif // IMAGEWRITER_H
