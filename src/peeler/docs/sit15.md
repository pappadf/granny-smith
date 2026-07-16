# SIT Method 15 ("Arsenic") — Format Specification

## 1  Introduction

### 1.1  What is Arsenic?

StuffIt archives (`.sit`) support multiple compression methods, identified by
number.  Method 15, internally named **"Arsenic"**, is a block-based
compression scheme whose design closely mirrors **bzip2**.  Both share the same
conceptual pipeline — Burrows–Wheeler Transform (BWT), Move-To-Front coding
(MTF), run-length encoding (RLE), and entropy coding — but Arsenic differs in
its choice of entropy coder (adaptive arithmetic coding instead of bzip2's
static Huffman) and in several on-wire format details.

Understanding the relationship with bzip2 is useful because anyone already
familiar with bzip2 will recognize most of the algorithmic building blocks.
The differences are called out explicitly throughout this document.

### 1.2  Purpose and Scope

This document is a complete, self-contained *format specification*.  It
describes the on-wire bit-level format and every algorithm needed to build a
fully compatible decompressor for Arsenic-compressed data.  It is not tied to
any particular implementation.

---

## 2  Compression Pipeline Overview

### 2.1  The Big Picture

To understand Arsenic, it helps to first understand *why* the pipeline looks
the way it does.  The goal is to compress arbitrary byte data as compactly as
possible.  Rather than attacking the problem with a single algorithm, Arsenic
(like bzip2) chains several simple, well-understood transforms that each make
the data "easier" for the next stage.

The **encoder** applies the following stages, in order:

```
Raw data
  │
  ▼
┌──────────────────────────────────────────────┐
│  Stage 1: Initial Run-Length Encoding (RLE)  │ ← Collapses long byte runs
└──────────────────────────────────────────────┘
  │
  ▼
┌──────────────────────────────────────────────┐
│  Stage 2: Burrows–Wheeler Transform (BWT)    │ ← Sorts contexts, clusters
└──────────────────────────────────────────────┘   repeated bytes together
  │
  ▼
┌──────────────────────────────────────────────┐
│  Stage 3: Move-To-Front Coding (MTF)         │ ← Converts clustered bytes
└──────────────────────────────────────────────┘   into mostly-small integers
  │
  ▼
┌──────────────────────────────────────────────┐
│  Stage 4: Zero Run-Length Suppression         │ ← Compactly encodes the many
└──────────────────────────────────────────────┘   zeros from MTF
  │
  ▼
┌──────────────────────────────────────────────┐
│  Stage 5: Adaptive Arithmetic Coding          │ ← Final entropy compression
└──────────────────────────────────────────────┘
  │
  ▼
Compressed bitstream
```

An optional **per-block randomization** step can be inserted between BWT and
MTF to mitigate pathological worst-case BWT inputs (see §9).

**Decompression reverses the pipeline**, reading from the compressed bitstream
and working backwards through the stages.  The decompression order is:

1. Arithmetic decode
2. Zero run-length expand
3. MTF invert
4. Inverse BWT
5. Randomization de-scramble (if flagged)
6. Final RLE expand

The rest of this document describes each stage in decompression order, since
that is what an implementer of a decompressor needs.

### 2.2  Why This Pipeline Works

Each stage has a clear purpose:

**Initial RLE (Stage 1 of compression / Stage 6 of decompression):**  
The BWT performs poorly on very long runs of a single byte (it can blow up the
output by a factor proportional to the alphabet size).  A simple run-length
limiter—capping runs at 4 + 255 = 259 bytes—prevents this worst case.  This
is the same trick bzip2 uses.

**Burrows–Wheeler Transform (Stage 2 / Stage 4):**  
The BWT is a reversible permutation of a data block that groups bytes by their
preceding context.  If the original data has any statistical structure at all,
the BWT output tends to have long stretches of identical or nearly-identical
bytes.  The transform is lossless and reversible given just the transformed
block and a single integer (the "primary index").

**Move-To-Front (Stage 3 / Stage 3):**  
MTF converts a sequence with many local repetitions (like post-BWT data) into
a sequence dominated by small values—especially zeros.  It maintains an ordered
list of the 256 byte values; each input byte is encoded as its current position
in the list, then moved to position 0.  Repeated bytes encode as 0, recently
seen bytes encode as small numbers.

**Zero Run-Length Suppression (Stage 4 / Stage 2):**  
After MTF, zeroes are extremely common.  Rather than arithmetic-coding each
zero individually, runs of the MTF-index-0 value are encoded as a compact
binary-weighted run-length.  This dramatically reduces the number of symbols
the arithmetic coder must process.

