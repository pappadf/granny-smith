# Apple Macintosh Display Card 24AC

**Contents:**

1. [Overview](#1-overview) — what the card is, identity and provenance, the two software
   populations, VRAM organisation, host machines and observed bring-up
2. [Register file](#2-register-file) — the slot-space map; the CLUT/RAMDAC block; STATUS and
   VIDCTL; the engine CONFIG/CONTROL pair; the mode/clock block; the board-configuration
   block; reset state
3. [Behaviour](#3-behaviour) — access discipline, monitor sensing, pixel-clock PLL, CLUT
   loading, the acceleration engine (apertures, fill, copy, raster-op, stretch), the
   24-bit framebuffer alias, the vertical-blank interrupt
4. [Programming model](#4-programming-model) — declaration-ROM structure, PrimaryInit,
   SecondaryInit, the video driver (Open/Close, Control, Status), depth and mode
   encoding, host-ROM bring-up, the QuickDraw accelerator control panel
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

Appendix A. Granny Smith implementation notes

---

## 1. Overview

### 1.1 What the card is

The **Apple Macintosh Display Card 24AC** is an Apple-branded NuBus video card of the
Macintosh II/Quadra generation: a full-length NuBus card carrying video RAM, a colour
palette (RAMDAC), a CRTC/timing generator, a pixel-clock synthesizer, a monitor-sense
interface, and — the feature that distinguishes it from the plain "display card" line — a
**hardware pattern/raster-op fill engine** that QuickDraw extensions drive through a second,
transforming window onto the card's VRAM. It is a *smart framebuffer*: the CPU still walks
the geometry of every drawing operation, but the inner per-pixel work of fills, region-masked
blits and scaled blits is handed to the engine, which replicates a latched pattern longword
or copies VRAM blocks under a latched raster-op.

The card presents itself to the host through the standard NuBus mechanisms: a declaration
ROM in the top of the slot space (format and contents per
[declaration-rom.md](../declaration-rom.md) §2–§10), a linear framebuffer at the bottom of
standard slot space, byte-wide status/control registers high in the slot space, and a
per-slot /NMRQ line for vertical-blank interrupts ([nubus.md](../nubus.md) §3.7). Two
independent software populations program it: the card's own **video driver**, resident in
the declaration ROM and opened by the Start Manager at boot, and a separately-installed
**QuickDraw accelerator control panel** whose 68K patch code drives the fill engine. The
two never touch each other's registers except for one overlap noted in §2.3.

Every hardware fact on this page is derived from primary evidence against the card itself:
the annotated disassembly of the card's declaration ROM [1] (cited by chip offset into the
32 KB image), the disassembly of the accelerator control panel's 68K resources together
with recorded traces of the live engine traffic [2], the two Apple card/firmware design
books [3] [4], and observed boot behaviour in hosted machines. Claims that rest on a
single reading of driver code are marked *inferred*; claims no shipped code exercises are
left to [§6](#6-open-questions). No schematic, datasheet or Apple developer note for this
specific card is known to exist, so silicon-level truth (which ASIC does what, internal
timings, reset circuitry) is uniformly out of reach; what is documented is the complete
software-visible contract.

### 1.2 Identity, provenance and dating

The card is an OEM product of **Radius Inc.**, rebranded and sold by Apple. The evidence:

- The declaration ROM's board sResource names the card **"Apple Macintosh 24AC"** [1]
  (chip $0A6A), and its VendorInfo strings credit **"Apple Computer, Inc."** [1] (chip
  $0B32) — but a second string, embedded in the trailing identity block, reads **"Firmware
  by Steve Lemke, Radius Inc."** [1] (chip $0B60).
- The firmware carries part number **630-0908**, version **1.1**, and the revision date
  **02-Dec-93** [1] (chip $0B4A–$0B5E) (*observed*; earlier revisions presumably exist but
  are not in the evidence set).
- The video driver is named **".Display_Video_Apple_Boogie"** [1] (chip $1A04); "Boogie" is
  the design's internal codename and appears throughout the ROM (the driver's private
  storage block is stamped with the signature `'Bgy1'` at Open, §4.4).
- The companion QuickDraw accelerator is the control panel **"Apple Macintosh 24AC
  v1.2"**, © 1990–1994 Radius Inc. [2], shipped on an 800 KB Disk Copy 4.2 floppy whose HFS
  volume was created 1994-06-23 [2].

The Apple marketing name pairs it with the Apple color displays of 1992–1994; the
introduction date of 1992 is consistent with the design's conventions but is not otherwise
evidenced (*inferred — unverified*). The "24AC" name (24-bit Accelerated Color, by
analogy with the Display Card 8•24 GC naming) is not decoded anywhere in the evidence.

### 1.3 The two software populations

| Attribute | Video driver | QuickDraw accelerator |
|---|---|---|
| Delivered in | the card's declaration ROM [1] | an install disk; a `cdev` copied into Control Panels [2] |
| Runs as | DRVR in the system heap, opened by the Start Manager (§4.8) | `INIT` resources at boot, then trap patches in QuickDraw's dispatch table (§4.9) |
| Programs | CRTC, PLL, RAMDAC/CLUT, MODE/DEPTH, VIDCTL, and the fill engine for whole-screen clears and gray pages (§3.6) | the fill/copy engine only: CONTROL, the operand aperture, the active VRAM alias (§3.5–§3.8) |
| Registers shared | reads STATUS (bits 4–6), writes VIDCTL bit 5/7 | reads STATUS (bits 0–3), reads CONFIG, writes CONTROL (§2.3–§2.4) |
| Ever touches the other's domain | CONTROL=$01 for `cscGrayPage` deep modes and mode-switch VRAM clears [1] (chip $239C, $2764) | never programs the display side [2] |

The split explains an unusual property of this page's register map: the video side and the
engine side have disjoint register pages ($D004xx/$D80xxx and $C80xxx versus $D404xx plus
the aperture windows), and only STATUS — read by both, for different bit fields — is
genuinely shared.

### 1.4 VRAM organisation and card variants

The card exists in (at least) two VRAM organisations, distinguished at run time through
two configuration bits the accelerator reads once at init:

- **STATUS bit 3** ($D00402, read): a card-class / VRAM-organisation bit. When clear, the
  accelerator halves its eight-entry stride table and loads a different pair of fill
  geometry constants [2] (§4.9).
- **CONFIG bit 0** ($D40402, read): a geometry-variant select which, with STATUS bit 3,
  chooses the fill stripe constants the accelerator keeps at `QCOD+$AA`/`+$AC` — drawn
  from the observed set `$40`, `$200`, `$240`, `$380` (§3.6) [2].

The accelerator also selects its **operand aperture** from a pixmap field of the mode the
OS has set: a base-VRAM-pitch threshold chooses `$FE000` (small) or `$3FE000` (large) as
the aperture offset within the slot [2]. The large-organisation card addresses the
advertised deep modes — the mode-name list includes 1152×870 and 1024×768 at depths the
driver's depth ladder (§4.7) extends to 32 bpp, which requires just under 4 MB — so the
large variant is a **4 MB** part and the small variant a 1 MB-class part (*inferred from
the aperture offsets and the mode list; no document states either figure*). The engine
alias sits at **slot offset $400000**, i.e. above the whole 4 MB framebuffer of the large
variant — the two organisations differ in where the operand aperture falls relative to the
VRAM top, not in the alias layout (§2.1).

### 1.5 Host machines and observed bring-up

Nothing in the evidence restricts the card to a machine family: it is a standard NuBus '90
card ([nubus.md](../nubus.md) §1.5) whose firmware follows the Slot Manager conventions
that every NuBus Macintosh from the Macintosh II onward implements
([declaration-rom.md](../declaration-rom.md) §4, §8). Observed operation covers two host
families — one 68k, one PowerPC — plus the architecture split the accelerator itself
imposes:

- **Macintosh IIcx** (68k, 24-bit Memory Manager mode): the card boots System 7.0.1 to a
  Finder desktop — by default 1152×870 at 1 bpp, and, with the slot PRAM seeded to the
  8-bpp sub-mode, a full 8-bpp colour desktop — with the driver's register traffic
  matching §4 in detail (*observed* in recorded boot traces [2] [1]).
- **Power Macintosh 8100** (PDM platform, BART bridge): with a monitor sensed on built-in
  video, the card comes up as a *second* screen — the driver opens, sets 640×480 at 1 bpp,
  and the desktop gray is painted into the card's VRAM — while the menu bar stays on
  built-in video ([bart.md](../../../machines/pdm/bart.md) §4.7, *observed* under System
  7.5; the same page records the card's slot interrupt line asserting and releasing once
  per frame for an entire session, §4.5).
- The accelerator's 68K path explicitly gates on the Gestalt architecture selector and
  bails on PowerPC machines, where the control panel's PowerPC code fragment takes over
  [2] (§4.9); that native path is not in the evidence set (§6.15).

The accelerator engages only in 32-bit addressing mode: on hosts running the 24-bit Memory
Manager it chains to stock QuickDraw and the card behaves as a plain framebuffer [2]
(§4.9).

## 2. Register file

### 2.1 The slot-space map

The card answers standard slot space `$Fs000000`–`$FsFFFFFF` (16 MB, `s` = slot $9–$E;
[nubus.md](../nubus.md) §2.1). Offsets below are from the slot base; every register named
in this page is a single byte on NuBus **lane 3** only, spaced by 4 — the byte at slot
offset $N is the only meaningful byte in its longword, and the driver never reads or writes
the other lanes [1] [2] (*observed*; the exact hardware register index inside the card is
`(offset-1)/4`, *inferred*).

| Slot offset | Function | Section |
|---|---|---|
| $000000–$3FFFFF | **Framebuffer VRAM**, passive bank: plain pixel reads/writes; the card's framebuffer base (QuickDraw `baseAddr`) | §3.5 |
| $400000–$7FFFFF | **Active bank**: alias of the passive bank through the fill/copy engine — writes are transformed per the latched CONTROL mode | §3.5–§3.8 |
| $0FE000 (small) / $3FE000 (large) | **Operand aperture**: the engine's latched 32-bit pattern/colour register (write to load, read to read back) | §3.5 |
| $4FE000 / $7FE000 | **Commit window**: active-bank alias of the operand aperture; the commit command is written here | §3.5 |
| $900000–$C7FFFF | **24-bit-mode framebuffer alias**: the same VRAM addressed through the 24-bit-mode base form `$Fss0_0000` ([declaration-rom.md](../declaration-rom.md) §4.1); the visible extent must stay below the first register page (≤ $380000) so it cannot shadow the register blocks | §3.9 |
| $C80000–$C8FFFF | **RAMDAC/CLUT block**: command, two index/data port pairs, end-of-load strobe | §2.2 |
| $D00000–$D0FFFF | **STATUS** (byte read, $D00402) and **VIDCTL** (byte read/write, $D00403) | §2.3 |
| $D40000–$D4FFFF | **CONFIG** (byte read, $D40402) and **CONTROL** (byte write, $D40403) — the engine pair | §2.4 |
| $D80000–$D8FFFF | **Mode/clock block**: MODE, DEPTH, SENSE_CLK, and the 9-register CRTC file | §2.5 |
| $FE0000–$FFFFFF | **Declaration ROM**: 32 KB chip image, lane-3 expanded to a 128 KB bus footprint ([declaration-rom.md](../declaration-rom.md) §2.1) | §4.1 |
| $FFFD8 | **BOARDCFG**: board-configuration/scratch block stamped by PrimaryInit and gated by the driver's Open | §2.6 |

Whether the card decodes anything else in the gaps (in particular between $D80035 and
$FE0000), and whether any of the named registers alias within their 64 KB pages, is not
established — the shipped code only ever touches the addresses above (§6.12).

### 2.2 The RAMDAC/CLUT block ($C80000)

Five byte registers program the colour palette, in two functionally parallel port pairs
plus a command and a strobe [1]:

| Offset | Access | Name | Function |
|---|---|---|---|
| $C80006 | write | RAMDAC command | written once with `$E3` during PrimaryInit's RAMDAC setup [1] (chip $0356) |
| $C8000A | write | CLUT data (init port) | the 16-entry blank path: index at $C8000E, then three writes R,G,B = 0 [1] (chip $037C) |
| $C8000E | write | CLUT index (init port) | as above |
| $C80016 | write | CLUT control / end-of-load strobe | terminates a palette load and resets the port's internal write pointer; also strobed `$FF` after partial loads [1] (chip $0360, $1F24) |
| $C8001A | write | CLUT data (runtime port) | three writes per entry (R, G, B) after an index write |
| $C8001E | write | CLUT index (runtime port) | index write also resets the R/G/B sub-counter within the entry [1] (chip $1F14) |

The two runtime-port registers are the ones the OS's `cscSetEntries` path drives, and both
are **active low**: the driver writes the one's-complement of the palette index *and* of
each gamma-corrected R/G/B component, and the RAMDAC inverts them internally before the
colour store and pixel lookup (§3.4). The init port, by contrast, is written with plain
(non-inverted) index and data during PrimaryInit's transient clear [1] [2] — the two ports
do not share the inversion.

### 2.3 STATUS and VIDCTL ($D00400 longword)

Two adjacent bytes of one longword at $D00400 carry the card's central status and control
state. The driver and the accelerator read **STATUS** for disjoint bit fields:

**STATUS** — $D00402, byte read [1] [2]:

| Bits | Meaning | Evidence |
|---|---|---|
| [2:0] | **Current depth/mode code** (0–7): re-read before every engine operation by the accelerator, used to index its eight-entry stride table (§4.9) | accelerator init and per-blit reads [2]; meaning *inferred* |
| [3] | **Card-class / VRAM organisation**: read once at accelerator init; selects stride-table halving and the geometry constants | accelerator init `BTST #3` [2]; meaning *inferred* |
| [4] | **Sync/CLUT-safe busy poll**: the driver waits for a safe window before CLUT writes by polling this bit | `cscSetEntries` poll loop [1] (chip $20B2, $21C2) |
| [5] | **Self-test "fill-quadrant" flag A**: sampled during PrimaryInit's self-test; asserted at least once clears the D2 residue | self-test loop [1] (chip $03F4–$0410) |
| [6] | **Self-test "fill-quadrant" flag B**: as bit 5 for the D1 residue | as above |
| [7] | never tested by any shipped code | — |

**VIDCTL** — $D00403, byte read/write, the display side's central latch [1]:

| Bits | Meaning | Evidence |
|---|---|---|
| [2:0] | **Depth/PBCR field**: the accelerator-facing depth code the init routines compare; **power-on value 2** | the `$47` gate (§3.2) reads it before any write [1] (chip $0176) |
| [3] | **Direct/"magic" mode** enable (the 24/32-bpp direct-pixel path); set/cleared by vendor Control csCode $80 (§4.5) | [1] (chip $2890–$28BC, $2B6, $1F4E) |
| [5] | **Config-commit strobe**: written low then high to latch a configuration (RAMDAC setup, mode switch, engine fill commit) | [1] (chip $0392–$0398, $2432–$2442, $27C2–$27D2) |
| [7] | **VBL interrupt mask/acknowledge**: 1 masks the vertical-blank interrupt; the VBL ISR acknowledges by writing bit 7 set then clear | [1] (chip $1A50–$1A60, $1D6E–$1D7A, $1DEE–$1DFA) |
| [4], [6] | never written or tested by shipped code | — |

The driver keeps a shadow of VIDCTL in its private storage (offset $30) so that interrupt
code can toggle bits without losing the rest [1] (chip $1C98, $1D6A).

### 2.4 CONFIG and CONTROL ($D40400 longword — the engine pair)

| Register | Offset | Access | Bits |
|---|---|---|---|
| CONFIG | $D40402 | byte read | [0] geometry/bank-variant select — chooses the engine's fill chunk width ($80 versus $200 pixels) and part of the init geometry; [3] a variant qualifier read on the direct-mode paths [1] (chip $23C8, $276E, $2D8); other bits never tested |
| CONTROL | $D40403 | byte write | the engine **mode latch** for subsequent active-bank writes (§3.5–§3.8) |

CONTROL is the only engine register the accelerator and the driver both write; the complete
set of values emitted by shipped code is [1] [2]:

| Value | Operation | Written by |
|---|---|---|
| $01 | **Pattern/solid fill** — replicate the latched operand | accelerator `bSETUP8`/`rMASK8`/`FastSlabMode` paths [2]; driver `cscGrayPage` deep modes and mode-switch VRAM clear [1] (chip $23E0, $2764) |
| $03 | **Stretch/scale** mode | accelerator `SetUpStretch` [2] |
| $7F | **Fast block copy** ("all planes") | accelerator `bSetup0`/`bLeft0`/`rMASK0` paths [2] |
| $00–$3F (computed) | **Raster-op / transfer-mode code** derived from the QuickDraw transfer mode | accelerator `bXMAIN8`/`rXMASK8`/`slXMASK8` [2] (§3.8) |

### 2.5 The mode/clock block ($D80000)

| Offset | Access | Name | Bits / function |
|---|---|---|---|
| $D80001 | read/write | **MODE** | bits 7–5 = pixel-depth code (the ladder of §4.7); bits 4–0 = timing index, written together from the mode's timing table [1] (chip $270E–$271E) |
| $D80005 | read/write | **DEPTH** | low nibble = depth/clock field written from the mode table; high nibble = sync/blanking state, stashed and restored by `cscSetSync` (§4.5) [1] (chip $2840–$288A) |
| $D8000D | read/write | **SENSE_CLK** | bits 7–5: monitor sense lines (read inverted); the same byte is the serial port of the pixel-clock PLL (§3.2–§3.3) [1] (chip $00A8, $0304) |
| $D80015–$D80035 | write (9 registers, stride 4) | **CRTC file** | horizontal/vertical timing register file, loaded descending ($D80035 first) from a per-mode timing table (§4.5) [1] (chip $26BE–$26FE); write-only as used |

The MODE register's timing-index field is what links a mode switch to the CRTC: the
programmer preserves the depth bits, ORs in the new timing index, and the nine CRTC bytes
plus a PLL divisor word follow from the same table [1] (chip $2714).

### 2.6 The board-configuration block (BOARDCFG, $FFFD8)

A 16-byte block at slot offset $FFFD8, stamped by PrimaryInit (§4.2) and consumed by the
driver's Open and by SecondaryInit. The layout, as written by PrimaryInit and read by the
driver [1] (chip $0412–$049E, $1AEE–$1CB6):

| Offset | Size | Meaning |
|---|---|---|
| +0 | byte | **Board-OK flag** — the self-test residue `D1\|D2`. The driver's Open gate: zero → purge the card's sResources and return `openErr` (-23) (§4.4) |
| +1 | byte | timing-record config byte 1; read by Open as a mono/colour marker (`SEQ` on zero) |
| +2 | byte | config code (`$82`/`$84`) |
| +3 | byte | depth index / current mode byte (SecondaryInit re-stamps it from the saved preference) |
| +4 | byte | base depth/mode index (range start) |
| +5 | byte | top depth/mode index (range end) |
| +6 | byte | VIDCTL shadow / config mirror (Open uses it as the default-mode argument) |
| +7 | byte | timing-record config byte 2 / id mirror |
| +8 | word | active width / rowBytes |
| +$A | word | pixel-geometry word (with +8, the default-mode arguments Open passes to its mode programmer) |
| +$C | byte | interlace/special flag |
| +$E | long | rebased family-mode list, entries 0–3 |
| +$12 | long | rebased family-mode list, entries 4–7 |

Whether the block is backed by dedicated registers or by ordinary VRAM cells at that
offset is not established (§6.12) — the driver only ever accesses it after PrimaryInit has
written it.

### 2.7 Reset state

Only one reset value is pinned by software: **VIDCTL's low three bits read 2 at power-on**
(*inferred — the value a healthy card must present*, from the `$47` gate: PrimaryInit
reads VIDCTL before writing it anywhere and forces the standard-monitor fallback unless the
low bits are 2, a path that leaves the card unusable, §3.2) [1] (chip $0176). The power-on
values of every other register — STATUS, CONFIG, MODE, DEPTH, the CRTC file, CONTROL, the
engine's latched operand — are never observed before being written, and nothing constrains
them (§6.11). NuBus /RESET (pulsed by the host at start; [nubus.md](../nubus.md) §3.9) and
the power-on state are indistinguishable in the evidence: no code re-initialises the card
differently after a warm restart, and the driver's Open always re-stamps mode state from
BOARDCFG and slot PRAM rather than assuming reset values [1] (§4.4).

## 3. Behaviour

### 3.1 Access discipline: byte lanes and addressing mode

Two disciplines govern every register access on this card, and both are load-bearing:

- **Lane-3 byte access.** The declaration ROM sits on byte lane 3 only (ByteLanes `$78`;
  §4.1), and so do the register bytes: the driver addresses each register as a single byte
  at its full slot offset, never as a packed longword [1] [2]. On the bus this means the
  card samples and drives lane 3 for register traffic ([nubus.md](../nubus.md) §2.6); a
  host that reads the whole longword gets the register byte on lane 3 and (presumably) VRAM
  or open-bus data on the others — never relied on by any shipped code (*inferred*).
- **32-bit addressing mode.** All register and aperture addresses live above the 1 MB
  24-bit-mode window, so the driver wraps *every* register sequence in `_SwapMMUMode`
  (saving and restoring the previous mode) [1] (e.g. chip $0094, $1AF6, $1F0A), and the
  accelerator additionally checks the low-memory `MMU32bit` flag ($0CB2) before each
  hardware blit, chaining to stock QuickDraw when the machine runs the 24-bit Memory
  Manager [2] (QCOD+`$01E0`). In 24-bit mode the only card address QuickDraw can reach is
  the 24-bit framebuffer alias of §3.9 — which is exactly why the alias exists.

### 3.2 Monitor sensing on SENSE_CLK

PrimaryInit identifies the attached display through the three Apple sense lines, driven
and read through bits 7–5 of SENSE_CLK ($D8000D) [1] (chip $00A8–$0172). The protocol:

1. **Primary code.** Drive all three lines low (clear the register), spin a ~1 ms settle
   delay (a `TimeDBRA`-scaled DBF loop), read the byte back, invert it, mask `$E0`, shift
   right 5 → a primary code 0–7. (The lines are inverted on the bus: a line the monitor
   pulls low reads as 1.)
2. **Dispatch.** The code indexes a word table at chip $014E; entries are either 0
   ("simple monitor") or an offset to an **extended-sense** decode table:

   | Primary code | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
   |---|---|---|---|---|---|---|---|---|
   | Table entry | 0 | 0 | 0 | $10 | 0 | $12 | $14 | $1C |

   Codes 3 and 5 dispatch to *empty* extended tables (immediate `$FF` terminator) and fall
   back to the simple path. Codes 6 and 7 decode further:

   | Primary | Extended value → monitor id |
   |---|---|
   | 6 | `$03`→`$6B`, `$0B`→`$6C`, `$23`→`$6D` — the Apple 640×480 / 800×600 / 832×624 multisyncs |
   | 7 | `$2D`→`$80`, `$17`→`$81`, `$3A`→`$82` — the Radius hi-res monitors |

3. **Extended sense.** For a dispatched code the driver drives each line group in turn —
   `$80` (bit 7), `$40` (bit 6), `$20` (bit 5) — settles ~1 ms, and reads back the *other
   two* lines each time, assembling a 6-bit code in D2 (bits 5/4 from the first drive,
   3/2 from the second, 1/0 from the third). The assembled value is matched against the
   code's `(value, id)` pairs; no match falls back to the simple default [1] (chip
   $00D4–$014C). This is the stateful line-probe of the extended-sense scheme: the readback
   must depend on which line group was just driven, so a static sense byte can only ever
   produce a simple id (*observed*; [4] documents the three-line sense codes, Table 12-2,
   but not this drive-out variant).
4. **Simple fallback.** monitor id = primary code + `$40` → ids `$40`–`$46`. With no
   monitor driving the lines, the primary code is 0 and the id is **`$40`** — which the
   timing directory maps to the 1152×870 two-page display, brought up at 1 bpp (§4.7).

The resulting id set — `$40`–`$46`, `$6B`–`$6D`, `$80`–`$82` — is exactly the population of
the ROM's monitor timing directory (§4.1), plus one marker outside it: **`$47`**, the
"standard monitor" fallback. `$47` is never sensed; it is *forced* in two places:

- **The VIDCTL gate.** After sensing, PrimaryInit reads VIDCTL and masks its low three
  bits; if they are not **2**, the sensed id is thrown away and `$47` is used [1] (chip
  $0176–$0188). This is why the power-on value of that field is load-bearing (§2.7): a
  card that does not read 2 there is forced down the `$47` path.
- **The self-test downgrade.** If the board self-test leaves residue 0, the saved monitor
  id is downgraded to `$47` [1] (chip $041E).

The `$47` path in turn **skips the self-test** and skips the colour-mode machinery
entirely [1] (chip $03A2), leaving `BOARDCFG[0]` = 0 — so any card that lands on `$47`
during bring-up cannot open its driver at all (§4.4). The chain VIDCTL-low-bits ≠ 2 → `$47`
→ no self-test → `BOARDCFG[0]` = 0 → `openErr` is the single most consequential behaviour
on the card.

The sensed id is compared against the slot-PRAM record: if it matches the saved id, the
saved mode list is kept; otherwise the PRAM is re-defaulted and the Slot Resource Table is
flagged for update [1] (chip $018E–$01AE).

### 3.3 Pixel-clock PLL programming

The pixel-clock synthesizer is programmed **serially through SENSE_CLK** — the same byte
that reads the sense lines doubles as a two/three-line serial port [1] (chip $0304–$0354).
The `PllShiftOut` routine:

1. Presents an idle frame (all lines low), then raises the bit-1 line ("enable"), then the
   bit-2 line ("frame start").
2. Shifts a **19-bit word, MSB first** (bits 18..0): per bit, the data line (bit 3, driven
   *inverted* — a 1 data bit clears bit 3) is presented, then the clock line (bit 2) is
   dropped and raised to latch the bit.
3. Drops the enable line and presents the all-low stop frame.

Every write to $D8000D is padded with two NOPs before and after — the bus write itself is
the timing reference, and the padding provides the synthesizer's setup and hold time
(*inferred*; nothing else explains the NOPs) [1] (chip $034A–$0354).

The 19-bit word is selected at init from the card's configuration state [1] (chip $02A6–
$0300):

| VIDCTL bit 3 (direct) | CONFIG bit 0 | CONFIG bit 3 | Serial word |
|---|---|---|---|
| 0 (indexed mode) | 0 | — | `$056AE6` |
| 0 (indexed mode) | 1 | — | `$055CAA` |
| 1 (direct mode) | 0 | 0 | `$048276` |
| 1 (direct mode) | 1 | 0 | `$0472AA` |
| 1 (direct mode) | — | 1 | `$05A6B6` |

and the mode-switch path reprograms the PLL from each mode's timing table, with two further
constants on the deep-mode branch (`$05E292` for depth code 4, `$0472AA` otherwise) [1]
(chip $2744, $1F6E–$1F7E). The words' internal meaning (reference divisor, postscaler,
charge-pump setting of an ICS-class part) is not decoded (*inferred to be a clock-synth
control word*); what is established is the bit order, the line assignment and the select
logic above.

