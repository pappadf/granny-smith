# MacBinary (Macintosh Binary Transfer Format) — Format Specification

## 1  Introduction

### 1.1  What is MacBinary?

MacBinary is a binary container format designed to serialize a single classic
Macintosh file — including both its **data fork** and **resource fork**, plus
all Finder metadata — into a flat byte stream suitable for storage or transfer
on non-Macintosh systems.

The format wraps a Macintosh file in a simple, predictable structure: a
**128-byte header** containing all metadata, followed by the raw contents of
the data fork, then the resource fork, each padded to a 128-byte boundary.
This self-describing, block-aligned layout made MacBinary files easy to
transmit over modems, store on foreign filesystems, or embed inside other
archive formats.

### 1.2  Historical Context

**The dual-fork problem:**  Unlike DOS, Unix, or Windows filesystems, the
classic Mac OS filesystem (MFS, later HFS) stored every file as two separate
byte streams:

* **Data fork** — the file's primary content (document text, pixel data,
  raw audio, etc.).
* **Resource fork** — structured data such as dialog layouts, icons, menus,
  code segments, and version strings.  Many classic Mac applications kept
  essentially all their executable code in the resource fork.

Both forks had to be preserved for Macintosh applications to function
correctly.  The filesystem also recorded a 4-byte **File Type** (e.g.
`"TEXT"`), a 4-byte **Creator** code (identifying the originating
application), 16-bit **Finder flags** (visibility, bundle, icon state), and
positional metadata (window coordinates) as part of the directory entry —
none of which had equivalents on other platforms.

When users needed to transfer Mac files over electronic mail, FTP, or BBS
systems — or store them on MS-DOS floppies — the resource fork and metadata
would be silently lost, rendering many files useless.

**MacBinary as the solution:**  Dennis Brothers, Harry Chesley, and Yves
Lempereur (among others in the early Mac community) developed MacBinary as a
community standard through open discussion on CompuServe in 1985.  The
format's design priorities were simplicity and robustness: a fixed-size header,
straightforward big-endian integer encoding, and 128-byte block alignment
(matching the Xmodem transfer protocol's block size, which was ubiquitous at
the time).

### 1.3  Format Versions

Three versions exist:

* **MacBinary I** (1985) — the original format.  Header fields cover the
  filename, type/creator, basic Finder flags, fork lengths, and dates.  No
  header CRC.
* **MacBinary II** (1987) — the de facto standard.  Adds a CRC-16 checksum
  over the header, Finder flags low byte, secondary header support, and
  several reserved fields.  Nearly all MacBinary files encountered in the wild
  are MacBinary II.
* **MacBinary III** (1997) — a minor revision (writer version byte 130) adding
  a few fields.  Very rarely encountered and not covered in detail here.

An important extension, **MacBinary II+** (1993), extends MacBinary II to
package entire **directory trees** by bracketing folders with special
start/end delimiter blocks.  MacBinary II+ saw limited adoption but is
described fully in §11.

### 1.4  Scope

This document is a complete, self-contained *format specification* for
MacBinary II and MacBinary II+.  It describes every field, algorithm, and
validation rule needed to build a fully compatible encoder or decoder.  It is
not tied to any particular implementation.

---

## 2  Global Conventions

Before examining the format in detail, three low-level conventions apply
uniformly throughout the entire format.

### 2.1  Byte Order

All multi-byte integers in MacBinary are **big-endian** (most-significant byte
first), reflecting the Motorola 68000 processor used in early Macintosh
computers.  A 32-bit integer stored at offset *p* is read as:

```
value = (byte[p] << 24) | (byte[p+1] << 16) | (byte[p+2] << 8) | byte[p+3]
```

Similarly, a 16-bit integer:

```
value = (byte[p] << 8) | byte[p+1]
```

### 2.2  Block Alignment

The header is exactly **128 bytes**.  Each subsequent section — data fork,
resource fork, optional secondary header, optional Finder comment — is padded
with `0x00` bytes to the next **128-byte boundary**.  That is, if a section
contains *N* raw bytes, the padding is `(128 − (N mod 128)) mod 128` zero
bytes.

This 128-byte alignment was chosen to match the block size of the Xmodem
file-transfer protocol, which was the dominant binary protocol on dial-up
bulletin board systems in the mid-1980s.

### 2.3  Zero-Fill Rule