**Adaptive Arithmetic Coding (Stage 5 / Stage 1):**  
This is the final entropy coding stage.  Unlike bzip2 (which uses static
Huffman coding with multiple coding tables selected per group of 50 symbols),
Arsenic uses a fully adaptive arithmetic coder that adjusts symbol
probabilities on the fly.  This avoids the need to transmit Huffman tables
but requires both encoder and decoder to maintain synchronized probability
models.

### 2.3  Comparison with bzip2

| Aspect | bzip2 | Arsenic |
|--------|-------|---------|
| Initial RLE | Yes ("run-length encoding #1") | Yes, identical scheme |
| BWT | Yes | Yes, identical algorithm |
| MTF | Yes | Yes, identical algorithm |
| Zero run suppression | RUNA / RUNB symbols | Selector tokens 0 / 1 (equivalent) |
| Entropy coder | Static Huffman with multiple tables | Adaptive arithmetic coding |
| Randomization | Yes, identical table | Yes, identical table (§9.1) |
| Block sizes | 100k–900k (increments of 100k) | 512 to 16 MiB (powers of 2) |
| Block CRC | CRC-32, verified | CRC-32, present but not verified |
| Stream magic | `"BZ"` + version byte | `"As"` (arithmetic-coded) |

The zero-run coding deserves special note: bzip2 uses a bijective base-2
encoding with symbols RUNA (=0) and RUNB (=1).  Arsenic uses an identical
mathematical scheme, but the tokens are the selector model's values 0 and 1.
The encoding is: at bit position *p*, token *t* ∈ {0, 1} contributes
(t + 1) × 2^p to the total run length.  Both formats use this exact formula.

---

## 3  Bit-Level Conventions

Two distinct bit-ordering conventions coexist in the same compressed stream.
Keeping them straight is essential for a correct implementation.

### 3.1  Byte-to-Bit Extraction (Raw Bitstream)

Compressed input bytes are consumed sequentially.  Within each byte, bits are
extracted **most-significant bit first** (MSB-first).  All "raw" bit reads —
that is, reads of N bits directly from the bitstream — follow this convention.

#### Shift-Register Implementation

A practical bitstream reader maintains a 32-bit **left-aligned** shift register
(`window`) and an `avail` count of valid bits occupying the MSB end.

**Refill:**  Pull whole bytes into the register while `avail ≤ 24` (and input
remains), placing each byte at bit position `24 − avail` and adding 8 to
`avail`.  The threshold of 24 guarantees that a single refill always yields
at least 25 valid bits when enough input is available.

**Read *n* bits** (1 ≤ *n* ≤ 25):  After ensuring `avail ≥ n` (refilling if
needed), extract the top *n* bits via `window >> (32 − n)`, then shift
`window` left by *n* and subtract *n* from `avail`.  If `avail` is still
insufficient after refill, the stream is truncated — treat as a fatal error.

**Read-long** (*n* > 25):  Split into a 25-bit read followed by an
(*n* − 25)-bit read, assembling the result as `(hi << (n − 25)) | lo`.
This is needed for the 26-bit arithmetic-coder bootstrap (§4.2) and any
other field wider than 25 bits.

### 3.2  Arithmetic-Coded Multi-Bit Fields (LSB-First Assembly)

Several header fields (signature bytes, block-size parameter, BWT index, CRC)
are not read as raw bits but are instead decoded as sequences of individually
arithmetic-coded binary symbols (each symbol is a single 0 or 1 decoded from
a probability model).

When these symbols are reassembled into an integer, the *first* decoded bit
occupies bit position 0 (the least-significant bit), the *second* decoded
bit occupies bit position 1, and so on.  This is **LSB-first** assembly order:

```
result = 0
for i in 0 .. n-1:
    bit = decode_symbol(model)   // returns 0 or 1
    if bit:
        result |= (1 << i)
```

**These two conventions — MSB-first raw bitstream; LSB-first reassembly of
arithmetic-coded bit strings — coexist in the same stream and must not be
conflated.**

---

## 4  Adaptive Arithmetic Coder

Arsenic's entropy coding layer is an adaptive arithmetic coder.  Unlike
Huffman coding (where the code table is built once and transmitted), an
arithmetic coder maintains a probability model that is updated after every
symbol.  Both encoder and decoder keep identical copies of the model, so no
side information needs to be transmitted — the model is implicitly
synchronized.

### 4.1  Probability Model