### 3.4 CLUT loading and the active-low port

The OS loads the palette through the runtime port (`$C8001E` index, `$C8001A` data), and
the port is **active low** [1] [2]: `cscSetEntries` stages each entry as
`{NOT(index << packShift), NOT(gammaR), NOT(gammaG), NOT(gammaB)}` and the RAMDAC inverts
each byte back internally:

```asm
; cscSetEntries sequential-load staging (chip $203A-$2082)
LSL.B   D2,D7              ; pack index per depth (pack-shift table at chip $2230)
NOT.B   D7                 ; index inverted
MOVE.B  $0(A4,D3.W),D3     ; gamma-corrected red
NOT.B   D3                 ; component inverted
...
MOVE.B  (A1)+,$C8001E      ; write inverted index
MOVE.B  (A1)+,$C8001A      ; write inverted R, then G, then B
```

The index's per-depth pack shift is `{7, 4, 0, 3}` for depth codes 0–3 [1] (chip $2230):
1-bpp entries pack eight palette indices per byte, 4-bpp packs two, 8-bpp is a clean `~N`,
and 16-bpp packs the index into 5-bit fields. Each index write resets the port's R/G/B
sub-counter, so entries are always written as index-then-three-components [1] (chip
$1F14). Loads are bracketed by a poll of STATUS bit 4 (the CLUT-safe window) and
terminated by the end-of-load strobe at $C80016 [1] (chip $20B2–$221E).

