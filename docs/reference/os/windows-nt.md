# Windows NT 4.0 for PowerPC on the Apple Network Server

**Contents:**

1. [Scope & identity](#1-scope--identity) — what Windows NT for PowerPC is, the HAL architecture and the
   ARC firmware seam, the evidenced media, the supported-hardware matrix, machine identification
   across the seam
2. [Boot architecture](#2-boot-architecture) — the two-layer design, Open Firmware under and ARC over;
   the 2.26NT ROM's little-endian contract; loading the veneer; the bootpath contract and the ARC
   name; SETUPLDR's device probe and Apple's `disk-label`; the HAL gate where the boot stops
3. [Hardware interface](#3-hardware-interface) — the storage path under ARC reads; endianness through
   the stack; the console; what an Apple Network Server HAL would face
4. [Installation & bring-up](#4-installation--bring-up) — the recorded configuration; the bring-up
   recipe in order; bring-up on real hardware
5. [Observed behaviour & quirks](#5-observed-behaviour--quirks)
6. [Open questions](#6-open-questions)

References

---

## 1. Scope & identity

### 1.1 What Windows NT for PowerPC is

Windows NT 4.0 for PowerPC is Microsoft's portable-NT operating system built for PowerPC machines.
Two structural properties define the port, both read off the shipped binaries themselves [1]
(*observed*):

- **The kernel is hardware-agnostic by construction.** The kernel image (`\PPC\NTKRNLMP.EXE` on
  the OEM CD) "imports everything hardware-related from `HAL.DLL`", and the loader binds those
  imports **only after a HAL (hardware abstraction layer) has been chosen** — the machine-specific
  half of the system is a separately selected DLL, not part of the kernel image.
- **The boot stack consumes ARC, not Open Firmware.** The NT loader chain is built against ARC —
  the boot-naming and boot-service conventions of the machine class Windows NT for PowerPC shipped
  for — and machines whose native firmware is Open Firmware reach it through an interposed
  translation layer: `VENEER.EXE`, the FirmWorks/Microsoft Open Firmware-to-ARC veneer shipped in
  the CD's `\PPC\` directory, which derives ARC device names and ARC firmware services from the
  Open Firmware client interface.

This page documents the port's one recorded contact with Apple hardware: the Apple Network Server
500/700 ("Shiner"), the Power Macintosh 9500-derived AIX server that carries an NT-capable
prototype ROM (the "2.26NT" Open Firmware image, §1.3) — and on whose hardware Windows NT 4.0
Setup is recorded reaching its computer-type (HAL) menu [1] [2]. The contrast with the machine's
other guests is the point of the page set: Apple's A/UX rides the Macintosh ROM's own boot path
under a Macintosh-side startup application ([A/UX](aux.md) §2.1); MkLinux is loaded by a Mac OS
control panel from a running Finder ([MkLinux](mklinux.md) §2.1); SCO XENIX on the Lisa rides the
Lisa's boot blocks ([XENIX](xenix-lisa.md) §2.1); the Network Server's own AIX is booted by its
firmware directly ([ans.md §2.7](../machines/ans/ans.md)). Windows NT does none of these: on the
recorded path it is entered from the Open Firmware command line, by hand-loading the veneer.

The evidence base is thin, and the page says where. **No Microsoft document is in the evidence
corpus** — no ARC specification, no NT hardware or HAL documentation, no DDK — and no Apple
document describes the NT-capable ROM beyond one marketing sentence (§1.3). Everything below is
built from three classes of primary evidence: the shipped binaries themselves, read through their
own embedded COFF symbol tables (`VENEER.EXE` and `\PPC\SETUPLDR` ship as bare COFF images with
their symbol tables intact, which name the veneer's sources `vr*.c` and the Setup loader's
`setup.c`) [1] [5] [6]; the recorded bring-up of 2026-09-05 [1]; and the public real-hardware
Open Firmware transcript that the recorded bring-up replays [2]. Claims are marked *observed*
(with the evidence class) or *inferred — unverified*; what the corpus does not establish is
collected in [§6](#6-open-questions). The page is deliberately leaner than its subject deserves.

### 1.2 Version, media and dating

| Item | Value | Evidence |
|---|---|---|
| Evidenced version | Windows NT 4.0, OEM CD-ROM, PowerPC edition | [1] [2] |
| Setup loader | `\PPC\SETUPLDR`, a relocatable image of six sections | [1] |
| Kernel image | `\PPC\NTKRNLMP.EXE`, 666 blocks on the CD as read | [1] |
| Setup information file | `TXTSETUP.SIF`, 126 KB, at the CD root | [1] |
| Veneer | `\PPC\VENEER.EXE`, image base `$50000` | [1] [5] |
| Veneer's default OS-loader path | `\os\winnt\osloader.exe` (patched out in the recorded session, §2.3) | [1] |
| Object format | bare COFF images, COFF symbol tables intact (veneer and SETUPLDR) | [1] [5] [6] |
| Firmware required | the Apple Network Server "2.26NT" prototype Open Firmware ROM | [1] [2] [3] |

The kernel image is read from the CD in full before the boot stops (§2.6), so the file-level facts
above are all *observed*. The version history of the PowerPC port — which NT releases were built
for PowerPC, when 4.0 shipped, and whether later builds exist — is not in the corpus
([§6](#6-open-questions), item 15).

### 1.3 The supported-hardware matrix

The OEM CD states its own machine support, indirectly but precisely: when Setup "could not
determine the type of computer you have", it presents a computer-type menu whose entries name the
machines the product carries HALs for [1] (*observed*):

| Menu entry | Platform named |
|---|---|
| IBM Power Series 6015 / 6020,40,42 / 6050,6070 and RS/6000 Model 7248 | IBM Power Series / RS/6000 |
| IBM RS/6000 Model E20/E30/F30 / 45M/H45 | IBM RS/6000 |
| MOTOROLA PowerStack / PowerStack2 / Big Bend | Motorola PowerStack |
| Powerized ES, MX, LX, TX (Uniprocessor) / (Multiprocessor) | Powerized |
| Other | — (manual specification) |

No Apple machine is on the list. Windows NT 4.0 for PowerPC is therefore **not a shipped product
for the Apple Network Server**, and the ANS is not a supported platform of the CD's contents; the
"Other" entry exists for machines whose HAL is supplied separately, and none for an Apple machine
is in evidence anywhere in the corpus ([§6](#6-open-questions), item 3). The NT-on-ANS story is a
prototype-ROM story, and its context is Apple's own: the Network Server's target-competition
analysis names "Sun Microsystems, SGI, and to some extent Windows NT encroachments" [9] §1.2.3,
and a Windows-NT-capable ROM image is among the Network Server ROM images preserved from Apple's
server group [2] [3] (*observed* as a live image; that NT is what the "NT" in its name denotes,
and that NT ran on it inside Apple, is *reported* in the community record and not documented by
Apple — [§6](#6-open-questions), items 1 and 2).

What the matrix bounds: the boot-path components evidenced on the ANS are the veneer and SETUPLDR,
running under the 2.26NT ROM on an ANS 500 in the recorded bring-up [1] (*observed*), and the same
binaries on a real ANS 700 in the public transcript [2] (*observed*). Nothing beyond Setup's menu
loop — no HAL, no kernel start, no installer — is evidenced on any machine.

### 1.4 Machine identification across the firmware seam

Three naming conventions meet in this boot, and keeping them apart is the recorded investigation's
first discipline [1]:

| Convention | Example for the recorded boot | Owner |
|---|---|---|
| Open Firmware device path | `/bandit/53c825@11/sd@0,0` | the IEEE 1275 firmware and the ANS device tree ([ans.md §4.2](../machines/ans/ans.md); [7]) |
| ARC name | `multi(0)scsi(0)cdrom(0)fdisk(0)` | the ARC conventions; derived by the veneer from the above [1] [5] |
| Apple machine identity | `AAPL,ShinerESB` | the production ROM's root `compatible` ([ans.md §1.3](../machines/ans/ans.md)) |

The mapping from the first to the second is the veneer's `find_boot_dev`, documented with its
failure mode in [§2.4](#24-the-bootpath-contract-and-the-arc-name). NT itself never reaches a
machine query: Setup stops at the HAL menu (§2.6) before anything distinguishes an Apple machine,
and what machine identity the 2.26NT ROM presents is not recorded in the corpus
([§6](#6-open-questions), item 1).

## 2. Boot architecture

### 2.1 Two layers: Open Firmware under, ARC over

The Network Server's firmware is Open Firmware — Apple states the conformance directly: "The
Open Firmware startup process conforms to IEEE Standard 1275 and the PCI Bus Binding to IEEE
1275-1994 specification" [10] Ch. 6 — and Windows NT's loader stack expects ARC. The shipped
product's answer is the veneer: a PE-format firmware-translator image that, once running, presents
the ARC environment NT's loaders are built against, on top of the Open Firmware client interface
[1] [5]. The full recorded chain [1] [2]:

```
power-on → POST (the LCD contract, [ans.md §2.7](../machines/ans/ans.md))
  → Open Firmware, the 2.26NT prototype ROM
  → client state configured little-endian (§2.2)
  → VENEER.EXE read from disk by hand, laid out by the ROM's pe-loader,
       four patches applied, `go` (§2.3)
  → veneer: ARC firmware services over the Open Firmware client interface
       → bootpath → the ARC boot name (§2.4)
       → SETUPLDR: six sections relocated, jump (§2.5)
  → SETUPLDR: CDFS mounted over raw ARC reads, TXTSETUP.SIF parsed,
       \PPC\NTKRNLMP.EXE read in full (§2.5)
  → computer-type (HAL) menu, polling the ARC console — where the boot stops (§2.6)
  → (not reached on any recorded boot) HAL choice → HAL.DLL import binding → NT kernel
```

Every stage down to the menu is *observed* in the recorded bring-up [1]; the final stage's
mechanics (HAL choice and import binding) are read off the loader's behaviour, not witnessed
past the choice [1] [6]. AIX, for contrast, needs none of this: the production ROM boots it
itself, through `bootapple` and the compressed `bosboot` image
([ans.md §2.7](../machines/ans/ans.md), [ans500.md §5.4](../machines/ans/ans500.md)). Whether the
2.26NT ROM carries an automatic NT boot path — a boot-device integration that would load the
veneer without the command line — is not established ([§6](#6-open-questions), item 5); only the
hand-loaded path is evidenced.

### 2.2 The 2.26NT ROM and the little-endian contract

Windows NT's PowerPC loader stack runs little-endian, and the NT-capable prototype ROM provides
for it. Its configuration variables, with the recorded session's values [1] [3] (*observed*):

| Configuration variable | Recorded value | Effect |
|---|---|---|
| `little-endian?` | `true` | the client program is started in the processor's little-endian mode |
| `real-mode?` | `false` | the client gets virtual (MMU-translated) mode |
| `real-base` | `$3F00000` | the client program's load region |
| `load-base` | `$3E00000` | the firmware's staging address for `read-blocks` loads |

The little-endian mode is the processor's own, not a firmware invention: the PowerPC 604's MSR bit
31 — "LE: Little-endian mode enable. 0 The processor runs in big-endian mode. 1 The processor runs
in little-endian mode." [8] Table 4-3 — with the 604's documented little-endian restrictions
(alignment exceptions for accesses not naturally aligned in little-endian mode, and for `lmw`,
`stmw`, `lswl`, `lswx`, `stswl`, `stswx` issued in little-endian mode) [8] Table 1-2. Two recorded
observations fix how the ROM uses it [1]: the firmware's own command interpreter runs little-endian
once the client state is configured, so the session's byte patches are applied with `c!` at
*program* addresses directly; and the whole NT stack — veneer, the firmware's client interface
under it, SETUPLDR's CDFS and INF parser, and the kernel image load — runs in that mode without a
fault.

The platform's endianness seam sits below this: the Network Server, like the whole TNT platform,
is big-endian on the processor bus and little-endian on PCI, with Bandit performing the
translation — "All register accesses will be default swapped by Bandit" [9] §2.2; the family-level
statement is [tnt.md](../machines/tnt/tnt.md) §2.5 and the conversion rules are
[bandit.md §3.5](../machines/tnt/bandit.md). A little-endian client moves the seam onto the
processor itself; the one peripheral that already matches the client's byte order is the on-board
Cirrus 54M30 frame buffer, "only a little-endian window into the packed-pixel frame buffer"
[9] §2.8 ([ans.md §2.5](../machines/ans/ans.md)).

### 2.3 Loading the veneer: `read-blocks`, `pe-loader`, four patches

The recorded load replays the public transcript's recipe [1] [2]:

1. **Read the image.** `read-blocks` — the deblocker's low-level block interface [7] §3.8.3 — pulls
   `VENEER.EXE`'s raw blocks from its carrier disk into the staging address.
2. **Lay it out.** The ROM's `pe-loader` package `init-program` claims the image's memory and
   initializes the saved program state, exactly the `boot`-time sequence IEEE 1275 defines for a
   valid client image [7] §7.4.3 — performed here by hand because the image is not being booted by
   the firmware's `boot` (§2.4). The `pe-loader` package is a property of the NT-capable ROM; the
   production ROM's loader set is XCOFF-oriented (`xcoff-loader`, plus `aix-boot` and
   `iso-9660-files`) [ans.md §1.3](../machines/ans/ans.md) (*observed*; the 2.26NT image's own
   package list is not enumerated in the corpus, [§6](#6-open-questions), item 1).
3. **Patch it.** Four patches, image offsets, as recorded [1] [2]:

| Patch (image offset) | Change | Effect |
|---|---|---|
| `$51E3C`, `$514E0` | `nop` two `claim` client-interface calls that fail | the veneer's memory claims no longer abort against this firmware |
| `$5CD30` | replace the default `\os\winnt\osloader.exe` with `\PPC\SETUPLDR` | the veneer's OS-loader path points at the Setup loader on the CD |
| `$53DB0` | (from the public transcript) | purpose not established ([§6](#6-open-questions), item 4) |

4. **`go`.**

Why the two `claim` calls fail on this firmware is not established [1] ([§6](#6-open-questions),
item 4); the patches are recorded as necessary and sufficient.

### 2.4 The bootpath contract and the ARC name

The `bootpath` contract is the standard's: IEEE 1275's `/chosen` node carries "bootpath — The
device path for the last boot device. Client programs may use this property to locate the device
they were booted from" [7] §3.5, and the `boot` loading process saves the device path and
arguments "so they may be retrieved later via the client interface" [7] §7.4.3. The 2.26NT ROM
honours exactly that — **only its `load`/`boot` (in `$load`) set `bootpath`**; a client loaded by
hand with `read-blocks` runs with the property unset [1] [3] (*observed*).

The veneer's `find_boot_dev` reads `/chosen` `bootpath` and derives the ARC boot name from it
[1] [5]. With no property set, the firmware's `finddevice("")` returns the root node, whose ARC
name is the Open Firmware root's own `name` — `device-tree` — and the veneer's `VrOpen` then cannot
find a `device-tree(0)` component under its ARC root. The result is the first recorded wall [1]
(*observed*):

```
Booting from 'device-tree(0)partition(1)\PPC\SETUPLDR'
VrOpen returned d
```

(`d` is the ARC error value `ENODEV`, printed as a single character [1].) The fix is to install the
property by hand before `go` [1] [2] (*observed*):

```
" /bandit/53c825@11/sd@0,0" encode-string " bootpath" _chosen (property)
```

with the recorded caveat that the external `dev /chosen … property` form did **not** take — only
the internal `(property)` with the `_chosen` phandle installed the property (verified with
`dev /chosen .properties`) [1].

The ARC name grammar, as the Setup loader parses it (`BlGenerateDeviceNames`, reached through its
error path `SlFriendlyError`, `setup.c` line 416) [1] [6] (*observed*):

| Token | Rule |
|---|---|
| `multi(n)`, `scsi(n)` | adapter tokens, first |
| `disk(n)` | must be followed by `rdisk(n)`/`fdisk(n)` and then `partition(n)` |
| `cdrom(n)` | must be followed by `fdisk(n)` and then **the end of the name** |
| any further token | EINVAL — rendered by `SlFriendlyError` as "The file %s is corrupted" |

The veneer appends `partition(1)` to every boot path it derives [1] [5] — right for a hard disk,
wrong for a CD-ROM, which must end after its `fdisk` token. The recorded boot device is the CD on
the first SCSI controller, so the derived name is illegal, and the Setup loader renders the EINVAL
as the second recorded wall [1] [2] (*observed*, identically on the real machine):

```
The file multi(0)scsi(0)cdrom(0)fdisk(0)partition(1) is corrupted.
Press any key to continue.
```

How `find_boot_dev` classifies the boot device as `cdrom` rather than `disk` (device type from the
firmware, or filesystem probe) is not established ([§6](#6-open-questions), item 6). The recorded
name, stage by stage [1]:

| Stage | Name |
|---|---|
| Open Firmware `bootpath` | `/bandit/53c825@11/sd@0,0` |
| veneer derivation, `partition(1)` appended | `multi(0)scsi(0)cdrom(0)fdisk(0)partition(1)` |
| after the `$5D0C0` patch (§2.5) | `multi(0)scsi(0)cdrom(0)fdisk(0)` |
| the file opened on it | `multi(0)scsi(0)cdrom(0)fdisk(0)\PPC\SETUPLDR` |

### 2.5 SETUPLDR's device probe and Apple's `disk-label`

Behind the naming wall sits a second, storage wall. SETUPLDR opens the boot device itself: when a
path has no partition token and no file, `VrOpen` appends `:0` to the Open Firmware device
argument [1] [5] (*observed*). It then probes the opened device with its own FAT/NTFS/CDFS
recognizers [1] (*observed*; purposes of the individual reads are *inferred*):

| Read | Size (bytes) | Offset | Recognizer target |
|---|---|---|---|
| 1 | `0x62` | 0 | FAT boot sector / BPB (*inferred*) |
| 2 | 512 | `0x2000` | second boot-sector probe (*inferred*) |
| 3 | 528 | 0 | extended BPB (*inferred*) |
| 4 | 2048 | `0x8000` | ISO 9660 volume descriptor, sector 16 (*inferred*) |

What those reads see is decided by Apple's `disk-label`, and here the firmware's behaviour is the
wall [1] (*observed*):

- `disk-label`'s `open`, given **any non-empty argument** (`0`, `1`, …) with no file name, detects
  the `CD001` signature and interposes `iso-9660-files`; that package's `open` with no path leaves
  the instance on the ISO **root directory** as a pseudo-file — 428 bytes on this CD — whose `seek`
  does not touch `fileposn` and whose `read` clamps to `filesize`.
- The four recognizer reads therefore return 98, 330, 0 and 0 bytes; the volume descriptor at
  sector 16 is never seen, and no recognizer identifies the medium.
- Only an **empty** argument makes `disk-label` return the raw device, whose `read`/`seek` go
  through the deblocker and work at any byte offset.

The standard defines the `disk-label` and `deblocker` packages' methods but not this argument
convention [7] §3.8.1, §3.8.3 — the partition-versus-raw semantics are Apple's firmware's own, and
they defeat exactly the raw-probe pattern an ARC loader performs.

Both walls — the `partition(1)` append and the `:0` append — are properties of the shipped veneer;
the disk-label argument semantics are Apple's. The recorded session clears all three with **two
one-byte patches** in `VENEER.EXE` [1] (*observed*; image base `$50000`, file offset = image −
`$50000` + `$200`):

| image | file | `.rdata` string | patch | effect |
|---|---|---|---|---|
| `$5D0C0` | `$D2C0` | `"partition(1)"` | first byte → `00` | the derived boot name loses the illegal token (§2.4) |
| `$5E168` | `$E368` | `":0"` | first byte → `00` | the bare-device open passes an empty Open Firmware argument → raw sector access |

Applied at the prompt as `00 5D0C0 c!` and `00 5E168 c!` (the firmware runs little-endian, §2.2,
so `c!` takes the program address directly) [1]. With raw sector access, SETUPLDR mounts the CD
with its own CDFS over raw ARC reads, relocates its six sections, parses the 126 KB
`TXTSETUP.SIF`, and reads `\PPC\NTKRNLMP.EXE` in full — 84 SCSI READ commands covering the
kernel's 666 blocks [1] (*observed*).

### 2.6 The HAL gate: where the boot stops

The recorded output past the second wall [1] (*observed*):

```
Booting from 'multi(0)scsi(0)cdrom(0)fdisk(0)\PPC\SETUPLDR'
 Windows NT Setup
 Setup is loading files (Windows NT Executive)...
 Setup could not determine the type of computer you have, or you have
 chosen to manually specify the computer type.
   IBM Power Series 6015 / 6020,40,42 / 6050,6070 and RS/6000 Model 7248
   IBM RS/6000 Model E20/E30/F30 / 45M/H45
   MOTOROLA PowerStack / PowerStack2 / Big Bend
   Powerized ES, MX, LX, TX (Uniprocessor) / (Multiprocessor)
   Other
```

![Windows NT Setup's computer-type menu, rendered from the recorded console
stream](../../assets/ans-nt-setup-hal-menu.png)

The kernel is in memory but **nothing of NT executes**: the kernel imports everything
hardware-related from `HAL.DLL`, the loader binds those imports only after a HAL is chosen, and no
HAL on the CD is for an Apple machine [1] [6] (*observed*). Control sits in SETUPLDR's menu loop,
polling the ARC console for a key [1]. That is the end of the evidenced path — and it is the point
the recorded investigation aimed at: the menu is the list of HALs a real Apple Network Server HAL
would have to be measured against ([§3.4](#34-what-an-apple-network-server-hal-would-face)).

## 3. Hardware interface

### 3.1 The storage path under ARC reads

Every disk byte the NT loader stack moves in the evidenced boot passes through the firmware; the
NT-side code never touches the SCSI chip. The layers [1] [7]:

| Layer | Component | Evidence |
|---|---|---|
| NT Setup filesystem | SETUPLDR's own CDFS, over raw sector reads | [1] [6] |
| ARC device layer | the veneer's ARC services over the Open Firmware client interface | [1] [5] |
| Open Firmware filesystem | `disk-label` on the raw device (empty argument) + `deblocker` | [1]; [7] §3.8.1, §3.8.3 |
| Open Firmware block device | `/bandit/53c825@11/sd@0,0` — the first 53C825A, target 0 | [1]; [ans.md §3.3](../machines/ans/ans.md) |
| Hardware | the two Symbios 53C825A Fast/Wide SCSI-2 PCI devices, IDSEL 17/18 on Bandit 1 | [ans.md §3.3](../machines/ans/ans.md); [sym53c8xx.md §1](../hardware/scsi/sym53c8xx.md) |

The chip-level programming the firmware performs — the 53C8xx register file, the SCRIPTS engine,
the chip's endianness pin and dual register offsets — is documented once on the chip page and
cited, not restated, here: [sym53c8xx.md §1.6](../hardware/scsi/sym53c8xx.md) (endianness),
[sym53c8xx.md §2](../hardware/scsi/sym53c8xx.md) (registers), and
[sym53c8xx.md §4](../hardware/scsi/sym53c8xx.md) (how the Network Server's firmware configures
and drives the part). The recorded session's 84 SCSI READ commands are the raw material for
verifying that path end-to-end [1].

The asymmetry matters for what comes next: an ARC loader can live entirely on firmware services,
but **an NT HAL cannot** — the HAL is the layer that would drive the machine's hardware itself, and
the Network Server's only fast disk path is the 53C825A pair ([ans.md §2.8](../machines/ans/ans.md)).
No NT driver for any member of the 53C8xx family is in evidence anywhere in the corpus
([§6](#6-open-questions), item 11).

### 3.2 Endianness through the stack

The endianness layout of the recorded boot, layer by layer:

| Layer | Byte order | Evidence |
|---|---|---|
| NT loader stack (veneer, SETUPLDR, kernel image) | little-endian, MSR[LE] = 1 | [1] (*observed*); [8] Table 4-3 |
| Firmware command interpreter, after client configuration | little-endian (byte patches at program addresses) | [1] [3] (*observed*) |
| Processor bus / RAM / ROM | big-endian (the platform's rule) | [tnt.md](../machines/tnt/tnt.md) §2.5; [9] §2.2 |
| PCI, behind Bandit | little-endian, register accesses "default swapped by Bandit" | [9] §2.2; [bandit.md §3.5](../machines/tnt/bandit.md) |
| 53C825A register file | endian-selected by its BIG_LIT/ pin, dual register offsets | [sym53c8xx.md §1.6](../hardware/scsi/sym53c8xx.md) |
| 54M30 frame buffer | little-endian window ("Big Endian operating systems are limited to 8 bits per pixel") | [9] §2.8; [ans.md §2.5](../machines/ans/ans.md) |

That the whole little-endian client stack runs the firmware's client interface, the CDFS and the
INF parser without a fault is the recorded observation [1]; the 604's little-endian restrictions
(alignment exceptions for non-natural accesses, `lmw`/`stmw`/string instructions) [8] Table 1-2
were not hit at any recorded point. What is *not* established is the byte-lane arithmetic of a
little-endian client's PCI register accesses through Bandit — the platform's documented conversion
rules ([bandit.md §3.5](../machines/tnt/bandit.md)) are stated for the big-endian host, and whether
they are symmetric for a little-endian one is unverified ([§6](#6-open-questions), item 10).

### 3.3 The console

The recorded bring-up's console is the serial stream: SETUPLDR prints its banner, file-load
progress and the HAL menu to the ARC console, and the recorded session reads that output from the
firmware's serial console [1] (*observed*; the menu screenshot above is rendered from that
stream). Whether the veneer or SETUPLDR writes the 54M30 frame buffer as well — that is, whether
Setup has a usable screen on this machine at all — is not established
([§6](#6-open-questions), item 7). The machine's other boot consoles belong to other software: the
front LCD is POST and Open Firmware territory ([ans.md §2.7](../machines/ans/ans.md)), and AIX
takes it over for its three-digit startup codes ([ans500.md §5.4](../machines/ans/ans500.md));
nothing in evidence says NT could use it.

### 3.4 What an Apple Network Server HAL would face

No Apple HAL exists in any evidence, and the menu does not name the machine; what follows is the
requirement list as *inference* from the machine pages, not from any NT-side source — every item
is *inferred — unverified*:

- **Interrupt architecture.** Grand Central's single-collector design with the ANS's own
  external-line remap ([ans.md §5.1](../machines/ans/ans.md)) — the delta that stops a 9500 ROM
  booting an ANS is the same class of problem a HAL's interrupt wiring faces.
- **Storage.** The 53C825A pair as the only fast disk path ([ans.md §2.8](../machines/ans/ans.md))
  — a HAL would need a SCSI driver that does not exist in evidence (§3.1).
- **PCI enumeration.** The firmware's device tree, discovery-order BAR assignment and interrupt
  properties ([ans.md §3.3](../machines/ans/ans.md), [ans.md §4.2](../machines/ans/ans.md)) —
  consumed rather than reprogrammed, as AIX consumes them ([ans.md §2.7](../machines/ans/ans.md)).
- **The platform's endianness seam** (§3.2) — an NT HAL is the one piece of ANS software that
  would run little-endian natively.

The recorded investigation's proposed experiment — select a shipped HAL (a PowerStack entry, say)
and watch where its **first hardware access** faults, to turn this inference list into a concrete
one — was not performed [1] ([§6](#6-open-questions), item 13).

## 4. Installation & bring-up

### 4.1 The recorded configuration

| Item | Value | Evidence |
|---|---|---|
| Machine | Apple Network Server 500/132 ([ans500.md §1](../machines/ans/ans500.md)) | [1] |
| RAM | 64 MB | [1] (*observed*) |
| Firmware | the 2.26NT NT-capable prototype Open Firmware ROM (§1.3) | [1] [2] [3] |
| NT 4.0 OEM CD | target 0 of the first SCSI controller, `/bandit/53c825@11/sd@0,0` | [1] |
| Veneer carrier disk | target 0 of the second controller, `/bandit/53c825@12/sd@0,0` | [1] |

No RAM minimum for NT on this machine is documented anywhere; 64 MB is the recorded working
configuration, not a requirement (*observed* [1]). The CD sits on the first controller and the
carrier disk on the second, matching the firmware's own disk-alias split of the backplane across
the two 53C825As ([ans.md §1.3](../machines/ans/ans.md)).

### 4.2 The bring-up recipe, in order

The recorded sequence [1], each step's necessity evidenced by the wall it removes:

1. `setenv little-endian? true`, `setenv real-mode? false`, `setenv real-base 3F00000`,
   `setenv load-base 3E00000`, `reset-all` — the client contract of §2.2.
2. Load the veneer: `read-blocks` from its disk, `init-program` through the ROM's `pe-loader`,
   then the four pokes of [§2.3](#23-loading-the-veneer-read-blocks-pe-loader-four-patches).
3. Install `bootpath` as in [§2.4](#24-the-bootpath-contract-and-the-arc-name) — the `VrOpen
   returned d` wall.
4. The two `c!` patches of [§2.5](#25-setupldrs-device-probe-and-apples-disk-label) — the
   corrupted-file wall and the raw-access wall.
5. `go`.

The result, at this configuration, is the HAL menu of [§2.6](#26-the-hal-gate-where-the-boot-stops)
— and no further: no HAL choice from the menu is evidenced, so no installation of the OS itself is
recorded. What the installer would do past the menu is entirely outside the corpus
([§6](#6-open-questions), item 14).

### 4.3 Bring-up on real hardware

The public transcript's furthest point on a real ANS 700 is the Setup banner followed by the
corrupted-file message [2] (*observed*). The recorded bring-up reproduces exactly that screen and
passes it [1]. Both walls are properties of the shipped binaries and of Apple's firmware (§2.4,
§2.5) — the `partition(1)` and `:0` appends are the veneer's, and the `disk-label` argument
semantics are the machine's own — so the two one-byte patches and the `bootpath` installation are
machine facts, not host-dependent ones: the same recipe applies on real hardware, and the recorded
investigation states it was worked out to be repeatable on either [1]. Everything past the second
wall — the CDFS mount, the SIF parse, the kernel load, the menu — is the same shipped binaries
running under the same machine's firmware [1].

## 5. Observed behaviour & quirks

### 5.1 The `partition(1)` append and the `cdrom` grammar

The veneer appends `partition(1)` to every boot path it derives, correct for a hard disk and
illegal for a CD-ROM; the Setup loader's name parser accepts `cdrom(n)fdisk(n)` **only** at the
end of a name, and renders the violation as "is corrupted" ([§2.4](#24-the-bootpath-contract-and-the-arc-name))
[1] [5] [6] (*observed*). This is the real-hardware wall — identical binaries, identical firmware
conventions [1] [2].

### 5.2 Apple's `disk-label` refuses raw access for a partition argument

Any non-empty `disk-label` argument with no file name triggers the `CD001` detection and the
`iso-9660-files` interposition, whose no-path `open` parks the instance on the ISO root directory
as a pseudo-file (428 bytes on the recorded CD) with clamping `read` and a `seek` that does not
move the position; raw sector access is available only with an **empty** argument
([§2.5](#25-setupldrs-device-probe-and-apples-disk-label)) [1] (*observed*). A firmware behaviour
of the machine, independent of NT — it bites any byte-oriented client that probes a device through
a partition-style argument.

### 5.3 The firmware's `$load`/deblocker ISO seek defect

Recorded alongside the NT work, and a firmware defect on its own terms: the deblocker is left at
the absolute extent and `read-blocks` adds the extent again once `open-ok?` is set, and the
firmware's `$load` never seeks — so a bare `load` of a large ISO file is wrong, on the machine's
own firmware [1] (*observed*). The NT path never hits it: the veneer seeks before every read, and
SETUPLDR reads raw sectors [1].

### 5.4 `bootpath` is a `load`/`boot` product only

A client loaded by hand runs with no `bootpath` set — only the firmware's `load`/`boot` install
it ([§2.4](#24-the-bootpath-contract-and-the-arc-name)) [1] [3] (*observed*). The corollary for
any hand-loaded Open Firmware client, NT or otherwise: install the property yourself, and use the
internal `(property)` with the `_chosen` phandle — the external `dev /chosen … property` form did
not take [1].

### 5.5 The kernel is loaded but never runs

The Setup loader reads `\PPC\NTKRNLMP.EXE` in full — 84 SCSI READ commands, 666 blocks — before
asking anything, then stops at the menu with the kernel inert in memory: the HAL's imports are
unbound until a HAL is chosen, and none is ([§2.6](#26-the-hal-gate-where-the-boot-stops)) [1]
[6] (*observed*). Load progress is not proof of execution; the recorded session is the clean
demonstration.

### 5.6 Small observed behaviours

- The final "Booting from '…'" line echoes the ARC name *after* the `partition(1)` patch — the
  name the veneer derived, not the Open Firmware path [1].
- `VrOpen`'s failure prints as a single character, the ARC error value (`d`, ENODEV) [1].
- SETUPLDR is a six-section relocatable image; the veneer relocates it and jumps to it [1].
- The computer-type menu offers uniprocessor **and** multiprocessor variants of the Powerized
  HALs [1] — on a machine family whose top configuration is a dual-604 ANS 700/200SMP
  ([ans700.md §1.1](../machines/ans/ans700.md)); no evidence connects the two.
- The Setup banner's file-load line names the "Windows NT Executive" — the kernel image being
  read is the loaded-but-inert `NTKRNLMP.EXE` of §5.5 [1].

## 6. Open questions

1. **The 2.26NT ROM's identity and deltas.** Its version and checksum fields, its package list,
   and its differences from the production 1.1.22 and prototype 2.0 images ([ans.md
   §1.3](../machines/ans/ans.md) tabulates those two) are not established; the NT-capable image
   is evidenced only by live use, and its `pe-loader` package by one recorded call.
2. **Whether NT ran on the ANS inside Apple.** Reported in the community record [2]; no Apple
   document in the corpus attests it, and the one Apple sentence that gestures at NT is
   marketing (§1.3).
3. **The CD's HAL inventory.** Which `HAL.DLL` files `\PPC\` carries and which file each menu
   entry selects is not recorded — only the menu text is.
4. **The load patches.** Why the two `claim` calls fail on this firmware, and what the fourth
   patch (`$53DB0`) changes, are both unestablished (§2.3).
5. **An automatic NT boot path.** Whether the 2.26NT ROM integrates the veneer with
   `boot-device`/`boot-file` so NT boots without the command line, or the hand-load is the only
   route, is not established (§2.1).
6. **The veneer's service surface and device classification.** `VrOpen` is named in the record;
   the read/seek/key services SETUPLDR uses are not enumerated, and how `find_boot_dev` classifies
   the CD as `cdrom` rather than `disk` is not established (§2.4).
7. **The console's reach.** Whether the veneer or SETUPLDR writes the 54M30 frame buffer — whether
   NT Setup has a usable screen on this machine at all — is not established; the recorded session
   used the serial console (§3.3).
8. **The recognizer read pattern.** Offsets and sizes are observed; which recognizer each read
   belongs to is inference (§2.5).
9. **The `iso-9660-files` root pseudo-file.** The 428-byte size and the `seek`/`read` clamps are
   observed on this CD; the package's general contract is not documented in the corpus (§2.5).
10. **Bandit under a little-endian client.** The platform's byte-lane conversion rules are stated
    for a big-endian host ([bandit.md §3.5](../machines/tnt/bandit.md)); their symmetry for a
    little-endian client is unverified (§3.2).
11. **A 53C8xx driver for NT.** None exists in evidence; without one no HAL can use the machine's
    only fast disk path (§3.1).
12. **The ARC name grammar's full token set.** `multi`/`scsi`/`disk`/`rdisk`/`fdisk`/`partition`/
    `cdrom` are observed from one EINVAL path; the complete grammar is not (§2.4).
13. **Whether a shipped HAL could run at all.** The experiment — select a shipped HAL and watch
    its first hardware access fault — was proposed, not performed (§3.4).
14. **The installer past the menu.** No HAL choice is recorded, so nothing of the actual
    installation flow — partitioning, file copy, the ARC boot-path setup an installed NT system
    would need — is in evidence (§4.2).
15. **The version history of the PowerPC port.** Which NT releases were built for PowerPC, when
    4.0 shipped, and whether any later build exists, is outside the corpus (§1.2).

## References

1. Investigation log, "Windows NT 4.0 Setup on the emulated Apple Network Server: through the
   veneer to the HAL question", 2026-09-05 —
   [docs/notes/2026-09-05-ans-windows-nt-setupldr.md](../../notes/2026-09-05-ans-windows-nt-setupldr.md):
   the wall-by-wall analysis of the recorded bring-up (the `VrOpen` ENODEV wall, the ARC-name
   EINVAL wall, the `disk-label` raw-access wall), the `bootpath` and two-byte patch recipes, the
   SETUPLDR probe and load counts, and the HAL-menu output.
2. Public community thread "Apple Network Server MacOS-based ROMs found" (TinkerDifferent forum),
   2025–2026 — posts 43745 (the detokenized 2.26NT Open Firmware source), 49404, 49460, 49785 and
   49786: the real-hardware Open Firmware transcript whose furthest point is the corrupted-file
   wall, the four-patch veneer load recipe, and the NT-capable ROM's provenance from Apple's
   server group.
3. Apple Network Server boot ROM, NT-capable prototype ("2.26NT") — 4 MB Open Firmware image;
   evidence used live: the `little-endian?`/`real-mode?`/`real-base`/`load-base` configuration
   variables, the `pe-loader` package's `init-program`, the `$load` `bootpath` contract, and the
   little-endian operation of the firmware's own command interpreter.
4. Windows NT 4.0 OEM CD-ROM, PowerPC edition — on-media structure as read in the recorded
   bring-up: `\PPC\VENEER.EXE`, `\PPC\SETUPLDR`, `\PPC\NTKRNLMP.EXE` (666 blocks), the 126 KB
   root `TXTSETUP.SIF`, and the computer-type (HAL) menu content.
5. VENEER.EXE, the Open Firmware-to-ARC veneer (FirmWorks/Microsoft), `\PPC\VENEER.EXE` as a bare
   COFF image with its COFF symbol table intact — image base `$50000`; `find_boot_dev`, the
   ARC-name derivation, the `partition(1)` and `:0` appends, the patched `claim` calls; source
   paths in the symbol table: `D:\nt\private\ntos\boot\veneer\vr*.c`.
6. SETUPLDR, the Windows NT Setup loader, `\PPC\SETUPLDR` as a bare COFF image with its COFF
   symbol table intact — six sections; `BlGenerateDeviceNames`, `SlFriendlyError` (setup.c line
   416), the FAT/NTFS/CDFS recognizers, the CDFS mount over raw ARC reads, the deferred
   `HAL.DLL` import binding; source path in the symbol table:
   `D:\nt\private\ntos\boot\setup\setup.c`.
7. IEEE Std 1275-1994, *IEEE Standard for Boot (Initialization) Firmware: Core Requirements and
   Practices* — §3.5 the standard system nodes (`/chosen`, the `bootpath` property), §7.4.3
   Booting (the loading process and `init-program`), §3.8.1 the `disk-label` support package,
   §3.8.3 the `deblocker` support package.
8. Motorola, Inc., *MPC604 RISC Microprocessor User's Manual* (MPC604UM/AD) — Table 1-2 alignment
   exception conditions in little-endian mode; Table 4-3 MSR bit settings (LE, bit 31).
9. Apple Computer, Inc., *Apple Network Server Hardware Developer Notes* — §1.2.3 target
   competition ("Windows NT encroachments"); §2.2 Bandit's default-swapped register accesses;
   §2.8 the 54M30's little-endian frame-buffer window.
10. Apple Computer, Inc., *Apple Network Server Software Developer Notes* — Chapter 6, "The Open
    Firmware Device Tree": the firmware's IEEE 1275 and PCI Bus Binding conformance.
