# BinHex 4.0 (.hqx) — Format Specification

## 1  Introduction

### 1.1  What is BinHex 4.0?

BinHex 4.0 is a binary-to-text encoding and archival format created for the
classic Macintosh.  Unlike simpler encoding schemes such as Base64 or
Uuencode — which operate on a single, unstructured byte stream — BinHex 4.0
is a *container format*.  It bundles a Macintosh file's two forks (data and
resource), along with essential filesystem metadata, into a single
ASCII-armored text file with the `.hqx` extension.

The format applies three processing layers in sequence:

1. **Structural assembly** — packs the filename, type/creator codes, Finder
   flags, data fork, and resource fork into a single binary stream, protected
   by three independent CRC-16 checksums.
2. **Run-length compression** — a lightweight RLE scheme (using 0x90 as a
   marker byte) that reduces repeated-byte sequences.
3. **6-bit ASCII encoding** — converts the compressed binary stream into
   printable 7-bit ASCII characters, making it safe for transmission over
   networks that could not reliably handle 8-bit data.

Decoding reverses the pipeline: strip the text envelope, decode the ASCII
characters back to bytes, decompress the RLE, then parse the binary stream
to extract the original file.

### 1.2  Historical Context

The development of BinHex 4.0 was driven by two concurrent technical
challenges of the mid-1980s.

**The 7-bit network problem:**  Early electronic mail systems and online
services (CompuServe, BITNET, early Internet gateways) transmitted only the
128 characters of 7-bit US-ASCII.  High bits could be stripped, control
characters misinterpreted, and line endings altered.  Transmitting raw binary
files through these channels corrupted data.  "ASCII armoring" — encoding
8-bit data as safe, printable characters — was the standard workaround.

**The Macintosh dual-fork problem:**  Unlike DOS or Unix, the classic Mac OS
filesystem (MFS, later HFS) stored every file as two separate byte streams:

* **Data fork** — the file's primary content (text, pixel data, etc.).
* **Resource fork** — structured data such as icons, menus, dialog layouts,
  sounds, and executable code segments.

Both forks had to be preserved for Macintosh applications to function
correctly.  Additionally, the filesystem recorded a 4-byte **File Type**, a
4-byte **Creator** code, and 16-bit **Finder flags** as part of the directory
entry — metadata with no equivalent on other platforms.

**Evolution:**  The BinHex lineage began on the TRS-80 as a simple hex (4-bit)
encoding.  When ported to the Macintosh, it initially handled only the data
fork.  Compact BinHex (version 2.0, `.hcx`) improved efficiency with 6-bit
encoding.  BinHex 4.0, released in 1985 by Yves Lempereur (skipping version
3.0 to avoid confusion with an unrelated program), introduced the full
dual-fork container, triple CRC protection, and integrated RLE compression.
It became the de facto standard for Mac file exchange until the late 1990s,
when MIME/Base64 combined with MacBinary superseded it.

### 1.3  Purpose and Scope

This document is a complete, self-contained *format specification*.  It
describes the text envelope, the three processing layers, and every algorithm
needed to build a fully compatible encoder and decoder for BinHex 4.0 files.
It is written as a specification, not as documentation of any particular
implementation.

---

## 2  Processing Pipeline Overview

### 2.1  The Big Picture

BinHex 4.0 applies three layers during **encoding**:

```
Macintosh file (data fork + resource fork + metadata)
  │
  ▼
┌──────────────────────────────────────────────────┐
│  Layer 1: Structural Assembly + CRC              │ ← Pack forks + metadata
└──────────────────────────────────────────────────┘   into a binary stream
  │                                                    with 3 CRC checksums
  ▼
┌──────────────────────────────────────────────────┐
│  Layer 2: Run-Length Encoding (RLE90)             │ ← Compress repeated
└──────────────────────────────────────────────────┘   byte sequences
  │
  ▼
┌──────────────────────────────────────────────────┐
│  Layer 3: 6-Bit ASCII Encoding                   │ ← Convert 8-bit bytes
└──────────────────────────────────────────────────┘   to safe ASCII text
  │
  ▼
┌──────────────────────────────────────────────────┐
│  Text Envelope: preamble + colons + line breaks  │ ← Wrap for transport
└──────────────────────────────────────────────────┘
  │
  ▼
.hqx text file
```