Any header byte not explicitly defined by the specification **must** be set to
zero by encoders.  Decoders should not assume undefined bytes have any
particular value, but the all-zeros convention aids format detection and
forward compatibility.

---

## 3  MacBinary II File Layout

A MacBinary II file has the following high-level structure:

```
Offset 0
┌───────────────────────────────────┐
│         Header (128 bytes)        │  ← Metadata: name, type, sizes, CRC
├───────────────────────────────────┤
│   [Secondary Header, if any]      │  ← Rare; length at header offset 120
│   [... padded to 128-byte boundary]│
├───────────────────────────────────┤
│         Data Fork (D bytes)       │  ← Raw data fork content
│   [... padded to 128-byte boundary]│
├───────────────────────────────────┤
│       Resource Fork (R bytes)     │  ← Raw resource fork content
│   [... padded to 128-byte boundary]│
├───────────────────────────────────┤
│   [Finder Comment, if present]    │  ← Optional; length at header offset 99
│   [... padded to 128-byte boundary]│
└───────────────────────────────────┘
```

The secondary header (§5) and Finder comment (§7) are both optional and
rarely present.  In the overwhelmingly common case, the file contains just
the 128-byte header, the data fork (padded), and the resource fork (padded).

---

## 4  The 128-Byte Header

The header is the heart of the MacBinary format.  It encodes the complete
Finder metadata for the original Macintosh file, the lengths of both forks,
and (in MacBinary II) a CRC-16 integrity check.

### 4.1  Field Table

Offsets are 0-based from the start of the file.  Types: Byte = 8 bits,
Word = 16 bits, Long = 32 bits.

| Offset | Size | Name | Description |
|-------:|-----:|------|-------------|
| 0 | 1 | Old version number | Must be **0** for MacBinary II file records.  (MacBinary II+ sets this to 1 for folder delimiters; see §11.)  Used as the first validation check. |
| 1 | 1 | Filename length | Length of the filename, 1–63 inclusive.  The filename is stored in Pascal string style: this length byte followed by the character data. |
| 2–64 | up to 63 | Filename | Only the first *length* bytes are significant.  Remaining bytes in this region must be zero.  No trailing NUL is required (the length byte delimits the string). |
| 65 | 4 | File type | Four ASCII characters identifying the Mac OS file type (e.g. `"TEXT"`, `"APPL"`). |
| 69 | 4 | Creator | Four ASCII characters identifying the creating application. |
| 73 | 1 | Finder flags (high byte) | Bits 15–8 of the 16-bit Finder `fdFlags` word.  See §4.2 for bit definitions. |
| 74 | 1 | Zero fill | Must be **0**.  This byte is part of the validation criteria (§6). |
| 75 | 2 | Vertical position | File's vertical position within the Finder window. |
| 77 | 2 | Horizontal position | File's horizontal position within the Finder window. |
| 79 | 2 | Window/folder ID | Finder window or folder ID where the file appeared. |
| 81 | 1 | Protected flag | Low-order bit: 1 = file is "protected" (a Finder-level write-protect). |
| 82 | 1 | Zero fill | Must be **0**.  Also used in MacBinary I fallback validation (§6). |
| 83 | 4 | Data fork length | Length of the data fork in bytes.  May be 0.  Historical upper bound recommendation: ≤ `0x007FFFFF` (~8 MB). |
| 87 | 4 | Resource fork length | Length of the resource fork in bytes.  May be 0.  Same upper bound recommendation. |
| 91 | 4 | Creation date | Macintosh epoch timestamp: seconds since 1904-01-01 00:00:00 UTC. |
| 95 | 4 | Modification date | Same epoch. |
| 99 | 2 | Get Info comment length | If non-zero, a Finder comment of this length follows the resource fork (§7).  In practice, nearly always 0. |
| 101 | 1 | Finder flags (low byte) | Bits 7–0 of the 16-bit Finder `fdFlags` word.  New in MacBinary II — MacBinary I had only the high byte at offset 73. |
| 102–115 | 14 | Reserved | Must be zero.  Some programs have historically used this area to store packaging information, but the spec requires zero-fill. |
| 116 | 4 | Unpacked length | For on-the-fly packers (e.g. PackIt).  When encoding a single unpacked file, write **0**.  Decoders that don't handle packed data can ignore this. |
| 120 | 2 | Secondary header length | If non-zero, a secondary header of this many bytes (padded to 128-byte boundary) follows immediately after the main header (§5).  **Must be 0** for normal MacBinary II files; exists for future expansion. |
| 122 | 1 | Writer version | The MacBinary version that wrote this file.  Starts at **129** for MacBinary II.  (MacBinary III uses 130.) |
| 123 | 1 | Minimum version needed | The minimum MacBinary version required to read this file.  Set to **129** for MacBinary II compatibility.  If a decoder's capability level is less than this value, it should treat the content as opaque binary and warn the user. |
| 124 | 2 | Header CRC-16 | CRC-16/XMODEM computed over header bytes 0–123 (inclusive).  See §5 for the algorithm.  New in MacBinary II. |

