# The National Semiconductor DP83932 SONIC

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, the Quadra logic-board
   context, clocking
2. [Register file](#2-register-file) — the RA<5:0> window: command, data configuration, receive and
   transmit control, interrupt registers, descriptor address registers, CAM registers, tally
   counters, watchdog timer; reset state
3. [Behaviour](#3-behaviour) — frame format and MAC, address filtering, receive and transmit state
   machines, FIFOs and DMA, buffer management in memory, loopback, network management, bus
   mastering, slave access, interrupts, reset, electrical and AUI specs
4. [Programming model](#4-programming-model) — bring-up from reset, descriptor initialization, CAM
   load, receive and transmit operation, interrupt service, the ROM driver, loopback procedure
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The **SONIC** (Systems-Oriented Network Interface Controller), National Semiconductor part
**DP83932**, is a "second-generation Ethernet Controller designed to meet the demands of today's
high-speed 32- and 16-bit systems" [1] §General Description. It is a single chip that integrates
everything between the host bus and an Ethernet transceiver cable: an IEEE 802.3 Manchester
encoder/decoder (ENDEC) whose operations "are identical to the DP83910A CMOS Serial Network
Interface device", a media access control (MAC) unit, two independent 32-byte FIFOs, a buffer
management engine, and a programmable system bus interface with a high-speed DMA engine that
"typically consumes less than 5% of the bus bandwidth" [1] §1.0, §1.1. Because the ENDEC is on
chip, the SONIC forms "a simple 2-chip solution for Ethernet when the SONIC is paired with the
DP8392 Coaxial Transceiver Interface or a twisted pair interface" [1] §General Description. The
part is fabricated in low-power CMOS in a 132-lead PQFP package [1] §Features.

The chip's defining feature — the one that shapes its entire programming model — is its
linked-list buffer management. The SONIC never copies packets: it DMAs received frames directly
into host memory buffers described by linked descriptors, and it fetches transmit fragments from
arbitrary, non-contiguous memory locations to serialize onto the network, so that "no intermediate
packet copy is necessary" [1] §General Description. One transmit command can drive a whole queue
of packets.

Apple's Quadra-generation Developer Notes name the chip plainly among the third-party ICs on the
main logic board: "Sonic, the DP83932 Ethernet controller IC made by National Semiconductor" [2]
p. 5, [3] p. 5. The available data manual is for the **DP83932B** step of the family; the manual
itself distinguishes the B/A steps from the original DP83932 in one bus-interface detail (§5.4.7
[1]), and the Developer Notes name the part without a suffix, so which stepping populates a given
Quadra board is not stated anywhere in the evidence (§6).

### 1.2 Machines that carry it

| Machine | SONIC as documented | I/O bus clock | Connector | Evidence |
|---|---|---|---|---|
| Macintosh Quadra 700 | "Sonic, the DP83932 Ethernet controller IC made by National Semiconductor"; "Sonic custom IC" on the I/O bus | 15.6672 MHz | Apple AUI (AAUI), accepts AUI (thick) cable or a FriendlyNet adapter | [2] pp. 3–6, 18 |
| Macintosh Quadra 900 | "Sonic, the DP83932 Ethernet controller IC made by National Semiconductor" | 15.6672 MHz | AAUI, accepts any FriendlyNet adapter (thick AUI, thin CheaperNet, 10BaseT) | [3] pp. 3–5, 21 |
| Macintosh Quadra 950 | "a 25 MHz version of the Sonic, the DP83932 Ethernet controller IC made by National Semiconductor" | 24.28416 MHz | Apple AUI connector | [4] p. 5 |

The Quadra 950's faster I/O bus is the reason for the faster SONIC part: "To accommodate the
faster I/O bus clock, the Macintosh Quadra 950 uses a 25 MHz version of the Sonic" [4] p. 5. The
data manual's AC specifications are given in two columns, for 20 MHz and 25 MHz bus clocks [1]
§7.0, which brackets both machines' I/O clocks.

### 1.3 The Quadra logic-board context

The Quadra 700/900/950 have three buses: the system bus at the MC68040's clock rate, the I/O bus,
and NuBus [2] p. 3. The SONIC is a creature of the **I/O bus**, the bus inherited from the
Macintosh IIfx: "The controller ICs that are connected to the I/O bus include the new Enhanced ASC
(Apple Sound Chip) and Sonic custom ICs as well as ICs shared with older models" [2] p. 3. The I/O
bus "runs at 15.6672 MHz and is completely asynchronous to the system bus clock" [2] p. 6, [3]
p. 7 (24.28416 MHz on the 950 [4] p. 5). Between the system bus and the I/O bus sit two adapter
ICs, JDB (Junction Data Bus) and Relayer; Relayer's duties include "generating chip select and
DSACK signals for devices on the I/O bus" [2] p. 6, [3] p. 7 — the signals that select the SONIC's
register window and terminate its slave cycles. The adapter ICs "contain no programmable registers
and do not require support from the system software" [2] p. 6.

The division of labor around the SONIC on the Quadra board is therefore:

| Function | Owner |
|---|---|
| Ethernet framing, CRC, address filtering, DMA, Manchester encoding | the SONIC [1] §1.0 |
| Chip select, DSACK generation, bus conversion for the I/O bus | Relayer [2] p. 6 |
| Media transceiver (thick/thin/10BaseT) | external — a FriendlyNet adapter on the AAUI port [2] p. 18, [3] p. 21 |
| Interrupt dispatch | VIA2 — the SONIC's interrupt is OR-gated into /SLOTIRQ on VIA2's CA1, level 2; polled via VIA2 port bit PA0 [2] p. 34, [3] p. 37 |
| Driver | ROM software — "The ROM software includes a new driver to support the Sonic IC" [2] p. 43, [3] p. 47 |

### 1.4 Clocking

The SONIC carries its own network timing: a 20 MHz fundamental-mode AT-cut parallel-resonant
crystal (±0.01% tolerance at 25°C, ±50 ppm over 0–70°C, motional resistance ≤25 Ω, load
capacitance ≤18 pF) or an external oscillator module drives the X1/X2 pins, and the oscillator
is "divided by 2 to generate the 10 MHz transmit clock (TXC) for the MAC unit" [1] §6.1.3,
Table 6-1. The IEEE 802.3 standard requires the transmit clock accurate to 0.01% [1] §6.1.3.1.
The part's other clock is **BSCK**, the bus clock that "provides the timing for the SONIC DMA
engine" [1] §5.2 — on the Quadra this is the I/O bus clock (15.6672 MHz on the 700/900, 24.28416
MHz on the 950 [2] p. 6, [4] p. 5), which the data manual's AC tables cover at 20 MHz and 25 MHz
grades [1] §7.0. Reset timing is defined in whichever of the two clocks is slower: the RESET pin
must be held for at least 10 transmit clocks, or 10 bus clocks "if the bus clock (BSCK) period is
greater than the transmit clock period" [1] §5.4.9.

---

## 2. Register file

### 2.1 The register window

The SONIC's status and control registers "occupy 64 consecutive address locations in the system
memory space (selected by the RA5-RA0 address pins)"; each register is 16 bits wide [1] §4.0,
§1.5. Registers are selected by asserting chip select and placing the register number on
RA<5:0> [1] §4.2. The 64 locations divide into three sets: **user registers** (Table 4-1), which
are everything a driver needs; **internal-use registers** (Table 4-2), "used by the SONIC during
normal operation and are not intended to be accessed by the user"; and the factory test registers
at RA $30–$3E, "for National factory use only and should never be accessed by the user" [1] §4.0,
Tables 4-1 to 4-3. During slave accesses the SONIC transfers register data only on lines
D<15:0> [1] §5.4.7.

The CAM memory cells are *not* in this window: they are reached indirectly through a CAM
descriptor area in system memory and the Load CAM command, or — for reads only — through the CAM
address ports while the SONIC is held in software reset [1] §4.1, §4.3.10.

**User registers** [1] Table 4-1:

| RA<5:0> | Access | Symbol | Register |
|---|---|---|---|
| $00 | R/W | CR | Command |
| $01 | R/W | DCR | Data Configuration (writable only in reset mode) |
| $02 | R/W | RCR | Receive Control |
| $03 | R/W | TCR | Transmit Control |
| $04 | R/W | IMR | Interrupt Mask |
| $05 | R/W | ISR | Interrupt Status |
| $3F | R/W | DCR2 | Data Configuration 2 (writable only in reset mode) |
| $06 | R/W | UTDA | Upper Transmit Descriptor Address |
| $07 | R/W | CTDA | Current Transmit Descriptor Address |
| $0D | R/W | URDA | Upper Receive Descriptor Address |
| $0E | R/W | CRDA | Current Receive Descriptor Address |
| $13 | R/W | EOBC | End of Buffer Word Count |
| $14 | R/W | URRA | Upper Receive Resource Address |
| $15 | R/W | RSA | Resource Start Address |
| $16 | R/W | REA | Resource End Address |
| $17 | R/W | RRP | Resource Read Pointer |
| $18 | R/W | RWP | Resource Write Pointer |
| $2B | R/W | RSC | Receive Sequence Counter |
| $21 | R/W | CEP | CAM Entry Pointer |
| $22 | R | CAP2 | CAM Address Port 2 (readable only in reset mode) |
| $23 | R | CAP1 | CAM Address Port 1 (readable only in reset mode) |
| $24 | R | CAP0 | CAM Address Port 0 (readable only in reset mode) |
| $25 | R/W | CE | CAM Enable (writable only in reset mode) |
| $26 | R/W | CDP | CAM Descriptor Pointer |
| $27 | R/W | CDC | CAM Descriptor Count |
| $2C | R/W | CRCT | CRC Error Tally Counter |
| $2D | R/W | FAET | FAE Tally Counter |
| $2E | R/W | MPT | Missed Packet Tally Counter |
| $29 | R/W | WT0 | Watchdog Timer 0 (low half) |
| $2A | R/W | WT1 | Watchdog Timer 1 (high half) |
| $28 | R | SR | Silicon Revision |

**Internal-use registers** [1] Table 4-2:

| RA<5:0> | Access | Symbol | Register |
|---|---|---|---|
| $08 | R/W | TPS | Transmit Packet Size (reads back inverted) |
| $09 | R/W | TFC | Transmit Fragment Count |
| $0A | R/W | TSA0 | Transmit Start Address 0 |
| $0B | R/W | TSA1 | Transmit Start Address 1 |
| $0C | R/W | TFS | Transmit Fragment Size (writes shifted once in 16-bit, twice in 32-bit mode) |
| $20 | R/W | TTDA | Temporary Transmit Descriptor Address |
| $2F | R | MDT | Maximum Deferral Timer |
| $0F | R/W | CRBA0 | Current Receive Buffer Address 0 |
| $10 | R/W | CRBA1 | Current Receive Buffer Address 1 |
| $11 | R/W | RBWC0 | Remaining Buffer Word Count 0 |
| $12 | R/W | RBWC1 | Remaining Buffer Word Count 1 |
| $19 | R/W | TRBA0 | Temporary Receive Buffer Address 0 |
| $1A | R/W | TRBA1 | Temporary Receive Buffer Address 1 |
| $1B | R/W | TBWC0 | Temporary Buffer Word Count 0 |
| $1C | R/W | TBWC1 | Temporary Buffer Word Count 1 |
| $1F | R/W | LLFA | Last Link Field Address |
| $1D | R/W | ADDR0 | Address Generator 0 |
| $1E | R/W | ADDR1 | Address Generator 1 |

### 2.2 Command register (CR, RA $00)

The CR issues commands: bits are set to command, and "for all bits, except for the RST bit, the
SONIC resets the bit after the command is completed"; writing a 0 to any bit has no effect [1]
§4.3.1.

| Bit | Name | Meaning |
|---|---|---|
| 15–10 | — | Must be 0 |
| 9 | LCAM | Load CAM from the CAM Descriptor Area at CDP; LCAM and TXP must never be set together — "The SONIC will lock up if both bits are set simultaneously" [1] §4.3.1 |
| 8 | RRRA | Read RRA: fetch the resource descriptor at RRP into CRBA0,1/RBWC0,1 in one block operation; "Generally this bit is only set during initialization" [1] §4.3.1, §3.4.4.2 |
| 7 | RST | Software reset: resets all internal state machines, disables the CRC generator, halts (but does not clear) the tally counters; set to 1 by a hardware reset; the SONIC becomes operational when cleared to 0 [1] §4.3.1 |
| 6 | — | Must be 0 |
| 5 | ST | Start Timer: start or resume the watchdog timer; resets STP [1] §4.3.1 |
| 4 | STP | Stop Timer: halt the watchdog timer; powers up as 1; setting ST and STP together stops the timer [1] §4.3.1 |
| 3 | RXEN | Receiver Enable: begin buffering to memory; resets RXDIS. If set mid-packet, both RXEN and RXDIS stay set until the network goes inactive [1] §4.3.1 |
| 2 | RXDIS | Receiver Disable: stop buffering after the packet in progress is processed; resets RXEN. Tally counters keep running regardless [1] §4.3.1 |
| 1 | TXP | Transmit Packet(s): transmit the descriptor list at CTDA; cleared on completion (EOL seen), on HTX, or on a transmit abort (EXC, EXD, FU or BCM in the TCR) [1] §4.3.1 |
| 0 | HTX | Halt Transmission: stop after the current packet's status is written; CTDA then points at the last descriptor transmitted [1] §4.3.1 |

### 2.3 Data Configuration register (DCR, RA $01)

The DCR "establishes the bus cycle options for reading/writing data to/from 16- or 32-bit memory
systems" and — critically — "must only be accessed when the SONIC is in reset mode (i.e., the RST
bit is set in the Command register)"; a write while not in reset mode does not alter the register
[1] §4.3.2. During a hardware reset only bits 15 and 13 are cleared, "all other bits are
unaffected", which is why "the first thing the driver software does to the SONIC should be to set
up this register"; the register is unaffected by a software reset [1] §4.3.2.

| Bit | Name | Meaning |
|---|---|---|
| 15 | EXBUS | Extended Bus Mode: repurposes TXD/LBK/RXC/RXD as programmable outputs EXUSR<3:0> (programmed via DCR2), turns TXC into the STERM synchronous termination input for Motorola-style buses, and switches Bus Retry to asynchronous sampling [1] §4.3.2 |
| 14 | — | Must be 0 |
| 13 | LBR | Latched Bus Retry: 0 = BRT forces the SONIC off the bus and it retries when BRT deasserts; 1 = it additionally waits until the BR bit in the ISR is cleared before retrying [1] §4.3.2 |
| 12, 11 | PO1, PO0 | Programmable Outputs: drive the USR1/USR0 pins high or low while the SONIC is a bus master [1] §4.3.2 |
| 10 | SBUS | Synchronous Bus Mode: 0 = RDYi / DSACK0,1 are internally synchronized, no setup/hold requirements; 1 = they must meet setup/hold to the rising edge of T1 or T2 [1] §4.3.2 |
| 9, 8 | USR1, USR0 | User-definable pins: these bits latch the level on the USR1/USR0 pins at the rising edge of hardware reset and are read-only thereafter [1] §4.3.2 |
| 7, 6 | WC1, WC0 | Wait State Control: 00 = 0, 01 = 1, 10 = 2, 11 = 3 extra T2 states per DMA cycle [1] §4.3.2 |
| 5 | DW | Data Width Select: 0 = 16-bit, 1 = 32-bit DMA data path [1] §4.3.2 |
| 4 | BMS | Block Mode Select: 0 = empty/fill (DMA runs until the receive FIFO empties or the transmit FIFO fills), 1 = block (each bus tenure transfers exactly the threshold count) [1] §4.3.2 |
| 3, 2 | RFT1, RFT0 | Receive FIFO Threshold: 00 = 2 words/1 long word (4 bytes), 01 = 4 words/2 long words (8 bytes), 10 = 8 words/4 long words (16 bytes), 11 = 12 words/6 long words (24 bytes) [1] §4.3.2 |
| 1, 0 | TFT1, TFT0 | Transmit FIFO Threshold: 00 = 4 words/2 long words (8 bytes), 01 = 8 words/4 long words (16 bytes), 10 = 12 words/6 long words (24 bytes), 11 = 14 words/7 long words (28 bytes) [1] §4.3.2 |

### 2.4 Data Configuration register 2 (DCR2, RA $3F)

DCR2 enables the extended bus interface options and, like the DCR, "should only be written to
when the SONIC is in software reset"; a hardware reset clears all bits except the extended
programmable outputs, and a software reset leaves every bit alone [1] §4.3.7.

| Bit | Name | Meaning |
|---|---|---|
| 15–12 | EXPO3–0 | Extended Programmable Outputs: level driven on EXUSR<3:0> while the SONIC is a bus master (only meaningful with EXBUS set) [1] §4.3.7 |
| 11–5 | — | Must be written with zeroes (read as don't-cares) [1] §4.3.7 |
| 4 | PH | Program Hold: 0 = HOLD (bus request, National/Intel mode) asserts/deasserts on the falling bus-clock edge; 1 = half a clock later, on the rising edge [1] §4.3.7 |
| 3 | — | Must be zero |
| 2 | PCM | Packet Compress when Matched: assert PCOMP when the destination address matches a CAM entry; used with the DP83950 Repeater Interface Controller's management bus ("Managed Bridge mode"). PCM and PCNM must never both be set [1] §4.3.7 |
| 1 | PCNM | Packet Compress when Not Matched: assert PCOMP on CAM mismatch ("Managed Hub mode"); never asserted for broadcast addresses regardless of the RCR's BRD bit [1] §4.3.7 |
| 0 | RJCM | Reject on CAM Match: invert the CAM's accept/reject sense (for small bridges); does not invert BRD, PRO or AMC [1] §4.3.7 |

### 2.5 Receive Control register (RCR, RA $02)

The RCR selects the receive filters and reports per-packet status. Bits 15–11 are filters
("Setting any of bits 15-11 to a '1' enables the corresponding receive filter. If none of these
bits are set, only packets which match the CAM Address registers are accepted"); bits 10–9 select
the loopback mode; bits 8–0 are status bits that are set when the corresponding condition was
true for the accepted packet and are written into the descriptor's status word [1] §4.3.3. The
register is unaffected by a software reset; a hardware reset clears the RNT, BRD and loopback bits
[1] §4.3.3.

| Bit | Name | Type | Meaning |
|---|---|---|---|
| 15 | ERR | r/w | Accept packets with CRC errors and ignore collisions |
| 14 | RNT | r/w | Accept runt packets (less than 64 bytes); cleared by hardware reset |
| 13 | BRD | r/w | Accept broadcast packets — CAM matching continues in parallel; cleared by hardware reset |
| 12 | PRO | r/w | Promiscuous mode: accept all physical-address packets |
| 11 | AMC | r/w | Accept all multicast packets (broadcast, a multicast subset, is accepted too) |
| 10, 9 | LB1, LB0 | r/w | Loopback control: 00 = none, 01 = MAC loopback, 10 = ENDEC loopback, 11 = transceiver loopback; cleared by hardware reset |
| 8 | MC | r | Multicast packet received |
| 7 | BC | r | Broadcast packet received |
| 6 | LPKT | r | Last packet in this RBA (RBWC dropped below EOBC) |
| 5 | CRS | r | Carrier sense activity |
| 4 | COL | r | Collision during reception |
| 3 | CRCR | r | CRC error on the packet (FAER is set instead if the packet is also misaligned) |
| 2 | FAER | r | Frame alignment error; set only when both frame-alignment and CRC errors occur |
| 1 | LBK | r | Loopback packet received |
| 0 | PRX | r | Packet received OK — no CRC, alignment, runt or collision errors |

[1] §4.3.3

### 2.6 Transmit Control register (TCR, RA $03)

The TCR mixes per-packet configuration with post-transmission status. Bits 15–12 are loaded at
the start of each transmission from the descriptor's TXpkt.config field; when transmission ends,
bits 10–0 report status and are written into the descriptor's TXpkt.status field together with
the collision count [1] §4.3.4, §3.5.1.2. A hardware reset sets bits 8 (NCRS) and 0 (PTX), giving
the reset value $0101; a software reset leaves the register alone [1] §4.3.4, Table 5-4.

| Bit | Name | Type | Meaning |
|---|---|---|---|
| 15 | PINTR | r/w | Programmable interrupt: raise PINT as soon as a descriptor with PINTR set is read |
| 14 | POWC | r/w | Out-of-window collision timer start: 0 = after SFD, 1 = after the first bit of preamble |
| 13 | CRCI | r/w | CRC inhibit: 1 = transmit without the 4-byte FCS |
| 12 | EXDIS | r/w | Disable the excessive-deferral timer |
| 11 | — | | Must be 0 |
| 10 | EXD | r | Excessive deferral: the SONIC deferred for 3.2 ms; aborts if the timer is enabled |
| 9 | DEF | r | Deferred on the first attempt; cleared when TXpkt.status is written |
| 8 | NCRS | r | No CRS during transmission; always set in MAC loopback |
| 7 | CRSL | r | CRS lost or never present; always set in MAC loopback |
| 6 | EXC | r | Excessive collisions: 16 collisions occurred; aborts |
| 5 | OWC | r | Out-of-window collision (after one slot time, 51.2 µs, from SFD or preamble per POWC); the transmission backs off normally |
| 4 | — | | Must be 0 |
| 3 | PMB | r | Packet monitored bad: self-received CRC invalid, frame misaligned, or source address not in the CAM |
| 2 | FU | r | FIFO underrun: the SONIC could not refill the FIFO in time; aborts |
| 1 | BCM | r | Byte count mismatch: TXpkt.pkt_size ≠ sum of the TXpkt.frag_size fields; aborts |
| 0 | PTX | r | Packet transmitted OK — none of EXC, EXD, FU or BCM |

[1] §4.3.4

### 2.7 Interrupt Mask register (IMR, RA $04)

The IMR masks the ISR: writing 1 to a bit enables the corresponding interrupt, and "During a
hardware reset, all mask bits are cleared" [1] §4.3.5. The bits are, from 14 down: BREN (bus
retry), HBLEN (heartbeat lost), LCDEN (load CAM done), PINTEN (programmable interrupt), PRXEN
(packet received), PTXEN (packet transmitted OK), TXEREN (transmit error), TCEN (timer complete),
RDEEN (receive descriptors exhausted), RBEEN (receive buffers exhausted), RBAEEN (RBA exceeded),
CRCEN (CRC tally rollover), FAEEN (FAE tally rollover), MPEN (missed-packet tally rollover) and
RFOEN (receive FIFO overrun); bit 15 must be 0 [1] §4.3.5.

### 2.8 Interrupt Status register (ISR, RA $05)

The ISR "indicates the source of an interrupt when the INT pin goes active"; a bit is cleared by
writing 1 to it, writing 0 has no effect, the register is cleared by hardware reset and left
untouched by software reset [1] §4.3.6.

| Bit | Name | Set when |
|---|---|---|
| 14 | BR | A Bus Retry occurred; in latched mode the SONIC waits for this bit to be cleared before resuming DMA |
| 13 | HBL | The transceiver sent no collision heartbeat (SQE test pulse) during the first 6.4 µs of the interframe gap after a transmission |
| 12 | LCD | The Load CAM command completed |
| 11 | PINT | A descriptor with PINTR set in TXpkt.config was read |
| 10 | PKTRX | A packet was accepted and buffered, and its descriptor (through RXpkt.seq_no) written |
| 9 | TXDN | Transmission done: EOL seen, HTX honored, or a transmit abort (BCM, EXC, FU or EXD) |
| 8 | TXER | A packet ended transmission with at least one of BCM, EXC, FU, EXD |
| 7 | TC | The watchdog timer rolled over from $00000000 to $FFFFFFFF |
| 6 | RDE | The receive descriptor list hit EOL — descriptors exhausted |
| 5 | RBE | RRP met RWP: all RRA resource descriptors consumed (an early warning, see §3.4) |
| 4 | RBAE | A packet did not fit in the remaining RBA space; reception of it is aborted |
| 3 | CRC | The CRC error tally rolled over $FFFF → $0000 |
| 2 | FAE | The FAE tally rolled over |
| 1 | MP | The missed-packet tally rolled over |
| 0 | RFO | The receive FIFO overran — the SONIC could not win the bus fast enough |

[1] §4.3.6

### 2.9 Transmit descriptor registers (UTDA, RA $06; CTDA, RA $07)

The **UTDA** holds address bits A<31:16> of the Transmit Descriptor Area; the TDA may be as large
as 32k words (16-bit mode) or 16k long words (32-bit mode) anywhere in memory [1] §4.3.8. The
**CTDA** holds the low half, bits A<15:1> — the SONIC concatenates the two to locate the current
transmit descriptor; bit 0 is the EOL flag; in 32-bit systems bit 1 must be 0 for long-word
alignment. Both registers survive hardware and software reset [1] §4.3.8.

### 2.10 Receive descriptor registers (URDA, RA $0D; CRDA, RA $0E; EOBC, RA $13)

The **URDA/CRDA** pair locates the Receive Descriptor Area the same way UTDA/CTDA locates the
TDA: URDA carries A<31:16>, CRDA carries A<15:1> with bit 0 as EOL, concatenated to point at the
first field of the current descriptor [1] §4.3.9. A software reset does not touch these registers;
a hardware reset affects only the EOBC and RSC among the receive registers [1] §4.3.9.

The **EOBC** (End of Buffer Word Count) sets a "false bottom" in each Receive Buffer Area: after
every reception the SONIC compares EOBC against the remaining word count, and "If the EOBC is
greater than the remaining number of words in the RBA after a packet is received (i.e., EOBC >
RBWC0,1), the Last Packet in RBA bit, LPKT ... is set and the SONIC fetches the next resource
descriptor" — so the next packet lands in a fresh RBA [1] §4.3.9. A hardware reset sets EOBC to
$02F8 (760 words = 1520 bytes) [1] §4.3.9, §3.4.4.4.

### 2.11 Receive Resource Area registers (URRA, RA $14; RSA $15; REA $16; RRP $17; RWP $18)

The **URRA** carries A<31:16> of the Receive Resource Area and is concatenated with any of the
four 15-bit offset registers below to form full 32-bit addresses [1] §4.3.9.

| Register | Holds |
|---|---|
| RSA | A<15:1> of the start of the RRA |
| REA | A<15:1> of the end of the RRA — the address of the last RXrsrc.ptr0 field plus 4 words (16-bit mode) or 4 long words (32-bit mode) |
| RRP | A<15:1> of the next resource descriptor the SONIC will read |
| RWP | A<15:1> of the next vacancy where the system places a resource descriptor |

[1] §4.3.9, §3.4.4.2. The LSB of each reads back as 0, and in 32-bit mode RWP's bit 1 must be 0
"to insure the proper equality comparison between this register and the RRP register" [1] §4.3.9.
The RWP = RRP comparison — the buffer-exhausted test — is made "after the complete RRA descriptor
has been read and not during the fetch", and the RWP must only point at an RXrsrc.ptr0 field or at
the RSA/REA values, otherwise "the RWP = RRP comparison" can never become true [1] §3.4.4.2.

### 2.12 Receive Sequence Counter (RSC, RA $2B)

A 16-bit register holding two modulo-256 counters: the high byte is the RBA sequence number (one
value per RBA in use, incremented when the SONIC moves to the next RBA), the low byte the packet
sequence number (incremented per packet, reset when the RBA number increments). The pair lets
software tally how many packets were processed within a particular RBA [1] §4.3.9, §3.4.3.2. A
hardware reset clears it; it can also be cleared by writing zero; a software reset has no
effect [1] §4.3.9.

### 2.13 CAM registers (RA $21–$27)

The Content Addressable Memory holds sixteen 48-bit address entries, each partitioned into three
16-bit cells reached through the address ports [1] §4.1, §4.3.10.

| Register | Role |
|---|---|
| CEP ($21) | 4-bit CAM entry selector: $0 = first entry, $F = last |
| CAP2 ($22) | Read port for bits <47:32> of the selected entry |
| CAP1 ($23) | Read port for bits <31:16> |
| CAP0 ($24) | Read port for bits <15:0> |
| CE ($25) | 16-bit enable mask: bit n enables CAM entry n; cleared (all entries disabled) by hardware reset |
| CDP ($26) | A<15:1> of the first descriptor in the CAM Descriptor Area (concatenated with URRA) |
| CDC ($27) | 5-bit count of CAM descriptors to load, 1 to 16 |

CAP2–CAP0 are read-only, and "can only be read when the SONIC is in reset mode (RST bit in the CR
is set)" — reads outside reset return invalid data; CE can only be written in reset mode and is
normally loaded by the Load CAM command itself [1] Table 4-1 notes, §4.3.10. The byte order
convention for a physical address 10:20:30:40:50:60 (10h first on the wire): CAP0 = $2010,
CAP1 = $4030, CAP2 = $6050 [1] §4.3.10. The CAM registers other than CE are unaffected by either
reset [1] §4.3.10.

### 2.14 Tally counters (CRCT $2C, FAET $2D, MPT $2E)

Three 16-bit network-management counters: CRC errors, frame alignment errors and missed packets
[1] §4.3.11. They count even when the receiver is disabled, are halted (not cleared) while RST is
set, and survive both resets [1] §4.3.11. A rollover from $FFFF to $0000 raises the matching ISR
bit if enabled [1] §4.3.11. The data written to these registers is inverted before latching —
"these registers are cleared by writing all '1's to them", and a written $FFFF reads back as
$0000 [1] §4.3.11, Table 4-1 note 4.

### 2.15 Watchdog timer (WT0 $29, WT1 $2A) and Silicon Revision (SR $28)

The general-purpose watchdog timer is 32 bits, programmed as WT1:WT0, "clocked at ½ the Transmit
Clock (TXC) frequency", counting *down*; it interrupts when it rolls over from $00000000 to
$FFFFFFFF and keeps counting unless stopped (CR.STP) [1] §4.3.12. A hardware or software reset
halts but does not clear it [1] §4.3.12. The Silicon Revision register is read-only and "contains
information on the current revision of the SONIC. The initial silicon begins at 0000h and
subsequent revision will be incremented by one" [1] §4.3.13.

### 2.16 Internal-use registers

The remaining registers belong to the SONIC's own engine and are listed here only because they
appear in the block diagrams and status discussions [1] Table 4-2. On the transmit side, TPS,
TFC, TSA0/1 and TFS hold the packet size, fragment count, fragment start address and fragment
size fetched from the current descriptor; TTDA is the shadow of CTDA from which the SONIC
recovers after a collision [1] Table 4-2, §3.5.3.1. MDT is the read-only maximum deferral timer
[1] Table 4-2. On the receive side, CRBA0/1 is the current write position in the RBA and RBWC0/1
the words remaining, while TRBA0/1 and TBWC0/1 are the saved copies from which the pointers are
recovered when a runt or errored packet is rejected [1] Table 4-2, §3.4.6.2. LLFA holds the
address of the RXpkt.link field the SONIC will re-read when the descriptor list was exhausted
[1] Table 4-2, §3.4.5. ADDR0/1 are address generators [1] Table 4-2. Note the two data-path
oddities flagged in the manual's own table: TPS "reads back the inversion of what has been
written" (Table 4-2 note 1), and a value written to TFS is shifted once in 16-bit mode and twice
in 32-bit mode (Table 4-2 note 2).

### 2.17 Reset state

The two reset modes are not interchangeable [1] §5.4.9. A hardware reset (RESET pin low, or
power-on) puts the part in a defined state; a software reset (CR.RST) "immediately terminates DMA
operations and future interrupts" and parks the state machines idle while leaving the registers
accessible [1] §5.4.9. The documented post-reset register contents [1] Table 5-4:

| Register | Hardware reset | Software reset |
|---|---|---|
| Command | $0094 | $0094 / $00A4 |
| Data Configuration (DCR, DCR2) | see §2.3 | unchanged |
| Interrupt Mask | $0000 | unchanged |
| Interrupt Status | $0000 | unchanged |
| Transmit Control | $0101 | unchanged |
| Receive Control | partially cleared (see §2.5) | unchanged |
| End of Buffer Count | $02F8 | unchanged |
| Sequence Counters | $0000 | unchanged |
| CAM Enable | $0000 | unchanged |

$0094 is RST + STP + RXDIS; hardware reset also: disables receiver and transmitter, halts the
watchdog timer, masks all interrupts, sets NCRS and PTX in the TCR, clears the interrupt status
register, disables all CAM entries including broadcast, disables loopback, selects unlatched bus
retry, disables Extended Bus mode, asserts HOLD from the falling clock edge, disables latched
ready, leaves PCOMP deasserted, and leaves packets accepted on CAM match [1] §5.4.9.

---

## 3. Behaviour

### 3.1 Frame format and the MAC

The SONIC transmits and receives the standard IEEE 802.3 frame: preamble, Start of Frame
Delimiter (SFD), destination address, source address, length/type, data and Frame Check Sequence
[1] §2.0. On transmit the SONIC generates and appends preamble, SFD and (unless inhibited) the
FCS; on receive the preamble and SFD are stripped and "The CRC is passed through to buffer memory
during reception" [1] §2.0. The preamble is 62 bits of alternating 1,0 followed by a 2-bit "1,1"
SFD, and it is "always transmitted in its entirety even in the event of a collision", which
guarantees a minimum collision fragment of 96 bits — 64 preamble/SFD bits plus the 32-bit jam
[1] §1.2.2. The FCS is a 32-bit CRC over the AUTODIN II polynomial X<sup>32</sup>+X<sup>26</sup>+
X<sup>23</sup>+X<sup>22</sup>+X<sup>16</sup>+X<sup>12</sup>+X<sup>11</sup>+X<sup>10</sup>+X<sup>8</sup>+
X<sup>7</sup>+X<sup>5</sup>+X<sup>4</sup>+X<sup>2</sup>+X+1 [1] §2.6. The SONIC does not operate on
the length/type field, does not pad short packets, does not check for oversize packets, and does
not insert the source address — but it can transmit and receive packets of up to 64k bytes, far
beyond the 1518-byte 802.3 maximum [1] §2.3–§2.5.

### 3.2 Address filtering

Three address classes are recognized: **physical** addresses (LSB of the first byte 0) must match
all bits of a CAM entry; **multicast** addresses (LSB 1) are treated identically, compared against
the CAM, so multicast needs no hashing algorithm; and the **broadcast** address (all ones) is
accepted when the RCR's BRD bit is set — the broadcast match runs in parallel with the CAM, so
broadcast and CAM-matched traffic are accepted simultaneously [1] §2.2, §4.1. Promiscuous mode
(PRO) accepts all physical-address packets [1] §2.2. During reception the address comparator
latches the destination address and matches it against the CAM while the packet is still on the
wire; on a match the deserializer hands the rest of the packet to the receive FIFO [1] §1.2.1.

### 3.3 The receive state machine

The **Receive State Machine (RSM)** sequences normal reception and self-reception during
transmission. When the network is idle it watches for activity; once active, it lets the
deserializer fill the receive FIFO. Reception of a packet is prevented or truncated by five
conditions: FIFO overrun (the 32-byte receive FIFO filled before the DMA could drain it), CAM
address mismatch, memory resource error (no buffers left), a collision or other error (when so
configured), and — for undersized or errored frames — the reject filters [1] §1.2.1. At the end of
reception the receive section checks frame alignment, CRC and length (runt) and posts the outcome
in the RCR [1] §1.2.1.

During the SONIC's own transmissions the receive section stays active to monitor the looped-back
copy of the packet: the CRC checker runs, and the source address is compared against the CAM. If
either disagrees with what was transmitted, PMB is set in the transmit status. No data is written
to the receive FIFO during this monitoring unless transceiver loopback is selected [1] §1.2.1,
§1.7.

### 3.4 Receive end-of-packet processing and overflow

When a packet is accepted, the SONIC writes 5 words of descriptor information into the RDA at
CRDA, then reads the RXpkt.link field to advance CRDA (checking its EOL bit), then decides
whether the next packet fits in the current RBA by comparing RBWC0,1 against EOBC; the whole
7-word descriptor access is one block bus operation [1] §3.4.6.1, §5.4.2. If a runt or errored
packet is rejected, the SONIC recovers its pointers from the TRBA0,1/TBWC0,1 shadow registers —
CRBA is not advanced and RBWC is not decremented [1] §3.4.6.2.

The manual names three resource-exhaustion conditions, all reported through the ISR [1] §3.4.7:

| Condition | ISR bit | Meaning and recovery |
|---|---|---|
| Descriptor resources exhausted | RDE | The RDA list hit EOL. The SONIC keeps ownership of the last descriptor and re-reads its link field at the start of each reception; the system appends descriptors by clearing EOL on the old last descriptor, or points CRDA at a new list |
| Buffer resources exhausted | RBE | RRP met RWP. Set when the SONIC *finishes using the second-to-last* buffer — an early warning. Reception stops once the last RRA descriptor is used, and resumes when software adds descriptors, updates RWP, and clears RBE |
| RBA limit exceeded | RBAE | A packet did not fit in the remaining RBA space (EOBC misprogrammed against the buffer size). The packet is truncated, an RDA is *not* written for it, the buffer space is not re-used, and the SONIC fetches the next resource descriptor |

The RBE handshake is delicate: if RBE is cleared before new buffers are added (RWP written),
the SONIC sets RBE again without reading the RRA; and after buffers are added, clearing RBE makes
the SONIC read the RRA immediately if the buffer that raised RBE has since been used [1] §4.3.6,
RBE notes.

### 3.5 The transmit state machine and CSMA/CD

The **Transmit State Machine (TSM)** drives the serializer, preamble generator and jam
generator. Absent a collision, it prefixes the 62-bit preamble and 2-bit SFD, serializes the
frame, and appends the optional 4-byte CRC [1] §1.2.2. The **protocol state machine** implements
the CSMA/CD discipline: it defers to network activity, then waits out the Interframe Gap Timer
(9.6 µs), which is itself split in two — during the first 6.4 µs a carrier restarts the timer,
beyond it the activity is ignored and the remaining 3.2 µs elapse before transmission [1]
§1.2.2. On a collision the SONIC finishes the preamble, sends a 4-byte jam of all ones "to assure
that all nodes on the network sense the collision", then waits a random number of 51.2 µs slot
times chosen by the truncated binary exponential backoff algorithm — a random integer r in
0 ≤ r ≤ 2<sup>k</sup>, k = min(n, 10) for the nth retransmission [1] §1.2.2. "If a collision
occurs on the 16th transmit attempt, the SONIC aborts transmitting the packet and reports an
'Excessive Collisions' error in the Transmit Control register" [1] §1.2.2.

Transmission stops after EOL is detected, or aborts on FIFO underrun, byte count mismatch,
excessive collisions, or (if enabled) excessive deferral; software can halt it with HTX after the
current packet's status is written [1] §3.5.3.2. In the event of a collision the SONIC recovers
its position in the TDA from the TTDA shadow register and retransmits the packet up to 15
times [1] §3.5.3.1.

### 3.6 FIFOs, thresholds and the DMA engine

Both FIFOs are 4 bytes wide by 8 deep — 32 bytes each — arranged as byte-lane memories with one
of four write/read pointers selected by the byte-ordering mode [1] §1.4. The **receive FIFO**
bridges the 8-bit deserializer to the 16/32-bit system side: the programmable RFT1,0 threshold
determines how many words accumulate before a DMA request fires, and each bus tenure moves either
exactly the threshold count (block mode, BMS=1) or everything the FIFO holds (empty/fill mode,
BMS=0) [1] §1.4, §1.4.1. At end of packet the SONIC fills the last word or long word with a
$FF fill byte, then writes the receive status into the FIFO, so "the entire packet, including any
fill bytes and the received packet status" is buffered to memory, always on word or long-word
boundaries [1] §1.4.1. The **transmit FIFO** fills from memory before transmission starts and is
replenished by repeated DMA requests; the TFT1,0 threshold sets the fill level at which the
serializer begins, and the byte-ordering state machine discards the extraneous bytes the DMA
writes for fragments starting on odd boundaries [1] §1.4.2. If the system is too slow to drain
the receive FIFO the result is the RFO overrun interrupt; too slow to refill the transmit FIFO
and the transmission aborts with FU [1] §4.3.6, §4.3.4.

### 3.7 Buffer management in memory

Five named areas in system memory carry the packet traffic [1] §3.1:

| Area | Direction | Contents |
|---|---|---|
| RRA — Receive Resource Area | system → SONIC | Circular queue of resource descriptors (buffer pointers + word counts) |
| RBA — Receive Buffer Area | SONIC → memory | Raw packet data, contiguous, one or more packets per buffer |
| RDA — Receive Descriptor Area | SONIC → memory | Linked list of per-packet status/control descriptors |
| TDA — Transmit Descriptor Area | bidirectional | Linked list of per-packet transmit descriptors |
| TBA — Transmit Buffer Area | memory → SONIC | Packet data, scattered across fragments at arbitrary byte alignment |

A packet is never scattered on receive — "the SONIC will not scatter a packet into multiple
buffers or fragments" — and the EOBC mechanism guarantees a maximum-sized packet always fits
contiguously in the current RBA [1] §3.4.2. Descriptor fields are 16-bit quantities aligned to
word boundaries (16-bit mode) or long-word boundaries (32-bit mode); fragments in the TBA alone
may sit on any byte boundary [1] §3.3.

The **resource descriptor** in the RRA is four words: RXrsrc.buff_ptr0, RXrsrc.buff_ptr1 (the
32-bit RBA start address), RXrsrc.buff_wc0, RXrsrc.buff_wc1 (the buffer size in 16-bit words); the
SONIC never writes into the RRA, and in 32-bit mode the upper word of each long word is unused
[1] §3.4.1, §3.4.4.2. The RRA is a ring bounded by RSA/REA, read at RRP and written by the system
at RWP; the SONIC advances RRP by 4 words (16-bit) or 4 long words (32-bit) per descriptor and
wraps at REA [1] §3.4.1.

The **receive packet descriptor** in the RDA is seven words [1] §3.4.3, Figure 3-5:

| Word | Field | Written by | Meaning |
|---|---|---|---|
| 1 | RXpkt.status | SONIC | Copy of the RCR (filters + per-packet status, §2.5) |
| 2 | RXpkt.byte_count | SONIC | Packet length from destination address through FCS |
| 3–4 | RXpkt.pkt_ptr0/1 | SONIC | The CRBA0,1 value — where the packet starts in the RBA |
| 5 | RXpkt.seq_no | SONIC | RBA and packet sequence numbers (the RSC pair, §2.12) |
| 6 | RXpkt.link | system | 15-bit pointer to the next descriptor; LSB is EOL |
| 7 | RXpkt.in_use | both | Handshake: system writes non-zero to hand over, SONIC writes $0000 to release |

The in_use handshake has one asymmetry: on the last descriptor of the list (EOL set) the SONIC
"maintains ownership of the descriptor" until the system appends more descriptors, releasing it
after the next packet [1] §3.4.3.

The **transmit descriptor** in the TDA is variable length, one packet per descriptor [1] §3.5.1:

| Words | Field | Meaning |
|---|---|---|
| 1 | TXpkt.status | Written by the SONIC at end of transmission (§2.6 bits + collision count) |
| 1 | TXpkt.config | PINTR, POWC, CRCI, EXDIS in bits 15–12, loaded into the TCR |
| 1 | TXpkt.pkt_size | Byte count of the whole packet |
| 1 | TXpkt.frag_count | Number of fragments |
| 3 per fragment | TXpkt.frag_ptr0/1, TXpkt.frag_size | 32-bit fragment address (any alignment) and byte count (minimum 1) |
| 1 | TXpkt.link | 15-bit pointer to the next descriptor; LSB is EOL |

On the transmit command the SONIC reads the descriptor (6 accesses for the first fragment, 3 for
each additional fragment's fields, then 2 accesses at end of transmission to write status and read
the link), moving back and forth between the TDA and the TBA until the whole list is sent [1]
§3.5.3.1, §5.4.2. Descriptors can be appended mid-queue: create the new descriptor with EOL=1,
clear EOL on the old last descriptor, and re-issue TXP — "If the SONIC is currently transmitting,
the Transmit command has no effect and continues transmitting until it detects EOL = 1" [1]
§3.5.4.

### 3.8 Loopback and diagnostics

Three loopback modes test progressively more of the transmit path [1] §1.7:

| LB1 LB0 | Mode | Data path | CSMA/CD |
|---|---|---|---|
| 0 1 | MAC loopback | Looped at the MAC; nothing leaves the chip; the ENDEC interface pins are not driven (but an ENDEC clock must still be supplied) | Not fully followed |
| 1 0 | ENDEC loopback | Looped inside the (internal or external) ENDEC; collision inputs ignored | Followed |
| 1 1 | Transceiver loopback | Looped at the external transceiver, as always happens physically; the difference from normal operation is that the receive FIFO is filled and the packet is buffered | Followed; affected by network activity |

Transmit and receive status and interrupts remain active in loopback — "it is as if the packet
was transmitted and received by two separate chips that are connected to the same bus and
memory" [1] §1.7. MAC loopback accepts only one queued packet, because the MAC section generates
no interframe gap between back-to-back packets and so does not let the receive section update
status [1] §1.7.1.

### 3.9 Network management

The SONIC "fully supports the Layer Management IEEE 802.3 standard", exposing per-packet
statistics in the RCR/TCR copies posted into the receive and transmit descriptors, plus the three
tally counters (§2.14) [1] §1.8. The manual's statistics table maps the standard counters to
register bits: frames transmitted OK (PTX), single/multiple collision frames (NC0–NC4), deferred
transmissions (DEF), late collisions (OWC), excessive collisions (EXC), excessive deferral (EXD),
internal MAC transmit errors (BCM, FU), frames received OK (PRX), multicast/broadcast frames
received (MC, BC), FCS errors (CRCT + CRCR), alignment errors (FAET + FAER), and frames lost to
internal receive errors (MPT + RFO) [1] Table 1-1.

### 3.10 Bus mastering

The SONIC requests the bus when a FIFO threshold is reached or a descriptor area must be
accessed, and "when the SONIC moves from one area in memory to another (e.g., RBA to RDA), it
always deasserts its bus request and then requests the bus again" [1] §5.4.1. Two arbitration
protocols exist, selected by the BMODE strap: the National/Intel HOLD/HLDA two-way handshake, in
which "The CPU ... can preempt the SONIC from finishing the block transfer by deasserting HLDA",
letting a higher-priority device steal the bus; and the Motorola BR/BG/BGACK three-way
handshake, in which the SONIC waits for BGACK, AS and DSACK0,1 (and STERM in asynchronous mode)
to be deasserted before taking mastership, and "It can not be preempted from the bus" [1]
§5.4.1. In both modes the SONIC inserts one idle holding state Th after its last cycle before
releasing the bus [1] §5.4.1.

Every bus tenure is a block operation — 4 accesses in the RRA, 7 in the RDA, 2, 3 or 6 in the
TDA, 4 in the CDA — transferring either the programmed threshold count (block mode) or the whole
FIFO contents (empty/fill mode) [1] §5.4.2. Three status pins S2–S0 report which area is being
accessed (TDA, TBA, RBA data, RBA address, RRA, RDA, CDA, or idle), stable for the duration of the
block except for the notated transitions around register access, source-address writes and RDA
field boundaries [1] §5.4.3, Table 5-2.

Master cycles run in T1/T2 states with programmable wait states (DCR WC1,0 inserts 0–3 T2(wait)
states; the memory system may also withhold DSACK0,1/STERM or RDYi) [1] §5.4.5.1. The
synchronous/asynchronous choice (DCR SBUS) fixes whether the ready inputs are sampled
asynchronously on falling edges or must meet setup to a rising edge; asynchronous cycles can
complete in as little as 2 bus clocks if setup is met [1] §5.4.5.3–§5.4.5.5. Bus errors are
handled by **Bus Retry**: asserting BRT forces the SONIC to finish the current cycle and get off
the bus, then retry — immediately in unlatched mode, or only after the ISR's BR bit is cleared in
latched mode [1] §5.4.6.

### 3.11 Slave access and the on-chip memory arbiter

Register (slave) accesses are driven by CS, SAS, SRW/SWR and RA<5:0>. The SONIC asserts SMACK one
to two bus clocks into the cycle, drives read data on the same edge as SMACK, latches write data
exactly 2 clocks after SMACK, and terminates with DSACK0,1 (Motorola mode) or RDY0
(National/Intel mode) a further 2 to 2½ clocks later [1] §5.4.7. If CS arrives while the SONIC
is a bus master it completes its current cycle first, so SMACK can arrive 5 bus clocks later, plus
any wait states [1] §5.4.7. For shared-buffer-memory designs an on-chip arbiter accepts MREQ and
answers SMACK, resolving host-vs-SONIC access to the shared memory; CS and MREQ "must not be
asserted concurrently" and successive assertions must be separated by at least two bus clocks
[1] §5.4.8, §5.2. (On the Quadra this arbitration role belongs to the board's I/O adapter
instead — Relayer generates the chip selects and DSACKs for the I/O bus [2] p. 6.)

### 3.12 Interrupts

The single INT output is asserted when any ISR bit set is also enabled in the IMR; disabled
sources never assert the pin [1] §4.3.5, §4.3.6, §5.2. In Motorola mode (BMODE = 1) INT is active
low [1] §5.2. ISR bits are cleared by writing 1 to them, and — in latched bus-retry mode — the
BR bit gates all further DMA until cleared [1] §4.3.6.

On the Quadra logic board the interrupt path is fully documented: "the built-in video interrupt
signal, and the Ethernet controller interrupt signal are routed through an OR gate to generate a
signal called /SLOTIRQ. This signal is connected to the CA1 input of VIA2", which raises a level
2 interrupt to the MC68040 [2] p. 34, [3] p. 37. The first-level dispatcher then polls VIA2's
port bits, where PA0 is the Ethernet IRQ ([2] Table 2-6, [3] Table 2-6; see also
[via.md](via.md)). Under A/UX the interrupt mapping is remapped so that Ethernet interrupts
arrive at level 3 instead [2] Table 2-7, [3] Table 2-7.

### 3.13 Reset behavior

The RESET pin, driven low, resets the part after 10 transmit clocks (or 10 bus clocks when the
bus clock is the slower of the two) [1] §5.4.9, §5.2. After power-on, "the SONIC must be hardware
reset before it will become operational", and a hardware reset sets the CR's RST bit — the SONIC
stays in software reset until the driver clears it [1] §5.4.9, Table 5-4. The two reset modes
"are not interchangeable since each mode performs a different function": hardware reset
reconfigures the registers (§2.17), software reset halts the engines and terminates DMA and
interrupts while preserving nearly all register contents [1] §5.4.9.

### 3.14 Electrical and AUI interface

The chip is 5 V ± 5% CMOS, 0–70°C, with a maximum average operating supply current of 80 mA;
outputs swing to VOH ≥ 3.0 V at −8 mA and VOL ≤ 0.4 V at 8 mA; absolute maximums include VCC of
−0.5 to 7.0 V, 500 mW power dissipation and a 1.5 kV ESD rating [1] §7.0. The AUI differential
transmit pair produces ±550 mV to ±1200 mV into the standard 78 Ω termination with 270 Ω pull-downs
to ground on each side, drives up to 50 meters of twisted-pair AUI cable, and requires a pulse
transformer between the pair and the AUI interface [1] §6.1, §7.0. Receive and collision inputs
are terminated with two 39 Ω series resistors and a pulse transformer, and their squelch
thresholds are −175 mV (reject) to −300 mV (accept) [1] §6.1.1, §6.1.2, §7.0. The decoder tolerates
up to 18 ns of bit jitter and detects end-of-frame within one and a half bit times [1] §6.1.1.
The ENDEC is pin-configured for half- or full-step transmit idle via the SEL pin: SEL low
(Ethernet I) leaves TX+ positive with respect to TX− at idle on the transformer primary, SEL high
(IEEE 802.3) leaves the pair equal [1] §6.1, §5.2.

---

## 4. Programming model

The data manual prescribes the sequences below; Apple's contribution on the Quadra is the driver
itself — "The ROM software includes a new driver to support the Sonic IC. Developers can contact
Apple Evangelism to obtain more information about built-in Ethernet, FriendlyNet, and Apple AUI"
[2] p. 43, [3] p. 47 — plus the board wiring and interrupt routing described in §1.3.

### 4.1 Bring-up from reset

A hardware reset leaves the SONIC holding itself in software reset (CR = $0094, RST set), with
interrupts masked and the receiver disabled [1] §5.4.9, Table 5-4. Because a hardware reset
clears only DCR bits 15 and 13 and leaves the rest indeterminate from a power-on state, "the
first thing the driver software does to the SONIC should be to set up this register" — the DCR
(and DCR2) can only be written while RST is set [1] §4.3.2, §4.3.7. The opening sequence is
therefore:

1. Hardware-reset the part (RESET low for ≥ 10 TXC, §3.13).
2. While RST is set, program the DCR — bus mode options, wait states, data width (DW), block/empty-
   fill selection (BMS), and both FIFO thresholds — and the DCR2 if the extended options are
   needed [1] §4.3.2, §4.3.7.
3. Initialize the descriptor page and CAM (§4.2, §4.3).
4. Clear RST; the SONIC becomes operational [1] §4.3.1.

### 4.2 Descriptor page initialization

All descriptor areas live within 32k-word (16-bit) or 16k-long-word (32-bit) pages placed
"anywhere within the 32-bit address range by loading the upper 16 address lines into the UTDA,
URDA, and URRA registers" [1] §3.4.4.1. The areas may share a base (URRA = URDA = UTDA) provided
they do not overlap [1] §3.2.3.

For the receive side [1] §3.4.4.2–§3.4.4.4:

1. Write the resource descriptors (buff_ptr0/1, buff_wc0/1) into the RRA; the word count must be
   even in 32-bit mode and the buffer pointer aligned to a word (16-bit) or long word (32-bit).
2. Load RSA, REA, RRP and RWP, respecting the RWP-pointing rule of §2.11.
3. Issue the Read RRA command (CR.RRRA) — one block operation fills CRBA0,1 and RBWC0,1; the bit
   clears itself when done. "Generally this command is only issued during initialization"; at all
   other times the SONIC reads the RRA on its own as buffers are consumed [1] §3.4.4.2.
4. Link the receive descriptors through RXpkt.link (EOL=1 on the last), write a non-zero
   RXpkt.in_use into each, and load CRDA with the address of the first descriptor [1] §3.4.4.3.
5. Program EOBC as a "false bottom": $02F8 (760 words/1520 bytes) is right for 32-bit mode
   (matching the hardware-reset value), 759 words (1518 bytes) for 16-bit mode; to force one
   packet per RBA, set EOBC 2 words (32-bit) or 1 word (16-bit) below the buffer size [1]
   §3.4.4.4. EOBC must exceed the largest packet to be accepted, or packets will hit RBAE [1]
   §3.4.7.

For the transmit side, initialize the TDA descriptors and load UTDA/CTDA with the address of the
first TXpkt.status field before issuing TXP [1] §3.5.3.

### 4.3 CAM load procedure

The CAM cannot be written directly; it is loaded by DMA from a CAM Descriptor Area that "must
reside within the same 64k page as the Receive Resource Area" [1] §4.1.1. Each CDA descriptor is
four 16-bit words — the CAM entry pointer value followed by the three 16-bit CAM cells — and one
extra field after the last descriptor holds the CAM Enable mask [1] §4.1, Figure 4-2. The manual's
procedure [1] §4.1.1:

1. Initialize URRA (the CDA shares its page).
2. Initialize the CDA descriptors and the trailing enable mask.
3. Load the CDC with the number of descriptors (1–16; only the low 5 bits are used).
4. Load the CDP with the first descriptor's address — it must be reloaded before every Load CAM.
5. Set LCAM in the CR.

A CAM load defers to any transmission or reception in progress; on completion the CDP points past
the enable field, the CDC is zero, LCAM clears, and LCD sets in the ISR [1] §4.1.1. To *read* the
CAM back, place the SONIC in software reset, select the entry in the CEP, and read CAP2, CAP1,
CAP0 [1] §4.3.10.

### 4.4 Starting the receiver

With the RRA, RDA and buffers prepared, set RXEN. From then on the SONIC buffers every accepted
packet autonomously: at the start of each reception it re-checks the EOL bit it remembered from
the last RXpkt.link — if the system has since cleared EOL (appended descriptors), reception
continues into the new part of the list; if EOL is still set, reception ceases and RDE is raised
[1] §3.4.5, §3.4.7. PKTRX fires after each packet's descriptor is written [1] §4.3.6.

### 4.5 Transmitting packets

Queue one descriptor per packet, link them (EOL=1 only on the last), load the address of the
first descriptor into UTDA/CTDA, and set TXP [1] §3.5.3. From the first descriptor the SONIC loads
TCR ← TXpkt.config, TPS ← pkt_size, TFC ← frag_count, TSA0/1 ← frag_ptr0/1, TFS ← frag_size, and
transmits; CTDA is loaded from TXpkt.link only after all fragments have been read and
successfully transmitted (and not if HTX halted the queue) [1] §3.5.3.1. Multi-fragment packets
are fetched fragment by fragment; a descriptor consisting of a single Source Address fragment is
the manual's suggested idiom since the SONIC does not insert source addresses itself [1] §2.3.
Transmit completion is signaled by TXDN and errors by TXER, with the detail in TXpkt.status [1]
§4.3.6.

### 4.6 Interrupt service

Service order follows the ISR: write 1 to clear each handled bit [1] §4.3.6. The three resource
conditions need the recovery protocols of §3.4: for RDE, append descriptors and clear EOL (or
repoint CRDA); for RBE, add resource descriptors, bump RWP, *then* clear the bit; for RBAE, no
descriptor exists for the truncated packet — fix EOBC [1] §3.4.7. The heartbeat-lost bit (HBL)
means the transceiver sent no SQE pulse in the first 6.4 µs after a transmission — diagnostic
information about the external media path, which on these machines lives in the FriendlyNet
adapter [1] §4.3.6; [2] p. 18.

### 4.7 The Quadra driver's environment

What the Developer Notes establish about the ROM's Ethernet support is structural: the ROM
carries a dedicated Sonic driver [2] p. 43, [3] p. 47; the interrupt arrives as a level-2 VIA2
interrupt (level 3 under A/UX) with the Ethernet request visible on VIA2 PA0 [2] pp. 34–35,
[3] pp. 37–38; and the media interface is the AAUI connector with FriendlyNet adapters for thick,
thin and 10BaseT cable [2] p. 18, [3] p. 21. The SONIC side of that connector is the part's own
ENDEC, whose differential pairs are meant to reach the transceiver through a pulse transformer
[1] §6.0, §6.1 — so the transceiver function sits in the external adapter rather than on the main
logic board (*inferred — unverified*: the Developer Notes do not describe the signals between the
SONIC and the AAUI connector, and the data manual's network-interface example is generic).

### 4.8 Loopback procedure

The manual's loopback recipe [1] §1.7.1:

1. Initialize the transmit and receive areas as for normal operation.
2. Load a CAM entry with the packet's destination address if address recognition is being
   verified.
3. Load a CAM entry with the source address (if different) to avoid a spurious PMB.
4. Program the RCR with the desired filter and the loopback mode LB1,0.
5. Set TXP and RXEN.

The operation completes when the looped packet has been fully received (or rejected on address
mismatch), with normal status and interrupts in both control registers [1] §1.7.1.

---

## 5. Quirks & errata

- **TXP and LCAM are mutually exclusive.** "This bit [TXP] must not be set if a Load CAM operation
  is in progress (LCAM is set). The SONIC will lock up if both bits are set simultaneously" — and
  symmetrically for LCAM during transmission [1] §4.3.1. A driver that queues a transmit while a
  CAM load is pending must serialize the two commands.
- **The DCR and DCR2 are write-gated on reset.** Writes while RST is clear are silently ignored
  [1] Table 4-1 note 3, §4.3.7. The CAM address ports return invalid data outside reset, and CE
  can only be written in reset [1] Table 4-1 notes 1–2.
- **The EOBC boundary must be crossed, in order.** If an RBA's remaining count lands below EOBC
  without a previous packet having crossed the boundary from above, "the test for RBWC0,1 < EOBC
  will not work properly and the SONIC will not fetch a new buffer", producing an RBAE overflow
  [1] §3.4.2.2.
- **RWP has three legal values.** It must point at an RXrsrc.ptr0 field, or at the RSA or REA
  value; anything else and the RWP = RRP buffer-exhausted comparison never becomes true [1]
  §3.4.4.2.
- **RBE is an early warning with a protocol.** It sets while one buffer still remains; clearing it
  before writing RWP makes the SONIC re-set it without touching the RRA [1] §4.3.6, RBE notes.
- **Tally counters are write-inverted.** Writing $FFFF reads back $0000; they are cleared by
  writing all ones, and both resets leave them alone [1] §4.3.11, Table 4-1 note 4.
- **TPS reads back inverted and TFS is write-shifted** (once in 16-bit, twice in 32-bit mode) [1]
  Table 4-2 notes 1–2 — a trap for software that round-trips these internal registers.
- **PINTR must alternate packets.** After a PINT interrupt, PINTR in the TCR must be cleared
  before being set again for the next packet; "The only effective way to do this is to set PINTR
  to a 1 no more often than every other packet" [1] §4.3.4.
- **MAC loopback takes exactly one packet.** The MAC generates no interframe gap between
  back-to-back transmissions, so the receive section cannot update status for a second queued
  packet [1] §1.7.1. NCRS and CRSL are always set in MAC loopback [1] §4.3.4.
- **PMB is meaningless after a receive FIFO overrun**, since the self-received packet was not
  completely monitored; and PMB is always low when CRCI suppressed the FCS [1] §4.3.4.
- **A minimum-size packet can still be transmitted short.** The SONIC neither pads to 64 bytes
  nor rejects oversize frames (up to 64k bytes pass through); the driver must extend pkt_size and
  the fragment sizes to at least 64 bytes itself [1] §2.5.
- **The source address must be supplied — twice.** Once in the packet data and once in the CAM,
  or every transmission reports PMB [1] §2.3, §1.7.1.
- **CS and MREQ must never overlap**, and successive assertions need two bus clocks of separation;
  CS must stay high for at least one bus clock after deassertion [1] §5.2, §5.4.7.
- **PCNM never compresses broadcast packets**, regardless of the BRD filter bit; and RJCM does
  not invert BRD/PRO/AMC — setting RJCM and BRD together *accepts* all broadcasts while
  rejecting CAM matches [1] §4.3.7.
- **PCM and PCNM together are illegal** ("not allowed"), and if both are clear the PCOMP output
  stays tri-stated [1] §4.3.7.
- **The DP83932B changed the slave-cycle termination wiring.** In the DP83932, SDS ended
  Motorola-mode slave cycles; in the DP83932B (and DP83932A), SAS does that job, so SDS "is no
  longer needed, and does not have to be driven" [1] §5.2. Board and adapter designers moving
  between steppings should note the change.
- **An asynchronous cycle can complete in two clocks.** If DSACK0,1/STERM/RDYi setup is met during
  T1, "the full asynchronous bus cycle will take only 2 bus clocks. This may be an unwanted
  situation" — deassert them during T1 if it is [1] §5.4.5.3, §5.4.5.5.
- **Bus retry leaves dangling handshakes.** If DSACK0,1, STERM or RDYI remain asserted after BRT,
  the next memory cycle "may be adversely affected"; and unless LBR is set, BRT must stay
  asserted until after the Th state [1] §5.4.6.
- **The manual's DCR2 field list and bit map disagree**: the field table names an LRDY (latched
  ready) field, but the bit assignments give bits 11–5 and 3 as "must be written with zeroes" [1]
  §4.3.7. Which published description matches the silicon is not resolved by the available
  evidence (§6).

---

## 6. Open questions

- **The SONIC register window's base address on the Quadra.** The Developer Notes state that the
  SONIC's registers are among the I/O devices reached through the JDB/Relayer adapter and show a
  simplified I/O address map (Figure 1-2) [2] pp. 8–9, but the map is reproduced only as artwork
  and its Ethernet address range is not stated in text. The mapping from CPU address bits to the
  SONIC's RA<5:0> register-select pins (and thus the register stride as seen by the 68040) is
  likewise a logic-board detail absent from both the Developer Notes and the data manual.
- **DP83932 vs DP83932B silicon on the Quadra boards.** The Developer Notes name "DP83932"
  without a suffix ([2] p. 5, [3] p. 5, [4] p. 5), and the Quadra 950 calls its part "a 25 MHz
  version of the Sonic, the DP83932" [4] p. 5, while the available data manual documents the
  DP83932B. Whether the Quadra boards carry the original DP83932, the DP83932A, or the B, and
  what the Silicon Revision register reads on each, is not evidenced. The B-step's one
  documented functional difference (SDS no longer required, §5) suggests the steps are
  interchangeable at the board level, but that is *inferred — unverified*.
- **Which DCR configuration Apple's driver chooses** — 16- vs 32-bit data width (DW), block vs
  empty/fill mode (BMS), FIFO thresholds and wait states. The manual documents the options and
  their trade-offs [1] §4.3.2, but no available Apple document or observable ROM listing in the
  evidence records the values the Quadra driver writes.
- **Whether BMODE is strapped to Motorola mode on the Quadra.** The 68040's big-endian byte
  order and the manual's BMODE description (BMODE=1 selects big-endian, Motorola-style BR/BG/
  BGACK handshake and active-low INT [1] §5.2, §1.3) make it near-certain, but the strap itself
  is not documented — *inferred — unverified*. Likewise EXBUS/STERM usage and the
  extended-bus pins.
- **Where the hardware (MAC) address of the built-in Ethernet port is stored.** The SONIC has no
  non-volatile storage of its own, so the node's burned-in address must come from an off-chip
  source; neither the Developer Notes nor the data manual describes such a PROM or its location
  on the Quadra logic board. The AAUI/FriendlyNet documentation [2] p. 18, [3] p. 21 does not
  cover it.
- **The exact interrupt electrical level at the SONIC's INT pin in the Quadra's OR-gate
  network.** The aggregate signal is named /SLOTIRQ and VIA2 CA1 receives it [2] p. 34, [3]
  p. 37; the manual defines INT as active-low only for BMODE=1 [1] §5.2. The combination is
  consistent with active-low wiring but the board-level polarity is not stated — *inferred —
  unverified*.
- **DCR2's LRDY field**: no bit position is assigned in the manual's own bit map (§5). Whether
  LRDY exists on the DP83932B, or the field list is a vestige of an earlier revision, is not
  resolvable from the available material.
- **Whether the Quadra 950's "25 MHz version"** is simply a speed-binned DP83932B (whose AC
  tables include a 25 MHz column [1] §7.0) or a distinct ordering part; the Developer Note names
  no separate part number [4] p. 5.