**Decoding reverses the pipeline**, peeling off layers from the outside in.
The rest of this document describes each layer in **decoding order** (outermost
first), which is the order an implementer of a decoder encounters them.

### 2.2  Why Each Layer Exists

**Text envelope (§3):**  Provides framing — the preamble string lets software
identify .hqx files among arbitrary text (email bodies, Usenet posts), and the
colon delimiters mark the start and end of the payload.

**6-bit ASCII encoding (§4):**  Converts the binary payload into printable
characters that survive 7-bit network transport.  This is the same concept as
Base64, but with a different character set and no explicit padding character.
The encoding expands data by 33% (3 binary bytes → 4 ASCII characters).

**Run-length encoding (§5):**  A simple compression pass that replaces runs of
3 or more identical bytes with a compact 3-byte representation.  The
effectiveness is modest — for arbitrary data the RLE may even expand the
stream slightly — but it helps significantly for files with large runs of
zero-padding or simple bitmap data, which were common in 1980s Macintosh files.

**Structural assembly + CRC (§6, §7):**  Packs all components of the
Macintosh file — filename, type, creator, flags, data fork, resource fork —
into a single sequential binary stream with three independent CRC-16 checksums
that protect against transmission errors.

---

## 3  Text Envelope

A `.hqx` file is a plain text file with a specific structure that frames the
encoded payload.

### 3.1  Preamble and Identification String

A `.hqx` file may begin with arbitrary text — email headers, descriptive
paragraphs, or any other content.  A decoder **must** scan through and ignore
everything until it finds the mandatory identification string:

```
(This file must be converted with BinHex 4.0)
```

This string must appear on its own, starting in the first column, followed by
a line ending.

**Decoder guidance:**  While encoders must write the string exactly as shown,
decoders should implement a tolerant search strategy.  Searching for the
substring `"(This file must be converted with BinHex"` is sufficient — this
allows the decoder to handle minor variations in the trailing text (some
encoders wrote version numbers slightly differently) while still being
specific enough to avoid false matches.

### 3.2  Colon-Delimited Payload

The encoded data block is framed by two colon characters (`:`):

* A **starting colon** appears on a new line immediately after the
  identification string.
* An **ending colon** appears on the same line as the final encoded
  characters.

Everything between the two colons (exclusive) is the encoded payload.

**Trailing exclamation mark:**  Some older or non-compliant encoders produced
an extra `!` character immediately before the final colon.  Because `!` is the
BinHex alphabet character for value 0, it simply contributes six zero bits to
the accumulator.  Since the decoder reads exactly the number of bytes
specified by the header — not until end-of-stream — these extra trailing zero
bits are never extracted and are harmlessly discarded.  No special-case
detection or stripping logic is necessary; the normal end-of-stream handling
takes care of it automatically.

### 3.3  Encoder Formatting Rules

Encoders must produce a canonical `.hqx` file:

* Insert a line break after every **64 characters** of encoded data.
* The first line of data (which includes the leading colon) will therefore be
  64 characters (colon + 63 data characters + line break).
* The final line (including the trailing colon) will be 2–65 characters.
* Use the platform-appropriate line ending: `CR` (0x0D) for classic Mac OS,
  `LF` (0x0A) for Unix, `CR LF` for DOS/Windows.

### 3.4  Decoder Whitespace Tolerance

Decoders must be **highly tolerant** of whitespace variations.  Files may have
passed through multiple email gateways that reformatted lines, inserted
spaces, or converted line endings.

* Accept lines of **any length** (the 64-character rule is for encoders only).
* Ignore all **whitespace characters** wherever they appear between the two
  colons.  "Whitespace" here means: carriage return (0x0D), line feed (0x0A),
  horizontal tab (0x09), and space (0x20).
