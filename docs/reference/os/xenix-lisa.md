# SCO XENIX 3.0 for the Apple Lisa 2

**Contents:**

1. [Scope & identity](#1-scope--identity) — what XENIX for the Lisa is, its ancestry and version; the
   distribution media set; the supported-hardware matrix; machine identification and keyboard
   conventions inside the OS
2. [Boot architecture](#2-boot-architecture) — the two-layer design: the Lisa boot ROM's device
   selection, then the XENIX boot program; the boot program's reverse-engineered anatomy; the
   kernel load and its placement; kernel startup, the root device and the `iinit` panic; the MMU in
   the boot path; the kernel's bus-error machinery
3. [Hardware interface](#3-hardware-interface) — the floppy controller as the boot program drives it;
   keyboard and mouse through COPS; the parallel hard-disk driver; exceptions and the group-0 frame;
   the console
4. [Installation & bring-up](#4-installation--bring-up) — the retail media set; bring-up requirements;
   the documented six-step installation; the 2.3-to-3.0 upgrade path; what the recorded bring-up
   exercised
5. [Observed behaviour & quirks](#5-observed-behaviour--quirks)
6. [Open questions](#6-open-questions)

References

---

## 1. Scope & identity

### 1.1 What XENIX for the Lisa is

XENIX is Microsoft's Unix-family operating system, and the port to the Lisa 2 — marketed and
distributed by The Santa Cruz Operation (SCO) under a joint copyright ("©The Santa Cruz Operation,
Inc., 1984 / ©Microsoft Corporation, 1983") [1] title page — is the Lisa's multiuser operating
system: the installation guide opens by describing it as "a powerful multiuser, multitasking system
of programs" [1] §1.1. The same paragraph states the defining architectural property: XENIX "takes
the place of your existing Operating System" [1] §1.1. Nothing of the Lisa's resident environment
survives into it — contrast [A/UX](aux.md), which boots *through* the Macintosh ROM and System
Software and keeps the Macintosh environment running as processes inside the Unix system
([aux.md §2.1](aux.md)). XENIX for the Lisa is the opposite shape: the boot ROM loads a XENIX boot
program, the boot program loads a Unix kernel, and the kernel owns the machine. That the Lisa's
other shipping operating systems (the Lisa Office System, MacWorks XL) are neither multiuser nor
Unix makes XENIX the machine's only multiuser Unix; that claim is *inferred from the machine's
software catalogue*, not from any document in the corpus.

This page documents XENIX 3.0 for the Lisa 2, the version the primary-evidence corpus covers: the
1984 SCO/Microsoft installation guide [1], the recorded bring-up investigation of the 3.0 boot on a
Lisa 2/5 — the reverse-engineered boot program, the verified kernel load, the kernel-startup
observations and the `hdinit`/`mkfs` analysis [2] — and the retail distribution media set [3].
Claims are marked *observed* (with the evidence class) or *inferred — unverified*; what the corpus
does not establish is collected in [§6](#6-open-questions). The evidence base is honest about its
own shape: the corpus contains the installation guide and recorded boot behaviour, but no XENIX
manual set, no kernel source, no kernel disassembly and no completed installation; every statement
below is bounded by that.

### 1.2 Version, media and dating

| Item | Value | Evidence |
|---|---|---|
| Evidenced version | XENIX 3.0 (kernel banner `SCO XENIX V3.0`) | [2] (*observed*) |
| Distribution form | 17 single-sided 400 KB Sony disks (800 blocks per side) | [3] (*observed*); disk geometry [fdc](../machines/lisa/fdc.md) §4.5 |
| Guide release line | `68-5-24-84-1.0/1.0` | [1] title page |
| Copyright | SCO 1984, Microsoft 1983 | [1] title page |
| Distribution systems | Operating System, Text Processing System, Development System — "You install each package separately"; all write-protected "except the Boot floppydisk of the Operating System" | [1] §1.2 |
| Kernel memory report | `System 122k User 1830k` (banner line, on a 2 MB machine) | [2] (*observed*) |
| Prior version | XENIX 2.3, evidenced only by the 3.0 upgrade path ("Use 'upgrade' to upgrade 2.3 to 3.0 xenix") | [1] §1.5.1, §1.7 |

The 17-disk retail set [3] (*observed*):

| Disk | Count | Role |
|---|---|---|
| "Xenix 3.0 Boot" | 1 | the boot program and the boot-floppy environment [1] §1.5.1 |
| "Xenix 3.0 Boot XProFile Patch" | 1 | a boot variant whose ProFile driver differs (§5.5) |
| "Xenix 3.0" 1–7 | 7 | the Operating System distribution copied by `firsttime` [1] §1.5.4 |
| "Development System" 1–4 | 4 | the Development System distribution [1] §1.5.4 |
| "Text Processing System" 1–4 | 4 | the Text Processing System distribution [1] §1.5.4 |

The guide never mentions the "XProFile Patch" disk; it appears only in the media set, and its
observed difference from the plain Boot disk is the ProFile command frame of a driver (§5.5). The
guide also dates nothing beyond the release line; the kernel's Unix lineage (System V or earlier)
is not stated by any document in the corpus ([§6](#6-open-questions), item 2).

### 1.3 The supported-hardware matrix

The guide states the requirements directly [1] §1.3:

| Requirement | Value |
|---|---|
| Machine | "A Lisa computer with at least 512k bytes of memory" |
| Mass storage | either a 10 megabyte internal hard disk (the Lisa 2/10) or "at least one external 5 megabyte ProFile hard disk" |
| Lisa 2/5 | the first ProFile "must be connected to the built-in parallel port" |
| Second ProFile (Text Processing / Development) | "connected to the upper parallel port of a parallel expansion board installed in expansion slot 1" |
| Full system size | "The complete XENIX System (all three packages) requires about 7 megabytes of disk capacity" [1] §1.5.4 |

So the supported matrix is the Lisa 2/5 (external 5 MB ProFile on the built-in parallel port) and
the Lisa 2/10 (internal 10 MB disk), in both cases with at least 512 KB of memory. The recorded
bring-up of 3.0 runs on a third configuration entirely inside that envelope: a 2 MB Lisa 2/5 with
the revision "H" boot ROM and a blank 5 MB ProFile (9728 blocks) on the built-in port [2]
(*observed*). Whether the OS actually runs at the guide's 512 KB minimum is not established by the
corpus ([§6](#6-open-questions), item 8). For the hardware behind the matrix — the parallel-port
wiring of the first ProFile, the slot-1 expansion card that carries the second, and the internal
disk of the 2/10 — see [profile](../machines/lisa/profile.md) §2.6 (the expansion card's port
images), [profile](../machines/lisa/profile.md) §4.7 (the second drive's documented
configuration) and [lisa.md §14](../machines/lisa/lisa.md) (the parallel hard-disk subsystem
summary).

### 1.4 Machine identification and keyboard conventions

The kernel identifies the machine on its boot banner: the recorded 2/5 boot prints
`Lisa II/5 (s): ROM 00A8 / Slot 0-2 Empty` [2] (*observed*) — a machine line, a ROM
identification, and an expansion-slot inventory. The recorded machine runs the revision "H" boot
ROM (version word `$0248`); what the kernel's `ROM 00A8` value measures, and what the `(s)` suffix
denotes, are not decoded ([§6](#6-open-questions), item 3).

The keyboard conventions are the guide's [1] §1.4, and they are the Lisa's Apple key pressed into
Unix service:

| Action | Keys | Meaning |
|---|---|---|
| Delete last character | BACKSPACE, or APPLE + `h` | backspace is `^h` |
| Delete the line | APPLE + `u` | "erase line is `^u`" [1] §1.5.1 |
| Continue with normal startup | APPLE + `d` | the guide's "Type CONTROL-d to continue with normal startup" [1] §1.5.4 — on the Lisa, Apple-D is read as Control-D |
| Boot-device selection | APPLE + numeric-keypad ENTER, then APPLE + `3`, then APPLE + `2` | self-test icons → the numbered boxes → the peripheral menu → the floppy [1] §1.5.1 |

"If your keyboard does not have an APPLE key (a key with an apple on it), use the COMMAND key
instead" [1] §1.4 — the note that dates the guide to the transition-era keyboard stock. The
boot-device selection keystrokes of the last row are the boot ROM's, not XENIX's; they are
§2.1's subject.

## 2. Boot architecture

### 2.1 Two layers: the boot ROM under, the XENIX boot program over

XENIX does not replace the Lisa boot path; it rides it, exactly as the resident Lisa Office System
does. The full chain, assembled from the guide's user-visible steps [1] §1.5.1 and the recorded
bring-up [2]:

```
Power-on (the soft-power relay click [1] §1.5.1; COPS and soft power,
  lisa.md §11.6)
  → Boot ROM POST: checksum, MMU register test, memory sizing, memory test,
    VIA test, screen test, I/O-board tests (lisa.md §16.1)
  → self-test icons
  → [floppy boot] APPLE+ENTER, APPLE+3 (peripheral icon menu), APPLE+2 (floppy)
  → [hard-disk boot] no keys: the ROM auto-loads the XENIX boot program
    from the hard disk [1] §1.5.3 (the ROM's ProFile bootstrap read,
    profile.md §4.2)
  → the XENIX boot program from the device: the "boot :" prompt
  → RETURN: the command line fd(2,0)xenix (floppy) / pf(0,0)xenix (hard disk)
  → kernel load into logical memory at the bottom of segment 0 (§2.3)
  → kernel: banner, rootdev/swapdev line, root-device bring-up, iinit
  → <BootFloppy> single-user shell (boot floppy) / "Entering System
    Maintenance Mode" (hard disk) → hdinit / firsttime / passwd root / mkuser
```

The Lisa-side half of this chain is documented at machine level elsewhere in this tree and is
cited, not restated: the ROM's POST milestones are [lisa.md §16.1](../machines/lisa/lisa.md), the
floppy controller's boot sequence is [fdc](../machines/lisa/fdc.md) §4.4, the ProFile bootstrap
read is [profile](../machines/lisa/profile.md) §4.2, and the MMU map the ROM installs before
any of it runs is [mmu](../machines/lisa/mmu.md) §4.1. The sections below cover what is
specific to XENIX: the boot program, the kernel load, and the kernel's own start-up.

The guide's boot-device keystrokes are worth one sentence of interpretation: holding APPLE keys
while the numbered boxes are displayed selects the ROM's boot menu, and the numbered icons name
the devices — the floppy is selected with APPLE+`2` [1] §1.5.1. How those numbers map to the ROM's
device slots is not decoded by the corpus ([§6](#6-open-questions), item 9).

### 2.2 The XENIX boot program

The "boot program" the ROM loads is the XENIX-specific half of the bootstrap: it prints the
`boot :` prompt, reads a command line, and loads the kernel file the command names. The recorded
bring-up reverse-engineered it from its execution on the boot floppy [2] (*observed*); its
logical-address anatomy:

| Function | Address (logical) | Observed behaviour |
|---|---|---|
| Prompt poll loop | `$22D46`–`$22D50` | `MOVE.B $00FCDD9B,D0` (VIA1 IFR), `ANDI #$3` (CA1/CA2 — COPS data ready), spin |
| Read character | `$22D86` | `MOVE.B $00FCDD83,D0` (VIA1 port A — the COPS byte), then `MOVE.B #$3,$00FCDD9B` to acknowledge (clears CA1/CA2) |
| COPS byte decoder | state word at `$0002633E` | `$80` → 2-byte reset pair; `$00` → 3-byte mouse report (dx/dy stored at `$24FB2`/`$24FB3`); bit 7 set with low 7 bits non-zero → key |
| Key-to-ASCII table | base `$24BD6` | indexed by keycode − `$20`; keycode `$48` → `$0D` (RETURN) |
| Command handler | `$234A6` | dispatches the completed command line |
| RAM-sizing probe | `$2A0` | installs fault-recovery PC `$2CC` into `$10C8E`, walks memory in `$200` steps writing and verifying `$1234` until the write faults, records the RAM top into `$10C70`, clears the recovery slot |
| MMU trampoline | `$1E810` | `MOVEA.L (A7)+,A1`; `TST.B $00FCE010` (setup strobe on); `MOVE.W D0,(A0)` (descriptor write in START mode); `TST.B $00FCE012` (setup off); `JMP (A1)` — used to clear descriptors and to map segments |
| Self-relocation | `$400`–`$452` | relocates code into segment 122; `JSR (A2)` at `$452` jumps to `$F5FEA4` — segment 122, page `$FF` |

The VIA1 register addresses are the standard Lisa COPS channel — port A at `$FCDD83`, the IFR at
`$FCDD9B`, register stride 2 — documented in [lisa.md §10.1](../machines/lisa/lisa.md), with the
CA1/CA2 strobe and acknowledge semantics in [cops](../machines/lisa/cops.md) §3.2. The COPS
byte classes the decoder separates are the standard response stream — reset/status pairs, mouse
triplets, key events ([cops](../machines/lisa/cops.md) §2.3–§2.6) — with the key code set of
[cops](../machines/lisa/cops.md) §4.7.

The RAM-sizing probe is the boot program's own, distinct from the ROM's POST memory sizing: it
installs a fault-recovery PC in a low-memory slot and walks the address space until a write
faults, which requires that a bus error during the probe can be caught and vectored in the
already-mapped environment (§3.4). The MMU trampoline wraps every descriptor write in the START
strobe pair — the setup-mode bypass the MMU provides for exactly this purpose
([mmu](../machines/lisa/mmu.md) §3.5; [lisa.md §4.7](../machines/lisa/lisa.md), §6.1) — so
the boot program reprograms segments from translated code.

Two boot programs exist: the floppy's (loaded by the ROM's floppy boot sequence,
[fdc](../machines/lisa/fdc.md) §4.4) and the hard disk's, installed by `hdinit` ("Installing
hard disk Boot..." [1] §1.5.2). Both reach an identical `boot :` prompt; the recorded comparison of
the plain and XProFile-Patch boot floppies shows the two disks' *boot programs* are identical and
the difference is in the kernel-side driver the loaded kernel carries [2] (*observed*).

### 2.3 Kernel load and placement

At the bare prompt, RETURN is a complete command: the boot program echoes `: fd(2,0)xenix` [1]
§1.5.1 and loads the kernel file. The recorded load [2] (*observed*):

1. The boot program reads the floppy starting at block 0 — the first bytes of the first sector
   read are `46 FC 27 00 2E 7C 00 02`, matching the media image byte-for-byte — then reads the
   kernel file, 394 sector reads from track 22 sector 3 onward, every read verified byte-exact
   against the media image.
2. The kernel image is placed at the bottom of logical memory: the running kernel's program
   counters are observed in segment 0 (e.g. `$010070`, `$00058A` while it reads the root
   superblock) [2] (*observed*) — the 122 KB "System 122k" of the banner fits inside segment 0's
   128 KB. Segment 0 is inside the region the boot ROM's POST-installed map already covers
   ([mmu](../machines/lisa/mmu.md) §4.1 — the ROM maps the installed RAM contiguously onto the
   low segments), so no remap is needed to start the kernel.
3. The kernel load region — logical `$0`–`$1E800` — **overlaps the boot program's own variable
   block at `$10C70`–`$10C98`** (the RAM-top variable at `$10C70`, the probe's fault-recovery PC
   slot at `$10C8E`); the copy overwrites those variables with kernel text [2] (*observed*). The
   boot program must not revisit them after the copy — the overlap is structural, on the media and
   in the loader, and is §5.2's subject.

The `fd(2,0)` of the command line names the floppy, and `pf(0,0)xenix` names the hard-disk copy
([§1.5.3]) — the two numbers of the tuple are not decoded by the corpus ([§6](#6-open-questions),
item 5).

### 2.4 Kernel startup: banner, root device, iinit

Once loaded, the kernel prints its banner and brings up its root device. The recorded 2/5 boot
[2] (*observed*):

```
SCO XENIX V3.0
Copyright Microsoft Corporation and The Santa Cruz Operation Inc, 1983. ...
Lisa II/5 (s): ROM 00A8 / Slot 0-2 Empty
rootdev 1 1 swapdev 0 0 / System 122k User 1830k
pf0 not on line
panic: iinit
```

The startup order the message sequence establishes:

1. **Banner and configuration lines** — version, joint copyright, machine identification
   (§1.4), the compiled-in root and swap device (`rootdev 1 1 swapdev 0 0`), and the kernel/user
   memory split.
2. **Root-device bring-up** — the root device is the first hard disk, `pf0`. With no disk on line,
   the kernel prints `pf0 not on line` and panics in `iinit` [2] (*observed*). The guide documents
   the same message from the installer's side: `hdinit` "displays the message `drivename not on
   line` when a profile disk is not connected to the proper parallel port or the disk's power is
   not on", with `drivename` "e.g. pf0 or pf2" [1] §1.5.2 — the user-visible face of the
   bring-up failure.
3. **The boot-floppy environment** — with a ProFile on line, the kernel reaches the boot-floppy
   single-user shell, prompt `<BootFloppy>`, over the help text:

   ```
   Lisa XENIX V3.0  Boot Floppy
   (backspace is ^h, erase line is ^u)
   Use "hdinit" to initialize hard disk.
   Use "upgrade" to upgrade 2.3 to 3.0 xenix
   ```

   [1] §1.5.1 [2] (*observed*).
4. **The hard-disk environment** — booted from the hard disk (`pf(0,0)xenix`), the kernel prints
   its copyright and "Entering System Maintenance Mode", offers `TERM = (lisa)`, clears the
   screen, prints "Terminal type is lisa" and a `#` prompt, and "immediately begins to execute the
   firsttime program" [1] §1.5.3 (§4.3).
5. **The startup gate** — on a fully installed system, the kernel offers
   "Type CONTROL-d to continue with normal startup: / (Type the root passwd to enter system
   maintenance)" [1] §1.5.4 — single-user maintenance versus multiuser normal startup, the
   classic Unix gate, with the Lisa's Apple-D standing in for Control-D (§1.4).

### 2.5 The MMU in the boot path

The Lisa's segment MMU — no Motorola PMMU, a custom paged-segment translator with 128 KB
segments, per-segment SOR/SLR descriptors and four contexts ([mmu](../machines/lisa/mmu.md) §1)
— is central to the XENIX boot at three points:

1. **The inherited map.** The boot ROM's POST installs the default map — the installed RAM
   contiguous onto the low logical segments, I/O at logical `$FCxxxx`, the ROM at `$FExxxx`
   ([mmu](../machines/lisa/mmu.md) §4.1, §4.2). On the recorded 2 MB machine the observed
   POST-installed table covers logical `$0`–`$180000` (segments 0–11, all installed RAM from its
   high physical base), with the segments above left at the invalid pattern [2] (*observed*).
   The kernel starts inside the covered region (§2.3).
2. **The boot program's self-relocation.** Before loading the kernel, the boot program relocates
   part of itself into **segment 122**, writing SOR `$387` / SLR `$400` into the descriptor
   ([mmu](../machines/lisa/mmu.md) §2.2, §2.3): access code `$4` — main memory, read-only,
   **stack** ([mmu](../machines/lisa/mmu.md) §2.3.1) — with length byte `$00`, which for a
   stack segment means **one valid page, the top page** of the segment
   ([mmu](../machines/lisa/mmu.md) §2.3.2). The `JSR (A2)` at logical `$452` jumps to
   `$F5FEA4` — segment 122, page `$FF`, exactly that top page [2] (*observed*). The relocation
   therefore depends, byte-for-byte, on the stack-segment length convention; an implementation
   that reads length `$00` as maximum length (the normal-segment convention) faults the fetch
   ([§5.3](#53-the-boot-programs-segment-122-relocation-and-the-stack-window)).
3. **The kernel's own mapping.** The kernel takes the MMU over from the boot program: it keeps
   its own 122 KB in segment 0, and runs user processes in separately-mapped segments — the
   recorded user fault of §3.4 lands in **segment 63, context 1**, with the segment's SLR
   `$604` (access code `$6`: main memory, read/write, stack; length byte `$04` → the top five
   pages `$FB`–`$FF` valid) [2] (*observed*). The user stack grows downward toward `$800000`,
   the kernel's user-stack ceiling, and the growth is **demand** growth: the fault that reaches
   the kernel's bus-error handler asks for the next page below the current window, and the
   kernel decides ([§3.4](#34-exceptions-the-group-0-frame-and-demand-stack-growth)).

### 2.6 The kernel's trap machinery

The kernel installs its own exception handling over the Lisa's exception vector table
([lisa.md §7.2](../machines/lisa/lisa.md) — vectors at logical `$0`–`1FF). The recorded
disassembly path of a user-mode bus error [2] (*observed*):

| Step | Address (logical) | Role |
|---|---|---|
| Vector 2 stub | `$504` | entry from the bus-error vector; saves the fault frame into low memory at `$10C7E`; `RTE` chain through `$538` → `$C8` |
| Dispatch table | `$C8` | BSR-table dispatch on the vector index |
| Trap dispatcher | `$4AA` | common trap entry, vector index 2 in hand |
| `trap()` | `$24B8` | classifies the trap; user path `$25F4` |
| Bus-error case | `$2730` | calls the fault resolvers `$14DC`, `$1536`, `$2366` in order; on failure delivers signal `$0B` (SIGSEGV) at `$263A` |
| Resolver 1 | `$14DC` | the stack-growth probe recognizer (§3.4) |
| Resolver 2 | `$1536` | not decoded ([§6](#6-open-questions), item 10) |
| Resolver 3 | `$2366` | the stack-grow test: grow iff `fault_addr < ($800000 − (u_ssize at $F00166) << 9)` |

The shape — a bus-error handler that inspects the faulting instruction in the frame, adjusts for
its prefetch advance, and either repairs the mapping or signals the process — is the same
contract the resident Lisa OS implements in its own `BUS_ERR` demand-load handler
([mmu](../machines/lisa/mmu.md) §4.6, whose witness is the released Lisa OS source [4]).
XENIX's kernel simply implements it for a Unix: the repair path grows a user stack, and the
failure path is a Unix signal.

## 3. Hardware interface

### 3.1 Floppy: the 6504 window as the boot program drives it

The boot program reads the kernel through the 6504 controller's 68000-visible window. The
window's geometry is the Lisa's odd-byte arrangement: the 6504's 1 KB shared RAM appears to the
68000 at odd addresses only — 6504 address *n* at `$C001 + 2n` — and "the even bytes in
$C000–$C7FF are not backed by the controller's RAM" ([fdc](../machines/lisa/fdc.md) §1.4).
Apple's own drivers use byte accesses and `MOVEP`; the XENIX boot program instead issues
**word and longword operations based at the even address `$00FCC000`**, with the odd bytes of
each operand carrying the controller data [2] (*observed*). Both conventions address the same
bytes; the recorded load is byte-exact throughout, so the controller sees the intended command
blocks and data either way. The enumeration the boot program performs before reading polls the
controller's disk-in status and requires the full byte `$FF` [2] (*observed*; the status block is
[fdc](../machines/lisa/fdc.md) §2.4).

The media are single-sided 400 KB disks — 800 blocks per side, the Sony geometry of
[fdc](../machines/lisa/fdc.md) §4.5 — and the boot program's reads follow the same
block-to-(side, track, sector) conversion the resident OS driver uses (*inferred from the
recorded read pattern*; the boot program's own conversion table is not disassembled).

### 3.2 Keyboard and mouse: COPS at the prompt

The `boot :` prompt is a COPS-keyboard application. The boot program polls VIA1 for the COPS
data-ready strobe and reads the bytes from port A (§2.2), then runs the byte-class decoder
over the stream: reset/status pairs, mouse triplets, and key events — the standard response
stream classes ([cops](../machines/lisa/cops.md) §2.3–§2.6). Two XENIX-specific observations
[2]:

- **Mouse reporting is enabled.** The guest issues COPS command `$7C`, after which mouse delta
  packets interleave with key bytes in the stream the decoder consumes [2] (*observed*). The
  exact decode of `$7C` in the COPS command set (the command byte format is
  [cops](../machines/lisa/cops.md) §2.2) and what the boot program does with the deltas are
  not established ([§6](#6-open-questions), item 13).
- **Key auto-repeat is in software.** A key held down — the down scancode not followed by its
  up — repeats [2] (*observed*), so the keyboard input layer owns repeat, not the COPS.

The COPS keycode-to-ASCII translation is the boot program's own table at `$24BD6`
(keycode − `$20` indexed, keycode `$48` → RETURN) [2] (*observed*), a table of the same shape as
the ROM's ([cops](../machines/lisa/cops.md) §4.7).

### 3.3 Parallel hard disk: the pf driver

The kernel's hard-disk driver `pf` drives the ProFile over the parallel port through the
standard five-beat handshake and six-byte command frame ([profile](../machines/lisa/profile.md) §3.1,
§3.2). The drives are named `pf0` — the first ProFile, on the built-in port — and `pf2` — the
second, on the slot-1 expansion card's upper port ([profile](../machines/lisa/profile.md) §4.7)
[1] §1.5.2.

The recorded `hdinit` run issues **249 ProFile transactions — 246 writes and 3 reads** — across
its "Making file system" and boot-block-install phases [2] (*observed*). The write command frame
observed on the wire [2]:

```
01 00 1E 40 0A 04     write, block $001E40 (7744), retry count 10, sparing threshold 4
```

The block number matches the kernel's own error report naming the same transaction —
`error on dev pf (0/0) block=7744 cmd=1` [2] (*observed*) — and the frame follows the documented
layout: command `$01` (write), 24-bit block number, retry count, sparing threshold
([profile](../machines/lisa/profile.md) §3.2). Apple's released drivers use sparing
threshold 3; the XENIX driver's observed 4 is a small divergence worth noting but nothing more
(*observed* [2]).

The retail Boot disk's driver sends a **different frame** — four bytes and a re-asserted command
strobe — which the controller never accepts; that failure, and the XProFile Patch disk that fixes
it, is [§5.5](#55-the-truncated-profile-command-and-the-xprofile-patch-disk).

### 3.4 Exceptions: the group-0 frame and demand stack growth

The kernel's most hardware-sensitive contract is its recognition of the compiler's stack-growth
probe. The recorded instance, from the `mkfs` run inside `hdinit` [2] (*observed*):

1. The user process executes `TST.B -$90(A7)` — opcode word `$4A2F`, displacement `$FF70` — at
   user PC `$801076`, with `A7 = $7FF4EE`; the access faults at logical `$7FF45E`.
2. The fault is genuine demand growth: the access is in segment 63 (context 1), one page below
   the segment's valid stack window (SLR `$604`, pages `$FB`–`$FF` valid — §2.5).
3. The 68000 group-0 frame reaches the kernel's vector-2 stub; the dispatch chain of §2.6 runs
   the bus-error case.
4. Resolver `$14DC` reads the word at **(saved PC − 2)** and compares it against **`$4A2F`** [2]
   (*observed*). Recognition therefore requires the frame's saved PC to be the faulting
   instruction's opcode address **plus 2** — the 68000's stacked PC for a faulted operand access,
   past the opcode word. With any other saved-PC convention the probe is not recognized, the
   resolvers fail, and the kernel delivers SIGSEGV — the recorded failure mode kills `mkfs` with
   `Memory fault - core dumped` [2] (*observed*).
5. On recognition, the kernel grows the segment: resolver `$2366`'s test decides, against the
   process's recorded stack size (`u_ssize`, at the kernel global `$F00166`) and the `$800000`
   ceiling (§2.5), and the mapping extends the window one page down.

The parallel with the resident OS is exact and worth stating: the Lisa OS's own `BUS_ERR`
demand-load handler also inspects the faulting instruction in the group-0 frame, adjusts the
saved PC by per-opcode prefetch amounts, and carries a dedicated TST stack-overflow path
([mmu](../machines/lisa/mmu.md) §4.6, witness [4]). Two operating systems written
independently for this machine converge on the same exception-frame contract — that is the
strongest evidence the contract is the hardware's.

### 3.5 Console

The console is the built-in video display, and the system's terminal type is the machine's own
name: the maintenance-mode exchange is `TERM = (lisa)` / "Terminal type is lisa" [1] §1.5.5, and
`/etc/getty` style console bring-up is implied by the respawn of a login over it [1] §1.5.5–§1.5.6.
The video hardware is [lisa.md §8](../machines/lisa/lisa.md); how the kernel drives the display
(byte order, scroll, cursor) is not traced by the corpus ([§6](#6-open-questions), item 11).

## 4. Installation & bring-up

### 4.1 The retail media set

The retail set is the 17 disks of §1.2 plus the target hard disk. The guide's own inventory is
the three distribution systems (Operating System, Text Processing, Development), each installed
separately, the Operating System required before the others, and all write-protected except the
Boot floppy [1] §1.2. The recorded media specimen [3] (*observed*) adds the undocumented piece:
the "Xenix 3.0 Boot XProFile Patch" disk, a second boot floppy whose kernel-side ProFile driver
differs from the plain Boot disk's (§5.5).

### 4.2 Bring-up requirements

| Requirement | Value | Evidence |
|---|---|---|
| Memory | at least 512 KB | [1] §1.3 |
| Hard disk | internal 10 MB (Lisa 2/10) or external 5 MB ProFile (Lisa 2/5, built-in parallel port) | [1] §1.3 |
| Second ProFile (Text Processing / Development only) | slot-1 expansion card, upper parallel port | [1] §1.3, [profile](../machines/lisa/profile.md) §4.7 |
| Full three-system install | ~7 MB of disk | [1] §1.5.4 |
| Power-on order | disks first: "Turn on the power to each hard disk", wait for the "ready" light to "glow a steady red", then the computer | [1] §1.5.1, §1.5.4 |
| Recorded configuration | Lisa 2/5, 2 MB, revision "H" boot ROM, blank 5 MB ProFile (9728 blocks) on the built-in port | [2] (*observed*) |

The recorded bring-up proves the 2 MB / 5 MB ProFile end of the envelope; the guide's 512 KB
minimum and the Lisa 2/10's internal disk are documented but not recorded ([§6](#6-open-questions),
items 8 and 15).

### 4.3 The documented six-step installation

The guide's procedure [1] §1.5, condensed to its milestones:

1. **Start from the Boot floppy** — power-on, APPLE+ENTER / APPLE+`3` / APPLE+`2` (§2.1), insert
   the Boot disk, RETURN at the `boot :` prompt → `fd(2,0)xenix` → the `<BootFloppy>` shell [1]
   §1.5.1.
2. **`hdinit`** — at the `<BootFloppy>` prompt, type `hdinit`. The program warns "This
   installation program will destroy the present contents of your hard disk" and asks
   `Do you want to continue <y/n>?`; `n` aborts to "Normal System Shutdown". It then asks
   "Enter size of hard disk (5 or 10)" — `5` for a Lisa 2/5's ProFile, `10` for a Lisa 2/10 —
   makes the file system, runs `fsck` (Phases 1–5, ending "XX files XXX blocks XXXX free" on
   `/dev/root`), copies the hard-disk boot program ("Installing hard disk Boot..."), and halts
   with "Disk initialization is complete." / "Normal System Shutdown" [1] §1.5.2.
3. **Start from the hard disk** — power-cycle; the ROM auto-loads the XENIX boot program from
   the disk; type `pf(0,0)xenix`; the kernel prints "Entering System Maintenance Mode" and
   immediately runs `firsttime` [1] §1.5.3.
4. **`firsttime`** — optionally makes a `/usr` file system on the second ProFile
   ("Do you want /usr to be on a second profile <y/n>?"), then copies the numbered distribution
   floppies in order ("First Floppy <y/n>?" / "Next Floppy <y/n>?", with `tar: please mount new
   volume, then press RETURN` as the swap prompt), reports "checking ownerships and permissions
   for XENIX run-time system ..." as initialization files execute, offers the Development and
   Text Processing distributions, makes `/lost+found`, and ends with "Xenix Installation
   complete." / "Normal System Shutdown" [1] §1.5.4.
5. **Super-user password** — reboot, enter maintenance mode, `passwd root`; the password "must
   be at least 5 characters long" [1] §1.5.5.
6. **First user account** — `mkuser`, creating the `guest` account [1] §1.5.6.

The recorded bring-up exercises steps 1 and 2 end-to-end [2] (*observed*): the `hdinit` prompts
accept `y` and `5` from the keyboard, the 249 ProFile transactions of §3.3 run, the file system
is made and checked — "/dev/root ** Phase 1–5 ... 54 files 609 blocks 6890 free" — the
hard-disk boot program is installed, and the run ends at "Disk initialization complete." with
the shutdown [2]. Steps 3–6 are documented but not recorded ([§6](#6-open-questions), item 12).

### 4.4 The upgrade path: XENIX 2.3 to 3.0

The guide carries a parallel procedure for existing 2.3 installations [1] §1.7: boot the 3.0 Boot
floppy, run `upgrade` instead of `hdinit` ("converting file system to 3.0 ..."), which converts
the on-disk file system, copies essential files, "preserves your /etc/passwd as /etc/passwd.SAVE",
and installs the new hard-disk boot program; then boot from the hard disk, back up local files
(`tar`, with blank disks formatted by `/diskutil -f /dev/rfd`), run `/secondtime` — the upgrade's
counterpart of `firsttime`, including the second-ProFile `/usr` conversion — and finally restore
the files, merging old and new `/etc/passwd` by hand [1] §1.7.3–§1.7.6. The existence of the 2.3
release is evidenced only by this path.

### 4.5 What gets installed

From the guide: a bootable hard disk carrying the kernel file `pf(0,0)xenix` names, the root
file system `hdinit` made on it, the copied distribution files under `/` and (optionally) `/usr`
on the second ProFile, the hard-disk boot program at the block the ROM's bootstrap read expects
([profile](../machines/lisa/profile.md) §4.2), `/lost+found`, a root password and the `guest`
account [1] §1.5.2–§1.5.6. The on-disk file system's format — beyond `/dev/root` as its name and
the `fsck` phase names — is not evidenced ([§6](#6-open-questions), item 7).

## 5. Observed behaviour & quirks

### 5.1 The COPS reset pair misclassified at the prompt

The boot program's COPS decoder classifies a received byte by sign-extended comparison against
`$80`, and the comparison as written never matches a real `$80` byte [2] (*observed*): the byte
is sign-extended to a longword (`$FFFFFF80`) before a compare against the literal `$00000080`.
The consequence is behavioural: a COPS reset response pair — `$80`, `$3F`
([cops](../machines/lisa/cops.md) §2.4) — arriving at the prompt is not consumed as the
two-byte reset sequence but as a mouse-report marker followed by a phantom dx byte, which leaves
the decoder's state machine one byte into a three-byte mouse packet. The next genuine key byte
is then swallowed as the packet's dy [2] (*observed*). The boot ROM issues two COPS resets
during POST, and the second reset's response pair is the observed source of the stray bytes at
the prompt [2] (*observed*). Whether the stray pair reaches the decoder on every real machine —
the timing of the ROM's drain versus the boot program's first read decides it — is open
([§6](#6-open-questions), item 4).

### 5.2 The kernel copy clobbers the boot program's variables

The kernel image's load region (logical `$0`–`$1E800`) and the boot program's variable block
(`$10C70`–`$10C98`, holding the RAM-top variable and the RAM-probe fault-recovery PC slot)
overlap (§2.3) [2] (*observed*). The kernel copy overwrites the variables with kernel text —
at the corresponding image offset the bytes read `$001D001D` [2] (*observed*). After the copy
the boot program's low-memory state is destroyed by design, and a post-copy bus error whose
recovery path re-reads the clobbered slot jumps to `$001D001D` — an odd address in an unmapped
segment — and the machine loops in its bus/address-error handler [2] (*observed*, as the
recorded investigation's diagnostic path). The re-implementation lesson is the structural fact:
the loader's recovery-PC indirection and the kernel's load address share memory, and only the
load's ordering makes the combination work.

### 5.3 The boot program's segment-122 relocation and the stack window

The boot program's self-relocation into segment 122 (§2.5) is a one-page stand on the MMU's
stack-length convention: SOR `$387`, SLR `$400` (read-only stack), length byte `$00`, and the
relocated code occupies exactly page `$FF`, the segment's top page [2] (*observed*). The
hardware's stack-window rule — a stack segment's valid pages are the top *L* pages, growing
downward, with the length adder's carry-in inverted for stacks
([mmu](../machines/lisa/mmu.md) §3.2, §2.3.2) — is what makes page `$FF` valid at length
`$00`. An implementation that applies the normal-segment convention (`$00` = maximum length,
window from the bottom) instead faults the relocated fetch, and the boot stops before the
kernel loads [2] (*observed*, as the recorded investigation's diagnostic path). The XENIX boot
program is, in effect, a test of that one bit of MMU semantics.

### 5.4 The group-0 frame's saved PC and the $4A2F probe

The kernel's demand stack growth depends on the 68000 group-0 exception frame carrying the
faulting instruction's opcode address **plus 2** — the resolver reads `(savedPC − 2)` and
compares against the probe opcode `$4A2F` (§3.4) [2] (*observed*). Any other saved-PC value —
the next instruction's address, or the faulting instruction's own address — reads the wrong
word, the probe goes unrecognized, and the process dies with SIGSEGV instead of growing its
stack [2] (*observed*). This is the Lisa counterpart of the bus-error restart semantics a
later Apple Unix depends on ([aux.md §5.4](aux.md)): same machine family, same class of
hardware contract, one generation apart.

### 5.5 The truncated ProFile command and the XProFile Patch disk

The retail "Xenix 3.0 Boot" disk's ProFile driver sends a **truncated four-byte command frame**
— `$01 00 1E 40` (command, block number, then nothing) — and re-asserts the command strobe [2]
(*observed*). The documented frame is six bytes ([profile](../machines/lisa/profile.md) §3.2);
the controller never presents the state the truncated driver sequence expects (the recorded
handshake runs state `$01`, then `$03`, where a complete command yields `$02`), the driver's
error path reports `error on dev pf (0/0) block=7744 cmd=1`, and the disk cannot be brought on
line [2] (*observed*). The "Xenix 3.0 Boot XProFile Patch" disk carries the fix: its driver
sends the full six-byte frame — `$01 00 1E 40 0A 04` — the handshake completes, and the same
installation proceeds to completion on the same hardware [2] (*observed*). Both disks boot to an
identical `boot :` prompt, so the difference is in the kernel-side driver the kernel carries, not
in the boot program [2] (*observed*).

What the retail configuration expected — whether every shipped ProFile accepts the truncated
frame, whether the patch disk shipped with every set or was a service fix, and what else the
patched kernel changes — is not established ([§6](#6-open-questions), item 6).

### 5.6 Small observed behaviours

- The kernel's banner names its own memory split (`System 122k User 1830k`), its root and swap
  devices (`rootdev 1 1 swapdev 0 0`), and the machine (`Lisa II/5 (s): ROM 00A8 / Slot 0-2
  Empty`) [2] (*observed*).
- The hard-disk `boot :` prompt's bare RETURN loads the installed system (`pf(0,0)xenix`) [1]
  §1.5.4 — the command line is a default, not a requirement.
- The boot program enables COPS mouse reporting before the prompt is serviced (§3.2) [2]
  (*observed*) — on a machine whose console is serial-less; the deltas' consumer is unknown.
- The keyboard input layer repeats held keys in software (§3.2) [2] (*observed*).
- The guide's power-on "click" — "listen closely — the click occurs soon after you turn on the
  power" [1] §1.5.1 — is the soft-power relay engaging; the identification is *inferred from
  the Lisa's soft-power architecture* ([lisa.md §11.6](../machines/lisa/lisa.md)), not from the
  guide.
- `hdinit`'s abort path is a clean halt: `n` at the continue prompt reaches "Normal System
  Shutdown" without touching the disk [1] §1.5.2.

## 6. Open questions

1. **The on-disk file system format.** `hdinit` makes a file system on `/dev/root` and `fsck`
   checks it through five phases; `upgrade` converts 2.3 layouts to "3.0 format" [1] §1.7.3.
   Nothing in the corpus establishes the layout itself — block size, super-block placement,
   inode format. No XENIX manual or on-disk dump is in evidence.
2. **The kernel's Unix lineage and build.** The banner says `SCO XENIX V3.0`; the release line
   says 1.0. Which Unix base the kernel carries (System V, or an earlier XENIX lineage), its
   build date and its relationship to other XENIX 68K ports of the era are not evidenced.
3. **The banner's machine line.** `Lisa II/5 (s): ROM 00A8` — what `00A8` measures (the recorded
   machine's ROM carries version word `$0248`), what `(s)` denotes, and how the slot inventory
   is built are undecoded.
4. **The COPS reset pair's reach.** Whether the ROM's second COPS reset response reaches the
   boot program's decoder on every real machine, or only at particular POST timings (§5.1) —
   and whether the retail configuration expected the first key at the prompt to be swallowed.
5. **The device-tuple syntax.** `fd(2,0)xenix` and `pf(0,0)xenix`: which number is the
   controller, unit or slice, and what the tuples enumerate. Not documented in the corpus.
6. **The XProFile Patch disk's scope.** Only the command-frame difference is observed (§5.5).
   Whether the patched kernel changes anything else, why the plain Boot disk's driver truncates
   the frame, and whether any real ProFile accepts the four-byte form are unknown.
7. **The drive-numbering scheme.** The first ProFile is `pf0` and the second `pf2` [1] §1.5.2 —
   why not `pf1`, and what unit numbers the expansion-card ports carry beyond the documented
   second drive, is not established.
8. **The 512 KB minimum.** The guide requires "at least 512k bytes" [1] §1.3; every recorded
   boot used 2 MB. Whether the OS boots, installs and runs at 512 KB is untested.
9. **The ROM boot-menu numbering.** The APPLE+`2`/APPLE+`3` keystrokes select devices by icon
   number [1] §1.5.1; the mapping from those numbers to the ROM's device slots is not decoded.
10. **The second fault resolver.** The bus-error case calls resolvers `$14DC`, `$1536` and
    `$2366` (§2.6); `$14DC` is the probe recognizer and `$2366` the stack-grow test, but
    `$1536`'s function is not decoded.
11. **The console driver.** The terminal type is `lisa` [1] §1.5.5; how the kernel drives the
    video hardware — byte order, scrolling, cursor, attributes — is not traced.
12. **The multi-disk install.** The recorded bring-up ends at `hdinit` completion; the
    `firsttime` floppy-copy flow, the `/usr` split across two ProFiles, and the completed
    system's boot and login are documented [1] §1.5.3–§1.5.6 but not recorded.
13. **The COPS command `$7C`.** Its decode in the COPS command set, its issuer (boot program or
    kernel), and the consumer of the mouse deltas it enables (§3.2) are not established.
14. **XENIX 2.3.** Evidenced only as the upgrade's source version [1] §1.7 — media, behaviour
    and hardware support are entirely outside the corpus.
15. **The Lisa 2/10's internal disk.** The guide supports the 10 MB internal disk of the 2/10
    [1] §1.3, §1.5.2; every recorded run used an external ProFile. Whether the 3.0 kernel drives
    the internal Widget (whose command set differs — [profile](../machines/lisa/profile.md) §3.10)
    is not observed.

## References

1. The Santa Cruz Operation, Inc. and Microsoft Corporation, *The XENIX Operating System
   Installation Guide for the Apple Lisa 2*, 1984 (title-page release line
   `68-5-24-84-1.0/1.0`; © SCO 1984, © Microsoft 1983) — §1.1 the multiuser/multitasking claim
   and "takes the place of your existing Operating System"; §1.2 the three distribution systems
   and write-protection; §1.3 the 512 KB / 5 MB ProFile / 10 MB internal requirements and the
   slot-1 second ProFile; §1.4 keyboard conventions; §1.5.1 boot-device selection, the `boot :`
   prompt, `fd(2,0)xenix` and the Boot Floppy banner; §1.5.2 `hdinit`, the 5-or-10 size prompt,
   the `fsck` phases, "Installing hard disk Boot", `pf0`/`pf2` "not on line"; §1.5.3 the
   hard-disk boot and `pf(0,0)xenix`; §1.5.4 `firsttime`, the ordered floppy copy, `/usr` on
   the second ProFile, CONTROL-d; §1.5.5–§1.5.6 `passwd root` and `mkuser`; §1.6 `/etc/haltsys`;
   §1.7 the 2.3-to-3.0 upgrade, `/etc/passwd.SAVE`, `/secondtime`, `diskutil`.
2. Recorded bring-up investigation of the SCO XENIX 3.0 boot on the Apple Lisa 2/5, 2026 (2 MB
   machine, revision "H" boot ROM, 5 MB ProFile) — the reverse-engineered XENIX boot program
   (prompt poll loop, COPS byte decoder and ASCII table, command handler, RAM-sizing probe,
   START-mode MMU trampoline, segment-122 self-relocation), the 394-sector kernel-load trace
   verified byte-exact against the media, the kernel-startup observations (segment-0 execution,
   the boot banner, `rootdev`/`swapdev`, the `pf0 not on line` / `panic: iinit` root-device
   failure, the `<BootFloppy>` shell), the ProFile command-handshake trace (the retail Boot
   disk's truncated frame, the XProFile Patch disk's six-byte frame, `hdinit`'s 249
   transactions), and the `mkfs` user-mode fault analysis (the `$4A2F` stack-growth probe, the
   kernel trap-dispatch chain, resolver `$14DC`, and the group-0 frame's saved-PC dependence).
3. SCO XENIX 3.0 distribution media set for the Lisa 2 (release 1.0) — 17 single-sided 400 KB
   disks: "Xenix 3.0 Boot", "Xenix 3.0 Boot XProFile Patch", "Xenix 3.0" 1–7, "Development
   System" 1–4, "Text Processing System" 1–4 — on-media structures and recorded read behaviour.
4. Apple Computer, Inc., *Apple Lisa Operating System* source release (released via the
   Computer History Museum, 2023) — cited for boot-path context where XENIX parallels the
   resident OS: the OS boot loader's device read-in (source-LDPROF.TEXT with source-LDEQU.TEXT),
   and the `BUS_ERR` demand-load handler's group-0 frame parsing, per-opcode saved-PC
   adjustments and TST stack-overflow path (SOURCE-EXCEPASM.TEXT).
