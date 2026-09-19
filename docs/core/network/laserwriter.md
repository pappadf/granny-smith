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
  wasm keeps it for a download UI). The placeholder query answers are gone
  — a query is just a job whose program prints its answer.

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
make PLATEN=1                          # wasm: nothing fetched (see below)
make -f Makefile.headless PLATEN=1 PLATEN_VERSION=0.0.3   # another release
```

The two builds reach the library differently
([`laserwriter_job.md`](laserwriter_job.md)): headless links the host
archive into the emulator and calls it directly; the browser build is a
threaded module and the archive's Rust standard library is not, so there
the interpreter runs in its own Web Worker with its own module, and the
main module compiles only a shared-memory ring transport — `make PLATEN=1`
links no archive and needs no fetch. The Emscripten archive named below is
what that worker module is built from (part 2B of the integration).

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

Over the live PAP path the driver queries the printer's resident fonts.
`platen`'s `FontDirectory` is empty at job start (its `findfont` resolves a
face but does not register it there), so the query reports no resident
fonts and the driver downloads a bitmap font, which the interpreter renders
at lower fidelity than the outline fonts a file-destination capture uses —
the page's layout matches the golden but its text glyphs are sparse. See
`docs/notes/2026-09-16-efterscript-platen-bridge.md` for the analysis and
the mitigation that is deferred because it trips a pre-existing guest-side
LocalTalk wedge.