### 4.2  Finder Flags

The 16-bit Finder flags word is split across two non-contiguous header bytes:
the **high byte** at offset 73 and the **low byte** at offset 101.  To
reconstruct the full flags word:

```
finder_flags = (header[73] << 8) | header[101]
```

The individual bits (as defined by the classic Mac OS Finder) include:

| Bit | Name | Meaning |
|----:|------|---------|
| 0 | kIsOnDesktop | File was on the Desktop (internal, do not set) |
| 1 | bFOwnAppl | Internal use by the Finder |
| 4 | kHasBundle | File has a BNDL resource |
| 5 | kIsInvisible | File is invisible in the Finder |
| 6 | kIsStationery | File is a stationery pad |
| 7 | kNameLocked | File name is locked |
| 8 | kHasBeenInited | Finder has recorded the file's position |
| 9 | kHasCustomIcon | File has a custom icon |
| 10 | kIsShared | File can be launched multiple times |
| 11 | kHasNoINITs | No INIT resources in this file |
| 13 | kIsAlias | File is an alias (System 7+) |

Not all bits are relevant to every application.  The important ones for
MacBinary post-decode sanitization are listed in §8.

### 4.3  Macintosh Epoch Dates

The creation and modification timestamps at offsets 91 and 95 use the
**Macintosh epoch**: the number of seconds since midnight, January 1, 1904.
This is a 32-bit unsigned value, providing coverage through approximately
February 6, 2040.

To convert to Unix epoch (seconds since 1970-01-01):

```
unix_time = mac_time − 2082844800
```

The constant 2,082,844,800 is the number of seconds between the two epochs
(66 years, including 17 leap years).

---

## 5  Header CRC

MacBinary II added a CRC-16 integrity check to the header, making it possible
to detect accidental corruption and to reliably distinguish MacBinary II files
from random data or MacBinary I files.

### 5.1  CRC Algorithm: CRC-16/XMODEM

The CRC variant used by MacBinary II is **CRC-16/XMODEM**, also known as
"CRC-CCITT (FALSE)" or "CRC-16-CCITT (init 0)" in various libraries.  The
parameters are:

| Parameter | Value |
|-----------|-------|
| Polynomial | 0x1021 (x^16 + x^12 + x^5 + 1) |
| Initial value | 0x0000 |
| Input reflected | No |
| Output reflected | No |
| Final XOR | 0x0000 |

Multiple independent references (CiderPress2, Maconv, various Mac developer
archives) confirm these parameters.

### 5.2  Coverage

The CRC is computed over header bytes **0 through 123** (inclusive) — that is,
everything except the 2-byte CRC field itself (at offsets 124–125) and the
2 unused bytes at offsets 126–127.

### 5.3  Storage

The resulting 16-bit CRC value is stored in **big-endian** order at header
offsets 124–125.

### 5.4  Implementation Tip

Most CRC libraries expose this variant as `CRC-16/XMODEM` or provide a
generic CRC-16 function where you can specify polynomial 0x1021 with initial
value 0.  A quick sanity check: a header of all zero bytes produces a CRC of
**0x0000** — which is important to keep in mind for validation (see §6).

A typical table-driven implementation processes one byte at a time:

```
crc = 0x0000
for each byte b in header[0..123]:
    index = ((crc >> 8) ^ b) & 0xFF
    crc = (crc << 8) ^ table[index]
    crc &= 0xFFFF
```

where `table` is the standard 256-entry CRC-16/XMODEM lookup table.

---

## 6  Header Validation

Reliably detecting whether a byte stream is a MacBinary file is non-trivial,
because the format has no unique magic number — the first byte is 0, and many
header fields can legitimately be zero.  A robust decoder should apply the
following checks, in order.

### 6.1  Primary Checks

