# BART — the PDM NuBus bridge

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, division of labor, slot
   topology, clocking
2. [Register file](#2-register-file) — the $F0000000 window: reset, wait-state bit, ID register,
   slot-$E mask, per-slot burst enables
3. [Behaviour](#3-behaviour) — address decode, transactions, arbitration and card DMA, fault
   semantics, interrupts, the BART 21 generation
4. [Programming model](#4-programming-model) — presence probe, boot reset sequence, Slot Manager
   declaration-ROM search, block-transfer enable, cacheability control
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The **BART NuBus controller** is "a chip that provides a data gateway between NuBus and the CPU bus"
in PowerPC processor-based Macintosh systems [1] p. 79. It is Apple's controller for the first
generation of Power Macintosh machines, internal codename PDM [1], and it is the only bridge between
the two buses of that platform: the 64-bit, non-pipelined PowerPC 601 processor bus on one side, and
the 32-bit, 10 MHz NuBus (ANSI/IEEE Std 1196-1990, "NuBus '90") on the other [1] pp. 17, 50.

BART is both a CPU-bus slave and a CPU-bus master. As a slave it accepts processor transactions —
one-cycle (single-beat) or four-cycle (32-byte cache-line burst) transfers [1] p. 17 — and forwards
them into NuBus slot space; as a master it contends for the processor bus on behalf of NuBus cards
that are themselves mastering the bus, turning their NuBus cycles into processor-bus cycles against
RAM or ROM. The Dev Note is explicit about the intended reach of that path: "The NuBus interface in
Power Macintosh computers provides access between RAM or ROM and plug-in expansion cards. It is not
designed to let plug-in cards gain access to peripheral devices directly" [1] p. 50.

Two silicon generations exist. **BART 4** ships in the 6100/60, 7100/66, 8100/80 and their variants
(and on the 6100 adapter card); **BART 21** replaces it in the 8100/100 and 8100/110, where it
"provides faster data transfers" — Apple notes that existing NuBus cards should work but that
"compatibility cannot be guaranteed" [2] p. 3. BART 21 "enables burst transactions for specific
slots instead of for the bus as a whole" and "also handles burst read transactions by NuBus masters"
[2] p. 4 (§3.7).

### 1.2 Machines that carry it

| Machine | Apple codename | BART | Where the chip lives | NuBus connectors |
|---|---|---|---|---|
| Power Macintosh 6100/60 | PDM ("Piltdown Man") | BART 4 | optional PDS NuBus adapter card | one (short cards only) |
| Power Macintosh 6100/60AV | PDM | — | none | none (PDS occupied by the AV card) [1] p. 50 |
| Power Macintosh 6100/66 | PDM | BART 4 | optional PDS adapter card [2] p. 3 | one |
| Power Macintosh 7100/66, /66AV | Carl Sagan | BART 4 | main logic board | three [1] p. 50, Table 4-1 |
| Power Macintosh 7100/80 | Carl Sagan | BART 4 [2] p. 4 | main logic board | three |
| Power Macintosh 8100/80, /80AV | Cold Fusion | BART 4 | main logic board | three [1] p. 50, Table 4-1 |
| Power Macintosh 8100/100 | Cold Fusion | BART 21 [2] p. 3 | main logic board | three |
| Power Macintosh 8100/110 | Cold Fusion | BART 21 [2] p. 8 | main logic board | three |

The 6100's NuBus capability is peculiar to the family: "Users of the Power Macintosh 6100/60 acquire
NuBus expansion capability by installing an adapter card, sold separately, in the computer's PDS
slot. This adapter card accepts a short NuBus expansion card, which lies parallel to the main
circuit board... The adapter card carries the BART NuBus controller chip, so this chip is present in
the Power Macintosh 6100/60 system only when the adapter card is installed" [1] p. 52. The 6100
accepts only short (4 by 7 inch) cards; the 7100 and 8100 accept both long (4 by 13 inch) and short
cards, of the same physical configuration as Quadra NuBus cards [1] p. 51. The connectors are 96-pin
Euro-DIN with the pin assignments of Table 4-2 [1] pp. 51–52, and each card may draw up to 2.0 A at
+5 V, 0.175 A at +12 V and 0.15 A at −12 V [1] p. 52, Table 4-3.

### 1.3 Division of labor

BART is nearly invisible to software, and most of what a NuBus card touches on a PDM machine is
*not* BART's. The split, established by the Dev Note's block diagram and register diagram [1]
pp. 13, 22–23 and by the 8100 schematics [4], is:

| Function | Owner |
|---|---|
| CPU-to-slot-space address windows (standard and super slot space) | BART |
| NuBus-master (card DMA) transactions into RAM/ROM — CPU-bus mastership | BART, via a request/grant pair into the HMC arbiter [4] sheets 4, 23 |
| CPU-bus arbitration (601 vs AMIC DMA vs PDS vs BART) | the HMC, not BART — see [HMC](hmc.md) §3.5 and [1] Table 2-4 p. 20 |
| Per-slot interrupt lines (/NMRQ) | the AMIC — each slot's line runs from the connector straight to an AMIC pin [4] sheets 12, 22; see [AMIC](amic.md) §2.3 |
| Slot-interrupt flag/enable registers (pseudo-VIA2 slot bank, $50F26000) | the AMIC (§3.6) |
| Declaration ROMs, sResources, per-slot PRAM records, Slot Manager | the cards and the ROM (§4.3) |

The consequence for the register file (§2) is that BART's own software surface is small: a handful of
byte-wide control registers at $F0000000, an identification longword, and the address windows it
decodes. Everything else a slot driver touches belongs to the AMIC or to generic NuBus.

### 1.4 Slot topology and numbering

NuBus slots are identified by their /ID3–/ID0 strap codes, which double as address decode: a card in
slot $s answers $Fs000000–$FsFFFFFF (standard slot space, 16 MB) and $s0000000–$sFFFFFFF (super slot
space, 256 MB) [3] pp. 54–55, 132–133. Macintosh machines use slot IDs $9 through $E [3] p. 132;
slot $0 is the computer itself, addressing $F0000000–$F0FFFFFF, and "the microprocessor cannot access
slot $0" [3] p. 136 — a rule the PDM platform breaks on purpose: BART's control registers sit at
$F0000000, inside slot $0's space, and the processor very much does access them (§2, §5).

The Slot Manager's per-model picture of the slots comes from the slot-capability tables the ROM
carries for each machine [5]:

| Slot | 6100 (PDM) | 7100/8100 (Carl Sagan/Cold Fusion) |
|---|---|---|
| $0 | pseudo-slot — built-in video, Ethernet and similar devices; can interrupt (dispatches as the vertical-blanking interrupt) | same |
| $6 | disabled; carries a PRAM record | same |
| $9 | disabled; carries PRAM and Ethernet interrupt duty (the comment in the ROM's table also notes the second megabyte of ROM appearing in the 24-bit alias of this slot) | same |
| $B–$D | — | the three physical NuBus connectors, with PRAM, interrupts and connector present |
| $E | the single physical NuBus slot — present only with the adapter card | the PDS video slot (HPV VRAM expansion card or AV card), not a NuBus connector; its software address in pseudo-slot space is $E [1] pp. 40, 42 |

On the 7100 and 8100 the three connectors are labeled on the 8100 schematic as **"NuBus Slot B"
(J11), "NuBus Slot C" (J12) "NuBus Slot D" (J13)**, each with its NuBusID<3..0> straps, and the
physical board order is B, D, C — the middle connector is slot $D [4] sheet 22 (*observed* on the
drawing). This contradicts the often-repeated "$C/$D/$E" numbering for these machines: on the 7100
and 8100, slot $E is the PDS video pseudo-slot, not a NuBus connector. But see §6.9: the slot
interrupt enables the shipped OS actually raises cover a set that reads $C/$D/$E under the usual
bit-to-slot mapping, and the tension between the connector labels and that observation is unresolved
here.

The 7100/66AV and 8100/80AV add one non-NuBus path through the slot area: the DAV connector on the
AV card, which "lets NuBus cards... gain access to the audio and video signal streams of the Apple
AV technologies" via a ribbon cable — the signals are tapped on the AV card and main board, not
through BART [1] pp. 50, 53.

### 1.5 Clocking

BART is a two-clock-domain part. The processor-bus side is clocked by `BartClk`, an output of the
ICS9178 clock synthesizer that runs in the CPU bus clock domain (30/33/40 MHz class, per model
[1] Table 2-2 p. 19), and a separate 40 MHz oscillator output also feeds the chip [4] sheets 11, 23
(*observed* in the wiring). The NuBus side runs at the NuBus '90 rate: /CLK, "an asymmetric duty
cycle of 75% high and a constant nominal frequency of 10 MHz," with signals driven on the rising
(driving) edge and sampled on the falling (sampling) edge [3] p. 52. The PDM connectors also carry
the NuBus '90 double-speed pair /CLK2X (nominal 20 MHz, 50% duty cycle, for 2X block transfers) and
the /CLK2XEN sense line [3] p. 52, [1] Table 4-2 pins B24/B26; the 8100 schematic shows both nets
bused between the three connectors and BART [4] sheet 22. Software never sees any of these clocks
except as transfer latency.

## 2. Register file

### 2.1 The register window

All of BART's software-visible registers live at physical **$F0000000**, at the bottom of slot $0's
standard slot space (§1.4). The window's extent is established only where the ROM touches it; the
named registers are:

| Offset | Width | Function | Shipped-ROM usage |
|---|---|---|---|
| +$00 | byte | NuBus reset pulse (§2.2) | written every boot; byte-read as the presence probe (§4.1) |
| +$01 | byte | wait-state select (§2.3) | never written by any shipped code |
| +$08 | long | identification register (§2.4) | never read by the shipped ROM |
| +$11 | byte | slot-$E disable mask (§2.5) | written every boot on the 7100/8100; skipped on the 6100 |
| +$18–+$80 | byte ×14 | per-slot burst-enable latches (§2.6) | written only when a card declares slave block transfers (§4.4) |

Whether the chip decodes further offsets inside $F0000000–$F00FFFFF, and whether the named registers
alias within it, is not established (§6.5). The registers are byte-wide; the two documented control
bits outside the burst file are described in the ROM's register definitions as "bit 8" of a 16-bit
big-endian register pair based at $F0000000 — that is, bit 0 of the byte at +$01, and bit 7 of the
byte at +$11 [5].

### 2.2 Reset register ($F0000000)

Writing $80 to this byte pulses the NuBus /RESET line to all cards. The pulse is issued in software
on every machine start — including soft restarts, precisely because "on a soft restart, the NuBus
reset line doesn't get asserted" by hardware, so the ROM must pulse it itself [5]. The exact pulse
width is not documented (§6.2). The register is also what the presence probe reads: a byte read of
$F0000000 that *completes* is the entire signal that BART exists (§4.1); the value returned is
immaterial, and no shipped code ever checks it.

### 2.3 Wait-state bit ($F0000001)

Bit 0 of this byte adds wait states, per the ROM's own definition of the register [5]. No code in
the shipped ROM image ever writes it, and no Apple document describes which transfers it slows —
all slots, slot-space reads only, or register accesses (§6.4). It is a latch with no observed
software contract.

### 2.4 Identification register ($F0000008)

A longword read register. First-revision silicon returns $43184000 [5] — a value known only because
a prototype-era ROM revision read it to gate an interrupt-line swap on early 7100/8100 prototype
boards. That read does not exist in the shipped ROM: a byte-pattern scan of the entire 4 MB image
finds neither the address $F0000008 nor the constant $43184000 anywhere [5]. No production software
reads the register, and the layout beyond the first-revision value is unknown (§6.3). A compliant
machine may return any value other than $43184000 at +$08 with no observable consequence; returning
$43184000 would resurrect a prototype code path that must never fire.

### 2.5 Slot-$E disable mask ($F0000011)

Writing $80 to this byte disables BART's slot-$E path — both the address decode and whatever
interrupt contribution BART itself makes for that slot (§6.3). The register exists for the 7100 and
8100, where slot $E belongs to the PDS video card (HPV or AV) and the PDS claims that window on the
CPU bus directly; exposing a BART-side slot-$E window there would shadow the video card. The ROM's
register definition annotates the bit "for Cold Fusion" [5]. The write is performed once per boot,
guarded by the machine identity: it is skipped when the software CPU ID reads $3011, the
"single-slot PDM-style machine with BART" case, so the 6100 keeps its only real slot fully enabled
(§4.2). The full semantics of what the mask silences are not established (§6.3).

### 2.6 Per-slot burst enables ($F0000010 area)

One byte per slot, descending: the byte for slot 1 is at +$80 and each subsequent slot's byte is
eight bytes lower, so slot 9 is at +$40 and slot 14 (slot $E) at +$18. Bit 0 of each byte is that
slot's block-transfer (burst) enable. The shipped ROM's block-transfer selector computes exactly
this arithmetic — `base + $80 − 8 × (slot − 1)` for slots 1 through 14, with a bounds check — and
toggles the bit with `BSET`/`BCLR #8` on the byte, which on the 68k reduces to bit 0 [5]
($FFC0F38C). The registers predate the BART 21 generation (§3.7).

One discrepancy is on record: the ROM's own definition of the register file ends its offset comment
with "$80, $78, $70, $68 ... $10", which would place slot $E's byte at +$10 rather than +$18 [5].
The shipped code implements the $18 arithmetic, and the code is authoritative for machines running
it; the comment presumably reflects an earlier layout (*inferred*).

### 2.7 Reset state

The power-on values of every latch in the file are unknown. No shipped code reads back the
wait-state bit, the slot-$E mask or the burst bytes, so nothing in the observed software contract
constrains them (§6.5). The register file is not reinitialized by a soft restart: the boot reset
sequence writes only +$00 and +$11 and leaves the rest standing [5].

## 3. Behaviour

### 3.1 CPU-side address decode

The coarse physical map of the platform [1] Table 2-5 p. 22 puts I/O at $50000000–$5FFFFFFF and
labels $60000000–$FEFFFFFF "not assigned" — that unassigned range is where BART's windows live; the
table simply does not break them out:

| Physical range | Meaning |
|---|---|
| $00000000–$0FFFFFFF | RAM (plus $10000000–$3FFFFFFF RAM aliases) |
| $40000000–$4FFFFFFF | ROM ($FF000000–$FFFFFFFF: ROM alias) |
| $50000000–$5FFFFFFF | I/O (AMIC; see [AMIC](amic.md) §2.1) |
| $60000000–$7FFFFFFF | not assigned |
| $80000000–$8FFFFFFF | super slot $8 — addressable only with the new ROM [2] p. 5 |
| $90000000–$EFFFFFFF | **super (major) slot space**, slots $9–$E, 256 MB each [3] p. 133 |
| $F0000000–$F00000xx | BART registers (§2) — inside slot $0's standard slot space |
| $F1000000–$F8FFFFFF | extended standard slot space, slots $1–$8 — new ROM only [2] p. 5 |
| $F9000000–$FEFFFFFF | **standard (minor) slot space**, slots $9–$E, 16 MB each [3] p. 132 |

Terminology differs between Apple's documents: what the 1992 card-design book calls standard and
super slot space [3] pp. 132–133, the Enhanced Dev Note calls minor and major slot space [2] pp. 4–5.
They are the same windows. The original 1994 ROM addresses minor slots $9 through $E only; the new
ROM (7100/80, 8100/100, 8100/110) extends the range to slots $1 through $E and makes major slot $8
addressable [2] p. 5, Table 1-3.

On the 7100 and 8100, slot $E's standard window ($FE000000–$FEFFFFFF) is claimed on the CPU bus by
the PDS video card, and the ROM disables BART's competing path every boot (§2.5, §4.2). On the 6100,
the adapter card's single connector *is* slot $E and BART serves it. Super slot space for slot $E
is likewise the PDS card's on the 7100/8100. Whether BART 4 decodes the super-slot window of slot
$A — which no PDM machine has a connector for, yet the Enhanced Dev Note's Table 1-3 lists "major
A–E" support for the BART 4 models [2] Table 1-3 p. 5 — is not established (§6.7).

Inside the 68k emulator's 24-bit mode, 24-bit addresses $sxxxxx translate to $Fs0xxxxx for slots
$9–$E [3] pp. 133, 136; the ROM's slot table notes that the 24-bit alias of slot $9 is repurposed on
PDM (the second megabyte of ROM appears there), so 24-bit slot mapping is an MMU concern rather
than a BART one [5].

### 3.2 NuBus-side view: what cards see

Electrically the slots are standard NuBus '90: /ID3–/ID0 identify each card's position; distributed
arbitration decides mastership with the card's ID code on /ARB3–/ARB0 after a two-clock contest in
which the highest ID present wins [3] pp. 54, 98–99; the slot's ID pins simultaneously decode its
standard slot space (/ID3–/ID0 mirrored on /AD27–/AD24) and its super slot space (/ID3–/ID0 mirrored
on /AD31–/AD28) [3] pp. 54–55. A requesting card asserts /RQST only while it sees it unasserted,
which serializes same-cycle requesters from highest to lowest ID and gives the bus its fairness
[3] p. 98.

A NuBus master that won arbitration and drives a RAM or ROM address gets onto the CPU bus through
BART, which masters the processor side on the card's behalf [1] pp. 17, 50; [4] sheets 4, 23. The
address is forwarded without translation — there is no IOMMU on the platform — so a DMA card sees
the same discontiguous physical RAM layout as the processor: eight 64 MB banks whose usable middles
leave at least 32 MB gaps between adjacent SIMM ranges, which is why the Enhanced Dev Note
dedicates an appendix to the RAM map "If you are designing a NuBus expansion card that uses direct
memory access to or from RAM" [2] pp. 9–10. Cards cannot reach peripheral devices through the bus:
the interface "is not designed to let plug-in cards gain access to peripheral devices directly"
[1] p. 50; whether BART enforces that by decode filtering or the I/O region simply does not answer
NuBus masters is not established (§6.8).

### 3.3 Data width, byte lanes and byte order

The NuBus is byte-lane consistent with the processor: Apple "chose to preserve byte-address
consistency; each of the 4 bytes of the processor is connected to its corresponding NuBus byte
lane" — byte $n$ of the processor to NuBus byte lane $n$ — and "only the bytes are swapped, not
bits within bytes," because NuBus encodes the least significant byte of a word in byte 0 while the
(big-endian) processor puts it in byte 3 [3] pp. 136–137. BART inherits this rule exactly as the
68k-era bridges did; the practical consequence is that a 32-bit value $12345678 stored by a
little-endian card is read by the Macintosh as $78563412 [3] p. 137. Byte, word and longword
accesses are all legal in slot space, with transfer widths selected by /TM1–/TM0 and /AD1–/AD0
[3] p. 60.

### 3.4 Transactions: beats, bursts and blocks

On the CPU-bus side BART "acts as a CPU bus master, transferring one-cycle or four-cycle
transactions" and is compliant with IEEE Std 1196 [1] p. 17; on the NuBus side "it supports NuBus
block transfers and data bursts to and from the main processor bus" [1] p. 8. A block transfer is
"data transfers of more than one longword at a time" [1] p. 79; as of the 1992 card book, block
transfers were implemented only in the Quadra family [3] p. 60, so the PDM machines are the first
NuBus Macintosh generation in which the general slot population can get them again — behind the
per-slot enables of §2.6.

NuBus block transfers come in 1X form (words at the 10 MHz /CLK rate) and 2X form (words at the
20 MHz /CLK2X rate, with data driven and sampled on /CLK2X assertion edges) [3] p. 52. A slave that
does not support block transfers terminates the first transfer of one with /ACK; its declaration
ROM must say so (the block-transfer sResource entries with slave size bits zero) [3] p. 181.

**Locking is not supported.** "PowerPC processor–based Macintosh systems do not support locking
cycles on NuBus. Resource locking will fail without a bus error being generated" [1] p. 52. This is
a deliberate silent-failure contract: a card (or processor code) attempting a locked transaction
must neither fault nor expect the lock to have taken. For contrast, NuBus locking itself is defined
on the bus — bus locking by sustained contention, resource locking by an attention-resource-lock
cycle [3] pp. 102–104 — but the bridge will not carry it.

### 3.5 Fault and timeout behavior

Unclaimed accesses in BART's windows must terminate with a **recoverable** error. This is
load-bearing three times in the shipped software:

1. The presence probe reads $F0000000 under a bus-error handler; a 6100 without the adapter card
   must take a clean, recoverable fault there so the ROM can record the bridge as absent (§4.1).
2. The start-up code probes slot $E's declaration space for the "VidReset" signature on every boot,
   before the Slot Manager runs, and must recover when the PDS slot is empty (§4.2).
3. The Slot Manager's declaration-ROM search reads the top bytes of every slot's standard space and
   relies on an empty slot erroring recoverably, so it can record the slot as empty and move on
   (§4.3). A read returning garbage instead of faulting makes every empty slot look like a broken
   card.

This contrasts with the platform's other failure mode: an address that the MMU maps but that no
hardware decodes causes the AMIC to assert an error signal after 40 µs, a condition "not generally
recoverable; forces restart" [1] p. 21. BART-claimed space must fault through the recoverable path,
not that one. The fault surfaces on the processor as a machine-check-class exception (vector
$00200 on the 601) which the nanokernel reflects into the 68k environment as a bus error, where the
caller's handler recovers (*observed* in a full boot of the March 1994 ROM under System 7.5: the
"VidReset" probe faults at $FEFFFF00, then one fault per empty slot at $FEFFFFFF, $FDFFFFFF,
$FCFFFFFF and $FBFFFFFF, descending, each taken exactly once, with the boot proceeding to the
Finder).

BART's own NuBus timeout value is not documented anywhere in the evidence set. Its architectural
sibling, the MUNI bridge of the Quadra 840AV/Centris 660AV, "generates a bus error if any NuBus
transaction takes longer than 25.6 µs" [6] p. 39; whether BART uses the same figure, and whether it
distinguishes an empty slot (no card to /ACK) from a slow card, is open (§6.1).

### 3.6 Interrupts

**BART is not in the slot-interrupt path.** Each NuBus slot's /NMRQ is an open-drain line with a
4.7 kΩ pull-up running from the connector directly to an AMIC pin, exactly like the PDS interrupt
line [4] sheets 12, 22. On the 8100 the three connectors' lines are `NuBusIRQ<0>*` (slot $B, J11),
`NuBusIRQ<1>*` (slot $C, J12) and `NuBusIRQ<2>*` (slot $D, J13), entering AMIC alongside `PdsIrq*`
from the PDS connector [4] sheets 12, 22 — i.e. pseudo-VIA2 slot-bank bits 2, 3, 4 and 5 are slots
$B, $C, $D and the PDS slot $E respectively, and BART's only interrupt involvement is the slot-$E
mask bit of §2.5 (what that mask actually silences is §6.3). On the 6100, the adapter card is
electrically a PDS card, so its slot's /NMRQ is expected to arrive on the PDS line, bit 5
(*inferred — unverified*; the 6100 board is not in the schematic evidence set).

The flags and enables live in the AMIC's pseudo-VIA2 **slot bank**, an RBV-layout register bank at
base $50F26000 [1] Figure 2-2 pp. 22–23 (the figure shows the SLT0–SLT2 + VBL bits, with SLT3
italicized as absent on the 6100); full bit-level detail is in [AMIC](amic.md) §2.3:

| Address | Register | Meaning on PDM |
|---|---|---|
| $50F26002 | slot flags | **active low** (0 = interrupting): bit 6 = VBL (built-in video), bit 5 = slot $E (PDS), bit 4 = slot $D, bit 3 = slot $C, bit 2 = slot $B; bits 1, 0 read as 1 |
| $50F26003 | interrupt flags | bit 7 = VIA2 IRQ, bit 1 = "any slot" aggregate; writing $82 clears the aggregate |
| $50F26012 | slot enables | bit 7 = set/clear select (classic VIA IER semantics); bits 6–2 mirror the flag bits |
| $50F26013 | device IER | SCSI, floppy, any-slot aggregate for the device bank |

The dispatch is the ROM's level-2 path, and the shipped code pins the semantics [5] ($FFC1F0CA):

```
MOVE.B #$82,$3(A1)    ; clear the any-slot aggregate in the flag register
MOVEQ  #$80,D0        ; build ~$7F
OR.B   $2(A1),D0      ; OR in the slot flags — active LOW
NOT.L  D0             ; → active-high pending mask
AND.B  $12(A1),D0     ; keep only enabled slots
BNE.S  SlotIntCommon
RTS
```

Three contracts fall out. The flag bits are level-sensitive images of the /NMRQ lines, not
write-1-to-clear latches — the card must drop its line when serviced. The any-slot aggregate
latches when any enabled line asserts and is cleared by the $82 write; the handler then **re-enters
itself until all slot lines deassert**, because the aggregate is edge-triggered on some machines
[5]. And the pending bits are dispatched through the ROM's priority table — bit 2 to slot $B, bit 3
to $C, bit 4 to $D, bit 5 to $E, bit 6 to slot $0 (the built-in video vertical blank) and bit 0 to
slot $9 (Ethernet) — highest slot first, each running its per-slot service queue [5].

No prototype interrupt-swap code survives in the shipped ROM: the constants $F0000008 and $43184000
appear nowhere in the 4 MB image, so the ID register is never read and the straight bit-to-slot
mapping above is unconditional [5].

### 3.7 The BART 21 generation

BART 21 (8100/100, 8100/110) differs from BART 4 in two documented ways: it "enables burst
transactions for specific slots instead of for the bus as a whole," and it "handles burst read
transactions by NuBus masters" [2] p. 4. Since the per-slot burst-enable registers (§2.6) predate
BART 21 — the selector that writes them is present in the original 1994 ROM [5] — the practical
reading is that on BART 4 the per-slot bits exist but gate block attempts collectively or partially
(perhaps only in the write direction), while on BART 21 they are honored per slot and cover master
reads too (*inferred*). To software the difference is invisible except as performance: single-beat
and burst transfers are indistinguishable to a driver, and the Enhanced Dev Note's summary table
marks "slot-specific burst transactions" yes only for the 8100/100 (and, by its Chapter 2, the
8100/110) [2] Table 1-3 p. 5, p. 8. Whether BART 21 carries additional registers behind the
per-slot cacheability calls (§4.6) is unknown (§6.8).

## 4. Programming model

The 1994 boot ROM (version $077D, header checksum $9FEB69B3) drives BART as follows; addresses are
in the ROM's $FFC00000-based alias of the 4 MB image [5].

### 4.1 Presence probe

Early in start-up the ROM probes for the bridge with a byte test of $F0000000 under a bus-error
handler. A completed read marks BART present and sets its flag in the low-memory decoder-info
field; a bus error marks it absent, and the machine boots on with zero NuBus slots [5]. The flag
is observable as a bit of the 64-bit decoder field at 68k logical $2400 (AddrMapFlags) — the ROM's
own block-transfer selector tests it with `BTST #2,$2402`, i.e. bit 42 of the field [5] (*observed*:
a booted 7100/8100 reads $000004D4 there with the bit set; a bridgeless 6100 faults the probe
twice and ends with the flag clear). The Slot Manager, the block-transfer selector and the boot
reset sequence all gate on this flag, so a machine that answers $F0000000 without implementing the
rest of §2 breaks in three places at once.

### 4.2 Boot reset sequence

On every machine start — cold or soft — the ROM's NuBus reset routine runs, and when the bridge is
present it performs exactly two writes [5] ($FFCC526C):

1. Fetch the BART base from the decoder-info table, and write $80 to +$00 — the /RESET pulse of
   §2.2 (on a soft restart the hardware line never asserts, so the software pulse is the only reset
   the cards get).
2. Read the software CPU ID, and — unless it is $3011 — write $80 to +$11, disabling BART's slot-$E
   path (§2.5).

The guard is subtler than it looks. Production 6100 hardware reports $3010 from the machine-ID
register at $5FFFFFFC ([1] Table 1-5 p. 10; [AMIC](amic.md) §2.8), but the ROM's identity routine
*promotes* it to $3011 after its AMIC revision probe — the ID the machine's product record actually
carries [5] (identity flow at ROM file offset $10070). So every production 6100 with the adapter
card matches the guard, skips the +$11 write, and keeps its only slot fully enabled; the 7100 and
8100 ($3012/$3013) perform the write and hand slot $E to the PDS video card. The two writes and the
guard are verified instruction-for-instruction in the shipped image [5] (*observed*: one pulse and
one mask write per cold boot on the 7100/8100; neither write on a bridgeless 6100).

Before the Slot Manager runs at all, the start-up code also probes the PDS video slot: it reads
declaration-ROM bytes at $FEFFFF00, +4, +8 and so on — a one-byte-lane stride, the signature checked
against the ASCII string **"VidReset"**, which the probe expects to find within the last 64 bytes of
a one-byte-lane declaration ROM — and on a match reads a follow-on register address and data byte
from the card's ROM and writes it, performing an early reset of an HPV, AV or third-party video card
[5] ($FFCC5298, signature bytes at file offset $C52DA). An empty slot $E must bus-error recoverably
this early in the boot (§3.5).

### 4.3 The Slot Manager's declaration-ROM search

For each slot, the Slot Manager reads the four bytes at $FsFFFFFF down to $FsFFFFFC looking for a
valid ByteLanes value — the low nibble a bit per used lane, the high nibble its complement, from
the 15 legal values of the format-block table, and its position also fixing the format block's
start address [3] pp. 156–159, Table 8-2 p. 159. It then verifies the TestPattern field ($5A932BC7),
the Format ($01, the Apple format) and RevisionLevel (1–9), and checks the whole ROM against the
format block's CRC, counting only bytes on used lanes; any failure stores a slot error in the
slot's sInfo record and the slot is written off [3] pp. 157–159.

The bridge's part in this is purely the fault contract of §3.5: an empty slot's read faults
recoverably and the Slot Manager records the slot as empty. When the first read faults, the search
is abandoned immediately — the follow-up byte-lane reads never happen (*observed*: a full boot
touches exactly one byte, $FsFFFFFF, per empty slot, before moving on).

### 4.4 Block-transfer enable

After a slot's primary initialization, the ROM's slot-initialization path reads the card's
`sBlockTransferInfo` sResource — the longword whose fields are IsMaster, IsSlave, LockedTransfer
and the supported transfer sizes [3] pp. 181–182, Table 8-8. Only a card declaring slave capability
with a supported slave block size of 2 or 8 longwords earns a burst-enable write; a size-4-only
slave is not taken [5] (*observed* in the ROM's slot-initialization code path). The write goes
through the `_HWPriv` trap, selector 12, with the slot number in the low byte of the argument and
the enable flag at bit 8; the selector validates the slot (1 through 14), computes the per-slot
burst byte of §2.6 and sets or clears bit 0, returning `noErr` — and `paramErr` on machines whose
decoder flags name no BART-class bridge [5] ($FFC0F342, BART path $FFC0F38C).

The enables are per-slot latches that no stock boot ever exercises twice: a machine whose cards all
declare no block-transfer capability writes none of them (*observed*: no burst-enable write at all
during a full System 7.5 boot with a display card seated, because that card declares none).

### 4.5 Slot-interrupt service

The OS never polls /NMRQ. A card that needs interrupts is serviced through the enable path — the
Slot Manager sets its pseudo-VIA2 slot-enable bit (set/clear form at $50F26012) when it installs a
handler — and thereafter the level-2 dispatch of §3.6 runs the card's handler whenever the line is
low. The handler acknowledges the device, the device drops /NMRQ, and the flags follow. A card
whose line asserts while its enable bit is clear latches forever: nothing polls the flag bank
outside the interrupt path (*observed*: an enabled display card in slot $C alternates assertion and
release once per frame for the whole of a System 7.5 session, ~989 assertions against ~870 releases
in one boot, with the OS's slot enables reading $78 at $50F26012 — bits 3–6, i.e. slots $C/$D/$E
plus the built-in VBL, under the bit-equals-slot-minus-9 mapping).

### 4.6 NuBus cacheability

NuBus pages are mapped write-through by default, and the HMC performs no cache snooping [1] p. 15 —
the two facts jointly explain why card DMA coherency is the driver's problem and why the DMA
buffers live in write-through or inhibited space. The new ROM (7100/80, 8100/100, 8100/110) adds
three `_HWPriv` selectors that change the cacheability of "a single, contiguous, ascending range of
pages in NuBus superslot space, for each slot": `_NuBusCacheWriteThru` (selector $10),
`_NuBusCacheCopyBack` ($0F) and `_NuBusCacheInhibit` ($0E), each taking a page-aligned start address
and a length that is a multiple of the native page size [2] p. 4. The 7100/8100 new-ROM machines
support all three modes; the 6100/66 and all original models support write-through only [2] p. 4,
Table 1-3 p. 5. These calls change PowerPC MMU page attributes, not BART state (*inferred*; the
selectors carry no BART register access).

### 4.7 Bring-up of a display card

A NuBus display card in a PDM machine is not automatically the boot screen. With a monitor sensed
on the built-in video port, the ROM keeps the menu bar and desktop on built-in video and brings the
card up as an additional screen: the slot's declaration-ROM video driver runs, sets its mode
(640×480 at 1 bpp for the observed card), and paints the standard secondary-screen gray dither
into the card's VRAM (*observed* with a Macintosh Display Card 24AC seated in slot $C of an 8100
under System 7.5; contrast the 68k NuBus machines, where the boot screen follows the highest video
slot). The decision follows the built-in monitor's presence, not slot priority; with nothing
connected to the built-in port, the ROM carves no built-in framebuffer at all and a card becomes
the machine's only screen. A bridgeless 6100 skips the entire NuBus subsystem and boots normally.

## 5. Quirks & errata

- **The processor can access slot $0.** The card-design book states flatly that "the microprocessor
  cannot access slot $0" [3] p. 136 — and BART's registers live at $F0000000, in slot $0's standard
  space, where the processor reads and writes them constantly. The PDM platform is the documented
  exception to the rule.
- **Slot $E has two personalities.** On the 7100/8100 it is the PDS video pseudo-slot, its window
  claimed by the PDS card on the CPU bus, and the ROM disables BART's path there every single boot
  (§2.5, §4.2). On the 6100 it *is* the NuBus slot, and the boot sequence must not disable it.
- **The $3011 guard tests software, not hardware.** Production 6100 hardware reports $3010; the ROM
  promotes it to $3011 before the guard runs, so every production 6100 skips the slot-$E write
  (§4.2). Reasoning from the raw machine-ID register value gets the wrong answer.
- **The wait-state bit is never used.** No shipped code writes $F0000001 (§2.3); its semantics are
  a hole in an otherwise fully mapped register file.
- **The ID register is never read.** The prototype interrupt-swap code that read $F0000008 is absent
  from every shipped ROM; returning the first-revision value $43184000 would resurrect it (§2.4).
- **Locking fails silently.** Locked NuBus transactions neither work nor fault [1] p. 52 — the
  single most surprising bus contract on the platform.
- **Empty slots must fault, not float.** The presence probe, the VidReset probe and the Slot
  Manager search all depend on recoverable faults for absent hardware (§3.5). A window that
  returns $FF for an empty slot breaks the Slot Manager's enumeration model.
- **The fault is a machine check on the way up.** The recoverable slot-space fault surfaces as a
  601 machine-check exception ($00200) reflected into the 68k environment as a bus error — not as
  a data-storage exception (§3.5, *observed*).
- **Burst-enable arithmetic contradicts its own comment.** The register file's definition comment
  ends at +$10; the shipped selector computes slot $E at +$18 (§2.6). The code wins.
- **The burst-enable bytes descend.** Slot 1 sits at +$80 and slot 14 at +$18; a tool that assumes
  an ascending per-slot file writes the wrong slot's latch (§2.6).
- **Slot flags are active low, and bits 1–0 read as 1.** The dispatch inverts the flag byte before
  masking with the enables (§3.6); treating the bank as write-1-to-clear or active-high breaks
  every card driver.
- **The aggregate is edge-triggered.** The handler re-enters itself until all slot lines deassert
  (§3.6) — a level-modelled aggregate stops dispatching after the first service.
- **Block transfers come back, but gated.** The card book's "Quadra only" restriction [3] p. 60 is
  lifted by BART [1] p. 8, but only per slot, only after the card's sResources declare slave sizes
  of 2 or 8 longwords, and only via the selector of §4.4.
- **Card DMA sees the discontiguous RAM map.** There is no translation layer; a DMA card that
  assumes contiguous physical RAM walks into 32 MB gaps [2] pp. 9–10.
- **Fair NuBus arbitration can starve the machine.** The bus gives the system no priority in
  arbitration contests [3] pp. 98–99 — the card book itself warns that many active NuBus masters
  can starve memory refresh and freeze the screen [3] p. 101. BART's mastership of the CPU bus does
  not exempt the platform from this.
- **Two names for the same windows.** DCaD's standard/super slot space [3] pp. 132–133 is the
  Enhanced Dev Note's minor/major slot space [2] pp. 4–5 (§3.1).
- **The board order of the connectors is B, D, C.** The middle connector is slot $D [4] sheet 22 —
  a physical detail that matters when mapping a card's position to its slot number.

## 6. Open questions

1. **BART's NuBus timeout value.** MUNI, the architectural sibling, uses 25.6 µs [6] p. 39; no
   document states BART's figure, or whether it distinguishes an empty slot (nothing to /ACK) from
   a slow card. Only the recoverable-fault outcome is pinned, by software behavior (§3.5).
2. **The reset pulse width** at $F0000000 (§2.2) — the ROM writes $80 and moves on; no document
   states how long the /RESET line stays asserted.
3. **What the slot-$E mask actually masks.** BART is not in the /NMRQ path (§3.6), yet the ROM
   masks "slot E irqs from BART" — presumably a BART interrupt output (timeout or status) that
   would otherwise wire-OR onto the PDS interrupt line (*inferred*). The mask's exact target is
   undocumented.
4. **The wait-state bit's scope** (§2.3): all transfers, slot-space reads only, or register
   accesses; and its reset value.
5. **Register decode granularity**: whether the chip answers offsets above the named registers
   within $F0000000–$F00FFFFF, whether the named registers alias there, and the power-on values of
   every latch in the file (§2.7).
6. **6100 adapter-card arbitration.** The adapter is electrically a PDS card, so its bus
   mastering is assumed to share the PDS request/grant pair into the HMC (*inferred — unverified*);
   the 6100 board is not in the schematic evidence set, and neither is the adapter card itself.
7. **Super-slot breadth on BART 4.** Table 1-3 lists "major A–E" for the BART 4 models [2] p. 5,
   yet no PDM machine has a slot $A connector, and the original ROM's minor-slot range is $9–$E
   per the same document's prose [2] p. 5 — whether BART 4 decodes $A0000000 unconditionally is
   unknown.
8. **BART 21's register surface**: whether the per-slot cacheability calls or the slot-specific
   burst transactions are backed by registers beyond §2.6, and what else the new ROM touches in
   the $F0000000 window (the enhanced-ROM image is outside this evidence set).
9. **Connector labels versus interrupt enables.** The schematic labels the connectors $B/$C/$D and
   the ROM's machine tables list $B/$C/$D as the physical slots [4] sheet 22, [5]; but the slot
   enables the shipped OS raises cover bits 3–5 — slots $C/$D/$E under the bit-equals-slot-minus-9
   mapping — and bit 2 (slot $B) is never enabled or serviced in any observed boot (§4.5). Either
   the AMIC flag bits are wired one slot later than the connector IDs, or the sheet-22 labels are
   board silkscreen rather than slot IDs. Unresolved; both readings are consistent with the
   observed dispatch of a card in slot $C.
10. **The $3011 machine identity**: which hardware, if any, reports it from the ID register without
    software promotion (§4.2), and what the "PDM in QFC" variant recorded in the ROM's machine
    tables at $A55A3011 [5] physically is.
11. **NuBus-master reach into I/O space**: whether BART filters the processor I/O region from
    NuBus masters by decode, or the region simply does not answer them (§3.2).
12. **Headless boot**: whether a 7100/8100 with nothing sensed on the built-in video port brings a
    NuBus display card up as the boot screen in all OS versions, or only as the second screen in
    some (§4.7); the observation here covers one card under System 7.5.
13. **BART 4's burst gating**: whether the per-slot burst bytes of §2.6 gate anything at all on
    BART 4 silicon, or only become meaningful on BART 21 (§3.7).

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh Computers* (Power Macintosh 6100/60,
   6100/60AV, 7100/66, 7100/66AV, 8100/80, 8100/80AV), Developer Press, March 1994 —
   §"Features and Capabilities" p. 8 (BART in all models; block transfers and data bursts; 6100
   optional, 6100/60AV none); Figure 2-1 p. 13 (block diagram: BART and three NuBus '90 slots on
   the CPU-bus spine, absent on the 6100); §"Apple Memory-Mapped I/O Controller" pp. 15–16;
   §"BART NuBus Controller" p. 17; §"High-Speed Memory Controller" p. 15 (no snooping);
   §"System Clocks" Table 2-2 p. 19; §"CPU Bus Arbitration" Table 2-4 p. 20; §"Address Errors"
   and §"Expansion Cards" p. 21 (40 µs unrecoverable error; PDS mastering rules); §"Physical
   Memory Allocations" Table 2-5 p. 22; §"Emulated Interrupt Handling" Figure 2-2 pp. 22–23;
   §"VRAM Expansion Card" p. 40 and §"AV Card" p. 42 (pseudo-slot $E); Chapter 4 "Expansion
   Capabilities" pp. 50–54: Table 4-1 p. 50, §"NuBus Slot Connections" and Table 4-2 pin
   assignments pp. 51–52, Table 4-3 power budget p. 52, locking warning p. 52, §"NuBus Cards for
   the Power Macintosh 6100/60" p. 52, §"DAV Interface" p. 53, §"PDS Expansion Cards" p. 54;
   Glossary ("BART NuBus controller", "block transfer") p. 79.
2. Apple Computer, Inc., *Developer Note: Enhanced Power Macintosh Computers* (Power Macintosh
   6100/66, 7100/80, 8100/100, 8100/110), Developer Press, 1994 — §"NuBus Support" pp. 3–5
   (BART 21 and its burst behavior p. 4; `_HWPriv` cacheability macros and parameters pp. 4–5;
   minor-slot extension to $1–$E and major slot 8 p. 5; Table 1-3 "NuBus changes" p. 5);
   §"Power Macintosh 8100/110" p. 8 (BART 21); Appendix "Power Macintosh RAM Layout" pp. 9–10
   (discontinuous physical addressing for DMA card designers).
3. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third edition,
   Addison-Wesley Publishing Company, 1992 — Chapter 3 "NuBus Data Transfer": §"NuBus Clock"
   p. 52 (/CLK 10 MHz; /CLK2X 20 MHz; /CLK2XEN), §"Card Slot Identification Signals" pp. 54–55
   (/ID3–/ID0 to /AD27–/AD24 and /AD31–/AD28), §"Read Transactions" p. 60 (block transfers
   Quadra-only as of 1992); Chapter 4 "NuBus Arbitration" pp. 98–104 (fair arbitration, contest
   and the /RQST rule pp. 98–99, starvation warning p. 101, bus and resource locking pp. 102–104);
   Chapter 7 "NuBus Card Memory Access" pp. 132–137 (§"NuBus Address Space" p. 132, super slot
   space p. 133, §"Slot Allocations" Table 7-4 p. 136, §"NuBus Bit and Byte Structure" and byte
   swapping pp. 136–137); Chapter 8 "NuBus Card Firmware" pp. 156–159 (§"The Format Block" p. 156,
   §"ByteLanes" pp. 158–159 and Table 8-2 p. 159, §"TestPattern", §"Format", §"RevisionLevel"
   p. 159), §"Block-Transfer Information" pp. 181–182 (Table 8-8).
4. Apple Computer, Inc., Power Macintosh 8100 main-logic-board schematics, drawing 051-0333
   rev A — sheet 22 "NuBus Connectors and Logic" (connectors labeled NuBus Slot B/C/D at J11/J12/J13
   with NuBusID<3..0> straps, board order B, D, C; `NuBusIRQ<2..0>*` per connector; NuBus '90
   control and clock nets with 220 Ω terminations); sheet 23 (BART: bus request `NBCtlrReq*`, grant
   `NBGrant*`, 40 MHz input); sheet 11 (ICS9178 clock synthesizer, `BusClk<0>` → `BartClk`);
   sheet 12 (AMIC slot-interrupt input pins); sheet 4 (HMC arbiter request/grant pairs).
5. Power Macintosh 6100/7100/8100 boot ROM, version $077D, header checksum $9FEB69B3 (March 1994;
   68k image, 4 MB, based at $FFC00000) — disassembly and data tables: the NuBus reset sequence and
   its $3011 guard at $FFCC526C; the slot-$E "VidReset" early probe at $FFCC5298 with signature
   bytes at file offset $C52DA; the slot-interrupt dispatch at $FFC1F0CA–$FFC1F0DE with the
   slot-priority table following; the block-transfer selector at $FFC0F342 with the BART path at
   $FFC0F38C (`base + $80 − 8 × (slot−1)`, `BSET/BCLR #8`); the machine-identity flow promoting the
   6100's $3010 to $3011 at file offset $10070; the per-machine slot-capability tables and the
   decoder-info field with the BART base and presence bit; byte-pattern scans of the whole image
   for the constants $F0000008 and $43184000 (absent).
6. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV
   Computers*, Developer Press, 1993 — §"NuBus Interface" p. 39 (the MUNI bridge's 25.6 µs NuBus
   transaction timeout, as the architectural-sibling comparison for §3.5).