---

## References

1. National Semiconductor Corporation, *DP83932B SONIC™ Systems-Oriented Network Interface
   Controller* (data manual) — §General Description and §Features (integrated ENDEC, 32-byte
   FIFOs, 16-address CAM, 132 PQFP, <5% bus bandwidth); §1.1–§1.2 (ENDEC and MAC units, receive
   and transmit state machines, CSMA/CD deferral/backoff/jam); §1.3 (data width and byte
   ordering, BMODE); §1.4 (FIFO threshold logic); §1.5–§1.7 (registers, bus interface, loopback);
   §1.7.1 (loopback procedure); §1.8 and Table 1-1 (network management statistics); §2.1–§2.7
   (frame format, addressing, FCS polynomial, MAC conformance); §3.1–§3.5 (buffer management:
   RRA/RBA/RDA/TDA/TBA, descriptor formats Figures 3-3, 3-5, 3-12; EOBC; overflow conditions;
   transmit process and dynamic descriptor append); §4.0–§4.3.13 (register programming model
   Figure 4-3, Tables 4-1 to 4-3, per-register descriptions); §5.0–§5.4.9 (pin descriptions
   Table 5-1, bus mode compatibility Table 5-3, bus status Table 5-2, arbitration, block
   transfers, master and slave cycles, bus retry, on-chip memory arbiter, chip reset and
   Table 5-4); §6.0–§6.1.4 (network interfacing, Manchester encoder/decoder, squelch, collision
   translator, oscillator and crystal Table 6-1); §7.0 (absolute maximum ratings, DC and AUI
   specifications, bus-clock AC timing at 20 MHz and 25 MHz).