A probability model tracks *N* symbols, each with an integer frequency.

#### Parameters (fixed at creation)

| Parameter | Meaning |
|-----------|---------|
| `first_symbol` | Lowest symbol value (inclusive) |
| `last_symbol` | Highest symbol value (inclusive) |
| `N` | Number of symbols = `last_symbol − first_symbol + 1`.  Must be ≤ 128. |
| `increment` | Amount added to a symbol's frequency each time it is decoded |
| `limit` | When `total_frequency` exceeds this, all frequencies are rescaled |

#### State

* `freq[i]` — frequency of the *i*-th symbol (0-indexed within the model).
  The actual symbol value corresponding to index *i* is `first_symbol + i`.
* `total_frequency` — sum of all `freq[i]`.

#### Initialization / Reset

Set every `freq[i] = increment`.  Then `total_frequency = N × increment`.

#### Frequency Update (called after every decode)

1. `freq[decoded_index] += increment`
2. `total_frequency += increment`
3. If `total_frequency > limit`:
   - For every *i*: `freq[i] = (freq[i] + 1) >> 1`  (halve, rounding up)
   - Recompute `total_frequency` as the sum of all `freq[i]`.

The halve-with-round-up ensures no frequency ever reaches zero, which would
break the arithmetic coder.  The `increment` and `limit` parameters control
the adaptation rate: a larger `increment` makes the model respond faster to
recent symbols; a lower `limit` forces more frequent rescaling, keeping the
total frequency small (which improves arithmetic coder precision).

### 4.2  Decoder State

The arithmetic decoder maintains a `range` and a `code` register:

| Variable | Bits | Initial Value |
|----------|------|---------------|
| `range` | — | 2^25 (= `1 << 25`) |
| `code` | 26 | Read 26 raw bits from the bitstream (MSB-first) |

**Constants:**

| Name | Value | Meaning |
|------|-------|---------|
| PRECISION | 26 | Total bit-width of the `code` register |
| RANGE_ONE | 2^25 | Maximum range value |
| RANGE_HALF | 2^24 | Renormalization threshold |

Note that bootstrapping the decoder requires reading 26 raw bits, which
exceeds the 25-bit single-read limit; use the "read-long" path from §3.1.

### 4.3  Decoding One Symbol

Given a probability model *M*, decode one symbol as follows:

1. **Scale:** `scale = range / M.total_frequency`.  
   If `scale == 0` → fatal error.

2. **Target:** `target = code / scale`.

3. **Symbol search:** Walk symbols in order (index 0, 1, …), accumulating
   `cumulative`.  Stop at index *k* when `cumulative + freq[k] > target`.  
   (If no such *k* is found before the last symbol, select the last symbol.)

4. Let `sym_lo = cumulative`, `sym_hi = cumulative + freq[k]`.

5. **Narrow interval:**
   - `code -= scale × sym_lo`
   - If `sym_hi == M.total_frequency`:  
     `range -= scale × sym_lo`
   - Otherwise:  
     `range = freq[k] × scale`

6. **Renormalize:** While `range ≤ RANGE_HALF`:
   ```
   range <<= 1
   code = (code << 1) | next_raw_bit()
   ```

7. Update model frequencies for index *k* (§4.1).

8. Return the decoded symbol value (`first_symbol + k`).

Step 5 deserves explanation: the special case for the *last* symbol in the
distribution (`sym_hi == total_frequency`) avoids a precision loss that would
occur if we computed `freq[k] × scale` — the last symbol's range extends to
the top of the interval, so we compute it as the remainder.

### 4.4  Decoding a Multi-Bit Field

To decode an *n*-bit integer from a binary model *M* (one with only symbols
0 and 1), decode *n* individual binary symbols and reassemble in LSB-first
order (see §3.2 for the assembly formula).

---

## 5  Stream Layout

An Arsenic compressed stream consists of a **header** followed by zero or more
**blocks**.  Each block contains a chunk of independently decompressible data.

### 5.1  Stream Header

All header fields are decoded through the **primary model**, a binary
probability model (symbols {0, 1}) with increment = 1 and limit = 256.

The primary model is initialized once at stream start and **never reset** —
it persists across all blocks.  This means that the header decoding and all
block header/footer decoding share the same continuously-adapting model,
which provides a small compression benefit for the metadata fields.

