# Apple Desktop Bus (ADB)

The Apple Desktop Bus is a single-master, multi-slave serial bus used for low-speed
input devices (keyboard, mouse, graphics tablets). This document covers the ADB
implementation in the Macintosh SE/30, IIx, and IIcx, which all share the same
hardware architecture and ROM-level ADB code.

## Hardware Architecture

### Two-Chip Design

The ADB subsystem uses a two-chip arrangement:

| Component | Role |
|-----------|------|
| **ADB transceiver IC** (Apple 342S0440-B) | 4-bit microcontroller that drives the ADB bus, converts TTL to/from open-collector variable-pulse-width signalling, performs automatic polling, and detects service requests |
| **VIA1** (6522-compatible) | Provides the CPU-visible interface: shift register for byte-serial transfers, port B bits for transaction state control and interrupt signalling |

There is no independent "ADB register file" that software reads or writes. All ADB
interaction goes through VIA1 registers.

The ADB transceiver IC (Apple part 342S0440-B) is a mask-programmed **PIC16CR54**
microcontroller. Its firmware has been extracted via die imaging by the
reverse-engineering community.

### Architecture Diagram

```
68030 CPU ──MMIO──► VIA1 (SR/ACR/PCR/IFR/IER + ports)
                      │
                      │ 8-bit serial via Shift Register
                      │ + ST0/ST1 transaction-state lines
                      │ + vADBInt status line
                      ▼
                ADB Transceiver IC
                (Apple 342S0440-B)
                      │
                      │ Open-collector ADB line
                      │ (variable pulse-width encoding)
                      ▼
                   ADB Bus
                   ├── Keyboard
                   ├── Mouse
                   └── Other ADB devices
```

### Model-Specific Differences

The ADB interface circuit differs between:

- **SE and SE/30**: No keyboard power-on feature. The POWER.ON pin (pin 2 on the
  ADB connector) is not connected.
- **IIx and IIcx** (and other Mac II family): Keyboard power-on supported. A key on
  the ADB keyboard momentarily grounds the POWER.ON pin to switch on the power
  supply.

At the VIA memory-map level, all three machines present **identical base addresses**
and the same ADB transaction sequencing model. The SE/30 ROM is identical to the
IIx ROM.

### ADB Connector

4-pin mini-DIN connector (active devices limited to 3 daisy-chained, cables max 5m):

| Pin | Signal | Description |
|-----|--------|-------------|
| 1 | ADB | Bidirectional data bus. Open-collector, pulled up to +5V through 470 ohm |
| 2 | POWER.ON | Keyboard power-on (Mac II family only; NC on SE/SE/30) |
| 3 | +5V | +5 volts (all ADB devices combined must not draw more than 500 mA) |
| 4 | GND | Ground |

### Electrical Characteristics

| Parameter | Min | Max |
|-----------|-----|-----|
| Low input signal voltage | -0.2 V | 0.8 V |
| High input signal voltage | 2.4 V | 5.0 V |
| Low output signal voltage | — | 0.45 V (at 12 mA) |
| High output signal voltage | 2.4 V | — |
| Device output current when off | — | -20 uA (at 0.4 V) |
| Device input capacitance | — | 150 pF |

## Memory Map and VIA Registers

### VIA Base Addresses

All three target machines use identical addresses (from ROM universal tables):

| Model | VIA1 Base | VIA2 Base |
|-------|-----------|-----------|
| SE/30 | `0x50F00000` | `0x50F02000` |
| IIx   | `0x50F00000` | `0x50F02000` |
| IIcx  | `0x50F00000` | `0x50F02000` |

Note: Classic Mac hardware often partially decodes I/O address lines, creating
register aliases. Use canonical base addresses only.

### VIA Register Offsets

Registers are accessed at sparse 0x200-byte steps from the VIA base:

| Offset | Register | Name |
|--------|----------|------|
| `0x0000` | vBufB | Data Register B (port B) |
| `0x0400` | vDirB | Data Direction Register B |
| `0x0600` | vDirA | Data Direction Register A |
| `0x0800` | vT1CL | Timer 1 Counter Low |
| `0x0A00` | vT1CH | Timer 1 Counter High |
| `0x1000` | vT2CL | Timer 2 Counter Low |
| `0x1200` | vT2CH | Timer 2 Counter High |
| `0x1400` | vSR | Shift Register |
| `0x1600` | vACR | Auxiliary Control Register |
| `0x1800` | vPCR | Peripheral Control Register |
| `0x1A00` | vIFR | Interrupt Flag Register |
| `0x1C00` | vIER | Interrupt Enable Register |
| `0x1E00` | vBufA | Data Register A (port A) |

### ADB-Relevant VIA1 Port B Bits

| Bit | Mask | Name | Function |
|-----|------|------|----------|
| 3 | `0x08` | vADBInt | ADB interrupt / service request (active low) |
| 4 | `0x10` | vADBS1 (ST0) | ADB transaction state bit 0 |
| 5 | `0x20` | vADBS2 (ST1) | ADB transaction state bit 1 |

### VIA Shift Register Modes (ACR bits 4-2)

