# The PCI bus in the Power Macintosh

The PCI bus is the expansion bus of the second-generation Power Macintosh platform: the 32-bit,
33 MHz, multiplexed backplane standardized by the *PCI Local Bus Specification* and wired into the
TNT machines (7200/7500/8500/9500 and their 1997 follow-ons), the Catalyst 7200 sibling, and the
Apple Network Server. This page documents PCI *as the Power Macintosh implements it* — the three
address spaces the host bridges expose, the configuration-space architecture and the header the
firmware programs, the memory and I/O windows, and the interrupt path from a slot's pins to the
single interrupt collector — the cross-machine contract every card, ROM and driver on the
platform lives inside. The PDM generation (6100/7100/8100) has no PCI: it is a NuBus platform whose
bridge is [bart.md](../../machines/pdm/bart.md)'s subject.

The bridges and controllers are their own pages, cited here by section and never restated:
[bandit.md](../../machines/tnt/bandit.md) is the AR-to-PCI host bridge (its register file, its
configuration ports, its windows and error behavior);
[grand-central.md](../../machines/tnt/grand-central.md) is the I/O controller and interrupt
collector; [tnt.md](../../machines/tnt/tnt.md) is the platform;
[ans.md](../../machines/ans/ans.md) is the Network Server's six-slot variant; the SCSI card
contract is [sym53c8xx.md](../scsi/sym53c8xx.md)'s, and the card pages live under
[cards/](cards/).

**Contents:**

