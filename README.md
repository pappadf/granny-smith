# Granny Smith

[![CI](https://github.com/pappadf/granny-smith/actions/workflows/tests.yml/badge.svg)](https://github.com/pappadf/granny-smith/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Granny Smith** is a browser-first Macintosh and Apple Lisa emulator spanning three CPU generations, from the 68000 Lisa 2 and Macintosh Plus to the 68040 Quadra towers. It runs Mac OS (System 2 through System 7.1) and A/UX 3.0.1 on the Macintosh, and the Lisa Office System, MacWorks XL, and Xenix on the Lisa 2 / Macintosh XL.

> **See it:** [Screenshot gallery of A/UX, MacTest, and more](GALLERY.md) from the test system

> **Try it:** [Run Granny Smith in your browser](https://pappadf.github.io/gs-pages/latest/)
> Use Chromium browser (Safari and Firefox have known issues)

![A/UX 3.0.1 running on Macintosh IIfx](docs/assets/iifx_aux3.png)

## Emulated Machines

Each machine is modeled faithfully: the original ROMs run unpatched, and all on-board devices the software touches are emulated in enough detail that even timing-sensitive, close-to-hardware diagnostics like Apple's MacTest suites pass.

- **Apple Lisa 2 and Macintosh XL**
  - 68000 behind Apple's custom segment MMU; COPS microcontroller for keyboard, mouse, clock, and soft power; twin VIAs; SCC serial
  - ProFile parallel-port hard disk and 400 K Sony floppy driven by the Lisa's 6504 floppy coprocessor; 720×364 (Lisa) / 608×431 (XL screen mod) bitmapped video
  - Tested with Lisa Office System 3.1 and SCO Xenix 3.0, both as full multi-floppy installs onto the ProFile, and MacWorks XL to the Finder
- **Macintosh Plus**
  - 68000; IWM 400/800 K GCR floppy, NCR 5380 SCSI, SCC serial, pre-ADB keyboard and quadrature mouse, PWM sound, 512×342 built-in video
  - Tested with System 2.0 through System 7
- **Macintosh SE/30, IIx, and IIcx** (the 68030 GLUE generation)
  - 68030 with on-chip PMMU + 68882 FPU; ADB via the VIA transceiver, SWIM floppy, NCR 5380 SCSI with pseudo-DMA, ASC sound, SCC serial
  - SE/30 built-in 512×342 video with its real declaration ROM; IIx (six) and IIcx (three) NuBus slots for the display cards below
  - Tested with System 6.0.8 through 7.1, and A/UX 3.0.1; on the SE/30 this includes the full retail A/UX Installer flow from floppy + CD
- **Macintosh IIfx**
  - 68030 + 68882; OSS priority-arbitrated interrupt controller, FMC memory controller, RAM-parity probe
  - The two Apple I/O processors (65C02-based IOPs) in front of the SCC and the SWIM + ADB bus are emulated functionally, covering their firmware protocols (mailboxes, bypass mode); Apple's custom SCSI DMA engine wraps the on-chip 53C80
  - Tested with System 7.0.1, and A/UX 3.0.1 all the way to an X11 session
- **Macintosh IIci and IIsi** (the MDU/RBV generation)
  - 68030; RBV RAM-based built-in video with the Bt450 CLUT
  - IIci with the classic ADB/RTC path; IIsi with the Egret system-manager chip (ADB, PRAM, soft power over its single-wire protocol)
  - Tested with System 7.0.1
- **Macintosh Quadra 700, 900, and 950** (the 68040 MCU/DAFB generation)
  - 68040 with on-chip MMU and FPU; MCU memory controller with its access-triggered ROM overlay; NuBus '90 slots
  - DAFB built-in video (AC842/AC842a RAMDAC, up to Thousands in 16-bit x555 on the Quadra 950), NCR 53C96 SCSI with TurboSCSI pseudo-DMA, SONIC Ethernet controller, EASC sound
  - The 900/950 towers add the Caboose system manager (Egret protocol), functional emulations of the two IIfx-style IOPs in front of SCC and SWIM/ADB, and a second 53C96 for the external SCSI bus
  - Tested with System 7.1, booting from floppy or SCSI hard disk

## Emulated Display Cards

NuBus display cards can be seated in any machine with free slots, including as a second display next to built-in video:

- **Apple Macintosh Display Card 8•24** - the standard "JMFB" card, up to 24-bit color
- **Apple Macintosh Display Card 24AC** - 4 MB VRAM, 1-32 bpp, including its hardware QuickDraw fill/copy accelerator
- **Apple Macintosh Display Card 8•24 GC** - the RISC-accelerated "GC" card; QuickDraw acceleration is emulated at the graphics-processor RPC level (and, authentically, its INIT declines to install on a 68040)
- Each card runs either from a dump of its **real declaration ROM** or from a **runtime-generated generic declaration ROM**, so no ROM dump is required; the generic ROM can also synthesize custom resolutions (e.g. 800×600)

## Project Principles

Two principles guide the project: **model the hardware faithfully**, and **keep it simple**. That may sound like a contradiction, since faithful hardware emulation is rarely simple, but we try to make both work.

Fidelity means compatibility is earned by behaving like the real machine, never by patching around differences. Machines boot their original, unpatched ROMs, and every on-board device the software touches is modeled. Chip behaviour is pinned against Apple documentation, chip datasheets, Apple's own system software sources, and the machines' own diagnostics, and CI boots every machine and matches screens and boot chimes against golden references.

For users, simplicity means the emulator runs in the browser with no installation, sessions are checkpointed continuously in the background so closing or reloading the tab loses nothing, disk images can be dragged straight onto the screen (compressed `*.sit.hqx` archives included), and a built-in AFP file server bridges the browser/host filesystem into the guest OS.

For developers, it means a highly portable C99 core with no special runtime requirements and no JIT or code generators: a ~550-line shared instruction decoder and one compact opcode header serve all three CPU generations, relying on the compiler and modern hardware for performance. Extensive automated tests (unit, headless integration, and Playwright end-to-end) keep it honest, and the hardware documentation is written in Markdown so it serves human developers and AI coding agents alike.

## Getting Started

You will need a ROM image and a bootable system disk image for the machine you want to run.

1. **[Open Granny Smith](https://pappadf.github.io/gs-pages/latest/)** in any modern browser
2. On first launch, upload a ROM for your chosen model (anything from the Macintosh Plus to the Quadra 950, or a Lisa); it is persisted in the browser's OPFS storage, so you only need to do this once
3. In the **Machine Configuration** dialog, pick a model, choose RAM, and attach disk images to the floppy / SCSI / CD slots (and display cards to NuBus slots)
4. Click **Boot** - your session is checkpointed continuously in the background, so closing or reloading the tab won't lose state
5. Once running, you can drag-and-drop additional disk images directly onto the screen to insert them at runtime

Disk images can be raw (`.dsk`, `.img`), compressed (`.sit.hqx`), or packaged in `.zip` archives. They are decompressed transparently via the in-tree [peeler](src/peeler) library.

For build, test, and contribution instructions, see [CONTRIBUTING.md](CONTRIBUTING.md). Architecture and design docs live in [docs/guide/ARCHITECTURE.md](docs/guide/ARCHITECTURE.md), and coding guidelines in [AGENTS.md](AGENTS.md).

## Known Limitations

- **Safari** - known rendering and audio issues; not currently supported
- **Firefox** - works partially; some compatibility problems remain
- **Ethernet** - the Quadras' SONIC controller is modeled at the register/self-test level but is not bridged to a network; networking is AppleTalk over LocalTalk (serial) only
- **Sound input** - not modeled on any machine; the Quadras' EASC currently runs as an ASC-compatible core
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
