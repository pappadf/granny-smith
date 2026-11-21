# Images: one model for every drive

One image model behind every drive: floppy (IWM, SWIM, SWIM3, New Age,
IOP SWIM), SCSI hard disk and CD-ROM, ATA hard disk and ATAPI CD-ROM, the
Lisa's Sony drive and ProFile. Each image is one base file plus one delta,
with one object-model node (`image`) and one identify (`files.identify`).
The kind table and the one kind-aware open live in
`src/core/storage/image_kind.c` (`image_kind.h`); the rest of the image
layer is `image.c`, `image.h`, `image_internal.h`, `storage.c`,
`format_registry.c`, `storage_class.c`, `archive.c`. Callers are
`src/core/system.c` (`media_open`, the image table),
`src/machines/runtime/checkpoint_images.c`, the device models
`src/core/peripherals/{floppy,floppy_gcr,floppy_swim,swim3_xfer,iop_swim,new_age,scsi,scsi_bus,ata}.c`,
`src/machines/lisa/{lisa,lisa_fdc,lisa_profile}.c` and
`src/machines/gossamer/gossamer_ata.c`.

**Status:** this page started as the design (§1 is the state it was
written against, `main` at 9b4766c; line numbers there refer to that
commit). It is implemented step by step (§5); the step list says which
steps have landed, and §2 onwards describes the model as it is once all
have. The owner's decisions are recorded in §6.

Contents: §1 Starting state · §2 Target model · §3 Object model ·
§4 Identify · §5 Migration plan · §6 Decisions.

## 1. Starting state

### 1.1 The shared core

Every drive already reads and writes through one `image_t`
(`image_internal.h`). An `image_t` is a base (any `source_t`, after the format
registry peels UDIF, NDIF, QCOW2, MOOF, DiskCopy 4.2, MacBinary, BinHex or
gzip), a `storage_t` delta and journal named by `instance_path`, a
`block_size` (512, or 532 for a ProFile), a size-guessed `enum image_type`,
an in-memory tag array, DiskCopy export identity, and the bare-volume wrapper
prefix. The I/O calls are `disk_read_data` / `disk_write_data` (whole blocks)
and `disk_read_tag` / `disk_write_tag`. One export body exists:
`image_export_begin/run/end` (`image.c:852`), with `medium_export.h` as the
shared object-method body for floppy and SCSI. Opening, ownership,
checkpointing and identify are not shared: each device does them its own
way.

### 1.2 Per-device lifecycle

| Device | Open (path → image) | Owner | Write path | Eject | Export | Checkpoint | Object node |
|---|---|---|---|---|---|---|---|
| Mac floppy (IWM/SWIM/SWIM3/New Age/IOP; one `floppy_t`) | `do_insert_fd` `system.c:625`: `image_create` or `image_open_readonly`. Bypasses `media_open` | image table (`config_add_image`) | `floppy_media_*_sector` `floppy_gcr.c:174`; `disk_write_data` + `disk_write_tag` in `floppy_gcr.c`, `floppy_swim.c:462`, `iop_swim.c:883/991`, `swim3_xfer.c:146` | `floppy_drive_eject` `floppy.c:834`: flushes tracks, **releases** the image (`config_release_image`) | `machine.floppy.drive[N].disk.export(path,[codec])`, I/O job | generic image list + `checkpoint_write_image_ref` (by filename) | `machine.floppy.drive[N].disk`: present, writable, density, path, filename, eject, export |
| SCSI HD (all SCSI buses: `scsi`, `scsi2`, `pci_<slot>`) | `media_open` `system.c:1528`: `image_create` + `image_wrap_volume` + drive catalog | image table | `scsi_bus.c` → `disk_*_data` | `scsi_eject_device` `scsi_bus.c:1957`: detaches, **does not release** (image stays open in the table) | `device[N].image.export(path,[codec])`, I/O job | generic list + ref | `machine.scsi.device[N].image`: present, path, filename, raw_size, writable, export, eject (returns bool) |
| SCSI CD-ROM | `media_open`: `image_open_readonly` + `image_set_type(cdrom)` | image table | none (device read-only) | drive stays, medium cleared; not released | as HD | as HD | as HD |
| ATA HD (Gossamer) | `media_open` HD branch (so it also gets a SCSI drive-catalog identity it never uses) | image table | `ata.c:317` `hd_io` | **refused** (`gos_media_eject` `gossamer_ata.c:363`: "not removable") | `machine.ata.export(unit,path)`: synchronous `image_export_to`, no codec, unit-first argument order | ref in `ata_checkpoint_save` | **none**; `machine.ata.devices` is a string |
| ATAPI CD (Gossamer) | `media_open` CD branch → hidden SCSI bus `atapi` | image table | none | as SCSI CD | `machine.atapi.device[N].image.export` | as SCSI | `machine.atapi.device[N].image` |
| Lisa Sony floppy | `do_insert_fd` (shared with Mac) | image table | `lisa_fdc.c:223-232`: `disk_*_data` + `disk_*_tag`; **no write-protect check** | `lisa_fdc_eject` `lisa_fdc.c:469`: **not released** | `machine.floppy.drive[0].export(path)`, I/O job, no codec | ref (`lisa_fdc_checkpoint` `:730`) | flat `machine.floppy.drive[0]` (present, filename, insert, eject, export); **no medium child**; `machine.floppy` has no `identify` |
| Lisa ProFile | `lisa_profile_attach` `lisa_profile.c:407`: own `image_create_with_geometry(532)`, own `pro_delta_dir` (matches the literal `"/tmp/"`, not `platform_path_is_volatile`); `media_open` PROFILE branch | **the ProFile itself**, not the table | `disk_*_data` on whole 532-byte blocks, tags in-band | `lisa_profile_detach`: `image_close` | `machine.hd.save(path)`, I/O job, no codec | **inline** `image_checkpoint` in the Lisa part; restore with `mac_checkpoint_restore_one_image(geom 532)` | `machine.hd`: present, attach, detach, save (+ pram_*) |
| Unattached (VFS mounts, `files.*` tools) | `image_open_readonly[_source]` in `vfs.c:503`, `storage_class.c` (partmap, partitions, probe, convert, verify, xfer_read_disk), `system_find_media` | caller | none | `image_close` | n/a | n/a | `files.mounts[N]` |

