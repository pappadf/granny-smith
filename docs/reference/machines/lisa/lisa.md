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
> generator) are documented in [via.md](../../hardware/via.md) and [scc.md](../../hardware/scc.md). This
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
| Memory management | [Custom Apple segment MMU](mmu.md) | 1K×12-bit descriptor RAM, external to the CPU |
| Keyboard / mouse / clock / power | [COPS (COP421-class) microcontroller](cops.md) | on the A-port of VIA1; always-on (standby supply) |
| Parallel I/O ×2 | two MOS 6522 VIAs | VIA1 = COPS, VIA2 = parallel hard-disk port |
| Serial | Zilog Z8530 SCC | two channels, autovectored |
| Floppy | [6504A-based intelligent controller](fdc.md) | one Sony 400 KB 3.5″ drive (Lisa 2 / XL) |
| Hard disk | [parallel-port ProFile (external) / Widget (internal 2/10)](profile.md) | NOT SCSI |
| Video | [discrete state machine + DAC](video.md) | 720×364 1 bpp, framebuffer in main RAM |

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
| `$000000 – $07FFFF` | **unpopulated** below 2 MB (the memory boards sit *high*); RAM with two 1 MB boards |
| `$000000 – $1FFFFF` | RAM. The boards fill the space down from `$100000`: RAM starts at `$100000` less the slot-2 board — 512 KB (one 512 KB board) ⇒ `[$80000,$100000)`, 1 MB (two 512 KB) ⇒ `[$80000,$180000)`, 1.5 MB (a 1 MB board over a 512 KB one) ⇒ `[$80000,$200000)`, 2 MB (two 1 MB boards) ⇒ `[0,$200000)`. |
| (within RAM) | the 32 KB video page, located by the Video Address Latch (§8) — near the top of RAM (e.g. `$1F8000` on a 2 MB machine) |

Physical addresses are 21 bits (2 MB). **Below 2 MB, RAM is based HIGH at
`$080000`, NOT at physical 0** (a long-standing error to watch for): physical
`[0, $80000)` is unpopulated. Evidence: the boot ROM's `MEMSIZ` (RM248.K) scans **up from physical
0** for the first RAM (so RAM cannot be at 0), and the OS's "minimum physical
address" `MINMEM` (`[$2A4]`) / `realmemmmu`/`logrealmem` reflect the high base;
matching the real Lisa RAM-board layout (RAM ends at `$100000` plus the slot-1
board and starts at `$100000` less the slot-2 board, so the base is `$80000`
with a 512 KB board in slot 2 and 0 with two 1 MB boards; the emulator reads
the boards from the configured size). The boot ROM then programs the MMU so logical RAM is contiguous *from this
high physical base*. With RAM modelled at 0 the OS placed the kernel stack on a
non-existent page → wild `RTS`/reset and screen-junk wild writes. The emulator
implements the high base in `lisa_mmu.c` (`ram_min`/`ram_max`); `lisa_mmu_init`
takes a `ram_high` flag set per machine — **`model=lisa` uses the board-based
layout above**, while `model=macxl` keeps RAM low (based at 0), since
MacWorks XL's framebuffer and boot path live in low memory. (`GSRAMMIN=1`
remains as a debug override that forces the high base on any model.)

### 3.2 I/O space map (physical `$00C000–$00FFFF`)

The full I/O space is below. No expansion cards are modelled, so an access to
any slot decode takes a **bus error**, as it does on a real Lisa with the slot
empty: the CPU waits 30-300 us for a device to answer and times out (Lisa
Hardware Manual 1983, bus handshaking). Both the boot ROM's `RDSLOTS` and the
Lisa OS's `EXISTS_CARD` detect an empty slot by that bus error; the model used
to read a floating `$FF` there, which the OS took for a card with ID `$FFF` in
every slot.

