# StuffIt (.sit) — Archive Format Specification

## 1  Introduction

### 1.1  What is StuffIt?

StuffIt is a family of archive and compression formats that originated on
classic Macintosh systems in the late 1980s.  Files using the `.sit` extension
can contain one or more files and folders, each with both a **data fork** and
a **resource fork** — reflecting the dual-fork file system of classic Mac OS.
Metadata such as file type, creator code, and Finder flags are also preserved.

Over the years, two fundamentally different archive layouts have shared the
`.sit` suffix:

- **Classic (versions 1.x–4.x):**  A straightforward sequential format with
  fixed-size file headers followed by compressed fork data.  This layout dates
  from the original StuffIt application and was used through version 4.x.

- **SIT5 (version 5.x):**  A redesigned format with variable-length entry
  headers, a linked-list structure for navigation, and support for newer
  compression methods.  It was introduced with StuffIt 5.0 and uses a distinct
  80-byte ASCII magic string.

Despite sharing a file extension, the two formats are structurally
incompatible and must be detected and parsed independently.

### 1.2  Macintosh Fork Model

Classic Macintosh files have two forks:

- **Data fork:** The primary data content (equivalent to a file on other
  operating systems).
- **Resource fork:** Structured data used by the Mac OS for icons, dialog
  layouts, code segments, and other application resources.

StuffIt archives preserve both forks as independent compressed streams.  Each
fork has its own compression method, compressed length, uncompressed length,
and CRC.  Either fork may have zero length, in which case it is omitted from
the archive.

### 1.3  Purpose and Scope

This document is a complete, self-contained *format specification* for the
StuffIt archive container and the compression methods implemented within it.
It describes all the on-wire byte- and bit-level details needed to build a
fully compatible extractor for both the classic and SIT5 archive formats.

Methods 13 (LZSS + Huffman) and 15 (Arsenic / BWT + Arithmetic) are complex
enough to warrant their own dedicated specification documents.  This document
covers their integration into the archive container and refers to the separate
specs for their internal format details.

---

## 2  Format Overview

### 2.1  Byte Order

All multi-byte integers in both the classic and SIT5 formats are stored in
**big-endian** (most-significant byte first) order unless explicitly stated
otherwise.  This applies to header fields, lengths, offsets, and CRC values.

The only exception is the LZW compression method (method 2), which uses
**little-endian** bit packing within its compressed bitstream (see §9.4).

### 2.2  The Two Archive Layouts

The two StuffIt layouts can be summarized as follows:

| Aspect | Classic (1.x–4.x) | SIT5 (5.x) |
|--------|-------------------|-------------|
| Magic | 4-byte signature + `rLau` at offset 10 | 80-byte ASCII string |
| Header size | 22 bytes (fixed) | ≥100 bytes (variable) |
| Entry headers | 112 bytes (fixed) | Variable-length, linked list |
| Folder model | Start/end marker pairs (stack-based) | Flag bit + parent offset + child count |
| Navigation | Sequential (header after header) | Offset-based linked list |
| Entry count | Explicit `file_count` field | `declared_entry_count`, augmented by folder children |
| Supported methods | 0, 1, 2, 3, 5, 8, 13, 15 | 0, 5, 8, 13, 15 |

Both formats can appear embedded inside other containers (e.g., SEA
self-extractors).  An implementation should scan the input for the respective
magic sequences rather than assuming the archive starts at byte 0.

### 2.3  Detection Strategy

A robust extractor detects the format by scanning the input data:

1. **Classic:** Look for any of the 9 known 4-byte signatures at the start,
   with `rLau` at offset 10–13.
2. **SIT5:** Look for the 80-byte ASCII magic string starting with
   `"StuffIt (c)1997-"`.

If both patterns appear, prefer the earliest match.  If neither is found, the
input is not a StuffIt archive.

---

## 3  CRC-16 Integrity Check

### 3.1  Background

StuffIt uses a CRC-16 checksum to verify the integrity of both archive
headers and decompressed fork data.  The specific algorithm is a **reflected
CRC-16** with generator polynomial **0x8005** (CRC-16/IBM, also known as
CRC-16/ARC), *not* the CCITT CRC-16 polynomial 0x1021 that some historical
documentation incorrectly mentions.

The reflected form means the polynomial bits are processed
least-significant-bit first, and the lookup table is pre-computed using the
bit-reversed polynomial **0xA001**.

### 3.2  Parameters

| Parameter | Value |
|-----------|-------|
| Polynomial | 0x8005 (reflected: 0xA001) |
| Width | 16 bits |
| Initial value | 0x0000 |
| Final XOR | 0x0000 |
| Input reflection | Yes |
| Output reflection | Yes |

### 3.3  Byte-at-a-Time Update

Given a running CRC value and a new byte `b`, the update formula is:

```
crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
```

To compute the CRC of a buffer, iterate this formula over all bytes, starting
with `crc = 0`.

### 3.4  Lookup Table Generation

The 256-entry lookup table is pre-computed as follows: for each byte value
0–255, apply 8 iterations of reflected polynomial division using the
generating polynomial 0xA001.  The complete table is provided in Appendix A.

### 3.5  Where CRCs Are Used

| Context | What is checksummed | Notes |
|---------|-------------------|-------|
| Classic file header | First 110 bytes of the 112-byte header (CRC field at bytes 110–111 is zeroed before calculation) | Validates header integrity. |
| Classic fork data | Decompressed (post-decompression) bytes of each fork | Stored in the file header as `rsrc_crc` and `data_crc`. |
| SIT5 entry header 1 | The full header 1 (bytes 32–33 of the header zeroed before calculation) | Stored at bytes 32–33 of header 1. |
| SIT5 fork data | Decompressed bytes of each fork | Stored as `data_crc` and `rsrc_crc` in headers 1 and 2. |
| Method 15 exception | CRC fields may be zero and are **not validated** | Method 15 handles integrity internally. |

