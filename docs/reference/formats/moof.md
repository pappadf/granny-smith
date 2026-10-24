# MOOF Disk Image Format

MOOF is the floppy image format of the Applesauce floppy controller for Macintosh 3.5" disks [1]. Unlike a sector
image ([diskcopy42.md](diskcopy42.md)), a MOOF holds what the drive head saw: per track and side, the raw stream of
bit cells (or, rarely, the flux transitions themselves), so the gaps, sync fields and any non-standard sectors of
the original disk survive. Large collections of original, often copy-protected, Macintosh software circulate in it.
This page covers the container, the bitstream and flux encodings, and the decoding of a Macintosh GCR track to its
sectors; the GCR sector format itself is the drive's, described in [iwm-floppy.md](../hardware/iwm-floppy.md)
("Sector Format").

## 1. Identification (magic, sizes)

### 1.1 What MOOF is

One specification exists, *MOOF Disk Image Reference 1.0* (September 2022) [1]; it has not been revised. The
container is the one the same tool uses for Apple II disks (WOZ 2.x) [2] with three differences: the magic, the
INFO chunk's fields, and the metadata keys. Everything is little-endian [1].

### 1.2 Magic

| Offset | Size | Value | Meaning |
|---|---|---|---|
| 0 | 4 | `4D 4F 4F 46` | ASCII `MOOF` |
| 4 | 1 | `FF` | catches a 7-bit transfer |
| 5 | 3 | `0A 0D 0A` | LF CR LF: catches line-ending translation |
| 8 | 4 | uint32 | CRC-32 of every byte from offset 12 to the end of the file, or 0 when not computed |

A file is a MOOF when its first eight bytes are `4D 4F 4F 46 FF 0A 0D 0A`.

### 1.3 Sizes

A bitstream capture of an 800K disk is about 1.3 MB: 160 tracks of 50 000-76 000 bits, each padded to whole
512-byte blocks (§4 decodes a 1 296 302-byte specimen). A 400K disk has half the tracks. Flux tracks are larger,
one byte per transition.

## 2. Layout tables

### 2.1 Chunks

After the 12-byte header the file is a sequence of chunks, each an 8-byte header followed by its data:

| Offset | Size | Field |
|---|---|---|
| +0 | 4 | chunk ID, four ASCII characters |
| +4 | uint32 | data size in bytes (the 8-byte header excluded) |
| +8 | n | data |

| ID | Required | Content |
|---|---|---|
| `INFO` | yes, always first | disk description, §2.2 |
| `TMAP` | yes | track map, §2.3 |
| `TRKS` | yes | track records and track data, §2.4 |
| `FLUX` | no | flux track map, §2.5 |
| `META` | no | text metadata, §2.6 |

The primary chunks sit at fixed offsets in files the tool writes: INFO's header at byte 12, TMAP's at 80, TRKS's at
248, track data from byte 1536 [1]. A reader walks the chunks anyway and skips any ID it does not know. TRKS's size
covers its record array and all track blocks, so the walk lands on the chunk after it.

### 2.2 INFO (60 bytes)

| Chunk offset | Type | Field | Values |
|---|---|---|---|
| +0 | uint8 | INFO version | 1; newer versions stay compatible, so test `>= 1` |
| +1 | uint8 | disk type | 1 = SSDD GCR (400K), 2 = DSDD GCR (800K), 3 = DSHD MFM (1.44 MB), 4 = Twiggy |
| +2 | uint8 | write protected | 1 = write protected |
| +3 | uint8 | synchronized | 1 = imaged with cross-track sync |
| +4 | uint8 | optimal bit timing | in 125 ns ticks: 16 (2 µs) for GCR, 8 (1 µs) for MFM |
| +5 | 32 bytes | creator | UTF-8, no BOM, padded with spaces |
| +37 | uint8 | padding | 0 |
| +38 | uint16 | largest track | block count of the largest track |
| +40 | uint16 | FLUX block | file block (byte offset = value × 512) of the FLUX chunk, or 0 |
| +42 | uint16 | largest flux track | block count of the largest flux track |
| +44 | 16 bytes | reserved | 0 |

A FLUX chunk is present only when both +40 and +42 are non-zero [1]. There is no side-count field: a disk of
type 1 is single-sided, and its side-1 entries in TMAP are all `FF`.

### 2.3 TMAP (160 bytes)

One byte per physical track and side, indexed `track × 2 + side` (track 0-79, side 0-1). Each is an index into the
TRK array (§2.4), or `FF` for a blank track, which a drive reads as random bits [1]. Several entries may name the
same record.

### 2.4 TRKS: TRK records and track data

TRKS begins with 160 eight-byte TRK records (file bytes 256-1535), record *i* being the one TMAP value *i* names:

| Offset | Type | Field |
|---|---|---|
| +0 | uint16 | starting block (byte offset = value × 512); 3 or more |
| +2 | uint16 | block count |
| +4 | uint32 | bit count (a bitstream track) or byte count (a flux track) |