1. **Byte 0 must be 0** (for MacBinary II file records; value 1 indicates
   MacBinary II+ folder blocks — see §11).
2. **Byte 74 must be 0.**

If either check fails, the data is not a MacBinary II file.

### 6.2  CRC Verification

If both primary checks pass, compute CRC-16/XMODEM over bytes 0–123 and
compare with the stored value at offsets 124–125:

* **CRC matches** → the data **is** MacBinary II.  Proceed with decoding.
* **CRC does not match** → check **byte 82**: if it is 0, the data **may** be
  MacBinary I (which predates the CRC field).  A decoder may choose to accept
  this as a MacBinary I file and proceed cautiously.  If byte 82 is non-zero,
  reject.

### 6.3  Additional Sanity Heuristics

These optional checks improve robustness against false positives:

* **Filename length** (offset 1) must be in the range **1–63**.
* **Fork lengths** (offsets 83 and 87) should each be ≤ **0x007FFFFF**
  (~8 MB).  This is a historical recommended bound; some implementations
  accept larger values within the 31-bit range (`0x7FFFFFFF`).
* **Offsets 101–125** should all be **0** unless you are specifically testing
  for MacBinary II features.
* **Writer version** (offset 122) should be **129** for MacBinary II or
  **130** for MacBinary III.

### 6.4  The All-Zeros Pitfall

A header consisting entirely of zero bytes produces a CRC of 0x0000, which
would match the stored CRC (also zero).  A simplistic validator that checks
*only* the CRC would incorrectly accept this as valid MacBinary.  The filename
length check (offset 1 must be ≥ 1) catches this edge case.

---

## 7  Finder Comment (Get Info)

If the **Get Info comment length** at header offset 99 is non-zero, a Finder
Comment of that many bytes is present in the stream **after** the resource fork
(and its alignment padding).  This comment corresponds to the "Get Info"
comment visible in the Mac OS Finder.

The comment section is padded to the next 128-byte boundary, like all other
sections.

### 7.1  Practical Status

Apple discouraged programs from reading and writing Finder comments at the
time MacBinary was designed, and the feature saw minimal use.  The vast
majority of MacBinary files have a comment length of 0.  Encoders should write
**0**; decoders should handle the field if present but may safely ignore its
content.

---

## 8  Finder Flags Sanitization

When **decoding** (downloading) a MacBinary file and reconstructing it on a
Macintosh filesystem, certain Finder flag bits and positional metadata must be
**cleared** to avoid stale or inappropriate UI state.  These bits were valid
in the context of the original user's desktop but are meaningless (or actively
confusing) when the file appears in a new location.

### 8.1  Bits to Clear

| Bit | Name | Why Clear It |
|----:|------|-------------|
| 0 | kIsOnDesktop | Desktop placement state; stale in a new context. |
| 1 | bFOwnAppl | Internal Finder bookkeeping; not user-meaningful. |
| 8 | kHasBeenInited | Tells the Finder the file's position has been recorded; must be re-initialized in the new location. |
| 9 | kHasCustomIcon | Should be re-evaluated after placement (sometimes listed; clearing is conservative). |
| 10 | kIsShared | Shared-launch state; stale in a new context. |

A bitmask to clear these bits from the 16-bit flags word:

```
sanitized = finder_flags & ~((1 << 0) | (1 << 1) | (1 << 8) | (1 << 9) | (1 << 10))
```

### 8.2  Positional Fields to Zero

Additionally, zero out:

* **`fdLocation`** — the vertical and horizontal positions (offsets 75–78).
* **`fdFldr`** — the window/folder ID (offsets 79–80).

These values place the file at a specific pixel coordinate in a specific
Finder window on the *sender's* machine.  Zeroing them causes the Finder to
assign a default position.

---

## 9  Secondary Header

The 16-bit field at header offset 120 specifies the length of an optional
**secondary header** that appears immediately after the main 128-byte header,
before the data fork.

### 9.1  MacBinary II Behavior

For standard MacBinary II file records, this field **must** be 0.  The
secondary header mechanism was designed as an extension point for future
versions.

### 9.2  When Present

If the secondary header length is non-zero:

1. Read that many bytes immediately after the 128-byte main header.
2. Skip padding to the next 128-byte boundary (same alignment rule as all
   other sections).
3. The data fork begins at the next 128-byte-aligned offset.

MacBinary II+ folder blocks (§11) may use non-zero secondary headers.

