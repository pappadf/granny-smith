# ATA / ATAPI channel

`src/core/peripherals/ata.c` (`ata.h`) models one ATA (IDE) channel: the
task file, up to two devices on it — an ATA hard disk over the image
layer, or an ATAPI CD-ROM whose PACKET commands run on the SCSI CD-ROM
model — PIO and DMA data transfer, and the INTRQ line.  It is
family-clean: the I/O controller that carries the channel decodes the
registers, wires INTRQ to its interrupt controller, gates the cell and
attaches a DBDMA channel to the data port.  Its one user today is the
beige G3's Heathrow (`src/machines/gossamer/gossamer_ata.c`, two cells).

Hardware facts are cited from the references: ATA/ATAPI-4 (T13 1153D) for
the task file, protocols and command set; SFF-8020i for the ATAPI packet
interface; the Heathrow wiring from
[g3.md §4.4](../../../reference/machines/g3/g3.md).

## 1. Responsibilities & design

The model is a **synchronous device**: every command completes during the
host's command-register write.  A read's first block is staged and DRQ is
up before the write returns; a DMA command has its data ready for the
DBDMA port; a non-data command posts its status at once.  The device never
shows BSY to the host except inside a software reset.  This keeps the
model free of scheduler events (and so of checkpointed timers), and it is
safe because **INTRQ is a level** held until the host reads Status (not
Alternate Status) or writes a new command — a host that enables its
interrupt late still takes it.

Each device keeps **its own copy of the task file** (status, error,
sector count, sector, the two cylinder registers, its INTRQ), as real
devices do: a host write to a task-file register reaches both devices,
but reads come from the selected one.  That is what lets a host select
device 1 after a reset and read *its* signature while device 0 holds a
different one.  Feature, Device/Head and Device Control are channel-wide.

ATAPI devices are not attached to the channel directly.  The channel
holds a pointer to a SCSI bus (the *ATAPI back end*) and treats the
CD-ROM at SCSI id `index * 2 + device` as its device: every register
access calls `ata_refresh_devices`, which picks up a drive that appeared
on the back end (e.g. `machine.atapi.attach_cdrom`) and drops one that
left.  A drive with no disc stays a device — it answers TEST UNIT READY
with NOT READY, as a real drive does.

## 2. Key types & files

