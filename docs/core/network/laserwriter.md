# LaserWriter Driver Workflow

Classic Macintosh systems running the LaserWriter driver follow a repeatable sequence any time a user prints over AppleTalk. The notes below describe that sequence from the driver and printer perspective, using examples captured from original PostScript traffic.

## 1. Discovery and Session Establishment

1. **NBP lookups.** The workstation issues Name Binding Protocol lookups for type `LaserWriter` in the chosen AppleTalk zone until it finds the desired printer.
2. **PAP OpenConn.** Once selected, the driver opens a Printer Access Protocol connection. The OpenConn payload specifies the client socket, desired flow quantum (usually 8), and optional status socket/interval. The printer replies with a status string such as `status: print spooler processing job` and both sides record the negotiated flow control parameters.
3. **Initial SendData credit.** Immediately after OpenReply, the driver sends a PAP `StatusRead` (function code `SendData`) to provide the printer with a credit to deliver status text or PostScript query responses. Credits continue to be granted this way throughout the session.
4. **Keepalives.** PAP tickles and explicit `SendStatus` calls are used to confirm the printer is alive and to fetch the current status text while no job data is pending.

## 2. PatchPrep Capability Query

Before streaming any user document, the driver determines whether the printer already holds Apple’s PatchPrep procset. The query is a small PostScript program sent over the newly opened PAP data channel:

```postscript
%!PS-Adobe-2.0 Query
%%Title: Query for PatchPrep
%%?BeginProcSetQuery: "(AppleDict md)" 71 0
userdict/PV known{userdict begin PV 1 ge{(1)}{(2)}ifelse end}{/md where{pop(2)}{(0)}ifelse}ifelse = flush
%%?EndProcSetQuery: unknown
```

* The driver expects the printer to execute this code and send the numeric result (`0`, `1`, or `2`) back over the status channel, terminated with CR/LF, exactly as a PostScript `=` operator would print it.
* A real LaserWriter answers `0` if PatchPrep is not installed and `1` once it has been successfully loaded. The `2` response indicates a more advanced revision.

### PatchPrep Upload

If the reply indicates PatchPrep is missing, the driver immediately transmits the full procset (roughly 2.4 KB) as ordinary PostScript data. The transfer begins with:

```postscript
%!PS-Adobe-2.0 ExitServer
%%BeginExitServer: 0000000000
serverdict begin exitserver
%%EndExitServer
%%Title: "PatchPrep -- The Apple PostScript Header"
%%Creator: Apple Software Engineering
%%CreationDate: Thursday, August 17, 1989
%%Patches version #1 0
%% ... hex payload follows ...
```

The printer ingests this payload, writes it into `userdict`, and responds with the value `1` when the upload is complete so the driver knows not to resend it in the future.

## 3. Font Directory Query

After resolving PatchPrep, the driver inventories the fonts it can expect on the device. The PostScript query looks like this:

```postscript
%!PS-Adobe-2.0 Query
%%Title: Query for list of known fonts
%%?BeginFontListQuery
save/scratch 100 string def FontDirectory{pop =}forall
systemdict/filenameforall known{(fonts/*){(.)search {pop pop pop}{dup length 6 sub 6 exch getinterval =}ifelse}scratch filenameforall}if
(*) = flush restore
%%?EndFontListQuery: *
```

The script prints each resident font name followed by a newline, then emits `*` to mark the end of the list. The driver continuously issues PAP `StatusRead` requests so the printer can deliver these strings over the status channel. A conventional LaserWriter reports the standard 13 built-in fonts (Courier, Helvetica, Times families, and Symbol) before the terminating asterisk.

## 4. Document Transmission

With the environment prepared, the driver streams the actual PostScript job:

1. **SendData requests.** The printer alternates between issuing PAP SendData transactions (requests for more PostScript) and waiting for responses. Each response corresponds to a segment of the document.
2. **Placeholder EOFs.** While the driver is waiting to start the real job it may send `%%EOF` placeholders on the status channel; printers typically ignore a small number of these until true job data arrives.
3. **Status updates.** Throughout the job the driver polls for status text (`StatusRead`) and may show progress messages to the user.
4. **Completion.** When the final `%%EOF` for the document arrives, the printer closes the job, returns `status: idle` on the next credit, and the driver either issues CloseConn or keeps the PAP session alive for the next print request.

## Summary of Expected Printer Behavior

* **Status strings** should follow the PAP format (`status: …`) and be ready any time the driver issues a `StatusRead`.
* **PatchPrep handshake** always precedes the first real page. Answer the initial query (`0` or `1`) and, if an upload follows, report completion with `1` before accepting subsequent queries.
* **Font list reply** is newline-delimited, terminated by `*`, and delivered via the status channel using the credits supplied by the workstation.
* **Flow control** relies entirely on the PAP SendData bitmap/credit scheme; the printer must not send unsolicited data.

