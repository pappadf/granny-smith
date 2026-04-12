# Macintosh Real-Time Clock (RTC) and Parameter RAM

## Overview

Classic Macintosh computers use a battery-backed Real-Time Clock (RTC) chip that
provides both timekeeping functionality and non-volatile parameter RAM (PRAM)
storage. This document describes the low-level hardware interface and protocol
for emulator implementers.

All three emulated machines — Macintosh Plus, SE/30, and IIcx — share the same
custom Apple RTC silicon (die number **0042**), communicate through an identical
3-wire serial protocol bit-banged via VIA1 Port B bits 0–2, and each contain
**256 bytes** of battery-backed PRAM. The only differences are the physical IC
package, the VIA1 memory-mapped base address, and the serial clock speed driven
by the host CPU.

## Chip Identity

The RTC/PRAM is a **fully custom Apple ASIC** — no off-the-shelf equivalent
exists and no formal datasheet was ever published. Apple internally designated
the die as **0042**. It appears under several part number variants depending on
manufacturing batch and package:

| Machine              | Part Number(s)   | Package | Battery               |
| -------------------- | ---------------- | ------- | --------------------- |
| **Macintosh Plus**   | 344-0042-A       | DIP-8   | 4.5 V PX21 alkaline   |
| **Macintosh SE/30**  | 343-0042-B       | DIP-8   | 3.6 V ½ AA lithium    |
| **Macintosh IIcx**   | 344S0042-B       | PLCC-20 | 3.6 V ½ AA lithium    |

The **343- vs. 344- prefix** indicates different suppliers or fabrication
processes, not a functional difference. The **-A / -B suffix** denotes die
revisions or production batches. The **S** in "344S0042" likely indicates CMOS
fabrication. All variants are functionally identical: same register set, same
protocol, same 256 bytes of PRAM.

The DIP-8 pinout, reconstructed from schematics and ATtiny85 replacement
projects, is:

| Pin | Function                                      |
| --- | --------------------------------------------- |
| 1   | TEST / N.C.                                   |
| 2   | XTAL1 (32.768 kHz crystal input)              |
| 3   | XTAL2 (32.768 kHz crystal output)             |
| 4   | GND                                           |
| 5   | CE — Chip Enable / Select (active low)        |
| 6   | D — Bidirectional serial data                 |
| 7   | SK — Serial clock (input from host)           |
| 8   | VCC (from battery or PSU via protection diodes)|

An earlier chip, the **344-0040**, was used in the Mac 128K and 512K and
contained only **20 bytes** of PRAM. The 0042 replaced it starting with the Mac
512Ke and Mac Plus, expanding to 256 bytes while remaining backward-compatible
with the 20-byte command set.

## References

1. Guide to the Macintosh Family Hardware
2. Inside Macintosh III
3. Inside Macintosh - Operating System Utilities (1994) chapter 7
4. A/UX Toolbox: Macintosh ROM Interface
5. RTC/PRAM mechanism in Linux m68k kernel (arch/m68k/mac/misc.c)
6. MAME, Mini vMac, and PCE emulator source code
7. ATtiny85 replacement projects: MacRTC.c (Andrew Makousky),
   attinyrtc / attinyrtcmodule (Phil Greenland)
8. Open-source KiCad SE/30 motherboard schematic reproduction
9. Logic analyzer captures at quantulum.co.uk

## Hardware Interface

### VIA Port B Connections

The RTC is accessed through three pins on VIA1 (6522 VIA) Port B. This
assignment is identical across the Plus, SE/30, and IIcx. The IIcx has a second
VIA (VIA2), but the RTC connects exclusively to VIA1.

| VIA1 Port B Bit | Signal     | Direction        | RTC Chip Pin |
| ---------------- | ---------- | ---------------- | ------------ |
| Bit 0            | vRTCData   | Bidirectional    | Pin 6 (D)    |
| Bit 1            | vRTCClk    | Output (host→RTC)| Pin 7 (SK)   |
| Bit 2            | vRTCEnb    | Output (active low)| Pin 5 (CE) |

The remaining Port B bits serve different functions depending on the model. On
the Plus, bits 3–7 handle mouse input and sound enable. On the SE/30 and IIcx,
bits 3–5 handle ADB (Apple Desktop Bus) and bits 6–7 handle other system
functions. The RTC bits 0–2 never change.