---

## 4  Classic Archive Format (Versions 1.x–4.x)

### 4.1  Identification

The classic format is identified by matching **two** magic values
simultaneously:

- **Bytes 0–3:** One of nine known 4-byte signatures.
- **Bytes 10–13:** Always the four ASCII bytes `rLau`.

The recognized signatures at offset 0 are:

| Signature | ASCII |
|-----------|-------|
| `0x53495421` | `SIT!` |
| `0x53543436` | `ST46` |
| `0x53543530` | `ST50` |
| `0x53543630` | `ST60` |
| `0x53543635` | `ST65` |
| `0x5354696E` | `STin` |
| `0x53546932` | `STi2` |
| `0x53546933` | `STi3` |
| `0x53546934` | `STi4` |

These signatures correspond to different versions and variants of the classic
StuffIt encoder, but the archive format itself is structurally identical
across all of them.

### 4.2  Main Archive Header (22 Bytes)

The archive begins with a 22-byte header:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | `magic1` | One of the 9 recognized signatures (§4.1). |
| 4 | 2 | `file_count` | Number of top-level entries in the archive. A folder (including all its nested contents) counts as one entry. Used to control the sequential iteration loop. See §4.6 and §4.7. |
| 6 | 4 | `total_size` | Total archive size in bytes (may be used for validation). |
| 10 | 4 | `magic2` | Always `rLau` (`0x724C6175`). |
| 14 | 1 | `version` | Version byte.  Semantics are unknown; preserved verbatim. |
| 15 | 7 | `unknown` | Seven bytes of unknown purpose.  Treated as opaque data. |

The first file/folder header begins immediately at offset 22.

### 4.3  File / Folder Header (Fixed 112 Bytes)

Each entry in the archive (whether a file or a folder marker) has a
fixed-size 112-byte header.  The header layout is:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `rsrc_method_raw` | Resource fork compression method byte (see §4.4 for encoding). |
| 1 | 1 | `data_method_raw` | Data fork compression method byte (same encoding). |
| 2 | 1 | `name_len` | Length of the file or folder name (0–63). |
| 3 | 63 | `name` | File/folder name in MacRoman encoding, **not** NUL-terminated. Unused bytes are padding. |
| 66 | 4 | `type` | Macintosh file type (4 ASCII characters, e.g., `TEXT`). |
| 70 | 4 | `creator` | Macintosh creator code (4 ASCII characters, e.g., `ttxt`). |
| 74 | 2 | `finder_flags` | Classic Finder flags (icon position, label, etc.). |
| 76 | 4 | `create_time` | Creation timestamp: seconds since the Mac epoch (1904-01-01 00:00:00 UTC). |
| 80 | 4 | `mod_time` | Modification timestamp (same epoch). |
| 84 | 4 | `rsrc_uncomp_len` | Uncompressed resource fork length in bytes. |
| 88 | 4 | `data_uncomp_len` | Uncompressed data fork length in bytes. |
| 92 | 4 | `rsrc_comp_len` | Compressed resource fork length in bytes. |
| 96 | 4 | `data_comp_len` | Compressed data fork length in bytes. |
| 100 | 2 | `rsrc_crc` | CRC-16 of the decompressed resource fork data (§3). |
| 102 | 2 | `data_crc` | CRC-16 of the decompressed data fork data (§3). |
| 104 | 6 | `unknown` | Six bytes of unknown purpose.  Stored verbatim. |
| 110 | 2 | `header_crc` | CRC-16 over the first 110 bytes of this header (§3). |

### 4.4  Method Byte Encoding

The `rsrc_method_raw` and `data_method_raw` bytes encode both the compression
method and special folder markers:

```
  Bits:  7  6  5  4  3  2  1  0
       +--------+--+-----------+
       | flags  |  | method ID |
       +--------+--+-----------+
```

| Value | Meaning |
|-------|---------|
| `0x20` (32) | **Folder start marker.** Pushes a new directory level onto the folder stack. |
| `0x21` (33) | **Folder end marker.** Pops one directory level from the folder stack. |
| Bit 4 (0x10) set | **Encryption flag.** Indicates the fork is encrypted (not supported). |
| Low nibble (bits 0–3) | **Compression method ID** (0–15).  Extracted as `method_raw & 0x0F`. |

**Important:** Entries where the high three bits (bits 7, 6, 5) are set
— i.e., `method_raw & 0xE0` is non-zero — and the value is not one of the
recognized folder markers (0x20, 0x21), are silently skipped during
extraction.  This allows future format extensions without breaking older
extractors.

### 4.5  Fork Data Layout

Immediately following each non-folder file header:

1. **Resource fork** compressed data: `rsrc_comp_len` bytes.
2. **Data fork** compressed data: `data_comp_len` bytes.

The next 112-byte header begins immediately after the last fork byte.

Either fork may be omitted if its **uncompressed** length is zero (the
compressed length should also be zero in this case).  An extractor should skip
zero-length forks rather than attempting to decompress them.

### 4.6  Folder Markers and Directory Structure

Classic StuffIt encodes folder hierarchy using paired start/end markers:

- **Folder start (0x20):** The name field gives the folder name.  The
  extractor pushes this name onto a directory stack.  No fork data follows
  (compressed lengths should be zero).

- **Folder end (0x21):** Pops one level from the directory stack.  No name
  or fork data is meaningful.

