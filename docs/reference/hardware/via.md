# Rockwell R6522/Synertek SY6522 VIA in the Macintosh Plus

## Overview

The Macintosh Plus incorporates a single **Synertek SY6522 Versatile Interface Adapter (VIA)**, also manufactured by Rockwell and VTI (as the R6522), as a critical component for I/O and system control. This chip manages a wide array of functions, acting as the interface between the MC68000 processor and various hardware components like the keyboard, mouse, real-time clock, and sound circuitry.

**Key Features:**
- Two 8-bit bidirectional I/O ports (Port A and Port B)
- Two 16-bit programmable timer/counters (T1 and T2)
- Serial bidirectional peripheral I/O (Shift Register)
- TTL compatible peripheral control lines (CA1, CA2, CB1, CB2)
- Input data latching capability on both ports
- Comprehensive interrupt control system

For emulator development, it's important to note that while direct hardware access is possible, Macintosh software was strongly encouraged to use **Macintosh Toolbox calls** for hardware control to ensure forward compatibility.

## Accessing the VIA

The VIA is a memory-mapped device, meaning the processor communicates with it by reading from and writing to specific memory addresses.

### Address and Timing

* **Base Address**: The VIA's 16 internal registers are accessed in a 4 KB block of memory. The base address is available in the global variable VIA and through the assembly-language constant vBase, which is **$EFE1FE**.  
* **Bus and Timing**: The VIA resides on the **upper byte** of the data bus, so software must use **even-addressed byte accesses only**. Communications are synchronized to the **E clock** (Φ2 clock), a 783.36 kHz signal from the processor. This makes VIA access relatively slow, averaging about **1.0 µs** per operation.
* **Chip Selects**: The VIA is selected when CS1 is high and !CS2 is low. The R/!W line controls read/write direction.

### Reset Behavior

When RESET (!RES) is asserted, the VIA:
- Clears all internal registers **except** T1 and T2 counters/latches and the Shift Register
- Places all peripheral interface lines (PA and PB) in the input state (high impedance)
- Disables the Timers (T1 and T2), Shift Register, and interrupt logic

### Register Map

The table below summarizes the addresses of the internal registers as offsets from vBase. The operating system sets up the Data Direction Registers (DDRA and DDRB) at startup.

| Register \# | Hex Offset | Symbolic Offset | Register Name | Write Operation | Read Operation |
| :---- | :---- | :---- | :---- | :---- | :---- |
| 0 | $000 | vBufB | **ORB / IRB** | Output Register B | Input Register B |
| 1 | $200 | vBufA | **ORA / IRA (Handshake)** | Output Register A | Input Register A |
| 2 | $400 | vDirB | **DDRB** | Data Direction Register B | Data Direction Register B |
| 3 | $600 | vDirA | **DDRA** | Data Direction Register A | Data Direction Register A |
| 4 | $800 | vT1C | **T1C-L** | T1 Low-Order Latches | T1 Low-Order Counter |
| 5 | $A00 | vT1CH | **T1C-H** | T1 High-Order Counter | T1 High-Order Counter |
| 6 | $C00 | vT1L | **T1L-L** | T1 Low-Order Latches | T1 Low-Order Latches |
| 7 | $E00 | vT1LH | **T1L-H** | T1 High-Order Latches | T1 High-Order Latches |
| 8 | $1000 | vT2C | **T2C-L** | T2 Low-Order Latches | T2 Low-Order Counter |
| 9 | $1200 | vT2CH | **T2C-H** | T2 High-Order Counter | T2 High-Order Counter |
| 10 | $1400 | vSR | **SR** | Shift Register | Shift Register |
| 11 | $1600 | vACR | **ACR** | Auxiliary Control Register | Auxiliary Control Register |
| 12 | $1800 | vPCR | **PCR** | Peripheral Control Register | Peripheral Control Register |
| 13 | $1A00 | vIFR | **IFR** | Interrupt Flag Register (write clears) | Interrupt Flag Register |
| 14 | $1C00 | vIER | **IER** | Interrupt Enable Register | Interrupt Enable Register |
| 15 | $1E00 | vBufA | **ORA / IRA (No Handshake)** | Output Register A | Input Register A |

## Peripheral Ports A and B

The VIA features two 8-bit bidirectional ports, Port A and Port B. Each pin can be independently configured as an input or an output on a pin-by-pin basis.

### Data Direction Registers (DDRA and DDRB)

Located at vBase+vDirA (register 3) and vBase+vDirB (register 2), these registers control the direction of each pin on the corresponding port.

* Writing a **0** to a bit configures the corresponding pin as an **input** (high impedance).  
* Writing a **1** to a bit configures the corresponding pin as an **output**.
* Direction can be selected on a line-by-line basis with intermixed input and output lines within the same port.

### Output Registers (ORA and ORB)

**Writing to Output Registers:**
- When a pin is configured as an output (DDR bit = 1), writing a 1 or 0 to the corresponding bit in the Output Register (ORA or ORB) sets the pin's voltage level high or low.
- Writing to a bit whose pin is configured as an input (DDR bit = 0) stores the value in the output register but has no immediate effect on the pin. The value will take effect if the pin is later reconfigured as an output.