### VIA1 Base Addresses

VIA1 base addresses differ by model:

| Machine          | VIA1 Base Address | Notes                              |
| ---------------- | ----------------- | ---------------------------------- |
| **Macintosh Plus**| 0xEFE1FE         | 68000-era memory map               |
| **SE/30, IIcx**  | 0x50000000        | Mac II–class I/O base              |

Registers are spaced at 0x200-byte intervals on SE/30 and IIcx.

**Register Offsets from VIA Base:**

- `vBufB` (0x0000) - Data register for Port B
- `vDirB` (0x0400) - Data direction register for Port B

## Communication Protocol

### Basic Protocol

The RTC uses a **custom synchronous serial interface** — not SPI, not I²C,
though it resembles both. Three signal lines carry all communication: chip
enable (CE, active low), a bidirectional data line (D), and a host-driven clock
(SK). All transfers are initiated by the host. Data is clocked **MSB first**.
The RTC samples incoming data on the **rising edge** of SK. During reads, the
RTC drives data onto the line on the **falling edge** of SK, allowing the host
to sample it on the subsequent rising edge.

**Regular command (works on both 20-byte and 256-byte chips):**

1. **Chip Enable:** Clear bit 2 of VIA Port B (vRTCEnb = 0) to enable the chip
2. **Data Transfer:** Host clocks out an 8-bit command/address byte
3. Host clocks out (write) or RTC clocks out (read) an 8-bit data byte
4. **Chip Disable:** Set bit 2 of VIA Port B (vRTCEnb = 1) to disable the chip

**Important:** All RTC operations must be performed with **interrupts masked at
level 7** ($0700) to prevent timing issues.

The RTC chip itself is purely a clock-follower and operates at whatever speed
the host drives. On the Mac Plus with its **8 MHz 68000**, the bit-bang loop
runs slowly enough that each serial clock cycle is ~50–1000 µs, yielding a
serial clock frequency of roughly **1–20 kHz**. On the SE/30 and IIcx with
their **16 MHz 68030**, the same loop executes much faster, producing a serial
clock of approximately **250 kHz** — about 12× faster.

### Bit Transmission (Write)

For each bit to be transmitted (MSB first), the host sets DDRB bit 0 to output
mode, then:

```
1. Drive vRTCClk low
2. Set vRTCData to the bit value
3. Drive vRTCClk high (data latched on rising edge)
```

The actual implementation uses a rotate-and-extend technique:

```assembly
ADDX.B  D1,D1           ; Shift next data bit into extend bit
ROR.B   #1,D0           ; Position for vRTCData bit
ADDX.B  D0,D0           ; Move extend bit to bit 0
MOVE.B  D0,VBufB(A2)    ; Write to VIA
BCLR    #vRTCClk,vBufB(A2)  ; Clock low
BSET    #vRTCClk,vBufB(A2)  ; Clock high
```

### Bit Reception (Read)

The host switches DDRB bit 0 to input mode (0), releasing the data line. For
each of 8 data bits (MSB first):

```
1. Drive vRTCClk high
2. Drive vRTCClk low (RTC places next bit on falling edge)
3. Read bit from vRTCData (bit 0 of Port B)
4. Shift bit into result register
```

Restore vRTCData direction to OUTPUT when done.

## Command Format

### Standard Commands (8-bit)

The command byte format is:

```
Bit:  7      6    5    4    3    2    1    0
    [R/W]  [A4] [A3] [A2] [A1] [A0] [ 1] [ 0]
```

**Bit 7** selects read (1) or write (0). **Bits 6–2** form a 5-bit register
address (0x00–0x1F). **Bits 1–0** are always fixed as binary `10`.

**Command pattern matching:**

- For pattern `z010aa01` (PRAM 0x10-0x13): Check `(cmd & 0x70) == 0x20` (bits
  6-5-4 = 010, bits 3-2 are address)
- For pattern `z1aaaa01` (PRAM 0x00-0x0F): Check `(cmd & 0x40) == 0x40` (bit 6 =
  1, bits 5-2 are address)

### Extended Commands (16-bit)