---

## 10  Data and Resource Forks

### 10.1  Layout

After the main header (and optional secondary header), the forks appear in
strict order:

1. **Data fork**: exactly *D* bytes (from header offset 83), followed by
   padding to the next 128-byte boundary.
2. **Resource fork**: exactly *R* bytes (from header offset 87), followed by
   padding to the next 128-byte boundary.

Either fork may have length 0.  A zero-length fork occupies no space in the
stream (no padding is needed for a zero-length section, since
`(128 − (0 mod 128)) mod 128 = 0`).

### 10.2  Reading Strategy

Always use the fork lengths from the header to determine how many bytes to
read.  **Never** infer fork lengths from the file size — the file might be
truncated, or the padding calculation would be ambiguous.

After reading *D* bytes of data fork, compute and skip the padding:

```
data_padding = (128 − (D mod 128)) mod 128
```

Then read *R* bytes of resource fork and skip its padding similarly.

### 10.3  Fork Selection Heuristic

Some MacBinary files (particularly self-extracting archives with the `.sea.bin`
extension) contain a StuffIt archive in the **resource fork** rather than the
data fork.  The data fork in these cases contains a small executable stub.

A decoder that feeds its output to a downstream archive extractor (e.g. a
StuffIt decoder) may want to apply a **heuristic**: if the data fork does
*not* begin with a recognized archive signature (such as `"SIT!"`, `"ST46"`,
or the SIT5 header) but a resource fork is present, prefer streaming the
resource fork to the downstream layer.

This is an implementation strategy, not a format requirement.

---

## 11  MacBinary II+ (Directory Trees)

### 11.1  Motivation

MacBinary II encodes a single file.  When users needed to transfer entire
folder hierarchies, they had to either archive the folder first (e.g. into a
StuffIt archive) or transmit each file individually.  MacBinary II+ extends
the format to carry **folders and their contents** by inserting special
128-byte **Start Block** and **End Block** delimiters that bracket each folder,
similar in concept to how `tar` represents directories.

MacBinary II+ was proposed in 1993 and implemented by a small number of tools.
Its adoption was limited; most real-world MacBinary files are plain MacBinary
II single-file records.  Nevertheless, a complete implementation should handle
it.

### 11.2  Stream Layout

A MacBinary II+ stream **must** begin with a Start Block representing the root
folder.  Files inside are encoded as standard MacBinary II file records.
Subfolders are represented recursively with their own Start/End Block pairs.

```
StartBlock("Root")                      ← version byte = 1
  [optional Secondary Header]
  [optional Finder Comment]
  File 1 (MacBinary II record)          ← version byte = 0
  File 2 (MacBinary II record)          ← version byte = 0
  StartBlock("Subfolder A")             ← nested folder
    File 3 (MacBinary II record)
  EndBlock                              ← closes "Subfolder A"
  File 4 (MacBinary II record)
EndBlock                                ← closes "Root"
```

All blocks (Start, End, and file records) are padded to 128-byte boundaries,
exactly like MacBinary II.

### 11.3  Start Block (Folder Begin)

A Start Block is a 128-byte block with the same field layout as a MacBinary II
header, but with specific values to identify it as a folder delimiter:

| Offset | Size | Value / Rule |
|-------:|-----:|-------------|
| 0 | 1 | **1** — marks this as a MacBinary II+ block (incompatible with older decoders). |
| 1 | 1 | Folder name length (1–63). |
| 2–64 | up to 63 | Folder name (Pascal-style, zero-padded). |
| 65 | 4 | File type = **`'fold'`** (0x666F6C64). |
| 69 | 4 | Creator = **`0xFFFFFFFF`** — distinguishes Start Blocks from End Blocks. |
| 73–81 | varies | Finder flags, zero-fill, position, window ID, protected flag — same layout as MacBinary II. |
| 83 | 4 | Data fork length = **0** (folders have no data fork). |
| 87 | 4 | Resource fork length = **0** (folders have no resource fork). |
| 91 | 4 | Creation date for the folder. |
| 95 | 4 | Modification date for the folder. |
| 99 | 2 | Comment length (may be > 0 if a Finder comment is attached to the folder). |
| 101 | 1 | Finder flags (low byte). |
| 116 | 4 | Unpacked total length of all files when unpacked.  May be **0** to avoid requiring a pre-parse. |
| 120 | 2 | Secondary header length — **may be non-zero** for Start Blocks.  If so, the secondary header follows immediately, padded to 128 bytes. |
| 122 | 1 | Writer version = **130** (recommended for II+). |
| 123 | 1 | Minimum version = **130**. |
| 124 | 2 | CRC-16 over bytes 0–123 (same algorithm as MacBinary II). |

