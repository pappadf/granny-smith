# The NuBus

The NuBus is the 32-bit synchronous expansion bus of the modular (68k and first-generation
PowerPC) Macintosh: a multiplexed, geographically addressed backplane standardized as
ANSI/IEEE Std 1196 and implemented by Apple across the Macintosh II through Quadra families,
the AV machines and — through the BART bridge — the first Power Macintosh generation. This
page documents the bus standard as the Macintosh implements it: the slot address space,
arbitration, transactions, block transfers, the slot interrupt lines and the reset/power
contract, together with the machine-independent parts of the declaration-ROM and Slot
Manager model. The declaration ROM's on-card data structures are their own page
([declaration-rom.md](declaration-rom.md)); this page covers only what the *bus* specifies
about them.

**Contents:**

1. [Overview](#1-overview) — what the bus is, the standard, which machines carry it, the
   bridge chips, clocking, physical form
2. [Register file](#2-register-file) — the address-space architecture: standard and super
   slot space, slot $0, the 24-bit compatibility map, the card-to-mainframe view, byte
   lanes and byte order
3. [Behaviour](#3-behaviour) — signals, cycle timing, transactions, arbitration, attention
   cycles and locking, block transfers, interrupts, errors and timeout, reset and power
4. [Programming model](#4-programming-model) — startup slot search, sResources and
   drivers, slot PRAM, interrupt service, block-transfer enable, how real drivers use
   slot space
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the bus is

The NuBus is "a 32-bit-wide bus chosen by Apple to mechanize the multislot expansion of the
Macintosh computers" [1] §"NuBus features" p. 38. Its defining properties, in Apple's own
summary [1] Table 2-1 p. 38:

| Design objective | Supporting feature |
|---|---|
| System architecture independent | Optimized for 32-bit transfers, supports 8-bit and 16-bit nonjustified transfers; not based on the control structure of any particular microprocessor |
| High-speed data transfer | 10 MHz clock synchronizes bus arbitration and read/write transfers over a single 32-bit address space |
| Simplicity of protocol | Reads and writes are the only operations; I/O and interrupts are memory mapped |
| Small pin count | Multiplexed address and data lines — 51 signals plus power and ground |
| Ease of system configuration | Geographical addressing (ID lines) makes cards free of DIP switches and jumpers; distributed, parallel arbitration eliminates daisy-chaining |

Two consequences run through everything below. First, there are no I/O cycles and no
interrupt-acknowledge cycles: everything is a read or a write of a 32-bit address, and
interrupts are carried by a wire per slot rather than by a transaction (§3.7). Second, the
bus is peer-to-peer — "the cards in NuBus slots are peers; no card or slot is a default
master," the one exception being that the main logic board drives the clock [1] p. 39. Any
card may master the bus and address any other card or the main logic board's RAM and ROM
(§2.4).

The NuBus is synchronous — "all transitions and signal samplings are synchronized to a
central system clock" — but "it has many of the features of an asynchronous bus;
transactions may be a variable number of clock periods long" [1] p. 39.

### 1.2 The standard: TI NuBus, IEEE 1196, and NuBus '90

The bus originates with Texas Instruments; "Texas Instruments, Inc. owns patents on NuBus.
If you wish to make a device that works with NuBus, you must obtain a license from Texas
Instruments" [2] p. 462. The IEEE standardized it as *Standard for a Simple 32-Bit
Backplane Bus: NuBus*, ANSI/IEEE Std 1196 [3], [2] p. 461. Apple's first implementation
generation follows the earlier TI specification; the Macintosh Quadra 700 and Quadra 900
implement the 1990 revision, "the latest 1990 draft specification of NuBus, the Standard
for a Simple 32-Bit Backplane Bus: NuBus, ANSI/IEEE Std 1196-1990" [1] p. 38, which Apple's
documentation calls **NuBus '90** [3], [1] §"NuBus '90 features" pp. 41–42.

NuBus '90 adds, over the original signal set: the double-rate block-transfer machinery
(/TM2, /CLK2X, /CLK2XEN); serial-bus lines /SB0 and /SB1 "defined in the P1394 standard"
(bused and terminated but never driven by the main logic board); a cache-coherency
protocol (/CM0–/CM2, /CBUSY) — assigned pins on the Quadra-family connectors, "but the
cache-coherency protocol defined in the NuBus '90 specification is not implemented" [1]
p. 58, [1] §"NuBus '90 features" pp. 41–42 — and the standby-power pin STDBYPWR, defined
only on the Quadra 900 [1] Table 5-5 p. 113. The signal count grows from 51 to 60 on the
same 96-pin connector (§3.1) [1] Tables 2-2, 2-3 pp. 42–43.

The eight connector pins that carried the −5.2 V supply in the original TI specification
were reassigned to the new signals on the Quadra 700 and 900. "Many older NuBus cards
connect those eight lines together; the presence of such a card in the Macintosh Quadra
700 and Macintosh Quadra 900 will disable the new features of all installed NuBus cards
that use those lines. All the other features of both the old and new cards will operate
normally" [1] p. 112 (see §5).

### 1.3 Which Macintoshes carry it

The bus appears, with different slot counts and ID assignments, across every modular
Macintosh generation from the Macintosh II (1987) to the Power Macintosh 8100/110 (1995),
plus adapter-card variants of the compact machines [1] Table 7-1 p. 133, [1] §"Card Slot
Identification Signals" pp. 54–55, [2] p. 461:

| Machine | NuBus slot IDs | Connectors | Notes |
|---|---|---|---|
| Macintosh II, IIx, IIfx | $9–$E | six | the original six-slot backplane [1] p. 54 |
| Macintosh IIcx | $9–$B | three | [1] Table 7-1 p. 133 |
| Macintosh IIci | $C–$E | three | [1] Table 7-1 p. 133 |
| Macintosh IIsi | $9 | one, via adapter card | NuBus adapter in the 120-pin PDS connector; card lies horizontal [1] pp. 54, 129–130 |
| Macintosh SE/30 | (pseudo-slots) | none — PDS only | built-in video emulates a card in slot $E; a PDS card can emulate slots $9, $A or $B [2] p. 102 |
| Macintosh Quadra 700 | $D, $E | two | slot $9's address space is used by built-in video [1] Table 7-3 p. 135 |
| Macintosh Quadra 900 | $A–$E | five | higher power budget, oversized cards, NuBus '90 signals — [q900.md](../../machines/mcu/q900.md) §4.1 |
| Macintosh Quadra 840AV / Centris 660AV | $C–$E / $C | three / one (adapter) | the MUNI bridge — [av.md](../../machines/av/av.md) §5.4 |
| Power Macintosh 6100 | $E | one (adapter card, short cards) | BART 4 on the adapter — [bart.md](../../machines/pdm/bart.md) §1.2 |
| Power Macintosh 7100/8100 | $B–$D | three | BART; slot $E is the PDS video pseudo-slot — [bart.md](../../machines/pdm/bart.md) §1.4 |

Two machines repurpose slot address space without a connector for it: on the Quadra 700
and Quadra 900, super and standard slot $9 space is used for video slot space; on the
IIsi, slot $E space is used for on-board video [1] Table 7-3 p. 135. The SE/30 extends the
trick to its built-in video, which "emulates a NuBus card in slot $E" [2] p. 102 — the
beginning of the pseudo-slot design that later let the Slot Manager describe PDS cards
[1] §"The Slot Manager and the Declaration ROM" pp. 141–142.

### 1.4 The bridge chips

Every machine reaches the NuBus through a bridge that translates between the asynchronous
processor bus and the synchronous 10 MHz NuBus. "The NuBus is a synchronous bus running
on a 10 MHz clock; the processor bus is asynchronous and uses a clock with a rate of
15.6672 MHz or higher, depending on the model," and the handshaking state machines live
"inside the NuBus controller IC (NuChip, NuChip30, or BIU30)" [2] pp. 465–466. Three
controller generations span the 68k era:

| Machine generation | Controller | Source |
|---|---|---|
| Macintosh II, IIx, IIcx | **NuChip** custom IC | [1] p. 31 |
| Macintosh IIci, IIsi adapter card | **NuChip 30** | [1] p. 31 |
| Macintosh IIfx | **BIU30** (control) + **BIU2** (transceivers) | [1] p. 31 |

The two state machines inside the controller divide the labor: "the NuBus-to-processor
bus state machine controls accesses from the NuBus, through the processor bus, to RAM,
ROM, and I/O," and "the NuBus slave state machine is synchronous to the NuBus and tracks
the state changes on the NuBus" [1] pp. 33–35. The NuBus-to-processor bus machine also
"monitors and records when the NuBus master initiates an attention-resource-lock cycle
and controls the subsequent events of a resource-locked transaction" [1] p. 35 (§3.5). The
later bridges are documented on their own pages: the MUNI of the AV machines
([av.md](../../machines/av/av.md) §5.4) and BART of the Power Macintosh
([bart.md](../../machines/pdm/bart.md) §1).

The transceiver side is where the bus's multiplexed /AD31–/AD0 lines meet the main logic
board's separate address and data buses: "the NuBus transceivers buffer, multiplex, and
demultiplex the NuBus address/data bus signals," and an encoder "translates the two
low-order address bits between the processor-bus values and the values used on NuBus"
[2] pp. 465–466 (§2.6, §3.3).

### 1.5 Clocking

The bus clock /CLK is driven from a single source on the main logic board: "an
asymmetric duty cycle of 75% high and a constant nominal frequency of 10 MHz. In general,
signals are changed at the rising (driving) edge of /CLK, and they are sampled at the
falling (sampling) edge" [1] §"Clock Signals" p. 52. The 100 ns period is split 75 ns
high / 25 ns low: "The asymmetric duty cycle of the clock provides 75 ns for propagation
and setup time. Bus skew problems are avoided by having 25 ns between the sample and
drive edges" [1] p. 44. The clock is the only signal a card must never drive; a card's
load on it is the most tightly limited of all pins (18 pF, and Apple warns "it is easily
damaged by the loading effect of a NuBus card") [1] Table 5-2 p. 109.

NuBus '90 adds **/CLK2X**, "a duty cycle of 50% and a constant nominal frequency of
20 MHz," which "synchronizes 2X block transfers between NuBus cards. During a 2X block
transfer, modules drive new data on the assertion edge of /CLK2X and sample the data on
the following assertion edge," and the sense line **/CLK2XEN**: "if there are no boards
that short this line to the other NuBus '90 lines, the line stays low, enabling the
/CLK2X driver" [1] p. 52. Both are defined only on the Quadra 700 and Quadra 900 [1]
p. 52, and the PDM machines carry them to their connectors as well
([bart.md](../../machines/pdm/bart.md) §1.5).

### 1.6 Physical form

The connector is a three-row 96-pin Euro-DIN: "NuBus is a 32-bit-wide, processor-independent
bus using Euro-DIN 96-pin connectors" [2] p. 461. Apple's card is the single-height
NuBus card; "the NuBus specification also specifies a much larger, triple-height card, but
that card cannot be used in a Macintosh computer" [1] p. 119, and "if a NuBus card is
developed according to the NuBus specification, it will be guaranteed to fit into the
entire line of Macintosh computers with the NuBus interface" [1] p. 120. The Quadra 900
additionally accepts oversized cards — the standard length but 2 inches taller — in any
slot, and "you cannot install an oversized NuBus card in any other Macintosh II-family
computer" ([q900.md](../../machines/mcu/q900.md) §4.1).

Slot ordering differs by machine in a way that matters only physically, since the ID
straps — not position — fix a slot's address decode: "when the main logic board is viewed
from above, NuBus slot ID ordering starts with a lower-number ID at the left side and
increases from left to right. However, on the Macintosh Quadra 700 and the Macintosh
Quadra 900, the NuBus slot ID ordering starts with higher ID numbers on the left and
decreases from left to right" [1] §"NuBus Slot Ordering" p. 128. The IIsi adapter mounts
the NuBus card horizontally over the main logic board; the adapter contains "the same
custom NuChip 30 that is found in the Macintosh IIci, ... a 20 MHz MC68882 FPU," and the
interface circuitry, with all translation "transparent, so that NuBus cards will work
successfully in the Macintosh IIsi" [1] p. 129.

## 2. Register file

The NuBus has no programmable register file of its own: every configuration surface the
bus exposes to software is either a *location in the address space* or a *wire*. This
section therefore documents the address-space architecture and the bus-visible
configuration surfaces — the slot windows, the slot-$0 prohibition, the 24-bit
compatibility map, what a card can reach of the main logic board, and the byte-lane
contract that the declaration ROM's format block hangs from.

### 2.1 The 32-bit address space and the two slot windows

"The NuBus architecture allows full 32-bit addresses, providing 4 GB of address space"
[1] §"NuBus Address Space" p. 132. Two of its regions are decoded per slot by the /ID3–
/ID0 straps, with no software configuration of any kind:

| Region | Range | Size per slot | Decode |
|---|---|---|---|
| **Standard slot space** | $F0000000–$FFFFFFFF, of which each slot owns $Fs000000–$FsFFFFFF | 16 MB | /ID3–/ID0 mirrored on /AD27–/AD24 [1] pp. 54–55, 132 |
| **Super slot space** | $90000000–$EFFFFFFF, of which each slot owns $s0000000–$sFFFFFFF | 256 MB | /ID3–/ID0 mirrored on /AD31–/AD28 [1] p. 55, §"NuBus Address Space" p. 133 |

"Because Macintosh computers with NuBus use only slot IDs $9 through $E, only the six
standard slot spaces $F9xx xxxx through $FExx xxxx are actually used" [1] p. 132; the
per-machine list is Table 7-1 [1] p. 133 (§1.3 above). A card needing more than 16 MB of
address space claims its 256 MB super slot window [1] p. 133. "This fixed address
allocation, based solely on the slot location of a card, enables the design of systems
that are free of jumpers and switches" [1] p. 55.

### 2.2 Slot $0: the main logic board, and the prohibition on accessing it

Slot $0 is the computer itself: "Slot $0 corresponds to the Macintosh computer itself. It
addresses the 16 MB of NuBus slot space from $F000 0000 through $F0FF FFFF" [1] §"Slot
Allocations" p. 136. Cards *can* address slot $0 — that is how a mastering card reaches
RAM and ROM (§2.4) — but the processor cannot: "The microprocessor cannot access slot $0,"
and "if the microprocessor attempts to access addresses in this range, it will immediately
generate a bus error (/BERR) exception. No NuBus transaction will take place" [1] Table 7-3
p. 135, §"Slot Allocations" p. 136. The prohibition is enforced before the bus is ever
touched [1] p. 33.

The first Power Macintosh generation breaks this rule on purpose — BART's control
registers live at $F0000000, inside slot $0's standard slot space, and the processor reads
and writes them constantly ([bart.md](../../machines/pdm/bart.md) §2.1, §5).

### 2.3 The 24-bit compatibility map

The 68k Macintosh operates in 24-bit or 32-bit addressing mode, and the bus honors both.
"In 24-bit mode, it can address only 1 MB of each card's standard slot space. This first
megabyte of standard slot space is called **minor slot space**. In 24-bit mode, the
computer hardware translates 24-bit addresses of the form $sx xxxx into 32-bit addresses
of the form $Fs0x xxxx, where s is a digit in the range $9 through $E" [1] p. 133. The
full hardware translation [1] Table 7-2 p. 134:

| 24-bit address range | 32-bit address range | Notes |
|---|---|---|
| $000000–$7FFFFF | $00000000–$007FFFFF | RAM |
| $800000–$8FFFFF | $40000000–$400FFFFF | ROM (aliased) |
| $s0 0000–$sF FFFF | $Fs000000–$Fs0FFFFF | minor slot space, s in $9–$E |
| $F0 0000–$FF FFFF | $50000000–$500FFFFF | I/O (aliased) |

An address of the form $Fssx xxxx "access[es] the same NuBus slot in both 24-bit and
32-bit modes," but in 24-bit mode it is translated to $Fs0x xxxx; "if you need less than
1 MB of address space to be accessible from NuBus, you should design your card to use
only bits /AD19–/AD0. By ignoring bits /AD23–/AD20, you guarantee that addresses of the
form $Fssx xxxx will be valid in both 24-bit and 32-bit modes" [1] p. 134. A card that
observes this rule is addressable by software that never leaves 24-bit mode — which is
why the first megabyte of standard slot space is the compatibility surface, and why the
Slot Manager's per-slot PRAM and the minor base/length sResource fields exist
([declaration-rom.md](declaration-rom.md) §1.1).

### 2.4 What a card sees of the main logic board

"All of the existing address space is accessible from NuBus. It is mapped onto the NuBus
address space" as follows [1] Table 7-3 p. 135:

| NuBus address | Maps to | Notes |
|---|---|---|
| $00000000–$007FFFFF | present RAM | |
| $00800000–$3FFFFFFF | RAM expansion | |
| $F0800000–$F0FFFFFF | ROM (aliased) | via processor-bus $40000000–$4FFFFFFF [1] p. 33 |
| $F0000000–$F070FFFF | I/O (aliased) | "Do not access from a slot card" [1] Table 7-3 p. 135 |
| $60000000–$6FFFFFFF | slow PDS slot space (IIfx) | |
| $70000000–$7FFFFFFF | fast PDS slot space (IIfx) | |
| $80000000–$8FFFFFFF | presently unused | |
| $90000000–$EFFFFFFF | super slot space, slots $9–$E | $9 video slot space on Quadra 700/900; $E on-board video on IIsi |
| $F0000000–$F0FFFFFF | slot $0 (the main logic board) | processor access to this range bus-errors immediately (§2.2) |
| $F1000000–$F8FFFFFF | inaccessible | no transaction |
| $F9000000–$FEFFFFFF | standard slot space, slots $9–$E | |
| $FF000000–$FFFFFFFF | presently unused | |

Two warnings attach to this map. The aliasing is generous by construction: the Macintosh
II, IIx and IIcx decode ROM by the top four address bits alone, leaving ten "don't care"
address bits, "so there are 1024 (2¹⁰) different addresses (aliases) that will access the
same ROM location" [1] p. 33. And the I/O window is explicitly not a supported target:
"The ability to access processor bus I/O devices is not intended for normal use. Access to
anything other than ROM or RAM will probably not be supported on future systems" [1]
p. 33 — a warning the PDM generation later made absolute ("It is not designed to let
plug-in cards gain access to peripheral devices directly," [bart.md](../../machines/pdm/bart.md)
§1.1).

### 2.5 Bus-visible configuration surfaces

The one structure the bus specification forces onto a card is the **declaration ROM**
("also known as the configuration ROM") [1] §"Chapter 8" p. 141: "an area on a NuBus
expansion card that contains firmware that identifies the card and its functions." The
bus-level contract is minimal and lives at the top of the card's standard slot space:

- The card's ROM may be 8, 16 or 32 bits wide, presented on one, two or all four byte
  lanes; "typically ROM sits only on one byte lane" [1] p. 157.
- The top of the ROM carries a format block whose first field, `ByteLanes`, "tells the
  computer which of the four NuBus byte lanes to use when communicating with an
  expansion card's declaration ROM" — a low nibble with a bit per used lane and the high
  nibble its complement — and whose position fixes the format block's start address;
  "the ByteLanes byte always occupies the highest address available in the byte lanes
  being used" [1] §"ByteLanes" pp. 158–159.
- Everything else — TestPattern, CRC, the sResource directory, drivers — is the Slot
  Manager's firmware format, documented in full in
  [declaration-rom.md](declaration-rom.md) §2–§8.

The other bus-visible declaration is the **block-transfer capability** longword
(`sBlockTransferInfo`): "if the entries specifying block-transfer information are
omitted, the bus master should assume that the target board does not support block
transfers," and a board that does not support block transfers but lacks an early-/ACK
termination "must have the sResource block-transfer information present with the slave
transfer size bits set to 0" [1] §"Block-Transfer Information" p. 181. The fields are
IsMaster, IsSlave, LockedTransfer and per-size TransferSize bits [1] Table 8-8 p. 182.

### 2.6 Byte lanes and byte order

The NuBus bit structure does not match the 68k processor bus, and the Macintosh resolves
the mismatch by choosing byte-address consistency over byte-significance consistency:
"Apple chose to preserve byte-address consistency; each of the 4 bytes of the processor
is connected to its corresponding NuBus byte lane. ... That is, byte $n$ of the processor
is connected to NuBus byte lane $n$" [1] §"NuBus Bit and Byte Structure" p. 137.
"Byte-lane routing is performed automatically by Macintosh computers. Only the bytes are
swapped, not bits within bytes" [1] p. 137. Within a NuBus word, byte 3 is the most
significant; in a 68020/68030/68040 longword, byte 3 is the least significant [1]
pp. 136–137. Bits do not correspond either: "bits D31–D24 (byte 0) of the processor are
connected to bits AD7–AD0 (byte lane 0) of the NuBus" [1] p. 137.

The practical consequence for heterogeneous cards: "a word read of a location within the
[Intel] 80386 card that contains a 32-bit value of $1234 5678 is seen as $7856 3412 by the
Macintosh processor because of the byte swapping" [1] p. 137 — the same rule the PDM
generation inherits verbatim ([bart.md](../../machines/pdm/bart.md) §3.3). All data
transfers are **unjustified**: "a byte of data is conveyed on the same byte lane
regardless of the transfer mode used to access it," so bytes with address 0 modulo 4
always travel on /AD0–/AD7, 1 modulo 4 on /AD8–/AD15, and so on [1] p. 59.

Related, and byte-lane visible, is **byte smearing**: "The MC68020 and MC68030 processors
share a characteristic that causes the data for byte and word transfers to be duplicated,
or smeared, across all 32 data lines" — a byte written by a 68020/68030 appears
replicated on every lane. "The byte-smearing feature does not exist on 68040-based
machines. If you have software or hardware that depends upon byte smearing, you must
revise it" [1] §"Byte Smearing" pp. 138–139.

## 3. Behaviour

### 3.1 The signal set

All NuBus signals are active low; "a slash preceding a signal name indicates that it is
active-low" [1] p. 45. The original implementation totals 51 signals on 96 pins [1]
Table 2-2 p. 42:

| Class | Signals | Electrical type |
|---|---|---|
| Address/data | /AD31–/AD0 | tristate |
| Control | /START, /ACK, /TM0, /TM1 | tristate |
| Arbitration | /ARB3–/ARB0, /RQST | open collector |
| Slot ID | /ID3–/ID0 | per-slot straps (pull-ups required on the card side, 3.3 kΩ–10 kΩ) [1] p. 54 |
| Parity | /SP, /SPV | tristate |
| Utility | /RESET, /CLK, /PFW, /NMRQ | /RESET open collector; /NMRQ a separate line per slot (§3.7) |

NuBus '90 reassigns eight of the former −5.2 V pins and brings the total to 60 signals:
/TM2, /CLK2X, /CLK2XEN, /CM0–/CM2, /CBUSY, /SB0–/SB1 and STDBYPWR [1] Table 2-3 p. 43,
Table 5-4 p. 112. The full connector pin assignments — original and NuBus '90 — are
Tables 5-3 and 5-4 of the card-design book [1] pp. 111–112; the signal-by-signal list is
the hardware guide's Table 14-9 [2] p. 465.

Two signals are specified but inert in every Macintosh: the parity pair. "In current
versions of the Macintosh computers, this line [/SP] is pulled high" and "cards that do
not generate bus parity never drive /SPV active, and cards that do not check parity
ignore /SP and /SPV. Future Apple products may employ this feature" [1] §"Bus Parity
Signals" pp. 57–58. Likewise the cache-coherency pins exist on the Quadra connectors but
the protocol "is not implemented" [1] p. 58.

### 3.2 Cycle timing and signal determinacy

A clock cycle is 100 ns, "from one rising edge to the next" [1] p. 46. Signals are driven
on the rising edge and sampled on the falling edge (§1.5), and the AC timing budget is
specified at the connector [1] Table 5-8 p. 117:

| Parameter | Meaning | Min | Max |
|---|---|---|---|
| Tcp | clock period (averaged over 1 s) | 99.99 ns | 100.01 ns |
| Tcw | clock width | 73 ns | 77 ns |
| Ton / Toff | turn-on / turn-off time at driver | 0 | 35 ns |
| 2Tpd | bus delay, 16 loaded slots / 8 loaded slots | — | 17 ns / 10 ns |
| Tsu | setup time at receiver | 21 ns | — |
| Th | hold time at receiver | Tcp − Tcw | — |

Signal determinacy — what a card may conclude about an undriven line — is defined by four
rules: a driven signal is determinate in its own cycle; a line unasserted in cycle $n$ and
not driven in $n+1$ stays unasserted; an open-collector line driven in $n$ and not driven
in $n+1$ is guaranteed unasserted in $n+1$; but "if a tristate signal is asserted during
cycle $n$ and is not driven during cycles $n+1$ and $n+2$, then the line is not guaranteed
determinate during cycle $n+1$ but is guaranteed to be unasserted during cycle $n+2$" [1]
§"Signal Line Determinacy" p. 55.

### 3.3 Single-data-cycle transactions

The base unit of transfer is the **transaction**: one start cycle, zero or more data
cycles, one acknowledge cycle [1] Figure 2-3 p. 50. "All transactions are initiated by a
bus master, which drives /START active while driving the /TMx, /AD0, and /AD1 signals to
define the cycle type. The remaining /ADx signals are also driven to convey the address.
The transaction is completed when the responding slave drives /ACK active while driving
status information on the /TMx lines" [1] p. 59. The 32 /AD lines are multiplexed —
address at start, data later [1] p. 57.

The cycle type is encoded by /TM0–/TM1 together with the two low-order address lines,
which carry mode rather than address during the start cycle [1] Table 3-1 p. 57:

| /TM1 | /TM0 | /AD1 /AD0 | Cycle |
|---|---|---|---|
| L | L | per /AD1–/AD0 | Write byte 3 / 2 / 1 / 0 (LL, LH, HL, HH) |
| L | H | L L | Write halfword 1 |
| L | H | L H | 1X block write |
| L | H | H L | Write halfword 0 |
| L | H | H H | Write word |
| H | L | per /AD1–/AD0 | Read byte 3 / 2 / 1 / 0 |
| H | H | L L | Read halfword 1 |
| H | H | L H | 1X block read |
| H | H | H L | Read halfword 0 |
| H | H | H H | Read word |

(The base unit of addressability is the NuBus word: /AD31–/AD2 select the word, /AD1–/AD0
and the /TMx lines select the part of it [1] p. 57. Halfword 1 is /AD15–/AD0, halfword 0
is /AD31–/AD16; byte 3 is the most significant byte of the word [1] Figure 3-1 p. 59.)
On the Quadra 700/900, /TM2 extends the encoding with the two double-rate block modes
[1] Table 3-1 p. 57: /TM2 L L H = 2X block write, /TM2 L H H = 2X block read.

A **read** transaction runs: the master drives /START, /ACK high and the /ADx//TMx lines
at R(1); the slave samples at F(1); the master releases the bus and waits; the slave
drives data and status and asserts /ACK at R(n), $2 \le n < 256$; the master samples at
F(n); both release at R(n+1) [1] p. 60. A **write** is the same shape with the master
driving data from R(2) and holding it until acknowledged, and "the bus master has the
responsibility for aligning data onto the appropriate /ADx lines for halfword and byte
writes" [1] pp. 61–62. The slave may take up to the system-defined time-out period
(§3.8) to acknowledge; inserted wait states appear as extra clock periods between start
and ack.

During the acknowledge cycle the slave reports status on /TM0–/TM1 [1] Table 3-2 p. 63:

| /TM1 | /TM0 | Acknowledge |
|---|---|---|
| L | L | Bus transfer complete |
| L | H | Error |
| H | L | Bus time-out error |
| H | H | Try again later |

The error and try-again-later codes "terminate the transaction in a normal manner, and
the bus master has the responsibility for handling the error condition reported"; a
master receiving try-again-later should retry, and "slaves should be designed so that a
large number of retries are not required" [1] p. 63.

### 3.4 Arbitration

Mastership is decided by a distributed, fair contest. Contenders drive /RQST (open
collector) and place their ID code on /ARB3–/ARB0; "after two clock periods, signal
transients have settled and the contest mechanism is complete. The contender with the
highest ID code has its code on the /ARBx lines, has won bus ownership, and may initiate
a transaction (after completion of any transactions in progress)" [1] §"Arbitration
Overview" p. 98. Each card's arbitration logic drops its /ARBx outputs when it sees a
higher ID on the bus; the card-design book gives the logic equations and a PAL
implementation [1] pp. 99–100, Appendix D. Contests last exactly two clock periods; the
winner "may now take control of the bus and assert /START on the next driving edge (25 ns
after the contest's second sampling edge) if the bus isn't in use," or immediately after
the current transaction's /ACK if it is [1] §"Arbitration Timing Overview" p. 101. The
/ARBx timing differs from all other signals — it is specified from the sampling edge of
the clock, with an arbitration period of at most 200 ns [1] Table 5-9 p. 118.

The fairness mechanism is one rule: "**The /RQST line may be asserted only while it is in
an unasserted state.** All cards that assert /RQST place their ID codes on the /ARBx lines
and contend for the bus" [1] p. 98. A card that requested in the same clock period as the
winner keeps its /RQST asserted and is served next; cards that only later *want* the bus
cannot assert /RQST until the line is free, "which keeps other cards from participating
in contests until all the original requesters have been served" [1] p. 98. Same-cycle
requesters are thus granted ownership in descending ID order, without further contests.

After its last transaction the winner releases /RQST and is **parked** on the bus: it
"may use it at any time (without rearbitration) until another card asserts /RQST. When
/RQST is finally asserted by another requester, the parked bus master finishes its
current transaction and relinquishes the bus to the new winner without commencing another
transaction" [1] §"Bus Parking" p. 105. A parked master may not enter a locked tenure
without re-arbitrating: "A bus owner is not allowed to go from a parked condition into a
bus-locked series of transactions without submitting to arbitration by asserting
/RQST" [1] p. 105.

Fairness has a documented dark side: no priority is given to the system. "If there are
many NuBus masters active at the same time, the CPU, especially in the Macintosh IIfx,
can be given too little time to refresh memory. This problem is actually caused by the
'fair' NuBus arbitration scheme; no priority is given the CPU in its bid for NuBus
access. ... On most Macintosh computers, the symptom of this problem will be extremely
slow or nonexistent updates to the screen" [1] p. 101.

### 3.5 Attention cycles and locking

An **attention cycle** is a cycle in which both /START and /ACK are asserted; the /TMx
lines then carry the attention coding [1] Table 3-3 p. 64:

| /TM1 | /TM0 | Attention cycle |
|---|---|---|
| L | L | Attention-null |
| L | H | Reserved |
| H | L | Attention-resource-lock |
| H | H | Attention-cache |

Attention-null reinitiates arbitration — "the new bus owner must generate an
attention-null cycle" if it wins the bus and decides not to transfer data — and
terminates a locked tenure; during any attention cycle the /ADx lines are ignored and no
data may be transferred [1] p. 64. Three implementation rules make the dual-purpose
lines safe: masters must drive /ACK high during their start cycle; a master's first
/ACK must terminate its transaction; and "slaves must qualify /START with the logical
complement of /ACK to decode a start cycle. Otherwise, an attention cycle could be
misinterpreted as a start cycle" [1] p. 64. (The attention-cache cycle belongs to the
unimplemented cache-coherency protocol [1] p. 64.)

**Bus locking** needs no extra mechanism: the master simply keeps /RQST asserted and its
ID on the /ARBx lines, so it wins every subsequent contest. "Fairness in arbitration
depends upon cards not locking the NuBus unless required and locking it only for the
shortest required tenure," and — the hard limit — "**The bus must not be held in a
locked condition for more than four transactions at a time**" [1] pp. 101–102.

**Resource locking** protects a shared resource (dual-ported RAM, or the main logic
board's RAM) against a local processor during an indivisible read-modify-write. The
owner opens the tenure with an attention-resource-lock cycle and closes it with
attention-null; "all cards that have shared resources capable of being locked must
monitor the NuBus for an attention-resource-lock cycle and must record the occurrence"
[1] §"Resource Locking" pp. 103–104. On the Macintosh side the bridge does this
automatically for the processor's own indivisible accesses: "On the Macintosh IIcx,
Macintosh IIci, Macintosh IIfx, Macintosh Quadra 700, and Macintosh Quadra 900, the NuBus
interface controller automatically performs an attention-resource-lock cycle before doing
a read-modify-write cycle, and an attention-null cycle afterward," and the state
machines then "lock the processor bus" against the local processor for the duration
[1] p. 104. In the other direction, the II-family VIA offers a protective signal:
VIA2 "provides a signal (/BUSLOCK) that blocks NuBus cards from directly accessing the
main logic board. This signal is used to protect time-critical operations from
interruption by NuBus transactions" [2] p. 156.

The first Power Macintosh generation drops resource locking silently: locked NuBus
transactions "fail without a bus error being generated" ([bart.md](../../machines/pdm/bart.md)
§3.4).

### 3.6 Block transfers

A block transfer conveys multiple consecutive words in one transaction, master-paced at
start. Two forms exist, both defined by NuBus '90 and both implemented only on the
Quadra 700 and Quadra 900 [1] p. 66, [1] p. 71:

- **1X block transfers** — words move at the 10 MHz /CLK rate. Allowed lengths are 2, 4,
  8 and 16 words, encoded on /AD5–/AD2 together with the block's starting address, which
  must correspond to the size: a 2-word block starts word-aligned, a 4-word block on a
  4-word boundary, and so on [1] Table 3-4 p. 66. Each intermediate data cycle is
  acknowledged by the slave asserting /TM0 alone; the final word takes a standard /ACK
  cycle with a status code [1] p. 66. Flow control is the slave's only: it holds /TM0
  unasserted until the next word is ready, and "the speed of a 1X block transfer is
  controlled by the slave; therefore, a master requesting a 1X block transfer must be
  capable of transferring data at the speed of the fastest slave in the system. ...
  If the master is incapable of transfers at the speed the slave specifies, an
  undetectable overrun (or underrun) occurs" [1] §"1X Block Transfer Errors" p. 70.
- **2X block transfers** — words move at the 20 MHz /CLK2X rate, two words per /CLK2X
  assertion. Lengths are 4 to 256 words (no 2-word size), encoded on /AD9–/AD2 with the
  same alignment rule [1] Table 3-5 p. 72. Both sides pace the transfer — the master
  with /TM2, the slave with /TM0 — "which makes larger burst transfers possible without
  the need for large buffers or frequent arbitration" [1] §"2X Block-Transfer Flow
  Control" p. 72.

The fallbacks are part of the protocol and are the reason the block-transfer sResource
of §2.5 exists. A slave that cannot support block transfers at all "should terminate
the first transfer with /ACK and a normal status code. This is not considered an error
condition. The data should be ignored for read or write purposes, but the master shall
not assume that the data transfer did not take place" [1] p. 70. A 2X request of 16
words or fewer to a 1X-only slave degrades gracefully — the slave "respond[s] by driving
/TM1 and /ACK unasserted, with /TM1 unasserted during all intermediate acknowledges ...
and the block transfer will be completed using the 1X block transfer" — but "if the bus
master issues a 2X block-transfer request of 32 NuBus words or more to a slave that
cannot support 2X block transfers, the slave issues an immediate acknowledgment cycle
with a bus-transfer complete status code" [1] p. 73. A block transfer may also be cut
short at any word by an error acknowledge [1] p. 70.

The Quadra-family machines "allow double-rate block transfers between NuBus cards, but
they do not support double-rate transfers to or from the main memory" [1] p. 71. Later
bridges re-generalized block transfers to main memory behind per-slot enables — the
MUNI's BlkAttmpt register and `_SlotBlockXferCtl` ([av.md](../../machines/av/av.md) §5.4)
and BART's per-slot burst bytes ([bart.md](../../machines/pdm/bart.md) §2.6, §4.4).

### 3.7 Interrupts: the per-slot /NMRQ lines

The bus proper defines one interrupt wire, the **nonmaster request** /NMRQ —
"asynchronous to /CLK," provided "as an interrupt mechanism for cards that are intended
to be slave-only. Such cards avoid the cost of implementing arbitration logic" [1]
§"Nonmaster Request Signal" p. 53. Three ways of carrying interrupts over a NuBus system
are possible, but only one is used by the Macintosh [1] §"Interrupt Operations" p. 65:

1. Interrupt by write transaction into a monitored address range — "Interrupts are not
   done this way on Macintosh computers."
2. All slots sharing a single wired-OR /NMRQ, with software polling the slots —
   "Interrupts are not done this way on Macintosh computers."
3. "By a dedicated /NMRQ line from each slot. Macintosh computers with NuBus use a
   separate (non-NuBus) /NMRQ line from each slot to support interrupts. Each card slot
   has a unique /NMRQ line driving an OR gate whose output is a real hardware interrupt
   signal to the microprocessor (through VIA2, or equivalent). In addition, each of the
   /NMRQ lines can be independently polled by the processor" [1] p. 65.

The service contract is level-based and explicit: "NuBus expansion cards must keep the
/NMRQ signal asserted until the interrupt service routine gets called. The interrupt
service routine must clear the interrupt" [1] p. 65 — the card drops its line when
serviced, not when the interrupt is taken.

The collector differs per machine generation. On the Macintosh IIx and IIcx "the GLUE IC
performs an OR operation on all the NuBus interrupt lines and sends the result to an
interrupt request line on VIA2. Each of the NuBus slot interrupt lines is also connected
to a data input of VIA2. When the main processor receives an interrupt, it polls the
Interrupt Flag register in VIA2 to determine the source and, if the interrupt was caused
by a NuBus slot, the main processor then polls VIA2 Data register A to determine which
slot was the source" [2] p. 102. The IIci folds the GLUE into the RBV and the IIfx uses
the OSS, each "performing both the recording of the interrupts from the individual slots
and the OR operation" [2] pp. 102, 157. The SE/30 — a PDS machine — provides its first
three slot-interrupt lines to the PDS connector "so an expansion card can emulate a NuBus
card in any of the first three NuBus slots (slots $9, $A, or $B)," while its built-in
video "emulates a NuBus card in slot $E" on the last line [2] p. 102. The later platforms
keep the per-slot-line model with new collectors: the AMIC's pseudo-VIA2 slot bank on PDM
([bart.md](../../machines/pdm/bart.md) §3.6) and the PSC-VIA2 window on the AV machines
([av.md](../../machines/av/av.md) §5.4).

### 3.8 Errors and the bus timeout

If a transaction is never acknowledged — an empty socket, a card that does not decode
the address — the main logic board ends it by impersonating the missing slave:
"Bus time-out support logic on the Macintosh main logic board enforces a period of 256
clock periods, or 25.6 µs, and assumes the role of the nonresponding slave; it generates
an acknowledge cycle with a bus time-out error code" [1] §"Acknowledge Cycles" p. 63.
Every transaction therefore terminates within 25.6 µs of its start cycle, bus healthy or
not; the same 25.6 µs figure is the MUNI bridge's transaction timeout on the AV machines
([av.md](../../machines/av/av.md) §5.4).

At the processor the three non-complete status codes surface as a single exception: "A
Macintosh computer generates a processor bus error exception (/TEA signal on 68040
machines, /BERR signal on others) if its microprocessor attempts a NuBus access that is
terminated with an error, a bus time-out, or a try-again-later response" [1] p. 63. On
the II-family machines the *type* of the fault is retained for software: VIA2 "records
two status lines from the NuBus (/TM0A and /TM1A). If an error occurs during a NuBus
access, a Bus Error signal is sent to the main processor by the NuBus controller (NuChip,
NuChip30, or BIU30) and the error type is sent to VIA2 over these two lines. The main
processor can read a register in VIA2 to find out the values of these two signals" [2]
p. 156.

The timeout is load-bearing for the whole expansion model: it is why an empty slot is
discoverable at all. The Slot Manager's declaration-ROM search reads the top of every
slot's standard space and relies on the empty ones answering with a clean, recoverable
fault rather than floating data (§4.1); the bridges inherit the same contract
([bart.md](../../machines/pdm/bart.md) §3.5).

### 3.9 Reset and power

/RESET is "an open-collector line that is asserted asynchronously to the NuBus clock.
When asserted, /RESET causes a NuBus interface initialization for all cards (bus
reset)" [1] §"Reset Signal" p. 52. Apple's implementation deviates slightly in duration
from the IEEE standard ("there is a slight deviation of the duration of the /RESET
signal from that specified in the IEEE 1196 NuBus standard"), and the timing depends on
how the reset happens [1] pp. 52–53:

| Reset cause | /RESET behavior |
|---|---|
| Initial power-on | driven low for a nominal 200 ms after the supplies stabilize; then, about 3 µs later, a second assertion of 33 µs from the ROM's startup code |
| Reset button | asserted as long as the button is held, plus the nominal 200 ms, then 3 µs deasserted and 33 µs asserted again |
| Restart command | the ROM executes two reset instructions about 3 µs apart: 33 µs asserted, 3 µs deasserted, 33 µs asserted |

"You should treat all assertions of /RESET (of any duration) identically" [1] p. 53. The
33 µs assertions come from software: "As part of the startup code in the ROM, a reset
instruction is executed shortly after the microprocessor comes out of hardware reset" [1]
p. 53 — which is why, on machines whose bridge does not re-assert /RESET on a soft
restart, the ROM must pulse the line itself in software
([bart.md](../../machines/pdm/bart.md) §2.2, §4.2).

/PFW is the power-control wire, overloaded with two jobs: "to allow the power supply to
be turned on and off by a low-voltage signal ... and to allow the power supply to warn
the computer of an impending power loss" [1] §"/PFW Interaction With the Power Supply"
p. 110. Its Macintosh semantics are unusual in that a *card* can use it: "Driving /PFW
high turns the computer on; driving /PFW low turns it off" [1] p. 53. The thresholds:
held between 3.0 and 6.8 V for at least 1.5 s the supply turns on and thereafter holds
itself up with its own +5 V output; pulled below 0.6 V it turns off; on an AC-line
failure the supply itself pulls /PFW low "at least 2 ms before the DC outputs fail" [1]
p. 110. A card intending to switch the machine must source 20 mA at 3 V for 2 s, add no
pull-up, and present a high impedance at all other times [1] pp. 109–110. On the Quadra
900 the standby pin STDBYPWR supplies "small current at +5 V when main power is off,"
specifically so a card can perform this /PFW power-on ([q900.md](../../machines/mcu/q900.md)
§4.1), [1] Table 5-5 p. 113.

The electrical contract for everything else: unasserted (H) is above 2.0 V and asserted
(L) below 0.8 V at the receiver [1] Table 5-1 p. 108; each card input may present up to
18 pF of AC load (2 pF of it the connector) [1] Table 5-2 p. 109; and the per-slot power
allowance is 2.0 A at +5 V, 0.175 A at +12 V and 0.150 A at −12 V — the famous 13.3 W
per card, with recommended filter capacitance of 1513 µF (+5), 536 µF (+12) and 698 µF
(−12) [1] Table 5-7 p. 114, §"Recommended Heat Dissipation Guidelines" p. 127. The
Quadra 900 breaks the budget: its supply "has enough power to support a total of two 25 W
cards and three 15 W cards," total not to exceed 95 W, and if the cards exceed it "the
Macintosh computer cannot be booted. During startup, the power supply attempts to turn
itself on but cannot, and it continues the attempt over and over" [1] p. 115,
[q900.md](../../machines/mcu/q900.md) §4.1. The IIsi is the opposite corner — one slot,
no borrowing: "Because the Macintosh IIsi has only one expansion slot, you cannot
'borrow' excess power from other slots that may not be filled" [1] p. 115.

## 4. Programming model

The NuBus is driven by software at exactly four places: the reset pulse at startup, the
Slot Manager's declaration-ROM search, the per-slot interrupt service, and the
block-transfer enables. Everything else is ordinary memory access.

### 4.1 Startup: reset, then the slot search

Startup order at the bus level: /RESET (§3.9), then the Slot Manager walk. The search is
specified precisely and is identical across the 68k machines [1] §"The Format Block"
pp. 156–157:

1. "When the computer is started up, the Slot Manager searches its slots for installed
   cards. For each slot it first searches NuBus addresses $FsFF FFFF through $FsFF FFFC
   (where $s is the slot number), looking for a valid ByteLanes value."
2. "If the Slot Manager finds a valid ByteLanes value, it verifies this value by
   examining the TestPattern field. Once the Slot Manager verifies the test pattern, it
   gets the CRC ... and checks the whole ROM to see if it matches."
3. "If everything matches, the Slot Manager recognizes the declaration ROM as valid. If
   no valid ByteLanes and TestPattern values are found, the Slot Manager stores a slot
   error in the corresponding sInfo record."

The searched addresses are lane-aligned by construction: the ByteLanes byte "always
occupies the highest address available in the byte lanes being used," and its position
among the four top bytes both identifies the lane set and fixes the format block's start
[1] §"ByteLanes" pp. 158–159. The field-by-field format, the fifteen legal ByteLanes
values and the CRC algorithm are [declaration-rom.md](declaration-rom.md) §2's subject.

An empty slot is handled entirely by the timeout of §3.8 — the search read faults
recoverably and the slot is recorded as empty. How the bridges handle the same probe, and
the per-machine slot-capability tables that decide which slot IDs are probed at all, is
machine material: on PDM the ROM consults a per-model table and reads exactly one byte,
$FsFFFFFF, per empty slot ([bart.md](../../machines/pdm/bart.md) §4.3).

### 4.2 sResources and drivers

A valid declaration ROM must contain "a format block, an sResource directory, and an
sResource (slot resource) for each function on the card plus one unique sResource called
a board sResource" [1] p. 142. The slot-resource model, in brief [1] §"sResources"
pp. 143–145:

- Every sResource is "a list of sResource entries terminated by a special EndOfList
  element. Each entry is a 32-bit record that consists of an 8-bit ID field and a 24-bit
  field that consists of either data or a signed offset to another structure."
- "There is typically one sResource for each function a card can perform plus one (and
  only one) unique sResource called a board sResource," which "provides a handy place to
  store card-related data ... the primary initialization routine that is called at
  system startup time, the board name, vendor identification."
- "If the board sResource is absent or invalid, the Slot Manager marks the slot where
  the card is located as invalid, and Slot Manager calls to that slot do not work."
- Each sResource carries an `sRsrcType` entry whose two longwords divide into Category
  (bits 30–16, bit 31 reserved for Apple), cType (15–0), DrSW (31–16 of the second
  longword) and DrHW (15–0) — a four-level hierarchy "Category, cType, DrSW, DrHW" that
  identifies the function, its subtype, its driver software interface and its specific
  hardware [1] §"The sRsrcType Entry" pp. 145–146. MacDTS assigns the values [1] p. 146.

The directory layout, the entry grammar, the video-specific sub-lists, the on-card
drivers and the PrimaryInit/SecondaryInit executable blocks are all
[declaration-rom.md](declaration-rom.md)'s territory (§3–§10); the end-to-end startup
sequence — search, validate, `PrimaryInit`, driver install, `SecondaryInit` — is its §11.

### 4.3 Slot PRAM

Each slot owns a 20-byte record in parameter RAM, initialized by the Slot Manager from
the board sResource's `PRAMInitData` sBlock on a cold boot (or when PRAM is invalid) and
used thereafter to restore the card's last-used configuration across reboots. The
record's layout and the Slot Manager's handling of it are covered with the board
sResource in [declaration-rom.md](declaration-rom.md) §5.2.

### 4.4 Interrupt service

A card that needs interrupts asserts its slot's /NMRQ and holds it low until serviced
(§3.7). The software side is platform plumbing, identical in shape everywhere: the
collector (GLUE-to-VIA2 on the IIx/IIcx, RBV on the IIci, OSS on the IIfx [2] pp. 102,
157; the AMIC slot bank on PDM, [bart.md](../../machines/pdm/bart.md) §3.6; the PSC-VIA2
window on the AV machines, [av.md](../../machines/av/av.md) §5.4) ORs the lines into one
processor interrupt and presents the per-slot lines as readable bits; the interrupt
handler reads the bits, calls the owning driver's service routine, and the driver makes
the card drop its line. On the II family the polling sequence is documented directly:
read the VIA2 Interrupt Flag register first, then "polls VIA2 Data register A to
determine which slot was the source" [2] p. 102.

The level-based contract has a corollary that bites card designers: a card whose line
asserts while its enable is masked has no way to be discovered except by polling, since
nothing in the bus latches the request.

### 4.5 Block-transfer enable

The bus lets any master attempt a block transfer, but a master should not: "if the
entries specifying block-transfer information are omitted, the bus master should assume
that the target board does not support block transfers and should not test for this
capability when the entries are not present" [1] p. 181. The capability longword
(`sBlockTransferInfo`) carries IsMaster, IsSlave, LockedTransfer and per-size
TransferSize bits [1] Table 8-8 p. 182; a block-incapable slave without early-/ACK
termination must publish the entry with the slave size bits zero [1] p. 181.

On the machine side, the main-logic-board *slave* path is gated per machine and per
slot: the MUNI performs the ROM-driven half automatically after `PrimaryInit` when the
card declares slave block size 4, and the `_SlotBlockXferCtl` trap sets the slot's
BlkAttmpt bit ([av.md](../../machines/av/av.md) §5.4); BART enables a slot's burst path
only for cards declaring slave sizes of 2 or 8 longwords, via `_HWPriv` selector 12
([bart.md](../../machines/pdm/bart.md) §4.4).

### 4.6 How real drivers use slot space

Two real display cards anchor the model in practice; both are dossiers with annotated
disassemblies of the cards' own code [4] [5].

**The Apple Macintosh Display Card 8•24 GC** (a six-slot-machine-era video card with an
onboard accelerator) declares its 64 KB declaration ROM on **byte lane 0 only** —
ByteLanes `$E1`, not the lane-3 `$78` of its non-GC siblings — so its 64 KB chip occupies
a 256 KB bus footprint [4]. Its driver reaches the card entirely through standard slot
space: the video driver programs card registers through the standard-slot window, and
the QuickDraw marshaller's fixed "gcp" command window sits at `card + $16C + $8C00`
(`$F9908C00` for a card in slot $9) — a standard-slot address inside minor-adjacent
space, chosen so the card works the same in any slot [4]. The card was exercised seated
in slot $9 of a Macintosh IIcx, where its declaration ROM is found by the standard
search of §4.1 (*observed* in recorded IIcx boot sessions, System 6.0.8 and 7.x [4]).

**The Apple Macintosh Display Card 24AC** (an Apple-branded Radius QuickDraw-accelerator
display card) demonstrates the 24/32-bit rule of §2.3 from the driver side: its
accelerator code "reaches the card's registers only in 32-bit addressing mode," wrapping
register access in `_SwapMMUMode` and gating its fast path on the `MMU32bit` low-memory
flag; the slot base is recovered by masking the Slot Manager device record's `baseAddr`
field with `$FF000000`, and all card apertures — framebuffer, engine alias at
`+$400000`, control registers near `$D00400` — are offsets into standard slot space [5]
(*observed* in the disassembled driver's own access protocol; the card hardware itself is
inferred from that protocol, not from a schematic [5]).

The two cards bracket the standard's range: one touches nothing outside the one-lane
declaration ROM contract and plain slot-space registers; the other builds a
super-slot-class memory map yet still routes every software-visible aperture through
standard slot space, keeping the 24-bit-compatible window the universal meeting point.

## 5. Quirks & errata

- **Reads and writes are all there is.** No I/O cycles, no interrupt transactions:
  "Reads and writes are the only operations used. I/O and interrupts are memory
  mapped" [1] Table 2-1 p. 38. A card that expects an interrupt-acknowledge cycle of
  another bus architecture will never see one.
- **The processor cannot address slot $0.** Any attempt "will immediately generate a bus
  error (/BERR) exception. No NuBus transaction will take place" [1] Table 7-3 p. 135 —
  yet cards address slot $0 constantly (it is how DMA reaches RAM), and the first Power
  Macintosh generation moved the bridge's own registers into slot $0's space and broke
  the rule from the processor side ([bart.md](../../machines/pdm/bart.md) §2.1, §5).
- **Bytes swap between the processor and the bus.** Byte lane $n$ of the processor
  connects to NuBus byte lane $n$, so significance inverts: $12345678 on the card is
  $78563412 to the Macintosh [1] pp. 136–137. "Only the bytes are swapped, not bits
  within bytes" [1] p. 137.
- **Data transfers are unjustified.** A byte always travels on its own lane regardless
  of the access mode [1] p. 59 — unlike the 68k bus, where the same data appears
  rotated onto other lanes depending on the transfer size.
- **Byte smearing exists on 68020/68030 machines and not on 68040 machines.** Hardware
  or software that relies on the duplication must be revised [1] pp. 138–139.
- **/RESET comes twice at power-on** (200 ms hardware, then a 33 µs software pulse from
  the ROM startup code), and soft restart is a double software pulse; "all assertions of
  /RESET (of any duration)" must be treated identically [1] pp. 52–53.
- **/PFW is the soft-power line.** A card can switch the whole machine on (drive high
  ≥ 1.5 s) or off (below 0.6 V), and the supply pulls it low 2 ms before the rails die
  [1] pp. 53, 110.
- **The bus timeout impersonates the missing slave.** 256 clocks (25.6 µs) after start,
  the main logic board acknowledges for whatever failed to [1] p. 63 — which is the only
  reason empty slots are enumerable.
- **Fair arbitration can starve the machine.** No CPU priority exists in contests; many
  active masters can starve RAM refresh, with "extremely slow or nonexistent updates to
  the screen" as the symptom — worst on the IIfx [1] p. 101.
- **The winner of a contest is chosen by connector strap, not performance.** Highest ID
  present wins; on a six-slot machine slot $E's card outranks the main logic board's own
  bids [1] pp. 98–99.
- **A parked master cannot enter a lock.** Bus ownership after releasing /RQST is free
  but lock-free; locking requires a fresh /RQST and contest [1] p. 105.
- **Bus locks are capped at four transactions**, and resource locks must be closed with
  attention-null; a card that monitors attention-resource-lock cycles must record them
  even when not itself addressed [1] pp. 101–104.
- **An attention cycle is a start cycle with /ACK low.** Slaves must qualify /START
  with /ACK high or they will decode every attention cycle as a transaction start [1]
  p. 64.
- **A non-block slave ACKs the first word of a block attempt normally** — "the master
  shall not assume that the data transfer did not take place" [1] p. 70 — and a 2X
  request of 32+ words to a 1X-only slave is refused with an immediate
  transfer-complete ACK, data ignored [1] p. 73.
- **One old card disables NuBus '90 for everyone.** The eight former −5.2 V pins are
  the '90 signals; a pre-'90 card that shorts them together in a Quadra 700/900 kills
  the new features "of all installed NuBus cards that use those lines," while "all the
  other features of both the old and new cards ... operate normally" [1] p. 112.
- **Parity is wired but dead**: /SP is pulled high and /SPV never driven by any Macintosh
  [1] pp. 57–58. The cache-coherency pins are equally inert [1] p. 58.
- **Slot $9's space is video space on the Quadra 700/900; slot $E's is on the IIsi** [1]
  Table 7-3 p. 135 — address space that looks like a NuBus slot but belongs to built-in
  video, with no connector.
- **Physical slot order reverses on the Quadra 700/900**: IDs descend left to right,
  against the rest of the family's ascending order [1] p. 128. Addressing never cares;
  people reading a chassis do.
- **13.3 W per slot is a hard budget** — exceeded only by borrowing adjacent slots, by
  oversized Quadra-900 cards (19 W per slot, 95 W total), and never on the single-slot
  IIsi [1] pp. 114–115.
- **Slot $0's I/O corner is off-limits by policy.** The alias window $F0000000–$F070FFFF
  is marked "Do not access from a slot card" [1] Table 7-3 p. 135, and Apple warns that
  "access to anything other than ROM or RAM will probably not be supported on future
  systems" [1] p. 33 — a promise the Power Macintosh generation kept by refusing card
  access to peripherals altogether ([bart.md](../../machines/pdm/bart.md) §1.1).

## 6. Open questions

1. **The IEEE text beyond Apple's restatement.** Every timing, encoding and protocol
   statement on this page is sourced from Apple's two books [1] [2], which restate
   ANSI/IEEE Std 1196-1990 [3] for card designers; the standard itself is not in the
   evidence corpus. Whether the standard specifies anything material that Apple's
   restatement omits (further transfer-status encodings, load rules for more than 16
   slots, the triple-height card) is unverified.
2. **The exact /RESET deviation.** Apple states only that "there is a slight deviation
   of the duration of the /RESET signal from that specified in the IEEE 1196 NuBus
   standard" [1] p. 52 — which direction, and for which reset paths, is not stated.
3. **The 33 µs software reset pulse's exact width per machine.** The 200 ms/33 µs/3 µs
   figures are the 1992 book's nominal values [1] pp. 52–53; whether every bridge
   generation reproduces them (and what BART's software pulse width is — its own
   open questions, [bart.md](../../machines/pdm/bart.md) §6) is unmeasured.
4. **The timeout's per-machine uniformity.** The 256-clock/25.6 µs figure is stated for
   "the Macintosh main logic board" generically [1] p. 63 and confirmed for the MUNI
   ([av.md](../../machines/av/av.md) §5.4); each bridge's own timeout figure — in
   particular BART's — is undocumented ([bart.md](../../machines/pdm/bart.md) §6).
5. **Whether any production card ever drove /SP, /SB0–/SB1 or the /CMx lines.** All
   three groups are specified, bused and terminated, and none is driven or monitored by
   any main logic board [1] pp. 57–58, 113; whether third-party silicon ever used them
   is unknown, and a card's behavior when it does (on a machine that pulls /SP high)
   is undefined.
6. **The /NMRQ pull-up values per machine.** The per-slot interrupt lines are open
   collector with main-logic-board pull-ups; the card-design book does not state the
   value, and it differs per platform (4.7 kΩ on the PDM boards, per their schematics —
   [bart.md](../../machines/pdm/bart.md) §3.6). The II-family values are not in the
   book evidence.
7. **The STDBYPWR current.** The Quadra 900's standby pin supplies "small current at
   +5 V" [1] Table 5-5 p. 113 with no amperage stated.
8. **Whether −5.2 V was ever supplied by a card.** The original-spec pins "are wired
   together, but not supplied with power from the computer. This voltage could be
   supplied by a card, in which case −5.2 V would be available to all cards" [1] Table 2-2
   p. 42 — whether any production card did so is unknown.
9. **Whether the Quadra 700/900 main logic board accepts 1X block transfers as a
   slave.** The books exclude only *double-rate* transfers to and from main memory [1]
   p. 71, leaving single-rate blocks between a card master and main memory ambiguous;
   the later MUNI/BART bridges re-enabled block transfers to RAM explicitly behind
   per-slot gates ([av.md](../../machines/av/av.md) §5.4,
   [bart.md](../../machines/pdm/bart.md) §4.4), which suggests the Quadra family did
   not (*inferred — unverified*).
10. **Which slot IDs each ROM actually probes.** The 68k Slot Manager searches the
    machine's documented slot set ($9–$E in six-slot machines, fewer in others [1]
    Table 7-1 p. 133); the PDM new ROM extended minor slots to $1–$E
    ([bart.md](../../machines/pdm/bart.md) §3.1). The exact probed set per machine per
    ROM revision is machine material and not established for every model.
11. **Where the byte swap physically happens.** The books place "byte-lane routing"
    in the bridge and transceivers ([2] pp. 465–466, [1] p. 137) without pinning down
    which chip performs it per generation (NuChip vs. discrete transceivers vs. BIU2);
    a card designer does not need the answer, but a board-level re-implementation does.
12. **The /TM0A//TM1A fault-status lines beyond the II family.** VIA2 records the NuBus
    error type on the II, IIx, IIcx and SE/30 [2] p. 156; whether any later collector
    (RBV, OSS, PSC, AMIC) preserves the fault type for software is not documented.
13. **The reserved attention code** (/TM1 L, /TM0 H during an attention cycle, Table
    3-3) is marked "Reserved" with no semantics given [1] p. 64; nothing on any
    Macintosh generates or consumes it.

## References

1. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third
   edition, Addison-Wesley Publishing Company, 1992 — Chapter 1 "Overview of Macintosh
   Computers With the NuBus Interface" pp. 13–35 (machines list p. 13; Table 1-1 p. 14;
   NuChip/NuChip30/BIU30 and the IIsi adapter p. 31; bus interface architecture and
   state machines pp. 33–35); Chapter 2 "NuBus Overview" pp. 37–50 (Table 2-1 design
   objectives p. 38; NuBus elements p. 39; NuBus '90 features pp. 41–42; signal
   classifications Tables 2-2/2-3 pp. 42–43; clock timing p. 44; terminology Table 2-4
   pp. 45–50); Chapter 3 "NuBus Data Transfer" pp. 51–96 (utility signals pp. 52–53;
   slot identification signals pp. 54–55; determinacy p. 55; transfer mode coding
   Table 3-1 p. 57; parity pp. 57–58; unjustified transfers p. 59; read and write
   transactions pp. 60–62; status coding and the 25.6 µs timeout Table 3-2 p. 63;
   attention cycles pp. 63–64; interrupt operations p. 65; 1X block transfers pp. 66–70
   with Table 3-4 p. 66; 2X block transfers pp. 71–80 with Table 3-5 p. 72); Chapter 4
   "NuBus Arbitration" pp. 97–106 (overview p. 98; logic equations pp. 99–100; timing
   and starvation warning p. 101; locking limits pp. 101–102; bus locking p. 103;
   resource locking pp. 103–104; bus parking p. 105); Chapter 5 "NuBus Card Electrical
   Design Guide" pp. 107–118 (logical states Table 5-1 p. 108; drive and load Table 5-2
   p. 109; /PFW pp. 110–111; connector pin assignments Tables 5-3/5-4 pp. 111–112;
   NuBus '90 signals Table 5-5 p. 113; power supply Table 5-6 p. 113; power budget
   Table 5-7 p. 114; 13.3 W and Quadra 900 budgets pp. 114–115; data-transfer timing
   Table 5-8 p. 117; arbitration timing Table 5-9 p. 118); Chapter 6 "NuBus Card
   Physical Design Guide" pp. 119–130 (card description p. 120; heat dissipation p. 127;
   slot ordering p. 128; IIsi adapter pp. 129–130); Chapter 7 "NuBus Card Memory Access"
   pp. 131–139 (address space p. 132; Table 7-1 slot IDs p. 133; super slot space p. 133;
   24-bit translation Table 7-2 p. 134; NuBus address mapping Table 7-3 p. 135; slot
   allocations Table 7-4 p. 136; bit and byte structure pp. 136–137; byte smearing
   pp. 138–139); Chapter 8 "NuBus Card Firmware" pp. 141–182 (Slot Manager and
   declaration ROM pp. 141–143; sResources pp. 143–145; sRsrcType pp. 145–146; format
   block and the startup search pp. 156–157; ByteLanes pp. 158–159 with Table 8-2 p. 159;
   block-transfer information pp. 181–182 with Table 8-8 p. 182); Appendix D PAL listing
   p. 591.
2. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition,
   Addison-Wesley Publishing Company, 1990 — Chapter 3 pp. 101–102 (NuBus slot
   interrupts through the GLUE/RBV/OSS; SE/30 pseudo-slot $E video and PDS emulation of
   slots $9–$B); Chapter 4 pp. 155–157 (VIA2 functions on the II, IIx, IIcx and SE/30:
   /SLOTIRQ, the per-slot interrupt register, /BUSLOCK, /TM0A//TM1A fault status; the
   IIci RBV); Chapter 14 "Expansion Interfaces" pp. 461–466 (the NuBus expansion
   interface; Texas Instruments licensing note p. 462; Figure 14-3 connector pinout
   p. 463; Tables 14-8/14-9 signal assignments pp. 464–465; the NuBus block diagram and
   the NuChip/NuChip30/BIU30 state machines pp. 465–466).
3. Institute of Electrical and Electronics Engineers, *Standard for a Simple 32-Bit
   Backplane Bus: NuBus*, ANSI/IEEE Std 1196-1990 (NuBus '90) — the standard Apple's
   Quadra-era implementation follows [1] p. 38; cited here for the 1990 revision's
   identity, with all restated content sourced from [1] and [2].
4. Apple Macintosh Display Card 8•24 GC declaration ROM, Apple part 341-0266, version
   1.1 (64 KB image, 68k code) — annotated disassembly covering the format header
   (ByteLanes `$E1`, lane 0), the sResource directory, the board and 57 functional video
   sResources, the PrimaryInit and SecondaryInit sExecBlocks, the on-card
   `.Display_Video_Apple_MDCGC` driver, and the recorded boot behavior of the card
   seated in slot $9 of a Macintosh IIcx (System 6.0.8 and System 7.x sessions).
5. Apple Macintosh Display Card 24AC (Radius QuickDraw Accelerator, v1.2, c. 1994) —
   annotated 68k disassembly of the accelerator software's `INIT`/`QCOD`/`QDPA`
   resources, from which the card's slot-space map is reconstructed: 32-bit-mode-only
   register access gated on the `MMU32bit` flag, slot base derived from the Slot Manager
   device record's `baseAddr` masked with `$FF000000`, and the framebuffer/engine/register
   apertures laid out in standard slot space.
