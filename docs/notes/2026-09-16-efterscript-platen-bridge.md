# EfterScript platen bridge — the emulated LaserWriter's PostScript path

*Dated investigation log; not reference. The reference is
`docs/core/network/laserwriter_job.md` and `laserwriter.md` §5.*

2026-09-16. Part 1 of wiring EfterScript's `platen` library (a per-job
PostScript-to-PDF interpreter with a C ABI) behind the emulated
LaserWriter. The split is fixed by the project owner: EfterScript provides
the mechanism only; everything about *acting like* a LaserWriter — PAP,
status strings, query handling, the host prelude, identity, UI — lives here.

## What was built

- **Build switch `PLATEN=1`** (default off) for `Makefile.headless` and
  `Makefile`, via a shared `src/core/network/laserwriter.mk`. It sets
  `PLATEN_DIR` (default `../efterscript`), the include path, and the link:
  `libplaten.a` after the objects, then exactly the system libraries
  `cargo rustc -p platen --release -- --print native-static-libs` reports
  on Linux — `-lgcc_s -lutil -lrt -lpthread -lm -ldl -lc`. A `PLATEN`-valued
  stamp is a prerequisite of every object so toggling rebuilds the tree. A
  missing archive is a clear configure-time error. The wasm Makefile points
  at the `wasm32-unknown-emscripten` archive; that link is unverified here
  (no `emcc` in this container) and is part 2.
- **The bridge** `src/core/network/laserwriter_job.{c,h}`: one `platen` job
  per EOF-delimited PAP job. Fed every PAP data payload in order; drains
  `platen_job_read_replies` / `_read_errors` after each feed; finishes at
  the workstation's EOF; frees (without finishing) on a mid-job teardown.
  Status text is composed from the job's own facts (`platen_job_pages`, the
  `%%Title:` name). The finished PDF goes to a weak `laserwriter_sink_document`
  the platform overrides.
- **`appletalk_printer.c`** now feeds the interpreter instead of the
  placeholder query heuristics (which are `#if !GS_PLATEN`): incoming Data
  → `laserwriter_job_feed`; interpreter output → the workstation's read
  credits; a status read with no pending output → the composed status
  string; EOF → finish + document + next job. The spool file is an optional
  capture (`appletalk.printer.capture`), not the input.
- **Object model** `appletalk.printer`: added `status`, `interpreter`,
  `capture`, `documents`, `last_pages`, `last_outcome`.
- **Prelude and identity, emulator-side.** `src/core/network/laserwriter_prelude.ps`
  (copied from gs-test-data, trimmed of the identity `def`s which are now
  seeded as `statusdict` entries so `statusdict`/`systemdict` agree),
  embedded at build time via `bin2c.py` → `build/laserwriter/laserwriter_prelude.h`.
- **Platform sinks:** headless writes `<print-dir>/<job>-<title>.pdf`
  (`--print-dir` / `$GS_PRINT_DIR`); wasm keeps the bytes for the download
  UI (part 2).
