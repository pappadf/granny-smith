# Cuda — the AV system-management MCU

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, division of labor, standby power,
   clocking
2. [Register file](#2-register-file) — the two register files: the host-side VIA1 window at $50F00000 and the
   MCU's own 68HC05 memory map
3. [Behaviour](#3-behaviour) — the five-wire transport, the packet protocol, ADB, PRAM and the clock, the
   one-second tick, I²C, power and reset
4. [Programming model](#4-programming-model) — how the boot ROM drives the chip, from the reset-time sync
   cycle to a running session
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

**Cuda** is a single-chip system-management microcontroller — Apple part **341S0788**, a Motorola 68HC05-class
part whose mask ROM self-identifies as **"Cuda 2.37"** [4] — that owns every function a Macintosh motherboard
needs to stay alive while the main CPU is off, reset, or busy: the Apple Desktop Bus, the battery-backed
parameter RAM, the real-time clock, the one-second tick, and power and reset control. Apple's own summary of
the part is a six-bullet list:

> The **Cuda** is a microcontroller chip. It
> - turns system power on and off
> - manages system resets from various commands
> - maintains parameter RAM
> - manages the Apple Desktop Bus (ADB)
> - manages the real-time clock
> - lets an external signal through the Apple GeoPort serial port control system power
> [1] p. 16

The same developer note's glossary compresses that to "a microcontroller chip that manages the ADB and
real-time clock, maintains parameter RAM, manages power on and reset, and performs other general system
functions" [1] p. 478. The real-time clock and parameter RAM are "powered by a long-life plug-in battery"
when the machine is off [1] p. 6 — Cuda is the silicon that battery keeps alive.

Cuda is **Egret's successor**: it takes over the system-management role the earlier Egret microcontroller
played on preceding Macintosh models, and the takeover is visible in the boot ROM, whose early start-up code
selects among controller-initialization routines through a three-valued field of the machine's
product-configuration record — value $01000000 takes a branch into a polled primitive with Egret's line
conventions, value $03000000 takes the Cuda path (*observed* in the ROM at $4080F252–$4080F280; the
non-Cuda branch at $4080E3C8 drives the port lines in the style of the older controller, and its
interpretation as "Egret-class" is *inferred — unverified*).

The host never sees a Cuda register. The microcontroller sits on the far side of a five-wire serial link
whose host end is the VIA1 shift register plus three port-B handshake bits (§2.2, §3.1); everything the
main processor does with Cuda — every ADB transaction, every PRAM byte, every read of the wall clock — is
a packet framed over that link (§3.6).

### 1.2 Which machines carry it

| Machine | Board | Cuda's duties in evidence |
|---|---|---|
| Macintosh Quadra 840AV | Cyclone | full set: ADB, PRAM, RTC, tick, power, plus the I²C bus to the video-input chips (§3.10) |
| Macintosh Centris 660AV (later sold as the Quadra 660AV) | Tempest | same platform, same chip [1] |

The AV Quadras are the first Macintosh machines documented to carry the part [1] p. 16. The following
year's first-generation Power Macintosh machines carry the same design: their developer note repeats the
Cuda section nearly verbatim — the bullet list differs only in that an external signal "from **either**
Apple GeoPort serial port" can control system power, because those machines have two GeoPort-capable
ports [2] p. 17. The Power Macintosh 8100 board carries it as **U46**, an unmarked 28-pin SOIC, with a
32.768 kHz crystal, a 3.6 V lithium cell and a battery-switchover chip dedicated to it [5] sheet 16; the
part number 341S0788 itself is known only from the AV-era firmware dump, whose copyright string spans
1989–93 [4] — the date range and the presence of the same transport protocol on both platforms make the
PDM part the same or a very close firmware revision (*inferred — unverified*; no PDM-board Cuda dump is in
the evidence set).

### 1.3 Division of labor

Cuda owns functions, not buses. The split that keeps this page honest:

| Function | Owner |
|---|---|
| ADB device protocol — command byte, register reads/writes, service requests | Cuda executes it; the protocol itself is specified at [ADB](../../hardware/adb.md) |
| ADB *content* — what the keyboard and mouse say | the devices |
| PRAM *medium* — the 256 battery-backed bytes and the read/write path | Cuda (§2.5, §3.9) |
| PRAM *layout* — which offset holds the startup device, the alarm, the volume | the ROM/OS convention; the map lives at [PRAM format](../../formats/mac-pram.md) |
| Real-time clock — the seconds counter, the epoch, the alarm | Cuda (§3.9) |
| The one-second interrupt as the OS sees it | a packet Cuda sends every second (§3.9), delivered by the ROM's level-1 interrupt path |
| Soft power on/off, wake, reset pulse | Cuda, plus the PSU's trickle rail (§3.11) |
| The I²C bus to the video digitizer chips | Cuda as bus master, via one pseudo-command (§3.10) |
| The five-wire transport, the VIA1 register bank, interrupt levels, timers | the [PSC](psc.md) — there is no 6522 chip on these boards (§2.2) |

### 1.4 Standby power

Cuda lives on the machine's battery- and trickle-backed power domain, not the main CPU rail. That is the
point of the part: with the machine "off" it keeps the clock and PRAM alive on the lithium cell [1] p. 6,
senses the power-on request lines, and switches the supply. The ADB connector dedicates a pin to this: pin
2, "Power on, fed by +5 V through 100 kΩ; connect to pin 4 to turn on the system" [1] Table 2-4 p. 21 —
shorting the ADB power pin to ground at the keyboard is the soft-power-on path, and Cuda is the logic that
sees it. The GeoPort path is the same idea from the serial port: an external signal on the telecom
connector can wake the machine [1] p. 16.

### 1.5 Clocking

Cuda's only documented clock is **CudaClk, a 0.032768 MHz (32.768 kHz) crystal** — "Crystal" appears in the
source column of the developer note's clock table, against every divided or oscillator-derived clock around
it [1] Table 2-2 p. 17. The successor note rounds the same crystal to "0.0320 MHz" [2] Table 2-3 p. 20 —
the same part, quoted to fewer digits. That crystal is the real-time clock's timebase: the firmware's
one-second interrupt handler is the vector at the top of the ROM [4] (§3.9), so the tick chain is
crystal → MCU timer interrupt → seconds counter.

Whether the 32.768 kHz crystal also clocks the 68HC05 core, or the core runs from a faster internal
derivation, is not established by any document in the evidence set (§6.8). The firmware does contain
timing that scales by board: its delay routines branch on a port-D strap bit to choose between two delay
lengths [4] (§6.8), so at least one firmware-visible property differs across boards that run this image.

## 2. Register file

Cuda presents **two** register files, one at each end of the wire, and neither is a CPU-mapped peripheral
bank. The host end is the VIA1 window; the Cuda end is the 68HC05's own memory map, recovered from the
firmware image. Both are specified here because the re-implementation test needs both: a working Cuda is a
pair of state machines that must agree bit-for-bit.

### 2.1 Host-side window: the VIA1 bank at $50F00000

There is no 6522 chip on an AV board. The developer note's functional-unit inventory has no VIA in it, and
the PSC's own bullet list says it "handles all internal system interrupts" and "handles system interrupts
from the Versatile Interface Adapter (VIA) inputs" [1] p. 13 — the VIA1 register bank the ROM drives is a
6522 emulation implemented inside the PSC, and the chip's corner-case fidelity beyond what the ROM
exercises is a PSC question ([PSC](psc.md)). The base address is not in the developer note; it
is pinned by the ROM, which references **$50F00000** absolutely and loads the same value from its
decoder-information table on every Cuda call (*observed*: `$50F01C00` and `$50F03C00` — the two IER
registers — are written during start-up at $4080AD34E–$4080AD36E, and every Cuda routine in the manager
cluster loads its base from table slot +8, e.g. `MOVEA.L $8(A0),A2` at $4080C84C) [3].

Registers follow the classic Macintosh VIA stride, register *n* at offset *n* × $200:

| Offset | Register | Used by the Cuda transport |
|---|---|---|
| +$0000 | vBufB — port B data | the three handshake bits, written and read constantly |
| +$0400 | vDirB — port B direction | PB4/PB5 outputs, PB3 input (§2.3) |
| +$1400 | vSR — shift register | the byte pipe: written by the host, filled by Cuda's clock |
| +$1600 | vACR — auxiliary control | bit 4 selects SR direction under external clock (§3.1) |
| +$1A00 | vIFR — interrupt flags | bit 2 = shift-register interrupt; bit 7 = any enabled flag ORed |
| +$1C00 | vIER — interrupt enables | bit 2 = SR interrupt enable, bit 7 = set/clear select |
| others | timers, port A, PCR | outside this page — the VIA1 timers carry the 60 Hz tick and the DBRA calibration, a family-level concern |

### 2.2 What the start-up code writes

The ROM's VIA bring-up probes the two IER registers by writing **$7F** to each — a 6522 write whose top
bit is clear, i.e. "disable every interrupt source" (*observed* at $4080AD34E–$4080AD36E) [3] — and the
Cuda sync cycle then leaves the port at idle polarity. The host's port-B writes in the Cuda manager use
exactly two constants: **$30**, setting PB4 and PB5 together (BYTEACK and TIP both negated — the idle
line state), and single-bit BSET/BCLR #4 (BYTEACK) and #5 (TIP) operations for the handshake steps
(*observed* throughout the manager cluster, e.g. `ORI.B #$30,(A2)` at $4080C850, `BCLR #4,(A2)` at
$4080C882, `BCLR #5,(A1)` at $4080C9BE) [3]. The direction bit the transaction code toggles is always
**ACR bit 4** (`BSET/BCLR #4,$1600` at $4080C9AC/$4080C9FE), flipping the externally-clocked shift
register between shift-in and shift-out without touching the SR-mode bits below it [3].

### 2.3 Port B bit map (host view)

| Port B bit | Direction | Name | Meaning |
|---|---|---|---|
| 3 | input | **TREQ** (transaction request) | active **low**: Cuda holds it low while it has a packet to send, and raises it **with the last byte** of that packet (§3.4) |
| 4 | output | **BYTEACK** (byte acknowledge) | a **level**, toggled by the host once per byte — its *change* means "byte consumed, next please"; also the sync-cycle strobe (§3.5). Idle high. |
| 5 | output | **TIP** (transaction in progress) | active **low**: the host asserts it to start a transaction and negates it to end one. Idle high. |
| 0–2, 6–7 | — | reserved | the ROM's port-B definitions mark these three plus two bits "reserved" for unnamed internal uses; nothing in the Cuda manager touches them, and their loads are unidentified (§6.3) |

BYTEACK is called TACK in the published Linux driver for this interface, which documents the same three
bits on the same port lines [7].

### 2.4 The Cuda side: 68HC05 memory map

The MCU's own address space, as the firmware image and its reset code establish it [4]:

| Range | Contents |
|---|---|
| $00–$06 | I/O port data and direction registers: PORTA, PORTB, PORTC, PORTD, then DDRA, DDRB, DDRC ($07+ are the further port/control registers of the 68HC05 family; the image touches at least one port-D strap bit in its delay code) |
| $40–$BF | the part's internal RAM (extent inferred from the addresses the image uses): work variables and buffers — every location in the table below lives in this band |
| $A0–$B4 | firmware state variables (table below) |
| $0100–$01FF | **parameter RAM — exactly one 256-byte page** (§2.5) |
| $0F00–$1FFF | mask ROM, 4352 bytes: identity strings at $0F00 and $0F4E, the 7-byte port/DDR init table at $0F57, code from $0F5E, the CPU vectors at $1FF6–$1FFF |

The image mapping at $0F00–$1FFF, with the CPU vectors in the top ten bytes, places the part squarely in
the 68HC05 family's 8 KB address-space mold (*inferred* from the image layout; no 68HC05 datasheet for
this Apple part is in the evidence set, §6.12). The CPU vector table [4]:

| Vector | Target | Role |
|---|---|---|
| $1FF6 | $1E72 | **one-second / real-time-clock tick** (§3.9) |
| $1FF8 | $1E99 | timer interrupt |
| $1FFA | $1E57 | host (VIA) interrupt — the transport's input path |
| $1FFC | $0000 | software interrupt — unused |
| $1FFE | $0F5E | **RESET** — immediately after the version string in the image, the first thing a tracing eye meets |

The state variables the disassembly establishes [4]:

| Address | Contents |
|---|---|
| $A2 | the **shadow of the host's BYTEACK level**, bit 2 — the receive routine spins until the pin and the shadow differ, which is how Cuda sees "the host toggled the level" |
| $A7–$AA | a three-byte **self-modifying stub** the PRAM routines build and call (§2.5) |
| $A3 | status byte, bit 6 set when the RTC equals the power-up/alarm time (§3.9) |
| $AB–$AE | the **32-bit real-time clock counter**, big-endian, incremented by the $1E72 handler (§3.9) |
| $AF–$B2 | the **power-up / alarm time**, compared against the counter every second (§3.9) |
| $B9, $BA | the current packet's **type** and **command/code** bytes — the packet layer's head (§3.6) |

### 2.5 PRAM: one page, one battery

The parameter RAM is **256 bytes at $0100–$01FF** [4]. The read and write routines do not index a table;
they build a three-byte instruction stub in RAM at $A7 — `LDA $01xx / RTS` for a read, `STA` for a write,
with the target's low byte spliced from the command packet — and call it [4] ($1808/$1827). A nonzero
address high byte in the command is rejected with **error packet code 4** [4]: the addressable window
really is one page, and nothing else in the MCU map is reachable through the PRAM commands.

The bytes themselves carry the classic Macintosh 256-byte extended-PRAM layout; the map — validity
signature, startup device, alarm, volume, and the rest — is specified at [PRAM format](../../formats/mac-pram.md)
and is not restated here. The ROM validates the page on every boot (§4.2) and reinitializes it when the
signature fails. Backup is the "long-life plug-in battery" [1] p. 6 — the same cell that keeps the clock
alive.

### 2.6 Reset state

The firmware's own reset is seven bytes wide: RESET copies **PORTA=$00 PORTB=$00 PORTC=$00 PORTD=$00
DDRA=$99 DDRB=$92 DDRC=$08** from the table at $0F57 into the port registers [4]. `DDRB = $92 = %10010010`
makes PORTB bits **1, 4 and 7 the only outputs** — the pin-level fact that pins the whole transport down
(§3.1). Nothing in the evidence set states the power-on values of the *host-side* latches (the VIA
emulation's registers), and the start-up code writes them before it ever reads them, so only the written
values matter to software (§6.10).

## 3. Behaviour

### 3.1 The transport in one paragraph

The link is five wires: the three handshake bits of §2.3 plus the VIA's CB1 (shift clock) and CB2 (shift
data). **Cuda is the clock master.** The firmware drives one low-to-high pulse on its PORTB bit 4 (wired
to the host's CB1) per bit, in *both* directions [4]; the host's shift register runs in the 6522's
externally-clocked modes, and the ROM's transaction code only ever flips ACR bit 4 — the direction —
never the mode bits that select "external clock" [3]. Both byte routines in the firmware — receive at
$1488, send at $154B — are **fully unrolled eight-bit sequences with no loop counter**, shift **MSB
first**, and drive the data line **non-inverted**: a one bit drives it high [4]. A byte therefore costs
eight Cuda clocks; there is no host-side bit rate to get wrong, but also no way for the host to clock
data itself.

### 3.2 Line states

The transport's state is the tuple (shift direction, TIP, BYTEACK, TREQ). The complete line-state table,
as the host-side manager of the era defines it, with the physical active-low levels of TIP and TREQ:

| Shift dir | /TIP | BYTEACK | /TREQ | Meaning |
|---|---|---|---|---|
| in | 1 | 1 | 1 | **Idle** — nothing happening |
| in | 1 | 1 | 0 | **Attention** — Cuda wants to send |
| in | 1 | 0 | 0/1 | **Abort/sync** — the sync-cycle strobe, or an aborted transaction |
| in | 0 | 0 | 0 | Receive data byte |
| in | 0 | 0 | 1 | Receive **last** data byte |
| in | 0 | 1 | 0/1 | Receive data byte |
| in | 0 | 1 | 1 | Receive **last** data byte |
| out | 0 | 0 | 1 | Transmit data byte |
| out | 0 | 1 | 1 | Transmit data byte |
| out | 1 | 0 | 0/1 | **Abort/sync** |
| out | 1 | 1 | 1 | **Idle** (all other combinations illegal) |

Two readings fall out of the table that an implementer must not miss: **TREQ is part of every state** —
the host may not start a command while Cuda is requesting the bus — and **BYTEACK is a level, not a
pulse**: its state relative to the previous byte is the signal, which is why the firmware keeps a shadow
copy and waits for pin and shadow to differ (§2.4) [4].

### 3.3 Host-to-Cuda flow (command packet)

1. The host requires idle: TREQ high, manager not busy. If TREQ is low, Cuda has traffic — the host must
   service that first (§3.4) and may then retry.
2. The host sets the shift register to output mode (ACR bit 4 set), writes the first byte — the **packet
   type** (§3.6) — to the SR, sets BYTEACK, and **asserts TIP low**. Loading the SR arms the transfer:
   Cuda sees TIP, clocks the eight bits, and leaves TREQ alone. The SR interrupt fires when the eighth
   Cuda clock completes.
3. **Collision check.** After the first byte's SR interrupt the host samples TREQ. TREQ low means Cuda
   started a transaction of its own at the same moment; the host yields — it flips the SR to input,
   dummy-reads it, raises TIP and BYTEACK, and drains Cuda's packet before re-sending.
4. Each further byte: write the SR, **toggle** BYTEACK, wait for the SR interrupt.
5. After the last byte the host flips the SR back to input, dummy-reads it once (a direction change can
   advance the 6522's internal bit counter — the read re-synchronizes it; §5), and raises TIP and
   BYTEACK together.

The per-byte rule "write, toggle, wait" and the terminal "raise both" are *observed* instruction-for-
instruction in the ROM's interrupt-driven send path ($4080C3F6–$4080C43E: SR-mode set, byte written to
$1400, BYTEACK set, TIP cleared, SR flag polled at $1A00, direction flipped back, dummy read) [3]. The
published Linux driver implements the identical sequence against this hardware [7].

### 3.4 Cuda-to-host flow (responses, unsolicited packets)

1. Cuda asserts **TREQ low** and clocks an **attention byte** into the host's SR. The byte's value is a
   don't-care — the host discards it; the event is the SR interrupt itself, taken with the lines in the
   attention state of §3.2. The host answers by asserting TIP low.
2. Cuda shifts the first real byte. Per byte, the host reads the SR (which clears the SR interrupt) and
   samples TREQ: **TREQ still low means more bytes follow** — the host toggles BYTEACK and Cuda shifts
   the next byte; **TREQ high with the byte means that byte was the last**. The Linux driver states the
   same rule: a read is finished when TREQ is no longer asserted with the byte in hand [7].
3. The host terminates with TIP and BYTEACK both high.
4. **The idle acknowledge.** A short, nonzero interval after the host terminates the transaction, Cuda
   clocks **one extra byte** through the SR. The host does not consume it as data; it waits for that SR
   interrupt, clears it, and only then treats the bus as idle. The sync cycle of §3.5 waits for it with
   an explicit bounded loop (*observed* at $4080C8C6–$4080C8D6, after the BYTEACK raise: a 15000-iteration
   wait on the SR flag with a branch to the dead-chip path on timeout) [3]. Until the idle acknowledge has
   been taken, the bus is not idle.

Unsolicited traffic — autopoll data, the one-second tick (§3.9), power messages — uses exactly this flow,
always TREQ-initiated, always ending with the TREQ rise on the final byte.

### 3.5 The sync cycle

The transaction that starts every session is a special case the manager runs before anything else:

1. With the bus idle (TIP and BYTEACK high), the host waits out a **3333-iteration** settle loop, then
   checks whether Cuda is already mid-transaction: TREQ low means the SR interrupt for Cuda's in-flight
   byte is pending or arriving — the host waits for it and dummy-reads the SR either way
   (*observed* at $4080C84C–$4080C878) [3].
2. The host **asserts BYTEACK low with TIP still high** — the only line state in the whole protocol
   where BYTEACK is low and TIP is high on purpose. This is the sync strobe.
3. Cuda answers by asserting TREQ and clocking one byte. The host waits for that byte's SR interrupt,
   with a **15000-iteration** timeout whose exhaustion takes the machine to the dead-chip path [3]
   ($4080C886–$4080C890).
4. The host **raises BYTEACK**. Cuda negates TREQ and, a short interval later, clocks the idle
   acknowledge byte of §3.4 step 4; the host waits for TREQ to rise, clears the SR interrupt, then waits
   for the idle-acknowledge SR interrupt — each wait with the same 15000-iteration budget, each timeout
   fatal [3] ($4080C896–$4080C8DA).
5. The host raises TIP and BYTEACK together and clears the SR interrupt. The bus is idle — and, from
   Cuda's side, **every asynchronous message source is now disabled**: autopoll, the one-second tick and
   power messages stay silent until the host re-enables them by command (§4.2).

The dead-chip path is a named destination in the ROM: **DeadCuda** at $4080C9DC raises both handshake
lines, loads error code **$30** into the argument the failure-reporting routine consumes, and branches
into the start-up failure path [3] — the one that plays the death chimes. A Cuda that answers nothing at
sync time is a machine that does not finish booting.

### 3.6 Packet formats

Every transaction in both directions is a packet. Types:

| Type | Name | Direction | Carries |
|---|---|---|---|
| $00 | ADB packet | both | an ADB command byte plus listen data; ADB responses and autopoll data come back the same way |
| $01 | pseudo-command packet | host→Cuda | the management command set (§3.7) |
| $02 | error packet | Cuda→host | a code, plus the type and command bytes of the rejected packet |
| $03 | tick packet | Cuda→host | the one-second tick — **only two bytes on the wire** (§3.9) |
| $04 | power message packet | Cuda→host | switch and power events (§3.11) |
| $05 | I²C packet | both | asynchronous I²C traffic (§3.10) |

**Command packet** (host→Cuda): `[type] [command] [parameters…] [data…]`. The type byte goes out with
the TIP assertion; every following byte is announced by a BYTEACK toggle.

**Response packet** (Cuda→host): `[attention] [type] [flags] [command] [data…]`. The attention byte is
discarded (§3.4); the response header proper is then type, flags, command, and the command byte echoes
what the response answers. Data length is not stated in the header — it is *encoded by TREQ rising with
the final byte* (§3.4), so the host reads until TREQ says stop. The flags byte carries per-transaction
status: bit 0 = a device asserted SRQ during the poll, bit 1 = the addressed device had no data, bit 2 =
excessive SRQ period, bit 3 = a bus bit-cell timing error, bit 6 = the data came from autopoll rather
than an explicit command, bit 7 = a response packet in progress [6].

**Error packet**: `[attention] [$02] [code] [type] [command]` — **five header bytes**, one more than
every other packet. The host reads the fifth header byte only when byte 1 equals $02 (*observed* in the
ROM's polled primitive: the response's type byte is compared against 2 and the read length changes on
equality, $4080C9A2–$4080C9A8) [3]. Error codes witnessed in the firmware: **2** — invalid pseudo-command;
**3** — meaning not established (§6.5); **4** — PRAM address out of range [4].

### 3.7 The pseudo-command set

Dispatch inside the firmware is a model of economy: the command byte is compared against **$25** (37),
anything at or above is rejected, and the value ×3 indexes a 37-entry jump table at $12E4 [4]. The count
37 matches the host-side command definitions of the era — the same interface seen from both ends
(*observed*: the dispatch at $12C4 is `CMP #$25 / out-of-range → error`, and the host-side bound of the
day is the same number) [4] [6].

Twenty-five commands are implemented; **twelve opcodes the host-side definitions of the era name are
rejected by this firmware** — their dispatch slots jump straight to the error path, returning an error
packet with code 2 [4]:

| Opcode | Host-side name of the era | Status |
|---|---|---|
| $04 | read ROM size | **rejected** |
| $05 | read ROM base | **rejected** |
| $06 | read ROM header | **rejected** |
| $0F | controller diagnostics | **rejected** |
| $15 | read PRAM size | **rejected** |
| $17 | write bus delay | **rejected** |
| $18 | read bus delay | **rejected** |
| $1C | enable/disable keyboard NMI | **rejected** |
| $1D | enable/disable parse mode | **rejected** |
| $1E | write hang timeout | **rejected** |
| $1F | read hang timeout | **rejected** |
| $20 | set default sound string | **rejected** |

The implemented set:

| Op | Function | Parameters | Returned | Semantics |
|---|---|---|---|---|
| $00 | no-op | — | ack | returns a normal acknowledgement |
| $01 | autopoll on/off | 1 byte: $00 off, nonzero on | — | enables autonomous ADB polling (§3.8) |
| $02 | read MCU address | 2: big-endian address | open-ended | reads any of Cuda's own RAM/register space |
| $03 | read time | — | 4 bytes | the RTC seconds counter, big-endian (§3.9) |
| $07 | read PRAM | 2: big-endian address | open-ended | §2.5; high address byte must be 0 |
| $08 | write MCU address | 2 + data | — | writes Cuda's own RAM/register space |
| $09 | write time | 4: big-endian seconds | — | sets the RTC (§3.9) |
| $0A | power down | — | ack | powers the machine off **after** the ack (§3.11) |
| $0B | write power-up time | 4: big-endian seconds | — | programs the alarm/power-up comparator (§3.9) |
| $0C | write PRAM | 2 + data | — | streams data bytes ascending from the address |
| $0D | monostable reset | 1 | — | reset flag semantics not fully established (§6.15) |
| $0E | write DFAC string | data bytes | — | legacy sound-control serial string; on these machines the sound path is the PSC/Singer chain — whether anything sends it is unestablished (§6.14) |
| $10 | read battery/control panel | — | 1 | battery-changed / control-panel sense |
| $11 | restart | — | ack | **system restart**: Cuda pulses the reset line, after the ack (§3.11) |
| $12 | set IPL / legacy Vpp | 1 | — | legacy line on machines of the era |
| $13 | enable/disable file-server mode | 1 | — | governs auto-repower after power loss (§3.11) |
| $14 | set autopoll rate | 1 | — | §3.8 |
| $16 | read autopoll rate | — | 1 | — |
| $19 | write device list | 2: bitmap | — | the 16-bit ADB autopoll bitmap, bit *n* = ADB address *n* (§3.8) |
| $1A | read device list | — | 2 | the bitmap, high byte first (§3.8) |
| $1B | write one-second mode | 1: 0 off, 3 = tick carries the clock | — | §3.9 |
| $21 | enable/disable power messages | 1: selector 0–4 | — | off/on/suspend/continue/debug-continue; the firmware sub-dispatches through a five-entry branch table at $1B28 [4] (§3.11) |
| $22 | I²C read/write | slave address + wire bytes | data on reads | **the only route to the video digitizer's I²C bus** (§3.10) |
| $23 | wakeup mode | 1 | — | arms wake events (§3.11) |
| $24 | shutdown-timer tickle | 1 | — | services Cuda's own shutdown watchdog while power messages are in use |

Opcode $25 is special-cased by the successor ROM's manager as a second I²C-class command with unknown
firmware semantics [6] — this firmware rejects it (dispatch bound $25, above), so on an AV machine it
must return the invalid-command error.

### 3.8 ADB: autopoll and the device list

The ADB command byte, register model, Talk/Listen/Flush encodings and service-request line are specified
once at [ADB](../../hardware/adb.md); this page covers only what the MCU adds. Cuda is the bus's only
master. It executes explicit ADB commands from command packets ($00) and, when autopoll is on, **polls
the bus on its own**: at the autopoll rate it issues a Talk-register-0 to the most recently serviced
device in its list, walking the list when a device asserts service request, and delivers the results as
unsolicited ADB packets with the autopoll flag (bit 6) set in the response header [6]. The polled set is
the **device-list bitmap** (write $19 / read $1A) — one bit per ADB address; the host installs the
enumerated device set into it and switches autopoll on (§4.4).

The firmware keeps two housekeeping bytes at $B3/$B4 recording the most- and least-recently serviced ADB
addresses; the host-side ADB layer pokes them through the write-MCU-address command ($08) when an
explicit command addresses a device missing from the bitmap — the successor ROM's ADB primitives do
exactly this [6], and the same two bytes sit inside this firmware's RAM map (*inferred — unverified* for
the AV ROM's own behavior; §6.6).

### 3.9 The real-time clock and the one-second tick

Time is a **32-bit big-endian seconds counter** at $AB–$AE, Macintosh epoch (00:00:00, 1 January 1904 —
the same epoch every Macintosh clock uses; the published Linux driver converts with a constant of
2082844800 seconds [7]). The one-second interrupt handler at $1E72 increments it [4].

**The reset seed is a birthday.** RESET loads the counter with **$630BD178**, which is **27 August 1956,
20:35:04** in the Macintosh epoch [4] — a machine with a dead battery or blank state therefore comes up
with its clock set to a late-August 1956 evening, a property Apple's firmware shipped deliberately (the
constant is data in the mask ROM, not an accident of cleared memory). There is **no write-protect
mechanism** on this clock — unlike the classic discrete RTC chip, whose PRAM and seconds counter sit
behind a write-protect register ([RTC](../../hardware/rtc.md)); protection here is purely by command
surface: only the write-time command ($09) moves the counter.

A second 32-bit value at $AF–$B2 is the **power-up / alarm time**; the same one-second handler compares
all four bytes against the counter every tick and sets bit 6 of the status byte at $A3 on a match [4] —
the mechanism behind "manages system resets from various commands" and scheduled power-ups.

**The tick as software sees it.** When one-second mode is enabled (write $1B), Cuda sends one unsolicited
packet per second. Two forms exist, both handled by the host-side tick handler [6]:

- the **bare tick packet**: type $03, and that is all — two bytes on the wire including the attention
  byte, no data, no clock value;
- the **clock-carrying form**: the tick arrives as a read-time response, a normal packet whose data is
  the four seconds bytes.

Which form the host gets depends on the mode value written: 0 silences the tick (the debugger does this
while stopped, §4.5), and the value 3 selects the clock-carrying form [6]. The handler that receives
either runs the classic one-second housekeeping — the same slot in the level-1 dispatch that the
one-second interrupt occupied on VIA-wire machines — then chains onward. Which form the AV ROM asks for
was not pinned by the disassembly evidence here (§6.9).

### 3.10 The I²C master: pseudo-command $22

Cuda is also the board's I²C master for the video digitizer. The Philips digital multistandard decoder
and the video display controller of the video-input subsystem are reachable **only** through
pseudo-command $22 — there is no other route from the CPU to their control registers; the handler lives
at $192C in the firmware [4], and the successor-era host manager's parameter convention is: the first
parameter byte is the **I²C slave address** whose **bit 0 is the direction** (even = write, odd = read),
and the remaining parameter bytes go onto the I²C wire — for these parts, the subaddress first [6].
Writes receive the ordinary acknowledgement; reads append the data bytes to it.

The two slaves on the AV machines are the decoder at address **$8A/$8B** and the video display
controller at **$B8/$B9** (write/read forms), the chips specified at [video digitizer](vdc.md). The
digitizer driver's bring-up writes shadow arrays of 25 and 17 bytes into the two parts through this
command, and every mode change of the video-input pipeline is a sequence of $22 transactions. On the
successor platform the same command instead reaches the monitor connector's DDC pair [5] sheet 18 —
same firmware facility, different bus load.

Anything the bus does not answer is an error; the firmware's behavior for I²C addresses beyond the two
parts was not analysed (§6.7).

### 3.11 Power and reset control

**Cuda is the soft-power latch.** There is no separate power-management chip on these boards: the
battery- and trickle-fed microcontroller senses the power-on request lines and drives the supply's enable.
The request paths are physical: the ADB connector's power pin (pin 2, pulled to +5 V through 100 kΩ,
"connect to pin 4 to turn on the system" [1] Table 2-4 p. 21) is the keyboard power key; the GeoPort
telecom connector carries an external wake signal [1] p. 16. What the AV-era Cuda drives on its side —
which port pin switches the supply, and how — is not documented for these boards; the successor board
documents it in full: Cuda's port A bit 0 drives a "power fail/enable" line to PSU connector pin 21, and
the cell-and-switchover circuit keeps Cuda alive on the trickle rail while the main supply is off
[5] sheet 16 (*inferred — unverified* that the AV boards wire the same pin; §6.11).

The command surface:

- **$0A power down** — Cuda acknowledges the command first, *then* removes power. The host therefore
  gets a clean completion before the machine dies.
- **$11 restart** — same discipline: acknowledge, then pulse the system reset line. On the successor
  board the reset path is Cuda's port C bit 3 into the system controller's reset input, and the front
  panel's reset button drives the same net in parallel, so a hardware reset works even with a wedged
  controller [5] sheet 16 — the AV wiring of that net is undocumented (§6.11).
- **$0B write power-up time** — the scheduled wake comparator (§3.9).
- **$23 wakeup mode** — arms wake events from the external sense lines.
- **$13 file-server mode** — the flag that governs whether the machine re-powers itself after an
  outage (a file server must come back; a desktop must not).
- **$21 power messages** and **$24 tickle** — when power messages are enabled (selector 1), Cuda
  reports switch and power events as unsolicited type-$04 packets, whose command byte identifies the
  initiator: chassis switch, keyboard switch, or key switch, with sub-selectors for the secure/on/power-
  down states [6]. The tickle services Cuda's own shutdown watchdog while this message stream is
  running — a machine that stops tickling while power messages are enabled is a machine Cuda will
  eventually power off on its own [6].

**Hard power-off does not consult Cuda.** The machines of this generation keep a physical switch in the
AC path; "turns system power on and off" [1] p. 16 refers to the soft path. The successor generation
adds an explicit hard-off capability to the same controller family [2] — the AV boards' exact hard-off
arrangement is a machine-level question ([Quadra 840AV](q840av.md)).

### 3.12 Hangs, timeouts and error paths

The protocol is deliberately timeout-free in its interrupt-driven phase: once the sync cycle has
passed, the command manager *relies on Cuda answering* — its byte loops are bounded only in the polled
primitive, which carries a **per-byte budget of $2000 polls** and a **retry budget of $0100 attempts**
(*observed* as the paired constants `MOVE.L #$20000100,D4` at the head of the polled primitive,
$4080C8EE) [3]. Budget exhaustion is fatal in the same way a sync timeout is: the dead-chip path with
error code $30 [3]. In normal operation the budgets never come into play — they exist to catch a Cuda
that stopped clocking.

The firmware's own error reporting is the error packet (§3.6): invalid pseudo-command (code 2), PRAM
address out of range (code 4), and an as-yet unidentified condition (code 3) [4]. The host-side manager
also synthesizes its own error values for malformed packets rather than putting them on the wire [6].
A transaction aborted mid-stream — the host dropping TIP, or Cuda seeing TIP high mid-transfer — is
cleaned up by the firmware's abort routine ($15E1), which returns the port lines to idle and raises TREQ
[4]; the host side of an abort is the drain-and-retry dance of §3.3 step 3.

## 4. Programming model

This is what the Macintosh Quadra 840AV / Centris 660AV boot ROM (2 MB, checksum $5BF10FD1, mapped at
$40800000) actually does with the chip, from reset to a running session [3].

### 4.1 Where Cuda sits in the boot

The 68040 fetches its reset vector from the ROM's header and lands in the start-up path at $40800074;
the Phase-B initialization block at $4080F1D0 — bus-error vector, hardware identification, relocation,
VIA bring-up, the system-management controller init, memory-controller setup — runs **before** the
power-on self tests and before RAM is sized [3]. The controller init is selected by the three-valued
product-configuration field of §1.1; on these machines the field reads 3 and control enters the sync
cycle at $4080C84C (branch at $4080F27A) [3]. The consequence is architectural: **Cuda is synchronized
before there is any RAM to speak of**, and the early traffic runs entirely on registers and the stack
the ROM carved for itself.

### 4.2 The sync cycle and what it buys

The sync cycle of §3.5 is the first transaction of every boot, and its side effect is as important as its
acknowledgement: **all asynchronous message sources are disabled by it** — autopoll, the one-second tick,
power messages all go silent, so that the tightly-polled early traffic that follows cannot be
interrupted by an unsolicited packet [6]. Nothing re-enables them until the OS asks, command by command
(§4.4, §4.5).

The cycle's timeouts make the machine's dependency explicit: three separate 15000-iteration waits, each
with a branch to DeadCuda ($4080C9DC, error code $30) [3]. A Cuda that does not complete the handshake
is a machine that plays the death chimes and stops.

### 4.3 Early PRAM traffic: polled, RAM-less

Between the sync cycle and the memory-system init, the ROM reads PRAM through the transport with no
interrupt manager and no RAM: the polled byte primitives bit-bang complete command packets by spinning
on the VIA flag register rather than taking interrupts — the polled primitive of §3.12 with its $2000/
$0100 budgets is this code (*observed*: the primitive's head at $4080C8EA polls the TIP state before
every byte and services an in-flight Cuda packet first) [3]. Through this path the start-up code reads
the PRAM validity signature — the extended-PRAM identification bytes — and later the OS's clock
validation routine checks the page's four-byte signature and, on failure, reinitializes the page from
the ROM's default-value tables (the layout and the signature semantics live at
[PRAM format](../../formats/mac-pram.md)). A behavioral Cuda must therefore be able to serve real
read-PRAM traffic byte-by-byte from the earliest moments of boot.

