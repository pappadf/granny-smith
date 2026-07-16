# Peeler

[![CI](https://github.com/pappadf/peeler/actions/workflows/ci.yml/badge.svg)](https://github.com/pappadf/peeler/actions/workflows/ci.yml)
[![C99](https://img.shields.io/badge/C-99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Peeler** is a small C library for unpacking legacy Macintosh compression and archive formats.

Just as you peel an orange to get to the fruit inside, Peeler unpacks old Mac archives to extract their contents — `.sit` (StuffIt), `.cpt` (Compact Pro), `.hqx` (BinHex), and `.bin` (MacBinary) files.

## Formats Supported

- **StuffIt** (`.sit`) — including methods 13, 14, and 15 ("Arsenic")
- **Compact Pro** (`.cpt`)
- **BinHex** (`.hqx`) — 4.0 encoding
- **MacBinary** (`.bin`)

These formats were commonly used for distributing software and documents on classic Macintosh systems before the widespread adoption of `.zip` and other modern formats.

## AI-Generated Library

This library is, to a large degree, **AI-generated**.

The development process involved two stages of AI-assisted work:

1. **Information Gathering & Specification** — AI agents researched these formats from various sources, consolidating scattered documentation into comprehensive, self-contained format specifications (see [`docs/`](docs/)).

2. **Code Generation** — Based on those specifications, other AI agents generated the actual C implementation, test harness, and supporting infrastructure.

The goal was to demonstrate that with careful specification and iteration, AI can produce working implementations of complex, under-documented legacy formats.

## Documentation

Detailed format specifications are available in the [`docs/`](docs/) directory:

- [`sit15.md`](docs/sit15.md) — StuffIt Method 15 ("Arsenic") compression
- [`sit13.md`](docs/sit13.md) — StuffIt Method 13 compression
- [`sit.md`](docs/sit.md) — StuffIt archive structure
- [`cpt.md`](docs/cpt.md) — Compact Pro format
- [`hqx.md`](docs/hqx.md) — BinHex 4.0 encoding
- [`bin.md`](docs/bin.md) — MacBinary format
- [`architecture.md`](docs/architecture.md) — Library design overview

These specifications were synthesized from scattered resources and are intended to serve as standalone references for anyone implementing decoders for these formats.

## Building

```bash
make
```

This produces the `peeler` executable in the `build/` directory.

## Usage

```bash
./build/peeler <input-file>
```

The tool will automatically detect the format and extract the contents.

## Testing

```bash
cd test
./run_tests.sh
```

The test suite includes 61 test cases covering various StuffIt versions and compression methods, Compact Pro archives, BinHex encodings, and MacBinary wrappers.

## Information Sources

Information about these legacy formats was gathered from numerous sources, including but not limited to:

- [Matthew Russotto's Arsenic Compression Notes](http://www.russotto.net/arseniccomp.html)
- [The Unarchiver Wiki](https://github.com/mietek/theunarchiver/wiki)
- [bzip2 Format Specification](https://github.com/dsnet/compress/blob/master/doc/bzip2-format.pdf) (for BWT algorithm reference)
- [stuffit-rs](https://github.com/benletchford/stuffit-rs) (MIT license)
- [StuffItReader](https://github.com/hughbe/StuffItReader) (MIT license)

## Acknowledgements

- **Matthew Russotto** — for his detailed documentation of the StuffIt Method 15 ("Arsenic") compression format
- **Dag Ågren** — author of [The Unarchiver](https://theunarchiver.com/), whose open-source implementation helped establish community understanding of these formats
- **Stephan Sokolow** — for providing StuffIt test images that were invaluable for validation

## License

This project is licensed under the **MIT License**.

## Trademarks

StuffIt, Compact Pro, BinHex, MacBinary, and other product names mentioned in this project are trademarks or registered trademarks of their respective owners. These names are used solely for identification purposes and do not imply any affiliation with or endorsement by the trademark holders.
