# Lisa floppy controller — implementation notes

`src/core/peripherals/lisa_fdc.{c,h}` models the Apple Lisa intelligent floppy
controller (a 6504A coprocessor + 1 KB shared RAM), reached at physical
`$00C001`. It is **not** an Apple IWM: the 68000 writes a high-level command
block into the shared RAM and the coprocessor returns logical 512-byte sectors,
so the model is behavioural (the iop_swim.c pattern) and never touches GCR
cells. See [docs/lisa.md](lisa.md) §13 for the hardware reference.

## Shared-RAM layout (verified against the rev-H boot ROM, `RM248.B.TEXT`)

Controller RAM byte N is at physical `$C001 + 2*N` (the RAM sits on the odd
bytes of the 68000 bus; the ROM accesses it with `MOVEP`). Command block:

| byte | field |
| --- | --- |
| 0 | command-issue register (`$81` execute, `$83` seek, `$85` clear-status, `$86/$87` int-mask, `$88` cold-start) — reads 0 once the command is taken |
| 1 | RWTS command (`0`/`7` read, `1` write, `2` unclamp, `3` format) |
| 2 | drive (`$00` = drive 2, `$80` = drive 1) |
| 3 / 4 / 5 | side / sector / track |
| 6 / 7 / 8 | speed / format-confirm / error status (`0` = OK) |
| 47 (`$C05F`) | drive-status (present / eject / RWTS-complete) |
| 500-511 | 12-byte sector tag / header (`DSKBUFF`) |
| 512-1023 | 512-byte data sector (`DSKDATA`) |

`(track, side, sector)` maps to an image byte offset via the Sony 5-zone
geometry helpers (`iwm_disk_image_offset` / `iwm_sectors_per_track`), and the
data is served with `disk_read_data` / `disk_write_data`. The per-machine
`hw_profile_t.fd_insert` / `fd_present` hooks route floppy insertion here
instead of the IWM/SWIM `cfg->floppy`.

### Completion — interrupt-driven (IPL 1) and deferred

Completion raises **FDIR**, which goes to **two** places (docs/lisa.md §7.1/§13):

1. **VIA1 PB4** — a pollable level the boot ROM watches (`CHKFIN`). The ROM runs
   with IPL ≥ 1, so it polls PB4 rather than taking the interrupt.
2. **IPL 1** — the floppy is a level-1 interrupt source (shared with VIA2/video).
   The Lisa OS Sony driver (`SOURCE-SONYASM`, `WAIT_INT`) issues an
   interrupt-generating command and **blocks the reader process** waiting for the
   completion interrupt; the level-1 handler reads `$00C05F` (§13.3) to service it.
   `lisa.c` therefore aggregates a third level-1 sub-source (`l1_floppy`) alongside
   the VIA2 IRQ and the video VBL.

Crucially the completion is **deferred**, not synchronous. Real floppy reads take
milliseconds; the OS driver blocks *after* issuing the command, then waits for the
interrupt. If the controller raised FDIR in the same instruction as the
command-register write, the IPL-1 interrupt would fire *before* the driver had
blocked — the wake-up would be lost and the reader process would hang forever (the
LisaOS "scheduler idles" symptom). So `lisa_fdc.c` latches the sector data
immediately but signals `COMPLETE` + FDIR on a scheduler event a sector-read time
later (`fdc_complete`, `FDC_COMPLETE_CYCLES` ≈ 4.7 ms). The boot ROM (which polls
PB4) is unaffected by the latency.

> Note: a faithful `STOP` instruction is a prerequisite for the deferred model to
> matter — the OS scheduler's idle `Pause` is `STOP #$2000`, and the CPU must
> actually halt there until the floppy IPL-1 interrupt arrives. See docs/lisa.md
> §7.1 and the CPU core's `STOP` handling.

### Disk-type byte (byte 10) — drives the loader's geometry

Controller RAM **byte 10** reports the geometry of the media currently in the
drive.  The MacWorks PREBOOT loader's block→(track, sector) converter reads it
(`MOVE.B $15(A0),D1` against the `$FCC000` window) and selects the disk's total
block count from it: `0` → 1702 blocks (the original Lisa Twiggy/FileWare
default), non-zero with bit 0 set → 800 blocks (Sony 400 KB single-sided),
bit 0 clear → 1600 blocks (Sony 800 KB double-sided).  We therefore set byte 10
to `$01` (400 KB) or `$02` (800 KB) whenever media is present.  **Without this
the loader assumes 1702-block Twiggy geometry, computes out-of-range
(track, sector) pairs for any block past track 0, and aborts with its own
"DISK READ ERROR".**  With it, MacWorks reads the whole boot disk (through
track 38+) and loads the Mac environment into high RAM.

