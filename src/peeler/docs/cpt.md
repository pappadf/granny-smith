# Compact Pro (.cpt) — Format Specification

## 1  Introduction

### 1.1  What is Compact Pro?

Compact Pro is a file compression and archiving utility developed by Bill
Goodman for the Apple Macintosh platform in the early 1990s.  Originally
released under the name "Compactor," it became a significant competitor to the
dominant StuffIt archiver, often delivering better compression speeds and, in
many cases, smaller archive sizes.  Its popularity was bolstered by its
efficient design, clean user interface, and distribution as shareware — it was
a common sight on Bulletin Board Systems (BBS) of the era.

Archives carry the `.cpt` file extension.  The format preserves the classic
Macintosh dual-fork file model: every file may have both a **data fork**
(ordinary file content) and a **resource fork** (structured resources — icons,
menus, code segments, etc.), along with Finder metadata (file type, creator
code, timestamps, Finder flags).

### 1.2  Purpose and Scope

This document is a complete, self-contained *format specification*.  It
describes the on-wire byte-level format, the directory structure, the two
compression algorithms (RLE and LZH), and every detail needed to build a fully
compatible decompressor for Compact Pro archives.  It is not tied to any
particular implementation.

---

## 2  Format Overview

### 2.1  The Big Picture

A Compact Pro archive is a flat binary file with the following high-level
structure:

```
┌──────────────────────────────────────────────────────┐
│  Initial Header (8 bytes)                            │ ← Points to directory
├──────────────────────────────────────────────────────┤
│  Fork Data Regions                                   │ ← Compressed fork data
│  (resource fork first, then data fork, per file)     │   at absolute offsets
├──────────────────────────────────────────────────────┤
│  Directory                                           │ ← Second header + entries
│  (CRC, entry count, comment, recursive entry tree)   │
└──────────────────────────────────────────────────────┘
```

Key design characteristics:

* **Byte order:**  All multi-byte integers throughout the format are
  **big-endian** (most significant byte first), reflecting the Motorola 68000
  heritage of the classic Macintosh.

* **Random access:**  File entries in the directory contain **absolute** byte
  offsets to their compressed fork data.  This means the archive must be fully
  available (not streamable) for directory parsing and extraction.

* **Dual-fork preservation:**  Each file stores its resource fork and data fork
  as separately compressed regions.  Each fork is compressed independently
  using one of two methods.

* **Two compression methods:**  *RLE* (run-length encoding alone) or *LZH*
  (LZSS + Huffman followed by RLE).  Per-fork flags in the file entry
  determine which method was used.

### 2.2  Compression Pipeline

Each fork's compressed data is decoded through one of two pipelines, selected
by a per-fork flag in the file entry:

**RLE-only fork** (flag bit clear):

```
Compressed bytes  ──►  RLE decoder  ──►  Original fork data
```

**LZH fork** (flag bit set):

```
Compressed bytes  ──►  LZH decoder  ──►  RLE-encoded stream  ──►  RLE decoder  ──►  Original fork data
```

The critical insight is that when a fork is LZH-compressed, the LZH stage does
**not** produce the final plaintext — it produces an *RLE-encoded* byte stream
that must then be RLE-decoded.  The RLE decoder is always the final stage.

### 2.3  Why Two Layers?

RLE is very fast and effective at collapsing runs of identical bytes, but it
cannot exploit more complex redundancy patterns.  LZSS + Huffman (called "LZH"
throughout this document) captures wider repetitive structures — repeated
substrings at varying distances — and then Huffman-codes the result for
entropy compression.

By using RLE as a final expansion stage even after LZH, the encoder gains
flexibility: the LZSS output can contain RLE-compressed sequences, effectively
getting a "free" extra compression pass on top of the dictionary coding.  For
forks where the data is already simple enough that LZSS provides no benefit,
the encoder can skip LZH entirely and use only RLE.

---

## 3  Archive Structure

### 3.1  Initial Archive Header (8 bytes)

Every Compact Pro archive begins with an 8-byte header:

| Offset | Size | Type | Field | Description |
|:------:|:----:|:----:|:------|:------------|
| 0 | 1 | uint8 | Magic | Always `0x01`. |
| 1 | 1 | uint8 | Volume number | `0x01` for single-volume archives.  Multi-volume semantics are not fully documented. |
| 2 | 2 | uint16 BE | Cross-volume ID | Multi-volume coordination value.  Exact semantics unclear; single-volume archives ignore this. |
| 4 | 4 | uint32 BE | Directory offset | Absolute byte offset from file start to the directory section. |

**Identification:**  A valid Compact Pro archive has byte 0 = `0x01` and
byte 1 = `0x01`.  As a practical sanity check, the directory offset should be
rejected if it exceeds 256 MiB (`0x10000000`), since no legitimate
single-volume archive approaches this size.  The directory offset must be at
least 8 (it cannot overlap the initial header).

### 3.2  Directory Section

The directory begins at the absolute offset given by the initial header.  It
consists of a **second header** followed by a flat, depth-first serialization
of all file and directory entries.