| # | Field | Width | Description |
|---|-------|-------|-------------|
| 1 | Signature byte 0 | 8-bit field (§4.4) | Must equal `0x41` (`'A'`). |
| 2 | Signature byte 1 | 8-bit field (§4.4) | Must equal `0x73` (`'s'`). |
| 3 | Block-size exponent *B* | 4-bit field (§4.4) | Valid range: 0 ≤ B ≤ 15.  `block_size = 1 << (B + 9)`. |
| 4 | Initial end-of-stream flag | 1 symbol | If 1, the stream is empty (no blocks follow). |

**Block sizes** range from 2^9 = 512 bytes (B = 0) to 2^24 = 16,777,216
bytes (B = 15).  Compared to bzip2's fixed increments of 100,000 bytes
(100k to 900k), Arsenic supports both far smaller and far larger blocks.

### 5.2  Block Structure

Blocks are decoded sequentially.  Each block consists of three parts:

```
┌─────────────────┐
│   Block Header   │  ← Decoded via primary model
├─────────────────┤
│   Block Data     │  ← Decoded via per-block models
├─────────────────┤
│   Block Footer   │  ← Models reset, then decoded via primary model
└─────────────────┘
```

#### 5.2.1  Block Header

The block header is decoded using the **primary model** (same one from §5.1):

| Field | Width | Description |
|-------|-------|-------------|
| Randomization flag | 1 symbol | 1 = randomization is enabled for this block (see §9). |
| BWT primary index | (B + 9)-bit field (§4.4) | Starting index for the inverse BWT.  Must be < decoded block length for non-empty blocks. |

#### 5.2.2  Block Data

At the start of each block's data section, the following **per-block** models
are freshly initialized:

| Model | Symbol Range | Increment | Limit |
|-------|-------------|-----------|-------|
| Selector | 0 … 10 | 8 | 1024 |
| MTF group 0 | 2 … 3 | 8 | 1024 |
| MTF group 1 | 4 … 7 | 4 | 1024 |
| MTF group 2 | 8 … 15 | 4 | 1024 |
| MTF group 3 | 16 … 31 | 4 | 1024 |
| MTF group 4 | 32 … 63 | 2 | 1024 |
| MTF group 5 | 64 … 127 | 2 | 1024 |
| MTF group 6 | 128 … 255 | 1 | 1024 |

A 256-entry **MTF table** (identity permutation: `T[i] = i`) is also
initialized per block.

The rationale for multiple MTF group models with varying increments is that
small MTF indices (which represent recently-seen or frequently-seen bytes)
occur much more often than large ones after BWT + MTF processing.  By
assigning larger adaptation steps (`increment`) to the models for small
indices, the coder adapts faster where it matters most.

The data section is a sequence of selector-coded symbols decoded in a loop
until the end-of-block marker is reached.  This process is detailed in §6.

#### 5.2.3  Block Footer

Immediately after the end-of-block marker (selector = 10), the **selector
model** and all seven **MTF group models** are reset to their initial state
(same parameters, all frequencies back to `increment`).

Then, from the **primary model**:

| Field | Width | Description |
|-------|-------|-------------|
| End-of-stream flag | 1 symbol | If 1, this was the final block. |
| CRC (conditional) | 32-bit field (§4.4) | Present **only** when end-of-stream = 1.  The CRC value is read and **discarded** (decompressors do not verify it). |

If the end-of-stream flag is 0, the decoder proceeds to the next block
(back to §5.2.1).

---

## 6  Block Data Decoding

The block data section fills a byte buffer of up to `block_size` bytes.  This
section interleaves three conceptual stages: selector decoding, zero run-length
expansion, and Move-To-Front inversion.

### 6.1  The Selector

The selector model (symbol range 0 … 10) is the "dispatch" mechanism.  Each
decoded selector symbol determines what happens next:

| Selector Value | Meaning |
|----------------|---------|
| 0 or 1 | **Zero run-length token** — enter or continue the zero-run sub-loop (§6.2).  These encode runs of the byte at MTF position 0. |
| 2 | **MTF index 1** — a literal; no sub-decode needed.  Pass index 1 through the MTF table (§6.3) and append the result to the block buffer. |
| 3 … 9 | **Group-coded MTF index** — decode one additional symbol from MTF group model (selector − 3).  The decoded value is the MTF index.  Pass it through the MTF table and append the result. |
| 10 | **End-of-block marker** — stop decoding data for this block. |

Note that selector value 2 maps to MTF index 1, not index 2.  MTF index 0 is
never coded directly — it is always implicit in the zero-run mechanism
(selector values 0 and 1).

The main decode loop structure is:

```
sel = decode_symbol(selector_model)
while sel ≠ 10:
    if sel < 2:
        handle zero-run (§6.2), which consumes more selectors internally
        sel = (the first non-run selector that terminated the run)
        continue
    else:
        idx = (sel == 2) ? 1 : decode_symbol(mtf_group[sel - 3])
        byte = mtf_decode(idx)
        append byte to block buffer
        sel = decode_symbol(selector_model)
```

### 6.2  Zero Run-Length Decoding

When a selector value of 0 or 1 is encountered, a zero-run sub-loop begins.
This encodes a run of repeated copies of whichever byte is currently at
MTF position 0.

**Why this exists:**  After MTF coding, the most common symbol is 0 (the
byte that was just seen is seen again).  Encoding each zero individually would
waste bandwidth.  Instead, consecutive occurrences of the MTF-0 symbol are
collected and encoded as a single run length using a bijective binary encoding.
bzip2 uses the same scheme with its "RUNA" and "RUNB" symbols.

#### State variables (local to each run)

* `weight = 1`
* `count = 0`

#### Accumulation loop

Starting with the selector *s* that triggered entry into the sub-loop:

1. If *s* = 0: `count += weight`.
2. If *s* = 1: `count += 2 × weight`.
3. `weight × = 2`.
4. Decode next selector *s*.
5. If *s* < 2: go to step 1.

After the loop, the selector *s* ≥ 2 that terminated the loop is **not
discarded** — it remains the "current selector" for the main loop and will be
processed normally (as an end-of-block marker if 10, or as a symbol decode
otherwise).

#### Equivalent shift-based formulation

The weight-doubling loop is mathematically equivalent to:

```
total = 0
bit_pos = 0
tok = first_selector          // 0 or 1
loop:
    total += (tok + 1) << bit_pos
    bit_pos += 1
    tok = decode_selector()
    if tok < 2: goto loop
```

Here `(tok + 1) << bit_pos` produces the same contribution as
`(1 + tok) × weight` since `weight = 2^bit_pos`.  Implementations may use
either form.

#### Output from the run

Look up MTF position 0.  This counts as one MTF decode of index 0 (updating
the MTF table once — moving the byte at position 0 to position 0, which is
a no-op for the table but is logically consistent).  Write that byte value
`count` times into the block buffer.

**Overflow check:** `current_buffer_length + count` must not exceed
`block_size`.

#### Example

Consider this sequence of selectors: 1, 0, 1, 3, ...

| Step | Selector | Contribution | Running total |
|------|----------|-------------|---------------|
| bit_pos=0 | 1 | (1+1) << 0 = 2 | 2 |
| bit_pos=1 | 0 | (0+1) << 1 = 2 | 4 |
| bit_pos=2 | 1 | (1+1) << 2 = 8 | 12 |
| — | 3 | (terminates run) | — |

The run length is 12.  The byte at MTF position 0 is written 12 times, then
selector 3 is processed as a group-coded symbol.

### 6.3  Move-To-Front Decode

A 256-entry table *T* is initialized to the identity permutation: `T[i] = i`.

To decode MTF index *k* (0 ≤ k ≤ 255):

1. `value = T[k]`.
2. Shift entries `T[0] … T[k−1]` one position to the right (toward higher
   indices), making room at position 0.
3. `T[0] = value`.
4. Return `value`.

In other words: extract the byte at position *k*, move it to the front of the
list, and return it.  When *k* = 0, the table is unchanged (the front element
stays at the front).

---

## 7  Inverse Burrows–Wheeler Transform

After the entire block data section has been decoded into a byte buffer of
length *L*, the inverse BWT reconstructs the original (pre-BWT) data.

### 7.1  Background

The Burrows–Wheeler Transform works by forming all cyclic rotations of the
input block, sorting them lexicographically, and taking the last column of the
sorted matrix.  This last column, together with the row number where the
original string appears (the "primary index"), is the BWT output.

The inverse transform exploits a key property: the first column of the sorted
matrix is just the sorted version of the last column, and there is a stable
one-to-one mapping between positions in the last column and positions in the
first column (the "LF-mapping").  By following this mapping starting from the
primary index, we can reconstruct the original data one byte at a time.

### 7.2  Build the LF-Mapping Table

Given the decoded block buffer of length *L*:

1. **Count frequencies:** Count the frequency of each byte value 0 … 255 in
   the buffer.

2. **Compute cumulative counts:** `C[v]` = number of bytes in the buffer that
   are strictly less than *v*.  (This is the standard prefix-sum /
   exclusive-scan of the frequency array.)

