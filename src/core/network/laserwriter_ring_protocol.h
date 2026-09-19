// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// laserwriter_ring_protocol.h
// The wire protocol between the emulated LaserWriter's bridge (the
// emulation pthread, C — laserwriter_transport_ring.c) and the browser's
// interpreter worker (JS — app/web2, part 2B), which runs EfterScript's
// platen library in its own non-threaded module.  Both sides read shared
// wasm memory; nothing crosses as a message except the initial attach
// (the control block's address) and the finished PDF, which the worker
// posts to the main thread itself — it never enters this ring.
// MIRRORED in app/web2/src/printer/platenProtocol.ts — keep the two in
// step: bump LWRING_PROTOCOL_VERSION whenever a layout or a record changes
// (both files), and the worker refuses a control block it does not
// understand.
//
// Layout of the region the bridge allocates (one calloc, 64-byte aligned):
//
//   control block   LWRING_CTRL_WORDS uint32 words (indices below)
//   outbound ring   LWRING_OUT_BYTES: records, core -> worker
//   inbound ring    LWRING_IN_BYTES:  records, worker -> core
//
// Each ring is a byte ring of RECORDS: {uint32 kind, uint32 len} followed
// by the payload; `len` counts the header and is a MULTIPLE OF 8
// (LWRING_PAD8 of the header plus the payload; fields inside a payload
// are padded to 4 with LWRING_PAD4).  A record never wraps: when one
// would not fit before the ring's end the writer emits a PAD record whose
// len reaches the end, and the next record starts at offset 0.  The
// 8-byte rule is what makes that PAD possible: every record starts at a
// multiple of 8 (the ring sizes are powers of two), so the remainder
// before the end is never less than the 8-byte header a PAD needs.  A
// reader rejects a len that is not a multiple of 8.  HEAD is the byte
// count the writer has published (monotonic, mod 2^32), TAIL the count
// the reader has consumed; the writer may use (size - (HEAD - TAIL))
// bytes.  The core notifies the worker (Atomics.notify on OUT_HEAD) after
// publishing and on IN_TAIL after consuming; the worker parks in
// Atomics.waitAsync on OUT_HEAD.  The core never waits: the emulation
// thread drains the inbound ring from a guest-time tick
// (laserwriter_transport_poll) and never blocks on the printer.
//
// All multi-byte fields are little-endian uint32 (wasm's byte order);
// text fields are raw bytes, not NUL-terminated, sized by a preceding
// length word.

#ifndef LASERWRITER_RING_PROTOCOL_H
#define LASERWRITER_RING_PROTOCOL_H

#include <stdint.h>

// === Constants ===

#define LWRING_PROTOCOL_VERSION 2u
#define LWRING_MAGIC            0x4C575250u // 'LWRP'

// Control-block word indices (uint32 / Int32Array for Atomics).
#define LWRING_C_MAGIC      0 // LWRING_MAGIC
#define LWRING_C_VERSION    1 // LWRING_PROTOCOL_VERSION
#define LWRING_C_OUT_OFF    2 // byte offset of the outbound ring from the control base
#define LWRING_C_OUT_SIZE   3 // bytes (power of two)
#define LWRING_C_IN_OFF     4 // byte offset of the inbound ring
#define LWRING_C_IN_SIZE    5 // bytes (power of two)
#define LWRING_C_OUT_HEAD   6 // core: outbound bytes published (the worker waits on this word)
#define LWRING_C_OUT_TAIL   7 // worker: outbound bytes consumed
#define LWRING_C_IN_HEAD    8 // worker: inbound bytes published
#define LWRING_C_IN_TAIL    9 // core: inbound bytes consumed (notified for a worker waiting for room)
#define LWRING_C_STATUS     10 // worker: LWRING_STATUS_*
#define LWRING_C_STAT_JOBS  11 // worker: jobs opened (statistics; not read by the core)
#define LWRING_C_STAT_FEEDS 12 // worker: feeds executed
#define LWRING_CTRL_WORDS   32

// Worker status.  The core treats anything but ATTACHED as "not yet":
// records written before the worker attaches are consumed when it does
// (the module is fetched on the first print job), and a job whose open
// outlives LASERWRITER_OPEN_TIMEOUT_NS (laserwriter_job.c) fails.  LOST
// means the worker gave up for good (the module failed to load or the
// worker crashed): every outstanding request fails at the next poll.
#define LWRING_STATUS_DETACHED 0u
#define LWRING_STATUS_ATTACHED 1u
#define LWRING_STATUS_LOST     2u

