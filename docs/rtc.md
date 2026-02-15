# Macintosh Plus Real-Time Clock (RTC) and Parameter RAM

## Overview

The Macintosh Plus uses a battery-backed Real-Time Clock (RTC) chip (Apple part
number **343-0042-B**) that provides both timekeeping functionality and
non-volatile parameter RAM (PRAM) storage. This document describes the low-level
hardware interface and protocol for emulator implementers.

**Note:** Earlier Macintosh models used a smaller NVRAM chip with part number
343-0040.

## References

1. Guide to the Macintosh Family Hardware
2. Inside Macintosh III
3. Inside Macintosh - Operating System Utilities (1994) chapter 7
4. A/UX Toolbox: Macintosh ROM Interface
5. RTC/PRAM mechanism in Linux m68k kernel (arch/m68k/mac/misc.c)

## Hardware Interface

### VIA Port B Connections

The RTC is accessed through three pins on VIA1 (6522 VIA) Port B:

| VIA Pin | Bit | Signal   | Direction     | Description                           |
| ------- | --- | -------- | ------------- | ------------------------------------- |
| PB0     | 0   | vRTCData | Bidirectional | Serial data line                      |
| PB1     | 1   | vRTCClk  | Output        | Serial clock signal                   |
| PB2     | 2   | vRTCEnb  | Output        | Chip enable (active low: 0 = enabled) |

**VIA1 Base Address:** Defined in the system's decoder info, typically accessed
via the `VIA` low-memory global.

**Register Offsets from VIA Base:**

- `vBufB` (0x0000) - Data register for Port B
- `vDirB` (0x0400) - Data direction register for Port B

## Communication Protocol

### Basic Protocol

The RTC uses a **synchronous serial bit-banging protocol**:

1. **Chip Enable:** Clear bit 2 of VIA Port B (vRTCEnb = 0) to enable the chip
2. **Data Transfer:** Send/receive 8 bits via vRTCData (bit 0), clocking each
   bit with vRTCClk (bit 1)
3. **Chip Disable:** Set bit 2 of VIA Port B (vRTCEnb = 1) to disable the chip

**Important:** All RTC operations must be performed with **interrupts masked at
level 7** ($0700) to prevent timing issues.

### Bit Transmission (Write)

For each bit to be transmitted (MSB first):

```
1. Load bit into vRTCData (bit 0 of Port B)
2. Clear vRTCClk (bit 1 = 0) - clock low
3. Set vRTCClk (bit 1 = 1) - clock high (data latched on rising edge)
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

For each bit to be received (MSB first):

```
1. Set vRTCData direction to INPUT in vDirB
2. Clear vRTCClk (bit 1 = 0) - clock low
3. Set vRTCClk (bit 1 = 1) - clock high
4. Read bit from vRTCData (bit 0 of Port B)
5. Shift bit into result register
6. Restore vRTCData direction to OUTPUT when done
```

## Command Format

### Standard Commands (8-bit)

Commands are 8 bits with the following format:

```
Bit 7: R/W (1 = Read, 0 = Write)
Bits 6-5: Register group selector
Bits 4-2: Address bits (specific register)
Bits 1-0: Function bits
```

**Common command patterns:**

- Bits 6-3 = `0111` (0x38): Extended write command prefix
- Bits 6-3 = `1011` (0xB8): Extended read command prefix

**Command pattern matching:**

- For pattern `z010aa01` (PRAM 0x10-0x13): Check `(cmd & 0x70) == 0x20` (bits
  6-5-4 = 010, bits 3-2 are address)
- For pattern `z1aaaa01` (PRAM 0x00-0x0F): Check `(cmd & 0x40) == 0x40` (bit 6 =
  1, bits 5-2 are address)

### Extended Commands (16-bit)

For accessing the full PRAM space (including bytes beyond the original 20),
extended commands use a two-byte sequence:

**First byte format (bits 6-3 = `0011` for extended):**

```
Read:  1011 1xxx (0xB8 + address_bits[7:5])
Write: 0011 1xxx (0x38 + address_bits[7:5])
```

**Second byte format:** Contains additional addressing information.

### PRAM Address Encoding

To read/write a PRAM byte at address `N` (0-255):

**For WRITE:**

```
1. Calculate command word:
   address_word = N << 3          ; Shift address left 3 bits [oooo o765 4321 0ooo]
   address_word = ROR.B #1        ; Rotate right by 1 [oooo o765 o432 10oo]
   address_word = ROR.W #8        ; Rotate word right by 8 [o432 10oo oooo o765]
   command_word = address_word | 0x0038  ; Add write bits [o432 10oo ooii i765]

2. If bits 6-3 = 0x3 (extended): Send first byte, then second byte
3. Send data byte
```

**For READ:**

```
1. Calculate command word (same as write but OR with 0x00B8):
   address_word = N << 3
   address_word = ROR.B #1
   address_word = ROR.W #8
   command_word = address_word | 0x00B8  ; Add read bits [o432 10oo ioii i765]

2. If bits 6-3 = 0xB (extended): Send first byte, then second byte
3. Read data byte
```

## Special Commands

### Write Protection Control

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

### Time Registers

The RTC stores time as seconds since January 1, 1904 (Mac epoch), in a 32-bit
value split across 4 registers.

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

The original Mac Plus RTC provides 20 bytes of PRAM (addresses 0x00-0x13),
though extended addressing allows access to 256 bytes. The complete PRAM memory
map is as follows:

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
timing-sensitive, but consistent timing should be maintained.

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