### 4.4 The manager, and ADB bring-up

Once the memory system exists, the ROM installs the interrupt-driven command manager: the VIA's
shift-register interrupt handler is hooked into the level-1 dispatch, the shift register is switched to
input, and every subsequent transaction is interrupt-driven with polled fallback [3] [6]. The manager's
traffic during a normal boot, in order:

1. **ADB reset** — the SendReset command on the bus, re-initializing every device ([ADB](../../hardware/adb.md)).
2. **Enumeration** — a Talk-register-3 probe at every ADB address in turn; a device that
   answers is recorded, and devices found sitting on default addresses are moved to free addresses by a
   Listen-register-3 address-change sequence, then re-probed, with a bounded number of move attempts to
   settle collisions [6].
3. **Autopoll enable** — the enumerated address set is written as the device-list bitmap (write $19),
   then autopoll is switched on (command $01 with a nonzero operand); from here on, mouse motion and
   keypresses arrive as unsolicited packets with the autopoll flag set [6].
4. **A keyboard flush** — clearing any key state that predates the enumeration.

The keyboard sits at ADB address 2 and the mouse at 3 by convention, and the two move when the
enumeration needs the space — the *devices'* answers to register 3 identify them, and the handler-ID
byte there is what the OS matches drivers against (the register model is specified at
[ADB](../../hardware/adb.md)). A minimal machine needs exactly a keyboard and a mouse that answer
Talk R0/R3, tolerate register-3 listens, and return a clean no-data timeout flag on empty polls [6].