#### 3.2.1  Second Header

| Offset | Size | Type | Field | Description |
|:------:|:----:|:----:|:------|:------------|
| 0 | 4 | uint32 BE | Directory CRC-32 | Checksum over the directory metadata (see §4.1). |
| 4 | 2 | uint16 BE | Total entry count | Number of top-level entries (files and directories) that follow. |
| 6 | 1 | uint8 | Comment length *N* | Length of the archive comment in bytes. |
| 7 | *N* | bytes | Comment | Archive comment (may be empty if *N* = 0). |

Immediately after the comment, the entry records begin.

#### 3.2.2  Directory Entry — Directory

The first byte of every entry is a combined name-length / type-flag byte:

* **Bit 7 set** → directory entry.
* **Bit 7 clear** → file entry.
* **Bits 6–0** → name length *N* (0 … 127).

A directory entry has this structure:

| Offset | Size | Type | Field | Description |
|:------:|:----:|:----:|:------|:------------|
| 0 | 1 | uint8 | Name length + type flag | Bit 7 = 1 (directory).  Lower 7 bits = name length *N*. |
| 1 | *N* | bytes | Directory name | Name of this directory. |
| *N*+1 | 2 | uint16 BE | Subtree entry count *C* | Total number of entries (files and subdirectories) contained within this directory's subtree. |

#### 3.2.3  Directory Entry — File

A file entry:

| Offset | Size | Type | Field | Description |
|:------:|:----:|:----:|:------|:------------|
| 0 | 1 | uint8 | Name length + type flag | Bit 7 = 0 (file).  Lower 7 bits = name length *N*. |
| 1 | *N* | bytes | File name | Name of this file. |
| *N*+1 | 1 | uint8 | Volume number | Volume on which this file's data resides. |
| *N*+2 | 4 | uint32 BE | File data offset | Absolute byte offset from the start of the archive to this file's compressed fork data. |
| *N*+6 | 4 | uint32 BE | File type | Mac OS four-character type code (OSType). |
| *N*+10 | 4 | uint32 BE | File creator | Mac OS four-character creator code (OSType). |
| *N*+14 | 4 | uint32 BE | Creation date | Seconds since 1904-01-01 00:00:00 (Mac epoch). |
| *N*+18 | 4 | uint32 BE | Modification date | Seconds since 1904-01-01 00:00:00 (Mac epoch). |
| *N*+22 | 2 | uint16 BE | Finder flags | Classic Mac Finder flags. |
| *N*+24 | 4 | uint32 BE | File data CRC-32 | CRC-32 over the uncompressed fork data (see §4.2). |
| *N*+28 | 2 | uint16 BE | File flags | Compression and encryption indicators (see below). |
| *N*+30 | 4 | uint32 BE | Resource fork uncompressed length | Original size of the resource fork in bytes. |
| *N*+34 | 4 | uint32 BE | Data fork uncompressed length | Original size of the data fork in bytes. |
| *N*+38 | 4 | uint32 BE | Resource fork compressed length | Size of the compressed resource fork in the archive. |
| *N*+42 | 4 | uint32 BE | Data fork compressed length | Size of the compressed data fork in the archive. |

The metadata portion after the name is always exactly **45 bytes** (1 + 4 + 4 +
4 + 4 + 4 + 2 + 4 + 2 + 4 + 4 + 4 + 4).

##### File Flags (16 bits)

| Bit | Meaning |
|:---:|:--------|
| 0 | Encrypted (algorithm unknown; a conforming decompressor should refuse these files). |
| 1 | Resource fork uses LZH compression (if clear, resource fork uses RLE only). |
| 2 | Data fork uses LZH compression (if clear, data fork uses RLE only). |
| 3–15 | Reserved / unused. |

### 3.3  Directory Hierarchy — Recursive Traversal

The entry tree is serialized **depth-first**.  Reconstruction works as
follows:

1. Read the total entry count from the second header.  This is the number of
   top-level entries to consume.

2. For each entry:
   - Read the name-length / type-flag byte.
   - If it is a **directory**: read the name and the 2-byte subtree count *C*.
     Then recursively parse *C* entries as children of this directory.  The
     directory entry itself plus its *C* children consume *C* + 1 entries from
     the parent's remaining count.
   - If it is a **file**: read the name and the 45-byte metadata block.  This
     consumes 1 entry from the parent's remaining count.

3. Full path names are reconstructed by concatenating parent directory names
   and file names with `/` separators (or the host-appropriate separator).

### 3.4  Fork Data Layout

For each file, the compressed fork data is stored at the **file data offset**
specified in the file entry.  The forks appear in a fixed order:

1. **Resource fork** compressed data — starts at the file data offset.
2. **Data fork** compressed data — starts at file data offset + resource fork
   compressed length.

This order is the same regardless of which fork is larger or which compression
method is used.  A fork with zero uncompressed length has zero compressed
length and occupies no space in the archive.

---

## 4  Checksums (CRC-32)

Compact Pro uses the standard IEEE CRC-32 (reflected polynomial) in two
places, with **different finalization**.

