## Bare-Volume Wrapper and the GSDisk Driver

A **bare volume** is an HFS (or HFS+) volume image with nothing in front of
it: block 0 holds the volume's boot blocks (`LK`), the Master Directory Block
sits at byte 1024 (`BD`), and there is no Driver Descriptor Map, no Apple
Partition Map and no driver. It is the shape most of the archive.org Macintosh
library is stored in (for example
`AppleMacintoshSystem753/System7_5_3.img`, 26,214,400 bytes).

The ROM cannot boot such a disk, nor can the Finder mount it: the Start
Manager's SCSI boot code reads block 0 for a Driver Descriptor Map, loads the
driver it names, and that driver is what puts the disk's volumes in the drive
queue. The ROM's SCSI Manager mounts nothing by itself. Without a map and a
driver, the disk is visible to SCSI probes and to nothing else.

So `scsi.attach_hd` (every SCSI hard-disk attach: `machine.attach_hd`, the
`hd=` command-line argument, the web UI's `?hd0=`) **wraps** a bare volume:
the guest sees a disk with a synthesised map and an in-tree driver in front of
the untouched volume.

Sources: [`image_wrap.c`](../../../../src/core/storage/image_wrap.c) /
[`image_wrap.h`](../../../../src/core/storage/image_wrap.h) (layout, sniff,
checksum), [`gsdisk/gsdisk_drvr.s`](../../../../src/core/storage/gsdisk/gsdisk_drvr.s)
(the driver), [`gsdisk/gsdisk.mk`](../../../../src/core/storage/gsdisk/gsdisk.mk)
(its build).

### What is wrapped, and what is not

`image_wrap_bare_volume()` wraps an image when its first three blocks show a
volume header at 1024 (`BD` or `H+`) and neither `ER` at 0 nor `PM` at 512.
It is called only on the SCSI hard-disk attach path (`media_open` in
`system.c`):

- **CD-ROM attaches are not wrapped.** The Apple CD-ROM driver reads bare HFS
  itself, and a CD presents 2048-byte blocks that a 512-byte map would
  misdescribe.
- **Floppies are never wrapped**: floppies are bare by design.
- **The VFS is unaffected**: browsing goes through the bare-HFS namespace as
  before.
- **Partitioned disks are never touched**, including a disk the wrapper itself
  produced (its block 0 is `ER`).

### Layout the guest sees

All blocks are 512 bytes. The prefix is a fixed 96 blocks
(`IMAGE_WRAP_PREFIX_BLOCKS`), whatever the driver's size, so a wrapped disk's
layout never shifts between builds.

| Blocks | Content |
|---|---|
| 0 | Driver Descriptor Map: `sbSig 'ER'`, `sbBlkSize 512`, `sbBlkCount` = 96 + volume, one descriptor `ddBlock 64`, `ddSize` = driver blocks, `ddType 1` (Mac OS 68k) |
| 1 | map entry 1: `Apple` / `Apple_partition_map`, blocks 1–63 |
| 2 | map entry 2: `Macintosh` / `Apple_Driver`, blocks 64–95; `pmBootSize` = driver bytes, `pmBootCksum`, `pmProcessor "68000"` |
| 3 | map entry 3: `MacOS` / `Apple_HFS`, blocks 96… (the volume) |
| 64–95 | the GSDisk driver |
| 96… | the volume, byte for byte |

`image_wrap_build_prefix()` produces the 96 blocks; the unit suite
(`tests/unit/suites/image_wrap`) checks them with the in-tree APM parser.

### How the prefix is served

The prefix lives in memory (`image_t.wrap_prefix`, `wrap_blocks`). Only
`disk_read_data` / `disk_write_data` know about it: offsets below the prefix
are served from memory, everything else is shifted down onto the image's
storage, which still holds just the volume. So:

- **the file is never modified** — guest writes land in the image's delta as
  for any disk, at the volume's own offsets;
- `raw_size` / `disk_size()` include the prefix (the SCSI layer's bounds and
  READ CAPACITY see the whole disk);
- `storage.export_raw` and checkpoints' consolidated data carry the volume
  only;
- a write into the prefix (a partitioning tool rewriting the map) changes the
  in-memory copy and is logged; it is not persisted, and the prefix is rebuilt
  on the next open.

**Checkpoints.** The per-image flags byte `image_checkpoint` writes
(`IMAGE_CKPT_WRITABLE`, formerly the bare `writable` flag) gains
`IMAGE_CKPT_WRAPPED`; the saved `raw_size` is the storage's. The restore
(`mac_checkpoint_restore_one_image`) re-wraps an image carrying the bit
before its storage is restored, so the SCSI device that re-binds to it by name
sees the same disk. Checkpoints written before this change never have the bit
set, and read unchanged.

### The ROM's side of the contract

Read from the ROMs' `SCSILoad` (the Plus v3 ROM at `$407D62`, the
IIci/Quadra 700 ROMs at `$40807224`; the SE/30 ROM has the IIci's code):

1. For each SCSI id (6→0 on the Plus, 7→0 later) without a driver yet
   (`SCSIDrvrs`), read block 0; require `ER` and a descriptor whose `ddType`
   is the one wanted (1 on a 68k machine).
2. `_NewPtrSys` a block of `ddSize × sbBlkSize` bytes and read `ddSize`
   blocks from `ddBlock` into it (READ(6): `ddBlock` must be < 2²¹, `ddSize`
   < 256).
3. **IIci-era and later only:** read block 1; if it is a `PM` map, walk it
   for an entry of type `Apple_Driver*` whose name starts `Maci` and whose
   `pmPyPartStart` equals `ddBlock`, and verify its checksum: every one of the
   first `pmBootSize` bytes of the loaded driver is added into a 16-bit sum
   that is then rotated left one bit; a 0 result counts as `$FFFF`; it must
   equal `pmBootCksum`. A mismatch disposes the driver and skips the disk.
   (No such entry but an `Apple_HFS` one: the driver runs unchecked.) The
   Plus ROM checks none of this.
4. `JSR` to the driver's **first byte** with **D5 = the SCSI id** and A0 → a
   copy of block 1.

Plain `Apple_Driver` is used rather than `Apple_Driver43`: every ROM accepts it
(the pre-SCSI-Manager-4.3 loaders compare only the first 12 characters), and
it keeps the old, well-defined entry convention.

After `SCSILoad`, the Start Manager walks the drive queue, `_Read`s each
drive's first 1024 bytes through its driver looking for `LK` boot blocks, and
`_MountVol`s the one it boots from.

### The driver

`gsdisk_drvr.s`: a fresh implementation against the published Device Manager
contract (*Inside Macintosh: Devices*) and the ROM behaviour above — no Apple
driver code is shipped, harvested from user disks or synthesised into images.
68000 instructions only, so one binary serves every family — the PowerPC
ROMs (PDM, TNT) run it under their 68k emulator exactly as they run Apple's
pre-native drivers; ~1.4 KB (3 blocks).

- **Install** (the first byte, entered from `SCSILoad`): allocate private
  storage (`_NewPtr ,Sys,Clear`); `_DrvrInstall` a DCE at unit `32 + id`
  (refNum `~(32 + id)`); fill in the DCE itself — `_DrvrInstall` only clears
  it and marks it RAM-based — with `dCtlDriver` → the DRVR header,
  `dCtlFlags = $6F20` (`dNeedLock`, `dNeedTime`, status/control/write/read
  enabled, `dOpened`; pointer-based), `dCtlDelay`, `dCtlStorage`; walk the
  partition map over SCSI and give each `Apple_HFS` partition (up to 4) a
  drive queue element (`_AddDrive`; `qType 1`, size in `dQDrvSz2:dQDrvSz`;
  drive number one past the highest queued, at least 5; non-ejectable).
  `dNeedLock` is required, not optional: without it the Device Manager's
  IODone treats `dCtlDriver` as a handle and `_HUnlock`s it.
  The driver avoids `_StripAddress`, which the Plus ROM does not implement.
- **Prime**: `dCtlPosition` (the Device Manager has already applied
  `ioPosMode`) and `ioReqCount` against the partition; whole blocks go
  straight to/from the caller's buffer as READ(10)/WRITE(10) of up to 64
  blocks, a partial block through a 512-byte bounce buffer
  (read-modify-write). SCSI I/O is the SCSI Manager's polled
  `_SCSIDispatch` calls (Get, Select, Cmd, Read/Write with an `scInc`/`scStop`
  TIB, Complete), retried up to 8 times on a busy bus or bad status (the first
  command after a reset reports a unit attention). Synchronous: completes
  through IODone (or RTS for an immediate call).
- **Control**: kill, verify, format, eject → no error (a preformatted,
  non-removable disk); 21/22 → the driver's own drive icon; **65 (`accRun`)**
  → announce drives (below); anything else `controlErr`.
- **Status**: 8 (drive status) → the drive's flag bytes and queue element;
  anything else `statusErr`.
- **Close**: `closErr` — a boot driver is never closed.

**Announcing volumes.** A disk-inserted event posted during `SCSILoad` does
not survive startup, so the boot volume is mounted by the Start Manager and
every other volume would stay unmounted. Like Apple's own disk drivers, GSDisk
announces from periodic time instead: with `dNeedTime` set, `accRun` posts a
`diskEvt` for each of its drives that has no mounted volume yet (a walk of the
VCB queue) and has not been announced; once all are, it clears `dNeedTime`.
The second wrapped disk on the bus is mounted this way (the `two-volumes`
row).

**Build stamp.** The driver carries `GSDisk build ` followed by 24 zero
bytes; `image_wrap_build_prefix` writes the emulator's build id there before
computing the checksum, so a guest's disk names the driver that ran.

### Build

`gsdisk.mk` is included by both `Makefile` (wasm) and `Makefile.headless`,
like `vrom68k.mk`: `m68k-linux-gnu-as -m68000` → `objcopy -O binary` →
`build/gsdisk/gsdisk_drvr.bin` → `scripts/bin2c.py` →
`build/gsdisk/gsdisk_driver.h`, which `image_wrap.c` includes. There is no
fallback when the assembler is missing.

### Tests

- `tests/unit/suites/image_wrap` — the DDM, the three map entries (parsed by
  `image_apm_parse_buffer`), the ROM's driver-partition checks and checksum,
  the build stamp, and the bare-volume sniff.
- `tests/integration/scsi-bare-volume` — one machine of every family that
  boots Mac OS from SCSI, each to the Finder off a wrapped volume: the
  archive.org System 7.5.3 volume on the SE/30 (glue), IIci (MDU), Quadra 700
  (MCU), IIfx (OSS), Power Mac 6100 (PDM) and 7500 (TNT); the Plus-only System
  7.0.1 volume on the Plus (compact); and on the Quadra 840AV (AV, the SCSI
  Manager 4.3 ROM) a bare copy of the AV suite's System 7.1 volume, cut out of
  its Apple-partitioned image at setup by `scripts/apm-extract-hfs.py`
  (System 7.5.3 stops with "illegal instruction" on the emulated 840AV behind
  Apple's own driver too). Plus two wrapped disks on one bus and a checkpoint
  round trip. The Lisa/MacXL (ProFile, no SCSI) and the Network Servers (no
  Mac OS) are out of scope. Media: `tests/data/systems/system_7_5_3_25mb_bare.img`
  and `system_7_0_1_10mb_bare_plus.img`.
- `tests/e2e/web2-specs/url-archive-boot.spec.ts` — the web UI's archive.org
  URL booting the same volume.

### Limits

- The PDM ROM needs a default startup device in PRAM before it boots from
  any SCSI disk (Apple-formatted ones too); the web UI and the test rows set
  it (`machine.rtc.pram.boot_device`).
- The volume must be < 4 GB (the driver computes byte positions in 32 bits,
  as the Device Manager does).
- Up to 4 `Apple_HFS` partitions per disk (the wrapper makes one).
- The prefix is not persisted: tools that rewrite the partition map (HD SC
  Setup's "Update") see their change until the disk is reopened.