### 4.5 The one-second interrupt

After the manager is running, the ROM enables the one-second tick (write $1B) so the OS's clock keeps
time; the tick handler at the far end runs the classic one-second housekeeping and chains onward
(§3.9). The debugger hooks the same switch: entering the low-level debugger silences the tick (mode 0)
so stopped machine time does not advance, and leaving it re-enables the mode the OS had [6]. Which mode
value the AV ROM writes was not pinned here (§6.9).

### 4.6 Interrupt masking: how much of the CPU Cuda is allowed to steal

The transaction primitives deliberately do **not** run at the top interrupt priority. The ROM's
byte-send path masks only to **interrupt level 3**: it examines the saved processor status, and where
the mask was higher it explicitly clears the top bit of the interrupt-priority field — the ANDI-to-SR
at $4080C410 — so that level-4-and-above sources keep firing mid-transaction (*observed* in the ROM
image; the byte-pattern scan finds no raise-to-7 masking anywhere in the Cuda manager cluster) [3].
This matters because of what lives above level 3 on this platform: the PSC's DMA channels, the SCSI
and serial streams, the [DSP3210](../../hardware/dsp3210.md) — a Cuda driver that masked fully would
starve them for the length of every byte. The successor ROM tightens exactly one window — it raises
the mask to 7 for the few instructions around the first-byte TREQ sample (an ORI-to-SR at $FFC0C33C in
that image) and leaves the rest at 3 [6]. Nothing in the AV image does so.

