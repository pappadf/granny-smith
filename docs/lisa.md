# Apple Lisa 2 and Macintosh XL — System Architecture

This document is a self-contained hardware reference for emulating the **Apple
Lisa 2** (the 3.5″-floppy Lisa, 1984) and its rebadged sibling the **Macintosh
XL** (1985). It covers the processor, the custom segment MMU, the three physical
address spaces, every on-board peripheral, and the exact register/bit layouts an
emulator must reproduce.

The Lisa is **not** a Macintosh architecturally — it predates the Mac and shares
only the 68000 core, the Zilog Z8530 SCC, and the MOS 6522 VIA part type. It has
its own custom memory-management unit, its own keyboard/mouse/clock
microcontroller (the COPS), an intelligent floppy coprocessor, and a parallel
(not SCSI) hard-disk interface.

> **Scope note.** The generic internals of the 6522 VIA (timers, IFR/IER, shift
> register, handshake modes) and the Z8530 SCC (write/read register file, baud
> generator) are documented in [via.md](via.md) and [scc.md](scc.md). This
> document specifies only how those parts are *wired and addressed* on the Lisa.
> Everything else needed to model the machine is contained here.

Where a value is described as authoritative it comes from the *Apple Lisa
Hardware Manual* (1983), the *Lisa Device Driver Manual* (1984), the Lisa Boot
ROM source (rev 2.48 / "H"), and Apple's released Lisa OS driver source.

---

## 1. Architectural Overview

```
              +-------------------------------------------------+
              |  68000 CPU @ 5.09 MHz (24-bit logical address)   |
              +------------------------+------------------------+
                                       | logical address
                          +------------v------------+
                          |   Custom segment MMU     |  1K x 12-bit descriptor RAM
                          |  128 segs x 4 contexts   |  512-byte pages
                          +----+---------+-----------+
            space tag (MEM/IO/SPECIAL) | physical address (21-bit)
       +---------------+---------------+----------------+--------------------+
       |               |                                |                    |
+------v-----+  +-------v--------+              +--------v-------+  +---------v--------+
| Main RAM   |  | I/O space      |              | Special I/O    |  | Video (in RAM) |
| <= 2 MB    |  | $00C000-$00FFFF|              | 16 KB boot ROM |  | 720x364 1bpp   |
| phys 0     |  |  FD/IO/CPU regs|              | + MMU registers|  | base = latch   |
+------------+  +--+----+----+---+              +----------------+  +----------------+
                   |    |    |
       +-----------+    |    +-----------------+
       |                |                      |
+------v------+  +------v------+      +--------v---------+
| Floppy 6504A|  | VIA1 + COPS |      | VIA2 + parallel  |
| $00C001     |  | $00DD81     |      | port (ProFile/   |
| Sony 400 KB |  | kbd/mouse/  |      | Widget HD)       |
+-------------+  | RTC/power   |      | $00D901          |
                 +-------------+      +------------------+
       +----------------+
       | Z8530 SCC       |  serial A/B  $00D241-$00D247
       +----------------+
```

**Core chip complement**

| Function | Device | Notes |
| --- | --- | --- |
| CPU | Motorola 68000 | 5.09375 MHz, 24-bit logical |
| Memory management | Custom Apple segment MMU | 1K×12-bit descriptor RAM, external to the CPU |
| Keyboard / mouse / clock / power | COPS (COP421-class) microcontroller | on the A-port of VIA1; always-on (standby supply) |
| Parallel I/O ×2 | two MOS 6522 VIAs | VIA1 = COPS, VIA2 = parallel hard-disk port |
| Serial | Zilog Z8530 SCC | two channels, autovectored |
| Floppy | 6504A-based intelligent controller | one Sony 400 KB 3.5″ drive (Lisa 2 / XL) |
| Hard disk | parallel-port ProFile (external) / Widget (internal 2/10) | NOT SCSI |
| Video | discrete state machine + DAC | 720×364 1 bpp, framebuffer in main RAM |

### 1.1 Lisa 2 vs. Macintosh XL

The two machines are the **same hardware**. The Macintosh XL is a Lisa 2/10 sold
from January 1985 with three changes:

1. **Boot ROM.** Lisa 2 uses the "H" ROM (parts **341-0175-H** + **341-0176-H**);
   the Macintosh XL uses the "3A" ROM (**341-0346-A** + **341-0347-A**), which is
   MacWorks-only and assumes the screen-modification timing. The 3A ROM will not
   run the Lisa Office System.
2. **Screen-modification kit.** Rewires the video timing to a **608-pixel-wide
   square-pixel** raster (the stock Lisa 2 is 720×364 with tall rectangular
   pixels). The nominal Screen-Kit raster is **608 × 432**, but the framebuffer
   is a single 32 KB page located by the `$00E800` latch (§8) and MacWorks
   programs the base at the **top** 32 KB-aligned slot ($F8000 on a 1 MB
   machine); that page holds 76 × 431 = 32 756 bytes, so the **framebuffer the
   Finder actually paints is 608 × 431** — the 432nd scanline falls past the top
   of the page. Either way the row stride is **76 bytes**, not the Lisa 2's 90.
   (Verified empirically: a MacWorks Finder framebuffer auto-correlates to a
   76-byte stride and fills all 431 rows; rendering it at 90 bytes shears the
   desktop pattern diagonally.)
3. **MacWorks XL software** (loaded from disk) turns the machine into a
   Macintosh-compatible environment (see §16).