Folder markers appear in either the resource or data method byte — either one
being 0x20 or 0x21 triggers the corresponding action.

**`file_count` is a count of top-level entries** — a folder (open through
close, with any nesting inside) counts as exactly one top-level entry,
regardless of how many files or sub-folders it contains.  The folder-start
and folder-end markers themselves are not separately tallied; the folder
becomes "consumed" when its outermost folder-end is reached and the depth
returns to zero.  Files at the top level (depth 0) each count as one
top-level entry as well.

A flat archive of N files thus reports `file_count = N`.  An archive
consisting of a single root folder (regardless of how many files are inside)
reports `file_count = 1`.

### 4.7  Classic Iteration Rules

Extraction proceeds sequentially from the first header (offset 22).  The
iterator maintains a `depth` counter (initially 0) and a `done` counter of
top-level entries consumed.  At each step:

1. Read the 112-byte header at the current cursor position.
2. Check the method bytes:
   - If either is 0x20 (folder start): push folder name onto the stack,
     increment `depth`, advance cursor by 112, continue.
   - If either is 0x21 (folder end): if `depth > 0`, decrement `depth`;
     advance cursor by 112; if `depth` is now 0, increment `done` (a
     top-level folder has just been fully consumed); continue.
   - If `method_raw & 0xE0` is non-zero and neither 0x20 nor 0x21: skip
     this entry (advance cursor by 112); if `depth == 0`, increment `done`;
     continue.
3. For a regular file: extract the low nibble of each method byte as the
   compression method ID.  Read fork metadata from the header.
4. Locate fork data: resource fork starts at `cursor + 112`, data fork starts
   at `cursor + 112 + rsrc_comp_len`.
5. Advance cursor to `cursor + 112 + rsrc_comp_len + data_comp_len`.
6. Build the full file path by prepending the current folder stack.
7. Decompress each non-empty fork, verify CRC (except for method 15).
8. If `depth == 0`, increment `done` (a top-level file has been consumed).
   Files nested inside a folder do not increment `done`.

**Folder stack:** The implementation maintains a stack of up to 10 nesting
levels, with each level holding a name of up to 63 bytes (matching the
maximum name length in the header).  Archives with deeper nesting than 10
levels may not be fully supported.

Continue iteration until `done == file_count` (all top-level entries have
been consumed) or the cursor exhausts the available data.  Validate that
compressed fork spans lie entirely within the archive bounds.

---

## 5  SIT5 Archive Format (Version 5.x)

### 5.1  Identification

SIT5 archives begin with an 80-byte ASCII magic string:

```
StuffIt (c)1997-YYYY Aladdin Systems, Inc., http://www.aladdinsys.com/StuffIt/\r\n
```

where `YYYY` represents year digits that vary between archives.

**Validation rule:** The magic is verified by checking two substrings:

- **Bytes 0–15:** Must equal `"StuffIt (c)1997-"` (16 bytes).
- **Bytes 20–77:** Must equal `" Aladdin Systems, Inc., http://www.aladdinsys.com/StuffIt/"` (58 bytes).

Bytes 16–19 (the year digits) and bytes 78–79 (the trailing CR LF) are
**not validated**.  This allows archives with different copyright-year stamps
to be accepted.

### 5.2  Top Header

The SIT5 top header extends beyond the 80-byte magic.  The minimum archive
size is 100 bytes.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 80 | `magic` | ASCII magic string (§5.1). |
| 80 | 4 | `unknown0` | Purpose not interpreted. |
| 84 | 4 | `total_size` | Total archive size in bytes. |
| 88 | 4 | `first_entry_offset` | Byte offset of the first entry header.  Historical field; **not used** for traversal by the implementation. |
| 92 | 2 | `declared_entry_count` | Nominal number of top-level entry headers.  Folder entries augment this count with their declared child counts. |
| 94 | 4 | `initial_cursor` | Starting cursor position for entry traversal.  This is the field actually used to begin iteration (not `first_entry_offset`). |
| 98 | … | `reserved` | Remaining bytes are not currently parsed. |

**Note:** The `first_entry_offset` field at offset 88 is a historical
artifact.  The implementation exclusively uses `initial_cursor` (offset 94)
to begin traversal.  The two values are usually equal but may differ.

### 5.3  Entry Header (Header 1)

Every entry (file or folder) in a SIT5 archive begins with a primary header
("header 1") located at the current traversal cursor.  This header contains
navigation pointers, metadata, and data fork information.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | `id` | Entry magic.  Must be `0xA5A5A5A5`. |
| 4 | 1 | `version` | Entry header version.  Only version 1 is supported. |
| 5 | 1 | `unknown1` | Unused. |
| 6 | 2 | `header_size` | Size of header 1 in bytes (from offset 0).  Used to locate header 2. |
| 8 | 1 | `unknown2` | Unused. |
| 9 | 1 | `flags` | Bit field (see below). |
| 10 | 4 | `create_time` | Creation timestamp (Mac epoch seconds). |
| 14 | 4 | `mod_time` | Modification timestamp (Mac epoch seconds). |
| 18 | 4 | `prev_offset` | Byte offset of the previous entry header (for backward traversal). |
| 22 | 4 | `next_offset` | Byte offset of the next entry header (for forward traversal). |
| 26 | 4 | `parent_offset` | Byte offset of the parent folder's header 1.  Zero for top-level entries. |
| 30 | 2 | `name_len` | Length of the entry name in bytes. |
| 32 | 2 | `header_crc` | CRC-16 over all of header 1 with these two bytes (32–33) zeroed before calculation (§3). |
| 34 | 4 | `data_uncomp_len` | Uncompressed data fork length.  `0xFFFFFFFF` is a special marker (see §5.6). |
| 38 | 4 | `data_comp_len` | Compressed data fork length. |
| 42 | 2 | `data_crc` | CRC-16 of the decompressed data fork.  May be zero for method 15. |
| 44 | 2 | `unknown3` | Unused. |
| 46 | 1 | `data_method` | Compression method for the data fork.  Low nibble (bits 0–3) is the method ID.  High bits encode the encryption flag (same layout as classic). |
| 47 | 1 | `data_pass_len` | Password blob length.  Non-zero with encryption flag set is unsupported. |
| 48 | M | `name` | Entry name (M = `name_len` bytes).  Encoding is ambiguous (UTF-8 or MacRoman historically); treat as raw bytes.  **Note:** the name always begins at byte 48; for the only supported case (non-encrypted, `data_pass_len == 0`) there is no password blob preceding it (see *Folder entries* below for the byte 46–47 reinterpretation). |
| 48+M | … | (tail) | Optional comment and trailing fields.  `header_size` defines the total extent of header 1. |