The polled primitive of §4.3 is the other half of the same discipline: it must also work with
interrupts masked entirely, which is why it spins on the flag register instead of waiting [3].

### 4.7 The digitizer's I²C, from the driver's side

The video-input driver's bring-up — reachable only once the manager exists — configures the two
Philips parts through pseudo-command $22 (§3.10): full register shadow writes (25 bytes to the decoder,
17 to the display controller), then per-mode updates as the video-input pipeline changes standard or
window. Each transaction is an ordinary command packet with the slave address in the first parameter
byte; a write returns a bare acknowledgement, a read appends the data. There is no bulk mechanism, no
DMA to the I²C bus — every digitizer mode change is a sequence of byte-at-a-time transactions over the
same five-wire transport that carries the mouse [4] [6]. The chips themselves are specified at
[video digitizer](vdc.md).

### 4.8 Power-off and restart as the OS issues them

Shutdown and restart go through the command surface, not through the VIA: a shutdown request ends as
the power-down command ($0A), whose acknowledgement returns to the requester *before* the power drops —
the OS finishes its teardown with the completion in hand [6]. Restart is the $11 command: the
acknowledgement, then the reset pulse, and the machine re-enters the boot path of §4.1 with Cuda already
synchronized... except that it is not: the reset the command issues does not restart the *controller*,
whose synchronized state survives; the boot path runs the sync cycle again regardless, and a
well-behaved Cuda answers it from either state (*inferred — unverified* that the AV ROM re-syncs
unconditionally; the cycle is idempotent from the controller's side by construction, §3.5).

### 4.9 The traffic of a running session

Once booted, the wire is quiet and event-driven: one tick packet per second (§3.9), autopoll packets
when a device has data (§3.8), a read of the clock or a PRAM write when the OS asks, and the digitizer's
I²C sequences when video input is in use. A session's worth of Cuda traffic is therefore small — but
every byte of it crosses the same five-wire handshake with the same per-byte SR interrupt, and the
idle-acknowledge gap of §3.4 step 4 is load-bearing here too: a Cuda that delivered that trailing byte
synchronously — before the host's wait loop begins — would leave the manager hunting for an SR
interrupt that had already been consumed, mid-session and not just at boot.

## 5. Quirks & errata

- **The default clock date is 27 August 1956.** RESET seeds the seconds counter with $630BD178 =
  1956-08-27 20:35:04 in the Macintosh epoch [4] — a dead battery gives a machine stuck in the summer
  of 1956, and a re-implementation that seeds with zero (1904) is wrong in an observable way.
- **Twelve named commands must be rejected, not implemented.** The host-side interface of the era names
  37 opcodes; this firmware implements 25 and errors the rest with code 2 [4] (§3.7). A generous
  implementation that answers them breaks nothing — until software reads a value the real chip never
  provides.
- **PRAM is exactly one page.** A nonzero address high byte returns error code 4 [4] (§2.5); there is
  no 512-byte or paged extension on this part.
- **BYTEACK is a level, not a pulse.** Its change — relative to the shadowed previous level — is the
  signal, on both sides of the wire: the firmware spins until pin and shadow differ (§2.4) [4], and the
  host toggles rather than strobes [3]. Treating it as a one-shot loses every second byte.
- **TIP is active low, and Cuda aborts on seeing it high.** Mid-transfer, a high TIP is the abort
  condition, routed to the firmware's abort routine [4] (§3.12) — the polarity is the opposite of the
  predecessor controller's session line, and the ROM's Phase-B selector (§1.1) is the visible place
  where the two conventions meet.
- **Cuda is the shift-clock master.** One CB1 pulse per bit, both directions, MSB first, no inversion
  [4] (§3.1). The host cannot clock the bus at all; a host that tries to generate its own bit clock
  on CB1 is fighting the firmware's unrolled send/receive loops.
- **The last byte is the length.** Responses carry no length field; TREQ rising with the final byte
  *is* the end-of-packet marker [7] (§3.4). An implementation that drops TREQ early truncates, and one
  that holds it late makes the host read phantom bytes.
- **The idle acknowledge is a real byte that must arrive late.** After every terminated transaction
  Cuda clocks one extra byte, and the host waits for its SR interrupt before calling the bus idle —
  the sync cycle budgets a 15000-iteration loop for it and treats its absence as a dead chip [3]
  (§3.4, §3.5). A device that delivers Cuda-initiated bytes synchronously — before the host starts
  waiting — fails the sync cycle and kills the boot; a device that lets a stale one land inside the
  next transaction garbles the stream (§4.9).
- **Error packets are one byte longer.** Five header bytes against four for everything else, and the
  host reads the fifth only after seeing type $02 [3] (§3.6) — a four-byte error reply desynchronizes
  the manager's read logic.
- **The tick packet is two bytes on the wire.** Attention byte plus type, nothing else [6] (§3.9) —
  the shortest packet on the bus.
- **Direction changes can advance the bit counter.** Flipping ACR bit 4 can produce an internal edge
  in the 6522 shift logic; the host dummy-reads the SR after every direction change to re-sync, and
  waits out a small settle loop before clearing the SR interrupt on output [3] (§3.3). Dummy SR reads in
  odd places are protocol, not noise.
- **Transactions mask to interrupt level 3, no higher.** The send path's explicit ANDI-to-SR at
  $4080C410 [3] (§4.6) keeps the DMA/DSP/serial sources alive during Cuda traffic. The successor ROM
  raises one window to 7 [6]; the AV image never does.
- **A silent Cuda is a dead machine.** Sync-time timeouts branch to DeadCuda, error code $30, death
  chimes [3] (§3.5) — there is no boot path that skips the controller.
- **The device list is Cuda's, not the OS's.** Autopoll polls what the bitmap says, and the host must
  keep it current — including poking the firmware's own most/least-recently-used bytes through the
  write-MCU-address command when it addresses a device outside the bitmap [6] (§3.8).
- **The RTC has no write-protect.** Unlike the classic discrete clock chip (§3.9), nothing guards the
  seconds counter except the command surface; software that sends write-time has written time.
- **The clock crystal is documented in two lengths.** 0.032768 MHz in the AV note [1] p. 17, "0.0320"
  in the successor note [2] p. 20 — the same 32.768 kHz part, and the difference is rounding, not
  hardware.
- **The part number is known only from the firmware dump.** The successor board's schematic carries
  an unmarked "U46/CUDA" symbol [5] sheet 16 — nothing on the boards themselves names 341S0788.

## 6. Open questions

1. **The unreachable 8.1%.** 352 of the 4352 firmware bytes are not proven reachable by the
   disassembly's call-graph pass: a block at $1BAF–$1BEA and small runs between $1EFB and $1FD4,
   reachable (if at all) only through computed jumps; an address-table-like structure at $1FE5 may
   resolve some of them [4]. Until traced, no claim of "the firmware never does X" over that range is
   sound.
2. **PORTB bit 7.** An output at reset (DDRB=$92) with no identified function [4] — the only port-B
   output the transport does not use.
3. **PORTA's four outputs and PORTC bit 3.** The reset table's DDRA=$99 = %10011001 makes PORTA
   bits 0, 3, 4 and 7 the outputs [4]; what they drive on the AV boards is unknown — on the successor
   board, port A carries the soft-power and switch-sense lines [5] sheet 16, and the AV wiring is
   undocumented (§6.11). PORTC bit 3 is likewise the DDR-set output on port C ($08) with no identified
   load; on the successor board the reset pulse is a port C line.
4. **The host-side reserved port-B bits.** PB0–2 and PB6–7 are marked reserved in the ROM's port
   definitions and never touched by the Cuda manager (§2.3); their loads on the AV boards, if any,
   are unidentified.
5. **Error packet code 3.** Witnessed in the firmware's error path but with no condition established
   that produces it [4].
6. **The autopoll state machine and the $B3/$B4 housekeeping.** The firmware's polling loop was
   located but not analysed (rate granularity, service-request walk order) [4], and the
   most/least-recently-used poke behavior is established for the successor ROM's ADB layer [6], not
   pinned in the AV image.
7. **The I²C handler's internals.** The $192C handler's timing, error behavior and behavior for
   addresses beyond the two Philips parts are unanalysed [4]; whether any third device sits on the AV
   I²C bus is unknown.
8. **The core clock and the timing strap.** Whether the 32.768 kHz crystal clocks the 68HC05 core or
   a faster derivation does; the delay routines select between two scalings on a port-D strap bit
   [4] — which boards differ, and how, is unresolved.
9. **Which tick mode the AV ROM enables.** The clock-carrying (mode 3) versus bare tick forms are both
   handled host-side [6]; the disassembly here did not pin the value the AV image writes with the
   write-one-second-mode command.
10. **Host-side power-on latch values.** The reset values of the VIA emulation's own registers (and
    the port-B latch state before the first write) are unconstrained by the evidence; the ROM writes
    before reading, so software cannot tell (§2.6).