| Item | Role |
|---|---|
| `ata_channel_t` | One channel: the channel-wide registers, the transfer state, `dev[2]`, then the runtime pointers (staging buffer, ATAPI response, images, back end, IRQ and DMA-kick callbacks). Everything before `buf` is plain data and is checkpointed as one block. |
| `ata_dev_t` | One device: kind, its task-file copy, its INTRQ, the CHS translation, capacity, MULTIPLE size, transfer mode, model and serial strings. |
| `ata_read` / `ata_write` | Task-file registers by index (`ATA_REG_*`: the register's offset / 16 on Heathrow). |
| `ata_read_data16` / `ata_write_data16` | The 16-bit data port; the first byte of the stream is in bits 15:8 — the order a big-endian host's halfword load presents. |
| `ata_read_altstatus` / `ata_write_devctl` | The control block. |
| `ata_dma_in` / `ata_dma_out` | The data port in `dbdma_port_t` shape: bytes move only while a DMA command is in its data phase; a short return stalls the channel until the next kick. |
| `ata_set_enabled` / `ata_hard_reset` | The I/O controller's cell enable and RESET- line. |
| `tests/unit/suites/ata/` | The unit suite (§6). |

## 3. Behaviour

### 3.1 Signatures and reset

After a hard reset, the release of a software reset (SRST 1 → 0) or
EXECUTE DEVICE DIAGNOSTIC, each device leaves its signature: error 01,
sector count 01, sector 01, and cylinder 00/00 with status DRDY|DSC for a
disk, 14/EB with status 00 for an ATAPI device (ATA-4).  While SRST is
held both devices show BSY.  A reset also selects device 0 and clears the
MULTIPLE setting.  An ATAPI device answers IDENTIFY DEVICE (`$EC`) by
aborting with its signature in place — how a host tells the kinds apart.

Reads with no device present return `$7F` (nothing drives the bus; DD7 is
pulled down); reads of an absent device 1 while device 0 is present
return 00.  A disabled cell (the I/O controller's enable clear) floats at
`$FF` and ignores writes.

### 3.2 ATA commands

| Command | Opcode | Notes |
|---|---|---|
| IDENTIFY DEVICE | `$EC` | 512 bytes: fixed disk, default 16/63 geometry capped at 16 383 cylinders, LBA and DMA capable, MULTIPLE up to 16, multiword DMA 0–2 (and the SET FEATURES selection), PIO 3/4, ATA-1..4, integrity word |
| READ / WRITE SECTORS | `$20/$21`, `$30/$31` | PIO, one sector per DRQ block |
| READ / WRITE MULTIPLE | `$C4`, `$C5` | PIO, MULTIPLE sectors per block; aborted until SET MULTIPLE |
| READ / WRITE DMA | `$C8/$C9`, `$CA/$CB` | through the DMA port, staged 256 sectors at a time |
| READ VERIFY | `$40/$41` | range check only |
| SET MULTIPLE MODE | `$C6` | 0, 2, 4, 8, 16 |
| SET FEATURES | `$EF` | `$03` records the transfer mode; the rest are accepted |
| INITIALIZE DEVICE PARAMETERS | `$91` | sets the CHS translation |
| SEEK, RECALIBRATE | `$70`, `$1x` | complete at once |
| power management, FLUSH CACHE | `$E0`–`$E3`, `$E6`, `$E7`, `$94`–`$99` | complete at once; CHECK POWER MODE answers active (`$FF`) |
| EXECUTE DEVICE DIAGNOSTIC | `$90` | both devices' signatures |

Anything else aborts (DRDY|ERR, error ABRT).  An address past the end, or a
CHS address with sector 0, aborts with IDNF.  LBA28 and CHS addressing are
both decoded; on completion an LBA command leaves the task file pointing at
the last sector it moved.

The PIO protocol is ATA-4's: a read posts INTRQ with each DRQ block and
none after the last; a write takes its first block without an interrupt
and posts INTRQ after each block, the last one included.  DMA posts one
INTRQ at completion.

### 3.3 The ATAPI packet path

PACKET (`$A0`) raises DRQ with interrupt reason CoD (accelerated DRQ, no
interrupt) and collects the 12-byte packet.  The channel then runs it on
the back end through the external-initiator API: select the target, push
CDB bytes while the bus is in COMMAND, then either drain DATA IN into a
response buffer (up to 4 MB) or, for DATA OUT, take the host's bytes first;
then the status and message bytes and the bus release.

A response is handed over in chunks of the host's byte-count limit (the
cylinder registers at PACKET time; 0 means `$FFFE`), each chunk posting
INTRQ with interrupt reason IO and the chunk length in the cylinder
registers; the completion posts INTRQ with IO|CoD.  With the PACKET
feature's DMA bit the response goes through the DMA port instead.  A CHECK
CONDITION completes with ERR and the target's sense key in error bits 7:4
— the host follows with REQUEST SENSE, which is another packet.

IDENTIFY PACKET DEVICE (`$A1`) answers word 0 `$85C0`: ATAPI, CD-ROM,
removable, accelerated DRQ, 12-byte packets.

The CD-ROM model learned two commands for this path: READ(12), which the
Mac OS ROM's ATAPI driver reads with, and MODE SENSE(10) with the CD
capabilities page `$2A`, which the disc's own driver asks for
([scsi_cdrom.md](scsi_cdrom.md)).

## 4. Object-model / shell surface

The channel has no object of its own; the owning machine publishes one.
On the G3 that is `machine.ata` (`devices`, `attach_hd(path, unit)`,
`attach_cdrom(path, unit)`, unit = cell × 2 + device) and the back end's
`machine.atapi` (the standard `scsi` class).  The `ata` log category
traces commands at level 3 and packet contents and completions at level 4.

## 5. Checkpointing

`ata_checkpoint_save` writes the plain-data block, the 128 KB staging
buffer, the ATAPI response (its length is in the block) and each HD
image's filename; `ata_checkpoint_restore` re-binds the images through the
machine's image table — a disk whose image is gone restores as absent.
The INTRQ line is re-derived, not re-driven: the owner's interrupt
controller restores its own source levels.  A transfer interrupted by a
checkpoint resumes mid-block.

## 6. Testing

`tests/unit/suites/ata/` builds a channel with an ATA disk on an in-memory
medium and an ATAPI CD-ROM on the real SCSI bus and CD-ROM model, and
drives it as a host driver does: both signatures, IDENTIFY with its
integrity word, multi-block PIO reads with per-block interrupts, PIO
writes, MULTIPLE, DMA in and out, IDNF past the end, nIEN, software reset,
the empty and disabled cell, a CD drive appearing on the back end, the
ATAPI identify pair, INQUIRY, a READ(10) split at the byte-count limit, a
DMA read, the UNIT ATTENTION → NOT READY pair after an eject, and a
checkpoint taken mid-read.

At machine level the G3 rows boot the Mac OS 9.2.1 CD from the ATAPI drive
and initialize and install onto an ATA disk.

## 7. Known debts

- No command timing: no BSY period, no seek or rotational latency.  A
  driver that measures how long a command takes sees zero.
- LBA48 and the ATA-5+ commands are absent; Ultra DMA modes are not
  advertised.
- The ATAPI path handles one DATA OUT chunk (MODE SELECT-sized); a
  multi-chunk ATAPI write is not modelled.
- An ATAPI device is always a CD-ROM; ATAPI Zip and tape are not modelled.

## 8. See also

[scsi.md](scsi.md), [scsi_cdrom.md](scsi_cdrom.md),
[g3.md](../../../reference/machines/g3/g3.md) §4.4, the G3 family page
[gossamer.md](../../machines/gossamer/gossamer.md).
