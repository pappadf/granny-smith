# MkLinux DR3 on the Power Macintosh

**Contents:**

1. [Scope & identity](#1-scope--identity) — what MkLinux is, the version matrix and why
   DR3, the release media, the supported-hardware matrix
2. [Boot architecture](#2-boot-architecture) — the Mac OS bootstrap and the MkLinux Booter;
   the kernel image and its entry sequence; the handoff's MMU contract; Mach startup, the
   Linux server and the console
3. [Hardware interface](#3-hardware-interface) — the PDM platform as MkLinux sees it; SCSI
   and the rz driver; video and the console; the ADB keyboard map; serial; the Cuda reboot
   contract
4. [Installation & bring-up](#4-installation--bring-up) — prerequisites; partitioning; the
   installer flow; the observed single-disk layout
5. [Observed behaviour & quirks](#5-observed-behaviour--quirks)
6. [Open questions](#6-open-questions)

References

---

## 1. Scope & identity

### 1.1 What MkLinux is

MkLinux is a Linux operating system for the Power Macintosh, developed by the Open Software
Foundation's research institute with Apple Computer: a Linux kernel compiled not as the
machine's own kernel but as a single *server* task running on the OSF Mach (osfmk)
microkernel, together with a Mach default pager for demand-paged memory [7]. The
composition ships as two source trees with different licenses — the Linux server and the
userland under the GPL, the osfmk microkernel under a Berkeley-style license — which is why
the release distributes the two separately and code cannot move freely between them [7].
The DR3 server identifies itself at a recorded root login as [8] (*observed*):

```
Linux localhost.localdomain 2.0.33-osfmach3 #1 Thu Jun 11 10:21:31 PDT 1998 ppc unknown
```

Where Apple's own [A/UX](aux.md) is a monolithic AT&T-lineage Unix for the 68030/68040
Macintoshes, loaded from HFS boot blocks by a Macintosh-side startup application
([A/UX](aux.md) §2), MkLinux is a microkernel composition that rides the Mac OS boot path
one layer higher: the machine boots Mac OS to the Finder, and a Mac OS control
panel/extension pair then loads the Mach kernel image from the HFS volume and hands the
machine over (§2). Both systems store their filesystems in `Apple_UNIX_SVR2` slices of the
standard Apple partition map ([A/UX](aux.md) §2.7; §4.2 below).

This page documents MkLinux Developer Release 3 (DR3, July 1998) on the NuBus Power
Macintosh machines of the PDM platform — the Power Macintosh 6100, 7100 and 8100, with the
[Power Macintosh 7100](../machines/pdm/pm7100.md) as the recorded subject. The evidence
base is the DR3 release itself [2] [3], the DR3 kernel sources released with the
distribution [6], the DR1-era Apple release notes and installation guide [4] [5], the
Mach kernel image as extracted and disassembled [1], and the recorded bring-up of DR3 on
a 7100 [8]. Claims are marked *observed* (with the evidence class) or
*inferred — unverified*; what the corpus does not establish is collected in
[§6](#6-open-questions). The evidence base is thin in places — above all, no annotated
reading of the released kernel's PDM device drivers exists yet — and the page is
deliberately leaner than its subject deserves, with the gaps made explicit.

### 1.2 The version matrix and why DR3

The release history, as recorded in the release-history investigation [7] (dates
corroborated by the release media [2] [4]):

| Release | Date | Kernel base | NuBus 7100-class suitability | Evidence |
|---|---|---|---|---|
| DR1 | May 1996 | Linux 1.2.13 server on Mach 3.0 | runs; primitive; no shared libraries, no RPM installer | [4] [5] [7] |
| DR2 / DR2.1 | Sep 1996 / Jan 1997 | Linux 2.0.x server | workable; still no shared libraries | [7] [9] |
| **DR3** | **July 1998** | **Linux 2.0.33 server on the osfmk Mach** | **the recommended target** | [2] [8] |
| R1 (MkLinux Developers Association) | 1999 | 2.2-era work | nominal; less NuBus testing | [7] |
| pre-R2 "Lazarus" / R2 RC1 "Phoenix" / RC2 / RC5 | 2002–2003 | newer kernels | NuBus instability reported; avoid for first bring-up | [7] (*reported, unverified*) |

DR3 is the version to target, for four recorded reasons [7]: it is the last Apple-produced
release; the NuBus 6100/7100/8100 were the original and best-tested platform for it; it is
the first release with dynamic shared libraries and a proper RPM-based installer (DR1/DR2
lack both and are much rougher); and it is the best-documented release — the release
carries its own installation guide [3], which is what lets an operator distinguish OS bugs
from hardware-behaviour gaps. DR2.1 (January 1997) is the useful fallback and bisection
point: simpler internally, no shared libraries, an older server [7] [9]. The community
releases after DR3 target newer kernels and userlands; period reports say the NuBus
machines became unstable with the later work-in-progress kernels, so they are poor
candidates for a first bring-up [7] (*reported, unverified* — the corpus holds no
first-hand evidence of the community releases).

### 1.3 The release media

The DR3 release is a CD image, 663,920,640 bytes, mastered 1998-07-27, distributed as a
hybrid HFS/ISO9660 volume and covered by a shipped checksum file the recorded media
verification matched [2] [8] (*observed*). Its on-media structure is unusual for a
Macintosh boot disc: block zero is zeros (no Driver Descriptor Map content), the HFS
signature sits at offset 1024 and the ISO9660 primary volume descriptor at 32768 [8]
(*observed*) — so the Macintosh ROM's SCSI boot scan finds no `'ER'` signature and loads
no disk driver from it; the disc is used by the running system, never as a ROM boot
device. The contents [2] (*observed*):

| Location | Contents |
|---|---|
| `Mac Files/` | `Mach Kernel` (1.28 MB), the `MkLinux Booter` (the Control Panel and the Extension), `MkLinux.prefs`, `lilo.conf` — the Mac OS-side boot chain (§2.2) |
| `MacOS Utilities/` | Drive Setup 1.5, Apple HD SC Setup 7.3.5, `pdisk` (with its HTML documentation), SimpleText |
| `RedHat/` | the binary RPM install tree the installer consumes |
| `SRPMS/` | 329 source RPMs — the release-exact source of the binaries |
| `mach_servers/` | `Mach_Kernel`, the `vmlinux` Linux server and the default-pager binaries |
| `extra/`, `mklinux_source/`, `misc_source/` | additional archives; the installer program's source is among them |
| `README` | README_DR3, the release's installation guide |

The DR1-era material in the corpus is the pair of Apple release documents — "MkLinux —
Read Me First" and "MkLinux Installation Notes" [4] [5] — which document the DR1
release's supported systems and known problems (cited throughout below).

### 1.4 The supported-hardware matrix

The DR1 release notes state the matrix explicitly: "This version of MkLinux has been
booted and run on the following types of systems: Power Macintosh 6100; Power Macintosh
7100/66, 7100/80; Power Macintosh 8100/80, 8100/100, 8100/110; PowerComputing 100 & 120"
— with the bounds "PowerPC 603/604 are not supported" and "PCI bus is currently
unsupported" [4] (*observed*, DR1 release notes).

DR3 widens both bounds. The single `Mach Kernel` image carries a 603/604 arm in its entry
code alongside the 601 arm [6] (*observed* in the released source), and the recorded
investigation's transcription of the release's machine list spans the whole PCI
generation — 7200–9600, the 54xx/64xx, and the G3 — in addition to the NuBus trio [8]
(*as transcribed; the list's exact wording is not in the corpus*,
[§6](#6-open-questions), item 1). The machines of this page are the NuBus set:

| Machine | CPU | Platform | Evidence |
|---|---|---|---|
| Power Macintosh 6100 | PowerPC 601 | PDM, NuBus | [4]; recorded DR3 boot to a root login [8] |
| Power Macintosh 7100/66, /80 | PowerPC 601 | PDM, NuBus | [4]; recorded DR3 prep, install and boot [8] |
| Power Macintosh 8100/80, /100, /110 | PowerPC 601 | PDM, NuBus | [4]; a recorded DR3 boot of the published disk image [8] |
| PowerComputing 100, 120 | PowerPC 601 | Mac OS-compatible clone | [4] only |
| PCI Power Macintosh (7200–9600, 54xx/64xx, G3) | 601/603/604 | PCI generation | DR3's machine list, as transcribed [8] |

The DR1 notes add per-subsystem bounds that still describe the architecture's edges:
only the on-board video and the HPV card are supported, NuBus video cards do not work,
and of ADB devices "only the standard Apple mouse and keyboards are specifically
supported" [4] (§3.3, §5.4). For the machines themselves — the 601, the PDM ASIC set, the
identity values — see [pdm.md](../machines/pdm/pdm.md) §1–§2 and
[pm7100.md](../machines/pdm/pm7100.md) §1; nothing on this page restates them.

## 2. Boot architecture

### 2.1 Two layers: Mac OS bootstrap under, Mach kernel over

MkLinux does not boot these machines from bare hardware, and it does not use their Open
Firmware as its boot path; the loader is Mac OS software [7]. The full chain, as recorded
on the 7100 [8], with the machine half cited rather than restated:

```
Power Macintosh 7100 reset and ROM boot: HWInit → nanokernel → 68k emulator
  → Start Manager, boot-device selection, SCSI drivers, HFS boot blocks
    (the machine's own four-phase boot contract — pdm.md §2.7, pm7100.md §5)
  → Mac OS Finder (System 7.5-era)
  → MkLinux Booter (Control Panel + Extension in the System Folder)
       splash screen: default OS choice, ten-second timeout
       loads 'Mach Kernel' from the HFS volume, passes boot arguments (lilo.conf)
  → Mach kernel entry (start.s): PVR dispatch, BAT setup, Mach bring-up (§2.3–§2.4)
  → the Linux server (vmlinux) and the default pager start
  → root mount: ext2 in an Apple_UNIX_SVR2 partition named by rootdev (§2.5)
  → init, login on the framebuffer console
```

Two consequences follow directly and are worth stating as architecture, not accident:

1. **A working Mac OS installation is a hard prerequisite.** The Booter, the kernel
   image and the preferences file all live in the Mac OS System Folder on an HFS volume;
   anything that stops the Mac OS boot stops MkLinux with it [7] (*observed* in the
   recorded installation, where every MkLinux step is reached through a Mac OS desktop).
   The DR1 release notes describe the same dependence four years earlier: the installer
   places "a Control Panel to control booting / not booting MkLinux", "an extension which
   is used to switch boot the Macintosh into MkLinux", and "the Mach Kernel file which
   takes over the booting process for bringing up MkLinux" [5].
2. **The machine spends every boot's first phase running the ordinary ROM and Mac OS
   path.** The 7100's ROM boot contract, the Start Manager's boot-device selection and
   the SCSI disk drivers the Booter depends on are
   [pdm.md](../machines/pdm/pdm.md) §2.7 and
   [pm7100.md](../machines/pdm/pm7100.md) §5, cited here and not restated.

The contrast with [A/UX](aux.md) §2 is instructive: A/UX's launcher is reached through
the HFS boot blocks' startup application, replacing the Finder; MkLinux's launcher is
reached *through* the Finder, and keeps a boot-time chooser between the two operating
systems (§2.2).

### 2.2 The MkLinux Booter

The Booter is the Mac OS-side loader, installed per the release's instructions as five
Finder copies from the CD's `Mac Files` directory [3] (*as recorded in the bring-up*,
[8]): `Mach Kernel` and the Booter Extension into the Extensions folder, the `MkLinux`
Control Panel into Control Panels, `lilo.conf` and `MkLinux.prefs` into Preferences.
There is no Mac OS-side installer application in DR3 — the Mac OS side is configured by
dragging these five files and then editing preferences; the DR1 release instead shipped a
Mac OS "Install MkLinux" application that placed the same three components (Control
Panel, extension, kernel) itself [5].

The Booter's recorded behaviour at boot [8] (*observed*):

- It presents a **splash screen at every boot**, with the default operating system and a
  **ten-second timeout**; the default and the timeout are configurable through the
  `MkLinux` Control Panel and `MkLinux.prefs`. Letting the timeout fire boots the default
  hands-free; clicking the other choice boots Mac OS and the machine reaches the Finder
  normally — the two systems coexist on one disk (§4.4).
- It loads `Mach Kernel` from the HFS volume it was installed on and passes the boot
  arguments from `lilo.conf` — a `lilo.conf`-style preferences file in which the kernel
  command line is set, including `rootdev` and console redirection [3] [7] [8]. The
  `lilo.conf` **as shipped on the CD points `rootdev` at the SCSI CD-ROM** (`/dev/scd0`),
  so a machine whose System Folder was just configured from the disc boots the *installer*
  on its first MkLinux boot [8] (*observed*); the installed system's `lilo.conf` names
  the root partition the installer reported (`/dev/sdb2` in the recorded two-disk
  layout, `/dev/sda6` in the recorded single-disk layout, §4.4).
- Console redirection to the serial line is a `lilo.conf` kernel-argument setting [7]
  (*recorded as a configuration fact; the exact syntax is not transcribed in the corpus*,
  [§6](#6-open-questions), item 7).

Alternative Mac OS-side loaders for the NuBus machines existed in the period (BootX,
miboot); period experience on these machines found the MkLinux Booter the most reliable,
and BootX 1.2.2 is reported to crash on NuBus [7] (*reported, unverified*). The corpus
holds no analysis of the Booter application itself — how it loads the kernel image, what
Mac OS services it uses, or what state it leaves the machine in at the jump
([§6](#6-open-questions), item 2).

### 2.3 The kernel image: MACH_BOOT_IMAGE and the ELF inside

`Mach Kernel` is not a raw binary: it is a 32-byte header — the ASCII tag
`MACH_BOOT_IMAGE` followed by a length — in front of a plain **ELF32 big-endian
PowerPC executable** [1] (*observed*; the same image is also present as
`mach_servers/Mach_Kernel` [2]). The executable is stripped; its recorded header values
[1] (*observed*):

| Property | Value |
|---|---|
| Entry point | `$0025E2D0` |
| `.text` linked address | `$00200000` |
| `.data` linked address | `$00400000` |
| File size | 1.28 MB [2] |
| Symbols | stripped (the released source is the symbol map) [1] [6] |

The entry sequence is the released source's `start.s` (the osfmk mach-kernel tree's
PowerPC entry code) [6], and the extracted ELF lines up with it
instruction for instruction at the recorded addresses [1] (*observed*). Its opening, as
the source and the disassembly agree [1] [6]:

```
ENTRY(_start)
    mfpvr   r10
    rlwinm  r10,r10,16,16,31      ; isolate the PVR version field
    cmpi    0,r10,1               ; version 1 = the 601
    bne     1f                    ; otherwise: the 603/604 arm
        ; 601 arm: BAT0 := BLPI=0, WIM=0, Ks=Ku=0, PP=2 (BATU $00000002)
        ;                     BRPN=0, V=1, 8 MB block        (BATL $0000007F)
        b   2f
1:      ; 603/604 arm: [I/D]BAT0 maps the bottom 32 MB
2:  li  r0, 0
    isync
    sync
    isync
    mtibatu 0, r7                 ; at $0025E340
    mtibatl 0, r8                 ; at $0025E344
    ... eight BAT writes, then sync; isync
```

The recorded machine's PVR reads `$00010001` — version 1, the 601 [8] (*observed*) — so
the 601 arm runs: BAT0 is set to BATU `$00000002` and BATL `$0000007F`, which maps
effective addresses `$00000000`–`$007FFFFF` one-to-one onto physical addresses zero
through 8 MB [6] [10] — the region the kernel image has just been loaded into, `.text` at
`$00200000` and entry at `$0025E2D0` [1]. That the kernel begins by re-establishing its
own 1:1 low mapping before anything else says plainly what the Booter hands it: a
machine whose address space is still the Mac OS loader's, on which the kernel trusts
nothing below its own BAT writes.

### 2.4 The handoff's MMU contract

The transition from Mac OS to the Mach kernel is the timing-sensitive point of the boot,
and the recorded investigation pinned both halves of the contract on it [8]
(*observed*):

1. **The loader's live BAT state is part of the interface.** At the kernel entry the
   loader has left BAT0's lower half at `$0280007F` — BRPN `$02800000`, valid, an 8 MB
   block. A faithful 601 must present exactly this value to the kernel's first reads of
   the BAT file, whatever the loader's mapping was for.
2. **The kernel writes its BATs in half-pairs and relies on the architecture's
   context-synchronization rules** [6] [10]: the eight writes in §2.3 are bracketed by
   `sync`/`isync` sequences with nothing between them, and the PowerPC 601 does not
   apply a BAT write to instruction fetches or translations that precede a
   context-synchronizing event [10]. The recorded boot shows why the kernel is entitled
   to rely on it: between `mtibatu 0` and `mtibatl 0` the register pair is in a
   **half-written state** — the fresh BATU (`$00000002`, BLPI = 0) combined with the
   loader's stale BATL (`$0280007F`, BRPN = `$02800000`) describes a mapping of
   effective `$00000000`–`$007FFFFF` to physical `$02800000`, one byte past the end of
   the recorded machine's 40 MB of RAM [8]. A fetch that observed the half-written pair
   would translate `$0025E344` to `$02A5E344`, read uninitialised memory (the recorded
   bytes are the `$AA` pattern), and fall into an exception storm through the Mac OS
   exception vectors, which are already torn down [8]. The architecture's rule — no
   effect until the context-synchronizing instruction, and the writes are fetched and
   translated before `mtibatu 0` retires — is precisely what makes the sequence safe
   [10]; the recorded boot completes the handoff when the rule is honoured [8].

### 2.5 Mach startup: the Linux server, the default pager, the console

Once the Mach kernel is established, it starts the Linux server (`vmlinux`) and the
default pager [7] — the three-process composition of §1.1. What the corpus establishes
about this phase is the observable end state rather than the sequence: the recorded
boot mounts its root filesystem from the ext2 volume in the `Apple_UNIX_SVR2` partition
named by `lilo.conf`'s `rootdev` (`/dev/sda6` in the recorded layout), runs `init`, and
reaches a root login on the framebuffer console, driven by the ADB keyboard [8]
(*observed*). The `uname` string of §1.1 is that session's. The default pager's swap
protocol on the `Apple_UNIX_SVR2` slice is not traced ([§6](#6-open-questions), item 4).

The boot console is the framebuffer; the kernel's diagnostics can instead be redirected
to the serial line (`ttya`, the modem port) through the Booter's kernel arguments, which
is the recommended window while the video console is still unproven [7] (*recorded
configuration advice, not a traced boot*). The DR1 release notes describe the same
console model from the other end: the video console is "temporarily considered to be a
`vt100` terminal", `/etc/termcap` is modified accordingly, and the operator is told to
set the row count to match the screen size (`stty rows 30` for a 640×480 Apple 13-inch
display) [4].

## 3. Hardware interface

### 3.1 The PDM platform as MkLinux sees it

The NuBus machines MkLinux DR3 runs on are the PDM platform; the parts the OS drives,
with the pages that document them:

| Subsystem | Part | Reference |
|---|---|---|
| CPU | PowerPC 601, PVR version 1 (observed `$00010001` at the kernel entry [8]) | [pdm.md](../machines/pdm/pdm.md) §2.2; [10] |
| Memory controller | HMC (the 35-bit serial configuration, bank placement) | [hmc.md](../machines/pdm/hmc.md) §2, §4 |
| I/O controller | AMIC — the pseudo-VIA1/VIA2 banks, the interrupt and DMA register files | [amic.md](../machines/pdm/amic.md) §2.2–§2.5 |
| SCSI | the Curio's 53C94-class cell, one channel on the 7100 | [ncr-53c96.md](../hardware/scsi/ncr-53c96.md) §1.3; [pm7100.md](../machines/pdm/pm7100.md) §3.5 |
| Serial | the SCC channels behind AMIC | [amic.md](../machines/pdm/amic.md) §3.5 |
| Video | on-board RAM-framebuffer video through Ariel II and AMIC | [ariel.md](../machines/pdm/ariel.md) §1.4, §3.2 |
| Input | ADB keyboard and mouse through the Cuda transport | [pdm.md](../machines/pdm/pdm.md) §4.7 |
| Floppy | SWIM III | [swim3.md](../machines/pdm/swim3.md) — no DR3 evidence ([§6](#6-open-questions), item 6) |

The corpus establishes the interfaces below and little else at driver level. The released
kernel's PDM drivers — their AMIC DMA register idioms, their SCC interrupt
configuration, their use of the 601's RTC and decrementer for timekeeping, the HPV card
driver — are in the released source [6] but no annotated reading of them exists in the
evidence base; those facts are open questions (items 3–4 of [§6](#6-open-questions)),
not silently assumed. What is recorded is that the OS drives this platform very
differently from Mac OS — its own SCSI and serial DMA programming, its own interrupt
dispatch, 601 timekeeping — so behaviour proven under Mac OS does not transfer
automatically [7].

### 3.2 SCSI: the rz driver, device naming, and the geometry probe

The kernel's SCSI disk driver is the `rz` driver of the released source [6]. Its
user-visible contract with the bus and the partition map [4] [8]:

- **Device naming.** Disks are `/dev/sdX` where the letter `a`–`g` names SCSI ID 0–6, and
  the trailing number names the partition: "the MkLinux device file /dev/sde6
  corresponds to SCSI disk ID 4, partition 6" [4]. The partition number names the
  **Apple partition-map entry, counted from 1** — in the recorded single-disk layout the
  root is partition-map entry 6 and is named `/dev/sda6` [8] (*observed*). The DR1 notes
  warn that some Apple utilities count partitions from zero, so "Partition 3" as shown
  by Apple HD SC Setup's Details command is partition 4 under MkLinux [4].
- **Device classes.** Only SCSI hard disks and CD-ROMs are supported in DR1; removable
  "personal storage" devices are untested, and "shadow drives are not supported" [4].
- **Dual-bus machines.** On systems with two SCSI buses (the DR1 notes name the 8100/100
  class), the two buses' ID maps are "logically merged into one"; devices sharing an ID
  across buses are not supported, and only the external bus's ID is accessible [4]. DR3
  on a dual-bus machine is unverified ([§6](#6-open-questions), item 11).
- **The geometry probe.** The `rz` driver's `get_phys_parms()` asks the target for MODE
  SENSE pages 3 (device-format) and 4 (rigid-geometry) and reads the page area at a
  fixed offset past the mode header [6] (*observed* in the released source). The
  recorded boot characterizes the contract from both sides [8] (*observed*): a target
  that answers MODE SENSE with GOOD status and **no page area** leaves the driver to
  parse whatever its buffer still holds — the driver's own previous INQUIRY data — and
  it then prints a nonsense geometry, in the recorded instance
  `rz1: mode_sense says 14384 bytes 12592 secs, 2 cyls, 0 heads, total 0`, where 14384
  and 12592 are the ASCII digits "80" and "10" read out of the INQUIRY leftovers. A
  target that returns the page data is answered with the ordinary probe result and the
  driver's platter geometry reconciles with READ CAPACITY. A disk model serving these
  machines must therefore implement pages 3 and 4, not merely status.
- **The chip side.** The 53C94-class cell's programming on PDM — reset, initialization
  sequence, DMA data phases — is
  [ncr-53c96.md §4.3, §4.7](../hardware/scsi/ncr-53c96.md), cited not restated; the
  AMIC DMA channel that feeds it is [amic.md](../machines/pdm/amic.md) §3.4.

### 3.3 Video and the console

The DR1 bounds still describe the architecture: "Only the Power Macintosh on-board video
and HPV card are currently supported; nubus video cards will not work in this release",
with depth limits — 32 bpp unsupported, 16 bpp supported but with colormap-adjustment
problems in X11 — and slow console scrolling, for which 1 bpp is recommended [4]. For
the 7100, "on-board video" is the Ariel II/AMIC framebuffer channel
([ariel.md §3.2](../machines/pdm/ariel.md)) and the HPV card is the PDS video card every
shipped 7100 carries ([pm7100.md](../machines/pdm/pm7100.md) §4.3).

The recorded DR3 bring-up exercises the on-board path only: the Mach kernel's PDM video
console and the installer's full-screen UI come up on the on-board video with no serial
assistance, and the ADB keyboard drives them [8] (*observed*). That is consistent with
the platform's shape: the on-board framebuffer is the same Sonora/Ariel-class video the
6100 carries ([ariel.md §1.4](../machines/pdm/ariel.md)), and the 6100 was the platform's
most common machine. Period reports say HPV framebuffer support was spottier than the
AV card's [7] (*reported, unverified*); the recorded bring-up did not test a PDS card.
One platform-level dependency is recorded and unexplained: the console's video mode
depends on the machine's RAM configuration (the PDM framebuffer is carved from main
memory, [ariel.md §4.3](../machines/pdm/ariel.md)) — a recorded 6100 boot at 40 MB
painted the console into a mode that reached a login prompt on a mode nothing displayed,
while the same disk at the machine's 24 MB default booted to a visible login every
time [8] (*observed*; the mechanism is [§6](#6-open-questions), item 13).

### 3.4 The ADB keyboard map

The Linux server's console keymap, from the released platform headers
(ppc/POWERMAC/keyboard.h) [6] (*observed*):

| Function | ADB keycode | Function | ADB keycode |
|---|---|---|---|
| Left / Right / Up / Down | `$3B` / `$3C` / `$3E` / `$3D` | Right Shift | `$7B` |
| — | — | Right Option | `$7C` |
| — | — | Right Control | `$7D` |

The right-hand modifiers `$7B`/`$7C`/`$7D` are intercepted as modifiers **before** the
keymap table is consulted, and the keymap's `$3B`–`$3E` entries carry the vi-style
letters `h l j k` [6] — dead code for the same reason. The consequence is §5.2.

### 3.5 Serial

The serial ports are the SCC channels behind AMIC ([amic.md](../machines/pdm/amic.md) §3.5);
the console redirection of §2.5 targets `ttya`, the modem port [7]. The DR1 notes'
console model (`vt100` on the console, `/etc/termcap` modified, `stty -onlcr` for console
sessions) implies a serial login path of the same shape [4]; the recorded DR3 flow used
the framebuffer console throughout [8], and the serial path is recorded as a
configuration option, not a traced boot ([§6](#6-open-questions), item 7).

### 3.6 Reset and power: the Cuda reboot contract

The OS's reboot path issues the Cuda microcontroller's RESET_SYSTEM pseudo-command
(`$11`) over the Cuda transport — the register-emulated VIA1 handshake of
[pdm.md](../machines/pdm/pdm.md) §2.8 — and **expects never to regain control**: the
released source's `powermac_reboot()` panics if the call returns [6] (*observed* in the
released source). The recorded DR3 post-install reboot prints
`panic: system should have restarted` after cleanly unmounting its filesystems [8]
(*observed*) — the unmount order is the OS's own safety property, and the panic is the
documented reaction to a Cuda that accepts the command without power-cycling the
machine. The contract, stated for a re-implementer: RESET_SYSTEM via the Cuda transport
must restart the machine, and a return from it is a fatal condition the OS reports, not
one it tolerates.

## 4. Installation & bring-up

### 4.1 Prerequisites

| Requirement | Value | Evidence |
|---|---|---|
| Mac OS | a working installation — the Booter, kernel and preferences live in its System Folder; System 7.5-era in the recorded flow | [3] [7] [8] (*observed*, System 7.5) |
| RAM | at least 16 MB (DR1 stated); the recorded DR3 installation ran at 32 MB; 32–72 MB is the recorded realistic range for these machines | [4] [7] [8] |
| Disk | at least 400 MB available (DR1 stated); a dedicated drive recommended | [4] [5] |
| Root placement | the root partition within the first 2 GB of the disk | [3] (*as transcribed in [8]*) |
| CD-ROM | a SCSI CD-ROM carrying the release disc, for the installer | [2] [8] |
| Target layout | `Apple_UNIX_SVR2` root + swap partitions on the target disk | [3] [4] [5] |

No Apple document in the corpus states a DR3-specific minimum; the DR1 figures are the
documented ones and the recorded DR3 flow's values sit above them.

### 4.2 Partitioning

MkLinux stores its filesystems in slices of the standard Apple partition map, using the
`Apple_UNIX_SVR2` ("A/UX") partition type — "the same basic partitioning structure that
was used for Apple's A/UX product" [5]; the partition-type and slice model is
[A/UX](aux.md) §2.7. The documented DR1 layout [5]:

```
+-----------------+
|  Partition Map  |
+-----------------+
|     Driver      |
+-----------------+
|  Root (slice 0) |   100-300 MB (300 MB minimum without a separate usr)
+-----------------+
|  Usr (slice 2)  |   optional, 200+ MB recommended
+-----------------+
|  MacOS Disk     |   optional
+-----------------+
| Swap (slice 1)  |   32-64 MB
+-----------------+
```

The documented limits and rules:

- **Partition count.** Only 8 partitions of any type (HFS, MkLinux, driver partitions)
  are supported; partitions numbered 9 and above cannot be used — and since "the
  Macintosh reserves several partitions for itself, this realistically limits you to 4
  or perhaps 5 user partitions" [4].
- **Swap.** 32–64 MB in DR1, with a 64 MB per-partition maximum ("compatibility with
  Linux 1.2.13") [4] [5]; the recorded DR3 installation used a 128 MB swap partition
  without incident [8] (*observed*; the DR3 documented maximum is not in the corpus,
  [§6](#6-open-questions), item 8). The installer keys swap off the partition *name*
  "swap" [3] (*as transcribed in [8]*).
- **Filesystem.** ext2; the DR1 notes recommend a 1 KB block size ("currently there are
  problems using 4K block-size filesystems") [4]; what DR3's installer lays down is
  unrecorded ([§6](#6-open-questions), item 5).
- **Tools.** Drive Setup *cannot* create A/UX-type partitions [3] (*as transcribed in
  [8]*); Apple HD SC Setup can ("you may use Apple HD SC Setup or any 3rd-party disk
  partitioner that can create 'A/UX' type partitions") [4], and the DR3 disc ships both
  it (7.3.5) and `pdisk` [2]. `pdisk` is the recorded tool of practice [8].

The recorded `pdisk` session on a blank disk at SCSI ID 1, driven from the CD's Mac OS
`pdisk` [8] (*observed*), producing a root-and-swap target:

```
e /dev/scsi0.1      ; edit the disk pdisk itself labels /dev/sdb
i                   ; initialize the partition map
c 2p 900m root      ; partition 2, root
c 3p 128m swap      ; partition 3, swap
w
y
```

`/dev/scsi<bus>.<id>` is `pdisk`'s own raw-device naming, distinct from MkLinux's
`/dev/sdX` [8]. A disk carrying no HFS partition needs no driver partition [3]
(*as transcribed in [8]*); the recorded two-disk target carries only the partition map
plus root and swap, and installs cleanly in that state [8].

### 4.3 The installer flow

DR3's installer is Red Hat-derived, runs under the MkLinux system booted from the CD
(the shipped `lilo.conf`'s `rootdev=/dev/scd0`, §2.2), and is driven entirely by the
keyboard. The recorded screen sequence [8] (*observed*):

1. Welcome.
2. Keyboard selection (`us` in the recorded flow).
3. Installation method: **Local CD-ROM**.
4. Installation path: **Install**.
5. **Partition Disks** — the screen that shells out to the partitioner; on a properly
   pre-partitioned disk this step is a pass-through, on a virgin disk it is the
   failure of §5.1.
6. **Select Root Partition** — in the recorded two-disk flow it offers exactly one
   candidate, `/dev/sdb2 64 1843263 921600k`, and correctly withholds the partition
   named `swap` [8].
7. Mounts (none extra in the recorded flow), **Active Swap**, **Format** (select all).
8. **Components** — the recorded flow installed the default set minus the X Window
   System: **224 packages, 183 MB** [8] (*observed*); the release's own "Absolute
   Minimum" selection is 58 MB across all mount points [3] (*as transcribed in [8]*).
   The package-install phase is the long one — tens of minutes on a real 7100 [7].
9. Mouse: **ADB**; Network: none in the recorded flow; Timezone (with the "hardware
   clock set to GMT" option); Services defaults; Printer: none.
10. Root password; **Info** — the screen that reports the root device to set
    (`Set rootdev equal to /dev/sda6` in the recorded single-disk flow); **Done**.
11. Back in Mac OS, the `MkLinux` Control Panel's **Custom…** opens `lilo.conf` in
    SimpleText; the operator sets `rootdev` to the reported value and the Startup
    Selection to MkLinux [3] [8] (*observed*).

One installer screen deserves its own warning, recorded the hard way: between the
method choice and the Installation Path the installer presents
"**Note: Insert your Red Hat CD into your CD drive now**" — a screen an operator
scripting the flow by key presses can easily not account for; every key pressed after it
lands one screen late and the sequence stalls on Partition Disks [8] (*observed*).

### 4.4 The observed single-disk layout

The recorded end state of the bring-up is one disk holding both operating systems
[8] (*observed*) — System 7.5 and MkLinux DR3 on one Apple partition map, booting to
either, with no operator input needed to reach a root login:

| # | Name | Type | Start | Size (blocks) |
|---|---|---|---|---|
| 1 | Apple | `Apple_partition_map` | 1 | 63 |
| 2 | Macintosh | `Apple_Driver43` | 64 | 54 |
| 3 | Macintosh | `Apple_Driver43` | 118 | 74 |
| 4 | Patch Partition | `Apple_Patches` | 192 | 512 |
| 5 | untitled | `Apple_HFS` | 704 | 81,920 (40.0 MB) — System 7.5 |
| 6 | root | `Apple_UNIX_SVR2` | 82,624 | 196,608 (96.0 MB) — ext2, `/` |
| 7 | swap | `Apple_UNIX_SVR2` | 279,232 | 32,768 (16.0 MB) |
| 8 | Extra | `Apple_Free` | 312,000 | 34,230 (16.7 MB) |

The disk was laid out by **Drive Setup 1.5 first, `pdisk` second**: Drive Setup's Custom
Setup initializes the disk with a 40 MB HFS volume and — the property that makes the
one-disk layout possible — is willing to leave the remainder of the drive *unallocated*
("Type: Unallocated", 129 MB in the recorded session), which `pdisk` then fills with the
root and swap partitions [8] (*observed*). The installed system reports
`/dev/sda6  93M size, 54M used, 61% capacity` for its root [8] (*observed*) — the
installed tree is 54 MB, consistent with the release's 58 MB "Absolute Minimum" figure.
The same disk boots on the other PDM machines unchanged — the recorded boots of the 6100
and 8100 use the same image and reach the same login [8] (*observed*) — which is the
strongest statement the corpus makes about the OS's machine independence within the
platform: nothing in the installed tree is machine-specific, and the per-machine
differences live in the one universal kernel's drivers.

## 5. Observed behaviour & quirks

### 5.1 The installer cannot partition a virgin disk

The installer's Partition Disks → Edit path is unusable on a disk that has never been
partitioned, and the reason is in the released installer source [6] (*observed*; the
recorded bring-up established the live symptom [8]): `partitionDrives()` declares its
partitioner path variable without an initialiser, then chooses the partitioner from two
probes of the raw device — the fdisk signature `$AA55` at offset 510 and the Apple
partition map's `'ER'` signature at block zero — and `execl`s the chosen command. A
virgin disk carries neither signature, so **no arm of the chooser assigns the variable
at all** and the `execl` runs an uninitialised pointer. The recorded symptom: pressing
Edit prints `pdisk`'s two-line banner and then drops straight to "You cancelled step
'Setup filesystems'" — the partitioner never prompts [8] (*observed*). The sibling
function `getDiskPartitions()` in the same file does not have the defect: it
initialises the command before the chain [6]. The consequence for any bring-up: the
target disk must arrive at the installer already carrying an Apple partition map, laid
out from Mac OS (§4.2).

### 5.2 The Extended-Keyboard arrow keys do not work

The console keymap of §3.4 puts the arrows at `$3B`–`$3E`; Mac OS (and the Macintosh
Extended Keyboard) puts the down-arrow at `$7D`. In the MkLinux console the keycode
`$7D` is intercepted as right-Control before the keymap is consulted [6], so an input
path that types a down-arrow by the Mac OS convention silently taps right-Control and
moves nothing [8] (*observed*). The workaround the recorded bring-up used is to send
`$3D`/`$3E` directly for down/up while the console is on screen; Return, Tab and Space
carry the same keycode in both worlds [8]. The same trap presumably awaits any
software ported to the console without re-reading the platform headers.

### 5.3 The `MkLinux.prefs` splash and the hands-free boot

The Booter's splash (§2.2) names the kernel's root device in the recorded flow
(`/dev/sda6` in the single-disk layout) — a useful single-screen check that the
configuration took [8] (*observed*). With the Startup Selection set to MkLinux, the
ten-second timeout carries the machine into the kernel with no input at all; with it
set to Mac OS the same disk boots to the Finder, and the control panel's setting is the
only switch between them [8] (*observed*).

### 5.4 DR1-era documented limitations

The DR1 release notes' known-problems list [4] — DR1 values, given here because they
bound the architecture and because the DR3 equivalents are mostly unrecorded
([§6](#6-open-questions), items 5–6, 12):

| Area | DR1 limitation |
|---|---|
| Video | on-board and HPV only; NuBus video cards do not work; 32 bpp unsupported; 16 bpp supported with colormap problems in X11; slow console scrolling (1 bpp recommended) |
| Console type | the console is treated as `vt100`; `/etc/termcap` modified, `stty -onlcr` set for console sessions; the operator sets the terminal's row count manually |
| PCI | unsupported |
| SCSI | hard disks and CD-ROMs only; removables untested; shadow drives unsupported; dual-bus machines merge the two ID maps and expose only the external bus's ID |
| ADB | only the standard Apple mouse and keyboards specifically supported; trackballs "may not work" |
| Partitions | 8 of any type; 64 MB per swap partition |
| Floppy | "no floppy disk access support available at this time" |
| HFS access | a tool set (`hmount`, `hdir`, `hcd`, `hcopy`) mounts one Macintosh volume at a time for file exchange |
| Scheduling | task priority changes not implemented; `nice` ineffective; all tasks under Mach scheduling |
| Swap | the kernel honours requests beyond available swap; "Paging Space over-committed" is generally benign; pages that get used after over-commit crash the system |
| Versions | utilities rev 1.3.54, kernel rev 1.2.13 |

### 5.5 Small observed behaviours

- The installed system's default hostname is `localhost.localdomain` — from the recorded
  `uname` line, §1.1 [8].
- The installer's Select Root Partition screen withholds any partition named `swap`
  from the root candidates [8] (*observed*).
- `pdisk` running under Mac OS draws its console in a SIOUX window whose insertion
  point blinks continuously [8] (*observed*) — a Mac OS-side behaviour, worth knowing
  when driving the tool from a screen-diff harness.
- The five Mac Files are the whole of the Mac OS-side configuration: copying a blessed
  System Folder between volumes carries the MkLinux configuration with it [8]
  (*observed*; the Finder blesses the copied folder on arrival).
- The DR3 disc mounts under the running Mac OS through the ordinary CD-ROM and
  ISO 9660 file-access extensions [8] (*observed*) — the DDM-less hybrid of §1.3 needs
  no special handling on the Mac OS side.

## 6. Open questions

1. **README_DR3's supported-machine list, verbatim.** The recorded investigation
   transcribes it as the whole PCI generation plus the NuBus trio [8]; the list's exact
   text, and whether any machine of either generation is excluded or footnoted, is not
   in the corpus.
2. **The Booter's internal protocol.** How the Booter loads `Mach Kernel` (which Mac OS
   services, which load addresses), what argument block it passes, what it does to the
   Mac OS and nanokernel state before the jump, and how the `MACH_BOOT_IMAGE` header is
   consumed are all untraced; the Booter application itself is not disassembled.
3. **The released kernel's PDM drivers at register level.** The AMIC DMA programming
   idioms, the SCC interrupt-mode configuration, the pseudo-VIA interrupt service, the
   601 RTC/decrementer timekeeping path and the HPV driver are all in the released
   source [6] but no annotated reading of them exists in the evidence base; this page
   records the interfaces it observed and no more.
4. **The default pager's swap protocol** on an `Apple_UNIX_SVR2` slice — format,
   header, and how `lilo.conf`'s swap activation maps to it.
5. **The ext2 block size DR3 installs.** DR1 recommends 1 KB and warns against 4 KB [4];
   what DR3's installer lays down is unrecorded.
6. **Floppy support in DR3.** DR1 has none [4]; whether DR3's kernel drives SWIM III is
   not established by the corpus.
7. **The serial console's exact syntax.** The console redirection to `ttya` is recorded
   as a `lilo.conf` kernel-argument setting [7]; the argument's form is not transcribed,
   and no serial-console boot is recorded.
8. **DR3's documented swap maximum.** The recorded install used 128 MB [8]; the DR1 cap
   is 64 MB [4]; whether DR3's release documents a cap is not in the corpus.
9. **The `Mach Kernel` symbol map.** The ELF is stripped [1]; the released source stands
   in for symbols, but no symbol extraction from the shipped binary has been done, so
   addresses in the shipped image map to source only by the recorded disassembly of the
   entry sequence.
10. **DR2.1's internal differences.** Its value as a bisection point against DR3 is
    recorded [7], but the corpus does not characterize what differs (server version,
    driver set, installer).
11. **Dual-bus SCSI under DR3.** The DR1 merge semantics (§3.2) on an 8100-class
    machine with two buses is not re-verified for DR3.
12. **NuBus video under DR3.** DR1 excludes NuBus video cards outright [4]; DR3's
    support for them is unknown.
13. **The console's RAM dependence.** The recorded 6100 behaviour of §3.3 — a visible
    login at 24 MB, an invisible console mode at 40 MB — is observed, not explained;
    which component picks the mode and why the framebuffer decision differs with RAM
    size is not traced.
14. **The community releases' behaviour on NuBus machines.** The reported instability of
    the post-DR3 kernels on NuBus [7] is period report only; the corpus holds no
    first-hand evidence of any community release, on any machine.

## References

1. MkLinux DR3 `Mach Kernel` image, extracted and analyzed: the 32-byte `MACH_BOOT_IMAGE`
   header, the embedded ELF32 big-endian PowerPC executable (entry `$0025E2D0`, `.text`
   `$00200000`, `.data` `$00400000`, stripped), and the annotated disassembly of the
   `start.s` entry sequence with the PVR dispatch and BAT writes.
2. MkLinux Developer Release 3 (DR3), OSF Research Institute and Apple Computer, July
   1998 — the distribution CD (hybrid HFS/ISO9660 image, 663,920,640 bytes, mastered
   1998-07-27, with its shipped checksum file): the on-media structure and the contents
   of `Mac Files/`, `MacOS Utilities/`, `RedHat/`, `SRPMS/` (329 source RPMs),
   `mach_servers/` and the source archives.
3. README_DR3 — the installation guide carried by the MkLinux DR3 release: the Mac
   Files placement, the partitioning rules (A/UX-type partitions, the 2 GB root rule,
   the no-driver-partition note, swap by partition name), Drive Setup's limits and the
   installer flow.
4. Apple Computer, Inc., "MkLinux — Read Me First", MkLinux DR1 release notes, May 1996
   — the supported-systems list, the known problems (video, PCI, SCSI, ADB, partitions,
   floppy, swap, scheduling) and the release's device-naming and HFS-tool notes.
5. Apple Computer, Inc., "MkLinux Installation Notes", MkLinux DR1 installation guide,
   May 1996 — RAM and disk requirements, the A/UX partition structure, the
   root/usr/swap layout diagram, the Mac OS-side installer's three components.
6. MkLinux DR3 kernel sources, released with the distribution (the osfmk Mach-microkernel
   and mklinux Linux-server trees, GPL and Berkeley-style): the osfmk mach-kernel tree's
   PowerPC entry code (`start.s`); the ppc/POWERMAC platform headers (`keyboard.h`); the
   installer program source (`install/hd.c`,
   `partitionDrives()`/`getDiskPartitions()`); the `rz` SCSI
   driver sources (`rz_labels.c`, `get_phys_parms()`, and the reboot path's
   `powermac_reboot()`).
7. The recorded MkLinux release-history and media-provenance investigation: the release
   matrix (DR1 through the community R2 releases, with dates), the media checksums and
   mirrors, the recorded boot-architecture investigation for the NuBus machines, and the period
   reports it collects (community-release NuBus stability, alternative loaders, the
   HPV/AV console quality).
8. The recorded MkLinux DR3 bring-up on the Power Macintosh 7100: the prep, install and
   boot traces — the partition maps and the `pdisk` sessions, the Booter splash and
   `lilo.conf` edits, the installer screen sequence including the Red Hat CD note and
   the recorded package set, the single-disk layout and its `df` output, the recorded
   boots on the 6100 and 8100 from the same image, the live readings at the kernel
   handoff (PVR, BAT state, the rz probe messages, the Cuda reboot panic), and the
   transcription of the release's machine list and README_DR3 rules.
9. MkLinux Developer Release 2.1 (DR2.1), Apple Computer and OSF, January 1997 — the
   fallback release, held as the bisection point against DR3.
10. Motorola, Inc. and International Business Machines Corporation, *PowerPC 601 RISC
    Microprocessor User's Manual* (MPC601UM/D), 1995 — the 601's PVR identification, the
    BAT register model and the context-synchronization requirement a BAT change depends
    on.
