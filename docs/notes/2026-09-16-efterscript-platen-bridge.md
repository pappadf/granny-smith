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

**Resolved 2026-09-20.** The wedge was the link-layer fault fixed on
2026-09-19 (the printer transmitting in the same instant as the guest;
see "The rebase regression, and the PAP fix"). With the wire reserved
for the guest's frames, the prelude registers the 13 faces again: the
font-list query reports them, the driver substitutes rather than
downloads, the document job is 36,900 bytes instead of 61,981, the PDF
carries Helvetica as a Type 1 face, and the acceptance row passes plain
and at log level 6 with the driver closing the connection itself. The
user's first browser print (before this) showed exactly the symptom:
"downloading bitmap font" messages and bitmapped text in the PDF.

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
  (EfterScript's `crates/efterscript-platen/docs/embedding.md` lists them); confirm the
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

## How the library arrives, and the toolchain it dictates

Decided 2026-09-19. The emulator consumes EfterScript's released
archives rather than building the crate from source: every EfterScript
tag attaches `libplaten-<version>-wasm32-unknown-emscripten-<emsdk>.a`,
`libplaten-<version>-x86_64-unknown-linux-gnu.a`, `platen-<version>.h`
and `SHA256SUMS` to its GitHub release (0.0.2 is the first). Part 2
replaces the source build in `laserwriter.mk` with a fetch of those
files by version, verified against the checksum file.

An Emscripten archive links only into a program built with the same
Emscripten SDK, so the emulator's pin follows EfterScript's: 6.0.7 for
0.0.2 (the version Rust 1.98.0's standard library was built with; the
file name carries it). That is why the SDK moves from 4.0.10 to 6.0.7
on this branch before the fetch step lands. Two further rules from
EfterScript's embedding guide: the final link passes
`-fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1`, because Rust compiles
the target with WebAssembly exceptions on and the archive references
the exception tag; and a later EfterScript release that names a newer
SDK means a matching bump here.

### The 4.0.10 to 6.0.7 jump, checked item by item

Against the SDK's own changelog for 5.x and 6.0.x, on the branch:

- **Incoming module keys.** 6.0.2 dropped `mainScriptUrlOrBlob` (and
  others) from the default `INCOMING_MODULE_JS_API`, so a build no longer
  compiles in the read of it; the front end sets that key so worker
  threads fetch the cache-busted script rather than a stale one. The link
  now lists the keys the front end passes explicitly
  (`arguments,canvas,locateFile,mainScriptUrlOrBlob,print,printErr`);
  verified by the read reappearing in the built module. The `on*`
  callbacks the embedded JavaScript reads are plain properties on the
  same object and need no listing.
- **Not applicable.** C only, linked with `emcc` (`DEFAULT_TO_CXX` off is
  moot); already `-pthread`; no `-shared`, `MEMORY64`, `USE_PTHREADS`,
  `.bat` launchers, or thread-pool internals; no C++ exceptions crossing
  into JS. `GROWABLE_ARRAYBUFFERS` still defaults off in 6.0.7's
  settings. `WASM_LEGACY_EXCEPTIONS` defaults on, matching the flag the
  platen link passes.
- **Browser floor.** Chrome 85 / Firefox 79 / Safari 15; the emulator
  already requires SharedArrayBuffer, OffscreenCanvas, WebGL2 and OPFS,
  and the README names Chromium, so nothing narrows.
- **Runtime in a browser is unverified here**: this container has no
  browsers, so the Playwright rows (`ui2-e2e`, `ui2-prod-smoke`) run in
  CI's web job on the new image.

## The rebase regression, and the PAP fix

After a rebase onto main the acceptance row failed: the whole guest flow
ran, the driver reached "processing job", and no document arrived. Bisected to the machines
commit, but that commit only moved boot timing (its VIA idle-line change,
reverted in isolation, changed nothing); the pre-rebase build itself
failed as soon as AppleTalk logging was raised. The defect was the
printer's: every SendData it issued left in the same instant as the frame
before it (the OpenReply, the TRel of the previous SendData, a reply to
the workstation's read). On the second query the TRel and the next
SendData reached the driver before its write completion had propagated,
and it answered the new sequence number with the buffer it had already
sent; the bridge fed the query twice, asked again, and the driver never
spoke again. Inside AppleTalk 2e, ch. 10, "Duplicate filtration", says a
new sequence number means new data, so the resend was the driver's
consequence of the timing, not a protocol choice.

Fix: `pap_schedule_senddata()` defers every SendData by
`PAP_SENDDATA_GAP_NS` (10 ms of guest time) on the stack's scheduler,
cancelled on session reset; a real printer takes far longer between
reads. Also `pap_now_ms()` now reads guest time when a scheduler exists,
so the 120 s inactivity timeout no longer runs on the host clock (a slow
host or heavy logging could end a session by itself). The row passes
with logging at level 6, the condition the old code failed under, and
the first SendData's one retry seen in every earlier transcript is gone.

The second half, one layer down. With the gap in place the row passed,
but after the document the driver held the connection with tickles and
never closed; the printer's retry limit (12 x 8 s) eventually cut it.
The transcript showed why: the driver's last read had been answered in
the same instant the driver itself was transmitting the job's final
fragment. The link layer noted the wire busy only for the printer's own
data frames, never for the guest's, and the SCC completes the guest's
transmission the instant the driver finishes writing, so the printer
could open its own dialog while, on a real line, the guest was still
sending; the driver never saw the answer and its read never completed.
Fix in `appletalk.c`: every received frame holds the wire for its wire
time plus the interframe gap (`llap_wire_note_peer_frame`), and
answering the guest's lapRTS reserves the wire until its data frame
arrives (`llap_wire_reserve_for_peer`, ceiling one maximal frame). After
it the driver closes the connection itself (`CloseConn`, "client
closed"), with no retries or lost handshakes, under full logging. The
six other AppleTalk rows (AFP x2, PPC, Apple events x3) and the unit
suites pass on the new timing.

Known deviation kept: the printer's SendData retries 12 x 8 s and then
aborts, where Inside AppleTalk specifies infinite retries at 15 s with
the two-minute tickle timer as the only reaper. It is a safety net that
the two fixes above make dormant on a live driver.

## Part 2 design: the interpreter in its own worker

Decided 2026-09-19 after the first Emscripten link. The emulator is a
threaded build (shared memory), and Emscripten's linker refuses the
released archive in such a program because Rust's prebuilt standard
library for the target has no atomics. Rather than ask EfterScript for a
threaded variant (nightly, rebuilt std) and carry ten megabytes in the
main module for everyone, the interpreter runs in its own Web Worker with
its own non-threaded module, built by this tree from the released archive
(`make platen-module`): the archive links standalone today (checked here:
a page rendered under Node; 11 MB wasm, 6.9 MB gzipped, to be trimmed on
EfterScript's side). The module is fetched on the first print job, not at
start; a first print waits like a real printer warms up.

**Transport.** The Voodoo GPU worker's pattern (`voodoo2_gpu_protocol.h`
/ `voodoo2Protocol.ts`, `voodoo2Gpu.worker.ts`): the core allocates a
control block and two byte rings in the wasm heap; the front end hands
the worker the shared memory and the block's address (`postMessage`,
once); after that the two sides exchange records through the rings with
`Atomics` (notify on write; the worker parks in `Atomics.waitAsync`; the
emulation pthread drains its inbound ring from its per-frame hook and
never blocks on the printer). Records, defined once in
`laserwriter_ring_protocol.h` and mirrored in `platenProtocol.ts` under a
version checked at attach:

- core -> worker: `open` (job id, the `platen_config` fields), `feed`
  (job id, sequence, up to one PAP flow quantum of bytes), `finish`,
  `abandon`.
- worker -> core: `opened` / `open_failed` (error text), `fed` (job id,
  sequence acknowledged, the feed's status, that feed's reply and error
  bytes), `finished` (job id, outcome, pages, error name, offending
  command). The PDF itself never enters the core in the browser: the
  worker posts it to the main thread (transferable) and it downloads as
  `<job>-<title>.pdf` at once; the core learns only the outcome and the
  page count.

**The bridge becomes asynchronous**, in both builds. `laserwriter_job.c`
issues the four records through a `laserwriter_transport` (direct calls
into the library for headless; the ring for wasm) and receives results
through callbacks; the direct transport defers its callbacks through the
scheduler so the headless acceptance row exercises the same asynchronous
bridge the browser runs. Two PAP rules follow from the library's
guarantee that a query's answer is available as soon as the feed that
completed it returns: the driver's read credits are answered with a
status line only while no feed is unacknowledged, and the next SendData
goes out after the acknowledgement (plus the issue gap). An OpenConn is
answered at once with a "starting up" status while the module loads; the
first SendData waits for `opened`; an `open_failed` aborts the session
with the error in the status string.

**Defaults.** The browser build always carries the printer (`PLATEN=1`
is the wasm default; the core no longer links the library, so no
toolchain or archive is needed for the main module, only for
`platen-module`). Headless keeps `PLATEN=0` by default until EfterScript
publishes an arm64 host archive; CI's x86_64 runners can run the
integration tier with `PLATEN=1`.

## Part 2A implementation notes

Done 2026-09-19: the core/headless half of the part 2 design above. The
browser worker, the TypeScript mirror, the front-end attach, the download,
and the PLATEN default flip are part 2B.

**Built.**

- `laserwriter_ring_protocol.h` — the shared-memory protocol: magic and
  `LWRING_PROTOCOL_VERSION`, control-block word indices (Int32 for
  Atomics: two rings' head/tail, a status word, statistics), two byte
  rings of `{kind, len}` records whose total length is a multiple of 8
  (`LWRING_PAD8`; fields inside a payload are padded to 4) and that
  never wrap — a PAD record reaches the ring's end, and the 8-byte rule
  is what guarantees the PAD's own 8-byte header always fits (a 4-byte
  rule left a 4-byte remainder possible; caught before part
  2B mirrors the protocol; readers reject `len & 7`), and the records:
  OPEN (job id plus every
  `platen_config` field the bridge sets, the identity as NUL-terminated
  pairs and the prelude inline), FEED (job id, PAP SendData sequence, at
  most one flow quantum of bytes), FINISH, ABANDON out; OPENED,
  OPEN_FAILED (text), FED (sequence, feed status, pages, reply bytes,
  error bytes, a truncation flag), FINISHED (outcome, pages, error name,
  offending command, the completion's output) in. The PDF is not a
  record. Ring sizes are compile-time and overridable for tests; the
  worker must read them from the control block.
- `laserwriter_transport.h` — open/feed/finish/abandon plus the four
  callbacks and `laserwriter_transport_poll()`; two implementations:
  `laserwriter_transport_direct.c` (headless: queues the request, runs the
  library call from a scheduler event 1 ms of guest time later, delivers
  from it; the document rides in the finished result) and
  `laserwriter_transport_ring.c` (wasm: records into the outbound ring,
  answers drained in poll; the region is allocated on the first open and
  `laserwriter_ring_attach_requested(ctrl_addr)` /
  `laserwriter_ring_notify(addr)` are weak no-ops for the platform to
  override; a LOST status word or corrupt inbound framing fails the
  outstanding request as its own kind of failure). The ring transport
  compiles natively and is unit-tested.
- `laserwriter_job.c` — a five-state job (idle, opening, ready, feeding,
  finishing) that turns the callbacks into four events for the PAP layer
  (OPENED, FED, FINISHED, FAILED), keeps the output queue, title scan,
  status text and counters, and runs a 1 ms guest-time poll tick while a
  request is outstanding, with a 120 s deadline on OPEN.
- `appletalk_printer.c` (`GS_PLATEN` path) — OpenConn answered at once
  with `status: starting up`, first SendData at OPENED; a transaction's
  fragments gathered into one feed (sequence = the SendData's); the next
  SendData at FED; status lines on read credits only while no feed or
  finish is unacknowledged (a held credit is answered from the FED); EOF
  → FINISH after the last FED → FINISHED finalises (counts, EOF on the
  read channel, idle, next job primed, next SendData); a later job on the
  connection opens on its first data, which waits for OPENED; FAILED
  aborts the session with the error in the status string; connection
  loss → ABANDON through `pap_session_reset`. The issue gap, the
  guest-time inactivity timer and `appletalk.c`'s wire reservation are
  untouched.
- Build: `Makefile.headless` compiles the direct transport
  (`filter-out` of the ring), `Makefile` the ring transport; the wasm
  link no longer names the platen archive or fetches it — `make PLATEN=1`
  needs only emcc. `laserwriter.mk` unchanged in defaults and fetch.
- Tests: `tests/unit/suites/laserwriter_ring/` plays the worker in C
  (8 KB / 4 KB rings via `-DLWRING_*_BYTES`): every record field,
  one-request-at-a-time refusal, a feed over one quantum refused, stale
  answers for an abandoned job dropped, a LOST worker failing the
  outstanding feed, both rings wrapping several times with PADs and byte
  identity checked, an inbound record engineered to need a PAD before
  the ring's end, an outbound ring with no room refusing an open (and
  recovering), an outbound position driven to exactly size - 8 (a PAD
  of just its header), 300 feeds of every size residue mod 8 through
  both rings with every record start asserted 8-aligned, and corrupt
  inbound framing (a 4-aligned but not 8-aligned len) marking the
  worker lost.

**Verified** (arm64 devcontainer; the release has no arm64 host archive,
so `PLATEN_DIR=/workspaces/efterscript` supplied `target/release/libplaten.a`):

- `make -C tests/integration test-appletalk-print PLATEN=1
  PLATEN_DIR=/workspaces/efterscript` — twice (before and after a final
  log-only tweak):
  `printer: documents=1 pages=1 outcome='ok' status='status: idle'`,
  `=== PASS: appletalk-print ===`.
- The same row with `debug.log "appletalk" "level=6"` prefixed (temporary
  twin directory, deleted afterwards):
  `printer: documents=1 pages=1 outcome='ok' status='status: idle'`,
  `=== PASS: appletalk-print-log ===`. In the transcript: OpenReply goes
  out before "job 1 started", the first SendData after it; every job's
  output is followed by `eof=1`; after the document the driver sends
  `CloseConn` itself ("job 4 complete (client closed)"); zero `retry
  timeout`, `no CTS` and `SendData timeout` lines; every status-line
  answer is issued while the printer's own SendData is on the wire, none
  while a feed is unacknowledged.
- `appletalk-afp`, `appletalk-afp-e2e`, `appletalk-ppc`, `aevt-finder`,
  `aevt-inbox`, `aevt-stress` with `PLATEN=1 PLATEN_DIR=…`: all
  `=== PASS`.
- `make unit-test`: "All 66 tests passed" (the new suite included).
- `make -j8 all` and `make -j8 PLATEN=1 all` under emsdk 6.0.7: both
  link; the PLATEN=1 link line carries no archive.
- `make -f Makefile.headless` (PLATEN=0): the stubs build.
- clang-format 18 `--dry-run --Werror` over `src`: clean.

**Deviations from the design, and why.**

- *Polling.* The design has the emulation pthread drain the inbound ring
  "from its per-frame hook". The core has no such hook, so the bridge
  polls the transport from a 1 ms guest-time scheduler tick while (and
  only while) a request is outstanding; the transport itself has no
  scheduler dependency, which is what lets the unit suite drive it with
  a simulated worker. `laserwriter_transport_poll()` stays public so the
  platform may also call it from its frame loop.
- *Feed granularity.* Part 1 fed each ATP fragment (≤ 512 bytes); now a
  SendData transaction's fragments are gathered and fed once (≤ 4096),
  which is what makes FEED/FED carry the SendData sequence and keeps one
  FEED per read. The library accepts any split.
- *FED carries the page count* (not in the design's list) so the status
  line can say `printing … page: n` without a live library handle.
- *A second job on a connection opens lazily* on its first data (as in
  part 1) rather than eagerly at FINISHED; the gathered data waits for
  OPENED. The first job still opens at OpenConn.
- *Open timeout* (120 s of guest time) added so a wasm build without the
  worker degrades to a failed job rather than a session that never
  reads; the driver's tickles keep the PAP timer from firing meanwhile.
- The manual and the code agree on everything this part touches; the
  one known deviation from Inside AppleTalk (the finite SendData retry
  limit) is unchanged and noted above.

**Open for part 2B.**

- `platenProtocol.ts` mirroring `laserwriter_ring_protocol.h` (check
  `LWRING_PROTOCOL_VERSION` at attach), the worker (Atomics.waitAsync on
  OUT_HEAD; read sizes from the control block; PAD on both rings; set
  STATUS to ATTACHED, LOST on a failed module load or a crash; cap each
  text field at `LWRING_TEXT_MAX` and set the truncation flag; post the
  PDF to the main thread with the job id), `make platen-module` from the
  release archive with `PLATEN_LIB_WASM` + `PLATEN_WASM_LDFLAGS`.
- Platform overrides in `src/platform/wasm`:
  `laserwriter_ring_attach_requested` (post the control block address
  to the page, once) and `laserwriter_ring_notify`
  (`emscripten_futex_wake`, as `gs_v2gpu_notify` does). Without them the
  weak no-ops log once and every open times out after 120 s.
- The download (`<job>-<title>.pdf`: the title is the bridge's; the
  worker only knows the job id — either the page asks the core for
  `appletalk.printer` state or the FINISH record grows a title field;
  the latter is a one-line protocol change with a version bump).
- The PLATEN default flip for the wasm build, and CI running the
  headless integration tier with `PLATEN=1` on x86_64.
- Optional: `appletalk.printer.transport` in the object model
  (`laserwriter_transport_name()` exists for it).

## Part 2B implementation notes

Done 2026-09-19: the browser half of the part 2 design — the worker's
module, the TypeScript mirror, the worker and its loop, the front-end
attach, the download, and the PLATEN default flip. The reference is
`docs/core/network/laserwriter.md` §5.5.

**Built.**

- `make platen-module` (`Makefile`; a prerequisite of `all` when
  `PLATEN=1`): `emcc` links `$(PLATEN_LIB_WASM)` alone — no C shim; the
  `-sEXPORTED_FUNCTIONS` list pulls the archive members — into
  `build/platen-<PLATEN_VERSION>.js` + `.wasm`: `-O2`,
  `$(PLATEN_WASM_LDFLAGS)`, `--no-entry`, `MODULARIZE` + `EXPORT_ES6`
  (`createPlatenModule`), `ENVIRONMENT=worker,node`, memory growth,
  `STACK_SIZE=1MB`, every `platen_*` entry plus `_malloc,_free`, runtime
  methods `HEAPU8,HEAPU32,UTF8ToString` (what the wrapper uses), and an
  explicit `INCOMING_MODULE_JS_API=locateFile,print,printErr`. 55 KB of
  glue, 11 MB of wasm. Served like `main.mjs`: the Vite middleware matcher
  gained `/platen-<v>.(js|wasm)`, `make ui2` copies `build/platen-*` into
  `dist/`; `build/` is already ignored. The version reaches the page from
  the core: the wasm `CFLAGS` carry `-DGS_PLATEN_VERSION="<PLATEN_VERSION>"`
  and `em_main.c` passes it in the attach callback, so `laserwriter.mk` is
  the single source of the version.
- `app/web2/src/printer/platenProtocol.ts` — the mirror of
  `laserwriter_ring_protocol.h` (same names, values; "keep in step" both
  ways), plus the framing helpers both sides share: `recordBytes`
  (PAD8 of header + words + PAD4 fields), `ringWrite` (PAD before the
  ring's end, no-room detection against TAIL), `ringRead` (rejects
  `len & 7`, a wrap, or more than published).
- `app/web2/src/printer/platenLib.ts` — the module loader (dynamic import
  of the cache-busted URL, `locateFile` for the wasm) and the C-ABI
  wrapper: `platen_config` laid out in the module's own memory (wasm32
  offsets, the identity as `platen_entry` pairs pointing at NUL-terminated
  strings, the prelude inline), feed/finish/read/pdf/free, the channels
  drained through one 64 KB scratch buffer with the cap and truncation
  flag, heap views re-read after every call (memory growth).
- `app/web2/src/printer/platenRing.ts` — the loop, factored so a test can
  drive it against a plain `SharedArrayBuffer`: the constructor checks
  magic/version (throws, nothing touched), reads the geometry from the
  control block, starts from `OUT_TAIL` / `IN_HEAD` (records the bridge
  wrote before the attach are consumed), sets STATUS ATTACHED; `run()`
  parks in `Atomics.waitAsync` on `OUT_HEAD`; `service()` consumes PAD /
  OPEN / FEED / FINISH / ABANDON and answers OPENED / OPEN_FAILED / FED /
  FINISHED, waiting on `IN_TAIL` when the inbound ring is full, notifying
  `IN_HEAD`, counting `STAT_JOBS` / `STAT_FEEDS`; a FEED or FINISH for a
  job it does not hold answers a failure; an OPEN over a live job frees
  it first (a lost ABANDON); the document is posted only under the
  bridge's own rule (a page shown, or an error) so downloads and the
  `documents` count agree. `documentName()` is the headless sink's
  naming, character for character.
- `app/web2/src/printer/platen.worker.ts` — the worker: `start`
  {moduleUrl, wasmUrl} → `ready` / `lost`; `attach` {memory, ctrl} →
  loop; any throw → STATUS LOST + `lost {reason}`; `document` posted with
  the PDF transferred.
- `app/web2/src/printer/platen.ts` — the page side, registered as
  `Module.onPrinterAttach` in `bus/emulator.ts`: starts the worker on the
  first attach (the module is fetched only then), queues the attach until
  `ready`, posts the wasm memory + ctrl, downloads each document through
  a Blob + anchor click and shows a toast; on `lost` logs, toasts, and
  lets the core's LOST handling / open timeout end the session.
- `src/platform/wasm/em_main.c` — `laserwriter_ring_attach_requested`
  (`MAIN_THREAD_ASYNC_EM_ASM` → `Module.onPrinterAttach(ctrl, version)`,
  the em_gpu.c pattern) and `laserwriter_ring_notify`
  (`emscripten_futex_wake`); the held-document `laserwriter_sink_document`
  override is removed (the ring result carries no bytes, so it was never
  reached). The hook prototypes now live in `laserwriter_transport.h`.
- **Protocol version 2**: FINISH is `{job_id, title_len}` + title
  (PAD4, capped at the new `LWRING_TITLE_MAX` = 63);
  `laserwriter_transport_finish(job_id, title)` in both transports (the
  direct one ignores it — its sink gets the bridge's title with the
  document); the bridge passes its `%%Title:` name; the C unit suite
  asserts the field and the PAD8 total; the TS mirror carries the same
  layout.
- **Defaults**: `Makefile` sets `PLATEN ?= 1` before including
  `laserwriter.mk` (headless keeps 0). `make` alone now builds the main
  module and the platen module (one ~27 MB archive fetch from the pinned
  release on the first build; no secrets). CI's `ui` job runs Vitest after
  `make ui2` so the module test runs for real there; the `test` job's
  `make` builds the module too.
- **Tests**: `app/web2/tests/unit/platenRing.test.ts`
  (`@vitest-environment node`; `tests/setup.ts` now guards its DOM patch;
  `vitest.config.ts` allows the repository root so Vite serves `build/`):
  loads the built module, lays out the rings in a `SharedArrayBuffer`,
  writes OPEN (the bridge's identity + `laserwriter_prelude.ps`), FEED
  (`0.5 setgray 100 100 200 300 rectfill showpage`), FINISH ("Test Page")
  before attaching, runs the real loop, and asserts OPENED / FED
  (WAITING, pages 1, seq echoed) / FINISHED (OK, pages 1) with PAD8
  totals, the STAT words, `OUT_TAIL` caught up, and a `%PDF-` document
  named `00001-Test_Page.pdf`; the framing test starts the outbound cursor
  so OPEN ends 16 bytes short of the end and the FEED forces a 16-byte PAD
  and restarts at 0 (and the inbound cursor so the worker's own FED needs
  a PAD of just a header — the core-side reader sees it); plus a version
  mismatch refusal, a FED-failed answer for an unknown job, the naming
  rule, and a `len & 7` rejection. Skips with a warning when
  `build/platen-*.js` is absent.

**Verified** (arm64 devcontainer, emsdk 6.0.7, Node 20.20.2).

- `make platen-module`: links; the exports alone pull the archive (no
  shim). A Node smoke script drove the module directly: empty prelude
  accepted, `feed` → 0 / pages 1, `finish` → 0, a 587-byte `%PDF-1.7`.
- `make ui2-check`: svelte-check 0 errors / 0 warnings; ESLint and
  Prettier clean.
- `make ui2-test`: `Test Files 65 passed (65)`, `Tests 365 passed (365)`
  — the new file's 7 tests included, the module test running (not
  skipped).
- `make -j8 all` (PLATEN=1 by default): links; `build/main.mjs` reads
  `Module.onPrinterAttach`; `build/platen-0.0.2.{js,wasm}` present.
  `make -j8 PLATEN=0 all`: links. `make ui2` then `node
  scripts/check-dist.mjs`: `dist/platen-0.0.2.{js,wasm}` beside
  `main.mjs`, `check-dist: OK`.
- `make -f Makefile.headless` (PLATEN=0): `Built: build/headless/gs-headless`.
- `make -C tests/unit test-laserwriter_ring`: `all tests passed (675
  outbound / 683 inbound record starts, all 8-aligned)`.
- `make -C tests/integration test-appletalk-print PLATEN=1
  PLATEN_DIR=/workspaces/efterscript`: `printer: documents=1 pages=1
  outcome='ok' status='status: idle'`, `=== PASS: appletalk-print ===`.
- clang-format 18 `--dry-run --Werror` over `src`: nothing printed.

**Deviations from the task, and why.**

- The download's name is composed in the worker (`documentName`) and
  posted as `name` beside `jobId`, `title` and `pages`; the page uses it
  as given. The naming is the headless sink's, not the old `em_main.c`
  one (which did not sanitise), so both platforms produce the same file
  name for the same job.
- `-sSTACK_SIZE=1MB` on the module link is not in the embedding guide's
  example: Emscripten's default is 64 KB and a PostScript job recurses;
  the cost is static memory in the worker's own module.
- Runtime methods are `HEAPU8,HEAPU32,UTF8ToString` rather than the
  longer list suggested: the wrapper writes its strings itself and calls
  the exports directly (no `ccall`/`cwrap`).
- The vitest runs in Node's environment (the module has no browser
  build), which needed a one-line guard in `tests/setup.ts` and an
  `fs.allow` for the repository root in `vitest.config.ts`.
- CI's `ui` job runs Vitest after the WASM build instead of before, so
  the module test is exercised rather than skipped.
- No Playwright row: nothing here can run a browser, and a "printer
  attaches and the module loads" check needs a booted guest printing.

**Open items.**

- A browser run is unverified here (no browser in the container): the
  attach → module fetch → download path has run only under Node through
  the loop; CI's Playwright rows exercise the page's bootstrap, not a
  print.
- A worker lost mid-session stays lost: the core keeps its ring region
  and never re-requests an attach, so every later job times out (120 s)
  with the LOST status. Recovery (a fresh region and a fresh worker) is
  a small core + page change if it is ever wanted.
- The module is 11 MB (6.9 MB gzipped); trimming is EfterScript's side.
- `appletalk.printer.transport` in the object model is still optional.
- The unit-test tier (`make unit-test` as a whole) was not re-run: only
  the ring suite is touched by the version bump.

## System 7.1 printed nothing: two defects behind one error

Reported 2026-09-20 from the browser build: printing from a IIcx running
System 7.1 gave "offending command" in the driver's dialog and a PDF no
reader would open. Reproduced headlessly (the new integration row
`appletalk-print-71`, which drives the same Chooser + "Print Window…"
flow on that machine) as `error: typecheck in and`, zero pages, and a
317-byte PDF with no page tree — hence the reader's refusal.

The System 7.1 image carries LaserWriter **7.1.2**; the row that existed
(`appletalk-print`, System 6.0.8 on a Plus) exercises **7.0**, and the
two send materially different PostScript. Capturing the job
(`appletalk.printer.capture = true`, 40 KB) and replaying it against the
interpreter directly isolated both causes.

**One, `cexec` (EfterScript's).** The AppleDict prologue opens with a
probe for the printer's native-code escape:

```
save LW dup 1 ne exch 2 ne and
false <encrypted>{eexec}stopped{dup type/stringtype eq{pop}if}if and
```

The encrypted section pushes a string of 68000 code and calls `cexec`.
The probe is written to fail: `undefined` → `stopped` true → the cleanup
pops the string → `and` gets its two booleans. EfterScript defined
`cexec` as `exec`, so the section succeeded, the string stayed, and the
`and` raised `typecheck`. Fixed in EfterScript (`cexec-not-an-operator`);
`cexec` is not a PLRM operator and is no longer defined.

**Two, the product string (ours).** With that fixed the job got further
and then failed on a *second* `cexec`, in a section the driver downloads
only for genuine LaserWriters. The prologue computes

```
LW = statusdict/product get (LaserWriter) anchorsearch …   (0 when it does not match)
ok = systemdict/statusdict known dup{LW 0 gt and}if
```

and `checkload` skips the native sections when `ok` is false. Our
product was `LaserWriter II NT`, so `LW` was 3 and the driver sent code
for a processor we do not have. `LASERWRITER_PRODUCT` is now
`EfterScript LaserWriter` — deliberately not beginning with the prefix
the drivers key on, so they send the generic PostScript path. The
AppleTalk entity type stays `LaserWriter`, so the Chooser is unchanged.

**After both.** One page, `outcome: ok`, a 7.8 KB PDF whose text is
Helvetica as a Type 1 face, rendering as the Finder window it printed.
System 6.0.8 is unaffected (`appletalk-print` passes, same font
treatment). The new row fails with either fix missing: against the
released 0.0.3 library it reproduces `typecheck in and` exactly.

**Sequencing.** `appletalk-print-71` needs an EfterScript release
carrying the `cexec` fix; until the pin moves past 0.0.3 it fails on the
extended tier.