On the 256-byte chip, regular command addresses 0x0E and 0x0F trigger extended
mode. The extended transaction adds a second command byte, creating a 16-bit
command followed by an 8-bit data byte:

1. Host asserts CE low
2. Host clocks out first command byte (8 bits)
3. Host clocks out second command byte (8 bits)
4. Data byte clocked in or out (8 bits)
5. Host de-asserts CE high

The two command bytes encode the full 8-bit XPRAM address:

```
First byte:   [R/W] [0] [0] [1] [1] [A7] [A6] [A5]
Second byte:  [ x ] [A4][A3][A2][A1][A0] [ x ] [ x ]
```

Bits 6–3 of the first byte are fixed as `0011`, which the chip recognizes as
the extended command prefix (this corresponds to address 0x0E or 0x0F in the
regular 5-bit address space). The **upper 3 bits** (A7–A5) of the XPRAM address
occupy bits 2–0 of the first byte. The **lower 5 bits** (A4–A0) occupy bits 6–2
of the second byte. The read/write flag remains in bit 7 of the first byte.

The 20 bytes of traditional PRAM accessible via regular commands map to the same
addresses via extended commands, providing full backward compatibility.

## Special Commands

### Write Protection

The **write-protect register** at address 0x0D is write-only. Setting bit 7 to
1 enables writes to all other registers; setting it to 0 locks them. The Mac ROM
disables write-protect before modifying the clock or PRAM, then re-enables
protection afterward.

**Disable Write Protection (required before writing):**

```
Command: 0x35 0x55  (two-byte extended command)
```

**Enable Write Protection (after writing):**

```
Command: 0x35 0xD5  (two-byte extended command)
```

**Clear Test Mode (initialization):**

```
Command: 0x31 0x00  (write zero to test register)
```

## RTC Registers

### Full Register Map

The RTC's internal register space, addressed via the 5-bit regular command
address, contains four functional regions:

| Address   | Contents                          | Notes                                 |
| --------- | --------------------------------- | ------------------------------------- |
| 0x00–0x03 | **Seconds counter** (32-bit)      | Byte 0 = LSB, seconds since Jan 1 1904|
| 0x04–0x07 | Seconds counter (mirror)          | Same 32-bit counter, may be read-only |
| 0x08–0x0B | PRAM group 1 (4 bytes)            | "Low" traditional PRAM                |
| 0x0C      | Test register                     | Factory use only (write-only)         |
| 0x0D      | **Write-protect register**        | Bit 7: 0 = protected, 1 = writes OK  |
| 0x0E–0x0F | Extended command prefix           | Triggers XPRAM addressing             |
| 0x10–0x1F | PRAM group 2 (16 bytes)           | "High" traditional PRAM               |

### Time Registers

The RTC stores time as seconds since January 1, 1904 (Mac epoch), in a 32-bit
value driven by the external **32.768 kHz watch crystal**. The maximum value
(0xFFFFFFFF) corresponds to **February 6, 2040, 6:28:15 AM** — the "year 2040
problem" for classic Macs.

**Command Pattern:** `z00*0001`, `z00*0101`, `z00*1001`, `z00*1101` where:

- `z` = bit 7 (1 for read, 0 for write)
- `00` = bits 6-5 must be 0
- `*` = bit 4 is "don't care" (can be 0 or 1)
- Final 4 bits identify the register (0001, 0101, 1001, 1101)

| Register    | Read Commands | Write Commands | Description            |
| ----------- | ------------- | -------------- | ---------------------- |
| Time byte 3 | 0x8D, 0x9D    | 0x0D, 0x1D     | Most significant byte  |
| Time byte 2 | 0x89, 0x99    | 0x09, 0x19     |                        |
| Time byte 1 | 0x85, 0x95    | 0x05, 0x15     |                        |
| Time byte 0 | 0x81, 0x91    | 0x01, 0x11     | Least significant byte |

**Note:** Both command variants (with bit 4 clear or set) are valid due to the
"don't care" bit in the command pattern.

**Reading time:** Read all 4 bytes (high to low), verify by reading twice and
comparing.

**Writing time:**

1. Disable write protection (0x35 0x55)
2. Write bytes 0-3 (low to high) using commands 0x01/0x11, 0x05/0x15, 0x09/0x19,
   0x0D/0x1D (either variant)
