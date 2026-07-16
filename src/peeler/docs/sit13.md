# SIT Method 13 ("LZSS + Huffman") — Format Specification

## 1  Introduction

### 1.1  What is Method 13?

StuffIt archives (`.sit`) support multiple compression methods, identified by
number.  Method 13 is an **LZSS + Huffman** compression scheme that combines a
sliding-window dictionary compressor with prefix (Huffman) codes for entropy
coding.  It belongs to the same broad family as DEFLATE (used in ZIP and gzip),
but differs in several important ways — most notably the use of *two
alternating* literal/length Huffman trees and a distance encoding with an
implicit high bit.

The LZSS (Lempel–Ziv–Storer–Szymanski) component replaces repeated byte
sequences with backward references into a 64 KiB sliding window, while the
Huffman coding assigns short bit sequences to frequently occurring symbols and
longer ones to rare symbols, achieving near-optimal compression without the
overhead of arithmetic coding.

### 1.2  Purpose and Scope

This document is a complete, self-contained *format specification*.  It
describes the on-wire bit-level format and every algorithm needed to build a
fully compatible decompressor for Method-13-compressed data.  It is not tied to
any particular implementation.

---

## 2  Compression Overview

### 2.1  The Big Picture

Method 13 compresses data using a dictionary-based approach.  The compressor
scans the input and either emits a *literal* byte (when no useful match is
found) or a *match reference* (a length + distance pair pointing backwards into
already-processed data).  Both literals and match parameters are entropy-coded
using Huffman trees.

```
Raw data
  │
  ▼
┌──────────────────────────────────────────────┐
│  LZSS: Find matches in 64 KiB window        │ ← Replace repeated sequences
└──────────────────────────────────────────────┘   with (length, distance) pairs
  │
  ▼
┌──────────────────────────────────────────────┐
│  Huffman coding: Encode literals, lengths,   │ ← Variable-length prefix codes
│  and distances with three Huffman trees      │   for near-optimal compression
└──────────────────────────────────────────────┘
  │
  ▼
Compressed bitstream
```

Decompression reverses the process: read Huffman-coded symbols from the
bitstream, interpret them as literals or match references, and reconstruct
the original data into a sliding window buffer.

### 2.2  Key Design Features

Several features distinguish Method 13 from other LZSS + Huffman formats:

**Two alternating literal/length trees.**  Most similar formats (e.g. DEFLATE)
use a single Huffman tree for literals and lengths.  Method 13 maintains *two*
separate literal/length trees and switches between them based on context: the
"first tree" is used after a literal byte, and the "second tree" is used after
completing a match copy.  This context-switching exploits the statistical
observation that the symbol following a literal tends to have a different
probability distribution than the symbol following a match.

**Distance encoding with implicit high bit.**  Rather than encoding distances
directly, the distance tree encodes the *bit-width* of the distance (minus
one).  The highest bit of the distance value is implicit and always 1, so only
the remaining lower bits need to be read from the bitstream.  This is similar
in spirit to the "extra bits" scheme in DEFLATE, but the mapping is different.

**No end-of-block marker.**  Unlike DEFLATE (which uses a dedicated
end-of-block symbol), Method 13 has no in-band termination signal.
Decompression halts strictly after producing a known number of uncompressed
bytes, supplied by the StuffIt container metadata.

**Two operating modes.**  Each compressed block can use either *dynamic* trees
(serialized in the bitstream using a meta-code) or one of five *predefined*
tree sets built into the format.

### 2.3  Comparison with DEFLATE

| Aspect | DEFLATE | Method 13 |
|--------|---------|-----------|
| Window size | 32 KiB | 64 KiB |
| Literal/length trees | 1 | 2 (alternating by context) |
| Distance encoding | Extra-bits table (fixed mapping) | Implicit high bit + variable low bits |
| Tree serialization | Code-length Huffman + RLE | Fixed 37-symbol meta-code + RLE |
| End-of-block | Dedicated symbol 256 | None (externally supplied size) |
| Predefined trees | 1 fixed set | 5 built-in sets |
| Bit order | LSB-first | LSB-first |

---

## 3  Bit-Level Conventions

### 3.1  Bit Order

Bits are consumed **least-significant bit first** within each byte.  That is,
bit 0 (the lowest-order bit) of each byte is read first, then bit 1, and so
on through bit 7, before moving to the next byte.

All multi-bit fields — including header fields, Huffman extra bits, and
distance low-order bits — are assembled in the same LSB-first manner.  When
reading an *n*-bit value, the first bit read occupies bit position 0 (the
least-significant position), the second bit occupies position 1, and so on.

### 3.2  Bitstream Reader

A practical implementation maintains a **bit accumulator** — a 32-bit integer
that holds pre-fetched bits in its low-order end.  Bytes are loaded one at a
time into the accumulator's upper bits, and individual bits are consumed from
the least-significant end.

**Refill:** While the accumulator holds 24 or fewer valid bits and input bytes
remain, load the next byte and place it at bit position `avail` (shifting it
left by the current number of valid bits).  Add 8 to the valid-bit count.

**Read *n* bits** (0 ≤ *n* ≤ 24): After ensuring enough valid bits, extract
the lowest *n* bits via `acc & ((1 << n) - 1)`, then right-shift `acc` by *n*
and subtract *n* from the valid count.

**Single-bit read:** A special case of the above for *n* = 1.  Useful for
walking Huffman trees one bit at a time.