3. **Build the mapping table:** For each position *i* (0 … L−1), let
   *b* = buffer[*i*].  Set `T[C[b] + rank_b] = i`, where `rank_b` is the
   number of times byte *b* has been seen so far (starting from 0).  Increment
   `rank_b`.

This produces a permutation table *T* of *L* entries.

### 7.3  Reconstruct Original Bytes

Starting index `idx` = BWT primary index (from the block header, §5.2.1).

For each output byte:

1. `idx = T[idx]`
2. Output byte = `buffer[idx]`

**Bounds check:** `idx` must remain in `[0, L)` at every step.

The output order is: the first byte output by this process is the first byte
of the original uncompressed block, and so on.

---

## 8  Final Run-Length Expansion

The encoder applied a byte-stuffing RLE as the **first** compression step
(before BWT).  Decompression therefore applies this expansion **last**, after
all other stages.

### 8.1  Encoding Rule (for reference)

Any run of 4 or more identical bytes *B* is encoded as exactly 4 literal
copies of *B* followed by a count byte *K* (0 ≤ K ≤ 255).  The total run
length represented is `4 + K`.  Runs of exactly 1, 2, or 3 identical bytes are
stored literally (no count byte follows).

This is identical to bzip2's "run-length encoding #1" — the same scheme, the
same threshold of 4, the same maximum extension of 255 additional copies.

### 8.2  Decoder State

Reset at each block boundary:

* `last_byte = 0` — the most recently emitted byte value.
* `run_count = 0` — how many consecutive identical bytes have been seen
  (0 through 4).
* `pending = 0` — remaining repeat bytes to emit from an extension count.

### 8.3  Decoder Algorithm

For each output byte requested:

1. **Drain pending repeats:** If `pending > 0`, emit `last_byte`, decrement
   `pending`, and return.

2. **Fetch upstream byte:** Obtain the next byte *b* from the previous stage
   (inverse BWT + randomization).

3. **Extension count handling:** If `run_count == 4`:
   - Reset `run_count = 0`.
   - Interpret *b* as the extension count *K*.
   - If `K == 0`: this byte produces no output (the run was exactly 4 bytes,
     already emitted one-by-one).  **Go back to step 1** to fetch another
     upstream byte.
   - If `K > 0`: set `pending = K − 1`, emit `last_byte`, and return.

4. **Normal byte:** Otherwise:
   - If *b* equals `last_byte`: increment `run_count`.
   - If *b* differs from `last_byte`: set `run_count = 1`, `last_byte = b`.
   - Emit *b*.

**Important note on K = 0:**  When the extension count is zero, the run was
exactly four bytes long and those four were already emitted individually
(one per call through step 4).  The extension byte `0x00` is consumed and
discarded — it is **not** re-interpreted as a data byte.  The decoder must
loop back to fetch a fresh upstream byte.

---

## 9  Randomization

This stage is only active when the block header's **randomization flag** is
set (§5.2.1).  It applies a deterministic XOR pattern to break up pathological
BWT inputs.

### 9.1  Background

The BWT's worst-case behavior occurs on certain repetitive inputs.  To
mitigate this, the encoder can optionally flip specific bits in the pre-BWT
data at positions determined by a fixed table.  The decoder must undo these
flips.

This is exactly the same mechanism used by bzip2 (though bzip2 deprecated it
in later versions).  The randomization table values are identical.

### 9.2  Algorithm

**Per-block state** (reset at each block boundary):

* `tbl_idx = 0`
* `next_pos = RAND_TABLE[0]`

For each byte at position *pos* (0-indexed within the block output, i.e.,
the output of the inverse BWT):

* If `pos == next_pos`:
  1. XOR the byte with 1 (flip the least-significant bit).
  2. `tbl_idx = (tbl_idx + 1) & 0xFF`
  3. `next_pos += RAND_TABLE[tbl_idx]`

This stage is applied **after** the inverse BWT (§7) and **before** the final
RLE expansion (§8).

### 9.3  Randomization Table

256 entries of unsigned 16-bit integers, identical to the bzip2 randomization
table:

```
  0xEE, 0x56, 0xF8, 0xC3, 0x9D, 0x9F, 0xAE, 0x2C,
  0xAD, 0xCD, 0x24, 0x9D, 0xA6,0x101, 0x18, 0xB9,
  0xA1, 0x82, 0x75, 0xE9, 0x9F, 0x55, 0x66, 0x6A,
  0x86, 0x71, 0xDC, 0x84, 0x56, 0x96, 0x56, 0xA1,
  0x84, 0x78, 0xB7, 0x32, 0x6A, 0x03, 0xE3, 0x02,
  0x11,0x101, 0x08, 0x44, 0x83,0x100, 0x43, 0xE3,
  0x1C, 0xF0, 0x86, 0x6A, 0x6B, 0x0F, 0x03, 0x2D,
  0x86, 0x17, 0x7B, 0x10, 0xF6, 0x80, 0x78, 0x7A,
  0xA1, 0xE1, 0xEF, 0x8C, 0xF6, 0x87, 0x4B, 0xA7,
  0xE2, 0x77, 0xFA, 0xB8, 0x81, 0xEE, 0x77, 0xC0,
  0x9D, 0x29, 0x20, 0x27, 0x71, 0x12, 0xE0, 0x6B,
  0xD1, 0x7C, 0x0A, 0x89, 0x7D, 0x87, 0xC4,0x101,
  0xC1, 0x31, 0xAF, 0x38, 0x03, 0x68, 0x1B, 0x76,
  0x79, 0x3F, 0xDB, 0xC7, 0x1B, 0x36, 0x7B, 0xE2,
  0x63, 0x81, 0xEE, 0x0C, 0x63, 0x8B, 0x78, 0x38,
  0x97, 0x9B, 0xD7, 0x8F, 0xDD, 0xF2, 0xA3, 0x77,
  0x8C, 0xC3, 0x39, 0x20, 0xB3, 0x12, 0x11, 0x0E,
  0x17, 0x42, 0x80, 0x2C, 0xC4, 0x92, 0x59, 0xC8,
  0xDB, 0x40, 0x76, 0x64, 0xB4, 0x55, 0x1A, 0x9E,
  0xFE, 0x5F, 0x06, 0x3C, 0x41, 0xEF, 0xD4, 0xAA,
  0x98, 0x29, 0xCD, 0x1F, 0x02, 0xA8, 0x87, 0xD2,
  0xA0, 0x93, 0x98, 0xEF, 0x0C, 0x43, 0xED, 0x9D,
  0xC2, 0xEB, 0x81, 0xE9, 0x64, 0x23, 0x68, 0x1E,
  0x25, 0x57, 0xDE, 0x9A, 0xCF, 0x7F, 0xE5, 0xBA,
  0x41, 0xEA, 0xEA, 0x36, 0x1A, 0x28, 0x79, 0x20,
  0x5E, 0x18, 0x4E, 0x7C, 0x8E, 0x58, 0x7A, 0xEF,
  0x91, 0x02, 0x93, 0xBB, 0x56, 0xA1, 0x49, 0x1B,
  0x79, 0x92, 0xF3, 0x58, 0x4F, 0x52, 0x9C, 0x02,
  0x77, 0xAF, 0x2A, 0x8F, 0x49, 0xD0, 0x99, 0x4D,
  0x98,0x101, 0x60, 0x93,0x100, 0x75, 0x31, 0xCE,
  0x49, 0x20, 0x56, 0x57, 0xE2, 0xF5, 0x26, 0x2B,
  0x8A, 0xBF, 0xDE, 0xD0, 0x83, 0x34, 0xF4, 0x17
```

---

## 10  Error Conditions

A conforming decompressor must treat the following as **fatal errors**:

* Signature mismatch (first two decoded bytes are not `'A'`, `'s'`).
* Bitstream exhaustion during any required bit read.
* Block-size exponent *B* that would cause `block_size` to overflow the
  implementation's integer type.
* `total_frequency` of any model reaching zero.
* `scale` (range / total_frequency) reaching zero during symbol decode.
* BWT primary index ≥ block length (for non-empty blocks).
* Block buffer overflow (decoded byte count exceeding `block_size`).
* BWT reconstruction index going out of bounds.
* Requesting output bytes beyond the end of the stream.

All of these conditions are non-recoverable.

---

## 11  Implementation Guidance

This section provides practical advice for implementers.  It is not part of
the format specification per se, but captures lessons learned from existing
implementations.

### 11.1  Error Recovery with setjmp/longjmp

Because errors can be detected deep inside nested function calls (arithmetic
decoder → bitstream refill → underflow), propagating error codes up through
every return path is tedious and error-prone.  A practical approach in C is to
use `setjmp`/`longjmp`:

- Place a `setjmp` at each public API entry point.
- Define an `abort` helper that calls `longjmp` to unwind to the nearest
  entry point.
- Every internal function that detects an error calls `abort` instead of
  returning an error code.

This keeps the hot path (the normal decode loop) free of error-checking
boilerplate.