### Input Registers (IRA and IRB)

**Reading from Port Registers:**

Reading from the port address transfers the contents of the Input Register (IRA or IRB) to the processor. However, the two ports behave differently:

#### Port A (IRA) - Pin-Level Reading

* **For input pins (DDRA bit = 0)**: Reading reflects the actual voltage level on the pin.
* **For output pins (DDRA bit = 1)**: Reading **still reflects the actual voltage level on the pin**, not necessarily the value written to ORA.
  * **Critical for emulation**: If an output pin is heavily loaded and cannot drive the intended voltage level, the read value might not match the value written to ORA.
  * This behavior can cause problems in practice but is useful for detecting hardware faults.

#### Port B (IRB) - Mixed Reading
 Port A Register (vBufA) - Macintosh Plus Assignments

This register controls various output functions and monitors the SCC Wait/Request line.

| Bit | Name | Direction | Description |
| :---- | :---- | :---- | :---- |
| **7** | vSCCWrReq | Input | Monitors the SCC's Wait/Request line. 1=Wait, 0=Request. Used to manage serial I/O when interrupts are off. |
| **6** | vPage2 | Output | Selects the screen buffer: 1=Main Buffer, 0=Alternate Buffer. |
| **5** | vHeadSel | Output | The **SEL** line to the floppy disk drive. Selects the read/write head on double-sided drives. |
| **4** | vOverlay | Output | When set (1), switches to the ROM overlay address map. **This is used only during system startup.** |
| **3** | vSndPg2 | Output | Selects the sound/disk-speed buffer: 1=Main Buffer, 0=Alternate Buffer. |
| **2-0** | vSound | Output | A 3-bit value controlling sound volume (111=Loudest, 000=Muted). |

**Data Direction (DDRA) typical startup value**: `0x7F` (bit 7 input, bits 6-0 output)

### Port B Register (vBufB) - Macintosh Plus Assignments

This register is primarily used for input from the mouse and for interfacing with the Real-Time Clock (RTC).

| Bit | Name | Direction | Description |
| :---- | :---- | :---- | :---- |
| **7** | vSndEnb | Output | Enables sound: 0=Sound Enabled, 1=Sound Disabled. This bit can be toggled automatically by Timer 1\. |
| **6** | vH4 | Input | Monitors the horizontal blanking signal from the video circuits. 1 indicates the blanking period. |
| **5** | vY2 | Input | Reads the mouse's vertical (Y-axis) quadrature signal for direction detection. |
| **4** | vX2 | Input | Reads the mouse's horizontal (X-axis) quadrature signal for direction detection. |
| **3** | vSW | Input | Reads the mouse button state: 0=Pressed, 1=Released. |
| **2** | rTCEnb | Output | Enables the Real-Time Clock serial interface: 0=Enabled. |
| **1** | rTCClk | Output | Provides the data clock signal for the RTC serial interface. |
|  Event Timers (T1 and T2)

The VIA has two independent 16-bit counter/timers that decrement at the E clock (Φ2) rate, meaning each count corresponds to **1.2766 µs** on the Macintosh Plus (783.36 kHz clock). Both can generate interrupts upon timeout.

**Important Emulation Note**: When setting a timer's 16-bit value, software must perform **two separate 8-bit byte writes** (one for the high-order byte and one for the low-order byte). A single 16-bit word write will not work correctly.

### Timer 1 (T1)

T1 is a flexible timer used primarily by the Sound Driver in the Macintosh Plus. It is the most feature-rich timer in the VIA.

#### Architecture

* **16-bit Counter**: T1C-H (high byte) and T1C-L (low byte)
* **16-bit Latch**: T1L-H (high byte) and T1L-L (low byte)
* The processor does **not** write directly into T1C-L. Instead, the low-order counter is automatically loaded from T1L-L when the processor writes to T1C-H.

#### Register Operations

**Register 4 (T1C-L)** - T1 Low-Order Counter:
- **Write**: Loads value into T1L-L latch only. The latch contents transfer to the counter when T1C-H is written (register 5).
- **Read**: Transfers current counter value to CPU. **Also clears the T1 interrupt flag (IFR bit 6).**

**Register 5 (T1C-H)** - T1 High-Order Counter:
- **Write**: Loads value into T1C-H directly and simultaneously transfers T1L-L into T1C-L, starting the countdown. **Also clears the T1 interrupt flag (IFR bit 6).**
- **Read**: Transfers current counter value to CPU. Does not affect interrupt flag.

**Register 6 (T1L-L)** - T1 Low-Order Latch:
- **Write**: Loads value into T1L-L latch. No effect on running counter or countdown.
- **Read**: Reads the latch value. Does **not** clear interrupt flag (unlike reading register 4).

**Register 7 (T1L-H)** - T1 High-Order Latch:
- **Write**: Loads value into T1L-H latch. No effect on running counter or countdown.
- **Read**: Reads the latch value.

#### Modes of Operation
 Shift Register (SR)

The VIA includes an 8-bit Shift Register for serial communication. The Macintosh Plus uses it exclusively for the **keyboard interface**.

### Hardware Interface