White and black round-trip through an inverting *and* a non-inverting port model alike
(the two missing inversions cancel for the index/colour pair `$00`↔`$FF`, `$FFFFFF`↔
`$000000`) — so a card whose port did not invert would still render a correct-looking
monochrome desktop while every colour entry lands at `~N` with inverted components
(*observed* as the classic failure signature of this card [2]).

Read-back is the mirror image: the driver's `GetEntries` status call returns the palette
from its 3×256 shadow table, re-inverting each stored component [1] (chip $2996–$2A08) —
the shadow, not the hardware, is the read path. PrimaryInit's own blank loads (the 16-entry
init-port clear and the `ClutPreloadRamp` black/white ramp) write **non-inverted** bytes
[1] (chip $037C, $05A8) — transient states the OS's first full `cscSetEntries` load
overwrites.

### 3.5 The acceleration engine: dual apertures and the operand

The engine's software model is a **dual-aperture framebuffer**. The same VRAM cells are
addressable twice: through the **passive bank** at slot offset 0, where reads and writes
are ordinary pixels, and through the **active bank** at **slot offset +$400000**, where a
write is interpreted as a command at the *corresponding passive position* [1] [2]. Which
transformation a write undergoes is fixed by the CONTROL byte last latched (§2.4); which
data it uses by the **operand** — a 32-bit pattern or colour register loaded through the
**operand aperture** at `$FE000`/`$3FE000`:

```
; the accelerator's operand load and commit (per pattern longword) [2]
write CONTROL ($D40403) = $01        ; fill mode
write LONG [aperture]    = pattern   ; load the operand register
write LONG [aperture+$400000] = 4    ; commit — the driver writes this TWICE
write LONG [aperture+$400000] = 4
; from here on, reads of [aperture] return the latched pattern
```

The commit value `4` and its double write are proven access patterns [2]; whether the
second write is a required handshake, a pipeline flush or a timing pad is not established
(§6.7). The operand aperture **reads back the latched operand**, and the accelerator
exploits this as a one-entry cache — it re-loads only when the pattern longword differs
from the read-back value [2] (QCOD+`$027E`).

The driver's own deep-mode fill path (`cscGrayPage` and the mode-switch clear) programs the
engine differently and is recorded separately (§3.6): it writes the two pattern longwords
to the *passive* positions at the fill base, kicks the engine with `$8` at the active alias
of that position, and then streams chunk-count writes — so the two software populations
use two visibly different operand-load rituals over the same engine (§6.7).

### 3.6 Fill (CONTROL = $01)

**Accelerator path.** With the operand committed, the accelerator fills the mask-free
interior of each scanline with **run-length writes** through the active bank [2]:

```
A = destAddress + $400000              ; active alias of the destination
repeat while bytes remain in the run:
    L = min(bytesRemaining, stripeWidth)   ; software clamp to the stripe width
    write LONG [A] = $40000000 | L     ; fill L bytes at the passive position with the operand
    A += L
```

The longword's low bits are the **byte count**; **bit 30 ($40000000) is a command/enable
flag** the accelerator ORs into every fill write, and the engine ignores it [2]. The
unflagged value carries a different meaning on the copy path (§3.7), so a fill count read
from the whole longword instead of the masked low bits runs to the end of VRAM. The run
length is clamped in software to the fill stripe width before the flag is OR-ed in, so the
count never collides with the flag bits.

Longword edges of a row (partial pixels needing masks) are **never** done by the engine:
the accelerator computes the masked left and right edges in software through the passive
bank and hands the engine only the aligned interior [2]. Per-pattern geometry — the stride
per depth code and the stripe width — comes from the init-time tables of §1.4; the stripe
constants observed are `$40`, `$200`, `$240` and `$380`, selected by STATUS bit 3 and
CONFIG bit 0 [2].

**Driver path.** `cscGrayPage` fills the screen with the desktop gray using the engine in
deep modes (depth code ≥ 2): it latches CONTROL=$01, chooses a chunk width of `$80` or
`$200` pixels from CONFIG bit 0, writes the background/foreground pattern longwords (from
the per-depth pattern table at chip $2466: `$AAAAAAAA`/`$F0F0F0F0`/`$FF00FF00`/
`$7FFF0000`/`00FFFFFF` by depth), kicks the engine with `$8` at the active alias of the
fill base, and then writes chunk counts — as `$40000000 | count` longwords — to the active
alias, advancing the destination by each chunk and alternating the pattern per row [1]
(chip $239C–$2428). Shallow modes (1 and 4 bpp) use a plain CPU loop into the passive bank
instead [1] (chip $2376–$2398). The mode-switch clear pass is the same ritual over the
whole framebuffer, with loop counts `$7C08`/`$1F02` for the two CONFIG-bit-0 geometries
and chunk descriptors `$40000080`/`$40000200` [1] (chip $2764–$27B8).

### 3.7 Block copy (CONTROL = $7F): the two-write latch/execute handshake

A VRAM-to-VRAM block copy — what `ScrollRect` and window moves compile to — runs as a
**two-write handshake per scanline**, not as a pixel stream [2]:

```
write CONTROL ($D40403) = $7F             ; copy mode
for each scanline:
    write LONG [src + $400000] = L         ; no flag: LATCH source position, length L
    write LONG [dst + $400000] = $40000000 | L   ; flag: EXECUTE the copy of L bytes
```

Three properties of the engine are pinned by recorded traces [2]:

- **Bit 30 marks the execute write.** The unflagged write only latches a source
  position/length pair (it stores nothing); the flagged write performs the copy. A
  driver-side disassembly pair anchors it: the source-latch store at QCOD `$ceda2`
  (`MOVE.L D4,(A4)`) and the flagged copy trigger at `$cedba` (`MOVE.L D5,(A5)`).
- **The latched source pointer auto-increments** by each executed length. One latch can
  drive several executes: when a copy's destination would straddle an **8 KB boundary**
  (`dst & $1FFF`), the accelerator splits it into two executes from a single latch and
  lets the engine supply the continuation source itself (`LATCH src, len=$200; COPY
  dst=$7F88 len=120; COPY dst=$8000 len=392` — the source advancing `$A788`→`$A800`
  between the two executes). A model that pins the source instead re-reads the source head
  and smears stale pixels along every boundary-crossing row.
- **Bit 31 ($80000000) is a backward-direction flag.** A copy whose destination lies above
  its source (a leftward `ScrollRect`) latches and executes with bit 31 set — the engine
  copies high-to-low so it does not overwrite the source before reading it, and the
  latched source pointer *decrements* by each executed length. The accelerator's
  backward routine (QCOD `$079E`–`$0842`) preloads `$80000000`/`$C0000000` into the
  latch/execute registers and steps the pointers with `SUBA`; the forward routine is the
  same code with bit 31 clear and `ADDA`.

The count is recovered by masking **both** flag bits off the written longword: the copy
count word is `{bit 31 direction}{bit 30 execute} | L` [2]. Copy lengths are
software-clamped to the stripe width before the flags are OR-ed in, exactly as for fill.

### 3.8 Raster-op modes (computed CONTROL codes) and stretch

For transfer-mode operations (XOR and friends) the accelerator computes a CONTROL code in
`$00`–`$3F` from the QuickDraw transfer-mode word `D7` [2] (QCOD+`$1094`):

```
if (D7 & 4) code = D7 >> 2; else code = ((D7 << 1) + 4) >> 2
```

and writes it to CONTROL before streaming. The mapping QuickDraw-mode → engine code is
proven; **the raster-op each code selects is not** — no observed session exercises the
codes end-to-end (the Finder's XOR work is all narrower than the 32-long eligibility
threshold, §4.9), so the per-code logic is open (§6.8) [2].

Two further engine-adjacent behaviours:

- **Stretch (CONTROL=$03).** `SetUpStretch` latches $03 once at the start of a scaled blit;
  the per-scanline work is still driven by a software DDA (the patched `_stScanLoop`), which
  fans out to the same fill/copy routines per output row — no additional engine registers
  are involved [2].
- **Off-card sources.** The `bXMAIN8` path streams the actual source pixels through the
  active aperture (a `DBF` loop of `MOVE.L src,(A2)+` writes at the destination's active
  alias), letting the engine apply the raster-op to host-RAM source data [2] (QCOD+`$10FE`).
  The observed boots never take this path (their copies are all VRAM-to-VRAM via §3.7), so
  its exact contract is unverified (§6.9).

### 3.9 The 24-bit framebuffer alias

The card decodes the framebuffer at a second window, **slot offset $900000**, so that the
24-bit-mode base form `$Fss0_0000` (the standard 24-bit slot addressing convention;
[declaration-rom.md](../declaration-rom.md) §4.1) lands on live pixels: with the card in
slot 9, the boot ROM resolves the screen base to `$F9900000` and 24-bit-mode QuickDraw
dereferences that pointer literally for every store [1] (*observed*: without the alias
every QuickDraw store vanishes into unmapped space and the screen never updates). The
alias is the reason the register pages sit *high* in slot space: the alias window must be
bounded below the first register page ($C80000) or it shadows the CLUT block — with the
visible extent capped at $380000 the alias spans $900000–$C7FFFF at most, clear of every
register [1]. The 32-bit-mode base (offset 0) is the one the declaration ROM's
MinorBaseOS entries declare; the two base forms are the standard pair
([declaration-rom.md](../declaration-rom.md) §4.1).

### 3.10 The vertical-blank interrupt

The card raises one interrupt: vertical blank, on the slot's /NMRQ line
([nubus.md](../nubus.md) §3.7), gated by **VIDCTL bit 7**. The contract, from the driver's
ISR and Open/Close code [1] (chip $1A3A–$1AC4, $1D62–$1D7A, $1DE0–$1E02):

- Open finishes by **clearing** bit 7 (unmasking); Close sets it. The mask is level, not
  edge: while set, no VBL is delivered.
- The ISR acknowledges by writing VIDCTL with bit 7 **set, then clear** — a pulse, after
  which the line can re-assert on the next frame. It then bumps a frame counter in the
  DCE, flags "VBL occurred", refreshes the cursor timestamp if the cursor image changed,
  and dispatches the system's slot VBL task chain (`JVBLTask`) for this slot.