| Physical range | Function |
| --- | --- |
| `$000000 – $001FFF` | Expansion slot 1, low decode |
| `$002000 – $003FFF` | Expansion slot 1, high decode |
| `$004000 – $005FFF` | Expansion slot 2, low decode |
| `$006000 – $007FFF` | Expansion slot 2, high decode |
| `$008000 – $009FFF` | Expansion slot 3, low decode |
| `$00A000 – $00BFFF` | Expansion slot 3, high decode |
| `$00C000 – $00CFFF` | Floppy-disk controller (6504A) shared RAM/registers |
| `$00D000 – $00D3FF` | Serial ports: the SCC (§15), every 8-byte mirror |
| `$00D800 – $00DBFF` | Parallel port: VIA2 |
| `$00DC00 – $00DFFF` | Keyboard/mouse: VIA1 (COPS) |
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
memory. It is **not** a Motorola PMMU and uses **512-byte pages** (not 4 KB):
128 segments of up to 128 KB each, four context tables with a live
supervisor-to-context-0 override, and a three-way space decode (main memory,
I/O, special I/O). The full register file (SOR and SLR, descriptor addressing
in special-I/O space, read-back and reset state), the behaviour (translation,
limit check, space decode, context selection, START mode, violations) and the
programming model (the Boot ROM's bring-up, the OS's System Mapping Table,
demand loading through the bus-error handler, MacWorks XL) are on the
[segment MMU](mmu.md) page, §2–§4. The subsections below remain as numbered
anchors for cross-references and point into that page.

### 4.1 Logical address decomposition

Bits 23–17 select one of 128 segments, bits 16–9 the page within it (up to
256), bits 8–0 the byte within a **512-byte page** — so a segment is up to
256 × 512 = 128 KB. See [mmu.md](mmu.md) §3.1.

### 4.2 Per-segment descriptors — SOR and SLR

Each segment, per context, carries a 12-bit **SOR** (the segment's physical
origin page) and a 12-bit **SLR** (bits 11–8 the access/space code, bits 7–0
the length in pages, two's-complement — sense inverted for stack segments).
Register detail: [mmu.md](mmu.md) §2.2–2.3.

### 4.3 Access-control / space-type bits (SLR bits 11–8)

The four code bits select main memory (read-only or read/write, each plain or
stack), I/O space, an invalid page, or special-I/O space, and decode to the
MEM / IO / RO / STK flags. See [mmu.md](mmu.md) §2.3 and §3.3.

### 4.4 Translation and limit check

The physical page is SOR + page displacement (high nibble of the sum forced
to zero); in parallel the displacement is limit-checked against the SLR, an
out-of-limit access suppressing CAS — no memory cycle — and raising a Bus
Error, with the carry sense inverted for stack segments. See
[mmu.md](mmu.md) §3.1–3.2.

### 4.5 Contexts (SEG1 / SEG2)

Four 128-segment tables selected by the SEG1/SEG2 latch bits (strobed at
`$00E008`–`$00E00E`, §6.1); supervisor mode (FC2) forces context 0 live, on
every supervisor-bit change. See [mmu.md](mmu.md) §3.4.

### 4.6 Special-I/O space and MMU-register addressing

Descriptors are written through special-I/O space at address words encoding
the segment number, with a low address line selecting SOR vs SLR; the boot ROM
is addressed directly in the same space. See [mmu.md](mmu.md) §2.4.

### 4.7 START / setup mode (MMU bypass at reset)

While START is set (the SETUP strobes `$00E010`/`$00E012`, §6.1 — note the
documented polarity errata), logical address bit 14 switches each access
between the boot ROM in special-I/O space (MMU bypassed) and normal
translation. See [mmu.md](mmu.md) §3.5 and §4.1.

### 4.8 Violation behavior (emulation)

An out-of-limit access, an access to an invalid segment, or a write to a
read-only segment suppresses CAS and raises a 68000 Bus Error; the OS recovers
demand-loaded segments through that fault, so the group-0 stack frame's
instruction register, faulting address and per-opcode saved-PC back-up
(including the RTS re-execution rule) are load-bearing, and data faults must
be delivered at instruction completion, not only fetch faults. The full
contract is on [mmu.md](mmu.md) §3.7 and §4.6.

---

## 5. The Memory Management Unit — emulation seam

For an emulator built around a logical→physical translation step, the Lisa MMU
slots in where a Motorola PMMU would, with three differences to honor: **page
size is 512 bytes**, not 4 KB; the translator must return a **space tag**
(main / I/O / special-I/O) from the SLR access bits so the access is routed to
RAM, the device decoder, or the ROM/MMU-register decoder; and there are **four
context tables** plus the supervisor-to-context-0 override instead of the
PMMU's supervisor/user split. The space flags and context behaviour are on
[mmu.md](mmu.md) §3.3–3.4, and the page-size, fault-delivery and read-back
consequences an implementation must honor are collected in its §5 (Quirks &
errata). Descriptor writes (§4.6), the context/START strobes (§6.1), and the
special-I/O decode all belong to the MMU model; the rest of the machine only
wires the strobe addresses to it.

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
> see docs/internals/machines/lisa/fdc.md. **This requires a faithful `STOP` instruction:** the OS
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

The Lisa's display is a **720 × 364**, 1-bit, ~60 Hz raster (379 scanned
lines, 364 displayed, 45 words per line) generated by a discrete video state
machine that scans a **32 KB page in main RAM**, located by the write-only
**Video Address Latch** at `$00E800` (an un-translated physical address —
bits 0–5 hold A15–A20) and relocatable to any 32 KB-aligned page. The
vertical-retrace (VBL) interrupt fires at **IPL 1**, edge-triggered and
latched until software acknowledges it, and is enabled/disabled by the
`$00E018`/`$00E01A` strobes; the retrace status is Status Register bit 2
(`$00F800`, active-low). The full register file (latch, strobes, status and
error bits, contrast latch), the interleaved video memory cycle, the state
machine, the framebuffer layout and the driver sequences the Boot ROM, the
Lisa OS and MacWorks XL actually perform are on the [video](video.md) page,
§2–§4. The subsection below remains as a numbered anchor and points into
that page.