### 3.3  Alignment

The bitstream is never force-aligned after the header or between symbols.
Decoding proceeds bit-continuously from the first bit of the header byte
through the last match or literal of the block.

---

## 4  Block Header

Each Method 13 compressed block begins with a single **header byte**.  This
byte controls which Huffman trees are used and how they are obtained.

### 4.1  Header Byte Layout

```
 Bit:  7  6  5  4    3      2  1  0
     +----------+--------+--------+
     |   SET    |   S    |   K    |
     +----------+--------+--------+
```

| Field | Bits | Meaning |
|-------|------|---------|
| SET | 7–4 | Code-set selector.  0 = dynamic (trees serialized in the bitstream).  1–5 = predefined code set number.  Values 6–15 are invalid. |
| S | 3 | Tree-sharing flag.  1 = the second literal/length tree is identical to the first.  0 = the second tree is independent.  (Ignored in predefined mode.) |
| K | 2–0 | Distance tree size parameter (dynamic mode only).  The number of distance tree symbols is `10 + K`, giving a range of 10–17.  Ignored in predefined mode, where sizes are fixed per set. |

The header byte is read as 8 raw bits from the bitstream according to the
LSB-first convention (§3.1).  The SET field occupies the four
most-significant bits of the byte value; S is bit 3; K is the three
least-significant bits.

---

## 5  Huffman Trees

Method 13 uses three Huffman trees for decoding:

1. **First literal/length tree** — used when decoding after a literal byte.
2. **Second literal/length tree** — used when decoding after a match.
3. **Distance tree** — used to decode match distances.

Both literal/length trees cover the same 321-symbol alphabet (§5.1).  The
distance tree covers a variable-size alphabet (§5.2).

### 5.1  Literal/Length Symbol Alphabet

The literal/length trees have 321 symbol slots, numbered 0 through 320:

| Symbol Range | Meaning |
|-------------|---------|
| 0–255 | **Literal byte values.**  The symbol value is the byte itself. |
| 256–317 | **Direct match lengths.**  The match length is `symbol − 253`, giving a range of 3–64 bytes. |
| 318 | **Extended match length (10-bit).**  Read 10 additional bits → `length = value + 65`.  Range: 65–1,088 bytes. |
| 319 | **Extended match length (15-bit).**  Read 15 additional bits → `length = value + 65`.  Range: 65–32,832 bytes. |
| 320 | **Reserved / invalid.**  Must not occur in a valid stream.  Treat as a fatal error.  (Historical notes mention this as an end-of-block marker, but no conformant implementation uses it.) |

The maximum possible match length is therefore 32,832 bytes (symbol 319 with
all 15 extra bits set to 1: `32767 + 65 = 32832`).

### 5.2  Distance Symbol Alphabet

The distance tree has a variable number of symbols.  In dynamic mode, the
count is `10 + K` (where K is from the header byte), giving 10–17 symbols.
In predefined mode, each set specifies its own symbol count (11, 13, 14, 11,
or 11 for sets 1–5 respectively).

Distance symbols encode the *bit-width* of the backward distance, with an
implicit highest bit.  The decoding procedure is:

| Distance Symbol | Meaning |
|----------------|---------|
| 0 | Distance = 1 (special case, no extra bits). |
| *d* ≥ 1 | Read (*d* − 1) extra bits from the bitstream to get value *x*.  Distance = `(1 << (d − 1)) + x + 1`. |

This encoding covers distances from 1 through 65,536 (the full 64 KiB window).

**Example:** Distance symbol *d* = 5 means: read 4 extra bits (*x*).  The
distance is `(1 << 4) + x + 1 = 17 + x`, covering the range 17–32.

### 5.3  Canonical Huffman Code Construction

All trees (except the meta-code tree, §6.2) are built from a list of
**code lengths** using the canonical Huffman construction:

1. **Group symbols by code length.**  Within each length, order symbols by
   ascending symbol number.

2. **Assign codes.**  Let `code = 0`.  Process lengths from 1 upward.  For
   each new length, left-shift `code` by 1.  Assign sequential code values to
   all symbols of that length, incrementing `code` after each.

3. **Build the decoding tree.**  Insert each assigned code into a binary tree,
   reading bits MSB-first from the code value.  During decompression, bits will
   be *read* from the bitstream LSB-first — but the tree is built MSB-first.
   **Do not reverse the canonical codes.**

A code length of 0 means the symbol is absent and receives no code.

### 5.4  Single-Symbol Tree Edge Case

If a tree has only one symbol (all other symbols have code length 0), the root
node itself *is* the leaf.  When decoding from such a tree, return the symbol
immediately **without consuming any bits** from the bitstream.  This is
critical for correctness — attempting to read a bit from a single-symbol tree
would produce incorrect results.

### 5.5  Maximum Code Length

The maximum observed code length in the predefined tables is 18.  The dynamic
meta-code permits arbitrarily large lengths in theory, but practical streams
stay within reasonable bounds.  Implementations should support code lengths up
to at least 18.

---

## 6  Tree Serialization (Dynamic Mode)

When the header byte's SET field is 0, all three Huffman trees are serialized
in the bitstream.  The trees are encoded as lists of code lengths, which are
themselves compressed using a fixed **meta-code** — a second-level Huffman code
that is hardcoded into the format.

The serialization order is:

1. First literal/length tree (321 code lengths).
2. Second literal/length tree (321 code lengths) — **skipped** if S = 1
   (tree sharing), in which case the second tree is an exact copy of the first.
3. Distance tree (`10 + K` code lengths).

### 6.1  Tree Sharing

When the S flag (header bit 3) is set to 1, only the first literal/length tree
is serialized.  The second tree is a direct alias — it uses the identical
code-length array and tree structure.  This saves space when the statistical
properties of post-literal and post-match contexts are similar.

When S = 0, both literal/length trees are serialized independently, each with
its own 321-entry code-length list.

### 6.2  The Meta-Code

The meta-code is a fixed 37-symbol Huffman code used to compress the
code-length lists of the three main trees.  It is defined by 37 explicit
(codeword, code-length) pairs and is **not** built using the canonical
construction algorithm (§5.3).  Instead, the 37 codewords are inserted
directly into the decoding tree using their literal bit patterns.

**This is a critical distinction:** using the canonical code construction
for the meta-code tree will produce an *incorrect* tree.  The meta-code must
be built by direct insertion of the following table:

```
Symbol  Codeword  Length
  0      0x00DD     11
  1      0x001A      8
  2      0x0002      8
  3      0x0003      8
  4      0x0000      8
  5      0x000F      7
  6      0x0035      6
  7      0x0005      5
  8      0x0006      5
  9      0x0007      5
 10      0x001B      5
 11      0x0034      6
 12      0x0001      5
 13      0x0001      6
 14      0x000E      7
 15      0x000C      7
 16      0x0036      9
 17      0x01BD     12
 18      0x0006     10
 19      0x000B     11
 20      0x000E     11
 21      0x001F     12
 22      0x001E     12
 23      0x0009     11
 24      0x0008     11
 25      0x000A     11
 26      0x01BC     12
 27      0x01BF     12
 28      0x01BE     12
 29      0x01B9     12
 30      0x01B8     12
 31      0x0004      5
 32      0x0002      2
 33      0x0001      2
 34      0x0007      3
 35      0x000C      4
 36      0x0002      5
```

Each codeword is inserted into the binary tree MSB-first (highest bit of the
codeword first).  During decoding, individual bits are read from the bitstream
LSB-first and used to walk the tree.

### 6.3  Meta-Code Symbols and Code-Length RLE

The 37 meta-code symbols are interpreted as commands that build up a list of
code lengths.  The decoder maintains a **current length** variable `L`,
initialized to 0 before each code-length list.

For each position *i* in the output list (until the required number of entries
has been produced):

1. Decode one meta-code symbol *m*.
2. Interpret *m* according to the following table:

| Symbol | Action |
|--------|--------|
| 0–30 | **Set length directly:** `L = m + 1`.  Emit `L` at position *i*. |
| 31 | **Absent symbol:** `L = 0`.  Emit `L` (= 0) at position *i*. |
| 32 | **Increment:** `L = L + 1`.  Emit `L` at position *i*. |
| 33 | **Decrement:** `L = L − 1`.  Emit `L` at position *i*. |
| 34 | **Conditional repeat:** Read 1 bit *b*.  If *b* = 1, emit **two** entries with value `L` (the extra copy plus the normal emit).  If *b* = 0, emit **one** entry with value `L` (just the normal emit).  Either way, advance *i* past all emitted entries and continue to the next meta-code symbol. |
| 35 | **Short repeat:** Read 3 bits → *n*.  Emit `L` for (*n* + 2) entries, then emit one more entry with value `L` (the normal emit).  Total: *n* + 3 entries. |
| 36 | **Long repeat:** Read 6 bits → *n*.  Emit `L` for (*n* + 10) entries, then emit one more entry with value `L` (the normal emit).  Total: *n* + 11 entries. |

**Important detail for commands 34–36:** These commands include the "normal"
per-iteration emit as part of their total output.  When command 34 with *b* = 1
is decoded, the two entries are: one from the conditional branch, plus one from
the standard per-iteration emit.  Commands 35 and 36 similarly append one
trailing emit after their bulk repeat.

Decoding stops as soon as exactly the required number of code lengths has been
produced for the current tree.

---

## 7  Predefined Trees (Sets 1–5)

When the header byte's SET field is 1–5, no trees are serialized in the
bitstream.  Instead, all three trees are constructed from built-in code-length
tables that are part of the format specification.

### 7.1  Predefined Mode Behavior

In predefined mode, the S (tree-sharing) flag from the header byte is
**ignored**.  Both the first and second literal/length trees are always built
independently from their respective predefined code-length arrays — even when
S = 1.  The K field (bits 2–0) is also unused in predefined mode, since
distance tree sizes are fixed per set.

### 7.2  Distance Tree Sizes

Each predefined set specifies its own distance tree symbol count:

| Set | Distance Tree Symbols |
|-----|----------------------|
| 1 | 11 |
| 2 | 13 |
| 3 | 14 |
| 4 | 11 |
| 5 | 11 |

### 7.3  Code-Length Tables

The following tables are normative.  Every conformant encoder and decoder must
use these exact values.

#### 7.3.1  First Literal/Length Tree Code Lengths

321 entries per set, defining the code length for symbols 0–320.

