# LaserWriter job bridge (`laserwriter_job.c`)

The PostScript side of the emulated LaserWriter. Everything about *acting
like* a LaserWriter lives in Granny Smith; the PostScript-to-PDF mechanism
is EfterScript's `platen` library — a printer that keeps its interpreter
between jobs, with a C ABI (`platen.h`). This module maps one PAP job onto
one platen job on the emulated machine's printer and moves bytes between
the two, asynchronously, through a *transport*
(`laserwriter_transport.h`). `appletalk_printer.c` owns the PAP session and
calls in here; `laserwriter_job.h` is the interface.

Built only with `PLATEN=1` (`-DGS_PLATEN=1`, see
[`laserwriter-session.md`](../../../reference/protocols/laserwriter-session.md)). Without it every entry is a stub and
`laserwriter_job_available()` returns false, so the printer keeps its
spool-only behaviour and the placeholder query answers.

## The transport: where the interpreter runs

The bridge never calls the library itself. It issues five requests through
`laserwriter_transport` — **open** (a job on a named printer), **feed**,
**finish**, **abandon**, **printer_free** — and receives four results as
callbacks — **opened** / **open_failed**, **fed**, **finished** — always
later, never from inside the request call. Printers are named by the
bridge (below); a transport creates a printer's interpreter on the first
open for its id and keeps it until printer_free.
One implementation is linked per build:

- **direct** (`laserwriter_transport_direct.c`, headless): the platen
  library is linked into the process, and printers are `platen_printer`
  objects in a small table keyed by id. Each request is queued and executed
  from a scheduler event 1 ms of guest time later
  (`LASERWRITER_DIRECT_DELAY_NS`), and its result is delivered from that
  event. The finished document's bytes ride in the `finished` result and
  go to the platform sink.
- **ring** (`laserwriter_transport_ring.c`, browser): the interpreter runs
  in a Web Worker with its own non-threaded module; the two sides share a
  control block and two byte rings in the wasm heap
  ([`laserwriter_ring_protocol.h`](../../../../src/core/network/laserwriter_ring_protocol.h)
  documents every word and record). Requests are records in the outbound
  ring; results are records the bridge drains from the inbound ring in
  `laserwriter_transport_poll()`. The worker keeps one `platen_printer` per
  id. The PDF never enters the ring: the worker posts it to the page, which
  shows it in the viewer dialog. The platform provides two hooks
  (`laserwriter_ring_attach_requested`, `laserwriter_ring_notify`; weak
  no-ops in the transport, overridden in `em_main.c`: the first fires
  `Module.onPrinterAttach`, the second `emscripten_futex_wake`). The FINISH
  record carries the bridge's `%%Title:` name so the worker can name the
  download; the browser side is
  [`laserwriter-session.md`](../../../reference/protocols/laserwriter-session.md) §5.5.

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
  issues ABANDON: the job is freed without finishing — its printer reverts
  it, keeping only what it had already made permanent — no document is
  produced, and any result still on its way for that job id is dropped.
  A connection closed cleanly *after* its EOF is not mid-job: the PAP
  layer keeps the job (`laserwriter_job_finishing()` says the FINISH is
  out) and lets it finish detached, dropping its output with
  `laserwriter_job_discard_output()` (see
  [`pap.md`](../../../reference/protocols/pap.md) §6.3a).

One request is outstanding at a time. The PAP layer asks
`laserwriter_job_ready()` before reading more data, and answers the
driver's read credits only with the job's output (see
[`pap.md`](../../../reference/protocols/pap.md) §6.3a for the PAP rules
this gives).

## The printer

Every job runs on the network's **printer**: an interpreter
(`platen_printer`) built once from the identity and prelude and kept
between jobs. Each job starts from the printer's state and is reverted at
its end, except what it made permanent with `exitserver` or `startjob`,
which every later job inherits — the way a LaserWriter keeps a downloaded
procset until it is switched off. The classic driver relies on it: it asks
whether PatchPrep is resident and uploads it with `exitserver` only when
not, so from the second print on the upload is skipped.