- Because the host's slot-interrupt dispatch is level-sensitive on some machines, the ISR
  also installs a **Time Manager re-arm task** (except under A/UX, detected via the
  `HWCfgFlags` bit 9): when the cursor sits high on the screen the ISR primes the task for
  6 ms rather than dispatching the VBL chain immediately — the same re-arm dance the
  built-in RBV video path uses to keep cursor tearing out of the top scanlines [1] (chip
  $1A92–$1AAC). The re-arm task is a heap-relocated copy of a small trampoline that
  tail-jumps through `JVBLTask` [1] (chip $1A2E, $1D2E–$1D60).
- The ISR installs through `_SIntInstall` with a slot-interrupt queue element (type 6,
  priority $80) whose parameter block carries the driver's private storage; it returns $FF
  ("serviced") on every entry [1] (chip $1CF0–$1D1C).

Observed on a PDM host: the card's slot line asserts and releases once per frame for an
entire System 7.5 session (roughly 989 assertions against 870 releases in one boot),
consistent with one VBL per frame plus the ISR's pulse-acknowledge cycle
([bart.md](../../../machines/pdm/bart.md) §4.5, §4.7).

## 4. Programming model

This section is the re-implementation half of the page: what the card's own firmware does,
what the host ROM does to it, and what the accelerator does to QuickDraw — in the order it
happens at boot.

### 4.1 Declaration-ROM structure

The declaration ROM is a 32 KB image occupying the top of standard slot space with a 128 KB
bus footprint on byte lane 3 [1]. The format block
([declaration-rom.md](../declaration-rom.md) §2) carries, at chip $7FE0–$7FFF [1]:

| Field | Value |
|---|---|
| ByteLanes | `$78` — lane 3 only, at slot $FsFFFFFF |
| Reserved | `$00` |
| TestPattern | `$5A932BC7` |
| Format | `$01` |
| RevisionLevel | `1` |
| CRC | `$D8DAAB87` |
| Length | `$00008000` (32 KB) |
| DirectoryOffset | `$FF89D2` (−$762E from the field) → directory at chip $09BE |

The sResource directory at chip $09BE holds the board sResource (`$01`, pointing to its
sub-list at $0A16) and **twenty functional video sResources, IDs `$80`–`$93`**, in
ascending-id order per the format ([declaration-rom.md](../declaration-rom.md) §3). The
board sub-list carries the standard identity
entries: `sRsrcType` (fixed CatBoard record), `sRsrcName` → **"Apple Macintosh 24AC"**
(chip $0A6A), the VendorInfo strings **"Apple Computer, Inc."**, **"1.1"**, **"630-0908"**,
**"02-Dec-93"**, **"Firmware by Steve Lemke, Radius Inc."** (chip $0B32–$0B83), and the
`PRAMInitData`/`PrimaryInit`/`SecondaryInit`/`sDriver` entries per
([declaration-rom.md](../declaration-rom.md) §5) [1].