3. Enable write protection (0x35 0xD5)
4. Verify by reading back

### PRAM Layout

The original RTC chip provides 20 bytes of traditional PRAM (4 at 0x08–0x0B
plus 16 at 0x10–0x1F), while extended addressing allows access to the full 256
bytes. These 20 bytes are copied to low memory at addresses **$1F8–$20B** during
boot as the `SysParmType` record.

#### Standard PRAM (0x00-0x13)

| Address | Size | Use         | Description                                             |
| ------- | ---- | ----------- | ------------------------------------------------------- |
| 0x00    | 1    | SPValid     | Validity status/checksum                                |
| 0x01    | 1    | SPATalkA    | AppleTalk node ID hint for modem port (SCC Port A)      |
| 0x02    | 1    | SPATalkB    | AppleTalk node ID hint for printer port (SCC Port B)    |
| 0x03    | 1    | SPConfig    | Serial port configuration bits                          |
| 0x04    | 2    | SPPortA     | SCC modem port configuration (word)                     |
| 0x06    | 2    | SPPortB     | SCC printer port configuration (word)                   |
| 0x08    | 1    | SPVolCtl    | Speaker volume & click sound state                      |
| 0x09    | 1    |             | Reserved                                                |
| 0x0A    | 2    | SPClikCaret | Caret blink time, double-click time (2 four-bit values) |
| 0x0C    | 1    |             | Menu blink count, boot drive hint                       |
| 0x0D    | 1    | SPMisc2     | Mouse scaling, startup disk, menu blink values          |
| 0x0E    | 1    |             | Various system settings                                 |
| 0x0F    | 1    |             | Printer connection port                                 |
| 0x10    | 4    |             | Finder information                                      |

**Note:** Time is stored in separate dedicated registers (not in PRAM). Due to
the "don't care" bit 4 in the command pattern, each register can be accessed
with two command variants:

- **Write:** 0x01/0x11, 0x05/0x15, 0x09/0x19, 0x0D/0x1D (bit 4 clear/set)
- **Read:** 0x81/0x91, 0x85/0x95, 0x89/0x99, 0x8D/0x9D (bit 4 clear/set)

#### Low Memory Global Variable Mapping

PRAM contents are also accessible through low memory global variables at the
following addresses:

| Variable    | Address | Size | Description                                         |
| ----------- | ------- | ---- | --------------------------------------------------- |
| SPValid     | 0x1F8   | 1    | Validation field (byte)                             |
| SPATalkA    | 0x1F9   | 1    | AppleTalk node number hint for port A               |
| SPATalkB    | 0x1FA   | 1    | AppleTalk node number hint for port B               |
| SPConfig    | 0x1FB   | 1    | Configuration bits: 4-7 A, 0-3 B                    |
| SPPortA     | 0x1FC   | 2    | SCC port A configuration (word)                     |
| SPPortB     | 0x1FE   | 2    | SCC port B configuration (word)                     |
| SPAlarm     | 0x200   | 4    | Alarm time (long)                                   |
| SPFont      | 0x204   | 2    | Default application font number minus 1 (word)      |
| SPKbd       | 0x206   | 2    | Keyboard repeat threshold in 4/60ths (2 four-bit)   |
| SPPrint     | 0x207   | 1    | Print stuff (byte)                                  |
| SPVolCtl    | 0x208   | 1    | Volume control (byte)                               |
| SPClikCaret | 0x209   | 2    | Double-click/caret time in 4/60ths (2 four-bit)     |
| SPMisc1     | 0x20A   | 1    | Miscellaneous (1 byte)                              |
| SPMisc2     | 0x20B   | 1    | Mouse scaling, sys startup disk, menu blink (1 byte)|

#### Extended PRAM (xPRAM, 0x14-0xFF)

**Extended PRAM** is available on machines with the newer clock chip (indicated
by hwCbExPRAM bit in hardware config).