* **CB2**: Bidirectional serial data line
* **CB1**: Serial clock (can be input or output depending on mode)

### Register 10 - Shift Register

**Shift Direction:**
- **Shift Out**: Bit 7 is shifted out first (MSB first), and is simultaneously rotated back into bit 0.
-  Control Registers

### Auxiliary Control Register (ACR) - Register 11

Located at vBase+vACR, this register controls the operating modes of the timers, shift register, and input latching.

**Register Bit Map:**

| Bits | Function | Description |
| :---- | :---- | :---- |
| **7-6** | **T1 Control** | Timer 1 operating mode |
| **5** | **T2 Control** | Timer 2 operating mode |
| **4-2** | **Shift Register Control** | Shift register mode (8 modes) |
| **1** | **PB Latching** | Port B input latching enable |
| **0** | **PA Latching** | Port A input latching enable |

**T1 Timer Control (Bits 7, 6):**

|  Interrupt System

The VIA is the source of all **level 1** processor interrupts on the Macintosh Plus. It consolidates seven internal interrupt sources into a single !IRQ line to the MC68000.

### Interrupt Architecture

**!IRQ Output:**
- Open-drain (open-collector) output that can be wire-ORed with other devices.
- Goes low when any enabled interrupt flag is set.
- Controlled by the logical OR of all enabled interrupt conditions:

  `IRQ = (IFR6 × IER6) + (IFR5 × IER5) + (IFR4 × IER4) + (IFR3 × IER3) + (IFR2 × IER2) + (IFR1 × IER1) + (IFR0 × IER0)`

  where × = AND, + = OR

### Interrupt Flag Register (IFR) - Register 13

Located at vBase+vIFR, this register's bits are set to 1 when a specific interrupt condition occurs.

**Register Bit Map:**

| Bit | Interrupt Source | Macintosh Plus Assignment | Set When | Cleared By |
|-----|------------------|---------------------------|----------|------------|
| **7** | **IRQ Status** | Master interrupt status | Any enabled interrupt flag (bits 0-6) is set | All active flags cleared |
| **6** | **Timer 1** | Sound/General timing | T1 underflows ($0000 → $FFFF) | Read T1C-L or Write T1C-H or Write 1 to IFR bit 6 |
| **5** | **Timer 2** | Disk I/O timing | T2 underflows ($0000 → $FFFF) | Read T2C-L or Write T2C-H or Write 1 to IFR bit 5 |
| **4** | **CB1** | Keyboard clock | Active transition on CB1 | Read or Write ORB or Write 1 to IFR bit 4 |
| **3** | **CB2** | Keyboard data | Active transition on CB2 | Read or Write ORB (unless independent mode) or Write 1 to IFR bit 3 |
| **2** | **Shift Register** | Keyboard data ready | 8 shifts completed | Read or Write SR or Write 1 to IFR bit 2 |
| **1** | **CA1** | Vertical blanking | Active transition on CA1 (from /VSYNC) | Read or Write ORA or Write 1 to IFR bit 1 |
| **0** | **CA2** | One-second tick | Active transition on CA2 (from RTC) | Read or Write ORA (unless independent mode) or Write 1 to IFR bit 0 |

**IFR Bit 7 (IRQ Status):**
- This bit is **not a flag** and cannot be directly set or cleared.
- When read, it returns 1 if any interrupt condition is active (any enabled flag in bits 0-6 is set).
- It automatically becomes 0 when all active interrupt flags are cleared or all active interrupts are disabled.

#### Reading IFR

- Reading IFR returns the current state of all interrupt flags.
- Bit 7 reflects the overall interrupt status (!IRQ line state).

#### Writing to IFR (Clearing Flags)

Flags can be cleared in two ways:

1. **Specific clearing action** (as listed in the table above)
2. **Writing 1 to the flag bit** in IFR:
   - Writing a 1 to a bit position clears that flag.
   - Writing a 0 to a bit position has no effect (leaves the flag unchanged).
   - **Common technique**: Read IFR, then write the same value back to clear all currently active flags.

**Example:**
```assembly
; Clear all active interrupt flags
MOVE.B  vIFR(A0),D0     ; Read current flags
MOVE.B  D0,vIFR(A0)     ; Write back to clear them all
```

#### Independent Interrupt Input Mode Exception

When CA2 or CB2 is configured as an "independent interrupt input" (PCR bits set to x01 or x11 for that control line):
- Reading or writing the port register (ORA/ORB) does **NOT** clear the CA2/CB2 interrupt flag.
- The flag must be cleared by writing a 1 to IFR bit 0 or 3.

### Interrupt Enable Register (IER) - Register 14

Located at vBase+vIER, this register enables or disables each of the seven interrupt sources.

**Function:**
- An interrupt flag in the IFR will only trigger !IRQ if its corresponding bit in the IER is set to 1.
- This allows selective control of which interrupt sources can actually interrupt the processor.

**Register Bit Map:**