```c
static const int8_t first_tree_lengths[5][321] = {
    /* Set 1 */
    4,5,7,8,8,9,9,9,9,7,9,9,9,8,9,9,9,9,9,9,9,9,9,10,9,9,10,10,9,
    10,9,9,5,9,9,9,9,10,9,9,9,9,9,9,9,9,7,9,9,8,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,8,9,9,8,8,9,9,9,9,9,9,9,7,8,9,7,9,9,7,7,9,9,9,
    9,10,9,10,10,10,9,9,9,5,9,8,7,5,9,8,8,7,9,9,8,8,5,5,7,10,5,8,
    5,8,9,9,9,9,9,10,9,9,10,9,9,10,10,10,10,10,10,10,9,10,10,10,10,
    10,10,10,9,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,9,10,10,
    10,10,10,10,10,9,9,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,
    10,9,10,10,10,10,10,9,10,10,10,10,10,10,10,10,10,10,10,10,10,10,
    10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,9,10,10,10,
    10,10,10,10,10,10,10,10,9,9,10,10,9,10,10,10,10,10,10,10,9,10,
    10,10,9,10,9,5,6,5,5,8,9,9,9,9,9,9,10,10,10,9,10,10,10,10,10,
    10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,9,10,9,9,9,10,
    9,10,9,10,9,10,9,10,10,10,9,10,9,10,10,9,9,9,6,9,9,10,9,5,

    /* Set 2 */
    4,7,7,8,7,8,8,8,8,7,8,7,8,7,9,8,8,8,9,9,9,9,10,10,9,10,10,10,
    10,10,9,9,5,9,8,9,9,11,10,9,8,9,9,9,8,9,7,8,8,8,9,9,9,9,9,10,
    9,9,9,10,9,9,10,9,8,8,7,7,7,8,8,9,8,8,9,9,8,8,7,8,7,10,8,7,7,
    9,9,9,9,10,10,11,11,11,10,9,8,6,8,7,7,5,7,7,7,6,9,8,6,7,6,6,7,
    9,6,6,6,7,8,8,8,8,9,10,9,10,9,9,8,9,10,10,9,10,10,9,9,10,10,
    10,10,10,10,10,9,10,10,11,10,10,10,10,10,10,10,11,10,11,10,10,
    9,11,10,10,10,10,10,10,9,9,10,11,10,11,10,11,10,12,10,11,10,12,
    11,12,10,12,10,11,10,11,11,11,9,10,11,11,11,12,12,10,10,10,11,
    11,10,11,10,10,9,11,10,11,10,11,11,11,10,11,11,12,11,11,10,10,
    10,11,10,10,11,11,12,10,10,11,11,12,11,11,10,11,9,12,10,11,11,
    11,10,11,10,11,10,11,9,10,9,7,3,5,6,6,7,7,8,8,8,9,9,9,11,10,
    10,10,12,13,11,12,12,11,13,12,12,11,12,12,13,12,14,13,14,13,15,
    13,14,15,15,14,13,15,15,14,15,14,15,15,14,15,13,13,14,15,15,14,
    14,16,16,15,15,15,12,15,10,

    /* Set 3 */
    6,6,6,6,6,9,8,8,4,9,8,9,8,9,9,9,8,9,9,10,8,10,10,10,9,10,10,
    10,9,10,10,9,9,9,8,10,9,10,9,10,9,10,9,10,9,9,8,9,8,9,9,9,10,
    10,10,10,9,9,9,10,9,10,9,9,7,8,8,9,8,9,9,9,8,9,9,10,9,9,8,9,
    8,9,8,8,8,9,9,9,9,9,10,10,10,10,10,9,8,8,9,8,9,7,8,8,9,8,10,
    10,8,9,8,8,8,10,8,8,8,8,9,9,9,9,10,10,10,10,10,9,7,9,9,10,10,
    10,10,10,9,10,10,10,10,10,10,9,9,10,10,10,10,10,10,10,10,9,10,
    10,10,10,10,10,9,10,10,10,10,10,10,10,9,9,9,10,10,10,10,10,10,
    10,10,10,10,10,10,10,10,10,9,10,10,10,10,9,8,9,10,10,10,10,10,
    10,10,10,10,10,10,9,10,10,10,9,10,10,10,10,10,10,10,10,10,10,
    10,10,10,10,9,9,10,10,10,10,10,10,9,10,10,10,10,10,10,9,9,9,
    10,10,10,10,10,10,9,9,10,9,9,8,9,8,9,4,6,6,6,7,8,8,9,9,10,10,
    10,9,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,7,10,
    10,10,7,10,10,7,7,7,7,7,6,7,10,7,7,10,7,7,7,6,7,6,6,7,7,6,6,
    9,6,9,10,6,10,2,

    /* Set 4 */
    2,6,6,7,7,8,7,8,7,8,8,9,8,9,9,9,8,8,9,9,9,10,10,9,8,10,9,10,
    9,10,9,9,6,9,8,9,9,10,9,9,9,10,9,9,9,9,8,8,8,8,8,9,9,9,9,9,
    9,9,9,9,9,10,10,9,7,7,8,8,8,8,9,9,7,8,9,10,8,8,7,8,8,10,8,8,
    8,9,8,9,9,10,9,11,10,11,9,9,8,7,9,8,8,6,8,8,8,7,10,9,7,8,7,7,
    8,10,7,7,7,8,9,9,9,9,10,11,9,11,10,9,7,9,10,10,10,11,11,10,10,
    11,10,10,10,11,11,10,9,10,10,11,10,11,10,11,10,10,10,11,10,11,
    10,10,9,10,10,11,10,10,10,10,9,10,10,10,10,11,10,11,10,11,10,
    11,11,11,10,12,10,11,10,11,10,11,11,10,8,10,10,11,10,11,11,11,
    10,11,10,11,10,11,11,11,9,10,11,11,10,11,11,11,10,11,11,11,10,
    10,10,10,10,11,10,10,11,11,10,10,9,11,10,10,11,11,10,10,10,11,
    10,10,10,10,10,10,9,11,10,10,8,10,8,6,5,6,6,7,7,8,8,8,9,10,11,
    10,10,11,11,12,12,10,11,12,12,12,12,13,13,13,13,13,12,13,13,15,
    14,12,14,15,16,12,12,13,15,14,16,15,17,18,15,17,16,15,15,15,15,
    13,13,10,14,12,13,17,17,18,10,17,4,

    /* Set 5 */
    7,9,9,9,9,9,9,9,9,8,9,9,9,7,9,9,9,9,9,9,9,9,9,10,9,10,9,10,9,
    10,9,9,5,9,7,9,9,9,9,9,7,7,7,9,7,7,8,7,8,8,7,7,9,9,9,9,7,7,7,
    9,9,9,9,9,9,7,9,7,7,7,7,9,9,7,9,9,7,7,7,7,7,9,7,8,7,9,9,9,9,
    9,9,9,9,9,9,9,9,7,8,7,7,7,8,8,6,7,9,7,7,8,7,5,6,9,5,7,5,6,7,
    7,9,8,9,9,9,9,9,9,9,9,10,9,10,10,10,9,9,10,10,10,10,10,10,10,
    9,10,10,10,10,10,10,10,10,10,10,10,9,10,10,10,9,10,10,10,9,9,
    10,9,9,9,9,10,10,10,10,10,10,10,10,10,10,10,9,10,10,10,10,10,
    10,10,10,10,9,10,10,10,9,10,10,10,9,9,9,10,10,10,10,10,9,10,9,
    10,10,9,10,10,9,10,10,10,10,10,10,10,9,10,10,10,10,10,10,10,10,
    10,10,10,10,10,10,10,9,10,10,10,10,10,10,10,9,10,9,10,9,10,10,
    9,5,6,8,8,7,7,7,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,
    10,10,10,10,10,10,9,10,10,5,10,8,9,8,9
};
```