### 8.1 Macintosh XL video

The Macintosh XL screen-modification kit rewires the horizontal timing to a
**608-pixel square-pixel raster** — 76-byte rows, with the Finder actually
painting 608 × 431 out of the same 32 KB page — leaving the framebuffer page,
the base latch and the VBL unchanged; only the displayed geometry and the
pixel aspect differ. See [video.md](video.md) §1.4 and §4.7.

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
[via.md](../../hardware/via.md); only the Lisa wiring and addressing are given here. The Lisa
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

A National Semiconductor **COP421-class** slave microcontroller (two of
them — one on the I/O board facing the 68000, one inside the keyboard)
services four peripherals through the A-port of VIA1: the keyboard, the
mouse, the real-time clock, and software power control. It runs from the
standby/battery supply and is **always powered** (it keeps the clock running
and can power the machine on/off); the host writes a **command byte** to
VIA1 ORA (`$00DD83`) and reads a **response stream** from the same register,
with every queued response byte raising VIA1's interrupt (IPL 2). The full
command byte, the response packets (key events, reset/status, keyboard
identification, mouse), the scan-loop and power behaviour, and the driver
sequences the Boot ROM and the Lisa OS actually perform are on the
[COPS](cops.md) page, §2–§4. The subsections below remain as numbered anchors
for cross-references and point into that page.

### 11.1 Command byte format (written to VIA1 port A)

The command byte encodes I/O-port on/off, clock read / write-nibble /
set-modes (including the power bit and the timer modes) and the NMI-key
nibbles; `1xxx xxxx` is a no-op. See [cops.md](cops.md) §2.2.

### 11.2 Reset / status response codes

A `$80` lead-in byte followed by a code byte: self-test failures, keyboard
unplugged, clock-timer interrupt, the soft power-off switch (`$FB`), or a
clock-data marker (`$Ey`). See [cops.md](cops.md) §2.4.

### 11.3 Keyboard

Key events are bytes of the form `d rrr nnnn` (direction bit plus
scan-matrix code) with N-key rollover; any key can be programmed to raise the
keyboard-reset NMI, and the keyboard reports a layout/legend ID byte. The key
codes, layout IDs and the boot ROM's final-US ASCII table are on
[cops.md](cops.md) §2.3, §2.5, §3.3 and §4.7.

### 11.4 Mouse

Mouse reports are three response bytes — a `$00` marker, then signed `dx` and
`dy` deltas that accumulate in the COPS until read; the button arrives as a
key code. The COPS counts the motion pulses itself, so the CPU sees cooked
deltas, never quadrature. See [cops.md](cops.md) §2.6, §3.4 and §4.9.

### 11.5 Real-time clock

