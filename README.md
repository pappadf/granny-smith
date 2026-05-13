# Granny Smith

[![CI](https://github.com/pappadf/granny-smith/actions/workflows/tests.yml/badge.svg)](https://github.com/pappadf/granny-smith/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Granny Smith** is a browser-first Macintosh emulator running Mac OS (System 2 through System 7) and A/UX.

> **See it:** [Screenshot gallery of A/UX, MacTest, and more](GALLERY.md) from the test system

> **Try it:** [Run Granny Smith in your browser](https://pappadf.github.io/gs-pages/latest/)
> Use Chromium browser (Safari and Firefox have known issues)

![A/UX 3.0.1 booting on SE/30](docs/assets/aux_boot.gif)

## Tested Configurations

Every ✅ is exercised on every CI run. Some other combinations may well work - they just aren't continuously verified.

| Software       |  Plus  | SE/30  |  IIcx  |  IIx   |
| -------------- | :----: | :----: | :----: | :----: |
| System 2.x     |   ✅   |    -   |    -   |    -   |
| System 3.x     |   ✅   |    -   |    -   |    -   |
| System 4.x     |   ✅   |    -   |    -   |    -   |
| System 6.x     |   ✅   |    -   |    -   |    -   |
| System 7.0.1 ‡ |    -   |   ✅   |   ✅   |   ✅   |
| System 7.1     |   ✅   |   ✅   |    -   |    -   |
| A/UX 3.0.1 †   |    -   |   ✅   |    -   |    -   |
| MacTest        |   ✅   |   ✅   |    -   |    -   |

† On SE/30, boots from a pre-installed HD image to a shell *and* the full retail Installer flow runs from floppy + CD.
‡ On IIx, only a no-crash boot smoke test - JMFB colour modes on the IIx profile are still minimum-viable.

## Design Principles

The guiding principle of this project is **"keep it simple"** - simple for users to run classic Macintosh software, and simple to develop and maintain.

**For users:**

- **Runs in the browser** - no installation required
- **Continuous background checkpointing** - close or reload the tab without losing your session
- **Drag-and-drop disk images** - even compressed `*.sit.hqx` archives are handled transparently
- **Built-in AFP file server** - bridge the browser/host filesystem into the guest OS

**For developers:**

- **Extensive automated testing** - unit tests, headless integration tests, and Playwright end-to-end tests
- **Highly portable C99 core** - no special runtime requirements
- **Simple CPU model** - no JIT compiler or meta tools; we rely on the compiler and modern hardware for performance
- **Compact and maintainable** - the entire CPU instruction decoder is under 550 lines; the 68000 opcode implementations fit in under 1,000 lines (68030 support excluded)
- **Comprehensive documentation** - hardware documentation in Markdown, accessible to both human developers and AI agents
- **AI-agent friendly** - repository organized to simplify work for coding agents

## Getting Started

You will need a Macintosh ROM image and a bootable system disk image.

1. **[Open Granny Smith](https://pappadf.github.io/gs-pages/latest/)** in any modern browser
2. On first launch, upload a **Macintosh Plus, SE/30, or compatible ROM** - it is persisted in the browser's OPFS storage, so you only need to do this once
3. In the **Machine Configuration** dialog, pick a model (Plus, SE/30, …), choose RAM, and attach disk images to the floppy / SCSI / CD slots
4. Click **Boot** - your session is checkpointed continuously in the background, so closing or reloading the tab won't lose state
5. Once running, you can drag-and-drop additional disk images directly onto the screen to insert them at runtime

Disk images can be raw (`.dsk`, `.img`), compressed (`.sit.hqx`), or packaged in `.zip` archives. They are decompressed transparently via the bundled [peeler](third-party/peeler) library.

For build, test, and contribution instructions, see [CONTRIBUTING.md](CONTRIBUTING.md). Architecture and design docs live in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), and coding guidelines in [AGENTS.md](AGENTS.md).

## Known Limitations

- **Safari** - known rendering and audio issues; not currently supported
- **Firefox** - works partially; some compatibility problems remain
- **A/UX runtime** - occasional residual errors during sessions
- **LaserWriter** - printer is identified, but print jobs don't complete correctly
- **AFP** - file content access not yet implemented (mounting and browsing only)

## Acknowledgments

- [raddad772](https://github.com/raddad772) for the 68K test suite ([single-step-tests](https://github.com/SingleStepTests/m68000))
- [xterm.js](https://xtermjs.org/) for terminal emulation in the browser
- [JSZip](https://stuk.github.io/jszip/) for ZIP file handling
- [Emscripten](https://emscripten.org/) for the WebAssembly toolchain

## Trademarks

All trademarks referenced in this project are the property of their respective owners and are used for identification purposes only. This project does not claim any endorsement by or affiliation with the trademark holders.

## License

[MIT](LICENSE)