| Address   | Size | Description                                                         |
| --------- | ---- | ------------------------------------------------------------------- |
| 0x14      | 4    | Startup screen patterns                                             |
| 0x18      | 12   | MultiFinder memory allocations                                      |
| 0x78-0x7F | 8    | Control panel settings                                              |
| 0x80-0xCF | 80   | Application-specific storage                                        |
| 0xD8      | 1    | AppleTalk node ID (printer port)                                    |
| 0xD9      | 1    | AppleTalk node ID (modem port)                                      |
| 0xDA      | 1    | AppleTalk zone hint                                                 |
| 0xF8      | 1    | **Startup device SCSI ID** (0-6 for SCSI, 0xFF for internal floppy) |
| 0xF9      | 1    | SCSI bus flags and configuration                                    |
| 0xFA-0xFF | 6    | System reserved                                                     |

_Note: This represents key locations. Some bytes may be application-defined or
system-reserved._

## Complete Command Sequences

### Example: Read PRAM Byte at Address 0x0C

```
1. Disable interrupts (SR = 0x2700)
2. Clear vRTCEnb (enable chip)
3. Send command byte:
   - Address 0x0C: shift and encode to 0xB9 (read, standard command)
   - Send 8 bits: 1-0-1-1-1-0-0-1
4. Read data byte:
   - Set vRTCData to input
   - Read 8 bits (MSB first)
   - Restore vRTCData to output
5. Set vRTCEnb (disable chip)
6. Restore interrupts
```

### Example: Write PRAM Byte at Address 0x08

```
1. Disable interrupts (SR = 0x2700)
2. Clear vRTCEnb (enable chip)
3. Send write-protect disable: 0x35, 0x55
4. Set vRTCEnb, then clear again
5. Send command byte:
   - Address 0x08: encode to 0x39 (write, standard command)
   - Send 8 bits: 0-0-1-1-1-0-0-1
6. Send data byte: 8 bits (MSB first)
7. Set vRTCEnb, then clear again
8. Send write-protect enable: 0x35, 0xD5
9. Set vRTCEnb (disable chip)
10. Restore interrupts
```

## Timing Considerations

### Clock Speed

The VIA runs at **783,360 Hz** (783.36 kHz). The RTC protocol is not highly
timing-sensitive — the chip is purely a clock-follower and operates at whatever
speed the host drives.

| Machine          | CPU         | Serial Clock Speed | Notes                 |
| ---------------- | ----------- | ------------------ | --------------------- |
| **Macintosh Plus**| 8 MHz 68000 | ~1–20 kHz         | ~50–1000 µs per cycle |
| **SE/30, IIcx**  | 16 MHz 68030| ~250 kHz           | ~12× faster than Plus |