### Disk-in-drive byte (byte 32, `$FCC041`) — Sony driver presence check

Controller RAM **byte 32** (`$FCC041`) is **non-zero when a disk is in the drive**.
LisaOS's Sony driver polls it at drive init (`SOURCE-SONYASM` `ISDISKIN`:
`MOVE.B DISKIN(A2),D0`, `DISKIN .EQU $41`) and only marks the volume present
(`disk_present := gooddisk`) when it reads non-zero; otherwise `FS_Mount` aborts
the boot-volume mount with `nodiskpres` (614), surfaced as startup error **10707**
(`stup_fsinit`).  We set it (with the disk-type byte) whenever media is present.
This is separate from the latched `$C05F` event bits (§13.3) — it is a live
presence flag, not a one-shot event.

### Disk-controller ROM id (byte 24, `$FCC031`) — machine type

Controller RAM **byte 24** (`$FCC031`, `idx = (offset>>1)`) is the disk-controller
ROM id the boot ROM (`SETTYPE`) and Lisa OS (`SOURCE-STARTUP`, `adr_ioboard`) read
to detect the machine and choose the **Twiggy vs Sony floppy driver** (see
`docs/lisa.md` §16.2 for the full byte→model table). It is **not** a command-block
field. A `calloc`-zeroed value reads as a Lisa 1 ⇒ the OS installs the Twiggy
driver on our Sony hardware and OS startup strands. `machine_lisa` therefore sets
it to `$A0` (`iob_sony`, Lisa 2/5) in `lisa_init` via `lisa_fdc_set_diskrom()`;
`machine_macxl` leaves it `0` (MacWorks XL's loader-eject only matches SYSTYPE 0).
Parameter memory (boot volume, contrast, etc.) also lives in this shared RAM at
`$FCC181` (= byte 192, 32 words, `CHKPM` rotate-sum checksum; `docs/lisa.md` §13.4).

## Bring-up status

With the segment MMU, video, the two VIAs, the COPS (incl. keyboard scan, clock
read, and periodic mouse reports), the SCC, the serial-number PROM, and the
parity-error NMI all in place, the rev-H boot ROM now **completes the entire
power-on self-test and auto-boots**, reading the inserted floppy through this
controller — verified headless: `fdc read trk=0 sec=0 → status=00`, then
`trk=1 sec=0`, etc.

### Boot-block tags (DiskCopy 4.2)

The boot ROM validates the boot block by reading sector 0's **tag** (12-byte
header) and checking `FILEID` (tag offset 4) `== $AAAA` (`BOOTPAT`). DiskCopy
4.2 images store these per-sector tags in a section after the data; the image
layer now loads that section (`image_load_diskcopy_tags`) and exposes
`disk_read_tag()`, and this controller fills the header buffer with the real
sector tag on every read. So the boot block validates (its tag is all `$AA` →
`FILEID = $AAAA`) and other blocks carry their true file-ids — exactly what the
loaded OS expects.

## Result: the Lisa boots from the floppy

With tags in place the rev-H ROM **auto-boots the inserted disk, loads the boot
loader to `$00020000`, and hands off to it** — verified headless on the
`lisa2_sys_dia_3.0` diagnostics disk: after the ROM's floppy reads the CPU runs
the loaded code in RAM (PC `$0208xx`), pulls in further sectors across multiple
tracks (status `00`), and renders the loaded tool's screen with no boot error.

## Media swap (the MacWorks XL two-disk boot)

The Lisa drive is **software-eject**: the `unclamp` RWTS sub-command (`$02`)
removes the disk, leaving the drive empty until new media is inserted. Insertion
latches a drive event and raises **FDIR (IPL 1)** so the host re-examines the
drive (`lisa_fdc_insert`). This is exactly the MacWorks XL boot flow: the loader
disk is read, then unclamped/ejected; the Mac ROM sits on its "?" disk; and when
a system disk is inserted the insertion interrupt prompts it to read the new
disk and boot. There is **no swap queue or auto-feed** — a disk is inserted the
normal way (`floppy.drives[0].insert`, routing through `sys_fd_insert`), which
the Lisa machine exposes as a small object tree over its one Sony drive. The
`tests/integration/xl-boot` test drives this: run to the eject point, insert the
system disk, and verify the Finder desktop renders (608×431, §8).

Two FDC behaviours this relies on: the `$C05F` status byte holds **latched
interrupt events** that `CLRSTAT` ($85) drains (not live state — see
docs/lisa.md §13.3); and controller byte 10 reports the **Sony disk type** so
the loader picks the right block→(track,sector) geometry (see above).
