# LaserWriter job bridge (`laserwriter_job.c`)

The PostScript side of the emulated LaserWriter. Everything about *acting
like* a LaserWriter lives in Granny Smith; the PostScript-to-PDF mechanism
is EfterScript's `platen` library — a per-job interpreter with a C ABI
(`platen.h`). This module maps one PAP job onto one platen job and moves
bytes between the two. `appletalk_printer.c` owns the PAP session and calls
in here; `laserwriter_job.h` is the interface.

Built only with `PLATEN=1` (`-DGS_PLATEN=1`, see
[`laserwriter.md`](laserwriter.md)). Without it every entry is a stub and
`laserwriter_job_available()` returns false, so the printer keeps its
spool-only behaviour and the placeholder query answers.

## One PAP job, one platen job

The LaserWriter serves one connection at a time, and the driver ends each
unit of work with a PAP EOF, so the bridge holds exactly one job:

- **begin** (`laserwriter_job_begin`) — at PAP OpenConn, and again for each
  later EOF-delimited job on the same connection. Creates a `platen_job`
  seeded with the identity entries (product/version/revision) and the
  prelude, compression on, embed-all off, and an execution budget.
- **feed** (`laserwriter_job_feed`) — every PAP data payload, in order, as
  the bytes arrived. The interpreter handles CR/LF and piece boundaries
  (it suspends mid-token and mid-read and resumes on the next feed), so the
  bridge never reframes the stream. Each feed also scans for the driver's
  `%%Title:` comment to learn the job name.
- **read output** (`laserwriter_job_read_output`) — after every feed the
  PAP layer drains the program's replies (standard output) and error
  reports (standard error, already in `%%[ Error: … ]%%` form) and hands
  them to the workstation's read credits. A PostScript *query* is just a
  job whose program prints its answer; there is no query heuristic here.
- **finish** (`laserwriter_job_finish`) — at the workstation's EOF. Runs
  the program to completion, then hands the finished document to the
  platform sink (`laserwriter_sink_document`) and frees the job. An error
  or budget outcome still keeps its document (the pages shown before the
  error are in it) and is logged with the error name and offending command.
- **abort** (`laserwriter_job_abort`) — a connection torn down mid-job
  frees the interpreter without finishing; no document is produced.

## Status and observability

`laserwriter_job_status` composes the PAP status string from the job's
facts: `status: idle` with no job, `status: busy …; job: <name>` once a job
is running, and `status: printing …; page: <n>` once `platen_job_pages`
advances. The PAP layer keeps the read-driven credit model and answers the
driver's status reads with this string.

The object model (`appletalk.printer`) exposes `interpreter` (is the
interpreter linked), `status`, `capture` (also write the PostScript to the
spool file), `documents`, `last_pages`, and `last_outcome` (`ok`, `error:
<name> in <command>`, or `budget`).

## The document sink

`laserwriter_sink_document` is a weak symbol the platform overrides:

- **headless** (`headless_main.c`) writes `<print-dir>/<job>-<title>.pdf`
  under `--print-dir` / `$GS_PRINT_DIR` and logs pages and any error.
- **wasm** (`em_main.c`) keeps the bytes for the browser download UI
  (part 2 of the integration) and logs the job.
- the default (no platform override) logs and drops the document.

## The prelude and identity

The interpreter is seeded from [`laserwriter_prelude.ps`](laserwriter_prelude.ps),
embedded at build time as a C array (`laserwriter.mk` → `bin2c.py` →
`build/laserwriter/laserwriter_prelude.h`), and three `statusdict` identity
entries derived from the same product/version/revision values, so
`statusdict` and `systemdict` agree. The prelude defines the `statusdict`
timeouts, the Level 1 page-size procedures, and the userdict page-type
names a LaserWriter provides; it is written from the published language
reference, not from any driver. It originally lived in gs-test-data; it now
lives in the repository as the emulator's own asset.
