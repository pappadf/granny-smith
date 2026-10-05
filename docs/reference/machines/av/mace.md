# MACE — the AV Ethernet controller

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, the Curio/PSC division of labor,
   addressing, clocking
2. [Register file](#2-register-file) — the $50F1C000 window: 32 registers on a 16-byte stride, full bit
   maps and reset values
3. [Behaviour](#3-behaviour) — receive and transmit datapaths, address filtering and the LADRF hash,
   interrupts, counters, the host bus interface, reset and loopback
4. [Programming model](#4-programming-model) — the shipped Ethernet driver: configuration records,
   bring-up, the address PROM, PSC DMA, interrupt service, the loopback self-test
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The **MACE** — AMD's "Media Access Controller for Ethernet" — is the Ethernet controller of the AV
Quadras. The Am79C940 device embodies the Media Access Control (MAC) and Physical Signalling (PLS)
sub-layers of IEEE 802.3 with an integrated Manchester encoder/decoder (MENDEC), an AUI port for an
external Medium Attachment Unit, a 128-byte receive FIFO and a 136-byte transmit FIFO, and a 16-bit
synchronous slave-bus interface expressly designed to sit in front of an external DMA engine or I/O
processor [1] p. 1. It is a slave-only part: the MACE never masters the host bus, and all data
movement is initiated by the host or by its DMA controller through register or FIFO-direct accesses
[1] p. 1.

On the AV machines the MACE is not a discrete Am79C940. The core is a macrocell inside **Curio**,
Apple's combined I/O ASIC — "a multipurpose I/O chip that contains a Media Access Controller for
Ethernet (MACE), a SCSI controller, and a Serial Communications Controller (SCC)" [2] p. 16. The ROM
never addresses Curio as a unit: each core has its own base address and its own driver, and the
Ethernet driver treats the MACE as a discrete chip. The SCSI core (53C96-class) is covered at
[53C96](../../hardware/scsi/ncr-53c96.md) and the SCC core at [SCC](../../hardware/scc.md); this page
covers the Ethernet core and the Apple board-level parts that belong to it.

The network attachment is Apple's **AAUI** connector — AUI on a compact 14-pin connector with the
transceiver powered from the host — whose pin assignments the Developer Note publishes as the
"Ethernet port" (§1.5). The AV machines are the first Quadras whose built-in Ethernet is *not* the
National DP83932 SONIC used elsewhere in the Quadra line; for that part see [SONIC](../../hardware/sonic.md).
Both models "contain built-in circuitry for Ethernet I/O" [2] p. 6.

Two further identifications avoid confusion. First, the ROM also contains the strings "MACE 3-to-1"
and "MACE 6-to-1" — that is the Sound Manager's *Macintosh Audio Compression and Expansion*, an
unrelated audio codec [3]. Second, the driver that drives this part is the declaration-ROM `.ENET`
driver "Built-In Ethernet Driver v1.0 ; © Apple Computer, Inc. 1992-1993", whose version string is
present in the ROM image at $4080D9E [3].

### 1.2 Machines that carry it

| Machine | Apple codename | Ethernet controller | Notes |
|---|---|---|---|
| Macintosh Quadra 840AV | Cyclone | MACE in Curio | 40 MHz 68040; built-in Ethernet [2] p. 6 |
| Macintosh Centris 660AV (later Quadra 660AV) | Tempest | MACE in Curio | 25 MHz 68040; built-in Ethernet [2] p. 6 |
| 33 MHz Cyclone/Tempest variants | — | MACE in Curio | machine-ID keys 43 and 79 exist in the ROM's MACE configuration records but no such machine shipped [3] |
| Power Macintosh 6100/7100/8100 | PDM | the same MACE core in the same Curio ASIC generation | different base ($50F0A000), AMIC DMA instead of PSC, a different driver; the contrast is drawn in §4.10 [4] |

The Curio silicon revision is visible through the MACE chip-identification register (§2.9): the
shipping AV machines report $0940, which the later PDM driver identifies as the "B0" Curio; a $0941
value exists in drivers' revision tables as the earlier "A2" Curio [3], [4].

### 1.3 Division of labor

The AV Ethernet subsystem is split across four owners, and the split matters: most of what a
re-implementation must build is *not* in the Am79C940 data manual.

| Function | Owner |
|---|---|
| MAC and PLS sub-layers, MENDEC, FIFOs, address matching, LADRF hash, counters, transmit/receive state machines | the MACE core (§2, §3) |
| Host-bus slave decode, byte-lane arrangement, FIFO-direct wiring, routing of RDTREQ/TDTREQ into the PSC's DMA channels | the Curio glue around the core (§3.8) |
| DMA engines, channel registers, interrupt routing to the 68040 | the PSC — channels ENetRd/ENetWr [2] p. 29, [PSC](psc.md) |
| Station-address PROM, AAUI connector, crystals | the Apple board (§4.3); the PROM is *not* a MACE feature and the data manual says nothing about it |
| Buffer chains, statistics, protocol multiplexing | the ROM driver (§4) |

The Developer Note names the two PSC DMA channels that serve this part: **ENetRd** and **ENetWr**,
each 16 bits wide with a 16-byte internal buffer, driven through two register sets each
[2] p. 29, and each channel is what the data manual's "user defined DMA engine" is on this platform
[1] p. 1.

### 1.4 Addressing and decode

The MACE register file decodes at physical **$50F1C000**, and the Apple address PROM at
**$50F08000** (§4.3). The shipped driver takes neither address from a compile-time constant: it
obtains the machine type from Gestalt, fetches a per-machine `'ecfg'` resource, and copies a
configuration record into its globals. Four byte-identical records exist in the shipping ROM, at
$4081EE30, $4081EE90, $4081EEF0 and $4081EF50, keyed on machine types 43, 78, 60 and 79, each
containing `MACEBase $50F1C000`, `EnetPROM $50F08000`, `XmitFrmCtl $01`, `RecvFrmCtl $00`,
`FIFOCfgCtl $2C`, `MACCfgCtl $03` [3] (*observed* as raw data in the ROM image).

Register *n* of the data manual's numbering sits at base + **n × $10**; all registers are 8-bit
except the two FIFO locations, which are 16-bit [1] pp. 74-76. The 16-byte stride is Apple's Curio
decode, not the discrete part's: the Am79C940 numbers its locations 0-31 on five address lines
(ADD4-0) [1] p. 26, and Curio presents those five lines as the upper bits within a 16-byte slot,
leaving the slot's low bits unused by the core (*inferred - unverified*; the wiring is documented
only for the PDM boards carrying the same ASIC generation). The driver always uses offset 0 within each 16-byte slot; what the other
byte lanes of a slot return is not established (§6.9).

### 1.5 Clocking and the external interface

The Developer Note's clock table lists a dedicated **C20M, 20.0000 MHz crystal** whose usage column
reads "MACE Ethernet" [2] Table 2-2 p. 17 — the network subsystem is the only one on the board with
its own crystal. The discrete Am79C940 specifies a 5-25 MHz system clock SCLK [1] p. 1, runs slave
bus cycles in 2 or 3 SCLK depending on its TC strap [1] p. 47, and encodes the wire as standard
10 MHz Manchester through the MENDEC. Whether C20M feeds the embedded core's SCLK directly or
divided is not documented (§6.8); the 1 ms phase-lock-loop settling delay the data manual requires
applies only after power-up, never after a software reset [1] p. 49.

The external port is the 14-pin AAUI, published as Table 2-5 of the Developer Note [2] p. 22:

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | +5 V | 8 | +5 V |
| 2 | DI+ | 9 | DO+ |
| 3 | DI– | 10 | DO– |
| 4 | Ground | 11 | Ground |
| 5 | CI+ | 12 | NC |
| 6 | CI– | 13 | NC |
| 7 | +5 V | 14 | +5 V |

Electrically this is AUI: differential data out (DO±), differential data in (DI±) and the
collision-presence pair (CI±), with transceiver power from the host. The MACE's port-select
encoding (§2.8) has AUI as its reset default, and the driver never moves it off AUI.

## 2. Register file

### 2.1 The register window

The complete register file at $50F1C000, after the data manual's register table summary, register
bit summary and programmer's register model [1] pp. 74-78, with the per-register detail pages
pp. 57-73. Register *n* is at offset n × $10 from $50F1C000.

| # | Offset | Mnemonic | Access | Reset | Bits 7 → 0 |
|---|---|---|---|---|---|
| 0 | $000 | RCVFIFO[15:0] | RO (word/byte) | — | receive data path; writes ignored, no DTV |
| 1 | $010 | XMTFIFO[15:0] | WO (word/byte) | — | transmit data path; reads ignored, no DTV |
| 2 | $020 | XMTFC | R/W | $01 | DRTRY, –, –, –, DXMTFCS, –, –, APADXMT |
| 3 | $030 | XMTFS | RO | $00 | XMTSV, UFLO, LCOL, MORE, ONE, DEFER, LCAR, RTRY |
| 4 | $040 | XMTRC | RO | $00 | EXDEF, –, –, –, XMTRC[3:0] |
| 5 | $050 | RCVFC | R/W | $01 | –, –, –, –, LLRCV, M/R, –, ASTRPRCV |
| 6 | $060 | RCVFS | RO ×4 | — | 4-byte frame status, read four times (§2.4) |
| 7 | $070 | FIFOFC | RO | $00 | RCVFC[3:0] (frames in RX FIFO) : XMTFC[3:0] (frames in TX FIFO) |
| 8 | $080 | IR | RO, read-to-clear | $00 | JAB, BABL, CERR, RCVCCO, RNTPCO, MPCO, RCVINT, XMTINT |
| 9 | $090 | IMR | R/W, **1 = masked** | $00 (all enabled) | same bit order as IR, per-bit mask |
| 10 | $0A0 | PR | RO, not read-to-clear | $00 | XMTSV, TDTREQ, RDTREQ, –, –, –, –, – |
| 11 | $0B0 | BIUCC | R/W | $20 | –, BSWP, XMTSP[1:0], –, –, –, SWRST |
| 12 | $0C0 | FIFOCC | R/W | $20 | XMTFW[1:0], RCVFW[1:0], XMTFWU, RCVFWU, XMTBRST, RCVBRST |
| 13 | $0D0 | MACCC | R/W | $00 | PROM, DXMT2PD, EMBA, –, DRCVPA, DRCVBC, ENXMT, ENRCV |
| 14 | $0E0 | PLSCC | R/W | $00 | –, –, –, –, XMTSEL, PORTSEL[1:0], ENPLSIO |
| 15 | $0F0 | PHYCC | R/W | $80 (LNKFL set) | LNKFL, DLNKTST, REVPOL, DAPC, LRT, ASEL, RWAKE, AWAKE |
| 16 | $100 | CHIPID[7:0] | RO | $40 | low byte of the chip identification (§2.9) |
| 17 | $110 | CHIPID[15:8] | RO | $09 (Curio) | high byte of the chip identification |
| 18 | $120 | IAC | R/W | $00 | ADDRCHG, –, –, –, –, PHYADDR, LOGADDR, – |
| 19 | $130 | — | — | — | reserved; read/write as zero |
| 20 | $140 | LADRF[63:0] | R/W ×8 | undefined | logical address filter, 8 sequential accesses, LS byte first |
| 21 | $150 | PADR[47:0] | R/W ×6 | undefined | physical address, 6 sequential accesses, LS byte first |
| 22, 23 | $160, $170 | — | — | — | reserved; read/write as zero |
| 24 | $180 | MPC | RO, read-to-clear | $00 | missed packet count |
| 25 | $190 | — | — | — | reserved; read/write as zero |
| 26 | $1A0 | RNTPC | RO, read-to-clear | $00 | runt packet count |
| 27 | $1B0 | RCVCC | RO, read-to-clear | $00 | receive collision count |
| 28 | $1C0 | — | — | — | reserved; read/write as zero |
| 29 | $1D0 | UTR | R/W | $00 | RTRE, RTRD, RPA, FCOLL, RCVFCSE, LOOP[1:0], – |
| 30, 31 | $1E0, $1F0 | RTR1, RTR2 | — | — | reserved test registers; access may damage the device (§2.12) |

Reserved bits read as zero and must be written as zero [1] p. 57. Access types: byte-wide for
registers 2-31, 16-bit for the two FIFOs; byte and word transfers may be mixed on the FIFOs [1] p. 31.

### 2.2 The FIFOs (registers 0 and 1)

The RCVFIFO location is a 16-bit read path out of the 128-byte receive FIFO; the XMTFIFO location
is a 16-bit write path into the 136-byte transmit FIFO [1] pp. 1, 32. Reading RCVFIFO before the
receive threshold is met returns no transfer acknowledge (no DTV) and no data; once RDTREQ has been
asserted, its de-assertion does not prevent further reads — it only reports how many bytes remain
before the FIFO is empty, after which reads again return no DTV [1] p. 57. The same rule holds in
the write direction for XMTFIFO and TDTREQ [1] p. 57. Writes to RCVFIFO and reads of XMTFIFO are
ignored [1] pp. 57-58.

The host is informed that the last byte or word of a received frame is being read by the MACE
asserting its bidirectional **EOF** signal [1] p. 57; on the transmit side the host asserts EOF on
its final write [1] p. 47. On the AV machines the host in both directions is the PSC DMA channel
(§3.8).

### 2.3 Transmit frame control and status (registers 2, 3, 4)

**XMTFC** (register 2, reset $01) is latched **per packet** on the FIFO write that asserts EOF, so
its bits configure the frame about to be transmitted, not the chip [1] pp. 49, 58:

| Bit | Name | Meaning |
|---|---|---|
| 7 | DRTRY | Disable retry — transmit exactly once; no retransmission on collision. Sampled at collision time and required to be stable while transmit data is queued. |
| 3 | DXMTFCS | Disable transmit FCS — suppress the 4-byte FCS the transmitter otherwise appends. |
| 0 | APADXMT | Automatic pad — pad frames shorter than 64 bytes; the pad, and the FCS if enabled, are counted as transmitted bytes. |

**XMTFS** (register 3, reset $00) is the transmit status of the last completed frame; its bit 7,
XMTSV, is the "status valid" flag for the whole register [1] p. 58:

| Bit | Name | Meaning |
|---|---|---|
| 7 | XMTSV | A transmit status is valid in XMTFS/XMTRC. |
| 6 | UFLO | Underflow — the transmit FIFO emptied mid-transmission. |
| 5 | LCOL | Late collision — a collision after slot time. |
| 4 | MORE | More than one retry was needed. |
| 3 | ONE | Exactly one retry occurred. |
| 2 | DEFER | The transmission was deferred. |
| 1 | LCAR | Loss of carrier — the MAU failed to loop DO± onto DI± (AUI only, §3.2). |
| 0 | RTRY | Transmit abandoned after 16 attempts. |

**XMTRC** (register 4, reset $00) carries bit 7 EXDEF (excessive deferral) and the 4-bit retry
count XMTRC[3:0] of the same frame [1] p. 59.

The status pair is buffered for at most **two** frames: "The MACE device will therefore not
commence a third transmit frame, until the status from the first frame is read" [1] p. 51. The read
order is mandatory — XMTRC must be read *before* XMTFS — and reading XMTFS while XMTSV is set
clears both registers, releasing the transmit path [1] pp. 51, 59. After RTRY, LCOL or UFLO the
transmit data request TDTREQ stays de-asserted until XMTFS is read: reading the status is what
advances the transmit FIFO's read pointer [1] pp. 51, 58-59. The XMTSV bit is mirrored at bit 7 of
the poll register, which unlike IR is *not* cleared by reading [1] pp. 64.

### 2.4 Receive frame control and status (registers 5, 6)

**RCVFC** (register 5, reset $01) holds three receive options [1] p. 59:

| Bit | Name | Meaning |
|---|---|---|
| 4 | LLRCV | Low-latency receive — lower the receive threshold from 64 bytes to 12, at the cost of accepting runts and collision fragments (they are no longer deleted). |
| 2 | M/R | Match/reject sense for the external address detection interface (EADI). When clear the EAM/R pin acts as external address *reject*; it must be tied inactive when EADI is unused. |
| 0 | ASTRPRCV | Automatic strip receive — strip pad and FCS from delivered frames and adjust RCVCNT accordingly. |

**RCVFS** (register 6) is one location read **four** times, delivering the status of the last
received frame [1] p. 60:

| Read | Name | Bits 7 → 0 |
|---|---|---|
| 1 | RFS0 | RCVCNT[7:0] — received byte count, low 8 bits |
| 2 | RFS1 | OFLO, CLSN, FRAM, FCS, RCVCNT[11:8] |
| 3 | RFS2 | RNTPC — runts since the last good frame; saturates at 255 |
| 4 | RFS3 | RCVCC — receive collisions since the last good frame; saturates at 255 |

RCVCNT is a 12-bit count of whole bytes received [1] p. 60 (the data manual's own prose is
inconsistent about whether RFS1 carries the upper three or four bits; its bit summary and register
model both show the four bits 3-0, and 12 bits is its own stated width). With ASTRPRCV clear the
count includes pad and FCS bytes; it is invalid whenever OFLO is set [1] p. 60. The status can be
collected two ways: by reading register 6 four times (register-address mode — until all four are
read, further RCVFIFO reads return no DTV), or by continuing to read the RCVFIFO location after the
frame data, in FIFO-direct mode, where each status byte appears **on both halves of the 16-bit data
bus** [1] pp. 47-48, 60. The AV driver's DMA path uses the latter (§3.8).

### 2.5 FIFO frame count (register 7)

FIFOFC's high nibble counts complete receive frames waiting in the receive FIFO, its low nibble
complete transmit frames awaiting status readout [1] pp. 62, 76. Two rules matter: the receive
count **saturates at 15 and further reception stops**, with the missed-packet counter accounting
for the frames dropped [1] p. 62; and the transmit nibble decrements when XMTFS is read, so a
driver that wants an exact count must read FIFOFC *before* it reads XMTFS [1] p. 51.

### 2.6 Interrupt registers and poll (registers 8, 9, 10)

**IR** (register 8, reset $00) is read-only and **read-to-clear**: each bit is set by its event and
cleared by reading the register; writes have no effect and reset clears all bits [1] pp. 61-63.
**IMR** (register 9, reset $00) masks the interrupt *pin* only — writing a **one** into a bit masks
the corresponding source, and masking never prevents the IR bit from latching [1] p. 63. The
open-drain INTR pin is the OR of IR over the unmasked IMR bits [1] p. 27; "no interrupt condition
can take place in the MACE device immediately after a hardware or software reset" [1] p. 27.

| Bit | IR name | Set when |
|---|---|---|
| 7 | JAB | Jabber — 20-150 ms of continuous transmission. Never generated while the AUI port is selected, so unreachable in the AV configuration. |
| 6 | BABL | Babble — transmission still on the wire past the maximum frame (set after 1519 bytes transmitted; the current frame still completes). |
| 5 | CERR | No SQE-Test message on CI± within about 20-40 bit times after a transmission ends. AUI only. |
| 4 | RCVCCO | The receive collision count register wrapped 255 → 0. |
| 3 | RNTPCO | The runt packet count register wrapped 255 → 0. |
| 2 | MPCO | The missed packet count register wrapped 255 → 0. |
| 1 | RCVINT | The host read the last byte/word of a received frame out of the FIFO — receive frame status is now available. Also raised for the frame that suffered a receive overflow. |
| 0 | XMTINT | A transmit frame completed and XMTFS was updated; raised whenever any XMTFS bit is set. |

**PR** (register 10, reset $00) is the polling image: XMTSV, TDTREQ and RDTREQ as live levels, for
drivers that poll instead of taking interrupts; it is not cleared by reading [1] pp. 63-64.

### 2.7 Bus-interface and FIFO configuration (registers 11, 12)

**BIUCC** (register 11, reset $20) [1] pp. 64-65:

| Bit | Name | Meaning |
|---|---|---|
| 6 | BSWP | Byte swap — selects which half of the 16-bit data bus carries the first byte of FIFO transfers and of register writes (§3.6). |
| 5-4 | XMTSP | Transmit start point — transmission begins when 4 (00), 16 (01), 64 (10, the reset default), or 112 (11) bytes are in the transmit FIFO; in all cases transmission also starts when the whole frame is in the FIFO or the FIFO fills. |
| 0 | SWRST | Software reset — equivalent to the hardware RESET pin; **self-clearing** ("The MACE device will clear SWRST during its internal reset sequence"). |

**FIFOCC** (register 12, reset $20) [1] pp. 65-66:

| Bits | Name | Meaning |
|---|---|---|
| 7-6 | XMTFW | Transmit watermark — assert TDTREQ after 8, 16 or 32 write cycles can be accepted (00, 01, 10); 11 is illegal. |
| 5-4 | RCVFW | Receive watermark — assert RDTREQ after 16, 32 or 64 bytes are present (00, 01, 10); 11 is illegal. |
| 3 | XMTFWU | Transmit watermark update — load the XMTFW value now; **self-clearing**, with the transmit FIFO reset as a side effect. |
| 2 | RCVFWU | Receive watermark update — load the RCVFW value now; self-clearing, with the receive FIFO reset as a side effect. The receive FIFO should be empty and the receiver disabled before an update is attempted. |
| 1 | XMTBRST | Transmit burst — change only the TDTREQ *de-assertion* threshold (hold the request until one word remains). |
| 0 | RCVBRST | Receive burst — the receive-side counterpart. |

The watermark-update bits are the documented way to flush a FIFO (§3.1), and the shipped driver
uses them as its flush primitive [3].

### 2.8 MAC, PLS and PHY configuration (registers 13, 14, 15)

**MACCC** (register 13, reset $00) [1] p. 66:

| Bit | Name | Meaning |
|---|---|---|
| 7 | PROM | Promiscuous — accept everything, bypassing all address filtering. |
| 6 | DXMT2PD | Disable two-part deferral processing. |
| 5 | EMBA | Enable modified back-off algorithm (for DIX vs IEEE deferral/backoff nuances). |
| 3 | DRCVPA | Disable receive of physical (own) address matches. |
| 2 | DRCVBC | Disable receive of broadcast frames. |
| 1 | ENXMT | Enable transmitter. |
| 0 | ENRCV | Enable receiver. |

**PLSCC** (register 14, reset $00) [1] p. 67:

| Bit | Name | Meaning |
|---|---|---|
| 3 | XMTSEL | With 0, DO+ and DO- are equal during transmit idle — zero differential, required for transformer-coupled loads. |
| 2-1 | PORTSEL | Port select: 00 AUI, 01 10BASE-T, 10 DAI, 11 GPSI. Reset selects AUI. |
| 0 | ENPLSIO | Enable the PLS I/O (EADI/external status) pins. |

**PHYCC** (register 15) governs the integrated 10BASE-T transceiver: bit 7 LNKFL reports link fail
(and is the only bit *set* by reset), bit 6 DLNKTST disables link test, bit 5 REVPOL reports receive
polarity, bit 4 DAPC disables automatic polarity correction, bit 3 LRT is the low threshold for
link pulses, bit 2 ASEL enables automatic 10BASE-T/AUI port selection, and bits 1-0 RWAKE/AWAKE
are the remote/normal wake functions [1] p. 68. The AV driver never writes PHYCC: the 10BASE-T
transceiver is unused, the port is AUI, and LNKFL simply reads back set [3].

### 2.9 Chip identification (registers 16, 17)

The discrete Am79C940 identifies as $X940, with X the silicon revision (X = 3 for the Rev C0
silicon the data manual describes) [1] p. 69. The Curio-embedded core does not follow AMD's
scheme: it reports **$0940** on the AV machines, a value the later PDM driver names the "B0" Curio,
with $0941 recorded as the earlier "A2" Curio [3], [4]. The AV driver reads the identification into
its globals and formats it into its SNMP interface description string, but never compares it or
branches on it [3]; the PDM driver does branch on it (§4.10).

### 2.10 Address programming: IAC, LADRF, PADR (registers 18, 20, 21)

**IAC** (register 18, reset $00) sequences access to the on-chip address RAM [1] p. 69:

| Bit | Name | Meaning |
|---|---|---|
| 7 | ADDRCHG | Address change — request the address-RAM access window. Setting it stops the receiver (ENRCV is cleared internally) and the missed-packet counter, lets the in-flight frame finish, and is **cleared by the chip** when the RAM is writable. Before ENRCV has ever been set, the window is open and ADDRCHG reads back 0 immediately. |
| 2 | PHYADDR | The next 6 accesses to register 21 load the physical address; the chip auto-clears the bit after the sixth access. |
| 1 | LOGADDR | The next 8 accesses to register 20 load the logical address filter; auto-cleared after the eighth access. If both bits are set, only LOGADDR is honored. |

The internal pointer of registers 20/21 auto-increments on every access, least-significant byte
first [1] pp. 69-71. **LADRF** (register 20) is the 64-bit logical address filter used for
multicast (§3.3); **PADR** (register 21) is the 48-bit station address compared against every
frame's destination field. A write with ENRCV set requires the full ADDRCHG handshake, after which
reception must be re-enabled [1] p. 69.

### 2.11 The error counters (registers 24, 26, 27)

MPC (missed packets), RNTPC (runts) and RCVCC (receive collisions) are free-running 8-bit
counters, each **read-to-clear**: reading returns the count and resets it to zero, and each wraps
255 → 0 raising the matching IR overflow bit [1] pp. 71-72. MPC counts frames that passed address
matching but were dropped anyway — receiver disabled, receive FIFO overflow, or the receive FIFO
frame count saturated — and does not increment until the receiver has been enabled at least once
after reset [1] p. 71. The copies of RNTPC and RCVCC embedded in receive frame status (RFS2/RFS3)
are per-frame snapshots that **saturate** instead of wrapping (§3.5).

### 2.12 User Test and the reserved test registers (registers 29, 30, 31)

**UTR** (register 29, reset $00) [1] p. 72:

| Bit | Name | Meaning |
|---|---|---|
| 7 | RTRE | Reserved test register enable — "access to the Reserved Test Register may cause damage to the MACE device if configured in a system board application". Must never be set. |
| 6 | RTRD | Reserved test register disable — lock the test registers out. Sticky: further writes to RTRD are ignored and only hardware or software reset clears it. |
| 5 | RPA | Runt packet accept — keep runts and collision fragments instead of deleting them. |
| 4 | FCOLL | Force collision on the next transmit (intended for internal loopback testing). |
| 3 | RCVFCSE | Allocate the single FCS generator to the receiver during loopback (§3.7). |
| 2-1 | LOOP | Loopback mode: 00 none, 01 external, 10 internal excluding the MENDEC, 11 internal including the MENDEC. |

Registers 30 and 31 (RTR1/RTR2) are the reserved test registers themselves; the data manual's
instruction is to leave them alone [1] pp. 72-73. The shipped driver opens every session by writing
$40 to UTR — setting RTRD and nothing else — permanently locking the test registers out [3].

### 2.13 Reset state

Hardware RESET must be held for at least **15 SCLK cycles** [1] p. 27; the self-clearing BIUCC.SWRST
is its software equivalent, restoring every default in §2.1 and forcing the JTAG test logic to
IDCODE [1] pp. 46, 65. Reset disables the transmitter and receiver, and "no interrupt condition can
take place ... immediately after" it [1] p. 27. The 1 ms PLL settling delay is a power-up-only
requirement and is not needed after SWRST [1] p. 49. The reset values of the address RAM
(LADRF/PADR) are undefined and must be written before use [1] pp. 69-71.

## 3. Behaviour

### 3.1 Receive datapath

No preamble or start-of-frame delimiter bytes enter the receive FIFO — all counts and thresholds
refer to bytes after the SFD [1] p. 34. A frame is accepted by the address filter (§3.3) and
accumulated in the 128-byte receive FIFO [1] pp. 1, 32. The first RDTREQ for a frame is asserted
only after the **longer** of two conditions: 64 bytes have been received (so that runts and
frames colliding within the slot time can be rejected first), or the programmed RCVFW watermark
plus 12 bytes (the margin that guarantees any byte/word access pattern) [1] p. 34:

| RCVFW | Bytes for first RDTREQ | Latency after first | Bytes for later RDTREQ | Latency after later |
|---|---|---|---|---|
| 00 (16 bytes) | 64 | 64 | 28 | 100 |
| 01 (32 bytes) | 64 | 64 | 44 | 84 |
| 10 (64 bytes) | 76 | 52 | 76 | 52 |

The shipped configuration uses RCVFW = 10, so the DMA channel sees its first request at 76
received bytes [1] p. 34, [3]. With low-latency receive (LLRCV) the threshold drops to 12 bytes, at
the price of no longer deleting runts and collision fragments [1] pp. 34, 59.

Frames that end or collide inside 64 bytes are automatically deleted from the FIFO, with no host
interaction and no status delivered, unless LLRCV or RPA is set [1] pp. 38, 59. As the host drains
data, the MACE asserts **EOF on the last data word**; the four status bytes follow through the same
FIFO-direct path, each duplicated on both halves of the data bus, and reading the last status byte
raises RCVINT [1] pp. 47-48, 55. The status order is data first, status after (§3.8).

Receive overflow is a defined recovery path, not a fault: the overflowing frame gets an immediate
EOF and RCVINT, only the OFLO bit is valid in its status, and the act of reading that status
resets the FIFO pointers — flushing the FIFO — after which reception resumes; until then, the
FIFO holds previously completed packets but the network is deaf regardless of destination address
[1] p. 57. The receive frame count in FIFOFC saturating at 15 has the same effect: reception stops
and MPC increments [1] p. 62.

### 3.2 Transmit datapath

The transmitter begins a frame once XMTSP bytes are in the transmit FIFO (64 in the shipped
configuration), and in all cases when the whole frame is in the FIFO or the FIFO fills [1] pp. 50,
64-65. XMTFC is captured at end-of-frame-write (§2.3), so padding, FCS and retry behaviour are
per-frame decisions. The transmitter handles deferral, backoff and up to 16 attempts on its own —
automatic retransmission needs no FIFO reload [1] p. 1 — and reports the outcome in XMTFS/XMTRC.
UFLO covers the case of the FIFO emptying before the frame is out [1] p. 58.

Status readout is the pacing mechanism of the whole transmit path. The chip buffers status for at
most two frames and will not start a third until the first frame's status is read [1] p. 51;
XMTRC must be read before XMTFS; and after RTRY, LCOL or UFLO, TDTREQ stays de-asserted until
XMTFS is read [1] pp. 51, 58-59. A driver that queues a third frame, or that reads the registers
out of order, stalls the transmitter by construction.

The **Curio deviation** from this model is the one that most affects re-implementation: the
embedded core in Curio behaves as a **one-deep** status buffer, not the two-deep of the data
manual. The evidence is the later PDM driver for the same core, which serializes transmits — one
packet in flight, the next refused until the level-3 interrupt handler has read the pending
status — and whose disassembly branches on the chip ID before deciding whether the IAC handshake
is even needed [4] (*observed* in the PDM ROM's driver; the depth itself is *inferred — unverified*,
from the serialization the driver imposes). The AV driver, by contrast, does not serialize: it
keeps both PSC transmit register sets armed and relies on XMTINT being unmasked so its level-3
handler drains status while the second frame is already queued [3] (*observed* in the AV ROM's
driver).

On the wire, babble protection cuts the transmitter off after 1519 transmitted bytes (BABL), with
the current frame allowed to complete [1] pp. 61-62, and jabber covers 20-150 ms of continuous
transmission — but only on the DAI and 10BASE-T ports; on AUI it is disabled, so JAB can never
fire on the AV machines [1] pp. 45, 62. SQE-Test belongs to the AUI port: a transceiver that fails
to return the test message on CI± within 20-40 bit times of a transmission sets CERR [1] pp. 52,
62, and LCAR on AUI means the MAU failed to loop DO± onto DI± [1] pp. 42, 52.

### 3.3 Address matching and the logical-address filter

Every received frame's destination field goes through three gates, in the order the data manual
describes [1] pp. 55, 69-71, 121:

1. **Physical address match** — equality with PADR, unless MACCC.DRCVPA is set.
2. **Broadcast** — the all-ones address, unless MACCC.DRCVBC is set; broadcast bypasses the hash
   filter entirely.
3. **Logical address filter** — the 64-bit LADRF selects one bit per multicast address via a hash.

Promiscuous mode (MACCC.PROM) short-circuits all three. The hash is the same polynomial as the
frame FCS, run over the six destination-address bytes only: the destination address "is passed
through the FCS generator" and "the high order 6-bits of this resultant FCS are used to select one
of the 64-bit positions in the Logical Address Filter" [1] pp. 70, 121. Precisely — and this is
the point the data manual's own wording obscures — the generator is the reflected (bit-at-a-time,
LSB-first) CRC-32 with the standard Ethernet polynomial, no final inversion, and "high order" means
the high-order bits of the *reflected* remainder:

```
crc = 0xFFFFFFFF
for each of the 6 destination-address bytes, in wire order:
    for bit = 0..7 (LSB first, i.e. wire order):
        top = (crc >> 31) & 1
        crc = (crc << 1) & 0xFFFFFFFF
        if (top ^ bit): crc ^= 0x04C11DB7
hash = bitreverse6(crc & 0x3F)        # == bitreverse32(crc) >> 26
frame accepted for multicast if LADRF bit `hash` is set
```

Taking `crc >> 26` without the 6-bit reversal selects the wrong filter bit for almost every
address; the definition above reproduces the data manual's own worked mapping table of logical
address to filter mask [1] pp. 121-122. The shipped driver computes exactly this hash in 68k code
to maintain its multicast table, keeping a shadow copy of LADRF and rewriting the chip's filter
through the IAC protocol on every group membership change [3] (*observed* in the driver's
multicast add/remove path). Two further rules: an all-zero LADRF with PROM clear rejects every
multicast, and multicast matching cannot be exercised in loopback unless RCVFCSE allocates the
FCS generator to the receiver [1] pp. 57, 70.

### 3.4 Interrupts

The interrupt contract is entirely in §2.6: IR latches, IMR masks the pin (one = masked), INTR is
open-drain and is the OR of the unmasked latches. The pin is not a 68040 interrupt line by
itself — on the AV platform it is wired into the PSC, which owns the processor interrupt levels;
the driver installs itself as the level-3 handler for the MACE's line and receives DMA
completions at level 4 (§4.6) [3]. The conditions worth restating as behaviour:

- **XMTINT** accompanies every completed transmission whose XMTFS is non-zero [1] p. 63.
- **RCVINT** fires on the read of the *last word of frame data* — it means "status available",
  not "frame arrived" [1] pp. 55, 63 — and also fires for an overflowed frame [1] p. 57.
- **MPCO, RNTPCO, RCVCCO** are the 255 → 0 wraps of the three read-to-clear counters [1] pp. 62-63.
- **CERR** is the SQE-Test watchdog, AUI-only [1] pp. 52, 62.
- **BABL** is the transmit watchdog; the Rev C0 erratum #6 makes it lie: the chip "will
  intermittently give BABL error indications when the network traffic has frames equal to or
  greater than 1518 bytes" on the *receiving* station, and AMD's own workaround is to mask BABL
  [1] p. 142. The AV driver leaves BABL unmasked and counts it into its frame-too-long and
  output-error statistics instead [3] (§4.9).
- **JAB** cannot occur on AUI [1] pp. 45, 62.

### 3.5 Counters: wrap versus saturate

| Counter | Width | On read | At 255 |
|---|---|---|---|
| MPC (register 24) | 8 | reset to 0 | wraps, raises IR.MPCO |
| RNTPC (register 26) | 8 | reset to 0 | wraps, raises IR.RNTPCO |
| RCVCC (register 27) | 8 | reset to 0 | wraps, raises IR.RCVCCO |
| RNTPC snapshot in RFS2 | 8 | per-frame snapshot | saturates |
| RCVCC snapshot in RFS3 | 8 | per-frame snapshot | saturates |
| XMTRC[3:0] (register 4) | 4 | cleared when XMTFS is read | not a counter: 0-15 retries, 16 failed attempts sets RTRY |

The rule is: the register-mapped counters are free-running, wrap and interrupt; the copies embedded
in receive frame status saturate [1] pp. 60, 71-72. MPC begins counting only after the receiver has
been enabled at least once, and counts address-matched frames lost to a disabled receiver, FIFO
overflow or FIFO saturation [1] p. 71.

### 3.6 Host bus interface: byte lanes, FIFO-direct, bursts

The MACE's 16-bit data bus is byte-lane programmable, and the data manual's lane rules are the
subtlety the AV design has to survive [1] pp. 31-32:

- **Register reads** (locations 2-31) drive the byte on *both* halves of the bus regardless of
  BSWP, ignoring the byte enables.
- **Register writes** take their byte from the low half (DBUS7-0) when BSWP is clear, from the
  high half when it is set.
- **FIFO transfers with BSWP clear** put the first wire byte on DBUS7-0 and the next on DBUS15-8;
  byte reads duplicate the byte on both halves; BSWP set reverses the pairing.
- On an odd-length frame's final word read, the single valid byte is in the low half regardless of
  BSWP.
- **Byte writes to XMTFIFO consume a whole word slot** (the serializer skips the hole, and EOF is
  placed on a 4-byte boundary), inflating FIFO occupancy — n byte writes occupy 2n bytes, or 2n+2
  when n is odd — which is why the data manual advises lowering XMTFW when byte writes are used.

There are two access methods. In **register-address mode** the five address lines select the
location. In **FIFO-direct mode** the host asserts FDS with R/W and the address lines are ignored:
reads come from the RCVFIFO, writes go to the XMTFIFO — the mode a DMA engine uses, requiring no
address increments at all [1] pp. 47-48. DTV is the transfer acknowledge; an access that returns
no DTV did not happen [1] pp. 57-58. Slave cycles take 2 SCLK (TC high) or 3 SCLK (TC low) [1] p. 47,
and Rev C0 erratum #5 forbids operating the part below 5 MHz SCLK [1] p. 142.

The burst bits change only de-assertion behaviour: with RCVBRST set, RDTREQ is held asserted until
a single word read remains; the assertion thresholds stay at the watermarks [1] pp. 34, 65-66.
A DMA engine that moves fixed-size chunks once granted — the PSC moves 16-byte granules — gains
nothing from them, and the shipped driver leaves both clear [3].

### 3.7 Reset and loopback

Software reset (SWRST) restores every default of §2.1 and is the reset the driver uses
exclusively; it self-clears, and the driver spins on the bit until it reads zero [1] p. 65, [3].
AMD's recommended bring-up order is: BIUCC, FIFOCC, IMR, PLSCC (twice when GPSI is used), PHYCC,
then the address RAM through IAC (LADRF, then PADR), then UTR, and **MACCC last** — the receiver
and transmitter are enabled after everything else is in place; the 1 ms PLL delay belongs only to
power-up [1] pp. 48-49. XMTFC and RCVFC are per-packet and may be written at any time [1] p. 49.

Loopback is selected in UTR [1] p. 72:

| LOOP | Name | Path | LCAR | Network reception |
|---|---|---|---|---|
| 01 | EXTLPB | out the AUI port, back through the transceiver | operates normally — a dead or absent AAUI transceiver fails this test | normal |
| 10 | INTLPB | internal, excluding the MENDEC | never set | prohibited |
| 11 | MENDECLPB | internal, including the MENDEC (Manchester encode/decode exercised) | never set | prohibited |

The chip has a single FCS generator, and loopback is where its allocation is visible: with
RCVFCSE clear the transmitter appends the FCS and the looped-back frame arrives four bytes longer;
with RCVFCSE set the host must supply the FCS in its last four transmit bytes and the receiver
checks it [1] p. 57. FCOLL forces a collision on the next transmission (16 attempts and RTRY with
DRTRY clear), for testing the retry machinery [1] p. 72. The driver uses the three loopback modes
as its open-time self-test (§4.7).

### 3.8 The Apple bus interface inside Curio

The Am79C940 data manual describes a standalone chip with its own pins: 16-bit DBUS, ADD4-0, byte
enables, CS, R/W, FDS, TC, SCLK, the request/ack pair RDTREQ/TDTREQ/DTV, bidirectional EOF, and
open-drain INTR [1] pp. 26-27. Inside Curio, none of that is a pin; it is glue logic between the
core and the PSC. What is established:

- The **DMA requests** are routed to the two PSC Ethernet channels: the receive request into
  ENetRd (PSC channel 1) and the transmit request into ENetWr (PSC channel 2), the channels the
  Developer Note names [2] p. 29. The channel pairing and direction are pinned by the driver's
  channel assignments [3].
- Data moves in **FIFO-direct mode**: the PSC reads the RCVFIFO and writes the XMTFIFO without
  addressing them, which is why the driver itself never touches the FIFO locations [3].
- **EOF is host-driven on transmit** (the PSC asserts it on the channel's final transfer) and
  **MACE-driven on receive** (on the last data word), matching the bidirectional pin [1] pp. 47-48;
  the driver's frame framing depends on exactly this [3].
- The **receive status block is eight bytes wide in memory** even though RCVFS is four bytes,
  because the PSC performs the four status reads as word cycles in FIFO-direct mode, where each
  status byte appears on both halves of the bus — four words, eight bytes, of which the driver
  decodes the low half of each word [1] pp. 47-48, 60; [3] (*observed* in the driver's packet
  decode).
- **The byte lanes are crossed in Curio's glue** (*inferred — unverified*). The driver programs
  BSWP clear and accesses the byte registers at their even Curio offsets on a big-endian 68040;
  taken literally, the data manual's lane rules would put every register write on the wrong half
  of the bus and swap every FIFO byte pair. Working Apple software is only consistent with glue
  that presents the core to the PSC and the CPU the other way round: byte registers at the
  16-byte stride behave normally, and a word read of the RCVFIFO returns the earlier wire byte in
  the high half so frames land in ascending order in memory. Nothing in the shipped software ever
  sets BSWP [3].
- How the status bytes reach **offset zero** of the driver's receive buffer, when the MACE emits
  status *after* data, is the one large hole in the receive story: the arrangement consistent with
  all observed behaviour is that the PSC's Ethernet receive engine skips the first 16-byte granule
  of each buffer, streams frame data from offset $10 until EOF, then performs the four FIFO-direct
  status reads and back-fills the first granule, consuming one buffer per frame (§4.5)
  (*inferred — unverified*).

## 4. Programming model

Everything in this section is the behaviour of the shipped driver — the ROM's `.ENET` "Built-In
Ethernet Driver v1.0" and its hardware-driving half — as established from the ROM image and its
annotated disassembly [3]. It is the programming model a re-implementation must satisfy.

### 4.1 Driver identification and configuration

The driver is reached as the `.ENET` name of the Ethernet slot device; built-in Ethernet is a
pseudo-slot device, not a NuBus card. At open it resolves the machine type through Gestalt, loads
the machine's `'ecfg'` resource, and copies the MACECfg record (§1.4) into its globals: base
address, PROM address, and the four configuration bytes it will later write to XMTFC, RCVFC,
FIFOCC and MACCC [3]. Four machine-ID-keyed records ship, byte-identical, covering the 840AV
(Cyclone), the 660AV (Tempest) and two 33 MHz variants that did not ship [3].

### 4.2 Bring-up sequence

The driver's reset-and-init flow, in order [3]:

1. **Disable** — write $00 to MACCC (ENXMT and ENRCV off).
2. **Flush the receive FIFO** — write FIFOCC with RCVFWU set (the watermark-update bit, whose side
   effect is the reset, §2.7).
3. **Reset the PSC receive channel** — software-reset both PSC register sets, clearing their
   interrupt-enable, -flag and completion bits; then the same for the transmit channel.
4. **Flush the transmit FIFO** — write FIFOCC with XMTFWU set.
5. **Software-reset the chip** — write SWRST to BIUCC and spin until it reads zero (the bit
   self-clears, §2.13).
6. MACCC = $00 again, then IMR = $77.
7. **Configure** — UTR = $40 (RTRD: lock the reserved test registers out), IMR = $77, BIUCC = $20,
   XMTFC = $01, RCVFC = $00, FIFOCC = $2C, PLSCC = $00.
8. Read the chip identification.
9. **Program the address RAM** — PADR from the address PROM, then LADRF cleared to zero (§4.3,
   §4.4).
10. Bring up the PSC channels and install the interrupt handlers (§4.5, §4.6).
11. **Enable** — MACCC = $03 (ENXMT | ENRCV); IMR = $02.

What the configuration values mean, against the register semantics of §2:

| Write | Value | Effect |
|---|---|---|
| UTR | $40 | RTRD set — test registers permanently locked out; no loopback, no runt-accept, no forced collision. |
| BIUCC | $20 | BSWP clear, XMTSP = 10 (start transmit at 64 bytes). Identical to the reset default — a re-write for order, not for value. |
| XMTFC | $01 | APADXMT set — pad to 64 bytes, FCS always appended. The reset default. |
| RCVFC | $00 | ASTRPRCV clear — **pad and FCS are delivered to memory and counted in RCVCNT** (differs from the $01 reset default); LLRCV clear; M/R clear. |
| FIFOCC | $2C | XMTFW = 00 (8 write cycles), RCVFW = 10 (64 bytes), both update bits set — load the watermarks now and reset both FIFOs; bursts off. |
| MACCC | $03 | ENXMT and ENRCV; two-part deferral enabled, standard backoff, physical + broadcast + LADRF matching active, not promiscuous. |
| PLSCC | $00 | PORTSEL = 00 — AUI; XMTSEL clear, the zero-differential idle transformer coupling requires; EADI off. |
| IMR | $77, then $02 | See §4.6. |
| PHYCC | never written | The 10BASE-T transceiver is unused; LNKFL reads back set. |

Deviations from AMD's recommended order (§3.7) are benign: UTR is programmed early (harmless — RTRD
is sticky from then on), PHYCC is never touched, and the address RAM is written while ENRCV has
never been set, so the IAC window is open without an ADDRCHG handshake [1] pp. 48-49, 69; [3].

### 4.3 The address PROM and the station address

The station address does not live in the MACE — there is no provision for a serial ROM in the
Am79C940 at all. It lives in a small Apple PROM at **$50F08000**, a board part shared in concept
with the SONIC Quadras (the address PROM at the same offset on those machines, see
[SONIC](../../hardware/sonic.md)). The layout the driver expects [3]:

- Eight **checksummed** bytes, read at offsets **$01, $11, $21, $31, $41, $51, $61, $71** — byte 1
  of each 16-byte group, the same stride-16 decode as the register file.
- The XOR of all eight bytes must equal **$FF**; anything else fails the driver's open with an
  error and the network stack does not come up [3].
- The **six address bytes** are the first six of those ($01 through $51): PROM byte *n* maps to
  address byte *n*. The driver walks them from $51 down to $01 — the loop direction is bookkeeping,
  not a reversed address — and **bit-reverses** each byte into IEEE bit order. Apple's OUI
  08:00:07 therefore appears in the PROM as 10 00 E0 bit-reversed [3].
- The last two bytes ($61, $71) carry no address; they only close the checksum.

Two overrides exist, both checked at open before the PROM is trusted: a nonzero address field in
the machine's `'ecfg'` record (all four shipped records carry a zero there), and an `'eadr'`
resource keyed on the driver's slot number, which replaces the PROM address entirely [3]. When
neither applies, the PROM address is loaded into PADR.

### 4.4 Loading PADR and LADRF

The driver follows the IAC protocol of §2.10 exactly [3]:

1. Straight after reset, with ENRCV never yet set, the address RAM is writable with no handshake;
   ADDRCHG reads back zero at once. The driver writes PADR by setting PHYADDR and doing six byte
   writes to register 21, least-significant byte first — the PROM address — and clears LADRF by
   setting LOGADDR and doing eight zero byte writes to register 20 [1] p. 69; [3].
2. Later, at run time, multicast changes go the long way: save MACCC, set ADDRCHG, **poll until
   the chip clears it**, set LOGADDR, write the eight shadow-filter bytes, and restore MACCC —
   which restores ENRCV, since setting ADDRCHG cleared it [1] p. 69; [3].
3. The multicast table is maintained as a 16-entry address list with per-hash-bit reference
   counts; each change recomputes the affected 6-bit hash (§3.3), updates the shadow LADRF and
   rewrites the chip [3].

### 4.5 PSC DMA bring-up and buffer layout

The data path is entirely PSC DMA — the driver never moves frame data through the CPU. The
channels, per the Developer Note, are ENetRd and ENetWr, 16 bits wide with 16-byte internal
buffers, each driven through two register sets so that one set can be armed while the other
transfers [2] p. 29. The driver's arrangement [3]:

- **Channel 1 (ENetRd, receive) runs in chain mode.** The channel is programmed with the physical
  base of a chain of **2048-byte buffers**, a limit, and a count that is the number of *buffers*
  remaining in the chain, not bytes. Each buffer holds one frame:

| Offset | Size | Contents |
|---|---|---|
| +$000 | 8 | the four RCVFS status bytes, each duplicated across a 16-bit word (§3.8) |
| +$008 | 8 | fill — the PSC's DMA granularity is 16 bytes, so the second granule is padding |
| +$010 | up to 1518 | frame data: destination, source, type, payload, FCS (RCVFC = $00, nothing stripped) |

  Buffers are 2048-byte aligned. The driver requests them non-cacheable and locked, with defaults
  of 16 receive buffers per channel, 4 channels, and a per-channel maximum of 1023 buffers [3].
- **Channel 2 (ENetWr, transmit) moves one frame per pass.** The address register takes the
  physical packet address masked to a 16-byte boundary, the count register the byte length, and
  the channel is armed sense/interrupt-enable/enable; the PSC writes words into the XMTFIFO and
  asserts EOF on the final transfer. Both register sets are kept armed; the driver's maximum packet
  is 1518 bytes, with 16 transmit packets outstanding and 8 buffers by default [3].
- **Flushing** the receive path resets the MACE receive FIFO through RCVFWU — after first
  disabling the receiver, matching the data manual's precondition that the FIFO be empty before a
  watermark update — then flushes the PSC channel and re-primes it [1] p. 66; [3].
- **Bus errors** on either channel (a bit in the channel control register) disable the MACE's
  receiver or transmitter, software-reset the channel, reinitialize and re-enable [3].

How the receive channel gets status into offset 0 of the buffer when the MACE emits it after the
data is the inference of §3.8; the driver's decode of the result is fixed regardless: the eight
status bytes are read as four words whose low halves are RFS0-RFS3, then the frame length is taken
from the assembled RCVCNT, and packets with any of OFLO/CLSN/FRAM/FCS set are dropped before
protocol dispatch [3].

### 4.6 Interrupt service

The MACE's INTR pin arrives at the PSC, and the driver services two distinct sources [3]:

- **Level 3, the MACE's own interrupt** (PSC level-3, bit 0). The handler reads IR (read-to-clear),
  discards RCVINT, and services everything else — transmit completion, the three counter
  overflows, CERR, BABL — looping until the read returns zero. It then reports "serviced"
  unconditionally, even when the masked read was empty.
- **Level 4, DMA completion** (PSC level-4, bit 3). The PSC's DMA dispatcher calls the driver for
  channel completions; receive completion schedules a deferred task that walks the chain-mode
  buffers and delivers frames to the protocol stack, transmit completion unblocks queued writes.

The interrupt-mask arithmetic that makes this work is the most inverted piece of the whole design
[3]: the driver's final IMR value is **$02**, and in the MACE's mask semantics (§2.6, one =
masked) that masks **only RCVINT**, leaving XMTINT, MPCO, RNTPCO, RCVCCO, CERR, BABL and JAB
enabled on the pin. Receive completion is *not* delivered through the chip's interrupt at all —
the level-4 DMA interrupt is the receive signal, and RCVINT is masked precisely because the
level-3 handler would otherwise see it. The interim $77 mask used during bring-up leaves bits 7
(JAB) and 3 (RNTPCO) unmasked — an incomplete "mask all", harmless because the transmitter and
receiver are off while it is loaded, and because JAB cannot fire on AUI anyway (§3.4).

### 4.7 The open-time loopback self-test

Before the driver declares itself open it runs **three** loopback tests, one in each mode —
INTLPB, MENDECLPB, then EXTLPB — and any failure aborts the open [3]:

1. Write the loopback mode to UTR (RTRD stays latched from bring-up; the loopback bits are the
   low bits of the same register).
2. Attach a protocol handler for EtherType $809B (AppleTalk over Ethernet) and post an
   asynchronous read into a small buffer.
3. Transmit **one 79-byte frame** — 14-byte header plus a 65-byte payload — with destination and
   source both the machine's own PADR, type $809B, and the payload the driver's version string
   (the same "Built-In Ethernet Driver v1.0 ; © Apple Computer, Inc. 1992-1993" string whose
   presence in the ROM identifies the driver, §1.1) [3].
4. Poll for up to three seconds for the read to complete, then detach the handler.
5. Compare the received payload with the sent one, byte for byte.

The hardware obligations this test pins down, for all three modes including EXTLPB [3]:

- the 79-byte frame written by PSC channel 2 is accepted, with the PSC's end-of-transfer taken as
  the MACE's EOF;
- no padding is added (79 bytes already exceeds the 64-byte minimum, and APADXMT is set) and a
  4-byte FCS is appended (RCVFCSE clear), so the frame comes back **83 bytes**;
- the destination — the machine's own PADR — matches physically, and the frame is delivered
  through PSC channel 1 with RCVCNT = 83 and OFLO, CLSN, FRAM, FCS all clear;
- XMTINT is raised with XMTSV set, so the level-3 handler can read XMTRC then XMTFS;
- the level-4 receive DMA interrupt fires, so the deferred task delivers the frame;
- in the two internal modes the frame never reaches the network, and LCAR is never set.

Because EXTLPB sends the frame out the AUI port and back through the transceiver, the third test
requires a transceiver that loops DO± onto DI± — which is what a healthy AUI/AAUI transceiver
does. Whether the AV hardware completes this test with no transceiver attached is not established
(§6.7): if it does not, the driver genuinely fails to open on a bare machine, by design.

### 4.8 Run-time transmit and receive flow

**Transmit.** A write request is copied into the next free transmit buffer, the channel's address
and count registers are set, and the channel is armed; the PSC drains the buffer into the XMTFIFO
word by word, asserting EOF on the last transfer. The MACE starts the frame at its 64-byte
threshold or on EOF, handles deferral and retries internally, and raises XMTINT when done; the
level-3 handler validates XMTSV, reads XMTRC then XMTFS in that mandatory order, accumulates the
statistics and completes the pending write [3] (*observed* in the driver's flow).

**Receive.** The MACE matches, filters, deletes runts, and presents data to the PSC at the
watermark thresholds of §3.1; the channel consumes one 2048-byte buffer per frame, back-fills the
status (§3.8), and interrupts at level 4; the deferred task walks the chain, decodes the status
block, drops frames whose status carries any error bit, strips nothing (RCVFC = $00 — the FCS is
part of the delivered length and is removed by the driver before protocol dispatch), and hands
each frame to the protocol stack [3].

### 4.9 Statistics

The driver maintains the standard Ethernet MIB counters from the chip's surfaces [3]:

- XMTRC and XMTFS feed the retry, collision, deferral and carrier statistics; a status read while
  XMTSV is clear increments an internal "invalid status" counter instead (the registers can
  change mid-read, §2.3).
- On MPCO, the driver does not read MPC — it adds a flat 256 to its input-error count, the wrap
  amount, per interrupt. The MPC, FIFOFC and POLL registers are never read at all in normal
  operation.
- BABL (including the false BABL of erratum #6, which the driver chooses not to mask) is counted
  into the frame-too-long and output-error counters.
- The chip identification is formatted into the SNMP interface description string.

### 4.10 The PDM contrast

The same MACE core, in the same Curio ASIC generation, ships on the first Power Macintosh
generation (PDM), and the differences in how Apple drove it there are a useful control on what is
MACE and what is Apple glue [4]:

| | AV Quadras (Cyclone/Tempest) | PDM Power Macintosh |
|---|---|---|
| Register base | $50F1C000, from the `'ecfg'` record [3] | $50F0A000, from the decoder table [4] |
| Address PROM | $50F08000, same layout and checksum [3] | $50F08000, same layout and checksum [4] |
| DMA engine | PSC channels ENetRd/ENetWr, chain-mode receive [2] p. 29; [3] | AMIC DMA: a 192-page 256-byte receive ring plus two transmit buffers [4] |
| Chip ID use | read, formatted into SNMP, never compared [3] | branched on: $0940 is the shipping "B0" Curio; $0941 takes a different IAC path — the ADDRCHG handshake is skipped for it [4] |
| Transmit pacing | both register sets armed; relies on XMTINT drain [3] | strictly serialized, one packet in flight [4] |
| Loopback self-test | three modes, 79-byte frame [3] | three modes, 80-byte frame (a 66-byte version string) [4] |
| Promiscuous mode | supported through MACCC.PROM [3] | compiled out of the shipped PDM driver [4] |

The receive status also lands differently in memory — the AV chain buffers duplicate each status
byte across a word (§3.8), while the PDM page ring packs the four bytes into one longword — which
confirms the duplication is a PSC-channel behaviour, not a property of the MACE core [4].

## 5. Quirks & errata

- **The AV machines do not use SONIC.** The Ethernet of the 840AV and 660AV is the AMD MACE core
  inside Curio, not the National DP83932 of the other Quadras — a different register file, a
  different DMA model (PSC chains vs. SONIC descriptors), and a different address-PROM checksum
  convention from the SONIC's. See [SONIC](../../hardware/sonic.md) for that part.
- **"MACE" means two things in this ROM.** "MACE 3-to-1" and "MACE 6-to-1" are Sound Manager audio
  compression ratios; the Ethernet controller is the unrelated AMD MACE (§1.1) [3].
- **The mask register is inverted, and the driver's mask is not "all".** IMR one = masked (§2.6);
  the driver's bring-up value $77 leaves JAB (bit 7) and RNTPCO (bit 3) unmasked, and its final
  $02 masks only RCVINT (§4.6). Reasoning from "mask all interrupts during init" gets the level-3
  handler's behaviour exactly backwards.
- **Receive delivery is not the chip's receive interrupt.** RCVINT is masked permanently; frames
  arrive through the level-4 PSC DMA interrupt. A model that raises INTR for RCVINT and expects
  the driver to act on it delivers nothing.
- **XMTRC must be read before XMTFS, always.** Reading XMTFS with XMTSV set clears both, and
  after RTRY/LCOL/UFLO the transmit path is parked until XMTFS is read (§2.3). Out-of-order status
  reads stall the transmitter.
- **The Curio core buffers one transmit status, not two.** The data manual promises two; the
  Apple drivers behave as if the embedded core holds one — the PDM driver serializes on it, and
  even the AV driver depends on the level-3 drain running between frames (§3.2) [4].
- **Status comes after data — but lives at buffer offset 0.** The MACE emits the four receive
  status bytes after the frame; the driver's buffers carry them at offset 0 (§3.8, §4.5). The
  bridging mechanism is inferred, and it is the least certain piece of the receive path.
- **Each status byte occupies two memory bytes.** The four FIFO-direct status reads are word
  cycles with the byte duplicated on both halves, so the "4-byte" status block is 8 bytes wide in
  the receive buffer (§3.8) [1] pp. 47-48; [3].
- **Nothing strips the FCS.** The driver sets RCVFC = $00, not the reset default $01: frames are
  delivered with pad and FCS, RCVCNT includes them, and the driver subtracts them itself
  (§4.2). A minimum-size frame therefore counts 64 bytes, and a loopback frame 83.
- **The chip identification is not AMD's scheme.** $0940/$0941 instead of $X940, and the AV driver
  never compares the value — only the PDM driver branches on it (§2.9).
- **The hash needs a 6-bit reversal.** The LADRF hash is the reflected CRC-32; "high order 6 bits"
  means bits 5-0 of the remainder, bit-reversed (§3.3). `crc >> 26` without reversal matches
  almost no address.
- **Writing RCVFWU flushes the receive FIFO.** The watermark-update bits are self-clearing, and
  the FIFO reset is a side effect — the driver's flush primitive (§2.7). The receiver must be
  disabled and the FIFO empty first.
- **The $77 "all masked" period leaves runt-overflow live.** Harmless only because ENXMT/ENRCV
  are clear while $77 is loaded, and because JAB cannot exist on AUI (§4.6).
- **False BABL is real and unmasked.** Rev C0 erratum #6 raises BABL on the *receiving* station
  when the wire carries full-size frames; AMD's workaround is to mask it, Apple instead counts it
  into the too-long and output-error statistics (§3.4, §4.9) [1] p. 142.
- **The chip must not run below 5 MHz SCLK.** Rev C0 erratum #5; the workaround text also
  recommends one-packet-at-a-time transmit — the same serialization the PDM driver later
  hard-coded (§3.6, §3.2) [1] p. 142.
- **UTR bit 7 can damage the device.** The data manual's own words; the driver permanently locks
  the reserved test registers out with RTRD on open, and RTRD is sticky (§2.12).
- **Register reads duplicate the byte on both halves of the bus.** Under a literal reading of the
  data manual's lane rules, the driver's BSWP-clear register writes on a big-endian host land on
  the wrong lane — the working-system resolution is that Curio crosses the lanes (§3.8,
  *inferred — unverified*).
- **The address PROM is bit-reversed, stride-16, and checksummed.** Byte *n* of the station
  address at $50F08001 + n×$10, bit-reversed; XOR of all eight bytes $FF; the last two bytes are
  checksum filler (§4.3).
- **JAB is unreachable on this port.** Jabber is disabled on AUI, so IR bit 7 can never be
  asserted on the AV machines — anything that reports a jabber condition is misreporting
  something else (§3.4).
- **The missed-packet counter is never read.** The driver never touches MPC, POLL or FIFOFC in
  normal operation; overflow of MPC is counted as a flat +256 in input errors (§4.9).

## 6. Open questions

1. **The Curio-to-PSC glue.** How RDTREQ, TDTREQ, DTV, EOF and FDS are actually routed between
   the MACE core and PSC channels 1 and 2 inside Curio, whether the embedded core exposes the TC
   (2 vs 3 SCLK cycle) and SCLK choices of the discrete part, and what logic — if any — a driver
   could use to reconfigure them. Only the externally observable behaviour is established (§3.8).
2. **The receive-status back-fill.** The mechanism that puts the four status bytes at offset 0
   of each receive buffer — first-granule buffering, a separate write pass, or a Curio-side
   modification of the core — is inferred, not proven (§3.8). Disambiguating it needs evidence
   the current corpus does not contain.
3. **The byte-lane crossing in Curio** (§3.6, §3.8): inferred from BSWP clear plus working
   big-endian software; no schematic of the AV logic board is in the evidence set to confirm it,
   nor to say what a word read of a byte register would return.
4. **Curio-core deviations from Rev C0 silicon.** The non-AMD CHIPID values (§2.9), the one-deep
   transmit status (§3.2), the different IAC behaviour on the $0941 "A2" Curio, and whether the
   discrete part's 128/136-byte FIFO depths carry over — none of these is covered by the data
   manual, which describes the standalone Am79C940.
5. **Address PROM contents beyond the architecture.** What byte 0 of each 16-byte group returns
   (the driver reads only byte 1), what the two checksum filler bytes at $61/$71 were intended to
   hold, and how wide the physical part is.
6. **The `'eadr'` override's provenance** — factory programming, built-to-order, or test fixture;
   nothing in the shipped software says who ever installs one (§4.3).
7. **Does `.ENET` open on a bare machine?** EXTLPB requires a transceiver to loop the AUI port;
   whether an unpopulated AAUI passes the third self-test on real hardware (and thus whether the
   driver fails to open without a transceiver attached) is unverified (§4.7).
8. **The SCLK actually feeding the core.** The Developer Note's C20M 20.0000 MHz crystal is
   labeled for "MACE Ethernet", but whether it is the core's SCLK directly, divided, or only the
   wire-side timing reference is not documented (§1.5).
9. **Register window extent and aliasing.** Whether the core decodes only the 32 slots of
   $50F1C000-$50F1C1FF, whether slots alias above that, what the other byte lanes of a 16-byte
   slot return, and whether the FIFO locations are readable at other widths (§1.4).
10. **RCVFWU during an in-flight slave read.** The driver's own recorded open question to AMD:
    whether resetting the receive FIFO while the PSC is mid-transfer can perturb the DMA engine.
    The data manual only gives the precondition (receiver disabled, FIFO empty) (§4.5).
11. **PSC behaviour with the burst bits set.** The MACE side of RCVBRST/XMTBRST is fully specified
    (§3.6); whether the PSC's Ethernet channels could use it, and what they would do, is unknown —
    Apple leaves both clear, so nothing exercises it.
12. **Whether the embedded core retains the 10BASE-T PHY.** PHYCC is never written and the port
    is always AUI; whether the Curio macrocell even includes the twisted-pair transceiver the
    discrete part integrates is untestable from software and undocumented (§2.8).
13. **840AV vs 660AV board differences.** The two machines are assumed electrically identical in
    their Ethernet subsystem (the same configuration record serves both); no schematic evidence
    either way is in the corpus (§1.2).
14. **MPC's exact accounting.** Which of the drop causes increment MPC for runts under RPA, and
    the interaction between FIFO saturation and the address filter, are only partially pinned by
    the data manual's prose (§2.11, §3.5).

## References

1. Advanced Micro Devices, Inc., *Am79C940 Media Access Controller for Ethernet (MACE)*, data
   manual, publication 16235D (describes Rev C0 silicon; Rev C0 errata appended) — Distinctive
   characteristics / general description p. 1 (128-byte receive and 136-byte transmit FIFOs;
   slave register architecture for an external DMA engine; 5-25 MHz SCLK; AUI/DAI/GPSI/10BASE-T);
   Host System Interface pp. 26-27 (DBUS15-0, ADD4-0; RDTREQ/TDTREQ pins; INTR open-drain and
   "no interrupt condition ... immediately after" reset; RESET ≥ 15 SCLK cycles); FIFO subsystem
   pp. 30-38 (TDTREQ behaviour, byte/word lane rules and BSWP, FIFO depths p. 32, RDTREQ
   assertion/latency table and burst modes p. 34, FRAM validity p. 36, runt deletion p. 38);
   MAC/PLS operation pp. 42-57 (LCAR p. 42, jabber disabled on AUI p. 45, JTAG/IDCODE p. 46,
   slave cycle timing and FIFO-direct mode pp. 47-48, initialization and reinitialization
   pp. 48-49, transmit start point and two-deep status buffer pp. 50-51, SQE-Test p. 52,
   preamble/SFD p. 54, RCVINT p. 55, address matching p. 55, overflow handling and loopback
   FCS allocation p. 57); register detail pp. 57-73 (RCVFIFO/XMTFIFO p. 57, XMTFC/XMTFS pp. 57-58,
   XMTRC/RCVFC p. 59, RCVFS p. 60, FIFOFC/IR/JAB/BABL/CERR pp. 61-62, IMR p. 63, PR p. 64,
   BIUCC pp. 64-65, FIFOCC pp. 65-66, MACCC p. 66, PLSCC p. 67, PHYCC p. 68, CHIPID/IAC/LADRF
   p. 69, LADRF hash definition p. 70, PADR/MPC/RNTPC/RCVCC p. 71, UTR/RTR1/RTR2 pp. 72-73);
   Register Table Summary p. 74, Register Bit Summary p. 75, Programmer's Register Model
   pp. 76-78; Logical Address Filtering and the mapping-of-logical-address table pp. 121-122;
   Rev C0 Silicon Errata pp. 141-143 (erratum #5 low SCLK and one-packet-at-a-time workaround,
   erratum #6 false BABL, p. 142).
2. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV
   Computers*, Developer Press, 1993 — §"Features" p. 6 (built-in Ethernet circuitry in both
   models); §"Curio" p. 16 (Curio contains MACE, a SCSI controller and an SCC); §"System Clocks"
   Table 2-2 p. 17 (C20M 20.0000 MHz crystal, usage "MACE Ethernet"); §"Ethernet Port" p. 22 and
   Table 2-5 (14-pin port pin assignments); §"PSC Functions" p. 29 (nine PSC DMA channels, ENetRd
   and ENetWr 16 bits with 16-byte buffers, two programming register sets per channel; Table 2-10)
   and Table 2-11 p. 30 (DMA channel priority to the I/O and CPU buses).
3. Macintosh Quadra 840AV / Centris 660AV boot ROM, ROM release $10F3, image checksum $5BF10FD1
   (2 MB, 68k-visible base $40800000) — resource-container dump and annotated disassembly of the
   Ethernet driver: the four byte-identical MACE `'ecfg'` records at $4081EE30, $4081EE90,
   $4081EEF0 and $4081EF50 (machine-ID keys 43, 78, 60, 79; `MACEBase $50F1C000`,
   `EnetPROM $50F08000`, `XmitFrmCtl $01`, `RecvFrmCtl $00`, `FIFOCfgCtl $2C`, `MACCfgCtl $03`);
   the `.ENET` driver "Built-In Ethernet Driver v1.0 ; © Apple Computer, Inc. 1992-1993" with its
   version string at $4080D9E; the driver's register stride, bring-up order and configuration
   writes (UTR $40, IMR $77/$02, BIUCC $20, XMTFC $01, RCVFC $00, FIFOCC $2C, PLSCC $00,
   MACCC $03); the address-PROM read (offsets $01-$71, XOR checksum $FF, per-byte bit reversal,
   `'ecfg'` and `'eadr'` overrides); the IAC/PADR/LADRF programming and multicast hash; the PSC
   channel assignments, chain-mode 2048-byte receive buffers and per-buffer status layout;
   the level-3/level-4 interrupt handlers and the RCVINT mask; the three-mode loopback self-test
   with its 79-byte, type-$809B frame; the statistics accounting (flat +256 on MPCO, BABL counted
   as frame-too-long, chip ID formatted into ifDescr); the never-read MPC, POLL and FIFOFC
   registers; and the Sound Manager "MACE 3-to-1"/"MACE 6-to-1" strings.
4. Power Macintosh 6100/7100/8100 boot ROM, version $077D, header checksum $9FEB69B3 (March 1994;
   4 MB, 68k-visible base $40800000) — annotated disassembly of the PDM variant of the same
   Curio-embedded MACE core: register base $50F0A000 and address PROM $50F08000 from the machine's
   decoder table; AMIC Ethernet DMA (192-page 256-byte receive ring, two transmit buffers) instead
   of the PSC channels; the chip-ID branch on $0940 ("B0") vs $0941 ("A2"), with the ADDRCHG
   handshake skipped for the latter; the strictly serialized one-packet-in-flight transmit path;
   the 80-byte loopback self-test frame; promiscuous mode compiled out; and the packed four-byte
   receive status longword of the AMIC ring, contrasted with the AV chain buffers'
   word-duplicated status.