Understanding this sequence makes it easier to build accurate emulations or troubleshoot why a particular workstation is stuck waiting—if PatchPrep never acknowledges, the driver simply keeps repeating the procset upload and never advances to the font query or main document.

---

# 5. Turning PostScript into a PDF: the platen bridge (`PLATEN=1`)

The workflow above is what the *driver* does. What the emulated printer
does with the PostScript it receives depends on the build:

* **Default (`PLATEN=0`).** The printer spools the PostScript to a file and
  answers the driver's queries with fixed placeholders (the PatchPrep `0`/`1`
  handshake and the built-in font list). No PDF is produced. This keeps
  `main` builds free of any Rust toolchain.
* **`PLATEN=1`.** EfterScript's `platen` library — a per-job PostScript
  interpreter that produces a PDF — is linked in. Every PAP job becomes one
  interpreter job (`src/core/network/laserwriter_job.c`,
  [`laserwriter_job.md`](laserwriter_job.md)): the driver's bytes are fed
  in as they arrive, a query is answered by the program's own output, and
  the finished document goes to the platform (headless writes a `.pdf`,
  the browser downloads one — §5.5). The placeholder query answers are gone
  — a query is just a job whose program prints its answer.

`PLATEN` defaults to **1 for the browser build** (`Makefile`) and **0 for
headless** (`Makefile.headless`, until EfterScript publishes a host archive
for every host the tree is built on).

### The identity the printer reports

`statusdict`'s `product` is `EfterScript LaserWriter`, and it deliberately
does not *begin* with `LaserWriter`. Apple's drivers compute

```
LW = statusdict/product get (LaserWriter) anchorsearch …
```

and a non-zero `LW` makes them download 68000 code for the printer's own
processor (`eexec` + `cexec`) instead of plain PostScript — System 7.1's
LaserWriter 7.1.2 does this for its smoothing procedures. A software
interpreter has no such processor, so we answer `LW = 0` and get the
generic path. The AppleTalk entity type advertised over NBP is still
`LaserWriter`, so the Chooser lists the printer as before. `version` is
`47.0` and must stay a number a driver can `cvr`.

## 5.1 Building with the interpreter

The library comes prebuilt from EfterScript's releases; no Rust toolchain is
needed. Every EfterScript tag `v<version>` attaches to its GitHub release a
host archive (`libplaten-<version>-<host triple>.a`), an Emscripten archive
named after the SDK it was built with
(`libplaten-<version>-wasm32-unknown-emscripten-<emsdk>.a`), the header
(`platen-<version>.h`) and `SHA256SUMS`. `src/core/network/laserwriter.mk`
pins the release in `PLATEN_VERSION` and, on the first `PLATEN=1` build,
`scripts/fetch_platen.sh` downloads what that build needs into
`local/platen/<version>/` and verifies it against the checksum file (a
present file is re-verified, never re-downloaded):

```sh
make -f Makefile.headless PLATEN=1     # fetches the host archive + header
make                                   # wasm (PLATEN=1 by default): the main module,
                                       #   then `platen-module` fetches the Emscripten
                                       #   archive and links build/platen-<version>.{js,wasm}
make PLATEN=0                          # wasm without the printer bridge or the module
make -f Makefile.headless PLATEN=1 PLATEN_VERSION=0.0.3   # another release
```

The two builds reach the library differently
([`laserwriter_job.md`](laserwriter_job.md)): headless links the host
archive into the emulator and calls it directly; the browser build is a
threaded module and the archive's Rust standard library is not, so there
the interpreter runs in its own Web Worker with its own module, and the
main module compiles only a shared-memory ring transport — the main link
names no archive. The Emscripten archive is what `make platen-module`
builds that worker module from (§5.5).

Two rules follow from how the archives are built. The Emscripten archive
links only with the SDK version in its name, so `PLATEN_EMSDK` follows
`EMSDK_REQUIRED_VERSION` and a newer EfterScript release that names a newer
SDK means bumping this tree's SDK in step. And Rust compiles that target
with WebAssembly exceptions on, so the wasm link passes `-fwasm-exceptions
-sWASM_LEGACY_EXCEPTIONS=1` (`PLATEN_WASM_LDFLAGS`); without it the
archive's exception tag is undefined at link time.