11. **The AV boards' power and reset wiring.** Which Cuda port pins drive the supply enable and the
    reset net on Cyclone/Tempest — established for the successor board [5] sheet 16, undocumented
    for these machines; no AV schematic is in the evidence set.
12. **The 68HC05 part's identity beyond the family.** The image disassembles cleanly as 68HC05 and the
    layout fits the family's 8 KB map [4], but no datasheet for Apple's part exists in the corpus —
    RAM size, port complement and electrical limits are inherited from the family, not verified.
13. **The parameter block at $0F38–$0F4D.** Sixteen bytes of ROM data, partly interpreted: it carries
    the command count (37, independently matching the dispatch bound) and three RAM addresses the
    firmware uses; the rest is undecoded [4].
14. **Whether anything on these machines sends the sound-control string ($0E).** The AV sound path is
    the PSC/Singer chain; the command is implemented in the firmware [4] but no AV-side sender was
    identified.
15. **The monostable reset command ($0D).** Implemented [4]; its effect on the 2.x firmware's state is
    not established from the disassembly alone.

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV
   Computers*, Developer Press, 1993 — §"Differences Between Models" p. 6 (replaceable battery
   powering the real-time clock and parameter RAM); §"Peripheral Subsystem Controller" p. 13 (the PSC
   handles all internal system interrupts and the VIA interrupt inputs); §"Cuda" p. 16 (the function
   list, GeoPort power control); Table 2-2 "System Clocks" p. 17 (CudaClk, 0.032768 MHz, crystal);
   §"External Device Interfaces" and Table 2-4 "ADB pin assignments" p. 21 (mini-DIN-4 socket; pin 2
   power-on sense, 470 Ω data pull-up, pin 3 +5 V); Glossary p. 478 ("Cuda").
