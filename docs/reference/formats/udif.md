# UDIF (Universal Disk Image Format, `.dmg`)

**Contents:**

1. [Identification (magic, sizes)](#1-identification-magic-sizes) — what UDIF is; the single-fork layout; the
   trailer that identifies it; size limits and byte order
2. [Layout tables](#2-layout-tables) — the `koly` trailer; the XML property list and its `blkx` array; the `mish`
   block table and its 40-byte chunk entries; the chunk type codes
3. [Algorithms (checksums, compression)](#3-algorithms-checksums-compression) — mapping a sector to its chunk; the
   zlib chunk; the three checksums and what each covers; writing an image in one forward pass
4. [Pitfalls](#4-pitfalls) — the mistakes that produce plausible but wrong images
5. [Open questions](#5-open-questions)

---

## 1. Identification (magic, sizes)

### 1.1 What UDIF is

**UDIF** — the *Universal Disk Image Format* — is the disk image container of Disk Copy 6.5 and later and of
every Mac OS X `hdiutil`, conventionally named `.dmg` [2] [3]. It replaced NDIF ([ndif.md](ndif.md)), whose chunk
map lived in the resource fork and was lost by any transfer that dropped it. UDIF is a **single-fork** file: the
payload, the block map and the trailer that locates them are all in the data fork, so it survives any byte-exact
copy [2].

The decoded image is an array of 512-byte sectors. The file describes it as runs of sectors ("chunks"), each
either zero-filled, stored, or compressed, and the block map says which run of the file holds each chunk. A
reader can therefore decode any sector without decoding the rest — the property a disk mounted from a
compressed image needs [2].

### 1.2 The single-fork layout

```
offset 0                   data fork: chunk payloads (any order; typically in sector order)
offset XMLOffset           XML property list (UTF-8), holding the block map
offset EOF − 512           'koly' trailer, 512 bytes
```

Nothing at the start of the file identifies it: the first bytes are chunk payload. A UDIF is identified by its
last 512 bytes [1] [2].

### 1.3 Identification

A file is a UDIF when its last 512 bytes begin with the ASCII signature `koly`, followed by version 4 and header
size 512 (both big-endian 32-bit) [1] [2]. The version and size checks matter: the four letters alone occur in
payload by chance.

All multi-byte fields, in the trailer and in the block tables, are **big-endian** [1] [2].

### 1.4 Size limits

Sector numbers and counts are 64-bit, so the format itself does not bound an image. The decoded size is
`SectorCount × 512` (trailer [§2.1](#21-the-koly-trailer)); there is no field for a length that is not a whole
number of sectors.

## 2. Layout tables

### 2.1 The `koly` trailer

512 bytes, big-endian [1] [2]:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x000 | 4 | Signature | `koly` |
| 0x004 | 4 | Version | 4 |
| 0x008 | 4 | HeaderSize | 512 |
| 0x00C | 4 | Flags | bit 0: flattened |
| 0x010 | 8 | RunningDataForkOffset | 0 for a single-segment image |
| 0x018 | 8 | DataForkOffset | where chunk offsets are measured from; usually 0 |
| 0x020 | 8 | DataForkLength | bytes of chunk payload |
| 0x028 | 8 | RsrcForkOffset | 0 in a flattened image |
| 0x030 | 8 | RsrcForkLength | 0 in a flattened image |
| 0x038 | 4 | SegmentNumber | 1 |
| 0x03C | 4 | SegmentCount | 1; more means a segmented image (`.dmgpart` siblings) |
| 0x040 | 16 | SegmentID | a UUID |
| 0x050 | 4 | DataChecksumType | 2 = CRC-32 |
| 0x054 | 4 | DataChecksumSize | in bits: 32 |
| 0x058 | 128 | DataChecksum | first 4 bytes: the CRC-32, big-endian |
| 0x0D8 | 8 | XMLOffset | the property list |
| 0x0E0 | 8 | XMLLength | |
| 0x0E8 | 120 | Reserved | zero |
| 0x160 | 4 | ChecksumType | the master checksum's type: 2 = CRC-32 |
| 0x164 | 4 | ChecksumSize | 32 |
| 0x168 | 128 | Checksum | first 4 bytes: the master CRC-32 |
| 0x1E8 | 4 | ImageVariant | 1 = device image |
| 0x1EC | 8 | SectorCount | decoded size in 512-byte sectors |
| 0x1F4 | 12 | Reserved | zero |

The 128-byte checksum blobs are why `XMLOffset` is at 0xD8 and `SectorCount` at 0x1EC rather than where a count
of the named fields alone would put them [1].

### 2.2 The property list

An Apple XML property list. The block map is the array `resource-fork` → `blkx`; each element is a dictionary with
`Attributes`, `CFName`, `Data`, `ID` and `Name` keys, `Data` holding one Base64-encoded `mish` block table
([§2.3](#23-the-mish-block-table)) [1] [2]. `Name` and `CFName` name the partition the table covers ("Apple_HFS :
3"); `hdiutil` writes one table per partition of an Apple partition map, and one whole-disk table for an image
with none [3]. The Base64 text is conventionally wrapped and indented; whitespace is not significant. Other keys
(for example `plst`, or an image writer's own) may sit beside `resource-fork`; a reader looks up `blkx` by key.

### 2.3 The `mish` block table

The decoded `Data` blob, big-endian [1] [2]:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x00 | 4 | Signature | `mish` |
| 0x04 | 4 | Version | 1 |
| 0x08 | 8 | SectorNumber | the table's first sector in the decoded image |
| 0x10 | 8 | SectorCount | sectors the table covers |
| 0x18 | 8 | DataOffset | 0 in every image seen; chunk offsets are absolute |
| 0x20 | 4 | BuffersNeeded | advisory: sectors of buffer a decoder needs |
| 0x24 | 4 | BlockDescriptors | the blkx resource ID — **not** a count |
| 0x28 | 24 | Reserved | zero |
| 0x40 | 4 | ChecksumType | 2 = CRC-32 |
| 0x44 | 4 | ChecksumSize | 32 |
| 0x48 | 128 | Checksum | first 4 bytes: CRC-32 of the table's decoded bytes |
| 0xC8 | 4 | NumberOfBlockChunks | entries that follow, terminator included |
| 0xCC | 40 × n | Chunk entries | below |

Each **chunk entry**, 40 bytes:

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | EntryType |
| 0x04 | 4 | Comment |
| 0x08 | 8 | SectorNumber — relative to the table's SectorNumber |
| 0x10 | 8 | SectorCount |
| 0x18 | 8 | CompressedOffset — in the data fork, from DataForkOffset |
| 0x20 | 8 | CompressedLength |

### 2.4 Chunk types

| EntryType | Meaning | Payload |
|---|---|---|
| `0x00000000` | zero-fill | none; the sectors read as zeros |
| `0x00000001` | raw | `SectorCount × 512` stored bytes |
| `0x00000002` | ignored | none; unallocated, contents unspecified (read as zeros) |
| `0x80000004` | ADC (UDCO) | Apple Data Compression stream ([ndif.md](ndif.md) §3) |
| `0x80000005` | zlib (UDZO) | one complete zlib stream [4] |
| `0x80000006` | bzip2 (UDBZ) | one bzip2 stream |
| `0x80000007` | LZFSE (ULFO) | |
| `0x80000008` | LZMA (ULMO) | |
| `0x7FFFFFFE` | comment | `+beg` / `+end` markers; covers no sectors |
| `0xFFFFFFFF` | terminator | ends the entry list |

The parenthesised names are the `hdiutil` format identifiers of images made of that chunk type [3].

## 3. Algorithms (checksums, compression)

### 3.1 Mapping a sector to its chunk

A chunk's absolute first sector is `table.SectorNumber + entry.SectorNumber`. Collect every entry (comments and
terminators dropped) with its absolute position; sorted by position the entries tile `[0, SectorCount)`. A sector
is found by binary search; a run of the decoded image may span several chunks, each decoded independently. A
compressed chunk decodes to exactly `SectorCount × 512` bytes — fewer or more is a damaged image [1] [2].

### 3.2 The zlib chunk

A `0x80000005` chunk is a complete zlib stream (RFC 1950): a 2-byte header, DEFLATE data (RFC 1951), and the
Adler-32 of the decoded bytes [4]. Each chunk stands alone: no dictionary carries over between chunks, which is what
makes random access possible. The Adler-32 detects a damaged chunk on its own decode.

### 3.3 Checksums

All three are CRC-32 with the reflected polynomial `0xEDB88320` (the one zlib and Ethernet use), initial value and
final XOR `0xFFFFFFFF`, stored big-endian in the first four bytes of the 128-byte blob [1] [2]:

- **per table** — over the table's decoded bytes in sector order; chunks of type *ignored* are excluded (zero-fill
  chunks are included, as zeros);
- **data fork** — over the `DataForkLength` stored bytes;
- **master** — over the concatenation of every table's 4-byte checksum, big-endian, in table order.

The CRC of `n` zero bytes can be computed in O(log n) by exponentiating the CRC's one-zero-byte linear map — the
`crc32_combine` technique — so a writer need not feed a large zero run through the table.

### 3.4 Writing in one forward pass

Because the block map and trailer follow the payload, an image can be written front to back with no seek: read the
decoded image a chunk at a time; for an all-zero chunk extend a zero-fill entry (adjacent zero chunks merge into
one), otherwise compress it, store it as raw when compression does not shrink it, append the payload and record the
entry; keep the per-table CRC over decoded bytes and the data-fork CRC over stored bytes as the bytes pass; finally
append the property list and the trailer. Memory is one chunk and the growing entry list.

## 4. Pitfalls

- **Table-relative sector numbers.** An entry's SectorNumber restarts at zero in every table; reading it as
  absolute decodes every table after the first onto the first.
- **`BlockDescriptors` at 0x24 is the resource ID**, not the entry count; the count is at 0xC8.
- **Trailer offsets.** The 128-byte checksum blobs move XMLOffset to 0xD8 and SectorCount to 0x1EC.
- **Ignored versus zero.** Both read as zeros, but only zero-fill is in the table checksum.
- **Partition naming.** Tools that open the payload may choose a reader from a table's `Name` (an `Apple_HFS`
  table read as a bare HFS volume); a whole-disk table over a partition map is best named for the map
  (`Apple_partition_scheme`).
- **Large chunks.** Nothing bounds a chunk's size; a random-access reader that decodes whole chunks must bound the
  decoded size it accepts, or re-chunk the image.

## 5. Open questions

- Whether `hdiutil` interprets `BuffersNeeded` beyond allocation sizing, and whether it requires a
  partition-typed table name to attach a whole-disk image, has not been confirmed against `hdiutil` itself.
- The master checksum's definition is from the format literature [2]; it has not been reproduced on a
  multi-table image.

---

## References

1. Toast-mastered CD-ROM image in UDZO format, 1996, version-4 trailer (specimen) — its `koly` trailer and every
   per-table `mish` CRC-32 decoded and reproduced; the field offsets in §2 are as measured there.
2. Jonathan Levin, "Demystifying the DMG File Format" — reverse-engineering study (the trailer and block-table
   layouts, chunk types, checksum fields).
3. `hdiutil(1)` manual page, Apple Inc., macOS — the image format identifiers (UDZO, UDBZ, ULFO, ULMO, UDCO,
   UDRW), `imageinfo`, `verify`, `convert`.
4. P. Deutsch and J-L. Gailly, *ZLIB Compressed Data Format Specification version 3.3*, RFC 1950, May 1996; and
   P. Deutsch, *DEFLATE Compressed Data Format Specification version 1.3*, RFC 1951, May 1996.
