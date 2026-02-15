# Macintosh Plus Mouse: Technical Reference

This document describes the hardware interface and software handling of the non-ADB mouse used in the Macintosh Plus (and earlier Macintosh 128K/512K/512Ke). This is distinct from the ADB (Apple Desktop Bus) mouse used in later Macintosh models starting with the Macintosh SE.

## Table of Contents

1. [Hardware Overview](#hardware-overview)
2. [Quadrature Encoding](#quadrature-encoding)
3. [Hardware Connections](#hardware-connections)
4. [Interrupt Mechanism](#interrupt-mechanism)
5. [Low-Memory Global Variables](#low-memory-global-variables)
6. [Interrupt Dispatch Tables](#interrupt-dispatch-tables)
7. [Interrupt Handler Operation](#interrupt-handler-operation)
8. [Cursor Task and Mouse Acceleration](#cursor-task-and-mouse-acceleration)
9. [Mouse Button Handling](#mouse-button-handling)
10. [Timing Considerations](#timing-considerations)
11. [Register Access Patterns](#register-access-patterns)
12. [Source Code References](#source-code-references)

---

## Hardware Overview

The Macintosh Plus mouse is an electromechanical pointing device containing:

- A **rubber-coated steel ball** that contacts two perpendicular capstans
- Two **interrupter wheels**, each connected to one capstan
- **Infrared LEDs and phototransistors** for optical encoding
- A **pushbutton switch** for the mouse button

Motion along the X axis rotates one wheel; motion along the Y axis rotates the other. Each wheel has a row of slots through which infrared light shines onto phototransistor detectors.

---

## Quadrature Encoding

The mouse uses **quadrature encoding** to detect both speed and direction of motion along each axis.

### Signal Generation

For each axis, there are two signals:
- **Interrupt signal** (X1 or Y1): Connected to SCC DCD inputs
- **Quadrature signal** (X2 or Y2): Connected to VIA data register B

The two phototransistor detectors per axis are offset so that as the wheel turns, they produce two square-wave signals **90 degrees out of phase**.

### Direction Detection

When the SCC generates an interrupt on a DCD transition, the interrupt handler reads the corresponding quadrature signal from the VIA to determine direction:

| SCC Interrupt Edge | VIA Quadrature Level | X-Axis Direction | Y-Axis Direction |
|--------------------|---------------------|------------------|------------------|
| Rising (positive)  | Low (0)             | Left             | Down             |
| Rising (positive)  | High (1)            | Right            | Up               |
| Falling (negative) | Low (0)             | Right            | Up               |
| Falling (negative) | High (1)            | Left             | Down             |

The logic is: **XOR the edge polarity with the quadrature level** to determine direction.

---

## Hardware Connections

### SCC (Zilog 8530 Serial Communications Controller)

The mouse interrupt signals are connected to the SCC's **DCD (Data Carrier Detect)** pins:

| Signal | SCC Input | SCC Channel | Purpose |
|--------|-----------|-------------|---------|
| X1 (Horizontal interrupt) | DCDA | Channel A | Horizontal motion interrupt |
| Y1 (Vertical interrupt) | DCDB | Channel B | Vertical motion interrupt |

**SCC Base Addresses (Macintosh Plus):**
```
SCCRBase = $9FFFF8    ; SCC read base address
SCCWBase = $BFFFF9    ; SCC write base address
```

**SCC Channel Offsets:**
```
Channel B Control Read:   SCCRBase + 0 = $9FFFF8
Channel B Control Write:  SCCWBase + 0 = $BFFFF9
Channel A Control Read:   SCCRBase + 2 = $9FFFFA
Channel A Control Write:  SCCWBase + 2 = $BFFFFB
Channel B Data Read:      SCCRBase + 4 = $9FFFFC
Channel B Data Write:     SCCWBase + 4 = $BFFFFD
Channel A Data Read:      SCCRBase + 6 = $9FFFFE
Channel A Data Write:     SCCWBase + 6 = $BFFFFF
```

### VIA (6522 Versatile Interface Adapter)

The mouse quadrature signals and button are connected to **VIA Data Register B**:

| Signal | VIA Bit | Bit Name | Purpose |
|--------|---------|----------|---------|
| Mouse button | Bit 3 | vSW | Mouse switch (0 = pressed, 1 = released) |
| X2 (Horizontal quadrature) | Bit 4 | vX2 | Horizontal quadrature level |
| Y2 (Vertical quadrature) | Bit 5 | vY2 | Vertical quadrature level |

**VIA Base Address (Macintosh Plus):**
```
VBase = $EFE1FE       ; VIA base address
```

**VIA Register Offsets:**
```
vBufB  = $0000        ; Data Register B (offset 0)
vBufA  = $1E00        ; Data Register A (offset $1E00)
vDIRB  = $0400        ; Data Direction Register B
vDIRA  = $0600        ; Data Direction Register A
```

**Absolute Addresses:**
```
AVBufB = $EFE1FE      ; VIA Buffer B (mouse state)
AVBufA = $F00000 - $1E02 ; VIA Buffer A
```

**Data Direction for Buffer B (vBOut):**
```
Bit 0 (vRTCData): Output (1)  - RTC data
Bit 1 (vRTCClk):  Output (1)  - RTC clock
Bit 2 (vRTCEnb):  Output (1)  - RTC enable
Bit 3 (vSW):      Input  (0)  - Mouse button
Bit 4 (vX2):      Input  (0)  - Mouse X quadrature
Bit 5 (vY2):      Input  (0)  - Mouse Y quadrature
Bit 6 (vH4):      Input  (0)  - Horizontal sync
Bit 7 (vSndEnb):  Output (1)  - Sound enable
```

---

## Interrupt Mechanism

### Interrupt Priority Levels

On the Macintosh Plus, interrupts are organized by autovector levels:

| Level | Source | Purpose |
|-------|--------|---------|
| 1 | VIA | VBL, timers, keyboard |
| 2 | SCC | Serial communications, **mouse interrupts** |
| 4 | SCC (alternate mapping) | Used on some machines |

The mouse generates **Level 2 interrupts** via the SCC.

### SCC Interrupt Vector Selection

When an SCC interrupt occurs, the 8530 provides a **modified interrupt vector** that indicates which of the 8 possible interrupt sources is active:

| Vector (bits 3-1) | Interrupt Source |
|-------------------|------------------|
| 000 (0) | Channel B: Transmit buffer empty |
| 001 (2) | Channel B: External/status (mouse Y) |
| 010 (4) | Channel B: Receive character available |
| 011 (6) | Channel B: Special receive condition |
| 100 (8) | Channel A: Transmit buffer empty |
| 101 (A) | Channel A: External/status (mouse X) |
| 110 (C) | Channel A: Receive character available |
| 111 (E) | Channel A: Special receive condition |

The **external/status** interrupts (vectors 2 and A) handle mouse movement. The DCD (Data Carrier Detect) transition triggers an external/status interrupt.

### SCC Register Configuration

For mouse interrupts, **Write Register 15** controls which external/status conditions generate interrupts:

```
WR15 bit 3: DCD interrupt enable
```

When DCD interrupts are enabled and a DCD transition occurs (mouse movement), the SCC:
1. Sets the DCD bit in Read Register 0
2. Generates an external/status interrupt

---

## Low-Memory Global Variables

The following low-memory locations are used by the mouse subsystem:

### Interrupt Dispatch Tables

| Address | Name | Size | Description |
|---------|------|------|-------------|
| $01B2 | Lvl2DT / SccDT | 32 bytes | Level 2 (SCC) interrupt dispatch table |
| $02BE | ExtStsDT | 16 bytes | External/status secondary dispatch table |

### SCC Status Storage

| Address | Name | Size | Description |
|---------|------|------|-------------|
| $02CE | SCCASts | 1 byte | Last SCC RR0 status for channel A |
| $02CF | SCCBSts | 1 byte | Last SCC RR0 status for channel B |

### Hardware Base Addresses

| Address | Name | Size | Description |
|---------|------|------|-------------|
| $01D4 | VIA | 4 bytes | VIA base address pointer |
| $01D8 | SCCRd | 4 bytes | SCC read base address pointer |
| $01DC | SCCWr | 4 bytes | SCC write base address pointer |

### Mouse Position Variables

| Address | Name | Size | Description |
|---------|------|------|-------------|
| $0828 | MTemp | 4 bytes | Low-level interrupt mouse location (V.H) |
| $082C | RawMouse | 4 bytes | Un-scaled ("un-jerked") mouse coordinates (V.H) |
| $0830 | Mouse | 4 bytes | Processed mouse location (V.H) |
| $0834 | CrsrPin | 8 bytes | Cursor pinning rectangle (bounds) |

**Note:** Mouse coordinates are stored as Point structures: vertical (Y) in high word, horizontal (X) in low word.

### Cursor Control Variables

| Address | Name | Size | Description |
|---------|------|------|-------------|
| $08CC | CrsrVis | 1 byte | Cursor visible flag |
| $08CD | CrsrBusy | 1 byte | Cursor locked out (semaphore) |
| $08CE | CrsrNew | 1 byte | Cursor position changed flag |
| $08CF | CrsrCouple | 1 byte | Cursor coupled to mouse flag |
| $08D0 | CrsrState | 2 bytes | Cursor nesting level |
| $08D2 | CrsrObscure | 1 byte | Cursor obscured semaphore |
| $08D6 | MouseMask | 4 bytes | V-H mask for ANDing with mouse |
| $08DA | MouseOffset | 4 bytes | V-H offset added after masking |
| $08EC | CrsrThresh | 2 bytes | Delta threshold for mouse scaling |
| $08EE | JCrsrTask | 4 bytes | Address of cursor VBL task |

### Mouse Button Variables

| Address | Name | Size | Description |
|---------|------|------|-------------|
| $016E | MBTicks | 4 bytes | Tick count at last mouse button change |
| $0172 | MBState | 1 byte | Current mouse button state (bit 7: 0=down, 1=up) |

### Mouse Acceleration

| Address | Name | Size | Description |
|---------|------|------|-------------|
| $0D6A | MickeyBytes | 4 bytes | Pointer to mouse acceleration table and globals |

---

## Interrupt Dispatch Tables

### SccDT (Lvl2DT) - Primary SCC Dispatch Table

Located at $01B2, this 32-byte table contains 8 long-word vectors:

| Offset | Vector | Purpose |
|--------|--------|---------|
| +$00 | SccDT+0 | Channel B transmit buffer empty |
| +$04 | SccDT+4 | Channel B external/status → **Mouse Y handler** |
| +$08 | SccDT+8 | Channel B receive character |
| +$0C | SccDT+12 | Channel B special receive |
| +$10 | SccDT+16 | Channel A transmit buffer empty |
| +$14 | SccDT+20 | Channel A external/status → **Mouse X handler** |
| +$18 | SccDT+24 | Channel A receive character |
| +$1C | SccDT+28 | Channel A special receive |

### ExtStsDT - External/Status Secondary Dispatch

Located at $02BE, this 16-byte table provides secondary dispatch for external/status interrupts:

| Offset | Vector | Purpose |
|--------|--------|---------|
| +$00 | ExtStsDT+0 | Channel B ext/sts non-mouse handler |
| +$04 | ExtStsDT+4 | Mouse vertical interrupt handler |
| +$08 | ExtStsDT+8 | Channel A ext/sts non-mouse handler |
| +$0C | ExtStsDT+12 | Mouse horizontal interrupt handler |

---

## Interrupt Handler Operation

### Primary SCC Interrupt Dispatcher (SccDecode)

When an SCC interrupt occurs:

```assembly
SccDecode:
    MOVE.L  SCCRd,A0            ; Get SCC read base address
    MOVE.B  (A0),D0             ; Dummy read to sync SCC
    MOVE.L  SCCWr,A1            ; Get SCC write base address
    
    MOVE.B  #2,(A1)             ; Select WR2 (interrupt vector register)
    LEA     SccDT,A2            ; Point to dispatch table
    MOVEQ   #$0E,D0             ; Mask for vector bits 1-3
    AND.B   (A0),D0             ; Read modified interrupt vector
    
    CMPI.B  #8,D0               ; Channel A interrupt?
    BLT.S   @ChB                ; No, channel B
    ADDQ    #2,A0               ; Adjust for channel A
    ADDQ    #2,A1
@ChB:
    ADD     D0,D0               ; Double for long-word index
    MOVE.L  (A2,D0.W),A2        ; Get dispatch vector
    JMP     (A2)                ; Jump to handler
```

### External/Status Interrupt Handler

For mouse interrupts (DCD transitions), the external/status handler:

```assembly
ExtAInt:                        ; Channel A (horizontal)
    MOVE.B  (A0),D0             ; Read SCC RR0 status
    LEA     SCCASts,A3          ; Previous status storage
    LEA     ExtStsDT+8,A2       ; Secondary dispatch table
    BRA.S   ExtABInt

ExtBInt:                        ; Channel B (vertical)  
    MOVE.B  (A0),D0             ; Read SCC RR0 status
    LEA     SCCBSts,A3          ; Previous status storage
    LEA     ExtStsDT,A2         ; Secondary dispatch table

ExtABInt:
    MOVE.B  #15,(A1)            ; Select WR15 (ext/sts interrupt enable)
    MOVE.B  (A3),D1             ; Get previous status
    MOVE.B  D0,(A3)             ; Save current status
    EOR.B   D0,D1               ; Get changed bits
    AND.B   (A0),D1             ; Mask with enabled interrupts
    MOVE.B  #$10,(A1)           ; Reset ext/sts interrupt
    
    ; D0 = current RR0 status
    ; D1 = changed bits
    
    MOVE.L  (A2),A2             ; Get secondary vector
    JMP     (A2)                ; Dispatch to mouse handler
```

### Mouse Interrupt Handler (Decoding Direction)

The mouse interrupt handler determines direction by:

1. Reading the **DCD bit from SCC RR0** (indicates interrupt edge polarity)
2. Reading the **quadrature signal from VIA buffer B**
3. XORing these values to determine direction
4. Incrementing or decrementing the appropriate coordinate in **MTemp**
5. Setting **CrsrNew** to signal the cursor task

```assembly
; Pseudo-code for mouse interrupt handler:

MouseHandler:
    ; D0 = SCC RR0 (bit 3 = DCD state)
    ; D1 = changed bits
    
    ; Read quadrature from VIA
    MOVE.L  VIA,A0
    MOVE.B  vBufB(A0),D2        ; Read VIA buffer B
    
    ; For horizontal: check vX2 (bit 4)
    ; For vertical: check vY2 (bit 5)
    
    ; Determine direction and update MTemp
    ; (implementation varies by ROM version)
    
    ; Signal cursor task
    MOVE.B  CrsrCouple,CrsrNew  ; Set if cursor coupled
    
    RTS
```

---

## Cursor Task and Mouse Acceleration

### JCrsrTask

The **JCrsrTask** vector ($08EE) points to a routine called during vertical blanking to process mouse movement. On the Macintosh Plus, this is patched to the `MapCode` routine.

### Mouse Mapping Algorithm

The cursor task implements mouse acceleration:

```assembly
MapCode:
    TST.B   CrsrNew             ; Mouse changed?
    BEQ     Done                ; No, return
    TST.B   CrsrBusy            ; Cursor locked?
    BNE     Done                ; Yes, return
    TST.B   CrsrCouple          ; Cursor coupled to mouse?
    BEQ     NoComp              ; No, skip computation
    
    ; Calculate delta from MTemp to RawMouse
    MOVE.W  MTemp+H,D0          ; Get X from interrupt handler
    SUB.W   RawMouse+H,D0       ; ΔX = MTemp.X - RawMouse.X
    MOVE.W  MTemp+V,D1          ; Get Y from interrupt handler
    SUB.W   RawMouse+V,D1       ; ΔY = MTemp.Y - RawMouse.Y
    
    ; Calculate magnitude = max(|ΔX|,|ΔY|) + min(|ΔX|,|ΔY|)/2
    ; (fast approximation of distance)
    
    ; Look up acceleration multiplier in MickeyBytes table
    ; Apply acceleration to delta values
    ; Update RawMouse
    
    ; Pin to CrsrPin rectangle
    ; Apply MouseMask and MouseOffset for "jerky" cursor
    ; Update Mouse variable
    
    ; Redraw cursor
    CLR.B   CrsrNew
    CLR.B   CrsrObscure
    _ShowCursor
    RTS
```

### Mickey Bytes Table

The acceleration is controlled by the **MickeyBytes** table, an 8-byte lookup table that maps mouse velocity to acceleration multiplier. This table is loaded from a 'mcky' resource and can be adjusted via the Mouse control panel.

```assembly
; Default Mickey Bytes table
default:  dc.b  4, 10, 15, 255, 255, 255, 255, 255
```

The algorithm:
1. Calculate mouse movement magnitude
2. Search the table until magnitude ≤ table[i]
3. Use index `i` as the acceleration multiplier

---

## Mouse Button Handling

The mouse button is **not** interrupt-driven on the Macintosh Plus. Instead, it is polled during:
- Vertical blanking interrupt (VBL)
- GetNextEvent / WaitNextEvent calls

### Reading the Button State

```assembly
    MOVE.L  VIA,A0              ; Get VIA base
    BTST    #vSW,vBufB(A0)      ; Test bit 3 of buffer B
    ; Z flag set = button DOWN (bit is 0)
    ; Z flag clear = button UP (bit is 1)
```

Or using the absolute address:

```assembly
    MOVE.B  AVBufM,D0           ; AVBufM = $EFE1FE (VBase+vBufB)
    BTST    #vSW,D0             ; Test bit 3
```

### Button Debouncing

The button is naturally debounced by the polling interval (approximately 1/60th second at each VBL). The system also implements additional debounce logic:

```assembly
; If button down within 2 ticks of button up, ignore (bounce)
    MOVE.L  Ticks,D1
    SUB.L   MBTicks,D1          ; Time since last change
    SUBQ.L  #2,D1               ; Less than 2 VBLs?
    BLT.S   @Ignore             ; Yes, ignore bounce
```

---

## Timing Considerations

### Interrupt Latency

- Mouse interrupts occur on **every edge** of the X1/Y1 signals
- At high mouse velocities, interrupts can occur at rates of several hundred per second
- Each interrupt must be serviced before the next edge, or counts will be lost

### Critical Timing

1. **SCC Access Timing**: The SCC requires specific timing between register accesses. A dummy read is performed to synchronize the chip.

2. **VIA Access Timing**: The VIA quadrature signals must be read during the SCC interrupt handler while they are still valid relative to the interrupt edge.

3. **Cursor Task Timing**: The cursor task runs at VBL time (approximately 60.15 Hz) and must complete before the next VBL.

### TimeVIADB

The low-memory variable **TimeVIADB** ($0CEA) contains a calibration value representing the number of VIA access iterations per millisecond, used for timing loops:

```assembly
TimeVIADB   EQU   $0CEA    ; (word) iterations of VIA access per ms
```

---

## Register Access Patterns

### Reading Mouse Signals

**SCC Status (RR0):**
```assembly
    MOVE.L  SCCRd,A0            ; Get SCC read base
    MOVE.B  (A0),D0             ; Channel B RR0 (dummy read to sync)
    ; or
    MOVE.B  2(A0),D0            ; Channel A RR0
```

**VIA Buffer B (quadrature and button):**
```assembly
    MOVE.L  VIA,A0              ; Get VIA base ($1D4 points to base)
    MOVE.B  vBufB(A0),D0        ; Read buffer B (offset 0)
    ; or directly:
    MOVE.B  AVBufB,D0           ; $EFE1FE
```

### Clearing SCC External/Status Interrupt

After reading the status, the interrupt must be cleared:

```assembly
    MOVE.L  SCCWr,A1            ; Get SCC write base
    MOVE.B  #$10,(A1)           ; Channel B: Reset ext/sts interrupt
    ; or
    MOVE.B  #$10,2(A1)          ; Channel A: Reset ext/sts interrupt
```

### SCC Register Access Sequence

```assembly
; To read SCC status register:
    MOVE.L  SCCRd,A0
    MOVE.B  (A0),D0             ; Sync read
    MOVE.B  (A0),D0             ; Read RR0

; To write SCC register N:
    MOVE.L  SCCWr,A1
    MOVE.B  #N,(A1)             ; Select register N
    MOVE.B  #value,(A1)         ; Write value to register N

; To read SCC register N (N > 0):
    MOVE.L  SCCRd,A0
    MOVE.L  SCCWr,A1
    MOVE.B  #N,(A1)             ; Select register N
    MOVE.B  (A0),D0             ; Read register N
```

---

## Source Code References

The following source files from the System 7.1 sources contain relevant mouse code:

### Mouse Cursor Task
- **OS/Mouse.a** - Mouse mapping/acceleration code for Mac Plus
  - `PlusMouseInit` - Initialization for Mac Plus
  - `MapCode` - Cursor task (jCrsrTask handler)
  - `MickeyBytesHelper` - Acceleration table loading

### ROM Patches (Macintosh Plus)
- **Patches/PatchPlusROM.a** - Mac Plus ROM patches
  - `SccDecode` - SCC interrupt dispatcher
  - Serial driver patches that configure DCD interrupts

### Interrupt Handlers
- **OS/InterruptHandlers.a** - General interrupt dispatch
  - `Level4SccInt` - SCC interrupt entry
  - `SccDecode` - SCC interrupt dispatcher
  - `ExtAInt`, `ExtBInt` - External/status handlers

### Hardware Definitions
- **Interfaces/AIncludes/HardwareEqu.a** - Hardware equates
  - VIA register definitions
  - SCC base addresses
  - Bit definitions for vSW, vX2, vY2

- **Interfaces/AIncludes/SysEqu.a** - System equates
  - Low-memory global definitions
  - Interrupt table definitions

### ADB Mouse (for comparison)
- **OS/ADBMgr/ADBMgrPatch.a** - ADB manager
  - `MouseDrvr` - ADB mouse driver (SE and later)
  - Shows how MTemp/CrsrNew are used in ADB context

---

## DB-9 Mouse Connector Pinout

The mouse connects to the Macintosh via a DE-9 (DB-9) connector:

| Pin | Signal | Description |
|-----|--------|-------------|
| 1 | GND | Ground |
| 2 | +5V | Power |
| 3 | GND | Ground |
| 4 | X2 | Horizontal quadrature → VIA bit 4 |
| 5 | X1 | Horizontal interrupt → SCC DCDA |
| 6 | - | Not connected |
| 7 | SW | Mouse button → VIA bit 3 (active low) |
| 8 | Y2 | Vertical quadrature → VIA bit 5 |
| 9 | Y1 | Vertical interrupt → SCC DCDB |

---

## Summary


The Macintosh Plus mouse is a sophisticated input device that relies on the coordination of three major system components:

1. **SCC (8530)**: Generates interrupts on mouse movement via DCD inputs
2. **VIA (6522)**: Provides quadrature signals and button state via Data Register B
3. **Software**: Interrupt handlers decode direction, cursor task applies acceleration

The design elegantly reuses the serial communications controller's DCD interrupt capability for mouse tracking, while using simple GPIO lines for direction detection and button state. This architecture was replaced by the unified ADB (Apple Desktop Bus) starting with the Macintosh SE.

