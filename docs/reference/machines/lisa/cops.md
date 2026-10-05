# COPS — the Lisa I/O microcontroller

**Contents:**

1. [Overview](#1-overview) — what the part is; the two COPS instances; the services it owns; the
   host channel through VIA1 port A; standby power
2. [Register file](#2-register-file) — no CPU-visible register file; the command byte; the response
   stream: key events, reset/status packets, keyboard identification, mouse packets; the CRDY line
3. [Behaviour](#3-behaviour) — the scan loop and command intake; response delivery and the CA1/CA2
   handshake; keyboard-side scanning and the serial keyboard link; mouse sensing; the real-time
   clock and timer; soft power and the power-on sequence; the keyboard-reset NMI
4. [Programming model](#4-programming-model) — boot-time bring-up and self-test; sending commands
   (Boot ROM and operating system); reading and setting the clock; the event-dispatch state
   machines; key codes and the Boot ROM ASCII table; NMI-key programming; mouse control and
   scaling; power off and power cycle
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The **COPS** is the Lisa's I/O microcontroller — a National Semiconductor
control-oriented processor system ("COPS" is the family's own name, expanded in the Hardware
Manual's glossary as "control-oriented processor system" [1] Glossary), a 4-bit single-chip
microcontroller. The Lisa's unit is the **COP421**, located at D-3 on sheet 2 of I/O-board
schematic 050-4008 [1] §6.6.2 p. 6-50; the Hardware Manual's bibliography refers the reader to the
*COPS421 User's Manual* for the chip's internal description [1] Preface, bibliography. It is a
*slave* processor: the 68000 never sees the COPS's instruction set, registers or memory. The entire
software contract is a one-byte **command** path into the COPS and a one-byte **response** stream
out of it, both carried through the A port of the keyboard 6522 VIA (VIA1, base `$00FCDD81`, port A
data at `$00FCDD83`; see [Lisa](lisa.md) §10.1 for the VIA wiring and [§3.3](lisa.md) for the I/O
address space).

The COPS services exactly four peripherals through that VIA port [1] §2.5.4 p. 2-18:

- the **keyboard** (it scans the keyboard link, reports key-down/key-up events, and detects the
  keyboard being unplugged and replugged);
- the **mouse** (it counts the mouse's motion pulses itself and hands the CPU *cooked signed
  deltas*, plus the button state and plug state);
- the **real-time clock** (a battery-backed clock/calendar with 0.1 s resolution and a programmable
  timer/alarm);
- **software power control** (the COPS can turn the machine on and off; it is the only part of the
  system that survives "off").

### 1.2 The two COPS instances

There are two COP421s in a Lisa, one at each end of the keyboard interface [1] Glossary, §6.6.2:

| Instance | Location | What it does |
|---|---|---|
| **I/O-board COPS** | I/O board, D-3 on sheet 2 of schematic 050-4008 [1] §6.6.2 p. 6-50 | Faces the 68000: receives commands and emits responses through VIA1 port A; maintains the clock/calendar; senses the power switch; drives the ON line; raises the keyboard-reset NMI |
| **Keyboard COPS** | Inside the keyboard assembly, C-1 on schematic 050-4001 [1] §8.2.1 p. 8-2 | Scans the key matrix through five 4067 analog multiplexers and ships key events serially to the I/O-board COPS over the keyboard cable |

"The COPS is identical to that found in the keyboard control logic on the I/O board... The same
routines are present in both devices. Only the routines that apply to the location of the device are
used" [1] §8.2.1 p. 8-2 — the two chips carry the same firmware and enable the half of it that
matches their socket. Both instances report through the I/O-board COPS's response stream: the
keyboard COPS's state is visible to the host only as keyboard events and the keyboard-identification
codes that follow a keyboard reset.

The chip is not unique to the Lisa's first release: the Macintosh XL (the 1984 re-badged Lisa 2/10)
keeps the same I/O board, the same two VIAs and the same COPS arrangement [2] "Internal VIA
locations"; the Hardware Manual's addresses apply to it with the errata's prefix correction (§5).

### 1.3 Host interface: the VIA1 port-A channel

Everything the 68000 exchanges with the COPS passes through one byte-wide bidirectional port. The
addresses and the VIA1 register file are documented in [Lisa](lisa.md) §10.1 and are not restated
here beyond what the COPS contract needs:

| Signal | Carrier | Direction (host view) | Function |
|---|---|---|---|
| Command / response byte | VIA1 port A (ORA/IRA `$00DD83`, ORA-no-handshake `$00DD9F`) | out and in | one byte each way; commands are written, responses are read with a handshake |
| `CRDY` | VIA1 PB6 (read at IRB `$00DD81`), wired to the COPS D3 line [1] §6.6.2 p. 6-50 | in | free-running ready/busy line from the COPS's scan loop (§3.1) |
| Data strobe | VIA1 CA1, wired to the COPS S0 line [1] §6.6.2 p. 6-50 | in | strobes each response byte into the port-A latch and raises the VIA1 interrupt |
| Acknowledge | VIA1 CA2, wired to the COPS S1 line [1] §6.6.2 p. 6-50 | out | pulses when the host reads a response byte; tells the COPS the byte was taken |
| Keyboard reset | VIA1 PB0 [1] §6.6.5 p. 6-52 | out | host-driven reset of the keyboard-side interface (§4.1) |

VIA1 aggregates to processor interrupt priority level **2** ([Lisa](lisa.md) §10.1); every response
byte the COPS has queued raises the level-2 interrupt through CA1, and the interrupt handler drains
the port (§4.5). The keyboard-reset NMI, by contrast, bypasses the VIA entirely: the COPS's G2 line
drives the system `NMI/` directly [1] §6.6.2 p. 6-50 (§3.7).

The other port-B lines of the keyboard VIA (speaker volume on PB1–PB3, floppy-disk interrupt on
PB4, parallel-port reset sense on PB5 and `CRES/` on PB7) belong to the VIA, not to the COPS — see
[Lisa](lisa.md) §10.1 and [1] §6.6.5 pp. 6-52..6-53.

### 1.4 Power: why the COPS is always running

The COPS runs from the machine's standby supply, not from the switched main supply: "The COPS
receives power from the backup supply. This voltage is available at all times, whether the Lisa is
powered down or even unplugged. The only time the COPS ceases functioning is if the battery is
allowed to run down by having the Lisa unplugged over a long period" [1] §2.5.4 p. 2-20. This is
what makes a *real* real-time clock and software-controlled power-up possible [1] Glossary, "RTC".

The supply chain has two stages [1] §10, §6.7.2:

1. **`+5 VSTBY`** — a linear standby supply inside the power supply (transformer T2, rectifiers,
   5 V regulator U1), "not switched off by the on-off signal" and of deliberately low current
   capacity [1] §10 p. 10-1, §10.5.1 p. 10-10.
2. **`+5 V` battery-backed rail on the I/O board** — a rechargeable nickel/cadmium battery behind
   a TL4193 switching regulator, which carries the rail when the machine is unplugged. The battery
   recharges whenever the Lisa is on, "is capable of running the COPS for 10 hours without loss of
   data", and the regulator disconnects the load at 4.5 V "to avoid cell reversal, which can destroy
   nickel/cadmium battery cells" [1] §6.7.2 pp. 6-54..6-55.

The practical consequences for software are concrete: the clock keeps running across a soft
power-off indefinitely (as long as the machine stays plugged in), for about 10 hours across an
unplug [1] §8.4.1 p. 8-6, and the COPS retains its programmed state — the timer/alarm that will
wake the machine (§3.6) and the programmed NMI key (§3.7) — across all of it.

## 2. Register file

### 2.1 There is no CPU-visible register file

The COP421's internal registers, RAM and ROM are not in the 68000's address space; the chip is
documented only through its behavior [1] §6.6.2 p. 6-50. What *is* register-like about the COPS, and
what a re-implementation must get bit-exact, is the **byte-level protocol on VIA1 port A**: the
command byte the host writes, the response bytes the COPS returns, and the state each side must
track to tell them apart. Those are specified here as tables.

### 2.2 The command byte

Every host→COPS transaction is a single byte written to port A, with the top nibble selecting the
command [1] §2.5.4 p. 2-20, Figure 2-14:

| Bits `7654 3210` | Command | Effect |
|---|---|---|
| `0000 0000` (`$00`) | Turn I/O port on | Issued by the Boot ROM at bring-up (`TURNON`) [3] RM248.K |
| `0000 0001` (`$01`) | Turn I/O port off | Complement of the above; not issued by any released software in the evidence set |
| `0000 0010` (`$02`) | Read clock data | COPS returns a 7-byte clock packet (§2.4, §3.5) |
| `0001 nnnn` (`$1n`) | Write nibble `nnnn` to the clock | One digit of a clock/alarm write, sent MSB-first while clock-set mode is enabled (§3.5, §4.4) |
| `0010 spmm` | Set clock modes | `s` = enable (1) / disable (0) clock-set mode; `p` = power on (1) / off (0); `mm`: `00` clock and timer disable, `01` timer disable, `10` timer-underflow interrupt, `11` timer-underflow power-on [1] Figure 2-14 |
| `0101 nnnn` (`$5n`) | Set NMI-key high nibble to `nnnn` | Upper bits of the keycode that will raise the keyboard-reset NMI (§3.7, §4.8) |
| `0110 nnnn` (`$6n`) | Set NMI-key low nibble to `nnnn` | Lower bits of the NMI keycode |
| `0111 ennn` (`$7n`) | Mouse control | `e` = enable (1) / disable (0) mouse reports; `nnn` = report interval in units of 4 ms, 0..7 [1] §2.5.4 p. 2-24 |
| `1xxx xxxx` | No operation | Any byte with bit 7 set is ignored by the COPS [1] Figure 2-14 |

The `$2n` mode byte is the soft-power and clock-mode command; the released software's entire
vocabulary of it is decoded in §3.6 Table 6. Note that *every* nibble of a clock setting is sent as
a separate `$1n` command byte, so setting the clock is 16 full command handshakes plus the bracketing
mode commands (§4.4).

### 2.3 Response stream: key events

A response byte with neither the reset lead-in nor the mouse marker is a **key event**. The format
is `d rrr nnnn` [1] §2.5.4 pp. 2-20..2-21, Figure 2-15: bit 7 `d` is the direction (1 = key down,
0 = key up) and the low 7 bits `rrr nnnn` select the key from the scan matrix. The Lisa OS defines
keycode values 0..127 [5] libhw-LEGENDS.TEXT; the low-space codes are not keyboard keys at all but
system events injected on the same channel [5] libhw-KEYBD.TEXT:

| Code | Event | Down/up forms |
|---|---|---|
| `$01`/`$81` | Disk 1 inserted / removed | bit 7 = inserted |
| `$02`/`$82` | Disk 1 button | bit 7 = button down |
| `$03`/`$83` | Disk 2 inserted / removed | bit 7 = inserted |
| `$04`/`$84` | Disk 2 button | bit 7 = button down |
| `$05`/`$85` | Parallel port (line state) | bit 7 = asserted |
| `$06`/`$86` | **Mouse button** | bit 7 = button down [1] §2.5.4 p. 2-24 |
| `$07`/`$87` | **Mouse plug** | `$87` = mouse plugged in, `$07` = unplugged [1] §2.5.4 p. 2-24 |
| `$08`/`$88` | **Power button** | bit 7 = pressed |

The keyboard proper occupies codes `$20`–`$7F`; the full code-to-key assignment is given in §4.7.

### 2.4 Response stream: reset and status packets

A `$80` byte is the **reset lead-in**: it announces that the next byte is a status code, not a key
event [1] §2.5.4 p. 2-22, Figure 2-16 p. 2-23. Each code is itself followed by further bytes where
the table says so:

| Lead-in + code | Meaning | Followed by |
|---|---|---|
| `$80 $FF` | Keyboard COPS failure detected ("keyboard COPS RAM error" [5] libhw-KEYBD.TEXT equates) | nothing |
| `$80 $FE` | I/O-board COPS failure detected ("I/O board COPS RAM error" [5] libhw-KEYBD.TEXT equates) | nothing |
| `$80 $FD` | Keyboard unplugged | when the keyboard is plugged back in: another `$80` lead-in and the keyboard identification code [1] Figure 2-16 |
| `$80 $FC` | Clock timer interrupt (timer underflow with interrupt mode armed) | nothing |
| `$80 $FB` | **Soft power-off switch pressed** while the machine is on | nothing; software is expected to shut down (§3.6) |
| `$80 $F0`–`$FA` | Reserved for future use | nothing |
| `$80 $Ey` | Clock data follows; `y` is the year nibble | exactly five packed-BCD bytes (§3.5) |
| `$80 id` (`$00`–`$DF`) | Keyboard identification number, "produced whenever the keyboard COPS is reset" [1] Figure 2-16 | nothing |

The ID range follows from the dispatch code of every consumer of the stream: both the Boot ROM's
and the operating system's handlers treat any byte ≤ `$DF` after a lead-in as a keyboard ID ([3]
RM248.M `WT4INPUT`; [5] LIBHW-DRIVERS.TEXT `COPS` state 4).

### 2.5 Response stream: keyboard identification

The keyboard ID byte encodes three attributes [5] libhw-LEGENDS.TEXT, "Keyboard identification":

| Bits | Field | Values |
|---|---|---|
| 7–6 | Manufacturer | `00` TKC, `10` Keytronics |
| 5–4 | Physical layout | `00` Old US (73 keys), `10` European (77 keys), `11` Final US (76 keys) |
| 5–0 | Layout/legends code (layout bits + legends nibble) | `$0F` Old US, `$24` French-Canadian, `$25` Dutch, `$26` Swiss-German, `$27` Swiss-French, `$29` Spanish/Latin-American, `$2A` Danish, `$2B` Swedish, `$2C` Italian, `$2D` French, `$2E` German, `$2F` UK, `$3E` US-Dvorak (proposed), `$3F` Final US; `$FF` is the default table selector |

The operating system masks the ID with `$3F` to pick a legend table and re-establishes the entire
keyboard mapping on every new ID [5] libhw-KEYBD.TEXT `KeyId`. The Final US layout is the 76-key
keyboard of the Hardware Manual's Figure 8-1 [1] §8.2 p. 8-2; the Old US layout has three fewer keys
(no `|\`, no Alpha-Enter, no Right-Option) and two keys that generate different codes, which
software remaps for compatibility [5] libhw-LEGENDS.TEXT (§4.7).

### 2.6 Response stream: mouse packets

Mouse motion is reported as a **three-byte packet** [1] §2.5.4 p. 2-24:

| Byte | Value | Meaning |
|---|---|---|
| 0 | `$00` | "mouse data follows" marker — also the signal that the next two bytes are deltas |
| 1 | `dx` | signed change in X, −127..+127 |
| 2 | `dy` | signed change in Y, −127..+127 |

"Each time the value is accepted by the CPU, the bytes are reset. Should the CPU not respond
immediately, the data are updated to show a cumulative value" [1] §2.5.4 p. 2-24 — the COPS
accumulates motion in its own counters until the host takes a packet, so a slow host never loses
motion, it just gets it coarser. The button and plug state ride the key-event channel instead
(`$06`/`$86` and `$07`/`$87`, §2.3). Packets are only produced when mouse reports are enabled
(`$7n` command, §2.2) and the mouse has moved within the report interval.

### 2.7 The CRDY line

`CRDY` is the COPS's free-running ready/busy output, readable by the host as bit 6 of VIA1 port B
(`$00DD81`) [3] RM248.K `COPSCMD`, [1] §6.6.2 p. 6-50. It is **not** a static level: the COPS
toggles it continuously as it cycles through its internal scan loop, and a sender must synchronize
to its *edges* (§3.1). A Boot ROM comment defines the polarity used by all the released senders:
CRDY **low = ready**, high = not-ready [3] RM248.K (`wait for "ready" (bit 6 = CRDY)`). This line and
its free-running behavior are the single most implementation-critical fact of the COPS interface: a
host that waits for a CRDY *level* change that never comes hangs forever, and both the Boot ROM and
MacWorks XL synchronize to edges ([Lisa](lisa.md) §11, *observed* — the free-running behavior is
established from the senders' edge-waiting code and recorded in lisa.md §11).

## 3. Behaviour

### 3.1 The scan loop and command intake

The COPS runs a continuous internal scan loop — polling the keyboard link, sampling the mouse,
advancing the clock — and the VIA port is serviced as part of that loop. That single fact explains
every timing property of the interface:

- **Commands are taken only at scan-loop boundaries.** The host cannot interrupt the COPS; it must
  present the command byte on port A at a moment the COPS is about to read the port, which it
  advertises by driving CRDY into its ready phase.
- **Responses are delivered when the COPS has them**, at the COPS's own pace, one byte per port
  visit, each strobed on CA1.
- **Multi-byte operations are multi-command operations.** Sixteen clock digits are sixteen
  separately handshaked command bytes (§4.4); a mouse packet is three separately strobed response
  bytes (§4.5).

The exact send sequence, as performed by the Boot ROM's `COPSCMD` [3] RM248.K ([4] `$0956`), is:

1. Preload the command byte into the port-A output register *without* handshake (offset 15,
   `$00DD9F`) — the pins do not change yet because DDRA still says input.
2. Wait for CRDY to go low (ready), with a timeout of about 10 ms (loop count `$061A`, each
   iteration about 32 cycles = 6.4 µs plus up to 2 µs of 6522 bus synchronization).
3. Kill about 15.2 µs (`MULU #1`) to make sure the ready phase just seen is spent.
4. Wait for CRDY to go low **again** — the second ready phase guarantees enough time remains in it
   to complete the transfer.
5. Jam `DDRA = $FF`, driving the preloaded command onto the port-A pins.
6. Wait for CRDY to go high (the COPS has left the ready phase — it is now consuming the byte).
7. Hold the data through a short fixed delay (§6.10), release the port (`DDRA = $00`), and enable
   the CA1 interrupt (`IER1 = $82`) so the response, if any, can be received.

If any of the three edge waits times out, `COPSCMD` returns with the carry bit set; every caller in
the ROM treats that as a hard COPS failure [3] RM248.K. The operating system's driver performs the
same dance with its own twist: it writes the command byte to the *handshake* register (offset 1),
and it opens a brief interrupt-enable window between CRDY phases so that level-2 interrupts are not
starved during the wait; the routine carries the warning "This code synchronizes with the 6522, and
is instruction timing sensitive. Modify will great care!" [5] LIBHW-DRIVERS.TEXT `COPSCMD`. The OS
driver then spins ten iterations with the port driven before releasing it — the same short hold as
the ROM's.

### 3.2 Response delivery: the CA1 strobe and CA2 acknowledge

In the response direction the COPS is the master of port A and the host's DDRA stays `$00`
(inputs). For each byte the COPS has queued:

1. The COPS drives the byte onto the port-A pins and raises its S0 line, wired to **CA1** [1]
   §6.6.2 p. 6-50. The host configures CA1 for positive-edge latching and enables port-A input
   latching (`PCR1` bit 0 set, `ACR1` = `$01` [3] RM248.K `CPSINIT`; the OS sets `PCR = $C9`
   outright [5] LIBHW-DRIVERS.TEXT `DriverInit`), so the byte is captured into IRA on the edge.
2. The CA1 active edge sets the VIA's CA1 flag (IFR bit 1) and, with IER bit 1 enabled, raises the
   **level-2 interrupt** ([Lisa](lisa.md) §10.1).
3. The interrupt handler reads the byte from the port-A register *with* handshake (offset 1,
   `$00DD83`); that access pulses **CA2** — wired to the COPS's S1 [1] §6.6.2 p. 6-50 — which tells
   the COPS the byte was consumed.
4. The COPS presents the next queued byte on its next port visit.

Because the byte is latched in IRA by hardware, the handler does not race the COPS for the data —
but it must run promptly: the operating system documents that "keyboard and mouse input are
disabled if level 2 interrupts are disabled" for more than about 10 ms, and offers a `Poll` routine
for software that runs with interrupts off [5] LIBHW-DRIVERS.TEXT, "Poll". Whether an unacknowledged
byte is dropped, overwritten or held by the COPS is not documented (§6.3).

### 3.3 Keyboard-side scanning and the serial keyboard link

The keyboard COPS scans a 76-key matrix (Final US layout; 73 keys Old US, 77 European [5]
libhw-LEGENDS.TEXT) built from **five 4067 16-to-1 analog multiplexers** in an upper and lower bank.
The COPS's SK output, through an LS03 gate, clocks the banks; its D0–D3 outputs select which of each
device's sixteen switches is being read; the switch states come back on G1–G3 — the lower bank uses
all three lines, the upper bank only G2 and G3 [1] §8.2.1 pp. 8-2..8-3. The scan is what makes the
keyboard a "true N-key rollover design. An arbitrary number of keys can be depressed without
causing phantom key problems" [1] §2.5.4 p. 2-20; the host must interpret modifier state and
auto-repeat itself, in software [1] §2.5.4 p. 2-22.

The link from keyboard to I/O board is a three-wire shielded cable from a 3-pin Molex connector on
the keyboard PCB to a standard 1/4 inch stereo phone jack on the front of the cabinet [1] §8.2 p.
8-1, §8.2.3 p. 8-3. The transfer is *initiated by the I/O-board COPS*, on demand:

1. The I/O-board COPS pulls the keyboard-data line low for approximately **20 microseconds** — the
   SYNC pulse — by asserting its SK output together with D2 [1] §6.6.3 p. 6-51.
2. The keyboard COPS senses the leading and trailing edges of the SYNC pulse and answers with an
   ACK pulse on the line (into its G0 input path); the data byte then follows serially, a lower and
   an upper nibble [1] §8.2.2 p. 8-3, Figure 6-13 timing.
3. "If no data are present in the keyboard COPS, no ACK pulse is sent. The interface becomes
   quiescent until the next SYNC pulse" [1] §8.2.2 p. 8-3.

Key data travels in serial form from the keyboard COPS's S0 output through the LS03 gates to pin 1
of the Molex connector; the same pin carries the Lisa's SYNC pulses inbound to the keyboard COPS's
G0 input [1] §8.2.1 p. 8-3. The keyboard-side COPS reports its own health through the same reset
codes as the I/O-board unit (`$FF` keyboard COPS failure), and emits its identification byte
whenever it is reset (§2.4) — which is how the host learns the keyboard model and layout at
power-on and at every hot replug.

### 3.4 Mouse motion and button sensing

The mouse is an electromechanical unit — a rolling ball, two directional wheels and one button —
on a cable to a 9-pin DB connector on the motherboard [1] §8.3 p. 8-4a. Unlike later
Macintoshes, where the CPU counts raw quadrature itself, on the Lisa the mouse belongs to the COPS:
"The mouse connects to the COPS" [1] §2.5.4 p. 2-24. The two direction wheels' pulse lines and the
switch lines reach the COPS through an **LS153 dual 4-to-1 multiplexer**, selected by the COPS's D1
and D2 outputs; "The COPS polls the signal states by reading the signals input to the multiplexer.
The movement of the mouse is detected by pulse edges on the relevant direction lines" [1] §6.6.3 p.
6-51 — the COPS does the edge counting internally. Three switch inputs `SW0`–`SW2` are provided;
"in the current Lisa only one mouse switch is present and is connected to the SW0 line" [1]
§6.6.3 p. 6-51.

The COPS converts counted edges into the signed `dx`/`dy` of §2.6 and accumulates between host
reads. Reports are gated by the `$7n` command: with `e` set, motion produces a three-byte packet
every `nnn` × 4 ms of movement [1] §2.5.4 p. 2-24; the operating system clamps the interval to
0..28 ms and initializes it to 16 ms [5] libhw-MOUSE.TEXT. The button is *not* part of the packet —
it is a key event (`$06`/`$86`), delivered the instant it changes.

### 3.5 Real-time clock and timer

The clock/calendar is implemented in the COPS — "a function implemented on I/O board by the COPS
device. Since power is always available to this device, it is used to calculate the real elapsed
time at all times" [1] Glossary, "RTC". Its resolution is 0.1 s and it "need be reset only every 16
years" [1] §2.5.4 p. 2-24.

**Digit layout.** The clock state is a string of digits written and read one nibble at a time. The
write format, in clock-set mode, is [1] §2.5.4 p. 2-25:

| Nibbles | Field | Range / encoding |
|---|---|---|
| `aaaaa` (5) | timer delay value in seconds | binary, 0..`$FFFFF` (1,048,575 s ≈ 12 days) |
| `y` (1) | year | binary, 0..15, anchored at **1980 = 0** [5] libhw-TIMERS.TEXT |
| `ddd` (3) | day of year | BCD, 1..366 |
| `hh` (2) | hour | BCD, 0..23 |
| `mm` (2) | minute | BCD, 0..59 |
| `ss` (2) | second | BCD, 0..59 |
| `t` (1) | tenths of a second | BCD, 0..9 |

— sixteen nibbles in all, "all... maintained in binary coded decimal (BCD) format, except for the
timer and year nibbles, which are binary" [1] §2.5.4 p. 2-25. The read format (the `$Ey` packet of
§2.4) is the same digits with the year nibble folded into the lead-in byte: `$80 $Ey` followed by
`dd dh hm ms st` [1] §2.5.4 p. 2-25. The operating system packs the six bytes into one value
`0000yyyy dddddddd ddddhhhh hhhhmmmm mmmmssss sssstttt` and treats the all-ones pattern
`$0FFF FFFF FFFF` as "clock never set since loss of battery power", converting it to 1 January 1980
00:00:00 [5] libhw-TIMERS.TEXT, "Clock".

**Set mode.** "In Clock Set Mode, only as many nibbles as are required need be sent. Once the `s`
bit is cleared, the peripheral assumes that the data was complete. The clock and timer must be
stopped while the clock is being set. The clock can be left running while setting up the timer"
[1] §2.5.4 p. 2-25 — that is the `mm` field's purpose: `00` stops both, `01` stops only the timer
(§2.2). Nibbles are written MSB-first, each as one `$1n` command ([3] RM248.B `TODSET`; [5]
libhw-TIMERS.TEXT `SetClock`).

**The timer/alarm.** The five `a` digits are a countdown that starts when the timer is armed. At
underflow the `mm` mode decides the consequence: `10` raises the `$80 $FC` clock-timer-interrupt
reset code, `11` powers the machine on [1] Figure 2-14, §2.5.4 p. 2-24. "An alarm can be programmed
via the RTC to generate an interrupt and/or turn the Lisa on after a timeout of up to `FFFFF`
seconds, or about 12 days" [1] §2.5.4 p. 2-24. The alarm can only turn the machine *on* — "The RTC
is not capable of turning the computer off" [1] §6.6.4 p. 6-52.

### 3.6 Soft power and the power-on sequence

Power to the whole machine other than the standby rail is *software's* to give and take: "Software
controls the on and off states in the Lisa" [1] §2.5.4 p. 2-25.

**Off → on.** The COPS senses the front-panel on-off switch through the `PWRSW/` signal (its G3
line — "used to sense the state of the on-off switch" [1] §6.6.2 p. 6-50) and can itself drive the
supply's ON line (its D0 line — "used to switch the Lisa on and off under control of firmware
resident in the COPS" [1] §6.6.2 p. 6-50; "The COPS can turn the Lisa on or off, via the ON line"
[1] §6.6.4 p. 6-52). At the supply end the ON signal gates the base of transistor Q5, which lights
the opto-isolator that switches the main supply; a logic 1 there turns the supply on and it "stays
on until this level is removed" [1] §10.2.2 p. 10-4. Pressing the switch while off therefore boots
the machine: the COPS turns on the supply, the 68000 comes out of reset, and the Boot ROM brings up
the COPS interface (§4.1). There is a second, purely electrical path onto the same rail: "pulling
the RESET/ signal high when the Lisa is turned off pulls the PWRSW/ signal low and turns the Lisa
on. Note that this can be also accomplished by pulling RESET/ to +5STBY with a 100 ohm resistor"
[1] §6.6.4 p. 6-52. The RTC wake (§3.5, `mm = 11`) uses the same ON line.

**On → off.** Pressing the switch while the machine is running does *not* remove power — it
delivers the `$80 $FB` reset code, "which allows software to finish operations in progress and
store work files before turning the Lisa off. Note that pressing the on-off switch does not remove
power from the Lisa; power can only be removed by unplugging the line cord" [1] §2.5.4 p. 2-25. The
shut-down path is then entirely in software's hands and ends with a `0010 spmm` command carrying
`p = 0`, plus whatever mode the clock should be left in. The released software's complete vocabulary
of mode commands, decoded against §2.2, is:

| Byte | `s` | `p` | `mm` | Meaning as used by the software | Issued by |
|---|---|---|---|---|---|
| `$20` | 0 | 0 | 00 | power off, clock and timer stopped | OS `PowerDown` when the clock was never set [5] libhw-MACHINE.TEXT |
| `$21` | 0 | 0 | 01 | power off, timer stopped, clock left running | Boot ROM `PowerOff` [3] RM248.M; OS `PowerDown` otherwise [5] libhw-MACHINE.TEXT |
| `$23` | 0 | 0 | 11 | power off **now**, timer armed to power back on at alarm underflow | OS `PowerCycle` [5] libhw-MACHINE.TEXT; burn-in power-cycle loop [3] RM248.B |
| `$25` | 0 | 1 | 01 | stay powered, leave clock-set mode with the timer stopped | finishes every clock set [3] RM248.B; [5] libhw-TIMERS.TEXT |
| `$2C` | 1 | 1 | 00 | enter clock-set mode, clock and timer stopped, stay powered | before a 16-nibble clock write [3] RM248.B; [5] libhw-TIMERS.TEXT |
| `$2D` | 1 | 1 | 01 | enter clock-set mode to write the five timer digits, timer stopped, stay powered | before an alarm write [3] RM248.B; [5] libhw-MACHINE.TEXT |

Note the `p` bit in every one of these: `1` keeps the machine on while the mode is being changed,
`0` is the actual power-off request. A shutdown that needs the timer armed (power-cycling, §4.10)
must therefore write the alarm digits *first*, in clock-set mode with the power still on, and only
then issue the `p = 0, mm = 11` byte that both kills the machine and arms the wake.

**After the command.** Power does not fall instantly: the Boot ROM waits a fixed 1.7 s
(`KBDDLY = ONESEC×17/10` [3] RM248.E) after its `$21` before concluding the machine refused to die
[3] RM248.M `PowerOff`; the operating system simply "coasts" in a long countdown loop and retries
the whole sequence if the machine is somehow still running [5] libhw-MACHINE.TEXT `PowerDown`. The
COPS itself keeps running through all of it on the standby rail.

### 3.7 The keyboard-reset NMI and the power-fail input

The COPS's G2 line does double duty: it "is used to interrupt the processor by means of the `NMI/`,
non-maskable interrupt, at B-2 on sheet 2... and to detect a power failure via D5 at B-2 on sheet 2"
[1] §6.6.2 p. 6-50. The interrupt use is the **programmable NMI key**: any key on the keyboard can
be nominated, by sending its keycode in two nibbles with the `$5n`/`$6n` commands, so that pressing
that key raises the keyboard-reset NMI instead of a key event [1] §2.5.4 pp. 2-20, 2-22. Nominating
keycode `$00` disables the feature [5] libhw-KEYBD.TEXT "SetNMIKey". The Boot ROM deliberately
disables the NMI key at power-up (§4.1) and the operating system re-programs it when asked (§4.8).
Because the NMI arrives outside the VIA, it can interrupt a COPS command *in progress* — the OS
driver keeps a transaction counter for exactly this case (§4.3).

The power-fail sense on the same pin has no documented software path in the evidence set (§6.8).

## 4. Programming model

This section records the sequences the shipped software actually performs, in the order it
performs them. Addresses of Boot ROM routines are cited from the assembled listing [4].

### 4.1 Boot-time bring-up and self-test

Early in the power-up test sequence, after the VIAs themselves have been verified, the Boot ROM
(revision 2H [3] RM248.E header) brings the COPS up in three steps [3] RM248.K:

1. **Interface init** (`CPSINIT`, [4] `$0920`): with VIA1 base `$00FCDD81` — program port-A input
   latching (`ACR1 = $01`), set the handshake modes (`PCR1 |= $09`: CA1 positive edge, CA2
   handshake), clear and disable all VIA1 interrupts (`IER1 = $7F`, `IFR1 = $7F`).
2. **Turn the COPS on** (`TURNON`): send `$00` (turn I/O port on); send `$70` (disable mouse
   reports); send `$50` and `$60` (NMI-key high and low nibbles both zero — NMI key disabled). A
   timeout at any of these sets the COPS error flag and aborts further testing [3] RM248.K.
3. **Reset scan** (`RSTSCAN`, [4] `$09C2`): first drain any pending response bytes from the COPS
   queue; then assert the keyboard reset — VIA1 port B bit 0 low for 12 ms (`RSTKBD`, [4] `$0AAA`)
   — and run a state machine over the codes that come back, to classify what is attached. The
   expected sequence after the reset pulse is the keyboard replug pattern: `$80 $FD` (unplugged)
   followed by `$80 <id>` (the keyboard COPS, coming out of reset, announces its identification).
   The scanner records: keyboard-COPS failure (`$FF`), I/O-board-COPS failure (`$FE`), keyboard
   connected (disconnect + connect + ID seen), keyboard absent (nothing at all), mouse
   disconnected (a `$87` followed by `$07`) [3] RM248.K. The keyboard reset is released afterwards
   with a 1.7 s settle (`CLRRST`, `KBDDELAY` [4] `$0ADC`) — "delay for normal COPS power-up time of
   about 1.7 seconds" [3] RM248.K. The keyboard ID is saved at low-memory `$01B2`, `$00` meaning
   "no ID received" [3] RM248.E.

Because the machine can also have been RTC-woken (§3.6), the same sequence doubles as the
"machine just appeared" path: the ROM's power-up status word at `$180`–`183` carries the COPS and
keyboard results forward for the OS to read [3] RM248.E.

The operating system's `DriverInit` then takes the interface over: port A to inputs, port B
direction preserved on bits 5 and 7, `PCR = $C9`, `ACR = $01`, `IER = $82` (enable the COPS's CA1
interrupt), send `$7C` (mouse reports on, 16 ms), install the level-2 handler, and finally **drain
the 16-byte keyboard queue the Boot ROM left behind** — "flush... to eliminate keys typed to ROM"
— replaying each queued byte through the OS's own COPS handler and then forcing the parser state
machine to its idle state [5] LIBHW-DRIVERS.TEXT `DriverInit`. If no keyboard ID was seen at all,
the OS assumes `$0F` (Old US) and calls its own `KeyId` [5] LIBHW-DRIVERS.TEXT.

### 4.2 Sending a command: the Boot ROM's `COPSCMD`

The routine is the reference send sequence of §3.1, called from a table of entry points also used
by the ROM monitor (`JMP COPSCMD`, [4] `$00A8`). Its contract [3] RM248.K:

- **Input:** D0 = command byte. **Output:** carry set on timeout (~10 ms per CRDY phase).
- Interrupts are hard-disabled during the sequence (the ROM raises the IPL to 7), so a send never
  collides with a response interrupt.
- The command byte is staged through the no-handshake port-A register (offset 15) so that no CA2
  pulse escapes before the port is actually driven.
- On success, CA1 interrupts are (re-)enabled before returning, so the response path is live.

### 4.3 Sending a command: the OS driver's `COPSCMD` and the transaction counter

The Lisa OS's driver keeps the same CRDY-edge protocol but adds two layers of robustness [5]
LIBHW-DRIVERS.TEXT:

- **Interrupt windows.** The driver runs at a reduced interrupt mask rather than IPL 7, briefly
  re-enabling interrupts between CRDY phases (a `<05Jan84>` change) so that level-2 response
  interrupts are not lost during long waits; it also unrolls a long sequence of CRDY bit tests so
  that the common case — the COPS becoming ready — exits quickly.
- **Transaction counter.** All multi-byte COPS conversations (a 16-nibble clock set, a 5-nibble
  alarm set) are wrapped in a `COPSCounter` read/compare: if an NMI lands mid-conversation — the
  programmable NMI key of §3.7 fires with interrupts off, and "Output to the COPS which has been
  interrupted by NMI is sent to the COPS again after the NMI handler has completed" ([5]
  LIBHW-DRIVERS.TEXT "NMISync") — the counter no longer matches and the whole conversation restarts.

Borrowers of the COPS (diagnostics that steal the level-2 vector) must call `COPSSync` and may only
hand the stream back **between packets** — "the COPS input stream must be between packets when it
is returned to the hardware interface" — because the driver interprets every byte in the context of
the previous one [5] LIBHW-DRIVERS.TEXT "COPSSync".

### 4.4 Reading and setting the clock

**Reading** (`READCLK` [3] RM248.S, [4] `$12A0`): with interrupts disabled, send `$02`, then wait
for the `$80` lead-in byte (discarding anything else), wait for the `$Ey` byte (`$E0`–`$EF`), then
read exactly five more bytes. Seven bytes total, `$80 Ey dd dh hm ms st`. A timeout anywhere sets
the clock error bit. The operating system's `Clock` routine does the same from the interrupt
handler side: it sends `$02`, then spins with interrupts enabled, letting the level-2 handler
collect the packet into `ClockHigh`/`ClockLow` bytes and set a `ClockReady` flag, up to a 20000-iteration
timeout, after which it beeps "then use[s] old clock value" [5] libhw-TIMERS.TEXT.

**Setting** (`SetClock` [5] libhw-TIMERS.TEXT; the ROM's burn-in code [3] RM248.B `TODSET` [4]
`$2466` is identical in shape): send `$2C` (enter clock-set mode, both counters stopped); send
sixteen `$1n` nibble commands, MSB-first — the OS deliberately zeroes the five alarm digits as it
goes; send `$25` (leave set mode, timer stopped, machine stays on). The ROM's burn-in
power-cycling code demonstrates the alarm half: `$2C`, eight nibbles of `SET1` (zero — alarm =
0 seconds), eight nibbles of `SET2` (`$10000000` — "day=01, all other values=0"), `$25` [3] RM248.B.

**Never-set detection.** The OS's `Clock` treats `ClockHigh = $0FFF` as "clock initialized?" no, and
`ClockToDate` converts the all-ones pattern to 1980, day 1, 00:00:00.0 [5] libhw-TIMERS.TEXT. A
consequence, recorded in [Lisa](lisa.md) §11.5: the year nibble anchors 1980–1995, so a present-day
date cannot be represented, and the OS's `PowerCycle` (§4.10) explicitly refuses to schedule a
reboot when the clock reads never-set [5] libhw-MACHINE.TEXT.

### 4.5 Event dispatch: the operating system's Level-2 handler

Every CA1 interrupt lands in `Level2`, which checks the VIA's CA1 flag and calls `COPS` — a
five-state parser over the response stream [5] LIBHW-DRIVERS.TEXT. One byte is taken per interrupt;
the state says what the byte means:

| State | Waiting for | Byte taken means | Next |
|---|---|---|---|
| 0 — normal wait | anything | `$00` → mouse marker; `$80` → reset lead-in; anything else → key event: bits split into keycode (`$7F`) and direction (`$80`), Old-US keycodes remapped, user KEY routine called | 1 / 4 / 0 |
| 1 — mouse Dx | first delta | `dx` saved | 2 |
| 2 — mouse Dy | second delta | `dy` saved; mouse-movement routine called with both deltas | 0 |
| 3 — clock bytes | packet payload | byte shifted into `ClockHigh`/`ClockLow`; after the fifth, `ClockReady` set | 0 |
| 4 — reset code | status byte | dispatched per the table below | 0 |

State 4's dispatch table [5] LIBHW-DRIVERS.TEXT:

| Code after `$80` | OS action |
|---|---|
| `$00`–`$DF` | keyboard ID: save it, clear the key bitmap (all keys up), clear shift state, re-establish legends |
| `$E0`–`$EF` | clock data: save year nibble, expect five more bytes (state 3) |
| `$F0`–`$FA` | reserved: ignored |
| `$FC` | clock timer interrupt: ignored (the OS runs its own alarm system on the millisecond timer) |
| `$FB` | power button: simulate the power-button keycode `$08` down/up through the keyboard event path |
| `$FD` | keyboard unplugged: clear the key bitmap and shift state |
| `$FE`/`$FF` | COPS failure: call the KEYERR routine (I/O-board vs keyboard-COPS error count) |

Key events flow on to user-replaceable KEY/KEYID/KEYERR routines running with interrupts at
priority 5 or lower locked out [5] libhw-KEYBD.TEXT. The state machine's byte-at-a-time design is
why packet alignment is a documented contract (§4.3) and why the never-set/timeout paths matter:
any byte lost mid-packet poisons the next one.

### 4.6 Event dispatch: the Boot ROM's `WT4INPUT`

The Boot ROM's monitor and boot-menu code uses a functionally identical parser, `WT4INPUT` [3]
RM248.M ([4] `$2D38`), with the same state split (key event / mouse packet / reset code) but
ROM-appropriate actions: `$00`–`$DF` saves the keyboard ID; `$E0`–`$EF` collects the clock bytes for
the burn-in clock display; `$F0`–`$FA` reserved, `$FC`/`$FD` ignored; **`$FB` calls `PowerOff`** —
the ROM's own shutdown path (§4.10); `$FE`/`$FF` select the keyboard-COPS or I/O-board-COPS error
code [3] RM248.M. The boot menu itself is driven from the key-event stream: "If any key is hit other
than caps lock or the mouse button, a menu is displayed on the screen showing the available boot
devices", and Apple-key sequences select alternate boot devices [3] RM248.E header. The mouse
deltas feed the ROM's own cursor, with the boot menu's rectangles hit-tested against the mouse
position (`CHKPOSN` [3] RM248.M).

### 4.7 Key codes and the Boot ROM ASCII table

The ROM carries a 96-byte `AsciiTable` ([3] RM248.G, assembled at [4] `$38BC`) mapping keycodes
`$20`–`$7F` to characters. The indexing law is fixed by `KeyToAscii` ([3] RM248.M, [4] `$271A`):
`ANDI #$007F` (strip the direction bit), `SUBI #32,D1` (the first table byte is keycode `$20`), then
a byte fetch; an keycode below `$20` yields the sentinel `$02` (invalid) [3] RM248.M. The table
"assumes alpha-lock so upper case only" [3] RM248.G — there are no lower-case entries.

|      | `+0` | `+1` | `+2` | `+3` | `+4` | `+5` | `+6` | `+7` |
| ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- | ---- |
| `$20` | Clear | Pad − | Left | Right | Pad 7 | Pad 8 | Pad 9 | Up |
| `$28` | Pad 4 | Pad 5 | Pad 6 | Down | Pad . | Pad 2 | Pad 3 | Enter |
| `$30` | — | — | — | — | — | — | — | — |
| `$38` | — | — | — | — | — | — | — | — |
| `$40` | `-` | `=` | — | — | `P` | BkSp | — | — |
| `$48` | Return | Pad 0 | — | — | `/` | Pad 1 | — | — |
| `$50` | `9` | `0` | `U` | `I` | `J` | `K` | `[` | `]` |
| `$58` | `M` | `L` | `;` | `'` | Space | `,` | `.` | `O` |
| `$60` | `E` | `6` | `7` | `8` | `5` | `R` | `T` | `Y` |
| `$68` | Option | `F` | `G` | `H` | `V` | `C` | `B` | `N` |
| `$70` | `A` | `2` | `3` | `4` | `1` | `Q` | `S` | `W` |
| `$78` | Tab | `Z` | `X` | `D` | — | Alpha Lock | Shift | Command |

The ROM's own row comments name the keys with no ASCII form (Option, Tab, Alpha Lock, Shift,
Command — stored as `$00` in the table) and identify which entries are the numeric keypad [3]
RM248.G. Two anchors to check any transcription against: keycode `$6B` is `H`, so `$EB` is `H` held
down (the boot menu's "boot from ProFile" key, *observed* in a rev-H boot), and `$F2` is `3` with
the direction bit.

The layout namespace above the codes is the ID byte of §2.5. Two Old-US keycodes differ from the
Final-US ones, and the operating system papers over the difference at interrupt time: "software
changes the keycode for ~` from `$7C` to `$68`, and the keycode for R-Option from `$68` to `$4E`"
[5] libhw-LEGENDS.TEXT — the two layouts produce identical canonical keycodes after the remap. The
European layout adds one key (`><`) at keycode `$43` [5] libhw-LEGENDS.TEXT.

### 4.8 Programming the NMI key

`SetNMIKey` splits the 7-bit keycode into two nibble commands and sends them as a pair: high three
bits OR'd with `$58` (`0101 1rrr`), low four bits OR'd with `$60` (`0110 nnnn`), then verifies the
transaction counter survived [5] libhw-KEYBD.TEXT. Sending `$50`/`$60` (keycode zero) disables the
key — which is what the Boot ROM's `TURNON` does at power-up [3] RM248.K, so that a stray keypress
can never NMI the machine before the OS is ready to handle one.

### 4.9 Mouse control and delta scaling

The `$7n` command is the only mouse control. The OS's `MouseUpdates` rounds the requested interval
to a multiple of 4 ms, clamps to 0..7, and ORs the result into `$78` (reports enabled) [5]
libhw-MOUSE.TEXT; the initial setting is 16 ms, and the Boot ROM's `MousInit` uses the same `$7C`
[3] RM248.M. Disabling (`$70`) is what the ROM does outside interactive code paths [3] RM248.K,
RM248.M.

Scaling of the delivered deltas is *host-side*, and both shipped implementations do it the same way
[3] RM248.M `MouseMovement`; [5] libhw-MOUSE.TEXT "Mouse Scaling":

- With scaling off: 1 delta unit = 1 pixel on each axis.
- With scaling on, the motion is classified by `|dx| + |dy|` against a threshold (initial 8): at or
  below it, **fine** movement — X at 1:1, Y at 5/8 (the ROM computes `dy×(5/8)` with rounding; the
  OS documents 2/3); above it, **coarse** movement — X at 3/2, Y at 1:1.
- The horizontal magnification exists "to compensate for the 2/3 aspect ratio of pixels on the
  screen" [5] libhw-MOUSE.TEXT — the Lisa's 720×364 display has non-square pixels ([Lisa](lisa.md)
  §8).
- The result is clamped to the screen bounds 0..720, 0..364 [3] RM248.M.

The COPS itself is done once the deltas are delivered; it knows nothing of pixels, thresholds or
the screen.

### 4.10 Power off and power cycle

Three shipped sequences end in a power-off command:

- **Boot ROM `PowerOff`** ([4] `$2DD8`): if the disk controller is healthy, enable and eject both
  floppy drives, tell the Twiggy controller to vacate shared memory, blank the screen contrast,
  wait one second, send `$21`, then wait the 1.7 s settle; if the machine is still alive it sounds
  the error tone and reports an I/O-board error [3] RM248.M. This is the routine behind the
  response-stream `$FB` (§4.6): the power button in software.
- **OS `PowerDown`**: dim the screen, read the clock, and choose `$20` (clock never set — nothing
  worth keeping running) or `$21` (clock running — leave it so), send it, coast, and retry the
  whole thing if the machine is still up [5] libhw-MACHINE.TEXT.
- **OS `PowerCycle`**: dim, verify the clock is set (a never-set clock cannot schedule a wake, so
  the request degenerates to `PowerDown`), send `$2D` (clock-set mode, timer stopped), send the
  requested delay as five `$1n` nibbles, send `$23` (power off, timer-underflow-power-on armed),
  then coast [5] libhw-MACHINE.TEXT. The delay is in seconds, 0..1,048,575 [5] libhw-MACHINE.TEXT.
- **Boot ROM burn-in `SHUTDOWN`** ([4] `$23E8`): the factory power-cycling mode — arm an alarm of
  (cycle minutes × 60) seconds via `$2D` + five nibbles, save the power-cycle boot code in
  parameter memory, send `$23`, and spin [3] RM248.B. The machine then wakes itself on timer
  underflow and re-runs the test loop, demonstrating the full RTC-wake path of §3.6.

The boot-time diskette ejection in the ROM's routine was an explicit late fix ("Add diskette
eject on power-off" [3] RM248.E change log), and the operating system's four-level power model —
fully operational, display dimmed, powered off, unplugged — with its phosphor-preserving screen
dimmer is built on top of this hardware [5] libhw-MACHINE.TEXT "Power Control".

## 5. Quirks & errata

- **The Hardware Manual's I/O addresses are missing the `FC` prefix.** The manual prints keyboard
  VIA registers as `$00xxxx`; the errata corrects the whole I/O space to `$FCxxxx` — the COPS VIA
  base is `$FCDD81` [2] "I/O Space Addresses", "Internal VIA locations". The Boot ROM agrees
  (`VIA1BASE = $00FCDD81` [3] RM248.E).
- **The Lisa OS source swaps the VIA names.** In the OS's LIBHW, `VIA2` is the *keyboard* VIA and
  `VIA1` is the hard-disk VIA — the reverse of the Hardware Manual and of this page. When the OS
  reads the COPS from "`VIA2+PORTA2`" it is reading port A of the keyboard VIA. See the warning in
  [Lisa](lisa.md) §10; keep the swap in mind for every OS source citation in this page.
- **CRDY is free-running, not a level.** The COPS toggles it continuously through its scan loop;
  senders synchronize to *edges*, twice per command (§3.1). A static CRDY level hangs every sender
  ([Lisa](lisa.md) §11, *observed*).
- **Port A is shared, so sending is a DDRA dance.** The host preloads the command into the ORA
  latch *before* driving the port, jams `DDRA = $FF` only inside the safe window, and releases it
  immediately after the COPS's read phase (§3.1). A sender that leaves DDRA driven loses the
  response path.
- **The ROM and OS stage the command byte differently.** The ROM writes the no-handshake register
  (offset 15) so no CA2 pulse escapes; the OS writes the handshake register (offset 1), which pulses
  CA2 on every command (§3.1). Both work against the same COPS; the difference is unexplained
  (§6.11).
- **Bit 7 of a command byte is a don't-care NOP** [1] Figure 2-14 — and bit 7 of a *response* byte
  is the key-down flag. The same byte means opposite things on the two sides of the port.
- **The mouse button, mouse plug, disk states and power button are keycodes**, riding the same
  response channel as keystrokes (`$01`–`$08`, §2.3). A parser that treats every non-`$80`, non-`$00`
  byte as a keyboard event will still work — by design.
- **The power button never removes power.** It only delivers `$80 $FB`; removal of main power is a
  *software* action (`p = 0` command), and the RTC can only turn the machine *on* (§3.6) [1] §6.6.4
  p. 6-52.
- **Powering off is asynchronous.** The ROM waits 1.7 s after `$21` before declaring failure [3]
  RM248.M; the OS coasts and retries [5] libhw-MACHINE.TEXT. Software must expect to keep running
  for a while after the off command.
- **Clock digits are sixteen separate commands.** And the first five are the alarm, not the time
  (§3.5) — a writer that forgets the alarm digits arms a wake (or an interrupt) it did not intend.
- **The clock stops while being set.** `mm = 00` in the set-mode command is what stops it; leaving
  `mm = 01` while writing time digits would be wrong for the clock half and right for the alarm
  half [1] §2.5.4 p. 2-25.
- **The keyboard takes over a second to come back.** The keyboard reset pulse is 12 ms, but the
  settle after it is 1.7 s (`KBDDLY`) [3] RM248.K/E — code that resets the keyboard and immediately
  expects the ID code will not see it.
- **Input dies with level-2 interrupts.** Response bytes must be drained within about 10 ms or
  keyboard and mouse input are lost (§3.2) [5] LIBHW-DRIVERS.TEXT "Poll".
- **An NMI can split a COPS conversation.** The NMI key arrives outside the VIA (§3.7); the OS
  restarts the interrupted output afterwards via the transaction counter (§4.3).
- **Keycode `$68` has two histories.** The OS legends file maps it to the `` ~` `` key on the Final
  US layout (after remap), while the Boot ROM's row comment calls it Option (§4.7, §6.13).
- **The manual disagrees with itself about the power switch location** — "lower-left of the Lisa
  cabinet" [1] §6.6.2 p. 6-50 vs. "lower-right corner" of the front [1] §8.4.1 p. 8-6. One of them is
  an error in the 1983 text.
- **`$7C` enables the mouse, not the clock.** The `$7n` mouse command and the `$2n` mode command
  share their top nibble space with everything else; mixing them up silently disables mouse reports
  instead of setting a mode.

## 6. Open questions

1. **The COP421's internals.** The chip's ROM/RAM sizes, instruction set, oscillator frequency and
   scan-loop period are all in the *COPS421 User's Manual* [6], which is referenced by the Hardware
   Manual's bibliography but not available in this evidence set. The CRDY period in particular is
   only bounded from above by the senders' ~10 ms timeouts (§3.1).
2. **What "turn I/O port on/off" (`$00`/`$01`) actually gates.** The Boot ROM sends `$00` at
   bring-up [3] RM248.K; no released software sends `$01`; no document describes the state
   difference.
3. **Response buffering and loss semantics.** The OS's `Poll` note implies input is lost when
   level-2 interrupts are off beyond ~10 ms (§3.2), but whether the COPS drops, overwrites or holds
   an unacknowledged byte — and how deep its queue is — is undocumented. The Boot ROM's scan state
   machine silently tolerates a full queue [3] RM248.K, implying a bounded buffer.
4. **CA1/CA2-to-S0/S1 polarity at the COPS end.** The manual states the wiring ("CA1 and CA2...
   attached to the S0 and S1 lines of the COPS" [1] §6.6.2 p. 6-50) and the host side is fixed by
   the PCR values (CA1 positive edge, CA2 handshake pulse), but which COPS line is the data strobe
   and the exact ack semantics seen by the COPS firmware are inferred from the host configuration
   (*inferred*).
5. **The full keyboard-ID value list.** Figure 2-16's ID enumeration is not legible in the scanned
   manual; the OS legends table (§2.5) is the OS's *interpretation* of the IDs, not proof of what
   the keyboard COPS emits for each physical keyboard.
6. **The scan matrix itself.** Figure 2-15 [1] p. 2-21 (the `rrr`/`nnnn` to key assignment) is a
   figure without recoverable text; the OS legends file carries a matrix, but its column/row
   alignment and its two disagreements with the ROM's row comments (§6.13) are unresolved from
   primary text alone.
7. **The COPS RAM self-test.** Both failure codes are glossed as COPS RAM errors in the OS equates
   [5] libhw-KEYBD.TEXT; what RAM is tested, and when (both COPSes? only the keyboard one?), is
   undocumented.
8. **The power-fail sense on G2/D5.** The manual names the path [1] §6.6.2 p. 6-50 but no released
   software handles a power-fail event, and no document says what the host observes (an NMI? a
   reset code?).
9. **Partial clock writes.** "Only as many nibbles as are required need be sent" [1] §2.5.4 p. 2-25
   — but which digits a short write leaves standing (does digit position reset per set-mode
   entry?) is not pinned by any released sequence, which always writes full fields.
10. **The hold-time comment in `COPSCMD`.** The ROM's delay loop after the COPS takes the byte is
    annotated "force about a 40 ms delay for COPS hold time" but is a bare ten-iteration loop [3]
    RM248.K ([4] `$09A2`) — microsecond-scale as written. Either the comment or the loop is wrong;
    the OS driver has the same short spin [5] LIBHW-DRIVERS.TEXT, so the short value is at least
    consistent across senders.
11. **The exact correspondence the CA2 pulse plays in command intake** — whether the OS's decision
    to write commands through the handshake register (pulsing CA2) is load-bearing for the COPS or
    merely tolerated (§5).
12. **Whether both shipped COPS firmware versions are field-identical.** The manual says the two
    COPSes are the same part with the same routines [1] §8.2.1 p. 8-2, but the keyboard ID carries
    manufacturer bits for at least two vendors (§2.5); whether all vendor keyboards carried a
    COP421 with Apple firmware is unverified.
13. **Keycode `$68`: Option or `` ~` ``?** The Boot ROM's `AsciiTable` row comment names it Option
    [3] RM248.G, while the OS legends matrix places `` ~` `` there on the Final US layout and remaps
    Old-US R-Option from `$68` to `$4E` [5] libhw-LEGENDS.TEXT (§4.7). The ROM stores `$00` for the
    code, so its software never distinguishes the two readings; which physical key sat at matrix
    position 110/1000 on the shipping 76-key keyboard is unestablished here.
14. **Mouse switch inputs SW1/SW2.** Three switch lines exist and only SW0 is used [1] §6.6.3 p.
    6-51; whether the COPS firmware would report a second button (and as what code) is unknown.
15. **Timing between the power-off command and the supply actually dropping.** The 1.7 s ROM wait
    and the OS coast loop (§4.10) bound it from above; the actual collapse time is set by the
    supply (a flyback switcher [1] §10) and is not documented.

## References

1. Apple Computer Inc., *Apple Lisa Computer: Hardware Manual*, April 1983 (with errata).
   Cited by manual section and page: §2.5.4 pp. 2-18..2-25, §6.6 pp. 6-49..6-53, §6.7.2
   pp. 6-54..6-55, §8.2..8.2.3 pp. 8-1..8-3, §8.3 p. 8-4a, §8.4.1 p. 8-6, §10 p. 10-1,
   §10.2.2 p. 10-4, §10.5.1 p. 10-10, Figures 2-13..2-16, 6-13, 8-1..8-2, Glossary.
2. Apple Computer Inc., *Apple Lisa Computer: Hardware Manual 1983 — Errata (May 1985)*, including
   "Macintosh XL Hardware Information" (Mark Baumwell, Apple Computer Inc., 16 May 1985).
3. Apple Computer Inc., *Lisa Boot ROM source, revision 2H (RM248)*, released source; files
   RM248.B.TEXT, RM248.E.TEXT, RM248.G.TEXT, RM248.K.TEXT, RM248.M.TEXT, RM248.S.TEXT. Cited by
   routine name.
4. *Lisa Boot ROM assembly listing, revision 2H*, same source release; cited by ROM address
   (`COPSCMD $0956`, `CPSINIT $0920`, `RSTSCAN $09C2`, `GETDATA $0A7E`, `RSTKBD $0AAA`,
   `KBDDELAY $0ADC`, `SHUTDOWN $23E8`, `TODSET $2466`, `DSPCLK $24CA`, `KeyToAscii $271A`,
   `WT4INPUT $2D38`, `PowerOff $2DD8`, `MouseMovement $2F2A`, `AsciiTable $38BC`).
5. Apple Computer Inc., *Apple Lisa Operating System and applications source code* (source release
   via the Computer History Museum); files LIBHW/LIBHW-DRIVERS.TEXT, LIBHW/libhw-KEYBD.TEXT,
   LIBHW/libhw-MOUSE.TEXT, LIBHW/libhw-TIMERS.TEXT, LIBHW/libhw-MACHINE.TEXT,
   LIBHW/libhw-LEGENDS.TEXT. Cited by routine name.
6. National Semiconductor Corporation, *COPS421 User's Manual*. Referenced as the chip-level
   reference by [1]'s bibliography; not part of the evidence set behind this page.
