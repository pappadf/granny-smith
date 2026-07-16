# SIT Method 3 ("Static Huffman") — Format Specification

## 1  Introduction

### 1.1  What is Method 3?

StuffIt archives (`.sit`) support multiple compression methods, identified by
a one-byte numeric ID stored in each fork's entry header.  Method 3 is the
oldest entropy-coding option in the format: a **per-byte static Huffman**
encoder.  It predates Lempel–Ziv methods (LZW, LZSS) in the same format and is
algorithmically a strict generalization of the textbook "construct a Huffman
tree from byte frequencies, write the tree, write the codes" exercise.

The method has exactly two on-wire components:

1. A serialized Huffman tree at the head of the compressed stream.
2. A sequence of variable-length codewords, one per input byte, with no
   end-of-stream sentinel.

There is no sliding window, no dictionary, no run-length pre-pass, no
adaptive update, and no escape mechanism.  The compressor has 256 leaf
symbols (one per possible input byte value) and emits exactly one codeword
per input byte.

### 1.2  Status within the format family

Method 3 was the second-introduced compression method (after method 1
"RLE90") and was the default compression for *uncompressed-looking* data in
StuffIt 1.x.  It was supplanted by method 2 ("LZW") in StuffIt 1.5.x as the
new default for general data, but method 3 remained available as an explicit
"Huffman only" choice through every classic StuffIt release (1.x through
4.5) and survives in real-world archives produced by every classic-StuffIt
version.  It is not present in SIT5-container archives — those use method
13 ("LZSS + Huffman") and method 15 ("Arsenic") for their entropy-coded
forks.

Method 3 has not been modified by any vendor since the original 1987
implementation; the format is frozen.  Any archive containing a method-3
fork can be decoded with the algorithm in this document, regardless of which
classic-StuffIt version produced it.

### 1.3  Purpose and Scope

This document is a complete, self-contained *format specification*.  It
describes the on-wire bit-level format and every algorithm needed to build a
fully compatible decompressor for method-3-compressed data.  It is not tied
to any particular implementation.

The specification covers:

- where method-3 data lives within a classic StuffIt archive (cross-reference
  to `sit.md`, but with all field offsets repeated for self-containment);
- the bit and byte order conventions used for the compressed stream;
- the recursive grammar of the serialized Huffman tree;
- the decoding loop;
- termination semantics and trailing-bit handling;
- edge cases (empty fork, single-leaf tree, malformed tree);
- the CRC-16/ARC integrity check applied to the decompressed output;
- worked examples that demonstrate the entire decode pipeline.

---

## 2  Where Method 3 Lives Within a StuffIt Archive

This section is a self-contained restatement of the container framing
needed to locate a method-3 fork.  The same information appears in
`sit.md` §4; it is repeated here so that an implementer who needs only
the method-3 decoder can work from this document alone.

### 2.1  Classic StuffIt Master Header (22 bytes)

The archive begins with a fixed 22-byte master header:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | `magic1` | `"SIT!"` (or one of 8 historical aliases: `ST46`, `ST50`, `ST60`, `ST65`, `STin`, `STi2`, `STi3`, `STi4`). |
| 4 | 2 | `file_count` | Number of top-level entries (UInt16 BE). |
| 6 | 4 | `total_size` | Total archive size in bytes including this header (UInt32 BE). |
| 10 | 4 | `magic2` | `"rLau"` (Raymond Lau signature). |
| 14 | 1 | `version` | `0x01` (StuffIt 1.5.x and earlier) or `0x02` (StuffIt 1.6 through 4.5). |
| 15 | 7 | reserved | Treated as opaque data. |

The master header carries no compression-method information.

### 2.2  Entry Header (112 bytes)