// Ring sizes.  Outbound holds an OPEN (the prelude, ~2 KB) and FEEDs of
// at most one PAP flow quantum each; the bridge keeps one request
// outstanding, so the ring is never near full unless the worker stalls.
// Inbound holds the worker's replies; a FED/FINISHED text field is capped
// at LWRING_TEXT_MAX so every record fits.  A test may override the sizes
// (-DLWRING_OUT_BYTES=...) to force wraps; the worker reads the sizes
// from the control block, never from these constants.
#ifndef LWRING_OUT_BYTES
#define LWRING_OUT_BYTES (64u << 10)
#endif
#ifndef LWRING_IN_BYTES
#define LWRING_IN_BYTES (256u << 10)
#endif

// Bytes in one FEED: one PAP flow quantum (Inside AppleTalk 2e ch. 10,
// "PAP specifications for the Apple LaserWriter printer": flow quantum 8,
// 512 bytes per ATP data packet).
#define LWRING_FEED_MAX (8u * 512u)

// Cap on each text field of an inbound record (reply bytes, error bytes,
// error name, offending command).  The worker truncates a longer channel
// and sets LWRING_FED_F_TRUNCATED; the bridge logs the loss.
#define LWRING_TEXT_MAX (64u << 10)

// Cap on the job title carried by FINISH (the bridge's %%Title: name,
// LASERWRITER_TITLE_MAX); the writer cuts a longer one.
#define LWRING_TITLE_MAX 63u

// Record header: {kind, len}.  A record's total length is LWRING_PAD8 of
// the header plus its payload.
#define LWRING_HDR_BYTES 8u

// === Records, core -> worker ===

#define LWRING_R_PAD 0u // skip to the ring start (either direction)

// OPEN {job_id, compress, embed_all_fonts, step_budget_lo, step_budget_hi,
//       server_password (int32), identity_count, identity_bytes, prelude_len}
//      + identity block + prelude bytes.
// The platen_config the bridge sets, field by field.  The identity block
// is identity_count pairs of NUL-terminated strings (key, value — the
// value a PostScript literal, as platen_entry takes them), identity_bytes
// long in total; the prelude follows immediately (unterminated, padded
// to 4 at the record's end).  The worker builds the platen_config from
// these and calls platen_job_new; it answers OPENED or OPEN_FAILED.
#define LWRING_R_OPEN        1u
#define LWRING_OPEN_JOB      0
#define LWRING_OPEN_COMPRESS 1
#define LWRING_OPEN_EMBED    2
#define LWRING_OPEN_BUDGET_L 3
#define LWRING_OPEN_BUDGET_H 4
#define LWRING_OPEN_PASSWORD 5
#define LWRING_OPEN_ID_COUNT 6
#define LWRING_OPEN_ID_BYTES 7
#define LWRING_OPEN_PRELUDE  8
#define LWRING_OPEN_WORDS    9

// FEED {job_id, sequence, len} + len bytes of the program, at most
// LWRING_FEED_MAX.  `sequence` is the PAP SendData sequence the bytes
// answered (Inside AppleTalk 2e ch. 10, "Duplicate filtration"); the
// worker echoes it in FED so the bridge matches acknowledgement to
// request.  The worker calls platen_job_feed, drains both channels, and
// answers FED.
#define LWRING_R_FEED     2u
#define LWRING_FEED_JOB   0
#define LWRING_FEED_SEQ   1
#define LWRING_FEED_LEN   2
#define LWRING_FEED_WORDS 3

// FINISH {job_id, title_len} + title bytes (padded to 4): end of data.
// The title is the job name the bridge scanned from the driver's %%Title:
// line ("" when absent), at most LWRING_TITLE_MAX bytes: the worker names
// the download from it and the job id (<job id, 5 digits>-<title>.pdf),
// since the PDF never passes through the core in the browser.  The worker
// calls platen_job_finish, drains both channels, posts the PDF to the main
// thread, and answers FINISHED; then frees the job.
#define LWRING_R_FINISH         3u
#define LWRING_FINISH_JOB       0
#define LWRING_FINISH_TITLE_LEN 1
#define LWRING_FINISH_WORDS     2

