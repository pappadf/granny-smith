# Serial Communications Controller (SCC) - Hardware Reference

## Overview

The Zilog Z8530 Serial Communication Controller (SCC) is a dual-channel, multiprotocol data communication peripheral used in the Macintosh Plus for serial communications. Each channel provides full-duplex serial communication with sophisticated on-chip support for various protocols including asynchronous, byte-oriented synchronous (Bisync), and bit-oriented synchronous (SDLC/HDLC).

The SCC contains two independent channels (A and B), each with its own set of registers, transmit and receive logic, and modem control signals. The Macintosh Plus uses Channel A for the printer port and Channel B for the modem port.

### Key Features

- Two independent full-duplex channels
- Synchronous/asynchronous data communication
- Up to 1/4 PCLK data rate with external clocking
- 5, 6, 7, or 8 bits per character
- Programmable parity (odd/even)
- Programmable stop bits (1, 1.5, 2)
- On-chip baud rate generators (BRG)
- Digital Phase-Locked Loops (DPLL) for clock recovery
- CRC generation and checking (CRC-16 and CRC-SDLC)
- FIFO buffering: 3-byte receive FIFO, 1-byte transmit buffer (NMOS version)
- Multiple data encoding modes: NRZ, NRZI, FM0, FM1
- Modem control signals: /RTS, /CTS, /DCD, /DTR

### Register Architecture

The SCC uses an indirect addressing scheme where Write Register 0 (WR0) acts as a pointer to select the target register for subsequent accesses. Each channel has:

- **Write Registers (WR0-WR15):** Control and configuration
- **Read Registers (RR0-RR15):** Status and data
- **Shared registers:** WR2 (interrupt vector) and WR9 (master control) are shared between channels

## Hardware Interface

### Memory-Mapped Registers

In the Macintosh Plus, the SCC is memory-mapped and accessed via the Universal Bus interface (Z85X30 variant). The addressing scheme uses:

- **D/C̅ (Data/Control):** Selects between data (WR8/RR8) and control registers
- **A/B̅ (Channel):** Selects Channel A or Channel B
- **R/W̅:** Read or write operation

The basic access sequence:
1. Write register pointer to WR0 (bits D2-D0 select register 0-7)
2. For registers 8-15, set "Point High" command in WR0 (bits D5-D3 = 001)
3. Access the target register

### Timing Requirements

The SCC requires a system clock (PCLK) for internal operation. All register accesses must meet setup and hold time requirements relative to PCLK. After a register write, the SCC requires a recovery time of at least 4 PCLK cycles before the next access.

### Interrupt Handling

The SCC generates interrupts for:
- Receive character available
- Transmit buffer empty
- External/status changes (modem lines, break, sync)
- Special receive conditions (parity error, overrun, framing error)

The interrupt system uses a daisy-chain priority scheme with these levels (highest to lowest):
1. Channel A receive
2. Channel A transmit
3. Channel A external/status
4. Channel B receive
5. Channel B transmit
6. Channel B external/status

WR2 contains the base interrupt vector, which is modified by status information when read from Channel B (if "Vector Includes Status" is enabled in WR9).

## Data Path Architecture

### Transmit Data Path

The transmit path consists of:
1. **Transmit Buffer (WR8):** 1-byte buffer loaded by CPU or DMA
2. **Transmit Shift Register:** 20-bit register for parallel-to-serial conversion
3. **CRC Generator:** Calculates CRC on transmitted data
4. **Zero Insertion Logic:** In SDLC mode, inserts a 0 after five consecutive 1s
5. **Data Encoder:** Applies NRZ, NRZI, FM0, or FM1 encoding
6. **TxD Output:** Serial data output pin

In synchronous modes, WR6 and WR7 hold sync characters or SDLC flags that are automatically inserted. The transmitter can append CRC automatically at end-of-message if the Tx Underrun/EOM latch is reset.

### Receive Data Path