| Bit | Interrupt Source | Function |
|-----|------------------|----------|
| **7** | **Set/Clear Control** | Write: 1=enable bits, 0=disable bits<br>Read: Always returns 1 |
| **6** | **Timer 1** | T1 interrupt enable |
| **5** | **Timer 2** | T2 interrupt enable |
| **4** | **CB1** | CB1 interrupt enable |
| **3** | **CB2** | CB2 interrupt enable |
| **2** | **Shift Register** | SR interrupt enable |
| **1** | **CA1** | CA1 interrupt enable |
| **0** | **CA2** | CA2 interrupt enable |

#### Writing to IER

The IER has a special write protocol to allow selective setting and clearing of individual bits:

**To Enable Interrupts:**
- Write a byte with bit 7 = 1.
- Each bit in positions 0-6 that is set to 1 will **enable** the corresponding interrupt.
- Each bit in positions 0-6 that is set to 0 will be **left unchanged**.

**To Disable Interrupts:**
- Write a byte with bit 7 = 0.
- Each bit in positions 0-6 that is set to 1 will **disable** the corresponding interrupt.
- Each bit in positions 0-6 that is set to 0 will be **left unchanged**.

**Examples:**
```assembly
; Enable Timer 1 and CA1 interrupts without affecting others
MOVE.B  #$C2,vIER(A0)    ; Bit 7=1 (enable), bit 6=1 (T1), bit 1=1 (CA1)

; Disable Timer 2 interrupt without affecting others
MOVE.B  #$20,vIER(A0)    ; Bit 7=0 (disable), bit 5=1 (T2)

; Enable all interrupts
MOVE.B  #$FF,vIER(A0)    ; Bit 7=1, all other bits=1

; Disable all interrupts
MOVE.B  #$7F,vIER(A0)    ; Bit 7=0, all other bits=1
```

#### Reading from IER

- When read, the IER returns the current enable state for each interrupt source in bits 0-6.
- **Bit 7 will always be read as 1** (regardless of what was written).

### Interrupt Priority and Service

**Hardware Priority:**
- The VIA does not have hardware-prioritized interrupt vectors.
- All seven interrupt sources share a single !IRQ output.

**Software Priority:**
- The processor must read the IFR to determine which interrupt source(s) caused the interrupt.
- Software typically checks flags in order from highest to lowest priority.

**Typical Macintosh Plus Priority Order:**
1. VBL (CA1) - Highest priority
2. One-second tick (CA2)
3. Timer 1 (sound)
4. Timer 2 (disk)
5. Keyboard (CB1, CB2, SR) - Lowest priority
|-----|------|---------|-----------|
| 1 | PB | 0 | Latching disabled |
| 1 | PB | 1 | Latching enabled (triggered by CB1 active edge) |
| 0 | PA | 0 | Latching disabled |
| 0 | PA | 1 | Latching enabled (triggered by CA1 active edge) |

### Peripheral Control Register (PCR) - Register 12

Located at vBase+vPCR, this register configures the behavior of the four control lines (CA1, CA2, CB1, CB2) for handshaking and interrupt generation.

**Important**: Changing these bits from their OS-defined startup values is not recommended in Macintosh software.

**Register Bit Map:**

| Bits | Function | Description |
|------|----------|-------------|
| **7-5** | **CB2 Control** | CB2 operating mode (8 modes) |
| **4** | **CB1 Control** | CB1 active edge select |
| **3-1** | **CA2 Control** | CA2 operating mode (8 modes) |
| **0** | **CA1 Control** | CA1 active edge select |

**CB2 Control (Bits 7, 6, 5):**

| PCR7 | PCR6 | PCR5 | Mode |
|------|------|------|------|
| 0 | 0 | 0 | Input mode - interrupt on negative edge |
| 0 | 0 | 1 | Independent interrupt input - negative edge* |
| 0 | 1 | 0 | Input mode - interrupt on positive edge |
| 0 | 1 | 1 | Independent interrupt input - positive edge* |
| 1 | 0 | 0 | Handshake output |
| 1 | 0 | 1 | Pulse output |
| 1 | 1 | 0 | Low output (manual control) |
| 1 | 1 | 1 | High output (manual control) |

**CB1 Control (Bit 4):**

| PCR4 | CB1 Active Edge | Latching Behavior |
|------|-----------------|-------------------|
| 0 | Negative edge | IRB latches on negative edge of CB1 (if ACR1=1) |
| 1 | Positive edge | IRB latches on positive edge of CB1 (if ACR1=1) |

**CA2 Control (Bits 3, 2, 1):**

| PCR3 | PCR2 | PCR1 | Mode |
|------|------|------|------|
| 0 | 0 | 0 | Input mode - interrupt on negative edge |
| 0 | 0 | 1 | Independent interrupt input - negative edge* |
| 0 | 1 | 0 | Input mode - interrupt on positive edge |
| 0 | 1 | 1 | Independent interrupt input - positive edge* |
| 1 | 0 | 0 | Handshake output |
| 1 | 0 | 1 | Pulse output |
| 1 | 1 | 0 | Low output (manual control) |
| 1 | 1 | 1 | High output (manual control) |

**CA1 Control (Bit 0):**

| PCR0 | CA1 Active Edge | Latching Behavior |
|------|-----------------|-------------------|
| 0 | Negative edge | IRA latches on negative edge of CA1 (if ACR0=1) |
| 1 | Positive edge | IRA latches on positive edge of CA1 (if ACR0=1) |

