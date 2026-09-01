# Granny Smith

[![CI](https://github.com/pappadf/granny-smith/actions/workflows/tests.yml/badge.svg)](https://github.com/pappadf/granny-smith/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Granny Smith** is a browser-first Macintosh and Apple Lisa emulator spanning everything from Lisa 2 to Power Macintosh 9500 and more that 15 computer models in between.

> **See it:** [Demos of PlainTalk speech recognition, A/UX, Marathon, and more](GALLERY.md)

> **Try it:** [Run Granny Smith in your browser](https://pappadf.github.io/gs-pages/latest/)
> Use Chromium browser (Safari and Firefox have known issues)

[![Live webcam video digitized by the emulated 840AV](https://i.ytimg.com/vi/zUiRTOQy7RM/hq720.jpg)](https://www.youtube.com/watch?v=zUiRTOQy7RM)

## Emulated Computer Models

In all models, the original ROMs runs without patches, and all on-board devices the software touches are emulated in enough detail to allow original device drivers to work.

- **Apple Lisa 2 and Macintosh XL**
- **Macintosh Plus**
- **Macintosh SE/30, IIx, and IIcx** (the "GLUE" family)
- **Macintosh IIfx**
- **Macintosh IIci and IIsi** (the "MDU" family)
- **Macintosh Quadra 700, 900, and 950** (the "MCU" family)
- **Macintosh Quadra 840AV and Centris 660AV** (the "AV" family)
- **Power Macintosh 6100, 7100, and 8100** (the "PDM" family)
- **Power Macintosh 7500, 8500, and 9500** (the "TNT" family)
- **Apple Network Server 500 and 700** ("Shiner" based on the "TNT" family)

## Emulated NuBus Cards

NuBus display cards can be seated in any machine with free slots, including machines that already have built-in video. Each card runs either from a dump of the real declaration ROM or from a runtime-generated generic declaration ROM (no ROM dump is required); the generic ROM can also synthesize custom resolutions (e.g. 800×600)

- **Apple Display Card 8•24** (the standard "JMFB" card)
- **Apple Display Card 24AC** (including hardware QuickDraw acceleration)
- **Apple Display Card 8•24 GC** (including hardware QuickDraw acceleration)

## Emulated PCI Cards

- **ATI Mach64 GX (Apple "Accelerated" PCI Card)** (including 2D hardware acceleration)
- **Cirrus Logic 54M30** (mainly used by ANS 700/500)
- **Symbios Logic 53C825A** (fast/wide SCSI with the on-chip SCRIPTS DMA engine)

## Verified Guest/Target Operating Systems

The emulated computer models have been tested with various combinations of the following operating systems:

- **Mac OS System 2 to 7.6**
- **A/UX 3.0.1**
- **Lisa Office System 3.1**
- **Lisa Xenix 3.0**
- **Lisa MacWorks XL 3.0**
- **Copland D11E4**
- **MkLinux DR3**
- **AIX 4.1.5 for Apple Network Servers**

## Work In Progress

- Power Macintosh 9500MP running BeOS
- Voodoo2 PCI Card

## Project Principles

Two principles guide the project: **stay true to the hardware**, and **keep it simple**. That may sound like a contradiction, as faithful hardware emulation rarely is simple, but we try to find a sweet spot between the two:

True to the hardware means that compatibility is achieved by behaving like the real underlying hardware, not by patching around differences. Machines boot their original, unpatched ROMs, and every needed on-board device the software touches is modeled. Chip behaviour is pinned against Apple documentation, chip datasheets, Apple's own system software sources.

Keeping it simple means for users that the emulator runs in the browser with no installation. For developers, it means a highly portable C99 core with no special runtime requirements, no JIT or code generators, relying on the compiler and modern hardware to acheive "enough" performance. Extensive automated tests keep verification simple, and the entire project has been created to be AI agent friendly.

## Getting Started

You will need a ROM image and a bootable system disk image for the machine you want to run.

1. **[Open Granny Smith](https://pappadf.github.io/gs-pages/latest/)** in any modern browser
2. On first launch, upload a ROM for your chosen model (anything from the Macintosh Plus to the Power Macintosh 8100, or a Lisa); it is persisted in the browser's OPFS storage, so you only need to do this once
3. In the **Machine Configuration** dialog, pick a model, choose RAM, and attach disk images to the floppy / SCSI / CD slots (and display cards to NuBus slots)
4. Click **Boot** - your session is checkpointed continuously in the background, so closing or reloading the tab won't lose state
5. Once running, you can drag-and-drop additional disk images directly onto the screen to insert them at runtime

Disk images can be raw (`.dsk`, `.img`), compressed (`.sit.hqx`), or packaged in `.zip` archives. They are decompressed transparently via the in-tree [peeler](src/peeler) library.

For build, test, and contribution instructions, see [CONTRIBUTING.md](CONTRIBUTING.md). Architecture and design docs live in [docs/guide/ARCHITECTURE.md](docs/guide/ARCHITECTURE.md), and coding guidelines in [AGENTS.md](AGENTS.md).

## Known Limitations

- **Safari** - known rendering and audio issues; not currently supported
- **Firefox** - works partially; some compatibility problems remain
- **Ethernet** - the Quadras' SONIC and the AV machines' MACE controllers are modeled at the register/self-test level but are not bridged to a network; networking is AppleTalk over LocalTalk (serial) only
- **Sound input** - not modeled on any machine; the Quadras' EASC currently runs as an ASC-compatible core, and the AV machines' Singer/AWACS sound is not modeled at all
- **LaserWriter** - printer is identified, but print jobs don't complete correctly

## A Note on AI

This project allows the use of AI (my project, my rules). This is not limited to code generation or code review; it also includes documentation and reverse engineering of the hardware involved.
There is no intention to track, at the file or commit level, which code was generated with AI assistance and which was not. I know that AI may be a red flag for some people, and I fully respect that.

## Related Projects

Sibling projects this one is built on:

- **[powerpc-sail](https://github.com/pappadf/powerpc-sail)** - a formal, executable Sail specification of the PowerPC ISA, and the oracle the PowerPC vectors are generated from
- **[powerpc-test](https://github.com/pappadf/powerpc-test)** - single-instruction test vectors for the PowerPC 601, generated from `powerpc-sail`; used here as the `third-party/powerpc-test` submodule
- **[m68k-test](https://github.com/pappadf/m68k-test)** - the same idea for the 68k, generated from an `m68k-sail` model
- **[peeler](https://github.com/pappadf/peeler)** - a C library for unpacking legacy Macintosh archive formats, which is what decompresses dropped-in disk images; vendored in-tree at [src/peeler](src/peeler)

## Acknowledgments

- [raddad772](https://github.com/raddad772) for the 68K test suite ([single-step-tests](https://github.com/SingleStepTests/m68000))
- [xterm.js](https://xtermjs.org/) for terminal emulation in the browser
- [JSZip](https://stuk.github.io/jszip/) for ZIP file handling
- [Emscripten](https://emscripten.org/) for the WebAssembly toolchain

## Trademarks

All trademarks referenced in this project are the property of their respective owners and are used for identification purposes only. This project does not claim any endorsement by or affiliation with the trademark holders.

## License

[MIT](LICENSE)