The receive path consists of:
1. **RxD Input:** Serial data input pin
2. **Data Decoder:** Decodes NRZ, NRZI, FM0, or FM1 encoding
3. **DPLL:** Optional clock recovery from data stream
4. **Receive Shift Register:** Serial-to-parallel conversion
5. **Sync/Hunt Logic:** Searches for sync characters or SDLC flags
6. **Zero Deletion Logic:** In SDLC mode, removes stuffed zeros
7. **CRC Checker:** Validates received CRC
8. **Receive FIFO (RR8):** 3-byte deep buffer (NMOS/CMOS)
9. **Error FIFO (RR1):** Parallel status FIFO storing per-byte error information

The receiver operates in phases:
- **Hunt Mode:** Searching for sync character or flag
- **Sync Mode:** Synchronized, receiving data
- **Special Condition:** Error or end-of-frame detected

## Register Descriptions

### Write Registers

#### WR0 - Command Register

Controls register pointer and issues immediate commands.

**Bits D2-D0: Register Pointer** (0-7, or 8-15 with Point High command)

**Bits D5-D3: Command Codes**
- `000` - Null command
- `001` - Point High (access WR8-WR15)
- `010` - Reset External/Status interrupts
- `011` - Send Abort (SDLC mode)
- `100` - Enable interrupt on next Rx character
- `101` - Reset Tx interrupt pending
- `110` - Error reset
- `111` - Reset highest IUS (Interrupt Under Service)

**Bits D7-D6: CRC Reset Codes**
- `00` - Null code
- `01` - Reset Rx CRC checker
- `10` - Reset Tx CRC generator
- `11` - Reset Tx Underrun/EOM latch

#### WR1 - Tx/Rx Interrupt and Data Transfer Mode

Controls interrupt generation and DMA/Wait modes.

**Bit D0:** External/Status master interrupt enable  
**Bit D1:** Tx interrupt enable  
**Bit D2:** Parity is special condition  
**Bits D4-D3: Rx Interrupt Mode**
- `00` - Rx interrupt disabled
- `01` - Rx interrupt on first character or special condition
- `10` - Interrupt on all Rx characters or special condition
- `11` - Rx interrupt on special condition only

**Bit D5:** Wait/Request on Transmit or Receive (0=Tx, 1=Rx)  
**Bit D6:** Wait/DMA Request function (0=Wait, 1=Request)  
**Bit D7:** Wait/DMA Request enable

#### WR2 - Interrupt Vector

Contains the base interrupt vector (8 bits). This register is shared between both channels. When read from Channel B with VIS (Vector Includes Status) enabled, the vector is modified with status information.

#### WR3 - Receive Parameters and Control

**Bit D0:** Rx enable (must be set last in initialization)  
**Bit D1:** Sync character load inhibit (strips sync chars from data stream)  
**Bit D2:** Address search mode (SDLC - match address in WR6)  
**Bit D3:** Rx CRC enable  
**Bit D4:** Enter hunt mode (search for sync)  
**Bit D5:** Auto enables (/DCD enables receiver, /CTS enables transmitter)  
**Bits D7-D6: Rx bits per character**
- `00` - 5 bits
- `01` - 7 bits
- `10` - 6 bits
- `11` - 8 bits

#### WR4 - Tx/Rx Miscellaneous Parameters

**Bit D0:** Parity enable  
**Bit D1:** Parity even/odd (1=even, 0=odd)  
**Bits D3-D2: Stop bits**
- `00` - Sync modes enable
- `01` - 1 stop bit
- `10` - 1.5 stop bits
- `11` - 2 stop bits

**Bits D5-D4: Sync mode selection**
- `00` - 8-bit sync (Monosync)
- `01` - 16-bit sync (Bisync)
- `10` - SDLC mode
- `11` - External sync mode

**Bits D7-D6: Clock rate**
- `00` - x1 clock mode
- `01` - x16 clock mode
- `10` - x32 clock mode
- `11` - x64 clock mode

#### WR5 - Transmit Parameters and Control

**Bit D0:** Tx CRC enable  
**Bit D1:** RTS (Request To Send) control  
**Bit D2:** CRC-16/SDLC polynomial select (1=CRC-16, 0=SDLC)  
**Bit D3:** Tx enable  
**Bit D4:** Send break  
**Bits D6-D5: Tx bits per character**
- `00` - 5 or fewer bits
- `01` - 7 bits
- `10` - 6 bits
- `11` - 8 bits