| ACR bits 4-2 | Mode | Direction |
|--------------|------|-----------|
| `111` (7) | Shift out under external clock (CB1) | CPU → transceiver |
| `011` (3) | Shift in under external clock (CB1) | Transceiver → CPU |

The OS switches modes with:
- `ORI.B #$1C, vACR` — sets bits 4-2 to `111` (shift-out)
- `BCLR #4, vACR` — clears bit 4, leaving `011` (shift-in)

### VIA Interrupt Sources for ADB

| IFR Bit | Source |
|---------|--------|
| 2 (IFR_SR) | Shift register — "complete 8 shifts" |
| 7 | Any enabled interrupt (summary bit) |

IER bit 7 controls set/clear: writing with bit 7 = 1 sets enables, bit 7 = 0 clears.

**Important**: In shift-register mode 7 (shift-out under external clock), the VIA's
internal SR timer must NOT fire IFR_SR. On the SE/30, mode 7 means the ADB
transceiver (GLU) controls timing. A spurious IFR_SR causes the ADB handler to
enter a polling loop at the wrong time, blocking level-1 interrupts and hanging
the machine.

## ADB Transaction State Machine

### States

Two signal lines (ST0, ST1) from VIA1 port B to the transceiver define the current
transaction state:

| ST1 | ST0 | State | Meaning |
|-----|-----|-------|---------|
| 0 | 0 | 0 | Start a new command |
| 0 | 1 | 1 | Transfer data byte (even) |
| 1 | 0 | 2 | Transfer data byte (odd) |
| 1 | 1 | 3 | Idle |

The default transaction state after startup/reset is **3 (idle)**.

### Transaction Sequence

1. ADB Manager writes the command byte to VIA shift register (SR)
2. ADB Manager sets ST1/ST0 = 00 (state 0: start command)
3. Transceiver shifts the command byte in from VIA, converts to ADB protocol,
   transmits on bus
4. VIA fires IFR_SR interrupt when byte transfer completes
5. ADB Manager alternates between states 1 and 2 for each data byte:
   - **Listen**: bytes shift out from VIA → transceiver → device
   - **Talk**: bytes shift in from device → transceiver → VIA
6. After last byte, ADB Manager sets state to:
   - **0** to send another command, or
   - **3 (idle)** where the transceiver auto-repeats the last Talk command every ~11 ms

```
OS/ADB Manager          VIA1 (SR + ports)       ADB Transceiver        ADB Device
      |                       |                       |                    |
      |-- Write cmd to SR --> |                       |                    |
      |-- Set ST=00 -------> |-- Shift byte -------> |                    |
      |                       |                       |-- Attention+Sync+Cmd -->|
      |<- IFR_SR interrupt -- |                       |                    |
      |                       |                       |                    |
      |-- Set ST=01 -------> |                       |<-- Data byte 0 ---|
      |<- IFR_SR interrupt -- |<- Shift byte in ----- |                    |
      |                       |                       |                    |
      |-- Set ST=10 -------> |                       |<-- Data byte 1 ---|
      |<- IFR_SR interrupt -- |<- Shift byte in ----- |                    |
      |                       |                       |                    |
      |-- Set ST=11 (idle) -> |                       |                    |
      |                       |     (auto-poll every ~11ms in idle)        |
```

## ADB Command Byte Format

```
Bit 7  6  5  4    3  2    1  0
    |  address  | cmd  | reg  |
    └──────┬────┘ └──┬──┘ └─┬──┘
     4-bit addr  2-bit   2-bit
     (0-15)      type    register
```

### Command Types

| Bits 3-2 | Command | Description |
|----------|---------|-------------|
| 00 | SendReset | Reset all devices to power-on state (address field ignored) |
| 01 | Flush | Clear device's internal data buffers |
| 10 | Listen | Host sends data bytes to device register |
| 11 | Talk | Host reads data bytes from device register |

### Key Command Bytes

| Byte | Meaning |
|------|---------|
| `0x00` | SendReset |
| `0x20` | Keyboard Flush |
| `0x2C` | Keyboard Talk R0 |
| `0x2F` | Keyboard Talk R3 |
| `0x30` | Mouse Flush |
| `0x3C` | Mouse Talk R0 |
| `0x3F` | Mouse Talk R3 |

## ADB Bus Protocol

### Signals

| Signal | Generated by | Description |
|--------|-------------|-------------|
| Attention | Computer | Long low pulse (800 us) marking start of every command |
| Sync | Computer | Short high pulse (65 us) following Attention, establishes bit timing |
| Global Reset | Computer | Bus held low for >= 3.0 ms; all devices reset |
| Service Request | Device | Device holds bus low for 140-260 us during stop bit to request service |

### Bit Encoding

Each bit is a "bit cell" consisting of low-then-high, distinguished by the
relative duration of the low portion:

- **"0" bit**: 65 us low, 35 us high (65% low time)
- **"1" bit**: 35 us low, 65 us high (35% low time)
- Total bit cell: 100 us nominal

### Timing Specifications

