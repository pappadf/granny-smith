# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v0.4.4] — 2026-05-19

### Fixed
- Macintosh IIfx scheduler-event registration: the SWIM IOP scheduled three periodic events (`swim_main_loop_tick`, `swim_drive_poll_tick`, `swim_adb_response`) but never registered them with the scheduler. Symptom was a recurring `scheduler_checkpoint` assertion ("event at timestamp N has no registered type") on every background-checkpoint save once IIfx booted to A/UX Startup. Added the missing `scheduler_new_event_type` calls via a new `register_events` callback in `iop_behavior_t`, wired from `iop_init`.
- Cross-machine scheduler state leak in `debug_mac.c::mouse_guard_tick` / `trace_mouse` and `appletalk.c::atp_register_scheduler_events`: each guarded its registration with a translation-unit-local `static bool`, so the second `machine.boot` (boot-matrix style) hit a fresh scheduler with `num_event_types = 0` while the flag still said "already registered". Dropped the redundant guards — `scheduler_new_event_type` is idempotent and updates entries in place.

### Added
- Schedule-time assertion in `scheduler_new_cpu_event`: rejects calls for any `(callback, source)` pair that wasn't first registered with `scheduler_new_event_type`. Previously this class of bug surfaced ~30 s later at the next checkpoint save, with no useful provenance. The new assert fingers the offending caller at the actual schedule site. Caught three latent bugs the same commit fixes.
- **Staging deployment workflow** at `.github/workflows/publish-staging.yml`. Manual (`workflow_dispatch`) trigger that builds the WASM + new UI bundle and pushes it to a SEMI-HIDDEN `/staging/` directory on `gs-pages`. Never touches `/latest/`, never rewrites the landing page — so it's reachable only via `https://pappadf.github.io/gs-pages/staging/` (which testers must know). New release process: merge → run `publish-staging` → exercise on real GitHub Pages → if good, cut a tagged Release to fire the existing `publish.yml`. Catches deploy-environment bugs (origin-rooted URLs, COI service worker handshake, etc.) BEFORE they land under a real version tag.

## [v0.4.3] — 2026-05-18

### Fixed
- Welcome-view HD / CD-ROM upload validation no longer requires a running machine. The `scsi.identify_hd` and `scsi.identify_cdrom` methods are now mounted on a static `scsi` singleton at root (via `scsi_class_register` from `shell_init`), matching the pattern `rom.identify` already follows. Per-machine `scsi_init` swaps the singleton out for the full bus-bound object; `scsi_delete` restores it. Symptom in v0.4.2 was "'…' is not a valid Hard Disk image" on fresh deploys, because cold-boot pages have no machine on the tree yet.

### Added
- **Production-bundle smoke test** under `tests/e2e/ui-prod-smoke/`. Builds `app/web2/dist/`, serves it from a SUBPATH (`127.0.0.2:18181/sub/path/`) with NO COOP/COEP headers, then asserts the page reaches `window.__gsReady === true`, `crossOriginIsolated === true`, and produces no console errors or 4xx responses. Mirrors the GitHub Pages deploy environment — would have caught the v0.4.0, v0.4.1 and v0.4.2 deploy regressions on the original push. Runnable via `make ui2-prod-smoke` and wired into the `ui` CI job.
- **Static dist validation** at `scripts/check-dist.mjs`: parses `dist/index.html`, fails on any origin-rooted `src=` / `href=` and on a missing COI service worker. Cheap (sub-second, no browser); runs immediately after Vite build in CI. Runnable locally via `make ui2-check-dist`.
- `app/web2/public/coi-serviceworker.js` — copied into Vite's `public/` so the bundle is self-contained even without `make ui2`.

## [v0.4.2] — 2026-05-18

### Fixed
- GitHub Pages deployment, take two: dynamic `import('./main.mjs')` inside the Vite bundle resolves relative to the bundle's URL, not the document, so it fetched `…/assets/main.mjs` and 404d. Also the codicon sprite was referenced as `/icons/sprite.svg`. Both are now resolved against `document.baseURI` so the bundle works under any deploy path.

## [v0.4.1] — 2026-05-18

> **Note:** v0.4.1's deploy fix was incomplete — the bundle still 404s on `main.mjs` and the icon sprite under `gs-pages/<version>/`. Use [v0.4.2](#v042--2026-05-18) instead.

### Fixed
- GitHub Pages deployment: Vite's default `/`-rooted asset paths 404 under the `gs-pages/<version>/` and `gs-pages/latest/` subpaths, so v0.4.0 loaded a blank page in production. Switched to relative asset URLs and re-registered the COOP/COEP service worker that the legacy UI shipped.