**\*Independent Interrupt Input Mode:**

When CA2 or CB2 is configured as an "independent interrupt input" (PCR bits x01 or x11):
- The interrupt flag is set on the specified edge of the control line.
- **The flag is NOT automatically cleared** by reading/writing the corresponding port register (ORA/ORB).
- Instead, the flag must be cleared manually by writing a 1 to the appropriate bit in the IFR.
- This mode is useful when the control line is used purely as an interrupt input and not for handshaking.

### Handshake Control

The VIA supports automatic handshaking for data transfers between the processor and peripheral devices.

#### Read Handshaking (Port A Only)

**Automatic read handshaking** is available only on Port A:

1. **CA1**: Accepts "Data Ready" signal from peripheral (input)
2. **CA2**: Generates "Data Taken" signal to peripheral (output in handshake mode)

**Sequence:**
1. Peripheral asserts CA1 ("Data Ready"), setting the CA1 interrupt flag.
2. Processor reads Port A (IRA), which clears the CA1 flag.
3. VIA automatically asserts CA2 ("Data Taken").
4. CA2 can operate as either a pulse or a level signal (controlled by PCR).

#### Write Handshaking (Port A or Port B)

**Write handshaking** is available on both ports:

1. **CA2 or CB2**: Generates "Data Ready" signal to peripheral (output in handshake or pulse mode)
2. **CA1 or CB1**: Accepts "Data Taken" signal from peripheral (input)

**Sequence:**
1. Processor writes to Port A or B (ORA or ORB).
2. VIA automatically asserts CA2 or CB2 ("Data Ready").
3. Peripheral responds by asserting CA1 or CB1 ("Data Taken").
4. The control line interrupt flag is set, and the "Data Ready" output is cleared.

#### Handshake Output Modes (CA2/CB2 in Output Mode)

**Handshake Mode (PCR bits x00):**
- Output goes low when the processor writes to the port register.
- Output goes high when the control input (CA1/CB1) receives the "Data Taken" signal.

**Pulse Mode (PCR bits x01):**
- Output generates a single negative pulse (one clock cycle wide) when the processor writes to the port register.
- The pulse occurs regardless of the control input state
| 1 | 0 | 1 | **Mode 5** | Shift out under control of T2 |
| 1 | 1 | 0 | **Mode 6** | Shift out under control of Φ2 (system clock) |
| 1 | 1 | 1 | **Mode 7** | Shift out under control of external clock (CB1) |

#### Mode 0 - Shift Register Interrupt Disabled

- The SR interrupt flag is disabled (held at 0).
- The processor can still read/write the SR, and it will shift on each CB1 positive edge, shifting in the value on CB2.
- No interrupt is generated.

#### Mode 1 - Shift In Under T2 Control

- Shift rate is controlled by the low-order 8 bits of T2.
- CB1 becomes an **output** that generates shift clock pulses for external devices.
- The time between clock transitions is determined by the system clock period and T2L-L value.
- Input data on CB2 should change before the positive-going edge of CB1.
- Data is shifted into the SR on the clock cycle following the positive edge of CB1.
- Minimum CB1 positive pulse width: one clock period.
- After 8 CB1 pulses, the SR interrupt flag is set.

#### Mode 2 - Shift In Under Φ2 Control

- Shift rate is directly controlled by the system clock frequency.
- CB1 becomes an **output** generating shift pulses.
- T2 operates independently and does not affect the SR.
- Reading or writing the SR triggers the shifting operation.
- Data is shifted into bit 0 on the trailing edge of each system clock pulse.
- After 8 clock pulses, the SR interrupt flag is set and CB1 clock output stops.

#### Mode 3 - Shift In Under External Clock (CB1)

- CB1 becomes an **input** for an external clock signal.
- This allows an external device to shift data into the SR at its own pace.
- Data must be stable during the first full cycle following CB1 going high.
- Minimum CB1 positive pulse width: one clock period.
- The SR counter interrupts the processor after 8 bits have been shifted in.
- After 8 counts, the SR stops and must be reset (by reading or writing the SR) to count another 8 pulses.

#### Mode 4 - Shift Out Free-Running at T2 Rate

- Similar to Mode 1, but the SR counter does not stop after 8 shifts.
- Since bit 7 is recirculated back into bit 0, the 8-bit pattern loaded into the SR is repeatedly clocked onto CB2.
- CB1 outputs continuous clock pulses.
- The SR counter is disabled (no interrupt after 8 shifts).
- Useful for generating repetitive waveforms.

#### Mode 5 - Shift Out Under T2 Control

- Shift rate is controlled by T2.
- CB1 outputs shift clock pulses; CB2 outputs serial data.
- Reading or writing the SR triggers the operation (if SR flag is set in IFR).
- The SR counter is reset and 8 bits are shifted onto CB2 with 8 shift pulses on CB1.
- After 8 shifts, the SR interrupt flag is set and CB2 remains at the last data level.
- **This is the mode typically used by the Macintosh Plus keyboard interface.**

#### Mode 6 - Shift Out Under Φ2 Control