- **Acceptance** `tests/integration/appletalk-print/`, gated on
  `appletalk.printer.interpreter` (skips on the harness's `PLATEN=0` build).

## The execution budget

Probed the captured Finder job (`gs-test-data/printjobs/finder-sys608-lw70-
print-directory.ps`) against `libplaten.a` at budgets from 10,000 to
unlimited: it runs in **under 10,000 objects** and the PDF is byte-identical
to the golden at every budget. A runaway `{ } loop` hits `limitcheck` at
~1e8 objects in ~6.5 s native, ~1.35 s at 2e7. Chose **100,000,000** — four
orders of magnitude of headroom over the real job, and a bad job stopped in
a few seconds rather than hanging a browser frame.

## Acceptance results

1. **Deterministic (byte-exact).** The captured job fed through the bridge's
   platen configuration (this prelude + identity) yields a PDF
   **byte-identical** to `tmp/lab/finder-ours.pdf`, one page. This is the
   canonical signal: it proves the bridge configuration reproduces the
   golden exactly, independent of emulator timing.
2. **Live emulator PAP.** Boot System 6.0.8 on a Plus, Chooser → LaserWriter
   → Finder "Print Directory" → Print, with `PLATEN=1` and `--print-dir`:
   the driver's PatchPrep and font-list queries are answered from the jobs'
   own output, the 36 KB document job runs to completion, one page, outcome
   `ok`, `documents == 1`, and the printer returns to `status: idle`. The
   PDF is written to the print directory (`00003-Macintosh_HD.pdf`, 22,291
   bytes). Its page layout — header rules, the "System Folder" icon and its
   position, the page number, the footer rule — matches the golden; its
   text glyphs are sparse (see the gap below).

The old placeholder path never printed this job over PAP: the original lab
capture (`tmp/lab/step6`) ended in "The directory could not be printed",
and the golden was captured via the driver's "Destination: PostScript File"
option instead. So the live PAP path completing to a saved PDF is new.

## The queries, answered by the interpreter and not by placeholders

The driver issues three query jobs before the document, each now a real
interpreter job:

- **PatchPrep** (`%%?BeginProcSetQuery: "(AppleDict md)" 71 0`): the program
  prints `0` (procset absent). Old placeholder: also `0`. The driver then
  uploads the procset as an ordinary job (job 2), which the interpreter
  ingests; the driver does not re-query it in the same session.
- **Font list** (`%%?BeginFontListQuery`): the program iterates
  `FontDirectory`, which `platen` leaves **empty**, so the reply is just the
  terminating `*`. Old placeholder: the 13 standard font names then `*`.
  This difference is the fidelity gap below.

## Fidelity gap: resident fonts, and a wedge that blocks the fix

The live document renders full layout but sparse text. Cause: the font-list
query reports no resident fonts (empty `FontDirectory`), so the driver
downloads a **bitmap** font, which `platen` renders at lower fidelity than
the outline fonts a file-destination capture (the golden) uses.

The mitigation is emulator-side and was tested: seed `FontDirectory` in the
prelude with the 13 resident faces
(`FontDirectory /Helvetica /Helvetica findfont put`, etc. — `findfont`
resolves them but does not itself register them). The font query then
reports all 13, and standalone the captured job stays byte-identical to the
golden. **But** on the live path this makes the driver substitute rather
than download, producing a shorter job — and partway through it the guest
stops answering LocalTalk RTS ("no CTS from 45"), the printer's SendData
retries exhaust, and the job aborts with no document. Raising the retry
limit from 12 to 40 did not help: the guest is wedged for a long stretch,
not a momentary gap. This is a **pre-existing** guest-side LocalTalk/timing
fault (the same class that failed the original lab print), triggered by the
shorter substitute-font job, not by the bridge. Shipping the font-registration
prelude would wedge every real print, so it is **left out**; the prelude
carries a comment saying why. Resolving the wedge (an LLAP/ATP timing
investigation) and then restoring the font registration is follow-up work.

## platen ABI gaps found

Stated precisely, per the task. None blocks part 1; all are recorded for
the owner.

1. **`findfont` does not register the face in `FontDirectory`.** In real
   PostScript, `findfont` enters the font in `FontDirectory`; here it
   resolves the face (its `/FontName` is correct) but `FontDirectory length`
   stays 0. Because the LaserWriter driver inventories resident fonts by
   iterating `FontDirectory`, an unmodified job always sees "no resident
   fonts" and downloads them. A host *can* work around it by writing
   `FontDirectory` directly (it is a writable dict), but the natural
   behaviour would be for `findfont` to populate it. This is the root of
   the fidelity gap above. **Where a fix belongs: EfterScript (`ps-vm`),
   not here.**
2. **No status/metrics probe (`bytesavailable`-style).** The bridge cannot
   ask a running job "are you blocked waiting for input vs. still working",
   which would let the status string distinguish "receiving" from
   "rasterising". Noted as undefined in EfterScript's own design follow-ups
   (`archive/2026-09-10-platen/design.md`). Not needed for part 1; the
   status string is composed from `platen_job_pages` and the job name.
3. **No cooperative yield.** A long job runs to its budget within one
   `feed`; there is no way to bound wall-clock per feed for a browser frame
   other than the step budget. Also already recorded in EfterScript's
   follow-ups. Relevant to part 2 (wasm), not part 1.

None of these required editing EfterScript or a workaround hack here.

## What part 2 needs

- **The wasm link.** `make PLATEN=1` links
  `$(PLATEN_DIR)/target/wasm32-unknown-emscripten/release/libplaten.a`,
  which needs `cargo build -p platen --release --target
  wasm32-unknown-emscripten` (and `emcc` on the path, absent here). The
  platen symbols must be added to the Emscripten `EXPORTED_FUNCTIONS`
  (EfterScript's `crates/platen/docs/embedding.md` lists them); confirm the
  `libplaten.a` symbols survive the linker's dead-code elimination since the
  bridge calls them from C, not from JS.
- **The download UI.** `em_main.c`'s `laserwriter_sink_document` already
  copies the finished PDF and names it `<job>-<title>.pdf`; part 2 wires a
  browser download of those bytes (the `gs_download` blob+anchor path is the
  model) and a small piece of web2 UI to trigger it. Consider whether to
  keep only the last document or a small list.
- **Budget/yield for the frame.** Revisit the step budget or a cooperative
  yield (gap 3) so a large job does not stall a RAF frame in the browser.
- **The font wedge.** Independent of part 2, but resolving it would let the
  prelude register the resident faces and bring live-path text fidelity up
  to the golden.
