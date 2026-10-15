# QCOW2 Disk Image Format

QCOW2 ("QEMU Copy-On-Write, version 2") is the native disk image format of the QEMU virtualiser [1]. It is <!-- lint-allow: QEMU -->
described here only because disk images in it circulate (installed Mac OS X systems among them) and are read so
that they can be used as they are; that compatibility is the only reason QEMU is named [1]. This page covers what <!-- lint-allow: QEMU -->
a reader needs: the header, the two-level cluster map, compressed clusters, and the features that change what
the guest sees. Snapshots, refcounts and bitmaps are write-side metadata and are summarised only.

## 1. Identification (magic, sizes)

### 1.1 What QCOW2 is

A QCOW2 file divides both itself and the virtual disk it holds into **clusters** of one size, a power of two
from 512 bytes to 2 MB (QEMU's own ceiling; the format allows larger) [1]. A two-level table maps each guest <!-- lint-allow: QEMU -->
cluster to where its bytes are: a host cluster, a compressed run of bytes, all zeros, or nowhere. Space is
allocated as the guest writes, so the file holds only what was written — a 10 GB disk with 1 GB on it is a file of
about 1 GB.

All numbers are **big-endian** [1].

### 1.2 Magic and versions

| Offset | Size | Value |
|---|---|---|
| 0 | 4 | `51 46 49 FB` — `"QFI\xfb"` |
| 4 | 4 | version: 2 or 3 |

Version 2 (written by older tools, and with `compat=0.10`) has a 72-byte header and no feature bits; version 3
adds feature bitmasks and a header length [1]. A file name means nothing (images are commonly called `.img`).

### 1.3 Sizes

- Cluster size `1 << cluster_bits`, `cluster_bits` 9..21 in practice; at least 14 with extended L2 entries [1].
- Virtual size: any byte count; it need not be a whole number of clusters, in which case the last guest cluster is
  partly outside the disk.
- Host offsets in the tables are 56-bit (bits 9–55), so the file can address 64 PB [1].

## 2. Layout tables

### 2.1 Header

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `QFI\xfb` |
| 4 | 4 | version | 2 or 3 |
| 8 | 8 | backing_file_offset | 0: no backing file |
| 16 | 4 | backing_file_size | name length, ≤ 1023 |
| 20 | 4 | cluster_bits | |
| 24 | 8 | size | virtual disk size, bytes |
| 32 | 4 | crypt_method | 0 none, 1 AES, 2 LUKS |
| 36 | 4 | l1_size | entries in the active L1 table |
| 40 | 8 | l1_table_offset | cluster-aligned |
| 48 | 8 | refcount_table_offset | cluster-aligned |
| 56 | 4 | refcount_table_clusters | |
| 60 | 4 | nb_snapshots | |
| 64 | 8 | snapshots_offset | |
| 72 | 8 | incompatible_features | v3 only (§2.2) |
| 80 | 8 | compatible_features | v3; bit 0 lazy refcounts |
| 88 | 8 | autoclear_features | v3; bit 0 bitmaps, bit 1 raw external data |
| 96 | 4 | refcount_order | v3; refcount width `1 << order` bits (v2: 4) |
| 100 | 4 | header_length | v3; ≥ 104, a multiple of 8 (v2: 72) |
| 104 | 1 | compression_type | present when header_length > 104: 0 deflate, 1 zstd |

A field past `header_length` is absent and reads as zero [1]. After the header come **header extensions**, each a
4-byte type, a 4-byte length and the data padded to 8 bytes, ending with type 0: backing file format
(`0xE2792ACA`), feature name table (`0x6803F857`), bitmaps (`0x23852875`), full disk encryption header
(`0x0537BE77`), external data file name (`0x44415441`); unknown types may be skipped [1]. None is needed to read the
data of an image without a backing file. A backing file's name, when there is one, is stored at
`backing_file_offset` in the rest of the first cluster [1].

### 2.2 Incompatible feature bits

| Bit | Name | Effect on a reader |
|---|---|---|
| 0 | dirty | refcounts may be stale — the L1/L2 map is still right; harmless to a reader |
| 1 | corrupt | some structure may be damaged; must not be written; may be read with care |
| 2 | external data file | guest data is in another file, at host offset = guest offset |
| 3 | compression type | `compression_type` is non-zero (zstd) |
| 4 | extended L2 entries | 128-bit L2 entries with 32 subclusters (§2.5) |
| 5–63 | reserved | an implementation must refuse an image with an unknown bit set |

### 2.3 L1 table

`l1_size` 64-bit entries, contiguous at `l1_table_offset` [1]:

| Bits | Meaning |
|---|---|
| 0–8 | reserved, 0 |
| 9–55 | host offset of an L2 table (cluster-aligned); 0: every cluster the table would map is unallocated |
| 56–62 | reserved, 0 |
| 63 | "copied": the L2 table's refcount is exactly one (write-side) |

The active L1 table may be longer than the virtual size needs; only the first
`ceil(size / (l2_entries * cluster_size))` entries map the disk.

### 2.4 L2 tables and cluster descriptors

An L2 table is exactly one cluster: `cluster_size / 8` entries (`/ 16` with extended entries) [1].

| Bits | Meaning |
|---|---|
| 0–61 | cluster descriptor (standard or compressed) |
| 62 | 0 standard, 1 compressed |
| 63 | "copied" (write-side) |

**Standard descriptor:**

| Bits | Meaning |
|---|---|
| 0 | reads as zeros (v3 without extended entries; always 0 otherwise). The offset may still be set — a preallocated cluster — and is not read |
| 1–8 | reserved |
| 9–55 | host offset of the cluster (cluster-aligned); 0: unallocated |
| 56–61 | reserved |

An unallocated cluster reads from the backing file, or as zeros when there is none (or it is shorter) [1].

**Compressed descriptor**, with `x = 62 − (cluster_bits − 8)`:

| Bits | Meaning |
|---|---|
| 0 .. x−1 | host byte offset of the compressed data — **not** aligned to anything |
| x .. 61 | number of additional 512-byte sectors the data occupies beyond the sector containing the offset |

The stored length is therefore at most `(additional + 1) × 512 − (offset mod 512)` bytes. The data may run into the
next host cluster; the last sector may be shared with the start of the next compressed cluster; decompression
stops when a whole cluster has been produced [1]. With 64 KB clusters `x = 54`.

### 2.5 Extended L2 entries

With incompatible bit 4 each L2 entry is 128 bits: the first 64 are the descriptor of §2.4 (bit 0 unused), the
second a subcluster bitmap. The cluster is 32 equal subclusters [1]:

| Bits | Meaning |
|---|---|
| 0–31 | subcluster *n* allocated: its bytes are at host offset + *n* × subcluster size |
| 32–63 | subcluster *n − 32* reads as zeros (its allocation bit is then 0) |

A subcluster with neither bit is unallocated (backing file or zeros). A compressed cluster has no subclusters and
its bitmap is 0.

### 2.6 Other structures

The **refcount table** (at `refcount_table_offset`) points at refcount blocks, one cluster each, holding a
`1 << refcount_order`-bit count per host cluster; a reader that does not write ignores them [1]. The **snapshot
table** describes internal snapshots, each with an L1 table of its own; the image's current contents are the
*active* L1 table of the header [1].

## 3. Algorithms (checksums, compression)

### 3.1 Reading a guest byte

```
l2_entries = cluster_size / (extended ? 16 : 8)
cluster    = guest_offset / cluster_size
l1_index   = cluster / l2_entries
l2_index   = cluster % l2_entries
in_cluster = guest_offset % cluster_size

l1 = L1[l1_index] & 0x00FFFFFFFFFFFE00
if l1 == 0:                          return backing_or_zero(guest_offset)
e  = L2(l1)[l2_index]               # first 64 bits of the entry
if e & (1 << 62):                    return inflate_cluster(e)[in_cluster]       # §3.2
if extended:
    bitmap = second 64 bits; sub = in_cluster / (cluster_size / 32)
    if bitmap & (1 << (32 + sub)):   return 0
    if not bitmap & (1 << sub):      return backing_or_zero(guest_offset)
elif version >= 3 and e & 1:         return 0
host = e & 0x00FFFFFFFFFFFE00
if host == 0:                        return backing_or_zero(guest_offset)
return file[host + in_cluster]       # zero if past the end of the file
```

### 3.2 Compressed clusters

With `compression_type` 0 the stored bytes are a **raw DEFLATE stream** [2] — no zlib header and no Adler-32 — that
decodes to exactly one cluster; anything after the stream's final block in the stored range is ignored [1]. With
type 1 they are a zstd frame [1].

```
x     = 62 - (cluster_bits - 8)
off   = e & ((1 << x) - 1)
extra = (e >> x) & ((1 << (62 - x)) - 1)
len   = (extra + 1) * 512 - (off % 512)      # clamp to the end of the file
cluster = raw_inflate(file[off : off + len]) # must produce cluster_size bytes
```

### 3.3 Checksums

There are none: neither the header, the tables nor the data are checksummed [1]. Integrity rests on the tables'
self-consistency (aligned, in-file offsets) and, for compressed clusters, the DEFLATE stream's structure.

### 3.4 Order of a file's contents

Clusters are allocated as the guest writes, so host order is the guest's write order, not disk order. A fresh
`qemu-img convert` lays most data out in disk order, but L2 tables are allocated when first needed and can sit
after the data they map (§4). A reader must therefore have random access to the file: converting it means
reading the tables first, then the clusters wherever they are.

## 4. Worked decode of a real specimen

Specimen [3], a 1 109 000 192-byte file named `MacOSPuma.img` holding an installed Mac OS X 10.1:

| Field | Value |
|---|---|
| magic, version | `QFI\xfb`, 3 |
| cluster_bits | 16 (64 KB clusters; 8 192 entries per L2 table, each table maps 512 MB) |
| size | `0x2_8000_0000` (10 GiB) |
| crypt_method, backing_file_offset | 0, 0 |
| l1_size, l1_table_offset | 20 (exactly ⌈10 GiB / 512 MB⌉), `0x30000` |
| refcount_table_offset, clusters | `0x10000`, 1 |
| nb_snapshots | 0 |
| incompatible / compatible / autoclear | 0 / 0 / 0 |
| refcount_order, header_length | 4, 112; byte 104 = 0 (deflate) |
| header extensions | feature name table (384 bytes, 8 entries), then end |

L1 entries 0, 1, 2 and 19 are set — L2 tables at host `0x40000`, `0x1C270000`, `0x3CE80000` and `0x130000`, each
with bit 63 — and the rest are 0, so guest bytes 1.5 GB … 9.5 GB are unallocated. The third table lies 1 GB into
the file, after most of the data, and the last (mapping the end of the disk, where an HFS+ volume keeps its
alternate header) sits before the second: host order is not disk order.

L2 table 0's first entries are `0x8000000000050000`, `…060000`, `…070000`: guest clusters 0, 1, 2 at host clusters 5,
6, 7, adjacent. Of its 8 192 entries 7 391 are allocated; none is compressed or zero-flagged. Guest cluster 0,
read at host `0x50000`, begins `45 52 02 00` — an Apple partition map's driver descriptor (`ER`, 512-byte blocks)
— followed by a map of three entries: `Apple_partition_map` (blocks 1–63), `Apple_HFS` "Untitled" (blocks
64 … 20 971 511) and `Apple_Free` (8 blocks).

## 5. Open questions

- The zstd frame layout of a compressed cluster (whether one frame per cluster, and how trailing bytes are
  bounded) is stated by [1] only by reference to zstd; it has not been decoded here.
- Whether any tool writes version 3 images with a `header_length` above 112 for fields beyond `compression_type`
  is not known; such fields are ignorable by rule [1].
- The specimen [3] is a single image; a version 2, compressed, or extended-L2 image from the wild has not been
  decoded. (Synthetic ones written with `qemu-img` decode as §3 describes.)

## Appendix A. Granny Smith implementation notes

QCOW2 is read, never written: an imported QCOW2 is converted to UDIF ([udif.md](udif.md)) and only the UDIF is
stored. Images with a backing file, encryption, an external data file, zstd-compressed clusters or an unknown
incompatible bit are refused with the reason. See `docs/internals/core/storage/image.md`.

## References

1. QEMU Project, *Qcow2 Image File Format* (`docs/interop/qcow2.rst`), qemu.org — the header, feature bits, <!-- lint-allow: QEMU -->
   header extensions, L1/L2 and compressed-cluster descriptors, extended L2 entries, refcounts and snapshots.
2. P. Deutsch, *DEFLATE Compressed Data Format Specification version 1.3*, RFC 1951, May 1996.
3. `MacOSPuma.img` (Mac OS X 10.1 installed on a 10 GiB QCOW2 disk), Internet Archive item `mac-ospuma` (specimen) —
   header, L1 table and first L2 table decoded as in §4.