* Only the 64 valid encoding characters and the terminating colon are
  significant; everything else is silently skipped.

---

## 4  6-Bit ASCII Encoding

This outermost encoding layer converts the binary payload to and from
printable ASCII characters.  The process is conceptually similar to Base64 but
uses a different character set and has no explicit padding marker.

### 4.1  The Character Set

BinHex 4.0 defines a 64-character alphabet.  Each 6-bit value (0–63) maps to
a specific printable ASCII character.  The alphabet string (reading left to
right, index 0 through 63) is:

```
!"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr
```

Note the **gaps**: the digits `7` is absent; the letters `O`, `W`, `g`, `n`,
`o`, and `s` through `z` (minus `p`, `q`, `r`) are omitted; and various
punctuation characters (`.`, `/`, `:`, etc.) are excluded.  These omissions
were a deliberate attempt to avoid characters commonly corrupted by 1980s
email gateways, though the set was ultimately less robust than the one later
adopted by Base64.

A complete character-to-value table is provided in Appendix A.

### 4.2  Bit Reassembly

The encoding process converts three 8-bit bytes into four 6-bit values:

**Encoding (binary → text):**

1. Read 3 bytes from the binary stream (24 bits total).
2. Split into four consecutive 6-bit groups.
3. Use each 6-bit value as an index into the character table.
4. Output the four corresponding ASCII characters.

**Decoding (text → binary):**

1. Read ASCII characters from the payload (skipping whitespace).
2. Map each character to its 6-bit value using the reverse of the character
   table (invalid characters are a fatal error).
3. Accumulate 6-bit values in a bit buffer.
4. Extract 8-bit bytes from the buffer as they become available.

A practical decoder maintains an integer accumulator and a count of valid
bits.  Each input character contributes 6 bits (shifted in from the right);
whenever the accumulator holds ≥ 8 bits, one byte is extracted from the top.

### 4.3  End-of-Stream Handling

BinHex 4.0 does **not** use an explicit padding character (unlike Base64's
`=`).  The encoding simply terminates when the binary stream is exhausted:

* If the final group has **1 remaining byte** (8 bits): it produces **2**
  output characters (8 bits → two 6-bit values, the second padded with zeros
  in its low-order bits).
* If the final group has **2 remaining bytes** (16 bits): it produces **3**
  output characters.
* If the binary stream length is a multiple of 3, the last group is a full
  group of 4 characters with no padding needed.

A decoder does not need to explicitly handle padding — the binary stream
lengths (data fork size, resource fork size, header size) are all known from
the header, so the decoder reads exactly as many decoded bytes as required
and any fractional trailing bits in the accumulator are simply discarded.

---

## 5  Run-Length Expansion (RLE90)

After removing the 6-bit ASCII encoding, the resulting binary stream must
be decompressed to recover the raw structured data.

### 5.1  Overview

BinHex 4.0 uses a simple run-length encoding scheme with `0x90` as the
escape (marker) byte.  The name "RLE90" comes from this marker value.

The scheme encodes runs of **3 or more** identical consecutive bytes as a
compact 3-byte representation.  Runs of 1 or 2 identical bytes are left
uncompressed.  The literal byte `0x90` itself must be escaped, since it
serves as the marker.

**Encoding rule** (for reference):

| Input condition | Encoded output | Notes |
|----------------|---------------|-------|
| Single byte *B* (B ≠ 0x90) | `B` | Pass through literally. |
| Two identical bytes *B B* (B ≠ 0x90) | `B B` | Pass through literally. |
| Run of *N* identical bytes *B* (N ≥ 3, B ≠ 0x90) | `B 90 N` | *N* is the total count (3–255). |
| Single literal 0x90 | `90 00` | The escape sequence. |
| Run of *N* 0x90 bytes (N ≥ 2) | `90 00 90 N` | First 0x90 escaped (establishes `prev = 0x90`), then a standard `90 N` run marker encodes the total count *N*. The decoder emits *N*−1 additional copies from the run (since one was already output by the escape). See §5.3. |

