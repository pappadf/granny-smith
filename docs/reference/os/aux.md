# Apple A/UX 3.x

**Contents:**

1. [Scope & identity](#1-scope--identity) — what A/UX is, its ancestry, the evidenced version
   and media, the supported-hardware matrix, machine identification inside the OS
2. [Boot architecture](#2-boot-architecture) — the two-layer design; the Macintosh-side
   bootstrap and the enabler gate; the kernel image and its load; the MMU/VBR handoff;
   kernel startup, the autoconfig probe and the root mount; process 1 and the console;
   the root filesystem and the partition/slice model
3. [Hardware interface](#3-hardware-interface) — per-machine dispatch inside one kernel;
   interrupt handling; the SCSI stack; IIfx chunked bus-master DMA; serial and the SCC;
   time-of-day
4. [Installation & bring-up](#4-installation--bring-up) — the retail media set; RAM and
   other requirements; the installer flow and its mount strategy; what gets installed
5. [Observed behaviour & quirks](#5-observed-behaviour--quirks)
6. [Open questions](#6-open-questions)

References

---

## 1. Scope & identity

### 1.1 What A/UX is

A/UX is Apple's Unix operating system for Motorola 68030- and 68040-based Macintosh
computers. It is a hybrid: an AT&T-derived System V kernel lineage — the shipped startup
scripts carry the banner `UniPlus V.2.1.4 (ATT 1.12)` [5] (*observed*, `/etc/rc`), and
the manuals credit AT&T Information Systems and UniSoft [3] — combined with Berkeley
facilities: the Programmer's Reference states that A/UX supports "System V file systems
(SVFS) and Berkeley 4.2 file systems (UFS)" and that it does not support Macintosh file
systems as mountable, while "the A/UX finder may read and write these file systems"
[3] fstab(4) context, §"Introduction". The same introduction describes the manual set's
coverage of NFS, RPC and Internet subsystems [3].

The defining architectural feature is that the Macintosh is not replaced by the Unix
system but embedded in it: the machine boots through the ordinary Macintosh ROM and
Macintosh System Software path, a Macintosh-side launcher hands control to a Unix
kernel image, and the Macintosh System environment continues to run afterwards as
processes inside the Unix system — the A/UX Finder reads and writes the same disks
[3]. One kernel binary contains the machine-specific driver paths for several
Macintosh models (§3.1), so the supported machines run the same kernel image.

This page documents A/UX 3.0.1, the version the primary-evidence corpus covers: the
kernel binary, its disassembly and symbol table [1], the annotated SCSI driver
disassembly [2], Apple's A/UX Programmer's Reference [3], the retail installation media
[4], an extracted root-filesystem specimen [5], and recorded boot traces of the
installer and installed-disk flows [6] [7] [8] [9]. Claims are marked *observed* (with
the evidence class) or *inferred — unverified*; what the corpus does not establish is
collected in [§6](#6-open-questions). The evidence base for this page is thin in
places — in particular, no Apple document in the corpus states the supported-machine
matrix, the installation requirements or the version history — and the page is
deliberately leaner than its subject deserves, with the gaps made explicit.

### 1.2 Version, media and dating

| Item | Value | Evidence |
|---|---|---|
| Evidenced version | A/UX 3.0.1 | media labels and recorded boot traces [4] [6] |
| Kernel object format | m68k COFF, magic `$0150`, flags `$0203` | [1] |
| Kernel build timestamp | Unix epoch 735590115 = 1993-04-23 18:35:15 UTC | [1] (*observed*) |
| Kernel file size | 857,878 bytes (with symbol table) | [1] |
| Defined kernel symbols | 5,935 raw, 4,965 kept | [1] |
| Installer boot floppy | "Installation Boot Disk", 1,440 KB HFS | [4] [6] |
| Installer boot floppy, SE variant | "Installation Boot Disk SE", 800 KB DiskCopy HFS | [4] [7] |
| Install CD-ROM | ~407 MB retail CD, Apple partition map, HFS partition plus three `Apple_UNIX_SVR2` slices | [4] [6] |
| System software on the boot floppy | System 7-era System file, System Enabler 040 | [4] [6] |

The kernel image's build stamp places the 3.0.1 kernel in April 1993 (*observed* [1]);
nothing in the corpus dates the release itself. Which other 3.x versions exist (3.0,
3.1), which machines they support, and how their kernels differ from the evidenced
3.0.1 binary is not established by this evidence base
([§6](#6-open-questions), item 1).

### 1.3 The supported-hardware matrix

No Apple-published machine-support matrix is in the corpus. Two independent pieces of
evidence bound it:

1. **The installer boot floppy's machine gate.** The "Installation Boot Disk" carries
   exactly one enabler-style extension, "System Enabler 040", whose machine list names
   five machines [6] (*observed*; [§2.2](#22-the-macintosh-side-bootstrap-boot-blocks--boot-resource--enabler-gate)
   for the mechanism):

   | Machine list ID | Machine (as identified in the recorded investigation) |
   |---|---|
   | `$2E` | Quadra 900 / 950 |
   | `$2F` | Quadra 700 |
   | `$18` | Macintosh IIci |
   | `$1E` | Centris 610 / Quadra 610 |
   | `$1D` | Centris 650 / Quadra 650 |

2. **Recorded boots.** A/UX 3.0.1 is recorded booting end-to-end on four additional or
   overlapping machines [6] [7] [8] [9] (*observed*):

| Machine | CPU | Boot path exercised | Recorded end state |
|---|---|---|---|
| Macintosh SE/30 | 68030 | retail installer, "Installation Boot Disk SE" floppy | installer UI; installed-disk flow reaches the graphical login [6] [9] |
| Macintosh IIx | 68030 | installed HD image | root login [6] |
| Macintosh IIci | 68030 | installed HD image | graphical login, 8-bit video [6] |
| Macintosh IIfx | 68030 | installed HD image | graphical login; X11 session; chunked SCSI DMA path [6] [8] |

The IIci appears in both lists; the SE/30, IIx and IIfx boots in the recorded corpus
all start from an already-installed disk image (or from the SE floppy variant), not
from the "Installation Boot Disk", so the enabler's five-machine list bounds the
*installer floppy*, not necessarily the OS ([§6](#6-open-questions), item 1). That the
3.x series targets 68030/68040 machines only is consistent with both lists — every
named machine is a 68030 or 68040/68LC040 design — but is *inferred from the machine
lists*, not stated in any Apple document in the corpus.

### 1.4 Machine identification: Gestalt and the A/UX-internal machine code

A/UX 3.0.1 uses two distinct machine-identification conventions, and conflating them
is a recorded investigation dead end [6]:

- **The Macintosh machine-type byte** (low-memory global `$0CB3`, `$03` on the SE/30),
  plus the ROM machine-type word at ROM base + 8 (`$0178` for the SE/30 ROM,
  checksum `$97221136`). The Mac-side boot code tests these ([§2.2](#22-the-macintosh-side-bootstrap-boot-blocks--boot-resource--enabler-gate));
  the enabler machine lists use this convention [6].
- **The kernel's own machine code**, a word global at kernel address `$0005ABBA`,
  equal to *Gestalt `('mach')` minus 2*: `$0007` on the SE/30 (Gestalt `$0009`),
  `$000B` on the IIfx (Gestalt `$000D`) — *observed* [1] [6]. The kernel dispatches
  machine-specific driver paths on this value; writing the plain Gestalt value here
  breaks downstream kernel initialization, as the recorded investigation established
  by accident [6].

For the machine hardware behind these identifiers, see
[iifx.md §1.3](../machines/iifx/iifx.md) (identity of the IIfx, its ROM checksum
`$4147DD77` doing double duty as the reset stack pointer) and
[glue.md §5](../machines/glue/glue.md) (the GLUE-family interrupt and identification
architecture the SE/30, IIx and IIcx share).

## 2. Boot architecture

### 2.1 Two layers: Macintosh bootstrap under, Unix kernel over

A/UX does not replace the Macintosh boot path; it rides it. The full chain, as recorded
on the SE/30 installer flow with confirmed addresses [6] and on the IIfx installed-disk
flow [6] [8] [9]:

```
Macintosh ROM reset and POST
  → Slot Manager / video bring-up
  → SCSI boot scan: disk drivers from Driver Descriptor Maps
  → HFS boot blocks of the boot volume
  → 'boot' resource: secondary bootstrap
       → enabler/extension machine gate ('gbly' resources)
       → Macintosh System environment completes
  → startup application ("A/UX Startup" on an installed disk; the installer
    application on the install medium)
  → kernel image load at its linked addresses
  → kernel bootstrap: MMU on, interrupt fence, machine dispatch
  → Unix kernel: autoconfig probe, root mount, process 1
  → /etc/init, /etc/inittab: multi-user bring-up
  → graphical login on the framebuffer
```

The Macintosh half of this chain is documented in machine-family terms elsewhere in
this tree and is cited, not restated, here: the SE/30 ROM phases, boot blocks,
`'boot'` resource, enabler gate and installer milestones are tabulated in
[se30.md, "A/UX 3.0.1 Retail Installer Boot Sequence"](../machines/glue/se30.md#aux-301-retail-installer-boot-sequence);
the IIfx ROM path from reset through POST to the boot-device search is
[iifx.md §7](../machines/iifx/iifx.md); the IIx and IIci boot summaries are
[iix.md §5](../machines/glue/iix.md) and
[iici.md](../machines/mdu/iici.md) §5; the Quadra 700 ROM path is
[q700.md](../machines/mcu/q700.md) §5. The sections below cover what is specific to
A/UX: the gate, the kernel image, the handoff, and the Unix side.

### 2.2 The Macintosh-side bootstrap (boot blocks → 'boot' resource → enabler gate)

On the "Installation Boot Disk" floppy, the recorded phases are [6] (*observed*; the
SE/30 ROM context is [se30.md](../machines/glue/se30.md#aux-301-retail-installer-boot-sequence)):

1. **Boot blocks** (HFS blocks 0–1 of the boot volume, 1 KB): signature `'LK'`, a
   branch to the entry at offset `+$8A`, the System file name "System" and startup
   application name "Finder" embedded at `+$0A`/`+$1A`. The entry tests HWCfgFlags
   (`$028E`), looks the ROM machine-type word up in a small table (`$0075`, `$037A`,
   `$067C` on this floppy), mounts the volume, opens the System file and executes
   `'boot'` resource ID 2 — control does not return.
2. **`'boot'` resource ID 2** (602 bytes): self-relocates to the stack, performs the
   early System initialization (stack and A5 world, heap limits), grows the ExpandMem
   block to 500 bytes, and *patches the eight SCSI Manager traps* `$A815`–`$A81C`
   (`_SCSIReset` … `_SCSIWr`) to RAM handlers — a Macintosh System code path replaced
   by A/UX-aware code before any extension loads [6]. It then runs the machine gate
   and finally launches the startup application named in the boot blocks (which on
   the install floppy is the A/UX installer application, despite the name "Finder").
3. **The machine gate.** System 7 enabler semantics: the System file carries a
   `'gbly'` resource (ID `$BFFF`) listing the machines it supports (23 one-word IDs,
   including `$03` for the SE/30), and each `'gbly'`-*typed* file in the System Folder
   is opened and its own `'gbly'` machine list scanned; a file loads only if the
   current machine is in its list. The scan, with the resource layout, is [6]:

```
'gbly' resource (ID $BFFF):
  +$00  6-byte header (version/flags)
  +$06  count (word) of machine IDs
  +$08  machine IDs, count × word

machine scan:
  machine := byte at $0CB3            ; Macintosh machine-type byte
  if ROM machine word ($02AE→base+8) = $0075 then machine := $FE   ; special case
  for each listed ID (word):
      if ID = machine then return match
  return no-match                    ; caller closes the file and skips it
```

4. **The consequence of a failed gate** is recorded in [§5.1](#51-the-enabler-gate-a-boot-floppy-that-excludes-machines):
   with no extension loaded, the installer's boot-device check fails and the machine
   restarts in a loop.

On an installed A/UX disk (the path the IIx, IIci and IIfx recordings exercise), the
Macintosh side boots from the disk's HFS partition in the ordinary way and the startup
application is "A/UX Startup", whose loader window reads "Welcome to A/UX. Loading…"
[6] (*observed*). The recorded IIfx trace shows the machine reach the Finder desktop
first and the startup application take over from there [6].

### 2.3 The startup application and the kernel image

The kernel the launcher loads is a single m68k COFF object, conventionally `/unix` on
the root filesystem [1] [5]. Its section layout, from the object's header [1]
(*observed*; addresses are the virtual addresses the sections are linked at):

| Section | vaddr | Size | Flags |
|---|---|---|---|
| `pstart` | `$00054000` | 31,136 | TEXT\|DATA |
| `.text` | `$10000000` | 611,336 | TEXT |
| `.data` | `$11000000` | 73,612 | DATA |
| `.bss` | `$12000000` | 287,608 | BSS |
| `MODULES` | `$00000000` | 5,120 | DATA |

The entry point (`_start`) is `$00054000`, the head of `pstart` [1]. `pstart` is a
low-memory bootstrap and trampoline section: it holds the entry code, the CPU/memory
probes, and — on the IIfx — the level-2 interrupt dispatcher `Level2Int` at
`$00058AD8` [1] [2], as well as the kernel's initial process code (`icode` at
`$0005B734`, with the literal pathname `/etc/init\0` at `$0005B75E` [1] [6]). The
main kernel body is the high `.text` at `$10000000`, with `.data`/`.bss` above it.

Recorded live kernel program counters during boot match these linked addresses
exactly, on both the SE/30 and the IIfx [1] [6] [8] [9] — the launcher loads the
image at its linked addresses, and the same kernel binary runs on both machines
(*observed*; the recorded investigation also established that the MMU control
registers TC/CRP/SRP/TT are byte-identical between the two machines at the
corresponding boot points [6]). Which component performs the load on an installed
system — the startup application itself or a helper it launches — and the exact
protocol by which it passes the machine state to `pstart` are not traced in the
corpus ([§6](#6-open-questions), item 9). On the installer path, the recorded trace
shows the kernel image loaded into `$0001F000`–`$00060000` with a display
initialization call at `$0001F008` before the kernel proper takes control [6]
(*observed*; that trace's loader conventions differ from the installed-disk flow,
and the reconciliation is not established).

### 2.4 The handoff: MMU on, VBR zero, interrupt fence

The transition from Macintosh System software to the Unix kernel is the most
timing-sensitive part of the boot, and the recorded investigation pinned its
mechanism at 100 K-cycle resolution [9]:

1. The kernel bootstrap runs with the Macintosh System environment still mapped at
   low physical RAM ("MAE" in the recorded investigations' shorthand for the
   Macintosh subsystem inside A/UX).
2. As it begins its critical setup, the kernel raises the 68030 interrupt mask to
   level 7 — an interrupt fence — while it remaps low memory under itself through
   the paged MMU.
3. After the remap, the kernel leaves the vector base register at zero: exception
   vectors are fetched through the MMU at logical address `$0`, where the kernel's
   own vector table now lives (the bus-error vector, logical `$08`, contains
   `$00054BC6`, the kernel's own handler) [6] (*observed*). Physical page zero is
   reserved by the kernel on every observed machine; the kernel keeps a zero offset
   between logical and physical kernel context.
4. When the fence drops, a Macintosh-side interrupt that arrived during the window is
   delivered *through the new mapping* — to the kernel's handlers, not to the
   Macintosh ROM's.

The recorded race that motivated this level of scrutiny is
[§5.3](#53-the-level-2-interrupt-race-at-the-mmu-handoff); the fence is what makes a
pending slot interrupt land in the kernel's drainer (`via2intr`, §3.2) instead of the
ROM's dispatcher.

### 2.5 Kernel startup: autoconfig probe and root mount

The kernel's own bring-up, in recorded order [6] [7] [8]:

1. **SCSI initialization** — `scsi_init` at `$1004AFFA` zeroes the per-target driver
   state at `$1201D40C` onward; host adapter ID is 7, maximum command retries 1000
   [1] [2].
2. **Autoconfig probe** — `scsitask` at `$1004B148` issues INQUIRY to each SCSI
   target. The completion callback `probe_complete` (`$10008204`) records a device's
   type in the kernel's disk table only when the request's return field
   (`req + $27`) is zero [1] [6]. The return codes the probe consumes are the
   driver's SST codes, produced by a table lookup at `$1100BDFC`
   (`sst_table` = `05 02 02 08 03 08 00 5D` indexed by driver state) [1] [6]:
   `$05` SST_SEL (no target answered selection), `$06` SST_TIMEOUT (watchdog fired),
   `$08` SST_MORE (data-in transfer ended short). Empty SCSI IDs produce SST_SEL;
   a healthy probe produces a zero return.
3. **Root mount** — `vfs_mountroot` (`$10032BC2`) selects the root device from the
   compiled-in `rootdev` global at `$11003544`, value `$1B06` in the installer
   kernel [1] [6]: major `$1B` is the SCSI block-device driver; the minor decodes to
   the installer's root slice on the CD (the recorded investigation's
   interpretation is target 3, slice 6 [6]; the minor-number encoding is
   [§6](#6-open-questions), item 3). Failure here is the recorded panic `no root
   file system`, emitted at `$10032C58` [1] [6].
4. **Process 1** — the kernel's `icode` builds process 1 and `exec`s `/etc/init` from
   the literal pathname in `pstart` [1] [6].

The probe loop, as pseudocode from the disassembly [1] [2] [6]:

```
for target in 0..6:                       ; host adapter is 7
    req.ret := 0
    issue INQUIRY(target, 36-byte buffer)
    wait for completion (watchdog armed)
    if selection failed:            req.ret := SST_SEL     ($05)
    if watchdog fired:             req.ret := SST_TIMEOUT  ($06)
    if data-in ended short:        req.ret := SST_MORE     ($08)
    if req.ret = 0:
        disk_table[target] := inquiry device-type byte
```

### 2.6 Process 1: /etc/init and the console

The extracted root-filesystem specimen carries a classic System V-style `/etc/inittab`
[5] (*observed*):

| ID | States | Action | Command |
|---|---|---|---|
| `sy` | — | sysinit | `/etc/sysinitrc </dev/syscon >/dev/syscon 2>&1` |
| `is` | `s` | initdefault | — |
| `bl` | — | bootwait | `/etc/bcheckrc </dev/syscon >/dev/syscon 2>&1` |
| `bc` | — | bootwait | `/etc/brc </dev/syscon >/dev/syscon 2>&1` |
| `rc` | — | wait | `/etc/rc 1>/dev/syscon 2>&1` |
| `sl` | — | wait | `rm -f /dev/syscon; ln /dev/systty /dev/syscon` |
| `pf` | — | powerfail | `/etc/powerfail 1>/dev/syscon 2>&1` |
| `co` | — | respawn | `/etc/getty console co_9600` |
| `nfs*`, `net*` | 2 | wait/once/off | `portmap`, `nfsd 4`, `biod 4`, `mount -at nfs`, `inetd` … |
| `00`, `01` | 2 | off | `/etc/getty tty0 at_9600`, `/etc/getty tty1 at_9600` |

`/etc/sysinitrc` [5] (*observed*) runs `/etc/mactime` — which sets the system clock
from the Macintosh real-time clock (its name and position are observed; its exact
protocol is not) — prints the date, offers an optional `fsck` on `/dev/dsk/c0d0s0`,
writes `/etc/mtab`, and runs `mount -f /` and `/etc/chgnod <hostname>`. `/etc/rc`
carries the UniPlus banner and mounts all filesystems of the installation's chosen
type (`mount -at 5.2` in the specimen) [5].

The console device `/dev/syscon` is the boot console; `/dev/systty` backs it. The
recorded boot reaches a graphical login on the framebuffer (a "Welcome to A/UX.
Launching…" window precedes it on the SE/30 [9]), and a root login yields a command
shell; an X11 session was driven on the IIfx recording [6] (*observed*). How the
graphical login relates to `getty`/`co_9600` — that is, whether the login window is a
Macintosh-side process of the embedded environment or a Unix-side program — is not
established by the corpus ([§6](#6-open-questions), item 12).

### 2.7 Root filesystem: file types and the partition/slice model

A/UX stores its filesystems in slices of the standard Apple partition map. The
Programmer's Reference documents the model formally [3]:

- **dpme(4)**, the disk partition map entry format, with the explicit note that the
  APM boot fields (`dpme_boot_bytes`, `dpme_load_addr`, `dpme_goto_addr`,
  `dpme_checksum`, `dpme_process_id`, `dpme_boot_args`, …) are "not used by A/UX"
  [3] dpme(4) — consistent with the boot architecture above: A/UX never boots from
  an APM boot block; the Macintosh ROM and Macintosh System software do the
  booting.
- **ptab(4)**, `/etc/ptab`, the partition table file: one line per partition with
  fields `name:type:controller:disk:slice[:comment]`, an empty type defaulting to
  `Apple_UNIX_SVR2`, and a sample showing root, swap and an `Apple_HFS` Macintosh
  partition coexisting on one disk [3] ptab(4).
- **fstab(4)**, `/etc/fstab`: `fsname dir type opts freq passno`, with type values
  "4.2, 5.2, nfs, swap, or ignore" [3] fstab(4). The specimen root's own entry is
  `/dev/dsk/c0d0s0 / 5.2 rw 1 1` [5].
- **ufs(4)** and the System V **inode(4)** formats, for the two filesystem families
  [3].

The device naming scheme (`/dev/dsk/…`) is [§3.6](#36-disk-device-naming-controllers-disks-slices).
The specimen root's filesystem is UFS-family: the recorded kernel directory code that
validates it is UFS (`ufs_dirbad`-family diagnostics) [8], and the mount observed in
the installer flow selects vfssw entry 3, which the retail kernel's table
(`$1100E970`) identifies as UFS [7] (*observed*). Which fstab label ("4.2" or "5.2")
corresponds to which vfssw entry is not established ([§6](#6-open-questions), item 10).

## 3. Hardware interface

### 3.1 One kernel, several machines: dispatch globals

The kernel selects machine-specific paths at run time through globals planted by
`pstart` [1] [2] (*observed*):

| Kernel address | Global | Observed values |
|---|---|---|
| `$0005ABBA` | machine code (Gestalt − 2, §1.4) | `$0007` SE/30, `$000B` IIfx |
| `$0005B20A` | SCSI chip base | `$50F08000` on the IIfx (the Apple 343S0064-A SCSI DMA window); the NCR 5380 base on the GLUE-family machines |
| `$00058AD8` | `Level2Int` (IIfx level-2 dispatcher, in `pstart`) | — |
| `$11004C24` | per-source level-function table (kernel `.data`) | dispatch target of both the OSS dispatcher (IIfx) and the VIA2 slot drainer (SE/30) |

Two families of machines, two interrupt architectures, one kernel: the GLUE machines
drive the NCR 5380 with CPU-paced pseudo-DMA and take slot and SCSI interrupts
through VIA2; the IIfx replaces VIA2 wholesale (there is none on the board —
[pic.md §5](../machines/iifx/pic.md)) and moves SCSI to the OSS interrupt controller
and a bus-master DMA wrapper chip. The kernel carries both paths and selects by
machine code; the recorded investigation confirmed the machine-code dispatch branches
are hardware-driver paths and that forcing the wrong one crashes the kernel [6].

### 3.2 Interrupt handling

**IIfx (OSS architecture).** The kernel hooks the CPU level-2 auto-vector with
`Level2Int` (`$00058AD8`, `pstart`). The recorded chain [2] [8] (*observed*):

```
SCSI DMA chip raises /INT → OSS asserts CPU IPL 2
  → Level2Int ($00058AD8)
      reads the OSS pending-sources word at $50F1A202
      walks the set bits, dispatching via the kernel table $11004C24
      → SCSI source: scsiirq ($1004A882)
          snapshot 53C80 status, classify the cause
          → scsisched: FSM (transition table $1100BC96, 12 bytes/state;
                          action handlers $1100BD4A, 10 bytes/state)
          → on clean end-of-process: dodmadone ($1004A1FE)
```

The OSS itself — level registers, pending word, autovector conventions — is
[pic.md](../machines/iifx/pic.md) (§2 register file, §3 request flow, §4.4 interrupt
service). Apple's IIfx developer material expected an alternative operating system to
reprogram the OSS level registers and re-point the autovectors, and recorded no other
change ([pic.md §4.6](../machines/iifx/pic.md)); the level values A/UX writes are not
decoded in the corpus ([§6](#6-open-questions), item 6).

**SE/30 and the GLUE family (VIA2 slot interrupts).** The kernel's own level-2
handler is `via2intr` (`$10011DE0`): it polls VIA2 IFR AND IER, and for the CA1
(slot-interrupt) bit dispatches through the same `$11004C24` table to `slotintr`
(`$10011E5A`) — the kernel's counterpart of Macintosh System software's
`Via2SlotInt` [1] [9] (*observed*). `slotintr` reads VIA2 port A (register 15, the
no-handshake read that does *not* clear CA1), priority-encodes the active slot bits,
invokes the per-slot handlers, and acknowledges CA1 explicitly by writing `$82` to
the IFR at `$50F03A02` — a byte write on the odd data lane [1] [9]. The VIA2 wiring
itself is [glue.md §5](../machines/glue/glue.md); the no-handshake register-15
semantics this drainer relies on are standard 6522 behaviour.

### 3.3 The SCSI stack

The driver stack, from the disassembly [1] [2] (*observed*; call chains recorded live
in the boot traces [6] [8]):

| Layer | Entry | Kernel address |
|---|---|---|
| Buffer cache | `getblk` / `bread` / `brelse` | `$100337E4` / `$10033234` / `$100334D6` |
| Exec / file loader | `getxfile`, `loadshlibs` | `$1001628C`, `$10015D9A` |
| Generic disk | `gdstart` (builds the scatter-gather source) | `$10003B64` |
| SCSI block driver | `sdread` | `$1000ABCC` |
| SCSI request layer | `scsireq` → `c80_req` | `$1000A018` |
| Task engine | `scsitask`, `scsi_dmatype`, `scsi_dma_len`, `scsi_vio` | `$1004B148`, `$10048E80`, `$10048F36`, `$10048FB0` |
| Per-arm engines | `scsi_in` (read), `scsi_out` (write) | `$1004915C`, `$1004956A` |
| Interrupt side | `scsiirq`, `scsisched`, `dodmadone`, `donotify` | `$1004A882`, —, `$1004A1FE`, — |

On the GLUE-family machines the 5380 is driven with CPU-paced, non-arbitrated
pseudo-DMA: bytes move under program control through the 53C80 data register, so a
transfer's destination is written one byte at a time by the CPU, whatever the buffer
cache allocated [6] [8]. On the IIfx the same driver instead programs the Apple
343S0064-A bus-master wrapper and lets the chip move the bytes — the fork selected by
`scsi_dmatype` (observed returning mode 2 and mode 5 in the recorded trace [8]).

The chip-level programming sequences — the 53C80 core registers, the wrapper's
`$100`/`$0C0`/`$080`/`$020`/`$050`–`$070` register file, Apple's published polled,
handshake and bus-master recipes, and the exact per-arm write series the A/UX driver
performs on the IIfx — are tabulated in
[scsi-dma.md §2](../machines/iifx/scsi-dma.md) and
[scsi-dma.md §4.5](../machines/iifx/scsi-dma.md) and are cited, not restated, here.

### 3.4 IIfx chunked bus-master DMA and the buffer allocator

The A/UX driver performs bulk disk transfers as *chunked arms*: one SCSI command per
8 KB chunk, each with its own DMA programming. The recorded instance that
characterizes the contract [2] [8] (*observed*):

- An 80 KB read issued as 10 arms: LBAs `L, L+16, L+32, …`, transfer lengths
  `80, 64, 48, …` KB remaining, i.e. `tl` decreasing by 16 blocks per arm; a 56 KB
  read issued as 7 arms the same way.
- Each arm: `scsi_in` writes the chip's DMA address register (`$100`) **once**, with
  the first chunk's destination address, and the byte count (`$0C0`) with the arm
  length (`$2000`); the driver maintains a software scatter-gather list of 10-byte
  entries {32-bit base, 32-bit length, 2 pad} hanging off the request block
  (`+ $30`), and the task structure tracks progress [2] [8]:

| `scsi_task` offset | Field | Observed use |
|---|---|---|
| `$00` | outer request block pointer | set by the request layer; `+ $30` holds the SG list |
| `$10` | base buffer address | per-arm DMA destination source |
| `$14` | total transfer size | "transfer complete" test: offset `==` total |
| `$18` | bytes advanced | incremented by `dodmadone` per arm |
| `$1C` | SG entry index | incremented when an entry is exhausted |
| `$20` | armed transfer size | residual = `$20` − chip count `$0C0` |
| `$24` | current SG entry size | "entry exhausted" test |
| `$28` | direction callback | `scsi_in` / `scsi_out` |
| `$2C` | current CDB's LBA | — |
| `$38` | flags word | bit 13 = "DMA in flight", cleared on end-of-process; bit 2 = verbose per-arm log; byte `$39` bit 6 = skip odd-byte fixup |
| `$31`/`$33` | SCSI status bytes | copied to the request block at completion |

- `dodmadone`'s accounting per arm, from the annotated disassembly [2] (*observed*):

```
bytes_transferred := task.arm_len − chip[$0C0]        ; residual count
if not task.skip_odd_fixup and chip[$30] bit 7:       ; last byte was odd
    bytes_transferred := bytes_transferred − 1
task.offset += bytes_transferred
if bytes_transferred = task.sg_entry_size:
    task.sg_index += 1                                ; entry exhausted
if bytes_reached = task.total:
    reason := ALL_DONE ($0E)                          ; whole transfer complete
else:
    reason := MORE_ARMS ($03)                         ; re-arm for the next chunk
scsisched(reason)
```

- After a clean end-of-process the handler spins up to 2,000,000 iterations waiting
  for REQ and the parity/handshake status bits to drop before reporting, and warns
  to the console log if they do not [2] (*observed*).

The unresolved property of this contract — the kernel programming one address per arm
while its buffer allocator may hand the driver *scattered* destinations — is
[§5.6](#56-the-chunked-dma-scattered-buffer-overrun) and
[scsi-dma.md §6](../machines/iifx/scsi-dma.md) (open question 3): no behaviour in the
published chip documentation distributes one `$100` write across scattered pages.

### 3.5 Serial: the SCC, the console and the IOP bypass

`/etc/inittab` respawns `/etc/getty console co_9600` and offers `tty0`/`tty1` at
`at_9600` [5] — the serial console runs over the Zilog SCC at 9600 baud with the
specimen's `gettydefs` entries. On the IIfx the SCC sits behind an I/O processor: the
host reaches the 8530 through the SCC IOP's bypass window ([iop.md, "I/O
Processors"](../machines/iifx/iop.md#io-processors-iops)). The recorded IIfx trace
shows the ROM's device-driver initialization poll the SCC through exactly that bypass
window at interrupt level 7 while the startup application runs [6] (*observed*) —
the Macintosh-side serial bring-up that must succeed before the launch completes.

### 3.6 Disk device naming: controllers, disks, slices

A/UX names disk devices `/dev/dsk/cCdDsS` (block) and `/dev/rdsk/cCdDsS` (raw) [3] [5]:
controller `C`, disk `D` on that controller, slice `S`. The specimen's
`/dev/MAKENODES` script creates the bus-0 block of that namespace [5] (*observed*):

```
MAJOR=4; UNITSHIFT=16
for UNIT in 0..6:                  ; disks 0 through 6
    for SLICE in 0..7:
        NAME  := "c0d" UNIT "s" SLICE
        MINOR := UNIT * UNITSHIFT + SLICE
        mknod /dev/rdsk/NAME  c MAJOR MINOR
        mknod /dev/dsk/NAME   b MAJOR MINOR
```

`ptab(4)` describes the controller/disk/slice triple the same way, with per-slice
types such as `Apple_HFS` for Macintosh partitions [3] ptab(4). The raw-device
convention extends the name with the partition-map entry's slice-31 device
(`/dev/rdsk/c?d?s31`, the partition map itself) [3] dpme(4) FILES.

The recorded installer investigation adds a second bus to the naming [7] (*observed,
from the installer application's disassembly and a live mount(2) trace*): the
installer application formats device names with a single-bus pattern `c%dd0s%d`
whose controller argument may be 100 or more, and 100 + n denotes *bus 1, disk n*;
the dual-bus pattern `c%d0%dd0s%d` produces the same string for bus 1. On a
single-bus machine the correct controller number is 0 — the recorded failure this
produces is [§5.5](#55-the-c100d0s0-mount-failure-of-the-retail-installer). Whether
any shipped machine has a second SCSI bus and what populates the `c1…` device nodes
is not established ([§6](#6-open-questions), item 13).

### 3.7 Real-time clock, PRAM and time-of-day

The Unix side reads the wall clock from the Macintosh: `/etc/sysinitrc` runs
`/etc/mactime` at single-user bring-up [5]. The Macintosh boot side depends on the
extended PRAM's Start Manager defaults — the default OS and boot device bytes at
XPRAM `$76`–`$89` — which the ROM's cold-boot initialization writes when the XPRAM
validity tokens are absent; the token semantics, the table and the boot-device
encoding are documented in [mac-pram.md §2](../formats/mac-pram.md) and
[mac-pram.md §4.2](../formats/mac-pram.md). The recorded IIfx boot trace shows the
Start Manager consuming exactly those bytes (default OS at `$77`, drive ID at `$78`,
partition at `$79`) when it builds its boot-device word before the SCSI driver
search [6] (*observed*). The specimen's default timezone is `TZ=PST8PDT`
(`/etc/sysinitrc`, `/etc/rc`) [5].

## 4. Installation & bring-up

### 4.1 The retail media set

The recorded installation set [4] [6] [7]:

| Medium | Content |
|---|---|
| "Installation Boot Disk" (1,440 KB HFS) | System 7-era System file (115 resource types; `'boot'` IDs 1/2/3 = 1,024 / 602 / 4,224 bytes; `'gbly'` ID `$BFFF`; 8 `'DRVR'`, 6 `'PTCH'`, 1 `'INIT'`); "System Enabler 040" as its only `'gbly'`-typed file; an installer application named "Finder" in the boot blocks |
| "Installation Boot Disk SE" (800 KB DiskCopy) | the SE-capable variant of the same bootstrap, used by the recorded SE/30 installer flow |
| Retail install CD (~407 MB, Apple partition map) | see the partition map below |
| Target HD image (169 MB, pre-partitioned) | `Apple_HFS` Macintosh partition plus an `Apple_UNIX_SVR2` slice-0 root, i.e. an already-laid-out A/UX target |

The install CD's on-media partition map [6] (*observed*):

| # | Name | Type | Start | Size (blocks) |
|---|---|---|---|---|
| 1 | Apple | `Apple_partition_map` | 1 | 63 |
| 2 | Macintosh | `Apple_Driver` | 64 | 32 |
| 3 | MacOS | `Apple_HFS` | 96 | 40,960 |
| 4 | Free UNIX slice 6 | `Apple_UNIX_SVR2` | 41,056 | 296,000 |
| 5 | Swap | `Apple_UNIX_SVR2` | 337,056 | 24,576 |
| 6 | UNIX Root&Usr slice 0 | `Apple_UNIX_SVR2` | 361,632 | 274,566 |
| 7 | Extra | `Apple_Free` | 636,198 | 196,329 |

The CD's Driver Descriptor Map is valid (signature `$4552`, 512-byte blocks, 832,527
blocks) but lists **zero** drivers, so the Macintosh ROM's SCSI boot scan loads no
driver from it; the CD is used by the running system, not the ROM [6] (*observed*).
The CD is itself a complete A/UX system: the kernel that the installer runs is
booted from its slices (§2.5), and the installer then lays a fresh A/UX onto the
target disk [6].

### 4.2 Bring-up requirements

| Requirement | Value | Evidence |
|---|---|---|
| RAM | at least 8 MB; the recorded installer flow halts at an error dialog during secondary boot with 4 MB, and runs at 16 MB | [6] (*observed*) |
| Boot device | Macintosh HFS volume for the bootstrap (floppy or HD), plus the SCSI disk to install onto | [4] [6] |
| SCSI configuration of the recorded flows | hard disk at target 0, CD-ROM at target 3, host adapter at 7 | [6] [7] |

No Apple document in the corpus states the official minimum configuration; the RAM
figure is a recorded observation, and the machine-gate behaviour of the boot floppies
(§5.1) is a second, independent constraint on which machines the *installer* runs on.

### 4.3 The installer flow (Macintosh-side application, Unix-side mount)

The recorded flow [6] [7] (*observed*):

1. The floppy bootstrap (§2.2) launches the installer application.
2. The installer loads the kernel from the CD, brings the machine through the handoff
   (§2.4), and the kernel mounts its root from the CD slice named by `rootdev`
   (§2.5).
3. The installer UI — a MacApp-framework application running in the embedded
   Macintosh environment (`AUXInstaller`) — presents the install flow ("Easy
   Install", a confirm dialog).
4. To place the target's filesystems, the application invokes the Unix side by
   *shelling out*: it builds a device path with `sprintf("c%dd0s%d", controller,
   slice)`, prepends `/dev/dsk/`, and calls
   `system("/etc/mount -t %s -o %s %s '%s' > /dev/null 2>&1")`; `/etc/mount` execs
   and issues the `mount(2)` system call (sysent entry 141, handler `$10032598`, four
   args: type word, directory, flags, data pointer whose first longword is the
   `fspec` device path) [7]. The retail kernel's mount type word 3 selects UFS
   (vfssw entry 3 at `$1100E970`) [7].
5. The recorded flow fails at exactly this step ([§5.5](#55-the-c100d0s0-mount-failure-of-the-retail-installer));
   the successful completion of the flow — the actual laying-down of the filesystem
   onto the target — is not recorded in the corpus. The pre-partitioned target
   image (§4.1) is the artefact of the layout step.

The layering is worth stating because it is unusual: a Macintosh GUI application
drives a Unix installation by constructing command lines for Unix binaries, over the
embedded environment's process bridge [7].

### 4.4 What gets installed

Not established for the retail installer (§4.3, step 5). The installed state is
observed, not through the installer but through the disk images the recorded boots
use [4] [6]: a Macintosh HFS partition carrying the Macintosh-side bootstrap
(System file, "A/UX Startup" application), and an `Apple_UNIX_SVR2` slice 0
carrying the Unix root (`/unix` kernel, `/etc` tree, `/dev/MAKENODES`, `/usr`, and
the shared libraries under `/shlib`) [5]. The specimen root's `/shlib` carries
`libc1_s`, `libc_s`, `libmac1_s`, `libmac_s`, `libuucp_s`, `libX11_s` [7] — the
shared-library set the recorded boots confirm the kernel loads and demand-pages
[6] [8].

## 5. Observed behaviour & quirks

### 5.1 The enabler gate: a boot floppy that excludes machines

The "Installation Boot Disk" carries "System Enabler 040", whose machine list is the
five 68040-class machines of §1.3 — the SE/30 (`$03`) is not in it. The gate (§2.2)
therefore loads **zero** extensions on an SE/30 [6] (*observed*): no CD-ROM driver in
the unit table, no boot-device handle where the installer expects one, no partition
record on the stack — although the SCSI Manager trap patches and the 500-byte
ExpandMem extension *are* performed, by the `'boot'` resource itself. The installer
then fails its boot-device check (§5.2) and the ROM restarts the machine: an
infinite restart loop [6]. Patching the enabler's machine list to include `$03`
makes the enabler load but does not advance the boot — the enabler's ROM patch
resources target ROM addresses that differ between the listed machines and the
SE/30, and the patched machine crashes before the installer loads [6] (*observed*).
The retail set therefore carries the separate "Installation Boot Disk SE" variant,
with which the SE/30 installer flow proceeds [6]. What that variant's own machine
list contains, and which A/UX version's bootstrap it carries, is
([§6](#6-open-questions), item 2).

### 5.2 The boot-device check and the zero-INIT restart loop

The installer's boot-device check, at `$0006E988` [6] (*observed*):

```
A0 := ExpandMem + $1EC                     ; boot-device handle installed by the
A0 := *A0                                  ; A/UX boot extension (when it loads)
if byte at A0 + $14 = 0:  jump ROM restart
else:                     continue; expect 'PREC' at $6(A7) and
                          partition type $7F at $4(A7)
```

On the zero-INIT boot of §5.1, `ExpandMem + $1EC` holds the bytes of two ordinary
System 7 ExpandMem fields read as one longword — the boot check dereferences heap
garbage, the byte test fails, and the machine restarts [6] (*observed*). The
`'PREC'`/`$7F` success-path record that a loaded extension is expected to push is
observed but not decoded ([§6](#6-open-questions), item 7).

### 5.3 The level-2 interrupt race at the MMU handoff

The recorded SE/30 investigation isolated a boot panic (`SysError dsBadSlotInt`,
the empty-slot-queue fatal error of Macintosh System software's `Via2SlotInt`) whose
occurrence depended on the real-time-clock seed of the boot [9] (*observed*). The
mechanism, resolved by cycle-resolved state capture [9]:

1. A deferred Macintosh-side task removes itself from the slot-interrupt queue
   (slot 14's queue head at `$73E0` becomes NULL).
2. The kernel raises its IM=7 fence and remaps (§2.4).
3. The next CA1 pulse from the GLUE-family interrupt controller arrives at some
   cycle in that window. If it arrives **before** the fence, the level-2 interrupt is
   delivered while the Macintosh System environment still owns low memory: the ROM
   dispatcher runs Macintosh `Via2SlotInt`, finds the slot queue empty, and panics.
   If it arrives **after**, the pending interrupt is held until the fence drops, and
   lands in the kernel's `via2intr`/`slotintr` (§3.2), which drains and acknowledges
   it safely [9].

Real hardware boots A/UX 3.0.1 reliably across wall-clock settings [9]; whether real
GLUE-family CA1 timing makes the "before the fence" ordering unreachable, or some
other synchronisation step exists on real silicon, is open
([§6](#6-open-questions), item 8). The kernel-side facts the race exposed — the
explicit IFR acknowledge at `$10011E90`, the no-handshake port A read, the
IM=7 fence around the remap — are stated in §3.2 and §2.4 and stand independently of
the race.

### 5.4 Dependence on 68030 bus-error restart semantics

The kernel runs demand-paged: pages are mapped lazily by its bus-error handler, and
the handler's `RTE` must **restart the faulting instruction** — the 68030's
format-`$B` exception frame restart semantics — for the mapping to take effect on
the access that faulted [6] (*observed*; the recorded investigation documents the
failure mode when a host executes the exception with the faulting instruction skipped
instead of restarted: the kernel consumes default-read garbage from the faulting
address, and the corruption propagates until a later fault pushes an exception frame
through an unmapped stack pointer and halts the machine [6]). Two further kernel
properties recorded on the same investigation: the kernel's own software page-table
walker (`$551E4`) depends on the 68030 `PTEST` instruction's address-output register
and function-code operand being honoured exactly [6]; and the physical-address
translation helper `realvtop` masks the `MMUSR` with `$E400` and expects the MC68030
bit layout (W bit 11, I bit 10, M bit 4) [6].

### 5.5 The `c100d0s0` mount failure of the retail installer

The recorded installer flow fails, deterministically, with the dialog
`Cannot mount "/dev/dsk/c100d0s0" - Invalid argument.` [7] (*observed*). The full
validated chain [7]: the `mount(2)` returns EINVAL with `fspec = /dev/dsk/c100d0s0`
and directory `/mnt`; the path was built by the installer application with
controller number **100** instead of 0 (the bus-1 encoding of §3.6 applied on a
single-bus machine); and the application's only easily-observable bus-related
function (`check_two_bus`, which the recorded investigation fully disassembles)
affects only a dialog *string*, not the mount path — patching it changes nothing
[7]. Every literal occurrence of the path on the install CD is in non-code data
(node tables, manifests, help text), so the path is runtime-formatted; **where the
controller number 100 comes from is not identified** [7]. The recorded
investigations route around the failure; it remains unexplained
([§6](#6-open-questions), item 4).

### 5.6 The chunked-DMA scattered-buffer overrun

On the IIfx, a recorded boot (after all earlier handoff and dispatch faults were
resolved) printed five kernel diagnostics of the form
`/: bad dir ino 15360 at offset N: mangled entry` — inode 15360 is `/dev` — when
the kernel later read a buffer-cache page that a chunked DMA read had overrun [8]
(*observed*). The mechanism, established by a buffer-allocator trace [8]:

1. The kernel allocates 7 buffer-cache heads (8 KB each) for a 56 KB read; on this
   boot the allocator returned **scattered** physical pages.
2. The driver issues 7 chunked arms (§3.4), programming the DMA address register
   once with the *first* head's address each arm and relying on the address counter
   advancing across arms.
3. An advancing counter writes one contiguous 56 KB region; on this boot it covered
   the page holding `/dev`'s cached directory blocks — a page belonging to a
   different, live buffer head — and the later directory validation rejected the
   entries [8].
4. The same kernel binary, same disk, on the SE/30 — where the 5380 is driven by
   CPU-paced pseudo-DMA and no bus-master counter exists — fills every scattered
   head correctly and boots past the point [8].

No behaviour in the published chip documentation can fill scattered destinations
from a single per-arm address write ([scsi-dma.md §6](../machines/iifx/scsi-dma.md),
open question 3): either the real chip holds an undocumented address behaviour, or
the kernel contains a staging path not yet identified, or 3.0.1's IIfx chunked path
is genuinely fragile when the allocator scatters. Which is true on real hardware is
open ([§6](#6-open-questions), item 5). The recorded investigation also proved, on
the same trace, what the failure is *not*: not a driver that reprograms the address
per arm (it does not), not a multi-entry hardware scatter-gather (the SG index stays
0 every arm), and not a copy in the interrupt handler (neither `scsiirq` nor
`dodmadone` contains a block copy) [8].

### 5.7 Small observed behaviours

- The specimen root greets logins with a customized `/etc/motd` ("Welcome to Cayman
  …") [5]; the startup scripts carry the internal project name "Cayman" in their
  headers (`sysinitrc`: "Cayman version 7/87") [5] (*observed*; that "Cayman" is the
  A/UX project's codename is *inferred from the headers*).
- `/etc/sysinitrc` offers an optional `fsck -DB -q /dev/dsk/c0d0s0` before mounting
  root, with a default of *no* [5].
- The installer application names its startup application "Finder" in the boot blocks
  even though the file is the installer [6] — a boot-block convention, not a
  filesystem one.
- The kernel logs its SCSI driver's transfers verbosely when the per-task verbose
  flag is set (bit 2 of the flags word, §3.4), hex-dumping the first up-to-8 bytes of
  each arm's buffer to the console [2] (*observed* in the disassembly).
- The 53C80 parity/handshake status is polled for up to 2,000,000 iterations after a
  clean end-of-process, with a console warning on timeout (§3.4) [2].

## 6. Open questions

1. **The official supported-machine matrix of 3.0.1.** No Apple document in the
   corpus states it. The installer floppy names five 68040-class machines; recorded
   boots prove SE/30, IIx, IIci and IIfx run the OS from installed disks. Whether
   other machines (IIcx, Quadra 950 variants, later Centris models) are supported,
   and how an IIfx — absent from the installer floppy's list — was meant to be
   installed, is unknown.
2. **The "Installation Boot Disk SE" variant.** Its own machine list and the version
   of the bootstrap it carries are not examined; why a 3.0.1 retail set contains an
   SE-capable floppy when its generic floppy's enabler excludes the SE/30 is
   unexplained by the corpus.
3. **The minor-number encoding.** The specimen `MAKENODES` encodes minor as
   `unit × 16 + slice`; the installer kernel's `rootdev = $1B06` was interpreted in
   the recorded investigation as target 3, slice 6. The two are not reconciled; the
   retail kernel's device-number encoding is not established.
4. **The `c100d0s0` failure.** Which component supplies controller number 100 to
   the installer's path builder on a single-bus machine is unidentified (§5.5).
5. **The chunked-DMA scattered-destination contract** (§5.6, mirrored by
   [scsi-dma.md §6](../machines/iifx/scsi-dma.md)): undocumented chip behaviour,
   an unidentified kernel staging path, or real-hardware fragility — no recorded
   evidence distinguishes them.
6. **A/UX's OSS programming on the IIfx.** The kernel re-points the level-2
   auto-vector and reads the pending word; the level-register values it assigns to
   each source are not decoded (cf. [pic.md §6](../machines/iifx/pic.md), open
   questions 1–2).
7. **The `'PREC'` record and partition type `$7F`.** The boot-device check's
   success-path expectations are observed as values only; their structure and
   producer (which extension builds the record) are not established.
8. **The handoff race on real hardware** (§5.3): is the "interrupt before the fence"
   ordering unreachable on real GLUE-family timing, or is there a synchronisation
   step the recorded investigation did not model?
9. **The installed-disk loader.** Which component loads `/unix` on an installed
   system, at which point the Macintosh environment is demoted, and how the boot
   device and kernel arguments are passed, is not traced.
10. **Filesystem type labels vs. kernel indices.** Whether fstab's "4.2" and "5.2"
    labels denote UFS and SVFS respectively on 3.0.1, and how they map to the
    retail kernel's vfssw entries, is not established (§2.7).
11. **The boot-block machine table.** The class table in the boot blocks (`$0075`,
    `$037A`, `$067C`) and the `$0075 → $FE` special case in the `'gbly'` scan are
    observed as values; which machines the words name is not decoded.
12. **The graphical login's nature** — a Unix program or an embedded-Macintosh
    process, and its relationship to the respawning `getty` entries (§2.6).
13. **The second SCSI bus of the naming scheme** (§3.6): which shipped machine has
    one, and which utility creates the bus-1 device nodes the specimen's
    `MAKENODES` does not.
14. **Version reach beyond 3.0.1.** Whether the 3.x series ran on any 68040 machine
    not named by the enabler, and whether any 3.x build exists for other CPU
    families, is outside the corpus.

## References

1. A/UX 3.0.1 kernel image: exploded COFF dump of the shipped `/unix` — section
   headers (`pstart`/`.text`/`.data`/`.bss`/`MODULES`), symbol table (~4,965 defined
   symbols), per-section annotated 68K disassembly, build stamp 1993-04-23.
2. A/UX 3.0.1 SCSI driver: hand-annotated disassembly extract of the complete driver
   (upper-layer entries, request layer, `scsi_vio`/`scsi_in`/`scsi_out`, the
   interrupt-reachable core `scsiirq`/`scsisched`/`dodmadone`/`donotify`, the OSS
   level-2 dispatcher `Level2Int`).
3. Apple Computer, Inc. and UniSoft Corporation, *A/UX Programmer's Reference*,
   Apple part 030-0785, 1990 — Sections 3 (Subroutines, M–Z), 4 (File Formats) and 5;
   cited entries: `fstab(4)`, `ptab(4)`, `dpme(4)`, `ufs(4)`, `inode(4)`,
   `mount(3)`, Introduction.
4. A/UX 3.0.1 retail installation media set: "Installation Boot Disk" and
   "Installation Boot Disk SE" HFS floppies, the ~407 MB retail install CD-ROM
   (Apple partition map with `Apple_HFS` and `Apple_UNIX_SVR2` slices), and the
   pre-partitioned 169 MB target hard disk image — on-media structures and recorded
   boot behaviour.
5. A/UX root-filesystem specimen, extracted from installed A/UX 3.0.1 media:
   `/etc/inittab`, `/etc/sysinitrc`, `/etc/rc`, `/etc/fstab`, `/dev/MAKENODES`,
   `/etc/motd`, `/shlib`.
6. Recorded boot-phase maps and root-cause analyses of the A/UX 3.0.1 boot, 2026 —
   with confirmed ROM, boot-block, `'boot'`-resource, enabler, installer and kernel
   addresses; the SE/30 installer investigation, the retail-floppy enabler
   root cause, the kernel MMU/PTEST analysis, and the installed-disk boot
   observations on SE/30, IIx, IIci and IIfx.
7. Recorded investigation of the A/UX 3.0.1 retail installer's mount failure, 2026 —
   disassembly of the AUXInstaller MacApp application (`TSCSI` methods,
   `check_two_bus`), live `mount(2)` trap trace, and the CD's extracted Unix root.
8. Recorded analysis of the IIfx chunked-SCSI-DMA behaviour of the A/UX 3.0.1
   kernel, 2026 — per-arm chip register trace, buffer-allocator trace (`getblk`
   exit), the scattered-buffer-head case and the SE/30 differential.
9. Recorded cycle-resolved analysis of the level-2 interrupt / MMU-handoff race in
   the A/UX 3.0.1 boot on the SE/30, 2026 — the `via2intr`/`slotintr` dispatch,
   the IM=7 fence, and the RTC-seeded reproducibility sweep.
