# Disk Copy 6.x NDIF (New Disk Image Format)

**Contents:**

1. [Identification (magic, sizes)](#1-identification-magic-sizes) — what NDIF is and when Apple used it; the dual-fork
   architecture; Finder type/creator codes and the four variants; size limits and byte order; how to identify a file
   that carries no in-band magic at all
2. [Layout tables](#2-layout-tables) — the two forks; the Resource Manager container as measured in a real file; the
   NDIF resource set; the `bcem` header and its 12-byte chunk descriptors; the chunk type codes, observed against the
   reverse-engineering literature; the self-mounting-image wrapper
3. [Algorithms (checksums, compression)](#3-algorithms-checksums-compression) — the chunk coverage rule and
   random-access sector lookup; the Apple Data Compression (ADC) token format and decoder; the RLE and overlap
   degenerate cases; the checksum question, contrasted with the Disk Copy 4.2 and Mac ROM conventions; full-image
   reassembly
4. [Worked decode of a real specimen](#4-worked-decode-of-a-real-specimen) — byte-by-byte decode of the Disk Copy
   6.3.3 self-mounting image: wrapper, resource fork, `bcem`, chunk table, first ADC tokens, reconstruction and
   verification, statistics
5. [Open questions](#5-open-questions)

---

## 1. Identification (magic, sizes)

### 1.1 What NDIF is

**NDIF** — the *New Disk Image Format* (the expansion "Nouveau Disk Image Format" also circulates) — is Apple's
second-generation disk image format, introduced with **Disk Copy 6.0** and produced by Disk Copy 6.x through the
Mac OS 9 era [2] [3]. It replaced the flat, floppy-bound Disk Copy 4.2 container
([diskcopy42.md](diskcopy42.md), whose rigid 84-byte header and four fixed floppy geometries it was designed to
escape) and was itself replaced by the single-fork UDIF (`.dmg`) format in Mac OS X [2] [3]. Reading support for
NDIF survived in the macOS disk-image tooling until it was removed in macOS 11 [2] [3].

The defining architectural decision of NDIF is that it is **not a self-contained byte-stream format**. An NDIF file
is a classic Mac OS dual-fork file:

- the **data fork** holds nothing but volume bytes — either a raw sector dump (uncompressed variants) or a
  packed sequence of independently coded chunks (compressed variants). It contains no header, no trailer, no
  magic number [3];
- the **resource fork** holds every piece of metadata needed to interpret the data fork, in the standard Resource
  Manager container ([§2.2](#22-the-resource-manager-container)) — above all the `bcem` chunk map
  ([§2.4](#24-the-bcem-resource)) [2] [3] [4].

This split is the format's strength on classic Mac OS (the Resource Manager indexes the metadata for free, and the
mounting code can random-access any sector through the chunk table) and its fatal weakness everywhere else: any
file transfer or filesystem that drops the resource fork makes a compressed NDIF permanently undecodable, because
nothing in the data fork records chunk boundaries or codecs [2] [3]. Distribution therefore always paired NDIF
with a dual-fork-preserving wrapper — MacBinary, BinHex, AppleSingle/AppleDouble — and Disk Copy 6 additionally
offered the **self-mounting image** (SMI), a small classic Mac OS application that embeds the image and mounts it
without Disk Copy installed ([§2.7](#27-the-self-mounting-image-smi)) [2] [3] [4].

Every fact in [§2](#2-layout-tables) and [§4](#4-worked-decode-of-a-real-specimen) about the `bcem` chunk table
is *observed*: it was established by a full byte-level decode of a real Disk Copy 6.3.3 specimen [1] — wrapper,
resource map, chunk table, ADC streams and reconstructed volume — with each field cross-checked for internal
consistency (chunk offsets chain exactly; chunk coverage tiles the volume; decompression lands on sector
boundaries; the embedded HFS structures validate). The reverse-engineering literature [2] [3] [4] supplies the
format history, the variant taxonomy and the ADC token tables, and where it disagrees with the specimen the
disagreement is stated in the text rather than smoothed over.

### 1.2 Finder metadata and type codes

NDIF has no in-band signature, so on a classic Mac OS filesystem identification lives in the Finder metadata
(cite: file type and creator codes as displayed in [diskcopy42.md](diskcopy42.md) §"Resource Fork" for the
predecessor format):

| Field | NDIF value | Contrast | Status |
|---|---|---|---|
| File type | `dimg` (lowercase `i`) | DC42 uses `dImg` (uppercase `I`) | reported [2] [3] [4] |
| Creator code | `ddsk` | DC42 uses `dCpy` | reported [3] [4] |
| Extension | `.img` | also `.image` (rare) | reported [3] |
| SMI type | `APPL` with extension `.smi` | a Mac application, not an image file | reported [3]; specimen observed [1] |

The specimen's own creator code is `oneb!`, not `ddsk` ([§4.1](#41-the-specimen-and-its-wrapper)) — so a decoder
must not key on the creator. The lowercase-`i` `dimg` / uppercase-`I` `dImg` distinction is itself only a
convention reported by the reverse-engineering literature [2] [3] and should be treated as a hint, not a
guarantee.

Out-of-band identification of a bare data fork is impossible: the first bytes of an uncompressed NDIF data fork
are the volume's own boot blocks, and of a compressed NDIF, the first stored chunk — neither says anything about
the container. This is the opposite philosophy from Disk Copy 4.2, which carries a private word (`$0100` at header
offset +82) and self-describing sizes ([diskcopy42.md](diskcopy42.md) §"Header (84 bytes)"), and from the PRAM
validity byte, where a magic token distinguishes valid from invalid contents (mac-pram.md §3). An NDIF file can
only be identified by opening its resource fork and looking for a `bcem` resource — or, failing that, by trusting
the Finder info that survived alongside it.

### 1.3 The four variants

The macOS command-line disk image tool lists four NDIF sub-formats, whose names appear in Disk Copy-related
documentation and tooling [3] [6]:

| Identifier | Meaning | Data fork contents | Resource fork needed to read? |
|---|---|---|---|
| `RdWr` | read/write image | raw sector bytes, no compression | no |
| `Rdxx` | read-only uncompressed | raw sector bytes, no compression | no |
| `ROCo` | read-only, ADC-compressed | chunked, ADC-coded stream | **yes** |
| `Rken` | read-only, KenCode-compressed | chunked, KenCode-coded stream | **yes** |

For `RdWr`/`Rdxx` the data fork is a byte-for-byte dump of the source volume; stripping the resource fork
degrades the file to a plain raw disk image, which remains mountable [3]. For `ROCo`/`Rken` the chunk map exists
only in the resource fork, and its loss is unrecoverable [2] [3]. The specimen of [§4](#4-worked-decode-of-a-real-specimen)
is a compressed image in the `ROCo` family (ADC chunks) *observed*; no `Rken` specimen was available for this
page, and the KenCode codec is documented only as undecoded literature ([§5](#5-open-questions), item 4).

The `Rdxx` creation path reportedly includes a free-space optimization: unused areas of HFS, MS-DOS and UFS
volumes are skipped rather than stored [2]. The specimen shows the compressed analogue of this behaviour — a
2,609-sector zero-fill run covering the volume's free tail ([§4.4](#44-the-chunk-table)) — but whether skipped
regions were necessarily zero in the source volume is not established ([§5](#5-open-questions), item 3).

### 1.4 Sizes and byte order

- **Sector size:** 512 bytes, throughout [3] [4]; every offset and coverage figure in the chunk table is in
  sectors or bytes of the data fork.
- **Maximum logical size:** 2 GB, reported [2] [3] [4]. The limit is *inferred — unverified* to follow from the
  32-bit offset/length fields and the 24-bit sector fields of the chunk table ([§2.5](#25-the-chunk-descriptors)):
  a 24-bit sector field caps a single image at 16,777,216 sectors, but the reported 2 GB ceiling is consistent
  with the classic Mac OS file-size APIs of the era rather than with any measured field width. No specimen near
  the limit was examined.
- **Byte order:** every multi-byte field in every NDIF structure — resource fork header, resource map, `bcem`
  header, chunk descriptors — is big-endian, *observed* [1] and reported [3] [4].

### 1.5 An identification procedure that works

Given a file of unknown provenance, the practical identification sequence is:

1. If the first 128 bytes look like a MacBinary-family header (Pascal file name, plausible type/creator, fork
   lengths that add up to the file size — [§4.1](#41-the-specimen-and-its-wrapper)), unwrap it to separate forks
   and continue.
2. Parse the resource fork's Resource Manager map ([§2.2](#22-the-resource-manager-container)) and look for a
   resource of type `bcem`. Its presence identifies a compressed NDIF (or an SMI embedding one).
3. Validate the `bcem` header ([§2.4](#24-the-bcem-resource)): descriptor count, total sector count, and
   terminator (type `0xFF`) whose start sector equals the total.
4. If no resource fork exists, test whether the data fork length is a multiple of 512 and the content is a
   recognizable volume — it may be a fork-stripped `RdWr`/`Rdxx` image, which is nothing but a raw dump [3].

There is no step 0 magic test. Files that fail all of the above are not NDIF.

## 2. Layout tables

### 2.1 The two-fork container

An NDIF image on a classic Mac OS filesystem is one catalog entry with two forks:

```
catalog entry (Finder type 'dimg', creator 'ddsk' — reported)
 ├── data fork:    volume bytes only, no framing
 │     uncompressed variants:  raw sectors, start to end
 │     compressed variants:    chunk 0 | chunk 1 | ... | chunk n-1   (packed, no padding)
 └── resource fork: Resource Manager container (§2.2)
       bcem 128 — the chunk map (§2.4)          mandatory for ROCo/Rken
       bcm# 129+ — continuation maps           reported [2] [3], never observed
       cism, plst, nlad, vers — auxiliary        reported [4], absent from the specimen
```

Chunks are packed back to back in the data fork with no alignment padding, and their physical order is the order
of their descriptors in the `bcem` table — the decoder must follow the table's offsets, never assume contiguity
beyond what the table says [3]. *Observed in the specimen*: consecutive chunks' `data_offset`/`data_length`
fields chain exactly, with the raw/ADC/zero-fill chunks interleaved in logical order [1].

### 2.2 The Resource Manager container

The resource fork is the standard classic Mac OS resource fork; the canonical description is the Resource
Manager chapter of *Inside Macintosh: More Macintosh Toolbox* [5]. The layout below is given with the field
offsets as measured in the specimen [1] (*observed*); the values in the specimen's fork are given in
[§4.2](#42-the-resource-fork).

Fork header (16 bytes, at fork offset 0):

| Offset | Size | Field | Specimen value |
|---|---|---|---|
| +0 | 4 | offset of resource data section | `$00000100` |
| +4 | 4 | offset of resource map | `$0001E71C` |
| +8 | 4 | length of resource data section | `$0001E61C` |
| +12 | 4 | length of resource map | `$0000041E` |

Resource data section: each resource is stored as a 4-byte big-endian length prefix followed by its raw bytes,
back to back. A resource's location is data-section-offset + the 24-bit offset recorded in its map entry.

Resource map (at the map offset; header-copy first):

| Map offset | Size | Field | Specimen value |
|---|---|---|---|
| +0 | 16 | copy of the fork header | as above |
| +16 | 4 | reserved (in-memory handle) | garbage on disk |
| +20 | 2 | reserved (file reference) | garbage on disk |
| +22 | 2 | resource fork attributes | `$0000` |
| +24 | 2 | offset from map start to type list | `$001C` (28) |
| +26 | 2 | offset from map start to name list | `$035E` (862) |

Type list (at map+24): a 2-byte count-of-types-minus-one, then per type an 8-byte entry:

| Entry offset | Size | Field |
|---|---|---|
| +0 | 4 | OSType (four-character code), e.g. `bcem` |
| +4 | 2 | count of resources of this type, minus one |
| +6 | 2 | offset from type-list start to this type's reference list |

Reference list (per type): 12 bytes per resource:

| Entry offset | Size | Field |
|---|---|---|
| +0 | 2 | resource ID (NDIF's `bcem` is ID 128 — *observed* [1]) |
| +2 | 2 | offset into name list, `$FFFF` if unnamed |
| +4 | 1 | attributes |
| +5 | 3 | offset of this resource's length prefix within the data section (24-bit) |
| +8 | 4 | reserved (in-memory handle, zero on disk) |

The name list stores Pascal strings (length byte + bytes), referenced from the reference entries. A decoder
that cannot or will not parse the full map can instead walk the data section linearly — length prefix, resource
bytes, repeat — and recognize resources by content (the `bcem` table is identifiable by its structure,
[§2.4](#24-the-bcem-resource)), but the map parse is the robust path.

### 2.3 The NDIF resource set

| Type | ID | Purpose | Evidence |
|---|---|---|---|
| `bcem` | 128 | block/chunk map: per-chunk start sector, codec, data-fork offset and length ([§2.4](#24-the-bcem-resource)) | observed [1] |
| `bcm#` | 129+ | continuation block maps for images whose map exceeds one resource; one per ~128 MB, reported | reported [2] [3] [4], never observed |
| `cism` | 128 | image-level checksum record: `u32 type = 2` (CRC-32), `u32 size = 32` (bits), `u32 crc` — the same value as the `bcem` checksum field, reported | reported [4], absent from the specimen |
| `plst` | 128 | 32-byte Finder property blob (`FInfo` + `FXInfo`) for the mounted volume's icon/window, reported | reported [4], absent from the specimen |
| `nlad` | 128 | 4-byte sector-size declaration (`u32 512`), reported; mounters reportedly infer it from the `bcem` totals when absent | reported [4], absent from the specimen |
| `vers` | 1 | producer version string, cosmetic | reported [4]; an SMI's `vers` observed [1] |

The compressed-NDIF minimum is the `bcem` resource alone; the specimen carries exactly that and nothing else of
the NDIF set, and decodes perfectly without the others [1] (*observed*). Note the discrepancy: the literature
[2] [3] at one point describes `bcem` as holding a UDIF-style "mish block table" with a 204-byte header and
40-byte entries; [4] explicitly retracts that — `bcem` is *not* UDIF's `mish`/`blkx` — and the specimen confirms
the retraction: a compact header with 12-byte descriptors ([§2.4](#24-the-bcem-resource), [§2.5](#25-the-chunk-descriptors))
[1]. Treat the UDIF `mish` layout as a different format; do not parse `bcem` with it.

### 2.4 The `bcem` resource

The `bcem` resource (ID 128) is a header followed by a variable-length array of 12-byte chunk descriptors. The
specimen's `bcem` is 248 bytes: a 128-byte header and 10 descriptors [1] (*observed*). Layout as measured:

| Offset | Size | Field | Specimen value | Status |
|---|---|---|---|---|
| +0 | 2 | unknown — `$000B` (11) | version? count+1? | unresolved ([§5](#5-open-questions), item 1) |
| +2 | 2 | zero | `$0000` | observed |
| +4 | 1..64 | Pascal string: image/volume label, length-prefixed, zero-padded | length 9, "Disk Copy" | observed |
| +68 | 4 | total sector count of the decompressed image | `5120` — matches the terminator's start sector exactly | observed, verified |
| +72 | 4 | unknown — `$00000201` (513) | chunk size 512 sectors + 1? version 2.1? | unresolved ([§5](#5-open-questions), item 2) |
| +76 | 4 | zero | `$00000000` | observed |
| +80 | 4 | candidate checksum — `$07E2D482` | no standard CRC-32 variant reproduces it ([§3.5](#35-checksums)) | unresolved ([§5](#5-open-questions), item 3) |
| +84 | 4 | zero | `$00000000` | observed |
| +88..+123 | 36 | zero padding | — | observed |
| +124 | 4 | chunk descriptor count **including the terminator** | `10` — exactly the number of 12-byte entries that follow | observed, verified |
| +128 | 12 × count | chunk descriptors ([§2.5](#25-chunk-descriptors)) | 10 entries | observed |

The two fields marked *verified* are load-bearing: the count at +124 tells a decoder how many 12-byte entries to
read, and the total at +68 tells it the decompressed size before it decodes a single chunk. Both check out against
the specimen's terminator entry and reconstructed image [1]. The literature's `bcem` header claims differ from
this measured layout ([4] describes an 86-byte header with segment/UUID fields, `data_offset`/`data_length` and
a CRC at +0x22; none of those offsets match the specimen) — the table above is what a real Disk Copy 6.3.3 file
contains, and the differences are recorded rather than reconciled ([§5](#5-open-questions), items 1–3, 7).

### 2.5 The chunk descriptors

Each descriptor is 12 bytes, all fields big-endian (*observed* [1]):

| Offset | Size | Field | Meaning |
|---|---|---|---|
| +0 | 4 | sector-and-type word | **bits 31..8: first sector covered** (24 bits); **bits 7..0: chunk type** ([§2.6](#26-chunk-type-codes)) |
| +4 | 4 | data offset | byte offset of this chunk's stored bytes within the data fork |
| +8 | 4 | data length | number of stored bytes (0 for zero-fill and terminator entries) |

Two rules make the table self-checking, and both hold exactly in the specimen [1] (*observed*):

- **Coverage rule.** A descriptor covers logical sectors `start` (its own low-24-bit sector) through the next
  descriptor's `start` minus one. Per-chunk sector counts are *not* stored; they are always implied by the
  neighbour. The terminator's start sector is the image's total sector count, so the last data descriptor's
  coverage closes at end-of-image. The table must be sorted by start sector with no gaps and no overlaps.
- **Storage rule.** For raw chunks, `data_length` = covered sectors × 512 exactly. For ADC chunks,
  `data_length` is the compressed byte count and the chunk decompresses to exactly covered sectors × 512
  bytes. For zero-fill chunks, `data_offset` and `data_length` are 0. For the terminator, `data_offset` equals
  the total stored length of the chunk stream (the end offset of the last stored chunk) — a free consistency
  check on the whole file ([§4.4](#44-the-chunk-table)).

Note the bit assignment: the type byte is the **low** byte of the first word and the sector the **high** 24 bits.
The literature [4] describes the opposite assignment (type in the high 8 bits, sector in the low 24); applied to
the specimen that parse produces non-contiguous sector runs and fails the coverage rule, so the measured
assignment above is the correct one for Disk Copy 6.3.3 files [1]. A decoder should still validate the coverage
rule and refuse tables that fail it.

### 2.6 The chunk type codes

Codes *observed* in the specimen [1], with the values the literature reports for comparison:

| Code | Meaning (observed) | Data fork bytes | Specimen chunks | Literature [4] says |
|---|---|---|---|---|
| `0x00` | zero-fill: covered sectors are all zero | none | 2 (2,610 sectors) | zero-fill — agrees |
| `0x02` | raw: stored bytes are volume bytes verbatim | covered sectors × 512 | 2 (6 sectors total) | "KenCode" — **disagrees** |
| `0x83` | ADC-compressed (Apple Data Compression, [§3.2](#32-the-adc-token-format)) | compressed stream | 5 (2,488 sectors) | `0x04` is ADC — **disagrees** |
| `0xFF` | terminator: no chunk; start = total sectors | none | 1 | terminator — agrees |

The specimen's raw chunks are demonstrably raw: their bytes land verbatim in the volume, and the second one is
the HFS alternate MDB, whose signature is visible unencoded in the stored stream [1]. The ADC chunks decode
byte-exactly under the ADC token rules ([§3.2](#32-the-adc-token-format)) [3]. The `0x83` code's internal
structure — compressed flag `0x80` plus codec 3? — and the codes for KenCode and any other codecs are not
established ([§5](#5-open-questions), items 4–6). A decoder written from this page must accept the three
observed codes, validate raw/zero chunks structurally, and reject unobserved codes explicitly rather than
guessing them.

### 2.7 The self-mounting image (SMI)

A **self-mounting image** is a classic Mac OS application (Finder type `APPL`, extension `.smi`) whose data fork
carries an embedded NDIF chunk stream and whose resource fork carries both the application stub and the image's
`bcem` map [3] [4]. Double-clicking launches the stub, which installs a disk driver and mounts the embedded
volume — the distribution mechanism Apple used so recipients did not need Disk Copy installed [3]. The specimen
is exactly this: its resource fork holds a 41,902-byte `DRVR` (driver name ".HDI"), `CODE` 0/1/2 resources
(51,606 bytes of code in the largest), Finder bundle/icon resources, and the `bcem` 128 map, alongside internal
tables that pair image kinds with driver names (an `hdi6` resource listing the NDIF driver family — NDIFhdro,
NDIFrohd, NDIFhdrv — plus raw, DART and Disk Copy 4.2 image kinds) and a signature-to-volume-name table (an
`hdi5` resource mapping `BD` to "Mac OS HFS", `H+` to "Mac OS HFS+", and MFS, ProDOS, ISO 9660, High Sierra,
Audio CD and Photo CD signatures to their names) [1] (*observed*). The stub's user-visible string "Loading disk
image driver" and its `vers` resource ("1.0", long version "+1.0, Copyright Apple Computer, Inc. 1983-99") are
also present [1] (*observed*).

For decoding purposes an SMI is an NDIF image with a peculiar resource fork: same `bcem`, same chunk rules,
plus application resources a decoder ignores, and possibly stub bytes appended to the data fork after the
chunk stream ends ([§4.1](#41-the-specimen-and-its-wrapper)). The mount sequence the stub performs (driver
installation, the handoff of the `bcem` to the driver) is not in the evidence set
([§5](#5-open-questions), item 12).

## 3. Algorithms (checksums, compression)

### 3.1 The coverage rule and random-access sector lookup

Because chunk coverage is implied by the sorted start sectors, sector lookup is a binary search followed by a
codec dispatch. Pseudocode:

```
function ndif_read_sector(bc, data_fork, S):            # bc = parsed bcem, S = sector number
    if S < 0 or S >= bc.total_sectors: error "out of range"
    # binary search: last descriptor whose start sector is <= S
    lo, hi = 0, bc.entry_count - 2                       # last real entry is entry_count-2;
    while lo < hi:                                       # terminator at entry_count-1
        mid = (lo + hi + 1) // 2
        if entry(bc, mid).start <= S: lo = mid
        else: hi = mid - 1
    d = entry(bc, lo)
    within = S - d.start                                 # sector offset inside the chunk
    switch d.type:
        case 0x00: return 512 zero bytes
        case 0x02: return data_fork[d.offset + 512*within : + 512]
        case 0x83: buf = adc_decompress(data_fork[d.offset : d.offset + d.length])
                   if len(buf) != 512 * (next_start(bc, lo) - d.start): error "bad chunk"
                   return buf[512*within : + 512]
        case 0xFF: error "terminator reached"            # table is malformed or S out of range
        default:  error "unknown chunk type"
```

`entry(i).start` is the low-24-bit sector of descriptor `i`; `next_start(bc, lo)` is the start of descriptor
`lo+1` (the terminator's start doubles as the image's total sector count, so the last data chunk closes
correctly). The validation steps are not decoration: the specimen satisfies them exactly, and a table that
does not is corrupt or not NDIF ([§2.5](#25-the-chunk-descriptors)).

### 3.2 The ADC token format

Apple Data Compression (ADC) is the codec of the `ROCo` variant: a byte-aligned LZ77-family scheme combining
literal runs with sliding-window back-references, documented in the reverse-engineering literature [3] and
verified byte-exactly against all five compressed chunks of the specimen [1]. Each token begins with a tag
byte whose top two bits select one of three token types:

| Tag bits | Token | Encoding | Length | Offset/window |
|---|---|---|---|---|
| `1xxxxxxx` | literal run | tag + N literal bytes | `(tag & $7F) + 1` = 1..128 | — |
| `01xxxxxx` | long match | tag + 2 offset bytes (big-endian) | `(tag & $3F) + 4` = 4..67 | 16-bit, 0..65535 |
| `00xxxxxx` | short match | tag + 1 offset byte | `((tag >> 2) & $0F) + 3` = 3..18 | 10-bit: `((tag & $03) << 8) \| byte1` = 0..1023 |

For both match forms, the source position is `output_position - offset - 1` — that is, the stored offset is the
backwards distance minus one, so offset 0 means "the immediately preceding byte" [3] (*verified by decode* [1]).
Matches are always copied one byte at a time, because source and destination can overlap
([§3.4](#34-rle-and-overlap-degenerate-cases)).

ADC is asymmetric by design: compression does the work, decompression is a tight loop of tag tests, arithmetic
and byte copies, which is why Apple used it for factory imaging and network distribution [2]. The compression
side — match-finding policy, literal-run flushing — was never published, and encoders cannot reproduce Apple's
byte streams; decoders do not care ([§5](#5-open-questions), item 8).

### 3.3 The ADC decoder

Runnable pseudocode for a complete chunk decoder:

```
function adc_decompress(in):                       # in: stored chunk bytes
    out = bytearray()
    ip = 0
    while ip < len(in):
        tag = in[ip]
        if tag & 0x80:                             # literal run
            n = (tag & 0x7F) + 1
            out += in[ip+1 : ip+1+n]               # copy n literal bytes verbatim
            ip += 1 + n
        elif tag & 0x40:                           # long match, 3-byte token
            n = (tag & 0x3F) + 4
            off = (in[ip+1] << 8) | in[ip+2]
            src = len(out) - off - 1
            if src < 0: error "back-reference before start of output"
            for i in 0 .. n-1: out.append(out[src + i])   # byte-by-byte: overlap allowed
            ip += 3
        else:                                      # short match, 2-byte token
            n = ((tag >> 2) & 0x0F) + 3
            off = ((tag & 0x03) << 8) | in[ip+1]
            src = len(out) - off - 1
            if src < 0: error "back-reference before start of output"
            for i in 0 .. n-1: out.append(out[src + i])
            ip += 2
    return out
```

In an NDIF context the caller must check that the returned length equals the chunk's implied coverage
(`next_start - start` sectors × 512) — in the specimen all five ADC chunks land exactly on their sector
boundaries [1] (*observed*), and a chunk that stops short or overruns indicates truncation or corruption.

### 3.4 RLE and overlap degenerate cases

Two special cases fall out of the `output_position - offset - 1` rule and must be handled by the byte-at-a-time
copy, not special-cased [3]:

- **offset 0 is RLE.** The source is the single byte immediately before the output position, so a match with
  offset 0 replicates that byte `n` times — the cheapest possible run-length encoding, and the specimen uses it
  heavily for the zero-filled areas of the volume [1] (*observed*: the first ADC chunk opens with a one-byte
  literal followed by an offset-0 short match, [§4.5](#45-first-tokens-walked)).
- **overlap is a pattern extender.** When `offset + 1 < length`, the copy reads bytes it has itself just
  written, which extends repeating patterns of period `offset + 1` — e.g. a 4-byte pattern can be extended by a
  match of any length with offset 3. This is why the copy loop must be byte-at-a-time; a block copy
  implementation corrupts every overlapped match.

### 3.5 Checksums

Apple's tooling names a checksum style for NDIF that its manual page calls "CRC28" and its own documentation
labels "CRC-32 (NDIF)" [3] [6], and the reverse-engineering literature takes it to be the standard IEEE 802.3 /
zlib CRC-32 (reflected polynomial `$EDB88320`, initial value and final XOR `$FFFFFFFF`), computed over the
decompressed image and stored both in the `bcem` header and in the `cism` resource [3] [4]. This page cannot
confirm that. Against the fully reconstructed specimen image — 2,621,440 bytes whose embedded HFS structures
validate ([§4.6](#46-reconstruction-and-verification)) — the `bcem` candidate field `$07E2D482` matches none of
the tested variants [1] (*observed*):

| CRC variant over the reconstructed image | Result |
|---|---|
| CRC-32/ISO-HDLC (zlib): reflected `$EDB88320`, init `$FFFFFFFF`, xorout `$FFFFFFFF` | `$780739E0` |
| same, init `0`, xorout `0` | `$1DC1B6F6` |
| same, init `$FFFFFFFF`, xorout `0` | `$87F8C61F` |
| non-reflected `$04C11DB7`: init `$FFFFFFFF`/xorout `0`; init `0`/xorout `$FFFFFFFF`; init `0`/xorout `0`; init `$FFFFFFFF`/xorout `$FFFFFFFF` | `$6CAD3E89`, `$045C5D2F`, `$FBA3A2D0`, `$9352C176` |
| same variants over the stored chunk stream, the materialized (non-zero) sectors only, the used volume prefix, and the `bcem` bytes | none equal `$07E2D482` |

The Disk Copy 4.2 rotating word checksum ([diskcopy42.md](diskcopy42.md) §"Checksum Algorithm") was also tried
against the reconstructed image (`$E7446E78`) and does not match either. Two readings are possible and the
evidence does not choose between them: the field is a checksum under an algorithm not identified here (possibly
a genuinely 28-bit CRC, per the "CRC28" name [6]), or it is a checksum over the *creation-time* source volume
whose skipped free space was not zero — content no longer recoverable from the file, since the chunk map only
says "zero" ([§1.3](#13-the-four-variants)). A decoder should therefore treat the field as opaque unless it can
verify it, and an encoder that wants round-trip fidelity through Apple's mounters should preserve it verbatim.
For contrast: the classic Mac ROM's one's-complement word checksum (mac-rom.md §3) is stored *in* the image it
covers and is self-verifying, and Disk Copy 4.2's checksum covers its data region directly — NDIF is the odd
one out in that no independent description of its checksum computation survives.

### 3.6 Whole-image reassembly

Complete decode of an NDIF file to a raw volume:

```
function ndif_to_raw(file):
    (data_fork, resource_fork) = unwrap(file)          # MacBinary/AppleDouble, or native forks
    bc = parse_resource_map(resource_fork)             # §2.2: find 'bcem' by type, read via map entry
    assert bc.entry_count == (len(bc.blob) - 128) / 12 # 128-byte header, 12-byte descriptors
    img = zero_bytes(bc.total_sectors * 512)           # zero-fill is the default fill
    for i in 0 .. bc.entry_count - 2:                  # all but the terminator
        d = entry(bc, i)
        cover = (entry(bc, i+1).start - d.start) * 512
        switch d.type:
            case 0x00: pass                            # already zero
            case 0x02: assert d.length == cover
                        img[d.start*512 : +cover] = data_fork[d.offset : +d.length]
            case 0x83: buf = adc_decompress(data_fork[d.offset : +d.length])
                        assert len(buf) == cover
                        img[d.start*512 : +cover] = buf
            default:   error "unsupported chunk type"
    assert entry(bc, bc.entry_count-1).type == 0xFF
    assert entry(bc, bc.entry_count-1).start == bc.total_sectors
    assert entry(bc, bc.entry_count-1).offset == last_stored_end   # §2.5 storage rule
    return img
```

Every assertion in that loop passes on the specimen [1]. The final assertion — the terminator's offset equals
the end of the stored chunk stream — is a whole-file integrity check that costs nothing and catches truncation
and offset corruption in one step.

## 4. Worked decode of a real specimen

### 4.1 The specimen and its wrapper

The specimen is the Disk Copy 6.3.3 distribution self-mounting image, `Disk Copy 6.3.3.smi`, as a 771,584-byte
file wrapped in a MacBinary-family header [1] (*observed*). The 128-byte header decodes as:

| Offset | Size | Field | Value |
|---|---|---|---|
| +1 | 1 | file name length | 19 |
| +2 | 19 | file name | "Disk Copy 6.3.3.smi" |
| +65 | 4 | Finder type | `APPL` |
| +69 | 4 | Finder creator | `oneb!` |
| +83 | 4 | data fork length (high byte zero) | 645,510 (`$0009D986`) |
| +87 | 4 | resource fork length | 125,754 (`$0001EB3A`) |
| +91 | 4 | creation date (Mac epoch) | `$B3560D70` = 1999-05-05 15:00 |
| +95 | 4 | modification date | `$B3560D70` |
| +117 | 2 | secondary header signature | `$8181` (129, 129) |

The layout is the MacBinary convention — name at +1, type at +65, creator at +69, fork lengths at +83/+87,
dates at +91/+95, and the 129/129 pair at +117 that the MacBinary II family uses to mark its secondary header.
The file geometry confirms the parse: 128 (header) + 645,510 (data fork) padded to the next 128-byte boundary
(645,632) + 125,754 (resource fork) = 771,482, padded to 771,584, the exact file size [1] (*observed*). Which
MacBinary generation produced it, and whether its header CRC validates, is not resolved
([§5](#5-open-questions), item 11); the fork arithmetic above is what the decode relies on.

The data fork is the SMI's NDIF chunk stream followed by 677 bytes of 68K code and string data that belong to
the application stub (they reference part number 062-4625 and driver-name strings) — the chunk stream proper
ends at offset 644,833, exactly where the chunk table's terminator says it does
([§4.4](#44-the-chunk-table)) [1] (*observed*).

### 4.2 The resource fork

The resource fork parses cleanly under the [§2.2](#22-the-resource-manager-container) layout [1] (*observed*):

- Fork header: data section at `$00000100`, map at `$0001E71C`, data length `$0001E61C`, map length `$0000041E`.
- The map's 29 types hold 50 resources in total. Application stub: `CODE` 0/1/2 (the 51,606-byte `CODE` 1 is
  the bulk of the stub), `DRVR` 0 (".HDI", 41,902 bytes), `SIZE`, `BNDL`, `FREF`, icons and patterns, `STR#`,
  `DLOG`, `DITL`, the internal driver-name and filesystem-signature tables `hdi1`–`hdi6`, and `vers` 1.0.
- The NDIF set is present as exactly one resource: **`bcem` 128, unnamed, 248 bytes**, located at data-section
  offset `$0001E4E6` (the 4-byte length prefix there reads 248).

The image metadata therefore lives in a single 248-byte resource; the other 49 resources are the mounter
application and are ignored by decode. No `cism`, `plst`, `nlad` or `bcm#` resources exist in this file — an SMI
evidently needs none of them to mount [1] (*observed*; the auxiliary resources remain reported-only,
[§2.3](#23-the-ndif-resource-set)).

### 4.3 The `bcem` header bytes

The 128-byte header of the `bcem` resource, in full [1]:

```
+000  00 0B 00 00 09 44 69 73 6B 20 43 6F 70 79 00 00   .....Disk Copy..
+010  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
+020  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
+030  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
+040  00 00 00 00 00 00 14 00 00 00 02 01 00 00 00 00   ................
+050  07 E2 D4 82 00 00 00 00 00 00 00 00 00 00 00 00   ................
+060  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
+070  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0A   ................
```

Reading it with the [§2.4](#24-the-bcem-resource) table: unknown word `$000B` at +0; the Pascal string "Disk
Copy" at +4; **total sector count `$00001400` = 5120** at +68; unknown `$00000201` at +72; candidate checksum
`$07E2D482` at +80; **descriptor count `$0000000A` = 10** at +124; descriptors from +128 to +247, exactly
120 bytes = 10 × 12 [1] (*observed*).

### 4.4 The chunk table

All ten descriptors, decoded per [§2.5](#25-the-chunk-descriptors) [1] (*observed*):

| # | Start sector | Type | Data offset | Data length | Covers | Decode |
|---|---|---|---|---|---|---|
| 0 | 0 | `0x02` raw | `$00000000` | `$00000A00` (2,560) | 0–4 (5 sectors) | volume boot blocks + primary MDB, verbatim |
| 1 | 5 | `0x83` ADC | `$00000A00` | `$00017711` (96,017) | 5–516 (512 sectors) | 262,144 bytes |
| 2 | 517 | `0x83` ADC | `$00018111` | `$00021E40` (138,816) | 517–1028 | 262,144 bytes |
| 3 | 1029 | `0x83` ADC | `$00039F51` | `$00024A3F` (150,079) | 1029–1540 | 262,144 bytes |
| 4 | 1541 | `0x83` ADC | `$0005E990` | `$00021070` (135,280) | 1541–2052 | 262,144 bytes |
| 5 | 2053 | `0x83` ADC | `$0007FA00` | `$0001DAE1` (121,569) | 2053–2508 (456) | 233,472 bytes |
| 6 | 2509 | `0x00` zero | 0 | 0 | 2509–5117 (2,609) | free-space fill |
| 7 | 5118 | `0x02` raw | `$0009D4E1` | `$00000200` (512) | 5118 (1 sector) | alternate MDB, verbatim |
| 8 | 5119 | `0x00` zero | 0 | 0 | 5119 (1 sector) | last sector |
| 9 | 5120 | `0xFF` term | `$0009D6E1` | 0 | — | terminator |

Every structural rule holds exactly: start sectors are contiguous with no gaps (0, 5, 517, ... 5119, 5120); the
raw chunks' lengths equal their coverage × 512 (2,560 = 5 × 512; 512 = 1 × 512); the ADC chunks' offsets chain —
each chunk's `data_offset` is the previous `data_offset + data_length`, ending at `$0009D6E1` (644,833), which
is precisely the terminator's offset field; and the terminator's start sector 5120 equals the header's total
[1] (*observed*). The chunk size in use is **512 sectors (262,144 bytes = 256 KB)** for the full ADC chunks —
the literature's 128-sector (64 KB) figure [4] does not match this file ([§5](#5-open-questions), item 9).

The volume shape explains the table: sectors 0–4 hold the boot blocks (all zero — the volume is not bootable)
and the primary HFS MDB; sectors 5–2508 hold the catalogued data; 2509–5117 are the volume's free tail, stored
as one zero-fill chunk; 5118 is the alternate MDB (HFS keeps it at total-sectors − 2), which Disk Copy stored
raw despite being 90% zeros — and sector 5119 is the final sector. Both MDBs carry the signature `BD` and the
volume name "Disk Copy 6.3.3" [1] (*observed*).

### 4.5 First tokens walked

The chunk stream's first ADC chunk begins at data-fork offset `$0A00` with the bytes
`80 00 10 00 83 01 00 00 03 14 0B ...`. Decoding under [§3.2](#32-the-adc-token-format) [1] (*observed*):

| Bytes | Token | Effect on the output |
|---|---|---|
| `80 00` | literal run, length 1 | emits `00` |
| `10 00` | short match, length 7, offset 0 | emits seven more `00` — offset 0 is RLE of the previous byte |
| `83 01 00 00 03` | literal run, length 4 | emits `01 00 00 03` |
| `14 0B` | short match, length 8, offset 11 | copies 8 bytes from 12 bytes back (all zeros so far) |
| `24 07` | short match, length 12, offset 7 | copies 12 bytes from 8 bytes back |
| `83 02 00 00 07` | literal run, length 4 | emits `02 00 00 07` |
| `00 23` | short match, length 3, offset 35 | copies 3 bytes from 36 bytes back |
| `80 28` | literal run, length 1 | emits `28` |
| `00 27` | short match, length 3, offset 39 | copies 3 bytes from 40 bytes back |

The decoded bytes are the start of sector 5 — the volume's allocation structures: mostly zeros with scattered
one-bit patterns, exactly what a mostly-empty HFS volume's bitmap area looks like, and exactly the content the
RLE-flavoured short match is best at. Note how the very first two tokens demonstrate the degenerate case of
[§3.4](#34-rle-and-overlap-degenerate-cases): one literal byte plus an offset-0 match manufactures a run of
eight zeros from two stored bytes.

### 4.6 Reconstruction and verification

Reassembling per [§3.6](#36-whole-image-reassembly) produces a 2,621,440-byte image (5,120 sectors) that
validates as an HFS volume on every check applied [1] (*observed*):

- primary MDB at sector 2 with signature `BD` and volume name "Disk Copy 6.3.3";
- alternate MDB at sector 5,118 (5,120 − 2) with the same signature and name;
- boot blocks (sectors 0–1) all zero — a non-bootable distribution volume, as expected for an SMI;
- the five ADC chunks decode to exactly their implied sector counts — 262,144 bytes each for chunks 1–4 and
  233,472 for the short chunk 5 — with no overrun and no shortfall;
- the stored stream's end matches the terminator's offset field to the byte.

The one field that does not verify is the header's candidate checksum `$07E2D482`
([§3.5](#35-checksums)) — recorded honestly as the format's open verification problem.

### 4.7 Statistics

| Quantity | Value |
|---|---|
| Decompressed volume | 2,621,440 bytes (5,120 sectors) |
| Stored chunk stream | 644,833 bytes |
| Overall ratio | 24.6% — 4.07:1 |
| ADC chunks: stored / decoded | 641,761 / 1,282,048 bytes (47.6% of volume at 50.1% of decoded size) |
| Chunk 1 token census | 96,017 stored bytes → 9,542 literal runs, 4,635 short matches, 15,315 long matches → 262,144 bytes |
| Raw chunks | 3,072 bytes, stored verbatim (0.1% of volume) |
| Zero-fill | 2,610 sectors = 1,336,320 bytes of volume (50.9%) stored as 0 bytes |
| SMI overhead beyond the chunk stream | 125,754-byte resource fork + 677 data-fork tail bytes |

The census makes the codec's economy visible: over half the volume is free space that costs literally nothing
(zero-fill), and the half that carries data compresses to half again. The long-match token dominates the
compressed stream of chunk 1 (15,315 tokens to 4,635 short and 9,542 literal runs) — a structured HFS volume's
redundancy sits at distances beyond the 1,024-byte short-match window [1] (*observed*).

## 5. Open questions

1. **The `bcem` header word at +0 (`$000B`).** Version number? Descriptor count plus one? A flag word? The value
   11 has no established meaning; 10 descriptors follow the 128-byte header, and the count at +124 already
   covers the array, so +0 is something else entirely ([§2.4](#24-the-bcem-resource)).
2. **The word at +72 (`$000201`, 513).** Chunk size 512 sectors plus one? A "2.1" format version? A count of raw
   chunks (the specimen has exactly two)? No second specimen exists in the evidence set to compare against
   ([§2.4](#24-the-bcem-resource)).
3. **The checksum field at +80 (`$07E2D482`).** No tested CRC-32 variant over any tested reconstruction of the
   specimen reproduces it ([§3.5](#35-checksums)). Is "CRC28" a genuinely 28-bit polynomial, a non-standard
   variant, or a checksum over creation-time source content whose skipped free space was not zero — content
   unrecoverable from the file? Without a second checksummed specimen or a disassembly of Disk Copy's
   verification path, this cannot be settled.
4. **KenCode.** The `Rken` codec's algorithm, its chunk type code under the observed byte layout, and any
   decoder documentation: none is in the evidence set. The literature records that no KenCode decoder existed
   publicly until a third-party one appeared in 2025 [2] [4]; this page deliberately documents nothing about the
   algorithm because nothing verifiable is available.
5. **The type-code space.** Only `0x00`, `0x02`, `0x83` and `0xFF` are observed. Is `0x83` "compressed flag +
   codec 3", making raw `0x02` "codec 2" and KenCode perhaps `0x84`? Are `0x01`, `0x03`–`0x7F` assigned? The
   literature's table (raw `0x01`, KenCode `0x02`, ADC `0x04` [4]) contradicts the specimen on two of three
   shared codes ([§2.6](#26-chunk-type-codes)) and cannot be reconciled by re-reading; it is presumably a
   different era or a different assumption about the descriptor word.
6. **The chunk-size policy.** The specimen uses 512-sector (256 KB) chunks; the literature claims 128-sector
   (64 KB) chunks and a 64 KB decompression buffer [4]. Does the size vary by Disk Copy version, by image size,
   or was the claim simply wrong? A decoder must not assume either value — the coverage rule makes chunk size
   self-describing, which is why this page treats the neighbour-implied coverage as the only rule.
7. **`bcm#` continuation tables.** Reported for images whose map exceeds one resource, with per-segment
   `first_sector`/`sector_count`/UUID fields [2] [3] [4]. Never observed; the specimen's single `bcem` covers
   5,120 sectors with room to spare, and no multi-segment specimen was available. The claimed segment-header
   fields (including the 16-byte segment id) remain unverified.
8. **The ADC encoder's policy.** Match-finding heuristics, lazy evaluation, literal-run flushing and the
   raw-fallback threshold are not published anywhere in the evidence set; only the decode side is fully
   specified ([§3.2](#32-the-adc-token-format)). Byte-identical re-encoding of Apple's streams is therefore not
   achievable from this page, and a writer can only target semantic equivalence (decode-equals-original).
9. **`cism`, `plst`, `nlad`.** The auxiliary resources' layouts exist only as reported structures [4]; the
   specimen — an SMI — carries none of them, so none of the three could be verified, and their presence in
   plain `ROCo` images remains reported-only ([§2.3](#23-the-ndif-resource-set)).
10. **The SMI's 677 trailing data-fork bytes.** 68K code and strings (part number 062-4625, driver-name
    fragments) beyond the terminator's end offset — the stub's own data, appended after the image's chunk
    stream. What the stub expects there, and whether every SMI carries such a tail, is unknown
    ([§4.1](#41-the-specimen-and-its-wrapper)).
11. **The wrapper's exact MacBinary generation.** The header matches the MacBinary family (129/129 at +117, fork
    arithmetic exact) but its version bytes do not cleanly identify a MacBinary II versus III, and the header
    CRC was not validated. The fork-length fields sit at +83/+87 as 4-byte values with the high byte zero; the
    widely-quoted 24-bit-at-+83 convention does not parse this file ([§4.1](#41-the-specimen-and-its-wrapper)).
12. **The SMI mount sequence.** How the stub's `CODE` resources install the ".HDI" driver, what it passes to the
    driver (the `bcem` handle? the data fork's location?), and how the driver surfaces the volume to the OS —
    the disassembly is not in the evidence set; only the resources' existence and names are observed
    ([§2.7](#27-the-self-mounting-image-smi)).
13. **The 2 GB ceiling.** Reported in three sources [2] [3] [4] but never observed near the limit: whether Disk
    Copy enforced it, whether it follows from the field widths or the era's file APIs, and what a
    multi-gigabyte attempt produces, are untested ([§1.4](#14-sizes-and-byte-order)).
14. **Uncompressed variants' resource forks.** `RdWr`/`Rdxx` images reportedly carry a "light" resource fork —
    checksums and properties but no chunk map — and degrade gracefully when it is stripped [3]. No specimen
    was available; whether they carry a `bcem` at all, and what their `cism` contains, is unverified
    ([§1.3](#13-the-four-variants)).
15. **The `oneb!` creator.** The specimen's SMI carries creator `oneb!`, not the `ddsk` reported for ordinary
    NDIF images [3] [4]. Whether `oneb!` marks all self-mounting images, Disk Copy's SMI generator, or this
    file's particular production path is unknown ([§4.1](#41-the-specimen-and-its-wrapper)).

## References

1. Disk Copy 6.3.3 distribution self-mounting image, specimen file "Disk Copy 6.3.3.smi" (MacBinary-wrapped,
   771,584 bytes; Finder type `APPL`, creator `oneb!`; created 1999-05-05 15:00 Mac epoch `$B3560D70`) — full
   byte-level decode: the 128-byte wrapper header and fork arithmetic; the resource-fork header, map, all 29
   types and 50 resources; the `bcem` 128 resource (248 bytes) header and all 10 descriptors; ADC decode of all
   five compressed chunks; reconstruction of the 2,621,440-byte volume and validation of its HFS structures;
   CRC evaluation over seven algorithms and eight data ranges. Offsets and byte values cited in the prose are
   into this file and its forks.
2. "The Macintosh New Disk Image Format (NDIF): Exhaustive Architectural Specification and Implementation
   Guide" — reverse-engineering study (format history, dual-fork architecture, variant taxonomy and type
   codes, resource-fork dependence, compression overview, deprecation timeline). Note: its `bcem` chapter
   describes a UDIF-style block table that the specimen contradicts; see [§2.3](#23-the-ndif-resource-set).
3. "Macintosh Disk Copy 6.x NDIF format specification" — reverse-engineering study (§"File-level structure and
   identification": Finder codes and variants; §"Resource fork layout and resource types"; §"The mish block
   table" — the UDIF-style reading later corrected in [4]; §"Apple Data Compression algorithm in full": the
   token tables this page verified against the specimen; §"Checksum fields and computation": the CRC-32 /
   "CRC28" claim examined in §3.5; §"How NDIF differs from UDIF").
4. "An implementation-ready specification for writing NDIF disk images" — reverse-engineering study (§"How NDIF
   actually lays out on disk" and §"The `bcem` resource in bytes": the 12-byte descriptor claim the specimen
   confirms, alongside the header layout, 64 KiB chunk-size claim and type-code table the specimen
   contradicts; §"The CRC-32 ('CRC28') computation"; §"The `cism`/`plst`/`nlad`/`vers` resource descriptions";
   §"Building the resource fork from scratch": the encoder-side resource-fork recipe).
5. Apple Computer, Inc., *Inside Macintosh: More Macintosh Toolbox*, Addison-Wesley Publishing Company, 1991 —
   Resource Manager chapter, "Format of the Resource Fork" section (the fork header, resource data section,
   resource map, type list, reference list and name list; the canonical container [§2.2](#22-the-resource-manager-container)
   restates it with offsets as measured in the specimen).
6. `hdiutil(1)` manual page, Apple Inc., Mac OS X — the disk-image framework's own vocabulary: the NDIF
   sub-format identifiers (`RdWr`, `Rdxx`, `ROCo`, `Rken`) and the "CRC28" checksum label for NDIF images
   (both as quoted in [3] §"File-level structure and identification" and §"Checksum fields and computation";
   the man page's NDIF coverage was removed from the tool in macOS 11).