### 5.2  Decoding Algorithm

The decoder maintains a single piece of state: the **last emitted byte**
(called `prev`).  Additionally, when a run marker is decoded, a **pending
repeat count** tracks remaining copies.

**State variables:**

* `prev` — the most recently emitted byte (initially undefined).
* `pending` — remaining repeat copies to emit (initially 0).
* `marker_seen` — true if the last raw byte was `0x90` (initially false).

**For each output byte:**

1. If `pending > 0`: emit `prev`, decrement `pending`, done.

2. Read the next raw byte *b* from the 6-bit decoder.

3. If `marker_seen` is true:
   - Clear `marker_seen`.
   - If *b* = 0x00: emit literal `0x90`.  Set `prev = 0x90`.  Done.
   - If *b* = 0x01: **error** — a count of 1 is illegal (see §5.3).
   - If *b* ≥ 0x02: set `pending = b − 2`, emit `prev`.  Done.  
     (The byte was already emitted once before the `0x90` marker; with the
     current emission, that is 2 copies total; the remaining `b − 2` copies
     will drain via step 1.)

4. If *b* = 0x90: set `marker_seen = true`, loop back to step 2 (the marker
   itself produces no output).

5. Otherwise (normal byte): set `prev = b`, emit *b*.  Done.

### 5.3  Edge Cases and Examples

The 0x90 marker creates several non-obvious situations that a conforming
implementation must handle correctly.

**A count of 1 is illegal.**  Since the byte before the marker was already
emitted literally, a count of 1 would mean "repeat the previous byte 1 time
total" — but it was already output once.  This would be a no-op that wastes a
byte.  Encoders must never produce it, and decoders should reject it as an
error.

**A count of 2 is valid but wasteful.**  A count of 2 means the byte appears
twice total — one literal emission plus one repeat.  Encoders should never
produce this (writing the byte twice literally is shorter: 2 bytes vs. 3),
but a robust decoder should accept it.

**The literal 0x90 escape (`90 00`).**  The byte after the marker is 0x00,
meaning "this was just a literal 0x90, not a run marker."  The literal 0x90
is emitted and becomes the new `prev` for subsequent run markers.

**Runs of the 0x90 byte itself.**  To encode *N* copies of 0x90 when N ≥ 2:
the first 0x90 is emitted as a literal escape (`90 00`), which establishes
`prev = 0x90`.  Then a standard run marker follows: `90 NN`, where *NN*
equals the total run length *N* — the same count semantics as any other
`B 90 N` run.  In the standard RLE scheme, *N* counts the original literal
emission before the marker as one of the *N* total, so the `90 NN` sequence
produces *N*−1 new copies.  The decoder processes this naturally: `90 00`
sets prev to 0x90, then `90 NN` repeats prev using the standard step 3 logic
from §5.2 (emit prev once, set pending = *NN*−2), yielding *NN*−1 additional
copies for a grand total of *N*.

| Encoded (hex) | Decoded (hex) | Explanation |
|--------------|--------------|-------------|
| `41 90 04` | `41 41 41 41` | Run of 4 × 0x41. |
| `41 41` | `41 41` | Two literal 0x41 bytes (no RLE). |
| `90 00` | `90` | Escaped literal 0x90. |
| `90 00 90 03` | `90 90 90` | Literal 0x90 (from `90 00`), then repeat 0x90 three times total (two more from `90 03`). |
| `41 90 00 34` | `41 90 34` | Literal 0x41, escaped literal 0x90, literal 0x34. |

---

## 6  Binary Stream Structure

After removing both the ASCII encoding and the RLE compression, the result is
a raw binary stream containing the file's header, data fork, and resource
fork, each followed by a 2-byte CRC.

### 6.1  Byte-Order Conventions

All multi-byte integer values in the binary stream are stored in **big-endian**
(most-significant byte first) order, following the Motorola 68000 convention
used by the Macintosh.

### 6.2  Master Stream Layout

The binary stream is a simple concatenation of three sections:

```
┌─────────────────────────────────────┐
│  Header  (variable length)          │
│  ├── filename + metadata fields     │
│  └── 2-byte Header CRC             │
├─────────────────────────────────────┤
│  Data Fork  (DLEN bytes)            │
│  └── 2-byte Data Fork CRC          │
├─────────────────────────────────────┤
│  Resource Fork  (RLEN bytes)        │
│  └── 2-byte Resource Fork CRC      │
└─────────────────────────────────────┘
```

### 6.3  Header

The header has a variable length because the filename is variable-length.
All offsets below are relative to the start of the header.

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 1 | uint8 | Filename length (*n*) | Number of bytes in the filename (1–63). |
| 1 | *n* | bytes | Filename | Classic Mac OS filename (Mac Roman encoding). |
| 1+*n* | 1 | uint8 | Null terminator | Always 0x00. |
| 2+*n* | 4 | OSType | File Type | 4-byte type code (e.g. `TEXT`, `APPL`). |
| 6+*n* | 4 | OSType | Creator | 4-byte creator code (e.g. `ttxt`, `MSWD`). |
| 10+*n* | 2 | uint16 | Finder Flags | 16-bit Finder flags (see §8.2). |
| 12+*n* | 4 | uint32 | Data Fork Length | Byte count of the data fork (*DLEN*). |
| 16+*n* | 4 | uint32 | Resource Fork Length | Byte count of the resource fork (*RLEN*). |
| 20+*n* | 2 | uint16 | Header CRC | CRC-16-CCITT of bytes 0 through 19+*n* (see §7). |

**Total header size** = 22 + *n* bytes.

The inclusion of both a length-prefix (Pascal-style) and a null terminator
(C-style) for the filename is a deliberate redundancy.  The length byte is
sufficient for parsing, but the null byte makes it trivial for C programs to
treat the filename as a standard null-terminated string.

### 6.4  Data Fork

Immediately after the header CRC:

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0 | *DLEN* | bytes | Data fork content |
| *DLEN* | 2 | uint16 | Data Fork CRC |

The CRC covers the data fork content plus two implicit null bytes (see §7.2).

### 6.5  Resource Fork

Immediately after the Data Fork CRC:

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0 | *RLEN* | bytes | Resource fork content |
| *RLEN* | 2 | uint16 | Resource Fork CRC |

The CRC covers the resource fork content plus two implicit null bytes (see
§7.2).

### 6.6  Zero-Length Forks

A file may lack one or both forks.  If a fork has length zero, the
corresponding length field in the header will be 0x00000000 and no content
bytes will be present for that fork.  However, the 2-byte CRC field **must
still be present** and must contain `0x0000`.

---

## 7  Data Integrity: CRC-16-CCITT

BinHex 4.0 protects the stream with three independent 16-bit CRCs — one for
the header, one for the data fork, and one for the resource fork.  This
"defense in depth" allows a decoder to pinpoint which component is corrupted.

### 7.1  Algorithm Specification

The CRC algorithm is **CRC-16-CCITT** (also known as CRC-16/XMODEM, CRC-CCITT
FALSE):

| Parameter | Value |
|-----------|-------|
| Polynomial | 0x1021 ($x^{16} + x^{12} + x^5 + 1$) |
| Initial value | 0x0000 |
| Input reflection | None |
| Output reflection | None |
| Final XOR | None |

**Bit-level algorithm** (for each input byte):

1. XOR the input byte into the **high** byte of the 16-bit CRC register  
   (i.e., `crc ^= byte << 8`).
2. For each of the 8 bits (MSB to LSB):
   a. If the MSB of the CRC register is 1: shift left by 1 and XOR with
      0x1021.
   b. Otherwise: shift left by 1.
3. Mask the register to 16 bits (`crc &= 0xFFFF`).

Equivalently, a byte-at-a-time lookup table can be used.  See Appendix C for
the pre-computed table.

### 7.2  The CRC Placeholder Rule