### 4.1  Common CRC-32 Parameters

| Parameter | Value |
|-----------|-------|
| Polynomial | `0xEDB88320` (reflected form of `0x04C11DB7`) |
| Initial accumulator | `0xFFFFFFFF` |
| Input reflection | Yes (table-driven reflected arithmetic) |
| Storage | Big-endian 32-bit |

This is the same CRC-32 used by Ethernet, zlib, and countless other formats —
the only variation is whether a final XOR is applied.

### 4.2  Directory CRC-32

**Finalization:** No final XOR.  The raw accumulator (initialized to
`0xFFFFFFFF` and fed all the bytes below) is compared directly to the stored
value.

**Coverage** (bytes are fed to the CRC in exactly this order):

1. 2 bytes — total entry count (uint16 BE).
2. 1 byte — comment length.
3. *N* bytes — comment (if any).
4. For each of the total entries (in serialization order):
   - 1 byte — name-length / type-flag byte.
   - *N* bytes — name (*N* = lower 7 bits of the flag byte).
   - Metadata:
     - **Directory:** 2 bytes (subtree entry count).
     - **File:** 45 bytes (volume through data fork compressed length).

### 4.3  Per-File Data CRC-32

**Finalization:** XOR the final accumulator with `0xFFFFFFFF`.  Equivalently,
the stored value equals the bitwise NOT of the raw accumulator.

**Coverage:** The uncompressed resource fork bytes followed immediately by the
uncompressed data fork bytes.  If a fork has zero length, it is simply omitted
(contributes zero bytes to the CRC input).

**Important:** The two CRC checks use different finalization.  The directory
CRC has no final XOR; the per-file CRC does.  Mixing these up is a common
implementation bug.

---

## 5  RLE — Run-Length Encoding

Compact Pro uses a stateful RLE scheme with a single escape byte (`0x81`).  It
is applied as the final decompression stage for every fork, and also as the
sole decompression stage for forks that are not LZH-compressed.

### 5.1  Overview and Design Rationale

Run-length encoding is the simplest form of lossless compression: consecutive
identical bytes are replaced with a compact representation.  Compact Pro's
variant uses an escape-byte approach rather than a flag-bit approach, which
means the encoded stream remains a plain byte sequence (no bit-level packing).

The escape byte `0x81` was chosen because it is uncommon in typical Macintosh
data.  When `0x81` does appear literally in the data, it must be escaped — the
scheme provides several escape sequences to handle this and other edge cases.

### 5.2  Escape Sequences

All control sequences begin with the escape byte `0x81`:

| Sequence | Meaning |
|----------|---------|
| `0x81 0x82 N` (N > 0) | **RLE run:** repeat the previously emitted byte.  Emit it once immediately, then emit N − 2 additional copies. |
| `0x81 0x82 0x00` | **Literal escape:** emit `0x81` followed by `0x82` (not a run). |
| `0x81 0x81` | **Double escape:** emit one literal `0x81`, then inject a phantom `0x81` that re-enters the decoder's classification logic (see §5.4). |
| `0x81 X` (X ≠ `0x81`, X ≠ `0x82`) | **Simple escape:** emit `0x81` followed by X on the next cycle. |

Any byte that is *not* `0x81` is emitted as a literal and becomes the new
"previously emitted byte" for subsequent RLE runs.

### 5.3  Decoder State

The decoder maintains exactly three state variables:

| Variable | Type | Initial Value | Purpose |
|----------|------|:-------------:|---------|
| `saved_byte` | byte | 0 | The last byte emitted to the output stream. Used as the repeated value in RLE runs. |
| `repeat_count` | integer | 0 | Number of additional copies of `saved_byte` still pending. |
| `half_escaped` | boolean | false | When true, a phantom `0x81` byte is injected at the top of the next decode cycle. |

### 5.4  The Half-Escape Mechanism

The double escape (`0x81 0x81`) is the subtlest part of the RLE scheme and
deserves careful explanation, because getting it wrong causes silent data
corruption on any file containing byte `0x81` in the RLE stream.

When the decoder encounters `0x81 0x81`:

1. It emits one literal `0x81` and sets `saved_byte = 0x81`.
2. It sets `half_escaped = true`.

On the *next* decode cycle, before reading from the input stream, the decoder
checks `half_escaped`.  If true, it:

1. Sets `byte = 0x81` (a "phantom" byte, **not** read from the stream).
2. Clears `half_escaped`.
3. **Falls through to the normal byte-classification logic.**

This is the critical point: the phantom `0x81` enters the `byte == 0x81`
branch and triggers escape processing.  It reads the next *real* byte from the
input stream as the escape operand.  The phantom byte is **not** simply
emitted as a literal — it starts a new escape sequence.

#### Worked Example

Consider the RLE input bytes: `0x81, 0x81, 0x82, 0x80`