- Shift rate is controlled by the system clock.
- CB1 outputs shift pulses; CB2 outputs serial data.
- After 8 shifts, the SR interrupt flag is set and CB2/CB1 stop.

#### Mode 7 - Shift Out Under External Clock (CB1)

- CB1 becomes an **input** for external clock pulses.
- The SR counter sets the SR interrupt flag after 8 pulses but does **not** disable shifting.
- Reading or writing the SR resets the interrupt flag and initializes the counter to begin counting the next 8 pulses.

### Timing - Critical for Emulation

**Mode 5 (Shift Out Under T2) - Used by Macintosh Keyboard:**

1. When the processor **writes to the SR**, the byte is placed in the shift register.
2. Shifting begins immediately (or on the next T2 timeout if the SR flag is set).
3. **The SR interrupt flag (IFR bit 2) is NOT set immediately.**
4. The flag is set after **8 VIA clock cycles** (~10.2 µs at 783.336 kHz) when the 8-bit shift operation completes.

**Why this matters:**
- The Macintosh OS uses the SR interrupt to know when it's safe to switch the VIA from output mode to input mode to receive the keyboard's response.
- **Emulation Error**: Setting the interrupt flag immediately upon write will cause the interrupt handler to run too early, before the keyboard has time to process the command and send its reply, leading to timing-related failures and lost keyboard data.

### Hardware Bug (Known Variant)

Some non-Synertek versions of the 6522 have a bug in **Mode 6** (shift out under Φ2 control, ACR bits 4-2 = 110):
- These chips shift out **9 bits instead of 8**.
- The Macintosh Plus likely used chips without this bug.
- Emulator developers should be aware of this variant if implementing accurate hardware emulation of other systems using the 6522.

### Interrupt Flag Clearing

The SR interrupt flag (IFR bit 2) is cleared by:
- Reading the Shift Register (register 10)
- Writing the Shift Register (register 10)
- Writing a 1 to IFR bit 2
- The timer counts down once from the loaded value.
- When the counter **underflows** from $0000 to $FFFF, the T1 interrupt flag (IFR bit 6) is set.
- The counter continues to decrement after underflow, allowing software to read it to determine elapsed time since interrupt.
- **The interrupt flag will not be set again** unless the timer is reloaded by writing to T1C-H.
- If PB7 output is enabled (ACR7=1 and DDRB7=1), PB7 goes low when T1C-H is written and returns high when the timer underflows, creating a single programmable-width negative pulse.

##### Free-Running Mode (ACR6=1)

- After the counter underflows from $0000 to $FFFF, the interrupt flag is set and the counter **automatically reloads** from the latches (T1L-H:T1L-L).
- This creates a continuous series of evenly spaced interrupts.
- The processor can write new values into the latches (registers 6 and 7) at any time without affecting the current countdown. The new values will be used on the next reload.
- If PB7 output is enabled (ACR7=1 and DDRB7=1), PB7 inverts each time the timer underflows, generating a continuous square wave.
- The interrupt flag can be cleared by writing T1C-H or T1L-H, by reading T1C-L, or by writing a 1 to IFR bit 6.

#### Interrupt Timing - Critical for Emulation

**The T1 interrupt flag is set when the counter underflows (wraps) from $0000 to $FFFF**, not when it reaches $0000.

- If you load the timer with a value of N, the interrupt will occur after N+1 clock cycles.
- Many datasheets ambiguously state "interrupt occurs upon reaching zero," but detailed timing analysis and hardware behavior confirm the flag is set on the clock edge **after** the zero state, when the counter transitions to $FFFF.
- After underflow, the counter continues to decrement: $FFFF, $FFFE, $FFFD, etc.

#### Re-triggering

All VIA timers are **re-triggerable**:
- Writing to T1C-H will always re-initialize the timeout period, even if a countdown is already in progress.
- The timeout can be prevented indefinitely if software continues to write T1C-H before the counter reaches zero.

### Timer 2 (T2)

T2 is a simpler timer used by the Disk Driver for timing I/O events in the Macintosh Plus.

#### Architecture

* **16-bit Counter**: T2C-H (high byte) and T2C-L (low byte)
* **8-bit Latch**: T2L-L (low byte only - no high-order latch)

#### Register Operations

**Register 8 (T2C-L / T2L-L)** - T2 Low-Order Latch/Counter:
- **Write**: Loads value into T2L-L latch only.
- **Read**: Transfers current counter low byte to CPU. **Clears the T2 interrupt flag (IFR bit 5).**

**Register 9 (T2C-H)** - T2 High-Order Counter:
- **Write**: Loads value into T2C-H directly and simultaneously transfers T2L-L into T2C-L, starting the countdown. **Clears the T2 interrupt flag (IFR bit 5).**
- **Read**: Transfers current counter high byte to CPU. Does not affect interrupt flag.

#### Modes of Operation

Timer 2 has two modes, controlled by ACR bit 5:

##### One-Shot Timed Interrupt Mode (ACR5=0)

- T2 operates as a simple interval timer, similar to T1 in one-shot mode.
- When the counter underflows from $0000 to $FFFF, the interrupt flag is set.
- The counter continues to decrement after underflow.
- **The interrupt flag is disabled from setting again** until the processor rewrites T2C-H.