#### Flags byte (offset 9)

| Bit | Mask | Meaning |
|-----|------|---------|
| 5 | 0x20 | **Encrypted.** Entry is encrypted (not supported). |
| 6 | 0x40 | **Folder.** Entry is a folder, not a file. |
| Others | — | Unknown; ignored. |

#### Folder entries

For folder entries (flags bit 6 set), bytes 46–47 of header 1 have a
different interpretation: instead of `data_method` and `data_pass_len`, they
form a 2-byte **number_of_contained_files** (big-endian).  This child count
is added to the running entry count during traversal, because the top-level
`declared_entry_count` does not account for files nested inside folders.

### 5.4  Secondary Header (Header 2)

Header 2 begins immediately at byte offset `entry_offset + header_size`
(i.e., right after header 1).  It contains Macintosh metadata and, optionally,
resource fork information.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 2 | `flags2` | Bit 0 (0x01): resource fork is present. Other bits: unknown. |
| 2 | 2 | `unknown_a` | Ignored. |
| 4 | 4 | `file_type` | Macintosh 4-character file type. |
| 8 | 4 | `file_creator` | Macintosh 4-character creator code. |
| 12 | 2 | `finder_flags` | Finder flags. |
| 14 | 2 | `unknown_b` | Ignored. |
| 16 | 4 | `maybe_date` | Observed to sometimes contain a date in later versions. |
| 20 | 12 | `unknown_c` | Ignored. |

At this point, an additional skip is applied based on header 1's version
field:

- **Version 1:** Skip 4 additional bytes (`unknown_d`) beyond offset 32.
  Resource fork fields (if present) begin at offset 36.
- **Other versions:** Resource fork fields begin at offset 32.

#### Resource Fork Fields (conditional)

These fields are present only when `flags2` bit 0 is set:

| Relative Offset | Size | Field | Description |
|-----------------|------|-------|-------------|
| +0 | 4 | `rsrc_uncomp_len` | Uncompressed resource fork length. |
| +4 | 4 | `rsrc_comp_len` | Compressed resource fork length. |
| +8 | 2 | `rsrc_crc` | CRC-16 of the decompressed resource fork.  May be zero for method 15. |
| +10 | 2 | `unknown_e` | Ignored. |
| +12 | 1 | `rsrc_method` | Compression method (low nibble = method ID). |
| +13 | 1 | `rsrc_pass_len` | Resource fork password blob length.  Non-zero is unsupported. |

### 5.5  Fork Data Layout

The compressed fork data follows header 2.  When a resource fork is present,
its compressed data comes **first**, immediately followed by the compressed
data fork:

```
┌──────────┐
│ Header 1 │
├──────────┤
│ Header 2 │
├──────────┤
│ Resource │  ← rsrc_comp_len bytes (only if resource fork present)
│ fork     │
├──────────┤
│ Data     │  ← data_comp_len bytes
│ fork     │
└──────────┘
```

The **next entry** is located at the `next_offset` from header 1, *not* by
scanning forward past the fork data.  This is an important difference from
the classic format.

### 5.6  Special Markers

Entries with `data_uncomp_len == 0xFFFFFFFF` are special markers — they do
not represent extractable files or folders.  Both folder entries and
non-folder entries can have this marker value.

**Important:** Marker entries do **not** carry a header 2 block.  The marker
check must be performed immediately after reading header 1, **before** any
attempt to access header 2.  A marker entry at the end of an archive may have
no bytes following header 1, so accessing header 2 would read past the
archive boundary.

- **Folder with `0xFFFFFFFF`:** The entry is skipped.  The running entry
  count is incremented by 1 (as a compensating adjustment, since the entry
  itself was already counted).  Traversal advances to the byte offset
  immediately after header 1 (at `entry_offset + header_size`), and the
  next entry is processed from there.

- **Non-folder with `0xFFFFFFFF`:** The entry is similarly skipped, and
  traversal advances past header 1 to find the next entry.  The remaining
  entry count is **not** decremented (only regular file entries decrement
  it).

### 5.7  Iteration Rules

Traversal is fundamentally **sequential** (depth-first) even though entries
contain linked-list pointers:

1. **Initialize:** Set the cursor to `initial_cursor` (offset 94 of the top
   header).  Set the remaining entry count to `declared_entry_count`
   (offset 92).

2. **Read header 1** at the cursor.  Validate the `0xA5A5A5A5` magic, version
   byte, and header CRC.  **Check for the `0xFFFFFFFF` special marker
   (§5.6) before accessing header 2.**