| Step | Action | Output | State |
|------|--------|--------|-------|
| 1 | Read `0x81` → escape prefix | — | — |
| 2 | Read `0x81` → double escape | `0x81` | saved=0x81, half_escaped=true |
| 3 | Phantom `0x81` injected, clear flag. Falls through to escape detection. | — | — |
| 4 | Phantom `0x81` → read `0x82` from stream → RLE trigger. Read count `0x80` (128). | `0x81` | saved=0x81, repeat_count=126 |
| 5–130 | Drain 126 more copies of `0x81` | 126 × `0x81` | — |

Total output: 128 bytes of `0x81` (1 from step 2 + 1 from step 4 + 126 from
step 5).

Without the fall-through behavior, a naive decoder would treat the `0x82` as a
literal byte, producing completely wrong output from that point onward.

#### Chained Double Escapes

Multiple double escapes can chain.  Consider: `0x81, 0x81, 0x81, 0x82, 0x80`

| Step | Action | Output |
|------|--------|--------|
| 1 | Read `0x81` → escape prefix | — |
| 2 | Read `0x81` → double escape | `0x81` (half_escaped=true) |
| 3 | Phantom `0x81` → escape prefix | — |
| 4 | Read `0x81` → double escape | `0x81` (half_escaped=true) |
| 5 | Phantom `0x81` → escape prefix | — |
| 6 | Read `0x82` → RLE: count=`0x80` (128) | `0x81` + 126 more |

Total: 129 bytes of `0x81` (1 + 1 + 1 + 126).

### 5.5  The N − 2 Rule

For RLE sequences (`0x81 0x82 N` where N > 0), the count byte N does **not**
directly specify the number of repeated bytes.  Instead:

* The decoder emits `saved_byte` once immediately.
* It then schedules N − 2 additional copies (i.e., `repeat_count = N − 2`,
  clamped to 0 if N < 2).

The total number of repeated bytes produced by the RLE sequence is therefore
N − 1 (one immediate + N − 2 deferred).  But this comes *after* the original
byte that was already emitted before the `0x81 0x82 N` sequence, so the
grand total run length (including the byte before the escape) is N.

**Why N − 2?**  The count byte encodes the total run length.  One copy was
already emitted when the byte was first encountered (before the RLE sequence).
One more is emitted immediately when the RLE sequence is processed.  That
leaves N − 2 for the pending queue.

#### Examples

| Input | Count N | Immediate | Pending (N−2) | Total Run |
|-------|:-------:|:---------:|:-------------:|:---------:|
| `A, 0x81, 0x82, 0x03` | 3 | 1 × A | 1 × A | 3 × A (including the A before the escape) |
| `A, 0x81, 0x82, 0x05` | 5 | 1 × A | 3 × A | 5 × A |
| `A, 0x81, 0x82, 0x02` | 2 | 1 × A | 0 | 2 × A |

### 5.6  Special Cases

**Zero count** (`0x81 0x82 0x00`): This is *not* an RLE run.  It encodes the
literal bytes `0x81, 0x82`.  The decoder emits `0x81` immediately, sets
`saved_byte = 0x82`, and schedules 1 pending copy of `0x82`.

**Count = 1** (`0x81 0x82 0x01`): This would produce −1 pending copies
(1 − 2 = −1), implying a negative repeat — which is nonsensical.  This
sequence should never appear in valid data.  A robust decoder may treat it as
a no-op (emit `saved_byte` once, with 0 additional copies).  Encoders must
never generate this sequence.

**End of input during escape:** If the input stream ends in the middle of an
escape sequence (after `0x81` but before the operand, or after `0x81 0x82` but
before the count byte), this is an error condition.

### 5.7  Complete Decoder Algorithm

```
function rle_decode(input, expected_length):
    saved_byte    = 0
    repeat_count  = 0
    half_escaped  = false
    out_pos       = 0
    in_pos        = 0

    while out_pos < expected_length:

        // Step 1: Drain pending run copies
        if repeat_count > 0:
            output(saved_byte)
            repeat_count -= 1
            continue

        // Step 2: Inject phantom byte from half-escape
        if half_escaped:
            byte = 0x81
            half_escaped = false
            // FALL THROUGH to step 4 — do NOT emit or continue
        else:
            // Step 3: Read next input byte
            byte = input[in_pos++]

        // Step 4: Classify the byte
        if byte != 0x81:
            // Normal literal
            output(byte)
            saved_byte = byte
        else:
            // Escape prefix — read operand
            next = input[in_pos++]

            if next == 0x82:
                // RLE or literal escape
                count = input[in_pos++]
                if count == 0:
                    // Literal 0x81, 0x82
                    output(0x81)
                    saved_byte = 0x82
                    repeat_count = 1
                else:
                    // RLE run
                    output(saved_byte)
                    repeat_count = max(0, count - 2)

            else if next == 0x81:
                // Double escape
                output(0x81)
                saved_byte = 0x81
                half_escaped = true

            else:
                // Simple escape: literal 0x81, then next byte
                output(0x81)
                saved_byte = next
                repeat_count = 1
```

The key structural requirement is that the `half_escaped` path sets
`byte = 0x81` and then **falls through** to the classification logic at step 4
(the `byte == 0x81` branch), rather than emitting and continuing.

---

## 6  LZH — LZSS + Huffman Compression