The CRC for each section is calculated over the section's content **plus two
implicit null bytes** (0x00, 0x00) in the position where the CRC value will
be stored.  This is a standard technique that enables a neat verification
property.

**Encoding:**

1. Prepare the content (e.g., the raw data fork bytes).
2. Append two null bytes (0x0000) to the content.
3. Compute the CRC-16-CCITT over the combined block (content + two nulls).
4. Store the resulting CRC value in place of the two null bytes.

**Decoding / Verification:**

1. Read the section's content followed by the 2-byte CRC as received.
2. Compute the CRC-16-CCITT over the entire block (content + received CRC
   bytes).
3. If the data is valid, the resulting CRC register value will be **0x0000**.
   Any non-zero result indicates corruption.

This self-checking property comes from the mathematics of CRC: feeding the
CRC of a message back through the same CRC computation yields a fixed
residual.  For CRC-16-CCITT with no final XOR, that residual is zero.

### 7.3  Verification Order

A decoder should verify CRCs as it processes the stream:

1. **Header CRC** — verified immediately after parsing the header fields.
   If invalid, the file is fundamentally corrupt and further parsing is
   unreliable.
2. **Data Fork CRC** — verified after reading all data fork bytes.
3. **Resource Fork CRC** — verified after reading all resource fork bytes.

### 7.4  Independent CRC Scopes

Each CRC computation is **independent**.  The CRC register must be
re-initialized to 0x0000 before computing the CRC for the header, again
before the data fork, and a third time before the resource fork.  The three
calculations do not share state.

---

## 8  Macintosh File Metadata

### 8.1  Filename Handling

The filename stored in the header uses the **Mac Roman** character encoding
and follows classic Mac OS naming conventions (maximum 63 characters, no
colons, no leading periods in the Finder).

**Decoder filename validation:**  When extracting to a classic Mac OS or
compatible system, certain characters must be translated:

| Character | Replacement | Reason |
|-----------|-------------|--------|
| Leading period (`.`) | Bullet (`•`, 0xA5 in Mac Roman) | Periods at the start of a filename are invisible in Unix-like systems and were unusual on classic Mac OS. |
| Colon (`:`) | Dash (`-`) | The colon is the Mac OS path separator and is illegal in filenames. |
| Slash (`/`) | Dash (`-`) | Required when extracting on A/UX (Apple's Unix). |

When extracting to a non-Macintosh system, normal platform-specific filename
sanitization should be applied.

### 8.2  Finder Flags

The 16-bit Finder flags field contains metadata used by the classic Mac OS
Finder.  A complete reference table is provided in Appendix B.

**Decoder behavior:**  When decoding, the following flags must be **cleared**
(set to 0) to prevent unexpected behavior on the destination system:

| Flag | Bit | Hex | Reason for clearing |
|------|-----|-----|-------------------|
| isInvisible | 14 | 0x4000 | The file would be hidden in the Finder. |
| hasBeenInited | 7 | 0x0080 | The Finder would not assign a fresh icon position. |
| OnDesk | 2 | 0x0004 | The file would be forced onto the desktop. |

All other flags should be preserved as-is.

**Encoder behavior:**  Copy the 16-bit flags field directly from the source
file's Finder Info metadata.

---

## 9  Error Conditions

A conforming decoder must treat the following as errors:

* **No identification string found** — the input is not a BinHex 4.0 file.
* **No starting colon** — the payload is missing or truncated.
* **Invalid encoding character** — a character between the colons that is not
  in the 64-character alphabet, is not whitespace, and is not the terminating
  colon.
* **Premature end of stream** — the encoded data ends before all expected
  bytes (header + data fork + resource fork + CRCs) have been decoded.
* **Invalid RLE count** — a count byte of 0x01 after a 0x90 marker (see §5.3).
* **Header CRC mismatch** — the header section's CRC verification yields a
  non-zero residual.
* **Data fork CRC mismatch** — the data fork's CRC verification fails.
* **Resource fork CRC mismatch** — the resource fork's CRC verification fails.
* **Filename length out of range** — a filename length of 0 or greater than 63.

How strictly to enforce each condition is an implementation decision.  A strict
decoder should abort on any error; a lenient decoder may choose to continue
after CRC errors (warning the user) in order to recover as much data as
possible.

---

## 10  Implementation Guidance

This section provides practical advice for implementers.  It is not part of
the format specification per se, but captures patterns that have proven
effective.

### 10.1  Layered Decoder Architecture

A natural implementation mirrors the three processing layers as three
cooperating modules, each providing a "read next byte" interface:

```
.hqx text input
  │
  ▼
┌──────────────────────────────┐
│  Six-to-Eight Converter      │  pull characters from text,
│  (6-bit → 8-bit)             │  map through alphabet, reassemble
└──────────────────────────────┘
  │  raw bytes
  ▼
┌──────────────────────────────┐
│  RLE90 Expander              │  expand 0x90-encoded runs
└──────────────────────────────┘
  │  decompressed bytes
  ▼
┌──────────────────────────────┐
│  Stream Dispatcher           │  route bytes to header parser,
│                              │  data fork buffer, resource fork
│                              │  buffer, and CRC verifier
└──────────────────────────────┘
  │  extracted forks
  ▼
Consumer
```

Data flows on demand: the consumer calls the stream dispatcher for bytes;
the dispatcher pulls from the RLE expander; the expander pulls from the
six-to-eight converter; the converter pulls characters from the text input.
This pull-based architecture keeps memory usage minimal — no intermediate
buffers for the full stream are required.

### 10.2  Six-to-Eight Converter

Maintain an integer accumulator and a bit count:

```
function next_raw_byte():
    while accum_bits < 8:
        ch = next_non_whitespace_char()
        if ch == ':' or ch == EOF: return END_OF_STREAM
        val = reverse_alphabet[ch]
        if val == INVALID: return ERROR
        accum = (accum << 6) | val
        accum_bits += 6
    accum_bits -= 8
    return (accum >> accum_bits) & 0xFF
```

The `reverse_alphabet` is a 256-entry table mapping each ASCII code to its
6-bit value (or `INVALID` for characters not in the alphabet).

### 10.3  RLE Expander State Machine

The state machine described in §5.2 can be implemented as a small function
that loops internally until it can return a decoded byte:

```
function next_decoded_byte():
    if pending > 0:
        pending -= 1
        return prev

    loop:
        b = next_raw_byte()
        if b == END_OF_STREAM: return END_OF_STREAM

        if marker_seen:
            marker_seen = false
            if b == 0x00:
                prev = 0x90
                return 0x90          // literal 0x90
            if b == 0x01:
                return ERROR         // illegal count
            pending = b - 2
            return prev              // repeat previous byte

        if b == 0x90:
            marker_seen = true
            continue                 // consume marker, loop for count

        prev = b
        return b                     // normal byte
```

### 10.4  Stream Dispatcher

The dispatcher tracks the current "section" (header / data fork / resource
fork / finished) and the remaining byte count for each fork.  After each
fork's bytes are consumed, it reads and verifies the 2-byte CRC trailer
before advancing to the next section.

For implementations that support iteration over forks (e.g., a layer that
presents the data fork and resource fork as separate "files" to a consumer),
the dispatcher should pause at section boundaries and allow the consumer to
advance explicitly.

---

## Appendix A: BinHex 4.0 Character Set

### A.1  Value-to-Character Table

The alphabet string, from index 0 to 63:

```
!"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr
```

Expanded as a table:

| Value | Char | Value | Char | Value | Char | Value | Char |
|------:|:-----|------:|:-----|------:|:-----|------:|:-----|
| 0 | `!` | 16 | `3` | 32 | `J` | 48 | `` ` `` |
| 1 | `"` | 17 | `4` | 33 | `K` | 49 | `a` |
| 2 | `#` | 18 | `5` | 34 | `L` | 50 | `b` |
| 3 | `$` | 19 | `6` | 35 | `M` | 51 | `c` |
| 4 | `%` | 20 | `8` | 36 | `N` | 52 | `d` |
| 5 | `&` | 21 | `9` | 37 | `P` | 53 | `e` |
| 6 | `'` | 22 | `@` | 38 | `Q` | 54 | `f` |
| 7 | `(` | 23 | `A` | 39 | `R` | 55 | `h` |
| 8 | `)` | 24 | `B` | 40 | `S` | 56 | `i` |
| 9 | `*` | 25 | `C` | 41 | `T` | 57 | `j` |
| 10 | `+` | 26 | `D` | 42 | `U` | 58 | `k` |
| 11 | `,` | 27 | `E` | 43 | `V` | 59 | `l` |
| 12 | `-` | 28 | `F` | 44 | `X` | 60 | `m` |
| 13 | `0` | 29 | `G` | 45 | `Y` | 61 | `p` |
| 14 | `1` | 30 | `H` | 46 | `Z` | 62 | `q` |
| 15 | `2` | 31 | `I` | 47 | `[` | 63 | `r` |

### A.2  Character-to-Value Reverse Lookup

For decoding, build a 256-entry reverse table from the alphabet string.
Initialize all entries to an invalid sentinel (e.g., 0xFF), then for each
index *i* in 0–63, set `reverse[alphabet[i]] = i`.  Any input character whose
reverse-table entry is still the invalid sentinel is not part of the BinHex
alphabet.

---

## Appendix B: Finder Flags Reference

The 16-bit Finder Flags field in the BinHex header.  Flags marked with *
should be cleared by decoders.

| Bit | Hex | Flag Name | Description |
|----:|:------|:------------------|:----------------------------------------------|
| 15 | 0x8000 | isAlias | File is an alias (symbolic link). |
| 14 | 0x4000 | isInvisible* | File is invisible in the Finder. |
| 13 | 0x2000 | hasBundle | File has a BNDL resource (icon/file associations). |
| 12 | 0x1000 | nameLocked | Filename cannot be changed from the Finder. |
| 11 | 0x0800 | isStationery | File is a "Stationery Pad" template. |
| 10 | 0x0400 | hasCustomIcon | File has a custom icon in its resource fork. |
| 9 | 0x0200 | (reserved) | Reserved; should be zero. |
| 8 | 0x0100 | isLocked | File is locked (read-only). |
| 7 | 0x0080 | hasBeenInited* | Finder has assigned icon position; clear on decode. |
| 6 | 0x0040 | hasNoINITs | File contains no INIT resources. |
| 5 | 0x0020 | isShared | Application can be opened by multiple users. |
| 4 | 0x0010 | requiresSwitchLaunch | Obsolete MultiFinder flag. |
| 3 | 0x0008 | isColor | Color label bits (part of Finder color coding). |
| 2 | 0x0004 | OnDesk* | File is on the desktop; clear on decode. |
| 1 | 0x0002 | (reserved) | Reserved; should be zero. |
| 0 | 0x0001 | isBusy | File is currently open or in use. |

---

## Appendix C: CRC-16-CCITT Byte-at-a-Time Implementation

### C.1  Lookup Table

The following 256-entry table allows computing the CRC one byte at a time
instead of one bit at a time.  Each entry is a 16-bit unsigned integer in
hexadecimal.

```
0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
```

### C.2  Byte-at-a-Time Algorithm

```
function crc16_ccitt(data, length):
    crc = 0x0000
    for i = 0 to length - 1:
        index = ((crc >> 8) XOR data[i]) AND 0xFF
        crc = ((crc << 8) XOR table[index]) AND 0xFFFF
    return crc
```

### C.3  Bit-at-a-Time Algorithm

For reference, the equivalent bit-level computation (polynomial 0x1021):

```
function crc16_ccitt_bitwise(data, length):
    crc = 0x0000
    for each byte in data:
        crc = crc XOR (byte << 8)
        for bit = 0 to 7:
            if (crc AND 0x8000) != 0:
                crc = (crc << 1) XOR 0x1021
            else:
                crc = crc << 1
            crc = crc AND 0xFFFF
    return crc
```