## [v0.4.0] — 2026-05-18

> **Note:** v0.4.0 does not deploy correctly to GitHub Pages — the published bundle loads a blank page. Use [v0.4.2](#v042--2026-05-18) instead. The source code itself is unaffected; the issue is in the deploy step.

### Added
- **Macintosh IIcx, IIx, and IIfx** — first non-SE/30 68030 machines; boot 7.x to Finder
- **Color graphics** — NuBus subsystem with the JMFB display adapter (8-bit and deeper modes)
- **New browser UI** — Svelte 5 + Vite + TypeScript frontend with overhauled panels (Terminal, Logs, Machine, Filesystem, Images, Checkpoints, Debug)
- **Typed object model** — `cpu.pc`, `memory.peek.bytes(...)`, etc. as a single dispatch tree replacing the legacy shell command surface

### Changed
- Legacy UI retained at `app/web-legacy/` (reachable via `make run-legacy`) during soak

### Fixed
- MMU hot-path performance: force-inlined `phys_to_host` / `phys_read32` / `phys_is_writable`; lazy identity-SoA install
- Numerous smaller fixes uncovered by the IIcx / IIfx bring-up and a code-review sweep across CPU, SCSI, ASC, IOP, and scheduler

## [v0.3.0] — 2026-05-01

### Added
- **A/UX 3.0.1 boot and install on SE/30** — boots from a pre-installed HD image, and runs the retail Easy Install from boot floppy + CD-ROM end-to-end
- CD-ROM emulation (AppleCD SC Plus / Sony CDU-8002)
- Tab completion and a richer headless debugger (find, logpoint value filters, MMU/SoA inspection)

### Fixed
- 68030 PMMU and CPU bus-error retry semantics across `MOVEM`/`MOVE`/`PUSH`/`CAS`/etc., so demand-paged faults restart cleanly
- NCR 5380 SCSI: non-arbitrated pseudo-DMA, ATN/MESSAGE OUT, and status-phase handling needed by the A/UX kernel
- Numerous smaller MMU, SCC, ADB, IWM, scheduler, and disassembler issues uncovered while bringing up A/UX

## [v0.2.1] — 2026-04-11

### Fixed
- GitHub Pages deployment: add COOP/COEP service worker to enable SharedArrayBuffer (required by pthreads since v0.2.0)

## [v0.2.0] — 2026-04-09

### Added
- Macintosh SE/30 emulation: 68030 CPU, MMU, FPU, ASC sound, ADB
- Machine configuration dialog with model selection, RAM sizing, and media dropdowns
- Unified upload pipeline: validate, extract archives (StuffIt/BinHex/ZIP), and persist to OPFS
- Unified command architecture (`fd insert/create/probe/validate`, `hd attach/validate`, `cdrom validate/attach`, `scc loopback`, `hd loopback`)
- Configurable RAM size per machine model
- Headless daemon mode with TCP shell for scripted debugging
- SCSI and SCC loopback test modes for MacTest compatibility
- 13 integration tests (boot, checkpoint, MacTest, SCSI, floppy, A/UX)
- E2E Playwright tests for config dialog and upload workflow

### Changed
- Storage backend migrated from IDBFS to OPFS for better performance and larger file support
- Media uploads stage to OPFS directly (no WASM heap pressure for large images)
- Floppy subsystem rewritten for accurate timing (MacTest-verified on Plus and SE/30)

### Fixed
- FPU: transcendentals, FMOD/FREM, packing, FMOVEM, FRESTORE null state
- SIT archive extraction for archives containing folders (peeler submodule)
- Path quoting for filenames with spaces throughout upload and boot pipeline

### Removed
- Legacy shell commands (`insert-fd`, `attach-hd`, `insert-disk`, `new-fd`, `scc-loopback`, `scsi-loopback`) replaced by unified commands

## [v0.1.0] — 2026-02-15

### Added
- Checkpoint save/restore dialog on page load

### Fixed
- Checkpoint behaviour at startup
- Flaky play/pause e2e test
- Simplified CI/CD configuration

## [v0.0.0] — 2026-02-15

### Added
- Initial open-source release
- Macintosh Plus emulation: 68000 CPU, VIA, SCC, SCSI, IWM floppy, keyboard, mouse
- Browser-based UI with WebAssembly (Emscripten), xterm.js terminal
- Drag-and-drop ROM and disk image loading
- URL parameter boot (`rom=`, `fd0=`, `hd0=`)
- AppleTalk networking (LocalTalk emulation, ATP/ASP/AFP protocols)
- Devcontainer with prebuilt image for development
- E2E test suite (Playwright) and CI via GitHub Actions