When a fork's LZH flag is set, the compressed data is first decoded through
the LZH stage before being passed to the RLE decoder (§5).  LZH uses LZSS
dictionary coding with Huffman-coded symbols, organized into blocks with
locally-rebuilt code tables.

### 6.1  Overview and Design Rationale

LZSS (Lempel–Ziv–Storer–Szymanski) is a sliding-window dictionary coder that
replaces repeated substrings with back-references of the form *(offset,
length)*.  Rather than storing these tokens as fixed-size fields, Compact Pro
Huffman-codes three separate symbol alphabets — literals, match lengths, and
match offsets — to exploit the non-uniform distribution of each.

The data is processed in **blocks**.  At the start of each block, three new
Huffman code tables (one per alphabet) are built from the compressed stream.
This allows the code tables to adapt to local data characteristics, improving
compression compared to a single global table.

### 6.2  Bitstream Conventions

The LZH decoder reads bits from the compressed byte stream in **MSB-first**
(most significant bit first) order.  Within each byte, bit 7 is extracted
first, bit 0 last.

An accumulator-based bit reader is recommended:

* Maintain a 32-bit left-aligned shift register (`acc`) and a bit count
  (`fill`).
* **Refill (demand-driven):** While `fill < needed` and input remains, shift
  the next source byte into position `24 − fill` of the accumulator and add 8
  to `fill`.
* **Read N bits:** Extract the top N bits via `acc >> (32 − N)`, then shift
  `acc` left by N and subtract N from `fill`.
* **Byte alignment:** Discard `fill % 8` bits to align to the next byte
  boundary.
* **Byte tracking:** Maintain a `bytes_read` counter incremented each time a
  byte enters the accumulator.  This is needed for the end-of-block flush
  (§6.7).

### 6.3  Sliding Window

| Parameter | Value |
|-----------|-------|
| Window size | 8192 bytes (8 KiB) |
| Window mask | `0x1FFF` |
| Initial contents | All zeros |
| Offset interpretation | 1-based (offset 1 = the byte immediately before the current write position) |

The window is circular: all positions are computed modulo 8192.

### 6.4  Huffman Code Tables

Each LZH block begins with three Huffman code tables, serialized in this
order:

1. **Literal table** — 256 symbols (byte values 0 … 255).
2. **Length table** — 64 symbols (match lengths 0 … 63).
3. **Offset table** — 128 symbols (upper 7 bits of 13-bit match offsets).

#### 6.4.1  Table Serialization Format

Each table is encoded as:

1. **1 byte:** `numbytes` — the number of packed bytes that follow.
2. **`numbytes` bytes:** each byte contains two 4-bit code lengths packed as
   high nibble (even-indexed symbol) and low nibble (odd-indexed symbol), in
   ascending symbol order.

This means `numbytes` bytes encode code lengths for up to `numbytes × 2`
symbols.  If `numbytes × 2` is less than the table size, the remaining symbols
have an implicit code length of 0 (not present in the code).  If
`numbytes × 2` exceeds the table size, the stream is invalid.

**Example:** For `numbytes = 3` and bytes `[0x21, 0x30, 0x04]`, the code
lengths for symbols 0–5 are: 2, 1, 3, 0, 0, 4.  All remaining symbols have
length 0.

#### 6.4.2  Canonical Huffman Code Construction

Compact Pro uses **canonical Huffman codes** — all codes of a given length are
consecutive integers, and shorter codes are numerically smaller.  The maximum
code length is **15 bits**.

Construction algorithm:

1. Process symbols in ascending order of code length, then ascending symbol
   value within the same length.  Skip symbols with code length 0.
2. Maintain a running integer `code` starting at 0.
3. For code length *L*:
   - Assign the current `code` value to each symbol of length *L*.
   - Increment `code` after each assignment.
4. When transitioning from length *L* to length *L*+1: `code <<= 1`
   (left-shift by 1).

#### 6.4.3  Decoding with a Binary Tree

For decoding, the canonical codes are typically stored in a binary tree.
Codes are inserted MSB-first: for a code of length *L*, bit index 0 is
`(code >> (L−1)) & 1` (the most significant bit), and subsequent bits
select left (0) or right (1) children down the tree.

To decode a symbol: read one bit at a time from the compressed stream,
traverse left on 0, right on 1, until a leaf node (containing a symbol value)
is reached.

A pool-based allocator (e.g., 2048 nodes per tree) avoids per-node memory
allocation overhead.  Each of the three trees gets its own independent pool,
which is reset when building tables for a new block.

### 6.5  Block Data — Decoding Literals and Matches

After the three code tables have been built, the block data section begins.
Each "symbol" is either a literal byte or a match reference, distinguished by
a 1-bit flag:

**Read one bit:**

* **Bit = 1 → Literal byte:**
  1. Decode one symbol from the **literal** Huffman tree (this is the byte
     value, 0 … 255).
  2. Write the byte to the sliding window at the current position.
  3. Emit the byte as output.
  4. Advance the window write position.
  5. Add 2 to the block cost counter.