**Bit D7:** DTR (Data Terminal Ready) control

#### WR6 - Sync Character or SDLC Address

Contains the transmit sync character (Monosync), first sync byte (Bisync), or SDLC station address for address matching.

#### WR7 - Sync Character or SDLC Flag

Contains the receive sync character (Monosync), second sync byte (Bisync), or SDLC flag pattern (01111110).

#### WR8 - Transmit Buffer

Write data to be transmitted. This is the primary data path for transmit data.

#### WR9 - Master Interrupt Control

Shared register controlling overall interrupt operation and reset.

**Bit D0:** VIS (Vector Includes Status) - modifies vector with status  
**Bit D1:** NV (No Vector) - disables vector generation  
**Bit D2:** DLC (Disable Lower Chain) - interrupt daisy chain control  
**Bit D3:** MIE (Master Interrupt Enable) - global interrupt enable  
**Bit D4:** Status high/low - controls vector modification bit positions  
**Bits D7-D6: Reset commands**
- `00` - No reset
- `01` - Reserved
- `10` - Channel reset
- `11` - Hardware reset

#### WR10 - Miscellaneous Tx/Rx Control

**Bit D0:** 6-bit/8-bit sync (0=8-bit, 1=6-bit)  
**Bit D1:** Loop mode (internal loopback)  
**Bit D2:** Abort/Flag on underrun (SDLC)  
**Bit D3:** Mark/Flag idle (0=mark idle, 1=flag idle)  
**Bit D4:** Go active on poll (SDLC loop mode)  
**Bits D6-D5: Data encoding**
- `00` - NRZ
- `01` - NRZI
- `10` - FM1 (biphase mark)
- `11` - FM0 (biphase space)

**Bit D7:** CRC preset (0=preset to 0s, 1=preset to 1s)

#### WR11 - Clock Mode Control

Selects clock sources for transmitter and receiver.

**Bits D1-D0: TRxC pin mode**  
**Bits D4-D3: Tx clock source**  
**Bits D6-D5: Rx clock source**  
**Bit D7:** RTxC XTAL/no XTAL (crystal oscillator enable)

Clock sources: /RTxC pin, /TRxC pin, BRG output, DPLL output

#### WR12 - Lower Byte of Baud Rate Generator Time Constant

Lower 8 bits of the 16-bit BRG time constant.

#### WR13 - Upper Byte of Baud Rate Generator Time Constant

Upper 8 bits of the 16-bit BRG time constant.

The baud rate is calculated as:

```
Baud Rate = Clock Frequency / (2 × Clock Mode × (Time Constant + 2))
```

Where Clock Mode is the multiplier from WR4 (1, 16, 32, or 64).

#### WR14 - Miscellaneous Control

**Bit D0:** BRG enable  
**Bit D1:** BRG source (0=/RTxC, 1=PCLK)  
**Bit D2:** DTR/Request function (0=DTR, 1=Request)  
**Bit D3:** Auto echo mode  
**Bit D4:** Local loopback  
**Bits D7-D5: DPLL commands**
- `000` - Null command
- `001` - Enter search mode
- `010` - Reset missing clock
- `011` - Disable DPLL
- `100` - Set source = BRG
- `101` - Set source = /RTxC
- `110` - Set FM mode
- `111` - Set NRZI mode

#### WR15 - External/Status Interrupt Control

Enables specific external/status interrupt sources.

**Bit D0:** WR7' access enable (ESCC/85C30 only)  
**Bit D1:** Zero count interrupt enable  
**Bit D3:** DCD interrupt enable  
**Bit D4:** Sync/Hunt interrupt enable  
**Bit D5:** CTS interrupt enable  
**Bit D6:** Tx underrun/EOM interrupt enable  
**Bit D7:** Break/Abort interrupt enable

### Read Registers

#### RR0 - Tx/Rx Buffer Status and External Status

