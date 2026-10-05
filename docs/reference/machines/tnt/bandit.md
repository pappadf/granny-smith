# Bandit — the AR-to-PCI host bridge

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, division of labor, instances and
   the address map, clocking
2. [Register file](#2-register-file) — the two configuration ports, the bridge's own header at device 11,
   the address-select and mode-select registers, the named-but-undocumented registers, reset state
3. [Behaviour](#3-behaviour) — configuration cycles and the IDSEL map, access discipline, address
   windows and decode, byte order, transactions and posting, arbitration, error handling, interrupts,
   Chaos the display-bus bridge
4. [Programming model](#4-programming-model) — Open Firmware's bridge nodes and slot probing, the
   coherency handshake, the Mac OS driver surface, a worked example
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The **Bandit PCI bus bridge IC** is "a PCI bus bridge controller" — the chip that connects the PCI bus
to the main processor and memory subsystem of the second-generation PCI Power Macintosh platform
[2] p. 11. Apple's Open Firmware documentation calls the two chips in a Power Macintosh 9500 "two AR
(Apple RISC) to PCI (Peripheral Component Interconnect) bridge chips" [4] p. 1: the processor side is
the 64-bit PowerPC 60x bus (the "AR bus"), the far side is a 32-bit PCI bus. The Developer Notes put
it in bridge terms: the Bandit IC "provides buffering and address translation between the processor
bus and the PCI bus" [1] p. 18, and in the 9500 the two Bandits "provide bus buffering and address
translation between the processor bus and the two PCI buses, PCI 1 and PCI 2" [2] p. 11. The PCI
book calls the same part the "IB chip (the PowerPC Processor to PCI Bridge)" [3] p. 45 and describes
it as one of a system's "peer PowerPC-to-PCI host bridges" — of which a Power Macintosh may carry up
to four [3] p. 58.

Bandit is both a bus slave and a bus master. As a slave it accepts processor transactions into the
PCI memory, I/O and configuration spaces; as a master it carries PCI traffic into main memory on
behalf of PCI bus masters — the slots' cards and Grand Central's DMA engines. It supports burst
transfers "in both directions, of up to 32 bytes in length — the size of a cache block" [1] p. 18,
[2] p. 11, and each Bandit "supports the full PCI bandwidth" [2] p. 11. The two sides run on
unrelated clocks: the buses "operate asynchronously: the PCI bus at a clock rate of 33 MHz and the
processor bus at 50 MHz" [1] p. 17 (40 or 44 MHz in the 9500, one-third of the processor clock [2]
p. 11).

The part number is **343S1126**: the bridge's own Open Firmware node carries `model AAPL,343S1126`
[4] p. 6, [5]. Two further facts fix the chip's identity in software terms: its PCI vendor ID is
Apple's $106B with device ID $0001 (revision $03 in the machines examined here), and it answers
configuration cycles at IDSEL device number 11 on its own bus ([6], [7]; §2.4 below).

A sibling bridge of the same silicon family, **Chaos** (343S1155), adapts the same host-bridge
mechanism to the display bus; it is covered in §3.10 because it shares Bandit's configuration
mechanism and register conventions.

### 1.2 Machines that carry it

| Machine | Apple platform | Bandits | PCI slots | Display path |
|---|---|---|---|---|
| Power Macintosh 7200 | Catalyst | 1 | — | Platinum (no Chaos) [5] |
| Power Macintosh 7300 | TNT | 1 | — | Chaos/Control [5] |
| Power Macintosh 7500 | TNT | 1 | 3 [1] pp. 20, 49 | Chaos/Control [1] p. 20 |
| Power Macintosh 8500 | TNT | 2 | 3 [1] pp. 20, 49 | Chaos/Control (+ AV) [5] |
| Power Macintosh 9500 | TNT | 2 | 6 [2] p. 33 | none on board — a PCI card is required [4] p. 2, [5] |

The one-ROM-serves-all structure of the platform makes the roster exact: a single 4 MB mask ROM
(versions $96CD923D and $9630C68B, August 1995) drives all four machines, and its Open Firmware
constructs the device tree — including how many `bandit` nodes exist — from the machine-identity
register [5]. The 7500 gets one bridge; the 8500 and 9500 get a second at $F4000000 [5]. The 9500's
two bridges are the documented case: "two Bandit ICs... one for PCI slots 1–3 and the other for PCI
slots 4–6" [2] p. 33. The 8500's second bridge has no slots assigned in any document; the 8500 has
three PCI slots in total [1] p. 49, so its second bus is presumably slotless (*inferred —
unverified*; §6.5).

The 7200 is the platform's second hardware platform, Catalyst: it shares the Bandit and Grand
Central silicon but drives its display through Platinum instead of Chaos/Control, and it lacks the
MESH SCSI controller [5]. The same chipset set extends beyond the Macintosh line: the Hammerhead
memory controller's multiprocessor arbitration support covers the Power Macintosh 9500/MP and the
Apple Network Server [9], so the TNT bridge population includes those machines too (*inferred —
unverified* for the Network Server's bridge count).

### 1.3 Division of labor

Bandit is nearly invisible to software, and most of what a PCI card touches on this platform is
*not* Bandit's. The split, from the Developer Note block structure [1] pp. 17–21 and the Open
Firmware device tree [4] pp. 2, 6:

| Function | Owner |
|---|---|
| Processor-to-PCI memory, I/O and configuration spaces | Bandit — one bridge per PCI bus |
| PCI bus arbitration (Grand Central vs. slots vs. Bandit master) | a separate logic device (gate array), not Bandit ([1] p. 18, [2] p. 11; §3.7) |
| 60x bus arbitration, DRAM, ROM, L2 cache | Hammerhead, the memory controller — see [hammerhead.md](hammerhead.md) |
| All built-in I/O: interrupts, DBDMA, SCSI, Ethernet, serial, sound, floppy, VIA/Cuda | Grand Central, itself a PCI device behind Bandit 1 — see [grand-central.md](grand-central.md) |
| Slot interrupt lines (INTA–INTD OR-combined per slot) | collected by Grand Central ([3] p. 36; §3.9) |
| Display data path | Chaos/Control on their own bus (§3.10) |
| Card identification, BAR assignment, driver matching | the cards' expansion ROMs and Open Firmware (§4.2) |

The consequence for the register file (§2) is that Bandit's own software surface is small: two
memory-mapped ports, one special-cycle port, and a handful of registers in its own PCI
configuration header. Everything else a card driver touches belongs to Grand Central, to Open
Firmware, or to the PCI specification itself.

### 1.4 Instances and the address map

Each bridge owns a fixed 32 MB slice of the physical address space. The PCI book's allocation table
assigns "PCI host bridge 0 control" $F0000000–$F1FFFFFF, bridge 1 $F2000000–$F3FFFFFF, bridge 2
$F4000000–$F5FFFFFF, bridge 3 $F6000000–$F7FFFFFF, system control $F8000000–$F8FFFFFF, and notes
that $80000000–$EFFFFFFF and $F9000000–$FEFFFFFF are "available to PCI expansion cards" [3] p. 58,
Table 2-1. The bridges observed on real machines and in the ROM's device tree are:

| Bridge | Base | Open Firmware alias | 32 MB control range | Carried by |
|---|---|---|---|---|
| Chaos | $F0000000 | `vci0` | $F0000000–$F1FFFFFF | 7500/8500 (and 7300) [5] |
| Bandit 1 | $F2000000 | `pci1` | $F2000000–$F3FFFFFF | all machines |
| Bandit 2 | $F4000000 | `pci2` | $F4000000–$F5FFFFFF | 8500/9500 [4] pp. 2, 6, [5] |

The aliases are verbatim strings in the ROM's `aliases` node — `pci1 → /bandit@F2000000`, `pci2 →
/bandit@F4000000`, `vci0 → /chaos@F0000000` [5]. The bridge 3 range $F6000000–$F7FFFFFF is
allocated by the book's scheme [3] Table 2-1 but not populated on any machine in this family
(*inferred — unverified*; §6.7).

Inside a bridge's 32 MB slice, the coarse layout that the Open Firmware `ranges` property of a real
Power Macintosh 9500 establishes precisely [4] p. 6 (and see §3.4):

| Range within Bandit 1's slice | Size | Function |
|---|---|---|
| $F2000000–$F27FFFFF | 8 MB | PCI **I/O space** window — PCI I/O address 0 at the bridge base ([4] p. 6; §3.4) |
| $F2800000 | 4 bytes | configuration **address** port ([6], [7], [8], [9]) |
| $F2C00000 | 8 bytes | configuration **data** port ([6], [8]) |
| $F2E00000 | — | special-cycle port ([9]; §2.8) |
| $F3000000–$F3FFFFFF | 16 MB | PCI **memory** pass-through window — PCI address $F3000000..$F3FFFFFF appears one-to-one at the same physical addresses ([4] p. 6) |
| $80000000–$8FFFFFFF | 256 MB | PCI memory space forwarded by Bandit 1 ([4] p. 6; §3.4) |

Bandit 2's slice is the exact mirror of Bandit 1's at $F4000000, and its forwarded PCI memory space
is the 256 MB at $90000000 [4] p. 6. The 68k side of the ROM reaches every built-in device through
these windows: its decoder table gives Grand Central at $F3000000 with every legacy cell offset
from it — MACE at $F3011000, SWIM III at $F3015000, AWACS at $F3014000, the SCC at $F3012000, MESH
at $F3018000, VIA at $F3016000 [5] — all inside the 16 MB pass-through window whose bottom 128 KB
Grand Central decodes.

### 1.5 Clocking

Bandit is a two-clock-domain part, and the domains are asynchronous by design. The PCI side runs at
the industry-standard rate: "The PCI expansion slots in these computers use a 33 MHz system clock"
[1] p. 49, [2] p. 33. The processor side runs at the 60x bus rate: 50 MHz on the 7500/8500 [1]
p. 17, and 40 or 44 MHz on the 9500, "one-third the processor speed" [2] p. 11. The bridge's Open
Firmware node publishes the PCI-side figure: `clock-frequency 01FCA055` — 33,333,333 Hz, "which is
33 MHz" [4] p. 6.

The PCI book describes the bridge's clock capability as "asynchronous clock operation up to 50 MHz
on the PowerPC bus and up to 33 MHz on the PCI bus" for the first PCI implementation on Power
Macintosh [3] p. 39. Bandit keeps the two sides alive at once: "It supports concurrent PowerPC bus
and PCI bus activity" [3] p. 39.

Chaos differs on one point: the display bus it serves runs "synchronous with the main system bus"
[1] p. 21 — the one bus bridge on the platform that is not asynchronous.

## 2. Register file

### 2.1 The register surface

All of Bandit's software-visible state lives in three places: two memory-mapped ports inside the
bridge's 32 MB control slice, and a small set of registers in the bridge's own PCI configuration
header, reached through those ports.

| Location | Width | Function | Detail |
|---|---|---|---|
| base + $800000 | 4 bytes | configuration address port (little-endian; reads back) | §2.2 |
| base + $C00000 | 8 bytes | configuration data port (little-endian) | §2.3 |
| base + $E00000 | — | special-cycle port | §2.8 |
| config dev 11, +$00 | dword | vendor/device ID: $106B/$0001 | §2.4 |
| config dev 11, +$08 | byte | revision ID: $03 | §2.4 |
| config dev 11, +$0C | dword | class code: host bridge ($060000) | §2.4 |
| config dev 11, +$40 | dword | named "Bandit config" in driver source; contents undocumented | §2.7 |
| config dev 11, +$48 | dword | **address select** — the coarse/fine decode masks | §2.5 |
| config dev 11, +$50 | dword | **mode select** — coherency, byte order, bus number, interrupt enable | §2.6 |
| config dev 11, +$58 | dword | named "ARBus holdoff" in driver source; contents undocumented | §2.7 |

Chaos implements the same two ports at $F0800000 and $F0C00000 with the same offsets and encodings
[5]; its header differs (device ID $0003) and its configuration space is fenced off in ways no
Bandit is (§3.10).

### 2.2 The configuration address port ($F2800000 / $F4800000)

A single 32-bit, **little-endian** latch — every driver that touches it uses byte-reversed accessors
(`outl_le`/`inl_le` in MkLinux [9], `out_le32`/`in_le32` in Linux [7], `out32rb`/`in32rb` in NetBSD
[6], `OSWriteSwapInt32` in Apple's I/O Kit [8]). Writing it selects the target of the next
configuration cycle: the address encodes an IDSEL device number, a function number and a register
offset (§3.1). The port reads back the value written — that read-back is not optional convenience
but a required handshake: Linux, MkLinux and Apple's own I/O Kit driver all write the address and
spin until the port returns it ([7], [9], [8]; §3.3).

**Zero means idle, not device 0.** The classic access sequence zeroes the port after every access
[6], and no driver ever selects device 0 through it — device numbers below 11 do not exist on the
primary bus in the first place (§3.2). A write of zero parks the latch; hardware that treated a
zero write as "select IDSEL 0" would break the NetBSD discipline [6].

### 2.3 The configuration data port ($F2C00000 / $F4C00000)

Eight bytes of address space are decoded, and the port is the data half of the configuration
mechanism: a read or write through it performs one configuration cycle at the address latched in
the address port. The low two bits of the configuration register offset are **not** part of the
latched address — they are carried by *which byte of the data port* is touched:

```
data_port_address = cfg_data + (register_offset & 0x03)
```

Apple's I/O Kit driver derives exactly this mask for Bandit-class bridges
(`configDataOffsetMask = 0x3`, as against `0x7` for later UniNorth-class parts) and indexes the
data port with it [8]; the MkLinux and NetBSD drivers compute `cfg_data + (reg & 3)` the same way
[9], [6]. Byte $j$ of the port is byte $j$ of the little-endian configuration dword, so a byte
write at data-port offset $n$ writes configuration-register byte $n$; a natural 32-bit access
touches bytes 0–3 as a unit.

### 2.4 The bridge's own header: IDSEL device 11

Bandit is itself a device on the PCI bus it hosts, and it answers configuration cycles at **device
number 11**, function 0 — the constant `kPCIBridgeSelfDevice = 11` in Apple's I/O Kit driver [8],
`PCI_BANDIT (11)` in MkLinux [9], `BANDIT_DEVNUM 11` in Linux [7]. Its header contents:

| Config offset | Value | Source |
|---|---|---|
| +$00 vendor ID | $106B (Apple) | [7], [9] |
| +$00 device ID | $0001 — `PCI_DEVICE_ID_APPLE_BANDIT`; some Bandits report **$0008** instead, and Linux accepts either | [7] §`init_bandit` |
| +$08 revision ID | $03 — Linux warns "Unknown revision for bandit" on anything else but continues | [7] |
| +$0C class code | host bridge ($060000), header type $00 | [7] |

The self-node is visible in real device trees: on a 9500 under Open Firmware 1.0.5, each bridge
appears as a child of its own node — `/bandit@F2000000/pci106b,1@B` and a second
`/pci106b,1@B` under `/bandit@F4000000`, the `@B` unit address being device 11 [4] p. 2. The later
PCI book's device-tree listing shows the same self-nodes named `/bandit@B` [3] Listing 10-1 p. 284.

The device IDs Apple's ROM enumerates when it scans for host bridges are $0001106B (Bandit),
$0003106B (Chaos) and an unidentified $0004106B in the same literal pool [5] (§6.4). The
commonly-repeated claim that Grand Central answers configuration space as device $0002 is not
supported by this ROM's literal pool [5]; Grand Central's node appears at device 16 (`/gc@10`) on
Bandit 1's bus [4] p. 2.

### 2.5 Address select (config +$48)

A 32-bit register holding **two programmable address-decode masks in one word**. Apple's own I/O
Kit driver — the only source that names and uses it (`kMacRISCPCIAddressSelect = 0x48`) — reads it
and derives the bridge's entire memory-window set from it [8]:

```c
addressSelects    = configRead32( bridge, kMacRISCPCIAddressSelect );
coarseAddressMask = addressSelects >> 16;      /* upper halfword */
fineAddressMask   = addressSelects & 0xffff;   /* lower halfword */

for( index = 0; index < 15; index++ )
    if( coarseAddressMask & (1 << index))
        addBridgeMemoryRange( index << 28, 0x10000000, true );      /* 256 MB */

for( index = 0; index < 15; index++ )
    if( fineAddressMask & (1 << index))
        addBridgeMemoryRange( (0xf0 | index) << 24, 0x01000000, true ); /* 16 MB */

addBridgeIORange( 0, 0x10000 );                                     /* 64 KB of I/O */
```

| Field | Granule | Window for bit *n* |
|---|---|---|
| bits 31–16 — **coarse mask** | 256 MB | `n << 28` — $00000000, $10000000, … $E0000000 |
| bits 15–0 — **fine mask** | 16 MB | `(0xF0 \| n) << 24` — $F0000000, $F1000000, … $FEFFFFFF |

The fine mask covers the $F0–$FE segment, which is exactly where the bridges, Grand Central and
Hammerhead live, so the fine bits are how software learns which 16 MB windows of the high segment
this bridge forwards — the pass-through window at base+16 MB among them [8]. Bit 15 of the coarse
mask would be "the F segment as a whole"; Apple's test for it is present but **commented out** in
the shipped I/O Kit source, suggesting it did not behave as expected [8]. No Linux, NetBSD or
MkLinux driver reads this register at all ([6], [7], [9]) — it is I/O Kit's private source of
bridge geometry.

### 2.6 Mode select (config +$50)

A 32-bit read/write register at device 11, config offset $50 — `kMacRISCPCIModeSelect` in Apple's
I/O Kit [8], `PCI_REG_MODE_SELECT` in MkLinux and NetBSD [9], [6], `BANDIT_MAGIC` in Linux [7].
Three sources name its bits, and they agree where they overlap:

| Bit | Name | Access | Meaning |
|---|---|---|---|
| $00000001 | `PCI_MS_BYTESWAP` | R/W | "Enable Big Endian mode" [9] — the bridge's byte-order control (§3.5) |
| $00000002 | `PCI_MS_PASSATOMIC` | R | "PCI Bus to ARBus Lock are always allowed" [9] |
| $0000000C | `PCI_MS_NUMBER_MASK` | R | PCI bus number [9] |
| $00000010 | `PCI_MS_IS_SYNC` | R | "Is Synchronous (1) or Async (0)?" [9] |
| $00000020 | `PCI_MS_VGA_SPACE` | R/W | "Map VGA I/O space" [9] |
| $00000040 | `PCI_MS_IO_COHERENT` / `BANDIT_COHERENT` | R/W | **the coherency enable** — the one bit every operating system sets ([6], [7], [9]; §4.3) |
| $00000080 | `PCI_MS_INT_ENABLE` | R/W | "Allow TEA or PCI Abort INT to pass to Grand Central" [9] — the bridge's own error interrupt gate (§3.8) |
| $00080000 | `kMacRISCPCIModeSelectRDGBit` | R/W | "RDG" — set only when the device tree carries a `DisableRDG` property, i.e. a workaround switch [8] |
| $00100000 | `kMacRISCPCModeSelectWCBit` | R/W | "WC" [8] |

The register latches: whatever is written reads back, which is the basis of the read-modify-write
coherency handshake every OS performs (§4.3). What the RDG and WC bits actually gate is not
documented anywhere (§6.3).

### 2.7 The named-but-undocumented registers (+$40, +$58)

The MkLinux driver tree names two more Bandit configuration offsets in its register-map header:
`PCI_REG_BANDIT_CFG 0x40` and `PCI_REG_ARBUS_HOLDOFF 0x58` [9]. No code in that tree, or in any
other source, reads or writes either one; their contents, widths and semantics are undocumented
(§6.1). They are listed here because the names are primary evidence that the register file extends
beyond the two well-described registers — an address-decode mask, a mode register, and at least
two more cells at $40 and $58.

### 2.8 The special-cycle port (base + $E00000)

PCI special cycles — the broadcast message transactions that the PowerPC architecture has no
equivalent of — are generated by writing a memory-mapped port in the bridge's control space. The
MkLinux driver defines `BANDIT_SPECIAL_CYCLE 0xe00000` and computes each bridge's special-cycle
port as `bridge base + 0xE00000` [9]; the PCI book describes the corresponding software interface
("Special Cycle Generation") in which a write "causes a special cycle (write) on the PCI bus. The
special cycle transmits the data message passed to the interface" [3] pp. 43, 468. The bridge
supports the special-cycle command as a master but not as a target [3] Table 1-3 p. 41.

### 2.9 Reset state

The power-on values of every latch in the file are unknown. Two facts constrain them. First, the
coherency bit is observed to read back **clear** on the machines the operating systems ran on,
because every OS init path finds it clear and sets it ([6], [7], [9]; §4.3) — though whether the
shipped Mac OS sets it, and whether the bit is clear at power-on or cleared by Open Firmware, is
not established. Second, the byte-order control is documented as a read/write bit that *enables*
big-endian mode [9], and the platform's contract is that "The PowerPC microprocessor and the PCI
host bridges are set for big-endian addressing when running a big-endian operating system such as
Mac OS" [3] p. 61 — the configuration the machines actually run in. Nothing else about the
power-on image of the header, the address port or the special-cycle port is attested.

## 3. Behaviour

### 3.1 Configuration cycles: the two address formats

The bridge generates configuration cycles indirectly, "in an indirect manner, similar to mechanism
#1 suggested in the PCI specification, using configuration address and configuration data
registers to create a single configuration cycle on the PCI bus" [3] p. 42. The address written to
the address port (§2.2) encodes the target, and there are two encodings.

**Type 0 — a device on this bridge's own bus.** The IDSEL select is carried **one-hot on the
address lines**: device number *N* is selected by setting bit *N*:

```
addr = (1 << device) | (function << 8) | (register_offset & 0xFC)
```

The one-hot form is the standard IDSEL-over-AD-lines arrangement: "IDSEL signals: Provided by
resistive connections to AD lines" [3] Table 1-2 p. 36, and the slots' IDSEL pins are asserted
during the address phase of a type-0 configuration cycle [1] Table 4-8 p. 50. Every driver
constructs the address exactly this way — `(1 << dev) | (func << 8) | reg` in NetBSD [6],
`MACRISC_CFA0(devfn, off)` in Linux [7], `addrCycle = (1 << space.s.deviceNum) |
(space.s.functionNum << 8) | offset` in I/O Kit [8].

**Type 1 — a device behind a subordinate bridge.** The low bit of the address is set and the full
bus/device/function tuple rides the upper bits:

```
addr = (bus << 16) | (devfn << 8) | (register_offset & 0xFC) | 1
```

NetBSD builds it as `x = tag | reg | 1` for a non-primary bus [6]; I/O Kit as
`addrCycle = space.bits | offset | 1` for the pass-through case [8]. Linux defines it as
`MACRISC_CFA1` [7]. The two bridges' own buses are the only ones on these machines — the
`bus-range` property of a 9500's bridges is `00 00` and `01 01` [4] p. 6 — so type-1 cycles exist
in the mechanism but reach no hardware until a PCI-to-PCI bridge card is installed.

### 3.2 The IDSEL map of the primary bus

Device numbers below 11 do not exist. NetBSD's driver carries the rule as a comment — "bandit's
minimum device number of the first bus is 11. So we behave as if there is no device when dev < 11"
— and returns $FFFFFFFF for a read, panicking on a write [6]. Linux refuses any `devfn < 11 << 3`
outright [7]. Apple's I/O Kit returns failure for any device below `kPCIBridgeSelfDevice` [8]. The
population that *does* exist, from the device tree of a real 9500 [4] p. 2 and the slot-names
property of its bridges [4] p. 6:

| Device (IDSEL) | Occupant | Evidence |
|---|---|---|
| 11 ($B) | the bridge itself (§2.4) | [4] p. 2, [7] |
| 12 | not identified on the PCI buses (§6.4) | — |
| 13–15 ($D–$F) | the three expansion slots of this bus (§3.2.1) | [4] pp. 6–7 |
| 16 ($10) | Grand Central, on Bandit 1's bus only | [4] p. 2 |

#### 3.2.1 Slot numbering

The `slot-names` property of each bridge carries a bit mask over device numbers and a label per
set bit [4] p. 7. On the 9500 both masks are $E000 — bits 13, 14, 15 — with labels **A1, B1, C1**
on Bandit 1 and **D2, E2, F2** on Bandit 2 [4] pp. 6–7. The three slots of a Bandit bus therefore
sit at IDSEL 13, 14 and 15, and a card's unit address in the device tree is its device number: the
example ATI card sits at `/bandit@F2000000/ATY,XCLAIM@D` — device $D, slot A1 [4] p. 8. The 9500's
six slots are "PCI slots 1–3" on Bandit 1 and "4–6" on Bandit 2 [2] p. 33.

### 3.3 Access discipline

Two different software disciplines drive the same hardware, and both must work.

**The write–read-back discipline.** Linux, MkLinux and Apple's I/O Kit write the address port and
then spin until the port reads the value back:

```
do {
    out_le32(cfg_addr, caddr);          /* write the config address */
    eieio();
} while (in_le32(cfg_addr) != caddr);  /* until the bridge latches it */
data = in_le32(cfg_data + (reg & 3));
```

MkLinux's comment on the loop is "Wait until BANDIT response with the right address" [9]; I/O
Kit's is the same construct in C++ [8]; Linux notes in-line that a read-back loop is strictly
required only for later UniNorth-class bridges, but performs it anyway [7]. The discipline pins
two hardware facts: the address port is readable, and it returns what was written.

**The write–settle–zero discipline.** NetBSD instead writes the address, waits, touches the data
port, waits, and zeroes the address port [6]:

```c
out32rb(pc->pc_addr, x);        DELAY(10);   /* latch the address        */
data = 0xffffffff;
if (!badaddr(pc->pc_data, 4))                /* probe under a fault handler */
        data = in32rb(pc->pc_data);           /* one 4-byte config read  */
DELAY(10);
out32rb(pc->pc_addr, 0);        DELAY(10);   /* park the latch           */
```

Three contracts fall out. The address port is zeroed after every access — zero is the idle value,
not a device select (§2.2). The data access is bracketed by settling delays on the order of 10 µs
[6]. And a configuration read of an absent device must **fault catchably or return all-ones, never
hang**: NetBSD probes the data port under a `badaddr()` fault handler and expects to recover [6];
MkLinux and Linux read all-ones for absent devices and treat a $FFFFFFFF vendor ID as absence
([9], [7]).

### 3.4 Address windows and decode

The bridge forwards three PCI address spaces into the processor's physical map, and the Open
Firmware `ranges` property of a real 9500 is the precise specification [4] p. 6. Each entry is
encoded `(child-phys, parent-phys, size)` with the PCI child address in three cells and the AR
parent address in one [4] pp. 6–7.

Bandit 1, verbatim from the machine [4] p. 6:

| PCI space | PCI address | AR (physical) address | Size |
|---|---|---|---|
| memory | $F3000000 | $F3000000 | 16 MB |
| I/O | $00000000 | $F2000000 | 8 MB |
| memory | $80000000 | $80000000 | 256 MB |

Bandit 2 is identical with $F4000000/$F5000000 and the 256 MB at $90000000 [4] p. 6. The entries
say three things:

1. **The pass-through memory window.** PCI memory addresses $F3000000–$F3FFFFFF appear at the
   same physical addresses on the processor bus — identity-mapped. This is the window Grand
   Central lives in: the 128 KB at its bottom is Grand Central's register block, reached by
   software at exactly $F3000000 [5]. The rest of the window is unclaimed by any built-in device
   on the Macintosh boards (*inferred* from the device tree of [4] p. 2, in which no other device
   carries an assigned address there).
2. **The I/O window is 8 MB at the bridge's own base.** PCI I/O address 0 sits at $F2000000, and
   the window is $00800000 deep — a full 23-bit PCI I/O space. This matches the PCI book's
   statement that "The first implementation of the PCI bus for Power Macintosh provides a 23-bit
   I/O space, although the Macintosh address allocation software tries to fit all I/O address
   space requests within the 16-bit minimum size" [3] p. 42, and its description of the interface:
   "The interface to I/O space uses a memory-mapped section in each PCI host bridge's control
   space" [3] p. 42. Apple's I/O Kit models the I/O range as 64 KB at I/O address 0
   (`addBridgeIORange(0, 0x10000)`) [8] — the span the allocator actually uses.
3. **The relocated PCI memory window.** 256 MB of PCI memory space at $80000000 (Bandit 1) and
   $90000000 (Bandit 2) — this is where card memory BARs are assigned; the example ATI card's
   frame buffer lands at $81000000 [4] p. 8. More than 1.8 GB of the 32-bit address space is
   allocated to PCI memory space in the architecture [3] p. 58.

The coarse/fine masks of the address-select register (§2.5) describe exactly this geometry: the
256 MB window is one coarse bit, the two 16 MB high-segment windows are two fine bits [8]. The
allocation table's own caveat is worth keeping: "The information in Table 2-1 is for illustrative
purposes only. Neither hardware nor software should rely on the address map described therein"
[3] p. 58 — the `ranges` property of the machine you are on is the operative description, and
Linux's `pci_process_bridge_OF_ranges()` consumes it exactly that way [7].

One boundary rule rides with the memory space: "The Power Macintosh implementation does not
support devices that address memory space below 1 MB" [3] Table 1-2 note 1 p. 36.

### 3.5 Byte order

The platform is bimodal by design: "Byte order for addressing on the processor bus is big-endian
and byte order on the PCI bus is little-endian. The Bandit IC performs the appropriate byte
swapping and address transformations to translate between the two addressing conventions" [1] p.
18, [2] p. 11. The bridge follows the processor's mode setting: "The PowerPC bus can be used in
big-endian or little-endian modes. PCI data is always little-endian, and is correctly translated
by the PCI host bridge to and from the PowerPC bus in conformance to the PowerPC mode setting. Mac
OS is big-endian, so the PowerPC mode setting is big-endian while running Mac OS" [3] p. 39.

The transformation is **address-invariant byte swapping**: "reverse the order of bytes within each
field... the addresses of the data bytes do not change" [3] p. 61 — the same convention that
makes the PCI book send drivers to the PowerPC byte-reversed load and store instructions
(`lwbrx` and its kin) for data that must appear in the other order [3] p. 61. Addresses
themselves are exempt: "the Macintosh system always transfers addresses as unbroken 32-bit
quantities" [3] p. 61. The bridge's own mode bit is the MkLinux-documented `PCI_MS_BYTESWAP`
("Enable Big Endian mode", read/write) in mode select [9] (§2.6).

Two consequences for driver writers, both from the PCI book: data in "PCI control, status, and
configuration registers for PCI video cards on Power Macintosh computers must be in little-endian
format" [3] p. 66, and frame buffers must present the Macintosh big-endian pixel ordering — a
card can satisfy both by offering multiple *apertures*, separate mappings of the same memory in
different formats [3] pp. 64–65.

### 3.6 Transactions, bursts and posting

The bridge's transaction repertoire is specified cycle type by cycle type [3] Table 1-3 p. 41:

| Command (C/BE) | As PCI master | As PCI target |
|---|---|---|
| Interrupt acknowledge | yes | no |
| Special cycle | yes | no |
| I/O read / I/O write | yes | no |
| Memory read / memory write | yes | yes |
| Configuration read / write | yes | yes |
| Memory read multiple | no | yes |
| Dual address cycle | no | no |
| Memory read line | yes | yes |
| Memory write and invalidate | yes | yes |

Beyond the table, the first-implementation bridge's operating characteristics are [3] p. 39:

- **Dual alternating 32-byte data transaction buffers**, one set for transactions initiated by the
  processor bus and one for transactions initiated by the PCI bus.
- **Split-transaction PowerPC bus support** and concurrent processor/PCI activity.
- **Posted writes, always enabled, from both masters** [3] p. 39 — "the bridge posts all PCI
  write transactions. If the target is in PCI memory space, the bridge writes data directly...
  The bridge acknowledges cycle completion even though the transaction may not have been
  completed at its destination. To check for final write completion, a driver may request a read
  transaction for the destination device" [3] p. 39 note.
- **32-byte cache line**, and a 32-byte ceiling on bursts: "The longest burst generated as a
  master or accepted before disconnecting as a target is 32 bytes, the Power Macintosh cache line
  size" [3] p. 41. Memory read multiple from a PCI master is accepted but "treated the same as
  memory read line" [3] pp. 39, 48.
- **Medium DEVSEL timing** as a PCI target [3] p. 41.

Bursting is governed by the PowerPC cache mode of the target's pages [3] pp. 45–48: with PCI
space at its default cache-inhibited setting the processor does not burst, so the bridge issues
only single-beat memory read/write commands. Software that marks a PCI range write-through earns
an eight-beat memory-read-line burst on reads; write-back earns an eight-beat memory-write-and-
invalidate burst on writes. From the PCI side, 8-byte-aligned accesses earn two-beat transactions
and 32-byte-aligned write-and-invalidate earns eight beats; read line and read multiple disconnect
after one cache line. Measured sustainable bandwidths (32-bit, 33 MHz, no wait states) reach
85 MB/s processor-to-PCI writes and 80 MB/s PCI-master-to-memory writes at full 32-byte bursts,
against 10–20 MB/s single-beat [3] Tables 1-5, 1-6 pp. 49–50.

I/O-space transfers do not burst: "The IB chip does not burst PCI I/O Read nor burst PCI I/O Write
commands" [3] p. 42.

### 3.7 Arbitration

PCI bus arbitration is **centralized** — one of the architectural differences from NuBus, whose
arbitration was distributed [3] Table 1-1 p. 35 — and it lives outside Bandit: "A separate logic
device (gate array) provides the priorities for bus arbitration" [1] p. 18; "Separate logic
devices (gate arrays) provide the priorities" [2] p. 11. The priority list is the same in both
Developer Notes:

1. Grand Central (the I/O controller; **highest priority**) [1] p. 18, [2] p. 11
2. PCI slots and the Bandit master (acting for the processor), **in round-robin sequence: that
   is, each in turn, with equal priority** [1] p. 18, [2] p. 11

The implementation option is stated at the architecture level: "PCI bus arbitration: Fair,
round-robin, all slots master-capable" [3] Table 1-2 p. 36. So Grand Central's DMA engines always
outrank everyone on the PCI bus, while the cards and the bridge itself contend as equals — the
bridge both sits in the round-robin pool as the processor's proxy master and hosts the bus that
the pool arbitrates for.

### 3.8 Errors: aborts, machine checks and the bridge interrupt

The bridge "responds to system error and exception conditions in a manner that prevents the
system from hanging... tries to signal the error or exception and terminate the transaction
gracefully" [3] p. 52. The translation tables [3] Tables 1-7, 1-8 p. 52:

**Bridge as PCI master** (acting for a processor-bus master):

| Transaction | PCI response | Result |
|---|---|---|
| Write | no DEVSEL (master abort) | data discarded after posting; master abort error interrupt generated |
| Write | target abort | data discarded after posting; target abort error interrupt generated |
| Read | no DEVSEL (master abort) | **machine check exception (bus error) generated**; master abort error interrupt generated |
| Read | target abort | machine check exception generated; target abort error interrupt generated |

**Bridge as PCI target** (a PCI master reaching processor-bus space):

| Transaction | Response | Result |
|---|---|---|
| Write | bus error | data discarded after posting; signaled target abort interrupt generated |
| Read | bus error | bridge generates target abort; signaled target abort interrupt generated |

Two properties of this table are load-bearing for software. An unclaimed *read* — the empty-slot
probe — surfaces on the processor as a catchable machine-check-class exception, which is why
probing can be done under a fault handler and recover ([6]; §3.3); an unclaimed *write* is
silently posted and dropped, so a write alone proves nothing about a device's presence. And every
error path also "generates" an interrupt — the bridge's own error interrupt, which mode-select
bit $80 gates: `PCI_MS_INT_ENABLE` — "Allow TEA or PCI Abort INT to pass to Grand Central" [9]. TEA
is the PowerPC Transfer Error Acknowledge; the interrupt the bridge raises on a PCI abort is
delivered to Grand Central's interrupt collector on the line the bridge's device-tree node
declares (§3.9).

### 3.9 Interrupts

**Slot interrupts are not the bridge's to carry.** Each slot's four PCI interrupt pins are
combined in hardware: "INTA#, INTB#, INTC#, INTD# wires combined by OR per slot to provide a
unique slot interrupt for each card" [3] Table 1-2 p. 36 — a unique line per *slot*, regardless of
which pin the card declares. The consequence is stated twice in the PCI book: the configuration
Interrupt Line register "has no meaning for Power Macintosh computers because interrupts are
OR-combined per slot in hardware" — startup firmware writes nothing into it and drivers must not
read it [3] Chapter 4. The interrupt number a driver actually receives comes from the device tree:
the `AAPL,interrupts` property created during the Open Firmware startup, which "is an internal
interrupt number... and does not necessarily reflect the actual interrupt bit number in the
interrupt controller" [3] p. 249. In the 9500 dump, the seated ATI card in slot A1 carries
`AAPL,interrupts 00000017` — number 23 — alongside `AAPL,slot-name A1` [4] p. 8.

**The bridge itself interrupts too.** Each `bandit` node carries an `AAPL,interrupts` property —
`00000016` (22) on Bandit 1, `0000001A` (26) on Bandit 2 — of which the Technote says only:
"AAPL, interrupts is not mentioned in any documentation and is a private property used to
identify one Bandit chip interrupts from the other" [4] p. 6. The interrupt source behind those
numbers is the bridge's error interrupt of §3.8 (TEA / PCI abort into Grand Central), per the
MkLinux mode-select gate [9]. What driver software is expected to do when the line fires is not
documented (§6.9).

**Mac OS does not use PCI interrupt-acknowledge cycles** — "but the Macintosh software supports
their generation in case some PCI bus chips require them", through a programming interface that
invokes an interrupt-acknowledge read cycle on the bus [3] p. 43. The bridge supports the
interrupt-acknowledge command as a master only [3] Table 1-3 p. 41.

### 3.10 Chaos — the display-bus bridge

Chaos (343S1155) is the same host-bridge mechanism adapted to the display bus. On the 7500/8500,
"Chaos, a custom IC, provides data bus buffering between the video subsystem and the processor
bus" while Control provides addressing and control, and together "the Control and Chaos ICs
provide a separate bus bridge for the video subsystem. The timing on the video bus is synchronous
with the main system bus" [1] pp. 20–21. In the device tree it is `chaos@F0000000`, device type
`vci`, alias `vci0`, `model AAPL,343S1155`, with the display controller — `control`, 343S1154 — as
its child [5]. The PCI book's allocation table counts its range as "PCI host bridge 0 control"
($F0000000–$F1FFFFFF) [3] Table 2-1 p. 58: in the architecture it is a peer host bridge, and
Apple's I/O Kit drives it with a subclass of the Bandit driver that matches `('pci', 'vci')` [8].

What Chaos shares with Bandit: the same two configuration ports at its base ($F0800000 and
$F0C00000, the +$800000/+$C00000 offsets of §2.2–2.3 [5]), the same type-0/type-1 address
encodings, and the same 256 MB forwarded memory space — at **$90000000**, the segment Bandit 2
uses on the slotless-bridge machines [4] p. 6. What differs:

- **No I/O space at all.** The I/O-window method of the Bandit driver returns zero for Chaos
  (`AppleMacRiscVCI::ioDeviceMemory()` returns 0), and its configure path adds a single
  hard-coded memory window: 256 MB at $90000000 [8].
- **A different device ID**: vendor $106B, device $0003 — Linux's Chaos map tests exactly
  `vendor == 0x106b && device == 3` [7], and the ROM's bridge-enumeration literal pool carries
  $0003106B [5].
- **It cannot be probed for devices** — Linux's verbatim comment on the Chaos controller is that
  it is "accessed the same way as bandit, but cannot be probed for devices" [7]. Its bus hosts
  only the on-board display chain (`control` on the 7500/8500, `platinum` on the 7200, `planb`
  for video input on the AV machines) [5].
- **Its configuration space is fenced.** Linux's `chaos_map_bus()` refuses a configuration access
  to the Chaos device itself for offsets $10 through $24 — **except $14 and $18** — and refuses
  any offset ≥ $100 [7]. NetBSD is blunter; its Chaos write routine is empty, with the comment
  "/chaos really hates writes to config space, so we just don't do them" [6].
- **Open Firmware itself does write it** — the exception the OS-era caution missed. The ROM's
  probe of the VCI bus, decoded from its FCode, walks a probe list of IDSELs 11, 12 and 14
  ($5800), claims 256 MB at $90000000, then sizes and *assigns* device 11's BARs $14 and $18
  through the $F0800000/$F0C00000 ports (*observed* in the ROM's own execution): BAR $18 — the
  64 MB VRAM aperture — is assigned $90000000, and BAR $14 — a 4 KB register block — is assigned
  $94000000. The assigned values are published in the `control` node's `assigned-addresses`
  ($82015814 → $94000000 and the $18 BAR at $90000000) while `reg` keeps zero addresses [5].
  The two readable/writable offsets being exactly the display controller's two BARs is
  presumably why they are the readability exceptions (*inferred*).

The practical model of Chaos for an implementer: same mechanism as Bandit, readable configuration
at $00–$0F plus $14 and $18, all-ones elsewhere in $10–$24, writes to $14/$18 latched and
everything else ignored — which satisfies both the Open Firmware probe and the NetBSD/Linux
caution ([5], [6], [7]).

## 4. Programming model

### 4.1 Boot: Open Firmware builds the bridges

On this platform Open Firmware performs the hardware bring-up, and the bridges are pure
discovery: the firmware reads the machine-identity register, selects the model, and constructs
the device tree accordingly [5]. Each Bandit appears as a node named `bandit` with unit address
equal to its base — the two nodes of a 9500 carry these verbatim properties [4] p. 6:

| Property | Bandit 1 | Bandit 2 |
|---|---|---|
| `name` / `device_type` | `bandit` / `pci` | `bandit` / `pci` |
| `model` | `AAPL,343S1126` | `AAPL,343S1126` |
| `AAPL,interrupts` | `00000016` | `0000001A` |
| `reg` | `F2000000 02000000` (32 MB at the base) | `F4000000 02000000` |
| `#address-cells` / `#size-cells` | 3 / 2 (PCI convention) | 3 / 2 |
| `clock-frequency` | `01FCA055` (33 MHz) | `01FCA055` |
| `slot-names` | mask $E000; labels A1, B1, C1 | mask $E000; labels D2, E2, F2 |
| `ranges` | the three windows of §3.4 | the mirrored set at $F4000000/$F5000000/$90000000 |
| `bus-range` | `00 00` | `01 01` |

The `#address-cells`/`#size-cells` split is the key to reading `ranges`: Bandit "is a child to the
AR bus and a parent to the PCI bus. #address-cells and #size-cells is fixed at 3 and 2,
respectively, for PCI but 1 and 1 for AR" [4] pp. 6–7. The PCI-side phys.hi cell packs the
relocatable-space bits, device, function and register exactly as the IEEE 1275 PCI binding
defines, a layout the Technote reproduces for the reader [4] p. 7.

The aliases node then publishes `pci1 → /bandit@F2000000`, `pci2 → /bandit@F4000000` and
`vci0 → /chaos@F0000000`, alongside the per-device aliases under Bandit 1 — `scsi →
/bandit/gc/53c94`, `scsi-int → /bandit/gc/mesh`, `enet → /bandit/gc/mace`, `ttya`/`ttyb` for the
serial channels, `fd` for the floppy, `kbd` for the ADB keyboard [4] p. 5, [5].

The byte order the whole construction runs in is itself managed by firmware: Open Firmware stores
the `little-endian?` variable in NVRAM, and "Each time the Power Macintosh startup firmware loads
an operating system, it checks to see whether the system's big-endian or little-endian operation
matches the value in little-endian?. If the match fails, the Power Macintosh startup firmware
changes the value in little-endian? and begins the Open Firmware startup process again" [3] p. 63.

### 4.2 Slot probing and BAR assignment

Open Firmware probes each bridge's IDSEL range, sizes every discovered device's BARs, assigns
them inside the bridge's `ranges` windows, and publishes the results — `assigned-addresses`,
`AAPL,slot-name`, `AAPL,interrupts`, `fcode-rom-offset` — on the device's node [4] p. 8. The
worked example from a real 9500 [4] p. 8: an ATI XCLAIM GT card in slot A1 (device $D) publishes

```
vendor-id         00001002
device-id         00004758
class-code        00030000
interrupts        00000001
AAPL,interrupts   00000017
AAPL,slot-name    A1
reg               ... BAR 0x10, memory space, 1 MB ...
assigned-addresses  82006810 00000000 81000000 00000000 01000000
```

— one megabyte of prefetchable memory at $81000000, inside Bandit 1's 256 MB window [4] p. 8. The
card's expansion ROM then runs under the Open Firmware model and contributes the `driver,AAPL,
MacOS,PowerPC` driver-is-a-property blob that Mac OS later matches [4] p. 8. On the VCI bus the
same machinery assigns the display controller's two BARs (§3.10) [5].

The result of the probe is what the Name Registry exposes to Mac OS — the Slot Manager is not in
the path on PCI machines: "In a computer that uses PCI expansion cards, the Slot Manager is
generally not available to provide information about display cards; instead, the Name Registry
must be used" [1] p. 69.

### 4.3 The coherency handshake: every OS performs it

The one Bandit register every non-Apple operating system writes is mode-select bit $40. Linux's
`init_bandit()` is the canonical statement [7]:

```
out_le32(cfg_addr, (1 << 11) + PCI_VENDOR_ID);     /* select device 11, reg 0   */
vendev = in_le32(cfg_data);                        /* expect $0001106B         */
if (vendev == (0x0001 << 16) + 0x106B) {
    out_le32(cfg_addr, (1 << 11) + PCI_REVISION_ID);
    rev = in_le32(cfg_data);                       /* expect $03; warn if not  */
} else if (vendev != (0x0008 << 16) + 0x106B)      /* BANDIT_DEVID_2            */
    warn("bandit isn't?");

out_le32(cfg_addr, (1 << 11) + 0x50);              /* mode select               */
magic = in_le32(cfg_data);
if ((magic & 0x40) == 0) {
    magic |= 0x40;
    out_le32(cfg_data, magic);                      /* latch the bit             */
}
info("Cache coherency enabled for bandit/PSX");
```

NetBSD's `bandit_init()` does the same, naming the register `PCI_REG_MODE_SELECT` and the bit
`PCI_MODE_IO_COHERENT` [6]; MkLinux's `powermac_is_coherent()` reads mode select, sets
`PCI_MS_IO_COHERENT` if clear, and reports the whole machine's coherency from the result [9];
Apple's I/O Kit driver is the exception — it does not set bit $40, and instead conditionally sets
the RDG bit when the device tree carries a `DisableRDG` property [8].

What the bit means in hardware is the difference between two domains the platform maintains. The
PCI bus itself carries no snooping: "SBO#, SDONE: Not used by the Macintosh system. No cache
coherency (snooping) across the PCI bus" [3] Table 1-2 p. 36. The processor-bus domain is
coherent by other means: "The Power Macintosh hardware supports processor cache snooping, which
guarantees that the RAM and cache memory domain is coherent" [3] p. 329, and MkLinux annotates the
standard command-register bit $10 with "Bandit is I/O coherent" [9]. The bit therefore makes the
bridge's own mastering participate coherently in the RAM/cache domain — which is why every
non-Apple kernel sets it before touching a descriptor-based DMA engine (see [dbdma.md](dbdma.md)
for the engine on the far side of Grand Central). The exact hardware mechanism behind the bit is
not documented (§6.2).

### 4.4 The Mac OS driver surface

Mac OS drivers do not touch the configuration ports directly for ordinary work. The Expansion Bus
Manager provides six routines — `ExpMgrConfigReadByte/Word/Long` and `ExpMgrConfigWriteByte/Word/
Long` — that take a Name Registry node and a configuration offset, and "Using RegEntryIDPtr lets
the system software and the bridge generate the correct PCI configuration cycle for the target
device" [3] pp. 460–462. The routine set performs the byte swapping the raw hardware does not
[3] pp. 455–458. I/O space is reached either through those routines or the fast way: the
`AAPL.address` property, "a vector of 32-bit logical address values, where the nth value
corresponds to the nth assigned-addresses vector entry" — dereferencing the logical address
"generates an I/O cycle on the PCI bus... in the same way it accesses a PCI device in memory
space" [3] p. 454. Between I/O accesses software must call `SynchronizeIO` to keep the accesses
ordered [3] p. 454.

For DMA buffers, Mac OS provides `PrepareMemoryForIO` and `CheckpointIO`, which "allocates
resident system memory to buffers, provides logical and physical address information, and in
conjunction with CheckpointIO manages coherency between system memory and the PowerPC caches"
[3] p. 47 — the driver-level counterpart of the bridge's coherency bit. To earn cache-line bursts
to a card, a driver marks the card's PCI range cacheable with `SetProcessorCacheMode` —
write-through for cache-line reads, copy-back for cache-line writes — with the documented
limitation that only one contiguous range per 256 MB PowerPC segment can hold a modified cache
mode [3] pp. 47–48.

### 4.5 What the ROM's 68k half knows

The 68k Toolbox ROM half carries a per-platform decoder table with Grand Central at $F3000000 and
every legacy device offset from it [5] — the addresses of §1.4, reached through Bandit 1's
pass-through window. It does not name the bridges themselves; the 68k side discovers nothing
about Bandit and cares only that the F-segment windows answer. The identity of the machine — and
therefore how many `bandit` nodes Open Firmware builds — is decided by the machine-identity
register before any of this runs [5].

## 5. Quirks & errata

- **Devices 0–10 do not exist on the primary bus.** A configuration read below device 11 returns
  all-ones; NetBSD panics on a write below 11 [6], Linux refuses [7], I/O Kit fails the access
  [8]. Device 11 is the bridge itself, so "device 0" is doubly meaningless — and the address
  port's zero value is *idle*, not a device select (§2.2, §3.2).
- **The configuration ports are little-endian on a big-endian bus.** Every accessor in every
  driver is byte-reversed (`out_le32`, `out32rb`, `OSWriteSwapInt32`) [6], [7], [8], [9]. A
  natural big-endian store to the address port latches a byte-swapped address and the next data
  access hits the wrong device.
- **Two bits of the register offset live in the data port's address.** The latch carries
  `offset & ~3`; which byte of the 8-byte data port you touch supplies `offset & 3` (§2.3).
  Tools that encode the full offset in the address port select the wrong register.
- **Two incompatible access disciplines both have to work.** The write–read-back loop of Linux,
  MkLinux and I/O Kit requires the address port to read back what was written [7], [8], [9];
  NetBSD's write–settle–zero sequence requires a zero write to park the latch and 10 µs of
  settling [6]. Hardware satisfying only one breaks the other.
- **$F3000000 is memory space, not I/O space.** The window Grand Central lives in is a 16 MB PCI
  *memory* pass-through, and the 8 MB *I/O* window sits at the bridge's own base, $F2000000
  ([4] p. 6). Reading the coarse allocation table alone ("host bridge 1 control" spans both) or
  stopping at "Grand Central is at an I/O base" conflates the two.
- **Writes are posted; only reads prove anything.** The bridge acknowledges a PCI write before it
  completes and discards posted data on aborts [3] pp. 39, 52. Presence probes and
  write-completion checks must read.
- **Unclaimed reads machine-check; unclaimed writes vanish.** The asymmetry of Table 1-7 [3]
  p. 52 — a probe must run under a fault handler, and a write probe returns nothing at all.
- **Bursts stop at 32 bytes, and only if software asks.** With the default cache-inhibited
  mapping the bridge never issues memory-read-line or memory-write-and-invalidate; write-through
  or copy-back page settings unlock eight-beat bursts [3] pp. 45–48.
- **Memory read multiple is a second-class citizen.** The bridge cannot generate it as a master
  and treats it as memory read line as a target [3] pp. 39, 41.
- **No PCI memory below 1 MB** [3] Table 1-2 note 1 p. 36 — a legacy-VGA decode trap.
- **LOCK#, PERR#, SERR# are not used by the system** [1] Table 4-8 p. 50, [3] Table 1-2 p. 36;
  the bridge's mode register does carry a read-only "PCI Bus to ARBus Lock are always allowed"
  bit [9], so atomicity is a bridge property, not a PCI-wire one.
- **The Interrupt Line register is meaningless.** Interrupts are OR-combined per slot in
  hardware; firmware leaves the register alone and drivers must ignore it [3] Chapter 4.
- **Some Bandits identify as device $0008.** Linux accepts $0001 rev $03 or $0008 as a Bandit
  and warns on any other combination [7]; which machines carry the $0008 silicon is not recorded
  (§6.6).
- **Chaos hates writes** — NetBSD's verbatim engineering judgment, implemented as an empty write
  routine [6] — **but Open Firmware writes it anyway**, to exactly the two BARs $14/$18 the
  Linux readability fence exempts [5], [7]. Both behaviors are real.
- **Chaos and Bandit 2 never share $90000000.** The 256 MB memory segment belongs to Chaos on
  the 7500/8500 and to Bandit 2 on the 9500 [4] p. 6, [5] — the two never coexist on one
  machine.
- **The self-node appears twice in the tree.** Each bridge is a parent (`bandit@F2000000`) and
  its own child at device 11 (`pci106b,1@B`, later `bandit@B`) [4] p. 2, [3] Listing 10-1 p. 284.
- **The address-select register is I/O Kit's private map.** No other driver reads $48, and its
  bit 15 test is commented out in Apple's own source [8] — an OS that ignores the register
  still boots, and an OS that trusts bit 15 inherits Apple's doubt.

## 6. Open questions

1. **Registers $40 and $58.** MkLinux names `PCI_REG_BANDIT_CFG` (0x40) and
   `PCI_REG_ARBUS_HOLDOFF` (0x58) and never uses them [9]. Contents, width, and purpose — the
   name "ARBus holdoff" suggests a processor-bus parking or retry control — are undocumented.
2. **What coherency bit $40 actually does in silicon.** Every OS sets it and MkLinux annotates
   command bit $10 "Bandit is I/O coherent" [9], but no Apple document describes the mechanism
   (snoop steering on the 60x side, an attribute the memory controller honors, or something
   else), and whether the shipped Mac OS sets it is unrecorded.
3. **What the RDG and WC bits of mode select gate.** Apple's I/O Kit names
   `kMacRISCPCIModeSelectRDGBit` ($00080000) and `kMacRISCPCModeSelectWCBit` ($00100000) and
   ties the RDG bit to a `DisableRDG` device-tree workaround property, but the expanders behind
   the initials are undocumented [8].
4. **IDSEL 12, and the $0004106B literal.** Device 12 is never populated on the PCI buses in any
   tree examined, yet the Chaos probe list includes IDSEL 12 ($5800 = 11, 12, 14) [5], and the
   ROM's bridge-enumeration literal pool carries an unidentified $0004106B alongside Bandit and
   Chaos [5]. What sits — or was meant to sit — at those positions is unresolved.
5. **The 8500's second bus.** The ROM builds two `bandit` nodes for the 8500 [5], but its
   Developer Note assigns all three slots to the one PCI bus and describes a single "Bandit PCI
   Bridge IC" [1] pp. 18, 49. Whether the 8500's second bridge hosts anything at all is not
   stated by any source (*slotless inferred*, §1.2).
6. **Which machines carry the $0008 device ID.** Linux tolerates `BANDIT_DEVID_2` = 8 [7] but no
   source says which Bandits report it or why.
7. **The bridge 3 range.** $F6000000–$F7FFFFFF is allocated as "PCI host bridge 3 control" [3]
   Table 2-1 p. 58; no machine in this family populates it, and whether any contemporary machine
   did is unknown.
8. **The power-on image of the register file.** Mode select's reset value (in particular the
   coherency and byte-order bits) is not attested; only the value the OSes *find* (coherency
   clear) is known from their behavior (§2.9, §4.3).
9. **The bridge's own interrupt service.** The `bandit` nodes declare Grand Central interrupt
   numbers 22 and 26 [4] p. 6, and MkLinux's mode bit gates "TEA or PCI Abort INT to Grand
   Central" [9] — but no document describes what a driver is expected to do when the line
   fires, or how the 8500 reconciles Bandit 2's interrupt 26 with the video interrupt that the
   same Grand Central line carries on the Control machines.
10. **The ROM's $7FC and $FFFFFC literals.** The `bandit` node's literal pool in the Open
    Firmware image carries these two constants, presumably configuration-address masking values,
    undecoded [5].
11. **Chaos devices 12 and 14 in the probe list.** The VCI probe list walks IDSELs 11, 12 and 14
    [5]; only device 11 (the Chaos/Control header and BARs) is accounted for. What the ROM
    expects at 12 and 14 on the display bus is unresolved.
12. **Slot interrupts beyond the one example.** The ATI card in the 9500 dump received interrupt
    23 [4] p. 8; the full per-slot assignment of Grand Central's external-interrupt lines on
    each machine is not published by any source held here.

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh 7500 and Power Macintosh 8500
   Computers*, Developer Press, 1995 — Chapter 1 "Architecture": §"Bus Bridge" p. 17 (asynchronous
   33 MHz PCI / 50 MHz processor bus), §"Bandit PCI Bridge IC" p. 18 (buffering, address
   translation, 32-byte bursts both directions, the arbitration gate array and its priority
   list), §"Big-Endian and Little-Endian Bus Addressing" p. 18, §"Video Subsystem ICs" p. 20 and
   §"Video Bus" p. 21 (Chaos and Control), Chapter 4 "Expansion Features": §"PCI Expansion
   Slots" pp. 49–50 (three slots, 33 MHz, PCI rev 2.0, 5 V signaling, Table 4-8 supported
   signals including /IDSEL and the unsupported optional signals), Glossary (Bandit, Chaos,
   Control).
2. Apple Computer, Inc., *Developer Note: Power Macintosh 9500 Computers*, Developer Press, 1995
   — Chapter 1 "Architecture": §"Bandit Bus Bridge ICs" p. 11 (two Bandit ICs, PCI 1 and PCI 2,
   arbitration priorities), §"Bus Clock Rates" p. 11 (33 MHz PCI, 40/44 MHz processor bus,
   full PCI bandwidth, 32-byte bursts), §"Big-Endian and Little-Endian Bus Addressing" p. 11;
   Chapter 4: §"PCI Expansion Slots" p. 33 (six slots, one Bandit per three slots, 90 W).
3. Apple Computer, Inc., *Designing PCI Cards and Drivers for Power Macintosh Computers*, Revised
   Edition, Apple Technical Publications, 1999 — Part 1 "The PCI Bus": Table 1-1 p. 35
   (PCI vs. NuBus), Table 1-2 p. 36 (implementation options: IDSEL over AD lines, per-slot
   OR-combined interrupts, fair round-robin arbitration, unused LOCK#/PERR#/SERR#, no
   cross-bus snooping, no memory below 1 MB), §"PCI Host Bridge Operation" pp. 39–41
   (asynchronous clocks, split transactions, dual alternating 32-byte buffers, endian
   translation, concurrent activity, posted writes, 32-byte bursts, Table 1-3 cycle types,
   medium DEVSEL), §"I/O Space" and §"Configuration Space" p. 42 (mechanism #1, 23-bit I/O
   space, memory-mapped I/O interface), §"Special Cycles" p. 43, §"PowerPC Processor and PCI
   Commands" pp. 45–48 (cache-mode-governed bursting), Tables 1-5, 1-6 pp. 49–50 (measured
   bandwidths), §"PCI Transaction Error Responses" p. 52 (Tables 1-7, 1-8), Chapter 2: Table 2-1
   p. 58 (address allocations, four peer host bridges), §"PCI Bus Cycles" p. 59, §"Addressing
   Mode Conversion" pp. 61–62 (address-invariant byte swapping, big-endian default),
   §"Addressing Mode Determination" p. 63 (the `little-endian?` NVRAM variable), §"Frame
   Buffers"/"Frame Buffer Apertures" pp. 64–66, Chapter 4 (Interrupt Line register ignored;
   `AAPL,interrupts` as an internal number, p. 249), Chapter 10 Listing 10-1 p. 284 (the 9500
   device tree with `/bandit@B` self-nodes), Chapter 12 "Expansion Bus Manager": §"Fast I/O
   Space Cycle Generation" p. 454 (`AAPL.address`, `SynchronizeIO`), §"Slow I/O Space Cycle
   Generation" pp. 455–458 (byte-swapping ExpMgr I/O routines), §"Configuration Space Cycle
   Generation" pp. 460–462 (ExpMgrConfigRead/Write), §"Interrupt Acknowledge Cycle Generation"
   p. 466, §"Special Cycle Generation" p. 468; §"Native Drivers and Cache Coherency" p. 329
   (processor cache snooping), §"Memory Coherency"/`PrepareMemoryForIO` pp. 364–367,
   `SetProcessorCacheMode` pp. 47–48 (one cache-mode range per 256 MB segment).
4. Apple Computer, Inc., *Fundamentals of Open Firmware, Part II: The Device Tree*, Apple
   Technical Note TN1062, September 1996 — the device tree of a Power Macintosh 9500: §"Viewing
   the Device Tree" p. 2 (the `dev /` listing with `/bandit@F2000000`, `/gc@10`,
   `/pci106b,1@B`, `/bandit@F4000000`, `/hammerhead@F8000000`), §"The PCI Bus" pp. 6–7 (the two
   bandit nodes' full property sets — `reg`, `ranges`, `bus-range`, `clock-frequency`
   `01FCA055`, `slot-names`, `AAPL,interrupts` — the ranges decoding, the phys.hi cell layout),
   §"A Look at the ATI Card" p. 8 (the `ATY,XCLAIM@D` node with `assigned-addresses` at
   $81000000, `AAPL,slot-name A1`, `AAPL,interrupts 17`).
5. Power Macintosh 7200/7500/8500/9500 boot ROM, 4 MB mask ROM image, versions $96CD923D (first
   release) and $9630C68B (second release), August 1995, containing Open Firmware 1.0.5 — the
   device-tree builder (machine-identity decode selecting per-model bridge population; the
   `bandit`/`chaos`/`gc` node constructions; the `aliases` node with `pci1`, `pci2`, `vci0`; the
   bridge-enumeration literal pool $0001106B/$0003106B/$0004106B; the VCI probe-list $5800 and
   the observed BAR assignment of device 11 through the $F0800000/$F0C00000 ports, published in
   the `control` node's `assigned-addresses`), and the 68k DecoderInfo tables (GrandCentral =
   $F3000000, MACE = $F3011000, SWIM3 = $F3015000, AWACS = $F3014000, SCCRd/SCCWr = $F3012000,
   SCSI96 = $F3018000/$F3010000, VIA1 = $F3016000; the Catalyst variant without MESH).
6. M. Tsubai, `bandit.c` — NetBSD/macppc Bandit/Chaos PCI host-bridge driver,
   `sys/arch/macppc/pci/bandit.c`, revision 1.35 — the configuration-address/data port offsets
   (+$800000, 4 bytes; +$C00000, 8 bytes), the little-endian access discipline with
   `out32rb`/`in32rb` and `DELAY(10)` settle/zero sequencing, the `badaddr()` fault probe, the
   dev<11 rule, `bandit_init()` (`PCI_REG_MODE_SELECT` 0x50, `PCI_MODE_IO_COHERENT` 0x40), and
   the empty `chaos_conf_write()` with its "/chaos really hates writes" comment.
7. P. Mackerras and B. Herrenschmidt, `pci.c` — Linux PowerPC "powermac" platform PCI support,
   `arch/powerpc/platforms/powermac/pci.c` — the Bandit/Chaos controller descriptions
   ("Bandit... present in all early PCI PowerMacs"; Chaos "accessed the same way as bandit, but
   cannot be probed for devices"), `MACRISC_CFA0`/`MACRISC_CFA1` address encodings, `init_bandit`
   (`BANDIT_DEVID_2` 8, `BANDIT_REVID` 3, `BANDIT_DEVNUM` 11, `BANDIT_MAGIC` 0x50,
   `BANDIT_COHERENT` 0x40, "Cache coherency enabled for bandit/PSX"), and `chaos_map_bus()`
   (readable offsets $00–$0F, $14, $18; nothing ≥ $100).
8. Apple Computer, Inc., `AppleMacRiscPCI.cpp` and `AppleMacRiscPCI.h` — the I/O Kit PCI
   host-bridge driver for MacRISC (Bandit/Chaos) bridges, released under the Apple Public Source
   License, 1998–2000 — `kPCIBridgeSelfDevice` 11, `kMacRISCPCIAddressSelect` 0x48 and
   `kMacRISCPCIModeSelect` 0x50 with the RDG ($00080000) and WC ($00100000) bits and the
   `DisableRDG` device-tree property; the coarse/fine mask decomposition and the commented-out
   F-segment test; `configDataOffsetMask` 0x3 (vs. 0x7 for UniNorth); the write–read-back
   configuration handshake; `AppleMacRiscVCI::ioDeviceMemory()` returning 0 and
   `AppleMacRiscVCI::configure()` adding 256 MB at $90000000.
9. The MkLinux Project (Apple Computer, Inc. and OSF Research Institute), POWERMAC PCI support —
   `pci.c`, `pci_probe.c` and `powermac_pci.h` — `PCI_BANDIT` 11, `APPLE_BANDIT_ID` 0x106b, the
   `PCI_REG_*` register map (`PCI_REG_BANDIT_CFG` 0x40, `PCI_REG_ADDR_MASK` 0x48,
   `PCI_REG_MODE_SELECT` 0x50, `PCI_REG_ARBUS_HOLDOFF` 0x58) and the `PCI_MS_*` mode-select bit
   definitions with their read/write annotations, `BANDIT_SPECIAL_CYCLE` 0xe00000, the config
   port bases (`addrs[0] + 0x800000` / `+0xc00000`), the `(1 << (11+slot))` IDSEL arithmetic
   with the "Wait until BANDIT response with the right address" read-back loop, and
   `powermac_is_coherent()`; `MPPlugIn.h` for the Hammerhead multiprocessor registers covering
   the Power Macintosh 9500/MP and the Apple Network Server.