2. Apple Computer, Inc., *Developer Note: Macintosh Quadra 700*, Developer Press, 1991 —
   §"Summary of major features" p. 2 (Ethernet by way of Apple AUI (AAUI)); §"Design
   architecture" pp. 3–6 (three buses, SONIC on the I/O bus, I/O bus clock 15.6672 MHz,
   third-party IC list including "Sonic, the DP83932 Ethernet controller IC made by National
   Semiconductor", JDB and Relayer functions); Figure 1-2 p. 9 (simplified I/O address map);
   §"Ethernet connector" p. 18 (AAUI and FriendlyNet adapters); §"Macintosh Quadra 700 Direct
   Slot interrupt handling" pp. 34–35 (Ethernet IRQ via /SLOTIRQ into VIA2 CA1, level 2;
   Table 2-6 VIA2 PA0 = Ethernet IRQ; Table 2-7 A/UX level 3); §"Support for Sonic Ethernet
   controller" p. 43 (ROM driver).
3. Apple Computer, Inc., *Developer Note: Macintosh Quadra 900*, Developer Press, 1991 —
   §"Summary of major features" p. 2; §"Design architecture" pp. 3–5 and §"I/O bus adapter ICs:
   JDB and Relayer" p. 7 (I/O bus 15.6672 MHz, Relayer chip select/DSACK generation, Sonic in
   the third-party IC list); §"Ethernet connector" p. 21 (AAUI, FriendlyNet adapters for thick,
   thin and 10BaseT); §"Macintosh Quadra 900 Direct Slot interrupt handling" pp. 37–38
   (/SLOTIRQ into VIA2 CA1, Table 2-6 PA0 = Ethernet IRQ, Table 2-7 A/UX level 3);
   §"Support for Sonic Ethernet controller" p. 47 (ROM driver).
4. Apple Computer, Inc., *Developer Note: Macintosh Quadra 950*, Developer Press, 1992 —
   §"Summary of major features" (Ethernet by way of an Apple AUI connector); §"Faster I/O bus"
   p. 5 (I/O bus clock 24.28416 MHz "provides better throughput on Ethernet"; "a 25 MHz version
   of the Sonic, the DP83932 Ethernet controller IC made by National Semiconductor").