**Bit D0:** Rx character available  
**Bit D1:** Zero count (BRG counter reached zero)  
**Bit D2:** Tx buffer empty  
**Bit D3:** DCD (Data Carrier Detect) status  
**Bit D4:** Sync/Hunt (synchronized or hunting)  
**Bit D5:** CTS (Clear To Send) status  
**Bit D6:** Tx underrun/EOM  
**Bit D7:** Break/Abort

#### RR1 - Special Receive Condition Status

Contains error flags associated with the character at the top of the receive FIFO.

**Bit D0:** All sent (transmit completely idle)  
**Bit D1:** Residue code 2 (SDLC)  
**Bit D2:** Residue code 1 (SDLC)  
**Bit D3:** Residue code 0 (SDLC)  
**Bit D4:** Parity error  
**Bit D5:** Rx overrun error  
**Bit D6:** CRC/Framing error  
**Bit D7:** End of frame (SDLC)

#### RR2 - Interrupt Vector

Returns the interrupt vector. When read from Channel B with VIS enabled, returns the modified vector with status information encoded in specific bit positions. When read from Channel A, returns the unmodified vector.

#### RR3 - Interrupt Pending Bits (Channel A only)

Shows which interrupt conditions are pending across both channels.

**Bit D0:** Channel B Ext/Status interrupt pending  
**Bit D1:** Channel B Tx interrupt pending  
**Bit D2:** Channel B Rx interrupt pending  
**Bit D3:** Channel A Ext/Status interrupt pending  
**Bit D4:** Channel A Tx interrupt pending  
**Bit D5:** Channel A Rx interrupt pending

#### RR8 - Receive Data Buffer

Returns the oldest character from the 3-byte receive FIFO.

#### RR10 - Miscellaneous Status

**Bit D0:** On loop (SDLC loop mode)  
**Bit D1:** Loop sending  
**Bit D2:** Two clocks missing (DPLL)  
**Bit D3:** One clock missing (DPLL)

#### RR12 - Lower Byte of BRG Time Constant

Returns the lower 8 bits of the BRG time constant.

#### RR13 - Upper Byte of BRG Time Constant

Returns the upper 8 bits of the BRG time constant.

## Ancillary Support Circuitry

### Baud Rate Generator (BRG)

Each channel contains a programmable baud rate generator consisting of:
- Two 8-bit registers (WR12, WR13) forming a 16-bit time constant
- 16-bit down counter
- Output flip-flop producing a square wave

The BRG is clocked from either the /RTxC pin or PCLK (selected via WR14 bit D1). The counter decrements on each clock cycle, and when it reaches zero, the output toggles and the counter reloads the time constant value.

**Time Constant Formula:**

```
TC = (Clock Frequency / (2 × Clock Mode × Baud Rate)) - 2
```

**Example:** For a 2.4576 MHz clock, x16 mode, and 9600 baud:
```
TC = (2457600 / (2 × 16 × 9600)) - 2 = 6
```

The BRG is enabled by setting WR14 bit D0. When first enabled, there is a synchronization delay of 1-2 clock cycles to prevent metastability.

### Digital Phase-Locked Loop (DPLL)

The DPLL recovers clock information from the received data stream, allowing clock recovery in synchronous modes without a separate clock line.

**Operating Modes:**
- **NRZI mode:** Requires 32x clock, for NRZ/NRZI data
- **FM mode:** Requires 16x clock, for FM0/FM1 data

**Clock Source:** BRG output or /RTxC pin (selected via WR14 bits D7-D5)

**Operation:**
1. The DPLL divides each bit cell into regions
2. Detects transitions in the data stream
3. Adjusts a 5-bit counter to align clock edges with bit cell centers
4. Generates receive and transmit clocks phase-aligned to the data

In NRZI mode, the counter normally counts 0-31. When a transition is detected:
- If early (counts 0-15.5): counter shortened by 1 (counts 31 next cycle)
- If on-time (counts 15.5-16.5): no adjustment
- If late (counts 16.5-31): counter extended by 1 (counts 33 next cycle)

The DPLL enters "search mode" when enabled (WR14 command 001) and locks onto the data stream once synchronized. Missing clock conditions (detected via edge timeout) are reported in RR10.

