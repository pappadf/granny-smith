# Granny Smith

[![CI](https://github.com/pappadf/granny-smith/actions/workflows/tests.yml/badge.svg)](https://github.com/pappadf/granny-smith/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Granny Smith** is a browser-first 68000/68030 Macintosh emulator.
> **[Try Granny Smith instantly in your browser!](https://pappadf.github.io/gs-pages/latest/)**  
> _For best results, use a Chromium-based browser (Chrome, Edge, etc.). Safari has known issues._

![A/UX 3.0.1 CommandShell on SE/30](tests/integration/se30-aux3-boot/shell.png)

The guiding principle for this project is to "keep it simple" — simple for everyone to run classic Macintosh software as well as simple to develop and maintain the emulator itself.

## Simple for Users

- **Run in the browser** – no installation needed
- **Continuous background checkpointing** – reload or close the browser without losing your running session
- **Mount disk images by drag-and-drop** – even if they're compressed in `*.sit.hqx` format
- **Access the browser/host filesystem** through a built-in AFP file server

## Simple for Developers

- **Extensive automated testing** – unit tests, headless integration tests, and Playwright end-to-end tests
- **Highly portable C99 core** – no special runtime requirements
- **Simple CPU model** – no advanced JIT compiler or meta tools; we rely on the compiler and modern hardware to make it fast enough
- **Compact and maintainable** – the entire CPU instruction decoder is under 550 lines; the 68000 opcode implementations fit in under 1,000 lines (new 68030 support excluded)
- **Comprehensive documentation** – hardware documentation in Markdown format, easily accessible for both human developers and AI agents
- **AI agent friendly** – repository organized to simplify work for coding agents

## Getting Started

To run Granny Smith you need a Macintosh ROM image and a bootable system disk image.

1. **[Open Granny Smith](https://pappadf.github.io/gs-pages/latest/)** in any modern browser
2. Upload a **Macintosh Plus or SE/30 ROM** file when prompted by the configuration dialog
3. Select a machine model and drag a **bootable disk image** onto the window
4. The emulator boots automatically — your session is saved continuously in the background

Disk images can be raw (`.dsk`, `.img`), compressed (`.sit.hqx`), or packaged in `.zip` archives. The emulator decompresses them on the fly via the bundled [peeler](third-party/peeler) library.

### Running Locally

If you want to build and run Granny Smith locally for development:

```bash
make run               # Build and start HTTP server on :8080
```

This builds the WebAssembly version and starts a local server with the required COOP/COEP headers. See [CONTRIBUTING.md](CONTRIBUTING.md) for detailed build instructions.

## Current Status

### What Works

- Macintosh Plus (68000) and SE/30 (68030) fully emulated (no ROM patching)
- Machine configuration dialog with model selection and RAM sizing
- Accurate enough timing (MacTest hardware test suite runs without errors on both models)
- Mounting of compressed disk images (e.g., `*.sit.hqx`, `*.zip`) via the [peeler](third-party/peeler) library
- Background checkpointing and on-demand checkpointing/restore
- Browser filesystem accessible as an AFP share with authentication, browsing, and file read/write

### Known Limitations

- LaserWriter emulation incomplete – printer is identified but print jobs don't finish correctly
- AFP file content access not yet implemented (just mounting/browsing)

## Development

### Codespaces (Quick Start)

The easiest way to get up and running is by creating a GitHub Codespace based on the repository. From the granny-smith code page, click **Code** (the big green button) → **Codespaces** → **Create a Codespace on main**. It will take a minute or two, then you're up and running.

```bash
make          # Build the WASM emulator
make run      # Build and start the HTTP server on :8080
```

To pre-load boot media, pass paths (relative to the repo root) as `make` variables:

```bash
make run ROM=path/to/rom.bin HD0=path/to/hd.zip
```

Available options for the `run` target:

| Variable | Description |
|----------|-------------|
| `ROM=path/to/rom.bin` | ROM image |
| `FD0=path/to/floppy.img` | Floppy disk image |
| `HD0=path/to/hd.zip` … `HD7=…` | Hard disk images (zip or raw) |
| `SPEED=max\|realtime\|hardware` | Emulation speed |

Example:

```bash
make run ROM=tests/data/roms/Plus_v3.rom HD0=tests/data/systems/hd.zip
```

### Local Development

**Build and test instructions**: See [CONTRIBUTING.md](CONTRIBUTING.md)

**Architecture and design docs**: See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

**Coding guidelines**: See [AGENTS.md](AGENTS.md)

| Area | Directory |
|------|-----------|
| Browser frontend | [app/web/](app/web/) |
| CPU core | [src/core/cpu/](src/core/cpu/) |
| Peripherals | [src/core/peripherals/](src/core/peripherals/) |
| Machine definitions | [src/machines/](src/machines/) |
| Hardware documentation | [docs/](docs/) |

## Roadmap

- Finalize LaserWriter emulation
- Add an Electron build target (just placeholder today)
- Bootstrap A/UX

## Acknowledgments

- [raddad772](https://github.com/raddad772) for the 68K test suite ([single-step-tests](https://github.com/SingleStepTests/m68000))
- [xterm.js](https://xtermjs.org/) for terminal emulation in the browser
- [JSZip](https://stuk.github.io/jszip/) for ZIP file handling
- [Emscripten](https://emscripten.org/) for the WebAssembly toolchain

## Trademarks

All trademarks referenced in this project are the property of their respective owners and are used for identification purposes only. This project does not claim any endorsement by or affiliation with the trademark holders.

## License

[MIT](LICENSE)


