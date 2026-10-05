# HMC — the PDM memory controller

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, clocking
2. [Register file](#2-register-file) — the 35-bit serial configuration register, reset state,
   machine-ID register
3. [Behaviour](#3-behaviour) — address decode, DRAM banks, refresh, the 601 bus,
   arbitration, L2 cache, video FIFO, ROM control
4. [Programming model](#4-programming-model) — the boot ROM's init sequence, RAM sizing,
   timing configuration, cache test, warm boot
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The HMC ("high-speed memory controller" [1]; the 8100 logic-board schematic titles the part
"Half A Memory Controller", U20, sheet 4 [5]) is the Apple ASIC that controls all memory
operations on the first-generation Power Macintosh platform, Apple codename PDM [1][2]. On one
chip it combines:

- **DRAM control** — RAS/CAS/WE generation for the 8 MB of soldered motherboard DRAM and all
  72-pin SIMM banks, with CAS-before-RAS refresh every 15.6 µs [1] and a software-programmed
  refresh-rate divider (§3.3).
- **ROM control** — chip-enable/output-enable and write strobe to the 4 MB ROM SIMM, plus two
  extra address lines to it (§3.8) [5].
- **The 601 bus protocol engine and address-bus arbiter** — all basic PowerPC 601 transfer
  protocols, four-beat 32-byte bursts, misaligned-read translation, address-only transactions;
  and central arbitration between the processor, DMA, video refresh, DRAM refresh and bus
  masters on the PDS and the NuBus controller (§3.4, §3.5) [1][5].
- **Second-level cache support** — control lines for the 160-pin cache SIMM, whose size is
  sensed through two connector pins at startup [1] (§3.6).
- **The video FIFO write side** — eight-beat burst fetches of frame-buffer data from DRAM into
  the data path chips' FIFOs, at the second-highest arbitration priority [1][5] (§3.7).
- **A serial configuration register** — the part's only software-visible programming
  interface: one 35-bit register, shifted bit-serially through a port decoded by the AMIC I/O
  chip (§2) [3][5].
- **A machine-ID register** at the top of I/O space, $5FFFFFFC [1] (§2.5).

The HMC generates no CPU interrupts; platform interrupts are collected by the AMIC's VIA
channels [1]. Everything the ROM and OS know about memory — bank placement, sizes, timing,
cache presence — is either read out of this chip or programmed into it through the serial port.

### 1.2 Machines that carry it

| Machine | Bus clock | Machine-ID low word | SIMM sockets (pairs) | Max RAM | L2 cache |
|---|---|---|---|---|---|
| Power Macintosh 6100/60, /60AV | 30 MHz | $3010 | 2 (1) | 72 MB | optional SIMM |
| Power Macintosh 6100/66 | 33 MHz | $3010 | 2 (1) | 72 MB | optional SIMM |
| Power Macintosh 7100/66, /66AV | 33 MHz | $3012 | 4 (2) | 136 MB | optional SIMM |
| Power Macintosh 7100/80 | 40 MHz | $3012 | 4 (2) | 136 MB | optional SIMM |
| Power Macintosh 8100/80, /80AV | 40 MHz | $3013 | 8 (4) | 264 MB | 256 KB standard |
| Power Macintosh 8100/100 | 33.3333 MHz | $3013 | 8 (4) | 264 MB | optional SIMM |
| Power Macintosh 8100/110 | 36.6667 MHz | $3013 | 8 (4) | 264 MB | 256 KB (new design) |

Bus clocks and cache data from [1] Table 2-2 and [2] Table 1-2 / p. on the 8100/110; SIMM
sockets and RAM maxima from [2] Table A-1; machine-ID values from [3] (§2.5). All models carry
8 MB of soldered 80-ns DRAM [1]. AV models carry the same HMC; their video card stores video
information in a frame buffer separate from main memory [1], so it does not draw on the HMC's
frame-buffer fetch path. The pin-level facts in this document come from the 8100 schematic [5];
the 6100 and 7100 boards are not in the evidence set, and their HMC wiring is *inferred* to
match (*unverified*). The Workgroup Server 9150 is reported to share the part (*inferred —
unverified*; see §6).

### 1.3 Clocking

The HMC is clocked by the CPU bus clock: the clock generator's bus-clock output (the same
30/33/40-class clock feeding the AMIC) drives the chip's CLK pin [5]. All HMC timing — DRAM
cycles, refresh prescale, arbitration — derives from this clock, and the ROM's configuration
tables are selected by the measured bus frequency (§4.4).

## 2. Register file

### 2.1 The serial configuration port

The HMC has no parallel register file. Its one configuration register is 35 bits wide and is
accessed bit-serially: the AMIC decodes the CPU addresses below and asserts the HMC's select
pin, while the data bit rides on I/O-data bit 0 into the HMC's MDIO pin [5] (*observed* in the
schematic wiring; the address protocol is *observed* in the boot ROM [3]).

| Address | Access | Effect |
|---|---|---|
| $50F40008 | byte write (any value) | Reset the bit pointer to bit 0. Performed once before each full read or write sequence. |
| $50F40000 | byte write | Shift one bit **in**: bit 0 of the written byte enters the register; pointer advances. |
| $50F40000 | byte read | Shift one bit **out**: the next bit appears in bit 0 of the returned byte; pointer advances. |

The exact decode granularity inside the $50F4xxxx window (which offsets strobe vs. shift) is
not established; the ROM only ever touches +0 and +8, so that pair suffices as the contract.

### 2.2 Access protocol

The boot ROM's write routine (at $FFF03838 in the 1994 ROM) writes the strobe, executes `eieio`,
then performs 35 single-byte writes to $50F40000, taking bit 0 of each byte and rotating the
source word right by one between writes — i.e. **bit 0 is shifted first, LSB-first** — 32 bits
from one word, 3 more from a second. The read routine ($FFF0387C) mirrors this with byte reads,
collecting bit 0 of each into the words the same way. Every access in both sequences is
separated by `eieio` [3]. A full sequence is thus 36 bus accesses: one strobe plus 35 shifts.

The 4 MB ROM contains exactly two copies of the routine pair — the early-init pair
($FFF03838/$FFF0387C) and a POST-region pair ($FFF08484/$FFF084C4) — and these four routines
are the only instructions in the entire image that access $50F4xxxx; every other occurrence of
the address in the ROM is table data (*observed*, byte-pattern scan of both ROM images [3][4]).
After the power-on tests complete, no ROM code touches the configuration register again.

Reads and writes share the single bit pointer; the ROM never interleaves them without a strobe
in between. The register is fully readable: bits 2–31 read back as written, bits 0–1 read the
cache-size sense pins (§2.3). Whether each shifted bit commits immediately or the register
latches only on the 35th bit is not established; the ROM always writes all 35 bits after a
strobe (its one partial update — the checksum-failure path, §4.8 — is a read-modify-write of
the whole register), so per-bit immediate commit is the simplest model consistent with all
observed use.

### 2.3 Configuration bit map

Bits are numbered by shift order: bit N is the Nth bit shifted (N = 0…34). Bits 0–31 form a
32-bit low word (masks below); bits 32–34 are a 3-bit high field (high-word bits 0, 1, 2).
Assignments are derived from the boot ROM's configuration code [3]; uncertain fields are
marked and listed again in §6.

| Bits | Low-word mask | Read | Write | Assignment |
|---|---|---|---|---|
| 0–1 | $00000003 | cache-SIMM size-sense code | ignored | L2 cache size code (§3.6) |
| 2–15 | $0000FFFC | as written | DRAM timing | DRAM timing field; bit 8 is the ROM's calibration bit (§4.5) |
| 16–21 | $003F0000 | as written | refresh divider | REFRESH_DIV (§3.3) |
| 22–23 | $00C00000 | as written | DRAM timing | timing; meaning unknown, varies with the bus-speed table |
| 24 | $01000000 | as written | timing/cache | cleared during sizing, set at end of init except on fast-bus-with-cache machines |
| 25 | $02000000 | as written | diagnostic | cache test window redirect at physical $50000000 (§4.6) |
| 26 | $04000000 | as written | DRAM timing | set in every table value; meaning unknown |
| 27 | $08000000 | as written | cache enable | second-level cache enable |
| 28 | $10000000 | as written | flag | set when an old AMIC revision is detected; hardware use unknown |
| 29–30 | $60000000 | as written | bank size | SIMM_BANK_SIZE — moves the 6100's SIMM banks (§3.2) |
| 31 | $80000000 | as written | bank size | MB_BANK_4MB — motherboard bank is 4 MB, not 8 MB (§3.2) |
| 32 | — | as written | — | 0 in all known writes; unknown |
| 33 | — | as written | — | video scan-out base select: set = fetch from $00000000, clear = $00100000 (*inferred — unverified*, §3.7) |
| 34 | — | as written | — | slow-bus timing: set in table entries for buses ≤ 37.5 MHz, clear for ≥ 40 MHz |

**L2 size code (bits 0–1).** On read these bits report the two size-sense pins of the cache-SIMM
connector: an empty socket's pull-up resistors make the HMC read "no cache" and disable all
external cache operations [1] p. 15. The boot ROM decodes the code as size = $10000 << code —
1 = 128 KB, 2 = 256 KB, 3 = 512 KB, 0 = none (*observed*, [3] $FFF032DC) — and both the POST
and the cache test gate on `readback & 3 == 0` as "no cache SIMM" [3]. The Dev Note documents
cache sizes of 128 KB to 256 KB [1] p. 15, [2]; which physical SIMM sizes produce which code is
*inferred* only (a 256 KB SIMM presumably reads 2). The write value is ignored by the hardware:
the ROM's tables always write 0 here.

**DRAM timing (bits 2–15, 22–23, 26).** The per-bus-speed timing value comes from the ROM's
table (§4.4); the individual meanings (RAS/CAS lengths, wait states) are not decoded. One bit
is behaviorally pinned: bit 8, which the ROM toggles for its bus-ratio measurement (§4.5). The
all-zero power-on state of the timing field is *slower* than any programmed value — the load
loop the ROM times against it runs measurably longer at reset configuration than under any
table value (*inferred from the ROM's calibration arithmetic and the production machines'
documented clock ratios*; see §4.5).

**Refresh divider (bits 16–21).** See §3.3.

**Bit 24.** Cleared before the first real configuration write even though every table default
sets it; set again near the end of init — unless the cache probe returned data and the measured
bus clock is ≥ 40,000,000 Hz, in which case it is cleared again [3] ($FFF0329C, $FFF032A4).
Plausibly a cache timing or clock-ratio select (*inferred*). The net final states: set on the
6100 and 7100 (and the 8100/100, whose bus is 33.3 MHz); clear on a 40 MHz-bus machine with a
cache SIMM installed — i.e. the 8100/80 as shipped.

**Bit 25.** Set (with bit 27 cleared) only inside the ROM's cache probe and test sequences, and
restored afterwards [3]. While set, physical accesses at $50000000 reach a diagnostic window on
the cache SIMM (§4.6).

**Bit 27.** Cleared before the first configuration write and throughout sizing; the ROM's final
write never sets it (§4.6), and the power-on cache test sets it only for the duration of the
test and clears it again [3].

**Bit 28.** Set when bits 2–3 of AMIC register $50F32008 do not stick when written — a
chip-revision probe of the AMIC — and re-read later by the ROM to shape a boot record
[3] ($FFF032D8, $FFF032EC). Whether the HMC hardware itself consumes the bit is unknown.

**SIMM_BANK_SIZE (bits 29–30) and MB_BANK_4MB (bit 31).** The bank-placement fields; §3.2.

**Bit 33.** Set in every ROM timing-table value, and force-set by the checksum-failure path
before the ROM paints its screen indication [3] ($FFF04018) — i.e. the ROM insists on this bit
whenever video must work. There is no other programmable frame-buffer fetch base in the
platform (the AV card's separate frame buffer excepted [1]), and the released MkLinux kernel
for these machines runs with the bit clear while locating its frame buffer at physical
$00100000 [6] — together these fix the reading: set = scan out from physical $00000000, clear =
from $00100000 (*inferred — unverified*).

**Bit 34.** Set in the ≤ 37.5 MHz table entries, clear in the ≥ 40 MHz entries [3][4];
read as a slow-bus timing select (*inferred*).

### 2.4 Reset state

The power-on value of the full 35-bit word is unknown. Observable power-on behavior:

- SIMM_BANK_SIZE reads as code 0 — the 6100's SIMM banks decode in their 128 MB windows at
  $10000000 and $08000000 (§3.2), because the ROM probes exactly those windows before any
  configuration write [3]; bank 1 also decodes from the motherboard top (§3.2).
- DRAM at physical $00000000 is readable and writable before any configuration write: the
  checksum-failure path hammers $00000000–$0002FFFF at reset configuration (§4.8), and the
  calibration loads run there (§4.5) [3].
- Refresh is running — DRAM contents survive into the warm-boot path (§4.7) [3].
- The machine-ID register works from reset (§2.5) [3].
- The cache-enable bit's reset value does not matter to the boot ROM, which clears it before
  probing [3].

A CPU reset does not clear the register: on a warm boot the ROM does not reprogram it and the
machine continues on the standing configuration (*observed*, [3] §4.7).

### 2.5 The machine-ID register ($5FFFFFFC)

A 32-bit read-only register at the top of I/O space. The Dev Note documents its three low bits
as the machine identification code: 6100 = %000, 7100 = %010, 8100 = %011 [1] Table 1-5.
The full low halfword is $3010 (6100), $3012 (7100), $3013 (8100); the boot ROM reads bytes 2
and 3 of the register with individual byte loads and compares the halfword against $3012/$3013,
dispatching everything else — including the 6100's $3010 — to the 6100 memory path [3]
($FFF0303C–$FFF0305C). The upper halfword reads $A55A on production machines (*inferred —
unverified*: consistent across independent driver reports for these machines, but not
established from a primary source). The meaning of bits 15–4 beyond the identification code is
not documented; see §6. The register must answer byte reads from reset, before any HMC
configuration [3]. No software writes it. Whether a 32-bit read returns the full $A55A30xx
value on real hardware is not verified (§6).

## 3. Behaviour

### 3.1 Physical address decode

The HMC owns the DRAM and ROM decodes of the 32-bit physical space; the I/O region is decoded
by the AMIC (which also gates the HMC's own serial port) [1][5]. The physical memory
allocations [1] Table 2-5:

| Range | Used for |
|---|---|
| $00000000–$0FFFFFFF | RAM |
| $10000000–$3FFFFFFF | RAM aliases |
| $40000000–$4FFFFFFF | ROM |
| $50000000–$5FFFFFFF | I/O (AMIC; the HMC's serial port at $50F40000 and the ID register at $5FFFFFFC live here) |
| $60000000–$FEFFFFFF | not assigned |
| $FF000000–$FFFFFFFF | ROM alias |

**RAM aliases.** The low 256 MB RAM map reappears at $10000000, $20000000 and $30000000 [1];
the boot ROM exercises this by addressing physical $20300000 as an uncached alias of $00300000
in its cache test (§4.6) [3]. How the alias images interact with the fixed SIMM bank windows
that lie inside that range on the 7100/8100 (§3.2) is not established (§6).

**ROM overlay.** The 4 MB, 100-ns ROM image [1] is decoded throughout $40000000–$4FFFFFFF,
repeating every 4 MB (ROM offset = address & $003FFFFF), and across $FF000000–$FFFFFFFF,
where it appears at $FFC00000–$FFFFFFFF: the 601's reset vector fetch at $FFF00100, the boot
ROM's deliberate re-basing of itself to the $40300000 alias, and the OS's canonical ROM base
$40800000 (which the ROM records as its own base/size in the boot header) all land on the same
image [3] (*observed*; the 4 MB repetition across $4xxxxxxx is *inferred* from those exercised
aliases and the Table 2-5 "ROM" range). HMC drives the ROM SIMM's two high address lines and
its output-enable and write strobes [5] (§3.8).

**Undecoded addresses.** Accesses to addresses the MMU maps but no hardware decodes do not
fault in the HMC; the AMIC asserts an error signal after 40 µs, a condition not generally
recoverable [1] p. 21. The HMC itself does not recover from transfer-error acknowledges [1]
p. 15.

### 3.2 DRAM bank architecture and placement

**Inventory.** Every model solders 8 MB of 80-ns DRAM to the board ("bank 0") [1]. Expansion
is by 72-pin SIMMs in identical pairs — two 32-bit SIMMs side by side form one 64-bit bank; a
double-sided pair forms two banks; each SIMM carries two banks of up to 16 MB per bank [1][2].
The HMC's DRAM interface multiplexes 12 row/column address lines (12+12 addressing across 8
bytes = the 128 MB maximum bank decode) and drives one CAS line per byte lane of the 64-bit
bus; it has three RAS outputs — one for the motherboard array and two for the SIMM banks
(A and B, the two banks of a pair); the 8100's eight SIMM banks fan out from the two SIMM RAS
lines through socket-buffering circuitry, so the chip itself distinguishes only A/B [5]
(*observed* on the 8100 schematic).

**Placement is model-dependent**, selected by the machine-ID dispatch (§2.5):

**7100/8100 — fixed, discontinuous windows.** Eight SIMM banks at fixed bases, independent of
SIMM size; each 64 MB bank exposes only its middle 32 MB (usable range starts 16 MB into the
bank and ends 16 MB before its end) [2] "Discontinuous Physical Addressing", Table A-2:

| Bank | Probe/decode window |
|---|---|
| motherboard | $00000000–$007FFFFF |
| 1 | $01000000–$02FFFFFF |
| 2 | $05000000–$06FFFFFF |
| 3 | $09000000–$0AFFFFFF |
| 4 | $0D000000–$0EFFFFFF |
| 5 | $11000000–$12FFFFFF |
| 6 | $15000000–$16FFFFFF |
| 7 | $19000000–$1AFFFFFF |
| 8 | $1D000000–$1EFFFFFF |

(bank *n* at $01000000 + (n−1) × $04000000, 32 MB usable each.) The boot ROM probes exactly
these nine windows [3]; identical to [2] Table A-2. The 7100, with only two pairs, uses the same
windows — banks 3–8 simply size to zero. Maximum contiguous RAM is 32 MB; there is an 8 MB
hole between the soldered RAM top and the first SIMM range [2].

**6100 — movable banks, a pure function of SIMM_BANK_SIZE.** In the power-on state
(SIMM_BANK_SIZE = 0, the "128 MB bank" code) the motherboard bank decodes at $00000000
(8 MB window) and the two SIMM banks decode in 128 MB windows — **SIMM bank 1 at $10000000,
SIMM bank 2 at $08000000** — which is why the ROM probes the windows $10000000–$17FFFFFF and
$08000000–$0FFFFFFF [3] (*observed*). After the ROM sizes the banks it writes a size code
derived from the measured second-bank size:

| Code (bits 29–30) | Bank size | Placement after the write |
|---|---|---|
| 0 | 128 MB (or bank 2 empty) | reset map: bank 1 @ motherboard top up to $07FFFFFF and @ $10000000, bank 2 @ $08000000 |
| 1 ($20000000) | 2 MB | bank 1 @ motherboard top; bank 2 @ bank 1 top |
| 2 ($40000000) | 8 MB | likewise, contiguous |
| 3 ($60000000) | 32 MB | likewise, contiguous |

I.e. for codes 1–3 the SIMM banks appear **contiguously after the motherboard bank**: bank 1 at
(motherboard size), bank 2 at (motherboard size + bank 1 size) [3] ($FFF03CF0–$FFF03D54)
(*observed*: the ROM writes the code and then immediately builds its bank records through the
new map). The decode window of a placed bank is *inferred* to match the size code (2, 8 or
32 MB); it is not separately observable because the probe runs before configuration.

**Code 0 with bank 2 empty.** Each SIMM carries up to two DRAM banks [1] p. 14 and the HMC
drives one RAS line per bank of the pair (`HmcSimmRasA*`/`HmcSimmRasB*`) [5] sheet 4, so a
pair of single-sided SIMMs fills SIMM bank 1 only — the common upgrades of an 8 MB machine
to 10, 12 or 16 MB leave SIMM bank 2 empty. The ROM then leaves SIMM_BANK_SIZE at code 0, accepts any bank 1 up
to $780 × 64 KB = 120 MB, and still re-bases the bank record: bank 1 at the motherboard top
($00800000) [3] ($FFF03CF0–$FFF03D20, then $FFF03D04). Every later stage builds on that
record — the 68k Start Manager fabricates its memory table from it without re-probing — so in
the code-0 map bank 1 is decoded from the motherboard top as well as in its $10000000 probe
window (*inferred* from the ROM's record; required for any single-bank machine to boot). The
120 MB ceiling is the gap between the motherboard top and bank 2's window at $08000000: bank
1 occupies $00800000–$07FFFFFF, aliasing through it when smaller. Whether bank 1's
$10000000 image is a separate decode or the RAM-alias image of the low 256 MB (§3.1) is not
observable to software.

**Aliasing.** An undersized bank aliases — wraps — throughout its decode window: the sizing
probe (§4.2) finds and measures banks *through* these aliases, so a 4 MB motherboard array
appears twice in its 8 MB window, an 8 MB SIMM bank appears four times in a 7100/8100 32 MB
window, and SIMM contents fill the whole 128 MB windows of the 6100 reset map [3]. A bank
smaller than its window reports its lowest alias as the base. An **empty** bank must not read
back what was last written to it — the probe's signature compare must fail on open sockets
(*observed* as a requirement of the probe algorithm [3]).

**MB_BANK_4MB (bit 31).** The motherboard bank is probed in its 8 MB window; 4 MB arrays alias
twice and the probe returns 4 MB, whereupon the ROM toggles bit 31 to record the 4 MB capacity
[3] ($FFF03CB8). The bit's effect on the decode beyond the recorded size is not established.

### 3.3 DRAM refresh

Refresh is a CAS-before-RAS cycle every 15.6 µs, and does not affect RAM timing for data
accesses [1] p. 22. The refresh *rate* is software-programmed through the REFRESH_DIV field
(bits 16–21): the boot ROM computes `floor(bus_Hz / 3,299,911) − 1` and inserts it there,
clearing the whole field if the divide yields zero [3] ($FFF038EC–$FFF03904). The table defaults
independently confirm the formula: 66.67 MHz → 19, 50 MHz → 14, 40 MHz → 11, 33.33 MHz → 9,
30 MHz → 8, 25 MHz → 6, 20 MHz → 5 [3]. The net effect is an internal prescale of ≈ 3.3 MHz;
what the divider counts and how it reaches the 15.6 µs cadence internally is not established
(§6). DRAM refresh holds the highest arbitration priority [1] Table 2-4.

### 3.4 The 601 bus interface

The HMC terminates the 601's 64-bit non-pipelined bus [1] and supports:

- all basic transfer protocols, including all single-cycle (single-beat) accesses;
- four-cycle 32-byte cache-line (burst) accesses;
- translation of misaligned read actions into double-word read actions;
- address-only transactions [1] p. 15.

It does **not** support: pipelining of memory bus transactions, cache snooping, recovery from
transfer-error acknowledge signals, or little-endian transfer mode [1] p. 15 — and the AMIC/HMC
pair rejects the 601's extended transfer protocols with a transfer error exception [1].

The 8100 schematic [5] shows the wiring behind this contract (*observed*): the full 32-bit
address bus and transfer-start, cache-inhibit, burst, transfer-size and one transfer-code line
enter the chip; only **two** transfer-type bits and **one** transfer-code bit are wired —
consistent with the documented lack of extended-protocol support. The HMC returns transfer
acknowledge, address acknowledge and data-retry to the CPU. It also drives the data path
chips' memory-side latches (read/write buffer enables, data latch enable, data write enable)
that bridge the 64-bit CPU bus, the 64-bit memory bus and the 16-bit I/O bus [1][5].

### 3.5 Bus arbitration

The HMC arbitrates the **address** part of the CPU bus; access to the data part follows
address tenure. The processor holds the bus by default [1] p. 20. Priority order, highest to
lowest [1] Table 2-4:

1. DRAM refresh
2. Video refresh
3. I/O DMA for SWIM III (floppy)
4. I/O DMA for AWAC (sound)
5. I/O DMA for SCSI
6. I/O DMA for SCC (serial)
7. Expansion card
8. Main processor

At the pin level (8100 schematic [5]) the request/grant pairs sit on the HMC itself: the AMIC's
DMA request, the PDS card's request, and the NuBus controller's master request come in, and the
HMC grants the bus to the DMA engine, the PDS, the NuBus controller and the 601. PDS design
rules [1] p. 20: a requesting card must verify the bus is free via the ABB signal, bus grants
arrive after a one-CPU-cycle latency, only basic processor cycles and cache-line operations
are recognized, and higher-priority grants may preempt — in particular video refresh "may
sometimes absorb much of the bus capacity" [1].

### 3.6 The second-level cache interface

All models accept an external second-level cache on a 160-pin SIMM (128–256 KB; the 8100/80
ships with 256 KB) [1] p. 15, [2]. The HMC senses the SIMM's size from two connector pins at
startup; with no SIMM installed, pull-up resistors on those pins cause the HMC to disable all
external cache operations [1] p. 15 — the sensed code surfaces in configuration bits 0–1
(§2.3). The HMC drives the cache SIMM's per-byte-lane write enables, address-advance,
transfer-start, output-enable and chip-select lines, and the tag write/select strobes; the tag
comparison itself happens on the SIMM, which returns a MATCH (hit) signal the HMC consumes
[5] (*observed*). The HMC performs no cache snooping [1] p. 15 — coherency between the cache
and DRAM is not maintained by hardware, a property the ROM's cache test explicitly depends on
(§4.6). The 8100/110 uses a new 256 KB cache SIMM design for its 36.6667 MHz bus [2].

### 3.7 The video FIFO write side

Built-in video keeps its frame buffer in main DRAM. The data path chips provide FIFO buffering
for video monitor data, "fetched from RAM as eight-cycle bursts" [1] p. 16: the AMIC's video
timing engine requests refills (VREQ) and the HMC fetches frame-buffer data from DRAM in
eight-beat bursts, writing it into the two data path chips' FIFOs under the "video refresh"
arbitration priority (§3.5) [1][5]. The *fetch base* is not programmable through any video
register: it is fixed by configuration bit 33 (§2.3) — set, fetch from physical $00000000;
clear, from $00100000 (*inferred — unverified*) — which is why the ROM's constant state has
the bit set (§4.4) and why its failure path force-sets the bit before painting the screen
(§4.8). AV machines' video card has its own frame buffer, separate from main memory [1], and
does not use this path.

### 3.8 ROM control

The HMC drives the ROM SIMM's output-enable, its write strobe (wired as a flash write enable),
and two address lines beyond the 4 MB image [5] (*observed*). The ROM is 4 MB of 100-ns parts
[1]; the two extra lines give the socket headroom for larger/flash parts, but no shipped ROM
image or known software exercises that capability (*inferred* from the wiring; see §6). The
decode windows are §3.1's: repeated every 4 MB across $40000000–$4FFFFFFF and present at
$FFC00000–$FFFFFFFF.

## 4. Programming model

The 1994 boot ROM (version $077D, checksum $9FEB69B3 [3]) drives the HMC exactly as follows;
the 1995 ROM ($9B7A3AAD [4]) differs only in the timing table noted in §4.4. All addresses are
ROM offsets in the $FFFxxxxx alias.

### 4.1 Cold-boot initialization sequence

The PowerPC hardware-init at $FFF03000 performs, in order (*observed* [3]):

1. **Checksum itself** (whole image, per-lane and doubleword sums). On failure, run the §4.8
   path, then continue regardless.
2. **Set up untranslated access** to physical segments 0, 1 and 5 (forced-I/O segment
   registers), so DRAM and I/O work with the MMU off; re-base execution to the $40300000 ROM
   alias.
3. **Quiesce the AMIC**: soft-reset every DMA channel, stop sound, mask interrupts, blank video.
4. **Measure the CPU clock** — a fixed dependent-add loop timed against the decrementer,
   snapped to a table of standard frequencies.
5. **Measure the bus ratio** using the HMC's timing field as an instrument (§4.5); divide the
   CPU clock by the measured ratio to obtain the bus clock.
6. **Write the timing configuration**: pick a table row by measured bus clock (§4.4), clear
   bits 24 and 27, recompute REFRESH_DIV from the measured bus clock, and write the full
   35-bit register — the first real configuration write.
7. **Dispatch on machine ID** (§2.5): $3012/$3013 → the nine fixed windows of §3.2; anything
   else → the three 6100 windows. **Probe every window** (§4.2).
8. On diagnostic boots only, mark banks bad from a flag area at physical $40004–$40024 by
   OR-ing bit 0 into their recorded sizes.
9. **Write the bank configuration**: verify bank 0 is 4 or 8 MB (set MB_BANK_4MB for 4 MB);
   on the 6100 require size(bank 1) ≥ size(bank 2) and sizes in {0, 2, 8, 32, 128} MB — any
   other result abandons the reconfiguration — then write the SIMM_BANK_SIZE code; **the SIMM
   banks move now** (§3.2). Special case: bank 2 empty allows bank 1 up to 120 MB with code 0,
   recorded at the motherboard top like the packed codes (§3.2).
10. Pick a staging area in the first bank with ≥ 128 KB (base + $A000), zero 512 bytes.
11. **Build the boot records**: a nine-entry {base, size} bank table (motherboard first, then
    banks in probe order), sizes truncated to 128 KB multiples, plus total RAM, the measured
    clock rates, and the ROM base $40800000 / size 4 MB.
12. **Probe the cache** (§4.6); finalize bit 24 (§2.3); run the AMIC-revision probe into bit 28;
    perform the **final configuration write** — which never sets the cache-enable bit (§4.6).
13. Hand the header and info records to the next boot stage. The 68k-side and disk-loaded
    software consume the pre-built bank table rather than re-probing memory (*inferred —
    unverified*: the handoff is observed; the consuming code is outside the disassembly
    evidence).

### 4.2 The RAM sizing probe

The probe routine at $FFF03B00 [3], called once per window with a 2 MB step and window bounds,
returns each bank's base and size while preserving memory contents:

1. Write the 8-byte signature pair $44617669/$64696B61 ("Davidika"), starting at (window top − 8)
   and stepping down by 2 MB: save the two words, write the signature, read it back. A
   mismatch steps down; running past the window bottom means the bank is empty (size 0).
2. On the first hit (top of RAM at address T), scan **up** from the window bottom for the
   signature's lowest alias, then write a swapped signature at T and confirm it appears at
   the alias — guarding against phantom matches. Restore the original data.
3. Repeat from the window bottom to find the base's lowest alias, and deduce base and size.

Consequences for the decode, all *observed* as requirements of this algorithm [3]: undersized
banks must alias through their windows (§3.2); empty banks must not echo writes; the probe
reads and writes locations across the whole window. The motherboard bank is probed the same
way in $00000000–$007FFFFF; a 4 MB array sizes at 4 MB through its double alias [3].

### 4.3 Bank configuration and the boot bank table

See §3.2 for the placement semantics and §4.1 steps 9–11 for the sequence. The resulting bank
table is the platform's memory contract: nine {base, size} longword pairs plus the total, in
the boot header, with sizes in 128 KB granularity and bit 0 of a size entry overloaded on
diagnostic boots as the bank-disable flag [3].

### 4.4 Timing configuration and the refresh divider

The ROM selects the first 12-byte record whose bus-clock threshold is ≤ the measured bus
frequency, from one of two tables chosen by whether the measured bus rate ≥ the measured CPU
rate; the two tables are byte-identical in both shipped ROMs [3][4] (§6). Each record is
{threshold:Hz, 3 high bits, 32-bit low word} [3]:

| Bus clock ≥ (Hz) | High bits | Low word |
|---|---|---|
| 66,666,667 | 2 | $0D532010 |
| 66,000,000 | 2 | $0D532010 |
| 50,000,000 | 2 | $0D8EA260 |
| 40,000,000 | 2 | $0DCBF2A4 |
| 37,500,000 | 6 | $0DCBF2AC |
| 33,333,333 | 6 | $0DC9F3FC |
| 33,000,000 | 6 | $0DC9F3FC |
| 25,000,000 | 6 | $0DC6F3FC |
| 20,000,000 | 6 | $0DC5F3FC |
| 0 | 6 | $0DC5F3FC |

Before writing, the ROM clears bits 24 and 27, and overwrites bits 16–21 with the computed
refresh divider (§3.3) [3]. Production machines land on: 6100/60 (30 MHz) → the ≥ 25 MHz row
with divider 8; 7100/66 (33 MHz) → the ≥ 33 MHz row, divider 9; 8100/80 (40 MHz) → the ≥ 40 MHz
row, divider 11 [3]. The **1995 ROM** changes exactly one row: the ≥ 37.5 MHz entry is replaced
by ≥ 36,000,000 Hz → high bits 2, low word $0DCAF2B4 (divider default 10) — added for the
8100/110's 36.6667 MHz bus, which the 1994 table would have configured with the 33.33 MHz
slow timing [4] (*observed*: the row's default divider matches the formula for 36.67 MHz,
a third confirmation of §3.3).

### 4.5 Bus-ratio calibration

Before any table configuration, the ROM writes the raw test value **$00090000** (timing field
all zero — the power-on state of that field) and times 4096 dependent loads from physical
$00000000 against the decrementer; then writes **$00090100** (bit 8 set) and times again. The
difference, over the add-loop timebase, yields the CPU:bus clock ratio; the bus clock is the
measured CPU clock divided by it [3] ($FFF0366C, $FFF03764). The subtraction order in the ROM
fixes the polarity: the all-zero-field run is the **slow** one [3] — i.e. the HMC's power-on
DRAM timing charges measurably more per load than any programmed configuration, and setting
bit 8 of the timing field speeds loads up (*the hardware claim is inferred from the ROM's
arithmetic plus the documented 2:1 ratios of the 6100/60, 7100/66 and 8100/80*; a model in
which all timing configurations are equally fast measures a zero delta and mis-derives the
bus clock). The contract for the chip: configuration writes are accepted this early, and DRAM
at $0 stays functional under both test values.

### 4.6 Cache probe and power-on cache test

**Early probe** [3] ($FFF039FC–$FFF03AA8) — runs during step 12 of §4.1:

1. Save the configuration; write it back with bit 25 set and bit 27 clear.
2. Enable floating-point; run a store/load body against physical $50000000 (two 8-byte
   constant patterns stored at $50000000, read back at +$10 and +$18), **twice** — once with
   the segment-5 mapping as configured, once with it toggled to overlay the ROM segment —
   and XOR-compare the readbacks.
3. Restore the mapping and the original configuration.

All readbacks equal → "no cache" (this is what an empty socket produces: the sense code reads 0
and the diagnostic window returns constant data); differing readbacks return cache response
bytes that the ROM stores in the boot header. I.e. **with bit 25 set, physical $50000000 is
redirected into a diagnostic window associated with the cache SIMM** (tag/SRAM test path),
overriding the I/O decode for at least $50000000–$5000001F (*mechanism observed; the window's
exact decode and contents are not established* — see §6).

**Power-on cache test** [3] ($FFF0831C–$FFF08504), reached from the POST sequence:

1. Read the configuration; if bits 0–1 read 0, skip (the caller logs the skip marker
   $4C697361 at the diagnostic area at physical $40024).
2. Map one 256 KB region twice: cached at $00300000 and uncached through the RAM-alias at
   $20300000 (§3.1).
3. Set bit 27 (clear bit 28) — cache on.
4. Write $55555555 across 256 KB through the cached mapping (store-with-flush each word),
   loading it through the cache; then write $AAAAAAAA across the same physical range through
   the **uncached alias**, so DRAM no longer matches the cache.
5. Read the cached mapping back: every word must read $55555555 — stale data served by the
   cache proves it is working. All match → pass; any mismatch → fail.
6. Restore the pattern, clear bit 27 — cache off — and restore the mappings.

The test depends on the documented absence of cache snooping (§3.6): the uncached-alias
writes must bypass the cache. A machine with no cache SIMM takes the skip path at step 1 and
never runs the test.

**Final state.** The ROM never leaves the cache enabled: the final configuration write of
§4.1 assembles bits 0–1 from its tables (always 0), so its conditional enable is dead code on
the cold path, and the POST test always re-clears bit 27 [3] (*observed*). The production
cache enable is performed by later, disk-loaded system software through the same serial
sequence (*inferred — unverified*; no such code exists in either ROM image, and which
component performs it on real machines is open, §6).

### 4.7 Warm boot

A restart that re-enters the hardware-init with translation enabled locates the ROM image via
the page tables, copies the init code to RAM, and **reuses the standing bank table instead of
re-probing**; the HMC configuration is not reprogrammed from the tables, and DRAM contents
survive [3] (*observed*). The configuration register therefore lives in the chip across CPU
resets — only power-on clears it.

### 4.8 The checksum-failure path

If the ROM checksum fails, the hardware-init reads the configuration register, **sets bit 33**
(video scan base — §3.7), writes it back, pokes the video DAC to paint a screen indication, then
runs 500 passes of alternating $00000000/$FFFFFFFF longwords over $00000004–$0002FFFF — before
RAM sizing, i.e. with the HMC at power-on configuration — and then continues the normal boot
[3] ($FFF03FFC) (*observed*). Two contracts fall out: DRAM at the bottom of the map is writable
in the reset configuration, and the full 35-bit register must support read-modify-write.

## 5. Quirks & errata

- **One register, bit-serial.** The entire programming interface is one 35-bit shift register
  with a strobe; there is no parallel register file, and the select decode belongs to the AMIC
  (§2.1).
- **Banks move under the CPU.** Writing SIMM_BANK_SIZE relocates the 6100's SIMM banks
  mid-boot; the ROM writes the code and immediately stores its bank records through the new
  map (§3.2). The 7100/8100 banks never move.
- **Aliasing is load-bearing.** The sizing probe measures banks *through* their decode-window
  aliases; a decode without aliasing, or an empty bank that echoes writes, breaks sizing
  (§4.2).
- **Power-on DRAM is the slow configuration.** The all-zero timing field is slower than any
  programmed value; the ROM's bus-ratio measurement depends on that ordering (§4.5).
- **The ID register must work before configuration** — byte reads at $5FFFFFFC precede any
  configuration write (§2.5).
- **The ROM's read-tail bug (1994 and 1995 ROMs).** The second copy of the serial read routine
  self-inserts a register value in its 3-bit tail loop [3] ($FFF084F0–$FFF084FC), so reads
  through the POST pair return garbage for bits 32–34 — and the cache test writes those bits
  back. Bits 32–34 therefore cannot be functionally critical after boot: the machines keep
  running with junk rewritten there (*observed*; the first routine pair is correct).
- **Bits 32–34 beyond bit 33** are otherwise unknown, and nothing may depend on them
  post-boot (previous item).
- **The ROM never enables the L2 cache** — the final write's conditional enable is dead code,
  and the POST test always re-disables it (§4.6). A machine with no cache SIMM satisfies every
  ROM path through the sense code reading 0.
- **The cache test needs the RAM alias.** Even with no cache modeled, the $20300000 ↔ $00300000
  alias must decode — though a cacheless machine skips the test (§4.6).
- **Refresh divider formula** — floor(bus Hz / 3,299,911) − 1, zero clears the field; confirmed
  by three independent table defaults (§3.3).
- **The 1995 ROM's one table change** (the ≥ 36 MHz row) exists because the 1994 table would
  have under-configured the 8100/110's 36.67 MHz bus (§4.4).
- **The checksum-failure path force-sets bit 33** even though it skips full initialization —
  the ROM requires the scan base select whenever it must paint a screen (§4.8).
- **Size granularity and the disable flag**: recorded bank sizes are truncated to 128 KB
  multiples, and bit 0 of a size entry doubles as the diagnostic-boot bank-disable flag (§4.3).
- **Documented vs. decoded L2 sizes**: the Dev Notes document 128–256 KB cache SIMMs [1][2],
  while the ROM decodes sense codes up to 512 KB (§2.3); the 512 KB code's use is unverified.
- **Dev Note 2's discontinuous map vs. the 6100**: [2] Table A-1 gives the 6100's highest RAM
  address as $06FFFFFF, consistent only with fixed windows; the shipped ROM demonstrably packs
  the 6100's banks contiguously after sizing (§3.2), putting 72 MB at $047FFFFF. The ROM's
  behavior is authoritative for machines running it; the appendix presumably describes the
  unconfigured decode (*inferred*).

## 6. Open questions

1. **Power-on value of the full 35-bit register** — unknown; only the behaviors of §2.4 are
   established.
2. **Per-bit semantics of the DRAM timing field** (bits 2–15, 22–23, 26): which bits set
   RAS/CAS lengths and wait states, and why bit 8 in particular accelerates loads.
3. **Refresh divider internals**: what the field counts from, and the divide from the
   ≈ 3.3 MHz prescale to the 15.6 µs cadence.
4. **The bit-25 diagnostic window**: its exact decode extent beyond $50000000–$5000001F and
   its contents (tag/SRAM). The early floating-point probe routine's constants are also valid
   Intel-style flash command values, and an alternative reading of the same routine — as a
   flash-ROM ID probe against the ROM socket — cannot be excluded from the disassembly alone;
   which reading is correct is unresolved.
5. **Flash and larger ROMs**: the write strobe and two spare address lines exist [5], but
   whether any shipped software ever writes ROM space, and what part the socket accepts, is
   unknown.
6. **The cache SIMM sense-pin encoding** for 128/256/512 KB parts (the ROM's decode is known;
   which physical SIMM reads which code is not), and **which disk-loaded component performs
   the production cache enable**.
7. **Bit 24 and bit 28 hardware meaning** — the ROM's use of both is observed (§2.3), but
   what the HMC does with them is not.
8. **Bit 33's scan-base reading** rests on inference (§2.3); direct hardware confirmation is
   lacking.
9. **Decode granularity of the serial port** — which offsets within the $50F4xxxx window strobe
   vs. shift; the behavior of shifts past the 35th bit; and read/write commit semantics
   (immediate vs. latch-on-35th).
10. **Machine-ID register**: the upper halfword's $A55A signature and the meaning of bits
    15–4; whether a 32-bit (non-byte) read returns the full value; which chip decodes
    $5FFFFFFC (it is described here because the boot ROM reads it before and around HMC
    configuration, not because its decode ownership is established).
11. **RAM-alias precedence**: how the $10000000–$3FFFFFFF alias images interact with the fixed
    SIMM bank windows that lie inside that range on the 7100/8100 (§3.1).
12. **The two identical timing tables** and their selector (bus rate ≥ CPU rate) — evidently
    provisioned for a 1:1-ratio variant never shipped; both are byte-identical in both ROMs.
13. **6100/7100 board wiring**: pin-level facts here come from the 8100 schematic; the smaller
    boards are assumed to match.
14. **The code-0 decode of bank 1**: that bank 1 answers at the motherboard top under code 0
    is required by the ROM's bank record (§3.2), but the decode's exact extent and its alias
    phase (offset 0 at $00800000, or at $00000000 with motherboard RAM taking precedence) are
    not observable from the ROM. [2] Table A-1 lists the 6100's highest RAM address as
    $06FFFFFF, which matches neither the packed nor the code-0 map of this ROM.
15. **Workgroup Server 9150**: reported to share the HMC; its ROM dispatch and bank layout are
    outside this evidence set.

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh Computers* (Power Macintosh 6100/60,
   6100/60AV, 7100/66, 7100/66AV, 8100/80, 8100/80AV), Developer Press, March 1994 —
   §"Machine Identification" Table 1-5 p. 10; §"Read-Only Memory" and §"Random-Access Memory"
   p. 14; §"Cache Memory" and §"High-Speed Memory Controller" p. 15; §"Data Path Chips" p. 16;
   §"CPU Bus Arbitration" Table 2-4 p. 20; §"Address Errors" p. 21; §"Physical Memory
   Allocations" Table 2-5 and §"RAM Access" p. 22.
2. Apple Computer, Inc., *Developer Note: Enhanced Power Macintosh Computers* (Power Macintosh
   6100/66, 7100/80, 8100/100, 8100/110), Developer Press, 1994 — Table 1-2 model-specific
   clocks p. 3; §"Revised ROM and ASICs" and the 8100/110 cache discussion; Appendix "RAM SIMMs"
   and "Discontinuous Physical Addressing", Tables A-1 and A-2 pp. 9–10.
3. Power Macintosh 6100/7100/8100 boot ROM, version $077D, header checksum $9FEB69B3 (March
   1994) — PowerPC hardware-init and POST disassembly; cited addresses: init entry $FFF03000,
   machine-ID dispatch $FFF0303C, CPU/bus clock measurement $FFF0366C/$FFF03764, serial write/
   read $FFF03838/$FFF0387C, timing-config $FFF038C0 with tables at $FFF0390C/$FFF03984,
   cache probe $FFF039FC, sizing probe $FFF03B00, bank config $FFF03C78 and packing
   $FFF03CF0–$FFF03D54, checksum-failure path $FFF03FFC, final config $FFF0327C–$FFF03324;
   cache test $FFF0831C–$FFF08504 with second routine pair $FFF08484/$FFF084C4 (read-tail bug
   at $FFF084F0); whole-image scan for $50F4xxxx references.
4. Power Macintosh boot ROM, header checksum $9B7A3AAD (January 1995 revision, shipped with
   the Power Macintosh 7100/80, 8100/100 and 8100/110) — disassembly; timing tables at file
   offsets $30395C/$3039D4, serial routine pairs at $303888/$3038CC/$3084DC/$30851C.
5. Apple Computer, Inc., Power Macintosh 8100 main-logic-board schematics, drawing 051-0333
   rev A — sheet 4 "HMC Memory Controller" (U20), with sheets 5 (data path), 8–9 (SIMM
   sockets), 10 (PDS/cache/ROM connectors), 11 (clocks) and 23 (NuBus controller).
6. Apple Computer, Inc. and Prime Time Freeware, *MkLinux DR3* kernel sources, Power Macintosh
   platform support (frame buffer placed at physical $00100000 with HMC configuration bit 33
   clear), 1997.