### Data Encoding/Decoding

The SCC supports four encoding methods (selected via WR10 bits D6-D5):

**NRZ (Non-Return to Zero):**
- Logic 1 = high level
- Logic 0 = low level
- Minimal clock information in data stream

**NRZI (Non-Return to Zero Inverted):**
- Logic 1 = no transition
- Logic 0 = transition
- Used in SDLC with zero stuffing to guarantee transitions

**FM1 (Biphase Mark):**
- Transition on every bit boundary
- Logic 1 = additional transition at bit center
- Logic 0 = no transition at bit center
- Self-clocking

**FM0 (Biphase Space):**
- Transition on every bit boundary
- Logic 0 = additional transition at bit center
- Logic 1 = no transition at bit center
- Self-clocking

## Communication Modes

### Asynchronous Mode

Selected by programming stop bits in WR4 (D3-D2 ≠ 00).

**Characteristics:**
- Start bit (space) marks beginning of character
- 5-8 data bits (LSB first)
- Optional parity bit
- 1, 1.5, or 2 stop bits (mark)
- Clock rate 1x, 16x, 32x, or 64x data rate

**Receiver operation:**
- Detects start bit (1→0 transition)
- Samples data in middle of bit cells
- Checks for stop bit (framing error if not mark)
- Optional parity checking

**Transmitter operation:**
- Inserts start bit
- Transmits data bits
- Calculates and appends parity if enabled
- Appends stop bits

**Error detection:**
- Parity error
- Framing error (invalid stop bit)
- Overrun (new character received before previous read)

### Monosync Mode

Selected by WR4 (D5-D4 = 00, D3-D2 = 00).

**Characteristics:**
- 6-bit or 8-bit sync character (selected via WR10 bit D0)
- Sync character in WR7 for receiver, WR6 for transmitter
- Byte-oriented synchronous protocol
- Optional CRC-16 or CRC-SDLC

**Receiver operation:**
1. **Hunt mode:** Searches for sync character match
2. Upon match, sets Sync/Hunt bit in RR0
3. **Sync mode:** Receives data characters
4. Optionally strips sync characters from data stream (WR3 bit D1)

**Transmitter operation:**
- Sends sync characters when idle
- Sends data when available in transmit buffer
- Optionally appends CRC at end of message

### Bisync Mode

Selected by WR4 (D5-D4 = 01, D3-D2 = 00).

**Characteristics:**
- 12-bit or 16-bit sync pattern (WR7:WR6)
- Two-byte sync sequence required for synchronization
- Similar to Monosync but with longer sync pattern

### SDLC/HDLC Mode

Selected by WR4 (D5-D4 = 10, D3-D2 = 00).

**Characteristics:**
- Flag-delimited frames (01111110)
- Zero bit stuffing/removal
- Address field matching (WR6)
- Automatic CRC-SDLC generation/checking
- Abort detection (7+ consecutive 1s)

**Frame Format:**
```
Opening Flag | Address | Control | Information | FCS | Closing Flag
  01111110   | 8 bits  | 8 bits  | variable    |16bit| 01111110
```

**Receiver operation:**
1. **Hunt mode:** Searches for flag
2. Upon flag detection, receives address byte
3. Optionally matches address against WR6 (address search mode)
4. Removes stuffed zeros (after five 1s)
5. Receives data and calculates CRC
6. Checks CRC at end of frame
7. Closing flag ends frame

**Transmitter operation:**
1. Sends opening flag (or continuous flags if idle)
2. Sends address, control, information fields
3. Inserts zero after five consecutive 1s
4. Calculates and appends CRC
5. Sends closing flag
6. Returns to flag idle or mark idle

**Special features:**
- Abort generation: Send 8-13 consecutive 1s (WR0 command 011)
- Abort detection: Receiver recognizes 7+ consecutive 1s
- Address search: Reject frames with non-matching address
- Shared zero-bit flag: Closing flag of one frame serves as opening flag of next

## Programming Sequences

### Initialization Sequence