#### 7.3.2  Second Literal/Length Tree Code Lengths

321 entries per set.

```c
static const int8_t second_tree_lengths[5][321] = {
    /* Set 1 */
    4,5,6,6,7,7,6,7,7,7,6,8,7,8,8,8,8,9,6,9,8,9,8,9,9,9,8,10,5,9,
    7,9,6,9,8,10,9,10,8,8,9,9,7,9,8,9,8,9,8,8,6,9,9,8,8,9,9,10,8,
    9,9,10,8,10,8,8,8,8,8,9,7,10,6,9,9,11,7,8,8,9,8,10,7,8,6,9,10,
    9,9,10,8,11,9,11,9,10,9,8,9,8,8,8,8,10,9,9,10,10,8,9,8,8,8,11,
    9,8,8,9,9,10,8,11,10,10,8,10,9,10,8,9,9,11,9,11,9,10,10,11,10,
    12,9,12,10,11,10,11,9,10,10,11,10,11,10,11,10,11,10,10,10,9,9,
    9,8,7,6,8,11,11,9,12,10,12,9,11,11,11,10,12,11,11,10,12,10,11,
    10,10,10,11,10,11,11,11,9,12,10,12,11,12,10,11,10,12,11,12,11,
    12,11,12,10,12,11,12,11,11,10,12,10,11,10,12,10,12,10,12,10,11,
    11,11,10,11,11,11,10,12,11,12,10,10,11,11,9,12,11,12,10,11,10,
    12,10,11,10,12,10,11,10,7,5,4,6,6,7,7,7,8,8,7,7,6,8,6,7,7,9,8,
    9,9,10,11,11,11,12,11,10,11,12,11,12,11,12,12,12,12,11,12,12,
    11,12,11,12,11,13,11,12,10,13,10,14,14,13,14,15,14,16,15,15,18,
    18,18,9,18,8,

    /* Set 2 */
    5,6,6,6,6,7,7,7,7,7,7,8,7,8,7,7,7,8,8,8,8,9,8,9,8,9,9,9,7,9,
    8,8,6,9,8,9,8,9,8,9,8,9,8,9,8,9,8,8,8,8,8,9,8,9,8,9,9,10,8,10,
    8,9,9,8,8,8,7,8,8,9,8,9,7,9,8,10,8,9,8,9,8,9,8,8,8,9,9,9,9,10,
    9,11,9,10,9,10,8,8,8,9,8,8,8,9,9,8,9,10,8,9,8,8,8,11,8,7,8,9,
    9,9,9,10,9,10,9,10,9,8,8,9,9,10,9,10,9,10,8,10,9,10,9,11,10,11,
    9,11,10,10,10,11,9,11,9,10,9,11,9,11,10,10,9,10,9,9,8,10,9,11,
    9,9,9,11,10,11,9,11,9,11,9,11,10,11,10,11,10,11,9,10,10,11,10,
    10,8,10,9,10,10,11,9,11,9,10,10,11,9,10,10,9,9,10,9,10,9,10,9,
    10,9,11,9,11,10,10,9,10,9,11,9,11,9,11,9,10,9,11,9,11,9,11,9,
    10,8,11,9,10,9,10,9,10,8,10,8,9,8,9,8,7,4,4,5,6,6,6,7,7,7,7,8,
    8,8,7,8,8,9,9,10,10,10,10,10,10,11,11,10,10,12,11,11,12,12,11,
    12,12,11,12,12,12,12,12,12,11,12,11,13,12,13,12,13,14,14,14,15,
    13,14,13,14,18,18,17,7,16,9,

    /* Set 3 */
    5,6,6,6,6,7,7,7,6,8,7,8,7,9,8,8,7,7,8,9,9,9,9,10,8,9,9,10,8,
    10,9,8,6,10,8,10,8,10,9,9,9,9,9,10,9,9,8,9,8,9,8,9,9,10,9,10,
    9,9,8,10,9,11,10,8,8,8,8,9,7,9,9,10,8,9,8,11,9,10,9,10,8,9,9,
    9,9,8,9,9,10,10,10,12,10,11,10,10,8,9,9,9,8,9,8,8,10,9,10,11,
    8,10,9,9,8,12,8,9,9,9,9,8,9,10,9,12,10,10,10,8,7,11,10,9,10,11,
    9,11,7,11,10,12,10,12,10,11,9,11,9,12,10,12,10,12,10,9,11,12,
    10,12,10,11,9,10,9,10,9,11,11,12,9,10,8,12,11,12,9,12,10,12,10,
    13,10,12,10,12,10,12,10,9,10,12,10,9,8,11,10,12,10,12,10,12,10,
    11,10,12,8,12,10,11,10,10,10,12,9,11,10,12,10,12,11,12,10,9,10,
    12,9,10,10,12,10,11,10,11,10,12,8,12,9,12,8,12,8,11,10,11,10,
    11,9,10,8,10,9,9,8,9,8,7,4,3,5,5,6,5,6,6,7,7,8,8,8,7,7,7,9,8,
    9,9,11,9,11,9,8,9,9,11,12,11,12,12,13,13,12,13,14,13,14,13,14,
    13,13,13,12,13,13,12,13,13,14,14,13,13,14,14,14,14,15,18,17,18,
    8,16,10,

    /* Set 4 */
    4,5,6,6,6,6,7,7,6,7,7,9,6,8,8,7,7,8,8,8,6,9,8,8,7,9,8,9,8,9,
    8,9,6,9,8,9,8,10,9,9,8,10,8,10,8,9,8,9,8,8,7,9,9,9,9,9,8,10,
    9,10,9,10,9,8,7,8,9,9,8,9,9,9,7,10,9,10,9,9,8,9,8,9,8,8,8,9,
    9,10,9,9,8,11,9,11,10,10,8,8,10,8,8,9,9,9,10,9,10,11,9,9,9,9,
    8,9,8,8,8,10,10,9,9,8,10,11,10,11,11,9,8,9,10,11,9,10,11,11,9,
    12,10,10,10,12,11,11,9,11,11,12,9,11,9,10,10,10,10,12,9,11,10,
    11,9,11,11,11,10,11,11,12,9,10,10,12,11,11,10,11,9,11,10,11,10,
    11,9,11,11,9,8,11,10,11,11,10,7,12,11,11,11,11,11,12,10,12,11,
    13,11,10,12,11,10,11,10,11,10,11,11,11,10,12,11,11,10,11,10,10,
    10,11,10,12,11,12,10,11,9,11,10,11,10,11,10,12,9,11,11,11,9,11,
    10,10,9,11,10,10,9,10,9,7,4,5,5,5,6,6,7,6,8,7,8,9,9,7,8,8,10,
    9,10,10,12,10,11,11,11,11,10,11,12,11,11,11,11,11,13,12,11,12,
    13,12,12,12,13,11,9,12,13,7,13,11,13,11,10,11,13,15,15,12,14,
    15,15,15,6,15,5,

    /* Set 5 */
    8,10,11,11,11,12,11,11,12,6,11,12,10,5,12,12,12,12,12,12,12,
    13,13,14,13,13,12,13,12,13,12,15,4,10,7,9,11,11,10,9,6,7,8,9,
    6,7,6,7,8,7,7,8,8,8,8,8,8,9,8,7,10,9,10,10,11,7,8,6,7,8,8,9,
    8,7,10,10,8,7,8,8,7,10,7,6,7,9,9,8,11,11,11,10,11,11,11,8,11,
    6,7,6,6,6,6,8,7,6,10,9,6,7,6,6,7,10,6,5,6,7,7,7,10,8,11,9,13,
    7,14,16,12,14,14,15,15,16,16,14,15,15,15,15,15,15,15,15,14,15,
    13,14,14,16,15,17,14,17,15,17,12,14,13,16,12,17,13,17,14,13,13,
    14,14,12,13,15,15,14,15,17,14,17,15,14,15,16,12,16,15,14,15,16,
    15,16,17,17,15,15,17,17,13,14,15,15,13,12,16,16,17,14,15,16,15,
    15,13,13,15,13,16,17,15,17,17,17,16,17,14,17,14,16,15,17,15,15,
    14,17,15,17,15,16,15,15,16,16,14,17,17,15,15,16,15,17,15,14,16,
    16,16,16,16,12,4,4,5,5,6,6,6,7,7,7,8,8,8,8,9,9,9,9,9,10,10,10,
    11,10,11,11,11,11,11,12,12,12,13,13,12,13,12,14,14,12,13,13,13,
    13,14,12,13,13,14,14,14,13,14,14,15,15,13,15,13,17,17,17,9,17,7
};
```

