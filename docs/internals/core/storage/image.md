## Image Module

The image module manages floppy and hard-disk containers while delegating all block I/O and persistence to the delta-file storage engine. Images are not loaded entirely into RAM; instead, a lightweight descriptor keeps track of backing files while the storage layer handles reads, writes, and crash recovery.

The image subsystem speaks **paths only**. It does not know about machine ids, slots, drives, or `/opfs/checkpoints/`; the higher layer (`config_t` in `system.c`) decides where to place per-image state and remembers the result across boots.

**Types & Key Values**
- **`image_t`** *(declared in `src/core/storage/image.h`; its layout is private to the storage module, in `image_internal.h`, and every other module reads it through the `image_get_*` / `image_is_writable` accessors in `image.h`, implemented in `image_access.c`)* keeps the paths and handles needed by the delta-file storage layer:
	- `storage`: opaque `storage_t*` handle used for every block read/write.
	- `filename`: original path supplied by the user (the immutable base image); it may run through an image or archive.
	- `source_key`: key of the byte source that path opened ([source.md](source.md)).
	- `format`: wrapper layers peeled to reach the disk, outermost first (`raw`, `dc42`, `lisaem`, `bin+ndif`, ...); reported as the image node's `encoding` (`files.images[n].encoding`, `….image.encoding`). <!-- lint-allow: LisaEm -->
	- `instance_path`: stem `<dir>/<id>` for the per-instance delta+journal pair, where `<id>` is a 16-hex-char opaque id minted by the image layer. `NULL` for read-only mounts.
	- `delta_path`: `<instance_path>.delta`.
	- `journal_path`: `<instance_path>.journal`.
	- `raw_size`: the disk's data bytes (`sectors * 512`, tags not counted), plus any wrapper prefix.
	- `block_size`: bytes per stored block — one sector, its tag first: `[tag_bytes][512 data]`. 512 for a disk without tags, 524 for a 400K/800K GCR floppy, 532 for a Lisa ProFile. It is the block the base view, the delta and the journal are made of.
	- `writable`: true when the caller asked for write access.
	- `ghost_instance`: true for a read-only image, whose delta+journal would live in a process-local scratch dir and be deleted on `image_close`. The pair is lazy (`storage_config_t.lazy`): it is created only by the first write to the storage, and the only writer a read-only image has is a consolidated checkpoint's restore over a missing base — a guest write is refused (`disk_write_data` returns 0: the image is write-protected), so a probe or a mount never puts a file on disk.
	- `kind`: what the device that opened it presents it as (`image_open_kind`): floppy, hard disk, CD-ROM or ProFile; `IMAGE_KIND_NONE` for an image no device holds. Reported as the image node's `kind`.
	- `from_diskcopy`: marks DiskCopy 4.2 sources so their headers can be skipped.
	- `tag_bytes` / `units_in_file`: the tags beside each 512-byte sector — 12 on a 400K/800K GCR floppy (the Lisa file system keeps its page labels there and the Lisa boot ROM checks the boot block's; the Mac file systems write them too), 20 on a ProFile, else 0. They are **in-band**: each sector is stored as one block, tag first, so a guest's tag write (`disk_write_tag`, from the IWM/SWIM, SWIM3, IOP SWIM and Lisa controllers) lands in the delta beside the data, and base + delta is the whole disk — a checkpoint carries them with the blocks, and reopening a delta keeps them. A ProFile's files hold whole 532-byte blocks (`units_in_file`); any other base holds 512-byte sectors and is seen through a tagged view (`tagged_view`): each block is the sector's tag — from a DiskCopy 4.2 source's tag section when it has one per sector, else zeros — then its data.
	- `dc42_name` / `dc42_format_byte`: how a DiskCopy 4.2 export labels the disk — a DiskCopy source's own header values, or a machine's (`image_set_diskcopy_identity()`: the Lisa's controller sets `-not a Macintosh disk-` and `$02` on insert); else derived at export.
	- `wrap_prefix` / `wrap_blocks` / `wrap_base` / `wrap_storage_size`: the volume wrapper's synthesised partition map + driver, served in front of an HFS volume when a bare volume or a driverless partitioned disk is attached as a SCSI hard disk ([bare-volume-wrapper.md](bare-volume-wrapper.md)). The volume starts `wrap_base` bytes into `storage` (0 for a bare volume, the `Apple_HFS` partition's start otherwise); `raw_size` is the prefix plus the volume, and `wrap_storage_size` the storage's own size.

**Module lifecycle**
- The module has no global state to set up or tear down; every image is opened and closed by its owner.

**Opening images** — three typed entry points

The single old `image_open(filename, writable)` is replaced by three explicit operations matching the three real use cases:

- **`image_open_readonly(const char *base_path)`** — opens a base image with no on-disk delta, write-protected: a write is refused. Its scratch instance under `/tmp/gs-image-ro/` is never created unless a checkpoint restore writes to it, and is deleted on `image_close`. Use this for probes (`files.partmap`, `files.identify`, …) and for VFS mounts; a drive opens through `image_open_kind` instead (below).
- **`image_create(const char *base_path, const char *delta_dir)`** — opens a fresh writable instance. The image layer mints a 16-hex-char opaque id and creates `<delta_dir>/<id>.delta` and `<delta_dir>/<id>.journal`. If `delta_dir` is `NULL`, the scratch root is used: the `GS_STORAGE_CACHE` directory when set (the integration runner's per-test sidecar routing), else `/tmp/gs-image-ro/`. Never the base image's own directory — a delta is per-instance, so one left beside the base is an orphan once the process exits, and shared media (`tests/data`) would collect them.
- **`image_open(const char *base_path, const char *instance_path)`** — reopens an existing writable instance. `instance_path` is the stem returned by `image_path()` when the instance was first created; the image layer appends `.delta` and `.journal` itself. Used by checkpoint restore.

After construction the caller queries the instance stem with **`image_path(const image_t *image)`** and persists it (in a checkpoint or in the higher-layer slot table) so a future `image_open(base, instance_path)` can find the same delta files. Returns `NULL` for read-only mounts.

**Non-512 block geometries** — inside the storage module each of the three openers has a `*_with_geometry(..., image_geometry_t geom)` variant (`image_internal.h`) that interprets the base as `geom.block_size`-byte blocks; `image_open_kind` picks the geometry from the kind, so no caller outside the module names one. For a disk with **no source file**, **`image_create_blank_kind(kind, blocks, instance)`** makes a writable, all-zero image (no base; unwritten blocks read as zeros) — a blank ProFile. Its delta is a writable instance like any other, in `image_delta_dir(NULL)` (or the saved `instance` a quick restore reopens), so a checkpoint carries it: the entry has no name and the restore recreates the blank image at the recorded size.

Common steps (shared by all three openers):

1. The path's forks go through the format registry's wrapper loop (UDIF, NDIF, QCOW2, DiskCopy 4.2, MacBinary, BinHex, gzip — any nesting); the innermost byte source is the disk. Its size must be a whole number of blocks.
2. A `storage_config_t` is built with that source as `base` (a DiskCopy image's base is a view past its header, so no offset is needed), `delta_path` and `journal_path`.
3. `storage_new()` takes its own reference on the base, opens or creates the delta and journal, and reads existing bitmaps when the delta already exists.

No seeding step is needed — unmodified blocks are read directly from the base file.

**Opening as a kind** — `image_kind.h`

A device never calls the three openers itself: it opens through **`image_open_kind(path, &(image_open_opts_t){kind, writable, instance, delta_dir}, err, cap)`** ([image_kind.md](image_kind.md)). The kind is the device's — `IMAGE_KIND_FLOPPY`, `HD`, `CD` or `PROFILE` — never a guess from the file's size, and it decides the block size (532 for a ProFile, else 512), whether the image may be written (a CD never) and which sizes are accepted (a floppy drive takes the four floppy sizes only). A fresh writable instance's delta goes to `delta_dir`, or, when that is NULL, to **`image_delta_dir(path)`**: the active machine's directory, so the delta shares `state.checkpoint`'s lifetime, or the scratch root for a base on volatile storage (`platform_path_is_volatile`). `fd insert`, `fd create`, every attach and the checkpoint restore go through it. The kind is kept on the image (`image_get_kind`; `IMAGE_KIND_NONE` for a probe or a mount no drive holds) and in its checkpoint entry. What a floppy-sized image's size implies is `image_layout()` (`IMAGE_LAYOUT_GCR400` … `MFM1440`), which the floppy controllers read.

**Image formats** — the format registry

Every opener names a path, and the path may run through an image or an archive (`outer.img/partition1/inner.img`, `disks.zip/System.dsk.gz`): `image_open_path()` opens the path's data fork and resource fork as byte sources through the installed path opener (the VFS — see [source.md](source.md) §3.3), and `image_open_source()` does the rest. The format registry's wrapper loop (`format_unwrap()`, [source.md](source.md) §3.4) peels every encoding layer — UDIF, NDIF, QCOW2, MOOF, DiskCopy 4.2 (and the LisaEm ProFile variant, read for compatibility with existing images), MacBinary, BinHex, gzip, in any nesting — and the innermost source is the storage engine's base. Nothing is decoded to a file: a DiskCopy 4.2 payload is a view past the 0x54-byte header, a LisaEm ProFile image a view that interleaves each block's 20-byte tag with its data in drive order, a MOOF is decoded whole when it opens (below), and an NDIF, UDIF or QCOW2 image is a chunk-mapped source (`image_chunkmap.c`) whose compressed chunks decode on first touch into the image cache (`chunk_cache_images()`, memory only — the default chunk cache spills to scratch files, which for a compressed disk would rebuild the expanded image on disk; `files.cache.image_mb`, default 16 MiB, sized for 64 KiB chunks: an image whose chunks are larger — `hdiutil` writes 1 MiB — gets the budget raised to hold as many of its chunks, up to 256 MiB, so a boot's working set is not re-inflated over and over; `files.cache.image_budget_mb` reads the budget in force). `image->format` records the layers peeled (`"raw"`, `"dc42"`, `"bin+ndif"`, …); `image->filename` is the path the caller named, which is what a checkpoint persists and a restore opens again. A new wrapper format is a new registry row. <!-- lint-allow: LisaEm -->

| | NDIF (Disk Copy 6.x) | UDIF (`.dmg`) | QCOW2 |
|---|---|---|---|
| Block map | `'bcem'` resource, 12-byte descriptors | `'mish'` tables, 40-byte entries | L1 table → L2 tables, one entry per cluster |
| Where it lives | the **resource fork** — needs an AppleDouble sidecar or a containing HFS volume | a **plist inside the file**, found via the 512-byte `'koly'` trailer at EOF | tables in the file, found from the header at offset 0 |
| Compressors | zero-fill, raw, ADC | zero-fill, ignored, raw, ADC, zlib, bzip2, LZFSE, LZMA (xz) | unallocated / zero flag, raw, raw DEFLATE |
| Parser | `image_ndif.c` | `image_udif.c` | `image_qcow2.c` |
| Source | `ndif_source_open()` | `udif_source_open()` | `qcow2_source_open()` |

UDIF specifics worth knowing before touching `image_udif.c`:

- **Everything is big-endian**, including on the PowerPC-era images this mostly exists to read.
- **Chunk sectors are relative to their table's `base_sector`.** Absolute position is `table.base_sector + chunk.sector`; one block table per partition, each restarting at zero. This is the classic way to misread the format, and `tests/integration/image-udif/` exists to catch it.
- **`SectorCount` lives at trailer offset 0x1EC**, not where a naive walk of the published struct puts it — the 128-byte checksum blobs shift several fields. Every `koly` and `mish` offset is defined once, in `udif_layout.h`, shared by the reader and the writer; static assertions there keep every trailer offset inside the 512 bytes. `udif_load()` (`image_chunkmap.c`) is the one trailer → property list → block map sequence, under `udif_source_open()`, `udif_verify()` and `udif_info()`, and `udif_plist_value()` the one key lookup (it unescapes `&lt;`/`&gt;`/`&amp;`, so a foreign table name such as `Data &amp; Tools` reads as `Data & Tools`).
- **The u32 at `mish` offset 0x24 is the blkx resource ID, not a descriptor count.** The count is at 0xC8.
- **Each block table carries a CRC-32 over its decoded bytes**, and chunks of type `UDIF_CHUNK_IGNORE` are excluded from it. The chunk-mapped source decodes chunk by chunk as the guest reads, so the per-table checksum is not verified; every run is validated against the map when the source opens (inside the image, inside the data fork, a supported codec, under 64 MiB compressed) instead.
- **Every chunk codec decodes through the codec library** ([`codec.md`](../../codec/codec.md)): `udif_chunk_codec()` maps the chunk type to its codec — ADC (UDCO), zlib (UDZO), bzip2 (UDBZ), LZFSE (ULFO), xz (ULMO) — and `udif_decode_chunk()` requires the stream to decode to exactly the chunk's sectors. A codec feature not implemented (a randomised bzip2 block, an xz filter other than LZMA2) fails that chunk with `-ENOTSUP`; a stream that does not decode, or decodes to other than its sectors, fails it with `-EIO` (the codec's code passes through `codec_errno()`, as in the NDIF and QCOW2 decoders), and the guest sees a read error.
- Encrypted (`encrcdsa`) and multi-segment (`.dmgpart`) images are **rejected explicitly** rather than partially decoded. A UDIF, NDIF or QCOW2 that detects but will not open is refused by the image opener with the reason; it is never read as a raw disk.
- **QCOW2** is read for compatibility with existing images (the format of the QEMU virtualiser) and is converted to UDIF on import, so a stored image is never one. The source walks the L1 and L2 tables once at open: every allocated cluster becomes a raw run (adjacent host clusters merge), every compressed cluster a run decoded on first touch, and unallocated and zero-flagged clusters, which read as zeros, no run at all. Extended L2 entries map each 1/32 subcluster separately. A raw cluster that runs past the end of the file reads as zeros past it. A backing file, encryption, an external data file, zstd-compressed clusters and unknown incompatible feature bits are refused (`-ENOTSUP`, the reason logged), as is a map of more than 4 Mi runs (`-EFBIG`). See [qcow2.md](../../../reference/formats/qcow2.md). <!-- lint-allow: QEMU -->
- **MOOF** (Applesauce floppy captures, [moof.md](../../../reference/formats/moof.md)) is a bit-level image, not a block map, and an image opened from one has two views. The sector view: `image_moof.c` frames every track's bitstream (or flux, converted to cells) into disk bytes as the IWM frames it in steady state (`gcr_frame_revolution()` in `gcr.c`: one revolution pre-rolled, the second kept), finds each 6-and-2 GCR sector by its address field (`moof_scan_track()`) and decodes its 12 tag and 512 data bytes when the image opens (through `gcr.c`, the codec the IWM/SWIM and SWIM3 models share), then serves the result as a DiskCopy 4.2 image in memory (`<key>#moof`), so the dc42 row takes it next (`moof+dc42`) and the tags load as for any DiskCopy file. A sector no track holds reads as zeros; missing sectors, data-checksum failures and a stale header CRC are logged, not refused. 400K and 800K GCR disks only: an MFM or Twiggy MOOF, or one with no readable sector at all, is refused by the image opener with the reason (its magic is unambiguous, so it is never read raw). Guest writes land in the delta like any other read-only base. The capture: `format_unwrap()` also retains the round's input — the MOOF source itself — in `format_unwrapped_t.capture` (as it retains the DiskCopy container in `.dc42`), and the image keeps that reference (`image->capture_src`) for its lifetime; for a MOOF behind gzip, MacBinary or BinHex it is the decoded inner source, for one in a zip the archive member's. `image_capture.c` parses it on first use — the GCR track builder asking for a track, a controller asking for a sector's status, a `.moof` export — into an immutable store: the file, its index, flux tracks converted, and per sector whether the capture decoded it, found no data field that decodes, or found a failing checksum (`image_sector_status()`). A VFS mount or a probe holds only the reference. The floppy layer serves the captured tracks to the IWM/SWIM GCR read path while the image's sectors still hold what the capture decodes them to, splicing a changed sector's data field into the capture, and reports a sector the capture could not read as unreadable on every controller ([floppy.md](../peripherals/floppy.md), "Tracks"); the image node's `capture` attribute says the image has one. INFO's write-protect flag is honoured when an insert does not say `writable=` (`image_open_opts_t.honour_media_lock`): the disk opens read-only, as the original was. What depends on when bits arrive (bit timing, weak bits) is not reproduced.
- **Chunk size bound.** A cache miss decodes a whole chunk, so a foreign UDIF whose compressed chunks decode past 4 MiB (`files.udif_max_chunk_kb`) is refused in place (`-EFBIG`, "import it to re-chunk it"). Images this emulator wrote carry a `gs-profile` key in the property list and are bounded by construction. `files.convert` passes every chunk once and opens with no bound, which is how a large-chunked image is re-chunked.

**Writing UDIF** — `udif_writer.h`

The disk images the web app stores, the blank hard disks `files.create` makes under a `.dmg` name, and exports to a `.dmg` name are written by a streaming UDIF writer: payload first, block map and trailer last, so it never seeks back and holds one partial chunk at most. It writes one profile (the "GS profile"):

| | |
|---|---|
| Layout | one segment, flattened; data fork, then the XML plist, then the 512-byte `koly` |
| Block map | one whole-disk `mish` table from sector 0, named for what the disk starts with (`Apple_partition_scheme`, `Apple_HFS`, `ISO9660`, or `Unknown Partition`) |
| Chunks | 128 sectors (64 KB) by default; each ZERO (consecutive ones merge — a blank 2 GB disk is one entry), ZLIB (a complete zlib stream, `CODEC_ZLIB`) — or LZMA (a complete xz stream, `CODEC_XZ`) when the caller asks for the xz codec — or RAW (when compression does not shrink it). Never IGNORE or ADC |
| Checksums | the table's CRC-32 over the decoded bytes (a zero run's in O(log n), `codec_crc32_zeros`), the trailer's over the data fork, the master checksum over the table checksums |
| Plist keys | `gs-profile` (1), `gs-byte-length` (the true length when it is not a whole number of sectors; the tail is zero-padded), `gs-source` (the original file name), `gs-origin` (where the bytes came from, as the caller gave it — the page records a URL download's URL; opaque to the core, read back by `files.udif_info` as `origin`) |

Each ZLIB chunk is deflated in blocks of up to 32768 tokens, each coded with its own length-limited Huffman codes, or with the fixed codes or stored when either is smaller (the codec library's encoder, `codec.h`). Against fixed codes alone that is 8–11% smaller.

**The xz codec is opt-in.** `files.convert`, `files.udif_open` and every drive's `image.export` take `codec="xz"` (`udif_codec_t` in `udif_writer.h`; the default level is then 6, zlib's stays 1). Its chunks are LZMA in an xz container, the chunk type hdiutil writes for ULMO. On three system disks at 64 KB chunks it is 14–16% smaller than the default (System 7.5: 9.0 → 7.8 MB; AIX 4.1.5: 32.1 → 27.0 MB; Mac OS 9.2.1: 147 → 126 MB), encodes in about zlib level 9's time, and decodes about 4× slower than zlib on every cache miss, which is why images the web app stores and reads in place stay zlib. Its images open in hdiutil on macOS 10.15 and later, not in 7-Zip or dmg2img.

The writer's images also open in the other tools users have, and CI checks that ([`udif-interop.yml`](../../../../.github/workflows/udif-interop.yml)): the `udif_writer` suite, given `GS_UDIF_INTEROP_DIR`, writes sample images beside the disks they decode to, and [`scripts/check-udif-interop.sh`](../../../../scripts/check-udif-interop.sh) opens each with hdiutil on macOS (`imageinfo`, `verify`, `convert -format UDTO`, `attach -readonly` with the raw device read back) and with 7-Zip and dmg2img, comparing bytes. hdiutil refuses (-192) a device image (`koly` variant 1) that has no partition map to read block-table IDs from, so the writer marks its one-table images as partition images (variant 2). Like hdiutil's own `-layout NONE` images, they also carry a `plst` resource beside `blkx` and -2 in the `mish` descriptor field. dmg2img takes a plist at offset 0 for none, so when the data fork is empty (an all-zero disk) one sector of padding precedes the plist.

[`scripts/bench-image-boot.sh`](../../../../scripts/bench-image-boot.sh) boots a machine for a fixed number of instructions from a raw disk and from the same disk converted to UDIF. It reports the median wall-clock time of each and whether the two boots end on identical screens. The result on a Linux x86-64 host, 5 runs each, default cache:

| Disk | Raw | Stored as UDIF | Boot raw → UDIF | Image cache misses / hits |
|---|---|---|---|---|
| System 7.6, Quadra 700 | 169 MiB | 18.6 MB (fixed codes: 20.8) | 11.5 → 11.9 s | 240 / 10771 |
| System 7.5, IIci | 76 MiB | 9.0 MB (10.1) | 3.5 → 3.7 s | 143 / 6149 |
| A/UX 3.0.1, IIx | 169 MiB | 38.2 MB (42.2) | 5.1 → 5.3 s | 294 / 16428 |

The screens were identical every time. The overhead measured 0–7% across runs, about the run-to-run noise. With the image cache cut to 1 MB (`files.cache.image_mb = 1`), the misses roughly double and the overhead stays between 2% and 10%.

7-Zip reads and checks the result (`7z t`) — a zlib image; neither 7-Zip nor dmg2img reads LZMA chunks, so the interop check runs the xz samples through hdiutil alone. `udif_verify()` decodes every chunk and checks every checksum; `udif_info()` reads only the trailer and plist. The object-model surface: `files.udif_open` / `udif_append` / `udif_finish` / `udif_abort` (the page streams an import through the transfer window), `files.convert` (any image the emulator reads → UDIF, or → raw with `format="raw"`, checked by decoding the result), `files.verify`, `files.udif_info`. `files.udif_open`, `files.convert` and `files.archive.import` take an optional `origin` string for `gs-origin`; `files.udif_open`, `files.convert` and `image.export` an optional `codec` (`zlib`, the default, or `xz`).

Every compressor and decompressor here — and the PNG reader's, peeler's zip and gzip — is the codec library's ([`codec.md`](../../codec/codec.md)): first-party code, one implementation of each in the tree; the core links no third-party C libraries.

**Where images live** — the image layer opens the path it is given. It does not copy volatile media into persistent storage; the web app does that before attaching (see `docs/guide/web.md`, "Filesystem").

**Reading/Writing image data**
- A tagged image's **data plane** (`disk_read_data` / `disk_write_data`) moves whole 512-byte sectors, each the data half of its block; a write keeps each sector's tag. **`disk_read_tag` / `disk_write_tag`** move a sector's tag, and **`disk_read_units` / `disk_write_units`** whole blocks as stored (the ProFile's 532-byte wire block is its stored block).
- **`disk_read_data(image_t *disk, uint64_t offset, uint8_t *buf, size_t size)`** and **`disk_write_data(...)`** enforce `disk->block_size` alignment (512 for flat disks, 532 for a ProFile) and forward to `storage_read_block` / `storage_write_block` in a loop.
- **Byte offsets and sizes are 64-bit** all the way down: `raw_size`, `wrap_base`, `wrap_storage_size`, `disk_size()`, `image_get_raw_size()`, the offsets `disk_read_data` / `disk_write_data` and the `storage_*_block` calls take, and every device model's offset arithmetic (`(uint64_t)lba * block_size`). In the wasm32 build `size_t` is 32 bits, so an offset narrowed to `size_t` anywhere on that path would land a block 4 GB too low; a disk over 4 GB opens and is addressed in full on every host. Only a transfer's length is `size_t` (it is a buffer in memory). Pinned by `tests/unit/suites/scsi_bounds` (`a_disk_past_4gb_is_addressed_in_full`), which runs in `make -C tests/unit run-wasm32`.
- **A read the backing image cannot serve** (a corrupt compressed chunk, a damaged archive member, a host or OPFS I/O error: `storage_read_blocks` returns `STATUS_E_IO`) makes `disk_read_data` return a short count. It neither halts the emulator nor hands back zeros; the first failure per image is logged at level 0 (`image` category), later ones at level 1. Each device model turns the short count into the read error its hardware reports, so the guest sees an I/O error:

  | Device | What the guest sees |
  |---|---|
  | SCSI hard disk / CD-ROM (`scsi_bus.c`), ATAPI CD-ROM | READ(6/10/12): CHECK CONDITION, MEDIUM ERROR / UNRECOVERED READ ERROR (11h). VERIFY: the same, with or without BytChk (a medium verification reads the blocks). ATAPI reports it as sense key 3 in the error register. |
  | ATA hard disk (`ata.c`) | Status ERR, error register UNC (uncorrectable data error), the task file addressing the first sector of the failing block. |
  | IWM/SWIM GCR floppy (`floppy_gcr.c`) | The track is read a sector at a time; each unreadable sector is laid down with a bad data checksum (the .Sony driver's badDCksum). The other sectors read. |
  | SWIM ISM MFM floppy (`floppy_swim.c`) | The sector is laid down with a data CRC that fails the ISM's CRC check. |
  | SWIM3 (`swim3_xfer.c`) | Data CRC error (`SWIM3_E_CRC_DATA`). |
  | New Age FDC (`new_age.c`) | Abnormal termination, ST1 DE / ST2 DD (data error in the data field). |
  | IOP SWIM block transfer (`iop_swim.c`) | ioErr. |
  | Lisa FDC (`lisa_fdc.c`) | Status $17 (unreadable). |
  | Lisa ProFile (`lisa_profile.c`) | Status byte 0 $09, the CRC error on read. |

  Image-layer readers (`image_read_bytes`, partition and filesystem walkers, the wrapper sniff) get `-EIO` / false and fail their operation.

**Background work**
- **`image_tick_all(config_t *config)`** calls `storage_tick()` for each registered image. With the delta model, `storage_tick()` is a no-op (no consolidation needed).

**Persisting changes / Exporting**
- **`image_export_to(image_t *image, const char *dest_path)`** streams the disk (base + delta) to a new file, in the format the destination's name picks:
	- `.dmg` (512-byte blocks): a UDIF written by the streaming writer — a modified 2 GB disk exports at its content's size.
	- `.dc42`, `.diskcopy`, or `.image` for a floppy (the classic Mac name for a DiskCopy file): a self-contained DiskCopy 4.2 file — header, data, then one tag per sector, with the data and tag checksums (the tag sum skips the first sector's tag, as DiskCopy does). The tags are split off the blocks the export streams (one per sector), so they are the view's moment, like the data. 400K, 800K, 720K and 1440K floppies only; anything else is refused. The header carries `dc42_name` and `dc42_format_byte` when set, so a DiskCopy disk exported unchanged comes back byte for byte; otherwise the source file's name and the Mac format byte (`$12` for 400K, `$22` otherwise).
	- `.moof`: an Applesauce MOOF bitstream image (`moof_encode()`): each track laid down as a Mac formats it — 2:1 interleave, self-sync gaps sized so a revolution is the zone's length at 394–590 rpm in 2 µs cells — with the tags when the image has 12 per sector, and the format byte as for DiskCopy. 400K and 800K floppies only. An image opened from a MOOF exports through `moof_encode_with()`: every track whose sectors the export's snapshot still holds as the capture does (the floppy layer's rule, `image_capture_track_diff()`) is copied through bit for bit — a flux track as its bitstream — and only the others are laid down from sectors; the capture's INFO (write protect, bit timing) and META are kept, the creator set and the CRC recomputed. `files.convert` to a `.moof` destination (format `moof`) is the same export of an image it opens itself. A block export (raw, DiskCopy, UDIF) of a captured disk logs how many sectors it writes as zeros because the capture could not read them.
	- anything else: a dense raw copy of the data (no tags) — except a ProFile, whose raw file is its 532-byte blocks, tags in-band, as the files it opens. A `.dmg` of a ProFile is refused (UDIF holds 512-byte sectors).

  Whatever the format, the destination is created exclusively (an existing file is never overwritten), a base block that cannot be read fails the export rather than writing zeros in its place, and the base image is never written in place. Every device's Save As goes through it, as one method of the one image node (`image_class.c`), an I/O job taking a `codec`: `machine.scsi.device[N].image.export`, `machine.floppy.drive[N].image.export` (IWM/SWIM/SWIM3/New Age and the Lisa's FDC), `machine.hd.image.export` (the ProFile), `machine.ata.device[U].image.export`, `files.images[N].export`; and `files.export_raw` (which exports a file's source, not a mounted disk).

**Creating blank hard disks**
- **`image_create_empty_udif(path, size)`** writes a UDIF of `size` zero bytes: one zero run, about 4 KB whatever the size. `files.create` uses it for a hard disk whose path ends in `.dmg` (the web app's Create Image names blank disks so); any other name gets **`image_create_empty()`**, a raw file of the full size, which the browser charges in full.

**Creating blank image files**
- **`image_create_file_kind(path, kind, size, err, cap)`** (`image_kind.c`; `files.create`) writes a new blank file of a kind holding `size` data bytes: a floppy of one of the four floppy sizes, a hard disk from 400K to 2 GiB (a UDIF of one zero run for a `.dmg` name, else raw), or a ProFile of whole sectors as raw 532-byte blocks; a CD-ROM is refused. Every file is created exclusively (**`image_create_zeros()`**), so an existing one is never overwritten.

**Checkpointing & metadata**
- **`image_checkpoint()`** writes `{uint32 len, path bytes, flags, kind, raw_size, uint32 instance_len, instance_path bytes, uint32 key_len, source_key bytes}`, and calls `storage_checkpoint()`, whose blocks carry the sector tags in-band. The `instance_path` field (added in the storage-isolation rewrite) lets the restore path locate the delta+journal pair without relying on adjacent-to-base sidecars. The source key lets a quick restore refuse a base that is no longer the same bytes ([source.md](source.md) §5). The storage layer writes the current bitmap for quick checkpoints or streams all blocks for consolidated checkpoints. A floppy's checkpoint also records its MOOF capture's fingerprint: a quick restore onto another capture is refused, while a consolidated one restores without the file (the recreated base has no capture, so the drive lays its tracks down from the restored sectors) or onto another capture ([floppy.md](../peripherals/floppy.md), "Checkpoints").
- During restore the machine init code reads back the same fields and reopens the image through `image_open_kind` as the kind it was saved as (a ProFile at its 532-byte block, a CD read-only):
	- writable + quick → the same delta files (`instance`).
	- writable + consolidated → a fresh instance in `checkpoint_machine_dir()`; the embedded blocks then repopulate it via `storage_restore_from_checkpoint()`.
	- read-only → no instance.
- Old checkpoints written before the format change become unreadable naturally through `checkpoint_validate_build_id`; no migration code is needed.

**Usage notes**
- Writable image deltas now live under `<delta_dir>/<id>.delta` (typically `/opfs/checkpoints/<machine_id>-<created>/<id>.delta`), not next to the base image. Removing the per-machine directory reverts every disk in that machine to its pristine base state in one step.
- DiskCopy images become writable overlays: mutations live in the delta file while the source archive stays untouched.
- Reusing the same base image for a fresh machine no longer replays stale deltas: every `image_create` mints a new random instance id, so two machines mounting the same base get two independent delta files.

---
Last updated for the per-machine checkpoint-isolation rewrite.