### 1.3 What is already shared

- The base+delta engine, the format peel, the export engine (every format
  for every device), and the read-error contract (each device turns a short
  `disk_read_data` into its own error, `image.md`).
- Bay addressing: `media_bus_t` / `media_bay_t` / `media_slot_t`
  (`machine_profile.h:173-224`), `system_media_attach_path` and the
  substrate hooks `media_attach/present/image/eject`.
  `machine.attach_media` / `eject_media` / `attach_hd` / `attach_cdrom`
  (`machine.c:898-976`) and headless `hd=` / `cdrom=` / `drive=` all go
  through them.
- The floppy geometry answer (`floppy_geometry.h`) is shared by every Mac
  floppy controller and by the Lisa FDC.

### 1.4 Where it diverges

Each item marked "verified" was confirmed with `build/headless/gs-headless`
on this commit.

1. **Two open paths.** Floppies open in `do_insert_fd` /
   `system_create_floppy`, everything else in `media_open`. The ProFile
   opens a third way, in `lisa_profile_attach`. `pick_delta_dir` exists
   twice (`system.c:80`, `lisa_profile.c:401`) with different volatility
   tests.
2. **Kind is guessed from size, then patched.** `classify_image`
   (`image.c:51`) decides floppy or HD from the byte count, and finds a CD
   from an ISO descriptor. A CD attach overrides this (`image_set_type`), but
   a restore re-guesses: an HFS-only CD comes back as `hd`, and its drive
   light becomes the HD light (`system_drive_io_counts` `system.c:456`).
3. **Tags are not in base+delta.** A floppy's 12-byte tags live in
   `image->tags`: they are loaded from DiskCopy or zero-filled for 400K/800K
   (`image_ensure_gcr_tags` `image.c:254`), carried as a checkpoint tail, and
   leave only in a DiskCopy or MOOF export. The ProFile's 20-byte tags are
   in-band (532-byte blocks), so the tree has two tag mechanisms. After a
   reload that reopens the delta without a checkpoint, the floppy's data
   survives and its tags are lost.
4. **Ownership and eject.** The floppy releases its image on eject; SCSI, the
   Lisa FDC and ATAPI do not. The image stays open in the 10-slot table
   until teardown, so the file stays busy and repeated swaps fill the table
   (verified: `files.images.count` stays 1 after `device[0].eject`, and after
   the Lisa `drive[0].eject`). The ProFile owns its image outside the table,
   so `files.images` and the drive light never see it.
5. **A blank ProFile is lost on checkpoint.** `image_create_blank` has no
   filename and a scratch delta. `image_checkpoint` writes an empty name,
   and the restore drops the disk (verified: `machine.hd.attach;
   checkpoint.save; checkpoint.load` gives `machine.hd.present == false`).