1. **Hardware reset** (optional, via WR9 D7-D6 = 11)
2. **Channel reset** (via WR9 D7-D6 = 10)
3. Configure WR4: Set clock mode, sync mode, stop bits, parity
4. Configure WR10: Set data encoding, sync type
5. Configure WR11: Select clock sources
6. If using BRG: Configure WR12, WR13 (time constant), WR14 (enable BRG)
7. If using DPLL: Configure WR14 (clock source, mode, enable)
8. Configure WR3: Set Rx bits/char, disable receiver initially
9. Configure WR5: Set Tx bits/char, disable transmitter initially
10. Configure WR6, WR7: Set sync characters or SDLC address/flag
11. Configure WR1: Set interrupt modes
12. Configure WR15: Enable desired external/status interrupts
13. Configure WR9: Set master interrupt enable
14. **Enable receiver:** Set WR3 bit D0 (after hunt mode if sync)
15. **Enable transmitter:** Set WR5 bit D3

### Transmit Sequence

**Polling method:**
1. Check RR0 bit D2 (Tx buffer empty)
2. If set, write data to WR8
3. Repeat until all data sent
4. Issue "Reset Tx Underrun/EOM" command before last byte to disable auto-CRC

**Interrupt method:**
1. Enable Tx interrupt (WR1 bit D1)
2. Write first byte to WR8
3. On Tx interrupt:
   - Write next byte to WR8
   - If no more data, issue "Reset Tx interrupt pending" command

**End of message (with CRC):**
1. Write last data byte to WR8
2. Wait for Tx buffer empty
3. Issue "Reset Tx Underrun/EOM" command (WR0 D7-D6 = 11)
4. On next transmit interrupt, CRC is being sent
5. Check RR1 bit D0 (All sent) to confirm transmission complete

### Receive Sequence

**Polling method:**
1. Check RR0 bit D0 (Rx character available)
2. If set, read RR1 for error status
3. Read RR8 for data
4. Repeat until all data received

**Interrupt method:**
1. Enable Rx interrupt mode in WR1 (bits D4-D3)
2. On Rx interrupt:
   - Read RR1 for error status (if special condition mode)
   - Read RR8 for data
   - Check for end-of-frame (SDLC: RR1 bit D7)
   - Issue "Error Reset" command (WR0) if error occurred

**Special receive conditions:**
- **Overrun:** Data lost, issue Error Reset command
- **Parity error:** Flag set in RR1 bit D4
- **CRC error:** Flag set in RR1 bit D6
- **End of frame (SDLC):** Flag set in RR1 bit D7

### SDLC Frame Transmission

```c
// Initialize for SDLC mode
write_scc(chan, 0, 0x00);           // Select WR0
write_scc(chan, 4, 0x20);           // WR4: x1 clock, SDLC mode, sync enable
write_scc(chan, 10, 0x00);          // WR10: NRZ, CRC preset to 0
write_scc(chan, 6, station_addr);   // WR6: Station address
write_scc(chan, 7, 0x7E);           // WR7: SDLC flag
write_scc(chan, 5, 0xEA);           // WR5: 8 bits/char, Tx enable, RTS, CRC enable
write_scc(chan, 3, 0xC1);           // WR3: 8 bits/char, Rx enable

// Send frame
write_scc(chan, 8, address);        // Address field
write_scc(chan, 8, control);        // Control field
for (i = 0; i < info_len; i++) {
    while (!(read_scc(chan, 0) & 0x04));  // Wait for Tx buffer empty
    write_scc(chan, 8, info[i]);    // Information field
}
// CRC appended automatically due to Tx underrun
```

### SDLC Frame Reception

```c
// Wait for frame
while (!(read_scc(chan, 0) & 0x01));     // Wait for Rx char available

// Read frame
uint8_t address = read_scc(chan, 8);
uint8_t control = read_scc(chan, 8);
while (1) {
    while (!(read_scc(chan, 0) & 0x01)); // Wait for next char
    uint8_t status = read_scc(chan, 1);  // Check RR1
    if (status & 0x80) {                 // End of frame
        if (status & 0x40) {             // CRC error
            // Handle CRC error
        }
        break;
    }
    uint8_t data = read_scc(chan, 8);
    // Store data
}
write_scc(chan, 0, 0x30);                // Error reset command
```