The functional sResources are the card's "screen mode entries"
([declaration-rom.md](../declaration-rom.md) §6): each sub-list (in the $15D6–$1A05 region)
carries `sRsrcType` (CatDisplay/TypVideo with Radius `DrHW`), `sRsrcName`, `sRsrcDrvrDir`
(pointing at the single driver sBlock), `sRsrcHWDevId` = 1, `MinorBaseOS`/`MinorLength`
(the framebuffer's placement), and per-depth mode entries whose offsets reach the
`mVidParams` sBlocks and CRTC timing tables in the trailing data at chip $2C00 onward
[1]. Which of the twenty IDs corresponds to which geometry is not decoded beyond the
mode-name evidence below (§6.3). The driver's own validation is purely on the range: `Open`
purges SRT records `$80`–`$93` on failure, and `cscSetMode`/`cscSwitchMode` reject any
requested id outside `$80`–`$93` [1] (chip $1B78–$1B8C, $1EB6, $2604–$264A).

Three more populations of the ROM's data areas are established [1]:

- **Mode names.** Nine name sBlocks at chip $EE6–$FD9, tagged `$C1`–`$C9`: " 640 x 480
  (60 Hz)", " 640 x 480 (67 Hz)", " 800 x 600 (60 Hz)", " 832 x 624 (75 Hz)", "1024 x 768
  (60 Hz)", "1024 x 768 (75 Hz)", "1152 x 870 (75 Hz)" (twice), "640 x 870 (75 Hz)"
  (twice) — the geometry set the card advertises.
- **Gamma tables.** Three named gamma sBlocks: **"Page-White Gamma"** (chip $1070; a
  three-channel 256-entry 8-bit ramp set, block size $324), **"Mac Std Gamma"** (chip
  $1390; one channel, $120) and **"Mac Gray Gamma"** (chip $14B4; one channel, $122); the
  `gDataCnt`/`gDataWidth` fields ($0100/$0008) are visible in each header, per
  ([declaration-rom.md](../declaration-rom.md) §6.3) [1] (*the header words between the
  name and the ramps include a per-table value whose field alignment is not fully
  established*, §6.10).
- **The monitor timing directory** at chip $62E: twelve `(offset, id)` entries —
  ids $81, $46, $80, $82, $40, $43, $45, $41, $6B, $6C, $6D — terminated by a `$FFFF`
  default entry [1]. Each offset points back into a compact table at chip $5E6–$62D of
  twelve 6-byte records (a length word — every record ends at chip $65E — plus four
  config bytes: default depth and flags), and at chip $65E–$675 sit three 8-byte
  **depth-family lists**: `{$80,$82,$84,$86,$88}`, `{$80,$82,$84,$86,$88,$8A}`,
  `{$80,$82,$84,$86,$88,$8A,$8C}` — the per-monitor mode-template lists PrimaryInit
  rebases and stamps into BOARDCFG+$E/+$12 (§2.6). What selects between the three lists is
  not established (§6.3).

The two SExecBlocks — PrimaryInit (chip $000C–$09FF, block length $9BE, revision-$02
sExec header) and SecondaryInit (chip $0B94–$0EE5) — and the driver block (chip $19D6,
DRVR header, Pascal name ".Display_Video_Apple_Boogie" at $1A04, an embedded "1.1"
version string) complete the image; everything between is data [1]. All three code bodies
execute as **relocated heap copies** loaded by the Slot Manager — the chip offsets below
are image positions, not run-time PCs [1].

### 4.2 PrimaryInit

PrimaryInit (the first SExecBlock, run by the Slot Manager per
[declaration-rom.md](../declaration-rom.md) §8) performs, in order [1] (chip $000C–$09FF):

1. **Version probe.** `_SlotManager` selector `sVersion`; the result is cached but only
   gates later SRT work.
2. **Slot PRAM read/repair.** `sReadPRAMRec` into a local record; two stale vendor bits in
   byte +4 are cleared and the record written back with `sPutPRAMRec`. If the saved mode
   byte +2 is not a plausible id (> $7F check), the record is re-seeded: mode `$80`, depth
   index 0, monitor-id placeholders `$F9` in bytes +6/+7.
3. **Enter 32-bit mode; park the depth.** `_SwapMMUMode`; DEPTH ($D80005) cleared (slowest
   clock) before any sensing.
4. **Monitor sense** (§3.2) and the VIDCTL `$47` gate.
5. **Timing-directory lookup.** The monitor id indexes the chip-$62E directory; the
   record's default-depth byte seeds the depth index (kept from PRAM if valid); two config
   bytes are cached for the BOARDCFG stamp.
6. **sResource refresh.** Back in the caller's mode, the sRsrc list is resolved through
   `sFindStruct`/`sGetBlock` and the per-mode enable/disable scan (below) is prepared.
7. **PLL program** (§3.3) with the configuration-selected synthesizer word.
8. **RAMDAC init.** Command `$E3` at $C80006; CLUT end-of-load strobe; a 16-entry blank
   through the init port; VIDCTL written from its shadow with the config-commit strobe
   (bit 5 low→high).
9. **Branch on the monitor id.** If the saved id is `$47`, the self-test is skipped
   (D1=D2=0) and the flow jumps straight to the BOARDCFG stamp — the fatal path of §3.2.
   Otherwise DEPTH is set from the cached mode byte, a second CLUT blank runs, the
   framebuffer's first 128 rows are cleared in 16-byte chunks, and `ClutPreloadRamp`
   loads the black/white endpoints.
10. **Board self-test.** A timed poll of STATUS bits 6 and 5 (budget `TimeDBRA << 5`):
    bit 6 ever asserted clears the D1 residue, bit 5 clears D2. The loop never halts early
    — it is a fixed-budget sampling of two status flags whose hardware meaning is
    inferred from the fill that precedes it (§6.5).
11. **BOARDCFG stamp** (§2.6): `[0] = D1|D2`, the config/depth words, the VIDCTL shadow,
    rowBytes/geometry — and the downgrade: residue 0 forces the monitor id to `$47`, the
    depth index to 0, and flags an SRT update.
12. **Per-mode sRsrc scan.** Walking depth indices base..top, each family-list mode id is
    enabled or disabled in the Slot Resource Table with `sSetsRsrcState`; the default mode
    is the one that survives (a different default is preferred when the hardware count is
    ≥ 2).
13. **Standard-monitor epilogue.** If the id is `$47`, a final CLUT blank and a DEPTH
    commit run on the way out.
14. **Persist.** If anything changed, `sPutPRAMRec` writes the record back; `seStatus` is
    left at 1 (success, set at entry).

The net observable contract of a healthy card after PrimaryInit: monitor sensed and
matching PRAM, CRTC/PLL/RAMDAC programmed for the default mode, VRAM cleared to the gray
ramp, interrupts still masked (VIDCTL bit 7 set — PrimaryInit never unmasks), BOARDCFG
stamped with `BOARDCFG[0]` nonzero, and the SRT trimmed to the modes the sensed monitor
supports [1].

### 4.3 SecondaryInit

SecondaryInit (the second SExecBlock, run by Slot Manager version 2+ after 32-bit
QuickDraw is available; [declaration-rom.md](../declaration-rom.md) §9) restores the user's
preferred mode across the PrimaryInit defaulting [1] (chip $0B94–$0EE5):

1. Queries the Slot Manager version. On version ≥ 2 it takes the PRAM path: read the
   slot's PRAM record, set a "configured" bit, write it back; if the record was already
   configured, it walks the live GDevice list to this slot's DCE, and calls a helper
   **inside the already-loaded driver** (at driver offset +$1164) with three arguments
   from the driver's private storage (the same default-mode triple Open uses), applying
   the saved mode directly.
2. On older Slot Managers (or when that path finds nothing) it requires 32-bit Color
   QuickDraw (trap `$AB03` compared against `_Unimplemented`), hides the cursor, scans
   the mode sResources `$80`–`$93` with `sRsrcInfo` for the active one, and reads the
   preferred mode either from the driver's private config block or — falling back — from
   BOARDCFG at slot+$FFFD8 directly (current mode byte +3, saved preference +5, and the
   family list at +$E/+$12).
3. If a saved preference exists, the old default is deleted from the SRT
   (`sDeleteSRTRec`), the preferred id inserted (`sInsertSRTRec`), and the other family
   candidates re-stamped.
4. Finally it strips the 32-bit address-flag bits (`$FF0FFFFF`) from the live device's
   PixMap base and the driver-private framebuffer pointer so QuickDraw and the card agree
   on VRAM addressing.

### 4.4 The video driver: Open and Close

The driver is a standard video DRVR ([declaration-rom.md](../declaration-rom.md) §7, §10);
its unit-table name is `.Display_Video_Apple_Boogie`, its routine offsets place Open at
chip $1AC6, Close at $1DE0, Control at $1E34 and Status at $291A, and a VBL ISR is
installed at chip $1A3A [1].

**Open** (chip $1AC6–$1DDE) [1]:

1. Derives the slot base from `dCtlDevBase` & `$FF000000`, and reads **`BOARDCFG[0]`**
   under `_SwapMMUMode`. **Zero → the card is dead**: the driver deletes this slot's SRT
   records `$80`–`$93` and returns **`openErr` (-23)** after tearing down. (*Observed*:
   pre-stamping BOARDCFG before the Open makes it return `noErr` instead.)
2. Otherwise: repairs the slot PRAM record (sets a "video configured" bit), and — unless
   running under A/UX (`HWCfgFlags` bit 9) or the record was already valid — reprograms
   the default mode by calling its internal mode programmer with BOARDCFG bytes/words
   +6, +8, +$A as arguments (the PrimaryInit default triple).
3. Allocates $4E bytes of locked DCE private storage (stamped `'Bgy1'`), snapshots the
   whole BOARDCFG into it and into a $976-byte work buffer, builds a 3×256-byte identity
   gamma ramp at work+$76 (the driver's default gamma), resolves the framebuffer base from
   the sResource tree (`sFindSInfoRecPtr`, `sFindStruct`, `sGetBlock` — the Apple
   base-address entry) and stores slot base + 0 as the framebuffer pointer.
4. Installs the VBL: a slot-interrupt queue element (type 6, priority $80, private storage
   as the parameter) through `_SIntInstall`, plus (not under A/UX) the relocated Time
   Manager re-arm task of §3.10.
5. **Unmasks the VBL** (VIDCTL bit 7 clear) and returns `noErr`.

**Close** (chip $1DE0–$1E32) masks the VBL (bit 7 set), frees the work buffer, removes the
slot interrupt handler (`_SIntRemove`), frees the queue element and the framebuffer
sResource block, removes the re-arm task, and returns `noErr` [1].

### 4.5 The video driver: Control calls

Control (chip $1E34) dispatches `csCode` through a twelve-word jump table, with two
vendor codes handled ahead of the table [1]:

| csCode | Routine | Chip | Behaviour |
|---|---|---|---|
| 0 | `cscReset` | $1E98 | stamps mode id `$80`, falls into `cscSetMode` |
| 1 | `cscKillIO` | $1E88 | returns `noErr` (no-op) |
| 2 | `cscSetMode` | $1EB4 | validates the requested mode id against `$80`–`$93` and the mode list maximum; extracts the depth token (`& $87`), skips if unchanged; then: CLUT index write `$FF` (reset sub-counter), three `$60` data writes, end-of-load strobe, VIDCTL low-bits re-latched from the depth table, MODE depth bits replaced (timing preserved), and — in direct mode with matching CONFIG bits — an engine/PLL reprogram |
| 3 | `cscSetEntries` | $1FA6 | the CLUT load of §3.4: sequential (csStart = −1) or indexed (csStart ≥ 0) paths, optional luminance mapping (weights `$4CCC`/`$970A`/`$1C28` ≈ 0.30/0.59/0.11), gamma-mapped, inverted, staged in a 1 KB stack buffer, and pushed to hardware in the STATUS-bit-4 safe window; rejected in direct mode |
| 4 | `cscSetGamma` | $2242 | installs a gamma table; in direct mode rebuilds the linear CLUT (the `csParam=$84` variant stages a plain per-index ramp; otherwise a 32-group 5:5:5-style remap loads 256 entries, §6.14) |
| 5 | `cscGrayPage` | $2316 | the desktop gray fill (§3.6): shallow depths by CPU loop, deep modes through the engine with CONTROL=$01; finishes by toggling the VIDCTL commit strobe; in direct mode also reloads the CLUT |
| 6 | `cscSetGray` | $25BE | sets/clears the luminance-mapping flag; colour requests are refused (`controlErr`) when the config block marks the display monochrome |
| 7 | `cscSetInterrupt` | $1E84 | **unsupported** — `controlErr` (-17) |
| 8 | `cscDirectSetEntries` | $1F94 | the direct-mode CLUT load; requires the direct-mode token, shares the SetEntries body |
| 9 | `cscSetDefaultMode` | $25DC | records the default mode id for the next boot through the Slot Manager (`sReadInfo`/`sPutPRAMRec`), validating `$80`–`$93` |
| 10 | `cscSwitchMode` | $262C | the full mode switch: resolves the mode sResource via the Slot Manager, commits the new id into the DCE, then calls the **CRTC programmer** below |
| 11 | `cscSetSync` | $2832 | sync/blanking through the DEPTH high nibble: sync-on stashes the high nibble in the config block and clears it in the register; sync-off restores it |
| $80 (vendor) | direct-mode toggle | $2890 | sets/clears VIDCTL bit 3 from the csParam word |
| $83 (vendor) | gamma select | $28C0 | installs a gamma table by id: the id is validated against the eleven-entry list at chip $2B4A (`$81,$46,$80,$82,$40,$43,$45,$41,$6B,$6C,$6D`, `$F9`-terminated — the same monitor-id set as the timing directory) and installed via `sReadInfo`/`sPutPRAMRec` |

The **CRTC programmer** (chip $26B8, called from Open, `cscSetMode` and `cscSwitchMode`)
is the card's master mode-change sequence [1]: enter 32-bit mode; load the nine CRTC bytes
from the mode's timing table, descending ($D80035 first); update MODE's depth bits and
timing index; write DEPTH's low nibble (the clock field — the high nibble stashed to the
config block when the monitor record demands it); record rowBytes and row count into
private storage; bit-bang the PLL divisor (§3.3); then **clear the whole framebuffer
through the engine** — CONTROL=$01, CONFIG-bit-0 chunk width, pattern `$FF` (deep) or
`$00` (shallow), kick `$8`, and chunk-descriptor writes `$40000080`/`$40000200` for
`$7C08`/`$1F02` iterations — and commit with the VIDCTL bit-5 strobe.

### 4.6 The video driver: Status calls

Status (chip $291A) range-checks `csCode` to 0–$D through a fourteen-word table, with two
vendor selectors handled ahead of it [1]:

| csCode | Routine | Returns |
|---|---|---|
| 2 | `GetMode` | current mode token (`& $87`), page 0, the private framebuffer base in `csBaseAddr` |
| 3 | `GetEntries` | the palette from the 3×256 shadow (each component re-inverted, §3.4), bounded by the per-depth max index (CountTbl at chip $2A28: `1`, `$0F`, `$FF`, `$1F`, `$FF` for depth codes 0–4) |
| 4 | `GetPageCnt` | always 1 — single page in every mode |
| 5 | `GetPageBase` | the private framebuffer base |
| 6 | `GetGray` | the luminance-mapping flag |
| 7 | `GetInterrupt` | **unsupported** — `statusErr` (-18) |
| 8 | `GetGamma` | pointer to the current gamma table |
| 9 | `GetDefaultMode` | the default mode from slot PRAM |
| $A | `GetCurMode` | the saved sResource depth-mode id in `csData`, then GetMode's fields |
| $B | `GetSync` | the sync state from the config block |
| $C | `GetConnection` | the display/connection code from the config block |
| $82 (vendor) | — | a slot-info lookup through `sRsrcInfo` |
| $84 (vendor) | mode-name query | copies the PRAM monitor-id bytes into csParam, writes the OSType `'mTyp'`, and returns a pointer to the chip-$2B4A id table |
| 0, 1, $D | `StatBad` | `statusErr` |

Note what is *absent*: there is no hardware cursor, no `VideoBase`/`RowWords` register
pair, and no multi-page support — the framebuffer base and rowBytes live in the driver's
private storage and the mode tables, not in slot registers [1].

### 4.7 Depth and mode encoding

The card's depth ladder has **five depths and no 2-bpp mode** [1] (depth table at chip
$1F88, pack-shift and count tables at $2230/$2A28, and the per-mode VPBlocks' `pixelSize`
values):

