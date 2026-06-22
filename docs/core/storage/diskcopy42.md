# DiskCopy 4.2 Image Format

DiskCopy 4.2 is a disk image format created by Apple for duplicating
and distributing 3.5" floppy disks on the Macintosh. The format stores
a complete block-level copy of a floppy disk along with optional tag
data and checksums for integrity verification.

All multi-byte integers are stored in big-endian (Motorola) byte order.

## Supported Disk Sizes

| Format  | Blocks | Data Bytes | Tag Bytes | Encoding     |
|---------|--------|------------|-----------|--------------|
| 400K    |    800 |    409,600 |     9,600 | GCR CLV ssdd |
| 800K    |  1,600 |    819,200 |    19,200 | GCR CLV dsdd |
| 720K    |  1,440 |    737,280 |         0 | MFM CAV dsdd |
| 1440K   |  2,880 |  1,474,560 |         0 | MFM CAV dshd |

GCR-encoded disks (400K and 800K) carry 12 bytes of tag data per sector.
MFM-encoded disks (720K and 1440K) have no tag data.

## File Layout

A DiskCopy 4.2 file consists of three contiguous regions with no padding
or alignment gaps between them:

```
Offset   Length         Contents
──────   ──────         ────────
0        84 bytes       Header
84       dataSize       User data (block contents)
84+dataSize  tagSize    Tag data (if present)
```

Total file size is always exactly `84 + dataSize + tagSize`.

## Header (84 bytes)

| Offset | Size    | Name         | Description                                                                                      |
|--------|---------|--------------|--------------------------------------------------------------------------------------------------|
| +0     | 64      | diskName     | Pascal string: first byte is the character count (0–63), followed by the name, zero-padded to 64 bytes total. |
| +64    | 4       | dataSize     | Number of bytes of user data (512 bytes per block × number of blocks).                           |
| +68    | 4       | tagSize      | Number of bytes of tag data (12 bytes per sector × number of sectors), or zero if none.          |
| +72    | 4       | dataChecksum | Checksum computed over the entire user data region.                                              |
| +76    | 4       | tagChecksum  | Checksum computed over the tag data region (see note below). Zero if tagSize is zero.            |
| +80    | 1       | diskFormat   | Disk format code: 0 = 400K, 1 = 800K, 2 = 720K, 3 = 1440K.                                     |
| +81    | 1       | formatByte   | Low-level format identifier (see below).                                                         |
| +82    | 2       | magic        | Format signature. Must be `0x0100`. If this value is different, the file is not DiskCopy 4.2.    |

### diskName

The name field is a classic Pascal string occupying a fixed 64-byte area.
Byte 0 holds the length (0–63). Bytes 1 through length hold the disk name in
the original Macintosh character encoding. Remaining bytes through offset 63
are zero.

### formatByte

This byte echoes the low-level format tag written to the disk media:

| Value  | Meaning                                                                  |
|--------|--------------------------------------------------------------------------|
| `0x12` | 400K Macintosh GCR disk                                                  |
| `0x22` | 800K Macintosh GCR disk (also used by DiskCopy for non-800K Apple II disks) |
| `0x24` | 800K Apple II GCR disk                                                   |

DiskCopy is inconsistent about this field for Apple II images. It sometimes
writes `0x22` for disks that are logically Apple II format.

### magic (private field)

The two-byte field at offset +82 must contain `0x0100` (big-endian). This
serves as the format version signature. Files with a different value here
are either corrupt or use a different (incompatible) format revision.

## User Data

Starting at file offset 84, this region contains the raw 512-byte blocks of
the disk in sequential order from block 0 through the last block. The total
size equals the `dataSize` field in the header.

For a bootable Macintosh disk, block 0 begins with the boot block signature
bytes `0x4C 0x4B` ("LK"), and the HFS Master Directory Block at block 2
begins with `0x42 0x44` ("BD" — the HFS MDB signature `0x4244`).

## Tag Data

Immediately following the user data, this region contains 12 bytes of tag
(scavenger) data per sector, in sequential sector order. Tag data is a
feature of the Sony GCR floppy controller used in the original Macintosh and
is only present for 400K and 800K disk images. For 720K and 1440K MFM disks,
the `tagSize` header field is zero and this region is absent.

Even when tag bytes are all zeroes (as is typical for Apple II disks and many
Macintosh disks), the tag data region is still present in 400K and 800K
images and its size is still reflected in `tagSize`.

Each 12-byte tag entry contains filesystem metadata that the Macintosh file
system originally used for scavenging (recovery). The structure of a tag
entry is:

| Offset | Size | Field           |
|--------|------|-----------------|
| +0     | 4    | File number     |
| +4     | 2    | Flags/attributes|
| +6     | 2    | Logical block   |
| +8     | 4    | Timestamp       |

Modern tools generally ignore this data but preserve it for round-trip
fidelity.

## Checksum Algorithm

DiskCopy uses a simple 32-bit checksum computed over the data interpreted as
a sequence of big-endian 16-bit words. The algorithm is:

1. Initialize a 32-bit accumulator to zero.
2. For each consecutive 16-bit word in the data (big-endian byte order):
   a. Add the word to the accumulator (unsigned 32-bit addition, discarding
      overflow beyond bit 31).
   b. Rotate the entire 32-bit accumulator right by one bit, wrapping bit 0
      into bit 31.
3. The final accumulator value is the checksum.

In pseudocode:

```
checksum = 0
for i = 0 to length-1 step 2:
    word = (data[i] << 8) | data[i+1]
    checksum = (checksum + word) mod 2^32
    bit0 = checksum & 1
    checksum = (checksum >> 1) | (bit0 << 31)
return checksum
```

The data checksum (`dataChecksum`) is computed over the entire user data
region — all `dataSize` bytes, starting from file offset 84.

### Tag Checksum Quirk

The FTN specification states that the tag checksum covers "all the tag data."
However, DiskCopy actually **skips the first 12 bytes** (sector 0's tags)
when computing the tag checksum. The checksum is calculated over bytes 12
through `tagSize - 1` of the tag data region, not from byte 0. This is an
undocumented implementation detail confirmed by verifying checksums against
images produced by DiskCopy itself.

If `tagSize` is zero, `tagChecksum` is zero.

## Identifying a DiskCopy 4.2 File

A file can be identified as DiskCopy 4.2 by checking:

1. The 2-byte magic field at offset +82 equals `0x0100`.
2. The `dataSize` field at offset +64 is one of the four expected values
   (409600, 819200, 737280, or 1474560).
3. The `diskFormat` byte at offset +80 is in the range 0–3.
4. Total file size equals `84 + dataSize + tagSize`.

Optionally, verify the `dataChecksum` against a freshly computed checksum of
the user data region.

## Resource Fork

On a Macintosh HFS filesystem, DiskCopy files have type `dImg` and creator
`dCpy`. The resource fork typically contains `vers` resources listing version
information and checksums, visible through the Finder's "Get Info" dialog.
The resource fork is not required to read or use the image; all essential data
is in the data fork.

When transferred to an Apple II via the GS/OS HFS FST, the file receives
ProDOS file type `$E0` and auxiliary type `$0005`. The binary format of the
data fork is identical regardless of the host platform.

## References

- Apple II File Type Note $E0/$0005, "DiskCopy disk image" — Matt Deatherage,
  Dave Lyons & Steve Christensen, May 1992
- Inside Macintosh: Devices (Sony Driver chapter, tag byte format)