The host archive is published for `x86_64-unknown-linux-gnu`; a build on
another host (an arm64 devcontainer, macOS) needs either an EfterScript
release that adds that triple or a checkout: `PLATEN_DIR=/path/to/efterscript`
uses that checkout's `cargo build -p platen --release [--target
wasm32-unknown-emscripten]` outputs instead of the release.

The link adds the archive after the objects, followed (native only) by
exactly the system libraries a Rust staticlib reports needing on Linux
(`-lgcc_s -lutil -lrt -lpthread -lm -ldl -lc`). Toggling `PLATEN` rebuilds
the tree (a stamp prerequisite), so a switched value never mixes objects
compiled either way.

## 5.2 Headless options

* `--print-dir=DIR` (or `$GS_PRINT_DIR`) — write each finished job as
  `DIR/<job>-<title>.pdf`. Without it a finished job is logged and dropped.
  A `PLATEN=0` binary warns that it has no interpreter.
* `appletalk.printer.capture = true` — also write the raw PostScript to the
  spool file beside the PDF (off by default with the interpreter linked, on
  without it since the spool is then the only output).
* `appletalk.printer` observability: `interpreter`, `status`, `documents`,
  `last_pages`, `last_outcome`.

## 5.3 Identity and prelude

The interpreter is seeded with the product/version/revision identity and a
host prelude that makes `statusdict` look like a LaserWriter. The prelude
lives in the repository as `src/core/network/laserwriter_prelude.ps`,
embedded at build time; see [`laserwriter_job.md`](laserwriter_job.md).

## 5.4 Acceptance and a known fidelity gap

`tests/integration/appletalk-print/` drives the Chooser + Finder "Print
Directory" flow and asserts `appletalk.printer.documents == 1`,
`last_pages == 1`, `last_outcome == "ok"`, and an idle status. It is gated
on `appletalk.printer.interpreter`, so it skips on the harness's default
`PLATEN=0` build; run it against a `PLATEN=1` binary to exercise the bridge.

The prelude registers the 13 resident faces in `FontDirectory`, so the driver's font-list query reports them and the driver substitutes the printer's outline faces instead of downloading bitmap fonts; text in the PDF is then the resident Type 1 faces. (The registration was left out at first because the shorter substitute-font job tripped a link-layer fault; that fault is fixed, see the bridge note.)

## 5.5 The browser path: the module, the worker, the download

In the browser the interpreter runs in its own Web Worker, and the bridge
in the core reaches it through a shared-memory ring
(`src/core/network/laserwriter_ring_protocol.h`, mirrored in
`app/web2/src/printer/platenProtocol.ts`; the two carry
`LWRING_PROTOCOL_VERSION` and the worker refuses a control block of another
version). The pieces:

* **The module** — `make platen-module` (a prerequisite of `all` with
  `PLATEN=1`) links the released Emscripten archive into a standalone,
  non-threaded ES6 module, `build/platen-<version>.js` + `.wasm`, with
  every `platen_*` entry of `platen.h` exported and the link flags the
  embedding guide gives (`-fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1`,
  memory growth). It is served beside `main.mjs`: the Vite dev middleware
  reads it from `build/`, `make ui2` copies it into `dist/`. The main module
  learns the version at compile time (`GS_PLATEN_VERSION`, from
  `PLATEN_VERSION` in `laserwriter.mk`) so the page fetches the matching
  file, cache-busted like `main.mjs`.
* **The attach** — on the first print job the bridge allocates the ring and
  `laserwriter_ring_attach_requested` (`src/platform/wasm/em_main.c`) fires
  `Module.onPrinterAttach(ctrl, version)` on the main thread.
  `app/web2/src/printer/platen.ts` starts the worker then — the module is
  fetched only now, and a first print waits for it like a printer warming
  up (the bridge's open timeout is 120 s of guest time) — and posts the
  wasm memory and the control block's address. `laserwriter_ring_notify`
  wakes the worker with `emscripten_futex_wake` after every publish.
* **The worker** — `app/web2/src/printer/platen.worker.ts` loads the module
  (`platenLib.ts` wraps the C ABI: the `platen_config` is built in the
  module's own memory from the OPEN record, identity pairs included) and
  runs the loop in `platenRing.ts`: `Atomics.waitAsync` on `OUT_HEAD`,
  OPEN / FEED / FINISH / ABANDON consumed in order, OPENED / OPEN_FAILED /
  FED / FINISHED written back with the 8-byte record framing (a PAD before
  the ring's end, text fields cut at `LWRING_TEXT_MAX` with the truncation
  flag), the STAT words counting jobs and feeds. A failed module load, a
  version mismatch, a framing violation or a library exception sets the
  STATUS word to LOST, after which the core fails whatever it was waiting
  on and the session aborts with the reason in the status string.
* **The download** — the PDF never enters the core. At FINISH the worker
  posts `{jobId, title, name, pages, pdf}` to the page (the PDF
  transferred), and the page downloads it at once through a Blob and an
  anchor click, as `<job id, 5 digits>-<title>.pdf` — the title is the
  bridge's `%%Title:` name, carried in the FINISH record and made
  file-safe the way the headless print directory names its files
  (`00003-Macintosh_HD.pdf`); a query job that shows no page produces no
  download. A toast names each document. `em_main.c` has no document sink:
  the ring result carries no bytes.

The loop is testable without a browser: `app/web2/tests/unit/platenRing.test.ts`
loads the built module under Node, lays the rings out in a
`SharedArrayBuffer`, plays the core, and checks the answers' framing and
the posted PDF (it skips with a warning when `build/platen-*.js` is
absent, so `npm test` still runs in a checkout that has not built).
