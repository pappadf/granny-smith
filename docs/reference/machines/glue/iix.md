# Macintosh IIx

The Macintosh IIx is the GLUE family's baseline machine: the modular 18.66-inch workstation that put the MC68030, the MC68882, the SWIM and the FDHD drive into (almost exactly) the Macintosh II's chassis, and that carries the family's full expansion complement — six NuBus slots, $9 through $E, behind a NuChip. Everything it shares with the Macintosh IIcx and Macintosh SE/30 — the GLUE ASIC, the 256 KB universal ROM, the address map, the VIA wiring, the interrupt architecture, the boot contract — is documented once in the family doc ([glue.md](glue.md) §2–§5), and this page cites it by section rather than restating it. What follows is everything unique to the IIx: its identity bits and their ROM consequences, its clocks, its memory population, its two-bay floppy subsystem, its six-slot NuBus implementation, its power-control circuit, and its boot branch points.

**Contents:**

1. [Identity](#1-identity) — what the machine is; identity values; the spec table; the enclosure and its factory load-out
2. [Deltas vs the family doc](#2-deltas-vs-the-family-doc) — the delta list; clocks; memory population; the ROM SIMM as fitted; no built-in video; power control
3. [Per-subsystem wiring](#3-per-subsystem-wiring) — processor and FPU; the six NuBus slots and the NuChip; SWIM and the two internal FDHD connectors; SCSI, serial and ADB; sound; real-time clock
4. [Expansion](#4-expansion) — the six NuBus slots; memory expansion rules; internal storage; what this machine does not expand by
5. [Boot sequence summary](#5-boot-sequence-summary) — the family boot contract with this machine's branch points
6. [Open questions](#6-open-questions)

---

## 1. Identity

### 1.1 What the machine is

"The Macintosh IIx computer contains components identical to those in the Macintosh II except for the following differences": the MC68030 (with its data cache and built-in memory management unit, in place of the MC68020 and its separate AMU/PMMU IC), the MC68882 FPU in place of the MC68881, the SWIM in place of the IWM driving one FDHD drive, and "All ROM in the Macintosh IIx computer is provided on a ROM SIMM" [1] p. 28. Board-level, "The Macintosh IIx computer is very similar in design to the Macintosh II, with the major exception that the Macintosh IIx has an MC68030 processor, and therefore does not have a separate AMU/PMMU device" [1] p. 55; the Guide's block diagram of the machine is drawn once, jointly, for the IIx and the IIcx (Figure 2-7 [1] p. 57), and the IIx's interior is "nearly identical in appearance" to the Macintosh II's [1] p. 26.

Within the family the IIx is the *earliest-introduced* of the three 68030 machines: the family doc records its introduction as 1988, against January 1989 for the SE/30 and March 1989 for the IIcx (glue.md §1.1), which makes it the first MC68030 Macintosh (*inferred — unverified*: an ordering built from those dates and the component lists, not from a printed "first" claim; the exact introduction date is itself unprinted — §6.2). The IIx is also the machine the universal ROM treats as the default II-family member: its two identity bits both read low, where the II, IIcx and SE/30 each differ in at least one (glue.md §6.4).

### 1.2 Identity values

The identity machinery — which bits exist, where they live, and how the ROM reads them — is the family's (glue.md §6.4). This machine's column:

| Identity value | Value | Witness |
|---|---|---|
| ROM family | the universal image, version `$0178`, checksum `$97221136`, 256 KB — byte-identical on IIx, IIcx and SE/30, based at `$40800000` | glue.md §1.3; [5] |
| VIA1 port A bit 6 (the CPU.ID1 position) | 0 — "tied low in the Macintosh II and the Macintosh IIx" | [1] p. 163; glue.md §4.2 |
| VIA2 port B bit 3 (the vFC3 position) | 0 — the II's AMU control line is "not used in the Macintosh IIx and the Macintosh IIcx" | [1] p. 175; glue.md §4.3 |
| Combined identity | `$00` — the only machine of the four the ROM boots with both bits low | glue.md §6.4 |
| ROM machine-type byte (stored at low-memory `$012F`, CPUFlag, and `$0CB3` on boot) | `$01` — the both-bits-low index into the ROM's type table | *inferred* from the disassembled flow ([4]; §5.3) — the SE/30's `$02` is *observed*, the IIx's `$01` is computed, not observed |
| Gestalt machine type | not printed in this evidence set; the IIcx's (6) and SE/30's (9) are recorded in their pages | glue.md §7 item 9; §6.2 |
| Box flag / low-memory identity globals beyond CPUFlag | not established from print | §6.2 |
| VIA1 PA0–PA2 burn-in straps | present on this generation, with soft pull-ups; the IIx's strapped levels are not printed | glue.md §4.2; [3] p. 60; §6.13 |

The practical consequence of `$00` is that the IIx is the machine the ROM lands on when every identity test reads its default — the device-base layout, slot handling and machine-type dispatch that the other three machines branch away from are, in effect, the IIx's configuration (glue.md §6.4).

### 1.3 Spec table

The backbone is the IIx's own entry in the specifications appendix [1] pp. 479–480:

| Parameter | Value |
|---|---|
| CPU | Motorola MC68030 — 32-bit internal architecture, 32-bit external data bus, 32-bit external address bus [1] p. 479; on-chip instruction and data caches (256 bytes each, glue.md §2.1) and on-chip paged MMU [1] pp. 28, 88 |
| Processor clock | 15.6672 MHz [1] p. 479 |
| FPU | Motorola MC68882, a true coprocessor on the same buses (§3.1) [1] pp. 88, 104 |
| Memory management | built into the MC68030; no separate MMU IC [1] pp. 28, 88 |
| RAM | 1 MB standard, expandable to 128 MB, "expandable to 2 GB in NuBus cards"; 256 bytes of parameter RAM in the RTC; 2 KB of sound RAM in the ASC; video RAM in NuBus cards [1] p. 479; §2.3 |
| ROM | 256 KB, on a SIMM, expandable [1] pp. 28, 479; §2.4 |
| Floppy | one internal 1440 KB Macintosh FDHD drive; one optional second internal FDHD drive [1] p. 479; §3.3 |
| Hard disk | one optional internal 40 or 80 MB SCSI hard disk; external SCSI connector accepts a SCSI hard disk [1] p. 479 |
| Video | none on the board — a separate monitor connected to a NuBus card is required [1] p. 479; §2.5 |
| I/O | ADB with two connectors; two RS-422 serial ports with synchronous modem support on one port; SWIM port with two internal connectors; SCSI with one internal and one external connector; stereo sound port for external amplifier or earphones [1] p. 479 |
| Expansion | six internal NuBus slots [1] p. 479; §3.2, §4.1 |
| Sound | four-voice stereo or mono, digital-to-analog conversion at 22.255 kHz or 44.1 kHz [1] p. 479 |
| Keyboards / mouse | Apple Standard Keyboard or Apple Extended Keyboard (ADB); Apple Standard Mouse, 100 or 200 counts per inch [1] p. 480 |
| Real-time clock | CMOS custom chip, 256 bytes of parameter RAM, long-life lithium battery backup [1] p. 480 |
| Power | line voltage 90–140 and 170–270 V rms, 47–63 Hz; 230 W maximum peak input; 132 W maximum sustainable output [1] pp. 479–480, 257, 263; §2.6 |
| Main unit | 140 mm high, 474 mm (18.66 in) wide, 365 mm deep; 10.9–11.8 kg depending on installed drives [1] pp. 26, 480 (Table A-6) |

### 1.4 The enclosure and its factory load-out

The chassis is the wide modular one, shared with the Macintosh II and IIfx: main units "18.66 inches wide", with six expansion slots, against the 11.9-inch three-slot machines [1] p. 26. The main system unit holds the main logic board, the power supply and a cooling fan; expansion cards plug into the NuBus slots, with a video card among them for the external monitor [1] p. 26. The Guide's back view of this chassis (Figure 1-10) shows the two serial port connectors, the SCSI port, the external sound jack and the two ADB connectors, plus punch-out panels where installed cards expose their own connectors [1] pp. 14–15. The drive bay complement is IIx-specific: the unit "can contain either one or two internal floppy disk drives and can contain a 5.25-inch or 3.5-inch hard disk drive", and — unlike the IIcx — "There is room in the case for both a second internal floppy disk drive and a SCSI hard disk" [1] pp. 26, 81.

The shipping configuration follows the spec table: 1 MB of RAM, one FDHD drive, no disk in the hard-disk bay, and a NuBus video card driving one of Apple's three contemporary monitors — the 12-inch 640-by-480 monochrome, the 13-inch 640-by-480 RGB, or the 15-inch 640-by-870 portrait [1] p. 479.

---

## 2. Deltas vs the family doc

### 2.1 The delta list

Everything not in this table is the family baseline by construction (glue.md §2–§5): one GLUE, one universal ROM image, one address map, one VIA pair, one interrupt architecture, one I/O stack.

| Area | Family baseline (glue.md) | Macintosh IIx |
|---|---|---|
| NuBus connectors | $9–$B on the IIcx; none on the SE/30 (§6.2, §6.3) | six, $9 through $E [1] pp. 85, 138 |
| NuBus controller | NuChip30 on the IIcx; none on the SE/30 (§6.2) | NuChip, Apple part 344S0606 [3] §"NuBus Interface" |
| Slot-interrupt inputs (VIA2 PA5–PA0) | three wired on the IIcx; three PDS /IRQ lines plus video on the SE/30 (§4.3) | all six wired to the six connectors [1] pp. 85, 166 |
| ROM carrier | four 512-Kbit ICs plus a SIMM socket on the IIcx; one SIMM on the SE/30 (§1.1) | one 64-pin ROM SIMM, all ROM on it [1] p. 28 |
| Floppy connectors | one internal on the IIcx plus external; one internal plus external on the SE/30 (§6.2) | two internal 20-pin connectors, no external [1] pp. 81, 344 |
| Video | SE/30 PALs emulating a slot-$E card (§6.3) | none — a NuBus card is required [1] p. 479 |
| Processor-direct slot | 120-pin 68030 PDS on the SE/30 (§6.3) | none [3] Figure 4-1 |
| Power control | trickle-charged capacitor and lockable rear switch on the IIcx; hard-wired switch on the SE/30 (§6.2, §6.3) | battery-backed soft power-on, thermal switch, DPDT rear off switch (§2.6) |
| Supply | 90 W sustained on the IIcx (§6.2) | 132 W sustained, 230 W peak, 15-pin connector (§2.6) |
| Identity | `$03` (IIcx) / `$02` (SE/30) (§6.4) | `$00` (§1.2) |
| Chassis | 11.9 in (IIcx) / compact (SE/30) (§1.1) | 18.66 in wide [1] p. 26 |

### 2.2 Clocks on this machine

The family's clock architecture — the GLUE as the single clock source, one 15.6672 MHz net, the derived SCC/ADB and E clocks — is glue.md §2.4's. This machine's column:

| Clock | Value | Consumers | Authority |
|---|---|---|---|
| Processor / system clock | 15.6672 MHz | MC68030, board logic | [1] pp. 113, 479 |
| FPU clock | the same 15.6672 MHz — "the FPU operates on the same 16 MHz clock as does the main processor" | MC68882 | [1] p. 107 |
| SWIM clock | 15.6672 MHz — "twice that used for the IWM interface IC that supports the 800 KB drive"; software halves it for 800 KB drives and the Hard Disk 20 | SWIM | [1] p. 345 |
| SCC / ADB clock | 3.672 MHz | SCC communication rates; the ADB transceiver's microprocessor | glue.md §2.4 |
| E clock | 783.36 kHz | synchronous VIA accesses | glue.md §2.4 |
| NuBus clock | 10 MHz, synchronous | the NuBus, with transceivers bridging to the asynchronous processor bus | [1] p. 85 |
| DRAM refresh | one cycle every 15.6 µs | the GLUE's refresh machinery | glue.md §2.1 |

The cache corollary also lands on this machine: the IIx is one of the GLUE-memory-controller machines that "utilize the 68020 memory controller, so cannot run these burst transfers" — the 68030's caches fill in single-entry mode, and NuBus memory is not cached at all [3] pp. 16–17 (glue.md §2.1).

### 2.3 Memory population

The family's memory contract — 30-pin SIMMs, banks A and B, the GLUE's two-RAS/four-CAS decode, the VIA2 RAM-size bits, the 4-Mbit exclusion — is glue.md §2.2 and §2.5. The IIx's part of it:

- **Standard population** is 1 MB: four 256 KB SIMMs in bank A, bank B empty — the first configuration of the family's configuration figure [1] Figure 5-11 p. 220.
- **The sockets**: four or eight SIMM sockets in the two banks, up to 128 MB [1] pp. 28, 479; their physical arrangement on this board is the one drawn for the II and IIx jointly (Figure 5-7, "RAM SIMM locations in the Macintosh II and Macintosh IIx computers" [1] p. 217), with each pair of SIMMs sharing a single position number [1] p. 216.
- **Speed**: DRAM with a RAS access time of 120 ns or less — the IIx shares the 120 ns requirement with the SE/30, II and IIcx, against the 80 ns of the IIci and IIfx [1] p. 216.
- **Densities**: the legal bank-A densities and the bank-A-at-least-as-large rule are glue.md §2.5's; the hardware overview's machine table lists "256K, 1M, 4M SIMM" as this machine's chip sizes [3] Table 1.4 — but the same book's refresh exclusion bars 4-Mbit DRAMs from the GLUE machines (glue.md §2.2, §7 item 8; §6.7).
- The RAM-size encoding the firmware writes into VIA2 PA7–PA6 at startup, and the address at which bank A gives way to bank B, are glue.md §2.2's; nothing about the IIx changes them.

### 2.4 The ROM SIMM as fitted

"All ROM in the Macintosh IIx computer is provided on a ROM SIMM" [1] p. 28 — the one component-list sentence that separates the IIx's ROM carrier from the IIcx's four soldered 512-Kbit ICs (plus upgrade socket) and matches the SE/30's SIMM. The socket is the family's 64-pin one that "can handle ROMs up to 8 Mbytes" [3] p. 19, and the specifications appendix agrees: ROM "256 KB, expandable to 8 MB" [1] p. 479. The signal assignments of the socket, the /ROMOE strobe and the A0/A1 footnote that distinguishes the IIx-style SIMM from the IIcx-style one are glue.md §2.6's (from [1] Table 5-11 pp. 237–238). One third book prints a different ceiling for the same socket — "256 KB standard in 64-pin ROM SIMM, expandable to 64 MB" [2] p. 15 — see §6.6.

The image the SIMM carries is the family's universal ROM (glue.md §1.3): 256 KB, version `$0178`, checksum `$97221136`, the same bytes that boot the IIcx and SE/30.

### 2.5 No built-in video

The IIx has no video hardware of its own: "Separate video display monitor connected to a NuBus card" [1] p. 479, and Apple's monitor options of the day are listed against that card, not the board [1] p. 479. This is the family's pre-SE/30 condition (glue.md §6.1), and it shapes the machine in three ways:

1. The Slot Manager's world is exactly the six connectors: there is no pseudo-slot $E, no video declaration ROM at the top of slot $E's standard space, and no video interrupt on the slot-$E line — the SE/30's PAL machinery (glue.md §3.6) simply does not exist on this board.
2. VIA2 port A bit 5, the slot-$E interrupt input (glue.md §4.3), is wired to connector $E like any other slot line, not to onboard video logic.
3. VIA1 port A bit 6 — the vPage2/video use on the SE/30 — is on this board the tied-low identity line (§1.2), with no video function at all.

The NuBus video card that must occupy one of the six slots is a standard NuBus device: declaration ROM, frame buffer, sResources and driver per [nubus.md](../../hardware/nubus/nubus.md) §4, and nothing on this page.

### 2.6 Power control: the Macintosh II's circuit, kept

The family's soft-power contract — the keyboard power key, the v2PowerOff bit, the 2 ms software power-down — is glue.md §4.3 and §5.5. The IIx-specific half is that this machine *keeps the Macintosh II's power-control circuit* rather than the IIcx's variant of it:

- **Power-on.** Pressing the power key on the ADB keyboard discharges a capacitor "generating a signal that causes the power supply to switch on within 2 seconds" [1] p. 244 — a figure the power-control section itself gives as "within 1.5 seconds" ([1] p. 270; §6.10). "The capacitor in the Macintosh II and the Macintosh IIx is kept charged by two 3-volt lithium batteries" [1] p. 244 — the family's only battery-dependent soft power-on (glue.md §6.1). The circuit in detail: with the machine off, /POWERON is held high by the same battery that runs the real-time clock; the keyboard switch shorts /POWERON to ground; the power-on circuit then connects the battery to the /PFW line, and "A voltage from +3 V to +6.5 V on the /PFW line causes the power supply to turn itself on. Once the power supply is on, +5 V from the power supply holds the /PFW signal high, keeping the power supply on" [1] p. 270. "The power-on function in the Macintosh IIx and Macintosh IIfx is the same as that in the Macintosh II" [1] p. 270 — against the IIcx's trickle-line variant (glue.md §6.2).
- **Power-off.** The rear switch is a double-pole double-throw switch that "simultaneously pulls /POWERON high (deasserting it) and pulls /PFW low, causing the power supply to switch itself off" — a hard-wired off switch that, like the IIcx's, "does not initiate the software shut-down procedures" of the Shut Down command [1] pp. 245, 271. The software path — VIA2's v2PowerOff assertion, the supply switching off after 2 ms — is the family's (glue.md §4.3, §5.5).
- **Thermal shutdown.** The II-family power-control circuit includes a thermal switch the IIcx's lacks: "If the temperature inside the computer gets too high, the thermal switch disconnects the /PFW signal from the +5 V pullup, causing the voltage to drop below +3 V, and thus causing the power supply to shut down" [1] pp. 270–271. The trip temperature is not printed (§6.9).
- **The supply.** The IIx shares the II/IIfx supply specification block: a 15-pin logic-board connector carrying +12 V, five +5 V pins, six grounds, an unused pin, −12 V and /PFW ([1] Table 6-6 p. 251); AC input 90–140 V and 170–270 V rms, 47–63 Hz, 300 V surge for 100 ms, 60 A peak inrush, 230 W peak input, 70 % minimum efficiency ([1] Table 6-11 p. 257); DC output limits +5 V at 4.90–5.20 V, +12 V at 11.50–12.80 V, −12 V at −13.40 to −10.80 V, with a maximum sustainable load of 18 A at +5 V, 2.5 A at +12 V and 1.0 A at −12 V — 132 W total, 156 W for a 15-second 10 % duty-cycle peak ([1] Tables 6-23/6-24 p. 263); ripple 20/30/30 mV line-frequency and 40/50/50 mV switching noise ([1] Table 6-25 p. 264). The crowbar/short-circuit and over-voltage shutdown behaviors and the /PFW warning contract (pulled low at least 2.2 ms before the DC outputs drop) are the II-family ones [1] pp. 264–265.
- **The monitor receptacle.** The rear-panel monitor power receptacle passes through the supply's input voltage at up to 3 A steady-state / 300 VA, fused at 6 A 250 V [1] Table 6-26 p. 265.

Two of these nets reach the expansion world: the keyboard power-on "signal is also available to NuBus cards, so a NuBus card can be made that can turn on the computer" [1] p. 270, and /PFW "can be monitored by a NuBus card to detect the power-fail warning from the power supply" [1] p. 271.

---

## 3. Per-subsystem wiring

### 3.1 The processor and the FPU

The processor subsystem is the family's spine (glue.md §2.1): an MC68030 with 32-bit external address and data buses and the on-chip MMU — the IIx has no AMU/PMMU socket because "the MC68030 has its own memory management capability" [1] p. 28 — and the GLUE as sole memory and device controller, timing every access per the family's wait-state schedule (glue.md §2.3).

The FPU wiring is worth stating because the Guide states it precisely for this generation: the MC68882 is a coprocessor, not a memory-mapped device. "When the main processor executes a coprocessor instruction (an instruction that begins with $F), it uses address bits A19 through A16 plus some control lines (called the *function code signals*) to indicate that a coprocessor is being addressed, and puts a coprocessor identification number on address bits A15 through A13. The GLUE decodes the address and asserts the device select to the FPU, after which the FPU communicates directly with the main processor without further intervention of the memory management unit or GLUE IC" [1] p. 106. To the programmer the 68882 "adds several instructions and data types, eight 96-bit floating-point data registers, a 32-bit control register, a 32-bit status register, and a 32-bit instruction address register" [1] p. 106; it is "a more efficient version of the MC68881; the MC68882 can run any program written to use the MC68881" [1] p. 106. All FPU data transfers are performed by the main processor at the FPU's request, so memory management, bus arbitration and error handling behave as if the FPU instructions were executed by the 68030 itself [1] p. 107. There is no FPU address window anywhere in the map (glue.md §3) — the device-select mechanism is the only interface.

### 3.2 The six NuBus slots and the NuChip

The NuBus itself — signal set, cycle timing, arbitration, the /NMRQ lines, declaration ROMs — is [nubus.md](../../hardware/nubus/nubus.md)'s; the family's slot-space map and the slot-$0 translation window are glue.md §3.5's. What is unique to the IIx is the *implementation*: six connectors, a real NuBus controller, and every slot-interrupt input wired.

- **Connectors and slot IDs.** Six connectors, slots $9 through $E, "the main logic board is addressed as slot $0" [1] p. 139; the slot ID is geographical — each connector's /ID3–/ID0 pins are strapped by the board to the slot's number, "The highest-numbered slot ($F) has the four signals wired low. The lowest-numbered slot ($0) has all ID signals high. In the Macintosh II, Macintosh IIx, and Macintosh IIfx computers there are six slots numbered $9 through $E" [2] p. 54.
- **The controller.** The NuChip, Apple part number 344S0606 (printed for "Mac II, Mac IIx, IIcx" jointly [3] §"NuBus Interface"; the Guide's component list names the NuChip for the IIx [1] p. 28). The bus interface is drawn as four state machines: the processor-bus-to-NuBus machine, activated by any physical address from $6000 0000 through $FFFF FFFF, which synchronizes the request to the NuBus clock and forwards it; the two NuBus-to-processor machines that carry card-initiated accesses to RAM, ROM and I/O; and a fourth machine that "prevents the NuBus from indefinitely awaiting an acknowledge by generating an acknowledge cycle in response to /START after 256 bus cycles (25.6 μs). A wait this long occurs when the processor makes an access to nonfunctional addresses, perhaps because the card being addressed is not present in any of the NuBus slots" [2] pp. 32–34. That fourth machine is the IIx's empty-slot behavior: an unpopulated slot's space times out after 25.6 µs and returns a bus error to the processor — the recoverable fault the Slot Manager's slot scan depends on (glue.md §3.7). If no slave responds at all, "a NuBus time-out occurs and a bus error (/BERR) signal is sent to the processor" [2] p. 32.
- **The $F0 window.** The processor-to-NuBus machine makes "a special check ... for access to $F0xx xxxx, which is the main logic board's slot address; if attempted, a bus error signal is generated immediately and no NuBus transaction is attempted" [2] p. 32 — the IIx side of the one-way window the family doc describes (glue.md §3.5, from [1] Table 3-10 p. 140).
- **Interrupts.** Every slot has an interrupt line; on the IIx "all the slot interrupt lines go to the VIA2 and to the GLUE IC. The GLUE IC performs an OR operation on the slot interrupt lines and sends the result (called SLOTS) to VIA2. VIA2 ... also records in an internal register the states of the six individual slot interrupt lines" [1] p. 85 — on this machine, all six of those register bits correspond to physical connectors ([1] p. 166; glue.md §4.3). The dispatch flow and the latch-until-serviced card contract are glue.md §5.4's.
- **Standard deviations.** The IIx's NuBus conforms to the 1196-1987 generation with the family's two printed deviations: −5.2 V is not provided (the −5.2 V pins are connected together), and reserved pins A2 and C2 are grounded [3] p. 29. The Apple implementation does not use bus parity and does not support block-move operations between the CPU and RAM, and "maximal transfer rates are not attained, due to insufficient buffering between the processor and NuBus" [3] p. 30.
- **Errors.** NuBus-side failures reach software through the NuChip's /TM0A//TM1A lines into VIA2 port B bits 5/4, decoded by the family's table, and the v2BusLk lockout answers card-to-board transactions with try-again-later (glue.md §3.5).

### 3.3 SWIM and the two internal FDHD connectors

The floppy subsystem is the family's (SWIM at the family's I/O window, the SEL line from VIA1, glue.md §3.3 and §4.2); the SWIM chip itself — its IWM and ISM modes, the four-write mode handshake — is [swim.md](../../hardware/swim.md)'s, with the IWM heritage in [iwm-floppy.md](../../hardware/iwm-floppy.md). The IIx's specifics:

- **The controller.** The SWIM, "an enhanced version of the IWM" [1] p. 28, "controls four of the disk state-control lines, generates signals to select either the internal or external drive, and generates the read-data and write-data signals for the disk drives" [1] p. 344; VIA1 supplies the SEL line (glue.md §4.2). It runs at 15.6672 MHz — twice the IWM's clock — "During the startup sequence, software sets a bit in the SWIM to divide the clock by two when reading and writing to the 800 KB drive or the Hard Disk 20. (The SWIM does not support the single-sided floppy disk drive.)" [1] p. 345. Peak serial rate to the drive is about 500 Kbit/s; because 4 bytes of GCR-encoded data carry 3 bytes of RAM data, the effective GCR-mode rate is about 375 Kbit/s [1] p. 345.
- **The connectors.** Two internal 20-pin FDHD connectors — the machine's drive-bay provision (§1.4) — and no external connector: "The Macintosh II and Macintosh IIx computers have two internal connectors for floppy disk drives ... The Macintosh II and Macintosh IIx have no external floppy disk connectors" [1] p. 81. The pinout is the IIx/IIcx/IIci/IIfx assignment of [1] Table 9-7 p. 346:

| Pin | Signal | Function | Pin | Signal | Function |
|---|---|---|---|---|---|
| 1 | GND | ground | 11 | +5V | +5 volts |
| 2 | PH0 | state-control line | 12 | SEL | state-control (from VIA1) |
| 3 | GND | ground | 13 | +12V | +12 volts |
| 4 | PH1 | state-control line | 14 | /ENBL1 | drive enable, drive 1 |
| 5 | GND | ground | 15 | +12V | +12 volts |
| 6 | PH2 | state-control line | 16 | RD | read data |
| 7 | GND | ground | 17 | +12V | +12 volts |
| 8 | PH3 | register write strobe | 18 | WR | write data |
| 9 | −12V | −12 volts | 19 | +12V | +12 volts |
| 10 | /WRREQ | write data request | 20 | PWMPU | pull-up resistor to +5 V |

  The IIx footnote matters here: "On the Macintosh IIx and the Macintosh IIfx, which can have two internal drives, there are two 20-pin connectors. Pin 14 of each connector carries its respective enable signal; the signals are named /ENABL1 and /ENABL2" [1] p. 346 — the per-connector enable lines are how the SWIM's two-drive support reaches the IIx's two bays. The signals PH2–PH0 (from the SWIM) plus SEL (from the VIA) select registers in the drive; PH3 writes them [1] p. 347. The interface's electrical detail is drawn for this machine specifically: "Figure 9-12 Circuit diagram of the FDHD drive interface on the Macintosh IIx computer" [1] p. 353.
- **Drive complement.** One FDHD drive standard, "and provision for both an internal hard disk and either a second FDHD drive or an 800 KB floppy disk drive"; the FDHD itself reads, writes and formats 400 KB, 800 KB and 1.4 MB disks [1] p. 28. The summary table agrees on the FDHD count and the absence of an external port: IIx, SWIM, "One or two FDHD" internal, none external [1] Table 2-1 p. 81 — see §6.12 for the 800 KB second-bay option's status.

### 3.4 SCSI, serial and ADB

All three subsystems are family wiring (glue.md §3.3, §4.2, §5.5) over device pages; the IIx-specific facts are connector- and population-level:

- **SCSI.** The controller is the 53C80-compatible part the overview's table prints for this machine [3] Table 1.4, at the family's register window with the two pseudo-DMA data paths (glue.md §3.3, §5.5; the chip's protocol in [ncr-5380.md](../../hardware/scsi/ncr-5380.md) §3.8). One internal 50-pin connector and one external DB-25 [2] p. 15; the internal bay takes an optional 40 or 80 MB drive [1] p. 479, of the 5.25-inch half-height 50-pin class the overview lists for this machine [3] Table 1.4. The GLUE's hardware-handshake /DSACK gating — "making SCSI transfers faster and more secure than in the Macintosh Plus" — is glue.md §2.3 and §5.5's.
- **Serial.** Two mini-8 RS-422 ports supporting RS-422 and AppleTalk [2] p. 15, "with synchronous modem support on one port" [1] p. 479 — the vSync/VIA1 mechanism behind that support is glue.md §4.2's; the SCC is the family's 8530 at the family's window, the GLUE's 2.2 µs back-to-back access hold-off applies (glue.md §2.3), and the chip itself is [scc.md](../../hardware/scc.md)'s.
- **ADB.** Two ADB connectors on the rear panel [1] p. 479. The transceiver is the Apple custom chip with its own microprocessor (glue.md §1.2, clocked at the 3.672 MHz net, §2.4), reached through VIA1's CB1/CB2 state lines and shift register (glue.md §4.2); the protocol and the transaction state machine are [adb.md](../../hardware/adb.md)'s. The keyboards and mouse are ADB devices [1] p. 480.

### 3.5 Sound

The sound hardware is the family's — the discrete ASC with its 2 KB of sound RAM [1] p. 479, the two Sony analog ICs, the external stereo jack — at the family's window with the interrupt on VIA2 CB1 (glue.md §3.3, §4.3). The IIx's platform-specific behavior is the II-family one, *not* the SE/30's always-stereo rule: the internal speaker is connected to the left channel only, and a VIA2 input bit — v2SNDEXT, port B bit 6, here a live jack-detect input rather than the SE/30's tie-down (glue.md §4.3) — tells the Sound Manager whether a plug is in the stereo jack; "When using the internal speaker, the Sound Manager sends all audio through the left channel. When an external plug is detected, the Sound Manager switches to true stereo" ([asc.md](../../hardware/asc.md) §3.5, tabulated in §14). The generator specs: four voices, stereo or mono, sample rate 22.255 kHz or 44.1 kHz [1] p. 479. The Sony chips' second job on this board — asserting /RESET for 0.25 s after power stabilizes — is the family's power-up contract (glue.md §2.7).

### 3.6 Real-time clock and parameter RAM

The RTC is the family's bit-banged custom chip behind VIA1 port B (glue.md §4.2), carrying 256 bytes of parameter RAM with lithium battery backup [1] p. 480; the register set and serial protocol are [rtc.md](../../hardware/rtc.md)'s. The battery tie is IIx-relevant twice over: the same battery holds the /POWERON line high when the machine is off (§2.6), and the Guide attributes the soft-power-on capacitor's charge to "two 3-volt lithium batteries" [1] p. 244 — the RTC backup and the soft-power reserve are one resource on this machine.

---

## 4. Expansion

### 4.1 The six NuBus slots

The address spaces a card answers in — 16 MB of standard slot space at $Fs00 0000 and 256 MB of super slot space at $s000 0000, with the 24-bit aliases reaching the lowest megabyte — are the family's (glue.md §3.2, §3.5) and the bus's ([nubus.md](../../hardware/nubus/nubus.md) §2.1). The IIx-specific card-facing facts:

- **Power budget.** "The power allowed in all Macintosh computers except the Macintosh Quadra 900 is 13.3 W per NuBus slot" [2] p. 115; the recommended per-card continuous current is 2.0 A at +5 V, 0.175 A at +12 V and 0.150 A at −12 V, with recommended maximum card filter capacitance of 1513 µF / 536 µF / 698 µF on those rails [2] Table 5-7 p. 114 — figures that already assume the IIx's worst case, a fully loaded machine with the internal hard disk (1.8 A rms max) and two floppy drives (0.2 A typical) installed [2] p. 114. A card that needs the power of multiple slots may take it "if the slot or slots adjacent to your card are not used", by mechanical barrier, multi-card implementation or slot covers [2] p. 115. Exceeding the total budget has a defined symptom: "If the amount of power used by NuBus expansion cards exceeds the total power budget, the Macintosh computer cannot be booted. During startup, the power supply attempts to turn itself on but cannot, and it continues the attempt over and over" [2] p. 115. The NuBus-side supply tolerances are Table 5-6's [2] p. 113.
- **Physical.** The nominal spacing between connector centerlines is 22.869 mm (0.900 inch) on this chassis — against 24.1395 mm on the three-slot machines [2] p. 121 — and the card-clearance envelope is the one drawn for the "Macintosh II, Macintosh IIx, Macintosh IIfx, and Macintosh Quadra 900" chassis, Foldout 3 [2] foldout 3. That envelope permits cards longer than the NuBus specification's 326.6 mm maximum, a grandfathered deviation Apple warns not to rely on for future machines [2] p. 121.
- **Machine-specific firmware hooks.** The sTimeOut declaration-ROM entry — the Slot Manager's lockout-retry count for cards capable of locking out the microprocessor — "is only recognized on the Macintosh II, Macintosh IIx, and Macintosh IIcx computers" [2] p. 178; a driver on this machine can rely on it, on later machines it cannot.
- **Power-line hooks.** The keyboard power-on signal and /PFW are both available at the connectors (§2.6): "a NuBus card can be made that can turn on the computer" [1] p. 270, and a card can monitor /PFW for the supply's power-fail warning [1] p. 271.
- **Memory beyond the sockets.** The specifications appendix credits the NuBus with the machine's furthest memory reach: RAM "expandable to 128 MB, expandable to 2 GB in NuBus cards" [1] p. 479 — RAM cards in slot space (with the family's cache-coherency caveat, glue.md §5.5: NuBus memory is not cached).
- **Reset.** /RESET reaches every expansion slot at power-up, so cards reset with the machine (glue.md §2.7).

### 4.2 Memory expansion rules

The population rules — 30-pin SIMMs of eight DRAMs each, a bank empty or full, larger density in bank A, 120 ns or faster, the 5 MB intermediate configuration, the 128 MB ceiling and the 4-Mbit exclusion — are glue.md §2.5's and apply unchanged here; the IIx's socket layout is Figure 5-7's (§2.3). The bank-A-above-bank-B ordering matters on this machine exactly as the family doc states it, and the firmware programs the same two VIA2 RAM-size bits at the end of RAM sizing (glue.md §2.2; §5).

### 4.3 Internal storage

The chassis holds, beyond the standard FDHD drive: a second internal floppy drive (FDHD, or an 800 KB drive — §6.12) and an internal SCSI hard disk — both at once, "There is room in the case for both a second internal floppy disk drive and a SCSI hard disk" [1] p. 81 — with the hard-disk bay taking a 5.25-inch or 3.5-inch SCSI drive [1] p. 13, of which Apple's contemporary option was 40 or 80 MB [1] p. 479. The internal SCSI cable and the external DB-25 share the family's one bus (glue.md §3.3). No Hard Disk 20 path exists: that drive connects to an external floppy connector, which this machine lacks [1] p. 81.

### 4.4 What this machine does not expand by

- **No processor-direct slot.** The overview's expansion table lists the IIx with six NuBus slots and no direct expansion of any kind [3] Figure 4-1 — the family's PDS is the SE/30's alone (glue.md §6.3).
- **No built-in video** and therefore no monitor connector on the logic board; the display path is a NuBus card, always (§2.5).
- **No cache slot** — that connector appears on the IIci, not on this generation's boards [1] p. 31.
- **No external floppy connector** [1] p. 81.
- **No FPU upgrade path in print**: the MC68882's socketing (or soldering) on this board is not stated by any document in evidence (§6.14).

---

## 5. Boot sequence summary

The boot contract is the family's, established instruction-by-instruction from the universal ROM (glue.md §2.7): the Sony reset supervisor, the overlay fetch of the reset vectors, the early hardware init, the POST engine, the peripheral inits, the 24-bit compatibility map. The machine-unique steps are where the ROM reads this board's identity and where its probes meet this board's decode:

1. **Reset fetch.** With the overlay active (glue.md §2.6), the 68030 takes SSP = $97221136 and PC = $4080002A from the ROM SIMM (glue.md §1.3) — the same first instructions on all three machines [5].
2. **Early init.** SR = $2700, a RESET instruction to the peripherals, the 68030 caches cleared and the PMMU flushed (glue.md §2.7).
3. **Machine-type detection.** The ROM's SWIM/machine-type routine ([4], *observed* in the disassembled flow): it writes $2909 to the 68030's CACR and reads it back, testing the instruction-cache burst bit to confirm a 68030 (the flag becomes 3 — 68030 with SWIM); it then reads VIA1 port A bit 6 at $50F01E00 and, on this board, finds 0; it clears bit 3 of the VIA2 register at base + $400 and reads port B bit 3, and finds 0. The two results index the six-byte lookup table at $4083F79E — $FF, $04, $01, $00, $03, $02 — at index 2 for the IIx (base 2, +2 for a set PA6, +1 for a set PB3): the machine-type byte is **$01** (*inferred*: the index arithmetic and the table bytes are *observed*, but only the SE/30's run — both bits set, index 5, byte $02 — has been recorded end-to-end; §6.3). The byte is stored at low-memory $012F (CPUFlag) and $0CB3 ([4]; the stores are *observed*, their IIx value is the inferred $01).
4. **POST.** The ROM-resident engine sets the VBR to its own vector table, probes for a diagnostic ROM at $58000000 against the signature $AAAA5555 (glue.md §2.7) — on the IIx that address belongs to no printed region of this machine's map (glue.md §3.1), so the probe's outcome here depends on the board's treatment of unassigned space, which is the family's bus-error rule (§6.5) — then verifies the ROM checksum and sizes RAM, leaving MemTop at $0108.
5. **Device-base probes.** The boot code tries $50F18000 before the printed SWIM base and $50F1C000 before the printed SCC base, falling back on probe failure ([4]; the family doc's §7 item 3 records the same alternates). On the SE/30 the probes fall back; what the IIx's decode answers at $50F18000 and $50F1C000 is not established from print (§6.5).
6. **Warm-start check and low memory.** The 'WLSC' cookie at $0CFC decides cold-boot clearing; CPUFlag and MemTop land at $012F/$0108 (glue.md §2.7).
7. **Peripheral init, in the family's fixed order** — VIA1, VIA2, SCC channel A, SCC channel B, SCSI, SWIM (glue.md §2.7) — at the family's device bases (glue.md §3.3).
8. **Timers.** VIA2 T1 loaded for the 60.15 Hz VBL chain, VIA1's CA1 path armed (glue.md §5.6).
9. **The 24-bit map.** The PMMU programmed for the 24-bit compatibility translation (glue.md §3.2) — the IIx needs no AMU emulation because the 68030's MMU does the translation the II's AMU did in hardware [1] p. 136.
10. **Slot scan.** The Slot Manager reads each populated slot's declaration ROM at the top of its standard space; on this machine an empty slot costs the NuChip's 25.6 µs timeout and a recoverable bus error (§3.2, [2] p. 32).

---

## 6. Open questions

1. **The Gestalt machine type and the introduction date.** No document in this evidence set prints the IIx's Gestalt machine-type value or its introduction date; the family doc carries the same gap (glue.md §7 item 9) and this page follows its 1988 dating (glue.md §1.1). Both belong in the record when a printed Apple source enters the library.
2. **BoxFlag and the low-memory identity globals.** Beyond CPUFlag ($012F), the IIx's box-flag byte and any other identity globals the ROM or system software reads are not established from print.
3. **The machine-type table's per-machine mapping.** The six bytes at $4083F79E ($FF, $04, $01, $00, $03, $02) are *observed*; which machine each serves is only partly pinned down. The IIx's $01 follows from the index arithmetic plus the printed strap levels (both bits low), but the annotation's own machine guesses are unreliable, and the SE/30's *observed* run reads both bits set despite the printed tie-low of PB3 — a tension the family doc also carries (glue.md §6.4). A trace of a real IIx boot, or of each byte's consumer in the ROM, would settle the mapping for all four machines.
4. **VIA2 PB3's idle level on this board.** The printed text says vFC3 is "not used in the Macintosh IIx and the Macintosh IIcx" [1] p. 175; that it *reads* 0 is the family doc's record (glue.md §6.4), not a printed strap statement. A floating versus pulled-down pin is indistinguishable in print.
5. **The alternate device bases against the IIx's decode.** What a IIx does at $50F18000 and $50F1C000 — bus error as unassigned space, or an alias under the I/O mirror stride (glue.md §7 item 12) — is unestablished; the printed $0178-generation map (glue.md §3.3) is authoritative, and the probes' fallbacks are what the SE/30 run shows [4]. Likewise whether the $58000000 diagnostic-ROM probe on a IIx reads RAM (in the largest configurations that range decodes as RAM, glue.md §3.1), bus-errors, or something else.
6. **The ROM SIMM ceiling.** "Expandable to 8 MB" [1] p. 479 and "can handle ROMs up to 8 Mbytes" [3] p. 19 against "expandable to 64 MB" [2] p. 15 for the same socket. The two lower figures agree; the third is unexplained (typographical or a later qualification is unverified).
7. **The 4-Mbit SIMM row.** The overview's machine table lists "4M" among this machine's chip sizes [3] Table 1.4 while the same book bars 4-Mbit DRAMs from the GLUE machines (glue.md §2.2, §7 item 8). The family doc's reconciliation guess — SIMM size versus IC density — is unverified.
8. **The IIx's board-level watchdog.** The 44 µs watchdog /BERR is printed for the SE/30 and IIsi boards only (glue.md §7 item 11); the NuChip's 25.6 µs NuBus timeout is this machine's only *printed* timeout. Whether a main-logic-board watchdog exists on the IIx, and its value, is open.
9. **The thermal switch's trip point.** The thermal-shutdown *mechanism* is printed (§2.6) but no document states the temperature at which the IIx's thermal switch opens.
10. **Power-on timing.** "within 2 seconds" [1] p. 244 against "within 1.5 seconds" [1] p. 270 for the same keyboard power-on event; which figure governs is undecidable from print.
11. **The NuBus power total.** The per-slot recommendation (13.3 W, 2.0 A +5 V, [2] pp. 114–115) is printed, but the total NuBus current the IIx's 132 W supply makes available — the number the per-slot figure was derived from by division — is not printed for this machine, and the IIcx's supply part number (699-0392-A, glue.md §6.2) has no printed IIx counterpart.
12. **The 800 KB second-bay option.** The chapter-1 component list offers "either a second FDHD drive or an 800 KB floppy disk drive" in the second bay [1] p. 28, while Table 2-1's IIx row reads "One or two FDHD" [1] p. 81. Whether an 800 KB drive actually works in the second internal connector — the SWIM's halved-clock mode exists for it [1] p. 345 — is stated by neither table.
13. **The burn-in straps.** VIA1 PA0–PA2 are the generation's burn-in/burn-in-mode straps with soft pull-ups ([3] p. 60; glue.md §4.2); the IIx's strapped levels, and the boot consequence of each pattern on this machine, are not printed.
14. **The FPU's socketing.** Whether the MC68882 sits in a socket (as the II's MC68881 does in a PMMU-capable board's coprocessor socket class) or is soldered is not stated for the IIx in any document in evidence; the II's AMU socket is the only socket the Guide names for this generation.
15. **VIA1 PB6 on this board.** vSyncEnA is "not used in the other Macintosh II-family computers" ([1] pp. 171–172); the IIx's PB6 idle level — and whether it reads back deterministically — is not printed.

---

## References

1. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition, Addison-Wesley
   Publishing Company, 1990 — Chapter 1 "Introduction to the Macintosh Hardware": the II-family
   drive-bay provision p. 13, §"Macintosh
   II–family computers" and §"Macintosh II and Macintosh IIx computers" pp. 26–28 (Figure 1-18;
   the II component list; the IIx delta list), front/back views Figures 1-9/1-10 pp. 14–15; Chapter 2
   "Architecture of the Macintosh Computers": the drive-bay prose and Table 2-1 "Non-SCSI disk drives
   used by Macintosh computers" p. 81, §"Macintosh II family" p. 55 and Figure 2-7 p. 57, §"NuBus
   expansion interface" p. 85, §"Expansion interfaces" p. 84; Chapter 3 "Processors and General
   Logic": Table 3-1 p. 88, §"MC68030 interrupts" pp. 101–103, Table 3-6 p. 104, §"MC68881 and
   MC68882 mathematics coprocessors" pp. 106–107, §"GLUE in the Macintosh SE/30, Macintosh II, and
   Macintosh IIx computers" pp. 113–114, §"Address map for the Macintosh II, Macintosh IIx, and
   Macintosh IIcx computers" pp. 135–139 and Table 3-10 p. 140, VIA tables pp. 163, 166, 171–175;
   Chapter 5 "Memory": SIMM speed and locations pp. 216–217 (Figures 5-7, 5-11 p. 220), §"ROM SIMMs"
   pp. 235–238; Chapter 6 "Power Supplies": §"Power up" p. 244, §"System startup" pp. 244–245,
   §"Power down" p. 245, Table 6-6 p. 251, Table 6-11 p. 257, Tables 6-23/6-24/6-25 pp. 263–264,
   Table 6-26 and the abnormal-condition responses p. 265, §"Power-control circuit in the
   Macintosh II-family computers" pp. 269–271 (Figure 6-7, the thermal switch, the DPDT rear
   switch); Chapter 9 "Floppy Disk Interfaces": Table 9-6 p. 344, the SWIM section pp. 344–345,
   Table 9-7 p. 346 and the PH/SEL note p. 347, Figure 9-12 p. 353; Appendix A "Macintosh Family
   Hardware Specifications": the Macintosh IIx entry pp. 479–480 (Table A-6 p. 480).

2. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third edition,
   Addison-Wesley Publishing Company, 1992 — Chapter 1 "Overview of Macintosh Computers With the
   NuBus Interface": Table 1-1 "Major features of Macintosh computers with the NuBus interface"
   pp. 14–16 (the IIx column), §"NuBus interface architecture" and the bus-interface state
   machines pp. 31–34 (the four state machines, the 256-cycle/25.6 µs timeout, the $F0xx xxxx
   special check), §"Identification signals" p. 54 (geographical slot IDs); Chapter 5 "NuBus Card
   Electrical Design Guide": §"Power supply specifications" and Table 5-6 p. 113, §"NuBus power
   budget" and Table 5-7 p. 114, the 13.3 W rule and the over-budget boot behavior p. 115; Chapter 6
   "NuBus Card Physical Design Guide": the card-length deviation and connector spacing p. 121;
   Foldout 3 "NuBus card clearance requirements for Macintosh II, Macintosh IIx, Macintosh IIfx,
   and Macintosh Quadra 900 computers"; the sTimeOut statement p. 178.

3. Apple Computer, Inc., *Macintosh Hardware Overview*, Revision 2, February 11, 1991 — Table 1.4
   "High-End Macintoshes" (the Macintosh IIx column: processor/FPU/MMU, ROM 256 KB, RAM options,
   GLUE, six NuBus slots with NUCHIP, 53C80 DB-25, SWIM, 4 MHz 8530 SCC, ASC with two Sony ICs,
   RTC, 6523 VIAs, 5.25-inch half-height 50-pin hard disk); §"3. Memory" pp. 18–20 (burst
   exclusion pp. 16–17, the 4-Mbit exclusion p. 20, the 64-pin ROM SIMM to 8 MB p. 19); §"4.
   System Expansion": §"NuBus Interface" pp. 29–30 (the −5.2 V and A2/C2 deviations, the unsupported
   NuBus features, Figure 4-1 "Macintosh Processor Expansion", the NuChip part number 344S0606);
   §"VIA functions" pp. 59–60 (the PA0–PA2 burn-in straps).

4. Macintosh IIx / IIcx / SE/30 universal boot ROM, version $0178, checksum $97221136, 256 KB image
   (based at $40800000) — annotated disassembly of the SE/30 image, used here for the
   machine-identity flow all three machines run: the SWIM/machine-type detection routine at
   $4083F74A (the CACR $2909 68030/SWIM test; the VIA1 port A bit 6 read at $50F01E00; the VIA2
   port B bit 3 read via base + $400; the six-byte type table at $4083F79E: $FF $04 $01 $00 $03
   $02), the machine-type stores at low-memory $012F (CPUFlag) and $0CB3, the SWIM base probe
   ($50F18000 then $50F14000) and SCC base probe ($50F1C000 then $50F16000) with their observed
   SE/30 fallbacks, and the diagnostic-ROM probe at $58000000 against $AAAA5555.

5. Macintosh IIcx/IIci factory diagnostics (MacTest) with the ROM POST path — annotated disassembly
   of the universal ROM image and recorded runtime behavior: the reset-vector longwords at
   $40800000 ($97221136 SSP, $4080002A PC), the byte-identity of the IIcx and SE/30 ROM images,
   and the ROM-side POST engine ($40802A14) shared by every machine the image boots — the evidence
   that the IIx executes the same boot code this page's §5 walks through.