##### Pulse Counting Mode (ACR5=1)

- T2 decrements on each **negative-going edge** detected on the PB6 pin.
- The pulse must be low on the leading edge of the system clock.
- Writing T2C-H clears the interrupt flag and allows the counter to decrement with each pulse on PB6.
- When the counter underflows from $0000 to $FFFF, the interrupt flag is set.
- It is necessary to rewrite T2C-H to allow the interrupt flag to set on a subsequent timeout.

#### Interrupt Timing - Critical for Emulation

Like T1, the **T2 interrupt flag is set when the counter underflows from $0000 to $FFFF**, not when it reaches zero.

- If you load the timer with a value of N, it will take N+1 clock cycles (or N+1 pulses on PB6 in pulse mode) to trigger the interrupt.
- This behavior is explicitly confirmed in **Synertek SY6522 Application Note (AN5)**, which was written to correct ambiguity in earlier datasheets.

### Timer Countdown Sequence Example

For a timer loaded with $0002:
1. Counter = $0002 (initial)
2. After 1 clock: Counter = $0001
3. After 2 clocks: Counter = $0000
4. After 3 clocks: Counter = $FFFF, **interrupt flag is set here**
5. Counter continues: $FFFE, $FFFD, ..
| **4** | vX2 | Input | Reads the mouse's horizontal (X-axis) quadrature signal for direction detection. |
| **3** | vSW | Input | Reads the mouse button state: 0=Pressed, 1=Released. |
| **2** | rTCEnb | Output | Enables the Real-Time Clock serial interface: 0=Enabled. |
| **1** | rTCClk | Output | Provides the data clock signal for the RTC serial interface. |
| **0** | rTCData | I/O | The bidirectional serial data line for the RTC. Its direction is set by DDRB bit 0\. |

### **Event Timers (T1 and T2)**

The VIA has two independent 16-bit timers that decrement at the E clock rate, meaning each count corresponds to **1.2766 µs**. Both can generate interrupts upon reaching zero (timeout).  
**Important Emulation Note**: When setting a timer's 16-bit value, software must perform **two separate 8-bit byte writes** (one for the high-order byte and one for the low-order byte). A single 16-bit word write will not work correctly.

#### **Timer 1 (T1)**

T1 is a flexible timer used primarily by the Sound Driver.

* **Architecture**: It consists of a 16-bit counter (T1C-H and T1C-L) and two 8-bit latches (T1L-H and T1L-L).  
* **Loading the Timer**:  
  1. Write the low byte of the count value to vT1C (T1C-L). This stores the value in the low-order latch.  
  2. Write the high byte to vT1CH (T1C-H). This action loads the high byte directly into the high-order counter and simultaneously transfers the low byte from the latch into the low-order counter, starting the countdown.  
* **Modes of Operation** (set by bits 6 and 7 of the ACR):  
  * **One-Shot Mode** (ACR6=0): The timer counts down once and sets the T1 interrupt flag upon reaching zero. To start another countdown, the timer must be reloaded by writing to T1C-H.  
  * **Free-Running Mode** (ACR6=1): After timing out, the timer automatically reloads the 16-bit value from the latches (T1L-H and T1L-L) and begins counting down again. This provides continuous, evenly spaced interrupts.  
* **Interrupt Timing (Wrap-Around/Underflow)**:  
  * **Important Clarification**: The interrupt flag for T1 is set **not immediately upon reaching zero**, but on the clock cycle where the counter transitions (wraps around) from `0x0000` to `0xFFFF`. Many datasheets ambiguously state the interrupt occurs "upon reaching zero," but detailed timing diagrams and hardware-level analysis confirm the flag is set on the clock edge following the zero state.
* **PB7 Output** (ACR7=1):  
  * In one-shot mode, PB7 will output a single negative pulse for the duration of the countdown.  
  * In free-run mode, PB7 will generate a continuous square wave, inverting at each timeout. Some older software used this to create simple tones by toggling the vSndEnb bit, but this technique is now considered obsolete.
  * For PB7 to function as the timer output, both DDRB bit 7 and ACR bit 7 must be set to 1. If only one is set, PB7 acts as a normal output pin controlled by ORB bit 7.

#### **Timer 2 (T2)**

T2 is a simpler timer used by the Disk Driver for timing I/O events.

* **Architecture**: It has a 16-bit counter and only a low-order latch.  
* **Modes of Operation** (set by bit 5 of the ACR):  
  * **One-Shot Timed Interrupt** (ACR5=0): T2 operates as a one-shot interval timer.  
  * **Pulse Counting Mode** (ACR5=1): T2 decrements each time a negative-going pulse is detected on the **PB6** pin.  
* **Emulation Nuance (Pulse Counting and Interrupt Timing)**:  
  * The T2 interrupt flag is **not** set when the counter reaches zero. It is set when the counter **underflows** from `0x0000` to `0xFFFF` (wrap-around). This means if you load the counter with a value of N, it will take N+1 pulses on PB6 to trigger the interrupt.
  * **Explicit Reference**: This behavior is confirmed by the Synertek SY6522 Application Note (AN5), which was written to correct this specific point in earlier, less clear datasheets.

