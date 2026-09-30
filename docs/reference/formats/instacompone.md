# InstaCompOne: the Apple Installer Installation Tome format

The Apple Installer of the System 7.5/7.6 era ships an installation set as one
or more **Installation Tome** files: single-file archives that carry every file
of the set as a per-fork compressed stream, behind a directory of file
metadata and per-fork checksums. This page documents the container and the
compression method of those tomes, as established by direct disassembly of
the Installer's own decompressor code and by measurement of a complete
specimen.

**Contents:**

1. [Identification (magic, sizes)](#1-identification-magic-sizes) — Finder
   identity and signature, provenance and scope of this page, an
   identification checklist
2. [Layout tables](#2-layout-tables) — overall file map, the 16-byte file
   header, the 128-byte directory entry, the 10-byte stream header, the
   decoder's working state
3. [Algorithms (checksums, compression, with runnable
   pseudocode)](#3-algorithms-checksums-compression-with-runnable-pseudocode) —
   checksum, bit reader, the literal/match flag, literal-run and match-length
   codes, distance dispatch, the method-0 value code, the chunk driver
4. [Worked decode of a real
   specimen](#4-worked-decode-of-a-real-specimen) — the "English Dialect"
   fork: header, first tokens, chunk arithmetic, the checksum comparison
5. [Open questions](#5-open-questions)

References

---

## 1. Identification (magic, sizes)

### 1.1 Finder identity and working name

A tome is a plain data file. The specimen in the evidence set is named
"Installation Tome 7" and carries Finder type `'idcp'` and creator `'kakc'`
[2] (*observed*). The file begins with the four bytes `6B 63 00 01`: ASCII
`'kc'` followed by what reads as a 16-bit format version, `0x0001` [2]
(*observed*).

No shipped artifact in the evidence set spells out a format name. This page
uses the working name **InstaCompOne** — "the `'kc'` compression, version
one" — for the version-`0x0001` container and stream format. The name is a
label of convenience, not an attested string; whether the format names
itself anywhere (a version resource, an Installer string table) is open
(§5).

Each directory entry names a stored fork and carries the Finder metadata
needed to materialize it on the destination volume: type, creator, and the
creation and modification dates (§2.3). The decoded output of a tome is
therefore ordinary Macintosh files — data and resource forks with Finder
information — the file model shared across the HFS family volume formats
[5] and mirrored in Apple's own interchange formats [4]. In this tree a tome
sits opposite the disk-image formats: a Disk Copy 4.2 image
([diskcopy42.md](diskcopy42.md)) is a block image of a whole disk, while a
tome is a file-level archive of individual forks; the NDIF disk-image format
(ndif.md, in this tree) is the closest contemporary in Apple's compressed
container family, and like it the tome trades a container header and
directory for a compact per-fork entropy coding.

### 1.2 Scope and provenance of this page

Every claim below is grounded in one of three primary sources: the full
annotated 68K disassembly of the Installer's own decompressor code resource
[1]; direct measurement of a complete tome specimen [2]; and a token-level
trace recorded from a live Installer session decoding one fork of that
specimen [3]. The surrounding-format context (the fork and Finder-metadata
file model) is cited from Apple's interchange documentation [4] [5].

The evidence base is thin in a specific, stated way: **one specimen, one
decompressor build, one traced fork.** The container, the checksum, the bit
reader, the literal-run and match-length codes and the chunk driver are
established well enough to re-implement from this page. The distance codes
are not: the decision trees that turn distance bits into a match distance
were still being extracted when this page was written, so an independent
decoder built from this page alone cannot yet complete a fork. §5 carries
that gap explicitly, and the page is sized to the evidence rather than
padded beyond it.

All multi-byte integers are stored in big-endian (Motorola) byte order: the
decoder is a 68K program that reads the directory and stream fields with
little-endian-hostile long-word loads, and the values only make sense
big-endian — the block-size word `0x00008000`, the stream offsets such as
`0x0003AB49` [1] [2] (*observed*).

### 1.3 Identification checklist

A file can be identified as an InstaCompOne tome by:

1. Bytes 0–1 equal `6B 63` (`'kc'`).
2. Bytes 2–3 equal `00 01` (version `0x0001`).
3. When the file still sits on a volume carrying Finder metadata, type
   `'idcp'`, creator `'kakc'`.
4. A directory of 128-byte entries starts at offset `0x80`, and for every
   adjacent pair, entry *n*'s `offset + length` (§2.3, `+0x74`/`+0x78`)
   equals entry *n+1*'s offset (*observed* in the specimen [2]).
5. Every fork's stream begins with the 10-byte header of §2.4, whose first
   word is `00 01`.

No single check is conclusive alone, and the header's counts and sizes past
`+0x04` are not decoded (§5), so the directory offset cannot today be
*derived* from the header — it is fixed at `0x80` in the one specimen
observed. A robust reader treats checks 1–2 as the signature and 4–5 as
confirmation.

## 2. Layout tables

### 2.1 Overall file map

| Offset | Size | Contents |
|---|---|---|
| `0x0000` | 16 | File header (§2.2) |
| `0x0010` | 72 | Unaccounted for by the extraction; undecoded (§5) |
| `0x0080` | 128 × *n* | Directory, one 128-byte entry per stored fork (§2.3) |
| entry[0] `+0x74` | entry[0] `+0x78` | Compressed stream of the first fork (§2.4) |
| … | … | Further streams, contiguous, in directory order |

Three facts pin this map down, all *observed* [2] [3]: adjacent directory
entries satisfy `offset + length == next offset`, so the streams are
contiguous and directory-ordered; the decoder's state block holds a
byte-for-byte copy of the directory entry it is currently decoding; and the
`+0x7c` checksum of the entry named "English Dialect" is exactly the value
the Installer compares against at the end of that fork's decode (§4.4). What
the 72 bytes between the header and the directory hold, and whether the
`0x80` directory start is mandated or incidental, is open (§5).

### 2.2 File header (16 bytes)

| Offset | Size | Field | Status |
|---|---|---|---|
| `+0x00` | 2 | Signature `'kc'` (`0x6B63`) | *observed*, fixed in the specimen |
| `+0x02` | 2 | Version `0x0001` | *observed*, only value seen |
| `+0x04` | 12 | Counts and sizes | not decoded (§5) |

The entry count of the directory is not known to be stored in the header; the
extraction did not decode any of the twelve bytes, so a reader today must
walk the directory until its entries stop validating (§1.3, check 4). That
termination rule is not established either — §5.

### 2.3 Directory entry (128 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| `+0x00` | 2 | Kind | `2` for every entry seen (*observed*); meaning unknown |
| `+0x2A` | … | Fork name: one length byte, then the name | Pascal-string form; see the quirk below |
| `+0x4A` | 8 | Finder type and creator | four bytes each |
| `+0x52` | 4 | Creation date | encoding not established (§5) |
| `+0x56` | 4 | Modification date | encoding not established (§5) |
| `+0x74` | 4 | Offset of this fork's compressed data, from file start | |
| `+0x78` | 4 | Compressed length of the stream | |
| `+0x7C` | 4 | Expected checksum of the decompressed fork | algorithm §3.1 |

All other bytes of the 128-byte entry are undecoded; in the specimen they are
present but uninterpreted [2] (§5). The name field has a quirk a reader must
respect: **the name area is not cleared between entries** — the tail of a
previous, longer name is left in place after the current, shorter one [2]
(*observed*). The length byte at `+0x2A` is authoritative; nothing beyond
`length` bytes belongs to the current entry.

The per-entry metadata set (name, type/creator, two dates) mirrors the
Finder-information model that Apple's interchange formats formalize for
foreign files [4]; the tome stores it per fork rather than per file.

### 2.4 Compressed stream header (10 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| `+0x00` | 2 | Method word | `00 01` = method 1; the **second byte** selects the token decoder (§3.3, §3.7) |
| `+0x02` | 4 | `00 00 39 80` | not decoded (§5) |
| `+0x06` | 4 | `00 00 80 00` = 32,768 | block/window size; matches the decoder, which loads `#$8000` into its state at `exfn241+$74` [1] |

This 10-byte header is **identical across every stream in the specimen**
[2] (*observed*) — including the method word, so every fork of this tome is
a method-1 stream. Whether other tomes carry method-0 (§3.7) or other
method numbers is open (§5).

### 2.5 The decoder's working state

The decoder keeps two structures that a re-implementation will want to
mirror, not because they are part of the file, but because the format's
semantics are defined in terms of them (window, quota, carried checksum).
Both are *observed* in the disassembly [1], cross-checked against the
recorded trace [3].

The outer decompressor state block:

| Offset | Meaning |
|---|---|
| `+0x14` | Expected total decompressed length of the current fork |
| `+0x36` | Negated window size (`-state[+0x40]`) |
| `+0x3A` | Per-call output quota (see §3.8); rewritten during chunk bookkeeping at `exfn241+$01BA` |
| `+0x40` | Window/block size — 32,768 for this archive |
| `+0x74` | Running checksum accumulator; the seed for the next chunk (§3.1) |
| `+0x80` | Byte-for-byte copy of the directory entry being decoded |

A separate bit-reader state, initialised per stream by `sub_1C6C`:

| Offset | Meaning |
|---|---|
| `+0x04` | Input byte pointer |
| `+0x08` | 32-bit bit accumulator |
| `+0x0C` | Bytes consumed |
| `+0x10` | Bits currently available in the accumulator |
| `+0x14` | Total bits read |
| `+0x18` | Input length (from initialisation) |

And the driver's stack frame, whose slots the token decoder resolves its
results into — the merge point at `exfn241+$1AFA` is where a trace observes
one decoded token:

| Slot | Meaning |
|---|---|
| `-$14(A6)` | Match length |
| `-$10(A6)` | Match distance |
| `-$18(A6)` | Literal count |
| `-$20(A6)` | Output write pointer |
| `-$6(A6)` | Method word, taken from the stream's second byte (§2.4) |
| `D7` | Output position |

## 3. Algorithms (checksums, compression, with runnable pseudocode)

### 3.1 Checksum: rotate-and-XOR, seeded and carried across chunks

The per-fork checksum is a 32-bit rotate-and-XOR over the **decompressed**
bytes, seeded — which is what makes it whole-file in the presence of chunking
(§3.8): each decode call re-seeds the accumulator from the running value at
`state[+0x74]`, checksums the chunk it produced, and stores the result back
(`sub_1BEC` reads `state[+0x74]`, passes it as the seed, and writes the
return value to the same slot) [1] (*observed*).

```python
def instacomp_checksum(data: bytes, seed: int = 0) -> int:
    # 32-bit rotate-left-8, then XOR the byte in
    acc = seed & 0xFFFFFFFF
    for b in data:
        acc = ((acc << 8) & 0xFFFFFFFF) | (acc >> 24)   # rotate left 8
        acc ^= b
    return acc
```

The final value — the checksum of the entire decompressed fork, since the
seed carries across chunks — is compared against the directory entry's
`+0x7C` field [1] [3] (*observed*). This is the comparison the recorded
session fails (§4.4).

### 3.2 The bit reader

The coded stream is read MSB-first through a 32-bit accumulator that is
topped up in whole bytes whenever fewer than 23 bits remain (the constant
`0x17` in the refill loop) [1] (*observed*). There are two reader routines
with identical semantics — `sub_1CAA` and a byte-at-a-time variant
`sub_1D4E` — and both length codes below call *both*, by bucket: buckets
0–8 of the match-length code (§3.5) call `sub_1D4E`, buckets 9–10 call
`sub_1CAA` [1] (*observed*). For a re-implementation the two are
interchangeable.

```python
def bit_read(n: int) -> int:
    # state: acc (32-bit), bits_avail, ptr, input length
    while state.bits_avail < 23:
        state.acc = ((state.acc << 8) | next_byte()) & 0xFFFFFFFF
        state.bits_avail += 8
    state.bits_avail -= n
    return (state.acc >> state.bits_avail) & ((0xFFFFFFFF >> (32 - n)))
```

One property is easy to miss when replaying a bit-read trace: **literal
bytes do not go through the bit reader at all**. The literal emitter (§3.4)
consumes input bytes directly and maintains its own bookkeeping, so a trace
of bit reads alone will not account for the input consumed by a literal run
[1] (*observed*).

### 3.3 Token decoding: the literal/match flag

A method-1 stream (`sub_18D0`) is a sequence of tokens, each beginning with
a match-length code (§3.5). What that code *means* is gated by a one-bit
state, the **literal/match flag**, initialised to 1 before the first token
(`sub_194E`, at `exfn241+$1988`) [1] (*observed*):

- **Length code 0 with flag ≠ 0** — a literal run: read the literal-run
  length (§3.4) and emit that many bytes.
- **Any other combination** — a match: the match length is the code adjusted
  per below, and the distance follows (§3.6).

After a literal run of *k* bytes, the flag is set to 1 if *k* ≥ 63, else 0.
After a match, the flag is set to 1 [1] (*observed*). The match length is
`code + 2`, plus one more when the flag was 0 at the time of the token —
that is, a match that directly follows a *short* literal run [1]
(*observed*).

The threshold is not arbitrary: 63 is the largest literal-run length the
code of §3.4 can express. The encoding therefore overloads the zero length
code: after a literal run that ended at the code's maximum (where the
encoder may have more literals queued), a zero means "another literal run";
after a literal run that ended early (so a match is due), a zero is folded
into the shortest match, of length 3. The encoder-side rationale is
*inferred — unverified*; the decoder-side polarity (flag **non-zero**
selects the literal path) is *observed* and verified against the trace
(§4.2).

```python
def decode_token(state) -> Token:
    # flag is initialised to 1; state.produced counts output bytes
    code = match_length_code(state)            # §3.5
    if code == 0 and state.flag != 0:          # literal run
        k = literal_run_length(state)          # §3.4
        state.flag = 1 if k >= 63 else 0
        emit_literals(state, k)                # §3.4
        return LiteralRun(k)
    else:                                      # match
        length = code + 2
        if state.flag == 0:
            length += 1                        # match after a short literal run
        state.flag = 1
        dist = decode_distance(state.produced, state)   # §3.6
        return Match(length, dist)
```

### 3.4 Literal-run length code

The literal-run length is coded in a small prefix scheme over 1–63
(`sub_399A`) [1] (*observed*):

```python
def literal_run_length(state) -> int:
    if bit_read(1) == 0:
        return 1
    v = bit_read(2)
    if v == 0:  return 2
    if v == 1:  return 3
    if v == 2:  return bit_read(2) + 4                  # 4-7
    q = bit_read(4)
    if q <= 7:  return q + 8                            # 8-15
    if q <= 11: return bit_read(2) + ((q - 8) << 2) + 16 # 16-31
    return bit_read(3) + ((q - 12) << 3) + 32           # 32-63
```

The buckets are contiguous end to end — every bucket's range begins exactly
one past the previous bucket's end — which is strong internal evidence the
extraction is a correct read rather than a misassembly [1] (*observed*;
the contiguity observation is the evidence, the "correct read" is
*inferred*).

Literal *emission* (`sub_1DB0`) deserves its own pseudocode, because it does
not read bits: it drains whole bytes already sitting in the bit accumulator,
then pulls further bytes straight from the input, shifting each through the
accumulator at the current bit offset [1] (*observed*):

```python
def emit_literals(state, count: int) -> None:
    avail = state.bits_avail
    acc = state.acc
    while avail >= 8 and count > 0:          # drain bytes already buffered
        avail -= 8
        output((acc >> avail) & 0xFF)
        count -= 1
    if count > 0:                            # one input byte per output byte
        state.bytes_consumed += count
        while count > 0:
            acc = ((acc << 8) | next_byte()) & 0xFFFFFFFF
            output((acc >> avail) & 0xFF)    # bit offset preserved
            count -= 1
        state.acc = acc
    state.bits_avail = avail
```

The accumulator acts as a shift register: once fewer than 8 bits of the bit
budget remain, each fresh input byte passes through the extraction window at
the unchanged bit offset, so the sub-byte alignment the length codes left
behind is preserved without ever charging these bytes to the bit budget.
This is the routine a naive re-implementation gets wrong by routing literal
bytes through the bit reader (§3.2).

### 3.5 Match-length code

The match-length code (`sub_3A5E`) is a unary prefix — count leading 1-bits,
capped at 10 — selecting a bucket, then bucket-specific extra bits [1]
(*observed*):

| Bucket | Leading 1-bits | Extra form | Range |
|---|---|---|---|
| 0 | 0 | `read(1)` | 0–1 |
| 1 | 1 | two-level: `read(1)` → 2, else `read(1)+3` | 2–4 |
| 2 | 2 | two-level: `read(1)` → `read(2)+7`, else `read(1)+5` | 5–10 |
| 3 | 3 | `read(3) + 11` | 11–18 |
| 4 | 4 | `read(3) + 19` | 19–26 |
| 5 | 5 | `read(5) + 27` | 27–58 |
| 6 | 6 | `read(6) + 59` | 59–122 |
| 7 | 7 | `read(7) + 123` | 123–250 |
| 8 | 8 | `read(8) + 251` | 251–506 |
| 9 | 9 | `read(9) + 507` | 507–1018 |
| 10 | 10 | `read(10) + 1019` | 1019–2042 |

The base values are recorded in the disassembly; the extra-bit widths follow
from the recorded bucket ranges. As with the literal-run code, the buckets
are contiguous end to end over 0–2042 [1] (*observed*). Recall from §3.3
how the value is used: 0 is overloaded by the flag, and every other value
*v* yields a match of length *v* + 2, or *v* + 3 when it directly follows a
short literal run.

```python
def match_length_code(state) -> int:
    bucket = 0
    while bucket < 10 and bit_read(1) == 1:
        bucket += 1
    if bucket == 0:  return bit_read(1)
    if bucket == 1:
        return 2 if bit_read(1) == 0 else bit_read(1) + 3
    if bucket == 2:
        return bit_read(2) + 7 if bit_read(1) else bit_read(1) + 5
    base = (11, 19, 27, 59, 123, 251, 507, 1019)[bucket - 3]
    return bit_read(bucket + 1) + base          # widths 3,3,5,6,7,8,9,10
```

### 3.6 Distance decoding: window-class dispatch

The match distance is decoded by a family of hand-written decision trees,
selected by the **current output position** — bytes produced so far — so
that early output, which can only reference a short history, uses a
small-window tree, and later output a larger one. The dispatch
(`sub_3BE8`) is a sequential cascade; the first line whose condition holds
wins [1] (*observed*):

| Condition (position ≤ …) | Or window size ≤ … | Tree |
|---|---|---|
| 10 | | `sub_217A` |
| 20 | | `sub_221C` |
| 40 | | `sub_22EE` |
| 80 | | `sub_23DC` |
| 160 | | `sub_24E8` |
| 672 | | `sub_2610` |
| 1000 | | `sub_2768` |
| 2688 | 2048 | `sub_28DE` |
| 5376 | 4096 | `sub_2A74` |
| 10752 | 8192 | `sub_2C28` |
| 21504 | 16384 | `sub_2DFA` |
| 43008 | 32768 | `sub_2FEA` |
| beyond | | further trees, not enumerated in the extraction |

The window size is `state[+0x40]` — 32,768 for this archive (§2.4), so the
`or window ≤ 32768` clause on the `sub_2FEA` line is always true: **every
position above 21,504 uses `sub_2FEA`, and no tree beyond it is ever
reached in this archive** [1] (*observed*).

The trees themselves — the 1-bit branches and the per-branch (extra-bits,
base) pairs — are the main part of the format **not recovered** (§5). Two
structural facts are established: `sub_2DFA` and `sub_2FEA` compare
instruction-for-instruction as the same 17 mnemonics in the same
addressing-mode forms (158 versus 167 instructions), so the trees are
variations on one pattern, scaled to their window class [1] (*observed*);
and in the recorded trace every distance falls within `[1, position]` with
a maximum of 43,008 [3] (*observed*). Until the trees are extracted, an
independent decoder can decode token *lengths* and literal runs but not
*distances* — see §4.5.

Distances are output-relative: the match copy is the byte-at-a-time loop at
`exfn241+$1B18`–`$1B2E`, `src = out - distance`, so overlapping copies are
intended (a run-length behaviour an LZ-family decoder expects) [1]
(*observed*).

### 3.7 Method 0: the universal value code

A stream whose second header byte is 0 selects the other token decoder,
`sub_1852`, built around a single **universal value code**: a unary prefix
of leading 1-bits (capped at 10) selects a bucket, and the bucket's extra
bits plus base give the value [1] (*observed*):

| Bucket | Extra bits | Base | Range |
|---|---|---|---|
| 0 | 2 | 0 | 0–3 |
| 1 | 2 | 4 | 4–7 |
| 2 | 2 | 8 | 8–11 |
| 3 | 3 | 12 | 12–19 |
| 4 | 4 | 20 | 20–35 |
| 5 | 5 | 36 | 36–67 |
| 6 | 6 | 68 | 68–131 |
| 7 | 7 | 132 | 132–259 |
| 8 | 8 | 260 | 260–515 |
| 9 | 9 | 516 | 516–1027 |
| 10 | 10 | 1028 | 1028–2051 |

The table is canonical and self-consistent — each base is the previous base
plus two to the previous bucket's bit count, covering 0–2051 [1]
(*observed*). Token semantics: a value of 0 with the carried flag 0 is a
literal token (decoded by `sub_38D6`, itself not yet recovered); anything
else is a match with length `value + 2` (plus one when the flag is 0), and a
distance from `sub_1F94`, which dispatches on window size to the same tree
family as §3.6 (`≤10 → sub_217A`, `≤20 → sub_221C`, `≤40 → sub_22EE`,
`≤80 → sub_23DC`, …) [1] (*observed*).

**Every fork of the specimen is method 1** (§2.4), so nothing in the
evidence set exercises this path end to end. It is recorded here because it
is fully extracted at the value-code level, and because a different archive
may use it.

### 3.8 The driver loop, windowing and chunking

One decode call does not decode a whole fork. The driver
(`exfn241+$1A4E`–`$1B9E`) loops over tokens with three long-word exit tests
at its head [1] (*observed*):

1. **Output buffer full** — the output write pointer has passed the
   caller's limit.
2. **Chunk quota reached** — a per-call output quota is loaded into
   `state[+0x3A]`; the code that prepares each call stores `#$10000` there,
   so **65,536 bytes is the designed per-call output quota**, not a
   corruption or a coincidence of the specimen.
3. **Input exhausted** — consumed input has reached the available input.

```python
def decode_chunk(state) -> int:
    produced = 0
    while True:
        if state.out_ptr > state.out_limit:  break     # output buffer full
        if state.quota - state.chunk_base + state.window <= produced:
            break                                     # chunk quota reached
        if state.input_consumed >= state.input_avail: break
        token = decode_token(state)                   # §3.3 / §3.7
        emit(token)                                   # match copy: src = out - dist
        produced += token.length
    state.checksum = instacomp_checksum(chunk_output,   # §3.1: seeded from
                                       state.checksum)  # state[+0x74], carried
    return produced
```

Two clauses of the second and third tests involve register operands whose
exact provenance the extraction log records ambiguously; the *conditions*
(above) are observed, the operand-by-operand transcription is not asserted
here (§5). The windowing falls out of the quota arithmetic: the output
buffer carries a **32 KB window prefix** ahead of the current chunk's
output, so distances may legitimately reach past the chunk start into
previously produced bytes (§4.3 measures this happening). The chunk
bookkeeping around `state[+0x3A]` (rewritten at `exfn241+$01BA`) is what
decides whether a second call is made for the remainder of a fork; the
recorded session's behaviour there is examined in §4.4 and remains open.
The outer refill loop (`$180A`–`$1838`) tops up input via `sub_11B4` and
**tolerates end-of-file** (`eofErr`, −39) as a non-fatal refill result [1]
(*observed*).

## 4. Worked decode of a real specimen

### 4.1 The specimen

The measured specimen is "Installation Tome 7", a complete tome from a Mac
OS 7.6 installation set [2]. The fork followed through this section is the
directory entry named "English Dialect" — an AppleScript dialect file, whose
decoded destination is the `Scripting Additions/Dialects` folder of the
System Folder [3]. Its directory entry records [2] [3] (*observed*):

| Field (§2.3) | Value |
|---|---|
| Data offset (`+0x74`) | `0x0003AB49` |
| Compressed length (`+0x78`) | 42,024 bytes |
| Expected checksum (`+0x7C`) | `0x0D0E5D0E` |
| Expected decompressed length | 91,598 bytes |

### 4.2 Stream header and first tokens

The stream begins `00 01 00 00 39 80 00 00 80 00` — method 1, the undecoded
`0x3980` word, and the 32,768 block size (§2.4) [2] (*observed*). The first
bits then decode, per §3.3–§3.5, as follows; the bit reads quoted are the
recorded first reads of the session [3] (*observed*):

**Token 1.** Match-length code: `read(1) = 0` — bucket 0, no leading
1-bits — then `read(1) = 0`, giving code 0. The flag is 1 (its initial
value), so this is a **literal run**. Literal-run length:
`read(1) = 1`, `read(2) = 3` (the `v == 3` branch), `read(4) = 3`, and
since `q = 3 ≤ 7`, the run length is `3 + 8 = 11`. Eleven literal bytes are
emitted per §3.4 — not through the bit reader. The flag becomes
`(11 ≥ 63) = 0`.

**Token 2.** Match-length code reads 0 again; the flag is now 0, so this
is a **match**, of length `0 + 2 + 1 = 3`. The distance decoder runs at
output position 11 (§3.6 dispatch: ≤ 20, tree `sub_221C`), and the recorded
token is distance 4 [3] (*observed*). The flag becomes 1.

**Token 3.** Another literal run, of 15 bytes, then further tokens —
`LITERALS x11, MATCH len=3 dist=4, LITERALS x15, …` are the recorded first
tokens of the fork [3] (*observed*).

This decode — lengths and literals reconstructed from the page's tables,
checked against the recorded trace — exercises every part of §3.3–§3.4 and
the dispatch of §3.6, but stops short of a full independent decode: the
distance trees of §3.6 are not extracted, so the distance 4 above is *read
from the trace*, not derived from the bit stream.

### 4.3 Chunk arithmetic and aggregate measurements

The recorded session observes 25,683 tokens at the driver's per-token point
[3] (*observed*). The aggregate measurements:

- Output positions run monotonically 32,768 → 98,305: the buffer carries a
  32 KB window prefix and the session covers exactly one 65,536-byte chunk
  — the per-call quota of §3.8.
- Every recorded distance lies within `[1, position]`, with a maximum of
  43,008; the 45 tokens whose distances exceed *bytes produced so far* are
  reads into the window prefix.
- The fork's expected total is 91,598 bytes (§4.1), so the fork needs two
  chunks: 65,536 + 26,062.
- At the end of the recorded session the input is consumed to 40,836 of
  the 42,024 compressed bytes.

What occupies the window prefix during a fork's *first* chunk — and
therefore what those 45 prefix-reaching distances legitimately copy — is
not established by the extraction; see §5.

### 4.4 The whole-file checksum comparison

The recorded session ends at a checksum mismatch [3]: the accumulated
checksum is compared against the directory entry's whole-file value
`0x0D0E5D0E` (the exact value the Installer compares, per the byte-for-byte
directory copy in its state block, §2.1) while only one chunk's worth of
output has been accumulated — 65,536 of the expected 91,598 bytes, with
input still unconsumed. The Installer writes no file: the destination
folder is empty after the session, so the decoded bytes exist only in the
decoder's memory [3] (*observed*).

Per §3.1 the checksum is designed to be carried across chunks and to match
the whole-file value only after the last chunk. Whether the second chunk is
never requested, or is requested and mis-sized — whether the fault is in
the chunk bookkeeping (`state[+0x3A]`, rewritten at `exfn241+$01BA`), in
the caller's quota, or elsewhere — is unresolved by the extraction and is
carried in §5. The observation stands on its own either way: the format's
own comparison point is the whole-file `+0x7C` value, seeded per §3.1.

### 4.5 What a from-scratch decoder can and cannot verify today

From this page alone, a reader can implement: the container walk (§2), the
checksum (§3.1), the bit reader (§3.2), and the token stream's *literal and
length* channels (§3.3–§3.5), and can validate the container against the
three cross-checks of §2.1. What no reader can yet do is decode
*distances* (§3.6) and therefore complete a fork or verify a checksum
end to end. The cheapest full validation once the trees are extracted is
the specimen's first directory entry ("Add Alias to Apple Menu", 5,011
bytes [2]) — small enough to decode by hand, with its `+0x7C` value as the
oracle — and then the fork of §4.1, whose expected length and checksum are
both on record.

## 5. Open questions

1. **The distance decision trees** (`sub_217A`, `sub_221C`, `sub_22EE`,
   `sub_23DC`, `sub_24E8`, `sub_2610`, `sub_2768`, `sub_28DE`, `sub_2A74`,
   `sub_2C28`, `sub_2DFA`, `sub_2FEA`, and any beyond): the 1-bit branch
   structure and per-branch (extra-bits, base) pairs are not extracted.
   This is the single largest gap — without it no independent decoder can
   complete a fork (§3.6, §4.5).
2. **Trees beyond `sub_2FEA`.** The dispatch cascade is enumerated only to
   the `pos ≤ 43008 / window ≤ 32768` line; what trees exist beyond it, and
   what window sizes would select them, is unknown (§3.6).
3. **The file header past `+0x04`** — twelve bytes of counts and sizes,
   undecoded — and the 72 bytes between the header and the directory. Is
   the directory's `0x80` offset fixed or pointed to? Where is the entry
   count stored, and how does a reader know where the directory ends?
   (§2.1, §2.2)
4. **The stream header's `00 00 39 80` word.** Constant in the specimen;
   meaning unknown — candidate interpretations (uncompressed-size hint,
   flags, version) are all unevidenced (§2.4).
5. **The directory entry's undecoded bytes**, including the kind field: `2`
   for every entry seen, meaning unknown. Do other kinds exist — resource
   forks, directories, aliases, packages? Are a file's two forks stored as
   two entries, and how is the resource fork of a stored file carried at
   all? Only single-fork entries are observed (§2.3).
6. **Date field encoding.** `+0x52`/`+0x56` are identified as creation and
   modification dates from their position adjacent to the Finder
   type/creator; the encoding (epoch, resolution, time zone) is not
   established (§2.3).
7. **The window prefix's content during a fork's first chunk.** The
   recorded trace shows 45 match distances reading into the 32 KB prefix,
   which is only legitimate if the region holds meaningful data at that
   point; whether it holds a preloaded prefix, stale state from a previous
   fork's decode, or is only exercised on later chunks is unresolved
   (§4.3).
8. **The chunk-continuation contract.** Whether the caller requests a
   second 65,536-byte call for a 91,598-byte fork, how `state[+0x3A]` is
   rewritten at `exfn241+$01BA`, and what ends a multi-chunk decode — the
   recorded session shows the whole-file checksum compared after one
   chunk, and whether that is a caller defect or a mis-read quota is open
   (§3.8, §4.4).
9. **The driver's exit-test operands.** The three exit conditions are
   observed; the register-by-register transcription of the second and
   third tests is ambiguous in the extraction log and is not asserted here
   (§3.8).
10. **Method 0 end to end.** The universal value code (§3.7) is extracted,
    but its literal decoder `sub_38D6` and the `sub_1F94` distance trees
    are not, and no method-0 stream exists in the evidence set to validate
    against. Do method numbers beyond 0 and 1 exist?
11. **Where the format names itself.** "InstaCompOne" is this page's
    working name; whether the `'kc'` signature, `'idcp'`/`'kakc'` types, or
    any Installer resource carries a human-readable format name, and
    whether other versions (a `00 02` header) exist, is unknown (§1.1).
12. **Compressed input beyond the last fork.** The refill path tolerates
    end-of-file; whether anything follows the last fork's stream in a
    tome, and whether the last chunk is expected to hit the input-exhausted
    exit rather than the quota exit, is not observed (§3.8).
13. **The encoder.** No encoder is in the evidence set; the heuristics
    implied by the codes — when the encoder chooses a ≥ 63 literal run
    (which re-arms the literal path), how it picks a distance class, and
    what the `0x3980` word is computed from — are all open (§3.3, §3.6).
14. **Generality of the specimen.** Every structural constant here —
    `0x80` directory, 128-byte entries, kind 2, the 10-byte stream header,
    window 32,768, method 1 — is *n* = 1: one tome from one installation
    set, decoded by one build of the decompressor. Which Installer
    versions ship this code (the traced build is the `exfn` resource 241 of
    the Mac OS 7.6 `InstallSystemSoftware` script), and whether earlier or
    later tomes differ, is unknown.

## References

1. Apple Installer decompressor code — annotated 68K disassembly of the
   `exfn` (external-function) code resource, id 241, 15,840 bytes, of the
   `InstallSystemSoftware` installer script from the Mac OS 7.6
   installation media. The resource implements the whole tome reader:
   directory parsing, checksum, bit reader, both token decoders, the
   distance dispatch and the chunk driver. Routine names (`sub_18D0`,
   `sub_399A`, `sub_3A5E`, `sub_3BE8`, …) are the disassembly's labels for
   routines at those offsets; offsets cited as `exfn241+$hhhh` are into the
   15,840-byte resource.
2. "Installation Tome 7" specimen — a complete Installation Tome file
   (Finder type `idcp`, creator `kakc`) from a Mac OS 7.6 installation
   set. The container measurements of §1–§2 (header, directory, stream
   headers, entry arithmetic) and the fork parameters of §4 are direct
   measurements of this file.
3. Recorded token-level decompression trace — a log of the Installer
   decoding the "English Dialect" fork of [2] during a Mac OS 7.6 install
   session on a Macintosh IIfx: 25,683 tokens observed at the driver's
   per-token merge point (`exfn241+$1AFA`), with per-token
   length/distance/literal-count records, the first bit reads of the
   stream, and the chunk arithmetic of §4.3. The session ended in the
   checksum mismatch of §4.4; the observations are of that session.
4. Apple Computer Inc., "AppleSingle/AppleDouble Formats for Foreign Files"
   developer's note (later revised as "AppleSingle/AppleDouble File
   Format: A File-Interchange and Translation Format") — the
   Finder-information and file-date metadata model that the tome's
   directory entries carry per stored fork.
5. Apple Computer Inc., Technical Note TN1150, "HFS Plus Volume Format" —
   the two-fork file model of the HFS family volume formats, into which
   the Installer materializes decoded forks.