Each entry header begins at the offset given by the iteration rules in
`sit.md` §4.7 (folder context aside, the simplest sequence is "right
after the master header, then right after each entry's combined fork
data").  The entry header is fixed at 112 bytes:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `rsrc_method_raw` | Resource-fork method byte (encoded per §2.3). |
| 1 | 1 | `data_method_raw` | Data-fork method byte (same encoding). |
| 2 | 1 | `name_len` | Filename length, 0–63. |
| 3 | 63 | `name` | Filename in MacRoman, padded with zero bytes. |
| 66 | 4 | `type` | Macintosh file type (4 chars). |
| 70 | 4 | `creator` | Macintosh creator code (4 chars). |
| 74 | 2 | `finder_flags` | Classic Finder flags. |
| 76 | 4 | `create_time` | Creation timestamp (Mac epoch). |
| 80 | 4 | `mod_time` | Modification timestamp (Mac epoch). |
| 84 | 4 | `rsrc_uncomp_len` | Uncompressed resource-fork length (UInt32 BE). |
| 88 | 4 | `data_uncomp_len` | Uncompressed data-fork length (UInt32 BE). |
| 92 | 4 | `rsrc_comp_len` | Compressed resource-fork length (UInt32 BE). |
| 96 | 4 | `data_comp_len` | Compressed data-fork length (UInt32 BE). |
| 100 | 2 | `rsrc_crc` | CRC-16/ARC of the *decompressed* resource fork (UInt16 BE). |
| 102 | 2 | `data_crc` | CRC-16/ARC of the *decompressed* data fork (UInt16 BE). |
| 104 | 6 | reserved | Treated as opaque data. |
| 110 | 2 | `header_crc` | CRC-16/ARC of the preceding 110 bytes (UInt16 BE). |

Immediately following the 112-byte entry header:

```
+--------------------------+
| resource fork compressed | rsrc_comp_len bytes
+--------------------------+
|     data fork compressed | data_comp_len bytes
+--------------------------+
```

The two forks are independent compressed streams, each tagged with its
own method byte.  They may use *different* methods.  The compressed
sizes (`*_comp_len`) tell the decoder how many bytes of compressed input
to consume; the uncompressed sizes (`*_uncomp_len`) tell the decoder how
many output bytes to produce.

### 2.3  Method-Byte Encoding

The 8-bit method byte for each fork is partitioned as:

```
  Bits:   7    6    5    4    3   2   1   0
        +-----+----+----+----+--------------+
        | 0x20/0x21 folder | encr |  method  |
        +------------------+------+----------+
```

- **Bits 0–3 (low nibble)** — Compression method ID, 0–15.  Method 3 is
  the value `0x03` in this nibble.
- **Bit 4 (`0x10`)** — Encryption flag.  If set, the fork is encrypted with
  the StuffIt DES variant and is out of scope for this document.
- **Whole-byte sentinels** — `0x20` (decimal 32) is `startFolder` and
  `0x21` (decimal 33) is `endFolder`.  Either method byte holding one of
  these values means the entry is a structural folder marker, not a
  file; the fork-data area is absent for such entries.  Method 3 only
  applies when the byte is neither a folder sentinel nor has the
  encryption flag set.

A method-3 file fork therefore has `(method_raw & 0x0F) == 3` and
`(method_raw & 0xF0) == 0` (no encryption, not a folder marker).

### 2.4  Locating Method-3 Fork Data

Given an entry-header start offset `H` and the field values above:

```
rsrc_method = entry[0]
data_method = entry[1]
rsrc_data   = entry[112 ..                       112 + rsrc_comp_len)
data_data   = entry[112 + rsrc_comp_len ..       112 + rsrc_comp_len + data_comp_len)
```

If `(rsrc_method & 0x0F) == 3`, the bytes `rsrc_data` are a method-3
compressed stream and must be decoded to exactly `rsrc_uncomp_len` bytes.
Likewise for the data fork.  A method-3 decoder operates *purely on the
compressed-byte slice* — it does not need to know anything about the
container above this layer.

---

## 3  Bit and Byte Ordering

Method 3 has exactly one bit-ordering convention, applied uniformly to
every bit read from the compressed stream (both the tree header and the
codeword stream).

### 3.1  Byte Stream

The compressed stream is the contiguous byte range identified in §2.4.
Bytes are consumed in increasing offset order: byte 0 first, then byte
1, … up to byte `comp_len − 1`.  No byte is ever revisited.

### 3.2  Bit Order Within a Byte

Within each byte, bits are consumed **most-significant first**.  Bit 7
(the `0x80` bit) is the first bit; bit 0 (the `0x01` bit) is the last
bit.

Worked example — the byte `0xB6` (`0b10110110`) yields the bit sequence
**1, 0, 1, 1, 0, 1, 1, 0** when read MSB-first.

### 3.3  Multi-Bit Field Order

When the format calls for an *n*-bit field (always *n* = 8 in this
specification, for leaf symbol values), the bits are read in the same
MSB-first order.  The first bit read is the most-significant bit of the
resulting integer.

Worked example — reading 8 bits starting at bit 4 of the byte stream
`[0x42, 0x53]` (`0b01000010 0b01010011`):

```
byte stream  :  0 1 0 0 0 0 1 0 | 0 1 0 1 0 0 1 1
bit position :  0 1 2 3 4 5 6 7 | 0 1 2 3 4 5 6 7
                        ^ start here, read 8 bits MSB-first
field bits   :          0 0 1 0   0 1 0 1
field value  :  0b00100101 = 0x25
```

The next bit read is the bit at position 12 of byte 1 (i.e., the bit
with weight `0x08` within `0x53`).

### 3.4  No Padding, No Alignment, No End-Marker

The bit stream is one continuous bit string starting at bit 7 of byte 0
of the compressed slice.  There is:

- **No padding** between the tree header and the codeword stream.  The
  first codeword starts at whatever bit position the tree serialization
  ended on, possibly mid-byte.
- **No alignment requirement** at any point.  Codewords are not byte- or
  word-aligned.
- **No end-of-stream symbol.**  The codeword alphabet has exactly the
  symbols that appear in the source (1 to 256 leaves), and no extra
  terminator code.  Decoding stops when exactly `uncomp_len` output
  bytes have been produced.
- **Trailing bits are discarded.**  After the last output byte is
  emitted, any remaining bits in the final byte of the compressed
  slice are ignored.  An encoder may freely pad the final byte with
  any bit pattern (typical encoders pad with zeros).

The `comp_len` field in the entry header is authoritative for the
number of compressed bytes the encoder produced (and therefore for
locating the next entry header), but it is *not* authoritative for the
number of bits the decoder must actually consume.  A correct decoder
runs strictly off `uncomp_len` and ignores `comp_len` after the slice
boundary has been used to bound input availability.

---

## 4  CRC-16/ARC Integrity Check

Every classic StuffIt CRC field uses the same algorithm, identified in
the literature as **CRC-16/ARC** (also called CRC-16/IBM, CRC-16/LHA,
or simply "ARC CRC").  This section documents the algorithm completely.

### 4.1  Parameters

| Parameter | Value |
|---|---|
| Width | 16 bits |
| Polynomial (normal form) | `0x8005` |
| Polynomial (reflected form) | `0xA001` |
| Initial value | `0x0000` |
| Input reflected (`refin`) | true |
| Output reflected (`refout`) | true |
| Final XOR | `0x0000` |
| Check value (CRC of ASCII `"123456789"`) | `0xBB3D` |

The "reflected" parameters mean that the algorithm is most efficiently
expressed with the reflected polynomial `0xA001` shifting **right** one
bit per step.  The check value `0xBB3D` is a standard self-test:
running the algorithm over the nine ASCII bytes `0x31 0x32 … 0x39`
yields `0xBB3D`.

### 4.2  Reference Implementation (Byte-at-a-Time, No Table)

```c
uint16_t crc16_arc(const uint8_t *buf, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001
                            : (crc >> 1);
        }
    }
    return crc;
}
```

### 4.3  Reference Implementation (256-Entry Table)

A standard speed optimization is a 256-entry lookup table, generated as:

```c
uint16_t crc_table[256];
for (int i = 0; i < 256; i++) {
    uint16_t c = (uint16_t)i;
    for (int b = 0; b < 8; b++) {
        c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1);
    }
    crc_table[i] = c;
}
```

Then for each input byte:

```c
crc = crc_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
```

The pre-computed table values for all 256 indices appear in `sit.md`
Appendix A and are reproduced verbatim from there for cross-reference.

### 4.4  Where CRCs Apply for Method 3

Three CRC fields are relevant to a method-3 fork:

| Field | Range | Notes |
|---|---|---|
| `header_crc` (entry header offset 110) | First 110 bytes of the 112-byte entry header | Computed by the encoder before writing the header to disk. |
| `rsrc_crc` (entry header offset 100) | The entire **decompressed** resource fork (`rsrc_uncomp_len` bytes) | Computed *after* method-3 decoding, against the resulting plaintext. |
| `data_crc` (entry header offset 102) | The entire **decompressed** data fork (`data_uncomp_len` bytes) | Same as `rsrc_crc` but for the data fork. |

The compressed bit stream itself is not separately checksummed.  An
incorrect Huffman decoder will produce wrong plaintext, which will fail
the CRC check at the end of fork decoding.  This is the primary
correctness signal for an implementation under test.

### 4.5  Tolerance of Zero CRCs

Some very early StuffIt 1.0/1.1 archives left `header_crc` as `0x0000`
without computing it.  A tolerant decoder may accept `header_crc == 0`
as a "don't-care" value; the fork CRCs (`rsrc_crc`, `data_crc`) are
always written correctly by the original encoder and a strict decoder
should treat any mismatch there as an error.

---

## 5  The Method-3 Compressed Stream

A method-3 compressed slice has two consecutive parts on the bit-stream
level (no separator, no length field between them):

```
+------------------------+--------------------------+
|  Serialized Huffman    |  Codeword stream:        |
|  tree (variable size)  |  one codeword per output |
|                        |  byte, exactly           |
|                        |  uncomp_len codewords    |
+------------------------+--------------------------+
```

### 5.1  Serialized Huffman Tree

The tree is encoded as a **pre-order recursive serialization**.  The
grammar is exactly two productions:

```
node  ::=  '1' literal              -- leaf
        |  '0' node node            -- internal node (zero-subtree, one-subtree)
literal ::= 8 bits, MSB-first       -- the leaf's symbol value
```

Reading order is depth-first, zero-subtree before one-subtree.  Each
leaf occupies exactly 9 bits (1 tag bit + 8 symbol bits).  Each internal
node occupies exactly 1 tag bit and is followed by the encodings of its
two children.

#### 5.1.1  Tag-Bit Convention

- A tag bit of **`1` (one)** means *the current node is a leaf*; the next
  8 bits are the symbol value.
- A tag bit of **`0` (zero)** means *the current node is an internal node*;
  it is followed by two child subtrees, encoded in the same grammar,
  zero-subtree first.

The grammar is unambiguous: the decoder always knows whether the next
bit it sees is a tag bit (it is, every time it begins reading a node)
versus a symbol bit (only the 8 bits following a `1` tag).

#### 5.1.2  Code Assignment

The Huffman codeword for a given leaf is the sequence of bits on the
path from the root to that leaf, where each branch contributes one bit:
**0 for the zero-subtree, 1 for the one-subtree**, matching the tag-bit
convention.

A decoder that walks the tree pointer-wise (one branch per input bit)
never needs to materialize codewords or codeword tables — it just reads
one bit, follows the matching child pointer, and emits the symbol when
it reaches a leaf.  This is the implementation strategy assumed by the
rest of this document.

#### 5.1.3  Worked Example — Tree Serialization

Consider an input that contains only three distinct byte values, with
frequencies sufficient to produce the tree:

```
              (root, internal)
                /         \
               0           1
              /             \
         (internal)      Leaf 'C'  (0x43)
           /     \
          0       1
         /         \
      Leaf 'A'   Leaf 'B'
       (0x41)     (0x42)
```

Codeword assignments:

- `A` (0x41) → bits `00`
- `B` (0x42) → bits `01`
- `C` (0x43) → bits `1`

The serialization of this tree, reading the grammar top-down with
zero-subtree first:

| Step | Bits emitted | What it represents |
|------|--------------|--------------------|
| 1 | `0` | root is internal |
| 2 | `0` | zero-child is internal |
| 3 | `1` | this is a leaf |
| 4 | `01000001` | symbol value 0x41 (`A`) |
| 5 | `1` | this is a leaf |
| 6 | `01000010` | symbol value 0x42 (`B`) |
| 7 | `1` | this is a leaf |
| 8 | `01000011` | symbol value 0x43 (`C`) |

Total: 2 + 9 + 9 + 9 = 29 bits.

Concatenating the bits as a single bit string:

```
0 0 1 0 1 0 0 0 0 0 1 1 0 1 0 0 0 0 1 0 1 0 1 0 0 0 0 1 1
```

Packed into bytes MSB-first (29 bits = 3 bytes and 5 bits, with the
remaining 3 bits filled by the start of the codeword stream below):

```
byte 0 : 0 0 1 0 1 0 0 0   = 0x28
byte 1 : 0 0 1 1 0 1 0 0   = 0x34
byte 2 : 0 0 1 0 1 0 1 0   = 0x2A
byte 3 : 0 0 0 1 1 ? ? ?   = 0x18 | (first 3 codeword bits in low nibble)
```

(The trailing `???` bits depend on the codeword stream that follows
the tree.)

### 5.2  Codeword Stream

Immediately after the tree header (at whatever bit position the tree
serialization happened to end on, which may be mid-byte) comes the
codeword stream: exactly `uncomp_len` codewords, one per output byte,
concatenated with no separator.

A codeword is the bit path from the root to a leaf, as described in
§5.1.2.  Codewords are variable-length; the bit count per codeword
varies between `0` (degenerate single-leaf case, §6.1) and however deep
the tree happens to be (bounded only by the node-pool size; see §6.4).

There is **no end-of-stream codeword** and **no length prefix on
codewords**.  The decoder knows when a codeword ends because it has
reached a leaf in the tree.  The decoder knows when the entire stream
ends because it has emitted `uncomp_len` output bytes.

#### 5.2.1  Worked Example — Codeword Stream

The tree in §5.1.3 was a *different* shape (a left-leaning tree with
codes `A=00`, `B=01`, `C=1`); §8 below uses a different example tree
and walks the full encode/decode end to end with concrete bit-exact
byte values.  Refer to §8 for the integrated worked example.

---

## 6  Decoding Procedure

### 6.1  Overall Algorithm

```
function decode_method3(compressed_bytes, uncomp_len):
    bits = bitreader(compressed_bytes)         # MSB-first, §3
    tree_root = read_tree(bits)                # §5.1, §6.2
    output = empty_buffer(uncomp_len)
    for i in 0 .. uncomp_len - 1:
        output[i] = decode_one_byte(bits, tree_root)   # §6.3
    return output
    # Any trailing bits in the final byte of compressed_bytes are
    # discarded.  The decoder does not consume them.
```

### 6.2  Tree Reader

A recursive descent matching the grammar of §5.1:

```
function read_tree(bits):
    tag = bits.read_bit()
    if tag == 1:
        symbol = bits.read_bits(8)             # MSB-first
        return Leaf(symbol)
    else:
        zero_child = read_tree(bits)           # zero-subtree first
        one_child  = read_tree(bits)
        return Internal(zero_child, one_child)
```

An iterative implementation is straightforward but the recursive form
matches the grammar 1:1 and is the most common in practice.  Recursion
depth is bounded by tree depth, which is bounded by node-pool size
(§6.4); a 1-leaf tree is depth 0 and a 256-leaf maximally-unbalanced
tree is depth 255.  An implementation may want to either (a) verify
maximum recursion depth, or (b) use an iterative tree reader, depending
on the host environment.

### 6.3  Single-Byte Decode Step

```
function decode_one_byte(bits, root):
    node = root
    while node is internal:
        b = bits.read_bit()
        node = node.one if b == 1 else node.zero
    return node.symbol
```

The walk consumes **one bit per level descended**.  Reaching a leaf
ends the codeword; the next codeword begins with the next bit read.

### 6.4  Node Pool Sizing

A full binary tree with up to 256 leaves has at most 2·256 − 1 = 511
total nodes.  Implementations typically pre-allocate a fixed pool of
512 entries (the upstream `unsit.c` decoder does exactly this) and
treat pool exhaustion as a corrupt-archive error.  Dynamic allocation
is also fine; the format places no upper bound on tree size beyond the
mathematical limit of 511 nodes for 256 distinct symbols.

If an implementation reads a tree header that would require more than
511 nodes, the input is corrupt and decoding must abort.

### 6.5  Stopping Rule

Decoding stops the moment `output` reaches `uncomp_len` bytes.  At this
point:

- The decoder may still be mid-byte in the compressed stream.  That is
  expected; trailing bits within the final compressed byte are
  discarded.
- The decoder must **not** read any further bits beyond what was
  necessary to produce the final output byte.  The output-byte
  counter, not the input-byte counter, is the stopping rule.
- The next entry header (in the surrounding archive) begins
  `rsrc_comp_len + data_comp_len` bytes after the current entry header
  — regardless of how many bits the decoder actually consumed from
  this fork's compressed slice.  `comp_len` is authoritative for next-
  entry positioning.

---

## 7  Edge Cases

This section enumerates every legal-but-unusual input shape that a
correct decoder must handle.  All of these occur in real-world archives
and a decoder that ignores any of them will fail on some inputs.

### 7.1  Empty Fork (`uncomp_len == 0`)

When `uncomp_len` is zero, the decoder must emit zero output bytes and
consume zero bits of input.  In particular:

- The decoder must **not** read the tree header — there is none.  The
  encoder may not even have written one.
- `comp_len` may be 0 or may be small-but-nonzero (the encoder is
  permitted to record an empty Huffman stream, including a degenerate
  tree, even though emitting one is unnecessary).  A correct decoder
  accepts both.

The output is the empty buffer.  CRC over zero bytes is `0x0000`; the
entry header's `*_crc` field should match.

### 7.2  Single-Leaf Tree (1 distinct symbol)

If the input consists of `N > 0` copies of a single byte value `S`, the
encoder may emit a Huffman tree consisting of just one leaf:

```
+-------------+
|  Leaf S     |
+-------------+
```

Serialized form: just `1` followed by the 8 bits of `S` — **9 bits
total**.  The codeword for `S` is the empty bit string.

Decoding this tree means:

- `tree_root` is a leaf.
- Every iteration of the decode loop reaches the leaf without reading
  any bit, so `output[i] = S` for `i = 0 .. uncomp_len-1`.
- The total compressed-bit consumption is exactly 9 bits (just the tree
  header).

An implementation that unconditionally enters the inner "while node is
internal" loop with a `read_bit()` call will incorrectly read random
bits from the codeword-stream area (or run off the end if the slice is
short).  A correct decoder must check whether `node` is already a leaf
before reading a bit; the algorithm in §6.3 does this naturally because
the `while` condition is checked first.

### 7.3  Trees Smaller Than 256 Symbols

The serialized tree need not contain all 256 possible byte values.  It
contains exactly the symbols that appeared in the encoder's input.  An
input consisting of only printable ASCII will produce a tree with
roughly 90 leaves, a tree of only the bytes 0x00 and 0xFF will have 2
leaves, etc.  Decoders must not assume a fixed leaf count.

### 7.4  Very Deep Trees

Method 3 places no upper bound on Huffman code length.  In principle a
256-leaf maximally-unbalanced tree (the worst case for an encoder that
naively follows the priority-queue Huffman construction) has codewords
of up to 255 bits.  Such trees are pathological — they only arise for
inputs with frequency distributions matching the Fibonacci sequence —
but they are legal.

A pointer-walking decoder (§6.3) is intrinsically safe at any depth
because it does not need to materialize codewords into fixed-width
registers.  A table-driven decoder that assumes ≤ 16-bit codewords will
fail on such inputs and is not conformant.

### 7.5  Short Compressed Slice

The decoder must detect attempts to read past the end of the compressed
slice and abort with a clear error rather than reading uninitialized
memory.  Two failure modes:

- The tree header itself runs past the end of the slice (extremely
  short or truncated archive).
- The codeword stream runs out before `uncomp_len` output bytes are
  produced (corrupted archive).

Both cases indicate a malformed archive.  The CRC-16/ARC check (§4) is
not the right defense here — by the time it runs, the decoder has
already read garbage.

### 7.6  Malformed Tree Pointers

The tree reader in §6.2 is exhaustive: every recursive call returns
either a leaf or an internal node with two valid child pointers.  A
defensive implementation may still want to assert that internal-node
children are non-null before walking them; this guards against pool
exhaustion bugs in the implementation itself rather than against
hostile inputs (the format itself cannot produce a null child pointer
from the grammar in §5.1).

### 7.7  Encrypted Method-3 Forks

A fork whose method byte has bit 4 set (`method_raw == 0x13`) is
encrypted with the StuffIt DES variant.  The encryption is applied
*around* the Huffman stream: the DES decryption produces the plaintext
Huffman stream, which is then fed to the method-3 decoder.

Encrypted entries are out of scope for this document.  An
implementation that does not support StuffIt encryption must refuse to
extract such entries rather than silently producing garbage.

### 7.8  Mixed-Method Archives

Within a single archive, different entries may use different methods,
and the two forks of a single entry may use different methods (e.g.,
resource fork with method 3, data fork with method 2).  The fork-level
method byte is authoritative; the decoder must dispatch on each fork's
method byte independently.

---

## 8  Worked End-to-End Example

This section walks through a complete encode-and-decode cycle on a tiny
input, so an implementer can spot-check their decoder against a known-
good bit stream.

### 8.1  Input

The 4-byte input:

```
'A' 'C' 'A' 'B'     i.e., bytes 0x41 0x43 0x41 0x42
```

`uncomp_len = 4`.

### 8.2  Frequency Counts and Tree Construction

Frequency table:

| Symbol | Count |
|---|---|
| `A` (0x41) | 2 |
| `B` (0x42) | 1 |
| `C` (0x43) | 1 |

Huffman tree construction (standard pair-the-two-rarest priority queue):

1. Start with three trees: `A:2`, `B:1`, `C:1`.
2. Pair the two rarest (`B:1`, `C:1`) into an internal node of total
   frequency 2.  Assign `B` as zero-child and `C` as one-child (the
   choice within an equal-frequency pair is encoder-specific and the
   format encodes the choice explicitly, so the decoder does not need
   to know the rule).
3. Pair `A:2` with the new `BC:2` node into a root of frequency 4.
   Assign `A` as zero-child and `BC` as one-child (again, encoder's
   choice).

Resulting tree:

```
                (root)
                /    \
              0       1
              |       |
            'A'    (internal)
                    /    \
                  0       1
                  |       |
                 'B'     'C'
```

Codewords:

- `A` → `0`
- `B` → `10`
- `C` → `11`

### 8.3  Tree Serialization

Pre-order, zero-subtree first:

| Step | Bit emitted | Note |
|---|---|---|
| 1 | `0` | root is internal |
| 2 | `1` | zero-child is leaf |
| 3 | `01000001` | symbol `A` (0x41) |
| 4 | `0` | one-child of root is internal |
| 5 | `1` | zero-grandchild is leaf |
| 6 | `01000010` | symbol `B` (0x42) |
| 7 | `1` | one-grandchild is leaf |
| 8 | `01000011` | symbol `C` (0x43) |

Tree bits, concatenated: `0 1 01000001 0 1 01000010 1 01000011`
→ `01010000010101000010101000011`  (29 bits).

### 8.4  Codeword Stream

`ACAB` → `0 11 0 10` → `011010` (6 bits).

### 8.5  Full Compressed Bit String

```
tree (29 bits)                  codewords (6 bits)
01010000010101000010101000011  011010
```

Total = 35 bits.

### 8.6  Packed Bytes (MSB-First, Encoder Pads Final Byte With Zeros)

Splitting 35 bits into bytes MSB-first, padding the final byte with
zero bits:

```
position    : 0  1  2  3  4  5  6  7
byte 0 bits : 0  1  0  1  0  0  0  0      = 0x50
byte 1 bits : 0  1  0  1  0  1  0  0      = 0x54
byte 2 bits : 0  0  1  0  1  0  1  0      = 0x2A
byte 3 bits : 0  0  0  1  1  0  1  1      = 0x1B
byte 4 bits : 0  1  0  0  0  0  0  0      = 0x40
              ^---^-- last 3 bits are codeword stream
                     followed by 5 zero pad bits
```

So the on-wire compressed slice (5 bytes) is:

```
0x50 0x54 0x2A 0x1B 0x40
```

`comp_len = 5`.  CRC-16/ARC of the 4 input bytes `"ACAB"` is `0x8955`
(this is the value the encoder writes into `data_crc`).

### 8.7  Decoding the Slice

Bit position 0 of the compressed slice is bit 7 of byte 0.  The decoder
proceeds:

```
read_bit() → 0   (root is internal; recurse into zero-child)
  read_bit() → 1   (leaf!)
  read_bits(8) → 0x41  (= 'A')
  return Leaf('A')
read_bit() → 0   (root's one-child is internal)
  read_bit() → 1   (leaf!)
  read_bits(8) → 0x42  (= 'B')
  return Leaf('B')
  read_bit() → 1   (leaf!)
  read_bits(8) → 0x43  (= 'C')
  return Leaf('C')
return Internal(zero='B', one='C')
return Internal(zero='A', one=<above>)
```

Tree is built; 29 bits consumed.  Bit cursor now at bit position 29
(= bit 5 of byte 3 in MSB-first numbering).  Begin codeword stream:

```
output[0]:
  node = root
  read_bit() → 0  → node = Leaf('A')
  emit 'A'

output[1]:
  node = root
  read_bit() → 1  → node = Internal(B, C)
  read_bit() → 1  → node = Leaf('C')
  emit 'C'

output[2]:
  node = root
  read_bit() → 0  → node = Leaf('A')
  emit 'A'

output[3]:
  node = root
  read_bit() → 1  → node = Internal(B, C)
  read_bit() → 0  → node = Leaf('B')
  emit 'B'
```

Output: `A C A B`.  4 output bytes produced.  Decoding stops.  Bit
cursor is at bit position 35 (= bit 3 of byte 4 in MSB-first
numbering); the remaining 5 bits of byte 4 are discarded.

CRC of the output `"ACAB"` is `0x8955`, matching the encoder's
`data_crc`.  Extraction succeeds.

### 8.8  Property Check

This worked example, while tiny, exercises every required mechanism:

- MSB-first bit reading.
- Tree-grammar recursion with both leaf and internal-node cases.
- Multi-leaf tree (not single-leaf).
- Codeword lengths of 1, 2, 2 (not all the same).
- Mid-byte transition from tree-header bits to codeword bits.
- Stopping by output count, not input bit count.
- Discarded trailing bits in the final compressed byte.
- CRC-16/ARC matching against the entry header.

An implementation that produces `"ACAB"` from `[0x50, 0x54, 0x2A, 0x1B,
0x40]` with `uncomp_len = 4` has correctly implemented the entire
method-3 decoder.

---

## 9  Implementation Notes

This section is non-normative; it captures practical advice that helped
the reference implementation in `sit3.c` come out correct and fast, but
is not a requirement of the format.

### 9.1  Single-Pass Bit Reader

A bit reader that buffers one byte at a time and counts a "next bit
index within that byte" variable is the simplest design and matches the
spec's "MSB within byte" rule directly.  The `sit3.c` implementation
uses exactly this shape:

```c
typedef struct {
    const uint8_t *src;
    size_t         len;
    size_t         byte_pos;
    unsigned       bit_pos;     // 0..7, position of next bit
} bits_t;

int read_bit(bits_t *b) {
    if (b->byte_pos >= b->len) abort("premature end of stream");
    int bit = (b->src[b->byte_pos] >> (7 - b->bit_pos)) & 1;
    if (++b->bit_pos == 8) { b->bit_pos = 0; b->byte_pos++; }
    return bit;
}
```

No accumulator register, no shift-count tracking, no separate
byte-fill phase.  At ~1 cycle of branch overhead per bit, this is
fast enough for any realistic StuffIt archive even without further
optimization.  A more aggressive implementation may load 4 or 8 bytes
into a register at a time and track a bit-count, at the cost of
slightly trickier end-of-stream handling.

### 9.2  Indexed vs Pointered Tree Nodes

Both approaches work; `sit3.c` uses indexed nodes (children are `int16_t`
indices into a fixed-size array) because it avoids the need for
`malloc`/`free` of individual nodes and makes the entire decoder
zero-allocation after the initial output buffer.  A pointer-based
design is fine if the host environment doesn't care about per-node
allocator pressure.

### 9.3  Single-Leaf Tree Optimization

The decode loop in §6.3 naturally handles single-leaf trees (the
`while` condition is false on the first iteration), but a `memset`
fast-path is trivial to add and is significantly faster on long runs of
a single byte value:

```c
if (root_is_leaf) {
    memset(output, root.symbol, uncomp_len);
    return;
}
```

The `sit3.c` implementation does this.

### 9.4  CRC Verification Placement

`sit.c` (the surrounding container parser) verifies the CRC over the
decompressed bytes after method-3 decoding completes.  A method-3
decoder *may* also verify the CRC internally (using its own buffer
state), but this is not necessary — pushing the check up one layer
keeps the decoder simple and lets the container handle the all-methods-
share-the-same-CRC logic uniformly.

### 9.5  Recursive Tree Reader Stack Depth

The tree reader in §6.2 recurses to a depth equal to the tree depth.
For the worst-case 256-leaf tree (depth ~255), this consumes roughly 4
KiB of stack on a 64-bit host (16 bytes per frame × 255).  This is
acceptable on essentially every environment, but an implementation
targeting deeply-embedded systems may want to use an iterative tree
reader with an explicit stack.  Real-world StuffIt 1.5 archives rarely
have trees deeper than ~20 levels.

### 9.6  Performance Profile

Method-3 decoding is heavily branch-bound: one branch per bit read,
two branches per tree walk step.  Throughput on a modern CPU is on the
order of 100–300 MB/s for the decompressed-byte rate, dominated by the
inner branch.  This is fast enough that method 3 is never a bottleneck
for any realistic StuffIt archive.

The encoder side is more complex (frequency counting, priority-queue
Huffman construction, codeword table assembly, bit packing) but is not
covered by this document; method 3 is decode-only for the purposes of
peeler.

---

## 10  Conformance Statement

A method-3 decoder is conformant if, for every legally-formed input as
described in this specification, it produces output that is byte-for-
byte identical to the encoder's original input and matches the stored
CRC-16/ARC value.

The specification is byte-for-byte testable.  Any pair of conformant
implementations, given the same `(compressed_bytes, uncomp_len)`
input, must produce identical output buffers.  No tolerance is
permitted in the output.

Reference test cases (suitable for inclusion in a test suite):

| Input | uncomp_len | Compressed bytes (hex) | Expected CRC-16/ARC | Expected output |
|---|---|---|---|---|
| Empty | 0 | (any, may be zero-length) | `0x0000` | empty |
| Single byte `'X'` | 1 | `AC 00` (= `1`+`01011000`+7 pad bits) | `0xFA01` | `'X'` |
| Five `'X'`s | 5 | `AC 00` | `0x4489` | `"XXXXX"` |
| `"ACAB"` | 4 | `50 54 2A 1B 40` | `0x8955` | `"ACAB"` |

(The first two cases exercise the empty-fork and single-leaf paths; the
third exercises the multi-leaf path documented in §8.)

A decoder that handles all four test cases correctly and produces
output whose CRC-16/ARC matches the entry-header `data_crc` field for
arbitrary real-world archives can be considered conformant.