6. **Read-only means a ghost delta.** `image_open_readonly` still creates a
   scratch delta and journal (`ghost_paths`), so every identify or probe
   creates and deletes two files, and writes to a "read-only" medium succeed
   silently. The Lisa FDC and the SWIM ISM write path (`floppy_swim.c:462`)
   do not check write-protect. `floppy_media_write_sector`, IWM GCR, IOP and
   New Age do.
7. **Insert defaults differ.** `machine.floppy.drive[N].insert` and the
   Lisa's default to read-only. `fd insert`, headless `fd=`,
   `machine.hd.attach` and every SCSI or ATA HD are writable.
8. **Three node shapes and four exports.** The node is `disk` on floppies,
   `image` on SCSI, none on ATA or the Lisa. `path` (the delta stem) and
   `filename` (the source) mean the same on both nodes. `files.images[N]`
   documents `path` as "the full host path" and `filename` as "the last
   component", which is wrong on both counts. Eject returns none on the
   floppy and bool on SCSI. Of the exports, ATA and the ProFile take no
   codec, ATA is synchronous, and the System tab's `export` handler
   (`SystemView.svelte:173`) calls `<node>.export [dest]`, which does not fit
   ATA's `(unit, path)` order. The ProFile's export is named `save`.
9. **HD eject differs.** A SCSI HD eject detaches the disk. An ATA HD eject
   is refused.
10. **Dead code.** `system_probe_floppy` (`system.c:660`) has no caller.

### 1.5 Identify today

Nine entry points answer parts of "what is this file", with different
openers and rules:

| Call | Where | Answers |
|---|---|---|
| `files.identify_floppy(p)` | `storage_class.c:624` | bool: opens and is floppy-sized |
| `machine.floppy.identify(p)` | `floppy.c:1323` | `"400K"`/`"800K"`/`"720K"`/`"1.4MB"`/`""`. The disk node's `density` spells the same values `"400k"`…`"1440k"` (verified). Missing on the Lisa, and absent before boot |
| `machine.scsi.identify_hd(p)` | `scsi.c:2219` | bool: opens, not floppy-sized, ≥ 400 KB |
| `machine.scsi.identify_cdrom(p)` | `scsi.c:2247` | bool: ISO 9660 or `format_contents == "disk"` (so any HFS disk passes, verified) |
| `machine.scsi.identify_iso(p)` | `scsi.c:2287` | bool: holds ISO 9660 |
| `files.image_error(p)` | `storage_class.c:612` | why a container will not open |
| `files.probe(p)` | `storage_class.c:631` | prints format, partitions, encoding, contents; bool |
| `files.partitions(p)` | `storage_class.c:678` | list of HFS startup partitions, numbered as attached (after the wrapper) |
| `files.archive.identify(p)` | `archive.c:389` | archive short name or `""` |

The three SCSI identify calls stay reachable before a machine exists only
through a static swap: `scsi_class_register` mounts a stub `machine.scsi`
from `core_init` (`scsi.c:2407`), `scsi_init_named` detaches it, and
`scsi_delete` restores it when `s_primary_live` falls to 0. As a result a
Lisa has a `machine.scsi` with no bus (`media-bays-lisa` asserts
`machine.scsi.hd_models`). The web app chains these calls with a duplicate
size table of its own (`lib/media.ts:290-365`, `upload.ts:623`,
`bus/media.ts:235`, `bus/vfs.ts:101`).

## 2. Target model

### 2.1 One rule for every medium

**A medium is an array of sectors, each sector 512 data bytes plus T tag
bytes, with T ∈ {0, 12, 20}.** The storage unit, which is the block of the
base view, the delta and the journal, is one sector stored **tag first**:
`[T tag][512 data]`, so `block_size = 512 + T`. Tag-first is the order both
media use on the wire: a GCR sector's data field is 12 tag bytes and then 512
data bytes [1], and a ProFile block is 20 tag bytes and then 512 data
bytes [2]. With this order the ProFile's 532-byte block becomes the general
case rather than a special one.

"Block size" means two different things, and the two are kept apart:

- the **medium sector**, always 512 data bytes plus T tag bytes. It belongs
  to the medium and is fixed when the medium is opened;
- the **device logical block**: 512 on SCSI or ATA disks, 2048 on a CD
  (changeable with MODE SELECT, `scsi_cdrom.md`), 524 on the GCR wire,
  532 on the ProFile wire. It belongs to the device and is mapped onto
  medium sectors by the device. A CD's 2048-byte block is 4 sectors, so
  `STORAGE_MAX_BLOCK_SIZE` (1024) needs no change. Raw 2352-byte CD images
  stay unsupported.