2. Apple Computer, Inc., *Developer Note: Power Macintosh Computers* (Power Macintosh 6100/60,
   6100/60AV, 7100/66, 7100/66AV, 8100/80, 8100/80AV), Developer Press, March 1994 — §"Cuda
   Microcontroller Chip" p. 17 (the successor function list, "either GeoPort serial port"); Table 2-3
   clock frequencies p. 20 (Cuda, 0.0320 MHz crystal, "Cuda real-time clock"); Table 3-1 ADB pin
   assignments p. 27.
3. Macintosh Quadra 840AV / Macintosh Centris 660AV boot ROM, 2 MB image, checksum $5BF10FD1, mapped
   at $40800000 — disassembly: the start-up path (reset vectors to $4080002A → $40800074; Phase-B
   block at $4080F1D0); the system-management-controller selector at $4080F252–$4080F280 (the
   $01000000/$02000000/$03000000 three-way test; value 3 branches at $4080F27A to the sync cycle);
   the non-Cuda polled primitive at $4080E3C8; the VIA IER probes and $7F writes at
   $4080AD34E–$4080AD36E and the VIA1 port writes at $4080AD396–$4080AD3B4; the sync cycle at
   $4080C84C–$4080C8E8 (idle ORI.B #$30, 3333-iteration settle, TREQ/SR-flag tests at $1A00, dummy
   SR reads at $1400, three 15000-iteration timeouts, BCLR/BSET #4 BYTEACK strobe); the polled
   command primitive at $4080C8EA–$4080C9DC (budgets $2000/$0100 in `MOVE.L #$20000100,D4`, ACR
   bit-4 direction flips at $1600, TIP assert BCLR #5, error-packet fifth-byte test at $4080C9A2);
   DeadCuda at $4080C9DC (error code $30 into the failure path); the interrupt-driven byte-send path
   at $4080C3F6–$4080C43E (ten-iteration settle loop, ANDI-to-SR dropping the interrupt mask to
   level 3 at $4080C410); byte-pattern scans of the whole image for raise-the-mask instructions
   (none in the Cuda cluster).
4. Cuda firmware, Apple part 341S0788, self-identified "Cuda 2.37" — 68HC05 mask ROM, 4352 bytes
   ($1100), mapped at $0F00–$1FFF — annotated disassembly (recursive descent from the CPU vectors
   and the dispatch tables; 91.9% of the image proven reachable): copyright string 1989–93 at
   $0F00, version string at $0F4E, port/DDR init table at $0F57 (DDRA=$99, DDRB=$92, DDRC=$08);
   CPU vectors $1FF6–$1FFE (tick $1E72, timer $1E99, host interrupt $1E57, RESET $0F5E); RcvByte
   $1488 / SendByte $154B (unrolled, MSB first, non-inverted, one CB1 pulse per bit); AbortXact
   $15E1; packet layer with $B9/$BA at SendErrPkt $12A9 (codes 2, 3, 4); the pseudo-command
   dispatch at $12C4 (`CMP #$25`, ×3 index into the 37-entry jump table at $12E4, twelve slots to
   the error path); the EnDisPDM sub-table at $1B28; the RdWrIIC handler at $192C; the PRAM access
   stubs at $1808/$1827 (window $0100–$01FF, nonzero high byte → error 4); the RTC counter at
   $AB–$AE with the power-up/alarm compare at $AF–$B2 and the reset seed $630BD178
   (1956-08-27 20:35:04); the delay family at $1E18–$1E56 with the port-D strap branch; the
   parameter block at $0F38–$0F4D.
5. Apple Computer, Inc., Power Macintosh 8100 main-logic-board schematics, drawing 051-0333 rev A —
   sheet 16 "Cuda, Power Monitor & ADB" (U46, unmarked 28-pin SOIC; 32.768 kHz crystal on the
   oscillator pins; 3.6 V lithium cell with battery/trickle switchover; port A supply-enable line to
   the PSU connector; port C reset line into the system controller, paralleled by the front-panel
   reset button; ADB line driver and 470 Ω pull-up; keyboard, chassis and NMI switch inputs on the
   sense lines); sheet 18 (ADB mini-DIN-4 connector; the monitor connector's I²C pair routed to the
   controller).
6. Power Macintosh 6100/7100/8100 boot ROM, version $077D, header checksum $9FEB69B3 (March 1994) —
   disassembly of the Cuda manager cluster (dispatcher at $FFC0C2E0 with the level-7 mask window at
   $FFC0C33C; the sync cycle at $FFC0C7FA, matching [3]'s shape instruction-for-instruction;
   DeadCuda; the per-byte budgets), read together with the host-side interface of the era whose
   definitions match the AV firmware's dispatch bound (37), packet types, flags bits, response
   header, error codes, power-message selectors and ADB enumeration flow — the same interface, one
   ROM generation later.
7. Linux kernel source, *drivers/macintosh/via-cuda.c* (the published Cuda host driver for 68k and
   PowerPC Macintosh; GPL) — the PB3/PB4/PB5 line assignment with active-low TREQ ("TIP"), TACK and
   the level-toggled acknowledge; read termination on TREQ rising; the bus-initialization ("sync")
   sequence; the Macintosh-epoch conversion constant 2082844800.