// ABANDON {job_id}: the connection went away mid-job.  The worker frees
// the job without finishing and answers nothing; a reply already on its
// way for that job is dropped by the bridge (job id mismatch).
#define LWRING_R_ABANDON     4u
#define LWRING_ABANDON_JOB   0
#define LWRING_ABANDON_WORDS 1

// === Records, worker -> core ===

// OPENED {job_id}: platen_job_new succeeded; the job takes feeds.
#define LWRING_R_OPENED     16u
#define LWRING_OPENED_JOB   0
#define LWRING_OPENED_WORDS 1

// OPEN_FAILED {job_id, text_len} + text: platen_job_new returned NULL
// (text = platen_last_error), or the worker could not load its module.
#define LWRING_R_OPEN_FAILED     17u
#define LWRING_OPEN_FAILED_JOB   0
#define LWRING_OPEN_FAILED_TEXT  1
#define LWRING_OPEN_FAILED_WORDS 2

// FED {job_id, sequence, status, pages, reply_len, error_len, flags}
//     + reply bytes + error bytes (each padded to 4).
// `status` is the feed's outcome: WAITING (PLATEN_OK: the job waits for
// more), ENDED (PLATEN_DONE: the job ended before its data did; later
// feeds are discarded and FINISH reports why), FAILED (a negative code;
// the job is unusable and the bridge aborts).  `pages` is
// platen_job_pages after the feed (the status line's page count).  The
// reply bytes are the program's standard output produced by this feed,
// the error bytes its standard error — a query completed by this feed is
// answered here, which is why the bridge answers no read credit with a
// status line while a FED is outstanding.
#define LWRING_R_FED         18u
#define LWRING_FED_JOB       0
#define LWRING_FED_SEQ       1
#define LWRING_FED_STATUS    2
#define LWRING_FED_PAGES     3
#define LWRING_FED_REPLY_LEN 4
#define LWRING_FED_ERROR_LEN 5
#define LWRING_FED_FLAGS     6
#define LWRING_FED_WORDS     7

#define LWRING_FEED_WAITING 0u
#define LWRING_FEED_ENDED   1u
#define LWRING_FEED_FAILED  2u

#define LWRING_FED_F_TRUNCATED (1u << 0) // a channel exceeded LWRING_TEXT_MAX and was cut

// FINISHED {job_id, outcome, pages, error_name_len, offending_len,
//           reply_len, error_len, flags}
//          + error name + offending command + reply bytes + error bytes
//          (each padded to 4).
// `outcome` is platen_job_finish's: OK, ERROR (error name and offending
// command set), BUDGET, or FAILED (a negative code: no document; the
// error name field carries platen_last_error).  `pages` is
// platen_job_pages of the finished document.  The reply and error bytes
// are what the completion produced.  The PDF itself is not here: the
// worker posts it to the main thread (job id, title-less; the page names
// the download from the job id and the title the bridge reports).
#define LWRING_R_FINISHED           19u
#define LWRING_FINISHED_JOB         0
#define LWRING_FINISHED_OUTCOME     1
#define LWRING_FINISHED_PAGES       2
#define LWRING_FINISHED_ERRNAME_LEN 3
#define LWRING_FINISHED_OFFEND_LEN  4
#define LWRING_FINISHED_REPLY_LEN   5
#define LWRING_FINISHED_ERROR_LEN   6
#define LWRING_FINISHED_FLAGS       7
#define LWRING_FINISHED_WORDS       8

#define LWRING_OUTCOME_OK     0u
#define LWRING_OUTCOME_ERROR  1u
#define LWRING_OUTCOME_BUDGET 2u
#define LWRING_OUTCOME_FAILED 3u

// === Helpers ===

// Bytes a text field of `n` occupies inside a payload (padded to 4).
#define LWRING_PAD4(n) (((uint32_t)(n) + 3u) & ~3u)

// A record's total length for `n` bytes of header plus payload (padded to
// 8: the framing rule above).
#define LWRING_PAD8(n) (((uint32_t)(n) + 7u) & ~7u)

#endif // LASERWRITER_RING_PROTOCOL_H