### 2.2 Kinds

The **device** chooses the kind when it opens the medium; the file's size no
longer decides it. One table in `image_kind.c` holds everything kind-specific:

| Kind | Devices | T | Accepts (also the identify predicate, §4) | Writable | Presentation | Raw export layout |
|---|---|---|---|---|---|---|
| `floppy` | Mac floppy controllers, Lisa FDC | 12 when the size is 400K or 800K (GCR media); 0 for 720K or 1440K | sector count 800, 1600, 1440 or 2880 | per insert | none | data only (`.dsk`) |
| `hd` | SCSI HD, ATA HD | 0 | opens, not floppy-sized, ≥ 400 KB | always | bare-volume wrapper (`image_wrap_volume`) | data |
| `cd` | SCSI CD, ATAPI CD | 0 | ISO 9660, or a partition map or bare volume | never | none | data |
| `profile` | Lisa ProFile | 20 | a whole number of 532-byte units, a LisaEm DiskCopy, or blank <!-- lint-allow: LisaEm --> | per insert | none | units (532, tag-first: today's raw ProFile file) |

`enum image_type`, `classify_image`, `image_set_type` and the
`image_is_floppy` size tests go away. Floppy *format* (GCR 400K or 800K, MFM
720K or 1440K) is derived from the sector count plus the drive's own
reformat tracking (`floppy_media_current`). It stays device state, see §2.9.

### 2.3 Tags: they move into the delta

| Option | base + delta complete? | Cost |
|---|---|---|
| A. Keep in memory (today) | no; tags are a checkpoint tail | two mechanisms (floppy in memory, ProFile in-band); tags lost when a delta is reopened without a checkpoint |
| B. A separate tag delta (second `storage_t` of packed tag blocks) | yes | two journals and two commits per medium, and a crash between them splits data from tags; packing code; still two mechanisms |
| C. A sidecar region inside the delta | yes | a second bitmap and region in the storage engine, which grows the engine |
| **D. In-band unit `[tag][data]`** | **yes** | base views for untagged and DiskCopy bases (§2.4); a sector API for the floppy controllers; a gather step on the data plane when T > 0 |

**Decision: D.** It is the only option with a single mechanism. The ProFile
already works this way, data and tag are journaled and committed together,
and the engine stays as it is (block size `512+T`; 524 and 532 are both
multiples of 4, as `storage_new` requires). The delta header already
records `block_size`, which is `512+T`, so the delta describes itself with
no new field (the design had a v3 header carrying `tag_bytes`).

Consequences:

- **Checkpoints.** The tag tail (`image_checkpoint` `image.c:1232`,
  `restore_image_tags`) and `image_set_tags` go away. A quick checkpoint
  needs nothing extra, because the tags are in the delta's blocks. A
  consolidated checkpoint carries them inside the block stream, at the same
  cost as today: an 800K floppy is 1600 × 12 = 19 KB.
- **DiskCopy export.** The export streams blocks and splits them into the
  data section and the tag section. The checksums are unchanged
  (`image_export_run_dc42`). MOOF export reads tags the same way. Raw and
  `.dmg` exports of a floppy write the data plane only, as today.
- **API.** Three planes, all on one `storage_t`:
  - data plane `disk_read_data/disk_write_data(img, off, buf, len)`: 512-strided
    data bytes. When T = 0 this is today's `disk_read_data` path,
    unchanged. When T > 0 the data parts are gathered, and a write keeps
    each sector's existing tag. Partition, HFS and wrapper readers use this
    plane.
  - tags `disk_read_tag/disk_write_tag(img, sector, buf, len)`: a
    sector's tag, read from or written into its block. The floppy
    controllers kept their calls (data, then tag), so they needed no change;
    the design's combined sector call was not needed for that.
  - blocks `disk_read_units/disk_write_units(img, first, buf, count)`: raw
    `[tag][data]`. The ProFile moves these (its wire block is its stored
    block); export and checkpoint stream the storage's blocks.

### 2.4 Bases: adapting files to the unit

The format registry still peels wrappers. Afterwards the medium layer
composes a base view with `512+T`-byte units:

| Innermost source | T = 0 | T = 12 or 20 |
|---|---|---|
| raw 512-multiple (any wrapper: UDIF, NDIF, QCOW2, gzip…) | direct | **zero-tag pad view**: each unit is T zero bytes followed by the data sector |
| DiskCopy 4.2 with a tag section (incl. MOOF → in-memory DiskCopy) | data view (today's `u.data`) | **merge view**: unit i = `tags[i·T]` + `data[i·512]`, generalising today's LisaEm ProFile view (`format_registry.c:117-160`) with interleave 1 <!-- lint-allow: LisaEm --> |
| LisaEm ProFile DiskCopy <!-- lint-allow: LisaEm --> | n/a | existing interleaving view (already 532 tag-first) |
| raw 532-multiple (ProFile) | n/a | direct |

A DiskCopy whose tag size differs from the kind's T is read through the
data view and padded with zero tags. A 400K or 800K DiskCopy that a kind
with T = 0 opens (none does today) drops its tags. The base file is never
written in either case.

### 2.5 Read-only, writable, blank

- **writable ⇔ a persistent delta.** A writable image always has
  `<delta_dir>/<id>.{delta,journal}`. A read-only image is write-protected:
  `disk_write_data` refuses a write, and its scratch pair is **lazy**
  (`storage_config_t.lazy`) — created only by the first write to the
  storage, which for a read-only image is only a consolidated checkpoint's
  restore over a missing base. Identify, probes, VFS mounts and CDs create no
  files. (The design had read-only images without any delta; the lazy pair
  keeps that one restore case working with no engine mode of its own.)
- **Write-protect is enforced twice.** The device reports it to the guest:
  the IWM/SWIM sense, SCSI DATA PROTECT, Lisa FDC status `$20`. The image
  refuses the write as a backstop. The Lisa FDC and the SWIM ISM write path
  check it too now.
- **`insert` is writable by default** everywhere (§6); `writable=false` asks
  for a write-protected disk.
- **Blank images** (`image_create_blank_kind(kind, blocks, instance)`) are
  writable images with no base: a delta where `image_delta_dir` puts it,
  unwritten blocks read as zeros, and a checkpoint entry with no name plus
  the size, which the restore recreates. This fixes §1.4 item 5. Creating a blank *file*
  (`fd_create`, `hd_create`, `profile_create`) is a separate verb and stays
  file-level (commit C7).
- **CD** is always read-only, whatever the caller asks.

### 2.6 Ownership, eject, delta placement

- **One owner.** The machine's media table (today `image_table`,
  `system.c:93`) owns every attached medium, the ProFile included. Devices
  borrow the pointer.
- **One eject.** Every eject — host or guest, on every device — drops the
  device's pointer and hands the image to `config_release_image`, which
  closes it and frees its table slot. The floppy did; SCSI (and so ATAPI),
  the ATA hard disk and the ProFile do now. The Lisa diskette is the one
  exception, on purpose: the Lisa FDC keeps a reinsert cache that hands the
  same image back when the same diskette goes in again, so what the OS wrote
  to it (the boot diskette's overmount stamp) survives an eject. A SCSI CD keeps
  its (empty) drive. The guest's PREVENT MEDIUM REMOVAL still refuses
  (`scsi_eject_device` `-2`). The host may eject an HD on every bus, which
  is SCSI's behaviour today, so ATA's refusal goes (§1.4 item 9).
- **One delta directory rule.** `image_delta_dir(path)` moves into
  `image_kind.c` and uses `platform_path_is_volatile`. Both copies of
  `pick_delta_dir` are removed.

### 2.7 Checkpoints

The images part (`machine_part_images`) is the only place a medium is
saved. Per entry:

`source (string, "" = blank) · kind (u8) · flags (writable, wrapped) ·
sectors (u64) · tag_bytes (u32) · instance (string) · source key (string) ·
storage blob · DiskCopy identity (name field, format byte)`

- The restore reopens with the recorded kind, so it gets the same T, the
  same base view and the same presentation. The `image_geometry_t` argument
  goes away, and so does the ProFile's private restore
  (`lisa_profile_init` `:471`, `lisa_profile_checkpoint` `:511`).
- Device parts store the image's **position** in the saved list (`u32`,
  `IMAGE_REF_NONE` for an empty drive) instead of the filename
  (`checkpoint_write_image_ref`). Matching by name cannot resolve a blank
  image, and it is ambiguous when two entries share a name. The images part
  stamps each image with its position as it saves it
  (`image_set_checkpoint_index`): NULL slots are skipped there, and it is
  saved before any device that refers to it.
- The format changes, which `checkpoint_validate_build_id` already handles.
  No migration code is needed (owner rule).

### 2.8 Export

`image_export_to_codec` (through `io_leaf_export_image`) is the only entry point and always runs as
an I/O job. The destination's name still picks the format:

| Destination | floppy (T=12) | floppy (T=0) | hd / cd | profile |
|---|---|---|---|---|
| `.dmg` | data plane, UDIF | data, UDIF | data, UDIF | refused (UDIF is 512-sector) |
| `.dc42` `.diskcopy` (`.image` for a floppy) | data + tags | data, no tags | refused | refused (see §6) |
| `.moof` | sectors + tags (GCR only) | refused | refused | refused |
| anything else | data only (`.dsk`) | data | data | units (532, today's raw ProFile) |

Every device gains `codec`. ATA's export becomes asynchronous like the
others.

### 2.9 What stays device-side

- Floppy: track and zone geometry, the GCR track cache, current-format
  tracking (`cur_format`), motor and head, DiskCopy identity defaults set on
  insert (`image_set_diskcopy_identity`, the Lisa).
- SCSI: INQUIRY identity, chosen from the drive catalog **by the SCSI
  device** rather than inside `media_open`, since ATA does not use it. Also
  the logical block size, MODE SELECT, PREVENT, unit attention.
- ATA: task-file state and the IDENTIFY data.
- ProFile: the handshake, the synthesised device-info block, PRAM (which
  stays on `machine.hd`).

## 3. Object model

### 3.1 One image node

One class, `image` (`image_class.c`), using the ops shape of C-CKOB-13
(`{image(dev, slot), eject(dev, slot)}`), is mounted as the `image` child of
every device node, and it is also each entry of `files.images`. The node
exists only while an image is loaded.

| Member | Type | Meaning |
|---|---|---|
| `kind` | enum | `floppy` · `hd` · `cd` · `profile` (`none` only in `files.images` for an image no drive holds) |
| `source` | string | the path the image was opened from; `""` for a blank one |
| `encoding` | string | wrapper layers: `raw`, `dc42`, `bin+ndif`, `moof+dc42`… |
| `size` | uint | data bytes = `sectors × 512` (tags excluded) |
| `sectors` | uint | 512-byte sectors |
| `tag_bytes` | uint | T: 0, 12 or 20 |
| `writable` | bool | the guest may write it (false = write-protected) |
| `delta` | string | instance stem; `""` when read-only |
| `reads` / `writes` / `read_errors` | uint, volatile | I/O counters |
| `export(path, [codec])` | → `true` or error | I/O job; §2.8 |
| `eject()` | → none or error | releases the image to the table; error text when locked or empty |

The node has no `present` member: a node that resolves holds an image.
Floppy `density` becomes the drive's `format` (§3.2).

### 3.2 Device nodes (device-specific state lives here)

Every device node carries `present` (an image is loaded) and
`insert(path, [writable])` / `eject()` with the same result types; the
`writable` default is `true` everywhere (§6), plus its own state:

| Device node | Device-specific members |
|---|---|
| `machine.floppy.drive[N]` (Mac and Lisa alike) | `index`, `track`, `side`, `motor_on`, `format` (`gcr400`/`gcr800`/`mfm720`/`mfm1440`, current) |
| `machine.<scsi bus>.device[N]` | `id`, `type`, `vendor`, `product`, `revision`, `block_size`, `read_only`, `info()`; `medium_present` → `present` |
| `machine.ata.device[U]` (new, U = cell·2 + device) | `type` (`hd`/`atapi`/`none`) |
| `machine.hd` (Lisa ProFile) | `pram_init/pram_save/pram_load`; `attach` → `insert`, `detach` → `eject`; `save` → `image.export` |

Bus-addressed attach verbs stay, because a bus with empty ids has no device
node to insert into: `machine.scsi.attach_hd/attach_cdrom`,
`machine.ata.attach_hd/attach_cdrom`, and
`machine.attach_media/eject_media/attach_hd/attach_cdrom`. All of them reach
the image layer through one call, `image_open_kind`.

### 3.3 Resulting paths

| Machine | Floppy | Hard disk | CD |
|---|---|---|---|
| Plus … Quadra, PDM, TNT (IWM/SWIM/SWIM3/New Age/IOP) | `machine.floppy.drive[N].image` | `machine.scsi.device[N].image` (`scsi2`, `pci_<s>` likewise) | `machine.scsi.device[3].image` |
| Gossamer (pmg3dt/pmg3mt) | `machine.floppy.drive[0].image` | `machine.ata.device[U].image` or `machine.scsi.device[N].image` | `machine.ata.device[U].image` (the ATAPI back end's image, also reachable at `machine.atapi.device[U].image`) |
| Lisa / XL | `machine.floppy.drive[0].image` | `machine.hd.image` | — |
| every machine | `files.images[i]`: the same node class over the image table | | |

### 3.4 Renamed and removed surface (no aliases)

| Before | After |
|---|---|
| `…drive[N].disk` | `…drive[N].image` |
| `.filename` / `.path` / `.raw_size` / `.format` / `.type` (on the image nodes and `files.images[i]`) | `.source` / `.delta` / `.size` / `.encoding` / `.kind` |
| `.disk.density` | `drive[N].format` |
| `.disk.present`, `.image.present`, scsi `medium_present` | device `present` |
| Lisa `drive[0].export`, `drive[0].filename` | `drive[0].image.export`, `.image.source` |
| `machine.hd.attach/detach/save` | `machine.hd.insert/eject`, `machine.hd.image.export` |
| `machine.ata.export(unit,path)`, `machine.ata.devices` | `machine.ata.device[U].image.export(path)`, `device[U].type` |
| `machine.floppy.create(path,[hd])`, `files.fd_create/hd_create/profile_create` | `files.create(path, kind, size)` + `drive[N].insert` (C7) |

## 4. Identify

### 4.1 `files.identify(path)`

Identify lives in `image_kind.c` and is exposed as `files.identify`.
`files` is a root service that exists before any machine, so no facade is
needed. It opens the file once, read-only, with no kind (T = 0, no wrapper),
then asks the archive layer and the kind table. It always returns a map: a
file that is no image is a map with `opens: false` and a reason, not an
error.

| Field | Type | Meaning |
|---|---|---|
| `opens` | bool | the image layer opens it |
| `error` | string | why a container will not open, else `""` |
| `archive` | string | `sit`/`cpt`/`zip`/`tar`/`hqx`/`bin`/`gz` when the archive layer recognises it, else `""` |
| `encoding` | string | wrapper layers peeled |
| `file_size`, `size`, `sectors` | uint | host bytes; disk data bytes; 512-byte sectors |
| `tags` | uint | tag bytes per sector the file *carries* (DiskCopy tag section, 532-byte layout), else 0 |
| `floppy` | string | `gcr400`/`gcr800`/`mfm720`/`mfm1440` when floppy-sized, else `""` |
| `map` | string | `apm` or `""` |
| `hybrid` | bool | ISO 9660 plus an HFS side |
| `volumes` | list | `{partition, name, filesystem, powerpc_booter}`; `filesystem` ∈ `hfs`, `hfs+`, `mfs`, `ufs`, `iso9660`, …; `partition` is the number **as attached as a hard disk** (after the wrapper), which is what an Open Firmware path names |
| `fits` | list | kinds whose accept predicate (§2.2) passes, in order `floppy`, `hd`, `cd`, `profile` |
| `kind` | string | the one kind an upload of no stated kind is filed as: ISO 9660 → `cd`; floppy-sized → `floppy`; 532-byte layout only → `profile`; else `hd` when it fits; else `""` |

At the REPL the map prints in readable form, so `files.probe` (a second
printer of the same facts, which had already drifted from the identify
predicates) is removed. `files.partmap` stays: it dumps the raw partition
table, not the identity.

### 4.2 Replacements

| Removed | Use |
|---|---|
| `files.identify_floppy(p)` | `"floppy" in files.identify(p).fits` |
| `machine.floppy.identify(p)` | `files.identify(p).floppy` |
| `machine.scsi.identify_hd(p)` | `"hd" in …fits` |
| `machine.scsi.identify_cdrom(p)` | `"cd" in …fits` |
| `machine.scsi.identify_iso(p)` | `…kind == "cd"` |
| `files.image_error(p)` | `…error` |
| `files.archive.identify(p)` | `…archive` (`files.archive.extract/import` stay) |
| `files.partitions(p)` | `…volumes` |
| `files.probe(p)` | `files.identify p` at the REPL; `assert files.identify(p).opens` in scripts |
| `system_probe_floppy` (dead) | deleted (C1) |

`system_find_media` uses the identify core (`fits` holds `floppy`). The
web's size table (`lib/media.ts`) is deleted: every `validate` makes a
single `files.identify` call.

### 4.3 The SCSI facade goes

With identify on `files`, the only member the static `machine.scsi` still
carries is `hd_models`, which is catalog data: it moves to
`catalog.hd_models`. `scsi_static_class`, `s_scsi_static_object`,
`scsi_class_register`, `scsi_static_detach`, `s_primary_live`,
`SCSI_CATALOG_MEMBERS` and the `core_init.c` call go — C-CKOB-16's facade
change, done by removal. A Lisa then has no `machine.scsi`.

## 5. Migration plan

Each step is one commit (split where review is easier) that builds, passes
its listed tests and updates its callers, docs and frontend in the same
commit (no aliases, no shims).

| # | Step | Landed |
|---|---|---|
| C1 | **One open path + kinds.** `image_kind.c`: the kind table, `image_open_kind(path, {kind, writable, instance, delta_dir})`, `image_delta_dir`, `image_create_blank_kind`; `media_open`, `do_insert_fd`, `system_create_floppy`, `lisa_profile_attach` and the checkpoint restore route through it; the kind is stored on the image and in its checkpoint entry; `files.images[].type` reports it. Removes `enum image_type`, `classify_image`, `image_set_type`, the ProFile's `pro_delta_dir`, `system.c`'s `pick_delta_dir`, the public `*_with_geometry` openers, `system_probe_floppy`. Fixes §1.4 items 1, 2, 10 | yes |
| C2 | **One owner, one eject, one checkpoint path.** The ProFile in the image table; every eject releases its image to the table (`config_release_image`; the Lisa diskette excepted, §2.6); refs by position; blank images with a persistent delta; read-only images write-protected, with a lazy scratch pair; write-protect checks in the Lisa FDC and the SWIM ISM; `insert` writable by default everywhere; host eject of an ATA hard disk. Fixes items 4–7, 9 | yes |
| C3 | **Tags in-band.** Blocks `[T][512]`; the tagged base view (a DiskCopy tag section, or zeros); `disk_read_tag/disk_write_tag` on blocks, `disk_read_units/disk_write_units` for the ProFile; DiskCopy and MOOF export split the tags off the streamed blocks; the checkpoint tag tail, `image->tags` and `image_set_tags` go. Fixes item 3 | yes |
| C4 | **`files.identify`.** The identify core and method; the nine entry points of §4.2 and the SCSI facade go; `catalog.hd_models` | |
| C5 | **One image node.** `image_class.c` on every device and on `files.images`; the ATA `device[U]` collection; the renames of §3.4; every export an I/O job with a codec; gs-test-data scripts updated in that repository. Fixes item 8 | |
| C6 | *Dropped* (§6): the type keeps its name, `image_t`, with `image.h` and the `disk_*` I/O calls | — |
| C7 | **`files.create(path, kind, size)`** replaces `files.fd_create/hd_create/profile_create` and `machine.floppy.create`; profile blank-disk entries name `{kind, size}` | |

### 5.1 Risks

- **Write-protect becomes real (C2).** Writes that landed silently in a
  ghost delta now fail. The device write-protect checks land in the same
  commit, and with `insert` writable by default the only read-only images
  are the ones a caller asked to be write-protected.
- **Six floppy controllers change their sector I/O (C3)** in hot paths
  (track build and flush); every controller is covered by a boot row.
- **Ownership move (C2).** One owner and one release path remove the
  double-free class recorded at `lisa_fdc.c`, but a teardown that also
  closes (`lisa_profile_delete`) has to be cut in the same commit.
- **Ref ordinals (C2)** have to match the saved order: NULL slots are
  skipped at save. A unit test pins this.
- **Cross-repository (C5).** The gs-test-data scripts break until updated;
  the two land together.
- **Upload validation (C4)** decides what the web stores where; the e2e
  upload specs are the guard.

## 6. Decisions

1. **Insert default:** writable everywhere; read-only means explicitly
   write-protected (C2).
2. **Node name:** `image` on every drive and in `files.images`, not
   `medium`. The kind module is `image_kind.c` and the node class
   `image_class.c`.
3. **DiskCopy export of a ProFile:** refused. A LisaEm ProFile DiskCopy is <!-- lint-allow: LisaEm -->
   read for compatibility only.
4. **C7** is in scope.
5. **C6 dropped.** With the node named `image`, renaming `image_t` to
   `medium_t` would reintroduce the split the owner removed: the type, its
   header and the `disk_read_data`/`disk_write_data` calls keep their
   names, with `disk_read_tag`/`disk_write_tag` and
   `disk_read_units`/`disk_write_units` beside them.

## References

1. *Inside Macintosh*, Volume II: The Disk Driver (sector tags; 12 tag bytes per GCR sector); see also [iwm-floppy.md](../../../reference/hardware/iwm-floppy.md), "Sector Format".
2. Lisa ProFile block layout: [lisa.md](../../../reference/machines/lisa/lisa.md) §14 and [profile.md](../../../reference/machines/lisa/profile.md).
3. [diskcopy42.md](../../../reference/formats/diskcopy42.md) (data then tag section; checksums).
4. [image.md](image.md), [storage.md](storage.md), [source.md](source.md), [bare-volume-wrapper.md](bare-volume-wrapper.md).
