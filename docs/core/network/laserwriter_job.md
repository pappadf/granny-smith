# LaserWriter job bridge (`laserwriter_job.c`)

The PostScript side of the emulated LaserWriter. Everything about *acting
like* a LaserWriter lives in Granny Smith; the PostScript-to-PDF mechanism
is EfterScript's `platen` library — a per-job interpreter with a C ABI
(`platen.h`). This module maps one PAP job onto one platen job and moves
bytes between the two, asynchronously, through a *transport*
(`laserwriter_transport.h`). `appletalk_printer.c` owns the PAP session and
calls in here; `laserwriter_job.h` is the interface.

Built only with `PLATEN=1` (`-DGS_PLATEN=1`, see
[`laserwriter.md`](laserwriter.md)). Without it every entry is a stub and
`laserwriter_job_available()` returns false, so the printer keeps its
spool-only behaviour and the placeholder query answers.

## The transport: where the interpreter runs

The bridge never calls the library itself. It issues four requests through
`laserwriter_transport` — **open**, **feed**, **finish**, **abandon** — and
receives four results as callbacks — **opened** / **open_failed**,
**fed**, **finished** — always later, never from inside the request call.
One implementation is linked per build:

- **direct** (`laserwriter_transport_direct.c`, headless): the platen
  library is linked into the process. Each request is queued and executed
  from a scheduler event 1 ms of guest time later
  (`LASERWRITER_DIRECT_DELAY_NS`), and its result is delivered from that
  event. The finished document's bytes ride in the `finished` result and
  go to the platform sink.
- **ring** (`laserwriter_transport_ring.c`, browser): the interpreter runs
  in a Web Worker with its own non-threaded module; the two sides share a
  control block and two byte rings in the wasm heap
  ([`laserwriter_ring_protocol.h`](../../../src/core/network/laserwriter_ring_protocol.h)
  documents every word and record). Requests are records in the outbound
  ring; results are records the bridge drains from the inbound ring in
  `laserwriter_transport_poll()`. The PDF never enters the ring: the worker
  posts it to the page, which downloads it. The platform provides two hooks
  (`laserwriter_ring_attach_requested`, `laserwriter_ring_notify`; weak
  no-ops in the transport, overridden in `em_main.c`: the first fires
  `Module.onPrinterAttach`, the second `emscripten_futex_wake`). The FINISH
  record carries the bridge's `%%Title:` name so the worker can name the
  download; the browser side is
  [`laserwriter.md`](laserwriter.md) §5.5.

So the headless acceptance row exercises the same asynchronous bridge the
browser runs: the shape of the conversation is identical, only the
turnaround differs.

While a request is outstanding the bridge runs a 1 ms guest-time tick
(`LASERWRITER_POLL_NS`) that calls `laserwriter_transport_poll()` — the
ring transport delivers from it; the direct one has already delivered from
its own event — and enforces the open deadline: an OPEN unanswered for
`LASERWRITER_OPEN_TIMEOUT_NS` (120 s; the browser fetches the interpreter
module on the first print job) fails the job.

## One PAP job, one platen job

The LaserWriter serves one connection at a time, and the driver ends each
unit of work with a PAP EOF, so the bridge holds exactly one job, in one of
five states: idle, opening, ready, feeding, finishing.

- **begin** (`laserwriter_job_begin`) — at PAP OpenConn for the first job,
  and on the first data of each later EOF-delimited job on the same
  connection. Issues OPEN with the configuration: the identity entries
  (product/version/revision), the prelude, compression on, embed-all off,
  and the execution budget. The job is *opening* until **OPENED**
  (`LASERWRITER_EVENT_OPENED`); an **OPEN_FAILED** raises
  `LASERWRITER_EVENT_FAILED` with the library's reason.
- **feed** (`laserwriter_job_feed`) — one SendData transaction's data, at
  most one flow quantum, with the SendData sequence it answered. The
  interpreter handles piece boundaries (it suspends mid-token and resumes
  on the next feed), so the bridge never reframes the stream. Each feed
  also scans for the driver's `%%Title:` comment. The job is *feeding*
  until **FED** (`LASERWRITER_EVENT_FED`), which carries that feed's
  replies (standard output) and error reports (standard error, already in
  `%%[ Error: … ]%%` form), the page count so far, and the feed's status
  (waiting, ended early, failed).
- **read output** (`laserwriter_job_read_output`) — the PAP layer drains
  what a FED or FINISHED queued and hands it to the workstation's read
  credits. A PostScript *query* is just a job whose program prints its
  answer; there is no query heuristic here.
- **finish** (`laserwriter_job_finish`) — at the workstation's EOF. The
  job is *finishing* until **FINISHED** (`LASERWRITER_EVENT_FINISHED`):
  the outcome, the pages, the completion's output, and (direct only) the
  document, which goes to the platform sink; the counters move; the job is
  freed. An error or budget outcome still keeps its document (the pages
  shown before the error are in it) and is logged with the error name and
  offending command.
- **abort** (`laserwriter_job_abort`) — a connection torn down mid-job
  issues ABANDON: the interpreter is freed without finishing, no document
  is produced, and any result still on its way for that job id is dropped.

One request is outstanding at a time. The PAP layer asks
`laserwriter_job_ready()` before reading more data and
`laserwriter_job_feed_pending()` before answering a read credit with a
status line (see [`appletalk_printer.md`](appletalk_printer.md) §6.3a for
the two PAP rules this gives).

## Status and observability

`laserwriter_job_status` composes the PAP status string from the job's
facts: `status: idle` with no job, `status: starting up` while the open is
unanswered (an OpenConn is acknowledged at once, before the interpreter
is), `status: busy …; job: <name>` once a job is running, and `status:
printing …; page: <n>` once the acknowledged page count advances. The PAP
layer keeps the read-driven credit model and answers the driver's status
reads with this string.

The object model (`appletalk.printer`) exposes `interpreter` (is the
interpreter linked), `status`, `capture` (also write the PostScript to the
spool file), `documents`, `last_pages`, and `last_outcome` (`ok`, `error:
<name> in <command>`, `budget`, or `failed: <reason>`).

## The document sink

`laserwriter_sink_document` is a weak symbol the platform overrides:

- **headless** (`headless_main.c`) writes `<print-dir>/<job>-<title>.pdf`
  under `--print-dir` / `$GS_PRINT_DIR` and logs pages and any error.
- **wasm** has no override: the ring transport never produces bytes here —
  the interpreter worker posts the PDF to the page, which downloads it
  ([`laserwriter.md`](laserwriter.md) §5.5). The document count still moves.
- the default (no platform override) logs and drops the document.

## The prelude and identity

The interpreter is seeded from [`laserwriter_prelude.ps`](laserwriter_prelude.ps),
embedded at build time as a C array (`laserwriter.mk` → `bin2c.py` →
`build/laserwriter/laserwriter_prelude.h`), and three `statusdict` identity
entries derived from the same product/version/revision values, so
`statusdict` and `systemdict` agree. Both travel in the OPEN request (the
ring transport copies them into the record; the direct one into its own
buffers), so the browser's worker needs nothing baked in. The prelude
defines the `statusdict` timeouts, the Level 1 page-size procedures, and
the userdict page-type names a LaserWriter provides; it is written from the
published language reference, not from any driver. It originally lived in
gs-test-data; it now lives in the repository as the emulator's own asset.