This speed difference is significant for replacement chips (e.g., ATtiny85
microcontrollers at 8 MHz can handle the Plus's slow clock but fail on the
SE/30's faster clock without being overclocked to 16 MHz).

### Requirements

1. **Interrupt Masking:** All RTC operations must disable interrupts at level 7
   to prevent timing disruptions
2. **Chip Enable:** The vRTCEnb line must be properly toggled between operations
3. **Clock Transitions:** Each bit requires a low-to-high clock transition
4. **Settling Time:** Brief delays between operations may be needed for chip
   state changes

### Source Code Reference

The actual timing is implemented through the instruction execution time rather
than explicit delays. The critical section is:

```assembly
MOVE    SR,D2                    ; Save SR
ORI     #$0700,SR                ; Mask all interrupts
; ... perform RTC operation ...
MOVE    D2,SR                    ; Restore SR
```

## 1-Second Interrupt

The RTC generates a **1-second interrupt** signal connected to VIA1, which the
operating system uses to increment its software clock. This mechanism is
identical across all three machines.

## Implementation Notes for Emulators

### Essential Features

1. **Persistent Storage:** PRAM must be saved to disk/file and restored on
   emulator restart
2. **Time Keeping:** RTC must accurately track real-world time when emulator is
   running
3. **Write Protection:** Implement write protection to catch software bugs
4. **Validation:** Implement checksum validation for PRAM (byte 0x13)

### Extended Command Detection

Commands with bits [6:3] having values `0011` (write) or `1011` (read) and bit
pattern not matching standard commands indicate extended addressing:

- Check if `(command & 0x78) == 0x38` for extended write
- The command is sent as two bytes for extended operations

### Default PRAM Values

On first boot or PRAM reset, initialize with these defaults (from `PRAMInitTbl`
in source):

- Checksum byte (0x13): 0xA8
- Volume (0x08): 0x03 (medium volume)
- Double-click time: reasonable default
- Other system parameters as per System 7 defaults

### Common Pitfalls

1. **Bit Order:** Bits are transmitted MSB first
2. **Interrupt Masking:** Forgetting to disable interrupts causes protocol
   errors
3. **Chip Enable:** Must be toggled properly (active low)
4. **Write Protection:** Must be disabled before any write operation
5. **Read Direction:** vRTCData must be set to INPUT for reading

## Protocol State Machine

### Write Operation State Machine

```
State 1: IDLE
  ↓ (start write)
State 2: DISABLE_INTERRUPTS
  ↓
State 3: ENABLE_CHIP (vRTCEnb = 0)
  ↓
State 4: SEND_WRITE_PROTECT_OFF (0x35, 0x55)
  ↓
State 5: DISABLE_CHIP (vRTCEnb = 1)
  ↓
State 6: ENABLE_CHIP (vRTCEnb = 0)
  ↓
State 7: SEND_COMMAND (8 bits)
  ↓
State 8: SEND_DATA (8 bits)
  ↓
State 9: DISABLE_CHIP (vRTCEnb = 1)
  ↓
State 10: ENABLE_CHIP (vRTCEnb = 0)
  ↓
State 11: SEND_WRITE_PROTECT_ON (0x35, 0xD5)
  ↓
State 12: DISABLE_CHIP (vRTCEnb = 1)
  ↓
State 13: RESTORE_INTERRUPTS
  ↓
State 14: IDLE
```

### Read Operation State Machine

```
State 1: IDLE
  ↓ (start read)
State 2: DISABLE_INTERRUPTS
  ↓
State 3: ENABLE_CHIP (vRTCEnb = 0)
  ↓
State 4: SEND_COMMAND (8 bits)
  ↓
State 5: READ_DATA (8 bits, vRTCData as input)
  ↓
State 6: DISABLE_CHIP (vRTCEnb = 1)
  ↓
State 7: RESTORE_INTERRUPTS
  ↓
State 8: IDLE
```

## System-Level Interface (A-Traps)

For operating system compatibility, emulators must implement the undocumented
A-Traps used by Mac OS to access PRAM. These provide high-level block read/write
operations without requiring the OS to implement the low-level serial protocol.

### \_ReadXPRam ($A051)

Reads a block of bytes from PRAM into a memory buffer.

**Calling Convention:**

- **A0 (Address Register):** Pointer to destination buffer in main RAM
- **D0.W (Low Word):** Starting offset in PRAM (0-255)
- **D0.W (High Word):** Number of bytes to transfer (1-256)
- **Returns:** D0.L = 0 for success, non-zero for error

**Example (68000 Assembly):**

```assembly
LEA     pram_buffer,A0    ; A0 -> destination buffer
MOVE.L  #$01000000,D0     ; Transfer 256 bytes (high word) from offset 0 (low word)
DC.W    $A051             ; _ReadXPRam trap
```

### \_WriteXPRam ($A052)

Writes a block of bytes from a memory buffer to PRAM.

**Calling Convention:**

- **A0 (Address Register):** Pointer to source buffer in main RAM
- **D0.W (Low Word):** Starting offset in PRAM (0-255)
- **D0.W (High Word):** Number of bytes to transfer (1-256)
- **Returns:** D0.L = 0 for success, non-zero for error

**Implementation Notes:**

- These traps automatically handle write protection (disable/enable sequences)
- They use the low-level serial protocol documented above
- Emulators should implement these traps to maintain OS compatibility

## Quick Reference Card

### VIA Port B Bits

- Bit 0: vRTCData (bidirectional)
- Bit 1: vRTCClk (output)
- Bit 2: vRTCEnb (output, active low)

### Command Construction

```
Standard Read:  0xB8 | ((addr & 0x1F) << 2)
Standard Write: 0x38 | ((addr & 0x1F) << 2)
Extended: Use full encoding formula above
```

### Write Protection

```
Disable: 0x35 0x55
Enable:  0x35 0xD5
```

### Time Registers

```
Read:  0x9D (MSB), 0x99, 0x95, 0x91 (LSB)
Write: 0x01 (LSB), 0x05, 0x09, 0x0D (MSB)
```
