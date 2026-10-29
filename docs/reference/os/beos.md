# BeOS 5.0.3 on the Power Macintosh

**Contents:**

1. [Scope & identity](#1-scope--identity) — what BeOS is and how it reaches Power Macintosh hardware; the
   evidenced release and its media; the supported-hardware matrix as the Launcher's own requirement
   string bounds it
2. [Boot architecture](#2-boot-architecture) — Mac OS beneath, the BeOS Launcher over; the kernel
   payload and the driver inventory its strings reveal; the untraced handoff
3. [Hardware interface](#3-hardware-interface) — the PCI platform as the release's drivers name it;
   MESH SCSI and the storage device path; the multi-track CD contract; video and the
   monitor-type question
4. [Installation & bring-up](#4-installation--bring-up) — the release media; prerequisites; the
   recovered dialog sequence; the installer's post-copy script; the target volume
5. [Observed behaviour & quirks](#5-observed-behaviour--quirks)
6. [Open questions](#6-open-questions)

References

---

## 1. Scope & identity

### 1.1 What BeOS is

BeOS is Be Inc.'s operating system, and the release this page's evidence covers — **BeOS
Professional Edition 5.0.3**, the 2000 Gobe Software edition — is a dual-platform product "for
Intel and PowerPC" [1]. On the PowerPC side BeOS does not boot these machines from bare metal:
the machine boots Mac OS first, and `BeOS_Launcher`, an ordinary Macintosh application carried
on the release disc, loads the BeOS kernel and hands the machine over [3] (*observed*, the
disc's own Mac Tools README, quoted in §2.1). The kernel is not a standalone image file the way
[MkLinux](mklinux.md) ships `Mach Kernel`; it rides in the Launcher's resource fork together
with its driver set [2] [7] (*observed*, string recovery).

The layered shape is therefore the one all three non-Apple operating systems on Macintosh
hardware share — Mac OS beneath, the guest system over, a Macintosh-side piece of software as
the hinge ([A/UX](aux.md) §2.1; [MkLinux](mklinux.md) §2.1) — but with a different hinge at
each layer:

| System | Loader | Reached by | Guest kernel image |
|---|---|---|---|
| [A/UX](aux.md) | a Macintosh startup application | HFS boot blocks, replacing the Finder ([aux.md §2.2](aux.md)) | its own file on the A/UX slice |
| [MkLinux](mklinux.md) | the MkLinux Booter (control panel + extension) | the Finder, with a ten-second boot-time chooser ([mklinux.md §2.2](mklinux.md)) | `Mach Kernel`, a file in the System Folder |
| BeOS | `BeOS_Launcher`, a plain application | the operator double-clicking it; an optional chooser extension ([§2.1](#21-the-mac-os-bootstrap)) | inside the Launcher's resource fork [2] |

This page documents BeOS on the Power Macintosh as far as the primary-evidence corpus takes
it. The corpus is honest about its own shape, and so is this page: it consists of a
sector-level survey of the release media [2], the install procedure recovered from the disc's
own figures and post-copy script [4] [5], the Macintosh-side tools and README as they sit on
the disc [3], and a string-level characterization of the Launcher binary [7]. **No BeOS boot
has been recorded on any machine** — nothing in the corpus establishes that the recorded
procedure has ever been driven to completion — the Launcher has not been disassembled, and
the developer documentation the disc carries has not been read into evidence. The page is
correspondingly lean: it records the media, the requirement bounds, the driver inventory the
strings reveal, and the recovered procedure, and leaves the rest — above all the handoff
protocol and the register-level driver behaviour — to [§6](#6-open-questions).

### 1.2 Versions and the evidenced release

| Item | Value | Evidence |
|---|---|---|
| Evidenced release | BeOS Professional Edition 5.0.3, Gobe Software edition, 2000 — "for Intel and PowerPC" | [1] (*observed*, the archived item's metadata record) |
| Media | one CD-ROM, three-track mixed mode, raw Mode1/2352, 328,360 sectors ([§4.1](#41-the-release-media)) | [2] (*observed*) |
| Copyright span of the on-disc installer script | © Be Inc. 1997–1999 | [5] (*observed*) |
| Prior release referenced | the R4-era generation — the installer's post-copy script exists to clean up files "obsoleted" since R4 ([§4.4](#44-the-post-copy-script-installerinitscript)) | [5] (*observed*) |
| Companion x86 release on the same disc | BeOS Personal Edition, installed as a file under Windows ([§5.5](#55-the-x86-side-of-the-same-disc)) | [6] |
| PowerPC kernel | carried in `BeOS_Launcher`'s resource fork, with a live `ppc` install branch ([§2.2](#22-the-kernel-payload-and-the-driver-inventory)) | [5] [7] (*observed*) |

The corpus holds exactly one BeOS release, at the end of the line; it contains no R3 or R4
media, no release notes for any version, and no statement of which earlier releases ran on
Power Macintosh hardware. The release-history questions that a version matrix would answer
are therefore open questions (items 4 and 5 of [§6](#6-open-questions)), not silently filled
from general knowledge.

### 1.3 The supported-hardware matrix

The only hardware bound the corpus states is carried inside the Launcher binary itself: the
requirement string

> The BeOS requires a PowerPC 603 or 604 microprocessor. [7] (*observed*)

That single sentence excludes the 601 machines outright, which against the machines this
reference tree documents leaves the two-Bandit TNT machines as the only candidates:

| Machine | CPU as shipped | Meets the "603 or 604" bound | Reference |
|---|---|---|---|
| Power Macintosh 6100 / 7100 / 8100 | PowerPC 601 | no | [pdm.md](../machines/pdm/pdm.md) §2.2 |
| Power Macintosh 7200 (Catalyst) | PowerPC 601, soldered | no | [tnt.md](../machines/tnt/tnt.md) §1.2, §6.4 |
| Power Macintosh 7500 | PowerPC 601 at 100 MHz | no | [tnt.md](../machines/tnt/tnt.md) §1.2; [pm7500.md](../machines/tnt/pm7500.md) §1 |
| **Power Macintosh 8500** | **PowerPC 604 at 120 MHz, on a card** | **yes** | [pm8500.md](../machines/tnt/pm8500.md) §1.3, §3.1 |
| **Power Macintosh 9500, 9500/MP** | **PowerPC 604 at 120 or 132 MHz; two 604s on the MP card** | **yes** | [pm9500.md](../machines/tnt/pm9500.md) §1.3, §3.1 |
| Power Macintosh 7300 / 7600 / 8600 / 9600 (1997) | 604e / 604ev | unverified — the string names neither | [tnt.md](../machines/tnt/tnt.md) §6.5 |

Two bounds stack on top of the CPU string. First, the disc's Mac Tools README rules out the
contemporary Mac OS: "MacOS 9 is not supported" [3] (*observed*); no floor version is stated
anywhere on the disc ([§6](#6-open-questions), item 3). Second, nothing in the corpus has
been *run* — the matrix above is a bound derived from the release's own requirement string,
not a list of verified boots. A practical note for the two eligible machines: the 8500 has a
built-in display path ([pm8500.md](../machines/tnt/pm8500.md) §3.3) while the 9500 has none and
presents its display as a PCI card ([pm9500.md](../machines/tnt/pm9500.md) §2.5, §3.8), and
the corpus establishes nothing about which display paths the release drives
([§3.4](#34-video-and-the-monitor-type-question)).

## 2. Boot architecture

### 2.1 The Mac OS bootstrap

BeOS reaches Power Macintosh hardware through Mac OS, and every recorded step before the
Launcher is the machine's own boot contract, cited rather than restated. The full chain, as
the disc's own tools define it [2] [3]:

```
Power Macintosh 8500/9500 reset and ROM boot: NanoKernel → Open Firmware
  (identity decode, device tree, PCI address assignment, NVRAM) → 68k emulator
  → Mac OS Toolbox → Finder
    (the platform's own boot contract — tnt.md §2.7)
  → the operator runs BeOS_Launcher (from the disc's "BeOS Mac Tools" folder)
  → the Launcher loads the BeOS kernel and driver set from its resource fork (§2.2)
  → the BeOS Installer runs under BeOS (§4.3)
```

The Macintosh-side entry point is three files [2] [3] (*observed*):

| File | Type/creator | Size | Role |
|---|---|---|---|
| `BeOS_Launcher` | `APPL`/`BELN` | data 25,933 + resource 535,049 | the launcher; its resource fork carries the kernel and drivers [7] |
| `_OS_Chooser` | `scri`/`BEOS` | resource 573,893 | "an extention which allows you to choose your OS at boot" [3] |
| `README` | `TEXT` | 177 | the four-line note quoted below |

The README, verbatim and complete [3] (*observed*):

> BeOS Tools for MacOS
>
> The Launcher will launch the BeOS from your Desktop.
>
> The OS Chooser is an extention which allows you to choose your OS at boot.
>
> MacOS 9 is not supported.

Two consequences, both the same shape as [MkLinux](mklinux.md) §2.1 draws for its Booter:

1. **A working Mac OS installation is a hard prerequisite.** The Launcher is a Macintosh
   application and `_OS_Chooser` is a Macintosh system extension — both run only on top of a
   booted Mac OS [3]. (The corpus does not record which Mac OS version the recorded media
   was exercised against, and the README bounds it only from above.)
2. **The machine spends every boot's first phase in the ordinary ROM and Mac OS path.** The
   NanoKernel, Open Firmware and Toolbox phases the Launcher depends on are
   [tnt.md](../machines/tnt/tnt.md) §2.7, cited here and not restated.

The `_OS_Chooser` extension differs from the MkLinux Booter's chooser in one recorded
respect: it is *optional* — the default path is a manual launch from the desktop, and the
extension adds a boot-time choice [3] ([mklinux.md §2.2](mklinux.md) describes the Booter's
own chooser for contrast).

### 2.2 The kernel payload and the driver inventory

`BeOS_Launcher`'s resource fork is not a stub: it carries the BeOS kernel and its driver set
[7] (*observed*, the strings recovered from it — the fork has not been disassembled, and the
resource-level layout of kernel-versus-drivers is not traced, [§6](#6-open-questions),
item 1). The recovered strings name the drivers the kernel carries:

| Recovered string | Driver | Hardware it drives | Reference |
|---|---|---|---|
| `MESH SCSI: id %.2x, regs %.8x int %d, dma-regs %.8x dma-int %d` | MESH SCSI | the internal SCSI cell on Grand Central — the MESH aperture, its DBDMA channel and both interrupt numbers ([grand-central.md](../machines/tnt/grand-central.md) §2.5; [dbdma.md](../machines/tnt/dbdma.md) §1.4) | [7] |
| `swim3 - cannot allocate request for I/O` | SWIM 3 | the floppy cell ([tnt.md](../machines/tnt/tnt.md) §4.3; [dbdma.md](../machines/tnt/dbdma.md) §1.4) | [7] |
| `IDE MAC: openfirmware device %s doesn't have a 'reg' property` | IDE | devices enumerated from the Open Firmware device tree | [7] |
| `nvram - unknown property size` | NVRAM | the Grand Central nonvolatile store ([grand-central.md](../machines/tnt/grand-central.md) §2.6) | [7] |
| a large body of BFS and DBDMA diagnostic messages | filesystem, DMA engine | the Be File System ([§4.5](#45-the-target-volume)); the DBDMA engine ([dbdma.md](../machines/tnt/dbdma.md) §1) | [7] |

The IDE string is direct evidence that the kernel enumerates hardware through the Open
Firmware device tree, reading node properties — the string reports, as an error, an
Open Firmware device *lacking* a `reg` property [7]. The TNT ROM's Open Firmware publishes
exactly such a tree, with `reg` and `AAPL,interrupts` properties on every device node
([tnt.md](../machines/tnt/tnt.md) §1.3; [dbdma.md](../machines/tnt/dbdma.md) §4.1). The
MESH string's four reported values — device registers, interrupt, DMA registers, DMA
interrupt — mirror the two-aperture-plus-two-interrupt publication the firmware's own `mesh`
node carries (register aperture at Grand Central +$18000, DMA channel at +$8A00,
interrupt pair 13/10) ([dbdma.md](../machines/tnt/dbdma.md) §1.4) — consistent, though the
identity of the two is *inferred from the string's wording*, not traced into code.

### 2.3 The handoff: not traced

Nothing in the corpus records how the Launcher transfers control to the kernel — which Mac
OS services it uses to claim memory, what it does to the Mac OS and Open Firmware state,
what MMU state the kernel entry expects, or what argument block (if any) passes the boot
device and display configuration. For [MkLinux](mklinux.md) this contract was pinned by
disassembly of the kernel's entry and the loader's live register state
([mklinux.md §2.3–§2.4](mklinux.md)); for BeOS the equivalent evidence does not exist, and
this page claims nothing about it. It is [§6](#6-open-questions), item 1.

## 3. Hardware interface

### 3.1 The PCI platform as the release's drivers name it

The TNT platform's own device set, with the driver evidence the corpus holds for each
subsystem — and, as important, where it holds none:

| Subsystem | Part | Driver evidence in the corpus | Reference |
|---|---|---|---|
| CPU | PowerPC 604 | the Launcher's requirement string [7] | [pm8500.md](../machines/tnt/pm8500.md) §3.1; [pm9500.md](../machines/tnt/pm9500.md) §3.1 |
| Internal SCSI | MESH (343S1146), DBDMA channel 10 | the MESH string [7] | [grand-central.md](../machines/tnt/grand-central.md) §2.5; [dbdma.md](../machines/tnt/dbdma.md) §1.4 |
| External SCSI | the Curio's 53C94 cell | none recovered | [pm8500.md](../machines/tnt/pm8500.md) §3.6; [pm9500.md](../machines/tnt/pm9500.md) §3.6 |
| Floppy | SWIM III, DBDMA channel 1 | the swim3 string [7] | [tnt.md](../machines/tnt/tnt.md) §4.3; [dbdma.md](../machines/tnt/dbdma.md) §1.4 |
| ATA/IDE | via the Open Firmware tree | the IDE string [7] | [tnt.md](../machines/tnt/tnt.md) §2.7 |
| NVRAM | Grand Central nonvolatile store | the nvram string [7] | [grand-central.md](../machines/tnt/grand-central.md) §2.6 |
| DMA | the DBDMA engine | DBDMA diagnostic messages [7] | [dbdma.md](../machines/tnt/dbdma.md) §1 |
| Filesystem | BFS | BFS messages [7]; the disc's own track-3 volume ([§3.3](#33-the-cd-rom-a-multi-track-contract)) | §4.5 |
| Video | unattested — the monitor-type dialog ([§3.4](#34-video-and-the-monitor-type-question)) is the only display evidence | — | [pm8500.md](../machines/tnt/pm8500.md) §3.3; [pm9500.md](../machines/tnt/pm9500.md) §3.8 |
| Ethernet | MACE | none | [tnt.md](../machines/tnt/tnt.md) §4.3 |
| Sound | AWACS | none | [tnt.md](../machines/tnt/tnt.md) §4.3 |
| Input | ADB through the Cuda | none | [tnt.md](../machines/tnt/tnt.md) §4.3 |

The empty rows are a statement, not an omission: the absence of an Ethernet, sound or input
string in the recovered set does not mean the release lacks those drivers — the string
recovery was not exhaustive [7] — but the corpus cannot place them, so neither does this
page ([§6](#6-open-questions), items 8–9).

### 3.2 SCSI: MESH and the storage device path

The installer writes to a **SCSI** target through BeOS's own MESH driver: the format panel
of the recovered procedure displays the raw device path `/dev/disk/scsi/020/0_3` for the
target volume [4] (*observed*, the disc's own figure). The numbering scheme of that path —
which digit names the bus, which the target ID, which the LUN — is documented nowhere in
the corpus and is [§6](#6-open-questions), item 10; the page declines to guess it. The chip
side — the MESH register file, its reset and data-phase behaviour, and the DBDMA channel
that feeds it — is [grand-central.md](../machines/tnt/grand-central.md) §2.5 and
[dbdma.md](../machines/tnt/dbdma.md) §4.6, cited not restated. What the string evidence adds
is the driver's own self-description: per-target state carrying the device registers, the
interrupt, the DMA registers and the DMA interrupt separately [7] — the two-aperture shape
the platform publishes.

### 3.3 The CD-ROM: a multi-track contract

The release disc is a three-track mixed-mode CD, and the OS depends on the whole of it. The
recorded sector-level survey [2] (*observed*, read from the raw image):

| Track | Start LBA | Frames | Contents |
|---|---|---|---|
| 1 | 0 | 48,658 | hybrid ISO9660 + HFS, both volumes named `BeOS_Tools` |
| 2 | 48,658 | 158,870 | x86 boot code (16-bit real-mode entry, bytes `B8 00 90 8E D0 89 C4 FC…`) |
| 3 | 207,528 | 120,832 | BeFS, `BFS1` / `BIGE` (big-endian), volume name `BeOS 5 Pro Edition` |

Every sector is raw Mode1/2352 — 16-byte sync/header, 2048 bytes of user data, 288 bytes of
EDC/ECC — across 328,360 sectors [2] (*observed*). Track 1 carries the HFS volume as an
overlay on the same file data (the HFS master directory block sits at byte offset 1024 of
LBA 0); there is no Apple partition map, which is normal for a hybrid CD — the disc is
consumed by the running system, never as a ROM boot device [2].

Track 3 is the PowerPC payload the installer copies from. Its BeFS superblock, as read
directly off the media [2] (*observed*):

| Field | Value |
|---|---|
| magic | `BFS1` / `BIGE` (big-endian) |
| block size / shift | 2048 / 11 |
| inode size | 2048 |
| magic2 | `$DD121031` |
| num_blocks | 120,832 (236.0 MiB) — identical to the track's frame count: one BeFS block per CD frame |
| used_blocks | 103,303 (201.8 MiB) |
| allocation groups | 8 (`ag_shift` 14, `blocks_per_ag` 1) |
| volume name | `BeOS 5 Pro Edition` |

The multi-track shape is not incidental: the installer's own post-copy script, extracted
from track 3, mounts **track 1 by volume name while the system runs from track 3** [5]
(*observed*, [§4.4](#44-the-post-copy-script-installerinitscript)). A CD device serving
this release must therefore present the full track table through the SCSI READ TOC command
and serve both tracks' data through the one device at once. The contract stated for a
re-implementer: a single-track view of this disc — a 2048-byte-per-sector image of track 1
alone, which is what a plain ISO conversion of the media produces [2] — is not the release
disc; it discards the PowerPC volume silently.

### 3.4 Video and the monitor-type question

The first dialog of the recovered install procedure asks a question before any graphical
mode is picked: the **monitor type**, `Fixed-Frequency` or `Multi-Frequency`, defaulting to
Multi-Frequency [4] (*observed*, the disc's own figure). The procedure's own annotation —
that this is "how BeOS decides what it may program the display to" — makes it a
Macintosh-specific branch: the release asks the operator to classify the monitor before it
programs the video hardware [4]. Which display hardware the release then drives — the 8500's
Chaos/Control/RaDACal built-in path ([pm8500.md](../machines/tnt/pm8500.md) §3.3), or a
PCI card of the kind the 9500 requires ([pm9500.md](../machines/tnt/pm9500.md) §3.8) — is
not established by the corpus ([§6](#6-open-questions), item 7). The practical consequence
of the dialog's position is [§5.3](#53-the-monitor-type-question-precedes-any-display-setup).

## 4. Installation & bring-up

### 4.1 The release media

| Item | Value | Evidence |
|---|---|---|
| Image | `beos-5.0.3-professional-gobe.bin`, 772,302,720 bytes, MD5 `FFFB618C7A48EA2091CB554E51DCF59D` — verified against the archived item's manifest | [1] [2] (*observed*) |
| Cue sheet | `beos-5.0.3-professional-gobe.cue`, 186 bytes — the track table of §3.3 in text form | [2] (*observed*) |
| Sector format | Mode1/2352 raw throughout; 328,360 sectors | [2] (*observed*) |
| Verification method | read raw sectors from the `.bin` directly; the `.bin` and `.cue` kept together | [2] |

The one warning the survey insists on, and this page repeats: **do not convert the media to
a single-track ISO image.** An ISO image is by definition one track of 2048-byte sectors; it
captures track 1 and silently discards track 3 — the PowerPC BeFS volume the installer
copies from [2] (*observed*; the mechanism is §3.3).

### 4.2 Prerequisites

| Requirement | Value | Evidence |
|---|---|---|
| Mac OS | a working installation; the README rules out Mac OS 9 and states no floor | [3] |
| CPU | PowerPC 603 or 604 | [7] |
| Entry | run `BeOS_Launcher` from the disc's `BeOS Mac Tools` folder; `_OS_Chooser` optional | [3] |
| CD device | must expose all three tracks of the release disc ([§3.3](#33-the-cd-rom-a-multi-track-contract)) | [2] [5] |
| Target disk | a second disk — the installer's `Onto:` pop-up offers HFS volumes as targets, but initializes them ([§4.5](#45-the-target-volume)) | [4] |
| Installer payload | copied from the track-3 BeFS volume | [2] [5] |

The corpus states no RAM minimum, no disk-size minimum and no Mac OS version floor; the
table contains every requirement the evidence supports.

### 4.3 The recovered dialog sequence

The procedure is recovered from the release disc itself [4]: the
`Macintosh/Installing_the_BeOS` document carries **13 PICT figures (resource ids
1000–1012)** and — the finding that forced the reconstruction — a genuinely **zero-byte**
`TEXT` resource ([§5.1](#51-there-is-no-prose-install-manual-on-this-disc)). Every dialog
below is a **BeOS dialog, drawn after the handover** — none is Mac OS [4]. The sequence,
by figure resource id:

1. **Monitor type** (id 1000): `Fixed-Frequency` or `Multi-Frequency`, default
   Multi-Frequency ([§3.4](#34-video-and-the-monitor-type-question)).
2. **Installer main window** (id 1001): source fixed as `Install from: BeOS Install`; the
   `Onto:` pop-up lists candidate targets (in the figure, `My Be Disk` and
   `Prodigal Sorcerer [Mac HFS]` — an HFS volume offered as a target, meaning reformat, not
   sharing); buttons `More Options` and `Begin`.
3. **More Options** (id 1011): checkboxes `Clean Install` and `Install Optional Items`, both
   unchecked by default, plus the `Install from:` source button.
4. **Initialization requirement** (ids 1002, 1004): an HFS or DOS target — "The disk
   'Prodigal Sorcerer' you have selected needs to be initialized prior to installation.
   Initializing the disk will erase all the Macintosh or DOS files you may have on the
   disk." — with `Initialize` / `Stop installation`; an existing BeOS volume instead offers
   a third choice, `Install as is`, which preserves the volume.
5. **Format** (ids 1005, 1006): the `Be File System` panel, titled with the raw device path
   (`/dev/disk/scsi/020/0_3` in the figure), fields `File System Block Size` (1024) and
   `Volume Name` (`My Be Disk`); then the destructive confirmation "Initializing will
   destroy all data on this partition."
6. **Copying** (id 1007): the main window shows the current item ("Installing GLTeapot")
   with only `Stop` enabled.
7. **Conflict prompts** (ids 1003, 1012), on a target carrying a previous BeOS: per file —
   "A copy of 'BeMail' already exists on the destination volume." with
   `Keep` / `Replace` / `Replace All Older`; and per folder — "The Installer found a folder
   'system' from a previous version of the BeOS. Should this folder be removed?" with
   `Remove` / `Keep`.
8. **Abort warning** (id 1008): a stopped install may leave the disk unbootable.
9. **Startup disk** (id 1009): "Do you want to make 'My Be Disk' the startup disk?"
10. **Done** (id 1010): "Installation was successful. Your machine will reboot after you
    exit the installer."

The block size offered by the format panel (1024) differs from the CD's own BeFS block size
(2048, §3.3) — the installer lays down a differently-blocked volume than the one it copies
from [2] [4] (*observed*).

### 4.4 The post-copy script: `InstallerInitScript`

The installer runs a shell script against the target after the copy — extracted from the
track-3 BeFS volume, 7,162 bytes, `/bin/sh`, © Be Inc. 1997–1999 [5] (*observed*). Two
parts of it matter to this page's evidence:

- **It mounts track 1 by volume name, while running from track 3** — the script ends:
  ```sh
  # mount BeOS_Tools ISO/HFS partition for shared files
  mountvolume -ro BeOS_Tools
  ```
  This is the direct evidence behind the multi-track contract of §3.3.
- **It carries a live PowerPC branch**, confirming `ppc` as a supported host rather than a
  leftover:
  ```sh
  if [ "$BE_HOST_CPU" = "ppc" ]; then
      rm -f `query "name = mergeres || name = mwbres || name = mwdres"`
      rm -f home/config/be/Preferences/Joysticks
      ...
  ```

The bulk of the script is upgrade cleanup — removing files obsoleted since the R4
generation, moving Metrowerks stationery, rebuilding indices with `mkindex`, rewriting
printer driver attributes with `catattr`/`addattr` — and it performs that work only when
`var/swap` already exists on the target, i.e. only when installing over a previous BeOS
[5] (*observed*). The R4 references are the corpus's only evidence of any release before
5.0.3 ([§6](#6-open-questions), item 4).

### 4.5 The target volume

The installer's target is a BFS volume it creates itself: the format panel of step 5 fixes
the file system (the Be File System), offers a block size (1024 in the figure) and a volume
name, and the initialization is destructive in both observed variants — an HFS or DOS volume
is erased wholesale, and an existing BeOS volume is kept only through the explicit
`Install as is` choice [4] (*observed*). The final two dialogs close the install: the
startup-disk question (step 9) and the reboot-on-exit (step 10) [4]. What the installed
system then boots *through* — whether the installed volume is reached on later power-ons
by Mac OS plus the Launcher again, by the `_OS_Chooser` extension, or by some
startup-disk setting the step-9 dialog writes — is not established by the corpus
([§6](#6-open-questions), item 12); the recorded evidence ends at the reboot.

## 5. Observed behaviour & quirks

### 5.1 There is no prose install manual on this disc

Worth stating plainly so nobody hunts for it again [2] [4] (*observed*): everything on the
disc was enumerated — the HFS and ISO9660 sides of track 1 (115 and ~120 entries) and all
9,446 named inodes of the track-3 BeFS volume — and the result is:

- `Macintosh/Installing_the_BeOS` has an **empty `TEXT` resource and a zero-byte data
  fork**; only its 13 figures survive. Its `styl` resource is 1,542 bytes (about 77 style
  runs), so styled prose did once exist — the text is missing from the media, not merely
  unused. Whether that is a mastering defect of this Gobe edition or true of the release
  generally is not established ([§6](#6-open-questions), item 15).
- The BeFS volume's `documentation/` tree is the **Be Book** (the developer API reference)
  and the BeIDE/Metrowerks manuals — no end-user guide.
- `MacOSNotes.fm.html` is a Metrowerks C compiler document about Mac OS targets, not a
  BeOS install note — a false lead worth flagging.

The release's ReadMe defers to "the installation section of the users guide" — the printed
*User's Guide*, which is not on the disc [2]. The procedure of §4.3 is therefore
reconstructed from the shipped artifacts themselves (the figures and the script), and is
the best available record.

### 5.2 Multi-track access is a prerequisite, not a convenience

The `mountvolume -ro BeOS_Tools` line of §4.4 runs while the installer's payload comes off
track 3: the system has both tracks of one disc in view at the same moment [5]. Any CD
subsystem serving this release must therefore carry the track table of §3.3 and serve
data from more than one track through the one device — the release simply does not install
against a single-track disc image [2] (*observed*; the ISO-conversion finding of §4.1).

### 5.3 The monitor-type question precedes any display setup

The first dialog of the recovered sequence (§4.3, step 1) asks the monitor type before the
graphical installer picks a mode [4]. The practical reading, from the procedure's own
annotation: an answer is required before any framebuffer is set up, so a driven bring-up
that reaches a dark screen should suspect this dialog rather than the video subsystem
(*inferred from the dialog's position in the sequence; the handoff's display state is not
traced*, [§6](#6-open-questions), item 1).

### 5.4 An HFS target is offered, and it means erasure

The `Onto:` pop-up offers HFS volumes as targets ([§4.3](#43-the-recovered-dialog-sequence),
step 2), which reads as coexistence and is not: selecting one triggers the initialization
requirement (step 4) — "Initializing the disk will erase all the Macintosh or DOS files
you may have on the disk" [4] (*observed*). Contrast the single-disk coexistence the MkLinux
bring-up achieves on an Apple partition map ([mklinux.md §4.4](mklinux.md)): BeOS 5.0.3's
installer offers no equivalent — the target volume is consumed whole.

### 5.5 The x86 side of the same disc

Not the PowerPC path, but it explains the disc's shape — tracks 1–2 and the `PMAGIC/` tree
[6] (*observed*, the disc's own readme documents): on a PC the operator either runs
`Personal/Setup.exe` to install **BeOS Personal Edition into a file under Windows**, or
uses the bundled **PartitionMagic Special Edition 2.02** (licensed by PowerQuest to Be) to
shrink an existing partition and create a 500 MB / 850 MB / 1.5 GB BeOS partition;
`Gobe/Rawrite.exe` plus `floppy.img` write a boot floppy for machines that cannot boot the
CD. Track 2's 16-bit real-mode boot code is that side's loader, and track 1's
`BeOS_Tools` volume is shared by both platforms — which is why the PowerPC installer mounts
it by name (§4.4).

### 5.6 The Launcher, observed end to end on a 9500

On a Power Macintosh 9500 (604, 64 MB) running Mac OS 7.6 with an ATI Mach64-based PCI display
card at 256 colours, `BeOS_Launcher` run from the disc's HFS side hands the machine over
directly — no dialog of its own precedes the handover — and the BeOS bootstrap scans for a
bootable BFS volume (*observed*, [8]): the bootstrap's strings name the scan order, SCSI disks,
SCSI CD-ROMs, then IDE [7]. Mac OS 7.6 is therefore sufficient (§6 item 3 narrows to "below
7.6 untested"). The display needs no driver of the release's own beyond the
`ATI-GX` app_server add-on; the monitor-type question of §4.3 step 1 was **not asked** on
this path.

With the track-3 BFS volume presented read-only on a SCSI CD-ROM device, the system boots
**from that volume** and, finding `/boot` read-only, runs `Bootscript.cd` in place of the
ordinary `Bootscript` [9]: only `app_server` and `registrar` start, and the Installer runs
directly (the license agreement first, then the window of §4.3 step 2). So the Launcher does
run BeOS live off the disc's BFS track (§6 item 2), and the Installer *is* that live system.
The Installer accepts an unpartitioned disk — it lists it by device path
(`[SCSI bus:0 id:1 partition:1]`), initializes BFS across the whole device
(`/dev/disk/scsi/0/1/0/raw`, block size 1024), copies, and on exit `Bootscript.cd` ejects the
disc and restarts the machine into Mac OS (*observed*). Running the Launcher again then boots
the installed volume to the Tracker and Deskbar desktop: the installed system still needs Mac
OS and the Launcher on every start (§6 item 12, for the Launcher path).

### 5.7 The kernel on the dual-processor 9500

`kernel_mac` carries a "2 processor machine" start path for the PowerSurge board
(`start_other_cpus`) beside the DayStar four-way paths [7]. Its protocol — the `ArbConfig`
probe, the `$F2800000` start vector, the `IntReg` AND-to-signal / OR-to-acknowledge pair, the
`WhoAmI` processor number, and the Ethernet-PROM read as the interrupt to processor 0 — is
recorded at the register level in [pm9500mp.md](../machines/tnt/pm9500mp.md) §3.4, from the
5.0.3 kernel itself [10]. On a 9500/180MP-class machine the system then runs both processors
symmetrically: "About BeOS" reports "2 PowerPC 604's", and Pulse shows two processor meters
(*observed*, [8]). Device interrupts stay on processor 0 — the only interrupt the second
processor can receive is the card's doorbell.

### 5.8 The kernel reads the time base with `mfspr`

The kernel's time-base reader samples `mfspr` SPR 285, 284, 285 (the TBU/TBL *write*
encodings) rather than `mftb` [10]. On the 603e family the two opcodes are one instruction —
"The MPC603e ignores the extended opcode differences between `mftb` and `mfspr` by ignoring
bit 25 of both instructions and treating them identically" [11] §2.3.5.1 — and the kernel
relies on the 604 behaving the same way: the value read calibrates the kernel's time scale,
and a processor that took the illegal-instruction exception instead would leave the scale at
zero, so that every delay loop (`snooze`) waits forever at the boot splash.

### 5.9 The Macintosh bootstrap and the interrupt controller

Before the kernel loads, the bootstrap services devices by polling Grand Central's interrupt
Events register itself, clearing what it handles with the Clear register [10]. On the
PowerSurge platform type it treats a pending source 30 as an interprocessor interrupt and
dispatches it to a handler table in which nothing is registered — a source-30 event latched at
that stage is never cleared and starves every other source. The bootstrap also reads the
station address from the Ethernet PROM (`$F3019000 + $10·n`, six bytes, bit-reversed) and
compares it with Open Firmware's `local-mac-address`. The two facts together constrain the
hardware: the primary's own PROM reads must not raise source 30
([pm9500mp.md](../machines/tnt/pm9500mp.md) §3.5).

## 6. Open questions

1. **The Launcher's internal protocol.** How `BeOS_Launcher` loads the kernel from its
   resource fork (which Mac OS services, which load addresses), what argument block it
   passes, what it does to the Mac OS, Open Firmware and MMU state before the jump, and
   how the resource fork divides kernel from drivers — all untraced. The binary has been
   characterized by strings only [7]; it has not been disassembled, and no handoff has
   been observed on any machine. This is the page's largest gap; compare what the MkLinux
   page could pin at the same point ([mklinux.md §2.3–§2.4](mklinux.md)).
2. **Whether the Launcher can run BeOS live off the CD**, or whether the kernel requires an
   installed volume — the corpus distinguishes the two paths nowhere.
3. **The Mac OS version floor.** The README excludes Mac OS 9 [3] and states no floor;
   whether the Launcher runs under 7.5/7.6/8.x is untested.
4. **The release history on Power Macintosh.** The corpus holds one disc; the
   `InstallerInitScript`'s R4-era cleanup [5] establishes that an R4 generation existed and
   was installed on PowerPC machines, but no R3 or R4 media, release notes or version
   matrix are in evidence. Which releases ran on which machines is not established.
5. **Whether the 604e / 604ev follow-on machines** (the 1997 7300/7600/8600/9600,
   [tnt.md](../machines/tnt/tnt.md) §6.5) satisfy the requirement string's "603 or 604",
   and what the release does on a 750-class processor — the string names neither.
6. **The `_OS_Chooser` extension's mechanism** — what it presents at boot, how it launches
   the Launcher, and where the choice persists. Only its one-line description and its
   resource-fork size are recorded [2] [3].
7. **Which display paths the release drives** — the 8500's built-in Chaos/Control/RaDACal
   video, PCI cards of the kind the 9500 requires, or both; and what the monitor-type
   dialog's two answers select in each case. Only the dialog itself is evidenced [4].
8. **Ethernet (MACE) support.** No driver string was recovered; presence unknown [7].
9. **Input: how the ADB keyboard and mouse reach the post-handover BeOS.** The installer is
   a graphical UI driven after the handover, so an input path must exist, but the corpus
   holds no evidence of it — the string recovery found no ADB or Cuda driver strings [7].
10. **The `/dev/disk/scsi/020/0_3` device-path numbering scheme** — which component names
    bus, target and LUN; documented nowhere in the corpus [4].
11. **The Be Book and the developer documentation on track 3** — enumerated [2] but unread
    in evidence; what they state about the PowerPC boot path, the driver model and the
    supported machines is unmined.
12. **How the installed system boots on subsequent power-ons** — what the step-9
    startup-disk dialog writes, whether Mac OS and the Launcher are still required, and
    what the installed volume's boot blocks look like. The recorded evidence ends at the
    reboot (§4.5).
13. **Register-level driver behaviour.** The DBDMA channels, MESH sequences, SWIM 3
    programming and NVRAM access the release's drivers perform are known only by name from
    the string inventory [7]; no annotated reading of the kernel or its drivers exists —
    everything at register level is inherited from the hardware pages
    ([dbdma.md](../machines/tnt/dbdma.md), [grand-central.md](../machines/tnt/grand-central.md))
    and unverified against BeOS itself.
14. **Sound and serial support** — AWACS and the GeoPort-capable ESCC channels: no evidence
    either way.
15. **The zero-byte `TEXT` resource of `Installing_the_BeOS`** (§5.1) — a mastering defect
    of this Gobe edition, or a defect of the 5.0.3 Professional media generally; one
    specimen has been examined [2].

## References

1. BeOS Professional Edition 5.0.3, Be Inc. (distributed by Gobe Software, Inc.), 2000 — the
   release CD-ROM as archived with its item metadata ("for Intel and PowerPC", Gobe
   Software, 2000) and manifest; the archived raw disc image
   (`beos-5.0.3-professional-gobe.bin`, 772,302,720 bytes, MD5
   `FFFB618C7A48EA2091CB554E51DCF59D`, verified against the manifest) and cue sheet.
2. The recorded BeOS release-media survey: the sector-level disc structure (Mode1/2352 raw,
   328,360 sectors; the three-track table with start LBAs 0 / 48,658 / 207,528 and frame
   counts 48,658 / 158,870 / 120,832; track 1's hybrid ISO9660+HFS volume `BeOS_Tools` with
   its master directory block at byte offset 1024 of LBA 0 and no Apple partition map; track
   2's 16-bit real-mode x86 boot code; track 3's BeFS volume `BeOS 5 Pro Edition` and its
   superblock — `BFS1`/`BIGE` big-endian, block size 2048, inode size 2048, magic2
   `$DD121031`, 120,832 blocks of which 103,303 used, 8 allocation groups), the full
   enumeration of the disc's catalogs (115 HFS entries, ~120 ISO9660 entries, 9,446 named
   BeFS inodes), the Mac Tools file inventory (`BeOS_Launcher` `APPL`/`BELN`, data 25,933 +
   resource 535,049; `_OS_Chooser` `scri`/`BEOS`, resource 573,893), the finding that
   `Installing_the_BeOS` carries a zero-byte `TEXT` resource with a 1,542-byte `styl`
   resource, and the string recovery from the Launcher binary (the 603/604 requirement
   string; the MESH, swim3, IDE/Open Firmware and nvram driver strings; the BFS and DBDMA
   diagnostic message bodies).
3. "BeOS Tools for MacOS" — the README carried in `Macintosh/BeOS Mac Tools/` on the release
   disc (TEXT, 177 bytes), quoted in full in §2.1: the Launcher's role, the OS Chooser
   extension, and the exclusion of Mac OS 9.
4. The recovered BeOS 5.0.3 installation procedure for Power Macintosh: the dialog sequence
   reconstructed from the 13 PICT resources (ids 1000–1012) of `Macintosh/Installing_the_BeOS`
   on the release disc — the monitor-type question, the installer window and its `Onto:`
   targets, More Options, the initialization requirement and its two variants, the Be File
   System format panel with the raw device path `/dev/disk/scsi/020/0_3`, the copying and
   conflict dialogs, the abort warning, the startup-disk question and the completion
   dialog.
5. `InstallerInitScript` — the `/bin/sh` post-copy script extracted from the BeFS volume on
   track 3 of the release disc (7,162 bytes, © Be Inc. 1997–1999): the closing
   `mountvolume -ro BeOS_Tools` line, the `BE_HOST_CPU = "ppc"` branch, and the R4-era
   upgrade cleanup (the `mergeres`/`mwbres`/`mwdres` queries, the Metrowerks stationery
   moves, `mkindex`, the `catattr`/`addattr` printer-driver rewrites, gated on `var/swap`).
6. The PC-side readme documents on the release disc (`Gobe/ReadMe.htm`,
   `PMAGIC/README.TXT`): the x86 paths — BeOS Personal Edition installed as a file under
   Windows, PartitionMagic Special Edition 2.02 (licensed by PowerQuest to Be) with its
   500 MB / 850 MB / 1.5 GB partition options, and `Gobe/Rawrite.exe` plus `floppy.img` for
   the boot floppy.
7. `BeOS_Launcher` — the Macintosh launcher application on the release disc, as characterized
   by string recovery from its resource fork: the requirement string "The BeOS requires a
   PowerPC 603 or 604 microprocessor", the driver strings ("MESH SCSI: id %.2x, regs %.8x
   int %d, dma-regs %.8x dma-int %d", "swim3 - cannot allocate request for I/O", "IDE MAC:
   openfirmware device %s doesn't have a 'reg' property", "nvram - unknown property size"),
   and the BFS and DBDMA diagnostic message bodies.
8. Observed runs of the release on Power Macintosh 9500 and 9500/180MP hardware
   configurations (604 processors, 64 MB, ATI Mach64 PCI display card, MESH SCSI disks,
   Mac OS 7.6): the Launcher handover, the read-only-volume Installer boot, the installation
   onto an unpartitioned SCSI disk, the reboot and the installed system's desktop; on the
   dual-processor configuration, the second processor's start and the "About BeOS" and Pulse
   processor reports.
9. `Bootscript` and `Bootscript.cd` — the system startup scripts in `beos/system/boot/` of the
   track-3 BFS volume: the `isvolume -readonly /boot` test and the CD script's
   app_server/registrar/Installer sequence, `shutdown -r` and `eject`.
10. BeOS 5.0.3 `beos/system/kernel_mac` and the bootstrap in `BeOS_Launcher`'s `Boot` resource,
    as disassembled: the multiprocessor start path and interrupt pair, the `mfspr`-based
    time-base reader, the bootstrap's Grand Central interrupt poll and its station-address
    comparison (register-level detail in pm9500mp.md [6]).
11. Motorola, Inc., *MPC603e & EC603e RISC Microprocessors User's Manual* (MPC603EUM/AD) —
    §2.3.5.1, `mftb` and `mfspr` treated identically.