* **Bit = 0 → Match reference:**
  1. Decode the match length from the **length** Huffman tree.  The decoded
     symbol value **is** the match length (no extra bits are added).  A
     match length of 0 indicates a corrupt stream — terminate decompression.
  2. Decode the upper 7 bits of the match offset from the **offset** Huffman
     tree.
  3. Read the lower 6 bits of the match offset as raw bits from the
     bitstream.
  4. Combine into a 13-bit offset: `offset = (upper7 << 6) | lower6`.
     This offset is **1-based** (offset 1 points to the byte immediately
     before the current write position).
  5. Copy `length` bytes from the sliding window starting at position
     `write_pos − offset`, wrapping around the circular buffer as needed.
     Write each copied byte to the window at the current write position and
     emit it as output.
  6. Add 3 to the block cost counter.

### 6.6  Overlapping Matches

A match may have `length > offset`, meaning the source and destination
regions overlap in the sliding window.  This is intentional and commonly used
by LZSS encoders to represent byte repetitions efficiently.

**Examples:**
* `offset = 1, length = 100` — repeats the single byte before the write
  position 100 times.
* `offset = 2, length = 6` — repeats the two bytes before the write position
  three times (ABABAB).

**Correct handling requires byte-by-byte copying.**  Each byte must be:

1. Read from `window[(write_pos − offset) & mask]`.
2. Written to `window[write_pos & mask]`.
3. Emitted as output.
4. `write_pos` incremented.

Only then is the next byte of the match processed.  Pre-copying the entire
match region into a temporary buffer before updating the window will produce
incorrect output for overlapping matches, because the temporary buffer would
contain stale values at positions that should have been updated by earlier
iterations of the same copy.

### 6.7  Block Size and Termination

Blocks are measured by **symbol cost**, not by output bytes:

| Symbol Type | Cost |
|-------------|:----:|
| Literal | 2 |
| Match | 3 |

The block ends when the cumulative cost counter reaches or exceeds
**`0x1FFF0`** (131,056 in decimal).  The final symbol may cause the counter to
exceed this threshold — that is expected.

**Last block:** If the compressed source is exhausted before the block counter
reaches `0x1FFF0`, decompression ends normally.  This is the standard
termination condition for the final block in the stream.

**Important:** Exhaustion may occur at *any point* during symbol decoding —
not only at the top of the block loop.  For example, the bitstream may run
out while traversing a Huffman tree, after reading a match length symbol but
before the offset, or before the lower 6 raw offset bits.  All of these are
normal final-block termination, not errors.  A correct decoder must treat
bitstream exhaustion as a clean end-of-stream at every bit-reading call site
within the block loop.

### 6.8  End-of-Block Input Flush

At the end of each block (when the cost counter reaches `0x1FFF0`), the
decoder must re-synchronize the bitstream before reading the next block's
Huffman tables.  The flush procedure is:

1. **Byte-align:** Discard any remaining bits in the current byte (advance to
   the next byte boundary).
2. **Count data bytes:** Let *B* be the number of bytes consumed by this
   block's **data portion** — that is, bytes consumed *after* the three
   Huffman tables were read, up to the current position.
3. **Skip padding:**
   - If *B* is **odd**: skip 3 bytes.
   - If *B* is **even**: skip 2 bytes.
4. The next block's three Huffman tables begin immediately after the skipped
   bytes.

**Critical detail:** The byte count *B* measures only the *data portion* of
the block — it does *not* include the bytes consumed by the three Huffman
table headers.  To compute this correctly, record the bit reader's
`bytes_read` counter immediately after building the three tables, then
subtract that saved value from the current `bytes_read` after the block loop
ends.

---

## 7  Putting It All Together — Decompression Walkthrough

This section ties all the pieces together into a complete extraction workflow.

### 7.1  Archive Loading

Since the directory contains absolute byte offsets to fork data scattered
throughout the archive, the entire archive must be available for random access.
A practical implementation reads the whole archive into memory (using a
dynamically-growing buffer, e.g., starting at 128 KiB and doubling as needed).

### 7.2  Header Parsing

1. Read the 8-byte initial header.  Verify byte 0 = `0x01`, byte 1 = `0x01`.
   Extract the 4-byte directory offset.
2. Seek to the directory offset.  Read the second header: CRC-32, total entry
   count, comment length, and comment.
3. Optionally verify the directory CRC-32 (§4.2).

### 7.3  Directory Parsing

Parse the entry tree recursively as described in §3.3.  Build a flat list of
file entries with full path names, fork sizes, compression flags, and data
offsets.

### 7.4  File Extraction

For each file entry:

1. Determine the compression method for each fork from the file flags (§3.2.3).
2. Locate the fork data:
   - Resource fork: starts at the file data offset.
   - Data fork: starts at file data offset + resource fork compressed length.
3. For each non-empty fork, set up the decompression pipeline:
   - **RLE-only:** Feed the compressed bytes directly to the RLE decoder (§5).
   - **LZH:** Feed the compressed bytes to the LZH decoder (§6), then pipe
     the LZH output through the RLE decoder.