For an emulator, the Macintosh XL is the Lisa 2 model with a different ROM
image, a **608×431 display** (vs the Lisa 2's 720×364), and (by convention) a
different machine name. The chip models are identical.

---

## 2. Processor and Clocks

- **CPU:** Motorola **68000**, 16/32-bit, with a 24-bit external address bus.
  No FPU. No on-chip MMU — memory management is performed by the external
  segment MMU (§4).
- **24-bit program counter (emulation-critical).** Because only A0–A23 are
  driven, the PC and every pointer are effectively 24-bit; the upper byte
  (A24–A31) is ignored on the bus. The Lisa OS exploits this — its inter-segment
  **jump-table** entries (§4.5) carry tag bits in the high byte of the 32-bit
  code pointer and jump to it directly (`JMP/RTS` through the raw entry), relying
  on the CPU to truncate. An emulator that keeps a full 32-bit PC **must mask the
  PC to 24 bits** (`pc &= 0x00FFFFFF`) on every instruction; otherwise a jump
  through such a tagged entry fetches/faults at the wrong (un-truncated) address
  and a demand-segment fault is mis-delivered (the faulting-address compare in
  the fetch-fault path fails — see §4.8). This is a no-op for the Mac Plus / Mac
  XL, whose code never sets the high byte.
- **Clock:** a 20.375 MHz master crystal divided by 4 gives a CPU clock of
  **5.09375 MHz** (~196 ns/cycle). The Hardware Manual rounds this to "5 MHz,
  200 ns."
- **Bus timing:** the CPU and the video logic interleave memory accesses. All
  bus cycles are multiples of 800 ns; instructions longer than 800 ns have wait
  states inserted so every instruction executes in a multiple of 800 ns. An
  emulator that models only CPU frequency (not the interleave) is sufficient for
  functional accuracy.
- **Reset:** the 68000 fetches the initial SSP from logical `$000000` and the
  initial PC from `$000004`. At power-on, however, the MMU is in *setup/START*
  mode (§4.7) and the CPU actually executes the boot ROM out of special-I/O
  space; the RAM reset vectors are not used until the OS installs them. See §15
  for the ROM's own reset vectors.
- **Bus error / timeout:** a device that does not respond within 30–300 µs of
  being addressed generates a 68000 Bus Error (the timeout is latched as the
  `Bus Timeout` bit in the Status Register, §7.4). MMU access violations also
  raise a Bus Error (§4.8).

---

## 3. Memory Architecture — Three Address Spaces

The MMU's output drives a 3-way space decode. The three spaces are **disjoint**
(they only collide numerically; the MMU's space tag keeps them separate):

| Space | Selected when (SLR access bits) | Contents |
| --- | --- | --- |
| **Main memory** | memory codes (`01xx`) | RAM, up to **2 MB** at physical `$000000`. The video framebuffer lives here. |
| **I/O** | `1001` | memory-mapped devices, physical `$00C000–$00FFFF` |
| **Special I/O** | `1111` | the 16 KB boot ROM **and** the MMU descriptor registers |

### 3.1 Physical main-memory map

| Physical range | Contents |
| --- | --- |
| `$000000 – $07FFFF` | **unpopulated** (no RAM here — the memory boards sit *high*) |
| `$080000 – $1FFFFF` | RAM. Boards are based at `$080000`: 512 KB ⇒ `[$80000,$100000)`, 1 MB ⇒ `[$80000,$180000)`, 2 MB ⇒ `[$80000,$200000)` (= 1.5 MB usable in the 2 MB / 21-bit space). |
| (within RAM) | the 32 KB video page, located by the Video Address Latch (§8) — near the top of RAM (e.g. `$1F8000` on a 2 MB machine) |

Physical addresses are 21 bits (2 MB). **RAM is based HIGH at `$080000`, NOT at
physical 0** (a long-standing error to watch for): physical `[0, $80000)` is
unpopulated. Evidence: the boot ROM's `MEMSIZ` (RM248.K) scans **up from physical
0** for the first RAM (so RAM cannot be at 0), and the OS's "minimum physical
address" `MINMEM` (`[$2A4]`) / `realmemmmu`/`logrealmem` reflect the high base;
matching the real Lisa RAM-board layout (minimum RAM base `$80000` for every
config, maximum per installed size). The boot ROM then programs the MMU so logical RAM is contiguous *from this
high physical base*. With RAM modelled at 0 the OS placed the kernel stack on a
non-existent page → wild `RTS`/reset and screen-junk wild writes. The emulator
implements the high base in `lisa_mmu.c` (`ram_min`/`ram_max`); `lisa_mmu_init`
takes a `ram_high` flag set per machine — **`model=lisa` defaults to the high
base** (`$80000`), while `model=macxl` keeps RAM low (based at 0), since
MacWorks XL's framebuffer and boot path live in low memory. (`GSRAMMIN=1`
remains as a debug override that forces the high base on any model.)

### 3.2 I/O space map (physical `$00C000–$00FFFF`)

The full I/O space (including the expansion-slot decodes, which are out of scope
for the built-in machine) is:

| Physical range | Function |
| --- | --- |
| `$000000 – $001FFF` | Expansion slot 1, low decode |
| `$002000 – $003FFF` | Expansion slot 1, high decode |
| `$004000 – $005FFF` | Expansion slot 2, low decode |
| `$006000 – $007FFF` | Expansion slot 2, high decode |
| `$008000 – $009FFF` | Expansion slot 3, low decode |
| `$00A000 – $00BFFF` | Expansion slot 3, high decode |
| `$00C000 – $00CFFF` | Floppy-disk controller (6504A) shared RAM/registers |
| `$00D000 – $00DFFF` | I/O-board devices: SCC, VIA2 (parallel), VIA1 (keyboard/COPS) |
| `$00E000 – $00FFFF` | CPU-board devices: control strobes, video latch, status |

The CPU-board block (`$E000–$FFFF`) decodes into four sub-blocks:

| Range | Strobe | Purpose |
| --- | --- | --- |
| `$E000 – $E7FF` | SYSC/ | system/processor control register strobes (§6.1) |
| `$E800 – $EFFF` | VAL/  | Video Address Latch write (§8) |
| `$F000 – $F7FF` | RMEA/ | read Memory Error Address latch (§7.3) |
| `$F800 – $FFFF` | RBES/ | read Bus Error / Status Register (§7.4) |

### 3.3 The `$FCxxxx` / `$FExxxx` logical convention (important)

The Hardware Manual register tables list **physical** I/O addresses
(`$00D901`, `$00E800`, …). At run time the boot ROM's default MMU map assigns:

- **segment 126 → I/O space**, so logical `$00FCxxxx` reaches physical I/O
  `$00xxxx` (e.g. logical `$00FCD901` = the parallel VIA at physical `$00D901`);
- **segment 127 → special I/O**, so logical `$00FExxxx` reaches the boot ROM.

Driver and OS source therefore use the `$FCxxxx` / `$FExxxx` *logical* forms;
the HM register tables use the `$00xxxx` *physical* forms. **They are the same
registers.** This document uses physical I/O addresses in the device tables and
the logical form only where software references it. An emulator decodes devices
in physical I/O space; the MMU's space tag is what routes an access there.

### 3.4 Logical address space

The CPU emits 24-bit logical addresses (16 MB). The MMU divides this into 128
segments of up to 128 KB each (§4). The exception vector table and OS live in
low logical memory once the MMU maps RAM there.

---

## 4. The Memory Management Unit (Segment MMU)

The Lisa MMU is a **custom segment translator** implemented as a 1024 × 12-bit
descriptor RAM, sitting between the CPU's logical address bus and physical
memory. It is **not** a Motorola PMMU and uses **512-byte pages** (not 4 KB).

### 4.1 Logical address decomposition

A 24-bit logical address splits into three fields:

```
 23                17 16              9 8               0
+--------------------+-----------------+-----------------+
|  segment number    | page within seg | byte in page    |
|   (7 bits, 0-127)  |  (8 bits, 0-255)|  (9 bits)        |
+--------------------+-----------------+-----------------+
```

- **Segment number** (bits 23–17): selects one of **128** segments.
- **Page displacement** (bits 16–9): selects one of up to 256 pages in the
  segment.
- **Byte offset** (bits 8–0): the low **9 bits** pass through untranslated
  (page = **512 bytes**).

A segment is therefore up to 256 × 512 = **128 KB**; the minimum is one 512-byte
page.

### 4.2 Per-segment descriptors — SOR and SLR

Each segment (per context, §4.5) has two 12-bit registers:

- **SOR — Segment Origin Register.** A 12-bit **physical page number** giving the
  origin of the segment in physical memory (in units of 512-byte pages). E.g. a
  segment whose storage begins at physical byte `$002000` has SOR = `$010`.
- **SLR — Segment Limit Register.** Holds, in 12 bits:
  - **bits 11–8**: access-control / space-type code (§4.3);
  - **bits 7–0**: segment **length in pages**, in **two's-complement** form:
    `$00` = maximum (256 pages = 128 KB), `$FF` = minimum (1 page = 512 bytes).
    For **stack** segments the sense inverts (`$00` = 1 page, `$FF` = 128 KB) and
    the segment grows downward.

### 4.3 Access-control / space-type bits (SLR bits 11–8)

| 11 10 9 8 | Meaning |
| --- | --- |
| `0 1 0 0` | main memory, read-only, **stack** |
| `0 1 0 1` | main memory, read-only |
| `0 1 1 0` | main memory, read/write, **stack** |
| `0 1 1 1` | main memory, read/write |
| `1 0 0 1` | **I/O space** |
| `1 1 0 0` | **page invalid** (segment not present) |
| `1 1 1 1` | **special-I/O space** (boot ROM + MMU registers) |

All other codes are invalid/unpredictable. The decoded hardware flags are
**MEM** (memory), **IO**, **RO** (read-only — inhibits writes), and **STK**
(stack — inverts the limit sense).

### 4.4 Translation and limit check

```
physical_page = SOR + page_displacement      ; (bits 16-9), high nibble of sum forced to 0
physical_addr = (physical_page << 9) | (logical bits 8-0)
```

In parallel, the MMU performs a **limit check**: the page displacement is
compared against the SLR length. If the page lies outside the segment (an
overflow the hardware calls **ACCK**), the access is terminated — **CAS is
suppressed so no memory cycle occurs** — and a Bus Error is raised to the CPU.
For stack segments the carry sense is inverted (the segment is valid from the
top downward).

### 4.5 Contexts (SEG1 / SEG2)

The descriptor RAM holds **four** complete 128-segment tables (4 × 128 × 2
12-bit words = 1024 words). The active context is selected by two latch bits:

| SEG2 | SEG1 | Context |
| --- | --- | --- |
| 0 | 0 | 0 (OS) |
| 0 | 1 | 1 (user) |
| 1 | 0 | 2 (user) |
| 1 | 1 | 3 (user) |

- **Supervisor mode (FC2 asserted) forces context 0** regardless of the latch
  bits. A trap from a user process into the OS therefore auto-switches to the OS
  segment table. **Emulation note:** the effective context follows FC2 *live*,
  so an emulator must re-select the active translation (supervisor → context 0;
  user → the latch context) on **every** supervisor-bit change — not only on
  exception entry but also on `RTE` / `MOVE`/`ANDI`-to-SR back to user. If the
  active context is only switched on exception *entry*, user code resumed by an
  `RTE` keeps running through the OS (context-0) view, and a user process's
  private segment (mapped in its own context, e.g. `SYSTEM.SHELL`'s code) faults
  forever. OS/system code segments live in context 0; a user process reaches them
  through the inter-segment jump table (§2), which faults them into the process's
  own context on first use via the demand-load path (§4.8).
- The latch bits are set/reset by *strobing* (a read or write, value ignored)
  addresses in the CPU-board control block (§6.1):

| Strobe address | Effect |
| --- | --- |
| `$00E008` | reset SEG1 |
| `$00E00A` | set SEG1 |
| `$00E00C` | reset SEG2 |
| `$00E00E` | set SEG2 |

### 4.6 Special-I/O space and MMU-register addressing

The MMU descriptor registers and the boot ROM both live in **special-I/O
space** (SLR code `1111`). Special-I/O accesses are further decoded by logical
address bits 15/16 into `ROM/` (boot ROM) vs `MMUIO/` (descriptor RAM).

An MMU descriptor is written through special-I/O space with an address word of
the form:

```
 SSSSSSS 010 ............ B ...
 (bits 23-17 = segment #)        B = 1 -> write SOR ; B = 0 -> write SLR
```

where `S` is the segment number whose descriptor is being written and the
SOR-vs-SLR selection is driven by a low address line. The boot ROM addresses the
ROM directly in special-I/O space (bits 1–13 = ROM address, not MMU-translated).

### 4.7 START / setup mode (MMU bypass at reset)

At power-on the descriptor RAM is undefined, so the hardware comes up in **START
(setup) mode**:

- START is asserted automatically at power-on, and can be set/cleared by
  software via the **SETUP** strobe (`$00E010` reset / `$00E012` set, §6.1).
- While START is set, **logical address bit 14 is a switch**:
  - bit 14 = 0 → the access is satisfied from the **boot ROM in special-I/O
    space**, MMU bypassed (this is how the CPU fetches the reset vector and runs
    the ROM before the MMU is programmed);
  - bit 14 = 1 → the access goes **through the MMU** (so RAM-resident code can
    run while START is still set).
- The boot ROM programs the descriptor RAM (mapping physical RAM contiguous from
  0, and installing segment 126 → I/O, segment 127 → special-I/O) and then
  clears START to enable normal translation.

### 4.8 Violation behavior (emulation)

An out-of-limit access, an access to an invalid segment (`1100`), or a write to
a read-only segment terminates the bus cycle with CAS suppressed and raises a
68000 **Bus Error** (exception vector `$000008`). The OS's bus-error handler
reads the Status Register (§7.4) and the Memory Error Address latch (§7.3) to
classify the fault. An emulator should route these through the same bus-error
path used for genuine timeouts.

**Demand-loaded segments (the OS's main use of the Bus Error).** Code and data
segments are *demand-loaded* on first reference: the OS leaves a not-yet-resident
segment's descriptor **invalid** (`1100`), so a `JMP`/`JSR`/`RTS` into it — or a
data access to it — faults. The Lisa OS `BUS_ERR` handler
(`OS/SOURCE-EXCEPASM.TEXT`) recovers it, and the contract on the **MC68000
group-0 stack frame** is exact — an emulator's 68000 *must* push the genuine
68000 group-0 frame (not a 68010+/68030 long frame):

```
+$00  special status word  (R/W, I/N, function-code bits)
+$02  access address (long) = the faulting logical address  ("BADADDR")
+$06  instruction register (word)                            ("B1/B2")
+$08  status register (word)
+$0A  program counter (long)                                 ("PCX")
```

The handler:

1. reads the **instruction register** (`+$06`) — on a real 68000 this holds the
   opcode of the instruction *executing* when the prefetch faulted, i.e. the
   control-transfer op that branched into the absent segment (`JMP.L $4EF9`,
   `JSR.L $4EB9`, `JMP/JSR (An)`, `RTS $4E75`, `RTE $4E73`, `TST`). It uses this
   to recognise a **recoverable code-segment fault** vs. a fatal one
   (`e_hardsyscode`);
2. reads the **access address** (`+$02`) to find which segment/offset to load;
3. **backs the saved PC up** by a per-opcode amount (the prefetch advance: `−2`
   for `JMP.L`/`JMP(An)`/`JSR(An)`/`RTS`, `−6` for `JSR.L`, `−4` for `JSR d(An)`;
   `RTE` resumes at the access address). For `JMP`/`JSR` (no stack side effect)
   this re-enters at the fault target, so the emulator's saved PC may be the
   target + advance. **`RTS` is special:** the handler *also* does `USP −= 4`
   (undo the pop) and then `−2`, i.e. it **re-executes the RTS** so the pop
   re-runs after the segment is in. The saved PC therefore must point at the
   **`RTS` instruction itself**, not at its (absent-segment) target — otherwise
   the OS backs `USP` up by 4 without the pop re-running and the user stack is
   left one long too low (a stale word then surfaces as the next routine's first
   argument);
4. validates and maps the segment (`CHECK_CS`/`MAP_SEGMENT`, demand-reading the
   code from disk if not in memory) and enters the scheduler / `RTE`s to retry.

Emulator requirements this implies: record the address of the instruction being
decoded each cycle (for `+$0A`/`+$02` and the fetch-fault PC match); on a
code-fetch fault, push the **opcode of the branching instruction** (kept from the
last successful fetch — the faulting fetch must *not* overwrite it) as `+$06` and
advance the saved PC by the per-opcode prefetch amount — except for `RTS`, whose
saved PC is the **`RTS` instruction's own address** (latched alongside the kept
opcode) so the OS re-executes it; and keep the PC 24-bit (§2) so `+$02` (24-bit)
matches the fetch PC. Getting any of these wrong makes the OS mis-classify the
fault (`e_hardsyscode`/line-F) instead of demand-loading, or corrupt the user
stack on an `RTS`-into-absent-segment recovery.

**Data faults must be delivered, not only fetch faults.** An instruction-fetch
fault is self-announcing — the unmapped page reads back as `$FFFF`, decodes as a
line-F opcode, and is delivered the moment that opcode is executed. A **data**
read/write fault (a write to a read-only segment, or a demand-loaded data
segment) has no such inline trigger: the memory layer can only flag it, so the
emulator **must defer the group-0 bus error and deliver it at instruction
completion** — exactly where a 68030 delivers its deferred bus error, and a step
the 68000 decode loop is just as responsible for. Dropping it is not benign: the
pending-fault flag that `−(A7)`/`LINK`/`MOVEM`/`JSR` consult (to roll back a
push whose store faulted mid-instruction) stays set, so every later push
silently skips its stack-pointer update and the supervisor stack corrupts — which
surfaces much later as a wild inter-segment jump, not as the original write
fault. The OS relies on data faults for write-protect detection and demand-loaded
data segments (e.g. the installer's `Read_PMem` writes its status through a VAR
pointer that may target a not-yet-resident or read-only segment).

---

## 5. The Memory Management Unit — emulation seam

For an emulator built around a logical→physical translation step, the Lisa MMU
slots in where a Motorola PMMU would, with three differences to honor:

1. **Page size is 512 bytes**, not 4 KB. A per-page host-pointer cache must be
   indexed at 512-byte granularity (or translations cached per contiguous
   segment run, since one SOR maps a whole contiguous page run affinely).
2. **The translator must return a space tag** (main / I/O / special-I/O) from
   the SLR access bits, so the access is routed to RAM, the device decoder, or
   the ROM/MMU-register decoder respectively.
3. **Four context tables** plus the supervisor→context-0 override, instead of
   the PMMU's supervisor/user split.

Descriptor writes (§4.6), the context/START strobes (§6.1), and the special-I/O
decode all belong to the MMU model; the rest of the machine only wires the
strobe addresses to it.

---

## 6. Processor-Board Control Registers

### 6.1 Control strobe latches (`$00E000–$00E01E`)

Each function is a **strobe**: an access to the "reset" address clears the latch,
an access to the "set" address sets it (the data value is ignored). These are
the byte/word addresses in the SYSC/ block.

| Reset addr | Set addr | Latch | Function |
| --- | --- | --- | --- |
| `$00E000` | `$00E002` | DIAG1 | Memory diagnostic 1 (force soft error for test) |
| `$00E004` | `$00E006` | DIAG2 | Memory diagnostic 2 (force hard error for test) |
| `$00E008` | `$00E00A` | SEG1 | MMU context bit 1 (§4.5) |
| `$00E00C` | `$00E00E` | SEG2 | MMU context bit 2 (§4.5) |
| `$00E010` | `$00E012` | SETUP/START | MMU setup mode (§4.7) |
| `$00E014` | `$00E016` | SFMSK | enable soft memory-error detect |
| `$00E018` | `$00E01A` | VTMSK | enable Vertical-Retrace interrupt (§8) |
| `$00E01C` | `$00E01E` | HDMSK | enable hard memory-error detect |

### 6.2 Video Address Latch (`$00E800`, write)

Holds the upper 6 address bits (A15–A20) of the 32 KB video page, i.e. the
**physical** base of the framebuffer in RAM (§8). Write-only.

### 6.3 Memory Error Address latch (`$00F000`, read)

Latches the high-order physical address of the failing word on every memory
cycle and freezes on error. Only the 15 high-order lines A6–A20 are latched
(64-byte resolution). The low latch bit is the **access type**:

- `0` = CPU access (data bits D1–D15 carry A6–A20 of the failing address);
- `1` = video access (data bits D10–D15 = A15–A20 valid; lower bits invalid).

Reading this latch (RMEA/) also resets the bus-timeout (`BUST/`) bit.

### 6.4 Status Register (`$00F800`, read) — see §7.4.

---

## 7. Interrupt Architecture

### 7.1 IPL assignment

The Lisa wires its interrupt sources to **fixed 68000 IPL levels** (an LS148
priority encoder drives IPL0–IPL2):

| IPL | Source(s) |
| --- | --- |
| 7 (NMI) | power failure, hard memory error, soft memory error, keyboard reset |
| 6 | Serial (Z8530 SCC) |
| 5 | Expansion slot 1 |
| 4 | Expansion slot 2 |
| 3 | Expansion slot 3 |
| 2 | COPS — keyboard / mouse / real-time clock / on-off switch (via VIA1) |
| 1 | floppy controller, parallel-port hard disk, video vertical-retrace |

**Level 1 is shared by three devices.** The level-1 handler must poll the
floppy status byte (`$00C05F`, §13), the parallel VIA's IFR (§12), and the
Status Register's vertical-retrace bit (§7.4) to identify the source.

> **Emulator note (the floppy IPL-1 source).** `machine_lisa` aggregates level 1
> from three tracked sub-sources — the VIA2 IRQ, the video VBL, and **`l1_floppy`
> (FDC completion / disk insert / eject)** — recomputed on each change so none
> clobbers the others. The FDC's FDIR is therefore *both* the pollable VIA1 PB4
> level (for the ROM) and an IPL-1 interrupt (for the OS's blocking Sony driver);
> see docs/lisa_fdc.md. **This requires a faithful `STOP` instruction:** the OS
> scheduler idles in `Pause` = `STOP #$2000`, so the CPU must genuinely halt there
> until the floppy (or timer/COPS) interrupt arrives. The 68000 core implements
> `STOP` as a real halt (sets a `stopped` flag and drains the current sprint; the
> scheduler fast-forwards emulated time to the next event and resumes on any
> interrupt). A no-op `STOP` makes the Lisa scheduler spin its idle loop forever —
> burning billions of instructions and masking real boot blockers.

### 7.2 Exception / interrupt vector table (logical addresses)

| Exception | Vector |
| --- | --- |
| Reset: initial SSP / initial PC | `$000000` / `$000004` |
| Bus Error | `$000008` |
| Address Error | `$00000C` |
| Spurious interrupt | `$000060` |
| Level-1 autovector (floppy/parallel/video) | `$000064` |
| Level-2 autovector (keyboard/mouse/RTC) | `$000068` |
| Slot autovectors (levels 3–5) | `$00006C` / `$000070` / `$000074` |
| RS-232 / SCC (level 6) | `$000078` |
| Non-Maskable Interrupt (level 7) | `$00007C` |
| TRAP vectors | `$000080 – $0000BF` |
| User interrupt vectors | `$000100 – $0003FF` |

The SCC uses **autovectoring** (it does not supply a vector).

### 7.3 NMI (level 7) sources

Four sources: power failure, hard memory error, soft memory error, and keyboard
reset. The keyboard-reset NMI is generated by the COPS when a programmable
"NMI key" is struck (§11). The NMI button on the back of the machine also
triggers it (used by the MacWorks ROM debugger, §16).

### 7.4 Status Register (`$00F800`, read-only, 16-bit)

Read by the bus-error handler to classify a fault:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | Soft Error | a soft (correctable) memory error occurred |
| 1 | Hard Error | a hard (uncorrectable/parity) memory error occurred |
| 2 | Vertical Retrace | **active-LOW**: 0 *during* the ~90 µs vertical-retrace window, 1 during active scan; the VBL interrupt source |
| 3 | Bus Timeout | the AS/-attached timer expired (30–300 µs) → Bus Error |
| 4 | Video Bit | diagnostic: the current video output bit |
| 5 | Horizontal Sync | diagnostic: state of horizontal sync |
| 6 | Video Mode | reserved |
| 7–15 | — | unused |

> **Bit 2 (vertical retrace) — model + polarity.** The bit is **active-low** (it
> reads 0 *while* the video state machine is in vertical retrace, 1 during active
> scan). `lisa_mmu.c` models it as a pure function of
> the cycle counter: a `vertical` latch is set on the rising edge into each frame's
> ~90 µs retrace window (84896-cycle frame at 5.09375 MHz ≈ 60 Hz, 458-cycle
> window), cleared during active scan **and by the VTIRDIS strobe (`$00E018`)**.
> The VTIRDIS-clear is essential: the ROM's video self-test (VIDTST, RM248.S) waits
> for bit 2 = 0 (retrace), then strobes VTIRDIS and expects bit 2 = 1 — which works
> precisely because VTIRDIS clears the latch (NOT because of any per-read toggle).
> An earlier active-high + per-read-toggle hack passed VIDTST by luck but broke the
> OS's retrace-paced software clock (it ran ~150× fast); the cycle-accurate model
> fixes both. `lisa_mmu_set_clock()` supplies the cycle source.
>
> **Interaction with the latched VBL interrupt (§8):** the bit is *no longer
> purely* cycle-derived. While a latched VBL IRQ is pending (`vbl_active`), the bit
> is *forced* to read 0 (in-retrace) so the level-1 handler — which reads this bit
> to identify the source — confirms the VBL even when it is serviced past the
> cycle-accurate retrace window (the kernel having been masked). The first read of
> the Status Register clears `vbl_active` and drops the IRQ (the OS's VBL ack);
> outside that window the bit reverts to the cycle-derived value above. VIDTST is
> unaffected because it runs with VTMSK off (no `vbl_active` is ever set).

---

## 8. Video

- **Display:** **720 × 364** pixels, 1 bit per pixel (black/white), ~60 Hz, on
  the stock Lisa 2. The video state machine scans 379 total lines of 720 pixels,
  of which 364 are displayed. The **Macintosh XL** screen-modification kit
  instead uses a **608**-wide square-pixel raster (76-byte rows); the framebuffer
  the Finder paints is **608 × 431** (nominal Screen-Kit raster 608×432 — see the
  model note above). Same framebuffer page, different scan dimensions.
- **Framebuffer:** a **32 KB page in main RAM** (720 × 364 / 8 = 32,760 bytes,
  rounded to a 32 KB page). Bit ordering is MSB-first (leftmost pixel = bit 7 of
  the byte). The last displayed byte's least-significant bit must be 1 so retrace
  shows black.
- **Screen base:** set by the **Video Address Latch** at `$00E800` (§6.2), which
  supplies address bits A15–A20 — i.e. the framebuffer is relocatable to any
  32 KB-aligned region of physical RAM. This is an **un-translated physical**
  address (the video fetch does not go through the MMU). An emulator reads this
  latch to locate the framebuffer each frame.
- **Vertical-retrace (VBL) interrupt:** generated by the video state machine
  ("VTIR"), at **IPL 1**. Enabled/disabled via the strobes `$00E018` (VTIRDIS,
  off) / `$00E01A` (VTIRENB, on) (§6.1). The retrace status is **bit 2 of the
  Status Register** (`$00F800`) — **active-low**, asserted (= 0) for the ~90 µs
  retrace window each frame; see §7.4 for the cycle-accurate model and the
  VTIRDIS-clears-the-latch behaviour the ROM's VIDTST depends on.
  - **The VBL interrupt is EDGE-triggered and LATCHED — model it as a held level,
    NOT a fixed-width pulse.** VTIR fires once at the rising edge into each retrace
    and stays asserted at IPL 1 until the CPU services it (a latched autovector
    on real hardware). This matters
    because the kernel is routinely interrupt-masked through the *entire* ~90 µs
    retrace window: a pulse that de-asserted at the window's end would be **lost**
    whenever the kernel was masked across it, and retrace-paced OS work would
    stall. (Concrete symptom: after mounting the boot volume the OS resumes a
    process that expects a *pending* VBL to drive the install-shell segment load;
    with a dropped VBL it ran ahead into a non-resident segment and the boot
    derailed.) The emulator (`lisa.c`) therefore asserts the level-1 VBL source at
    the retrace edge and holds it until the OS reads the Status Register — the
    level-1 handler's first action, which identifies *and* acknowledges the VBL
    (`lisa_vbl_ack` / `lisa_mmu_set_vbl_ack`); the old 458-cycle auto-drop is gone.
- **Contrast:** software-controlled. A value written to VIA2's A-port is latched
  in a 74C174 and summed to a DC level driving the CRT (output ~+2 V full black
  to ~+7 V full white). This is cosmetic; an emulator may model it as a stored
  level. Because the contrast DAC shares VIA2 with the hard disk, drivers
  bracket disk I/O so the two uses don't collide.

### 8.1 Macintosh XL video

The Macintosh XL screen-mod kit changes the dot clock to make pixels square; the
displayed bitmap stays 720×364. For frame-accurate emulation only the displayed
geometry (720×364) and a pixel-aspect flag are needed. (The exact modified dot
clock is not part of the programming model and does not affect boot or rendering
correctness.)

---

## 9. I/O-Board Devices — Address Summary

All addresses are physical I/O space (reached at logical `$00FCxxxx`; §3.3).

| Device | Physical base | Stride | IPL | Section |
| --- | --- | --- | --- | --- |
| Floppy controller (6504A) | `$00C001` | shared RAM | 1 | §13 |
| Z8530 SCC | `$00D241` | see §15 | 6 | §15 |
| VIA2 (parallel port / hard disk) | `$00D901` | 8 | 1 | §10, §14 |
| VIA1 (keyboard / COPS) | `$00DD81` | 2 | 2 | §10, §11 |

---

## 10. The Two 6522 VIAs

Both are standard MOS 6522 VIAs. Their internal register semantics (timers T1/T2,
ACR/PCR handshake modes, IFR/IER, shift register, CA/CB lines) are described in
[via.md](via.md); only the Lisa wiring and addressing are given here. The Lisa
I/O-board 6522s are clocked at the 68000 cycle ÷ 4 (~785 ns on Lisa 2).

> **⚠️ The Lisa OS source SWAPS the VIA1/VIA2 names** relative to the Hardware
> Manual and this emulator. In `LIBHW/LIBHW-DRIVERS.TEXT`, OS `VIA1` is the
> *Hard-Disk* VIA (`IOSpace+$D901` — our **via2**, the parallel/ProFile port, IPL 1)
> and OS `VIA2` is the *Keyboard* VIA (`IOSpace+$DD81` — our **via1**, the COPS, IPL
> 2); the OS uses stride-2 register offsets on both (`PORTA2 .EQU 2` = reg 1 IRA,
> `IFR2 .EQU 26` = reg 13). So when the OS reads the COPS from "`VIA2+PORTA2`" it is
> physically reading **our via1** ($DD81) — our wiring (COPS on via1, ProFile on
> via2) is correct; only the labels differ. Keep this in mind when cross-referencing
> the OS source against the emulator or this doc (which use the Hardware-Manual
> naming: **VIA1 = COPS, VIA2 = parallel**).

### 10.1 VIA1 — Keyboard / COPS (base `$00DD81`, register stride 2)

| Phys addr | Reg # | 6522 register |
| --- | --- | --- |
| `$00DD81` | 0 | ORB/IRB |
| `$00DD83` | 1 | ORA/IRA |
| `$00DD85` | 2 | DDRB |
| `$00DD87` | 3 | DDRA |
| `$00DD89` | 4 | T1C-L |
| `$00DD8B` | 5 | T1C-H |
| `$00DD8D` | 6 | T1L-L |
| `$00DD8F` | 7 | T1L-H |
| `$00DD91` | 8 | T2C-L |
| `$00DD93` | 9 | T2C-H |
| `$00DD95` | 10 | SR (shift register) |
| `$00DD97` | 11 | ACR |
| `$00DD99` | 12 | PCR |
| `$00DD9B` | 13 | IFR |
| `$00DD9D` | 14 | IER |
| `$00DD9F` | 15 | ORA/IRA (no handshake) |

**Port wiring.** Port A (`$00DD83`) is the byte-wide command/response channel to
the COPS microcontroller (§11): commands are written to it and responses are read
from it. VIA1 also carries:

- three sound volume-control lines and one speaker tone line (Lisa sound is a
  simple VIA-driven square-wave speaker + volume DAC, not a Mac-style PWM buffer);
- the floppy-disk interrupt input;
- the parallel hard disk's `DIAGPAR`, controller-reset `CRES/` (PB7), and the
  `CHK` line (§14) — i.e. a few parallel-port control signals live on the
  keyboard VIA rather than VIA2.

VIA1 aggregates to **IPL 2**.

### 10.2 VIA2 — Parallel Port / Hard Disk (base `$00D901`, register stride 8)

| Phys addr | Reg # | 6522 register |
| --- | --- | --- |
| `$00D901` | 0 | ORB/IRB |
| `$00D909` | 1 | ORA/IRA |
| `$00D911` | 2 | DDRB |
| `$00D919` | 3 | DDRA |
| `$00D921` | 4 | T1C-L |
| `$00D929` | 5 | T1C-H |
| `$00D931` | 6 | T1L-L |
| `$00D939` | 7 | T1L-H |
| `$00D941` | 8 | T2C-L |
| `$00D949` | 9 | T2C-H |
| `$00D951` | 10 | SR |
| `$00D959` | 11 | ACR |
| `$00D961` | 12 | PCR |
| `$00D969` | 13 | IFR |
| `$00D971` | 14 | IER |
| `$00D979` | 15 | ORA/IRA (no handshake) |

Port A (`$00D909`) is the 8-bit bidirectional data path to the parallel hard
disk (and the contrast DAC). The handshake uses CA1/CA2/CB2 and several PB lines
(§14). When CA1 is in pulse-handshake mode and the parallel-port enable is low,
CA1 pulses on each A-port read/write; CA1 feeds CA2 to allow A-port latch mode.
VIA2 aggregates to **IPL 1** (shared with floppy and video).

> **⚠️ VIA2 chip-select ignores address bit 8 — the whole `$00D800-$00D9FF`
> window decodes to VIA2** (register = `(addr>>3) & 15`, so the `$D8xx` and `$D9xx`
> halves are aliases). The boot ROM (`VIA2BASE = $00FCD901`) and the OS clock use
> the `$D9xx` alias, but **the LisaOS parallel hard-disk driver
> (`SYSTEM.CD_PROFILE` / `PROF_INIT`) addresses VIA2 off base `$00D801`** (e.g. IER
> at `$00D871`). The emulator must therefore decode the full `$D800-$D9FF` range;
> a narrow `$D901`-only mapping silently drops the driver's register writes and the
> ProFile is never detected by the Office-System installer. (MacWorks XL uses only
> the `$D9xx` alias.)

---

## 11. COPS Microcontroller (Keyboard / Mouse / RTC / Soft-Power)

A National Semiconductor **COP421-class** slave microcontroller services four
peripherals through the A-port of VIA1: the keyboard, the mouse, the real-time
clock, and software power control. It runs from the standby/battery supply and is
**always powered** (it keeps the clock running and can power the machine on/off);
it stops only if the battery is exhausted.

The host writes a **command byte** to VIA1 ORA (`$00DD83`) and reads **response
bytes** from the same register. Keyboard, mouse, clock, and reset events arrive
as response bytes; the COPS raises VIA1's interrupt (IPL 2).

**Send handshake — CRDY (VIA1 PB6).** Port A is shared (bidirectional), so the
host must hand a command byte to the COPS in sync with the COPS's bus access.
`CRDY` (VIA1 **PB6**, COPS-driven) is a **free-running ready/busy line**: the
COP421 toggles it continuously as it cycles through its internal scan loop, not
a static level. To send, the host (a) presents the byte on ORA, (b) spins until
it sees a CRDY **edge** to a ready phase, (c) drives port A (`DDRA = $FF`) to
clock the byte into the COPS, then (d) waits for the next CRDY edge and releases
`DDRA`. The rev-H boot ROM's `COPSCMD` and MacWorks XL's own driver both
synchronise to CRDY *edges* this way — so an emulator must **toggle CRDY
continuously** (e.g. a steady period well under the senders' ~10 ms per-edge
timeout); holding it at a fixed level lets one sender through but hangs the
other while it waits for an edge that never comes.

### 11.1 Command byte format (written to VIA1 port A)

| Bits 7…0 | Function |
| --- | --- |
| `0000 0000` | turn I/O port on |
| `0000 0001` | turn I/O port off |
| `0000 0010` | read clock data |
| `0001 nnnn` | write nibble `nnnn` to the clock |
| `0010 spmm` | set clock modes: `s` = enable clock-set mode; **`p` = power on(1)/off(0)**; `mm` = 00 clock/timer disable, 01 timer disable, 10 timer-underflow interrupt, 11 timer-underflow power-on |
| `0101 nnnn` | set NMI-key high nibble |
| `0110 nnnn` | set NMI-key low nibble |
| `1xxx xxxx` | no operation |

### 11.2 Reset / status response codes

A reset/status response is a `$80` lead-in byte followed by a code byte:

| Code | Meaning |
| --- | --- |
| `$FF` | keyboard-COPS self-test failure |
| `$FE` | I/O-board-COPS self-test failure |
| `$FD` | keyboard unplugged |
| `$FC` | clock timer interrupt |
| `$FB` | **soft power-off switch pressed** |
| `$F0–$FA` | reserved |
| `$Ey` | clock data follows (5 bytes; `y` = year nibble) |

### 11.3 Keyboard

Key events are reported as bytes of the form `d rrr nnnn`, where `d` = direction
(1 = key down, 0 = key up) and `rrr nnnn` selects the key from the scan matrix.
The keyboard supports N-key rollover. Any key can be programmed (via the
`0101`/`0110` commands) to raise the keyboard-reset NMI.

Keyboard **layout/legend** IDs (selected by the keyboard hardware, reported to
software): `$0F` = old US layout, `$3E` = Dvorak, `$3F` = final US layout (plus
several international layouts). An emulator maps host key events to Lisa key
codes through the layout table for the configured layout.

### 11.4 Mouse

The mouse is enabled with a command of the form `#111 ennn`, where `e` enables
mouse interrupts and `nnn` selects the report interval (`nnn × 4 ms`). Mouse
movement is reported as **three response bytes**:

```
byte 0 = $00     ; "mouse data follows" marker
byte 1 = dx      ; signed change in X (-127..+127)
byte 2 = dy      ; signed change in Y (-127..+127)
```

Deltas **accumulate** in the COPS until the host reads them, then reset. The
mouse **button** is delivered as a key code `d000 0110` (`d` = 1 pressed, 0
released). Plugged/unplugged status arrives as `1000 0111` / `0000 0111`.

Unlike the Macintosh — whose CPU reads the mouse's raw **quadrature** off the
VIA/SCC and counts it in software — the Lisa mouse plugs into the COPS, which
polls the pulse edges through a multiplexer and counts them itself (HM §6.6.3),
handing the CPU these **cooked signed deltas**. The CPU never sees quadrature, so
host input injection feeds `dx`/`dy` directly to the COPS report path (see
[cops.md](cops.md)), not quadrature pulses.

**Absolute positioning (`mouse.move x y "global"`).** Because the mouse is relative
and the OS scales the deltas, the emulator can't place the cursor with one report.
Instead the COPS runs a **closed-loop "warp"** (`cops_set_warp`): on each mouse
report it reads the OS's live on-screen cursor position and emits a corrective
delta toward the target pixel, converging in a handful of reports — the standard
closed-loop technique for positioning a relative mouse. Two LOS facts make it exact:

* The live cursor is in OS globals **`$CC00F0` = X, `$CC00F2` = Y** (word, signed),
  written by the cursor tracker in **supervisor** mode (context 0) and re-asserted
  every VBL — so a supervisor read always returns the current cursor. (These are
  *not* the `$486`/`$488` globals other references mention; those are a separate copy
  that stays stale in this model.)
* The OS scales each COPS delta to screen pixels by a fixed per-axis factor —
  **X × 3/2** (the 720×364 non-square pixel aspect) and **Y × 1** — linearly, with no
  acceleration threshold. So to close an error `(errX, errY)` the warp injects
  `dx = errX × 2/3`, `dy = errY`; the loop re-reads each report, so integer-division
  residue self-corrects.

A `mouse.click` is then the mouse-button keycode, which the OS hit-tests against the
cursor it has tracked onto the target. (The OS does not blit the cursor sprite on the
idle Install menu, so a menu screenshot is cursor-less even though the position is
tracked; the cursor becomes visible once a dialog opens.)

### 11.5 Real-time clock

Resolution is 1/10 second with a 16-year span. The clock is set and read via the
`0001 nnnn` (write nibble) and `0000 0010` (read) commands; read-back is the
`$80 $Ey …` 5-byte form. The clock timer can interrupt and/or power the machine
on after a programmed interval. An emulator drives the clock from the host wall
clock, with a deterministic override for reproducible boots.

### 11.6 Soft power

Pressing the front power switch while running delivers reset code `$FB`; software
performs cleanup and then powers the machine off by issuing a `0010 spmm` command
with `p = 0`. Power is otherwise only removed by unplugging.

---

## 12. Parallel VIA Interrupt Demux (level 1)

Because the parallel hard disk (VIA2), the floppy controller, and the video VBL
all interrupt at IPL 1, the level-1 handler reads, in turn: VIA2's IFR (for the
`BSY`/parallel transition), the floppy status byte at `$00C05F` (§13), and the
Status Register VBL bit (§7.4). An emulator must drive all three independently
and let the handler poll.

---

## 13. Floppy-Disk Controller (6504A)

The Lisa floppy controller is an **intelligent coprocessor** (a 6504A
microcomputer with 4 KB of private program ROM and a **1 KB buffer RAM shared
with the 68000**), *not* an Apple IWM. The 68000 issues high-level commands by
writing a command block into the shared RAM; the coprocessor performs the
sector-level transfer and returns logical 512-byte sectors. This means an
emulator models the controller at the **command-block / logical-sector** level
and never deals with raw GCR cells.

- **Drive:** one **Sony 400 KB** 3.5″ double-sided mechanism on the Lisa 2 /
  Macintosh XL. (The Lisa 1's twin Twiggy 5.25″ drives are a different machine
  and out of scope.)
- **Shared RAM / registers:** physical `$00C001 – $00C7FF` (logical `$00FCCxxx`).
- **Interrupt:** **IPL 1**; fires on disk insertion, eject-button press, and
  command (RWTS) completion. The interrupt-enable mask must be set before the
  68000 may access the shared floppy RAM (otherwise a bus error results).

### 13.1 Controller commands (written to `$00C001`)

| Value | Command |
| --- | --- |
| `$81` | execute RWTS (parameters in the command block at `$00FCC003+`) |
| `$83` | seek to side/track |
| `$84` | JSR to a routine at `$00C003` |
| `$85` | clear interrupt status |
| `$86` | set interrupt mask |
| `$87` | clear interrupt mask |
| `$88` | wait in ROM for cold start |
| `$89` | loop in ROM |

### 13.2 RWTS command block (logical `$00FCC003+`)

| Address | Field | Values |
| --- | --- | --- |
| `$FCC003` | command | `00` read, `01` write, `02` unclamp, `03` format, `04` verify, `05` format-track, `06` verify-track, `07` read-no-checksum, `08` write-no-checksum |
| `$FCC005` | drive | `00` = drive 2 (lower), `80` = drive 1 (upper) |
| `$FCC007` | side | `0x` = side 1, `1x` = side 2 |
| `$FCC009` | sector | Sony: 0–11 (variable by track zone); Twiggy: 0–22 |
| `$FCC00B` | track | Sony: 0–79; Twiggy: 0–44 |
| `$FCC00D` | speed | motor-speed byte. Also used by the `$84` (JSR) call as a host↔coprocessor **busy flag**: the host sets it `$FF`, issues `$84`, and polls it back to `0` for completion. |
| `$FCC00F` | format-confirm | |
| `$FCC011` | error status | result code (e.g. `$14` write-protect, `$17` unreadable, `$18` unwritable) |
| `$FCC013` | disk ID | |
| `$FCC015` | disk type / geometry | The controller reports the inserted media here, and the boot loader reads it to choose the block→(track,sector) conversion: `00` = Twiggy/FileWare (1702 blocks); non-zero with **bit 0 set** = Sony **400 KB** single-sided (800 blocks); **bit 0 clear** = Sony **800 KB** double-sided (1600 blocks). If this byte is left `0`, the loader assumes the 1702-block Twiggy geometry and computes out-of-range (track, sector) for any block past track 0. |

Sectors are 512 bytes plus tag bytes; the I/O buffer the controller uses is 524
bytes. The Sony 400 KB format uses Apple's 5-zone variable-speed GCR layout:
12, 11, 10, 9, 8 sectors per track across the five 16-track zones (50 sectors per
zone-pair × 16 × 512 = 400 KB). An emulator maps `(track, side, sector)` to a
linear block offset using that zoning.

### 13.3 Status byte (`$00C05F`)

| Bit | Meaning |
| --- | --- |
| 0 | drive 1 disk-inserted event |
| 1 | drive 1 eject button |
| 2 | drive 1 RWTS complete |
| 3 | OR of bits 0–2 |
| 4–6 | same as 0–2 for drive 2 |
| 7 | OR of bits 4–6 |

These are **latched interrupt-event** bits — set when the event occurs (raising
FDIR / IPL 1), *not* a live snapshot of drive state. **`CLRSTAT` ($85) clears
them all.** Software drains pending events by issuing `CLRSTAT` until the byte
reads `0`; consequently bit 0 is a one-shot *insertion* event, **not** a level
held while media sits in the drive (a perpetually-set "present" bit would make
that drain loop spin forever). "Unclamp" ($02 RWTS) ejects the disk — the Lisa
drive is software-eject, so a host that wants to swap disks unclamps the current
one and the drive then accepts new media.

> **Drive-1 bit layout — verified (session 9).** Lisa 2's single Sony is **drive 1**
> (bits 0–3 here; `DRIVE` byte `$80`, `LOWER` in `SOURCE-SONYASM`). A drive-1 RWTS
> completion therefore sets **bit 2 + bit 3** (`$0C`), which is what `lisa_fdc.c`
> writes — and both the ROM/loader and the OS Sony driver accept it. (A plausible
> alternative encoding — drive-1 = bit 6 + bit 7 = `$C0` — was tested and **resets
> the early ROM/loader boot**, confirming the bits-0–3 layout above is correct for
> the rev-H ROM. Don't re-try `$C0`.)

### 13.4 Controller ROM id and parameter memory

Two further regions live in the controller's shared RAM (battery/standby-backed
on real hardware), unrelated to the RWTS command block:

- **`$FCC031` — disk-controller ROM id** (the OS's `adr_ioboard`). The boot ROM
  and OS read this byte to detect the machine type and choose the Twiggy vs Sony
  floppy driver (§16.2). An emulator that models a Sony machine must set it
  accordingly; a zeroed value reads as a Lisa 1.
- **`$FCC181` — parameter memory** (PM): 64 bytes = 32 words holding boot volume,
  screen contrast, beep volume, mouse settings, `pm_ExtendMem`/`pm_MEM_LOSS`, a
  device-configuration table, and a checksum. The boot ROM's `CHKPM` validates it
  with a rotate-sum (add word, `ROL #1`) over the 32 words and treats the block as
  **valid** when the result is `0`. Note this means an all-zero region (e.g. a
  freshly `calloc`-ed emulator buffer) checksums as a *valid* all-zero PM with
  boot volume `0` (= Twiggy drive 1); real cold-boot SRAM is undefined and
  normally fails the checksum, so the firmware initializes PM to defaults.

---

## 14. Parallel Hard Disk — ProFile / Widget (NOT SCSI)

The Lisa hard disk is driven over an 8-bit bidirectional **parallel port** built
from VIA2 (`$00D901`), with a request/strobe handshake. The external **ProFile**
(5 MB / 10 MB) and the internal **Widget** (on the Lisa 2/10 and Macintosh XL)
both use this interface. There is no SCSI hardware on the Lisa.

### 14.1 6522 pin mapping

| 6522 line | Signal | Direction | Notes |
| --- | --- | --- | --- |
| VIA2 PA0–PA7 | data DD0–DD7 | bidirectional | the data byte |
| VIA2 CA2 | `PSTRB/` (data strobe) | out | strobes each byte |
| VIA2 CA1 / PB1 | `BSY` | in | wired to both; CA1 is the interrupt edge |
| VIA2 PB0 | `OCD` (open-cable detect) | in | 1 = disconnected |
| VIA2 PB3 | `DRW` (read/write direction) | out | 1 = read, 0 = write |
| VIA2 PB4 | `CMD/` | out | 0 = command phase asserted |
| VIA2 CB2 | `PARITY/` | in | latched; cleared by any access to the B register |
| VIA1 PB5 | `DIAGPAR` | out | diagnostic parity (on the keyboard VIA) |
| VIA1 PB7 | `CRES/` | out | controller reset (on the keyboard VIA) |
| — | `CHK` | in | check; encoded on the keyboard VIA, raises its own key code |

### 14.2 Handshake and command protocol

The byte-level handshake (derived from Apple's ProFile/Widget driver):

1. The host asserts `CMD/` true (clear VIA2 PB4) and waits for `BSY`.
2. Host and controller exchange a standard handshake byte **`$55`** (the
   controller's "ready" reply).
3. The host sends a **6-byte command buffer** out VIA2 port A, one byte per
   handshake, in the direction set by `DRW`:

   | Byte | Field |
   | --- | --- |
   | 0 | command: `$00` read, `$01` write, `$02` write-verify (plus controller "system"/spare-table commands on the Widget) |
   | 1–3 | block number (24-bit, big-endian) |
   | 4 | retry count |
   | 5 | sparing threshold |

4. For a read, the controller returns a status phase followed by the data; for a
   write, the host sends the data after the command. Each transferred byte is
   gated by the `PSTRB/`/`BSY` handshake.

### 14.3 Block format

Each disk block is **532–536 bytes**: 512 bytes of data plus a tag/"pagelabel"
header (the OS uses a 24-byte distributed-directory record). The driver default
retry count is 10 and the sparing threshold is 3; the Widget detects its size at
init, and a ProFile is assumed to be ~9,720 blocks. An emulator backs the disk
with a flat image and services the command buffer with block-level reads/writes,
synthesizing the status phase and honoring the handshake timing (consecutive
port accesses must be spaced to support both 6522 clock rates — roughly ≥ 14
68000 cycles apart).

---

## 15. Serial — Z8530 SCC

A dual-channel Zilog **Z8530 SCC**. Its internal register file and baud-rate
generator are described in [scc.md](scc.md); the Lisa specifics are:

- **Addresses** (physical I/O space; the address pins follow the standard
  A1 = A/B-select, A2 = data/control convention):

  | Phys addr | Channel | Access |
  | --- | --- | --- |
  | `$00D241` | B | control |
  | `$00D243` | A | control |
  | `$00D245` | B | data |
  | `$00D247` | A | data |

- **Interrupt:** **IPL 6**, autovectored (vector `$000078`).
- **Clocks:** channel A's PCLK input is **4.000 MHz**, channel B's is **3.6864
  MHz**. Baud time constants are `TC(A) = 4,000,000 / (2 × baud) − 2` and
  `TC(B) = 3,686,400 / (2 × baud) − 2`. Port B is used for AppleBus (LocalTalk).

Because the address pins follow the same A1/A2 convention as the Macintosh, a
generic SCC model that decodes `channel = ~(A1)` and `data = A2` works unchanged
when mapped at base `$00D240`.

---

## 16. Boot ROM

The boot ROM is **16 KB**, supplied as **two 8 KB byte-slice chips** that must be
**interleaved** to form the 16-bit ROM image:

- **even bytes** ← `341-0175` (high byte, D8–D15);
- **odd bytes** ← `341-0176` (low byte, D0–D7).

For the Lisa 2 "H" ROM this yields the version word `$0248` (rev 2.48) at ROM
offset `$3FFC`, and the reset image at offset 0 reads initial SSP `$00000480`,
initial PC `$00FE00F6` (a special-I/O ROM address). The Macintosh XL "3A" ROM
(`341-0346-A` + `341-0347-A`) interleaves identically.

The ROM is reached only in **special-I/O space** (logical `$00FExxxx`); it is not
MMU-translated. At reset the CPU runs from it with START set and logical bit 14 =
0 (§4.7).

### 16.1 Power-on self-test sequence

The ROM runs a fixed self-test sequence before booting; an emulator bringing up
the machine can use these as milestones:

1. ROM checksum
2. MMU register test (writes/reads the descriptor RAM)
3. Memory sizing
4. Preliminary memory test
5. VIA test
6. Screen-memory test
7. I/O-board tests (COPS, floppy, etc.)

On success the ROM selects a boot device and loads the OS (or MacWorks).

### 16.2 Machine-type detection (`$FCC031`, SYSTYPE, iomodel)

Both the boot ROM (`SETTYPE`) and Lisa OS (`SOURCE-STARTUP`) identify the machine
by reading the **disk-controller ROM id byte at `$FCC031`** (the OS calls it
`adr_ioboard`). This single byte determines whether the system installs the
**Twiggy** or **Sony** floppy driver, so an emulator of a Sony-based Lisa 2 / Mac
XL **must** present a non-zero value here — a zeroed byte is read as a Lisa 1 and
the OS drives the Sony as a Twiggy (a fatal mismatch that strands OS startup).

The boot ROM records its decision in low RAM `SYSTYPE` (`$2AF`): `0` = Lisa 1;
`1` = Lisa 2, external disk, **slow** timers; `2` = Lisa 2, external disk,
**fast** timers; `3` = Lisa 2, internal disk ("Pepsi"), fast timers. (`$FCC031`
bit 7 set = Lisa 2; bit 5 `SLOTMR` = slow timers; bit 6 `FASTMR` = fast.)

Lisa OS reads `$FCC031` as a **signed** byte to pick its I/O-board model
(`iob_*`), which the configurable-driver layer maps to the floppy driver:

| `$FCC031` | OS `iomodel` | machine | floppy driver |
| --- | --- | --- | --- |
| `$00`–`$7F` | `iob_lisa` | Lisa 1 (twin Twiggy) | Twiggy |
| `$A0`–`$BF` | `iob_sony` | **Lisa 2/5** (old "Lisa Lite" board + Sony + external ProFile) | Sony |
| `$C0`–`$DF` | `iob_lite` | Pepsi board, Sony, no built-in HD | Sony |
| `$80`–`$9F` / `$E0`–`$FF`, with `$FCC015` ≠ 0 | `iob_pepsi` | **Macintosh XL** (Pepsi board + Sony + Widget) | Sony |

(The OS `Mach_Info` syscall exposes this as `io_board`: `IOlisa`/`IOpepsi`/
`IOlisaLite`/`IOpepsiLite`. Our `machine_lisa` presents `$A0` = `iob_sony`;
`machine_macxl` is left at `0` for MacWorks-XL compatibility.)

---

## 17. Macintosh XL and MacWorks XL

The Macintosh XL boots **MacWorks XL** from disk to become Macintosh-compatible.
Hardware-relevant facts for an emulator:

- The Lisa boot ROM contains **no operating system** — only diagnostics and
  boot-device selection. MacWorks supplies the Macintosh ROM itself: it loads a
  complete **128 KB-Mac-ROM image as a resource file from disk** and, using the
  segment MMU, maps it where a Macintosh expects it (the `$00400000` range). **No
  separate Macintosh ROM image is required** to emulate the Macintosh XL — only
  the Lisa 3A boot ROM and the MacWorks disk.
- Boot path: the Lisa ROM loads sector 0 to `$00020000` and runs it; a stage-1
  loader then reads the stage-2 loader and the ROM image.
- Lisa detection by software: `MACW` appears at logical `$00400040` (Mac code
  tests `CMPI.L #'MACW', $00400040`).
- **Address collision to honor:** a Macintosh Plus's VIA is at `$00EFFFFE`,
  which on the Lisa is the screen-base register. MacWorks rewrites the
  hardware-dependent kernel and low-memory globals to Lisa addresses; an emulator
  must reproduce the Lisa hardware faithfully at these addresses so MacWorks's
  own remapping behaves.
- The displayed screen MacWorks presents to Macintosh software is the Lisa's
  720×364 (square-pixel on the XL).

---

## 18. Summary of Lisa-Specific Models an Emulator Must Provide

| Subsystem | New model vs. reuse |
| --- | --- |
| Segment MMU (§4–5) | **new** — 512-byte pages, 4 contexts, 3 space tags, START mode |
| COPS (§11) | **new** — command/response byte protocol on VIA1 port A |
| Parallel hard disk (§14) | **new** — VIA2 handshake + 6-byte command + 532/536-byte blocks |
| Floppy 6504A (§13) | **new** front-end, but block-level (no GCR cells); reuses image block I/O |
| Video (§8) | direct 1-bpp framebuffer in RAM, base from the `$E800` latch |
| Processor-board control / status (§6, §7) | strobe latches + status/error registers |
| 6522 VIA ×2 (§10) | reuse the generic 6522 model ([via.md](via.md)); Lisa wiring only |
| Z8530 SCC (§15) | reuse the generic SCC model ([scc.md](scc.md)); Lisa addresses/clocks only |
| 68000 CPU (§2) | reuse; feed fixed-IPL interrupts and bus errors |

The genuinely new silicon is the **segment MMU**, the **COPS**, and the
**parallel hard-disk** interface; everything else is either a thin Lisa-specific
front-end over an existing block/image path or a reuse of the generic 6522/8530
and 68000 models with Lisa wiring.