## Emulation Implementation Notes

### Receive Buffering

The SCC hardware contains a 3-byte receive FIFO with corresponding error status for each byte. An accurate emulation must:

1. Maintain separate FIFO for receive data and error status
2. Implement proper FIFO depth (3 bytes for NMOS/CMOS)
3. Handle overrun condition when FIFO is full and new character arrives
4. Preserve error status associated with each character
5. Lock FIFO on special condition in certain interrupt modes
6. Unlock FIFO when Error Reset command issued

For SDLC/LocalTalk operation, consider implementing an additional frame queue to buffer multiple incoming frames, as the real hardware timing allows back-to-back frame reception.

### Transmit Operation

1. Implement Tx Underrun/EOM latch state
2. Handle automatic CRC transmission on underrun
3. Implement Tx interrupt timing (immediate vs. delayed)
4. Support abort generation in SDLC mode
5. Handle RTS deassertion timing
6. Support continuous sync/flag transmission when idle

### Interrupt State Machine

1. Maintain separate IP (Interrupt Pending) and IUS (Interrupt Under Service) bits
2. Implement interrupt priority resolution
3. Handle vector modification based on status
4. Support "Reset Highest IUS" command
5. Implement proper External/Status interrupt latching and re-enabling

### Clock and Timing

1. PCLK-based timing for register access recovery
2. BRG counter operation and output generation
3. DPLL clock recovery (if needed for protocol)
4. Proper synchronization of mode changes to clock boundaries

### Register State

1. Maintain full register state for all WR0-WR15
2. Handle shared registers (WR2, WR9) correctly
3. Implement register pointer (WR0) for indirect addressing
4. Handle read-only, write-only, and read-write registers
5. Implement proper reset behavior (hardware, channel, software)

### Special Modes

1. **SDLC mode:** Zero insertion/deletion, flag recognition, CRC calculation, abort handling
2. **Loop mode:** Internal loopback for testing
3. **Auto echo:** Received data automatically retransmitted
4. **Local loopback:** Tx output connected to Rx input internally

### Error Conditions

Properly detect and report:
- Parity errors
- Framing errors
- Overrun (Rx FIFO full)
- Underrun (Tx buffer empty at wrong time)
- CRC errors
- Abort reception
- Break detection

---

## Original Implementation Notes

### Receive buffering

The original SCC emulation only exposed a single staging buffer (`ch->sdlc_in`) that was overwritten whenever `scc_sdlc_send()` was invoked. If two LocalTalk frames were injected back-to-back (e.g., a PAP OpenReply immediately followed by an ATP status request), the later frame would clobber the earlier one before `check_rx()` had a chance to deliver it to the guest, producing spurious ATP retransmits.

To make the SCC behave more like the hardware 8530, each channel now owns a small pending-frame queue (`RX_PENDING_QUEUE_DEPTH = 8`). `scc_sdlc_send()` copies every injected frame into this queue and logs an overflow if the queue is full. A helper (`scc_schedule_rx_if_ready`) drains the queue whenever the receiver is enabled, in hunt mode, and not already processing a frame. This helper is invoked after enqueue, when WR3 enables RX/enters hunt, and after the guest consumes a frame (when `check_rx` clears `sdlc_in.len`).

### Interrupt behavior

Delivery still flows through the existing `check_rx()` path, so RR0/RR1/RR3 bits and IRQ behavior remain unchanged. The only visible difference is that multiple injected frames are serialized naturally instead of being dropped. A level-5 trace (`scc:rx enqueue`, `scc:rx dequeue`, `scc:rx deliver`) now shows the lifecycle: enqueue, queued depth, dequeue, and final delivery.

### Design goals

- Preserve all higher-level AppleTalk state machines: buffering lives in the SCC where the real hardware would queue bytes.
- Avoid large RAM growth: depth 8 × 1 KB covers LocalTalk bursts without noticeable footprint.
- Keep logging actionable: overflow cases emit level-1 warnings so we can catch unexpected drops.