4. Read decompressed bytes until the fork's uncompressed length is reached.
5. After both forks are decompressed, optionally verify the per-file CRC-32
   (§4.3) over the concatenation of uncompressed resource fork + data fork.

### 7.5  Fork Iteration Order

A decompressor typically iterates forks in the order most useful to the
consumer.  The archive stores forks in resource-then-data order, but the
extraction API may present them in data-then-resource order (data fork first
is more natural for most consumers).  Forks with zero uncompressed length are
skipped.

---

## 8  Error Conditions

A conforming decompressor should treat the following as errors:

* **Header mismatch:** Byte 0 ≠ `0x01` or byte 1 ≠ `0x01`.
* **Directory offset out of range:** Offset < 8, exceeds archive size, or
  exceeds the 256 MiB sanity limit.
* **Truncated entries:** Entry records or metadata extending past the end of
  the archive.
* **Directory CRC mismatch** (if checked).
* **Per-file CRC mismatch** (if checked).
* **Encrypted files:** Flag bit 0 set (algorithm unknown; cannot decrypt).
* **LZH errors:**
  - Huffman table `numbytes × 2` exceeds the table's symbol count.
  - Code length exceeds 15 bits.
  - Huffman tree traversal reaches an invalid node.
  - Match length symbol = 0 (corrupt stream).
  - Bitstream exhaustion during mid-block decoding in a *non-final* block
    (i.e., when a previous block ended normally at the cost threshold and
    new tables were read, but the data portion runs out unexpectedly).
    Note: for the *final* block, bitstream exhaustion at any point is the
    normal termination condition (see §6.7), not an error.
* **RLE errors:**
  - Incomplete escape sequence (input ends after `0x81` or after `0x81 0x82`
    but before the count byte).
  - Sequence `0x81 0x82 0x01` (invalid; implies negative repeat count).
* **Fork data extends past archive bounds:**  File data offset + compressed
  fork lengths exceed archive size.

---

## 9  Implementation Guidance

This section provides practical advice for implementers.  It is not part of
the format specification per se, but captures lessons learned from existing
implementations.

### 9.1  Memory Model

Because Compact Pro archives use absolute offsets for fork data, the simplest
approach is to read the entire archive into a single contiguous memory buffer.
This allows the decompressor to set up a byte source for any fork by simply
pointing into the buffer at the appropriate offset and length.

For the LZH decoder, a uniform byte-supplier callback interface (e.g.,
`int (*getbyte)(void *ctx, int *out)` returning 1 on success, 0 on EOF)
cleanly separates the bit reader from the byte source.  When chaining LZH
into RLE, a thin adapter function bridges the LZH decoder's output to the
RLE decoder's input callback, enabling flexible pipeline composition without
coupling the two decoders.

### 9.2  Streaming Match Handling in LZH

Rather than pre-buffering an entire match into a temporary array (which breaks
overlapping matches — see §6.6), maintain persistent state for the current
match:

* `match_src` — absolute source position in the window for the current match.
* `match_rem` — bytes remaining in the current match.

On each call to the LZH output function, if `match_rem > 0`, emit one byte
from `window[match_src & mask]`, write it to the current window position,
increment both `match_src` and `write_pos`, and decrement `match_rem`.  This
ensures overlapping matches resolve correctly without any temporary buffer.

### 9.3  Huffman Tree Pool Allocation

Each of the three Huffman trees (literal, length, offset) benefits from a
flat-array node pool (e.g., 2048 nodes per tree) rather than per-node
`malloc`.  The pools are independent and are reset (by setting `used = 0`)
each time new block tables are built, so no deallocation is needed between
blocks.

### 9.4  Bit Reader Byte Counting

The end-of-block flush (§6.8) requires knowing how many bytes the block's data
portion consumed.  The simplest approach is to track a `bytes_read` counter in
the bit reader (incremented each time a byte enters the accumulator).  Save
this counter immediately after building the three Huffman tables; after the
block loop, the difference gives the data-portion byte count.

The `bytes_read` counter also enables computing the exact byte position in the
source stream at any point: `effective_position = bytes_read − (fill / 8)`.

### 9.5  Fork Stream Composition

A clean implementation uses a "fork stream" abstraction that:

1. Initializes either an LZH decoder or a direct memory source, depending on
   the fork's compression flag.
2. Wraps the LZH output (or direct source) with the RLE decoder.
3. Provides a single `read(dst, max)` function that the caller invokes.

The fork stream is re-initialized for each new fork (resource then data)
within each file entry.

### 9.6  CRC Validation

Directory and per-file CRC-32 validation, while recommended, is optional in
a minimal implementation.  If skipped, be aware that corrupt archives may
produce wrong output silently.

---

## 10  Validation Test Cases

Any implementation should pass these exact RLE test cases:

### 10.1  Basic RLE

```
Test 1: Simple 2-byte run
Input:  [0x41, 0x81, 0x82, 0x03]
Output: [0x41, 0x41]

Test 2: 4-byte run
Input:  [0x41, 0x81, 0x82, 0x05]
Output: [0x41, 0x41, 0x41, 0x41]

Test 3: Run of a different byte
Input:  [0x41, 0x42, 0x81, 0x82, 0x03]
Output: [0x41, 0x42, 0x42]
```