### 11.2  Memory Allocation

Each decoded block requires two buffers:

| Buffer | Element Size | Count | Purpose |
|--------|-------------|-------|---------|
| Block data | 1 byte | `block_size` | Decoded MTF output (input to inverse BWT) |
| LF-mapping | 4 bytes (`uint32_t`) | `block_size` | Inverse-BWT permutation table |

Total allocation per block = **5 × block_size** bytes.

With the maximum exponent B = 15, `block_size = 2^24 = 16,777,216`, giving a
peak allocation of **80 MiB** for block buffers alone (plus the decoder state
struct).  Implementations should verify that `1 << (B + 9)` does not overflow
their integer type before allocating.

Since blocks are decoded sequentially, a single pair of buffers can be reused
for every block without reallocation.

### 11.3  Demand-Driven (Pull-Based) Block Decoding

Blocks should be decoded lazily — a new block is decoded only when the
previous block's output is fully consumed.  The output loop follows this
pattern:

1. If the final RLE stage still has buffered repeat bytes, emit from those.
2. If the current block is exhausted (`out_pos ≥ blk_len`):
   - If the end-of-stream flag is set, signal end-of-data (or error if the
     caller requested more bytes).
   - Otherwise, decode the next block (§5.2, §6, §7).
3. Emit one byte through the BWT → randomization → final-RLE pipeline.

This pull-based pattern means block buffers can be reused across blocks
without reallocation, and the caller controls how many output bytes are
produced per call.

### 11.4  Public Decoder API

Two API styles are practical:

**One-shot** (`sit15_decompress`): Takes source buffer, destination buffer,
and lengths.  Allocates state internally, decodes the full stream into the
destination, frees state, and returns the number of bytes produced.  Returns 0
on any error.

**Streaming** (`sit15_init` / `sit15_read` / `sit15_free`):

* `sit15_init(src, src_len)` — allocate state, parse the stream header (§5.1),
  allocate block buffers.  Returns an opaque context handle, or NULL on error.
* `sit15_read(ctx, out, cap)` — decode up to `cap` bytes into `out`.  Returns
  the number of bytes actually produced (0 at end-of-stream, negative on
  error).  The three-way end-of-stream condition is:
  `eos_flag && out_pos ≥ blk_len && rle_repeat == 0` — all three must hold
  before the stream is considered fully drained.
* `sit15_free(ctx)` — release block buffers and state.

Both APIs should set up a `setjmp` trap at their entry point so that any
internal error (§10, §11.1) safely unwinds to the caller.

### 11.5  Additional Notes

* **K = 0 in final RLE (§8):** When the extension count is zero, the run was
  exactly four bytes and those four were already emitted one-by-one.  The
  extension byte is consumed and discarded; the decoder loops back to fetch
  a fresh upstream byte.  The byte value 0 is *not* re-interpreted as a
  regular data byte.

* **Streaming context `live` field:** Some implementations carry a boolean
  `live` flag in the opaque context struct, set to `true` at init time.  This
  field is currently unused (dead code) and exists only as a future extension
  point for invalidating the context after a fatal error.

---

## Appendix A: Complete Decompression Walkthrough

This appendix traces the full decompression of a single block, tying together
all the stages described above.

1. **Parse stream header** (§5.1): Initialize the arithmetic decoder (§4.2).
   Set up the primary model.  Decode the `"As"` signature, block-size
   exponent, and initial end-of-stream flag.

2. **Parse block header** (§5.2.1): Using the primary model, decode the
   randomization flag and BWT primary index.

3. **Initialize per-block models** (§5.2.2): Set up the selector model,
   seven MTF group models, and the MTF table.

4. **Decode block data** (§6): Enter the selector loop.  For each selector:
   - 0 or 1 → accumulate a zero-run (§6.2), then fill bytes via MTF[0].
   - 2 → MTF decode index 1.
   - 3–9 → sub-decode from the appropriate MTF group model, then MTF decode.
   - 10 → end of block.

5. **Build LF-mapping** (§7.2): Compute byte frequencies, cumulative counts,
   and the permutation table from the decoded block buffer.

6. **Output bytes**: For each requested output byte:
   a. Follow one step of the LF-mapping (§7.3) to get a post-BWT byte.
   b. Apply randomization XOR if active (§9).
   c. Pass through the final RLE expander (§8).

7. **Parse block footer** (§5.2.3): Reset per-block models.  Decode the
   end-of-stream flag.  If set, decode and discard the 32-bit CRC.

8. If not end-of-stream, go to step 2 for the next block.