1. [Overview](#1-overview) — what the bus is, which machines carry it, division of labor, the
   bridge population, electrical and mechanical form
2. [Register file](#2-register-file) — the address-space architecture: the three spaces, memory
   and I/O windows, configuration-space addressing, the standard header and the firmware's
   command-register policy, the IEEE 1275 address format, the bus and device properties
3. [Behaviour](#3-behaviour) — cycle types, arbitration, transactions and bursting, byte order,
   interrupts, errors, reset
4. [Programming model](#4-programming-model) — the Open Firmware startup process, expansion ROMs
   and FCode, BAR sizing and assignment, the Name Registry and driver matching, interrupts in
   Mac OS, non-Apple operating systems
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the bus is

The PCI Local Bus is "a high-performance local bus standard" adopted by Apple "to achieve maximum
compatibility with PCI-compliant devices and plug-in cards" [1] §"The Macintosh Implementation of
PCI" p. 36. The Power Macintosh implementation "is designed to comply with the PCI Local Bus
Specification, Revision 2.1," a compliance Apple scopes to "signal types and pin assignments, bus
protocols including arbitration, signal electrical characteristics and timing, configuration data
and card expansion ROM formats, and plug-in card mechanical specifications" [1] p. 36. The
individual machine Developer Notes, written a revision earlier, state the card-facing contract as
"standard PCI cards as defined by the *PCI Local Bus Specification*, Revision 2.0" with 5 V
signaling and the standard ISA fence [3] p. 49, [4] p. 33; the discrepancy is §6.10's.

Structurally the bus is "a nonsplit bus with 32-bit multiplexed address and data" [3] p. 49, [4]
p. 33, running at a 33 MHz system clock [3] p. 49, [4] p. 33 — one load per signal, no enforced
termination rules, centralized arbitration, and three addressing spaces where NuBus had one [1]
Table 1-1 p. 35. Where the NuBus model was that any card may master the bus and address any peer,
the PCI model on this platform is that every bus hangs off a host bridge: "The most basic function
of the PCI host bridge is to translate between PowerPC processor bus cycles and PCI bus cycles"
[1] §"PCI Host Bridge Operation" p. 39. The platform's system architecture "supports up to four
peer PCI bridge connections to the main processor bus" [1] p. 36 — the ARBus (Apple RISC bus),
Apple's implementation of the PowerPC processor bus [1] Figure 1-1 p. 37 — "although to date no
more than two built-in PCI buses have been included in shipping Macintosh configuration" [1] p.
37. The observed maximum is two per machine: Chaos plus Bandit 1 on the 7500/8500, Bandit 1 plus
Bandit 2 on the 9500 and the Network Server [bandit.md](../../machines/tnt/bandit.md) §1.4.

The platform boundary matters for everything below. The machines this page covers are the "Old
World" PCI Macintoshes — the ones whose Open Firmware and Mac OS Toolbox live in a 4 MB boot ROM
and whose built-in I/O hangs behind the Bandit/Grand Central pair
([tnt.md](../../machines/tnt/tnt.md) §1.3, §2.1). The 1999 revision of the PCI book also
describes the NewWorld architecture (iMac and later Power Macintosh G3), which moves ROM into RAM
and replaces the interrupt and NVRAM models [1] §"The Macintosh ROM and The NewWorld Architecture"
pp. 70–74; where a fact below is NewWorld-specific the text says so, and NewWorld as a whole is
out of this page's scope.

### 1.2 Which Macintoshes carry it

| Machine | Platform | Host bridges | PCI slots | Evidence |
|---|---|---|---|---|
| Power Macintosh 7200 | Catalyst | Bandit 1 (+ Chaos on the 7300 form) | none | [tnt.md](../../machines/tnt/tnt.md) §6.4 |
| Power Macintosh 7500 | TNT | Chaos + Bandit 1 | 3 | [3] p. 49 |
| Power Macintosh 8500 | TNT | Chaos + Bandit 1 + Bandit 2 | 3 | [3] p. 49; [bandit.md](../../machines/tnt/bandit.md) §1.2 |
| Power Macintosh 9500 | TNT | Bandit 1 + Bandit 2 | 6 | [4] p. 33 |
| Power Macintosh 7300/7600/8600/9600 | TNT follow-ons | as per form | 3 or 6 | [tnt.md](../../machines/tnt/tnt.md) §6.5 |
| Apple Network Server 500/700 | TNT-derived | Bandit 1 + Bandit 2 | 6 | [ans.md](../../machines/ans/ans.md) §3.3 |

Two columns deserve a note. The bridge count is a device-tree fact — the one ROM that drives the
whole family constructs one or two `bandit` nodes from the machine-identity register, and the
8500's second bridge is assigned no slots in any document [bandit.md](../../machines/tnt/bandit.md)
§1.2. And the Catalyst 7200 carries the Bandit silicon without a slot in sight: its display path
is Platinum, not the Chaos/Control chain, and the bus hosts only on-board devices
[tnt.md](../../machines/tnt/tnt.md) §6.4 — the clearest evidence that on this platform the *bus*
and the *slot* are independent concepts.

### 1.3 Division of labor

The bus page states what is true of every slot on every machine of the platform; everything
specific to a part belongs to the part's page:

| Function | Owner | Where |
|---|---|---|
| Processor-to-PCI translation: config ports, address/data windows, byte order, posting, errors | the Bandit bridge | [bandit.md](../../machines/tnt/bandit.md) §2, §3 |
| PCI bus arbitration priority ordering | a gate array outside the bridge | [bandit.md](../../machines/tnt/bandit.md) §3.7 |
| Interrupt collection, 32 sources, the single CPU line | Grand Central | [grand-central.md](../../machines/tnt/grand-central.md) §2.2, §3.2–§3.5 |
| The physical address map, bridge slices, RAM/ROM decode | the TNT platform | [tnt.md](../../machines/tnt/tnt.md) §3 |
| Six slots, IDSEL remap, AIX interrupt contract | the Network Server | [ans.md](../../machines/ans/ans.md) §3.3, §5.1–§5.2 |
| A real card's configuration-space contract | the card pages | [sym53c8xx.md](../scsi/sym53c8xx.md); [cards/](cards/) |
| Card identity, BAR sizing, driver matching | Open Firmware and the expansion ROM | §4 below |

### 1.4 The bridge population and the address-space scheme

The architecture allocates a fixed control slice per host bridge and opens the low half of the
32-bit space to PCI memory: "PCI host bridge 0 control" at $F0000000–$F1FFFFFF, bridge 1 at
$F2000000–$F3FFFFFF, bridge 2 at $F4000000–$F5FFFFFF, bridge 3 at $F6000000–$F7FFFFFF, system
control at $F8000000–$F8FFFFFF, with $80000000–$EFFFFFFF and $F9000000–$FEFFFFFF "available to
PCI expansion cards" [1] Table 2-1 p. 59. More than 1.8 GB of address space is allocated for PCI
memory space [1] p. 58. Apple attaches its own caveat to the table: "The information in Table 2-1
is for illustrative purposes only. Neither hardware nor software should rely on the address map
described therein" [1] p. 59 — the operative description of any real machine is its Open Firmware
`ranges` property ([bandit.md](../../machines/tnt/bandit.md) §3.4;
[tnt.md](../../machines/tnt/tnt.md) §3.1–§3.3). A system may also carry PCI-to-PCI bridges "in any
configuration to create up to 256 PCI buses," but "properties that must be stored on disk or in
NVRAM between system startups can be addressed only to five levels of PCI-to-PCI bridges behind
each host bridge," capping fully supported hardware at 24 buses [1] p. 58; the Old World machines
ship with none, so type-1 configuration cycles exist in the mechanism but reach no hardware until
a bridge card is installed [bandit.md](../../machines/tnt/bandit.md) §3.1.

### 1.5 Electrical and mechanical form

The Old World slots are 32-bit, 5 V signaling, 33 MHz parts in the standard ISA fence: "The cards
are required to use the 5 V signaling standard and to use the standard ISA fence described in the
specification" [3] p. 49, [4] p. 33. The mechanical spec is enforced by dimension: "short PCI
cards for Macintosh computers should not be longer than the 6.875-inch (174.63 mm) dimension
specified. In some Macintosh models, 6.875 inches represents the maximum length for a PCI card,
while in other models cards may be any length up to 12.283 inches" [1] p. 54.

The signals the slots carry are the required set plus the optionals Apple kept; each machine's
Developer Note prints the same list (Table 4-8 in the 7500/8500 note, Table 4-6 in the 9500's):

| Signal | Role | Notes |
|---|---|---|
| AD[31:0] | address and data, multiplexed | [3] Table 4-8 p. 50 |
| C/BE#[3:0] | bus command and byte enables, multiplexed | [3] Table 4-8 p. 50 |
| PAR | parity over AD and C/BE | [3] Table 4-8 p. 50 |
| FRAME#, IRDY#, TRDY#, STOP#, DEVSEL# | transaction framing and handshake | [3] Table 4-8 p. 50 |
| IDSEL# | initialization device select — used during configuration | [3] Table 4-8 p. 50 |
| REQ#/GNT# | arbitration request and grant per master | [3] Table 4-8 p. 50 |
| CLK | 33 MHz; the rising edge times all transactions | [3] Table 4-8 p. 50 |
| RST# | reset to a known state | [3] Table 4-8 p. 50 |
| INTA#/INTB#/INTC#/INTD# | interrupt request pins — "wired together on each slot" | [3] Table 4-8 p. 50; §3.5 |

The unsupported optionals are identical in both Developer Notes: "64-bit bus extension signals,
cache support signals, JTAG (boundary scan) signals, /LOCK signal, error reporting signals /PERR
and /SERR" [3] p. 50, [4] p. 33 — and the PCI book adds that LOCK#, PERR#, SERR#, SBO#/SDONE and
JTAG are "Not used by the Macintosh system," with no cache coherency (snooping) across the PCI bus
[1] Table 1-2 p. 38. The IDSEL lines are "provided by resistive connections to AD lines" [1]
Table 1-2 p. 38 (§2.4).

Power is allocated per *machine*, not per slot, and the PCI book is explicit about the consequence:
"The PCI specification allocates power per slot, but the Macintosh implementation contains one
power allocation for all slots. For example, a three-slot Power Macintosh computer has 9 A of 5 V
power or 6 A of 3.3 V power available for PCI cards, which can be installed in any combination
among the slots. Apple recommends that cards stay within the proportional allotment: 3 A for 5 V
and 2 A for 3.3 V cards" [1] Table 1-2 note 2 p. 38. The per-machine budgets: 50 W across the
7500/8500's three slots [3] p. 49; 90 W across the 9500's six [4] p. 33; 50 W (500) or 90 W (700)
on the Network Server, with the per-card PCI limit of 25 W always applying
([ans.md](../../machines/ans/ans.md) §3.3).

One connector beside the slots is part of the bus's mechanical story: the DAV connector, a 60-pin
dual-row header near a PCI slot "by means of a cable and plug" for which "a PCI expansion card can
pick up the A/V signals" — the 4:2:2 unscaled digital video input and the digital audio input,
transferred "without passing through the PCI bus" [3] §"DAV Connector" pp. 50–51. It is a
side-channel, not a bus extension: a card using it "can access data and perform PCI bus
transactions independently" [3] p. 51.

## 2. Register file

The "register file" of a bus is its address-space architecture: the spaces that exist, how an
address in each is formed, and the configuration-space surfaces software actually touches. The
host bridge's own registers — the two configuration ports and the bridge's header at IDSEL device
11 — are [bandit.md](../../machines/tnt/bandit.md) §2's, not repeated here.

### 2.1 The three address spaces

PCI defines three addressing spaces, and the Macintosh implements all of them [1] Table 1-2 p. 38:

| Space | Bus cycle commands | Reached by software via | Notes |
|---|---|---|---|
| Memory | memory read/write, read line, read multiple, write and invalidate | direct processor access — "supported through the bridge transparently" [1] p. 41 | no device may address below 1 MB [1] Table 1-2 note 1 p. 38 |
| I/O | I/O read, I/O write | memory-mapped section in each bridge's control space [1] p. 42 | 23-bit space provided; allocator targets 16 bits (§2.3) |
| Configuration | configuration read, configuration write | the bridge's indirect config mechanism [1] p. 42; [bandit.md](../../machines/tnt/bandit.md) §2.2–§2.3 | geographical, per-function, 64-byte header (§2.4–§2.5) |

Two further cycle types carry no address at all: special cycles (writes, broadcast, data conveys
the message) and interrupt-acknowledge cycles (reads, "intended to support interrupt control
hardware associated with PCI devices") [2] §2.1.4.5 p. 4. Because the PowerPC architecture has no
equivalent of I/O, configuration, interrupt-acknowledge or special cycles, "Macintosh bridges
create these additional address spaces and cycle types by accessing memory-mapped regions of the
bridge control space," and drivers reach them through interface routines rather than raw loads
and stores [1] §"PCI Bus Cycles" p. 59. Memory space alone is directly addressable, and "to ensure
compatibility with future Power Macintosh computers, software must use these routines to access
PCI spaces other than PCI memory space" [1] p. 41.

### 2.2 PCI memory space

More than 1.8 GB of the 32-bit space is allocated to PCI memory [1] p. 58, split between the
relocatable card windows below the F segment ($80000000/$90000000 on the TNT machines, where card
BARs land) and the pass-through windows inside the F segment where Grand Central lives — the exact
placement, per bridge, is the `ranges` property's and therefore
[bandit.md](../../machines/tnt/bandit.md) §3.4's statement. Two bus-level rules ride on top:

- **Nothing lives below 1 MB.** "The Power Macintosh implementation does not support devices
  that address memory space below 1 MB" [1] Table 1-2 note 1 p. 38 — the sentence that kills
  legacy VGA decode: "You can never hard decode addresses below 1 MB (for example, VGA addresses
  A0000 through BFFFF) because the Power Macintosh implementation of PCI does not support devices
  that address this space" [1] p. 55.
- **Hard decoding is a declared liability.** A card that decodes a fixed address "gives no
  indication to the system that it has done so," which "cripples the ability of system software to
  resolve address conflicts between devices"; a card that cannot stop hard decoding "must" put a
  fixed-address entry in its `reg` property, and "it is essential that devices which hard decode
  address spaces after reset provide a method to turn off their hard-decoding logic," executed in
  FCode before the device enters its `reg` property [1] §"Hard Decoding Device Address Space" p. 55.

### 2.3 PCI I/O space

The specification requires a 16-bit minimum I/O space; "the first implementation of the PCI bus
for Power Macintosh provides a 23-bit I/O space, although the Macintosh address allocation
software tries to fit all I/O address space requests within the 16-bit minimum size" [1] p. 42.
The interface to it "uses a memory-mapped section in each PCI host bridge's control space," and
"the system determines which PCI host bridge and bridge area to use when accessing each specific
card" [1] p. 42 — the per-bridge 8 MB I/O window at the bridge base is
[bandit.md](../../machines/tnt/bandit.md) §3.4's. Apple's performance advice is to avoid it:
"Because PCI allocations in I/O space are highly fragmented, high-performance interfaces should
try to use the PCI memory space instead of I/O space" [1] p. 42. I/O transactions never burst —
"the IB chip does not burst PCI I/O Read nor burst PCI I/O Write commands" [1] p. 51 — and the
`AAPL.address` property that turns an `assigned-addresses` entry into a dereferenceable logical
I/O address, plus the `SynchronizeIO` ordering rule between accesses, are
[bandit.md](../../machines/tnt/bandit.md) §4.4's statement of [1] pp. 454, 51.

### 2.4 Configuration space: the address

Configuration space is "effectively geographical": a function's configuration registers are
selected by its physical position — the slot, or its position in a tree of PCI-to-PCI bridges [2]
§2.1.4 p. 3. The address has four fields [2] §2.1.4 pp. 3–4:

| Field | Width | Meaning |
|---|---|---|
| Bus number | 8 bits | unique per bus within a PCI domain, assigned during initialization |
| Device number | 5 bits | selects one IDSEL line on the bus — "effectively selects a particular slot" |
| Function number | 3 bits | one of up to 8 independent register sets in one device; single-function devices must use zero |
| Register number | 8 bits | byte offset within the function's register set |

The 5-bit device field can name 32 devices, but "electrical limitations restrict the number of
devices on an individual PCI bus to fewer than the 32" [2] §2.1.4.2 p. 3. The binding also pins
the two facts that make the Macintosh implementation distinctive:

- **IDSEL is not a binary field on the wires.** "Some PCI bus controllers use the same physical
  wires for the IDSEL lines and higher-numbered address lines, thus... the 5-bit field is decoded
  to a 'one of n' select that asserts exactly one upper address line. This fact does not affect
  the logical representation of the Device Number as a 5-bit binary-encoded field" [2] §2.1.4.2
  p. 3. On this platform the IDSEL pins are "provided by resistive connections to AD lines" [1]
  Table 1-2 p. 38, and the bridge drives the one-hot form on the address lines of a type-0 cycle —
  the address encodings and the device population (nothing below device 11, slots at 13–16,
  Grand Central at 16, the per-machine IDSEL maps) are
  [bandit.md](../../machines/tnt/bandit.md) §3.1–§3.2's and
  [ans.md](../../machines/ans/ans.md) §3.3's.
- **The bus-number field is software's view, not the wires.** "In some systems, special registers
  are used to generate Configuration Space cycles... the hardware method for specifying the Bus
  Number and Device Number is system-dependent," and the binding describes them as binary fields
  only because that representation "is capable of representing the entire possible space" [2]
  §2.1.4 p. 3.

The low two address bits are not byte addressing: "the PCI bus uses the two low address bits
(AD[1::0]) not to identify the particular byte to be accessed, but instead to convey additional
information about the data transfer"; bytes are selected by the byte enables, and Open Firmware
addressing re-imposes the software view in which "addresses identify individual 8-bit, 16-bit, and
32-bit registers" [2] §2.1.4.6 p. 4. On the Macintosh the two low bits of a configuration
*register* offset ride in the address of the data-port access itself
([bandit.md](../../machines/tnt/bandit.md) §2.3).

### 2.5 The standard configuration header

The first 64 bytes of a function's configuration space are the standard header (type 0 layout;
read-only fields shaded in [1] Figure 4-1 p. 96). The table below is the header layout with the
action the Old World startup firmware performs on each register during the Open Firmware startup
process [1] §"Register Actions" pp. 96–102:

| Offset | Register | Firmware action |
|---|---|---|
| $00 | Vendor ID | read; stored as `vendor-id`; the `xxxx` of the generated `pcixxxx,yyyy` name when the card has no FCode and no subsystem ID [1] pp. 96–97 |
| $02 | Device ID | read; stored as `device-id`; the `yyyy` of the generated name [1] p. 97 |
| $04 | Command | written per the bit policy of §2.6 [1] p. 97 |
| $06 | Status | bits 10–9 (DEVSEL speed) stored as `devsel-speed`; bit 7 (fast back-to-back capable) noted, creating `fast-back-to-back` if nonzero; no other action [1] p. 98 |
| $08 | Revision ID | read; stored as `revision-id` [1] p. 98 |
| $09 | Class code | read; stored as `class-code` [1] p. 98 |
| $0C | Cache Line Size | "set for all devices as specified in the PCI Specification 2.1. This value may change from Macintosh platform to Macintosh platform for various performance reasons" [1] p. 98 |
| $0D | Latency Timer | set per PCI 2.1, platform-dependent value [1] p. 98 |
| $0E | Header Type | bits 6–0 select the $10–$3F layout (standard vs. PCI-to-PCI bridge); bit 7 set means the firmware probes for multiple functions [1] pp. 98–99 |
| $0F | BIST | "No action is taken" [1] p. 99 |
| $10–$24 | Base address registers | sized by the all-ones write probe (§4.3); cleared to zero after sizing; programmed from `assigned-addresses` [1] p. 99 |
| $28 | Subsystem Vendor ID | if nonzero, stored as `subsystem-vendor-id`; can supply the generated name's `xxxx` [1] p. 100 |
| $2C | Subsystem ID | if nonzero (and the vendor property exists), stored as `subsystem-id`; the generated name's `yyyy` [1] p. 100 |
| $30 | Expansion ROM Base | probe-time temporary mapping, FCode discovery, then disabled and cleared — §4.2 [1] pp. 100–101 |
| $3C | Interrupt Line | "No action is taken... It has no meaning for Power Macintosh computers because interrupts are OR-combined per slot in hardware... This register contains no useful information for drivers" [1] p. 101 |
| $3D | Interrupt Pin | read; if nonzero, the value appears in the `interrupts` property [1] p. 101 |
| $3E | Min_Gnt | read; stored as `min-grant` [1] pp. 101–102 |
| $3F | Max_Lat | read; stored as `max-latency` [1] p. 102 |

A PCI-to-PCI bridge function carries the type 1 header instead, with its own field set — primary/
secondary/subordinate bus numbers, I/O and memory base/limit pairs, bridge control — mapped in
[1] Figure 4-2 p. 103 and configured by the firmware with its own register settings [1] §"Register
Settings" pp. 103–110; the book notes that bridge headers read the command register's Memory
Write and Invalidate and Special Cycle bits as zeros, because a bridge "cannot respond to Special
Cycles" and only propagates write-and-invalidate as an agent [1] p. 104.

### 2.6 The Command register bit policy

The firmware's Command register policy is specified bit by bit, and several of the choices are
load-bearing platform facts rather than generic PCI practice [1] pp. 97–98:

| Bit | Name | Firmware action | Why |
|---|---|---|---|
| 9 | Fast Back-to-Back Enable | set **if all** devices on the bus are fast back-to-back capable | the `fast-back-to-back` property of every node is consulted |
| 8 | SERR# Enable | cleared to 0 for all devices | "the Power Macintosh system doesn't respond to SERRs" |
| 7 | Wait Cycle Control | cleared to 0 for all devices | — |
| 6 | Parity Error Response | cleared to 0 for all devices | — |
| 5 | VGA Palette Snoop | cleared to 0 for all devices | — |
| 4 | Memory Write and Invalidate Enable | set to 1 for all devices | "the Power Macintosh system fully supports this command type and optimizes for it" |
| 3 | Special Cycle Enable | set to 1 for all devices | "the Power Macintosh system can generate special cycles" |
| 2 | Bus Master Enable | set to 1 for all devices | "the Power Macintosh system supports masters in all PCI locations" |
| 1 | Memory Space Enable | **cleared** before an operating system is loaded | the driver must set it, "after checking that the memory resources required... appear in the device's `assigned-addresses` property" |
| 0 | I/O Space Enable | **cleared** before an operating system is loaded | same contract, for I/O space |

The two cleared enables are the bus's contribution to the platform's boot discipline: a card is
addressable but silent until a driver that has verified its assigned resources turns its decodes
on. The policy for PCI-to-PCI bridges differs only in bits 4 and 3 — read-only zeros on bridges —
and in bits 1 and 0, which are written *set* so memory and I/O cycles "pass through the bridge
transparently" [1] p. 104. Note the tension with the interrupt-side contract: bit 8 is cleared
because nothing listens to SERR# (§3.6), not because parity is disabled.

### 2.7 The IEEE 1275 PCI address format

Everything the firmware, the device tree and the drivers exchange about a PCI address uses the
Open Firmware numerical representation: three cells for the address, two for the size [2] §3.1.1
p. 11. The `phys.hi` cell packs the address's entire identity [2] §2.2.1.1 p. 4:

```
phys.hi cell: npt000ss bbbbbbbb dddddfff rrrrrrrr
phys.mid cell: 32-bit unsigned
phys.lo cell:  32-bit unsigned
```

| Field | Meaning |
|---|---|
| n | 1 if the address is non-relocatable (hard-decoded), 0 if relocatable |
| p | 1 if the region is prefetchable ("reflects the state of the P bit in the corresponding hardware Base Address register") |
| t | 1 if the address is aliased (non-relocatable I/O, ten-bit alias), below 1 MB (memory), or below 64 KB (relocatable I/O) |
| ss | the space code — configuration, I/O, 32-bit memory, 64-bit memory |
| bbbbbbbb | the 8-bit bus number |
| ddddd | the 5-bit device number |
| fff | the 3-bit function number |
| rrrrrrrr | the 8-bit register number |

Hard-decoded spaces are represented with the n bit set and base-register field zero; a device
allowed to alias its hard-decoded I/O addresses by "ignoring all but the lower 10 bits" sets the
t bit to include all aliases in one entry [2] §2.1.3 pp. 2–3. The text representation a user sees
in a device path is the unit address: `DD` or `DD,FF` for a configuration-space address, with the
bus number deliberately *absent* — "the pathname of a particular device would depend on the
particular assignment of bus numbers" if it were present, so a function's path depends only on its
physical position [2] §2.2.1.3 p. 7. The unit address of a card node is therefore its IDSEL
device number (in hex): the 9500's ATI card sits at `@D`, the Network Server's slots at `@D`
through `@10` ([bandit.md](../../machines/tnt/bandit.md) §3.2.1; [ans.md](../../machines/ans/ans.md)
§3.3 — the decimal/hex trap is the unit-address entry of §5).

### 2.8 The bus and device properties

The properties a PCI bus node and a PCI device node carry are the binding's; the observed
instances on real machines are [bandit.md](../../machines/tnt/bandit.md) §4.1–§4.2's. The formats:

| Property | On | Format and meaning |
|---|---|---|
| `device_type` | bus node | the string `pci` [2] §3.1.1 p. 11 |
| `#address-cells` / `#size-cells` | bus node | 3 and 2 — "reflecting PCI's 64-bit address space" [2] §3.1.1 p. 11 |
| `clock-frequency` | bus node | the PCI clock in hertz [2] §3.1.2 p. 11 — 33,333,333 on the TNT bridges [bandit.md](../../machines/tnt/bandit.md) §1.5 |
| `bus-range` | bus node | two integers: this bus's number and the largest subordinate bus number [2] §3.1.2 p. 11 |
| `slot-names` | bus node | an integer bitmask over device numbers — "the least-significant bit corresponds to Device Number 0" — followed by one label string per set bit, in device-number order, "the label that is printed on the chassis" [2] §3.1.2 p. 11 |
| `bus-master-capable` | bus node | a bitmask of devices wired to be master-capable [2] §3.1.2 p. 11 |
| `ranges` | bus node | the parent-to-child address mapping; for PCI-to-PCI bridges one entry per mapped space, absent if nothing is mapped [2] §3.1.1 p. 11 |
| `name` | device node | the FCode's name if present; otherwise class-derived, or `pciVVVV,DDDD` from the IDs [2] §4.1.1 pp. 13–14 |
| `compatible` | device node | an ordered list from `pciVVVV,DDDD.SSSS.ssss.RR` down to `pciclass,CCSS`, most-specific first [2] §2.5 p. 9 |
| `vendor-id`, `device-id`, `revision-id`, `class-code`, `interrupts`, `min-grant`, `max-latency`, `devsel-speed`, `fast-back-to-back`, `subsystem-vendor-id`, `subsystem-id`, `cache-line-size` | device node | copied from the configuration header during probing [2] §2.5 p. 8, [1] pp. 96–102 |

The binding's probe algorithm (§4.1) also requires the firmware to create a `power-consumption`
property "from the state of the PRSNT1# and PRSNT2# pins of the connector, if possible" [2] §2.5
p. 10; whether the Old World firmware ever creates it is not established in the evidence set
(§6.5).

## 3. Behaviour

### 3.1 Cycle types on the bus

A PCI transaction's command rides C/BE#[3:0] during the address phase. The full command space,
with the platform's support as PCI master and target, is [bandit.md](../../machines/tnt/bandit.md)
§3.6's table of [1] Table 1-3 p. 41: the bridge can *generate* interrupt acknowledge, special
cycle, I/O read/write, memory read/write, configuration read/write, memory read line and memory
write and invalidate; it *answers* memory read/write, configuration read/write, memory read line,
memory read multiple (treated as read line) and memory write and invalidate; reserved encodings and
the dual address cycle are not supported in either role. The bus-side consequence of the master/
target split is that the processor can originate every cycle the platform needs, while a card
mastering the bus can only reach memory space through the bridge — I/O and configuration cycles
have no target on the processor side ([bandit.md](../../machines/tnt/bandit.md) §3.6, §3.8).

Commands a driver will actually see, by initiator [1] Table 1-4 p. 44:

| Initiator | Commands generated |
|---|---|
| Processor (through the bridge) | I/O read/write, configuration read/write, memory read/write |
| Processor or PCI master | memory read line, memory read multiple, memory write, memory write and invalidate |

### 3.2 Arbitration

PCI arbitration is centralized — the arbiter is a device beside the bus, not a protocol among the
cards [1] Table 1-1 p. 35. On this platform the option implemented is "Fair, round-robin, all
slots master-capable" [1] Table 1-2 p. 38, and the arbiter is a separate gate array whose priority
list puts Grand Central first and the slots and the bridge's own mastering in a round-robin pool as
equals — the priority list, and the fact that it lives outside the bridge, are
[bandit.md](../../machines/tnt/bandit.md) §3.7's statement of the Developer Notes. The bus-level
consequences a card designer must design for: every slot is a potential master, no slot has fixed
priority over another, and "because the IB chip competes for system memory along with other system
devices, continuous PCI bursting is not possible" [1] p. 44 — the achievable bandwidth is
"dependent on the PCI target's hardware design and the architecture of the driver software" [1]
p. 44.

Two configuration registers govern a master's share of the bus, both programmed by the firmware
at startup: the Latency Timer is "set for all devices as specified in the PCI Specification 2.1"
with a platform-dependent value [1] p. 98, and the binding's rule for choosing it is "values
appropriate for the other devices on that bus, according to the values of the other device's
MIN_GNT and MAX_LAT registers" [2] §2.5 p. 10. The Cache Line Size register is likewise set per
platform [1] p. 98; the value that matters everywhere on Power Macintosh is 32 bytes — "A cache
line is 32-bytes for Apple Power Macintosh computers" [1] p. 45 — and the Network Server notes
make the register a performance contract: cards "need to implement the Cache Line Size register
(32 bytes, 8 DWORDS), and Write with Invalidate for burst writes, and Read Multiple or Read Line
for burst reads; otherwise the bridge will perform single beats to memory which decreases
bandwidth significantly" ([ans.md](../../machines/ans/ans.md) §3.3).

### 3.3 Transactions, bursting and bandwidth

A burst is "one PCI bus transaction with a single address phase followed by two or more data
phases"; the master arbitrates once, issues the start address and transaction type, and "it is
the responsibility of the target device to latch the start address into an address counter and
increment the addressing from data phase to data phase" [1] p. 44. The bridge's own transaction
repertoire — posted writes from both masters, dual alternating 32-byte buffers, the 32-byte burst
ceiling, medium DEVSEL timing, split-transaction support, concurrent bus activity — is
[bandit.md](../../machines/tnt/bandit.md) §3.6's. What belongs to the bus page is how *software
behavior* selects the transaction shapes, because on this platform bursting is a cache-mode
phenomenon:

- **Default: single beat.** "The PowerPC processor... sets PCI address space to cache inhibit
  mode," and "the PPC processor will not burst to or from address space marked cache inhibited.
  Therefore, under default cache settings, the IB chip will not initiate the Memory Read Line or
  the Memory Write and Invalidate commands to a PCI target" [1] pp. 45–46. A driver that does
  nothing gets one of "a single-beat Memory Read or Write command" [1] p. 45.
- **Floating point doubles the beat count.** "Provided software is written to utilize
  floating-point load and store instructions, as opposed to integer operations, the IB chip will
  burst a two-beat Memory Read or Memory Write command (two 4-byte data phases with one PCI
  transaction)" — 8-byte floating-point data against 4-byte integer data, "nearly doubl[ing] the
  PCI bandwidth over single-beat" transfers [1] p. 46.
- **Cache modes unlock the eight-beat line.** Write-through earns "an eight-beat burst read on
  PCI with the Memory Read Line command"; write-back earns "an eight-beat burst write on PCI with
  the Memory Write and Invalidate command" [1] p. 46. Marking the range is the driver's job, with
  the documented limitation that only one contiguous cache-mode range per 256 MB PowerPC segment
  can hold a modified mode — a second driver's call in the same segment "will not work nor will
  it report an error" [1] p. 50.
- **From the PCI side, alignment decides.** An 8-byte-aligned address earns a two-beat
  transaction; a Memory Write and Invalidate earns eight beats at 32-byte alignment; Memory Read
  Line and Memory Read Multiple disconnect after one 32-byte line in either case [1] p. 46.

The measured ceilings, "realistic ranges that have been measured moving large buffers," at 33 MHz
PCI with a ≥40 MHz processor bus and no wait states [1] pp. 47–49:

| Direction | Bytes/transaction | Bandwidth |
|---|---|---|
| Processor write to PCI | 4 (integer store) | 20 MB/s |
| Processor write to PCI | 8 (floating-point store) | 40 MB/s |
| Processor write to PCI | 32 (copy-back cache mode) | 85 MB/s |
| Processor read from PCI | 4 | 11 MB/s |
| Processor read from PCI | 8 | 20 MB/s |
| Processor read from PCI | 32 (write-through cache mode) | 40 MB/s |
| PCI master write to memory | 4 / 8 / 32 | 20 / 35 / 80 MB/s |
| PCI master read from memory | 4 / 8 / 32 | 10 / 15 / 30 MB/s |

The write-completion rule completes the picture: the bridge posts all writes and "acknowledges
cycle completion even though the transaction may not have been completed at its destination. To
check for final write completion, a driver may request a read transaction for the destination
device" [1] p. 42 — the read both proves delivery and flushes the bridge ([bandit.md](../../machines/tnt/bandit.md)
§3.6).

### 3.4 Byte order

"Byte order for addressing on the processor bus is big-endian and byte order on the PCI bus is
little-endian," and the bridge "performs the appropriate byte swapping and address transformations
to translate between the two addressing conventions" ([bandit.md](../../machines/tnt/bandit.md)
§3.5, quoting the Developer Notes). The transformation is **address-invariant byte swapping**:
"reverse the order of bytes within each field... the addresses of the data bytes do not change"
[1] §"Addressing Mode Conversion" p. 61, and "the Macintosh system always transfers addresses as
unbroken 32-bit quantities" [1] p. 62. The bridge follows the processor's mode setting — "PCI
data is always little-endian, and is correctly translated by the PCI host bridge to and from the
PowerPC bus in conformance to the PowerPC mode setting. Mac OS is big-endian, so the PowerPC mode
setting is big-endian while running Mac OS" [1] p. 40 — and the mode itself is a managed system
property: Open Firmware stores the `little-endian?` variable in NVRAM, and "each time the Power
Macintosh startup firmware loads an operating system, it checks to see whether the system's
big-endian or little-endian operation matches the value in little-endian?. If the match fails, the
Power Macintosh startup firmware changes the value in little-endian? and begins the Open Firmware
startup process again" [1] §"Addressing Mode Determination" p. 63. On a stock machine
`little-endian?` is `false` [7] p. 7.

Two card-facing consequences come from the bus page's own book: "data in PCI control, status, and
configuration registers for PCI video cards on Power Macintosh computers must be in little-endian
format" [1] p. 66, and frame buffers "must support the existing Macintosh big-endian pixel
ordering," which a card satisfies by offering multiple *apertures* — "if accessible in more than
one data format, frame buffers on cards should also support multiple views... by being mapped in
different formats to separate areas of memory" [1] pp. 54, 64–65.

### 3.5 Interrupts

The platform's slot-interrupt architecture is one decision with four consequences: a slot's four
interrupt pins are "combined by OR per slot to provide a unique slot interrupt for each card" [1]
Table 1-2 p. 38 — the Developer Notes' wiring phrasing is "INTA#, INTB#, INTC#, INTD# — interrupt
request pins; wired together on each slot" [3] Table 4-8 p. 50.

1. **The pin identity is destroyed.** Which of INTA#–INTD# a card asserts is invisible to the
   system; the configuration header's Interrupt Pin register survives only as the `interrupts`
   property [1] p. 101, and the Interrupt Line register "has no meaning for Power Macintosh
   computers... This register contains no useful information for drivers" [1] p. 101.
2. **A slot has exactly one interrupt source.** "Since all interrupt pins on the PCI bridge chip
   are 'ORed' together, there can be only one interrupt source on your device. Your device will be
   required to supply, in the case of multiple sources, a method for the driver to determine which
   source is the one requiring service" [6] §"The interrupts Property" p. 3.
3. **The line goes to the collector.** Each slot's combined line is an *external interrupt* into
   Grand Central — one of EXT0–EXT10, which are sources 20–30 of the collector's 32-source block —
   and the collector, its registers, its two acknowledge modes and its single line into the CPU
   are [grand-central.md](../../machines/tnt/grand-central.md) §2.2, §3.2–§3.5's subject. On the
   9500 the six slots sit on EXT3–EXT5 and EXT7–EXT9 (sources 23–25, 27–29); on the Network Server
   the same six lines are renamed `Slot1_Int`–`Slot6_Int` and two are traded away for the
   fast/wide SCSI controllers ([grand-central.md](../../machines/tnt/grand-central.md) §3.2;
   [ans.md](../../machines/ans/ans.md) §5.1).
4. **The number a driver receives is internal.** The device tree's `AAPL,interrupts` property is
   "an internal interrupt number... and does not necessarily reflect the actual interrupt bit
   number in the interrupt controller" — the property's role, the bridge's own error-interrupt
   numbers, and the observed values are
   [bandit.md](../../machines/tnt/bandit.md) §3.9's and [ans.md](../../machines/ans/ans.md) §5.2's.

Interrupt-acknowledge cycles exist in the mechanism but not in the OS: "Mac OS does not use
interrupt acknowledge cycles, but the Macintosh software supports their generation in case some
PCI bus chips require them," through a programming interface that invokes an interrupt-acknowledge
read cycle whose returned data is "traditionally an Intel-style interrupt vector number" [1] p. 43.
Mac OS's own interrupt dispatch model — the interrupt source tree — is §4.5's subject.

### 3.6 Errors

The platform's error posture is defined by the optionals it dropped: PERR#, SERR#, LOCK# and the
snoop pair are "Not used by the Macintosh system" [1] Table 1-2 p. 38, [3] p. 50, and parity is
therefore not a functional error path (the Command register's Parity Error Response bit is cleared
for all devices, §2.6). The transaction-level error contract — a master-aborted read surfacing as
a catchable machine-check exception, a master-aborted write silently discarded from the posting
buffer, target aborts interrupting the bridge's error line into Grand Central — is
[bandit.md](../../machines/tnt/bandit.md) §3.8's table of [1] Tables 1-7, 1-8 pp. 52–53. The
bus-page fact that rides on it is what probing must tolerate: the binding mandates that "the
first attempted access to each function shall use `lpeek`, because in some systems an attempted
access to a non-existent device might result in a processor exception (e.g. a 'bus error'" [2]
§2.5 p. 8, and notes that either behavior is legitimate on the wires — a host bridge "could
terminate the processor cycle with a bus error" or "complete the cycle and return all ones,"
because "Open Firmware peek and poke can behave in their normal way" under either [2] §2.5 p. 8.
The Macintosh bridges take the exception path on reads ([bandit.md](../../machines/tnt/bandit.md)
§3.8), so a probe that cannot take a machine check cannot probe at all.

One error class is banned outright rather than unimplemented: locking. "Semaphores must be
maintained in main system memory through processor control, using the routines described in
'Atomic Memory Operations'... Power Macintosh does not support the use of semaphores in PCI memory
space" [1] Table 1-2 note 4 p. 39 — the ISA-level replacement for the missing LOCK# wire.

### 3.7 Reset

RST# is the bus's reset: "used to bring registers and signals to a known state" [3] Table 4-8 p.
50. At power-on the platform's reset chain brings it up with the rest of the machine
([tnt.md](../../machines/tnt/tnt.md) §5.5); the details of whether and how a soft restart re-pulses
RST# to the slots — the NuBus machines' equivalent was a software pulse through the bridge — are
not established for PCI in the evidence set (§6.3). What *is* contractual is the state a card
must assume: the binding's probe sequence works from the configuration header immediately after
reset, so the header — vendor/device IDs, class code, BARs, Interrupt Pin — must be readable
without any initialization by the card, and the platform's discovery order and BAR-assignment flow
(§4.1–§4.3) are the definition of "a known state."

## 4. Programming model

### 4.1 The Open Firmware startup process

The Old World boot, from power-on to a running Mac OS [1] §"Startup Sequence" p. 90:

1. "System-specific firmware performs initialization and self-testing on memory and other hardware
   systems."
2. "The startup firmware in the Power Macintosh ROM probes each PCI bus, generates a device tree
   node for each device, and when it finds a device executes the FCode (if any) found in each PCI
   card's expansion ROM."
3. "The startup firmware in the Power Macintosh ROM finds an operating system in ROM or on a mass
   storage device; it loads it into RAM and transfers processor control to it."
4. "Mac OS completes the startup sequence."

The process "conforms to IEEE Standard 1275 and to the *PCI Bus Binding to IEEE 1275-1994*
specification" [1] §"The Open Firmware Startup Process" p. 83, which makes the binding's probe
algorithm the platform's own [2] §2.5 pp. 8–10:

- "Scan all slots in numerical order." For each, read the header type field of function 0's
  configuration registers; if it indicates a PCI-to-PCI bridge, recurse per the bridge-probing
  rules; if it indicates a multi-function device, repeat for each function "as determined by the
  presence of a non-FFFFh value in the Vendor ID field" [2] §2.5 p. 8.
- "The first attempted access to each function shall use `lpeek`" — the absent-device fault
  contract of §3.6.
- Create the header-derived properties (`vendor-id`, `device-id`, `revision-id`, `class-code`,
  `interrupts`, `min-grant`, `max-latency`, `devsel-speed`, `fast-back-to-back`, `subsystem-id`,
  `subsystem-vendor-id`, `cache-line-size`, `66mhz-capable`, `udf-supported`) [2] §2.5 p. 8.
- If the function has an FCode program, "copy the FCode program from expansion ROM into a
  temporary buffer in RAM and evaluate it as with byte-load," set `fcode-rom-offset` to the image
  offset where the FCode was found, and let it create the `name` and `reg` properties; if not,
  construct `reg` from the configuration registers — "an entry describing the Configuration Space
  for the device" first, then one entry per active base register in configuration-space order,
  then the expansion ROM if implemented [2] §2.5 pp. 9–10.
- "Disable fixed-address response by clearing the Bus Master, Memory Space and IO Space bits in
  the Command Register" — the probe leaves the card dark [2] §2.5 p. 10.
- "After all slots have been so probed, exit 'probe state' and assign base addresses... for each
  distinct base address register," with the allocated size the maximum of the BAR's indicated size
  and the `reg` property's; then create `assigned-addresses`, set each bus's Cache Line Size
  registers, set the Latency Timer registers "according to the values of the other device's
  MIN_GNT and MAX_LAT registers," and set the fast back-to-back enable bits if every target on the
  bus is capable [2] §2.5 p. 10.

Two platform rules refine the algorithm. Discovery order is a contract, because it fixes BAR
assignment order: "Open Firmware maps devices for their requested spaces in discovery order.
Order of discovery is in slot order; on board input-output devices are configured prior to slots"
([ans.md](../../machines/ans/ans.md) §3.2). And the probe order is a user-tunable configuration
variable: `pci-probe-list` holds "list of slots to probe with `probe-pci`," a string of
lower-case hex device numbers 0–1F, comma-separated, whose default "includes all available slots,
in numerically-ascending order" [2] §2.3 p. 8 — though the shipping Macintosh firmware carries the
value as `-1` [7] p. 7, the sentinel for the default rather than a string (*observed* on a
9500-class machine's `printenv`; whether the Apple firmware parses the string form at all is
§6.9).

The startup firmware also finds the operating system: "it selects some or all of the following
startup devices, based on an order of priority stored in the system hardware and on the presence
of suitable device properties in the device tree: a keyboard..., a display..., a boot device
(mass storage or ROM, indicated by the boot path environment variable)," and "in the case of Mac
OS, Open Firmware transfers processor control to the Mac OS ROM" [1] §"Starting the Operating
System" p. 91. Open Firmware itself runs card drivers with no interrupt support: "The Macintosh
Open Firmware does not provide interrupt service routines (ISR) for handling hardware
interrupts; Open Firmware drivers must detect external events by polling devices" [1] §"Open
Firmware Driver Support" p. 89.

### 4.2 The expansion ROM and FCode

"Every PCI expansion card should contain code in its expansion ROM conforming to IEEE Standard
1275" [1] §"PCI Card Characteristics" p. 54; the Old World contract is PCI "type 1 containers"
defined by the PCI specification [1] §"Device Configuration" p. 86. The firmware's ROM discovery
sequence, in order [1] §"Expansion ROM Base" pp. 100–101:

1. Query the Expansion ROM Base register "to see whether the register is implemented, following
   the [BAR sizing] procedure" of §4.3.
2. "Temporarily map in an amount of memory space equal to the requirement found from the base
   register query and then program that value into the base register."
3. "Enable the expansion ROM by an OR operation with 1 on bit 0 of the register and enable the
   card's memory space by writing a 1 to the correct bit in the Command register."
4. "Read the expansion ROM's first locations, by accessing the space temporarily mapped in,
   looking for the PCI signature (0x55AA). If it finds the signature, it continues to look for an
   Open Firmware ROM image signature. If it finds that signature, it locates the FCode, copies it
   to RAM, and executes it."
5. "After the card's FCode has finished executing, or if it was determined that there was no
   FCode, the system Open Firmware disables the card's memory space and expansion ROM and clears
   the Expansion ROM Base register to 0s."

Bit 0 of the register — the Expansion ROM Enable — is "left as 0 (disabled) by the system Open
Firmware. If the run-time driver is interested in accessing the PCI Expansion ROM, it must first
check that it has received an `assigned-addresses` entry, and then it must enable both its memory
space... and its ROM," with read-modify-write discipline [1] p. 101.

The support levels Apple defines, in descending order [6] §"Providing Open Firmware Support"
pp. 1–3, [1] §"Open Firmware FCode Options" pp. 87–88:

| Level | Contents | Consequence |
|---|---|---|
| Full | FCode boot driver, properties, and a run-time driver in the ROM | plug-and-play, unambiguous matching, usable at startup by any OS it carries a driver for |
| Mac OS and Open Firmware | Mac OS run-time driver + minimum Open Firmware properties (`name`, `reg`, `device-type` — plus a `driver,AAPL,MacOS,PowerPC` property for the run-time driver) | works at startup with Mac OS on ROM-based (Old World) machines only |
| Minimum | the Open Firmware properties `name`, `reg`, `device-type` | unique name for unambiguous driver matching; driver loads from disk |
| None | no FCode, or no ROM at all | "at system startup time, the card is recognized and address space is allocated for the device, but no peripheral initialization or driver code is loaded" — and no distinct `name`, so driver matching is ambiguous |

The properties of a compliant ROM's FCode are TN1044's catalog [6] pp. 3–4: `name` (the only
required property — if absent, the firmware constructs one), `reg` (the card's PCI memory-mapped
areas including I/O space), `device-type` (function: display, network, block...), `interrupts`,
`model`, `compatible` (alternate names for driver matching), `status` — plus the standard node
methods per device class (block, byte, display, network, serial) [6] Table 1 p. 4.

Real Old World cards follow exactly this contract (*observed* in the collected card ROMs of the
platform's display-card generation):

- The Apple Accelerated PCI Graphics Card (ATI 88800GX) ROMs (part revisions 113-32900-101 and
  113-32900-104) both begin with the $55AA PCI expansion-ROM signature and a `PCIR` data structure
  carrying vendor $1002 / device $4758, followed by an FCode program that walks byte-exactly. It
  publishes `name "ATY,mach64"`, `model "ATY,88800GX"`, `device_type "display"`, a `reg` of
  configuration space plus one 16 MB BAR, `depth $00000008`, and the run-time driver as a
  `driver,AAPL,MacOS,PowerPC` property [8].
- The same FCode drives its own configuration space through the parent bus node's config methods —
  `my-space config-l@` reads, and `command |= 1` followed by `command &= ~1` through `config-l!`
  enables and then disables the card's *own* PCI I/O decode around its bring-up — and asks the
  parent to `map-in` 64 KB of PCI I/O space [8]. The config methods are the binding's standard bus
  node interface: `config-l@`/`config-l!`, `config-w@`/`config-w!`, `config-b@`/`config-b!`, plus
  `intr-ack` and `special-!` for the address-less cycles [2] §3.2.3 pp. 12–13.
- The IMS/ixMicro TwinTurbo 128 M8A and M2 ROMs carry the same $55AA + `PCIR` header with vendor
  $10E0 / device $9128 and an FCode program publishing `name "IMS,tt128mb"` [9] — the header
  contract holds across vendors.

### 4.3 BAR sizing and address assignment

The sizing probe is the PCI-standard all-ones write, performed by the firmware for every card
without FCode and for any BAR an FCode's `reg` did not cover [1] §"Base Registers" p. 99:

1. "Write all 1s to each base register location," then "read the locations to see how many of the
   1s are still there. If the register reads back as all 0s, then the register is not implemented
   and a `reg` entry is not made for it."
2. From the surviving bits, determine "whether the register is of type memory or I/O, the amount
   of address space required, whether it is a 64-bit address, whether it is prefetchable, and
   whether it must be located below 1 MB" — the last being a null question on this platform (§2.2)
   — "and encode [it] appropriately into the `reg` entry."
3. "Once the `reg` property is stored in the node, Open Firmware clears the Base registers to all
   0s," then assigns addresses from the accumulated, conflict-free map and programs each covered
   BAR "with the address value stored in the `assigned-addresses` property" [1] p. 99.

TN1044's phrasing of the same probe names the register range: "writing all 1's to the Base
Address Registers (0x10 through 0x24) in Configuration Space... These registers are hardwired in
powers of two to define your memory needs. Bits returning a zero effectively define the memory
requirements" [6] §"The reg Property" p. 3. The allocation itself is platform policy — where in
the bridge's `ranges` windows the assignments land — and the worked examples are elsewhere: the
ATI card's 1 MB prefetchable frame buffer at $81000000 inside Bandit 1's 256 MB window
([bandit.md](../../machines/tnt/bandit.md) §4.2), and the Network Server's Symbios 53C875 with
memory BARs in Bandit 2's pass-through window, I/O space at $400, and the expansion ROM down at
$90000000 ([ans.md](../../machines/ans/ans.md) §3.2).

### 4.4 The Name Registry and Mac OS driver matching

The Mac OS half of the boot copies the tree: "The Name Registry device tree is created by copying
the Macintosh-relevant nodes and properties from the Open Firmware device tree," after which the
Code Fragment Manager and the interrupt tree are initialized, NVRAM-persistent properties are
restored, and the ROM-resident PCI drivers are processed — "PCI expansion ROM device drivers
required for booting are loaded and initialized," `kDriverIsLoadedUponDiscovery` drivers go into
the Device Manager unit table, and `kDriverIsOpenedUponLoad` drivers are opened [1] §"PCI Boot
Sequence" pp. 163–164. Disk-based drivers follow: the Extensions folder is scanned for `'ndrv'`
files, experts run, and families load and open the remaining devices [1] pp. 164–166. On PCI
machines the Slot Manager is not in the path — "In a computer that uses PCI expansion cards, the
Slot Manager is generally not available to provide information about display cards; instead, the
Name Registry must be used" [1] p. 69 (quoted at [bandit.md](../../machines/tnt/bandit.md) §4.2).

Driver matching is a three-way cascade over the node's identity [1] §"Matching Drivers With
Devices" pp. 164–165:

1. A node with a driver in ROM needs no match; the newest version wins.
2. A node whose `name` came from FCode is matched against disk drivers by `name`, then by each
   entry of `compatible` — "the first matching driver with the latest version number."
3. A node with no FCode is matched against the generated name `pcixxxx,yyyy` — "both these ID
   values must be hexadecimal numbers, without leading 0s, that use lower case for the letters A
   through F" [1] p. 165.

The driver description structure's `nameInfoStr` is the comparison key, and the Driver Loader
Library's match-install routines re-try down the sorted candidate list "if the first initialization
call to the driver fails" [1] pp. 165–166 — the reason a driver must probe its device before
claiming it.

### 4.5 Interrupts in Mac OS: the interrupt source tree

The "native driver interrupt model for PCI-based Macintosh computers" replaces the 68k era's
vector-and-VIA discipline with a hierarchical **interrupt source tree (IST)**: "an interrupt
source for a device is represented by a node in a hierarchical tree... Generally the leaf nodes of
the tree represent interrupt sources for devices and the parent nodes represent dispatching or
demultiplexing points," which "removes the need for drivers to respond in detail to hardware
interrupt mechanisms; they need only contain interrupt-handling code specific to the devices they
control" [1] §"Interrupt Model" p. 382. The tree's shape is the platform's priority structure:
"the fact that tree transversal proceeds from the root member toward leaf members gives members
closer to the root a higher priority" [1] §"Interrupt Priority" p. 389.

The parts a bus implementer must honor:

- **Construction is automatic.** "The Mac OS startup process automatically performs the initial
  construction and maintenance of the IST for all built-in I/O ASICs, PCI expansion cards, and
  PCI-to-PCI bridges that use the default PCI bridge IST extensions" [1] §"Interrupt Source Tree
  Construction" p. 389.
- **A driver finds its member by property.** The `driver-ist` property on the device's Name
  Registry node is an array of up to three `InterruptSetMember` values — the device's controller
  chip or slot interrupt, then DMA output, then DMA input, null where not applicable [1]
  §"Interrupts and the Name Registry" p. 391.
- **Dispatch is a tree walk.** The root member's transversal ISR picks a child, that child's ISR
  picks the next, until a leaf member's handler ISR services the device; the walk then retraces to
  the root applying set-level dispatching options [1] §"Interrupt Dispatching" pp. 386–387.
- **The completion protocol is a convention, not a wire.** "Mac OS expects an ISR to return a
  value of `kIsrIsComplete` if it believes that the hardware associated with the ISR caused, or may
  have caused, the interrupt. Only if an ISR is certain that its hardware did not cause the
  interrupt, should it return a value of `kIsrIsNotComplete`" [1] p. 387 — the direct consequence
  of the OR-combined slot line: a handler is invoked for *any* interrupt on its member's path,
  and "interrupt service routines (ISRs) may be called when their devices did not cause the
  interrupt" [1] p. 383.
- **Secondary interrupts defer the work.** Hardware handlers "perform only those actions that
  must be synchronized with the external device... and then queue a secondary interrupt handler,"
  executed serialized at a level between hardware interrupts and applications [1] §"Hardware and
  Secondary Interrupt Levels" pp. 383–384.

The mapping from the tree down to Grand Central's sources — which member enables and disables
which of the collector's bits, per machine — is
[grand-central.md](../../machines/tnt/grand-central.md) §3.3–§3.5's and §4.5's subject; the AIX
equivalent on the Network Server (the `AAPL,interrupts`-driven ODM configuration) is
[ans.md](../../machines/ans/ans.md) §5.2's.

### 4.6 Reaching the non-memory spaces from a driver

Memory space is plain load/store. Everything else goes through the Expansion Bus Manager, whose
six configuration routines (`ExpMgrConfigReadByte/Word/Long`, `ExpMgrConfigWriteByte/Word/Long`)
take a Name Registry entry and an offset and let "the system software and the bridge generate the
correct PCI configuration cycle for the target device" [1] §"Configuration Space Cycle Generation"
pp. 460–462; whose I/O generation offers a fast path (the `AAPL.address` logical dereference) and
a slow byte-swapping path ([bandit.md](../../machines/tnt/bandit.md) §4.4's statement of [1]
§"PCI Nonmemory Space Cycle Generation" pp. 453–458); and which also provides interrupt-acknowledge
and special-cycle generation ([1] pp. 466, 468). For DMA, `PrepareMemoryForIO` and `CheckpointIO`
"allocate[] resident system memory to buffers, provide[] logical and physical address
information, and in conjunction with CheckpointIO manage[] coherency between system memory and
the PowerPC caches" [1] p. 49 — the driver-level counterpart of the bridge's coherency bit
([bandit.md](../../machines/tnt/bandit.md) §4.3).

### 4.7 Non-Apple operating systems

The bus contract is OS-independent by design — the binding exists so "a PCI card that wants to
participate in the startup process of any operating system must include an expansion ROM containing
an Open Firmware FCode driver" [1] p. 83 — and the Old World machines ran three other kernels
besides Mac OS. Their common preoccupations are documented at the bridge: the little-endian
configuration-port access discipline with its write–read-back handshake and the dev<11 rule
([bandit.md](../../machines/tnt/bandit.md) §2.2, §3.2–§3.3), and the coherency handshake on the
bridge's mode register that every non-Apple kernel performs before touching descriptor-driven DMA
([bandit.md](../../machines/tnt/bandit.md) §4.3). AIX on the Network Server consumes the same
device tree through its own configuration methods, reading the card's `AAPL,interrupts` property
rather than the wire number ([ans.md](../../machines/ans/ans.md) §5.2). An OS that wants
little-endian operation flips the platform with it: the firmware restarts Open Firmware on a
`little-endian?` mismatch (§3.4), which is why the variable lives in NVRAM rather than RAM.

## 5. Quirks & errata

- **The Interrupt Line register is meaningless.** Interrupts are OR-combined per slot in
  hardware; firmware writes nothing into the register and "this register contains no useful
  information for drivers" [1] p. 101. A card or driver that programs it is talking to itself.
- **The Interrupt Pin register is publish-only.** Its value is copied into the `interrupts`
  property and nothing else [1] p. 101 — the wire it names is indistinguishable from the other
  three the moment it leaves the card (§3.5).
- **The bus deliberately hands out dark cards.** The firmware clears Memory Space and I/O Space
  enables on every device before the OS loads (§2.6); a card that decodes before a driver sets
  its enables violates the platform's boot discipline.
- **No memory space below 1 MB — ever.** Legacy VGA and IDE decode ranges are structurally
  impossible [1] Table 1-2 note 1 p. 38, p. 55, and a card that hard decodes anywhere must be
  able to turn its hard decode off in FCode before entering `reg` [1] p. 55.
- **LOCK#, PERR#, SERR#, SBO#/SDONE and JTAG are absent**, and semaphores are banned from PCI
  memory space — "Power Macintosh does not support the use of semaphores in PCI memory space" [1]
  Table 1-2 pp. 38–39, [3] p. 50. The 68k/NuBus idiom of locking a card register does not
  translate.
- **No snooping across the bus.** "No cache coherency (snooping) across the PCI bus" [1] Table 1-2
  p. 38 — DMA buffer coherency is the driver's problem (`PrepareMemoryForIO`) and the bridge's
  coherency bit's ([bandit.md](../../machines/tnt/bandit.md) §4.3).
- **Bursting is opt-in and silently lost.** PCI space defaults to cache-inhibited, so the bridge
  issues single beats; and the cache-mode range limitation bites without an error — "a second
  call... to modify the cache setting in segment 8 will not work nor will it report an error" [1]
  pp. 45–46, 50.
- **I/O space never bursts**, and the allocator tries to squeeze every card into 16 bits of a
  23-bit space [1] pp. 42, 51 — a card with a large I/O BAR is fighting the platform's own
  allocation policy.
- **Writes prove nothing.** All writes are posted and acknowledged before completion; "to check
  for final write completion, a driver may request a read transaction for the destination device"
  [1] p. 42. Presence probes and flush checks must read.
- **An absent device faults a read and answers nothing to a write.** The unclaimed-read
  machine-check and the discarded posted write are the bridge's contract
  ([bandit.md](../../machines/tnt/bandit.md) §3.8); the binding's `lpeek`-first rule exists
  because of exactly this platform class [2] §2.5 p. 8.
- **Devices 0–10 do not exist on a Bandit bus.** Configuration reads below device 11 return
  all-ones and writes are refused by every kernel ([bandit.md](../../machines/tnt/bandit.md)
  §3.2); the "device number" of a slot is never below 13 on any machine of this platform
  ([bandit.md](../../machines/tnt/bandit.md) §3.2.1; [ans.md](../../machines/ans/ans.md) §3.3).
- **The allocation table is not the address map.** Apple's own caveat — "Neither hardware nor
  software should rely on the address map described therein" [1] p. 59 — means the `ranges`
  property is the only authoritative window list; the TNT and ANS trees both deviate from the
  illustration (the TNT machines place no bridge at $F6, and the ANS places none at $F0).
- **The unit address is hex, the IDSEL is decimal.** IDSEL 16 is `@10` and IDSEL 17 is `@11`
  ([ans.md](../../machines/ans/ans.md) §3.3); a probe list, a slot mask and a unit address are
  three different number systems over the same devices.
- **`pci-probe-list` ships as `-1`,** not as the binding's comma-separated hex string [7] p. 7 vs.
  [2] §2.3 p. 8 — a firmware that implements the binding's string parser but not Apple's sentinel
  reads the wrong default.
- **The Expansion ROM Enable bit is left clear.** A run-time driver that wants its ROM must first
  verify its `assigned-addresses` entry, then enable memory space and the ROM bit by
  read-modify-write [1] p. 101.
- **Discovery order is observable state.** BAR assignment order — and therefore every assigned
  address a driver later reads — depends on "on board input-output devices are configured prior
  to slots" and on slot order ([ans.md](../../machines/ans/ans.md) §3.2); an enumerator that
  walks IDSELs in the wrong order produces a tree no published dump matches.

## 6. Open questions

1. **The Cache Line Size and Latency Timer values the Old World firmware actually writes.** Both
   are "set for all devices as specified in the PCI Specification 2.1" with values that "may
   change from Macintosh platform to Macintosh platform" [1] p. 98 — no source in the evidence
   set prints the numbers for any machine, and neither register is dumped in the TN1062 tree.
2. **Whether the shipped Mac OS sets the bridge's coherency bit.** Every non-Apple kernel does
   ([bandit.md](../../machines/tnt/bandit.md) §4.3); Mac OS's own behavior is unrecorded
   ([bandit.md](../../machines/tnt/bandit.md) §6, item 2).
3. **RST# and the soft restart.** Whether a warm restart re-asserts RST# to the slots, or the
   cards keep state across it, is not stated by any document in the evidence set; the probe
   sequence's "known state" contract (§3.7) is the only constraint.
4. **Interrupt-acknowledge cycles in the field.** The generation interface exists "in case some
   PCI bus chips require them" [1] p. 43 — no Old World card, driver or machine in the evidence
   set is recorded as using one.
5. **The `power-consumption` property.** The binding asks firmware to create it from the PRSNT1#
   and PRSNT2# pins "if possible" [2] §2.5 p. 10; no published device tree of an Old World
   machine shows it, and whether the firmware reads the presence pins at all is unknown.
6. **`min-grant` and `max-latency` on bridge headers.** The binding warns that the fields "have a
   different meaning in the bridge header format" and leaves the firmware's choice open [2]
   §2.5 note p. 9; the book's policy copies the values without discussing the difference [1]
   pp. 101–102.
7. **The bridge-3 range.** $F6000000–$F7FFFFFF is allocated by the scheme [1] Table 2-1 p. 59 but
   populated by no machine of this platform ([bandit.md](../../machines/tnt/bandit.md) §1.4, §6
   item 7; [ans.md](../../machines/ans/ans.md) §3.1).
8. **The 66 MHz claim.** The PCI book lists the clock rate option as "33 MHz (30 ns cycle time)
   and 66 MHz" [1] Table 1-2 p. 38 and attributes 66 MHz capability to the G3-era architecture
   [1] p. 40; every Old World machine and bridge node in the evidence set is 33 MHz
   ([bandit.md](../../machines/tnt/bandit.md) §1.5). No Old World machine runs 66 MHz PCI on the
   evidence held here.
9. **`pci-probe-list` semantics in the Apple firmware.** The binding defines a string
   configuration variable [2] §2.3 p. 8; the Macintosh `printenv` carries `-1` [7] p. 7. Whether
   Apple's firmware ever parses a probe-list string, and what `-1` means to it beyond "default,"
   is not documented.
10. **Which PCI revision the Old World slots comply with.** The Developer Notes accept
    "Revision 2.0" cards [3] p. 49, [4] p. 33; the 1999 book claims Revision 2.1 compliance [1]
    p. 36; the Network Server notes say the machine "attempts to be PCI 2.1 compliant" with
    "open issues surrounding this compliance... Special Cycle support and discontinuous byte
    enables" ([ans.md](../../machines/ans/ans.md) §3.3). The three statements are not
    reconcilable on the evidence held, and the practical answer (what a rev 2.2-only card does
    in a 7500) is untested.
11. **The expansion-ROM image format on Old World cards.** The $55AA signature and `PCIR`
    structure are observed on every collected ROM of the platform's generation [8], [9], but the
    multi-image header fields (vendor/device matching, image length, revision) were never
    enumerated for a Macintosh ROM; the book's Figure 4-1-era documentation defers to the PCI
    specification itself.
12. **What consumes the `interrupts` property at run time.** The pin value is published [1]
    p. 101 and the `driver-ist` array carries the slot line [1] p. 391, but no document states
    whether Mac OS ever uses the pin number — as opposed to the OR-combined line — for anything.

## References

1. Apple Computer, Inc., *Designing PCI Cards and Drivers for Power Macintosh Computers*, Revised
   Edition (revised 3/26/99), Apple Technical Publications, 1999 — Chapter 1 "PCI Bus Overview":
   Table 1-1 p. 35 (PCI vs. NuBus), Table 1-2 p. 36 and pp. 38–39 (implementation options: IDSEL
   over AD lines, OR-combined slot interrupts, fair round-robin arbitration, unused LOCK#/PERR#/
   SERR#, no cross-bus snooping, no memory below 1 MB, semaphores banned from PCI memory space,
   per-machine power allocation), §"PCI Host Bridge Operation" pp. 39–42 (endian translation,
   posted writes, cycle-type table, I/O and configuration space), §"Maximizing PCI Bus
   Performance" pp. 43–51 (burst rules by cache mode, alignment rules, Tables 1-5/1-6 bandwidth,
   cache-mode segment limitation), §"PCI Transaction Error Responses" pp. 52–53, §"PCI Card
   Characteristics"–§"Hard Decoding Device Address Space" pp. 54–55, Chapter 2: Table 2-1 p. 59
   (allocation scheme and its caveat), §"PCI Bus Cycles" p. 59, §"Addressing Mode Determination"
   p. 63, Chapter 4: §"The Open Firmware Startup Process" pp. 83–91, §"Open Firmware FCode
   Options" pp. 87–88, §"PCI Bus Configuration" pp. 95–102 (Figure 4-1, the register actions, the
   Command bit policy), §"PCI-To-PCI Bridges" pp. 102–110, Chapter 7: §"PCI Boot Sequence" and
   §"Matching Drivers With Devices" pp. 163–166, Chapter 11: §"Interrupt Model"–§"Interrupts and
   the Name Registry" pp. 381–391, Chapter 12 as cited by [bandit.md](../../machines/tnt/bandit.md)
   §4.4.
2. IEEE 1275 Working Group, *PCI Bus Binding to: IEEE Std 1275-1994 Standard for Boot
   (Initialization Configuration) Firmware*, Revision 2.1 — §2.1.3–§2.1.4 pp. 2–4 (hard-decoded
   spaces, configuration-space addressing, address-less cycles, low-order address bits),
   §2.2.1.1 p. 4 (the phys.hi cell layout and its fields), §2.2.1.3 p. 7 (unit-address
   representation and the absent bus number), §2.3–§2.4 p. 8 (`pci-probe-list` and the probe-list
   format), §2.5 pp. 8–10 (the probe algorithm, property creation, the `lpeek` rule, address
   assignment, cache-line-size and latency-timer policy), §3.1 pp. 11–12 (bus node properties:
   `device_type`, `#address-cells` 3, `#size-cells` 2, `ranges`, `clock-frequency`,
   `bus-range`, `slot-names`, `bus-master-capable`), §3.2.3 pp. 12–13 (the `config-l@` family,
   `intr-ack`, `special-!`), §4.1.1 pp. 13–14 (child node `name`).
3. Apple Computer, Inc., *Developer Note: Power Macintosh 7500 and Power Macintosh 8500
   Computers*, Developer Press, 1995 — Chapter 4 "Expansion Features": §"PCI Expansion Slots"
   p. 49 (three slots, 33 MHz, Revision 2.0, 5 V signaling, ISA fence, 50 W total), Table 4-8
   p. 50 (the slot signal list and the unsupported optionals), §"DAV Connector" pp. 50–51.
4. Apple Computer, Inc., *Developer Note: Power Macintosh 9500 Computers*, Developer Press, 1995
   — Chapter 4 "Expansion Features": §"PCI Expansion Slots" p. 33 (six slots, two Bandit ICs,
   33 MHz, Revision 2.0, 5 V signaling, 90 W total, Table 4-6 signals, the unsupported
   optionals).
5. Apple Computer, Inc., *Fundamentals of Open Firmware, Part II: The Device Tree*, Apple
   Technical Note TN1062, September 1996 — the device tree of a Power Macintosh 9500, cited here
   via [bandit.md](../../machines/tnt/bandit.md) §4.1–§4.2 and [ans.md](../../machines/ans/ans.md)
   §3.2–§3.3.
6. Apple Computer, Inc., *Fundamentals of Open Firmware, Part III: PCI Expansion ROM Contents for
   Mac OS 8*, Apple Technical Note TN1044, Revision 1.1, May 1996 — the three Open Firmware
   support levels (pp. 1–3), the common properties (pp. 3–4), the BAR probing description
   (§"The reg Property" p. 3), the one-interrupt-source rule (§"The interrupts Property" p. 3),
   and Table 1 of standard device methods (p. 4).
7. Apple Computer, Inc., *Fundamentals of Open Firmware, Part I: The User Interface*, Apple
   Technical Note TN1061, 1996 — the Open Firmware user interface, the NVRAM `printenv` listing
   with `little-endian? false` and `pci-probe-list -1` (p. 7), and the device-tree walk (pp. 8–9).
8. Apple Accelerated PCI Graphics Card ("Spinnaker") expansion ROM dumps, Apple part revisions
   113-32900-101 and 113-32900-104 (ATI 88800GX "MACH64"), 32 KB each — *observed*: the $55AA
   PCI expansion-ROM signature and `PCIR` structure with vendor $1002 / device $4758; the
   byte-exact FCode program (30,840 and 30,196 bytes); the published property set
   (`name "ATY,mach64"`, `model "ATY,88800GX"`, `device_type "display"`, `reg` = configuration
   space + one $01000000 BAR, `depth $00000008`, `driver,AAPL,MacOS,PowerPC`); and the bring-up
   sequence's configuration-space activity — `config-l@` reads of $00/$04/$10, the Command
   register I/O-enable set and clear, and a `map-in` of 64 KB of PCI I/O space.
9. IMS/ixMicro TwinTurbo 128 M8A (revision 3.8Ab2) and TwinTurbo 128 M2 expansion ROM dumps —
   *observed*: the $55AA signature and `PCIR` structure with vendor $10E0 / device $9128 and an
   FCode program publishing `name "IMS,tt128mb"`.