| Depth code | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| bpp | 1 | 4 | 8 | 16 | 32 |
| sub-mode id (within a family) | $80 | $81 | $82 | $83 | $84 |
| VIDCTL low bits (OR'd) | $04 | $05 | $06 | $07 | $00 |
| MODE bits 7–5 | $00 | $20 | $40 | $A0 | $C0 |
| max CLUT index | 1 | $0F | $FF | $1F | $FF |
| CLUT index pack shift | 7 | 4 | 0 | 3 | — |

Mode ids addressed by the OS are `family id + depth`: the mode token the driver keeps is
`depth | $80` (bit 7 marks the direct/direct-capable flag), and `cscSetMode` indexes the
depth table with `token & 7` [1] (chip $1EDA–$1F04). Codes 5–7 of the depth table (MODE
values `$3C`/`$2B`/... on the hi-res PLL branch) exist in the table but map to no shipped
desktop mode; their meaning is open (§6.3).

**Slot PRAM** holds the persistence (layout per [declaration-rom.md](../declaration-rom.md)
§5.2): bytes +0/+1 the BoardID, **+2 the saved mode id** (the system's `VendorUse1`
depth-restore byte, written by the Monitors panel), +3 the saved sResource id/depth index,
+4 vendor flags (bit 0 "mode valid", bit 1 "video configured"), +6/+7 the saved monitor-id
mirror pair [1] [2]. PrimaryInit keeps a saved mode in +2 only if it names a depth the
sensed monitor supports, and normalises an invalid one back to the 1-bpp default — so the
boot depth is the OS's choice constrained by the card's family list, never the card's [1]
(*observed*: seeding +2 to the 8-bpp sub-mode of the 640×480 family boots an 8-bpp colour
desktop; an unsupportable value boots 1 bpp).

### 4.8 Host-ROM bring-up and the failure path

The host Start Manager finds the card through the standard slot search
([nubus.md](../nubus.md) §4.1, [declaration-rom.md](../declaration-rom.md) §11):
`OpnVidDeflt` gets the video default, checks the sRsrcType (CatDisplay/TypVideo/DrSWApple),
reads the default mode's `mVidParams` into the low-memory screen globals, opens the slot's
driver — and any failure falls back to `InitDummyScreen`, a 32×32 1-bpp GDevice with a
junk base address (*observed*: a 24-bit-mode boot resolves the screen base to `$F9900000`,
the alias of §3.9, and stamps `VideoMagic` `$5A932BC7` on success) [1] [3] (p. 241
describes the Start Manager sequence and the 1-bpp/gray/VBL-off rules PrimaryInit
follows). The driver `Open` returning `openErr` — the BOARDCFG gate of §4.4 — is the
failure that produces the dummy screen; the `rowBytes = 4, bounds 32×32` GDevice is its
fingerprint [1].

Two secondary bring-up facts matter to reimplementation. First, the driver and both
SExecBlocks run as **relocated heap copies**, so their run-time addresses are not stable
identifiers — only their register writes and the low-memory/BOARDCFG side effects are
[1]. Second, on 68k hosts in 24-bit Memory Manager mode the whole boot draws through the
$900000 alias; the accelerator never engages (§3.1) [2].

### 4.9 The QuickDraw accelerator control panel

The acceleration product is a separate install: the control panel **"Apple Macintosh
24AC"** (type `cdev`, creator `'BØØg'`, a fat binary — 68K `INIT`/`QCOD`/`QDPA` resources
plus a PowerPC PEF code fragment in the data fork, named in its `cfrg` resource), shipped
on an 800 KB Disk Copy 4.2 floppy with its Installer script and Apple Installer 3.4 [2].
The page's concern is what its 68K side makes the *card* do.

**Boot.** `INIT` 0 runs at start-up and gates the environment through Gestalt: System ≥
6.0.4, 32-bit Color QuickDraw > 2.0, and — via `gestaltSysArchitecture` — a 68K CPU; on
PowerPC machines it bails (the PEF fragment owns that platform) [2]. It then probes for
the card through its `Bütt` code resource (selectors 0/1/2/4), loads `GDEF` and the patch
resources, registers a private Gestalt selector (`'BØØg'`) whose value is a shared state
block, and persists its effective configuration into the `cfig` resource [2].

**The hook layer.** The accelerator does *not* patch `_CopyBits` or install
`grafProcs`. It replaces **thirteen of QuickDraw's internal blit primitives in the trap
dispatch table** — the hooks that QuickDraw's BitBlt/RgnBlt/StretchBits paths call through
the trap table, which is what makes them replaceable at all [2]; the design-book context
for video acceleration is QuickDraw's direct access to the card's VRAM
([3] p. 241):

| Trap | Primitive | Patch body | Tier |
|---|---|---|---|
| $AB34 | `_bSETUP8` (pattern fill, 8-bit) | `QCOD+0x01C2` | hardware |
| $AB0C | `_FastSlabMode` (solid slab fill) | `QCOD+0x11F4` | hardware |
| $AB5E | `_rMASK8` (region-masked fill, 8-bit) | `QCOD+0x035A` | hardware |
| $AB38 | `_bXMAIN8` (transfer-mode blit, 8-bit) | `QCOD+0x1010` | hardware |
| $AB84 | `_slXMASK8` (stretched masked blit, 8-bit) | `QCOD+0x0DEA` | hardware |
| $AB62 | `_rXMASK8` (transfer-mode masked fill, 8-bit) | `QCOD+0x0BAA` | hardware |
| $AB58 | `_bSetup0` (68040 fast copy setup) | `QCOD+0x052A` | hardware |
| $AB59 | `_bLeft0` (68040 fast copy left edge) | `QCOD+0x06B0` | hardware |
| $AB5A | `_rMASK0` (region-masked fill, 1-bit) | `QCOD+0x084C` **over** `QDPA 1` | hardware, degrading to software |
| $AB24 | `_SetUpStretch` (scaling setup) | `QCOD+0x1432` | hardware — **System 7.0+ only** |
| $AB99 | `_stScanLoop` (stretch DDA) | `QDPA 0` | software (terminal) |
| $AB40 | `_bEND0` / $AB30 `_bMAIN0` (basic copy loop) | `QDPA 2` | software (terminal) |

**Install mechanism.** The patches go in two passes, in this order [2]:

1. The **`QDPA` software pass** patches four traps directly with `_SetToolTrapAddress`,
   saving no original — those bodies are terminal replacements.
2. The **`QCOD` hardware pass** walks a table-driven installer over the `QCOD` resource's
   8-byte entries `{trapWord:W, patchOffset:W, savedOriginal:L}` — one sub-list installed
   unconditionally (nine entries, terminated by a zero trap word) and a System-7-only
   sub-list (the `SetUpStretch` entry) selected by a three-word dispatch header gated on
   the cached Gestalt `'sysv'`. For each entry it calls `_GetToolTrapAddress` to save the
   displaced handler, then installs `QCOD base + patchOffset` (bit 11 of the trap word
   selects the OS-versus-ToolBox variant).

Because the software pass runs first, the hardware `rMASK0` installed second *saves the
`QDPA 1` software body as its chain-through target* — the one hook with both a hardware
and a software tier, degrading hardware → software instead of hardware → ROM [2].

**Accelerate-or-chain.** Every hardware body opens with the same eligibility gate; any
failure tail-jumps into the saved original (the classic `MOVE.L <savedOriginal>,-(A7)`
/ `RTS` idiom, e.g. the `$0354` reject of `bSETUP8`) [2]. The gate tests, in order: the
destination's slot is in the accelerator's capability bitmap, the run is wide enough
(≥ $20 longs), the `MMU32bit` flag is set, the per-slot aperture base is nonzero, and the
destination row-bump is longword-aligned. The per-slot aperture bases were built at init by
walking the Slot Manager's device list for the card, reading the card's STATUS/CONFIG once
under `_SwapMMUMode`, and storing `$Fs0FE000`/`$Fs3FE000` per slot [2].

**What is and is not accelerated.** Pattern fills, transfer-mode fills, region-masked
fills, solid slabs and scaled blits run on the engine (§3.5–§3.8); the edges of every run
are still software. A plain `CopyBits` move is **never** hardware-accelerated: the basic
copy hooks (`bMAIN0`/`bEND0`) are replaced by *tighter CPU loops* — self-contained
scanline loops with a source-overrun guard the ROM loop did not need — that write straight
into the card's VRAM [2]. And the accelerator ships no 68040-tuned software of its own:
its software bodies are plain 68020-class loops (`BFEXTU`/`MOVE.L`, no `MOVE16`); the
genuine `MOVE16` fast copy belongs to the ROM, and the hardware `bSetup0`/`bLeft0` bodies
chain back to it when they decline [2].

## 5. Quirks & errata

- **VIDCTL's low three bits must read 2 at power-on.** It is the least-documented and most
  consequential reset value on the card: any other value forces the `$47` monitor path,
  which skips the self-test, leaves `BOARDCFG[0]` = 0, and makes the driver return
  `openErr` (§2.7, §3.2, §4.4).
- **One failed self-test deletes the card.** The driver's Open purges sResources `$80`–
  `$93` from the Slot Resource Table on a zero board-OK flag — the slot then enumerates as
  empty for the rest of the boot (§4.4).
- **The CLUT port is active low** — index *and* components — while the init port is not;
  and because white/black are self-complementary, a non-inverting port still renders a
  correct monochrome desktop while every colour is wrong (§2.2, §3.4).
- **The engine's count word carries two flag bits.** Bit 30 marks the execute/fill write,
  bit 31 the backward direction; the byte count is the longword with both masked off. A
  count read that misses a flag makes fills run to the end of VRAM or copies span
  gigabytes (§3.6, §3.7).