#### 7.3.3  Distance Tree Code Lengths

Variable number of entries per set.

```c
static const int8_t offset_tree_lengths[5][14] = {
    {5,6,3,3,3,3,3,3,3,4,6},          /* Set 1: 11 symbols */
    {5,6,4,4,3,3,3,3,3,4,4,4,6},      /* Set 2: 13 symbols */
    {6,7,4,4,3,3,3,3,3,4,4,4,5,7},    /* Set 3: 14 symbols */
    {3,6,5,4,2,3,3,3,4,4,6},          /* Set 4: 11 symbols */
    {6,7,7,6,4,3,2,2,3,3,6}           /* Set 5: 11 symbols */
};
```

---

## 8  Sliding Window

Method 13 uses a 64 KiB (65,536-byte) circular sliding window for LZ-style
dictionary matching.

### 8.1  Initialization

Before decoding the first symbol of a block, the entire window is initialized
to **zero bytes**.  This means that a match reference pointing to the "past"
before any output has been produced will copy zero bytes, which is a valid
(if unusual) encoding of zero-filled data.

### 8.2  Window Operations

Every output byte — whether from a literal or from a match copy — is written
into the window at the current position, and the position advances by 1.  The
position wraps modulo 65,536 (i.e. `position & 0xFFFF`).

