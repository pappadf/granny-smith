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
data is served with `disk_read_data` / `disk_write_data`. Completion raises
**FDIR** on VIA1 PB4, which the boot ROM polls (`CHKFIN`). The per-machine
`hw_profile_t.fd_insert` / `fd_present` hooks route floppy insertion here
instead of the IWM/SWIM `cfg->floppy`.

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
Driving the loaded software further (and booting Lisa Office System / MacWorks
to the desktop) is the remaining UI/keyboard-injection work.
