# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
