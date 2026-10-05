# The GLUE family — Macintosh SE/30, IIx, IIcx

**Contents:**

1. [Overview & membership](#1-overview--membership) — what the family is, the three machines, the GLUE ASIC's
   job list, the universal ROM, division of labor
2. [Board architecture common to the family](#2-board-architecture-common-to-the-family) — the common board
   plan, the GLUE as memory controller and bus supervisor, clocking, the RAM and ROM subsystems, power-up
3. [Memory map & address decode shared by the family](#3-memory-map--address-decode-shared-by-the-family) —
   the 32-bit map, the 24-bit compatibility map, the I/O window and its strides, VIA addressing, NuBus slot
   space, the SE/30 pseudo-slot and video windows, error paths
4. [Device roster](#4-device-roster) — every part, its window and its page; the shared VIA1 and VIA2 wiring
5. [Interrupt, bus, and clock architecture](#5-interrupt-bus-and-clock-architecture) — IPL levels and the
   GLUE priority encoder, level 1 and level 2 detail, slot dispatch, bus mastership and pseudo-DMA, timing
   contracts
6. [Per-machine index](#6-per-machine-index) — IIx, IIcx, SE/30: what differs from the family baseline
7. [Open questions](#7-open-questions)
- [References](#references)

---

## 1. Overview & membership

### 1.1 What the family is

The GLUE family is Apple's first Motorola MC68030 generation: the **Macintosh SE/30**, the **Macintosh
IIx** and the **Macintosh IIcx**, three machines introduced within a year of each other that share one
general-logic architecture. The family is named for the **GLUE**, the Apple custom integrated circuit that
"performs a variety of logic functions" on all three boards: "It decodes addresses to determine which
device or auxiliary processor is being requested by the main processor… [and] asserts the device-select
signal to the appropriate device" [1] p. 113. The name is Apple's own pun: "based on the acronym GLU for
general logic unit and the fact that the GLUE IC 'glues together' the other chips by providing handshaking
between the I/O devices and the main processor" [1] p. 114, [1] p. 22. It must not be confused with the
smaller **GLU** PAL of the Macintosh SE, which Apple explicitly warns about: "Do not confuse the GLU IC in
the Macintosh SE with the GLUE IC in the Macintosh SE/30 and Macintosh II–family computers" [1] p. 114.

The Guide's summary table of general-logic circuits lists GLUE for exactly this set: "Macintosh SE/30 —
GLUE and video PALs; Macintosh II — GLUE; Macintosh IIx — GLUE; Macintosh IIcx — GLUE" [1] Table 3-7
p. 111. The 68020 Macintosh II is the family's direct ancestor — it carries the same GLUE, the same
address map and the same ROM — but it is a 68020 machine and its story belongs to its own page; every
statement in this document is scoped to the three 68030 boards unless the II is named explicitly. The
family's successors, the Macintosh IIci and later, replace the GLUE with the MDU/RBV pair; where the
RBV inherited part of the GLUE's job, this page says so and cites
[rbv.md](../mdu/rbv.md) §1.2 rather than restating it.

Three machines, one design center:

| | Macintosh IIx | Macintosh IIcx | Macintosh SE/30 |
|---|---|---|---|
| Introduced | 1988 | March 1989 ([iicx.md](iicx.md)) | January 1989 ([se30.md](se30.md)) |
| Form factor | modular desktop, 18.66 in wide [1] p. 26 | modular desktop, 11.9 in wide [1] p. 26 | compact all-in-one [1] p. 22 |
| CPU | MC68030, 15.6672 MHz [1] p. 28 | MC68030, 15.6672 MHz [1] p. 29 | MC68030, 15.6672 MHz [1] p. 22 |
| FPU | MC68882 [1] p. 28 | MC68882 [1] p. 29 | MC68882 [1] p. 22 |
| Memory controller / general logic | GLUE [1] Table 3-7 | GLUE [1] Table 3-7 | GLUE + video PALs [1] Table 3-7, p. 114 |
| Expansion | six NuBus slots $9–$E [1] p. 138 | three NuBus slots $9–$B [1] p. 138 | 120-pin 68030 PDS; pseudo-slots $9–$B [2] p. 351 |
| NuBus controller | NuChip [3] Table 1.2 | NuChip30 [1] p. 29 | none (no NuBus connector) |
| Built-in video | none | none | video PALs emulating a NuBus video card in slot $E [1] pp. 23, 114 |
| ROM | 256 KB, on a ROM SIMM [1] p. 28 | 256 KB in four 512-Kbit ICs, plus a ROM SIMM connector [1] p. 29 | 256 KB, on a ROM SIMM [1] p. 23 |
| Floppy | SWIM + FDHD [1] p. 28 | SWIM + FDHD [1] p. 29 | SWIM + FDHD [1] p. 23 |
| SCSI | NCR 5380 [1] p. 28 | NCR 5380 [1] p. 29 | NCR 5380 [1] p. 23 |
| Power switch | soft power-on from keyboard; hard-wired off switch at the rear [1] pp. 244–245 | lockable rear switch; soft power-on from keyboard [1] pp. 244–245 | hard-wired switch, like the Macintosh SE [1] p. 244 |

Every machine in the table runs the same 256 KB ROM image (§1.4) and shares the address map of
§3, the VIA wiring of §4 and the interrupt architecture of §5. What differs per machine — slot count,
video, power control, identity bits — is the per-machine index (§6).

### 1.2 The GLUE ASIC

The GLUE is a gate array, not a microprocessor: it has no register file of its own that any Apple document
names, and every configuration knob it honors is reached through the two VIAs (§4.2, §4.3) or by driving
its address-decode inputs. Apple's own function list, in full [1] pp. 113–114:

- It decodes addresses to determine which device or auxiliary processor is being requested by the main
  processor, and asserts the device-select signal to the appropriate device.
- It sends acknowledge signals to the main processor that indicate that a device is present and that
  specify the width of that device's data bus.
- It generates the RAM address strobes (/RAS and /CAS) and controls the multiplexers (MUXs) that feed
  addresses to RAM.
- It generates the signals that refresh dynamic RAM.
- It generates the 15.6672 MHz clock used by the main processor.
- It generates the 3.672 MHz clock used by the SCC to control communication rates and by the
  microprocessor in the ADB transceiver.
- It generates the 783.36 kHz clock (the E clock) used to synchronize communications between the VIAs and
  the main processor.
- It monitors data transfers and generates the Bus Error signal to halt the main processor if a transfer
  fails to complete successfully.
- It handles hardware handshaking with the SCSI controller, "making SCSI transfers faster and more secure
  than in the Macintosh Plus."
- It performs an OR operation on the six slot interrupt signals and sends the result to VIA2, which
  generates the processor interrupt.
- It monitors the interrupt lines from the VIAs, the SCC, the power switch (early Macintosh II only), and
  the nonmaskable interrupt switch, assigns a priority to each interrupt, and asserts the appropriate
  interrupt lines to the main processor. "If the GLUE IC receives more than one interrupt at the same time,
  it passes only the highest priority interrupt on to the main processor."

The last three bullets are why this chip is the family's interrupt and bus architecture, not merely its
memory decoder; they are expanded in §2.3, §3.7 and §5. The first RAM-related bullets make the GLUE a
68020-class memory controller in 68030 machines — the direct reason the family cannot perform 68030 burst
cache fills (§2.1).

The chip's Apple part number is not established by any document in this evidence set; the number
344S0602 is current in this tree's machine pages ([iicx.md](iicx.md), [se30.md](se30.md)) but no printed source states it (§7.1).

### 1.3 The universal ROM

"The Macintosh SE/30, Macintosh IIx, and Macintosh IIcx computers all use the same ROM, which has a
startup routine almost identical to that used in the Macintosh II" [1] p. 243. The image is 256 KB, version
$0178, checksum $97221136; the word $0178 sits at offset $8 of the image [4]. The IIcx and SE/30 ROM
images are byte-identical [6], and the same image also boots the 68020 Macintosh II — the ROM is
"universal" precisely in the sense that one image, one Slot Manager and one VIA map serve the whole
II-generation, distinguishing the boards at run time by reading hardware identity bits (§6.4).

The image's first longwords double as the 68030's reset state, because under the ROM overlay address
$00000000 fetches ROM (§2.6): the first longword read at reset is $97221136 — the checksum, consumed as
the initial stack pointer — and the second is $4080002A, the initial PC [6] (*observed*: the reset-vector
longwords dumped from the image; the annotations cross-check instruction-for-instruction in [4]).

### 1.4 Division of labor

What the GLUE does not do is as important as what it does. The split, assembled from the component lists
[1] pp. 22–30, the GLUE function list [1] pp. 113–114 and the VIA function lists [1] pp. 153–157:

| Function | Owner |
|---|---|
| Address decode, device selects, /DSACK generation, bus error | GLUE [1] p. 113 |
| RAM strobes, address multiplexing, refresh, bank A/B select | GLUE, configured through the VIA2 RAM-size bits [1] pp. 113, 166–167 |
| Clock generation (15.6672 MHz, 3.672 MHz, E clock 783.36 kHz) | GLUE [1] pp. 113–114 |
| Interrupt priority encoding onto the 68030's /IPL lines | GLUE [1] p. 114, [3] Figure 2-1 |
| Slot-interrupt ORing (the /SLOTIRQ aggregate) | GLUE [1] pp. 85, 114 |
| Per-slot interrupt capture, SCSI/ASC interrupts, power-off, bus lock | VIA2 [1] pp. 155–156 |
| ADB transceiver interface, RTC interface, ROM overlay control, 60.15 Hz VBL and 1-second interrupt delivery | VIA1 [1] pp. 153–154 |
| NuBus arbitration, slot-space transactions, NuBus error reporting | NuChip (IIx), NuChip30 (IIcx) [1] pp. 27, 29, 139–140 |
| Sound generation and its FIFOs | ASC with two Sony analog ICs [1] pp. 23, 27, 29 |
| Floppy interface | SWIM [1] pp. 23, 28, 29 |
| Serial | Zilog 8530 SCC [1] pp. 23, 27, 29 |
| Real time clock and parameter RAM | custom RTC, bit-banged through VIA1 port B [1] pp. 144–145 |
| Video | NuBus video cards (IIx, IIcx); video PALs emulating a NuBus card in slot $E (SE/30) [1] pp. 23, 114 |

The chip-level documents for everything but the GLUE itself are the device pages cited in §4; the GLUE
has no page of its own, so its full treatment lives here.

## 2. Board architecture common to the family

### 2.1 The common board plan

All three boards are built the same way around the same spine: an MC68030 with a 32-bit external address
and data bus and an on-chip MMU (no external MMU is fitted — the Guide lists the II's AMU/PMMU socket only
for the II [1] pp. 27–28), an MC68882 FPU as a true coprocessor on the same buses [1] p. 128, the GLUE as
sole memory and device controller, and the two Apple-custom 6523 VIAs [3] p. 59 as the I/O catch-all. The
block diagram prose is explicit for the IIx: "very similar in design to the Macintosh II, with the major
exception that the Macintosh IIx has an MC68030 processor," and for the IIcx: "The architecture of the
Macintosh IIcx is identical to that of the Macintosh IIx, with the exception that the Macintosh IIcx has
only three NuBus expansion slots, has only one internal floppy disk connector, and has an external floppy
disk connector" [1] p. 55. The SE/30's logic board is "more closely related to those in a Macintosh IIx"
[1] p. 22 — plus its video PALs and PDS.

Processor-side facts common to all three:

- The 68030's 256-byte instruction cache and 256-byte data cache (16 lines × 4 longwords, write-through
  data cache) are filled in **single-entry mode** — the GLUE "cannot run these burst transfers", so the
  68030's 4-longword cache-fill burst is never used [3] p. 17. "Because memory on the NuBus cannot be
  guaranteed to be coherent with the on-chip cache, NuBus memory is not cached, thus no burst reads are
  done from the NuBus to the 68030" [3] p. 12.
- Every RAM or ROM access costs **one wait state** — "Because the MMU in the MC68030 requires no wait
  states, only one wait state is required for each ROM or RAM access in the Macintosh IIx and Macintosh
  IIcx" [1] p. 138, the same figure as the SE/30's [1] p. 134 note.
- The processor does not share RAM cycles with video or sound: "Except for memory refresh, which takes
  one access cycle every 15.6 µs, the main processors in those computers have uninterrupted access to
  RAM" [1] p. 195. Average RAM access rate is 15.67 MB/s on all three machines [1] Table 5-3 p. 194.
- The processor, like the II before it, "can gain access to the expansion card in the same way that it
  gains access to any of the computer's I/O devices", and an expansion coprocessor takes the bus from the
  68030 with /BR, /BG and /BGACK [2] p. 341 — there is no second arbitration tier.

### 2.2 The GLUE as memory controller

The GLUE drives the RAM array the way the classic Macintosh's PALs did, now on 32-bit data: on a RAM
access "the GLUE decodes this address as a request to address RAM… The RAM address MUXs divide the address
into two 12-bit parts: the row address and the column address. First the row address is fed into the
appropriate bank of RAMs by one of two Row Address Strobe (RAS) signals generated by the GLUE. Then the
column address is fed into the appropriate SIMM by one of four Column Address Strobe (CAS) signals
generated by the GLUE. In this way, up to 128 MB of RAM can be addressed through the 12 RAM address lines"
[1] p. 210. RAM is four or eight 30-pin SIMMs in one or two banks of four; each SIMM carries eight DRAMs
on 12 address and 8 data pins, and a bank of four SIMMs forms the 32-bit data bus [1] p. 210.

Which bank answers which address is firmware-configured, not strapped: the two **RAM-size bits** on VIA2
port A (bit 6 v2RAM0, bit 7 v2RAM1; [3] Table 13.1 names them RAM.SIZ0/RAM.SIZ1) "are set at system
startup by the firmware to indicate the size of the RAM ICs being used in the RAM SIMMs in bank A…
The RAM-size bits determine the physical address at which the GLUE IC stops selecting bank A and starts
selecting bank B" [1] p. 166. The encodings [1] Table 4-10 p. 167:

| v2RAM1 (bit 7) | v2RAM0 (bit 6) | Bank-A DRAM density | Bank A size (4 SIMMs × 8 ICs) |
|---|---|---|---|
| 0 | 0 | 256 Kbit | 1 MB |
| 0 | 1 | 1 Mbit | 4 MB |
| 1 | 0 | 4 Mbit | 16 MB |
| 1 | 1 | 16 Mbit | 64 MB |

The 4-Mbit row of that table is a trap: "The Mac II, IIx, IIcx, and SE/30 do not support 4 Mbit DRAMs,
since refresh cycles on these machines can put these DRAMs into a test mode" [3] p. 20 — a GLUE refresh
timing limitation that is the reason 4 MB SIMMs (eight 4-Mbit ICs) do not work in this family, even though
the VIA encoding for them exists (§7.8). Maximum RAM is 128 MB, at $00000000–$07FFFFFF [1] Table 5-2
p. 193.

Refresh is the GLUE's, and it is the only thing that interrupts the processor's RAM stream: one cycle
every 15.6 µs [1] p. 195. The refresh clocking is driven from the same 15.6672 MHz net that clocks the
processor ([1] p. 113 — the GLUE generates it; the RBV inherited exactly this duty on the successor
machines, [rbv.md](../mdu/rbv.md) §1.2).

### 2.3 The GLUE as bus supervisor: selects, acknowledges, wait states

Every main-logic-board device access is timed by the GLUE, and the acknowledgment carries the port width:
"The GLUE responds to any I/O device address with a /DSACK0 signal. The /DSACK0 and /DSACK1 signals are
used to acknowledge a transaction with a device and to indicate to the MC68030 the size of a device's
data bus" [1] pp. 134, 138. The wait-state schedule, identical for SE/30 [1] p. 134 and IIx/IIcx [1]
p. 138:

| Device / access | GLUE behavior |
|---|---|
| RAM, ROM | one wait state per access [1] pp. 134, 138 |
| SWIM (the II-family section calls it IWM) | one wait state [1] p. 138; one wait state on the SE/30 [1] p. 134 |
| SCSI, no hardware handshaking | one wait state [1] pp. 134, 138 |
| SCSI, hardware handshaking | /DSACK0 held off until the transaction completes [1] pp. 134, 138 (§5.5) |
| ASC reads | two wait states [1] pp. 134, 138 |
| ASC writes | one wait state [1] pp. 134, 138 |
| SCC | 2.2 µs recovery: "in the case of back-to-back accesses to the SCC, the GLUE holds off the second access for that amount of time" [1] pp. 134, 138 |
| VIAs | synchronous access on the E clock — the VIAs "are MC6800-compatible peripheral devices that require synchronous communication with the MC68030" [1] pp. 134, 138 (§5.6) |

A 68030 processor aborts a cycle that no device terminates, and the board enforces a hard ceiling: "an
overriding watchdog timer on the Macintosh SE/30 and the Macintosh IIsi main logic boards … generates a
/BERR signal any time the address strobe is asserted for longer than 44 µs" [2] p. 352. The watchdog's
value on the IIx and IIcx is not separately printed (§7.13). Unassigned addresses are a bus error, not a
silent read: "An access to any address range to which no device is assigned results in a bus error" [1]
p. 135, and the $F0xx xxxx range from the main processor is a deliberate fault (§3.5).

Interrupt acknowledgment is autovectored: "All interrupts to the MC68030 are autovectored using addresses
that contain the interrupt vectors" [2] p. 351, which obliges the GLUE to run the 68030's
interrupt-acknowledge cycle correctly even though nothing will terminate it — "The MC68030 starts an
interrupt acknowledge cycle before it checks the level of the AVEC (autovector) pin. Once the processor
determines the AVEC pin is signaling an autovector, it aborts the bus cycle without the assertion of /DSACK
or /STERM. Hardware designers must be aware of this abort cycle" [2] p. 352. That abort cycle is the
GLUE's business on the main logic board and the PDS card designer's business at the connector; it is a
contract every device on the board lives with.

### 2.4 Clock generation

Three clocks leave the GLUE [1] pp. 113–114:

| Clock | Frequency | Consumers |
|---|---|---|
| Processor/system clock | 15.6672 MHz | the MC68030 (and, per [1] p. 113's wording, the memory-controller and bus-error circuitry generally) |
| SCC/ADB clock | 3.672 MHz | the SCC (communication rates) and the microprocessor in the ADB transceiver |
| E clock | 783.36 kHz | synchronous VIA accesses with the main processor |

On the SE/30's PDS the same clocking is exposed as two nets with a compatibility contract: "There are two
clock signals present on the expansion connector. The CPUCLK signal should be used for signal timing and
synchronization… The C16M signal is a general-purpose 15.6672 MHz clock that will be present in future
machines. In the Macintosh SE/30, these two clocks have the same frequency and phase relationship" [2]
p. 352 — C16M is the machine-independent 15.6672 MHz reference, CPUCLK the per-machine processor clock
(20 MHz on the IIsi that shares this connector definition).

### 2.5 The RAM subsystem

The SIMM rules that the GLUE's two-bank, four-CAS decode imposes [1] pp. 209–211:

- 30-pin SIMMs, identical to the Macintosh Plus/SE socket; each SIMM must hold eight RAM ICs (a
  nine-IC parity SIMM works but the ninth IC is unconnected on these machines).
- RAM ICs must be 120 ns RAS access or faster; all ICs in a bank must have the same access time and the
  same size.
- A bank is empty or full — one, two or three SIMMs in a bank is not a configuration.
- If the two banks hold different densities, the larger ICs go in bank A.
- Legal bank-A densities (with the 4-Mbit exclusion of §2.2) give system sizes from 1 MB (four 256-KB
  SIMMs, bank B empty) to 128 MB; the intermediate 5 MB configuration is bank A 1 MB + bank B 256 KB
  SIMMs [1] Figure 5-11 p. 220.

The RAM sockets are wired per [1] Table 5-6 pp. 212–213 (bank-A and bank-B address buses RAAF11–0 /
RABF11–0, one /RAS per bank, four /CAS lines /CASLL…/CASUU for the four byte lanes, one R/W per bank) —
i.e. the byte-lane CAS organization the GLUE's four column strobes require.

### 2.6 The ROM subsystem and the overlay

ROM is 256 KB on all three machines (§1.1). On the IIx "All ROM in the Macintosh IIx computer is provided
on a ROM SIMM" [1] p. 28; the IIcx ships four 512-Kbit ROM ICs on board plus "a connector for a ROM SIMM"
for upgrades [1] p. 29; the SE/30 carries its ROM on one SIMM [1] p. 23. The ROM SIMM socket is 64-pin and
handles ROMs up to 8 MB [3] p. 20, with signal assignments in [1] Table 5-11 pp. 237–238 (32-bit data,
A0–A22, /ROMOE and an alternate chip select at pin 63; A0/A1 are used only by the IIcx/IIci-style SIMMs —
a footnote that documents that the IIcx's onboard ROM is byte-organized differently from the IIx's [1]
Table 5-11 note p. 237).

In the normal address map ROM occupies $40000000–$4FFFFFFF, mirrored; system software and the boot code
address it at $40800000, the base at which the image is also disassembled [4], [6]. At power-on or reset
the GLUE instead uses the **ROM overlay** map, which "maps addresses from $0000 0000 to $3FFF FFFF to
locations in ROM rather than to RAM. The RAM cannot be addressed at all when the ROM overlay address map is
being used" [1] pp. 131, 136. The overlay is controlled from VIA1: "The startup or Reset handler software
switches from the ROM overlay address map to the normal address map by setting low the Overlay signal from
VIA1" [1] pp. 131, 136; the controlling bit is vOverlay, port A bit 4 [1] Table 4-5 p. 163.

### 2.7 Power-up and startup

The hardware sequence, common to the SE/30 and the whole II family [1] p. 244:

1. One of the Sony sound ICs monitors the board voltages and asserts /RESET until 0.25 second after they
   stabilize.
2. /RESET brings the CPU, the general logic ICs and all internal devices to a known state; the line is
   also available to every expansion slot so cards reset with the machine.
3. The Sony IC releases /RESET; VIA1, reset, holds the Overlay signal high, so the GLUE decodes the ROM
   overlay map — ROM answers at $0000 0000 (and $4000 0000–$4FFF FFFF) — and the 68030 fetches its SSP and
   PC from there.
4. The CPU executes the Reset handler.
5. One of the first instructions drops the Overlay signal, switching the GLUE to the normal map: RAM at
   $0000 0000–$3FFF FFFF, ROM at $4000 0000–$4FFF FFFF.
6. The Reset handler proceeds with the startup described in *Inside Macintosh*.

The ROM's own conduct through those steps is observed instruction-for-instruction [4]: entry at
$4080002A jumps through a high-ROM dispatch ($4083F61C), sets SR = $2700 (supervisor, all interrupts
masked), executes a RESET instruction, clears the 68030 caches and flushes the PMMU, then jumps to the
POST/StartTest engine at $40802A14, which: builds a temporary stack at $2000; sets the VBR to a ROM-resident
exception table at $40802806 so the destructive RAM passes survive; checks for a diagnostic ROM at
$58000000 against the signature $AAAA5555 (jumping to its entry if present); writes $3D to VIA1 DDRB;
clears the overlay bit in VIA1 port A; runs the machine-type and memory-controller detection; verifies the
ROM checksum; and sizes RAM. On return it stores the machine-type byte at low-memory $012F (CPUFlag; value
$02 on an SE/30 boot) and MemTop at $0108, honors the warm-start cookie 'WLSC' at $0CFC (clearing low
memory only on a cold boot), then initializes the peripherals in a fixed order — VIA1 and VIA2
($408006F2), SCC channel A ($408007B4), SCC channel B, SCSI ($4080066C), SWIM ($40800532) — and finally
programs the 68030 PMMU for the 24-bit compatibility map (§3.2). The POST exception table maps every
vector to a stub that records an error code — $FF01 bus error, $FF02 address error, $FF03 illegal, through
$FF0A line-F, $FF10 level-1 and $FF15 level-6 autovector [6] (*observed*: the stub table and codes
disassembled from the image and exercised by the factory diagnostics [5]).

Power control differs per machine and is in §6; the soft power-off signal itself (VIA2 port B bit 2) is
family-wide and described in §4.3.

## 3. Memory map & address decode shared by the family

### 3.1 The 32-bit physical map

"The Macintosh II, Macintosh IIx, and Macintosh IIcx have the same address map" [1] p. 135, and the
SE/30's is the same map with the slot-space roles reassigned (§3.6). The common skeleton, from the address
map sections [1] pp. 130–140 and the card-design summary [2] Table 15-9 p. 342:

| Physical range | Region | Notes |
|---|---|---|
| $00000000–$07FFFFFF | RAM, up to 128 MB | top address per configuration [1] Table 5-2 p. 193 |
| $08000000–$3FFFFFFF | RAM (expansion/undefined) | decoded as RAM in the largest configurations [2] Table 15-9 p. 342 |
| $40000000–$4FFFFFFF | ROM, 256 KB mirrored | canonical base $40800000 (§2.6) |
| $50000000–$50FFFFFF | I/O devices | §3.3; [2] Table 15-9 p. 342 marks $5001 8000–$57FF FFFF "undefined" |
| $58000000–$5FFFFFFF | 68030 Direct Slot expansion (SE/30) | preferred non-pseudo-slot PDS region [2] p. 349; the POST's diagnostic-ROM probe reads $58000000 [4] |
| $60000000–$8FFFFFFF | super slot space, presently unused | [1] Table 3-10 p. 140 |
| $90000000–$EFFFFFFF | NuBus super slot space, slots $9–$E | 256 MB per slot [1] pp. 138, 140 |
| $F0000000–$F0FFFFFF | NuBus slot $0: card access to main logic board | processor access here is a bus error (§3.5) [1] pp. 139–140 |
| $F1000000–$F8FFFFFF | standard slot space, slots $1–$8, presently unused | [1] p. 140 |
| $F9000000–$FEFFFFFF | NuBus standard slot space, slots $9–$E | 16 MB per slot [1] pp. 138, 140 |
| $FF000000–$FFFFFFFF | standard slot space, presently unused | [1] Table 3-10 p. 140 |

"Notice that whereas the 24-bit address map provides only 1 MB of address space for each NuBus slot, the
32-bit address map provides a 16 MB address space plus a 256 MB address space for each slot" [1] p. 136 —
and only the first 1 MB of each card's standard space is reachable in 24-bit mode (§3.2).

Address-decoding responsibility is the GLUE's, down to the device: "Most of these addresses are different
for other Macintosh computers" [1] p. 130 warns, in an Important note repeated for the II-family section
[1] p. 136 — the decode is ROM-version-specific in Apple's own framing, and the addresses in this section
are the $0178-ROM generation's.

### 3.2 The 24-bit compatibility map

When the operating system runs in 24-bit mode, "the memory management unit in the MC68030 is programmed to
ignore the high-order 8 bits from each address coming from the main processor and to translate the
resulting 24-bit address into a 32-bit address for decoding by the GLUE" [1] pp. 130–131, 136. The
translation [1] Table 3-8 p. 131 (identically printed for card designers as [2] Table 15-11 p. 350):

| 24-bit range | 32-bit range |
|---|---|
| $000000–$7FFFFF | $00000000–$007FFFFF (RAM) |
| $800000–$8FFFFF | $40000000–$400FFFFF (ROM) |
| $900000–$9FFFFF | $F9000000–$F90FFFFF |
| $A00000–$AFFFFF | $FA000000–$FA0FFFFF |
| $B00000–$BFFFFF | $FB000000–$FB0FFFFF |
| $C00000–$CFFFFF | $FC000000–$FC0FFFFF |
| $D00000–$DFFFFF | $FD000000–$FD0FFFFF |
| $E00000–$EFFFFF | $FE000000–$FE0FFFFF |
| $F00000–$FFFFFF | $50000000–$500FFFFF (I/O) |

A 24-bit address of the form $sx xxxx, s in $9–$E, therefore reaches the card in slot s; only that slot's
lowest megabyte is visible [1] p. 139. The firmware switches the MMU to 32-bit mode only "when necessary
to access more than a megabyte of address space on an expansion card" under pre-7.0 systems [1] pp. 131,
136; system software 7.0 and A/UX instead use the 68030's paged MMU for full virtual memory [1] pp. 131,
136. The 68030's on-chip MMU can be programmed to perform "the same 24-bit to 32-bit address translation
as done by the AMU in the Macintosh II" [1] p. 136 — the ROM does exactly this at the end of its early
boot (§2.7).

### 3.3 The I/O window

I/O lives at $50000000 in 8 KB ($2000) windows, listed here at their canonical $50F0xxxx addresses — the
base the boot ROM itself uses for every device (*observed*: VIA1 base $50F00000 stored in the low-memory
global at $01D4; SCSI base $50F10000 in $0C00; the SCC base in the globals at $01D8/$01DC [4], [5]). The
printed card-design table gives the same windows at their $5000xxxx mirror addresses and names each
device's register stride [2] Table 15-9 p. 342:

| Window | Device | Register stride | Notes |
|---|---|---|---|
| $50F00000–$50F01FFF | VIA1 | $200 | §3.4; the full 16-register file of the 6523 |
| $50F02000–$50F03FFF | VIA2 | $200 | §3.4 |
| $50F04000–$50F05FFF | SCC | $2 | read and write bases identical on these machines [4]; channel B at base + 2 in the boot init [4] |
| $50F06000–$50F07FFF | SCSI data, handshake pseudo-DMA | — | DRQ-mediated pseudo-DMA (§5.5) [2] Table 15-9 p. 342 |
| $50F10000–$50F11FFF | SCSI registers | $10 | the eight 5380 registers, 16 bytes apart [2] Table 15-9 p. 342; *observed*: the ROM's SCSI Manager drives ICR/MR/TCR/CSR at +$10/+$20/+$30/+$40 from this base [4] |
| $50F12000–$50F13FFF | SCSI data, blind pseudo-DMA | — | no DRQ wait (§5.5) [2] Table 15-9 p. 342 |
| $50F14000–$50F15FFF | ASC (sound) | $200 within +$800 | register file at base + $800 and up; *observed*: the factory diagnostics write and read back every ASC register at $50F14800+ [5] |
| $50F16000–$50F17FFF | SWIM (floppy) | — | [2] Table 15-9 p. 342 |

Everything above $50F17FFF through $57FFFFFF is "undefined" [2] Table 15-9 p. 342 — a card may claim it.
The device block repeats throughout the I/O region on a $20000 stride (*inferred — unverified*: no printed
document states the mirror period, but the ROM's $50F00000-family addresses sit 120 × $20000 into the
region, and both the $5000xxxx (printed) and $50Fxxxxx (observed) spellings reach the same registers;
§7.12).

The GLUE answers every one of these device addresses with /DSACK0 — an 8-bit-port acknowledgment — and
paces them per the wait-state schedule of §2.3. The 68030's dynamic bus sizing then steers each byte
through the device's fixed lane: "On the 68020 and 68030, dynamic bus sizing permits 8 or 16 bit peripheral
devices to be accessed on arbitrary byte or word boundaries" [3] p. 12.

### 3.4 VIA register addressing

Inside each VIA's 8 KB window the sixteen 6523 registers sit on a **512-byte stride** — [2] Table 15-9
p. 342 marks both VIA windows "(x0200)", and the boot ROM and the factory diagnostics both address IFR at
base + $1A00 and IER at base + $1C00 (*observed* [4], [5]):

| Offset | Register | Offset | Register |
|---|---|---|---|
| +$0000 | ORB/IRB — port B data | +$1000 | T2C-L |
| +$0200 | ORA/IRA — port A data (with handshake) | +$1200 | T2C-H |
| +$0400 | DDRB | +$1400 | shift register |
| +$0600 | DDRA | +$1600 | ACR |
| +$0800 | T1C-L | +$1800 | PCR |
| +$0A00 | T1C-H | +$1A00 | IFR |
| +$0C00 | T1L-L | +$1C00 | IER |
| +$0E00 | T1L-H | +$1E00 | ORA — port A data, no handshake |

The 512-byte stride is the wiring, not the chip: a 6522-family VIA selects its registers with four
register-select pins, and the board wires them to address lines A9–A12, so A0–A8 never reach the chip. The
consequence, established in [se30.md](se30.md) §"Byte-Lane Aliasing Within the 8 KB Window" and reasoned
there from the chip's pinout plus the 68030's dynamic bus sizing, is that **every byte address within a
register's 512-byte bank reaches that same register** — $1A00, $1A01, $1A02 and $1A03 are all IFR. The
shipped software takes advantage of it (*observed*: a full A/UX 3.0.1 kernel boot on an SE/30 reads VIA2
IFR at +$1A03 and IER at +$1C13; see [se30.md](se30.md) §"Byte-Lane Aliasing Within the 8 KB Window"), so
an implementation that filters the low address bits fails a real guest. The VIA family's internal register
semantics — handshakes, timers, shift register — are [via.md](../../hardware/via.md)'s subject.

### 3.5 NuBus slot space and the slot-$0 translation window

The IIx and IIcx are NuBus machines: "Accesses to 32-bit addresses in the range $6000 0000 through
$FFFF FFFF (except for $F0xx xxxx) initiate a NuBus transaction. Each NuBus slot is allocated two slot
spaces: 16 MB within the range $F100 0000 through $FFFF FFFF (called the standard slot space), and
256 MB within the range $6000 0000 through $FFFF FFFF (called the super slot space)" [1] p. 138. The
connector population differs: "the Macintosh II and Macintosh IIx have connectors for slots $9 through $E
only, and the Macintosh IIcx has connectors for slots $9 through $B only" [1] p. 138. The bus itself —
arbitration, cycle timing, the /NMRQ interrupt lines, byte lanes — is
[nubus.md](../../hardware/nubus/nubus.md)'s subject (§2.1 for the address space, §3.4 for arbitration,
§3.7 for interrupts, §3.8 for errors and timeouts); the IIx's and IIcx's connectors implement the 1196-1987
generation, with reserved pins A2 and C2 grounded on these machines [3] p. 29.

Slot $0 belongs to the NuBus cards, not the processor. A card that puts $F000 0000–$F07F FFFF on the bus
reaches main logic board I/O ($5000 0000–$507F FFFF), and $F080 0000–$F0FF FFFF reaches ROM
($4080 0000–$40FF FFFF); $0000 0000–$3FFF FFFF on the bus is main logic board RAM [1] p. 139,
Table 3-10 p. 140. The footnote is the sharpest edge in the family's map:

> "A NuBus card can use an address in this range to access the main logic board. If the main processor
> attempts to access addresses in this range, it immediately generates a bus error (/BERR) exception and
> no NuBus transaction takes place." [1] Table 3-10 note p. 140

So $F0xx xxxx is a one-way window: card-to-board only, processor-faulting.

NuBus errors come back to software through VIA2: "If an error occurs during a NuBus access, a Bus Error
signal is sent to the main processor by the NuBus controller (NuChip, NuChip30, or BIU30) and the error type
is sent to VIA2 over these two lines" (/TM0A and /TM1A) [1] pp. 156, 174, decoded per Table 4-16 p. 175
(00 no error, 01 read/write error handled by the bus master, 10 bus timeout, 11 try again later). The
v2BusLk bit on VIA2 port B bit 1 blocks card-to-board transactions entirely when written 0, answering the
card with try-again-later — "That bit can be used to protect time-critical processor activity from NuBus
direct-memory access transactions" [1] p. 175.

### 3.6 The SE/30's pseudo-slot and video windows

The SE/30 has no NuBus connectors, but its PDS reuses the slot architecture twice over.

**Pseudo-slots.** "In order for an expansion card to appear to the firmware as if it occupied one of these
NuBus slots, it must respond to one of the address ranges shown in Table 3-9" [1] p. 134 — the first three
slots' standard spaces, with their 24-bit aliases [1] Table 3-9 p. 134, [2] Table 15-12 p. 351:

| Interrupt line | 32-bit space | 24-bit alias | Presented as |
|---|---|---|---|
| /IRQ1 | $F900 0000–$F9FF FFFF | $900000–$9FFFFF | NuBus slot $9 |
| /IRQ2 | $FA00 0000–$FAFF FFFF | $A00000–$AFFFFF | NuBus slot $A |
| /IRQ3 | $FB00 0000–$FBFF FFFF | $B00000–$BFFFFF | NuBus slot $B |

A pseudo-slot card carries a declaration ROM at the top of its space and is managed by the ordinary Slot
Manager firmware — "the existing Slot Manager ROM firmware controls this card as if it were a NuBus card
but the electrical interface is via the MC68030 bus" [2] p. 349. A card that declines the pseudo-slot
method can instead claim $5800 0000–$5FFF FFFF, the PDS's preferred 32-bit-only region [2] p. 349 — the
same region the boot ROM probes for a diagnostic ROM signature at $58000000 (§2.7).

**Video.** The SE/30's video logic "emulates a NuBus video card installed in NuBus slot $E" [1] p. 23:
the PALs implement the frame buffer controller and declaration-ROM functions of a NuBus video card, minus
a color lookup table [1] p. 114. The frame buffer is 64 KB of dedicated VRAM (two 256-Kbit ICs [1] p. 23)
in "Video RAM space $FE00 0000–$FE00 FFFF" [2] Table 15-9 p. 342, and the 8 KB video declaration ROM
occupies $FEFF 0000–$FEFF FFFF [2] Table 15-9 p. 342 — the upper limit of slot $E's standard space, where
a declaration ROM must sit to be found (§3.5). (A differing VRAM base is in circulation in this tree's SE/30
page; see §7.4.) The video interrupt rides the slot-$E line, and VIA1 port B bit 6 (vSyncEnA) enables it
(§6.3).

### 3.7 Error paths

Three distinct faults, all ending in the same processor exception:

1. **Unassigned addresses.** "An access to any address range to which no device is assigned results in a
   bus error" [1] p. 135 — the empty-slot case the Slot Manager depends on when scanning slot space.
2. **The $F0 window from the processor.** Immediate /BERR, no NuBus transaction [1] Table 3-10 note
   p. 140 (§3.5).
3. **The watchdog.** A device (or PDS card) that asserts address strobe without terminating for more than
   44 µs is killed by the watchdog's /BERR [2] p. 352.

The GLUE generates the Bus Error signal on the board [1] pp. 113–114; the NuChip/NuChip30 generates it for
NuBus-side failures [1] p. 156. The ROM's POST vectors all of these to coded stubs ($FF01 for bus error,
§2.7) so the failure is observable rather than fatal.

## 4. Device roster

### 4.1 The roster

Every device common to the three boards, its window (§3.3) and its page. The VIA chips are Apple's custom
6523: "Starting with the Mac II, an Apple custom version of the VIA, known as the 6523, began to be used.
This chip synchronized the serial port inputs and fixed a few minor logic bugs. In the Mac II, IIx, IIcx,
and SE/30, this chip was used for both the VIA1 and VIA2 functions" [3] p. 59 (specification "6523 Spec,
Apple No. 338S6523" [3] p. 59).

| Device | Part | Window | Page |
|---|---|---|---|
| VIA1 | Apple 6523 | $50F00000 | [via.md](../../hardware/via.md) (chip); wiring §4.2 |
| VIA2 | Apple 6523 | $50F02000 | [via.md](../../hardware/via.md); wiring §4.3 |
| SCC | Zilog 8530 | $50F04000 | [scc.md](../../hardware/scc.md) |
| SCSI controller | NCR 5380 (53C80-compatible) | $50F10000 + DMA windows | [ncr-5380.md](../../hardware/scsi/ncr-5380.md) |
| SWIM floppy controller | Apple SWIM (part number not established in print; §7.1) | $50F16000 | [swim.md](../../hardware/swim.md), [iwm-floppy.md](../../hardware/iwm-floppy.md) |
| ASC sound chip | Apple ASC | $50F14000 | [asc.md](../../hardware/asc.md) |
| Analog sound | two Sony sound ICs | — (also the reset supervisor, §2.7) | [asc.md](../../hardware/asc.md) §15 |
| Real-time clock | Apple custom RTC + battery | via VIA1 port B | [rtc.md](../../hardware/rtc.md) |
| ADB transceiver | Apple custom, with its own microprocessor | via VIA1 CB1/CB2 + shift register | [adb.md](../../hardware/adb.md) |
| NuBus controller | NuChip (IIx) / NuChip30 (IIcx) | slot space | [nubus.md](../../hardware/nubus/nubus.md) §1.4 |
| Video (SE/30) | video PALs + 64 KB VRAM + 8 KB video ROM | $FExxxxxx | [se30.md](se30.md), [video-overview.md](../../hardware/video-overview.md) §3 |
| Keyboard, mouse | ADB devices | — | [keyboard.md](../../hardware/keyboard.md), [mouse.md](../../hardware/mouse.md) |

The SCSI part number is printed as "5380" in the Guide's component lists ([1] pp. 23, 28, 29) and "53C80"
in the Hardware Overview's machine table [3] Table 1.2 — compatible second sources for the same part; the
chip-level behavior is [ncr-5380.md](../../hardware/scsi/ncr-5380.md)'s subject, and this page covers only
the family's wiring of it (§5.5).

### 4.2 VIA1 wiring shared by the family

VIA1 "performs many of the functions of the VIA in one-VIA machines" [1] p. 153. Its family-wide jobs:
overlay control, ADB transceiver interface, RTC interface, the 60.15 Hz (/VBLK) interrupt from VIA2, the
one-second RTC interrupt, the SCC synchronous/asynchronous select, the floppy SEL line, and the SCC
/W/REQ monitor [1] pp. 153–154.

**Port A** [1] Table 4-5 p. 163, with [3] Table 13.1's names:

| Bit | Name | Direction | Function |
|---|---|---|---|
| 7 | vSCCWrReq | in | SCC Wait/Request, both channels wire-ORed; lets the OS keep serial alive with SCC interrupts masked [1] p. 163 |
| 6 | vPage2 / CPU.ID1 | out (SE/30) / in | SE/30: screen-buffer select [1] pp. 163, 154; II/IIx: model-identity line, "tied low in the Macintosh II and the Macintosh IIx" [1] p. 163; IIcx: reads high (§6.4) |
| 5 | vHeadSel | out | floppy state-control line SEL (head select) [1] p. 163 |
| 4 | vOverlay | out | 1 = ROM overlay map; "RAM is inaccessible until the Overlay signal is deasserted" [1] p. 163 |
| 3 | vSync | out | 1 = synchronous modem support on SCC channel A (external receive clock from the GPI pin) [1] p. 163 |
| 2–0 | — | — | reserved; on these machines "either unused or … used only as machine-ID signals" [1] p. 164 — [3] Table 13.1 names PA0–PA2 the burn-in/burn-in-mode straps, and warns that they must carry soft pull-ups "since these bits are programmed as the sound volume level for machines with 'Mac Plus Sound'" [3] p. 60 |

**Port B** [1] Table 4-14 p. 171:

| Bit | Name | Direction | Function |
|---|---|---|---|
| 7 | vSndEnb | out | sound-enable compatibility bit — an output "only to maintain compatibility with the classic Macintosh and Macintosh SE computers" [1] p. 172 |
| 6 | vSyncEnA | out | SE/30 only: enables the slot-$E video interrupt (§6.3); "not used in the other Macintosh II-family computers" [1] pp. 171–172 |
| 5 | vFDesk2 | out | ADB state input 1 (ADB.ST1) [1] p. 172 |
| 4 | vFDesk1 | out | ADB state input 0 (ADB.ST0) [1] p. 173 |
| 3 | vFDBInt | in | ADB interrupt (/ADB.INT): "an ADB transaction is pending" [1] p. 173 |
| 2 | rTCEnb | out | RTC enable (0 = enabled) [1] p. 173 |
| 1 | rTCCLK | out | RTC data clock [1] p. 173 |
| 0 | rTCData | in/out | RTC bidirectional serial data [1] p. 173; the protocol and register set are [rtc.md](../../hardware/rtc.md)'s |

**Control lines and PCR** [1] Table 4-18 p. 177: CA1 carries the 60.15 Hz VBL request (bit 0 of the PCR
governs its edge), CA2 the one-second interrupt, CB1 the ADB clock, CB2 the ADB data line — the ADB
transaction state machine and the shift-register protocol are [adb.md](../../hardware/adb.md)'s subject.
The IFR layout is the stock 6522/6523 one: bit 7 IRQ, 6 T1, 5 T2, 4 CB1/ADB clock, 3 CB2/ADB data,
2 shift register, 1 CA1/VBL, 0 CA2/1-second [1] Table 4-25 p. 185. Apple's standing warning belongs to
every driver: "Do not change any of the bits in the Peripheral Control register" [1] p. 176.

### 4.3 VIA2 wiring shared by the family

VIA2 is the slot/interrupt VIA: it captures the individual slot interrupt lines, receives the GLUE's
aggregate, watches SCSI and ASC, and carries the family's control outputs [1] pp. 155–156.

**Port A** [1] Table 4-9 p. 166:

| Bit | Name | Direction | Function |
|---|---|---|---|
| 7 | v2RAM1 | out | RAM-size bit 1 — bank-A density, drives the GLUE's bank select (§2.2) [1] p. 166 |
| 6 | v2RAM0 | out | RAM-size bit 0 [1] p. 166 |
| 5 | v2IRQ6 | in | interrupt request from expansion slot $E [1] p. 166 |
| 4 | v2IRQ5 | in | slot $D [1] p. 166 |
| 3 | v2IRQ4 | in | slot $C [1] p. 166 |
| 2 | v2IRQ3 | in | slot $B [1] p. 166 |
| 1 | v2IRQ2 | in | slot $A [1] p. 166 |
| 0 | v2IRQ1 | in | slot $9 [1] p. 166 |

On the IIx all six inputs are wired to its six connectors; on the IIcx only bits 2–0 have connectors;
on the SE/30 bits 0–2 are the PDS's three /IRQ lines and bit 5 carries the built-in video's slot-$E
interrupt [1] p. 166. "Also in the Macintosh SE/30 computer, the interrupt for slot $E can be generated by
the video logic circuits on the logic board" [1] p. 166.

**Port B** [1] Table 4-15 p. 174:

| Bit | Name | Direction | Function |
|---|---|---|---|
| 7 | v2VBL | out | driven by timer T1: the 60.15 Hz interrupt request to VIA1, once every 16.63 ms [1] p. 174 |
| 6 | v2SNDEXT | in | 0 = plug in the external sound jack (II family); tied low on the SE/30 so the Sound Manager always operates in stereo [1] p. 174 |
| 5 | v2TM0A | in | NuBus transfer-mode acknowledge 0 — error status, Table 4-16 (§3.5) [1] p. 174 |
| 4 | v2TM1A | in | NuBus transfer-mode acknowledge 1 [1] p. 174 |
| 3 | vFC3 | in/out | Macintosh II only: AMU/PMMU 24/32-bit control; "tied low in the Macintosh SE/30 and … not used in the Macintosh IIx and the Macintosh IIcx" [1] p. 175 — but the ROM reads it as a model-identity bit on the IIcx (§6.4) |
| 2 | v2PowerOff | out | 0 = shut off the power supply [1] p. 175; on the SE/30 the signal reaches the PDS connector only, "so an expansion card can use this signal to determine that it is about to lose power" [1] pp. 156, 175 |
| 1 | v2BusLk | out | 0 = NuBus transactions to the main logic board are locked out, answered try-again-later (§3.5) [1] p. 175 |
| 0 | v2CDis | out | 0 = disable the 68030's instruction and data caches [1] p. 176 — the debugger's cache kill switch |

**Interrupt flags** [1] Table 4-27 p. 186, mapped onto the 6523's CA/CB inputs: bit 7 IRQ, bit 6 T1,
bit 5 T2, bit 4 ASC interrupt (CB1), bit 3 SCSI IRQ (CB2), bit 2 shift register, bit 1 the slot
interrupt (CA1 — the GLUE's /SLOTIRQ aggregate), bit 0 SCSI DRQ (CA2). The enable register follows the
stock set/clear semantics: bit 7 of the written byte selects enable vs disable, "0's in bits 0 through 6
have no effect", and reading IER always returns bit 7 as 1 [1] p. 187.

One Overview caveat travels with this chip set: "The timers in the VIA2 chip are not used, and are not
even implemented in the newer ASIC implementations, so they should never be assumed to be available" [3]
p. 59 — true of the later ASICs, but on these three machines VIA2's T1 is the 60.15 Hz source (§5.6) and
the factory diagnostics exercise VIA2's T2 explicitly [5] (*observed*: the MacTest timer suite runs VIA1
T1/T2 and VIA2 T2 interrupt tests to completion on an SE/30).

## 5. Interrupt, bus, and clock architecture

### 5.1 Interrupt levels and the GLUE priority encoder

The GLUE is the only path to the 68030's interrupt pins: "It monitors the interrupt lines from the VIAs,
the SCC, the power switch (early Macintosh II only), and the nonmaskable interrupt switch, assigns a
priority to each interrupt, and asserts the appropriate interrupt lines to the main processor. If the
GLUE IC receives more than one interrupt at the same time, it passes only the highest priority interrupt
on to the main processor" [1] p. 114. The levels, for every MC68030 machine of the generation [1]
Table 3-5 p. 101, with the Overview's per-machine table [3] Figure 2-1 p. 16 agreeing for "Mac II, IIx,
IIcx, SE/30":

| Level | /IPL2–/IPL0 | Source | Autovector |
|---|---|---|---|
| 1 | 110 | VIA1 | $19 |
| 2 | 101 | VIA2 | $1A |
| 3 | 100 | none | $1B |
| 4 | 011 | SCC | $1C |
| 5 | 010 | none | $1D |
| 6 | 001 | power switch (original Macintosh II hardware only) | $1E |
| 7 | 000 | nonmaskable interrupt switch (the programmer's switch) | $1F |

"As in the current Macintosh II, the power-off switch in an MC68030-based computer is connected directly
to hardware and does not generate an interrupt" [1] p. 101 — the level-6 row survives in the table for the
original II's rear power switch, which "generates a level-6 interrupt to the CPU" [1] p. 245; the IIx, IIcx
and SE/30 have hard-wired rear switches instead [1] p. 245. Levels 3 and 5 are unconnected. All interrupts
are autovectored, with the AVEC abort cycle described in §2.3.

Interrupt-acknowledge masking is the 68030's own: "When the MC68030 is executing a level x interrupt, it
first sets the interrupt mask to level x, so further interrupts at level x and below will be ignored. Once
the interrupt handler is executed and an RTE instruction is processed, the interrupt mask is restored" [2]
p. 351.

### 5.2 Level 1: VIA1

VIA1 aggregates the family's housekeeping interrupts [1] Table 4-25 p. 185: the 60.15 Hz VBL on CA1
(VIA2's T1 output, §5.6), the RTC's one-second tick on CA2, the ADB clock/data/shift-register events on
CB1/CB2/SR, and the VIA's own timers T1 and T2. The boot ROM leaves VIA1 with timer T1 enabled as a
short-interval time source during calibration, then disables it (*observed*: the ROM programs VIA1 T1 with
$03FF-class reloads, arms IER bit 6, and later writes $20-only to IER to disable it again [4]). The
chip-level details of the 6523 timer model are [via.md](../../hardware/via.md)'s.

### 5.3 Level 2: VIA2

VIA2 is where the expansion world, SCSI and sound surface [1] Table 4-27 p. 186. The dispatch order is
fixed by the chip's flag bits: the handler reads IFR, sees bit 1 (the slot aggregate on CA1), bit 0
(SCSI DRQ on CA2), bit 3 (SCSI IRQ on CB2) or bit 4 (ASC on CB1), and clears by the stock write-1
semantics. The boot ROM enables exactly two sources at the end of startup — "VIA2 IER = $82 (enable slot
+ CA1 interrupts)" (*observed* [4]: the ROM's early IER write of $02 then $82 to VIA2 + $1C00) — leaving
SCSI, DRQ and ASC interrupts for the drivers that need them.

### 5.4 Slot interrupt dispatch

The slot path has two tiers, and the GLUE owns the first: "The GLUE IC performs an OR operation on the
three or six interrupt lines from the expansion slots to generate this signal [SLOTIRQ]. When VIA2
receives this signal, it asserts the interrupt line to the processor. [VIA2] monitors the interrupt lines
from the expansion slots (/IRQ1 through /IRQ6) in a register where software can read the state of the
interrupt lines. When the main processor receives a /SLOTIRQ interrupt from VIA2, software reads this
register to find out which slot is generating the interrupt" [1] pp. 85, 155–156.

The software contract, per the card-design book for the SE/30's identical-to-NuBus mechanism: "The first
-level interrupt dispatcher determines which hardware device — SCSI, sound chip, real-time clock, or
expansion slot — is requesting the interrupt and dispatches code to the appropriate interrupt handler. If
the interrupt generated by the VIA is a slot interrupt, the software polls the second VIA, bits PA0
through PA5, to determine which slot generated the interrupt. PA0 is equal to IRQ1, PA1 is equal to IRQ2,
and PA2 is equal to IRQ3. PA5 is equal to the video interrupt. Once the software determines which
pseudoslot generated the interrupt, the Slot Manager software executes the interrupt handler for that slot
device" [2] pp. 351–352 — the same flow the IIx and IIcx run against their real slots, polling VIA2 port A
bits 0–5 for slots $9–$E. The bus-level protocol of the /NMRQ lines is
[nubus.md](../../hardware/nubus/nubus.md) §3.7 and §4.4.

Because dispatch is polled, not latched by hardware, the card must hold its line asserted until serviced:
"There is a delay between the assertion of a slot interrupt and the actual execution of the interrupt
handler. During this time, the software polls the actual slot /IRQ signal. The recommended design practice
is to latch the slot /IRQ signal so that once it is asserted, the interrupt handler software for the card
has the responsibility of clearing the interrupt" [2] p. 352.

### 5.5 Bus mastership and pseudo-DMA

**SCSI pseudo-DMA.** The family's NCR 5380 has no true DMA ("Macintosh models other than the Macintosh IIfx
do not support true direct-memory access by the SCSI controller, but they do support the SCSI controller's
pseudo-DMA mode" [1] p. 392). In a transfer the CPU sets up a block in normal (polled) mode, then carries
the data in pseudo-DMA mode; "Once the data transfer has begun, the SCSI controller uses the DRQ bit in
its Bus and Status register to indicate when it has received a byte from, or written a byte to, the
peripheral device" [1] p. 392. Two data paths exist (§3.3): the **handshake** window at $50F06000, where
the GLUE is in the loop — "the computer's general logic IC… does not complete each byte transfer until the
SCSI controller's DRQ line goes high. This handshaking makes read and write operations safe in pseudo-DMA
mode, even when using the blind transfer method" [1] p. 393 — and the **blind** window at $50F12000,
where after the first byte "the remaining bytes in the block are read or written in pseudo-DMA mode at the
maximum rate possible for the interface" [1] p. 392. The chip-side protocol that makes a blind access safe
(despite no DRQ check) is analyzed in [ncr-5380.md](../../hardware/scsi/ncr-5380.md) §3.8; the family's
contribution is the GLUE's /DSACK gating, identical to its handshake duty in §2.3.

**PDS bus mastership.** An SE/30 expansion coprocessor "requests the bus from the MC68030 using the bus
request signal (/BR). The MC68030 grants the bus (/BG) and tristates itself off the bus at the end of that
bus cycle. The coprocessor then takes over as bus master (/BGACK). At this point the coprocessor has
complete access to all Macintosh SE/30 electronics" — with the GLUE still timing everything: "For all
devices on the Macintosh SE/30 main logic board, the timing of an access is controlled by the GLU gate
array… The gate array also generates the /DSACKx signals to inform the coprocessor of cycle completion" [2]
p. 341. DMA masters must assert the data strobe when accessing main logic board devices, matched to the
68030's own [2] p. 352.

**NuBus mastership.** NuBus cards master the bus through the NuChip/NuChip30 and reach RAM, ROM and (via
slot $0) main logic board I/O per §3.5; the v2BusLk lockout is the processor's defense (§3.5). Cache
coherency is the software's problem: NuBus memory is not cached at all [3] p. 12, and there is no bus
snooping in this generation, so a NuBus master writing system RAM that the 68030 has cached is a hazard
system software must manage.

**Soft power.** VIA2's v2PowerOff bit (§4.3) is the firmware's off switch: "When the software is ready to
turn off power to the computer, it causes a control IC (VIA2 or OSS) to assert the /POWEROFF signal. This
signal causes the power supply to switch off after 2 ms" [1] p. 245.

### 5.6 Timing contracts the devices impose

The E clock (783.36 kHz) is the VIA access clock — MC6800-style synchronous cycles, per §2.3. The SCC
demands 2.2 µs between back-to-back accesses, enforced by the GLUE holding off the second cycle [1]
pp. 134, 138. VIA2's T1, free-running with PB7 output, is the 60.15 Hz source: "The v2VBL bit is driven by
timer T1 to send the 60.15 Hz interrupt request to VIA1 once every 16.63 ms" [1] p. 174, and the boot ROM
loads the latch with $196E (6510 decimal) in T1 free-run mode, sets DDRB bits 7–6, and arms VIA1's CA1
path (*observed* [4]: the T1 latch write pair at $4080074C writes T1C-L = $6E, T1C-H = $19; the enable
sequence at $4080014E writes $02 to VIA2 IER). The count rate implied by 16.63 ms against 6510 is about
391.68 kHz — one half of the E clock — which is consistent with a VIA counting at Φ2 fed at half the E
rate, but no document states the VIA's Φ2 on these boards (§7.5). This chain is what holds the VBL at exactly 60.15 Hz — a rate that "must be maintained for software
 timing compatibility" on machines whose displays scan at other frequencies [3] p. 60 — so the
VIA2-T1 → PB7 → VIA1-CA1 wiring is load-bearing, software-visible behavior, not an internal detail.

## 6. Per-machine index

### 6.1 Macintosh IIx

The IIx is the family's baseline: "The Macintosh IIx computer contains components identical to those in
the Macintosh II except for the following differences" — the 68030 with its on-chip MMU, the 68882 FPU,
the SWIM replacing the IWM with one FDHD drive, and "All ROM in the Macintosh IIx computer is provided on
a ROM SIMM" [1] p. 28. Six NuBus slots, $9 through $E, behind a NuChip [1] pp. 138, [3] Table 1.2. No
built-in video: a monitor needs a NuBus video card. RAM is the family's four-or-eight SIMM arrangement to
128 MB (§2.5). Identity: VIA1 port A bit 6 is tied low [1] p. 163, and with VIA2 port B bit 3 low the ROM's
two-bit scheme (§6.4) reads $00 — the plain IIx, the machine the universal ROM treats as its default
II-family member. Power: pressing the power key on the ADB keyboard discharges a capacitor that switches
the supply on within 2 seconds, and "The capacitor in the Macintosh II and the Macintosh IIx is kept
charged by two 3-volt lithium batteries" [1] p. 244 — the family's only battery-dependent soft power-on;
the rear switch is a hard-wired off switch that "does not initiate the software shut-down procedures" [1]
p. 245. The machine-level page is [iix.md](iix.md).

### 6.2 Macintosh IIcx

The IIcx is the IIx folded into an 11.9-inch case: same architecture, "with the exception that the
Macintosh IIcx has only three NuBus expansion slots, has only one internal floppy disk connector, and has
an external floppy disk connector" [1] pp. 30, 55 — connectors for slots $9–$B only, behind a NuChip30 [1]
pp. 29, 138. ROM ships as four 512-Kbit ICs on the logic board with a ROM SIMM connector for upgrades [1]
p. 29. Identity: VIA1 port A bit 6 reads high and VIA2 port B bit 3 reads high — combined $03 in the ROM's
scheme (§6.4), the value that steers the universal ROM onto the IIcx's three-slot slot tables. Power
control is the family's most developed: the keyboard power-on capacitor is "kept charged by a trickle
current from the power supply" [1] p. 244, the rear switch can be locked on with a screwdriver so "the
machine will turn itself back on after a power interruption" [1] p. 245, and Shut Down with the switch
locked restarts the machine instead of powering it off. The supply itself is specified in the Overview: a
modular unit (spec 699-0392-A) taking 85–270 VAC, delivering up to 12 A at +5 V, 1.5 A at +12 V and 1.0 A
at −12 V for 90 W, with the soft-power-on control line held at +5 V at 1 mA [3] p. 61. The machine-level
page is [iicx.md](iicx.md).

### 6.3 Macintosh SE/30

The SE/30 is the compact member: "The interior of the Macintosh SE/30 computer is similar in appearance to
that of the Macintosh SE, although the components on the logic board in a Macintosh SE/30 are more closely
related to those in a Macintosh IIx" [1] p. 22. On top of the family baseline it adds:

- **Built-in video**, implemented by PALs that "perform the video functions handled by the BBU in the
  Macintosh SE, plus the video functions performed by a NuBus video card in a Macintosh II-family
  computer" — frame buffer controller and declaration-ROM functions, but no color lookup table [1] p. 114
  — emulating a NuBus video card in slot $E [1] p. 23, with 64 KB of VRAM in two 256-Kbit ICs and an 8 KB
  video ROM [1] p. 23 (§3.6). Two screen buffers exist, selected by VIA1 port A bit 6, vPage2 [1] pp. 154,
  163; the video interrupt is the slot-$E line, gated by VIA1 port B bit 6 (vSyncEnA): "When enabled, this
  interrupt request is asserted each time the vertical blanking signal is asserted by the video logic
  circuits" [1] p. 172. The 60.15 Hz VIA2-generated VBL and this true video interrupt are distinct [1]
  p. 172.
- **The 120-pin 68030 processor-direct slot**: "The processor-direct slots in the Macintosh SE/30 and the
  Macintosh IIfx are Euro-DIN 120-pin connectors providing unbuffered access to most of the MC68030
  signals" [1] p. 84, with pseudo-slot addressing and interrupts per §3.6, the /BUSLOCK, /TM0A//TM1A,
  /POWEROFF signals exposed at the connector [1] pp. 156–157, and the electrical rules (44 µs watchdog,
  CPUCLK/C16M, data-strobe discipline for DMA masters) of §2.3 and [2] pp. 341–352.
- **Sound**: "the Macintosh SE/30 sound circuit includes a mixer to convert stereo to mono for the
  internal speaker; thus in the Macintosh SE/30, the Sound Manager always operates in stereo mode" [1]
  pp. 156–157 — which is why v2SNDEXT is tied low on the SE/30 (§4.3). Platform differences among the
  three machines's ASC wiring are tabulated in [asc.md](../../hardware/asc.md) §14.
- **Power**: a hard-wired switch "identical to the one used in the Macintosh SE" [1] p. 244 — no soft
  power-on from the keyboard; the /POWEROFF VIA2 output exists but reaches only the PDS (§4.3).

Identity: VIA1 port A bit 6 reads high (it is vPage2, an output) and VIA2 port B bit 3 is tied low —
combined $02 in the ROM's scheme (§6.4); the boot ROM stores machine-type byte $02 at $012F on an SE/30
boot (*observed* [4]). The machine-level page is [se30.md](se30.md).

### 6.4 The identity bits and how the ROM reads them

The universal ROM tells the boards apart with two readable bits — VIA1 port A bit 6 (the CPU.ID1 position,
Table 4-5) and VIA2 port B bit 3 (the vFC3 position, Table 4-15) — and the machine pages record the
per-board values from the ROM's own flow:

| Machine | VIA1 PA6 | VIA2 PB3 | Combined | Source |
|---|---|---|---|---|
| Macintosh IIx | 0 (tied low [1] p. 163) | 0 | $00 | [iix.md](iix.md), [se30.md](se30.md) §"Machine Identification" |
| Macintosh II | 0 (tied low [1] p. 163) | 1 (vFC3, the AMU control line [1] p. 175) | $01 | [se30.md](se30.md) §"Machine Identification" |
| Macintosh SE/30 | 1 (vPage2 output [1] p. 163) | 0 (tied low [1] p. 175) | $02 | [se30.md](se30.md) §"Machine Identification"; *observed* machine-type byte $02 [4] |
| Macintosh IIcx | 1 | 1 | $03 | [iicx.md](iicx.md) (introduction) |

The printed documents give the bit definitions but not the per-board strap table; the table above is the
ROM-flow record of the machine pages, consistent with the printed bit functions (the SE/30's PB3 tie-down
and the IIx's PA6 tie-down are printed facts; the IIcx's highs and the II's PB3-via-vFC3 reading are
*observed in the ROM's machine-type detection*, [4] — the boot code reads VIA1 PA6 to choose between
device-base layouts before the SCC is even initialized). A further, burn-in-related strap set exists on
VIA1 PA0–PA2 [3] p. 60, and later machines widen the scheme to four CPU.ID bits [1] Table 4-8 p. 165 —
outside this family.

## 7. Open questions

1. **The GLUE's part number.** No document in this evidence set prints one; 344S0602 circulates in this
   tree's machine pages ([iicx.md](iicx.md), [se30.md](se30.md)) without a printed source. A board
   photograph or the chip's spec sheet would settle it.
2. **The GLUE's register surface.** No address window is assigned to the GLUE in any printed map
   ([2] Table 15-9 p. 342 lists none), and its RAM-size inputs arrive over VIA2; but nothing establishes
   that the chip has *no* addressable latch — a bus-analyzer session against a real board would answer
   whether any address reads differently from RAM/ROM/slot space by GLUE decode alone.
3. **The boot ROM's alternate device bases.** The universal image probes $50F18000 before $50F14000 for
   the floppy and $50F16000/$50F1C000 for the SCC (*observed* [4]) — candidates for other machines the
   same ROM boots. Which machine each candidate serves, and why the floppy probe's annotations name
   $50F14000 both "SWIM base" and (in the diagnostics) "ASC base", is unresolved in the annotations; the
   printed $0178-generation map (§3.3) is authoritative for these three boards.
4. **The SE/30's VRAM base.** [2] Table 15-9 p. 342 prints "Video RAM space $FE00 0000–$FE00 FFFF";
   [se30.md](se30.md) §"Physical Memory Map" records $FEE00000–$FEE0FFFF for the same 64 KB. One of the two
   is wrong (or the space is wider than 64 KB and both windows decode). A dump of the SE/30 video driver's
   frame-buffer address from the running system would settle it.
5. **The 60.15 Hz arithmetic.** The observed T1 latch $196E (6510) against the documented 16.63 ms period
   implies the VIA counts at about 391.68 kHz — E/2 — which no document states. The VIA's Φ2 frequency on
   these boards needs a datasheet-plus-schematic confirmation.
6. **VIA2's timers.** [3] p. 59 says the VIA2 timers "are not used" — yet the boot ROM runs VIA2 T1 as the
   60.15 Hz source [1] p. 174, [4] and the factory diagnostics test VIA2 T2 [5]. The Overview statement
   presumably describes the later ASIC machines; confirm it is not also true of some II-family board
   revision.
7. **The SCC low-memory globals.** The boot ROM stores the SCC base in the globals at $01D8 and $01DC
   (*observed* [4]), which the machine pages name SCCWr/SCCRd; the two annotation sets of the same image
   disagree about which global is the read base and which the write base (§7 of the diagnostics notes it
   both ways). The assignment needs a decisive low-memory reference or a trace.
8. **4 Mbit DRAMs.** [3] p. 20 excludes them (refresh puts the parts into test mode), yet the same book's
   machine table lists "4M" among the IIx/IIcx SIMM options, and [1] Table 4-10 p. 167 defines the VIA
   encoding for them. The exclusion is taken as fact here; the table's column meaning (SIMM size vs IC
   density) is the likely reconciliation but is unverified.
9. **The IIx's dates and Gestalt value.** The corpus gives no IIx introduction date and no Gestalt machine
   type; the IIcx's (6) and SE/30's (9) are recorded in [iicx.md](iicx.md) and [se30.md](se30.md) (their introductions).
   Fill the IIx column from a printed Apple source when one is in the library.
10. **The IIcx's NuBus controller.** [1] p. 29 says NuChip30; [3] Table 1.2 says "NUCHIP" for the IIcx.
    Either the Overview abbreviates or the boards varied; a IIcx board photograph would settle it.
11. **The watchdog on the IIx and IIcx.** The 44 µs figure is printed for the SE/30 and IIsi boards [2]
    p. 352; whether the IIx and IIcx share the value is assumed, not established (§2.3).
12. **The I/O mirror stride.** The $20000 repetition of the device block (§3.3) is inferred from the
    ROM's $50Fxxxxx addressing against the printed $5000xxxx windows; no printed document states the
    stride or the region's exact bounds.
13. **Slot-interrupt behavior with no card seated.** Whether an empty IIx/IIcx slot's VIA2 port A bit
    reads a defined level (pulled up) or floats is not printed; the Slot Manager's empty-slot path (a
    recoverable bus error on the declaration-ROM read) is documented, but the port bit's idle level is
    not.
14. **The $0178 ROM's II compatibility edges.** The identity-bit table (§6.4) includes the II, whose PB3
    is the AMU control line vFC3 [1] p. 175 — an output on that board. How the universal ROM's read of an
    output pin yields a stable $01 on real II hardware is not established from print.
15. **PDS signals on the IIx/IIcx.** The SE/30's PDS exposes /BUSLOCK, /TM0A//TM1A and /POWEROFF at the
    connector [1] pp. 156–157; nothing states whether any equivalent signals reach the IIx/IIcx NuBus
    connectors beyond the standard NuBus set ([nubus.md](../../hardware/nubus/nubus.md) §3.1).

## References

1. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition, Addison-Wesley
   Publishing Company, 1990 — Chapter 1 "Introduction to the Macintosh Hardware": §"Macintosh SE/30
   computer" pp. 22–23, §"Macintosh II and Macintosh IIx computers" pp. 26–28, §"Macintosh IIcx and
   IIci computers" pp. 29–31; Chapter 2 "Architecture of the Macintosh Computers": §"Macintosh II family"
   p. 55, Figures 2-4/2-7 pp. 53, 57, §"Processor-direct expansion interfaces" and NuBus interface
   pp. 84–86; Chapter 3 "Processors and General Logic": §"MC68030 interrupts" and Table 3-5 p. 101,
   §"General logic circuits" Table 3-7 p. 111, §"GLUE in the Macintosh SE/30, Macintosh II, and
   Macintosh IIx computers" pp. 113–114, §"Video PALs in the Macintosh SE/30 computer" pp. 114–115,
   §"Address map for the Macintosh SE/30 computer" pp. 130–135 (Tables 3-8, 3-9), §"Address map for the
   Macintosh II, Macintosh IIx, and Macintosh IIcx computers" pp. 135–140 (Table 3-10), §"Real-time
   clock (RTC)" pp. 144–146; Chapter 4 "Versatile Interface Adapter (VIA) ICs": §"VIA functions in the
   Macintosh SE/30 and Macintosh II-family computers" pp. 153–157, Table 4-5 p. 163, Table 4-9 p. 166,
   Table 4-10 p. 167, Table 4-14 p. 171, Table 4-15 p. 174, Table 4-16 p. 175, Table 4-18 p. 177,
   Tables 4-25/4-27 pp. 185–186, §"Interrupt Enable register" p. 187; Chapter 5 "Memory": Table 5-2
   p. 193, Table 5-3 p. 194, §"RAM access rate in the Macintosh SE/30, Macintosh II, Macintosh IIx, and
   Macintosh IIcx computers" p. 195, §"RAM configuration in the Macintosh SE/30 and Macintosh II-family
   computers" pp. 209–213 (Table 5-6), Figure 5-11 p. 220, §"ROM SIMMs" pp. 235–238 (Table 5-11);
   Chapter 6 "Power Supplies": §"Power up, system startup, and power down" and §"System startup"
   pp. 241–245; Chapter 11 "SCSI": §"SCSI data transfers" pp. 392–393.

2. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third edition,
   Addison-Wesley Publishing Company, 1992 — Chapter 15 "Electrical Design Guide for 68030 Direct Slot
   Expansion Cards": §"Memory and I/O access from a Macintosh SE/30 expansion card" pp. 340–341,
   Table 15-9 "Macintosh SE/30 32-bit physical address spaces" p. 342, §"Pseudoslot design guidelines"
   pp. 349–350 (Table 15-11), Table 15-12 p. 351, §"Interrupt handling for the Macintosh SE/30 and the
   Macintosh IIsi 68030 Direct Slot" pp. 351–352, §"Design hints for Macintosh SE/30 and Macintosh IIsi
   expansion cards" p. 352 (watchdog, CPUCLK/C16M, data strobe).

3. Apple Computer, Inc., *Macintosh Hardware Overview*, Revision 2, February 11, 1991 — §"Processor"
   pp. 11–12 (dynamic bus sizing; burst exclusion); Figure 2-1 "Macintosh interrupt priorities" and the
   cache discussion p. 16; Table 1.2 "Desktop Macintoshes" (front matter; GLUE/NUCHIP/SWIM/53C80 columns
   for II, IIx, IIcx); §"3. Memory" pp. 18–20 (4-Mbit DRAM exclusion p. 20, 64-pin ROM SIMMs up to 8 MB
   p. 19); §"4. System Expansion" p. 29 (NuBus reserved pins grounded); §"VIA functions" pp. 59–60 (6523
   history and spec numbers, PA0–PA2 burn-in straps, 60.15 Hz compatibility, VIA2 timers); Appendix "VIA
   Bits", Tables 13.1–13.3 pp. 63–66 (per-machine VIA pin assignments); §"Power supply & control"
   pp. 61–62 (IIcx supply 699-0392-A, soft power-on).

4. Macintosh IIx / IIcx / SE/30 universal boot ROM, version $0178, checksum $97221136, 256 KB image
   (based at $40800000) — annotated disassembly of the SE/30 image: reset entry $4080002A and
   high-ROM dispatch $4083F61C; POST/StartTest at $40802A14 (temporary stack $2000, VBR $40802806,
   diagnostic-ROM probe $58000000/$AAAA5555, VIA1 DDRB $3D, overlay clear, machine-type and
   memory-controller detection, ROM checksum); peripheral init sequence VIA1/VIA2 $408006F2, SCC
   $408007B4/$408006AA, SCSI $4080066C, SWIM $40800532; low-memory globals VIA1 base $01D4, SCC bases
   $01D8/$01DC, SCSI base $0C00, machine type $012F, MemTop $0108, warm-start cookie $0CFC; VIA2 T1
   setup $4080074C (latch $196E, free-run) with the VIA1 enable writes $50F02000+$1C00; the SCSI Manager
   dispatch and register offsets +$10/+$20/+$30/+$40 from $50F10000 at $408266B8 ff.

5. Apple Computer, Inc., *MacTest SE/30*, Version F1.0, 1989 (factory diagnostics, 800 KB disk image) —
   disassembly and recorded behavior: VIA base addresses and 512-byte register stride; VIA1/VIA2 timer
   test suite; ASC register-space access at $50F14800+; the ROM POST RAM-test invocation and its
   exception-code conventions.

6. Macintosh IIcx/IIci factory diagnostics disk with the ROM POST path — annotated disassembly of the
   universal ROM ($97221136; the IIcx ROM image is byte-identical to the SE/30's): reset-vector longwords
   at $40800000 ($97221136 SSP, $4080002A PC), the ROM exception vector table at $40802806, the
   $FF01–$FF15 POST error stubs, and Phase 7's vector-table restoration to RAM.