### 10.2  Escape Sequences

```
Test 4: Simple escape
Input:  [0x81, 0x41]
Output: [0x81, 0x41]

Test 5: Zero count (literal 0x81, 0x82)
Input:  [0x81, 0x82, 0x00]
Output: [0x81, 0x82]

Test 6: Double escape
Input:  [0x81, 0x81, 0x42]
Output: [0x81, 0x81, 0x42]
```

### 10.3  Complex Sequences

```
Test 7: Mixed operations
Input:  [0x41, 0x81, 0x82, 0x03, 0x43, 0x81, 0x44]
Output: [0x41, 0x41, 0x43, 0x81, 0x44]

Test 8: Run after escape
Input:  [0x81, 0x42, 0x81, 0x82, 0x04]
Output: [0x81, 0x42, 0x42, 0x42]
```

---

## Appendix A: LZH Block Decoder Pseudocode

```
function lzh_decode(source):
    init sliding window (8192 bytes, all zeros)
    write_pos = 0

    while not end_of_source:
        // Parse three Huffman code tables for this block
        lit_lens = read_code_lengths(source, 256)
        len_lens = read_code_lengths(source,  64)
        off_lens = read_code_lengths(source, 128)
        lit_tree = build_canonical_tree(lit_lens, max_length=15)
        len_tree = build_canonical_tree(len_lens, max_length=15)
        off_tree = build_canonical_tree(off_lens, max_length=15)

        record block_data_start = bytes_consumed

        block_cost = 0
        while block_cost < 0x1FFF0:
            // Any bit-read may signal end-of-source for the final block.
            // Treat exhaustion at any point here as normal termination.
            flag = read_bit()          // → end_of_source? return
            if flag == 1:
                // Literal
                sym = decode_symbol(lit_tree)   // → end_of_source? return
                window[write_pos & 0x1FFF] = sym
                emit(sym)
                write_pos += 1
                block_cost += 2
            else:
                // Match
                mlen = decode_symbol(len_tree)   // → end_of_source? return
                if mlen == 0: error("corrupt stream")
                upper7 = decode_symbol(off_tree) // → end_of_source? return
                lower6 = read_bits(6)            // → end_of_source? return
                offset = (upper7 << 6) | lower6    // 1-based

                for i in 0 .. mlen-1:
                    b = window[(write_pos - offset) & 0x1FFF]
                    window[write_pos & 0x1FFF] = b
                    emit(b)
                    write_pos += 1
                block_cost += 3

        // End-of-block flush
        align_to_byte_boundary()
        data_bytes = bytes_consumed - block_data_start
        if data_bytes is odd:
            skip(3 bytes)
        else:
            skip(2 bytes)
```

## Appendix B: Complete Implementation Checklist

A quick-reference for implementers:

- [ ] Big-endian for all multi-byte integers.
- [ ] 8-byte initial header: magic `0x01`, volume `0x01`, directory offset.
- [ ] Second header at directory offset: CRC, total entries, comment.
- [ ] Entry parsing: bit 7 of name-length byte distinguishes directory (1) vs
      file (0).
- [ ] Directory recursion: subtree count *C*, consume *C* + 1 entries.
- [ ] File metadata: 45 bytes after the name.
- [ ] File flags: bit 0 = encrypted, bit 1 = resource LZH, bit 2 = data LZH.
- [ ] Fork order in archive: resource fork first, then data fork.
- [ ] Data fork offset = file data offset + resource fork compressed length.
- [ ] Directory CRC: init `0xFFFFFFFF`, **no** final XOR.
- [ ] Per-file CRC: init `0xFFFFFFFF`, **final XOR** `0xFFFFFFFF`, over
      uncompressed resource ‖ data.
- [ ] LZH pipeline: LZH output is RLE-encoded; must RLE-decode after LZH.
- [ ] LZH bit order: MSB-first.
- [ ] LZH Huffman tables: nibble-packed lengths, canonical codes, max 15 bits.
- [ ] LZH match offset: 13 bits = (upper 7 via Huffman) ‖ (lower 6 raw),
      1-based.
- [ ] LZH overlapping matches: byte-by-byte copy (not pre-buffered).
- [ ] LZH block cost: 2 per literal, 3 per match, threshold `0x1FFF0`.
- [ ] LZH end-of-block flush: byte-align + skip 2 or 3 bytes (even/odd of
      data-portion byte count).
- [ ] RLE escape byte: `0x81`.
- [ ] RLE run count: `0x81 0x82 N` → emit `saved_byte`, then N − 2 more
      copies.
- [ ] RLE zero count: `0x81 0x82 0x00` → literal `0x81`, `0x82`.
- [ ] RLE double escape: `0x81 0x81` → emit `0x81`, phantom re-enters escape
      detection.
- [ ] RLE simple escape: `0x81 X` → emit `0x81`, then X.
- [ ] `0x81 0x82 0x01` is invalid; never produce it.
