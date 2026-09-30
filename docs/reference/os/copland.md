# Copland (Mac OS 8 Developer Release D11E4)

**Contents:**

1. [Scope & identity](#1-scope--identity) — what Copland is, the evidenced build and media, the
   supported-hardware matrix, the two-boot volume design
2. [Boot architecture](#2-boot-architecture) — the loader chain; the rewritten boot blocks and the
   Caps Lock gate; the disk-based Open Firmware; the SecondaryLoader Forth; the tertiary loader;
   the kernel handoff; the non-fatal claim
3. [Hardware interface](#3-hardware-interface) — families, plugins and per-machine driver sets;
   demand paging of code sections; the exception and panic contract; the NuBus probes; the SWIM3
   floppy driver; ADB and Cuda input; the relocated framebuffer; time and file dates; serial
4. [Installation & bring-up](#4-installation--bring-up) — the DDK media set; the installer rules;
   Apple's install flow; what the installer writes; the drive gate; the recorded bring-up
5. [Observed behaviour & quirks](#5-observed-behaviour--quirks)
6. [Open questions](#6-open-questions)

References

---

## 1. Scope & identity

### 1.1 What Copland is

Copland is Apple's never-shipped next-generation Macintosh operating system, developed to be
released as Mac OS 8: a microkernel-based system with protected address spaces and preemptive
tasking, intended to replace the cooperative System 7 line while running existing applications
through a compatibility environment. Apple cancelled the project in August 1996 before any
customer release; the "Mac OS 8" that shipped in July 1997 was the legacy Mac OS restyled with
Copland-derived appearance work [14] (*historical context, not part of the technical evidence
base*). What exists as runnable software are the developer releases Apple seeded under
non-disclosure — of which the corpus here holds one: **Mac OS 8 Developer Release D11E4**, dated
June 1996, distributed as the installable payload of the **Mac OS 8 Driver Development Kit
v0.4** (DDK 0.4) disc [3] [5].

The system's architecture, in Apple's own published words from the DDK read-me: "Most drivers
will no longer interface with the Device Manager" — a driver belongs to one of the system's
*families* and is written as a plug-in; "applications reside in a different address space than
drivers do. They can not interact with hardware directly"; and "PowerPC machines have only one
interrupt line; unlike 680x0 machines that had seven", so interrupt handlers must be minimal [3].
The DDK read-me positions the release: the Driver Development Kit v0.4 is "the third of several
planned driver developer releases", preliminary, oriented towards ADB, Block Storage, Graphics,
Input Device, Open Transport and SCSI driver developers, with documentation that lags the code
(the Keyboard and Pointing Device families had just been merged into one User Input Device
Family, which the documentation did not yet reflect) [3]. The release notes call the kernel
itself "functionally complete" and state that error codes will change after the first developer
release [2].

This page documents D11E4 as a guest of the Power Macintosh 7100/66, from four evidence classes:
Apple's own DDK-era documents [1] [2] [3] [4]; the DDK 0.4 disc specimen [5]; recorded
installation and boot observations, including a block-by-block installation differential [6],
a recorded end-to-end bring-up of the loader chain and kernel [7], and recorded reverse
engineering of the floppy driver [8], the ADB/Cuda input stack [9] and the file-date path [10];
and recovered disassembly material — the tertiary loader with its 345 traceback-recovered
function names and call sequence [11], the installer's rule resources [12], and the
Apple-branded-drive investigation [13]. D11E4 is a developer build, and everything it ships
carries PowerPC traceback tables, so its functions are legible under their real names [7].
There is no source code for any of this: the loader chain is Forth plus raw PowerPC, the Open
Firmware image is stripped XCOFF, and every statement below about internals comes from the
binaries' instructions, registers and bytes on disk, marked *observed* where it was recorded
live and *inferred — unverified* where it is reasoned rather than read. Where the evidence base
is thin the page says so; the gaps are collected in [§6](#6-open-questions).

Where Apple's own [A/UX](aux.md) rides the ordinary Macintosh boot path and hands control to a
Unix kernel from a Macintosh-side startup application (§2 of that page), and MkLinux is a Linux
server composed onto an OSF Mach microkernel loaded by a Macintosh-side Booter
([MkLinux](mklinux.md) §1–§2), Copland goes further: its installer rewrites the boot blocks of
the volume itself, and the boot from there on is Open Firmware, Forth, a native PowerPC loader
and a new kernel — the classic Mac OS appears only as the System 7.5 installation that shares
the volume and the compatibility environment that runs inside the new system.

### 1.2 The build, media and dating

| Item | Value | Evidence |
|---|---|---|
| Documented build | Mac OS 8 Developer Release D11E4, "Compatibility Edition" | [3] [12] (the installer's own title text) |
| Distribution | Mac OS 8 Driver Development Kit v0.4 CD-ROM, effective date June 10, 1996 | [3] [5] |
| Disc specimen | 60 MB UDIF (zlib-chunked) disc image, Apple partition map, HFS volume `Mac OS 8 DDK 0.4`, 2,099 entries | [5] (*observed*) |
| Installable payload | `Mac OS 8 Runtime SW/Mac OS 8 D11E4 (Debug)/`, 532 entries | [5] (*observed*) |
| Installer script | "Install Mac OS", type `'kajr'`, creator `'kajr'` (the Apple Installer script type) | [5] (*observed*) |
| Disc utilities | Drive Setup 2.0d5c2; Power Mac Debugger 2.1d13; DriverCertifier 1.3; Forth Tokenizer | [1] [5] |
| Build ident strings | Version strings across components read "8.0D11E4Dbg" — a debug build | [14] (*community-recorded; not in the corpus's Apple documents*) |
| Earlier build in the corpus | D7E1 ("Scarecrow"), a two-volume design with its own instructions | [4] [5] |
| Component sizes (recorded) | Tertiary loader data fork with 345 recovered names; KernelCore PEF 431,232 bytes with 1,173 recovered names | [7] [11] (*observed*) |

Three developer builds are known to circulate outside Apple — D7E1, D9 and D11E4 [14]
(*community-recorded*); only D11E4 (and D7E1's instruction sheet) is in this corpus, and this
page documents D11E4. The DDK disc also carries a "PowerSurge NVRAM Fixer" for the PCI 7500/
8500/9500 machines [5] (*observed*), which is consistent with the PCI machines of the
supported-hardware list but whose function is not traced ([§6](#6-open-questions), item 9).

### 1.3 The supported-hardware matrix

Apple's install instructions carry a "Hardware Supported" list [1] (*verbatim*):

| Class | Machines |
|---|---|
| NuBus-based Macintoshes | 6100/60, 6100/60AV (no AV functionality), 6100/66, 6100/66 AV (no AV functionality), 6100/66 DOS (no DOS functionality), 7100/66, 7100/66 AV (no AV functionality), 7100/80, 7100/80 AV (no AV functionality), 8100/80, 8100/100, 8100/100 AV (no AV functionality), 8100/110 |
| NuBus-based Performas | 6110CD, 6112CD, 6115CD, 6117CD, 6118CD |
| PCI-based Macintoshes | 7200/70, 7200/90, 7500/100, 8500/120, 9500/120, 9500/132 |
| Keyboards and mice | "All Apple keyboards and mice are now supported!" |
| Displays | monitors on built-in video or a card, at 256 colors (8-bit) or Thousands (16-bit) |
| Drives | "Drives formatted with Drive Setup (other initialization software may work; if you have trouble, try reinitializing with Drive Setup 1.0.4 or later)" |

The DDK read-me adds the configurations Apple actually tested the build against: a matrix of
6100/60 and /66, 7100/66 and /80, 8100/80 through /110, and the PCI 7200/7500/8500/9500, at RAM
sizes from 16 MB to 48 MB [3]. Two machine-identification layers are observed in the installer
itself: rule 200 of the installer script tests Gestalt `('mach')` against seven values (75, 100,
112, 47, 65, 55, 40) and rule 213 against four more (108, 68, 69, 67) plus a separate package
[12] (*observed*; the mapping of Gestalt values to the models above is *inferred from the list
membership* — see [§6](#6-open-questions), item 6). For the machine hardware behind the
evidenced boot, see [pm7100.md](../machines/pdm/pm7100.md) §1 (identity and spec table of the
7100/66) and [pdm.md](../machines/pdm/pdm.md) §1 (the PDM family).

The recorded bring-up establishes one machine end-to-end (*observed* [7]):

| Machine | CPU | RAM | Path exercised | Recorded end state |
|---|---|---|---|---|
| Power Macintosh 7100/66 | PowerPC 601 | 24 MB | Drive Setup 2.0d5c2 format off the DDK disc; System 7.5.0 install from seven floppies; "Install Mac OS"; Caps Lock restart | complete loader chain; kernel bring-up; Copland Finder desktop |

Real-hardware reports add two constraints the corpus's Apple documents do not state: that the
system runs on PowerPC 601 and 604 machines but not on the 603/603e/604e, and that
installations above roughly 32–40 MB fail with "Kernel: Dispatcher: No kernel stacks
available!" [14] (*community-recorded; unverified here* — Apple's own tested matrix includes
48 MB on a 6100/60 [3], so the ceiling is not established; [§6](#6-open-questions), item 4).
Apple's list marks every AV variant "no AV functionality" and the DOS card variant "no DOS
functionality" [1]; the recorded bring-up deliberately ran with empty NuBus slots and onboard
video [7], so the boot's interaction with those cards is untraced ([§6](#6-open-questions),
item 9).

### 1.4 The two-boot volume and the Caps Lock gate

D11E4 installs onto the **same volume** as a resident System 7.5 installation [1], and the two
operating systems are selected at restart by the Caps Lock key: "Put down the Caps Lock key",
then restart, "you should boot into Mac OS 8" [1]. Without the latch the machine boots ordinary
System 7.5 — recorded as a control row of the bring-up [7] (*observed*). The choice of Caps Lock
is not arbitrary: it is the only mechanically locking key on the Apple keyboard
([adb.md](../hardware/adb.md)), so it is the only key that can already be down at power-on
without being held. The gate is implemented twice — once in 68K code the installer splices
into the HFS boot blocks, and once in the Open Firmware boot command (§2.2, §2.3) — and both
read the same key state.

The earlier D7E1 release used the opposite arrangement: a separate "scarecrow" volume carried
the new system, the conventional volume carried System 7.5 with a "ModernOS Enabler" and the
`ModernOSLoader.pef` fragment, the two volumes were kept apart by "No Scarecrow Mount" marker
files, and the Caps Lock key gated the same way: "Very Important: Hold down the caps-lock key
in order to boot NuKernel!" [4]. The install instructions for D11E4 inherit the marker-file
convention as "No Mac OS 8 Mount" on any drive not to be mounted under the new system [1].

## 2. Boot architecture

### 2.1 The chain

The recorded chain, from the ROM's boot-device search to the kernel [7] (*observed*; the
Macintosh-side ROM path is [pdm.md](../machines/pdm/pdm.md) §2.7 — the ROM boot contract — and
[pm7100.md](../machines/pdm/pm7100.md) §5, the machine's boot sequence summary):

```
Macintosh ROM reset, hardware init and POST
  → Start Manager boot-device search → HFS boot blocks of the boot volume
       (rewritten by the Copland installer; 68K code)
  → Caps Lock gate #1: btst #1,($017B).w — KeyMap's bit for key $39
  → GetResource('nkld',$80) → jmp (a0)
  → 'nkld' starts Open Firmware ('opfw') at physical $00004000
  → OF boot-command: Caps Lock gate #2 (get-key-map, byte 4, mask $40)
  → 'BootMacOS' from nvramrc: find the Apple_MacOSPrep partition (pmPartStatus bit 3)
  → interpret the SecondaryLoader Forth stored in that partition
       prints "hi"; walks the HFS catalog itself
       loads the tertiary loader: "Loading from part# 6 Mac OS Loader"
  → tertiary loader ('Mac OS Loader' data fork) at physical $00200000
  → TertiaryLoaderMain: mount the volume, init the framebuffer, show the logo,
       load the fragments, build the page tables, setupMMU
  → kernel: System Libraries/KernelCore, PEF fragment at physical $00500000
       (virtual $C0000000, code section at $C0002000)
  → kernel bring-up: exception machinery, drivers, the low-level debugger nub
  → user-mode tasks; the Copland Finder desktop
```

The classic Mac OS never appears on this path once the gate passes; the System 7.5 on the
volume exists for the installer's prerequisite check (§4.2) and as the boot that the machine
falls back to when the gate does not pass. This is the sharpest contrast with
[A/UX](aux.md) §2, where the Macintosh ROM environment completes, a Macintosh application
loads the kernel, and the Macintosh environment survives inside the Unix system: Copland's
chain leaves the 68K world entirely — after the gate, the recorded machine runs with
`msr = $00003030` and no 68K low-memory address translates any more [7] (*observed*).

### 2.2 The rewritten HFS boot blocks and the first Caps Lock gate

The installer rewrites the boot volume's HFS boot blocks (blocks 720–721 of the recorded
volume): the entry branch's displacement moves from `+$86` to `+$96` and a Pascal string
"Mac OS Loader" is spliced into the header — 16 bytes, exactly the shift [6] (*observed*).
The code at the new entry is the stock System 7.5 boot-block code with a prologue
(*observed* [6], from the 1 KB of patched boot blocks):

```
movea.l ROMBase,a0
cmpi.w  #$077D,8(a0)      ; ROM version floor — the 7100/66 ROM is exactly $077D
blt.s   ordinary_boot
cmpi.b  #$20,$12(a0)      ; and a ROM byte that must lie in [$20,$21)
blt.s   ordinary_boot
cmpi.b  #$21,$12(a0)
beq.s   ordinary_boot
btst    #6,($240B).w      ; a disable flag; $2B on the recorded machine, bit 6 clear
bne.s   ordinary_boot
btst    #1,($017B).w      ; KeyMap's eighth byte, bit 1 = key $39 = Caps Lock
beq.s   ordinary_boot
...
move.l  #'nkld',-(a7)     ; the NuKernel loader resource
_GetResource
jmp     (a0)
```

On the recorded machine every condition except the KeyMap bit already passes (ROM version
`$077D`, ROM byte `$20`, `($240B) = $2B`) [7] (*observed*). The KeyMap test is the gate.

How the latch reaches `KeyMap` is a property of the ADB keyboard, and the recorded bring-up
established it by measurement and deduction [7]: classic Mac OS builds the `KeyMap` global
only from Register 0 key-transition events; the ROM's ADB initialization issues a bus reset,
enumerates by shuffling device addresses, and then **flushes** every device — discarding a
key-down replayed at the reset microseconds later; and the recorded boots issue **zero**
Talk Register 2 commands from reset to desktop. Since Apple's own instructions ("put down the
Caps Lock key, then restart") worked on real machines, the keyboard must re-report a
still-latched Caps Lock on Register 0 after the ADB initialization has cleared the transition
[7] (*inferred — the deduction is falsifiable and was tested in the bring-up; the ADB command
and register semantics are [adb.md](../hardware/adb.md)*). Caps Lock is again the only key
for which this matters: every other key would have to be held down across the restart.

### 2.3 'nkld' and the disk-based Open Firmware

Everything above the boot blocks ships in the **resource fork of `Mac OS Folder/Mac OS
Loader`** on the target volume; the data fork of the same file is a different program, the
tertiary loader [7] (*observed*):

| Resource | ID | Size (bytes) | What it is |
|---|---|---|---|
| `boot` | 1 | 1,024 | the HFS boot blocks the installer writes (§2.2) |
| `nkld` | 128 | 1,840 | the loader the boot blocks jump to |
| `opfw` | 0 | 119,292 | **Open Firmware** — XCOFF (magic `$01DF`), one `.text`, stripped |
| `2ldr` | 1, name "SecondaryLoader" | 4,637 | the SecondaryLoader Forth — also written into the `Apple_MacOSPrep` partition (§2.4) |
| `ofpt` | 1–4, 128–131, 4177, −14, −15 | — | Open Firmware NVRAM variables and patches |
| `nvrm` | 0 | 8,192 | an NVRAM image |

`'nkld'` loads and starts Open Firmware at **physical `$00004000`** [7] (*observed*). This is
not the machine's ROM Open Firmware: it is a disk-resident Open Firmware image shipped with the
OS, and it runs on a machine whose firmware had already booted the classic Mac OS — the PDM
ROM's own boot path completed before the gate ran. The `ofpt` resources carry the OF
environment as plain text [7] (*observed*):

```
auto-boot?    true
use-nvramrc?  true
load-base     300000
boot-command  " kbd" find-device get-key-map device-end
              drop 4 + @ 40 and if BootMacOS then bye
```

So Open Firmware performs its **own** Caps Lock check — `get-key-map`, byte 4, mask `$40` —
before running `BootMacOS`, a second gate after the 68K boot blocks' KeyMap test [7]
(*observed*). The role of the `ofpt` patch resources themselves, and the contents of the
8,192-byte `nvrm` NVRAM image, are not decoded ([§6](#6-open-questions), item 7).

### 2.4 'BootMacOS' and the SecondaryLoader Forth

`nvramrc` (an `ofpt` resource) defines `BootMacOS`: it opens the boot device, walks the
partition map for an entry **named `Apple_MacOSPrep`** with bit 3 of `pmPartStatus` set,
builds `"boot <device>:<part#>"` and `-eval`s it — trying `ata-int/ATA-Disk@0`, then
`scsi-int/sd@X` and `scsi/sd@X` for X = 6 down to 0; if nothing matches it prints `ldr?` [7]
(*observed*). The `boot` verb of this firmware is therefore extended to boot *from a partition*,
and what it boots is the Forth source stored in that partition.

That partition is what the installer creates (§4.4): on the recorded volume the
`Apple_MacOSPrep` partition at blocks 64–73 holds 4,637 bytes of Forth — the same bytes as the
`'2ldr'` "SecondaryLoader" resource — which Open Firmware interprets [6] [7] (*observed*).
The Forth prints `hi`, walks the HFS catalog **itself** (the classic Mac OS is not running and
no Macintosh file manager is available to it), and loads the tertiary loader from partition
number 6, announcing `Loading from part# 6 Mac OS Loader` [7] (*observed*).

### 2.5 The tertiary loader

The tertiary loader is the **data fork of `Mac OS Loader`**: a flat PowerPC binary, loaded at
**physical `$00200000`**, entering at `TertiaryLoaderMain` (`$00201040`), running
identity-mapped until it turns the MMU on itself [7] (*observed*). It is a developer build and
carries PowerPC traceback tables; 345 function names are recoverable from them, which makes
its whole call structure legible [7] [11]. `TertiaryLoaderMain`'s recorded call sequence [11]
(*observed*; `displayTimeStatistic` calls between steps omitted):

| Step | Call | Role |
|---|---|---|
| +`$0024` | `InitializeCI` | bind the Open Firmware client interface (peer, `finddevice` ×2, `getprop` ×4, `open`, version probe) |
| +`$004c` | `printf` | "Hello from the Tertiary Loader!" on the serial console |
| +`$005c` | `claimLoadedAreas` | tell OF which memory is already occupied — **fails, non-fatally** (§2.7) |
| +`$006c` | `initializeAlignmentHandler` | |
| +`$0094` | `initializeTemporaryPool` | |
| +`$00ac` | `EHFSInitialize` | an HFS reader for the boot volume |
| +`$0128` | `mountBootVolume` | |
| +`$01d8` | `loadResourceBasedPatches`, +`$020c` `loadFileBasedPatches` | |
| +`$0248` | `findBestMainDisplay` | |
| +`$0294` | `initFrameBuffer` | |
| +`$0380` | `getKernelBootImageMembers` | enumerate the kernel image's members |
| +`$03fc` | `findFamiliesandPlugins`, +`$0474` `GetOFTree`, +`$04d0` `findFamiliesForAllDrivers` | the driver-family search, driven from the OF device tree |
| +`$053c` | `sortMembers`, +`$0544` `buildCfrgResource`, +`$054c` `assignBootImageVirtualSpace` | the boot image as a CFM closure |
| +`$0580` | `loadFragments` | load the PEF fragments |
| +`$05e8` | `prepareBootImageClosure` | |
| +`$061c` | `buildPhysicalSpaceTable` | |
| +`$0650` | `initializeHashTable` | the PowerPC hashed page table (`sdr1`) |
| +`$0658` | `initializeKernelStacks` | |
| +`$0694` | `buildKernelSpaceTable` | |
| +`$06fc` | `setupMMU` | MMU on; the loader's identity mapping ends |
| +`$0740` | `showLogo` | the boot logo on the framebuffer |

Every one of these steps is recorded reaching completion in order, and `FatalError` is **never
called** [7] (*observed*, by logpoints on each named entry). After `setupMMU` control leaves
the loader for the kernel; `load-base` is `$300000`, which is *not* where the tertiary loader
actually lands — the measured load addresses are the table in §2.6.

### 2.6 The handoff and the kernel

Control passes to **`System Libraries/KernelCore`**, a PEF container (`Joy!peff`) of 431,232
bytes whose code section the loader maps at **virtual `$C0002000`** — the fragment starts at
virtual `$C0000000`, which is **physical `$00500000`** [7] (*observed*). The measured image
layout of the chain (*observed* [7]):

| Image | Physical | Virtual | Notes |
|---|---|---|---|
| Open Firmware (`opfw`) | `$00004000` | identity | started by `'nkld'` |
| Tertiary loader (`Mac OS Loader` data fork) | `$00200000` | identity until `setupMMU` | entry `TertiaryLoaderMain` at `$00201040` |
| KernelCore | `$00500000` | `$C0000000` (code at `$C0002000`) | PEF; 1,173 traceback-recovered names; `r2` = `$C4030E90` |

The kernel builds its hashed page table with `sdr1 = $05000003` on the recorded 24 MB machine —
`$05000000` is a real DRAM bank window on this machine (bank 1's window base, which is backed
even though the address looks oversized for a 24 MB machine) [7] (*observed*). Like MkLinux,
and unlike the classic Mac OS, Copland keeps its exception vectors at physical low memory —
its ISI dispatch was single-stepped at physical `$00003B88` — and relocates its framebuffer
above the first megabyte (§3.7) [7].

The kernel is not a static image mapped once: the loader prepares a **boot image closure** of
PEF fragments (§2.5, `prepareBootImageClosure`) and the Code Fragment Manager-style
preparation reads each fragment's container header and loader section, then executes its code
section *in place*, demand-paged (§3.2). Kernel-space containers were recorded at
`$C0000000`+ and task-space containers at `$54000000`+ [7] (*observed*).

### 2.7 The failing `claim` — real, and not fatal

The one error the loader prints is `CLAIM failed` [7] (*observed*). The failing call is
`claimLoadedAreas`, the fourth step of `TertiaryLoaderMain`, which builds a five-argument
Open Firmware client-interface call on its own stack — `call-method`, the method name
`claim`, an ihandle that `InitializeCI` opened, align 0, size `$2000`, physical `$4000` —
five arguments and two returns, i.e. `claim` invoked as a *method on an instance*, not the
bare client service [7] (*observed*, the argument array read word by word at a breakpoint).
The method returns `-1`; the address `$4000` is exactly **Open Firmware's own load address**
(§2.3), and the routine's name says what it is for: telling OF "this memory is already in use,
do not hand it out", starting with OF's own image.

The loader does not notice: `claimLoadedAreas` tests only the **client-interface status**
(`r3`, zero = the call was made) and never reads the method's own return value, so the boot
continues with that region unclaimed and runs to completion [7] (*observed*). The `claim`
genuinely fails, and why is not established ([§6](#6-open-questions), item 1). The failure is
also not isolated damage: `CallRawClientInterface` is reached from 14 sites — among them
`allocateMappedPages`, `buildRAMTableEntries`, `doLogicalToPhysicalMapping`,
`findDisplayAddressesAndSize` and `initFrameBuffer` — so the same claim path is load-bearing
for the memory map the loader builds [7] (*observed*).

## 3. Hardware interface

### 3.1 One system, per-machine driver sets

Apple's driver model for Mac OS 8, as published in the DDK read-me: drivers belong to
*families* (the User Input, Block Storage, Graphics, Open Transport, SCSI families among
others), are written as plug-ins, and "most drivers will no longer interface with the Device
Manager" — the Device Manager remains only for products that fit no family [3]. The release
notes state the current roster bluntly: the NuBus "Slot Manager API's not yet supported",
"Sound in not supported", the Keyboard Family was eliminated (its I/O now lives in the
User-Input Family), and "Booting does not support 3rd party chaining during boot process,
such as volume encryption or volume password" [2].

On the installed volume the drivers are stored as PEF fragments in two folders: `System
Libraries` (275 files) and `Hardware Support` (58 files) [7] (*observed*). Machine
dispatch is by which plug-ins the loader's family search finds: the recorded boot maps the
floppy driver `Hardware Support/BSFloppyPDM` (the PDM/AMIC variant) whose sibling
`BSFloppyGRC` is the Curio/GrandCentral variant — the same layout with a different hardware
abstraction layer — and the motherboard driver `NgaioPDMMotherboard`, whose PCI counterpart
`PSMotherboard` is correctly absent on a PDM machine [7] [8] (*observed*). Driver fragments
run as kernel tasks; the floppy driver's task is named `'BSDX'` in the kernel's own panic
report [7] (*observed*; which component assigns task names is [§6](#6-open-questions),
item 14).

Applications are fenced off from all of this by the address spaces: "applications reside in
a different address space than drivers do" and cannot touch hardware [3]. The recorded boot
shows the split concretely: kernel-space containers at `$C0000000`+, task-space containers
at `$54000000`+, and the Finder itself running as a user-mode task with `MSR[PR]=1` and a
deep user stack [7] (*observed*).

### 3.2 Demand paging of code sections

The kernel pages its code lazily. A fragment's container header and loader section are made
resident when the fragment is prepared; the code section is executed **in place**, and each
code page is faulted in on first execution [7] (*observed*). The recorded instance that
characterizes the contract [7]:

| Virtual page of `BSFloppyPDM` (`$5433C000`–`$54341D84`, six pages) | Physical |
|---|---|
| `$5433B000` | unmapped |
| `$5433C000` — code page 0 (`FloppyPluginInit` runs here) | `$011DC000` |
| `$5433D000` — `RegisterFloppyISR` lives here | unmapped until first executed |
| `$5433E000`–`$54340000` | unmapped |
| `$54341000` — last code page | `$011DD000` |
| `$54342000` — first data page | `$011DE000` |

The resident pages hold **consecutive** physical frames for **non-consecutive** virtual pages —
pages arrive in touch order and take the next free frame [7] (*observed*). The single
instruction-fetch page fault this produces on the first branch into an untouched code page is
the load path working normally; how the kernel classifies such faults is the next section.

### 3.3 The exception contract and the panic path

The kernel's exception ladder, from its own disassembled and named functions [7]
(*observed*; addresses are the recorded live ones):

```
PreprocessException    $C0009000   vectors 24 (DSI) and 32 (ISI) share a case;
                                    kind = 3 (kAccessException) is the DEFAULT,
                                    refined by the checks below
  PageFault            $C002993C
    GetFaultInformation $C0030B5C   decodes the fault from the machine state
    CheckMemoryException $C0028400  -> returns the covering area, or 0 with a kind
  IsStackOverflowException           r4 = $FFFFF68E (-2418) on the overflow path
  RunExceptionHandler  $C000E068     no handler -> Panic $C0005D54
```

`CheckMemoryException` runs a ladder of checks — task level, an area covers the address, the
address is inside it, permissions, area flags — and passes them all for an ordinary demand
fault, then tests one flag byte before returning the area so the page can be brought in
(*observed* [7]):

```
CheckMemoryException+0x1A4  lbz   r4,123(r1)     ; the "hard access error" flag
                    +0x1A8  cmpwi cr1,r4,0
                    +0x1AC  beq   cr1,+0x1BC     ; clear -> return the area, page it in
                    +0x1B0  li    r5,3           ; set  -> kind = kAccessException
```

The flag is computed in `GetFaultInformation` **from SRR1**: for an instruction-access
exception (vector 32), the mask is `$10200000` — bits 3 and 10; for a data exception the
mask on DSISR is `$04700000` [7] (*observed*). The consequence is a hard contract on the
PowerPC exception encoding: **an ISI whose SRR1 carries bit 10 is treated as a hard access
error and is never paged** — `kAccessException` (kind 3, "no valid area covers this address"),
which ends in `Panic`; a demand-fetch fault with bit 10 clear pages normally. That a real
PowerPC 601 leaves SRR1 bit 10 clear for an instruction-fetch hash-table miss is the
consistent reading — Copland shipped expecting to boot on these machines, and every
demand-paged code page would otherwise die — and it is corroborated by the PDM boot ROM's
own nanokernel, whose `InstStorageInt` tests SRR1 with `andis. r8,r11,$4020` followed by
`beq` (file offset `$311878`, the only such instruction in the 4 MB image): an
either-bit-satisfies test, consistent with bit 1 alone [7] (*inferred — unverified*).

`Panic` (`$C0005D54`) formats its message — the recorded one reads `Unhandled task
exception at 0x5433D13C, task 'BSDX', type kAccessException (0x400, instruction access)`,
built from the format string `Unhandled %s exception at 0x%X, task '%.4s', type %s (0x%X,
%s)` — sets the low-level-debugger-enabled byte to 1, and executes `tw 28,r1,r1` [7]
(*observed*). The trap re-enters the exception path, which now finds the debugger nub
enabled and calls it: `LLNubMain` → `HandleException` → `ConnectToHost` → `DoNubTransaction`
→ `WaitForByte`, which polls ESCC channel A RR0 bit 0 (physical `$50F04002`) for a Power
Macintosh Debugger to answer — and reads a "timeouts enabled" byte that is zero, so the
wait has **no deadline at all** [7] (*observed*). This is the "sits on the serial port
forever" state of a failed boot: the panic *report* waiting for a debugger, not a boot-time
handshake. The serial port itself is the SCC's channel A ([scc.md](../hardware/scc.md); the
PDM serial block is [pdm.md](../machines/pdm/pdm.md) §4.5).

The debugger nub is also entered for **recovered** faults, and the two are told apart by one
byte: `LLNubMain` tests the nub-enabled flag `*(*(0xC403030C))` in its first four
instructions — seven recorded nub entries with the byte clear came from NuBus slot probes
(§3.4) and were recovered from; the fatal entry had the byte set and never returned [7]
(*observed*).

### 3.4 The NuBus probes — expected faults

The kernel probes the NuBus standard slot space for declaration ROMs through
`DriverSupport`'s `DeviceProbe`, which brackets the access with the debugger-nub disable
and re-enable [7] (*observed*):

```
DeviceProbe(addr, dest, accessType)
  -> EnableLLDebugger(0)
  -> DeviceProbe8/16/32        ; sync x3, load, sync x3, blr
  -> EnableLLDebugger(1)
  -> return the status the handler left
DeviceProbeFailure              ; kinds 3 and 4 -> skip the instruction, status = -200
```

The recorded boot probes virtual `$55383FFF/FE/FD/FC`, which translate to the last four
bytes of slots `$E`, `$B`, `$C` and `$D` of standard slot space — the classic top-of-slot
format-block test, the same probe the PDM ROM performs ([bart.md](../machines/pdm/bart.md) §4.1;
slot-space addressing at [bart.md](../machines/pdm/bart.md) §1.4) [7] (*observed*). Each
probe faults on the empty slots, `DeviceProbeFailure` recovers it, and the boot goes on:
**these faults are normal**, and their nub entries are the benign ones §3.3 distinguishes
from a panic [7].

### 3.5 The SWIM3 floppy driver

`Hardware Support/BSFloppyPDM` — the PDM floppy plug-in — has a fully recorded register
contract [8] (*observed*; annotated disassembly, 5,953 instructions, 86 recovered function
names; the SWIM3 register file itself is [swim3.md](../machines/pdm/swim3.md) §2):

- **Device base.** The fragment's TOC holds the SWIM3 base twice (a pointer chain at
  TOC −16 and a direct copy at TOC +1152); in the recorded boot it is virtual `$53E8D000`,
  translating to physical `$50F16000` — the PDM SWIM3 island, registers at stride `$200`
  exactly as the classic Mac OS drives them ([swim3.md](../machines/pdm/swim3.md) §2.2):
  `+$200` Timer, `+$400` Error, `+$C00` Zeroes, `+$1000` Intr, `+$1E00` IntMask.
- **The settle primitive.** `SwimIIISmallWait(n)` loads the **1 µs countdown timer**
  (register 1) with `n+1` and polls it until it reads zero — no interrupt, no Time
  Manager, no bound other than the hardware count. Every drive-signal change settles
  through this primitive (`SwimIIISetSignal` → `SmallWait`) [8] (*observed*). The poll is
  only meaningful if the **running count reads back** as it counts — a live-read-back
  requirement the SWIM3's published register description leaves unstated
  ([swim3.md](../machines/pdm/swim3.md) §2.8), and which a second, independent guest now
  confirms.
- **The interrupt-side contract.** `HALISRHandler` reads `Intr` (read-to-clear) into a
  shadow, returns −1 if a first-level handler saw nothing, else masks everything
  (`IntMask ← 0`), clears mode bit 0 through the Zeroes port, and if shadow bit `$20`
  (error interrupt) is set reads `Error` into another shadow [8] (*observed*).
- **Cross-fragment calls.** Imports from other fragments go through per-import glue
  stubs packed after the last function (`lwz r12,-N(r2); stw r2,20(r1); lwz r0,0(r12);
  lwz r2,4(r12); mtctr r0; bctr`), so a program counter inside a stub carries the
  *callee's* TOC; the caller's own TOC is at `20(r1)` [8] (*observed*).

The boot-time path through the driver, with no diskette in the drive, is ordinary
initialization: `FloppyPluginInit` → `InitializeDrive` → `PowerDriveDown` →
`HALPowerDownDrive` → `SwimIIISetSignal` → `SwimIIISmallWait` [8] (*observed*) — the
driver powers the drive down and settles each signal change on the hardware timer, so a
boot with no floppy inserted still requires a running timer.

### 3.6 ADB and Cuda input

How mouse and keyboard data reaches the Copland desktop, recorded stage by stage [9]
(*observed*; container addresses from the recorded boot; the Cuda transport and ADB bus are
[pdm.md](../machines/pdm/pdm.md) §4.7 and [adb.md](../hardware/adb.md)):

```
Cuda autopoll (unsolicited packet, type 0 = ADB; flags & $0E are errors; $40 = autopoll, OK)
  CudaLib (kernel space, $C0106000)
    CudaUnexpectedAttention -> CudaReceiveData -> ProcessResponse
    NotifyNewCudaEvent(pkt, pkt+3, len, status):
      status != 0 -> drop; type 1/3 -> drop; type 4 -> tick handler (§3.8)
      type 0 -> {addr = pkt[2] >> 4, data, len} -> registered handler
  ADBCudaPlugin (server task): PlugInEventNotification — copy, forward
  ADBServer (task space, $54306000): ADBFamAutopollArrived(addr, data)
      per-address ring: base = *(TOC-32) + addr*508 + 40; 16 entries of 28 bytes
      {full flag, 12 data bytes, timestamp}; write index at +448
      then the 16-bit OPEN-ADDRESS MASK at TOC+44: bit set = a driver has the device
      open -> signal that connection; bit clear -> "unexpected autopoll" event
  InputDevADBMouse: GetEvents -> ADBGetNextAutopoll drains the ring;
      Initialize did ADBOpen + ADBSetHandlerID (handler 2 = classic 200 cpi mouse)
```

Two recorded behaviours of the enumeration are load-bearing [9] (*observed*):

1. **Device movement.** The kernel's `FullProbe` (with `MoveDevice`, `PickDeviceToMove`,
   `GetFreeAddress`) relocates devices to new ADB addresses and leaves them there — the
   recorded mouse ended at address `$D` — so the autopoll must ask where devices live, not
   poll fixed addresses.
2. **Handler-ID semantics.** The Listen R3 handler byte is a command, not a value to store:
   `$FE`/`$00` move the device and preserve the handler ID, `$FF`/`$FD` are self-test and
   conditional, and only a plain ID is adopted. A device reporting handler `$FE` after
   enumeration is classified as "not a mouse" and dropped by the input family
   ([adb.md](../hardware/adb.md) for the register semantics).

The kernel's ADB bring-up enables autopoll early — before some Cuda transport states settle
— so a sync (Abort/Sync) can arrive mid-transfer; the Cuda byte-level state table treats
ByteAck with TIP negated as Abort/Sync from any state, and the guest relies on that [9]
(*observed*; the table is [pdm.md](../machines/pdm/pdm.md) §4.7).

### 3.7 Video: the relocated framebuffer

The PDM platform has **no framebuffer-base register**; the scan-out base is selected by
HMC serial-configuration bit 33 — set, fetch from physical `$00000000`; clear, fetch from
physical `$00100000` ([hmc.md](../machines/pdm/hmc.md) §2.3, §3.7; the framebuffer's three
addresses are [ariel.md §3.2](../machines/pdm/ariel.md)). The classic Mac OS leaves the bit
set — every recorded System 7.5 boot ends with the bit at 1, scanning physical 0 — while
the recorded Copland boot runs the same ROM initialization sequence and then issues **one
more write that clears the bit**, relocating scan-out to physical `$00100000` [7]
(*observed*, every bit-33 shift of two boots logged). The recorded desktop itself sits at
physical `$100000`–`$14B000` as 640×480×8bpp — menu bar, desktop pattern, the `untitled`
volume icon, the Trash, the cursor [7] (*observed*).

This is the same placement MkLinux DR3's Power Macintosh platform support uses for its
frame buffer ([hmc.md](../machines/pdm/hmc.md) reference 6) — an OS that keeps its vectors
at physical 0 relocates video rather than memory. The Copland video plug-in
`FBufPDMPlugin` (HAL codename SVELTE, 107 recovered function names) carries no address
constants at all: the framebuffer base comes from the Open Firmware device tree, and the
hardware follows bit 33 [7] (*observed*).

### 3.8 Time and file dates

The OS takes the wall clock from the **Cuda one-second tick**, not from an explicit RTC
read: a PDM boot of System 7.5 or Copland issues no `RdTime` at all — it programs
`Wr1SecMode 3` ("Mode3Clock") and relies on the one-second tick packet to carry the 4-byte
RTC value, which `CudaTickHandler` writes into the classic low-memory `Time` global
(`$020C`) [10] (*observed*). When the tick carries no value, `Time` counts from zero —
and files written during a session are stamped in **uptime seconds** rather than wall-clock
time [10] (*observed*: a freshly-installed volume carries files dated seconds-to-minutes
after epoch).

The consequence is a recorded fatal path [10] (*observed*): the Finder's application-launch
rewrite of the "System Folder" catalog record goes through the new OS's HFS validator
`ValidCatalogEntry`, which requires directory records to satisfy **crDat ≤ mdDat**. A volume
whose files carry near-epoch dates from two separate boots with non-monotonic clocks can
violate the invariant; the validator fires `DebugAssert("...invalid catalog entry!")`,
traps into the low-level debugger nub, and the machine hangs in the serial-port wait of
§3.3. The debugger entry points are recorded as touch-string-then-`tw` stubs — the same
terminal state as the boot panic [10].

### 3.9 The serial console and the debugger contract

The loader prints its transcript over SCC channel A (§5.1); the kernel prints **nothing**
there unless the debugger nub is engaged [7] (*observed*). Apple's debugger contract [1]:
the debugging machine can be any Macintosh with 7 MB free RAM (virtual memory acceptable) —
it need not be a PowerPC machine — connected by serial cable to the **modem port** of the
Mac OS 8 machine, running Power Macintosh Debugger 2.1d13 with Remote Host set to the modem
port. Debugger entry points, typed on the Mac OS 8 machine [1]: hold **Option** at startup
to enter the debugger during boot; **Cmd-Shift-6** to enter it while running; **Cmd-Shift-7**
for MacsBug; **Cmd-Power** to force a non-maskable interrupt into the kernel; hold
**Cmd-Ctrl-Power** to reboot. Apple's own hedge about the two-machine requirement is quoted
in [§4.3](#43-apples-install-flow-and-the-debugger-hedge).

## 4. Installation & bring-up

### 4.1 The DDK media set

The disc specimen [5] (*observed*):

| Path | Contents |
|---|---|
| `/DDK 0.4 Read Me First!` | the read-me of [3] |
| `/Applications/` | Acrobat Reader, SimpleText 1.3.1 |
| `/Developer Documentation/` | Bus Architecture, Inside Macintosh for Mac OS 8, Mac OS 8 Architecture, Open Transport |
| `/Mac OS 8 Runtime SW/` | Drive Setup 2.0d5c2; "How to Install Mac OS 8 (D11E4)" [1]; "Mac OS 8 (D11E4) Release Notes" [2]; `Mac OS 8 D11E4 (Debug)/` — the installable payload, 532 entries |
| `Mac OS 8 D11E4 (Debug)/` | `Install Mac OS` (`'kajr'`/`'kajr'` installer script), `Installer` (`'APPL'`/`'kajr'`), `Mac OS Folder/` (System, Finder, Extensions, Themes, **Mac OS Loader**, Hardware Support, System Libraries, …), `Mac OS 8 Applications/` (GXSlidemaster, MonitorsCP.ppc, StartupOpenTptATBC), `Optional Pieces/`, `PowerSurge NVRAM Fixer/`, `Stationery/` |
| `/Mac OS 8 Dev. Env. SW/` | interfaces and libraries matching the release |
| `/Sample Code/`, `/Tools/`, `/Presentations/`, `/Developer Program/` | driver samples, OpenFirmware samples; Power Mac Debugger 2.1d13, DriverCertifier 1.3, Forth Tokenizer, PCI Peek; WWDC-era presentations; program forms |

Two circulating disc images of this CD were compared entry by entry and differ only in
Finder bookkeeping (Desktop DB/DF sizes, one zero-length icon file) — the same disc [5]
(*observed*). The DDK's own positioning is worth stating for scope: "this collection of code
and documentation has demonstrated its usefulness for driver developers... the DDK is not
intended to provide support for application developers", with application compatibility work
deferred to the "Mac OS 8: Developer Release: Compatibility Edition" promised for that
summer [3] — the title the installer script itself carries [12].

### 4.2 The installer rules

The authoritative gate list is not the prose documentation but the `'inrl'` (installation
rule) resources of the `Install Mac OS` script, read directly off the disc [12]
(*observed*):

| id | Rule | Constant / check | Meaning |
|---|---|---|---|
| 200 | Check Supported CPU | Gestalt `'mach'`, 7 values: 75, 100, 112, 47, 65, 55, 40 | the NuBus PDM set (*inferred from list membership*) |
| 201 | Error — Not Supported | — | "this developer release … cannot be used on this computer." |
| 202 | Check Min Memory | `$00000010` = 16 | 16 MB minimum |
| 203 | Error — Not enough Memory | — | "at least 16 megabytes of RAM installed." |
| 204 | Check Tgt Vol Size | custom function | 230 MB minimum |
| 205 | Error — Tgt Vol Too Small | — | "a capacity of at least 230 megabytes." |
| 206 | Add All Packages | packages 200, 500, 214 | "Mac OS 8 Developer Release: Compatibility Edition" |
| 207 | **Check for Secondary Ldr Partn** | custom function | the Drive Setup partition check |
| 208 | Error — Need to Run Drive Setup | — | "…is not properly formatted for installing Mac OS 8." |
| 209 | Check Mac OS 8 Active | custom function | refuses to install from within Mac OS 8 |
| 210 | **Check Valid 7.5 System on Tgt** | `$0750` | System 7.5.0 or later |
| 211 | Error — No Valid 7.5 System | — | "You must have System 7.5 installed on the hard disk…" |
| 212 | Check for previous Error | — | — |
| 213 | Check Supported CPU (PCI) | Gestalt `'mach'`: 108, 68, 69, 67 | the PCI set, plus package 301 |

Rule 210's encoding is 18 bytes with `$0750` as the **only** system-version constant in the
entire 321-resource fork: any 7.5.x satisfies the gate, and 7.5.3 is not specifically
required [12] (*observed*). Rule 207 is the reason the target cannot be a plain HFS volume:
it checks for the secondary-loader partition that Drive Setup creates (§4.4), and a volume
without it fails rule 208 [12]. The release notes confirm the installer script was retuned
for this release: the "not enough capacity" error "now reflects minimum HD capacity value
of 230MB for DR1", the buggy `equalstring` call is gone, partition flavors are correctly
determined, and "script will recognize ATA driver" [2] — consistent with the ATAPI device
first in `BootMacOS`'s device order (§2.4).

### 4.3 Apple's install flow and the debugger hedge

Apple's instructions, step by step [1]:

1. **Back up** — "This is a Pre-Release operating system. BACK UP all information on all
   drives connected to a machine on which you plan to run Mac OS 8. You may experience
   disk corruption and data loss on this release."
2. Place a text file named **"No Mac OS 8 Mount"** at the root of any drive not to be
   mounted under Mac OS 8.
3. **Initialize a 250 MB or larger drive** with Drive Setup — Apple recommends the bundled
   Drive Setup 2.0d5c2: Custom Setup → Partitioning Scheme "1 Macintosh HFS" → Initialize,
   with the status field reading "New Partitioning: 1 Macintosh HFS / Initialization
   Options: none". "You should be able to install Mac OS 8 on a drive initialized by any
   software."
4. **Install (do not copy) a fresh System 7.5.x** onto the Mac OS 8 volume.
5. Run the Mac OS 8 Installer script **"Install Mac OS"** and install onto the same volume.
6. Set the startup disk to the Mac OS 8 volume.
7. **Put down the Caps Lock key** and restart.

On the debugger, Apple's instructions are internally hedged: "You currently need two
machines to boot Mac OS 8" (one from the supported-hardware list, one running the Power
Macintosh Debugger over a serial cable) — and, in the same document: "There is currently no
reliable way to run Mac OS 8 without the debugger at this time; however, **you should be
able to boot to Finder in most cases without a debugging machine connected**" [1]. The
earlier D7E1 build's instructions carry no such hedge — for it, "in order for the system to
boot, the host debugger on a separate machine **must** be running" [4]; that build also
required its separate scarecrow volume, a blessed system folder that "tends to get
de-blessed" and must be re-blessed, and a filesystem Apple described as "NOT stable" [4].
Which of the two positions the D11E4 kernel actually depends on is
[§6](#6-open-questions), item 3.

### 4.4 What the installer writes

The recorded installation differential — the target volume compared block by block before
and after the installer run — shows exactly **three regions changed** outside file data [6]
(*observed*):

| Region | Blocks (recorded volume) | What the installer does |
|---|---|---|
| Partition map entry | 1 | stamps `pmBootSize` 8192, `pmBootAddr`/`pmBootEntry` `$00600000`, `pmPartStatus` `$37`→`$7F` (setting bit 3, the flag `BootMacOS` tests, §2.4) |
| `Apple_MacOSPrep` partition | 64–73 | writes PowerPC code into a partition that was all zeros after formatting: the 4,637-byte SecondaryLoader Forth |
| HFS boot blocks | 720–721 | moves `bbEntry`'s `BRA.W` from `+$86` to `+$96`, splices the Pascal string "Mac OS Loader" into the header (§2.2) |

The partition the installer fills is created by **Drive Setup 2.0d5c2 at format time**, not
by the installer: the recorded format writes a 7-entry partition map whose payload is the
`SecondaryLoader` partition (type `Apple_MacOSPrep`) plus an empty `Apple_HFS` volume named
"untitled"; System 7.5's own HD SC Setup writes a 4-entry map instead [7] (*observed*).
This is rule 207's check (§4.2), and it is why the volume must be prepared with the newer
tool.

### 4.5 The drive gate: Apple-branded drives

Drive Setup formats only Apple-branded drives, and "Apple-branded" is a property of the
drive's firmware pages, not of its INQUIRY string: what Apple's formatters key on is MODE
SENSE page `$30` — Apple's vendor page carrying the ASCII string `APPLE COMPUTER, INC.` —
which a stock mechanism and its Apple-labelled twin differ in [13] (*recorded
investigation*; the INQUIRY vendor/product/revision fields and READ CAPACITY do not
distinguish them). The recorded bring-up used an Apple HD230SC (a Quantum ProDrive LPS
240S, 479,350 blocks of 512 bytes) [7] [13] (*observed*). Apple's own instruction text is
softer than the tool's gate — "you should be able to install Mac OS 8 on a drive
initialized by any software" [1] — and whether an Apple-branded drive is strictly required
in practice is community-unsettled [14]; the recorded success is on an Apple-branded drive.

### 4.6 The recorded bring-up configuration

The recorded end-to-end bring-up [7] (*observed*):

| Item | Value |
|---|---|
| Machine | Power Macintosh 7100/66, PowerPC 601 ([pm7100.md](../machines/pdm/pm7100.md) §1) |
| RAM | 24,576 KB (24 MB — inside the 16–32 MB window of the installer's rule 202 and the community-reported ceiling) |
| ROM | PDM boot ROM version `$077D`, checksum `$9FEB69B3` |
| Video | built-in Ariel II, 640×480, 8 bpp, sense code 6 ([ariel.md](../machines/pdm/ariel.md)) |
| SCSI target 0 | the 230 MB target volume (Apple HD230SC), Drive-Setup-formatted |
| SCSI target 3 | AppleCD 300i carrying the DDK 0.4 disc |
| Floppy | internal SuperDrive, used for the System 7.5.0 install (seven disks) |
| NuBus | empty — no cards; onboard video, per Apple's "no AV functionality" caveats [1] |

The volume chain, each stage consuming the previous stage's artifact [7] (*observed*):
blank HD230SC → formatted by Drive Setup 2.0d5c2 run from the DDK disc → System 7.5.0
installed from the seven retail floppy images → "Install Mac OS" run from the DDK disc →
the bootable Mac OS 8 volume. Apple's tested-configuration matrix for the build spans 16 MB
to 48 MB across the supported machines [3]; the recorded bring-up ran at 24 MB throughout.

## 5. Observed behaviour & quirks

### 5.1 The serial boot transcript

The complete loader transcript over SCC channel A, recorded live [7] (*observed*):

```
hi
Loading from part# 6 Mac OS Loader

Hello from the Tertiary Loader!
CLAIM failed
```

`hi` is the SecondaryLoader Forth announcing itself; `Loading from part# 6` is it loading
the tertiary loader after its own catalog walk; `Hello from the Tertiary Loader!` is
`TertiaryLoaderMain`'s `printf`; `CLAIM failed` is the non-fatal `claimLoadedAreas` failure
of §2.7, which the loader proceeds past. A log line is a claim to verify, never a
conclusion: the recorded bring-up established that the loader ignores the failure it just
reported, and that the seven debugger-nub entries a boot produces before any fatal one are
recovered NuBus probes, not crashes [7].

### 5.2 The on-screen bring-up

The recorded boot renders its progress on screen: the verbose console lines (including
"Microkernel initialization starting…complete."), the Mac OS logo from `showLogo`, and
then the desktop — menu bar (File, Edit, View, **Spaz** — the build's codename, carried in
the Finder's menu set), the `untitled` volume icon, the Trash, the arrow cursor [7]
(*observed*). The Finder is the 2,050,724-byte PEF `Mac OS Folder/Finder`, its 1.6 MB code
section mapped at `$55EF3A20`–`$56078024`, running as a user-mode task [7] (*observed*,
byte-matched against the on-disk fragment at two independent stack addresses). On the
recorded machine the release-to-desktop span is roughly 8.5 G instructions, of which the
former stopping point (§3.3's panic, before its root cause was understood) was 1.6 G [7].

### 5.3 The panic contract in practice

The recorded kernel panic of §3.3 — `kAccessException` on the first demand-fetched code
page — was traced instruction by instruction [7]: the faulting fetch is `BSFloppyPDM`'s
`FloppyPluginInit+0x64`, a `bl` to `RegisterFloppyISR` at `$5433D13C`, one page beyond the
code page the plugin's initialization runs in; the ISI carries `SRR1` bits that
`GetFaultInformation`'s `$10200000` mask reads as a hard access error; and the kernel
panics instead of paging, in the words of its own report, as task `'BSDX'`. The
classification contract — not the fault itself — is what decides whether a demand-paged
system boots, and it is restated in §3.3 as the durable hardware-facing fact.

### 5.4 Documented crashes and traps

From Apple's own documents, observed to be warned about rather than observed to occur [1]
[2]:

- **Desktop printer setup**: creating a desktop printer requires hitting **Cancel** at the
  "Setup" dialog — "HIT CANCEL AT THIS DIALOG OR YOU WILL CRASH" [1]; the release notes
  add that selecting "Input Trays" from the desktop printer's Printing menu crashes unless
  three preconditions hold, "Disable Manual Feed Alerts" crashes, and a spool file or PDD
  dropped on the boot drive's root window "will generate a lot of assertions and an access
  fault" [2].
- **`%GLURG%`**: immediately after installing, the volume may show a folder named
  `%GLURG%` and no "Mac OS Folder" — the installer's temporary name; closing and reopening
  the volume window reveals the real folder [1].
- **The Finder of this release** does not reopen windows left open at shutdown, clips some
  window titles on redraw, and collapsing an outline view "may beep and then do nothing"
  as a deliberate guard against a crash in asynchronous outline updates [2].
- **The Communications Toolbox** ships as six shared libraries of which five are "creeper
  libraries — every exported call just sends a debugstr and returns null", so applications
  depending on it do not work [2].
- **The screen saver** is off by default; a file named "Enable Screen Saver" at the root of
  the Mac OS 8 drive turns it on [1].

### 5.5 Small observed behaviours

- The Finder is launched by type and creator (`'MACS'`, `'ÄNDR'`): "any app used as a
  'replacement' finder must have this signature" [2].
- The Folder Manager uses the MacOS Loader's `FolderDescriptor` resources but not its
  folder-routing resources, and "the system disk is assumed to be vRefNum −1"; the trash
  folder for File Sharing is not supported [2].
- Theme selection in this build is by file renaming — the D7E1 instructions' "for fun"
  note ("rename the z theme in the themes folder … to default theme") carries forward [4],
  and this release has no Appearance Manager control panel [14] (*community-recorded*).
- The one-second tick of §3.8 is the only time delivery in a PDM boot — no `RdTime` or
  `WrTime` appears anywhere in a recorded boot [10] (*observed*).
- The microkernel release notes describe three standing kernel problems without bug
  numbers: `ReplyToMessage` at SIH level "may fail randomly", resource exhaustion "may
  result in a crash" without a defined failure, and heavy paging demand may produce
  "unable to get free page" [2].

## 6. Open questions

1. **The failing `claim`.** Why the Open Firmware `claim` of 8 KB at physical `$4000`
   returns −1 (§2.7) is not established — whether the OF image region is claimable on real
   hardware, and what the loader's memory map loses by proceeding without it.
2. **The D7E1/D9 loader chain.** Apple's D7E1 instructions describe a two-volume design
   with a ModernOS Enabler and `ModernOSLoader.pef` [4]; only the D11E4 chain is traced
   here, and how the earlier builds boot is outside the corpus's binaries.
3. **The debugger dependency.** Whether D11E4's kernel genuinely requires the attached
   debugger or the two-machine text is conservative (§4.3) is unresolved: Apple's own
   hedge, single-machine boots reported on real hardware [14], and the kernel's no-timeout
   serial wait (§3.3) are all consistent with either reading.
4. **The RAM ceiling.** Community reports place a "No kernel stacks available!" failure
   above roughly 32–40 MB [14], while Apple's tested matrix includes 48 MB [3]; the
   kernel's stack-pool sizing is not analyzed and the true ceiling is unestablished.
5. **The CPU restriction.** The 601/604-only, no-603/603e/604e claim is community-attested
   [14]; Apple's lists name only 601 and 604 machines [1], and no Apple document in the
   corpus states the restriction or its mechanism.
6. **The Gestalt IDs of the installer rules.** Rules 200/213 test `('mach')` values 75,
   100, 112, 47, 65, 55, 40 and 108, 68, 69, 67 [12]; the mapping from those values to the
   models of §1.3 is inferred from list membership, not established from Gestalt
   documentation.
7. **The `'nkld'` and `'ofpt'` resources.** The 1,840-byte `'nkld'` loader's relocation
   and start protocol, and the role of every `ofpt` patch resource (IDs 1–4, 128–131,
   4177, −14, −15), are not decoded; neither is the 8,192-byte `'nvrm'` NVRAM image.
8. **The NVRAM layout.** Community reports place this build's Open Firmware variables at
   NVRAM offset 0 rather than the usual `$1800` [14]; the corpus holds the `'nvrm'` image
   but it is not analyzed.
9. **The AV and DOS cards, and the PowerSurge NVRAM Fixer.** Apple's list marks every AV
   variant "no AV functionality" [1], and the DDK carries a "PowerSurge NVRAM Fixer" for
   the PCI machines [5]; neither the boot's interaction with an AV/DOS card nor the
   fixer's function is traced (the recorded bring-up ran with empty slots).
10. **The 7.5.x interaction.** Community practice removes the "System 7.5 Update" file
    before Caps-Lock booting [14], while rule 210 accepts any 7.5.x [12]; which
    boot-time behaviour differs is not established.
11. **The boot image's full membership.** `System Libraries` (275 files) and `Hardware
    Support` (58 files) are only partly identified and mapped [7]; the closure of
    fragments the loader prepares (§2.6) is not enumerated.
12. **The Open Firmware SCSI driver.** The loader reads the boot volume through the OF
    client interface, so the disk-based OF contains a working HFS reader and SCSI driver
    (§2.4); neither's bus-level protocol is reverse engineered here.
13. **Kernel serial output.** The kernel is observed to print nothing on the serial
    console unless the nub is engaged (§3.9); whether any non-nub kernel path ever prints
    there is not systematically established.
14. **Task naming.** Kernel tasks carry four-character names (`'BSDX'`, §3.1); which
    component assigns them is not decoded.

## References

1. Apple Computer, Inc., "How to Install Mac OS 8 (D11E4)", last revision 6/13/96 —
   installation, Drive Setup and debugger instructions; the "Hardware Supported" list;
   troubleshooting notes (`%GLURG%`, "No Mac OS 8 Mount", "Enable Screen Saver",
   desktop-printer steps, debugger hotkeys). On the Mac OS 8 Driver Development Kit v0.4
   disc under `Mac OS 8 Runtime SW`.
2. Apple Computer, Inc., "Mac OS 8 (D11E4) Release Notes" — sections cited: MicroKernel,
   I/O, Installer Script, Finder, Folder Manager, Process Manager, Communications Toolbox,
   High-Level Toolbox, Mac OS 8 StdCLib, System Logging. Same disc.
3. Apple Computer, Inc., "Essential Information About This Release of Mac OS 8 DDK"
   (the DDK 0.4 read-me), an appendix to the Prototype License and Confidentiality
   Agreement, effective date June 10, 1996 — the release's position in the DDK series, the
   tested-configuration matrix, the driver-family model, the applications/drivers address
   space separation, the single-interrupt-line note.
4. Apple Computer, Inc., "Installing the D7E1 Build", instruction sheet distributed with
   the D7E1 (Scarecrow) developer build — the two-volume scarecrow design, the ModernOS
   Enabler and `ModernOSLoader.pef`, the hard external-debugger requirement, the Caps
   Lock gate, "No Scarecrow Mount", the volume-corruption and re-blessing warnings, the
   theme-rename note.
5. Mac OS 8 Driver Development Kit v0.4 CD-ROM, disc specimen (June 1996) — on-media
   structure as listed in §4.1; the `'kajr'`/`'kajr'` identity of "Install Mac OS"; the
   recorded entry-by-entry comparison of the two circulating disc images.
6. Recorded installation differential of Mac OS 8 D11E4 onto a Drive-Setup-formatted
   230 MB target volume, 2026 — the block-by-block image comparison before and after the
   installer run; the three changed regions; the partition-map entry fields; the patched
   boot-block code and its Caps Lock gate.
7. Recorded bring-up investigation of the Mac OS 8 D11E4 boot on the Power Macintosh
   7100/66, 2026 — the loader-chain reverse engineering (boot blocks, `'nkld'`, Open
   Firmware, `BootMacOS`, the SecondaryLoader Forth, the tertiary loader), the measured
   load addresses, the recovered traceback symbols, the kernel exception-ladder and panic
   analysis, the NuBus probe record, the HMC bit-33 measurement, and the observed Finder
   desktop.
8. Recorded reverse engineering of the Mac OS 8 SWIM3 floppy driver
   (`Hardware Support/BSFloppyPDM`), 2026 — annotated disassembly (5,953 instructions, 86
   recovered function names), the TOC anchors and device base, `SwimIIISmallWait`'s timer
   poll, `HALISRHandler`'s register contract, the import-stub glue.
9. Recorded reverse engineering of the Mac OS 8 ADB/Cuda input stack, 2026 — the
   CudaLib → ADBCudaPlugin → ADBServer → InputDevADBMouse call chain, the per-address
   autopoll ring, the open-address mask, device movement and handler-ID semantics.
10. Recorded analysis of the Mac OS 8 D11E4 application-launch hang, 2026 — the HFS
    catalog date validator, the low-memory `Time` global, and the Cuda one-second tick
    contract.
11. Annotated disassembly of the `Mac OS Loader` data fork (the tertiary loader) with
    recovered PowerPC traceback symbols, 2026 — 345 function names and the complete
    `TertiaryLoaderMain` call sequence.
12. Recorded analysis of the `'inrl'` installation-rule resources of the D11E4
    "Install Mac OS" installer script, 2026 — the rule table of §4.2, its constants and
    error texts, read off the DDK disc's resource fork.
13. Recorded investigation of Apple-branded SCSI drives and the Drive Setup drive gate,
    2026 — INQUIRY identity strings, READ CAPACITY block counts, MODE SENSE pages `$04`,
    `$03` and the Apple vendor page `$30`.
14. Recorded community and historical research on the Mac OS 8 developer releases, 2026 —
    build history (D7E1, D9, D11E4), the cancellation history, the debugger discrepancy,
    real-hardware RAM and CPU constraints, version strings and the disc's circulation;
    cited only where marked, never as sole authority for a technical claim.