Resolution is 1/10 second with a 16-year span (the year nibble anchored at
1980); the timer can interrupt and/or power the machine on after a programmed
interval. The digit layout and the read and set sequences are on
[cops.md](cops.md) §3.5 and §4.4.

### 11.6 Soft power

The power switch while running delivers reset code `$FB`; software performs
cleanup and powers the machine off with a `0010 spmm` command with `p = 0`.
The power-on/off sequencing is on [cops.md](cops.md) §3.6 and §4.10.

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
with the 68000** at physical `$00C001`–`$00C7FF`, logical `$00FCCxxx`), *not*
an Apple IWM: the 68000 writes a command block into the shared RAM, strokes a
single go byte, and the 6504 performs the whole sector-level operation,
interrupting at **IPL 1** on disk insertion, eject-button press and command
completion (the interrupt flag must be enabled before the 68000 may access
the shared RAM, or a bus error results — see [fdc.md](fdc.md) §3.3). An
emulator therefore models the controller at the **command-block /
logical-sector** level and never deals with raw GCR cells; the one drive is a
Sony 400 KB 3.5″ mechanism on the Lisa 2 / Macintosh XL (the Lisa 1's twin
Twiggy drives are a different machine and out of scope). The full shared-RAM
register map, the coprocessor's behaviour (command handshake, interrupts,
seek, spindle-speed control, GCR sector format, retries) and the driver
sequences the boot ROM, loader and OS actually perform are on the
[floppy controller](fdc.md) page, §2–§4; the subsections below remain as
numbered anchors for cross-references and point into that page.

### 13.1 Controller commands (written to `$00C001`)

The **go byte** command set — `$81` execute RWTS, `$83` seek, `$84` call,
`$85`–`$87` interrupt status and mask control, `$88`/`$89` diagnostic loops —
is documented with its handshake on [fdc.md](fdc.md) §3.1 and §4.1.

### 13.2 RWTS command block (logical `$00FCC003+`)

Command, drive, side, sector, track, speed/busy, format-confirm, error,
disk-ID and disk-type bytes — the disk-type byte selects the Twiggy vs Sony
block-to-(track, sector) conversion — plus the Sony five-zone GCR zoning. See
[fdc.md](fdc.md) §2.2, §4.2 and §4.5.

### 13.3 Status byte (`$00C05F`)

Latched interrupt-event bits per drive (insertion, eject, completion, with
the OR bits), cleared by the `$85` command — one-shot events, not a live
drive snapshot, with the single Sony as drive 1 (bits 0–3). See
[fdc.md](fdc.md) §2.4 and §3.4.

### 13.4 Controller ROM id and parameter memory

`$FCC031` — the disk-controller ROM id the boot ROM and OS read to detect the
machine type (§16.2) — and `$FCC181` — the battery-backed 64-byte parameter
memory, valid when its rotate-sum checksum reads `0`. See [fdc.md](fdc.md)
§2.3 and §2.6, and [pram.md](pram.md) for the parameter block itself.

---

## 14. Parallel Hard Disk — ProFile / Widget (NOT SCSI)

The Lisa hard disk is driven over an 8-bit bidirectional **parallel port**
built from VIA2 (`$00D901`, decoded across the whole `$00D800`–`$00D9FF`
window — see §10.2), with a request/strobe handshake: the external
**ProFile** (5 MB / 10 MB) and the internal **Widget** (on the Lisa 2/10 and
Macintosh XL) both use this interface, and there is no SCSI hardware on the
Lisa. The host exchanges state bytes over VIA2 port A, sends a 6-byte
command block (command, 24-bit block number, retry count, sparing
threshold), and whole **532-byte blocks** (a 20-byte page label plus 512 data
bytes) stream one byte per `PSTRB/` handshake, interrupting at **IPL 1** via
CA1/`BSY`. The full connector and 6522 wiring, the handshake primitive, the
command, status and block formats, sparing and the Widget's system commands,
and the probe and driver sequences the Boot ROM and the Lisa OS actually
perform are on the [ProFile/Widget](profile.md) page, §2–§4; the subsections
below remain as numbered anchors and point into that page.

### 14.1 6522 pin mapping