3. **Folder entries** (flags bit 6 set):
   - If `data_uncomp_len == 0xFFFFFFFF`: skip (adjust count, advance past
     header 1).  Do not access header 2.
   - Otherwise: parse header 2, record the folder in a directory map (up to
     32 entries), storing its offset and computed path.  Add the folder's
     child count to the remaining entry count.  Advance cursor past **both**
     header 1 **and** header 2 (into the folder's children) and recurse.

4. **File entries:**
   - If `data_uncomp_len == 0xFFFFFFFF`: skip (advance past header 1).
     Do not access header 2.
   - Otherwise: parse header 2, extract fork metadata, decompress forks.
     Decrement the remaining entry count.  Advance cursor past the fork
     data (to `payload_start + rsrc_comp_len + data_comp_len`).

5. Repeat until the remaining entry count reaches zero.

**Path construction:** File paths are built by looking up `parent_offset` in
the directory map (a linear scan of up to 32 previously seen folder entries).
The folder's stored path is prepended to the entry name.

**Encrypted entries:** Entries with flags bit 5 (0x20) set and non-zero
password lengths in the method fields are rejected as unsupported.

---

## 6  Compression Methods

### 6.1  Method Table

Both archive formats identify compression methods by a numeric ID stored in
the low 4 bits of the method byte.  The following methods are defined:

| ID | Name | Status | Description |
|----|------|--------|-------------|
| 0 | None | Implemented | Raw copy — no compression. |
| 1 | RLE90 | Implemented | Escape-based run-length encoding. |
| 2 | LZW | Implemented | StuffIt variant of LZW (14-bit max, block mode). |
| 3 | Static Huffman | Implemented | Original Raymond Lau (1987) per-byte static Huffman.  See `sit3.md`. |
| 5 | LZAH | Not implemented | Used by SIT5; not publicly documented. |
| 8 | LZMW (Miller–Wegman) | Not implemented | Used by SIT5; not publicly documented. |
| 13 | SIT13 | Implemented | LZSS + dual Huffman trees.  See separate specification. |
| 14 | Unknown | Not implemented | Reserved / unknown purpose. |
| 15 | SIT15 ("Arsenic") | Implemented | BWT + arithmetic coding.  See separate specification. |

Encountering an unsupported method ID is a fatal error — the fork cannot be
decompressed.

### 6.2  Fork Processing Order

When extracting a file entry, forks are processed in a defined order:

1. **Data fork first** (if its uncompressed length is non-zero).
2. **Resource fork second** (if present and non-zero length).

Each fork is decompressed independently using its own method.  A running CRC
is computed over the decompressed output and verified against the stored CRC
at the end of the fork — except for method 15, which handles integrity
checking internally and whose CRC fields may be zero.

### 6.3  CRC Verification Rule

After fully decompressing a fork, the computed CRC-16 over the decompressed
bytes must match the CRC stored in the entry header.  A mismatch indicates
data corruption and is treated as a fatal error.

**Exception:** For method 15 (Arsenic), the CRC field in the container header
may be zero and is **not verified** by the container layer.  Method 15 has its
own internal integrity mechanisms.

---

## 7  Method 0: None (Raw Copy)

Method 0 is the simplest: the fork data is stored uncompressed.  The
compressed bytes are an exact copy of the original fork content.

**Decompression:** Copy exactly `comp_len` bytes from the compressed data to
the output buffer.  The total number of decompressed bytes must equal the
header's `uncomp_len` field.

CRC is computed over the output bytes as they are copied.

---

## 8  Method 1: RLE90 (Escape Run-Length Encoding)

### 8.1  Background

RLE90 is a simple run-length encoding scheme that uses the byte value `0x90`
as an escape marker.  It is one of the oldest compression methods in StuffIt
and provides modest compression for data with many repeated byte sequences.

The scheme is straightforward: most bytes pass through literally, but when a
run of repeated bytes occurs, the encoder outputs the byte once, then emits
the escape marker `0x90` followed by a count.  The literal value `0x90` itself
is encoded as the two-byte sequence `0x90 0x00`.

### 8.2  State

The decoder maintains a single state variable:

* `last_byte` — the most recently output literal byte (initialized to 0).

### 8.3  Algorithm

Process the compressed input byte-by-byte:

1. **Read the next byte `b`** from the compressed stream.

2. **If `b ≠ 0x90` (not the escape marker):**
   - Output `b` as a literal.
   - Set `last_byte = b`.
   - Continue.

3. **If `b == 0x90` (escape marker):**
   - Read the next byte `n` (the count byte).  If the stream is exhausted,
     this is a truncation error.
   - **If `n == 0`:** Output a literal `0x90` byte.  (The `last_byte`
     variable is **not** updated — this is significant for subsequent repeat
     sequences.)
   - **If `n == 1`:** Output nothing.  This represents zero additional copies
     of `last_byte` (the original byte was already emitted).
   - **If `n > 1`:** Output `last_byte` repeated `(n − 1)` additional times.
     The total run, including the original literal that preceded the escape
     marker, is `n` copies of `last_byte`.

4. Continue until the required uncompressed length has been produced.  Ignore
   any trailing input padding.

### 8.4  Edge Cases

- **Repeat before any literal:** If a repeat marker (0x90 followed by n > 0)
  appears before any literal byte has been output, the decoder repeats the
  initial `last_byte` value of 0.  Such streams are tolerated for
  compatibility with legacy encoders.

- **Encoding literal 0x90:** The byte value 0x90 cannot appear literally in
  the compressed stream because it is the escape marker.  It must always be
  encoded as the two-byte sequence `0x90 0x00`.  The count byte for a repeat
  never encodes a repeat of the literal 0x90 itself.

### 8.5  Example

Consider the compressed sequence: `41 42 42 90 05 90 00 43`

| Input | Action | Output |
|-------|--------|--------|
| `0x41` | Literal `'A'` | `A` |
| `0x42` | Literal `'B'`, sets `last_byte = 0x42` | `B` |
| `0x42` | Literal `'B'` (same byte again) | `B` |
| `0x90 0x05` | Repeat `last_byte` (0x42 = `'B'`) 4 more times (5 − 1) | `BBBB` |
| `0x90 0x00` | Literal `0x90` | `0x90` |
| `0x43` | Literal `'C'` | `C` |

Total output: `A B B B B B B 0x90 C` (9 bytes).

---

## 9  Method 2: LZW (StuffIt Variant)

### 9.1  Background

LZW (Lempel–Ziv–Welch) is a dictionary-based compression algorithm that
builds a codebook of previously seen byte sequences on the fly.  It was widely
used in the 1980s and 1990s, appearing in the UNIX `compress` utility,
GIF image format, and TIFF files.

StuffIt's LZW variant closely resembles the UNIX `compress` implementation
(often called "LZC"), with a maximum code width of 14 bits, little-endian bit
packing, and a block-mode clear code.  The key difference from textbook LZW
is the block alignment behavior when processing clear codes.

### 9.2  Parameters

| Parameter | Value |
|-----------|-------|
| Initial codes 0–255 | Single-byte identity entries. |
| Code 256 | **Clear code** — resets the dictionary. |
| First user code | 257 (the next free dictionary slot). |
| Initial code width | 9 bits. |
| Maximum code width | 14 bits (dictionary capacity: 16,384 entries). |
| Previous code (initial) | −1 (no previous code). |
| Bit packing | Little-endian (LSB-first within the byte stream). |

There is no explicit "stop" code.  End of data is determined either by
exhausting the compressed input or by producing the required uncompressed
length.

### 9.3  Dictionary Structure

The dictionary is represented as a tree of entries, each extending a parent
entry by one byte.  For efficient implementation, a struct-of-arrays layout
with four parallel arrays is used (all arrays have 16,384 entries):

| Array | Type | Description |
|-------|------|-------------|
| `prev_code[i]` | uint16 | Back-link to the parent code.  Root entries (0–255) use a sentinel value (e.g., `UINT16_MAX`). |
| `suffix[i]` | uint8 | The byte appended by this entry. |
| `head[i]` | uint8 | The first byte of the entire chain (precomputed for fast KwKwK resolution). |
| `chain_len[i]` | uint16 | Length of the expansion string (root entries = 1). |

**Root entries (codes 0–255):** These are initialized as identity mappings:
`prev_code[i] = UINT16_MAX`, `suffix[i] = i`, `head[i] = i`,
`chain_len[i] = 1`.

### 9.4  Bit Packing (Little-Endian)

Unlike the big-endian byte conventions of the archive container, the LZW
compressed bitstream uses **little-endian** bit packing.  Each code is read
from the least-significant bits of successive bytes.

A practical implementation reads a 32-bit little-endian word starting at the
current byte offset, then right-shifts by the bit offset within that word
and masks to the current code width:

```
byte_off = bit_pos / 8
shift    = bit_pos % 8
acc      = read_le32(src + byte_off)
code     = (acc >> shift) & ((1 << code_bits) - 1)
bit_pos += code_bits
```

### 9.5  Decoding Loop

The core LZW decoding loop:

1. **Read the next code** `c` using the current code width.  If the input is
   exhausted, stop.

2. **If `c == 256` (Clear code):** Perform the clear procedure (§9.6) and
   continue to the next code.

3. **If this is the first code after a reset** (previous code is −1 / invalid):
   - The code must be < 256 (a single byte).
   - Output that byte directly.
   - Set previous code = `c`.
   - Continue.

4. **Determine first byte** of the current expansion:
   - If `c < tbl_next` (code exists in dictionary): `first_byte = head[c]`.
   - If `c == tbl_next` (KwKwK case, §9.8): `first_byte = head[prev]`.

5. **Add a new dictionary entry:** `(prev, first_byte)` — the previous code
   extended by the first byte of the current expansion (§9.7).

6. **Expand the code:**
   - If `c < tbl_next`: Walk the dictionary chain from code `c` backwards,
     collecting bytes into a staging buffer.
   - If `c == tbl_next` (KwKwK): Expand the previous code's chain, then
     append `first_byte`.

7. Output the expanded bytes. Set previous code = `c`. Continue.

### 9.6  Clear Code and Block Alignment

When code 256 (Clear) is encountered, the dictionary is reset:

1. **Skip remaining codes in the current 8-symbol block.**  LZW codes are
   grouped into blocks of 8.  After a clear code, any remaining unused code
   slots in the current 8-code block are discarded.  The number of bits to
   skip is:

   ```
   bits_to_skip = code_bits × (8 − (block_count & 7))
   ```

   where `code_bits` is the code width **before** the reset, and
   `block_count` is the number of codes read since the last clear (or since
   the start of the stream).

2. **Reset the dictionary:** Set `tbl_next = 257`, `code_bits = 9`,
   `block_count = 0`, `prev = −1`.

This block alignment is a quirk inherited from the original UNIX `compress`
implementation in block mode.  It ensures that the decoder's bit position
stays synchronized with the encoder's block boundaries.

### 9.7  Code Width Expansion

The code width starts at 9 bits and increases as the dictionary grows.  The
trigger for widening is when the dictionary size (`tbl_next`) reaches a
power of two:

```
if tbl_next < 16384 and (tbl_next & (tbl_next - 1)) == 0:
    code_bits += 1
```

The widths progress through 9, 10, 11, 12, 13, 14 as the dictionary reaches
sizes 512, 1024, 2048, 4096, 8192, 16384 respectively.  At 16,384 entries
(14-bit codes), no further widening occurs and new entries cannot be added.

### 9.8  The KwKwK Case

A subtle edge case in LZW occurs when the encoder produces a code that
references the dictionary entry that is *about to be added* (i.e., the code
equals `tbl_next`).  This happens when the input contains a pattern like
`KwKwK` — a string followed by itself with an overlapping prefix.

When `code == tbl_next`:

1. The code doesn't yet exist in the dictionary.
2. The expansion is: the previous code's expansion, followed by the first
   byte of the previous code's expansion.
3. This is logically equivalent to: the dictionary entry that *would* have
   been added (previous code + first byte of current expansion) happens to
   be exactly the code that was emitted.

The `head[]` array in the dictionary structure makes resolving this case
efficient — `head[prev]` gives the first byte without needing to walk the
chain.

### 9.9  Staging Buffer

LZW expansion produces strings in **reverse order** because the dictionary
chain is walked backwards (from the code to its root).  A staging buffer
(16,384 bytes, matching the maximum dictionary capacity) holds the reversed
expansion, which is then consumed by the caller.

**Partial consumption** is supported: if the caller's output buffer cannot
hold the entire expanded string, the staging buffer tracks a read position
(`stage_rd`) and total length (`stage_len`).  The next call to the decoder
drains the remaining staged bytes before reading the next code.

---

## 10  Method 13: LZSS + Dual Huffman Trees

Method 13 is a dictionary-based compression scheme that combines LZSS
(Lempel–Ziv–Storer–Szymanski) with Huffman entropy coding.  It uses a 64 KiB
sliding window and three Huffman trees — two alternating literal/length trees
and one distance tree.

Due to its complexity, method 13 has its own dedicated format specification
document (`sit13.md`).  The key integration points with the archive container
are:

- **Input:** The compressed fork data bytes (from the archive's fork data
  region) are passed directly to the method 13 decompressor.
- **Output:** The decompressor produces the original uncompressed fork bytes.
- **Termination:** Decompression stops after producing exactly `uncomp_len`
  bytes (as specified in the entry header).  There is no in-band end-of-stream
  marker.
- **CRC:** The container layer computes and verifies the CRC over the
  decompressed output using the standard CRC-16 algorithm (§3).
- **Streaming:** Decompression is streaming — the caller repeatedly requests
  chunks of output bytes.

---

## 11  Method 15: Arsenic (BWT + Arithmetic Coding)

Method 15, internally named "Arsenic", is a block-based compression scheme
that uses the Burrows–Wheeler Transform, Move-To-Front coding, run-length
encoding, and adaptive arithmetic coding — closely mirroring the bzip2
pipeline.

Due to its complexity, method 15 has its own dedicated format specification
document (`sit15.md`).  The key integration points with the archive container
are:

- **Input:** The compressed fork data bytes are passed to the Arsenic
  decompressor.
- **Output:** The decompressor produces the original uncompressed fork bytes.
- **CRC skip:** The CRC fields in the container header (both `data_crc` and
  `rsrc_crc`) **may be zero** for method 15 forks.  The container layer skips
  CRC verification for method 15 because Arsenic has its own internal
  integrity checking (a 32-bit CRC in the stream footer, though it is
  typically not verified by decompressors either).
- **Streaming:** Decompression is streaming, with a pull-based demand-driven
  interface.

---

## 12  Unsupported Methods

The following compression methods are defined in the StuffIt format but are
not publicly documented to a sufficient level for implementation:

| ID | Name | Notes |
|----|------|-------|
| 3 | Static Huffman | Historical classic method.  Rarely encountered. |
| 5 | LZAH | Used by SIT5. |
| 8 | LZMW (Miller–Wegman) | Used by SIT5. |
| 14 | Unknown | Reserved; purpose unclear. |

Encountering any of these methods should produce a clear error message
indicating an unsupported compression method.

---

## 13  Error Conditions

A conforming implementation must treat the following as fatal errors:

### 13.1  Archive-Level Errors

- **No recognized magic:** The input does not contain a valid classic or SIT5
  signature.
- **Archive too small:** The input is shorter than the minimum header size
  (22 bytes for classic, 100 bytes for SIT5).
- **Header overrun:** An entry header extends beyond the archive bounds.
- **Fork overrun:** A compressed fork's byte span extends beyond the archive
  bounds.
- **Invalid entry magic (SIT5):** Header 1's `id` field is not `0xA5A5A5A5`.
- **Unsupported entry version (SIT5):** Header 1's version is not 1.
- **Header CRC mismatch:** The computed CRC of a header does not match the
  stored value.

### 13.2  Decompression Errors

- **Unsupported method:** The compression method ID is not one of the
  implemented methods (0, 1, 2, 13, 15).
- **Encrypted entry:** The encryption flag is set with non-zero password
  lengths.
- **Fork CRC mismatch:** The computed CRC-16 of the decompressed fork data
  does not match the stored CRC (except for method 15).
- **Premature input exhaustion:** The compressed data stream ends before
  the full uncompressed output has been produced.
- **Invalid LZW code:** A code references a dictionary entry that does not
  exist (and is not the valid KwKwK case).
- **LZW truncation:** Cannot read a complete code from the bitstream.
- **Method-specific errors:** See the separate specifications for methods 13
  and 15.

---

## 14  Implementation Guidance

This section provides practical advice for implementers.  It is not part of
the format specification per se, but captures important patterns and lessons.

### 14.1  Archive Ingestion

Both classic and SIT5 formats require **random access** to arbitrary byte
offsets within the archive (for header parsing, fork data retrieval, and
parent offset resolution in SIT5).  The recommended approach is to read
("slurp") the entire archive into a contiguous in-memory buffer.

A practical ingestion strategy:

1. Allocate an initial buffer (e.g., 4096 bytes or twice the prefix size).
2. Read from the input stream until exhausted, doubling the buffer capacity
   as needed.
3. The final buffer contains the complete archive image.

This approach supports embedded archives (archives inside self-extractors or
other containers) because the archive offset can be any position within the
buffer.

### 14.2  Embedded Archive Detection

StuffIt archives may be embedded within larger files (e.g., SEA self-
extractors).  An implementation should scan the entire input buffer for the
magic sequences rather than assuming the archive starts at byte 0.  Prefer
the earliest match.

### 14.3  Extraction Pipeline

For each file entry, the extraction pipeline is:

1. **Open the entry:** Parse the header, determine fork count and methods.
2. **For each non-empty fork (data first, then resource):**
   a. Initialize the appropriate decompressor based on the method ID.
   b. Decompress in streaming fashion, accumulating a running CRC.
   c. At end-of-fork, verify CRC (unless method 15).
3. **Expose metadata** to the caller: filename (with folder path), file type,
   creator, Finder flags, fork type, and uncompressed length.

### 14.4  Decoder Dispatch

A per-fork decoder is initialized based on the method ID:

| Method | Initialization | Streaming |
|--------|---------------|-----------|
| 0 (Raw) | Point to compressed data offset | Copy bytes directly |
| 1 (RLE90) | Reset `last_byte = 0`, `run_left = 0` | Byte-at-a-time state machine |
| 2 (LZW) | Create LZW context, initialize dictionary | Produces bytes via staging buffer |
| 13 | Delegate to `sit13_init()` | `sit13_read()` produces chunks |
| 15 | Delegate to `sit15_init()`, set CRC-skip flag | `sit15_read()` produces chunks |

All decoders share a common streaming interface: the caller requests up to
N bytes, the decoder produces as many as possible (returning the count), and
returns 0 at end-of-fork.

### 14.5  Memory Considerations

The archive buffer is the primary memory cost at the container level.  For
the compression methods:

- **Methods 0, 1:** No additional allocation beyond the archive buffer.
- **Method 2 (LZW):** ~128 KiB for the dictionary arrays (4 arrays ×
  16,384 entries) plus a 16 KiB staging buffer.
- **Methods 13, 15:** Allocated internally by their respective decompressors.
  Method 15 in particular can require up to 80 MiB for large block sizes
  (see the Arsenic specification).

---

## Appendix A: CRC-16 Lookup Table

The complete 256-entry CRC-16 lookup table (reflected polynomial 0xA001):

```
0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
```

To generate this table programmatically, for each byte value `i` (0–255):

```
entry = i
for bit in 0..7:
    if entry & 1:
        entry = (entry >> 1) ^ 0xA001
    else:
        entry >>= 1
table[i] = entry
```

---

## Appendix B: Complete Classic Extraction Walkthrough

This appendix traces the full extraction of a classic StuffIt archive,
tying together all the sections above.

1. **Ingest** (§14.1): Read the entire input into a contiguous buffer.

2. **Detect** (§4.1): Scan for a 4-byte signature at offset 0 with `rLau` at
   offset 10.  If found, this is a classic archive.

3. **Parse main header** (§4.2): Read `file_count` (offset 4), note
   `total_size` (offset 6).  Set cursor = 22.

4. **Iterate entries** (§4.7):
   - Read the 112-byte header at the cursor.
   - Check method bytes for folder markers:
     - 0x20 → push folder name, advance cursor by 112.
     - 0x21 → pop folder, advance cursor by 112.
     - Unknown high bits → skip, advance by 112.
   - For a regular file:
     a. Extract compression methods: `rsrc_algo = hdr[0] & 0x0F`,
        `data_algo = hdr[1] & 0x0F`.
     b. Extract name, type, creator, flags from the header.
     c. Build full path from the folder stack.
     d. Locate fork data after the header.

5. **Decompress each fork** (§6.2, §7–§11):
   - Initialize the decoder for the fork's method.
   - Stream decompressed bytes, accumulating CRC.
   - Verify CRC at end-of-fork (unless method 15).

6. **Advance** to the next entry and repeat until `file_count` entries have
   been consumed.

---

## Appendix C: Complete SIT5 Extraction Walkthrough

1. **Ingest** (§14.1): Read the entire input into a contiguous buffer.

2. **Detect** (§5.1): Scan for the 80-byte SIT5 magic string.

3. **Parse top header** (§5.2): Read `declared_entry_count` (offset 92) and
   `initial_cursor` (offset 94).

4. **Iterate entries** (§5.7): Set cursor = `initial_cursor`, remaining =
   `declared_entry_count`.

5. **For each entry:**
   a. Validate header 1: check magic (`0xA5A5A5A5`), version (1), and CRC.
   b. If folder: record in directory map, add child count to remaining,
      advance into children.
   c. If special marker (`data_uncomp_len == 0xFFFFFFFF`): skip.
   d. If regular file:
      - Parse header 2 for metadata and resource fork info.
      - Build path from parent_offset via the directory map.
      - Locate fork data (resource first if present, then data).
      - Decompress each fork, verify CRC (except method 15).
      - Advance cursor past fork data.

6. Repeat until remaining entry count reaches zero.

---

## License

This specification text is provided under the same MIT license as the project
source code.