Unused records are all zeros. Track data follows from block 3 (byte 1536); each track starts on a block boundary
and is padded to whole blocks.

A **bitstream** track is `bit count` cells, most significant bit of each byte first, one cell per optimal bit
time; a 1 is a flux transition. The stream is exactly one revolution and loops: after the last bit comes bit 0
[1]. Nothing aligns it to bytes or to the disk bytes it encodes. Because each track's length already reflects
its zone's rotation speed, a player clocks every track at the same cell time and a shorter track simply comes
round sooner.

A **flux** track is a byte stream: each byte is the number of 125 ns ticks since the previous transition, and 255
means "add 255 and keep counting" (255, 255, 10 is 520 ticks). It is also exactly one revolution [1].

### 2.5 FLUX (160 bytes)

Laid out like TMAP, naming the TRK records whose data is flux. A track that uses flux has `FF` in TMAP; if both
name a record, FLUX wins. The chunk starts on a block boundary (the INFO FLUX block) and occupies its own block
[1].

### 2.6 META

UTF-8 text, one `key<TAB>value<LF>` row per key, the last row terminated too; several values are joined with `|`.
Keys are case-sensitive and unique; values may not contain `|`, tab or LF. The defined keys are `title`,
`subtitle`, `publisher`, `developer`, `copyright`, `version`, `language` (from a fixed list), `requires`,
`colordepth` (from 1, 2, 4, 8, 16, 24), `notes`, `disk_name`, `disk_number`, `contributor` and `image_date`
(RFC 3339); others may be added under the same rules [1]. META is the last chunk and is not padded.

## 3. Algorithms (checksums, compression)

### 3.1 CRC

The header CRC is the reflected CRC-32 (polynomial `0xEDB88320`, initial value 0 complemented, result
complemented: the zlib/PKZIP function) over bytes 12 to the end [1]. A stored 0 means none was computed. The CRC
is not needed to read the file, so a mismatch is a warning, not a refusal [1]. A writer computes it last, over
the finished file.

### 3.2 Framing a bitstream into disk bytes

The drive's controller shifts cells into an 8-bit register, most significant first, and a byte is complete when
its top bit is set [3]:

```
reg = 0
for each cell c (looping at bit count):
    reg = (reg << 1) | c
    if reg & 0x80: emit reg; reg = 0
```

Leading zeros fall away, so the framer resynchronises on the self-sync field (`FF` bytes followed by two zero
cells, ten cells each) that precedes every address and data field. Framing two revolutions catches a sector that
straddles the end of the stream. A track that relies on bit timing beyond this (copy protection) needs a full
controller model.

### 3.3 Converting flux to cells

For a flux track, each interval becomes `round(ticks / timing)` cells (at least one), the last of them a 1 and
the rest 0s, `timing` being INFO's optimal bit timing (16 for GCR). The resulting bitstream is framed as §3.2.

### 3.4 Macintosh GCR sectors

A GCR track carries 12 (tracks 0-15), 11, 10, 9 or 8 (tracks 64-79) sectors per side [3]. Each sector is:

| Field | Disk bytes |
|---|---|
| sync | self-sync `FF`s |
| address prologue | `D5 AA 96` |
| track | 6-and-2 code of track & `3F` |
| sector | code of 0-11 |
| side | code of `20` for side 1, OR `01` for tracks 64-79 |
| format | code of the format byte (`22` on an 800K Mac disk, `02` or `12` on a 400K one) |
| checksum | code of (track ^ sector ^ side ^ format) & `3F` |
| address epilogue | `DE AA` |
| sync | self-sync `FF`s |
| data prologue | `D5 AA AD` |
| sector | code of the sector number again |
| data | 699 codes: 12 tag bytes + 512 data bytes |
| checksum | 4 codes: the 24-bit data checksum |
| data epilogue | `DE AA` |

The 64 disk bytes that code the 6-bit values `00`-`3F`, in order:

```
96 97 9A 9B 9D 9E 9F A6 A7 AB AC AD AE AF B2 B3
B4 B5 B6 B7 B9 BA BB BC BD BE BF CB CD CE CF D3
D6 D7 D9 DA DB DC DD DE DF E5 E6 E7 E9 EA EB EC
ED EE EF F2 F3 F4 F5 F6 F7 F9 FA FB FC FD FE FF
```

The 699 data codes are 175 groups: a code holding the top two bits of the next three bytes, then the low six
bits of each (the last group has two bytes, 524 = 174 × 3 + 2). The bytes are XORed with a running three-byte
checksum, which this undoes (the 24-bit check follows the data):

```
c1 = c2 = c3 = 0
for each group j:                                   # b1, b2, b3: bytes rebuilt from the group
    c1 = (c1 & 0xFF) << 1;  if c1 & 0x100: c1 += 1  # rotate c1 left
    v = b1[j] ^ (c1 & 0xFF);  c3 += v;  if c1 & 0x100: c3 += 1; c1 &= 0xFF
    out += v
    v = b2[j] ^ (c3 & 0xFF);  c2 += v;  if c3 > 0xFF:  c2 += 1; c3 &= 0xFF
    out += v
    if len(out) == 524: break
    v = b3[j] ^ (c2 & 0xFF);  c1 += v;  if c2 > 0xFF:  c1 += 1; c2 &= 0xFF
    out += v
c1 &= 0xFF; c2 &= 0xFF; c3 &= 0xFF
check codes == [ (c1>>6 & 3) | (c2>>4 & 0x0C) | (c3>>2 & 0x30), c3 & 0x3F, c2 & 0x3F, c1 & 0x3F ]
```