**Placement of secondary header and comment:**  If present, the secondary
header comes immediately after the Start Block (padded to 128 bytes).  If a
Finder comment is present (offset 99 > 0), it follows the Start Block (or its
secondary header), then is padded to 128 bytes.

### 11.4  End Block (Folder End)

An End Block closes the most recently opened folder.  It is also a 128-byte
block:

| Offset | Size | Value / Rule |
|-------:|-----:|-------------|
| 0 | 1 | **1** (folder delimiter). |
| 65 | 4 | File type = **`'fold'`** (0x666F6C64). |
| 69 | 4 | Creator = **`0xFFFFFFFE`** — distinguishes End Blocks from Start Blocks. |
| 116 | 4 | Unpacked total length (may be 0). |
| 120 | 2 | Secondary header length (may be non-zero; same semantics as Start Block). |
| 122 | 1 | Writer version = **130**. |
| 123 | 1 | Minimum version = **130**. |
| 124 | 2 | CRC-16 over bytes 0–123. |

All other bytes in the End Block may be zero.  **Decoders must not rely on
the content of undefined fields in End Blocks.**

### 11.5  Distinguishing Block Types

All block type recognition is based on the tuple (version, type, creator):

| version (offset 0) | type (offset 65) | creator (offset 69) | Meaning |
|:---:|:---:|:---:|---------|
| 0 | (any) | (any) | MacBinary II file record |
| 1 | `'fold'` | `0xFFFFFFFF` | II+ Start Block (folder begin) |
| 1 | `'fold'` | `0xFFFFFFFE` | II+ End Block (folder end) |

### 11.6  Files Inside Folders

Internal files within a MacBinary II+ stream are **standard MacBinary II file
records**: offset 0 (version) = 0, writer version 122 = 129, minimum version
123 = 129.  They are encoded and decoded exactly as described in §4 through
§10.

---

## 12  Differences: MacBinary II vs. MacBinary II+

| Aspect | MacBinary II | MacBinary II+ |
|--------|-------------|---------------|
| Scope | Encodes **one file** (header + data fork + resource fork). | Encodes a **directory tree** with Start/End folder delimiters; files inside are ordinary MacBinary II records. |
| Top-level marker | Byte 0 = **0**, CRC at 124–125. | Stream begins with a **Start Block** (byte 0 = 1, type `'fold'`, creator `0xFFFFFFFF`). |
| CRC | CRC-16/XMODEM over bytes 0–123, stored at 124–125. | Same CRC on Start/End blocks and on all internal file headers. |
| Secondary header | Field exists (offset 120) but **must be 0** for files. | Start/End blocks **may** have non-zero secondary headers. |
| Comments | Finder comment after resource fork if offset 99 > 0 (rare). | Start Blocks may have their own Finder comment; internal file records behave like MacBinary II. |
| Version bytes | 122 = 129, 123 = 129. | 122 = 130, 123 = 130 (for Start/End blocks); 129/129 for internal file records. |
| Adoption | Widely used, the de facto standard. | Proposed in 1993, implemented by few tools, **little-used**. |

---

## 13  Encoding (MacBinary II)

This section describes the steps to create a MacBinary II file from a classic
Macintosh file.

### 13.1  Steps

1. **Gather metadata** from the source file: filename (≤ 63 characters),
   file type, creator code, Finder flags, window position, creation and
   modification dates, data fork length, resource fork length.

2. **Assemble the 128-byte header** according to §4:
   * Write all multi-byte integers in big-endian order.
   * Set all undefined bytes to `0x00`.
   * Set offset 0 = **0**, offset 74 = **0**, offset 82 = **0**.
   * Set offset 122 (writer version) = **129**.
   * Set offset 123 (minimum version) = **129**.
   * Set offset 120 (secondary header length) = **0**.
   * If not sending a Finder comment, set offset 99 = **0**.

3. **Compute CRC-16/XMODEM** over bytes 0–123 and store the result at
   offsets 124–125 in big-endian order (§5).

4. **Write the data fork**: emit exactly *D* bytes, then pad with zeros to
   the next 128-byte boundary.