During match copies, the source position also wraps modulo 65,536.  Overlap
between the source and destination ranges is permitted and produces the
standard LZ repeating-pattern behavior (e.g. a distance of 1 copies the most
recent byte repeatedly).

---

## 9  Decompression Procedure

This section describes the main decode loop — the heart of the decompressor.

### 9.1  State

The decoder maintains the following state:

| Variable | Initial Value | Description |
|----------|--------------|-------------|
| Active tree | First tree | Which literal/length tree to use for the next symbol.  Alternates between first and second. |
| Output position | 0 | Number of bytes produced so far. |
| Window buffer | All zeros | 64 KiB circular buffer. |
| Window position | 0 | Current write position in the window. |

### 9.2  Main Loop

The decoder loops until the required number of uncompressed bytes (supplied by
the StuffIt archive container) has been produced:

1. **Decode one symbol** from the currently active literal/length tree.

2. **If the symbol is a literal** (0–255):
   - Emit the literal byte to the output.
   - Write it into the sliding window and advance the window position.
   - Switch the active tree to the **first** tree.
   - Continue to step 1.

3. **If the symbol encodes a match length** (256–319):
   - Determine the match length *L*:
     - Symbols 256–317: `L = symbol − 253` (range 3–64).
     - Symbol 318: read 10 extra bits → `L = value + 65` (range 65–1,088).
     - Symbol 319: read 15 extra bits → `L = value + 65` (range 65–32,832).
   - Decode one symbol from the **distance tree** to get distance symbol *d*.
   - Determine the match distance *D*:
     - If *d* = 0: `D = 1`.
     - If *d* ≥ 1: read (*d* − 1) extra bits → *x*.
       `D = (1 << (d − 1)) + x + 1`.
   - Copy *L* bytes from position `(window_pos − D)` in the sliding window,
     wrapping with `& 0xFFFF`.  For each copied byte, write it to both the
     output and the window, advancing the window position.  Overlap is
     permitted.
   - After the entire match has been copied, switch the active tree to the
     **second** tree.
   - Continue to step 1.

4. **If the symbol is 320 or higher:** treat as a fatal error.

### 9.3  Context-Switching Rationale

The alternation between first and second trees is the key distinguishing
feature of Method 13.  The intuition is:

- After a **literal**, the next symbol is more likely to also be a literal
  (text tends to have few nearby matches).  The first tree can weight its
  code lengths to favor literals.
- After a **match**, the next symbol is more likely to start another match
  (repetitive data tends to cluster matches).  The second tree can weight
  its code lengths accordingly.

By using separate probability distributions for these two contexts, Method 13
achieves better compression than a single-tree approach, at the cost of
slightly increased complexity.

---

## 10  Termination

There is **no in-band termination marker** in the Method 13 format.
Decompression must stop exactly after producing the number of uncompressed
bytes specified by the StuffIt archive container metadata.

Symbol 320 is *not* a valid end-of-block marker, despite what some historical
documents suggest.  Any occurrence of symbol 320 (or any symbol > 319) must be
treated as a fatal error.

---

## 11  Error Conditions

A conforming decompressor must treat the following as fatal errors:

* Invalid SET value in the header byte (SET ≥ 6).
* Symbol 320 or any symbol > 319 decoded from a literal/length tree.
* Distance exceeding the window size (> 65,536).
* Premature exhaustion of the compressed input bitstream.
* Malformed Huffman tree (e.g. navigating to a nonexistent child node during
  tree traversal).
* Invalid meta-code command producing a code length inconsistency.

---

## 12  Implementation Guidance

This section provides practical advice for implementers.  It is not part of
the format specification per se, but captures lessons learned from existing
implementations.

### 12.1  Bit Reader Design

Because bits are consumed LSB-first, a simple accumulator-based reader that
loads bytes into the *low* bits works naturally.  A 32-bit accumulator is
always sufficient — the widest single read is 15 bits (for symbol 319's extra
field).  Refilling whenever the accumulator drops to 24 or fewer bits
guarantees that any single field can be read without running out of bits
mid-read.