| Parameter | Nominal | Host Tolerance | Device Tolerance |
|-----------|---------|---------------|-----------------|
| Bit-cell time | 100 us | +/-3% | +/-30% |
| "0" low time | 65 us | 65% +/-5% | 65% +/-5% |
| "1" low time | 35 us | 35% +/-5% | 35% +/-5% |
| Attention low time | 800 us | +/-3% | — |
| Sync high time | 65 us | +/-3% | — |
| Stop bit low time | 70 us | +/-3% | +/-30% |
| Global Reset low time | 3 ms min | 3 ms min | 3 ms min |
| Service Request low time | 300 us | — | +/-30% |
| Stop-to-start time | 200 us | 140 us min / 260 us max | 140 us min / 260 us max |

### Error Conditions

- If bus stays low >= 3.0 ms, all devices interpret as Global Reset
- If bus stays high beyond max bit-cell time during a transaction, all devices
  ignore the command and wait for the next Attention signal

## ADB Devices

### Default Addresses

| Address | Device Type | Example |
|---------|------------|---------|
| `$00-$01` | Reserved | — |
| `$02` | Encoded devices | Keyboard |
| `$03` | Relative devices | Mouse |
| `$04` | Absolute devices | Graphics tablet |
| `$05-$07` | Reserved | — |
| `$08-$0F` | Any other | — |

Although ADB can address 16 devices, daisy-chaining more than 3 is not recommended
due to connector resistance and signal degradation.

### Device Registers

Each device may have up to 4 registers (0-3), each 2-8 bytes:

| Register | Purpose |
|----------|---------|
| 0 | Primary data register (key events, mouse movement). Must have data when device asserts SRQ |
| 1 | Device-specific data |
| 2 | Device-specific data (e.g., modifier key state on keyboards) |
| 3 | Status and identification: device address (bits 11-8), handler ID (bits 7-0), SRQ enable (bit 13) |

### Register 3 Bit Layout

| Bit | Description |
|-----|-------------|
| 15 | Reserved (must be 0) |
| 14 | Exceptional event, device specific (always 1 if unused) |
| 13 | Service Request enable (1 = enabled) |
| 12 | Reserved (must be 0) |
| 11-8 | Device address |
| 7-0 | Device Handler ID |

### Reserved Device Handler IDs

| ID | Function (via Listen R3) |
|----|--------------------------|
| `$FF` | Initiate self-test |
| `$FE` | Change address if no collision detected |
| `$FD` | Change address if activator is pressed |
| `$00` | Change address and enable fields to new values |

`$00` as a Talk R3 response indicates self-test failure.

### Service Request (SRQ)

When a non-active device has data, it asserts SRQ by holding the bus low during the
stop bit of any transaction (140-260 us beyond normal duration). The transceiver
reflects this as VIA1 port B bit 3 going low. The ADB Manager then polls each device
with Talk R0 until it finds the requester.

The last device to send data becomes the "active device" and receives automatic
Talk R0 polling every ~11 ms while idle.

## Keyboard

### Register 0 (Talk R0) — 2 Bytes

| Byte | Bit 7 | Bits 6-0 |
|------|-------|----------|
| 1 | Key status (0 = down, 1 = up) | ADB key code (7-bit) |
| 2 | Key status (0 = down, 1 = up) | ADB key code (7-bit) |

- Two key events can be reported per Talk R0
- Idle (no keys): `0xFF 0xFF`
- Key codes are 7-bit ADB virtual key codes (0x00-0x7F)
- Key-up transition codes = key-down code with bit 7 set

### Register 2 (Modifier Keys) — 2 Bytes

**Apple Standard Keyboard:**

| Bit | Key |
|-----|-----|
| 15 | Reserved |
| 14 | Delete |
| 13 | Caps Lock |
| 12 | Reset |
| 11 | Control |
| 10 | Shift |
| 9 | Option |
| 8 | Command |
| 7-0 | Reserved |

**Apple Extended Keyboard** adds:

| Bit | Key |
|-----|-----|
| 7 | Num Lock/Clear |
| 6 | Scroll Lock |
| 2 | LED 3 (Scroll Lock) |
| 1 | LED 2 (Caps Lock) |
| 0 | LED 1 (Num Lock) |

A 0 indicates key is down or LED is on. LEDs can be set via Listen R2.

### Handler IDs

- Standard Keyboard: `$02` (Apple Standard), `$01` (old)
- Extended Keyboard: `$02` default, `$03` enables separate right-hand modifier codes

## Mouse

### Register 0 (Talk R0) — 2 Bytes

