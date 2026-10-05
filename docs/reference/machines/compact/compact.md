# The compact Macintosh family

**Contents:**

1. [Overview & membership](#1-overview--membership) — the classic Macintosh computers, the compact design
   idea, the evidence base, and what this page covers against the machine and device pages
2. [Board architecture common to the family](#2-board-architecture-common-to-the-family) — the two boards,
   the MC68000, the PAL general logic, the shared-RAM model, ROM, video, sound and disk-speed, the
   reset and ROM-overlay sequence
3. [Memory map & address decode shared by the family](#3-memory-map--address-decode-shared-by-the-family)
   — the 24-bit map, the /DTACK and /VPA halves, every device-select window, the phase read, and the
   no-bus-error contract
4. [Device roster](#4-device-roster) — every chip on the board and where its page lives
5. [Interrupt, bus, and clock architecture](#5-interrupt-bus-and-clock-architecture) — the three interrupt
   levels, the VIA and SCC interrupt machinery, clocks, and the single-master bus
6. [Per-machine index](#6-per-machine-index) — the 128K, 512K, 512K enhanced and Plus, and where the
   family ends
7. [Open questions](#7-open-questions)

---

## 1. Overview & membership

### 1.1 What the family is

The **compact Macintosh family** is the original all-in-one design: a 9-inch built-in monochrome CRT,
a separate keyboard and mouse, a single internal floppy drive, and a main logic board built around a
Motorola MC68000, all in one case. Apple's collective name for the first four machines is
the **classic Macintosh computers**: "The Macintosh Plus, Macintosh 512K enhanced, Macintosh 512K,
and Macintosh 128K are sometimes referred to collectively as the classic Macintosh computers" [1]
p. 4. Apple's own summary matrix classifies the whole group in one row: configuration *Compact*,
CPU MC68000, no other processors, PALs as both the memory-management and general-logic ICs, **one
VIA**, RAM SIMM expansion, a hardware on/off switch, the Macintosh (non-ADB) mouse and keyboard, an
800 KB floppy, SCC serial, SCSI, built-in black-and-white video, PWM sound, and *no* expansion method
[1] Table 1-1 pp. 2–3. This page follows Apple's definition: the family is the **128K, 512K, 512K
enhanced and Plus**; the compact-cased machines that followed (SE, SE/30, Classic) carry different
general logic and an expansion slot and are outside it (§6.5).

The family's defining architectural fact is that **one dynamic RAM array is shared between the CPU,
the video circuitry, the sound generator and the floppy disk-speed controller**. The classic Macintosh
computers "use part of the main memory for the video display and for pulse-width-modulated (PWM)
sound samples", and the RAM data-bus buffers "can either connect the main data bus to the RAM data
bus, or isolate the two buses and give the video and sound circuits direct access to RAM" [1] p. 60.
Only the CPU writes RAM; the video display, the sound generator and the disk-speed controller read
from it, interleaved into the CPU's cycle stream [1] pp. 65–66, 192–193. Every other consequence in
this page — the 2.56 MB/s average RAM access rate (§2.4), the 60.15 Hz vertical blanking interrupt
(§5.2), the sound and disk-speed buffers at the top of memory (§2.7) — flows from that one decision.

The four members are one design in four configurations. Apple's description of the group: the
Macintosh 128K has "an MC68000 microprocessor running at 8 MHz, 128 KB of RAM, 64 KB of ROM, and a
400 KB internal disk drive"; the 512K "is identical to the Macintosh 128K" except for memory; the
512K enhanced swaps in "128 KB of ROM, and an 800 KB internal disk drive. The ROM and disk drive in
the Macintosh 512K enhanced computer are the same as those used in the Macintosh Plus"; and the Plus
adds the SCSI port, new serial connectors, the keypad keyboard, and RAM "expanded to 4 MB" [1] p. 4.
The exact processor clock is 7.8336 MHz [1] p. 18 — the "8 MHz" in Apple's feature lists is the
rounded figure — generated from the 15.6672 MHz master crystal by the general logic (§5.5).

### 1.2 Membership

| Machine | RAM | ROM | Internal floppy | Serial connectors | SCSI | Input devices | Page |
|---|---|---|---|---|---|---|---|
| Macintosh 128K | 128 KB soldered [1] pp. 4, 196 | 64 KB | 400 KB single-sided | two DB-9 | none | keyboard + mouse (non-ADB) | §6.1 |
| Macintosh 512K | 512 KB soldered [1] pp. 4, 196 | 64 KB | 400 KB single-sided | two DB-9 | none | keyboard + mouse (non-ADB) | §6.2 |
| Macintosh 512K enhanced | 512 KB soldered [1] p. 4 | 128 KB | 800 KB double-sided | two DB-9 | none | keyboard + mouse (non-ADB) | §6.3 |
| Macintosh Plus | 1 MB on SIMMs, expandable to 4 MB [1] p. 4 | 128 KB | 800 KB double-sided | two mini-DIN 8 | one external, NCR 5380 | keypad keyboard + mouse (non-ADB) | §6.4, [plus.md](plus.md) |

Three structural properties hold for every member and separate this family from everything Apple
shipped afterwards:

- **No expansion interface.** The expansion-method column of Apple's matrix reads "(none)" for the
  compact row [1] Table 1-1 p. 3; the multi-purpose expansion interfaces (PDS and NuBus) belong to
  the SE and the Macintosh II family [1] pp. 83–84. The only intentional external buses are the
  serial ports, the floppy port, the SCSI port (Plus) and the keyboard/mouse connectors.
- **No ADB.** The keyboard and mouse connect directly to the VIA and SCC (§4); the Apple Desktop Bus
  "is used to connect keyboards, mouse devices … starting with the Macintosh SE and Macintosh II"
  [1] p. 75.
- **One VIA.** A single standard Rockwell or VTI 6522 handles every system-control and interrupt
  duty (§4, §5.2); two-VIA machines start with the SE/30 and the Macintosh II family [1] p. 73.

### 1.3 Scope of this page, and the evidence behind it

This page is the **family doc** of the three-level machine set: it holds what the four boards share.
The Plus — the one member this reference emulates — has its own machine doc,
[plus.md](plus.md), which adds the SCSI port, the SIMM geometry and the per-machine deltas to what
is written here; the other three members are context and are covered as far as the evidence goes
(§6). The devices have their own pages, and this page cites them by section instead of restating
them: [via.md](../../hardware/via.md) for the 6522, [scc.md](../../hardware/scc.md) for the Z8530,
[iwm-floppy.md](../../hardware/iwm-floppy.md) for the floppy controller and drive interface,
[rtc.md](../../hardware/rtc.md) for the clock/PRAM chip, [ncr-5380.md](../../hardware/scsi/ncr-5380.md)
for the SCSI controller, [keyboard.md](../../hardware/keyboard.md) for the keyboard protocol and
[mouse.md](../../hardware/mouse.md) for the mouse.

The evidence base for this family is honest about its limits, and so is this page. There is no
developer note or theory-of-operation document for any of the four machines: the primary published
source is the *Guide to the Macintosh Family Hardware*, whose compact-Macintosh sections (the block
diagrams of Chapter 2, the PAL and address-map sections of Chapter 3, the VIA chapter, the memory
chapter, the power chapter and the whole of Chapter 7 on the Plus mouse and keyboard) are the citable
backbone [1]. The rest is built from the Macintosh Plus boot ROM, whose annotated disassembly
[2] supplies the device-select addresses, the boot sequence and the driver behaviours as *observed*
facts. Where neither source speaks — PAL part numbers, decode granularity, reset values — the gap is
recorded in §7 rather than papered over; this page is deliberately leaner than its subject deserves
until better evidence lands.

## 2. Board architecture common to the family

### 2.1 The two boards

The compact Macintosh is physically two boards. "The microprocessor, RAM, ROM, and the various
input/output ICs are located on the main logic board (also called the digital board). The vertical
analog board contains the power supply and video circuitry for the built-in monitor. The interiors
of other classic Macintosh computers are similar in appearance to the interior of the Macintosh
Plus" [1] p. 18. The two boards are joined by one connector whose signals are all analog-board
services: /VIDEO (the video bit stream), /HSYNC and /VSYNC (the sync signals the logic board's PALs
generate), SPKR (the audio output), +5 V, +12 V, −12 V, ground, and BAT — the battery line that
keeps the real-time clock alive when the machine is off [1] Table 6-1 p. 246. The horizontal and
vertical deflection and the CRT live on the analog board; everything with an address lives on the
logic board.

### 2.2 The MC68000 and its bus

Every member runs an MC68000: internal 32-bit registers, a 24-bit external address bus and a 16-bit
external data bus [1] p. 49. All 16 data lines go to the processor, the ROM and the RAM data-bus
buffers and nothing else [1] p. 60 — each peripheral rides one byte of the bus (§3.2). The MC68000
has no external A0; the pair of data strobes carries it: "when /UDS is asserted, A0 is considered to
be 0, and when /LDS is asserted, A0 is considered to be 1. Therefore even-addressed byte-wide reads
and writes use the upper byte of the data bus and odd-addressed accesses use the lower byte" [1]
pp. 34, 90 — the rule that fixes every device's byte lane in §3.2.

The CPU "communicates asynchronously with all devices except the VIAs. Communications with the VIAs
in the classic Macintosh … computers are synchronized by a special signal generated by the MC68000
called the Enable signal or the E clock" [1] p. 61; the general logic circuits generate /DTACK and
/VPA and all device-select signals [1] p. 61. The MC68000's bus arbitration pins exist but go
unused in the stock machine: "Although no coprocessors share the processor buses in the … classic
Macintosh computers, developers have taken advantage of the bus arbitration capability to design
accelerator cards for these computers" [1] p. 91. There is no DMA anywhere in the family: the CPU is
the only bus master and the only writer of RAM [1] pp. 65–66.

### 2.3 PALs: the general logic circuits

The Plus and its predecessors have no custom Apple silicon at all — no GLUE, no BBU, no memory
controller. "The Macintosh Plus and earlier Macintosh computers use PALs, which are programmable
logic array devices" [1] p. 65, and Apple's published function list for them is the whole
specification of the machine's control layer [1] p. 111. The PALs:

- decode addresses and assert the device-select signal for the addressed device (§3);
- generate the RAM address strobes /RAS and /CAS and control the address multiplexers that feed the
  RAM (§2.4);
- control the timing and sequencing of the video functions, read video data directly from RAM, and
  direct the Video Shift register to send the data as a bit stream to the video circuits (§2.6);
- generate the vertical and horizontal blanking interrupt signals (§5.2);
- control sound generation, including reading sound data from RAM and converting it to a PWM signal
  (§2.7);
- read disk-speed control data from RAM for 400 KB floppy drives, convert it to a PWM signal, and
  send it to the disk drives (§2.7);
- generate the 7.8336 MHz clock used by the main processor and the 3.672 MHz clock used by the SCC
  to control communication rates (§5.5).

The PAL equations and part numbers are not published anywhere in the evidence set (§7.1).

### 2.4 RAM: soldered DRAM (128K/512K/512Ke) and SIMMs (Plus)

RAM on the 128K, 512K and 512K enhanced is DRAM soldered to the logic board: "sixteen individual
64 Kbit DRAMs" for the 128K and "16 individual 256 Kbit DRAMs" for the 512K/512Ke, arranged in two
rows of eight [1] p. 196. The Plus moves RAM onto SIMMs — two or four of them, each carrying eight
DRAM ICs, arranged in pairs called rows (row 1 = SIMMs 1+2, row 2 = SIMMs 3+4), two SIMMs
supplying the 16-bit word [1] p. 197. Plus RAM configuration and the SIMM socket pinout are Plus
facts and live in [plus.md](plus.md) §3; the family-level fact is the access cycle structure, which
is identical.

An address in the RAM range is decoded by the PALs, which enable the data-bus buffers and drive the
RAM address multiplexers: the multiplexers split the address into a row half and a column half, the
row half is strobed in by /RAS, and the column half by a CAS line — CAS1 and CAS2, one per row of
eight, on the three soldered-RAM machines; CAS1 through CAS4, one per SIMM, on the Plus, with ten
RAM address lines addressing up to 4 MB [1] pp. 196–197. The installed size is discovered by software
at startup and recorded in the global MemTop [1] p. 196.

The access cycle structure is the family's signature. "In the Macintosh SE and classic Macintosh
computers, the main processor's RAM-access cycles are interleaved with the video display's access
cycles. The video display access cycles occur only during the active portion of a screen scan
line. The video logic scans the video screen buffer in RAM 60 times each second and sends the data
to the screen. This process both refreshes the dynamic RAM and refreshes the image on the video
screen" [1] p. 194 — the CRT scan is the only DRAM refresh mechanism the machine has. On the Plus
"one out of every two RAM access cycles is devoted to video data during a scan line. The last RAM
access before the beginning of a scan line is used for sound and disk-speed control data" [1] p. 194,
which gives the processor an average RAM access rate of about 2.56 MB/s [1] Table 5-3 p. 194. When
video or sound or disk-speed data is being fetched, "the RAM data-bus buffers isolate the RAM data
bus from the CPU data bus", and the CPU cannot touch RAM during that access [1] pp. 60, 66.

### 2.5 ROM

The 128K and 512K carry 64 KB of ROM; the 512K enhanced and the Plus carry 128 KB [1] p. 4. The ROM
"contains system routines, including most of the code for the Macintosh Toolbox, and the Reset
handler that is executed each time the computer is started up or reset", and each machine's ROM code
is unique to it [1] p. 68. Physically the ROM is two byte-wide ICs read as a word: "the CPU always
reads one word of data at a time from ROM; the low byte of each word is read from one ROM IC and the
high byte is read from the other" [1] p. 68. The CPU never shares ROM access with anything [1]
p. 68. Address decode assigns the ROM the range from $400000, and the PALs answer the whole
$400000–$43FFFF select window, but only the populated portion has devices attached: with a 128 KB
ROM, "128 KB ROMs are disabled (no device assigned) everywhere A17=1" [1] p. 124 — that is, a Plus
answers $400000–$41FFFF. The 128 KB Plus image carries its own version and checksum bytes in its
header (ROM version $75, checksum $4D1F8172, machine type $00) and places its boot entry at file
offset $2A [2] — the values a re-implementation of the Plus must reproduce; the earlier machines'
ROM images are not in the evidence set (§7.13).

### 2.6 Video: the shared-RAM frame buffer

The built-in display is a 512×342, 1-bit, black-and-white CRT of "approximately 4.6 by 7 inches",
one bit per square pixel of about 1/72 inch [1] p. 399. A full screen is 21,888 bytes of RAM
[1] p. 70, and there are two of them: "Two areas of memory are reserved for use by the video
circuitry: the main screen buffer and the alternate screen buffer. A bit in VIA Data register A
determines which screen buffer is read by the video circuitry" [1] p. 401 — vPage2, bit 6, with 0
selecting the alternate buffer [1] Table 4-2 pp. 160–161. A 0 bit displays white and a 1 bit
displays black [1] pp. 69, 401.

The scan numbers, which time everything else in the machine (§2.7, §5.5):

| Quantity | Value | Source |
|---|---|---|
| Pixel clock | 15.6672 MHz (about 0.064 µs per pixel) | [1] p. 400 |
| Active line | 512 pixels, 32.68 µs | [1] p. 400 |
| Horizontal blanking | 192 pixel-times, 12.25 µs | [1] p. 400 |
| Full scan line | 44.93 µs; horizontal rate 22.25 kHz | [1] p. 400 |
| Visible frame | 342 lines, 15.37 ms | [1] p. 401 |
| Vertical blanking | 28 line-times, 1.26 ms | [1] p. 401 |
| Full frame | 370 lines, 16.6 ms; vertical rate 60.15 Hz | [1] p. 401 |

At the start of the vertical blanking interval the VIA generates the vertical blanking interrupt
[1] p. 400 — the machine's 60.15 Hz heartbeat (§5.2). To fetch a line, the PALs disable the RAM
data-bus buffers, drive a screen-buffer address through the multiplexers onto the RAM, and signal
the Video Shift register on the logic board to take the data and serialize it to the analog board;
in the Plus and earlier machines the fetch is one word per video access [1] p. 401, timed to
minimize the RAM time taken from the CPU. The video generator's display of the buffer repeats the
same 21,888 bytes every frame, so the CPU sees a perfectly ordinary block of RAM it shares with a
very regular DMA engine.

The screen buffer lives at the top of physical RAM: the Plus boot code sets BufPtr to MemTop minus
$5900, which is $3FA700 on a 1 MB machine [2] (*observed* in the Reset handler). The placement rule
for the alternate screen buffer is not stated in the evidence set (§7.6).

### 2.7 Sound and disk-speed: the PWM channels

The family predates the Apple Sound Chip: it is one of Apple's two sound architectures, "the Macintosh
SE and classic Macintosh computers use their general logic circuits plus a Sony sound IC to
generate output for the internal speaker or external sound jack" [1] p. 71. The system is fully
described in four facts [1] pp. 71–72, 429–431:

1. **The sample source is system RAM.** The sound buffers are ordinary RAM at the top of memory; on
   the Plus and earlier machines there are two, each "370 words of data. Sound information is
   stored in the high (even address) byte of each word, and disk-speed information is stored in the
   low (odd address) byte" [1] p. 430. The boot code sets SoundBase to MemTop minus $300 — $3FFD00
   on a 1 MB machine — and the boot beep is written there [2] (*observed*). The alternate buffer
   sits at SoundBase − $5C00 [1] p. 429, selected by vSndPg2, bit 3 of VIA Data register A [1]
   pp. 160, 429.
2. **The fetch rate is the horizontal scan rate.** "The sound circuitry reads one word (16 bits) in
   the sound buffer during each horizontal blanking interval … and uses the high-order byte of the
   word to generate a pulse of electricity whose duration is proportional to the value in the byte"
   [1] p. 430 — the last RAM access before each scan line (§2.4). The buffers are scanned at
   370 words per frame, "repeating the full cycle 60.147 times per second (the exact sample rate is
   22.2545 kHz)" [1] p. 431.
3. **The Sony sound IC integrates and attenuates.** The pulse train goes to the Sony sound IC, which
   integrates it into a waveform whose amplitude is proportional to pulse width, then attenuates it
   "according to a 3-bit volume-control value from the VIA" and drives the internal speaker and the
   external mini-phone jack; inserting a plug disconnects the speaker [1] pp. 71, 428. The volume
   value is the low three bits of VIA Data register A (vSound, 111 = maximum, 000 = minimum) and the
   output is gated by vSndEnb, bit 7 of Data register B, which Timer 1 can toggle automatically for
   square-wave output [1] pp. 160, 167, 179–180, 431; the boot code sets the volume from parameter
   RAM before the beep [2] (*observed*).
4. **The low bytes are a floppy motor speed control.** "The logic circuits … take the low-order
   byte of the word of data from the first sound buffer and generate a train of pulses with it just
   as the sound PWM does for sound. This train of pulses is fed directly to the motor of any
   single-sided (400 KB) floppy disk drive connected to the IWM port. The power to the motor is
   turned on and off by these pulses, so that the inertia of the motor itself integrates the pulses,
   controlling the motor speed" [1] pp. 72–73 — the hardware behind the classic Macintosh's
   variable-speed 400 KB drive format. Double-sided (800 KB) drives have internal speed control and
   ignore the signal [1] p. 73, and only the 128K and 512K shipped single-sided drives [1] p. 18.
   Because the two share a buffer, the disk drivers store speed data only in the main sound buffer,
   and Apple warns that an application with its own sound driver must switch back to the main
   buffer before touching a single-sided drive [1] p. 430.

The boot beep is the worked example of the whole path, and the ROM shows it end to end: fill 370
even bytes of the buffer at SoundBase, wait for the VBL flag in the VIA, set and clear the sound
enable bit around the burst [2] (*observed*).

### 2.8 Reset and the ROM overlay

The power-on sequence is Apple's most explicit statement of the machine's bring-up, and it is the
same on all four members [1] p. 242:

1. The Sony sound IC on the analog board monitors the board voltages and asserts /RESET until
   0.25 second after they stabilize.
2. /RESET puts the CPU and all internal devices in a known state.
3. The sound IC releases /RESET. The CPU fetches the Reset vector from the first words of memory —
   and since RAM holds nothing yet, "when the VIA is reset, it pulls the Overlay signal high, which
   causes the PALs to use an address decoding scheme that puts ROM at location $00 0000" [1] p. 242.
   This is the **ROM overlay address map**: addresses $000000–$01FFFF (the size of the largest
   family ROM) decode to ROM, and the range $400000–$43FFFF still decodes to ROM as well [1]
   pp. 121–122, 242.
4. The CPU jumps through the Reset vector and starts the Reset handler in ROM.
5. One of the first instructions clears the Overlay bit — vOverlay, bit 4 of VIA Data register A —
   "which switches the PALs to the normal address map. In this address map, RAM is located at
   $00 0000 through $3F FFFF (for 4 MB of RAM) and ROM is located at $40 0000 through $43 FFFF"
   [1] p. 242.
6. The Reset handler runs the startup procedure.

The overlay is thus armed by the VIA's reset state and dropped by software, not by an access — the
later families' access-triggered overlays work differently, and a re-implementation must not confuse
them. The Plus ROM performs the drop as part of its first VIA writes, setting Data register A to
$6B with the volume bits merged in — bit 4 clear is the overlay off [2] (*observed*; the same write
sets the screen buffer select and sound buffer select from reset).

## 3. Memory map & address decode shared by the family

### 3.1 The 24-bit map and its two halves

The MC68000 sees 16 MB, and the family never sees more: "the classic Macintosh, Macintosh SE, and
Macintosh Portable computers use an MC68000 processor, which has a 24-bit address bus" [1] p. 40 —
there is no MMU and no 32-bit mode anywhere in the family. The map has two halves with different bus
protocols, and the split is the key to everything:

> "The PALs respond to any address in the range $00 0000 through $DF FFFF with a /DTACK signal and
> to any address in the range $E0 0000 through $FF FFFF with a /VPA signal. The /DTACK signal is
> used to acknowledge a transaction with a device that uses asynchronous communication, and the
> /VPA signal is used to acknowledge a transaction with a device that requires synchronous
> communication with the main processor." [1] p. 123

Only the VIA sits in the synchronous half — it is the one E-clocked device (§2.2) — and the
interrupt-acknowledge window at the top of memory is answered with /VPA too, which is what makes the
MC68000 use automatic vectoring (§5.1). Every asynchronous device — RAM, ROM, SCC, IWM, SCSI — lives
below $E00000.

The family map, with every window's owner and byte lane:

| Range | Function | Byte lane | Evidence |
|---|---|---|---|
| $000000–$3FFFFF | RAM, all members (populated to the installed size; §3.4) | word (16-bit) | [1] pp. 123, 191 |
| $000000–$01FFFF | ROM overlay image of the same ROM (reset only) | word | [1] pp. 121–122, 242 |
| $400000–$43FFFF | ROM select window; $400000–$40FFFF populated on 64 KB machines, $400000–$41FFFF on 128 KB machines | word | [1] pp. 124, 242 |
| $580000–$5FFFFF | SCSI (Plus only): register file at A9=0, pseudo-DMA data aperture at A9=1; even addresses read, odd write (lower byte) | byte | [1] pp. 34, 121; [2]; see [plus.md](plus.md) §3 |
| $9FFFF8–$9FFFFF | SCC read window (top of the $9xxxxx megabyte): control B/A and data B/A on $2 strides, even addresses (upper byte) | byte | [1] p. 364; [2] |
| $BFFFF9–$BFFFFF | SCC write window (top of the $Bxxxxx megabyte), odd addresses | byte | [1] p. 364; [2] |
| $DFE000–$DFFFFF | IWM softswitch window (lower byte, odd addresses) | byte | [2]; [iwm-floppy.md](../../hardware/iwm-floppy.md) §"Macintosh Plus CPU-visible interface" |
| $EFE000–$EFFFFF | VIA register block (upper byte, even addresses) | byte | [1] pp. 159–160; [2]; [via.md](../../hardware/via.md) §"Accessing the VIA" |
| $F00000–$F7FFFF | Phase read: the startup timing probe (§3.3) | — | [1] p. 123 |
| $F80000, $F80080 | Test-software hook probes of the boot ROM; no documented device (§7.3) | — | [2] |
| $FFFFF0–$FFFFFF | Interrupt acknowledge: /VPA, automatic vectoring, no device activated | — | [1] p. 124 |

The exact base addresses in the table are *observed* in the Plus boot ROM [2] — the SCC primitives
are built on read base $9FFFF8 and write base $9FFFF9 stored into the SCCRd/SCCWr low-memory
globals, the SCSI primitives on $580000/$580001, the VIA block on $EFE1FE with the registers at
$200 strides, and the keyboard/RTC/overlay interface entirely through that VIA block — and they
match Apple's descriptions of the same registers [1] pp. 159–168, 363–364. The *bounds* of the SCC
and IWM windows (how far below the top of their megabytes each device still decodes) are not
established (§7.2).

### 3.2 Device select and byte lanes

Device selection is memory-mapped: "each time the processor reads or writes to a device, it places
an address on the address bus and asserts the address strobe … a special-purpose logic circuit …
(such as a PAL …) decodes the address to determine which device is being selected, and asserts the
device-select signal" [1] p. 34. The allocation is deliberately coarser than any device needs, and
Apple's own worked example is the family's SCSI decode:

> "the SCSI controller in the Macintosh Plus contains eight registers that can be selected by
> signals on address lines A6 through A4. Notice, however, that the address space allocated to each
> device is considerably larger than the minimum needed … For example, signals on address lines A18
> through A10, A8, A7, and A3 through A1 have no significance for the SCSI in the Macintosh Plus
> computer, so there are thousands of possible addresses that will access the same register."
> [1] p. 121

For the SCSI that means the significant lines are A23–A19 (selecting the $58xxxx window), A9
(register file at A9=0 versus the pseudo-DMA data aperture at A9=1, at $580200/$580201) and A6–A4
(the 5380's register number) — the address arithmetic the ROM's SCSI primitives actually perform
[2] (*observed*; the DCaD card-design book confirms the same register and pseudo-DMA offsets are
used by the Plus and SE SCSI chips [3] pp. 231, 235).

Byte lanes follow the /UDS//LDS rule of §2.2, and each device sits on one byte of the 16-bit bus
[1] p. 60:

| Device | Byte lane | Access rule |
|---|---|---|
| RAM, ROM | both bytes (word-wide) | word reads; ROM reads always one word [1] p. 68 |
| VIA | upper byte | "you must use even-addressed byte accesses only" [1] p. 160 |
| SCC | upper byte | even-addressed byte reads, odd-addressed byte writes; "a byte-wide read from an odd address resets the SCC" [1] p. 365 |
| IWM | lower byte | odd-addressed byte accesses [iwm-floppy.md](../../hardware/iwm-floppy.md) §"Macintosh Plus CPU-visible interface" |
| SCSI | lower byte | even addresses read, odd addresses write [1] p. 34; [2] (*observed*) |

The SCC's byte-lane arrangement is the source of a whole family of driver rules: the write base is
one above the read base, and Apple adds a timing rule — "on the Macintosh Plus and earlier
Macintosh models, it is necessary to let the SCC lines stabilize for $2.2 µs between accesses. On
later models … the general logic IC … delays the acknowledge signal until the SCC lines have
stabilized" [1] p. 364. The classic machines have no such delay hardware; the drivers carry it.

### 3.3 The phase read and the clock-phase correction

The interleaved RAM timing of §2.4 depends on the 15.6672 MHz and 7.8336 MHz clocks sitting in the
right phase relationship, and the machine exposes that relationship to software in a most unusual
way:

> "At system startup, the operating system reads an address in the range $F0 0000 through $F7 FFFF
> (labeled Phase read in Figures 3-1 and 3-2) to determine whether the computer's high-frequency
> timing signals are correctly in phase. When the timing signals are not in phase, RAM accesses are
> not timed correctly, causing an unstable video display, RAM errors, and VIA errors. A word-wide
> access to any SCC address causes a phase shift in the processor clock, and is used by the operating
> system to correct the phase when necessary." [1] p. 123

Apple repeats the warning as a developer tip: "be careful never to make a word-wide access to the
SCC … a word-wide access to any SCC address causes a phase shift in the processor clock in those
models" [1] p. 365. A word-wide SCC access is thus a *hardware command*, not an I/O transaction —
an accidental word write to the SCC can desynchronize the CRT refresh and RAM timing of the whole
machine.

The Plus ROM implements the check at the very top of the Reset handler, with interrupts masked
($2700): it reads three longwords from the phase-read window at $F00000, tests the low bit of each,
and if they do not add up to the expected count, performs a word-wide access to an SCC address to
nudge the phase before proceeding [2] (*observed*). What the phase-read window returns, and how the
128 ns-granularity correction accumulates, is not documented anywhere in the evidence set (§7.4).

### 3.4 RAM images, undecoded space, and the no-bus-error contract

Two properties of the family's decode are unusual enough to be load-bearing for re-implementations:

**RAM repeats through its window.** "The contents of RAM and ROM are repeated throughout unused
address space assigned to memory … If there is only 1 MB of RAM installed in the system, it appears
to software as if there are four identical RAM 'images' in memory; that is, the same data is fetched
by a read to any of the addresses $00 1000, $10 1000, $20 1000, or $30 1000" [1] p. 121. A 1 MB
Plus addresses RAM through $0FFFFF and mirrors it to $3FFFFF [1] Table 5-1 p. 191; a 512K machine
through $07FFFF, a 128K through $01FFFF. The mirrors are a property of unconnected address lines,
not a software convention: code that computes pointers beyond MemTop silently wraps into the
low image.

**There is no bus error.** "The PALs always generate either /DTACK or /VPA in response to a memory
access, even to an address space with no device. Of course, writing to an unoccupied address doesn't
change anything, and reading fetches meaningless data from an undriven bus. Because every access is
guaranteed by the design of the hardware to be successfully completed, the bus error signal (/BERR)
is not used in the classic Macintosh" [1] p. 124. This is the family's error contract: no fault, no
timeout, no retry — an access to nothing returns undefined data and completes. Software probing for
hardware (the SCSI-presence test in the Plus ROM reads the HWCfgFlags global rather than probing the
chip [2]) cannot rely on faults, and a re-implementation that raises bus errors where the hardware
returns garbage will diverge from every classic Macintosh driver.

## 4. Device roster

The complete part inventory, grouped by role. Registers and behaviour belong to the device pages;
this roster states what each part is and where it lives. Apple's own component list for the family
is [1] pp. 18–19 (the 128K/512K/512Ke list, plus the Plus additions), corroborated by Table 1-1
[1] pp. 2–3 and the per-subsystem chapters.

### 4.1 The system core

| Part | Role | Page |
|---|---|---|
| MC68000, 7.8336 MHz | the only processor; 16 MB address space, 16-bit bus | §2.2, [1] pp. 18, 90–91 |
| PALs (several) | address decode and device select, RAM RAS/CAS and MUX control, video timing and fetch, sound and disk-speed PWM, clock generation, interrupt routing to /IPL0 | §2.3, §5.1 |
| ROM, 64 KB or 128 KB in two byte-wide ICs | Toolbox and OS code, Reset handler | §2.5, [1] pp. 4, 68 |
| RAM, 128 KB–4 MB | the shared system/video/sound/disk-speed array | §2.4 |
| Video Shift register | serializes the word fetched by the PALs into the /VIDEO line to the analog board | §2.6, [1] p. 111 |

### 4.2 The I/O devices

| Part | Role | Where | Page |
|---|---|---|---|
| 6522 VIA (standard Rockwell or VTI part [1] p. 73) | keyboard interface, half the mouse interface, RTC serial interface, interrupt concentrator, screen/sound buffer selects, overlay control, sound volume and enable, floppy SEL, SCC /W/REQ monitor | $EFE000–$EFFFFF | [via.md](../../hardware/via.md) |
| Zilog Z8530 SCC | two RS-422 serial ports, the mouse interrupt inputs, and the clock-phase shifter | $9FFFF8/$BFFFF9 | [scc.md](../../hardware/scc.md) |
| IWM | floppy disk interface: GCR serial/parallel conversion, drive control; drive-select lines partly from the VIA | $DFE000–$DFFFFF | [iwm-floppy.md](../../hardware/iwm-floppy.md) |
| RTC (custom IC, on the VIA's 3-bit serial line) | 4-byte seconds counter, 1 Hz interrupt, parameter RAM (20 bytes on the 128K/512K, 256 bytes on the 512Ke/Plus [1] pp. 77, 144), battery-backed | VIA port B bits 0–2 | [rtc.md](../../hardware/rtc.md) |
| NCR 5380 SCSI controller (Plus only) | the external SCSI bus, polled — IRQ and DRQ not connected (§5.4) | $580000–$5FFFFF | [ncr-5380.md](../../hardware/scsi/ncr-5380.md), [plus.md](plus.md) |
| Sony sound IC | PWM integrator, 3-bit attenuator, speaker/jack driver; also the power-on /RESET supervisor (§2.8) | analog board | §2.7 |

### 4.3 The external interfaces

| Interface | Connector | Notes |
|---|---|---|
| Keyboard | four-wire RJ-11 telephone-style, front panel [1] p. 281 | bidirectional data line, keyboard-driven clock; the keyboard contains its own scanning microprocessor with private ROM and RAM [1] p. 280; protocol in [keyboard.md](../../hardware/keyboard.md) |
| Mouse | DB-9, back panel [1] Table 7-2 p. 278 | quadrature X1/Y1 to the SCC, X2/Y2 and the button to VIA port B [1] p. 275; wiring in [mouse.md](../../hardware/mouse.md) |
| Serial, two ports | DB-9 (128K/512K/512Ke), mini-DIN 8 (Plus) [1] p. 8 | RS-422 via the SCC [1] p. 357 |
| Floppy | one internal drive; external IWM connector on the 128K/512K/512Ke and Plus [1] pp. 19, 80 | 400 KB single-sided (128K/512K) or 800 KB double-sided (512Ke/Plus) [1] p. 18 |
| SCSI (Plus) | DB-25 external only [1] p. 376 | no terminator power on the connector [1] pp. 379–380 |

### 4.4 What the family does not carry

Worth stating explicitly, because every later Macintosh has them: no Apple Desktop Bus (the
keyboard and mouse are direct-wired, §4.3) [1] p. 75; no expansion slot of any kind [1] Table 1-1
p. 3; no second VIA [1] p. 73; no Apple Sound Chip (the ASC machines start with the SE/30 [1]
pp. 71–72); no SWIM (the IWM is the only floppy controller [1] p. 80); no MMU, FPU or coprocessor
[1] Table 1-1 p. 2; and no DMA of any kind [1] pp. 65–66.

## 5. Interrupt, bus, and clock architecture

### 5.1 The three interrupt sources and their levels

The family has exactly three interrupt sources — "the VIA, the SCC, and the programmer's interrupt
switch" [1] p. 92 — and they map onto the MC68000's three /IPL lines directly enough that the
*combination* of asserted sources is itself the priority level. Apple's complete table [1]
Table 3-1 pp. 92–93:

| Level | /IPL2 | /IPL1 | /IPL0 | Source | Auto-vector |
|---|---|---|---|---|---|
| 0 | 1 | 1 | 1 | none | — |
| 1 | 1 | 1 | 0 | VIA | $19 |
| 2 | 1 | 0 | 1 | SCC | $1A |
| 3 | 1 | 0 | 0 | SCC + VIA (transient) | $1B |
| 4 | 0 | 1 | 1 | programmer's interrupt switch | $1C |
| 5 | 0 | 1 | 0 | switch + VIA | $1D |
| 6 | 0 | 0 | 1 | switch + SCC | $1E |
| 7 | 0 | 0 | 0 | switch + SCC + VIA (transient) | $1F |

The wiring behind the table: the VIA's interrupt request "goes to the PALs, which can assert an
interrupt to the processor on line /IPL0. The PALs also monitor interrupt line /IPL1, and deassert
/IPL0 whenever /IPL1 is asserted" [1] p. 92 — which is why levels 3 and 7 "can appear only very
briefly before reverting to level 2 or 6" [1] p. 92. The SCC output is wired straight to /IPL1, the
programmer's switch straight to /IPL2. The highest source is the user-installed programmer's switch
on the left side of the case, used for debugging; it generates level-4 interrupts and "you cannot
generate nonmaskable (level-7) interrupts on the classic Macintosh computers" [1] p. 93 — the family
has no NMI at all.

Vectoring is automatic. The MC68000 acknowledges by driving $FFFFF0–$FFFFFF; "when the PALs
receive an address in the range $FF FFF0 to $FF FFFF, they assert /VPA, which causes the main
processor to jump to the location in memory containing the appropriate interrupt handler. When an
address in this range is read, no device is activated and any data read from the data bus is
ignored" [1] p. 124 — the vector number comes from the level itself, multiplied by four to index
the low-memory vector table [1] pp. 91, 124.

### 5.2 The VIA: the single interrupt concentrator

Everything time-critical on the board arrives through the one VIA, at level 1. Apple's function
list for the classic Macintosh VIA [1] pp. 151–152:

- keyboard interface (the shift register and its clock);
- part of the mouse interface (quadrature bits X2/Y2 and the button, §4.3);
- the RTC serial interface and the one-second interrupt (§4.2);
- the vertical blanking interrupt request (/VSYNC from the general logic circuits) — "this signal
  provides a 60.15 Hz interrupt, used by a variety of firmware and software" [1] p. 151;
- sound volume control and sound on/off;
- the sound/disk-speed buffer select and the screen buffer select;
- the screen's horizontal retrace monitor (vH4);
- the ROM overlay control (vOverlay, §2.8);
- the SCC /W/REQ monitor (vSCCWrReq), "so that the software can detect activity on the serial port
  when interrupts are disabled" [1] p. 152 — the /W/REQA and /W/REQB lines are wire-ORed together
  on the board [1] p. 160;
- the floppy state-control line SEL (vHeadSel) [1] pp. 151–152.

The classic VIA's port assignments are Apple's Tables 4-2 and 4-11 [1] pp. 160–161, 167–168,
reproduced with the driver-side semantics in [via.md](../../hardware/via.md) §"Peripheral Ports A
and B" and §"Interrupt Flag Register (IFR) - Register 13"; the interrupt sources map onto the VIA's
CA1 (vertical blanking), CA2 (the RTC's one-second tick), CB1/CB2 (keyboard clock and data), the
shift register (keyboard byte complete) and the two timers [1] pp. 183–184. The boot ROM enables
only the vertical blanking interrupt at reset (IER write $82 — bit 1, CA1) and clears the
peripheral control register to zero before the beep [2] (*observed*).

Two timers live in the VIA, both clocked at the E-clock rate of one tick per 1.2766 µs [1] pp. 73,
180–182, and both are spoken for: "Timer T1 of the classic Macintosh … VIA … is used by the Sound
Driver and Sound Manager. Timer T2 is used by the Disk Driver to time disk I/O events" [1] p. 182.
T1's free-running mode can toggle bit 7 of Data register B automatically, which is the documented
square-wave sound technique of the classic machines [1] pp. 179–180 — and which Apple warns stops
working on every later machine, because they lack the PWM sound enable bit [1] pp. 179, 429.

### 5.3 The SCC: serial, mouse and phase

The Z8530 carries the two serial ports and the mouse's motion interrupts. The mouse wiring is the
unusual part: "The interrupt signals, X1 and Y1, are connected to the DCDA and DCDB inputs,
respectively, of the SCC chip. The quadrature signals, X2 and Y2, go to inputs of the VIA's Data
register B. When the SCC receives a mouse-interrupt signal, it sends an interrupt signal to the
CPU. The mouse driver can then read bits in the SCC to determine which mouse-interrupt signal
caused the interrupt, and whether the interrupt was caused by a rising edge or a falling edge of
the signal" [1] p. 275 — the motion decoder is a software state machine split across two chips,
described in [mouse.md](../../hardware/mouse.md). Serial-port and AppleTalk traffic shares the
same /IPL1 line; the register file, read/write windows, recovery time and odd-read reset are
§3.2's, with the chip itself in [scc.md](../../hardware/scc.md).

### 5.4 The SCSI (Plus): polled, interrupt-less

The Plus's SCSI controller is wired to the bus but not to the interrupt system:

> "In the Macintosh Plus computer, neither of the NCR 5380 interrupt signals (IRQ and DRQ) is
> connected to the CPU: there is no hardware handshaking on the SCSI port. Instead, software must
> poll the Bus and Status register in the NCR 5380 to detect interrupt requests. Approximate
> maximum SCSI transfer rates within a block are 170 KB per second for polled transfers and 263 KB
> per second for blind transfers." [1] p. 394

This is the family's one deliberate omission that later machines correct: every Macintosh from the
SE on routes the 5380's IRQ and DRQ to a VIA and adds DRQ-based hardware handshaking in its general
logic IC, and the Plus's lack of it is why its SCSI is slow and its drivers poll [1] pp. 393–394.
There is consequently no SCSI interrupt level in Table 3-1's world — a level-1 VIA interrupt and a
level-2 SCC interrupt are all the machine can raise. The SCSI window, register file and pseudo-DMA
aperture are §3.2's; the chip is [ncr-5380.md](../../hardware/scsi/ncr-5380.md).

### 5.5 Clocks

One crystal times the entire machine, and every derived clock is a PAL output [1] p. 111:

| Clock | Frequency | Consumer |
|---|---|---|
| Master/pixel clock | 15.6672 MHz | the video shift chain; source of all divisions [1] p. 400 |
| CPU clock | 7.8336 MHz | the MC68000 [1] pp. 18, 111 |
| SCC clock | 3.672 MHz | the SCC's communication-rate reference [1] p. 111 |
| VIA E clock | 783.36 kHz (one timer tick per 1.2766 µs) | the VIA timers and shift register [1] pp. 73, 180 |
| Horizontal scan | 22.25 kHz (44.93 µs per line) | video, sound and disk-speed fetch cadence [1] p. 400 |
| Vertical scan | 60.15 Hz (370 lines) | the VBL interrupt and the sound buffer cycle [1] pp. 151, 401 |

The 60.15 Hz figure is exact enough to be a compatibility surface: sound buffer scanning (22.2545
kHz), the Ticks counter and every time-based driver hang off it [1] pp. 151, 431. The phase
relationship between the derived clocks is itself software-checkable and software-correctable — the
phase-read window and the SCC word-access trick of §3.3 exist because the PAL-generated clocks can
come up out of phase with each other after reset [1] p. 123.

The bus architecture underneath all of this is as simple as a Macintosh gets: one master (the CPU),
no DMA, no arbitration in the stock machine, every cycle either a CPU access or a PAL-steal for
video/sound/disk-speed (§2.2, §2.4). The MC68000's arbitration pins are exposed only to accelerator
cards, which use them to take the bus from the CPU [1] p. 91.

## 6. Per-machine index

The four members differ in exactly four dimensions — RAM, ROM, floppy drive and (on the Plus) SCSI
and connectors — and Apple's text pins each difference [1] p. 4. Everything else in this page is
common. The Plus is the family's only member with a machine page in this reference.

### 6.1 Macintosh 128K

The original configuration: 128 KB of soldered DRAM (16 64-Kbit ICs in two rows of eight), 64 KB of
ROM, and a single-sided 400 KB internal drive [1] pp. 4, 18, 196. RAM ends at $01FFFF [1]
Table 5-1 p. 191. Its parameter RAM is 20 bytes [1] pp. 77, 144. The 400 KB drive is the only member
that *depends* on the disk-speed PWM channel of §2.7 for its variable-speed format. Its ROM image
and version are not in the evidence set (§7.13).

### 6.2 Macintosh 512K

"Except for the additional memory, the Macintosh 512K is identical to the Macintosh 128K" [1] p. 4:
512 KB of soldered 256-Kbit DRAM, RAM ending at $07FFFF [1] pp. 4, 196, Table 5-1 p. 191. Same
64 KB ROM, same 400 KB drive, same 20-byte parameter RAM.

### 6.3 Macintosh 512K enhanced

The bridge to the Plus: "The ROM and disk drive in the Macintosh 512K enhanced computer are the
same as those used in the Macintosh Plus. In all other respects, the Macintosh 512K enhanced is
identical to the Macintosh 512K" [1] p. 4 — so it takes the 128 KB ROM (and its decode: ROM
answers to $41FFFF, §2.5), the 800 KB double-sided drive with internal speed control, and the
256-byte parameter RAM of the Plus [1] pp. 77, 144, while keeping the 512K's soldered RAM, DB-9
serial connectors and lack of SCSI. Whether its logic board carries any other Plus-era changes
beyond the ROM and drive is not established by the evidence set (§7.14).

### 6.4 Macintosh Plus

The family's fully specified member, and the one this reference emulates: 1 MB of SIMM RAM
expandable to 4 MB, the 128 KB ROM, the 800 KB drive, the SCSI port, mini-DIN 8 serial connectors
and the keypad keyboard [1] pp. 4, 8. Its machine doc, [plus.md](plus.md), carries the per-machine
facts: the SIMM geometry and configuration rules (two or four SIMMs, the row-pairing rules, the
RAM-size resistors, the 150 ns access requirement [1] pp. 197–202), the SCSI window in full (the
polled controller and the pseudo-DMA aperture, §3.2 and §5.4 here), the DB-25 connector and the
missing terminator power [1] pp. 379–380, and the power-supply specifications (85–135 V or
170–270 V input, four output rails, 46.8 W maximum continuous load [1] pp. 253, 258). The boot ROM
facts observed in this page — the overlay drop, the VIA initialization sequence, the phase check,
the IWM reset, the boot beep — are all Plus observations and transfer to the 512K enhanced's ROM
only where its image is known to share them (§7.13).

### 6.5 Where the family ends

The Macintosh SE is compact-cased but is not one of Apple's classic Macintosh computers [1] p. 4,
and its board is a different architecture: the BBU custom IC replaces the PALs, ADB replaces the
direct-wired keyboard and mouse, a 68000 PDS expansion connector appears, and the SCSI controller
gains an interrupt line [1] pp. 8, 49–51, 62, 75. This page deliberately stops at the Plus. The SE itself has no page in this reference; the
SE/30's general logic does, in [se30.md](../glue/se30.md). The Macintosh Classic and later
compact-cased machines are later still and equally outside.

## 7. Open questions

1. **PAL identities and equations.** No PAL part numbers, fuse maps or decode equations appear
   anywhere in the evidence set; Apple's function list [1] p. 111 is the entire published
   specification of the machine's control layer. The precise device-select equations — which
   address bits each select tests — are recoverable only from the observed windows (§3) and
   behaviour, and even the /DTACK-//VPA split's exact edge cases are untested.
2. **Decode granularity and aliasing of the I/O windows.** The bounds of the SCC read and write
   windows below their top-of-megabyte registers ($9FFFF8, $BFFFF9), the bounds of the IWM softswitch
   window above and below $DFE1FF, whether the VIA block decodes all of $EFE000–$EFFFFF or only the
   $200-stride register offsets, and how the SCSI window aliases within $580000–$5FFFFF beyond the
   documented don't-care lines [1] p. 121 are all unestablished. Only the ROM's observed access
   addresses [2] are pinned.
3. **The $F80000/$F80080 test-software probes.** The Plus Reset handler reads a magic constant
   ($55AAAA55) and a jump target from $F80000/$F80004 and again from $F80080/$F80084, and jumps if
   either matches [2] (*observed*). No device is documented anywhere in that range, on any classic
   Macintosh; what a stock machine returns there — open bus, a stable pattern, or factory
   test-equipment decode — is unknown, and it decides whether the hook can ever fire.
4. **The phase-read mechanism.** What the $F00000–$F7FFFF window returns (the ROM reads three
   longwords and tests their low bits [2]), what "in phase" means quantitatively, the size of one
   correction step (the disassembly annotation suggests 128 ns, but that figure is the annotator's,
   not Apple's [2]), and how many steps the ROM will take — none of this is documented beyond
   [1] pp. 123, 364.
5. **The odd-byte SCC read reset.** "A byte-wide read from an odd address resets the SCC" [1]
   p. 365 — but the extent of the reset (full chip state, write registers, or the interrupt
   latches) is not stated, and the boot ROM's phase-correction sequence reads a byte at $9FFFF7,
   one below the read window's first register [2], whose exact effect is unestablished.
6. **The alternate screen buffer's placement.** The main buffer is at MemTop − $5900 on the
   Plus [2] (*observed*), but the rule fixing the alternate buffer's address — the companion of the
   alternate sound buffer's SoundBase − $5C00 [1] p. 429 — appears in no source in the evidence
   set.
7. **VIA reset state and the PCR.** The power-on values of the VIA's port latches and Peripheral
   Control register are not documented; Apple only warns "do not change any of the bits in the
   Peripheral Control register" [1] p. 176, and the boot ROM writes zero to it [2] (*observed*).
   Which CA1/CA2 edge polarities the VBL and one-second interrupts actually use after that write
   is not stated anywhere.
8. **The 400 KB disk-speed channel in detail.** The encoding of the motor-speed byte in the low
   half of each sound-buffer word (pulse width per value, update cadence, the motor's tolerance)
   is described only qualitatively [1] pp. 72–73; the quantitative law lives in the Sony driver's
   tables, not in any Apple hardware document in evidence.
9. **The Sony sound IC.** No part number, attenuation law (dB per volume step) or output-stage
   specification appears in the evidence set; the chip is specified only by its function in
   [1] pp. 71, 428 and its secondary role as the power-on reset supervisor [1] p. 242.
10. **Keyboard and keypad model identity.** The Model Number command returns a keyboard model in
    bits 1–3 of the response [1] p. 283 Table 7-4; which models the 128K through Plus keyboards
    actually report (and the earlier separate-keypad devices' model numbers [1] pp. 284–286) is
    not established here, though the wire protocol is fully covered in
    [keyboard.md](../../hardware/keyboard.md).
11. **Determinism of undecoded reads.** The no-bus-error contract guarantees an access to nothing
    returns "meaningless data from an undriven bus" [1] p. 124; whether a given machine returns a
    stable value (and which) on a given undecoded address is a property of the specific board's
    bus termination, not documented anywhere.
12. **The 512K enhanced's board.** Apple defines it by its ROM and drive only [1] p. 4; whether its
    logic board is a 512K board with those two parts swapped, or carries Plus-era PAL or decode
    changes, is not established.
13. **The 128K/512K/512Ke ROM images.** The Plus image is identified by its header (version $75,
    checksum $4D1F8172 [2]); the earlier machines' ROM versions, checksums and boot-code
    differences are not in the evidence set, so the *observed* boot behaviours of this page are
    Plus-only facts.
14. **The SCC clock tree.** The 3.672 MHz SCC clock is a PAL output [1] p. 111, but the division
    chain from the 15.6672 MHz crystal to it, and which SCC input (PCLK versus RTxC) each serial
    port's baud generator actually uses on the classic machines, is not spelled out in the
    compact-Macintosh sections; [scc.md](../../hardware/scc.md) carries the chip-level picture.

## References

1. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition, Addison-Wesley,
   1990. Compact rows of the feature matrix Table 1-1 pp. 2–3 and the machine definitions p. 4;
   exterior views pp. 6–8; interior and component lists pp. 18–19; memory-mapped device selection p. 34
   and the SCSI don't-care lines p. 121; 24-bit address maps pp. 35–40; block diagrams pp. 49–51;
   data buses and RAM data-bus buffers p. 60; CPUs and device handshaking p. 61; interrupts
   pp. 62–63; general logic, RAM and ROM pp. 65–68; built-in video, screen buffers and sound
   circuits pp. 69–72; disk-speed control pp. 72–73; VIAs pp. 73–74; non-ADB keyboard and mouse
   pp. 74–75; ADB p. 75; RTC pp. 77–78; SCSI pp. 78–79; IWM pp. 80–81; SCC pp. 82–83; MC68000 and
   its interrupts pp. 90–93 (classic interrupt levels Table 3-1 pp. 92–93); PAL functions p. 111;
   address maps and the classic map pp. 120–124; RTC interface pp. 144–147; VIA functions
   pp. 151–152, register addresses pp. 159–160, Data register A Table 4-2 pp. 160–161, Data
   register B Table 4-11 pp. 167–168, PCR p. 176, ACR and timers pp. 179–182, processor-interrupt
   registers pp. 183–184; RAM addresses Table 5-1 p. 191, RAM sharing pp. 192–193, access rates
   p. 194, RAM configurations pp. 196–202; ROM configurations pp. 231–232; classic power-up
   p. 242, power connector Table 6-1 p. 246, AC input Table 6-7 p. 253, DC output Tables 6-12/6-13/
   6-14 p. 258; mouse and keyboard chapter pp. 273–286 (mouse Table 7-1 p. 276 and connector
   Table 7-2 p. 278; keyboard connector Table 7-3 p. 281, communication pp. 282–283, commands
   Table 7-4 p. 283); SCSI chapter pp. 376–395 (Plus tip p. 394); display scanning and circuits
   pp. 397–401; PWM sound pp. 427–431.
2. Macintosh Plus boot ROM, annotated 68000 disassembly (ROM version $75, header checksum
   $4D1F8172, 128 KB image). Header and boot vector at file offset $0–$2A; Reset handler phase
   check, screen-buffer and VIA initialization $62–$F8; boot beep $28A–$350; sound and screen
   buffer placement $37E–$3A4; SCC base equates (SCCRd $9FFFF8, SCCWr $BFFFF9) $82C–$83C; Sony
   driver poll setup $18BEA; SCSI register base $580000/$580001 and pseudo-DMA aperture $580200/
   $580201 in the SCSI primitives $17294–$173E6; test-software probes $42, $368.
3. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third edition,
   Addison-Wesley, 1992. SCSI-NuBus test card: register and pseudo-DMA offsets "the same as on a
   Macintosh SE or Macintosh Plus" p. 231; the card's SCSI chip "identical to that used in the
   Macintosh Plus" p. 235.
