# PSC — the Peripheral Subsystem Controller

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, division of labor, the two
   buses, clocking
2. [Register file](#2-register-file) — the $50F31000 window: level 3–6 interrupt registers, the UTSC, the DMA
   channel control and register-set arrays, the sound/DSP block, and the VIA1 and VIA2 windows inside the chip
3. [Behaviour](#3-behaviour) — the interrupt architecture and dispatch, the DMA engine's double-buffer
   execution, I/O-bus arbitration, the UTSC, the sound engine
4. [Programming model](#4-programming-model) — boot-time initialization, interrupt installation, and the
   sequences the shipped SCSI, floppy, serial, Ethernet, sound and DSP code actually perform
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The **Peripheral Subsystem Controller (PSC)**, Apple part **343S1100**, is "a CMOS chip in a 208-pin
package" [1] p. 13 and the heart of the AV Quadra platform: the one custom IC through which every
non-video peripheral of the Macintosh Quadra 840AV and Macintosh Centris 660AV reaches the
processor. Apple's own function list for the part is short and complete:

- it "provides nine dedicated DMA channels"
- it "decodes the I/O memory mapping"
- it "handles all internal system interrupts"
- it "handles system interrupts from the Versatile Interface Adapter (VIA) inputs"
- it "contains 16-byte buffers for sound data to and from the Singer sound encoder and decoder
  (**codec**)" [1] p. 13

In fuller form: "The PSC contains nine programmable DMA channels that transfer data between
random-access memory and various I/O interfaces. The PSC also performs address decoding for many
I/O memory allocations" [1] p. 29. The nine channels "can access RAM or ROM but cannot access the
I/O or NuBus address spaces" [1] p. 29 — the PSC moves data *between RAM and its own I/O side*,
never within the I/O region itself.

The PSC is thus four things at once, and the rest of this page is organized around them:

- an **interrupt controller** — it drives the 68040's `/IPL(0–2)` interrupt priority lines directly
  ([1] Table 3-22 p. 49 calls them "Interrupt priority lines from the PSC; not to be used as
  wire-OR lines; can be monitored by a PDS card"), supplies the level-by-level device interrupt
  registers of §2.2, and hosts the pseudo-VIA1 and pseudo-VIA2 windows of §2.8;
- a **DMA controller** — seven general-purpose channels (SCSI, Ethernet receive and transmit,
  floppy, both SCC serial ports plus a second port-A channel) plus the two Singer sound channels,
  each general channel with two register sets for gapless double buffering (§3.2);
- a **bus bridge** — the PSC is one of the four masters of the CPU bus ("Four chips are able to
  take control of the CPU bus: the main processor, the MUNI, the PSC, and the DSP" [1] p. 18) and
  the sole master of the 16-bit I/O bus, which it arbitrates between itself and the main processor
  (§3.3); and
- the **address decoder for the whole I/O region** — the I/O bus itself "contain[s] five address
  lines and 16 data lines" [1] p. 18, so every peripheral register visible to software is decoded
  by the PSC first; the devices see only register-select lines and the 16 data bits.

The part number and the platform are locked together: this PSC generation exists on the two AV
main logic boards and nowhere else in the Macintosh line. Its successor in the PowerPC era,
[Grand Central](../tnt/grand-central.md), inherits the role — whole-machine interrupt collection
plus DMA for every device — but not the register design; its predecessor in role on the 68k
Quadras was the discrete SCSI-DMA-plus-VIA2 arrangement the PSC absorbs (see [AMIC](../pdm/amic.md)
for the PDM generation's parallel solution).

### 1.2 Machines that carry it

| Machine | Apple codename | Processor | PSC clocks in at | Notes |
|---|---|---|---|---|
| Macintosh Quadra 840AV | Cyclone (33 and 40 MHz builds) | 68040 at 40 MHz (SysEnvirons 78) [1] p. 8 | BClk 40.0000 MHz | three NuBus cards [1] p. 39, through the MUNI [1] p. 14 |
| Macintosh Centris 660AV | Tempest (25 and 33 MHz builds) | 68040 at 25 MHz (SysEnvirons 60) [1] p. 8 | BClk 25.0000 MHz | one NuBus card; NuBus (and MUNI) optional, on the PDS adapter card [1] pp. 18–19, 39 |

Both boards are one design with speed variants: the two models "contain essentially the same
circuit board and system components, with variations as noted" [1] p. 10. The boot ROM carries four
per-variant records for the pair — the four AV Ethernet-configuration resources, named "Cyclone33",
"Cyclone40", "Tempest25" and "Tempest33", are byte-identical in content, evidence that no
PSC-visible difference exists between the speed variants [3] (ROM offsets $1AEE30, $1AEE90,
$1AEEF0, $1AEF50). The 660AV's PDS connector carries the machine's bus-master capability
constraint on its face: the Centris 660AV "does not support using an accessory card as a bus master
in addition to the existing bus masters (the processor, the DSP, the PSC, and the MUNI)" [1] p. 49.
See the machine pages [Quadra 840AV](q840av.md) and [Centris 660AV](q660av.md) and the family
overview [AV](av.md) for everything board-level.

### 1.3 Division of labor

The PSC is a controller of controllers; most devices are separate ICs it serves rather than
functions it contains. The split, from the developer note's chip inventory [1] pp. 13–16:

| Function | Owner |
|---|---|
| All system interrupts; `/IPL(0–2)` to the 68040; the VIA1 and VIA2 *functions* (§2.8) | the PSC |
| DMA for SCSI, Ethernet, floppy, serial, sound input and sound output (§2.5–§2.7, §3.2) | the PSC — nine channels |
| I/O-region address decode; the 16-bit, 5-address-line I/O bus (§3.3) | the PSC |
| CPU-bus arbitration between processor, PSC, [MUNI](av.md) and DSP; DRAM/ROM control; bus timeout (16 µs / 32 µs) | the MCA [1] pp. 13, 18–19 |
| NuBus | the MUNI [1] p. 14 |
| Ethernet MAC, the external SCSI controller (53C96) and the SCC serial controller | the **Curio** IC — "a multipurpose I/O chip that contains a Media Access Controller for Ethernet (MACE), a SCSI controller, and a Serial Communications Controller (SCC)", with 8-byte FIFOs on both SCC data streams [1] p. 16 — all reached across the PSC's I/O bus and all DMA'd by PSC channels; see [MACE](mace.md) |
| Floppy (SuperDrive, GCR and DOS-format MFM) | the **New Age** controller, "based upon Industry Standard 765" [1] p. 5, at $50F2A000; see [New Age](new-age.md) |
| Sound codec | the **Singer** codec, serially attached to the PSC's dedicated sound-DMA engine (§3.5); see [Singer](singer.md) |
| DSP host control (reset, frame-overrun) | the PSC's `$21C` register (§2.7); the DSP3210 itself and its program interface — [DSP3210](../../hardware/dsp3210.md), [DSP board](dsp3210-board.md) |
| ADB, PRAM, real-time clock, power-on, soft power | the **Cuda** microcontroller, transported through the PSC's VIA1 window — [Cuda](cuda.md) |
| Graphics and video-in framebuffers, video timing | CIVIC, Sebastian and the video-in chain [1] pp. 14, 16 — their *interrupts* still arrive through the PSC's VIA2 window (§2.8) |
| Memory-map strapping, machine identity | the YMCA decoder [av.md](av.md) |

Note the consequence of the first row: on this platform there is no other interrupt path. Video,
NuBus, slots, the DSP — every device in the machine reaches the 68040 through the PSC, and "all
internal system interrupts" is meant literally [1] p. 13.

### 1.4 Clocking

The PSC is a multi-clock part: it runs its CPU-bus side at the bus clock, synthesizes the I/O-bus
clocks from a 31.3344 MHz oscillator, and consumes the audio-rate clocks supplied by the ATECS
telecom codec section. The clock inventory, from Table 2-2 [1] p. 17:

| Clock | Frequency (MHz) | Direction | Purpose |
|---|---|---|---|
| BClk | 40.0000 / 25.0000 | into the PSC (from the system divider) | the CPU-bus side: "The clock rate for determining cycle times is BClk for the main processor, the MUNI, and the PSC" [1] p. 20 |
| C32M | 31.3344 | into the PSC (oscillator) | the PSC's I/O-side synthesizer source |
| C16M | 15.6672 | **out of** the PSC | feeds New Age and Curio — the SCC's PCLK and the floppy controller's clock |
| C22_5792M | 22.5792 | **out of** the PSC | feeds Singer — 44.1 kHz sample-rate clocking |
| C24_576M | 24.5760 | **out of** the PSC | feeds Singer — 48 kHz sample-rate clocking |
| C45_1584M | 45.1584 | into the PSC (from ATECS) | the PSC's 44.1 kHz-rate reference |
| C49_1520M | 49.1520 | into the PSC (from ATECS) | the PSC's 48 kHz-rate reference |

So software never supplies a sample-rate clock to the sound system: it selects between the two
fixed rate families through the sound control register's rate field (§2.7), and the PSC has already
distributed the matching clock to the codec. The 15.6672 MHz C16M output is likewise load-bearing
far from sound: it is the frequency the serial hardware assumes for its high-speed clocking option
(§4.5). When a PSC channel masters the CPU bus, its DRAM cycle times are the "MUNI and PSC" column
of Table 2-3 — e.g. a single read costs 5 BClk cycles at 40 MHz and 4 at 25 MHz [1] p. 20.

## 2. Register file

### 2.1 Base address and how software finds it

Every PSC register lives behind the fixed base **$50F31000**. The address is a constant of the
platform, but shipped software almost never hard-codes it: the ROM and every driver recover it at
runtime from the machine's decoder-information record. The idiom is verbatim in the ROM's
interrupt dispatchers [3] ($40810834–$4081083A):

```
MOVEA.L $0DD8,A0        ; low-memory pointer to the product information record
ADDA.L  (A0),A0         ; + DecoderInfo pointer
MOVEA.L $6C(A0),A5      ; DecoderInfo + $6C = the PSC base ($50F31000)
```

The PSC occupies **slot 27** of the decoder record (byte offset +$6C of the base-address table),
and the same table carries a second entry that aliases it: slot 33, labeled for the DSP, contains
$50F31000 as well, because the DSP3210's one host-visible register lives inside the PSC window
(§2.7) [3]. The record also carries a per-device validity bit that start-up code tests before
first use; the PSC's bit is set on every AV machine [3]. Drivers are free to ignore the table —
the ROM's own floppy driver hard-codes both of its channel-3 aliases (`$50F31C30` and
`$50F32060`) [3] ($4098EC56, $4098EC70), and the ROM's DSP driver hard-codes the base twice — but
the table is what makes the same ROM image run on every machine of the family.

The PSC window sits inside the I/O region $50F00000–$50F3FFFF; the family page ([AV](av.md))
documents the region's alias at $50F40000 and higher (the same I/O space, mapped non-serialized)
(*inferred — unverified* for the PSC window specifically; the alias is established for the region
as a whole). The full software-visible file, offsets from $50F31000:

| Offset | Width | Name | Function |
|---|---|---|---|
| +$130 | byte | L3IR | level 3 interrupt register (read status / write acknowledge) |
| +$134 | byte | L3IER | level 3 interrupt enable register |
| +$140 | byte | L4IR | level 4 interrupt register |
| +$144 | byte | L4IER | level 4 interrupt enable register |
| +$150 | byte | L5IR | level 5 interrupt register |
| +$154 | byte | L5IER | level 5 interrupt enable register |
| +$160 | byte | L6IR | level 6 interrupt register |
| +$164 | byte | L6IER | level 6 interrupt enable register |
| +$200–+$21C | mixed | sound/DSP block | the Singer sound engine and the DSP host register (§2.7) |
| +$300 | long | LSUTSC | Universal Time-Stamp Counter, low 32 bits |
| +$304 | long | MSUTSC | UTSC, high 16 bits valid (upper 16 read as zero) |
| +$400 | byte | PSCTEST | test register; no shipped software touches it (§6) |
| +$800 | byte | BERRIE | bus-error interrupt enable (bit 0) |
| +$804 | long | PSC_ISR | DMA interrupt status (§2.4) |
| +$C00 + ch×$10 | word | channel control | one per DMA channel, 16-byte stride (§2.5) |
| +$1000 + ch×$20 | 16 bytes | channel register set 0 | {Addr, Cnt, CmdStat} per channel (§2.6) |
| +$1010 + ch×$20 | 16 bytes | channel register set 1 | the second set (§2.6) |

Two further windows belong to the PSC but have their own base addresses and their own decoder
slots: the VIA1 function at **$50F00000** and the VIA2 function at **$50F02000** (§2.8). Whether
the chip decodes anything between the named offsets — and whether the named registers alias
within their stride — is not established (§6).

### 2.2 The device interrupt registers (levels 3–6)

Four pairs of byte-wide registers hold the PSC's own device-interrupt state, one pair per 68040
interrupt level from 3 to 6. In each pair the IR is the live status of that level's request lines
and the IER the enable mask. The bit assignments, identical by bit number in IR and IER:

| Level | IR / IER | Bit 0 | Bit 1 | Bit 2 | Bit 3 | Bits 4–6 | Bit 7 |
|---|---|---|---|---|---|---|---|
| 3 | $130 / $134 | MACE — the Ethernet chip's interrupt | — | — | — | — | L3B7 |
| 4 | $140 / $144 | SNDSTAT — Singer codec status | SCCA — serial port A | SCCB — serial port B | DMA — any DMA channel's completion | — | L4B7 |
| 5 | $150 / $154 | DSP — DSP3210 to host | FRMOVRN — sound/DSP frame overrun | — | — | — | L5B7 |
| 6 | $160 / $164 | 60 Hz — a periodic timer | SCCA | SCCB | — | — | L6B7 |

The dispatchers confirm the per-level masks in the shipped ROM: the level-3 dispatcher tests bit 0
only (`ANDI.B #$01`), level 4 tests bits 3–0 (`#$0F`), level 5 bits 1–0 (`#$03`), level 6 bits 2–0
(`#$07`) [3] ($408109C0, $40810900, $408108A0, $40810830). The two SCC bits at level 6 exist in
hardware — the mask proves the dispatcher reads them — but no shipped software ever enables them:
serial interrupts are taken at level 4 (§4.5).

**Bit 7 is the level's sense bit and serves both polarities.** Read from an IR, bit 7 reports the
OR of all pending request bits on that level. Written to an IER, bit 7 is a VIA-style sense bit:
1 means "set the enable bits written as 1", 0 means "clear them". The ROM's own initialization
demonstrates the set polarity [3] ($40810A38):

```
MOVE.B #$86,$144(A0)    ; L4IER := SENSE | SCCB | SCCA
```

and the shipped drivers demonstrate both: the serial HAL enables port A's serial and DMA bits with
`$8A` to $144 and disables port A alone with `$02` (sense clear) [4] (resource offsets $0266,
$04A8); the DSP driver enables the level-5 DSP and frame-overrun bits with `$83` to $154, disables
them with `$01`/`$02`, and — importantly — **acknowledges** the level-5 DSP flag by writing `$01` to
the *IR* at $150 [3]. The IR is therefore not a read-only port: writing it with the sense bit clear
clears the named flag bits, and the sound path uses the same trick at level 4, where the ROM's sound
driver reads L4IR, masks to bit 0 and writes the value back to acknowledge the codec status
interrupt [3]. This sense-bit convention (bit 7 of byte registers, bit 15 of word registers —
§2.5, §2.6) recurs throughout the PSC's register file.

One further behavior is pinned by the dispatch code itself: **an IR read can be unstable while a
request arrives, and every dispatcher reads the IR twice until two reads agree** (§3.1). A model
whose IR values are not repeat-consistent across back-to-back reads hangs the ROM in a two-
instruction compare loop.

### 2.3 The Universal Time-Stamp Counter

At +$300/+$304 lives a free-running 48-bit counter, the **UTSC**: low longword at $300, high
longword at $304 with only the low 16 bits valid (the upper 16 read as zero) [3] [4]. It exists to
give drivers a monotonic time reference at arbitrary interrupt level, and it is the only counter
of its kind on the platform: no shipped software uses it for wall-clock time, only for coarse
timeouts. The serial HAL reads it through a helper that loads both longwords twice with `MOVEM.L`
and retries until the two samples agree — the same repeat-consistency contract as the IRs — and
returns bits 16–47 as a millisecond-scale value [4] (resource offset $1BE0). Two call sites use it:
a transmit drain timeout (WaitAllSent) and a counter that damps CTS interrupt storms by refusing
to count transitions that arrive within one UTSC sample of each other [4] (offsets $0D10, $17CA).
Its tick rate is nowhere documented (§6).

### 2.4 Test, bus-error and DMA status registers

Three registers complete the control block:

- **+$400, PSCTEST** — a test register whose function is unknown; no shipped code in the ROM or the
  Enabler touches it, and it is the only named PSC register of which that is true [3] [4].
- **+$800, BERRIE** — the bus-error interrupt enable. Bit 0 globally enables the PSC's bus-error
  interrupt path; per-channel bus errors are additionally latched in the channel's control register
  (§2.5, §3.2). No shipped software writes $800, so the entire bus-error interrupt path is
  unexercised by the system software this page's evidence covers — drivers poll the channel
  register instead [3] [4].
- **+$804, PSC_ISR** — the DMA interrupt status register, read as one longword. Only the **top
  byte** is defined: bit 7 of the byte at $804 (bit 31 of the longword) is channel 0's request,
  bit 6 channel 1's, and so on down to bit 0 = channel 7:

| Byte bit at $804 | Longword bit | Channel | Device |
|---|---|---|---|
| 7 | 31 | 0 | SCSI (highest priority) |
| 6 | 30 | 1 | Ethernet receive |
| 5 | 29 | 2 | Ethernet transmit |
| 4 | 28 | 3 | Floppy |
| 3 | 27 | 4 | SCC port A receive |
| 2 | 26 | 5 | SCC port B transmit |
| 1 | 25 | 6 | SCC port A transmit |
| 0 | 24 | 7 | unassigned |

The channel-number-to-bit mapping is fixed by the dispatch code, which scans the byte from bit 7
downward with a 68k `BFFFO` (§3.1); bits 23–0 of the longword are never tested and their read
values are unknown (§6). Channel 7 has no register set in the channel file (§2.6) and no interrupt
slot; whether the status bit exists in silicon is unknown.

### 2.5 The DMA channel control registers

The seven general DMA channels are addressed by two interleaved arrays. The **control array** is
one **word** per channel with a 16-byte stride, at **+$C00 + channel × $10**:

| Ch | Device | Control register |
|---|---|---|
| 0 | SCSI (53C96 via Curio) | $50F31C00 |
| 1 | Ethernet receive (MACE) | $50F31C10 |
| 2 | Ethernet transmit (MACE) | $50F31C20 |
| 3 | Floppy (New Age) | $50F31C30 |
| 4 | SCC port A receive | $50F31C40 |
| 5 | SCC port B transmit | $50F31C50 |
| 6 | SCC port A transmit | $50F31C60 |

The word's bits:

| Bit | Name | Meaning |
|---|---|---|
| 0 | active set | read-only: which register set (0 or 1) the engine is currently running |
| 8 | CIRQ | channel interrupt request — the OR of both sets' interrupt flags |
| 9 | DMAFLUSH | write 1 (with sense) to flush the channel FIFO; **self-clearing** — writers poll it back to 0 |
| 10 | PAUSE | write 1 (with sense) to pause the channel, 0 to release it |
| 11 | SWRESET | software reset of the channel (§3.2) |
| 12 | CIE | channel interrupt enable — the per-channel gate that feeds PSC_ISR |
| 13 | BERR | a bus error occurred on this channel; bit 0 then names the offending set |
| 14 | FROZEN | read-only: the channel has actually stopped after a pause — polled, never written |
| 15 | SENSE | write sense bit: 1 = set the written bits, 0 = clear them |

The bit numbering and the sense convention are confirmed by the shipped code that exercises them:
the ROM's floppy driver writes `$8400` to pause (sense + PAUSE) and `$0400` to release (PAUSE with
sense clear), spins on bit 14 while pausing, and reads bit 0 to find the active set [3]
($4098EC56–$4098EC8A); the serial HAL writes `$9000`/`$1000` for CIE, `$8800` for SWRESET, and
`$8200` for a flush, polling bit 9 until it self-clears [4] (resource offsets $09B0, $0A30, $0AC0).

### 2.6 The channel register sets

The **register-set array** gives each channel **two independent 16-byte sets**, sets 16 bytes
apart and channels 32 bytes apart, at **+$1000 + channel × $20 + set × $10**:

| Ch | Device | Set 0: Addr / Cnt / CmdStat | Set 1: Addr / Cnt / CmdStat |
|---|---|---|---|
| 0 | SCSI | $1000 / $1004 / $1008 | $1010 / $1014 / $1018 |
| 1 | Ethernet receive | $1020 / $1024 / $1028 | $1030 / $1034 / $1038 |
| 2 | Ethernet transmit | $1040 / $1044 / $1048 | $1050 / $1054 / $1058 |
| 3 | Floppy | $1060 / $1064 / $1068 | $1070 / $1074 / $1078 |
| 4 | SCC port A receive | $1080 / $1084 / $1088 | $1090 / $1094 / $1098 |
| 5 | SCC port B transmit | $10A0 / $10A4 / $10A8 | $10B0 / $10B4 / $10B8 |
| 6 | SCC port A transmit | $10C0 / $10C4 / $10C8 | $10D0 / $10D4 / $10D8 |

Each set is:

| Field | Width | Meaning |
|---|---|---|
| Addr | long | the **32-bit physical** address of the buffer — never a logical address; every driver resolves physical addresses before arming |
| Cnt | long | the byte count. The register is **17 bits wide**: the serial HAL caps every transfer at $1FFFC, and the Ethernet driver masks the count to $1FFFF [3] [4] |
| CmdStat | word | command/status, below |

CmdStat bits:

| Bit | Name | Meaning |
|---|---|---|
| 0–7 | SETMASK | a set-indication mask; defined in the register layout but never used by any shipped software (§6) |
| 8 | IF | interrupt flag — set by hardware when the set completes; cleared by writing it with the sense bit clear |
| 9 | DIR | direction: **1 = device-to-memory** (read in), 0 = memory-to-device |
| 10 | TERMCNT | terminal count reached — the set completed normally |
| 11 | ENABLED | arm bit: written 1 (with sense) to arm the set; **hardware clears it when the set completes** |
| 12 | IE | this set's interrupt enable |
| 15 | SENSE | write sense bit, as everywhere in the file |

The observed arm words of the shipped drivers pin the bit arithmetic. A floppy read arms its set
with **$8A00** = SENSE | ENABLED | DIR [3] ($4098EC80); a serial receive arms with **$9A00** =
SENSE | IE | ENABLED | DIR, a serial transmit with **$9800** = SENSE | IE | ENABLED (DIR cleared by
a preceding `$0200` write with sense clear) [4] (resource offsets $0948–$099A); an Ethernet
transmit frame arms with SENSE | IE | ENABLED and the interrupt flag cleared first [3]. Note that
the floppy's arm word deliberately omits IE — the floppy driver never takes channel interrupts
except for bus-error recovery (§4.4).

### 2.7 The sound and DSP block (+$200–+$21C)

The PSC's eighth and ninth DMA channels are not part of the general file: sound input and
sound output run on a **dedicated double-buffer engine** behind its own register block, which also
carries the DSP3210's single host-visible register. From [3] and [Singer](singer.md):

| Offset | Width | Name | Function |
|---|---|---|---|
| +$200 | word | sndComCtl | sound and communications control: bits 0–1 input subframe select, bits 2–5 output subframe select (a per-subframe mask), bit 6 frame-interrupt enable, bit 7 sound-input DMA enable, bit 8 sound-output DMA enable, bits 9–10 sample rate (0 = 24.0 kHz, 1 = 32.0 kHz, 2 = 48.0 kHz), bits 11–12 communications-codec rate (never written by shipped code) |
| +$204 | long | singerCtl | codec control image — attenuation, ports, mute; unmute is software's job |
| +$208 | long | singerStat | codec status image, read-only |
| +$20C | long | sndPhase | the DMA play position, read-only; the boot chime spin-waits on it (§4.7) |
| +$210 | long | sndInBase | sound-input double-buffer base |
| +$214 | long | sndOutBase | sound-output double-buffer base |
| +$218 | word | sndSize | half-buffer size **in sample frames** (4 bytes each), not bytes |
| +$21C | byte | dspOverRun | the DSP3210 host register: reset and frame-overrun latch |

The sound engine is described in §3.5; the codec-side semantics, the serial frame bus and the
rate table belong to [Singer](singer.md). The DSP register obeys the byte-register sense
convention: bit 7 sets or clears the named bits, with bit 0 `pdspReset`, bit 1 `pdspResetEn` and
bit 2 the frame-overrun latch. Every observed write in the ROM's DSP driver fits: `$83` asserts
reset (reset-enable + reset), `$01` releases the DSP to run, `$81` halts it again, `$04` clears
the frame-overrun latch, and `$82`/`$02` set and clear reset-enable for sleep and wake [3]. The
write with bit 7 clear and no data bits (`$00`) is the neutral form; note that a write of `$01`
*clears* the reset bit — the sense bit, not the value, decides polarity.

### 2.8 The VIA1 and VIA2 windows

The AV machines have no 6522 chips. Both classic VIA functions are register windows inside the
PSC, at their own base addresses and decoder slots — the developer note lists `gestaltVIA1Addr`
(`'via1'`) and `gestaltVIA2Addr` (`'via2'`) among the machine's identification selectors [1] p. 8
— and the ROM drives them exactly as it drove the discrete parts [3].

**VIA1 at $50F00000** is a complete 6522: the ROM programs both data-direction registers, both
buffer registers, the peripheral control register and the auxiliary control register, and it uses
both timers and the shift register — the Cuda transport runs over the shift register, and the
start-up code reads the PRAM signature through it before most other hardware is touched [3] (ROM
offsets $AD340–$AD4A0). The register positions follow the Macintosh family's spaced layout:

```
$0000 vBufB   $0400 vDirB   $0600 vDirA   $0800 vT1C   $0A00 vT1CH
$1000 vT2C    $1200 vT2CH   $1400 vSR     $1600 vACR   $1800 vPCR
$1A00 vIFR    $1C00 vIER    $1E00 vBufA
```

with the classic 6522 semantics beneath ([VIA](../../hardware/via.md); [2] pp. 155–157,
186). The AV-specific port wiring, established by the ROM's initialization values and the
drivers that read them [3] [4]:

- **Port A, bit 3 (output)** — disables the external SCC baud-rate clock option.
- **Port A, bits 4 and 6 (outputs)** — the SCC DMA request-term enables: with serial DMA active,
  the SCC's request lines belong to the PSC, and the serial HAL saves these two bits, clears them
  for the life of its channels and restores them on close [4] (resource offsets $026C–$027A).
  Port A bit 7 reads the combined (masked) request state; on the shipping ROM's port assignments
  bit 7 reads the OR of the two request terms, each of which can be masked off by bits 4/6
  (*inferred — unverified*: the HAL's save/clear/restore of bits 4/6 is observed, the bit-7
  combining rule is not).
- **Port A bits 0/1** — read at power-on self-test as a burn-in jumper.
- **Port B bits 3–5** — the Cuda handshake: bit 3 the transaction request (input), bits 4 and 5
  byte-acknowledge and transaction-in-progress (outputs, both idle-high) [3]; the transport is
  [Cuda](cuda.md)'s.
- **PCR/ACR** — CA1 on a negative edge (the 60 Hz-class retrace line), CB1 and CB2 on negative
  edges (the Cuda clock and data), the shift register enabled for Cuda [3].

The VIA1 timer interrupts (60 Hz tick, one-second tick from Cuda, and the timers themselves) are
level 1 — the classic model [2] p. 145; the ROM's level-1 vector points at the VIA1 window [3].

**VIA2 at $50F02000** is *not* a 6522. Only three registers exist, at the positions of the classic
IFR/IER/BufA trio:

| Offset | Absolute | Name | Function |
|---|---|---|---|
| +$1A00 | $50F03A00 | flags/status | the window's flag register — with a twist, below |
| +$1C00 | $50F03C00 | vIER | interrupt enables, classic sense bit 7 |
| +$1E00 | $50F03E00 | vSInt | the slot/status byte — not a classic VIA register |

and the ROM's initialization path writes the IER [3] (ROM offsets $AD360, $AD4AC). The window is
the AV machine's slot-and-device interrupt bank in the RBV tradition ([2] pp. 157, 186): a status
byte of request lines read directly, plus enable registers for the sources that need masking.
Known bit assignments, each anchored in shipped code:

| Bit | Source | Evidence |
|---|---|---|
| 0 | the SCSI controller's **DMA request line** — the pseudo-DMA handshake | the SCSI library's DREQ handshake reads this address, and this register position is the one the library's own equate names for it [3] |
| 1 | NuBus/slot interrupt aggregate | the slot-interrupt dispatch path [3] (*inferred — unverified* at the bit level) |
| 2 | MUNI | [3] (*inferred — unverified* at the bit level) |
| 3 | external SCSI controller IRQ | the SCSI interrupt dispatch path [3] (*inferred — unverified* at the bit level) |
| 5 | New Age floppy interrupt | the floppy driver enables it with `$A0` (sense | bit 5), disables it with `$20`, and polls the flag here [3] ($4098EC92) |
| 6 | sound frame interrupt | the sound block's frame-interrupt enable lands here at level 2 [3] |

Bit 0's double role is the register's defining quirk: on the RBV machines whose layout this window
inherits, the VIA2 flag register's bit 0 was wired to the SCSI DMA request, and the AV window
keeps that wiring — the compatibility SCSI code reads the register as a DREQ status byte while
the floppy driver reads bit 5 as an interrupt flag [3]. Which of the register's bits accept the
classic write-1-to-clear acknowledgement, and which are live line images the device must drop,
is not established (§6); the level-2 handlers that service this window do not blindly clear it.

The vSInt status byte carries the slot request lines **active low** — slots $C, $D, $E plus the
on-board video vertical blanking under a mask of $78 — and the level-2 dispatch inverts them
before dispatching; it does **not** write the IFR to acknowledge, unlike the level-2 handlers of
some other families [3] (*observed* in the ROM's slot-interrupt dispatch; the exact bit-to-slot
mapping is *inferred*). Development-stage Cyclone boards carried the three VIA2 registers at
offsets $000/$004/$008 instead, identified by a different CPU-ID value (*inferred — unverified*;
no shipping board is affected). All VIA2-sourced interrupts are level 2; all PSC-register-sourced
device interrupts are levels 3–6; the split is total (§3.1).

### 2.9 Reset state

The power-on values of every latch in the file are unknown. The ROM initializes the VIA1 and VIA2
windows and writes one PSC interrupt-enable register (§4.1), but no shipped software ever reads
back an IR, an IER, a control register or a CmdStat to observe a reset value, so nothing in the
observed software contract constrains them (§6). What is pinned is the engine's *post-completion*
behavior: hardware clears a set's ENABLED bit, sets IF and TERMCNT, and switches the active set
(§3.2) — the register file's only documented self-modifying behavior.

## 3. Behaviour

### 3.1 The interrupt architecture

The PSC drives the 68040's three `/IPL` lines and thereby presents the whole machine — NuBus
slots, video, DSP, every peripheral — as the classic seven-level 68k autovector model. The
developer note pins the electrical side: the lines are "from the PSC; not to be used as wire-OR
lines; can be monitored by a PDS card" [1] Table 3-22 p. 49. The level-to-source map, as the
shipped ROM implements it [3]:

| IPL | Window | Sources |
|---|---|---|
| 1 | VIA1 ($50F00000) | the 6522's own: timers 1 and 2, CA1 (the 60 Hz-class retrace tick), the shift register and CB1/CB2 (the Cuda transport), the one-second tick from Cuda [2] p. 145 |
| 2 | VIA2 ($50F02000) | external SCSI IRQ, the slot interrupt aggregate (slots $C/$D/$E and on-board video vertical blanking, read from vSInt), MUNI, the New Age floppy interrupt, the sound frame interrupt (§2.8) |
| 3 | L3IR/$130 | the MACE Ethernet chip |
| 4 | L4IR/$140 | Singer codec status, SCC port A, SCC port B, and **any DMA channel completion** — one shared bit that fans out through PSC_ISR |
| 5 | L5IR/$150 | DSP3210 to host, sound/DSP frame overrun |
| 6 | L6IR/$160 | the periodic 60 Hz-class timer, SCC port A, SCC port B |
| 7 | — | NMI (not PSC-sourced) |

The split between levels 2 and 3–6 is structural: devices whose classic-Mac ancestors interrupted
through VIA2 (SCSI, floppy, slots, sound *frame*) still do, through the pseudo-VIA2 window;
devices new to the platform (Ethernet at level 3, DMA and codec status at 4, DSP at 5) use the
PSC's own registers. A driver writer who assumes "the PSC handles all interrupts" means "all
interrupts are in the PSC register file" misses the entire level-2 bank.

The dispatchers themselves are compact, and their shape is the contract. The level-6 handler,
verbatim from the ROM [3] ($40810830):

```
MOVEM.L D5/A4-A5,-(A7)
MOVEA.L $0DD8,A0            ; product info
ADDA.L  (A0),A0
MOVEA.L $6C(A0),A5          ; A5 = PSC base
MOVEA.L $02B6,A4           ; ExpandMem
MOVEA.L $210(A4),A4         ; A4 = the interrupt handler table
@rd:  MOVE.B  L6IR(A5),D5      ; read the IR ...
      CMP.B   L6IR(A5),D5      ; ... twice ...
      BNE.S   @rd              ; ... until two reads agree
      ANDI.B  #$07,D5          ; bits 2..0
      AND.B   L6IER(A5),D5     ; mask with the enables
      BEQ.S   done             ; nothing pending: return
```

Three contracts fall out, all of them load-bearing:

1. **Read the IR twice, until two reads agree.** All four level dispatchers and the DMA dispatcher
   do this unconditionally [3] (ROM offsets $10846, $108B6, $10916, $109D2, $10988). It is a
   silicon metastability workaround: a request arriving between the fetch and the mask must not
   yield a half-updated status. Hardware whose IR reads are not repeat-consistent hangs the ROM
   in the compare loop.
2. **The pending mask is `IR AND IER`, per level.** There is no higher-level mask register: to
   silence a device, clear its IER bit (sense-0 write) or its channel's CIE; there is no other
   switch.
3. **The handler table is a heap block reached through low memory.** Dispatchers fetch it as
   `ExpandMem+$210` [3] ($40810842); its layout is fixed and uniform (below).

Within a level, the dispatch order is fixed. Level 4 services **SCCA first, then SCCB, then codec
status, then DMA**, and loops back to re-read L4IR after servicing [3] [4]; level 5 services the
DSP bit then the frame-overrun bit; level 6 services the timer then the two SCC bits. Each source's
slot in the handler table is 8 bytes — a handler pointer and a parameter pointer — and the handler
is entered with the parameter in A1 [3]. The full layout as the shipped software fills it:

| Offset | Slot | Filled by | Purpose |
|---|---|---|---|
| +$00 | level 3 MACE | the ROM's Ethernet driver | MACE chip interrupt service |
| +$08 | level 4 codec status | the sound stack | Singer status interrupt |
| +$10 | level 4 SCCA | serial HAL (saves the previous occupant) | SCC port A secondary dispatch |
| +$18 | level 4 SCCB | serial HAL (ditto) | SCC port B secondary dispatch |
| +$20 | level 5 DSP | the DSP stack | DSP3210 host interrupt |
| +$28 | level 5 frame overrun | the sound/DSP stack | frame overrun |
| +$30 | level 6 timer | RAM system software | the periodic timer |
| +$38/+$40 | level 6 SCCA/SCCB | nobody in shipped software | unassigned |
| +$48–+$78 | DMA channels 0–6 | per driver (below) | one 8-byte slot per channel, in channel order |
| +$80 | deferred level-2 sound handler | the sound stack | the sound emulation shim (below) |

The DMA slots sit at +$48 exactly, indexed by channel number times 8 — the DMA dispatcher
computes `slot = $48 + channel × 8` from the PSC_ISR scan [3] ($40810984).

The DMA fan-out is its own dispatcher, entered whenever level 4's shared DMA bit is pending. It is
the cleanest statement of the PSC's channel priority — strict, by bit position, SCSI highest [3]
($40810980):

```
@rd:  MOVE.L  PSC_ISR(A5),D5
      CMP.L   PSC_ISR(A5),D5
      BNE.S   @rd                  ; double-read, as everywhere
      BFFFO   D5{#0:#8},D4         ; find the first set bit in bits 31..24
      BEQ.S   done                 ; no channel pending
      MOVE.L  0(A6,D4.W*8),D0      ; the channel's handler
      BEQ.S   next                 ; slot empty: skip
      MOVEA.L D0,A0
      MOVEA.L 4(A6,D4.W*8),A1      ; the channel's parameter, in A1
      JSR    (A0)
next: BFCLR  D5{D4:#1}              ; clear the bit in the working copy
      BRA.S  @scan                  ; scan again — the working copy, not the register
```

Two details in that loop are the whole interface. The dispatcher scans bits 31→24 of the
longword — the byte at $804, MSB first — so **channel number = 7 − bit position**, and channel 0
(SCSI) always wins. And it never re-reads PSC_ISR inside the loop: the caller (the level-4
dispatcher) re-reads L4IR after the DMA pass, so a new completion that arrives mid-dispatch is
caught by the *level* loop, not the channel loop [3].

One structural extra completes the architecture: the **deferred level-2 sound shim** in the
table's last slot. The sound system's interrupt code historically ran at level 2 (the Apple Sound
Chip's interrupt was a VIA2 affair); on the AV machines the codec's status interrupt arrives at
level 4 and the DSP's at level 5. To keep RAM-based sound code seeing the level it was written
for, the level-4/5 dispatch epilogue checks the interrupted processor level and, when the return
would drop to level 0 or 1, temporarily lowers the interrupt level to 2 and calls the +$80
handler directly, before the system's deferred-task machinery runs [3] (*observed* in the
dispatch epilogue; the shipped sound components in System Enabler 088 are its only installers).

### 3.2 The DMA engine

Each of the seven general channels is the same engine with different wiring. Apple's own summary
of the per-channel characteristics [1] Table 2-10 p. 29:

| Channel | I/O-bus width | FIFO depth | Serves |
|---|---|---|---|
| 0 — SCSI | 16 bits | 16 bytes | the external SCSI port (53C96) |
| 1 — Ethernet receive | 16 bits | 16 bytes | MACE receive |
| 2 — Ethernet transmit | 16 bits | 16 bytes | MACE transmit |
| 3 — FDC | 8 bits | 4 bytes | New Age floppy |
| 4 — SCC A | 8 bits | 4 bytes | SCC channel A receive (GeoPort) |
| 5 — SCC B | 8 bits | 4 bytes | SCC channel B transmit |
| 6 — SCC A Tx | 8 bits | 4 bytes | SCC channel A transmit |
| sound in / sound out | 1 bit | 16 bytes | the Singer codec, on the dedicated engine (§3.5) |

A channel runs a set — a physical address and a byte count in one of its two register sets — when
that set's ENABLED bit is set and the channel is not paused. The engine's complete behavioral
contract, as pinned by shipped drivers [3] [4]:

- **Arming**: write Addr (a physical address), Cnt, then the CmdStat arm word. The direction bit
  must be set (device-to-memory) or cleared (memory-to-device) with its own sense write; the
  interrupt flag is cleared with a sense-0 write of IF before arming so that a stale flag from a
  previous transfer does not fire on the new one.
- **Execution**: the channel moves data between its I/O-side device and RAM, decrementing Cnt as
  it goes. Cnt is live: mid-transfer it reads as the residual count, and the serial HAL's
  special-condition handler decides which register set holds a corrupt byte by comparing the
  *other* set's Cnt against its primed value [4].
- **Completion**: when the count reaches zero, hardware sets the set's TERMCNT and IF, clears its
  ENABLED, raises the channel's CIRQ, sets the channel's bit in PSC_ISR (if CIE is set, feeding
  the level-4 DMA gateway bit), and **switches the active set** — the control register's bit 0 now
  names the other set. If the other set is armed, the channel continues with it without software
  intervention; this is the double-buffer pattern's whole value, and Apple says so: "Using two
  register sets helps software optimize data transfers to and from physical memory and increases
  the limit of system interrupt latency when the data input is continuous (such as from
  Ethernet)" [1] p. 29.
- **Pause and freeze**: a sense-1 PAUSE write stops the channel at a clean boundary and the
  FROZEN bit then reads 1. Every driver that pauses spins on FROZEN before touching the set
  registers [3] ($4098EC60) [4] ($0A60) — a channel that never asserts FROZEN hangs its driver
  on the first flow-control event.
- **Flush**: a sense-1 DMAFLUSH write drains (receive) or discards (transmit) the channel FIFO and
  **self-clears**; drivers poll the bit back to zero [4] ($0AC0). On a receive channel the flush
  completes the set — bytes in the FIFO reach memory and the set's completion status is raised —
  which is the only way to make buffered-but-undelivered receive data visible to software
  (*inferred*: the serial HAL's flush path issues only the flush and expects the data to arrive
  through the subsequent completion interrupt [4]).
- **Software reset**: a sense-1 SWRESET write resets the channel: both sets' ENABLED bits clear,
  and the channel is left **paused** — every driver that uses SWRESET follows it with an explicit
  un-pause [4] ($02D0, §4.5).
- **Bus errors**: a CPU-bus error during a channel transfer sets the control register's BERR bit
  and records which set was running in bit 0; the PSC's own bus-error interrupt (BERRIE, §2.4) is
  the optional notification, unused by shipped software. Recovery is always the same: SWRESET,
  reinitialize, re-arm, un-pause [3] [4].
- **Reach**: the channels "can access RAM or ROM but cannot access the I/O or NuBus address
  spaces" [1] p. 29. Addresses are physical and 32-bit; buffers must be locked and physically
  contiguous — "In a virtual memory environment, software must guarantee that memory pages are
  contiguous when DMA transfers controlled by the PSC cross a page boundary" [1] p. 29 — and the
  shipped serial stack additionally aligns every serial buffer to a cache-line boundary
  (§4.5). The shipped SCSI library refuses client buffers at or above $40000000 — ROM, NuBus and
  slot space — and copies through a locked RAM buffer instead; whether that boundary reflects a
  PSC limitation or driver policy is not documented (*inferred — unverified*, see §6).
- **Counting**: Cnt is 17 bits (§2.6). On channel 1 only, the Ethernet driver uses a different
  counting model: Addr points at a chain of 2048-byte receive buffers and **Cnt counts buffers,
  not bytes** — the channel decrements it by one per completed buffer and the driver computes
  occupancy as the primed limit minus the current count [3] (*observed* in the ROM's Ethernet
  driver; the interpretation of the on-the-wire behavior as a hardware chain mode is *inferred*).

When the MCA grants the PSC the CPU bus for a channel beat, the PSC is the master: its DRAM
access cycles are the "MUNI and PSC" columns of the developer note's timing table [1] p. 20. The
MCA also polices it: "If the CPU, PSC, or MUNI does not respond to a cycle start signal within a
critical time, the MCA terminates that chip's bus control and issues a bus error signal" — 32 µs
in NuBus address space, 16 µs elsewhere [1] p. 19.

### 3.3 I/O-bus arbitration

On the I/O side the PSC is the arbiter, and its rule is asymmetric in the DMA's favor: "At the
first level of arbitration, the PSC grants its DMA channels two accesses for every one access
granted to the main processor" [1] p. 29. Among contending channels, priority is fixed and
different on the two sides of the chip [1] Table 2-11 p. 30:

| Priority | To the I/O bus | To the CPU bus |
|---|---|---|
| highest | FDC | SndOut |
| | SCCA | SndIn |
| | SCCA Tx | FDC |
| | SCCB | SCCA |
| | ENetRd | SCCA Tx |
| | ENetWr | SCCB |
| | SCSI | ENetRd |
| lowest | | ENetWr |
| | | SCSI |

The two orderings are not permutations of the PSC_ISR scan order (§3.1), which is purely
positional: interrupt priority and bus priority are independent axes, and SCSI is lowest on both
bus lists while still first in interrupt dispatch.

### 3.4 The UTSC in operation

The UTSC is free-running and monotonic by contract. The only shipped consumer reads it twice per
sample and retries until the pair agrees (the same metastability discipline as the IRs), then
compares samples across time [4]. There is no interrupt, no overflow flag and no documented tick
rate; the register simply advances. Hardware that never advances it does not hang the serial HAL —
its drain timeout degenerates to a pure status poll — but hardware that jumps backward can defeat
the CTS-storm damper [4]. The 48-bit width with a 16-bit-valid high word means the counter wraps
at 2^48; at any plausible tick rate no shipped software can observe the wrap.

### 3.5 The sound DMA engine

The two sound channels run on a dedicated engine, not the general channel file: no control
register, no CmdStat, no PSC_ISR bit. The engine is a double-buffer stream player/recorder over
the buffer pair {sndInBase/sndOutBase, sndSize}: it plays or records half A, then half B, then
half A again, re-latching the base register once per full pass, and exposes its position through
the read-only sndPhase register in 1/64-frame units — the integer frame offset lives in bits
6–17, isolatable with $003FFFC0 [3]. Sample frames are 4 bytes: 16-bit signed big-endian
interleaved left and right; sndSize counts frames, not bytes [3].

Three completion mechanisms exist, all exercised by shipped code [3]:

1. **Polling sndPhase** — the boot chime's method (§4.7); no interrupt involved.
2. **The frame interrupt** — sndComCtl bit 6, delivered through the pseudo-VIA2 window at level 2
   (§2.8); the DSP frame *overrun*, by contrast, is a level-5 source on the PSC's own L5IR.
3. **The codec status interrupt** — level 4, bit 0 of L4IR, acknowledged by the write-back-to-clear
   trick of §2.2.

The engine's clients are the ROM's boot chime and the ROM-resident DSP driver that serves the
Sound Manager's path; the codec-side behavior, frame-bus structure and rate selection are
[Singer](singer.md)'s subject. The developer note also places the PSC's sound channels on the
DAV bus: "The Singer sound codec uses time-division multiplexing to transfer multiple audio
channels between the DAV connector, the Singer chip, and the PSC for DMA transfers to and from
RAM memory" [1] p. 44 — the 1-bit "width" of the sound channels in Table 2-10 is that serial
stream.

## 4. Programming model

### 4.1 Boot-time initialization

Start-up touches the PSC in a fixed order [3]:

1. **Decoder record.** The machine's product record — selected from the ROM's per-machine tables
   by the CPU-ID and strap reads — installs the PSC as the machine's interrupt architecture and
   publishes the base address at decoder slot 27, aliased at slot 33 for the DSP (§2.1).
2. **VIA1.** The initialization path at ROM offsets $AD340–$AD4A0 programs the pseudo-VIA1's
   direction registers, buffer registers, PCR and ACR, exercises the Cuda transport over the
   shift register, and leaves the IER disabled ($7F). VIA1 interrupts come up later, enabled by
   the subsystems that own each source.
3. **VIA2.** The same path initializes the VIA2 window and leaves its IER disabled [3] (ROM
   offsets $AD360, $AD4AC); the sources are enabled later by their owners.
4. **Level-4 enables.** The ROM writes **$86 to L4IER** — sense plus the two SCC bits, no DMA bit
   [3] ($40810A38). This is the *only* PSC interrupt-enable write the boot ROM makes, and its
   shape looks deliberate: with the DMA bit clear and the SCC bits set, the serial ports behave
   the way classic serial drivers expect, while no DMA interrupt can fire until RAM software arms
   a channel and enables the gateway bit (*inferred — unverified*: the value is observed, the
   rationale is not). Everything else — level 3 for Ethernet, level 5 for the
   DSP, the DMA bit at level 4 — is enabled later by the driver that owns it.

A machine that wants to boot the shipped ROM must therefore present: stable (repeat-consistent)
L3–L6 IR/IER reads, a PSC_ISR that reads zero when no channel has completed, a VIA1 window with a
working shift register, and the three-register VIA2 window — every one of these is exercised
before the first disk access [3].

### 4.2 Interrupt installation and service conventions

Software never talks to the `/IPL` lines. The universal pattern, identical across every driver
observed [3] [4]:

1. Locate the base through the decoder record (or hard-code $50F31000).
2. Allocate the handler table if it is yours to allocate (the ROM does this once at start-up, a
   zeroed system-heap block reached through `ExpandMem+$210` [3] ($40810842)).
3. Install: write the 8-byte {handler, parameter} pair into the slot that matches your level and
   bit; a slot reading zero is skipped by the dispatcher, so an uninstalled source is silently
   inert.
4. Enable: the IER sense-write for your level bit, plus the channel's CIE if a DMA completion is
   the source.
5. Service: the handler runs with the parameter in A1, does the minimum at interrupt level, and
   hands client notification to the deferred-task machinery — every shipped DMA completion
   handler follows this shape [4].

Who installs what, across the shipped software set:

| Source | Installer | Notes |
|---|---|---|
| L3 MACE | the ROM's Ethernet driver | enables L3 bit 0 with $81 |
| L4 codec status | the sound stack (Enabler components + ROM DSP driver) | acknowledged by the L4IR write-back of §2.2 |
| L4 SCCA/SCCB | the serial HAL, from the Enabler | saves and restores the previous occupant [4] |
| L4 DMA gateway | each DMA driver, per channel | the serial HAL writes $8A/$8C; the Ethernet driver $88; the floppy never enables it |
| DMA channel 0 (SCSI) | nobody | the SCSI library polls channel 0's control register for completion instead (*inferred*: no slot-0 install site exists anywhere in the shipped ROM or Enabler) |
| DMA channel 3 (floppy) | the ROM's floppy driver | installed for bus-error recovery only [3] |
| DMA channels 4/5/6 (serial) | the serial HAL | one handler per channel, with per-port state as the parameter |
| DMA channels 1/2 (Ethernet) | the ROM's Ethernet driver | one shared completion handler for both |
| L5 DSP / frame overrun | the DSP stack | enables via $83 to $154 [3] |
| L6 timer | RAM system software | no ROM installer exists |

### 4.3 SCSI

The external SCSI port is served by channel 0 and driven by the SCSI Manager 4.3 library in the
boot ROM. Its channel programming is the same as every other driver's — the ROM's setup code
writes the 32-bit buffer address into SCSI_ADDR0 **byte-wise, most significant byte first**
(descending through $50F32003 to $50F32000), then manipulates the command/status word with
single-bit sets and clears [3] (ROM offset $1F080). The DMA hardware handshakes with the 53C96
directly: "There is no possibility of bus errors with the Macintosh Quadra 840AV or Macintosh
Centris 660AV, because the DMA hardware does not attempt to transfer data until the SCSI
controller indicates that it is ready" [1] p. 380 — handshaking descriptors remain in the API
for non-DMA transfers on other machines, but the AV's SCSI Manager 4.3 path is DMA and needs no
escape hatches. The *classic* SCSI Manager that ships in the same ROM does not use the PSC's
channel 0 at all: its compatibility pseudo-DMA path runs through the SCSI controller's
DMA-request line — read through the VIA2 window's bit 0 (§2.8) — and touches no PSC channel
register [3] (*observed*: the ROM's absolute references to $50F03A00 all sit in SCSI and
interrupt-shutdown code, never in the channel-0 DMA path).

The library's own buffer policies — 16-byte-aligned buffers, a minimum transfer below which it
prefers programmed I/O, transfers clipped to contiguous physical runs, and the refusal of client
buffers at or above $40000000 (§3.2) — are library policy; no Apple document ties any of them to
the silicon (*inferred — unverified*, §6).

### 4.4 Floppy

The ROM's `.NewAge` floppy driver drives channel 3. Its read path is the cleanest worked example
of the whole DMA contract, verbatim from the ROM [3] ($4098EC50–$4098EC92):

```
MOVEA.L #$50F31C30,A3        ; channel 3 control register
MOVE.W  #$8400,(A3)           ; SENSE | PAUSE: pause the channel
@frz: MOVE.W (A3),D0          ; \
      BTST  #$E,D0            ;  | spin until FROZEN
      BEQ.S @frz              ; /
      MOVE.W (A3),D0
      ANDI.W #$0001,D0        ; bit 0 = the active set
      ASL.W  #4,D0            ; -> +$00 or +$10
MOVEA.L #$50F32060,A5         ; channel 3 register set 0
      MOVE.L buffer,0(A5,D0.W) ; Addr = the (physical) buffer address
      MOVE.L D2,4(A5,D0.W)    ; Cnt  = the byte count
      MOVE.W #$8A00,8(A5,D0.W); CmdStat = SENSE | ENABLED | DIR (read)
      MOVE.W #$0400,(A3)      ; PAUSE with sense clear: release
      MOVE.B #$20,$50F03C00   ; and disable the FDC VIA2 interrupt for now
```

The driver keeps both register sets in play, using the active-set bit to program the *other* set
mid-transfer when a read must be split at a sector boundary, and takes its completions by
polling, not by interrupt: the channel-3 handler slot is installed solely so a DMA bus error has
somewhere to land, and it does nothing but issue the software reset [3]. The floppy device
interrupt itself is a level-2 affair through the VIA2 window (bit 5), enabled with `$A0` and
disabled with `$20` [3] ($4098EC92) — the DMA channel and the device interrupt are separate
machinery.

One contract reaches outside the PSC: the New Age controller requires a terminal-count signal on
the last byte of a transfer, and the channel's TERMCNT completion is what delivers it; a PSC
model that completes the count without signaling terminal count to the floppy controller makes
every otherwise-good transfer end with an abnormal-termination status (*inferred* — the
requirement is the controller's, the delivery path is the PSC's; see [New Age](new-age.md)).

### 4.5 Serial

The AV serial stack is the PSC's most elaborate client, and the developer note documents its
architecture outright [1] pp. 407–408. A new DMA serial driver ('SERD' resource, four driver
images for `.AIn/.AOut/.BIn/.BOut`) calls a hardware abstraction layer for everything
hardware-specific; "The first HAL implemented is PSCHAL, a DMA HAL for the Macintosh Quadra 840AV
and Macintosh Centris 660AV" [1] p. 407. The HAL is a code resource — **SerialHALPSC**, 'shal'
resource 1 — shipped both in the boot ROM (an earlier revision, at ROM offset $16B820 [3]) and in
System Enabler 088 (the newer revision, 7408 bytes, which the resource manager finds first) [4].
Its channel assignment is fixed and documented [1] p. 408, confirmed by the disassembly [4]:

| Port | Receive | Transmit |
|---|---|---|
| A (modem/GeoPort) | channel 4, DMA, W/REQ on receive, 2 × 1 KB double buffer | channel 6, DMA |
| B (printer/LocalTalk) | **programmed I/O**, one character per receive interrupt | channel 5, DMA, W/REQ on transmit |

Full-duplex DMA exists on port A only: "Full-duplex serial DMA is not supported on port B,
because the printer port is used primarily for output" [1] p. 408. Each SCC channel has a single
W/REQ pin, so a channel can request DMA in one direction only; port A's pin is committed to
receive and port B's to transmit, which is why port B receives by PIO. Where channel 6's transmit
request physically originates — port A's pin being taken — is not visible in any register;
it is presumably an internal Curio-to-PSC connection (*inferred — unverified*, §6).

The DMA constraint the developer note names is alignment: "The PSC DMA hardware presents a minor
limitation in that all serial data transfers must begin on longword boundaries" [1] p. 408, so
the HAL "uses a mixed DMA/SCC model where DMA is used if possible and convenient. If DMA is not
convenient, the classic character-oriented SCC interrupt model is employed until synchronization
is regained with a longword boundary" [1] p. 408 — PIO and DMA coexist on the same port, and the
HAL's send-XON/XOFF path pauses the transmit channel, waits for the SCC transmitter to drain by
PIO, writes the character, and resumes DMA [4]. Receive buffers must be "64 bytes or larger,
aligned to a cache line boundary and a multiple of 16 bytes in length... locked in physical
memory and physically contiguous" [1] p. 408; the HAL allocates its own page-aligned, locked
buffers and copies through them rather than DMA'ing from client memory [4].

The PollProc mechanism is gone, and the developer note gives the PSC the credit: "PollProcs are
completely disabled. The PSC is capable of reading incoming serial data while interrupts are
disabled" [1] p. 408.

The HAL's PSC choreography, from the disassembly [4]:

- **Bring-up (per port)**: fetch the base from the decoder record; allocate, lock and
  physicalize page-aligned buffers; save the previous level-4 SCC handler slot; install the
  level-4 secondary dispatcher; install the channel handlers (port A: channels 4 and 6; port B:
  channel 5); enable the channel's CIE; on port A, save and clear the VIA1 port-A request-mux
  bits (§2.8); arm **both** channel-4 register sets ($9A00 each); program the SCC; release the
  channel; write the L4IER enables ($8A for port A, $8C for port B).
- **Reconfigure**: the full restart — disable the level's SCC bit, CIE off, clear IF on both
  sets of both channels, SWRESET both channels, re-prime both receive sets, program the SCC,
  un-pause, re-enable. Port B's restart is the same minus every receive step.
- **Close**: SWRESET, CIE off, clear IE on both sets of both channels, restore the saved handler
  slots, restore the VIA1 mux bits, free the buffers.
- **Completion (all three channels)**: toggle the tracked active set, clear the completed set's
  IF, read the residual Cnt, deliver the bytes, re-prime the set, and post a deferred task that
  calls the driver's classic `Lvl2DT` routine — "The HAL dispatches serial driver interrupt
  handlers through the 'Level 2' vector tables" [1] p. 407; the familiar Lvl2DT and ExtStsDT
  structures survive, and the HAL chains external/status interrupts to the classic `ExtStsDT`
  vectors after its own CTS bookkeeping [4].
- **Flow control**: the whole of XON/XOFF and CTS transmit throttling is a PAUSE/UNPAUSE of the
  transmit channel, with the FROZEN spin on every pause [4].
- **Special conditions**: on a receive error, the HAL determines which register set holds the bad
  byte by comparing the *other* set's live Cnt against its primed value, flushes the channel, and
  records the error bits per set [4].

### 4.6 Ethernet

The ROM's Ethernet driver (its configuration records are the four `ecfg` resources of §1.2;
the driver code ships in the same ROM) owns channels 1 and 2 plus the level-3 MACE bit [3]:

- **Bring-up**: software-reset both channels (clearing both sets' ENABLED and leaving them
  paused), reset the MACE FIFOs, then bring the channels up with a SENSE | CIE write each, clear
  the interrupt flags on both sets, install the shared completion handler in channel 1 and 2's
  slots, enable L3 bit 0 and the L4 DMA bit, and release the channels.
- **Receive (channel 1, chain mode)**: Addr = the physical base of a chain of 2048-byte buffers,
  Cnt = the number of buffers in the chain; the channel consumes one buffer per received frame,
  decrementing Cnt; the driver tracks occupancy as primed-minus-current [3]. The per-buffer
  layout leaves a 16-byte status granule at offset 0 that the channel back-fills after the MACE
  emits end-of-frame status — the only arrangement consistent with the codec emitting status
  *after* data (*inferred*, §6).
- **Transmit (channel 2)**: one frame per set: Addr = the packet's physical address masked to a
  16-byte boundary, Cnt = the frame length, armed with SENSE | IE | ENABLED; both sets are kept
  armed so the next frame is already primed when the current one completes [3].
- **Teardown / bus error**: software-reset the channel, clear CIE, clear IE and IF on both sets
  [3].

### 4.7 Sound and the DSP

Two ROM-resident clients drive the sound block, and both are fully recovered [3]:

**The boot chime** (entry $408C5BB0, plus four death-chime callers) is an open-coded polling
player that runs at interrupt level 7 with the caches off: it zeroes sndComCtl, initializes the
codec muted, sets sndSize (960 frames for the standard chime), writes sndOutBase, enables output
DMA with sndComCtl = $104, unmutes, and then **spin-waits on sndPhase's frame offset** — waiting
for the offset to leave zero, advancing the base by one full double-buffer pass each wrap,
until the chime's length has played (*observed*: hardware whose sndPhase never advances hangs
the boot at this loop, interrupts masked, forever). It never touches an interrupt; the teardown
leaves the output subframe selected with DMA off ($0004).

**The DSP driver** ($4099B190–$4099CFA0) is the Sound Manager's path: it computes sndSize from
the sample and frame rates, arms the input and output buffers, selects the sample-rate field,
and runs the frame machinery — the frame interrupt through sndComCtl bit 6 into the VIA2 window,
the codec status interrupt at level 4, the DSP-to-host interrupt at level 5. The DSP itself is
held in reset through dspOverRun at bring-up: the driver asserts reset ($83), bootstraps the
DSP3210 through the physical-zero page trick, and releases it ($01); a boot-time timeout halts
it again ($81); sleep and wake toggle reset-enable ($02/$82) [3]. The frame-overrun latch at
level 5 is cleared with $04 — and reset-enable is masked while the frame engine is stopped, so an
overrun cannot reset a sleeping DSP (*inferred*: the two bits are always written together, never
separately) [3].

### 4.8 Slot and expansion interrupts

NuBus and PDS slot interrupts never reach the PSC's own registers: they arrive, active low, in
the VIA2 window's status byte, and the level-2 dispatch reads that byte, inverts it and dispatches
the Slot Manager's per-slot service queues — without writing the IFR to acknowledge, since the
lines are level images the card must drop when serviced [3] (*observed* in the ROM's level-2
dispatch; the exact bit-to-slot assignment is *inferred*, §6). The MUNI's interrupts share the
window. A PDS card that wants to watch the machine's interrupt state can monitor `/IPL(0–2)`
directly — the developer note permits exactly that and nothing more: the lines "can be monitored
by a PDS card" but must not be driven [1] Table 3-22 p. 49.

## 5. Quirks & errata

- **Everything writable is behind a sense bit.** Byte registers use bit 7, word registers bit 15.
  A plain write with the sense bit clear *clears* the named bits — writing `$02` to L4IER
  disables a serial port, and writing `$01` to L5IR acknowledges the DSP interrupt. Half the
  register file silently inverts under a writer who assumes plain data writes.
- **The IRs are read-twice registers.** Every dispatcher reads its IR twice and loops until the
  two reads agree [3] (§3.1). Unstable IR reads hang the machine in a two-instruction loop before
  the first boot phase completes.
- **The IRs are writable.** The sound path acknowledges the codec status interrupt by reading
  L4IR, masking to bit 0 and writing the value back; the DSP path acknowledges with `$01` to L5IR
  [3] (§2.2). A read-only IR model breaks the sound stack.
- **FROZEN must assert.** Every pause in every driver spins on the control register's bit 14
  before touching the set registers [3] [4] — a channel that pauses but never reports frozen
  hangs the first flow-controlled serial transfer.
- **DMAFLUSH self-clears — and completes a receive set.** Writers poll the bit back to zero [4];
  the flushed receive bytes must still reach memory through the completion path (*inferred*,
  §3.2).
- **SWRESET leaves the channel paused.** It clears both sets' ENABLED bits and stops; every user
  follows with an explicit un-pause [4]. A reset-implies-running model breaks the reconfigure
  path.
- **Hardware clears ENABLED at completion and flips the active set.** Drivers poll ENABLED clear
  before re-arming, and read control bit 0 to learn which set to program [3] [4]. A model that
  keeps ENABLED set, or that does not switch sets, breaks gapless streaming.
- **Cnt is 17 bits.** The serial HAL's $1FFFC transfer cap and the Ethernet driver's $1FFFF mask
  are the witnesses [3] [4] (§2.6).
- **Channel 1 counts buffers, not bytes.** The Ethernet receive channel's count is consumed one
  2048-byte buffer at a time [3] (§3.2) — a byte-decrement model silently corrupts the chain
  occupancy arithmetic.
- **One level-4 bit fans out to seven channels.** The DMA gateway bit sends dispatch through
  PSC_ISR, whose top byte is channel-descending: bit 7 is channel 0, bit 1 is channel 6 [3]
  (§2.4). Interrupt priority is positional in that byte and independent of the I/O- and
  CPU-bus arbitration orders (§3.3).
- **Devices interrupt at level 2; their DMA interrupts at level 4.** SCSI and the floppy
  controller are VIA2-window affairs, while their channels' completions come through the level-4
  DMA gateway [3] (§2.8, §4.4). A driver must service both paths.
- **The boot ROM enables only two PSC interrupt bits.** $86 to L4IER — the two SCC bits, no DMA
  bit [3]. Nothing DMA-related can interrupt until a RAM-resident driver enables the gateway.
- **VIA2 is three registers, not a 6522.** Only the flag register, the IER and the slot status
  byte exist at $50F02000 [3]; code that pokes the absent direction/control registers of a
  classic VIA2 hits nothing. Development-stage boards placed even those three at different
  offsets (§2.8).
- **The slot status byte is active low** and the dispatch inverts it without acknowledging
  through the IFR [3] — the opposite assumption breaks every NuBus card driver.
- **Serial DMA must start on a longword boundary** [1] p. 408, so the serial HAL mixes DMA and
  PIO on the same port and even on the same transfer (§4.5).
- **The 0.5 ms interrupt-latency budget is a PSC consequence.** "Do not disable interrupts for
  longer than 0.5 ms" [1] p. 7 — the DMA channels and the sound engine keep running while
  interrupts are masked, and "The PSC is capable of reading incoming serial data while
  interrupts are disabled" [1] p. 408: masking hides completion, it does not pause the hardware.
- **Sound interrupt code still sees level 2.** The dispatch epilogue's deferred level-2 shim
  (§3.1) exists so pre-AV sound code runs at the interrupt level it was written for [3].
- **The decoder table lists the PSC twice.** Slot 27 for the PSC, slot 33 for the DSP — both
  $50F31000, because the DSP's host register is inside the PSC window [3] (§2.1).

## 6. Open questions

1. **The PSC ERS.** No register-level documentation for the part was ever published; the map in
   §2 is reconstructed from the shipped ROM, the shipped Enabler and the developer note's prose.
   Apple's internal engineering requirement spec would settle most of this list and is the one
   document that could.
2. **PSCTEST (+$400).** Its function is unknown; no shipped software in the evidence set touches
   it. Test modes, if any, are unexercised and undescribed.
3. **The UTSC's tick rate.** The width, layout, monotonicity contract and both shipped consumers
   are known (§2.3); the frequency is not, and no Apple document states it.
4. **PSC_ISR bits 23–0** — read as zero, mirrored garbage, or further status? Only the top byte
   is ever tested [3]. Relatedly, whether a channel-7 status bit exists in silicon: no register
   set, no handler slot and no driver exist for a seventh general channel.
5. **The CmdStat low byte (SETMASK).** Defined in the register layout, never used by any shipped
   software; its intended semantics are unknown.
6. **IR latching semantics.** Which request bits are latched and which are live line images; why
   the codec-status and DSP bits need the write-back acknowledge (§2.2) while MACE and DMA bits
   clear at the device. The shipped code is consistent with either model for the latter.
7. **Reset values of every register** (§2.9). Nothing in the observed software contract reads a
   reset value; the power-on contents of every latch are unknown.
8. **The bus-error interrupt path.** BERRIE (+$800) is never written by shipped software, so the
   vector it drives, its acknowledgment and its priority relative to the levels are unobserved;
   drivers poll the channel BERR bit instead (§3.2).
9. **The $40000000 DMA boundary.** The SCSI library refuses client buffers there and copies
   through RAM; the developer note says the channels can reach "RAM or ROM" [1] p. 29, and no
   shipped code ever DMAs to or from ROM. Whether the silicon rejects ROM-space transfers, or
   only the driver does, is unresolved.
10. **The level-6 60 Hz source.** Its exact rate (60.15 Hz-class), its purpose, its installer and
    its relationship to VIA1's 60 Hz timer input are all outside the shipped software set; no ROM
    code installs a handler for it (§4.2).
11. **The VIA2 bit map below bit 5.** Bits 0–3 are attested as SCSI DREQ, slot aggregate, MUNI
    and SCSI IRQ at the dispatch level; the exact source-to-bit assignment, the vSInt byte's exact
    bit-to-slot mapping, and whether the +$1A00 register accepts the classic write-1-to-clear
    acknowledgement at all are inferred from the masks and reads the shipped code applies (§2.8).
12. **Channel 6's transmit request origin.** Port A's W/REQ pin is committed to receive; the
    SCC-visible registers never program a transmit request, yet the channel runs (§4.5). The
    request is presumably an internal Curio-to-PSC signal.
13. **Ethernet chain-mode hardware semantics.** The driver's buffer-chain behavior is pinned
    (§4.6); how the channel skips the status granule, back-fills it after end-of-frame, and
   handles an end-of-frame arriving mid-granule is inferred, not documented.
14. **The sound engine's input side.** Every shipped exercise of the engine observed in this
    evidence set is an output path (chime, DSP driver output frames); the input double-buffer
    and capture cadence are undescribed beyond the register layout (see [Singer](singer.md)).

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV
   Computers*, Developer Press, 1993 — §"Summary of Features" p. 5 (PSC DMA, New Age, MUNI);
   §"Compatibility Issues" p. 7 (the 0.5 ms interrupt rule); §"Machine Identification" Table 1-1
   p. 8 (SysEnvirons 78/60, `gestaltVIA1Addr`/`gestaltVIA2Addr`); §"Physical Forms" p. 10 (the
   two boards as one design); Chapter 2: §"Memory Controller
   and Arbiter" and §"Peripheral Subsystem Controller" p. 13 (the PSC function list), §"Curio"
   p. 16, §"System Clocks" Table 2-2 p. 17 (BClk, C32M, C16M, C22_5792M, C24_576M, ATECS
   clocks), §"Signal Buses" and §"Bus Arbitration" p. 18 (the I/O bus; four CPU-bus masters),
   §"Bus Timeouts" p. 19, §"Access Timing" Table 2-3 p. 20, §"Serial Ports" p. 22, §"PSC
   Functions" pp. 29–30 (nine channels; Table 2-10 channel characteristics; two register sets and
   the VM contiguity rule; Table 2-11 arbitration priorities), §"NuBus Interface" p. 39 (three
   cards on the Quadra 840AV, one on the Centris 660AV), §"Sound I/O" p. 38, §"DAV
   Connector" p. 44 (Singer TDM through the PSC), §"Processor-Direct Cards" Table 3-22 p. 49
   (`/IPL` lines from the PSC; the 660AV bus-master list); Chapter 9 §"Handshaking of Data
   Bytes" p. 380 (no bus errors with DMA); Chapter 10 §"Interrupt Handling" p. 407, §"DMA Versus
   Non-DMA Transmissions" and §"PollProc Mechanism" and §"DMA Use" p. 408 (longword alignment,
   receive-buffer rules, channels 4/5/6 fixed directions, PollProc elimination).
2. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition, Addison-Wesley
   Publishing Company, 1990 — the classic VIA1/VIA2 model the PSC's windows emulate: §"The
   one-second interrupt" p. 145; §"Functions of VIA2" pp. 155–157 (VIA2's absorption into custom
   ICs on the IIci/IIfx as precedent); Table 4-27 p. 186 (VIA2 interrupt flag bits). The book
   predates the AV machines and is used here only for the pre-existing architecture the PSC
   implements.
3. Macintosh Quadra 840AV / Centris 660AV boot ROM (2 MB mask ROM, release $10F3, image checksum
   $5BF10FD1, mapped at $40800000; `CPU address = $40800000 + file offset`) — full disassembly,
   resource-container decode and data-table analysis. Cited sites: the PSC base fetch idiom and
   level-6 dispatcher $40810830–$40810858 (and the level-3/4/5 dispatchers at $408109C0,
   $40810900, $408108A0, with their double-read loops at $10846/$10916/$108B6/$109D2); the DMA
   dispatcher $40810980–$408109AA and the handler-table fetch at $40810842 (`ExpandMem+$210`);
   the L4IER initialization write $40810A38 (`$86`); the VIA1/VIA2 initialization path at ROM
   offsets $AD340–$AD4A0; the `.NewAge` floppy DMA sequence $4098EC50–$4098EC92 (pause/frozen/
   active-set/$8A00/un-pause and the VIA2 IER writes at $4098EC92); the SCSI channel setup at ROM
   offset $1F080 (byte-wise descending address writes into $50F32003–$50F32000); the boot chime
   at $408C5BB0 with its callers and the DSP driver at $4099B190–$4099CFA0 (sound block writes,
   dspOverRun protocol, L5IR/L5IER values); the four AV Ethernet configuration resources at ROM
   offsets $1AEE30, $1AEE90, $1AEEF0, $1AEF50; the ROM-resident earlier serial HAL at ROM offset
   $16B820.
4. Apple Computer, Inc., System Enabler 088 (System 7.1 enabler for the Macintosh Quadra 840AV
   and Macintosh Centris 660AV, version 1.0, 1993) — resource inventory and disassembly. Cited:
   the `'shal'` resource 1, "SerialHALPSC" (7408 bytes, a locked compressed system-heap code
   resource; the Enabler's revision supersedes the ROM's copy at first resource lookup). Cited
   resource-relative offsets: the dispatcher and `halVars` block $0000–$01FF; channel bring-up
   $01B0–$02D0 (decoder-record base fetch, VIA1 port-A mux save/clear, handler-slot install,
   L4IER `$8A`/`$8C`); the reconfigure restart $0430–$05FA; transmit start $0900; receive arming
   $0960 (channel 4 hard-coded, arm word `$9A00`); the control primitives $09B0–$0AC0 (CIE
   `$9000`/`$1000`, IF clear `$0100`, SWRESET `$8800`, pause `$8400` with the FROZEN spin, flush
   `$8200` with the self-clear poll); the UTSC reader $1BE0 and its call sites $0D10 and $17CA;
   the completion handlers $1A00/$1AC0/$1B00; the special-condition handler $1840; the
   `$1FFFC` transfer cap at $0628.