`out[0..11]` is the sector's tag and `out[12..523]` its data. Encoding runs the same rolling sums with the XOR on
the input side. Sectors are identified by the number in their address field, never by position: a Mac lays them
down 2:1 interleaved (0 6 1 7 2 8 3 9 4 10 5 11 on a twelve-sector track).

### 3.5 Sector order of the decoded disk

The decoded disk lists sectors by track, then side, then sector number — DiskCopy order (see
[diskcopy42.md](diskcopy42.md)): an 800K disk is T0S0 sectors 0-11, T0S1 0-11, T1S0, and so on, 1600 sectors; a
400K disk is side 0 only, 800 sectors.

## 4. Worked decode of a real specimen

*Beyond Dark Castle — Disk 1.moof* (Silicon Beach Software, 1987), from the Internet Archive's moof-a-day
collection [4], 1 296 302 bytes:

- Header: `4D 4F 4F 46 FF 0A 0D 0A`, CRC `0xA00A03CA`, which matches bytes 12 to the end.
- INFO at 12, size 60: version 1, disk type 2 (800K), write protected 1, synchronized 0, bit timing 16, creator
  `Applesauce v1.74`, largest track 19 blocks, no FLUX (both flux fields 0).
- TMAP at 80: `00 01 02 03 …` — track *t* side *s* uses record 2*t* + *s*; all 160 records used.
- TRKS at 248, size 1 295 616. Record 0 = start block 3, 19 blocks, 74 261 bits. Bit counts per zone (first and
  last track of each, side 0): 74 261 / 74 146, 69 032 / 69 034, 62 561, 57 198 / 57 199, 50 048 / 50 049 — about
  404, 434, 480, 524 and 600 rpm at 2 µs cells.
- META at 1 295 872 (block 2531), size 422, unpadded: `title Beyond Dark Castle`, `disk_number Disk 1`,
  `publisher Silicon Beach Software`, `copyright 1987`, `requires Mac 512Ke, Mac Plus, …`, `contributor`,
  `image_date 2019-02-10T13:08:33.833Z` and the non-standard `genre` and `collection` keys.
- Framing and decoding every track as §3 describes finds all 1600 sectors with valid address and data checksums,
  every address field carrying format `22`. The disk's block 2 (byte 1024) starts `42 44`, an HFS master directory
  block, volume name `BDC 1`.

## 5. Open questions

- The MFM (disk type 3) bitstream is described by [1] only in passing ("the track index for MFM is always located
  at the start of the bits"); its decode — IBM-style sectors at 1 µs cells — has not been checked against a
  specimen here.
- Twiggy (type 4) is listed but its track map is not described, and no specimen has been found.
- Whether any capture carries a `WRIT` chunk (defined for WOZ 2.x [2], not mentioned by [1]) is not known; a reader
  skips it.
- The format nibble of a 400K Mac disk is given as `12` by some sources and `02` by others; only 800K specimens
  (`22`) have been decoded here.

## Appendix A. Granny Smith implementation notes

`src/core/storage/image_moof.c` decodes every track of a 400K or 800K GCR MOOF (bitstream or flux) to its sectors
and tags when the image is opened and serves the result as a DiskCopy 4.2 image in memory, so the format registry
reports it as `moof+dc42` and the floppy controllers, the boot and the VFS see an ordinary disk; guest writes go to
the image's delta. Missing sectors read as zeros and are logged with any data-checksum failures. MFM and Twiggy
MOOFs are refused. Only what decodes to sectors survives: protection that relies on bit timing or non-standard
nibbles between sectors is not reproduced. `disk.export` to a `.moof` name writes a bitstream MOOF (creator
`Granny Smith`, 2:1 interleave, gaps sized to each zone's revolution at 394-590 rpm). See
`docs/internals/core/storage/image.md`.

## References

1. J. K. Morris, *MOOF Disk Image Reference 1.0*, applesaucefdc.com, 29 September 2022 — header, chunks, INFO,
   TMAP, TRKS, FLUX, META, CRC routine.
2. J. K. Morris, *WOZ Disk Image Reference 2.1*, applesaucefdc.com — the shared TMAP/TRKS/FLUX structures, blank
   tracks, flux encoding.
3. Apple Computer, *SWIM Chip User's Reference*, revision 1.5, 11 January 1988 — the controller's byte framing, the
   GCR address and data field layout, the data checksum.
4. *Beyond Dark Castle (moof-a-day collection)*, Internet Archive item `moofaday_Beyond_Dark_Castle` (specimen) —
   decoded as in §4.