- **The copy source pointer auto-increments per executed length.** One latch feeds many
  executes — the accelerator relies on it to split copies at 8 KB destination boundaries,
  and a fixed source smears stale pixels along boundary rows (§3.7).
- **The commit value differs by caller**: the accelerator writes `4` twice at the operand
  aperture's active alias; the driver writes pattern longwords at the passive fill base
  and kicks with `$8`. Both are proven rituals over the same engine; the unified register
  model behind them is not established (§3.5, §3.6, §6.7).
- **Run lengths are software-clamped to a stripe width** before the flags are OR-ed in, so
  the low bits of a command word never collide with the flag field (§3.6).
- **Registers are lane-3 bytes spaced by 4**, and every access is bracketed by
  `_SwapMMUMode`; the accelerator refuses the hardware path entirely under the 24-bit
  Memory Manager (§3.1).
- **The framebuffer answers at slot+$900000 as well as at slot+0** — the 24-bit base form —
  and that alias must stop below the register pages or it shadows the CLUT block; the
  card's visible extent therefore tops out at $380000 in that window (§2.1, §3.9).
- **The monitor-sense port is stateful.** Extended sense drives line groups and reads the
  others back; a static sense byte can only ever produce a simple id — the multisync and
  Radius hi-res monitors are unreachable without the stateful probe (§3.2).
- **`$47` is a marker, not a monitor.** It is never sensed and has no timing-directory
  entry; it exists only as the forced fallback — and everything that lands on it fails
  (§3.2).
- **No 2-bpp depth.** The ladder runs 1/4/8/16/32; the depth table's codes 5–7 are
  populated but unexplained (§4.7, §6.3).
- **`cscSetInterrupt` and `GetInterrupt` are unimplemented** — `controlErr`/`statusErr`.
  The card's only interrupt is VBL, and only through VIDCTL bit 7 (§4.5, §4.6, §3.10).
- **Plain `CopyBits` is never accelerated.** The basic copy hooks are *software*
  replacements — tighter CPU loops — and the accelerator has no `MOVE16` code of its own;
  the 68040 fast path belongs to the ROM (§4.9).
- **`rMASK0` is the only double-installed hook.** The software pass installs first, the
  hardware pass layers on top and chains to the software body — the one hook that degrades
  hardware → software rather than hardware → ROM (§4.9).
- **`SetUpStretch` is hooked only on System 7.0+**; on 6.0.x the stretch path runs the ROM
  setup with the card still accelerating the per-scanline fills (§4.9).
- **The driver runs as a relocated heap copy**, as do both SExecBlocks — run-time addresses
  are not stable identities for these routines (§4.8).
- **Gamma ids are monitor ids.** The vendor gamma-select call validates its id against the
  same eleven-entry monitor-id list as the timing directory (§4.5).

## 6. Open questions

1. **The silicon.** No schematic, datasheet or part list exists in the evidence set: which
   ASIC implements the engine, the RAMDAC, the CRTC and the clock synthesizer, and how the
   8 KB copy-boundary behaviour arises from the VRAM organisation, are all open (§1.1,
   §3.7).
2. **VRAM sizes.** The two organisations are inferred as 1 MB-class and 4 MB from the
   aperture offsets and the mode list; no document states either, and the small variant's
   exact framebuffer extent is unknown (§1.4).
3. **The mode sResource map.** Which of the twenty directory ids `$80`–`$93` corresponds
   to which monitor×geometry×depth combination, what selects between the three
   depth-family lists at chip $65E, and what depth-table codes 5–7 (`$3C`/`$2B`) mean, are
   undecoded beyond the mode-name list (§4.1, §4.7).
4. **CRTC semantics.** The nine timing bytes are known only as opaque per-mode table
   values; no field (hsync/vsync/blanking widths, dot clocks) is identified, and the
   tables' exact offsets per mode are not tabulated here (§2.5, §4.5).
5. **The self-test's hardware meaning.** STATUS bits 5/6 are read as "fill-quadrant done"
   flags from the polling logic alone; what the hardware actually reports, and what a
   genuinely faulty board does, is unknown. The precise software contract: the board-OK
   residue is `D1|D2`, zero only if *both* bits were seen asserted during the poll window —
   and a healthy board leaves both clear throughout (*observed*) (§2.3, §4.2).
6. **STATUS bit 4.** The poll is proven; whether the bit tracks vertical sync, a blanking
   interval, or a RAMDAC-specific "safe window", and its polarity, are not (§2.3, §3.4).
7. **The operand-register model.** The accelerator's aperture-load + double-`$4`-commit
   and the driver's passive-base pattern pair + `$8` kick are both proven rituals; whether
   they address the same register file, what `$4` versus `$8` commands, and why the commit
   is written twice, are unresolved (§3.5, §3.6).
8. **The raster-op codes.** The QuickDraw-mode → CONTROL code formula is proven; the
   raster-op each of the 64 codes selects has never been observed end-to-end (§3.8).
9. **The off-card source stream.** `bXMAIN8` streams source pixels through the active
   aperture for off-card sources; no observed session exercises it, so its handshake (if
   any) and raster-op behaviour are unverified (§3.8).
10. **Gamma block headers.** The three tables' names, sizes and 256×8 ramps are read; the
    exact field alignment between the name and the ramps (one word per table resists the
    standard layout) and how the Monitors panel selects among them are open (§4.1, §4.5).
11. **Reset state beyond VIDCTL's low bits.** No shipped code reads any register before
    writing it except STATUS/CONFIG/VIDCTL at init and the sense port, so power-on values
    (and any NuBus-reset-versus-power-on distinction) are unconstrained (§2.7).
12. **Register decode granularity.** Whether the card answers offsets around the named
    registers within each 64 KB page, whether the named registers alias, and whether
    BOARDCFG is register-backed or plain VRAM, are unknown (§2.1, §2.6).
13. **VBL timing.** The frame rate's exact value, the ISR's 6 ms re-arm rationale (the
    top-of-screen cursor guard is *inferred* by analogy to the built-in RBV path), and
    whether the card offers any other interrupt source are open (§3.10).
14. **Direct-mode ("magic") pixel formats.** The 16/32-bpp direct pixel packing and the
    5:5:5-style remap in the direct-mode CLUT reload are read from the load loop but not
    decoded against a live display (§2.3, §4.5).
15. **The PowerPC accelerator path.** The control panel ships a PowerPC code fragment
    (`Joy!peff`/`pwpc`) that the 68K `INIT` deliberately leaves in charge on PowerPC
    machines; its register usage is not in the evidence set, and whether it drives the
    same engine contract is unverified (§1.5, §4.9).

## Appendix A. Granny Smith implementation notes

The emulator's model of this card lives in
[display_card_24ac.md](../../../../internals/core/peripherals/nubus/cards/display_card_24ac.md)
(internals mirror of the card model); the coverage here is the hardware authority it
cites. Two verification levers exist in the tree: the engine-vs-software-fallback oracle
(forcing the accelerator's eligibility gates to fail makes the patched primitives fall
back to CPU writes into the passive bank, giving a golden reference for every accelerated
operation), and the integration test that boots the card in a IIcx to a pixel-exact Finder
desktop at 1 bpp and 8 bpp.

## References

1. Apple Macintosh Display Card 24AC video-ROM disassembly — the card's declaration ROM
   (internal name "Radius Boogie"), raw chip image 32,768 bytes, firmware version 1.1,
   part number 630-0908, dated 02-Dec-93, format-block CRC $D8DAAB87; annotated 68K
   disassembly of the full chip: the region map (first-block header $0000; PrimaryInit
   $000C–$09FF; mode params and identity strings $0A00–$0B93; SecondaryInit $0B94–$0EE5;
   mode strings, sResource directory, gamma directory and driver header $0EE6–$1A2D; the
   driver $1A2E–$2BFF; trailing mode/CRTC/gamma tables $2C00–$7FDF; format block $7FE0–
   $7FFF). Chip offsets cited in the prose are into this image.
2. "Apple Macintosh 24AC" QuickDraw accelerator control panel, version 1.2, © 1990–1994
   Radius Inc. — disassembly of the `INIT`/`QCOD`/`QDPA`/`Bütt` resources (the install
   engine, the trap-patch table and the thirteen hook bodies) and recorded traces of the
   live engine traffic (fill run-lengths with the bit-30 flag, copy latch/execute pairs,
   the 8 KB boundary splits and the bit-31 backward copies), together with the boot
   observation log of the card in a Macintosh IIcx (System 7.0.1, 1-bpp and 8-bpp
   desktops).
3. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third
   edition, Addison-Wesley Publishing Company, 1992 — Chapter 10 "NuBus Design Examples"
   pp. 221–244 (§"Video drivers" p. 241: the Start Manager's video bring-up sequence, the
   1-bpp/page-0/gray-clear/VBL-off init rules, the slot-PRAM depth byte, and the
   no-monitor sResource-removal rule; §"Video declaration ROM information" p. 242; the
   video driver's Open/Control/Status requirements p. 244); Chapter 11 "The Macintosh II
   Video Card" pp. 245–302 (§"Declaration ROM operation" p. 296; §"The driver" p. 297;
   §"The primary initialization code" pp. 298–299, including the monitor-sense
   description; the video connector and its sense lines, Table 11-1 p. 300).
4. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition,
   Addison-Wesley Publishing Company, 1990 — Chapter 12 "Displays" pp. 397 ff. (§"Video
   card features" and §"Video card components" pp. 404–406; the three monitor sense lines
   and their connector pins p. 447 with the sense-line value table Table 12-2 p. 448; the
   Macintosh IIci's sense-line monitor table pp. 457–459). Cited for the standard three-
   line sense scheme the card's sensing implements; the extended drive-and-read-back
   variant of §3.2 is established from the card's own ROM [1].