### 12.2  Tree Construction — Don't Reverse Codes

The combination of LSB-first bitstream reading and MSB-first tree insertion
may seem contradictory, but it works because the canonical code assignment
produces MSB-first codes that are read one bit at a time from the bitstream.
The LSB-first ordering only affects which bit is read *first* (the low bit),
but each bit is used to choose left or right in the tree traversal.

The critical rule is: **do not reverse the canonical Huffman codes**.  Build
the tree MSB-first from the assigned codes, and drive the tree walk with
individual LSB-first bit reads from the stream.

### 12.3  Pool-Based Node Allocation

All Huffman tree nodes (meta-code tree + up to three main trees) can share a
single contiguous node pool.  A pool of 2,048 nodes is sufficient for all
trees combined.  The worst case with predefined tables (which don't need a
meta-code tree) requires approximately 1,392 nodes.  This avoids per-node
`malloc` overhead and simplifies cleanup — the entire pool is freed in one
shot.

### 12.4  Streaming Interface

An implementation may output bytes incrementally rather than all at once.  For
large matches exceeding the application's buffer size, maintain `match_left`
(remaining bytes in the current match) and `match_from` (the source position
within the sliding window) so that copying can resume across calls without
re-decoding symbols.

Two API styles are practical:

**One-shot** (`sit13_decompress`): Takes source buffer, destination buffer,
and lengths.  Decodes the full stream in a single call.  Returns the number of
bytes produced, or 0 on error.

**Streaming** (`sit13_init` / `sit13_read` / `sit13_free`):

* `sit13_init(src, src_len)` — parse the header, build trees, initialize the
  window.  Returns an opaque context.
* `sit13_read(ctx, out, cap)` — decode up to `cap` bytes into `out`.  Returns
  the number of bytes produced.  Pending match state is preserved across calls.
* `sit13_free(ctx)` — release all resources.

---

## 13  Limits and Ranges Summary

| Parameter | Value |
|-----------|-------|
| Window size | 65,536 bytes |
| Literal symbols | 0–255 |
| Match length (short) | 3–64 (symbols 256–317) |
| Match length (medium, 10-bit extra) | 65–1,088 (symbol 318) |
| Match length (long, 15-bit extra) | 65–32,832 (symbol 319) |
| Maximum match length | 32,832 bytes |
| Distance range | 1–65,536 |
| Literal/length tree symbols | 321 (0–320) |
| Distance tree symbols (dynamic) | 10–17 |
| Distance tree symbols (predefined) | Set-dependent: 11, 13, 14, 11, 11 |
| Maximum observed code length | 18 (in predefined tables) |
| Meta-code symbols | 37 |
| Node pool budget | ≤ 2,048 nodes for all trees |

---

## Appendix A: Complete Decompression Walkthrough

This appendix traces the full decompression of a single Method 13 block from
start to finish.

1. **Read the header byte** (§4): Extract the 8-bit header.  Determine the
   mode (dynamic or predefined), tree-sharing flag, and distance tree size.

2. **Build the Huffman trees** (§5–§7):
   - *Dynamic mode (SET = 0):*  Build the 37-symbol meta-code tree (§6.2).
     Use it to decode the first literal/length tree's 321 code lengths.  If
     S = 0, decode the second tree's code lengths; if S = 1, share the first
     tree.  Decode the distance tree's `10 + K` code lengths.  Build all three
     trees using canonical construction (§5.3).
   - *Predefined mode (SET = 1–5):*  Build all three trees from the
     corresponding built-in tables (§7.3), using canonical construction.

3. **Initialize state** (§8, §9.1): Zero-fill the 64 KiB window.  Set the
   active tree to the first literal/length tree.  Set the output position to 0.

4. **Main decode loop** (§9.2): Repeatedly decode symbols until the required
   uncompressed size is reached:
   - Literal → emit byte, update window, switch to first tree.
   - Match → decode length and distance, copy from window, switch to second
     tree.

5. **Return output.**  The decompressed data is the sequence of bytes written
   to the output buffer.

## Appendix B: Reference Pseudocode

```
init_window_zero()
read header H (8 bits)
SET = H >> 4
S   = (H >> 3) & 1
K   = H & 7

if SET == 0:
    build meta-code tree from fixed 37-symbol table
    first  = decode_lengths(meta, 321) → build canonical tree
    second = first if S==1 else decode_lengths(meta, 321) → build
    dist   = decode_lengths(meta, 10 + K) → build canonical tree
else if 1 ≤ SET ≤ 5:
    first  = build canonical tree from predefined_first[SET]
    second = build canonical tree from predefined_second[SET]
    dist   = build canonical tree from predefined_dist[SET]
else:
    error

active = first
while output_count < target_size:
    sym = decode(active)

    if sym < 256:
        emit literal sym
        window[wpos++ & 0xFFFF] = sym
        active = first
        continue

    if sym ≤ 317:
        L = sym - 253
    else if sym == 318:
        L = read_bits(10) + 65
    else if sym == 319:
        L = read_bits(15) + 65
    else:
        error

    d_sym = decode(dist)
    if d_sym == 0:
        D = 1
    else:
        D = (1 << (d_sym - 1)) + read_bits(d_sym - 1) + 1

    for i in 0..L-1:
        b = window[(wpos - D) & 0xFFFF]
        emit b
        window[wpos++ & 0xFFFF] = b

    active = second
```