5. **Write the resource fork**: emit exactly *R* bytes, then pad with zeros
   to the next 128-byte boundary.

6. **(Optional) Write the Finder comment**: if offset 99 > 0, write that
   many bytes, then pad to the next 128-byte boundary.  Most encoders omit
   this entirely.

### 13.2  Encoding a MacBinary II+ Directory Tree

1. **Start Block**: write a 128-byte Start Block (§11.3) with the root folder's
   name, type = `'fold'`, creator = `0xFFFFFFFF`, dates, and CRC.  Write any
   secondary header and/or comment, each padded to 128 bytes.

2. **For each child** of the folder:
   * If the child is a **file**: write a standard MacBinary II file record
     (§13.1).
   * If the child is a **subfolder**: recursively write a Start Block, its
     children, and an End Block.

3. **End Block**: write a 128-byte End Block (§11.4) with type = `'fold'`,
   creator = `0xFFFFFFFE`, and CRC.

---

## 14  Decoding (MacBinary II)

This section describes the steps to decode a MacBinary II file (or MacBinary
II+ stream) back into its constituent parts.

### 14.1  MacBinary II File Record

1. **Read 128 bytes** into a header buffer.

2. **Validate** the header:
   * Bytes 0 and 74 must both be 0 (§6.1).
   * Compute CRC-16/XMODEM over bytes 0–123 and compare with offsets 124–125
     (§6.2).  If the CRC does not match but byte 82 is 0, the file may be
     MacBinary I — proceed cautiously.
   * Filename length (offset 1) must be 1–63.

3. **Handle the secondary header**: if offset 120 is non-zero, read that many
   bytes and skip padding to the next 128-byte boundary (§9).

4. **Read the data fork**: read *D* bytes (from offset 83), then skip padding
   to the next 128-byte boundary.

5. **Read the resource fork**: read *R* bytes (from offset 87), then skip
   padding to the next 128-byte boundary.

6. **Read the Finder comment** (optional): if offset 99 > 0, read that many
   bytes (and skip padding).  Otherwise, stop.

7. **Reconstruct the file** on a system supporting HFS semantics.  If the
   target system does not support dual-fork files, store forks separately or
   in a platform-specific container.

8. **Sanitize Finder flags** and zero out positional metadata per §8.

### 14.2  MacBinary II+ Stream

1. **Read 128 bytes** into a header buffer.

2. **Examine byte 0**:
   * If byte 0 = **0**: this is a MacBinary II file record.  Decode per §14.1.
   * If byte 0 = **1**: check that offset 65 = `'fold'`.

3. **If Start Block** (creator at offset 69 = `0xFFFFFFFF`):
   * Skip any secondary header (offset 120) and Finder comment (offset 99),
     each with their 128-byte alignment padding.
   * Enter the folder: decode children in a loop, reading 128-byte headers
     and dispatching based on byte 0 and creator.
   * Stop when the matching End Block is encountered.

4. **If End Block** (creator at offset 69 = `0xFFFFFFFE`):
   * Return to the parent folder (or end of stream if this closes the root).

5. If byte 0 is neither 0 nor 1 (with type `'fold'`), reject as not
   MacBinary.

---

## 15  Error Conditions

A conforming decoder should treat the following as errors:

* Byte 0 is not 0 (for file records) or 1 (for II+ blocks).
* Byte 74 is not 0.
* CRC-16 mismatch (unless falling back to MacBinary I with byte 82 = 0).
* Filename length is 0 or greater than 63.
* Fork length exceeds the implementation's maximum (recommended: at least
  `0x7FFFFFFF`; historical strict bound: `0x007FFFFF`).
* Unexpected end of stream while reading header, forks, or padding.
* In MacBinary II+: unbalanced Start/End blocks.
* In MacBinary II+: byte 0 = 1 but type ≠ `'fold'`.

---

## 16  Implementation Guidance

This section provides practical advice for implementers.  It is not part of
the format specification per se, but captures useful insights.

### 16.1  Identifying MacBinary Files

MacBinary has no strong magic number.  Byte 0 is zero, and many other header
fields are commonly zero.  Do not rely on a single check; combine the
validation steps in §6.

Common file extensions for MacBinary are `.bin` and `.macbin`.  The media type
is sometimes reported as `application/macbinary` or `application/x-macbinary`.
Different PRONOM signatures exist for MacBinary I, II, and III.  However,
programmatic identification should follow the validation rules in §6 rather
than relying solely on magic bytes.