### **Shift Register (SR)**

The VIA includes an 8-bit Shift Register for serial communication, which the Macintosh Plus uses exclusively for the **keyboard interface**.

* **Interface**: **CB2** is the bidirectional data line, and **CB1** is the clock.  
* **Operation**: The SR is configured to receive key transition codes from the keyboard. An interrupt can be generated after 8 bits have been shifted. Reading or writing the Shift Register at vBase+vSR initiates the transfer.
* **Timing (Critical for Emulation)**:
  * When writing to the SR in **shift-out mode** (ACR bit 4 = 1), the byte is immediately placed on the data line and begins shifting.
  * However, the **IFR_SR interrupt flag is not set immediately**. Instead, it is set after **8 VIA clock cycles** (~10.2 µs at 783.336 kHz), when the 8-bit shift operation completes.
  * This delay is essential for correct keyboard protocol timing. The Macintosh OS uses this interrupt to know when it's safe to switch the VIA from output mode to input mode to receive the keyboard's response.
  * **Emulation Error**: Setting the interrupt flag immediately upon write will cause the ISR to run too early, potentially before the keyboard has had time to process the command and send its reply, leading to timing-related failures.
* **Emulation Nuance (Hardware Bug)**: Some non-Synertek versions of the 6522 have a bug in shift-out mode under Φ2 control (ACR bits 4-2 \= 110), where 9 bits are shifted out instead of 8\. While the Macintosh Plus likely used chips without this bug, it is a known variant in the 6522 family that an emulator developer should be aware of.

### **Control Registers**

#### **Auxiliary Control Register (ACR)**

Located at vBase+vACR, this register controls the operating modes of the timers and the Shift Register.

| Bits | Function | Description |
| :---- | :---- | :---- |
| **7-6** | **T1 Control** | 00: One-shot, 01: Free-run, 10: One-shot w/ PB7 output, 11: Free-run w/ PB7 square wave. |
| **5** | **T2 Control** | 0: Timed interrupt mode, 1: Count pulses on PB6 mode. |
| **4-2** | **Shift Register Control** | Selects one of 8 modes for serial I/O. |
| **1** | **PB Latching** | 1 enables input latching on Port B, triggered by the active transition on CB1. |
| **0** | **PA Latching** | 1 enables input latching on Port A, triggered by the active transition on CA1. |

#### **Peripheral Control Register (PCR)**

Located at vBase+vPCR, this register configures the behavior of the four control lines (CA1, CA2, CB1, CB2). **Changing these bits from their OS-defined startup values is not recommended.** When CA2 or CB2 are set to an "independent interrupt input" mode, their flags in the IFR are *not* cleared by reading/writing the corresponding port register; they must be cleared manually by writing to the IFR.

### **Interrupt System**

The VIA is the source of all **level 1** processor interrupts. It consolidates seven internal interrupt sources into a single /IRQ line to the MC68000.

#### **Interrupt Flag Register (IFR)**

Located at vBase+vIFR, this register's bits are set to 1 when a specific interrupt condition occurs.

| Bit | Interrupt Source | Set Condition | How to Clear Flag |
| :---- | :---- | :---- | :---- |
| **7** | **IRQ Status** | Set if any enabled flag (bits 0-6) is set. | Cleared when all other active flags are cleared. |
| **6** | **Timer 1** | T1 counts down to zero. | Read T1C-L or Write T1C-H. |
| **5** | **Timer 2** | T2 underflows from 0 to 0xFFFF. | Read T2C-L or Write T2C-H. |
| **4** | **Keyboard Clock** | Active transition on CB1. | Read or write ORB. |
| **3** | **Keyboard Data** | Active transition on CB2. | Read or write ORB. |
| **2** | **Keyboard Data Ready** | 8 bits have been shifted in/out (set after 8 VIA clock cycles in shift-out mode). | Read or write the Shift Register (SR). |
| **1** | **Vertical Blanking** | Active transition on CA1 from /VSYNC. | Read or write ORA. |
| **0** | **One-Second Tick** | Active transition on CA2 from RTC. | Read or write ORA (unless in independent mode). |

**Clearing Flags**: A flag can be cleared by its specific reset condition (e.g., reading a timer) or by **writing a 1 to its corresponding bit position in the IFR**. A common technique to clear all currently active flags is to read the IFR and then immediately write that same value back to the IFR.

#### **Interrupt Enable Register (IER)**

Located at vBase+vIER, this register enables or disables each of the seven interrupt sources. An interrupt flag in the IFR will only trigger an /IRQ if its corresponding bit in the IER is 1\.

* **Writing to IER**:  
  * To **enable** interrupts, write a byte with bit 7 set to 1\. Every other bit set to 1 (bits 0-6) will enable its corresponding interrupt.  
  * To **disable** interrupts, write a byte with bit 7 set to 0\. Every other bit set to 1 (bits 0-6) will disable its corresponding interrupt.  
  * A 0 in bits 0-6 leaves the corresponding enable bit unchanged.  
* **Reading from IER**: When read, the register shows the current enable state for bits 0-6. **Bit 7 will always be read as a 1**.