The LaserWriter is a node on the AppleTalk network, not part of the Mac
(`appletalk.h`, "Lifecycle"). It lives as long as the Mac it serves stays
on the cable; a change of machine restarts it, so no job runs on from one
machine into the next:

| Event | Printer |
|---|---|
| `machine.boot` (a new machine) | new |
| `machine.restart` (the same machine power-cycled; nothing torn down) | kept |
| `machine.reset` (warm reset) | kept |
| `checkpoint.load` that succeeds | new (the printer is never in a checkpoint) |
| `checkpoint.load` that fails | kept: the running machine never left the cable |
| `appletalk.printer.restart()` | new; the machine is untouched |
| an interpreter wedged by an earlier job | replaced, logged |

A new printer is what `appletalk.printer.restart()` makes
(`atalk_printer_plug(NULL)`, when a machine leaves the cable): the PAP
session goes, any job in flight is abandoned -- one finishing after its
connection closed included -- and what earlier jobs made permanent goes
with the interpreter. The printer's name, enabled state and capture setting
are the host's configuration and stay.

The bridge names printers with a process-unique id (`g_lw.printer_id`,
never reused; 0 until the first job) and passes it with every OPEN, so the
interpreter itself is created on the printer's first job and a printer
that never prints costs nothing. Only `appletalk.printer.restart()` moves
the id: `laserwriter_printer_retire()` abandons the job, sends
PRINTER_FREE, and the next job gets a new id (`laserwriter_job.h`, "The
printer").

A printer whose `platen_printer_job` fails is *wedged*: an earlier job
kept its interpreter (a document that could not be closed, a panic). The
bridge holds one job at a time, so this is never contention; the
transport replaces the printer once, as a device restart would, and says
so in OPENED (`printer_restarted`), which the bridge logs.

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
<name> in <command>`, `budget`, or `failed: <reason>`), and for the
printer: `interpreter_jobs` (jobs it has served since it was created) and
`interpreter_permanent_jobs` (of those, the jobs whose replies carried
`exitserver`'s acknowledgement, `%%[exitserver: permanent state may be
changed]%%`; `startjob` writes none and is not counted). `restart()` is
the printer's power switch: a PAP session in progress is closed and its
job abandoned (a job finishing detached goes too), the printer is retired,
and the counters start again. It leaves the name, the enabled state, the
capture setting and the advertisement alone, and refuses on a `PLATEN=0`
build.

## The document sink

`laserwriter_sink_document` is a weak symbol the platform overrides:

- **headless** (`headless_main.c`) writes `<print-dir>/<job>-<title>.pdf`
  under `--print-dir` / `$GS_PRINT_DIR` and logs pages and any error.
- **wasm** has no override: the ring transport never produces bytes here —
  the interpreter worker posts the PDF to the page, which shows it in the
  viewer dialog
  ([`laserwriter-session.md`](../../../reference/protocols/laserwriter-session.md) §5.5). The document count still moves.
- the default (no platform override) logs and drops the document.

## The prelude and identity

The interpreter is seeded from [`laserwriter_prelude.ps`](../../../../src/core/network/laserwriter_prelude.ps),
embedded at build time as a C array (`laserwriter.mk` → `bin2c.py` →
`build/laserwriter/laserwriter_prelude.h`), and three `statusdict` identity
entries derived from the same product/version/revision values, so
`statusdict` and `systemdict` agree. Both travel in every OPEN request
(the ring transport copies them into the record; the direct one into its
own buffers), so the browser's worker needs nothing baked in; they are
used when the OPEN creates the printer and ignored after, so the prelude
runs once per printer, not per job. The prelude
defines the `statusdict` timeouts, the Level 1 page-size procedures, and
the userdict page-type names a LaserWriter provides; it is written from the
published language reference, not from any driver. It originally lived in
gs-test-data; it now lives in the repository as the emulator's own asset.