### 16.2  StuffIt-in-Resource-Fork Heuristic

A number of MacBinary files encountered in the wild (particularly
self-extracting StuffIt archives with extension `.sea.bin`) have a StuffIt
archive embedded in the **resource fork** while the data fork contains a small
executable stub.  A decoder that is part of an unpacking pipeline may want to
detect whether the data fork begins with a known StuffIt signature:

* Classic StuffIt: first 4 bytes are one of `"SIT!"`, `"ST46"`, `"ST50"`,
  `"ST60"`, `"ST65"`, `"STin"`, `"STi2"`, `"STi3"`, `"STi4"`, AND bytes
  10–13 are `"rLau"`.
* SIT5: bytes 0–15 match `"StuffIt (c)1997-"` and bytes 20–77 match
  `" Aladdin Systems, Inc., http://www.aladdinsys.com/StuffIt/"`.

If the data fork does **not** match any of these signatures and a resource fork
is present, the decoder may prefer to stream the resource fork to downstream
processing.

### 16.3  Memory and I/O Considerations

MacBinary decoding is simple and requires minimal state: the 128-byte header,
the current fork's remaining byte count, and knowledge of whether padding
needs to be skipped.  No temporary buffers beyond a small skip buffer are
needed.

### 16.4  Streaming / Iteration API

A practical decoder API might expose an `open(FIRST)` / `open(NEXT)` iterator
that presents the data fork as the first item and the resource fork as the
second.  This maps naturally to the sequential layout of the format.  After
each fork is read (or partially read), the decoder must skip any remaining
fork bytes plus alignment padding before presenting the next fork.

---

## Appendix A: Complete Decoding Walkthrough

This appendix traces the full decoding of a MacBinary II file step by step.

1. **Read header** (128 bytes).  Verify byte 0 = 0, byte 74 = 0.  Verify
   CRC-16.  Extract filename (`hdr[1]` bytes starting at `hdr[2]`), type
   (offset 65), creator (offset 69), Finder flags (offsets 73 + 101), data
   fork length *D* (offset 83), resource fork length *R* (offset 87), dates
   (offsets 91, 95).

2. **Check secondary header** (offset 120).  If non-zero, skip that many
   bytes plus padding.  (Almost always zero.)

3. **Read data fork**: consume *D* bytes.  Skip
   `(128 − (D mod 128)) mod 128` padding bytes.

4. **Read resource fork**: consume *R* bytes.  Skip
   `(128 − (R mod 128)) mod 128` padding bytes.

5. **Read Finder comment** (offset 99).  If non-zero, consume that many
   bytes plus padding.  (Almost always zero.)

6. **Sanitize metadata**: clear Finder flag bits 0, 1, 8, 9, 10.  Zero out
   the positional fields (offsets 75–80).

7. **Reconstruct the file** with both forks and the cleaned metadata.

---

## Appendix B: Conformance Checklist

To be **100% compatible** with MacBinary II and MacBinary II+:

- [ ] All multi-byte integers are big-endian; undefined header bytes are 0x00.
- [ ] Header is exactly 128 bytes; CRC-16/XMODEM over bytes 0–123 placed at
      offsets 124–125.
- [ ] Data fork and resource fork written in that order, each padded to a
      128-byte boundary.
- [ ] Secondary header, if present, padded to 128-byte boundary between main
      header and data fork.
- [ ] Finder comment only if header offset 99 > 0; padded to 128-byte
      boundary after resource fork.  Most tools omit it.
- [ ] On decode: clear Finder flag bits 0, 1, 8, 9, 10; zero out positional
      fields (offsets 75–80).
- [ ] Filename length validated as 1–63.
- [ ] Fork lengths read as big-endian 32-bit; bounds-checked against
      implementation limits.
- [ ] CRC validated on decode; MacBinary I fallback if CRC fails and byte
      82 = 0.
- [ ] **II+ only:** Stream starts with Start Block (byte 0 = 1, type
      `'fold'`, creator `0xFFFFFFFF`); each folder ends with End Block
      (creator `0xFFFFFFFE`).
- [ ] **II+ only:** Start/End blocks may have non-zero secondary headers
      and/or Finder comments.
- [ ] **II+ only:** Internal files use standard MacBinary II encoding
      (byte 0 = 0, version bytes 129/129).