| Bit | Meaning |
|-----|---------|
| 15 | Button status (1 = up, 0 = down) |
| 14-8 | Y move counts (7-bit signed, two's complement) |
| 7 | Not used (always 1 for single-button mouse) |
| 6-0 | X move counts (7-bit signed, two's complement) |

- Positive Y = down, negative Y = up
- Positive X = right, negative X = left
- Range: -64 to +63
- Idle (no movement, button up): `0x80 0x80`
- Deltas are accumulated and reset after each Talk R0

### Handler IDs

- `$01`: 100 +/-10 counts per inch (default on startup/reset)
- `$02`: 200 +/-10 counts per inch

## ROM and OS Interaction

### Boot Sequence (ADB Init)

1. ROM calls `_ADBReInit`
2. `SendReset` (`0x00`) sent to reset all devices to defaults
3. `Talk R3` sent to addresses 1-15 to enumerate connected devices
4. Start Manager resolves address conflicts via `Listen R3` with handler ID `$FE`
   (move device if no collision detected)
5. ADB device table built with default address, current address, and handler ID
6. Normal polling begins via `RunADBRequest` / VBL handler

### Address Conflict Resolution

At startup, if multiple devices share an address:
1. Start Manager sends Talk R3 — colliding devices detect collision
2. Losing device sets internal collision flag and disables movable address
3. Start Manager sends Listen R3 with handler ID `$FE` and new address — the
   non-colliding device moves
4. Repeat until Talk R3 times out (no more devices at that address)
5. Move one device back to the default address
6. Proceed to next address

### ADB Manager Traps

| Trap | Function |
|------|----------|
| `_ADBReInit` | Reinitialise bus: SendReset, rebuild device table |
| `_ADBOp` | Queue a Talk/Listen/Flush/SendReset (always asynchronous) |
| `_CountADBs` | Return number of ADB devices |
| `_GetIndADB` | Get device info by index |
| `_GetADBInfo` | Get device info by address |
| `_SetADBInfo` | Set device info |

`_ADBOp` is always asynchronous — it returns queueing success, not command success.
Completion is delivered via a completion routine callback.

### Polling Protocol

After startup:
1. ADB Manager sends Talk R0 to address `$03` (mouse, the default active device)
2. Transceiver repeats this Talk R0 every ~11 ms while idle
3. If a device asserts SRQ, the ADB Manager polls each device with Talk R0 until
   it finds the requester
4. The responding device becomes the new active device
5. The ROM's ADB VBL task fires every ~30 ticks (~500ms) to check device state

## SE/30 OS-Level Interrupt Handler (FDBShiftInt)

Source: `OS/IoPrimitives/ADBPrimitives.a` in the System 7.1 source tree.

The ROM ADB handler uses a coroutine-style resume mechanism. Each wait routine
saves its return address to `ShiftIntResume($138(A3))`. When the next SR
interrupt fires, `FDBShiftInt` jumps directly to that saved address.

### FDBShiftInt Entry (Coroutine Dispatcher)

```
FDBShiftInt:
    MOVEA.L  ADBBase,A3             ; A3 → ADB globals (from low-mem $0CF8)
    MOVEA.L  ShiftIntResume(A3),A0  ; A0 → resume address
    BTST     #vFDBInt,vBufB(A1)     ; test bit 3 of port B (sets Z flag)
    JMP      (A0)                   ; jump to resume handler (Z flag preserved)
```

The resume handler sees the Z flag from `BTST #3`:
- bit 3 = HIGH → Z=0 → `BNE` branches are taken
- bit 3 = LOW → Z=1 → `BNE` branches fall through

### Coroutine Mechanism

Each handler uses `BSR` to "call" a wait routine. The wait routine pops the
return address from the stack and saves it as `ShiftIntResume`, then does `RTS`
to return from the ISR. When the next VIA SR interrupt fires, `FDBShiftInt`
dispatches to the saved address, effectively resuming execution after the `BSR`.

```
; Caller:
    BSR.W   @getNextByte        ; "call" — pushes return addr, enters getNextByte
    ; execution resumes HERE on the next SR interrupt
    BNE.S   @noSRQ              ; Z flag set by FDBShiftInt's BTST #3

; @getNextByte:
    MOVE.B  vSR(A1),D0          ; read the byte from VIA shift register
    MOVE.B  D0,buffer(A3,cnt)   ; store in data buffer
    ADDQ.B  #1,fDBCnt(A3)       ; increment byte counter
    EOR.B   D1,vBufB(A1)        ; toggle ST bits (handshake with transceiver)
    MOVE.L  (A7)+,ShiftIntResume(A3) ; save return addr as resume point
    RTS                         ; return from ISR
```

### vADBInt (Bit 3) Semantics During Talk Reply

| Bit 3 Value | Z Flag | Meaning |
|-------------|--------|---------|
| 1 (HIGH) | 0 (clear) | Data present / line idle — continue fetching bytes |
| 0 (LOW) | 1 (set) | End of transfer or SRQ asserted — exit fetch loop |

The emulator sets bit 3 via `set_adb_int()`:
- **Real data byte**: `set_adb_int(true)` → bit 3 = HIGH → "more data"
- **Dummy end-of-transfer byte**: `set_adb_int(false)` → bit 3 = LOW → "done"

### Key Resume Handlers

| Handler | Set By | Purpose |
|---------|--------|---------|
| `@SendExit` | `@sendCmd` | Awaits command byte shift completion |
| `@waitForInput` | `@SendExit` | Awaits first reply byte; switches ACR to shift-in |
| `@getNextByte` | `@waitForInput`, self | Reads subsequent reply bytes from SR |
| `@sendByte` | Listen data phase | Shifts out data bytes to device |

### Three-Interrupt Sequence for a 2-Byte Talk Reply

| Interrupt | Bit 3 | Z | Action | fDBCnt |
|-----------|-------|---|--------|--------|
| 1st | 1 (data available) | 0 | Switch to shift-in mode, clear IFR_SR, toggle state; deliver byte 0 | 0 |
| 2nd | 1 (more data) | 0 | Read byte 0 from SR, deliver byte 1, toggle state | 1 |
| 3rd | 0 (done) | 1 | Read byte 1, exit fetch loop at `@fetchDone` | 2 |

The third interrupt delivers a dummy byte that is not consumed; bit 3 = 0 signals
end of data.

**No-reply scenario**: bit 3 = 0 on the first interrupt → sets `fDBNoReply`,
`fDBCnt` zeroed at `@fetchDone`.

### Data Replay Phase (Phase 2)

After the completion handler processes received data, the ROM replays processed
data back to the VIA SR using a separate function. This function reads from a
buffer, writes each byte to VIA SR, and toggles EVEN/ODD on port B:

```
; Data replay function (ROM $4F466):
    MOVEA.L  $134(A3),A0         ; buffer pointer
    MOVE.B   (A0)+,$1400(A1)     ; write byte TO VIA SR
    MOVE.L   A0,$134(A3)         ; update pointer
    SUBQ.B   #1,$163(A3)         ; decrement counter
    EOR.B    D1,(A1)             ; toggle port B state (EVEN↔ODD)
    MOVE.L   (A7)+,$138(A3)      ; save ShiftIntResume
    RTS                          ; coroutine exit
```

Two entry points control which ST bits are toggled:
- Entry at $4F460: `MOVEQ #$10,D1` — toggles ST0 only
- Entry at $4F464: `MOVEQ #$30,D1` — toggles both ST0 and ST1

**Emulation note**: The EVEN/ODD transitions during Phase 2 are ROM-driven SR
writes, NOT requests for the emulator to deliver data. The emulator must
suppress byte delivery scheduling during replay (see `dummy_sent` guard below).

### State Flow

```
@sendCmd       → write cmd to SR, set ACR mode 7, transition to CMD
@SendExit      → coroutine exit; resumes at @waitForInput on next interrupt
@waitForInput  → switch ACR to shift-in, read SR to clear IFR_SR, toggle state bits
@getNextByte   → read SR (clears IFR_SR), increment fDBCnt, store byte, toggle state
@fetchLoop     → repeat until 8 bytes collected or done condition
@fetchDone     → validate reply, check SRQ, return byte count
```

### ADB Low-Memory Globals

| Address | Name | Notes |
|---------|------|-------|
| `$0CF8` | ADBBase | Pointer to ADB globals block (A3 base) |

Offsets from A3 (ADBBase):

| Offset | Name | Notes |
|--------|------|-------|
| `$130(A3)` | fDBCmd | Active device list pointer |
| `$134(A3)` | Buffer pointer | Used by data replay function |
| `$138(A3)` | ShiftIntResume | Coroutine resume address for FDBShiftInt |
| `$15D(A3)` | (flags byte) | Bit 5 = ADB init in progress |
| `$15E(A3)` | FDBFlag | Bit 0 = fDBQEmpty, Bit 3 = FDBInit |
| `$163(A3)` | fDBCnt | Data byte counter for fetch/replay |

## Auto-Polling (ROM-Driven)

After the boot scan completes, the ROM's ADB Manager continuously sends Talk R0
commands to poll devices. This is **ROM-driven**, not GLU-driven: the CPU
initiates every transaction through VIA port B and the shift register.

### How the ROM Drives Auto-Poll

Source: `OS/ADBMgr/ADBMgr.a` in the System 7.1 source tree.

After the boot scan's final Talk R0 completes, the ADB init code calls
`RunADBRequest`. This function is the core of the auto-poll loop:

```
RunADBRequest:
    btst.b  #fDBQEmpty,fDBFlag(a3)    ; explicit commands queued?
    beq.s   @runFromQueue             ; yes → run queued command
    moveq.l #0,d2                     ; zero send byte count
    moveq.l #-1,d3                    ; negative = "resume polling"
    btst.b  #FDBInit,fDBFlag(a3)      ; still initializing?
    beq.s   @resumePolling            ; no → resume auto-poll
    rts                               ; yes → return (init drives commands)

@resumePolling:
    movea.l StartReqProc(a3),a0       ; get HW-dependent start procedure
    jmp     (a0)                      ; start the ADB request
```

`StartReqProc` is a hardware-dependent routine that:
1. Writes the Talk R0 command byte to VIA SR (in shift-out mode 7)
2. Transitions port B ST bits to state 0 (CMD)
3. Sets up `ShiftIntResume` for the first interrupt of the new transaction

The transaction completes via the normal FDBShiftInt coroutine chain. When
the response is fully received, `ImplicitRequestDone` processes the data
(dispatches to the device's completion routine), then calls `RunADBRequest`
again to start the next poll cycle.

### Explicit vs Implicit Commands

| Type | Trigger | Completion Handler |
|------|---------|-------------------|
| **Explicit** | `_ADBOp` trap (OS/app request) | `ExplicitRequestDone` → calls user completion routine |
| **Implicit** | `RunADBRequest` auto-poll loop | `ImplicitRequestDone` → dispatches to device handler, then calls `RunADBRequest` again |

Explicit commands take priority: if the command queue is non-empty,
`RunADBRequest` dequeues and runs the explicit command instead of polling.

### SRQ During Auto-Poll

The ROM checks vADBInt (bit 3) after receiving each Talk response to detect
Service Requests from other devices. Source: `OS/IoPrimitives/ADBPrimitives.a`:

```
BSR.W   @getNextByte              ; wait for the second byte
BNE.S   @noSRQ                    ; bit3=HIGH → Z=0 → no SRQ
BSET    #fDBSRQ,FDBAuFlag(A3)     ; bit3=LOW → SRQ detected
```

When SRQ is detected, the ADB Manager polls all devices to find the requester.
The SRQ check uses the Z flag left by FDBShiftInt's `BTST #3,vBufB(A1)`.

### Emulator Auto-Poll: Transceiver-Style Callback

The ROM's `RunADBRequest` → `StartReqProc` loop drives explicit Talk R0
commands during boot and when processing queued requests. However, once the
boot scan completes and the system enters steady state, the real ADB transceiver
IC takes over: it autonomously repeats the last Talk R0 command every ~11 ms
while in idle (state 3), firing IFR_SR whenever a device responds.

The emulator implements this via `adb_autopoll_deferred`, a scheduler callback:

1. **Scheduled on IDLE entry**: When `adb_port_b_output` transitions to state 3,
   it cancels any stale auto-poll and schedules a new one at 11 ms.
2. **Checks for pending data**: Mouse deltas, keyboard queue entries, or a held
   mouse button all count as pending.
3. **Polls `last_poll_addr`**: Always repeats the last Talk R0 target issued by
   the ROM (tracked in `last_poll_addr`). The emulator does NOT choose which
   device to poll — this matches real transceiver behaviour.
4. **Has-data path**: If `last_poll_addr` device has pending data, prepares a
   Talk R0 reply and fires IFR_SR with bit3=HIGH and a 0xFF wake-up byte in SR.
   The ROM's `FDBShiftInt` picks up from `ShiftIntResume` and fetches actual
   reply bytes via EVEN/ODD transitions (starting from `reply_index` 0).
5. **SRQ path**: If `last_poll_addr` device has no data but another device does,
   fires IFR_SR with bit3=LOW (SRQ). The ROM's SRQ handler then sends explicit
   Talk R0 commands to each registered device slot. Idle devices return no-reply
   (timeout), so the scan advances until it finds the device with data.
6. **Silent reschedule**: If no device has data, reschedules at 11 ms without
   firing IFR_SR — the ROM stays waiting.
7. **Fast reschedule on input**: `adb_keyboard_event` and `adb_mouse_event`
   reschedule the auto-poll with a short delay (`ADB_SHIFT_DELAY`) so new
   input is picked up promptly rather than waiting for the next 11 ms tick.

### Previous Misconception: GLU-Driven Data Delivery

An earlier iteration attempted to have the GLU proactively deliver data by
loading bytes into VIA SR and firing IFR_SR via a timer callback, without
respecting the ROM's transaction state machine. This was incorrect:

1. **ShiftIntResume is invalid between polls**: After a transaction completes,
   `ShiftIntResume` may point to a handler that expects a specific state. Firing
   IFR_SR asynchronously causes the ROM to jump to an invalid or stale handler.

2. **The ROM still drives the byte-fetch loop**: Even with transceiver-initiated
   polls, the ROM's EVEN/ODD transitions control byte delivery. The emulator
   must not push bytes into SR outside the transaction state machine.

### Timing

| Parameter | Value |
|-----------|-------|
| CPU clock (SE/30) | 16 MHz (62.5 ns/cycle) |
| Auto-poll interval | ~11 ms |
| ADB effective bit rate | ~10 kbps |
| Per-byte transmission time | ~800 us |
| Typical 2-byte response window | ~2-3 ms |
| SR shift duration (8 bits) | ~2.64 ms |

## Emulator Implementation Notes

### Entry Points

The ADB controller is driven primarily by the port B output callback:

1. **`adb_shift_byte` (shift_cb)**: Intentionally a **no-op**. On the SE/30,
   VIA1 SR operates in mode 7 (shift out under external clock). The ROM
   sometimes writes SR during interrupt handling while ACR is still mode 7,
   triggering spurious `sr_shift_complete` callbacks. The ADB module reads SR
   directly at each port B state transition instead.

2. **`adb_port_b_output` (output_cb)**: The main entry point. Called by the
   machine layer when the OS changes ST0/ST1 on port B. Handles all four states:

   - **State 0 (CMD)**: Read the command byte directly from `via_read_sr()`.
     Decode the command, prepare the reply buffer, and schedule a deferred
     completion event (`adb_shift_complete_deferred`) that fires IFR_SR after a
     realistic ADB bus delay.

   - **State 1/2 (EVEN/ODD)**: If a Listen command is active, read the data
     byte from SR. Otherwise, schedule deferred delivery of the next reply byte
     (or dummy) via `adb_deliver_next_byte_deferred`. A `dummy_sent` guard
     suppresses scheduling during the ROM's data replay phase (see below).

   - **State 3 (IDLE)**: Cancel stale `adb_shift_complete_deferred` and
     `adb_deliver_next_byte_deferred` events from the previous transaction.
     This is necessary because the ROM may transition CMD→IDLE without
     fetching reply data (aborted Talk during SRQ scan), leaving stale
     events that would fire spuriously. Detect aborted Talks (reply prepared
     but never fetched) and re-mark `mouse_data_pending` so the next
     auto-poll can retry delivery. Update vADBInt (bit 3) to reflect
     whether any device has pending data. Cancel any stale auto-poll event
     and schedule a new `adb_autopoll_deferred` callback at
     `ADB_AUTOPOLL_INTERVAL` (~11 ms).

### Port B Change Filtering

The machine layer (`se30.c`) must only call `adb_port_b_output` when the ST
bits (bits 5:4) actually change. The ROM frequently writes port B to update
other bits (sound volume, overlay control) without changing ST. Calling the ADB
handler for these writes would cause spurious command decodes.

### Deferred Events

| Event | Delay | Purpose |
|-------|-------|---------|
| `adb_shift_complete_deferred` | `ADB_SHIFT_DELAY` (~3 ms) | Fires IFR_SR after command byte shift-out completes. Simulates the real ADB bus timing. |
| `adb_deliver_next_byte_deferred` | `ADB_BYTE_DELAY` (~1.6 ms) | Delivers the next reply byte to VIA SR. Ensures the ROM ISR has time to finish before the next IFR_SR fires. |
| `adb_autopoll_deferred` | `ADB_AUTOPOLL_INTERVAL` (~11 ms) | Emulates the transceiver's idle-state auto-poll. Prepares a Talk R0 reply and fires IFR_SR to wake the ROM when a device has pending data. Reschedules silently when no data is pending. |

### dummy_sent Guard

After the end-of-transfer dummy byte is delivered, the ROM's completion handler
may enter a **data replay phase** (Phase 2) where it writes processed data back
to VIA SR and toggles EVEN/ODD on port B. These port B transitions trigger the
EVEN/ODD handler, which would normally schedule another `deliver_next_byte`.

The `dummy_sent` flag prevents this:
- Set to `true` when the dummy byte is delivered in `adb_deliver_next_byte()`
- Cleared when a new command is decoded in `adb_decode_command()`
- The EVEN/ODD handler checks `if (!dummy_sent)` before scheduling delivery

Without this guard, the emulator delivers spurious dummy bytes during replay,
overwriting the ROM's SR writes and corrupting the data stream.

### Null Responses

| Device | Has pending data | Response |
|--------|-----------------|----------|
| Keyboard (keys queued) | yes | 2-byte register 0 data (reply_len=2) |
| Mouse (movement/button change) | yes | 2-byte register 0 data (reply_len=2) |
| Keyboard (no keys pending) | no | no-reply (reply_len=0, bit 3 = LOW immediately) |
| Mouse (no movement, button up) | no | no-reply (reply_len=0, bit 3 = LOW immediately) |
| Unknown/absent address | — | no-reply (reply_len=0, bit 3 = LOW immediately) |

On real ADB hardware, a device with no pending data does **not** drive the bus in
response to Talk R0 — the transceiver sees a timeout. The emulator matches this:
`prepare_talk_reply()` checks `device_has_pending_data()` and returns a no-reply
(timeout) for idle devices. This is critical for the ROM's SRQ scan, which polls
each registered device with explicit Talk R0 commands after detecting an SRQ.
Idle devices must timeout so the scan advances to the device with actual data.

### Timing Constraints

- Do NOT call `via_input_sr()` during shift-out mode (ACR bits 4-2 = 111).
  `via_input_sr` in mode 7 sets IFR_SR only (does not change SR contents).
- The ROM switches ACR to shift-in mode (mode 3) in the ISR before reading SR.
- Reply bytes are delivered via deferred events, not synchronously from the
  port B handler. This ensures the ISR has finished and RTE'd before the next
  IFR_SR fires. Without this delay, the ROM reads uninitialised `ShiftIntResume`
  pointers.
- Cancel `adb_shift_complete_deferred` and `adb_deliver_next_byte_deferred`
  in the IDLE handler. When the ROM transitions CMD→IDLE without fetching
  reply data (aborted Talk during SRQ scan), these stale events would fire
  spuriously and confuse the ROM's ADB state machine. The CMD handler's
  shift-complete fires IFR_SR before IDLE is reached in normal transactions,
  so cancelling on IDLE entry is safe and only catches the orphaned case.

### Checkpointable State

**Save**: bus state, reply buffer (data/length/index), listen state
(active/buffer/index/addr/reg), keyboard queue + pressed-key tracking, mouse
button state + dx/dy deltas + data_pending flag, device addresses + handler IDs,
dummy_sent flag, last_poll_addr.

**Do not serialize**: VIA pointer, scheduler pointer (runtime references).

### Known Emulation Issues (BUG Tracker)

**BUG-003** (fixed): Delivering reply bytes synchronously from the port B handler
caused the ROM to read uninitialised `ShiftIntResume` pointers. Fix: use deferred
events with realistic ADB bus delays.

**BUG-004** (fixed): The ROM sometimes writes SR while ACR is still in mode 7,
triggering spurious `sr_shift_complete` callbacks. Fix: the `adb_shift_byte`
callback is a no-op; command bytes are read directly from `via_read_sr()` in the
CMD handler. Additionally, `set_adb_int(true)` at CMD start deasserts vADBInt
as real hardware would on seeing an attention pulse.

**BUG-006d** (fixed): The ROM's data replay phase (Phase 2) toggles EVEN/ODD on
port B, causing the emulator to schedule spurious dummy byte deliveries. Fix:
`dummy_sent` flag suppresses delivery after the end-of-transfer dummy.

**BUG-007** (fixed): After boot, the ROM's auto-poll loop (`RunADBRequest`)
was not issuing Talk R0 commands because the emulator had no transceiver-level
auto-poll. The real ADB transceiver IC repeats the last Talk R0 every ~11 ms in
idle state; this fires IFR_SR to restart the ROM's ADB state machine. Fix:
implemented `adb_autopoll_deferred`, a scheduler callback that prepares a Talk R0
reply and fires IFR_SR when a device has pending data. See
`docs/adb-debug-handover.md` for the full investigation history.

**BUG-009** (fixed): Mouse button release (`mouse-button up`) was lost on
SE/30 when delivered through the ADB hardware path. Two sub-issues:

(a) *Stale events from aborted Talk*: When the ROM transitions CMD→IDLE
without fetching reply data (aborting a Talk during its SRQ scan), pending
`adb_shift_complete_deferred` and `adb_deliver_next_byte_deferred` events
fired spuriously, confusing the ROM's ADB state machine and preventing
subsequent auto-polls from delivering data.

(b) *Lost data on aborted Talk*: During the ROM's SRQ scan, an explicit Talk R0
caused `prepare_mouse_reply()` to fill the reply buffer and clear
`mouse_data_pending`. If the ROM then aborted the Talk (CMD→IDLE without
reading data), the button-up event was lost — `mouse_data_pending` was false
and no future auto-poll would deliver it.

Fix: Cancel stale `shift_complete` and `deliver_next_byte` events on IDLE
entry. Detect aborted Talks (reply prepared but `reply_index == 0` and
`!dummy_sent`) and re-mark `mouse_data_pending = true` so the next auto-poll
retries delivery.

**BUG-008** (fixed): Mouse pointer froze on SE/30 after MacTest installed a
custom ADB mouse handler via `_SetADBInfo`. Two sub-issues:

(a) *Autopoll device override*: `adb_autopoll_deferred` chose which device to
poll based on pending data, overriding `last_poll_addr`. The ROM keeps a
device-handler pointer at `$134(ADBBase)` that corresponds to `last_poll_addr`;
overriding the target caused the ROM to dispatch mouse data to the keyboard
handler, which discarded it. Fix: always poll `last_poll_addr`; signal SRQ
(bit3=LOW) when a different device has data so the ROM's SRQ handler discovers it.

(b) *Talk R0 idle response*: `prepare_talk_reply` always returned 2-byte data for
known devices, even with nothing pending (keyboard: `FF FF`, mouse: `80 80`).
On real ADB, idle devices do not respond (bus timeout). This prevented the ROM's
SRQ scan from advancing past idle devices to the one with actual data. Fix: added
`device_has_pending_data()` check; idle devices now return no-reply (timeout).

## Source References

### ROM Addresses (SE/30 Universal ROM, checksum 0x97221136)

| Address | Function |
|---------|----------|
| `$40806D80` | ADB_BOOT_INIT — ADB initialization during boot |
| `$40806DD8` | BTST #5,$15D(A3) — boot scan completion poll loop |
| `$40806DE0` | JSR after poll loop — likely RunADBRequest call |
| `$40807002` | ADB_VIA1_SR_HANDLER — shift register interrupt handler |
| `$4F47E` | FDBShiftInt — ADB ISR entry (loads ShiftIntResume, BTST #3, JMP) |
| `$4F3A2` | Auto-poll SRQ handler |
| `$4F3D4` | @sendCommand — sends ADB command byte |
| `$4F3FA` | @waitForInput — awaits first reply byte |
| `$4F40C` | @getNextByte — reads subsequent reply bytes |
| `$4F460` | Data replay entry (D1=$10, toggles ST0 only) |
| `$4F464` | Data replay entry (D1=$30, toggles both ST bits) |
| `$4F466` | Data replay body — writes buffer to VIA SR, toggles port B |
| `$4F48C` | Completion handler — processes received ADB data |
| `$F7B8` | VIA2 CA1 RAM handler — dispatches slot interrupts via SIQ table |