VIA2 port A is the data path; port B carries `OCD`, `BSY`, `DEN`, `DRW`,
`CMD/` and the diagnostic lines, with CA1 = `BSY` (the interrupt edge), CA2 =
`PSTRB/` and CB2 = parity status; `PRES/` and `CRES/` live on VIA1 (§10.1).
See [profile.md](profile.md) §2.2.

### 14.2 Handshake and command protocol

Every phase is the same five-beat handshake — assert `CMD/`, read the
controller's state byte, reply `$55` — followed for I/O by the 6-byte
command block, its bytes clocked by `PSTRB/` and spaced 14–21 CPU cycles
apart. See [profile.md](profile.md) §3.1 and §3.2.

### 14.3 Block format

Each block is 532 bytes on the interface — a 20-byte page-label header plus
512 data bytes, label-first on the ProFile — which the OS expands into its
24-byte pagelabel/distributed-directory record; the Widget reports its size
at init and a ProFile is assumed to be ~9,720 blocks. See
[profile.md](profile.md) §3.6, §3.8 and §3.3.

---

## 15. Serial — Z8530 SCC

A dual-channel Zilog **Z8530 SCC**. Its internal register file and baud-rate
generator are described in [scc.md](../../hardware/scc.md); the Lisa specifics are:

- **Addresses** (physical I/O space; the address pins follow the standard
  A1 = A/B-select, A2 = data/control convention). The chip select is the
  whole Serial Ports Control block, `$00D000–$00D3FF` (Hardware Manual
  Fig. 2-5), and only A1 and A2 reach the chip, so every 8-byte mirror is
  the same four registers. The boot ROM and the manual's table use `$D24x`;
  the Lisa OS RS-232 driver uses `$D20x`:

  | Phys addr (ROM / OS) | Channel | Access |
  | --- | --- | --- |
  | `$00D241` / `$00D201` | B | control |
  | `$00D243` / `$00D203` | A | control |
  | `$00D245` / `$00D205` | B | data |
  | `$00D247` / `$00D207` | A | data |

- **Interrupt:** **IPL 6**, autovectored (vector `$000078`).
- **Clocks:** channel A's PCLK input is **4.000 MHz**, channel B's is **3.6864
  MHz**. Baud time constants are `TC(A) = 4,000,000 / (2 × baud) − 2` and
  `TC(B) = 3,686,400 / (2 × baud) − 2`. Port B is used for AppleBus (LocalTalk).

Because the address pins follow the same A1/A2 convention as the Macintosh, a
generic SCC model that decodes `channel = ~(A1)` and `data = A2` works unchanged
when mapped across `$00D000–$00D3FF`. (It was mapped at `$00D240` only, eight
bytes, which served the boot ROM and silently dropped every access the OS's
serial driver made.)

- **Serial A's handshake and output.** The OS's RS-232 driver transmits on
  port A only while DSR is asserted, which the Lisa wires to the SCC's
  `/SYNC` input and reads as RR0 bit 4 (`source-rs232`: `xmtrr0 := $10` for
  channel 0). The machine declares that wiring
  (`scc_set_port_ready_line`), and a host file attached to the port stands
  for a ready device on the cable: `machine.scc.a.output = "/path/file"`
  raises DSR and streams everything the guest transmits into the file;
  `machine.scc.a.output = none` lowers it again. With the Office System's
  device configuration set to "Imagewriter / II DMP" on Serial A (the
  LOS 3.1 install default), File/Print then writes the document's
  ImageWriter command stream into the file; with no output set, the driver
  reports "difficulty printing" and sends nothing. Port B (AppleBus) has no
  wired handshake. See [scc.md](../../hardware/scc.md), "The far end
  of a port", and the `lisa-serial-output` integration row.

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
| 6522 VIA ×2 (§10) | reuse the generic 6522 model ([via.md](../../hardware/via.md)); Lisa wiring only |
| Z8530 SCC (§15) | reuse the generic SCC model ([scc.md](../../hardware/scc.md)); Lisa addresses/clocks only |
| 68000 CPU (§2) | reuse; feed fixed-IPL interrupts and bus errors |

The genuinely new silicon is the **segment MMU**, the **COPS**, and the
**parallel hard-disk** interface; everything else is either a thin Lisa-specific
front-end over an existing block/image path or a reuse of the generic 6522/8530
and 68000 models with Lisa wiring.
