# Programmatic Mouse Control on Classic 68K Macs

This document is the single authoritative reference for programmatically
controlling the mouse cursor and simulating clicks in the Granny Smith
emulator.  It covers **all ADB-era machines** (SE, SE/30, Mac II family)
and the **Mac Plus** (pre-ADB, quadrature mouse).  For Mac Plus hardware
details (SCC quadrature encoding, VIA signals, connector pinout), see
[mouse.md](mouse.md).

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [The Complete Mouse Pipeline: ADB to Cursor](#2-the-complete-mouse-pipeline-adb-to-cursor)
3. [ADB Bus Protocol](#3-adb-bus-protocol)
4. [VIA Interrupt and ROM ADB State Machine](#4-via-interrupt-and-rom-adb-state-machine)
5. [SE/30 ROM Mouse Handler Disassembly](#5-se30-rom-mouse-handler-disassembly)
6. [Low-Memory Globals Reference](#6-low-memory-globals-reference)
7. [CrsrVBLTask: The VBL Cursor Task](#7-crsrvbltask-the-vbl-cursor-task)
8. [Framebuffer Compositing](#8-framebuffer-compositing)
9. [The Phantom ADB Data Problem](#9-the-phantom-adb-data-problem)
10. [The MTemp Guard](#10-the-mtemp-guard)
11. [Emulator Debugger Commands](#11-emulator-debugger-commands)
12. [Click Mechanism: PostEvent and Applications](#12-click-mechanism-postevent-and-applications)
13. [Mac Plus: MBTicks Hack and Quadrature Mouse](#13-mac-plus-mbticks-hack-and-quadrature-mouse)
14. [How Other Emulators Inject Mouse Input](#14-how-other-emulators-inject-mouse-input)
15. [Dead Ends and Debunked Claims](#15-dead-ends-and-debunked-claims)
16. [End-to-End Timing](#16-end-to-end-timing)
17. [Quick Reference for Test Authors](#17-quick-reference-for-test-authors)

---

## 1. Executive Summary

Classic Mac OS provides **no official API for programmatic mouse control** --
there is no `SetMouse` trap.  Instead, the universal technique is direct
manipulation of three low-memory globals: **MTemp** (`$0828`), **RawMouse**
(`$082C`), and **CrsrNew** (`$08CE`).  This approach was pioneered by
ChromiVNC, refined by MiniVNC, and adopted by every major emulator (Basilisk
II, Mini vMac, SheepShaver, MAME).

On the SE/30, the ROM mouse handler at `$408074CE` is remarkably simple:

- It reads 2-byte ADB response data from `ADBBase+$164`/`$165`.
- It extracts 7-bit signed deltas via sign extension.
- It adds those deltas **directly to MTemp** (`$0828`/`$082A`).
- **There is no private position accumulator** -- MTemp IS the accumulator.
- **CrsrThresh is never referenced** -- there is no acceleration in this handler.
- Button state changes trigger `_PostEvent` **before** deltas are applied.

Writing MTemp directly via `set-mouse --global` is therefore correct and
deterministic.  The **MTemp guard** (a 1 kHz scheduler event) continuously
pins the position to prevent drift from phantom ADB data in the shared
response buffer.

### The Universal Positioning Technique

The technique used by all major emulators and VNC servers:

```c
/* Move cursor to (x, y) in global screen coordinates */
*(Point *)0x0828 = newPos;           /* MTemp */
*(Point *)0x082C = newPos;           /* RawMouse */
*(char *)0x08CE = *(char *)0x08CF;   /* CrsrNew = CrsrCouple */
```

If System 7's `LowMem.h` accessors are available:

```c
LMSetMouseTemp(newPos);
LMSetRawMouseLocation(newPos);
LMSetCursorNew(LMGetCrsrCouple());
```

For button simulation, MBState + PostEvent:

```c
*(char *)0x0172 = 0x00;        /* MBState = button down */
PostEvent(mouseDown, 0L);       /* event code 1 */
/* ... hold ... */
*(char *)0x0172 = 0x80;        /* MBState = button up */
PostEvent(mouseUp, 0L);         /* event code 2 */
```

The related `PPostEvent` variant returns a pointer to the queue element for
further modification (e.g., setting the modifier keys field).

---

## 2. The Complete Mouse Pipeline: ADB to Cursor

The journey from mouse movement to cursor redraw spans six layers:

```
Layer 1: ADB Bus         Mouse sends 2-byte Talk R0 response (~11ms polling)
Layer 2: VIA1 SR          Shift register fires IFR bit 2 interrupt
Layer 3: ROM State Machine FDBShiftInt reads bytes, manages EVEN/ODD/IDLE states
Layer 4: adb_exit_idle    Dispatches to registered device handler
Layer 5: Mouse Handler    Adds deltas to MTemp, posts button events
Layer 6: CrsrVBLTask      Clips MTemp → writes Mouse → redraws cursor (~60Hz)
Layer 7: Application      Reads Mouse ($0830) or event record where field
```

**Key invariant:** The interrupt-time handler writes MTemp; the VBL task
reads MTemp and writes Mouse.  Applications see the final position via
Mouse or the event `where` field.

---

## 3. ADB Bus Protocol

The Apple Desktop Bus is a **single-wire, open-collector, self-clocking
serial bus** pulled up to +5V.  Bit encoding uses a **100μs bit cell**: a '0'
bit holds the line low for 65μs then high for 35μs; a '1' bit holds low for
35μs then high for 65μs.  Effective bandwidth is ~10 kbit/s, with practical
throughput of 100-200 bytes/sec due to protocol overhead.  The host always
initiates; devices never speak unsolicited.

### Talk Register 0 Transaction

Every ADB command begins with an 8-bit command byte `AAAA_CCRR` -- four
address bits, two command bits, two register bits.  For the standard mouse
at default address `$3`, Talk R0 is `$3C`.

1. Host pulls bus low 800us (attention), releases 65us (sync)
2. Host clocks out 8-bit command, MSB first, 100us/bit
3. 140-260us gap (Tlt)
4. Device responds with 16 bits + stop bit (~1.9ms), or stays silent (no data)

### Mouse Register 0 Data Format

```
Byte 0:  [B1] [Y6..Y0]    B1: button 1 (0=pressed, 1=released, active-low)
Byte 1:  [B2] [X6..X0]    B2: button 2 (always 1 on single-button mice)

Y6-Y0: Y delta, 7-bit two's complement (-64 to +63, negative = up)
X6-X0: X delta, 7-bit two's complement (-64 to +63, negative = left)
```

Deltas represent accumulated motion since the last successful Talk R0.  After
responding, the device clears its internal accumulator.

Handler ID 1 = 100 cpi; handler ID 2 = 200 cpi.  Extended protocol (handler
ID 4) adds bytes for third/fourth buttons (System 7.5+ Cursor Device Manager).

### Auto-Poll and SRQ

The ADB transceiver autonomously polls the "active device" (normally the
mouse at address `$3`) every ~11ms (~91 polls/second).  Apple TN HW01 claims
up to 150 polls/second under optimal conditions.

On the **Mac SE**, the transceiver is a PIC16CR54 (512-byte mask ROM); on the
**Mac II/IIcx/SE/30**, a similar ASIC.  Starting with the IIsi/Classic II,
Apple consolidated into the Egret (68HC05E1) and later Cuda chips.

When a non-active device needs attention, it asserts **SRQ** by holding the
bus low for 300us during any transaction's stop bit.  The transceiver signals
SRQ via **VIA1 Port B bit 3** (active low).  The ROM sweeps all known
addresses until the requesting device responds and becomes the new active
device.

---

## 4. VIA Interrupt and ROM ADB State Machine

### VIA1 Shift Register Interrupt

ADB response bytes transfer through VIA1 SR (external clock mode).  After 8
bits shift in, VIA sets **IFR bit 2**, asserting a **level-1 autovector**
interrupt.  The Lvl1DT dispatch table at `$0192` routes IFR bit 2 to the
ADB handler (`FDBShiftInt`).

### VIA1 IFR Bit Assignments

| Bit | Source | Signal |
|-----|--------|--------|
| 0 | CA2 | One-second interrupt (RTC) |
| 1 | **CA1** | **VBlank** (vertical blanking) |
| 2 | **SR** | **ADB data ready** (Shift Register) |
| 3 | CB2 | ADB data line |
| 4 | CB1 | ADB clock line |
| 5 | T2 | Timer 2 |
| 6 | T1 | Timer 1 |
| 7 | IRQ | Composite |

### ROM State Machine

`FDBShiftInt` manages bus state via VIA1 Port B bits 4-5 (ST0/ST1):

| State | Meaning |
|-------|---------|
| CMD (0) | OS writes a command byte to SR |
| EVEN (1) | First reply byte expected |
| ODD (2) | Second reply byte expected |
| IDLE (3) | Transaction complete; transceiver may auto-poll |

After all reply bytes are received and the end-of-transfer dummy byte signals
completion (vADBInt = LOW, port B bit 3), the ROM calls `adb_exit_idle`.

### Device Handler Dispatch (`adb_exit_idle` at `$40807376`)

```asm
40807376  MOVEM.L   D0/A0-A3,-(A7)
4080737a  MOVEA.L   $0CF8,A3          ; A3 = ADBBase
4080737e  MOVE.L    $134(A3),D0       ; handler pointer
40807382  BEQ.S     skip              ; no handler → skip
40807384  MOVEA.L   D0,A1             ; A1 = handler function
40807386  MOVEA.L   $130(A3),A0       ; A0 = response buffer pointer
4080738c  MOVE.B    $160(A3),D0       ; byte count received
40807392  MOVE.B    $15C(A3),D0       ; ADB command byte
40807396  MOVEA.L   $138(A3),A2       ; A2 = device data area pointer
4080739a  JSR       (A1)              ; CALL DEVICE HANDLER
```

| Register | Contents |
|----------|----------|
| **A0** | Response buffer pointer |
| **A1** | Handler function |
| **A2** | Handler's private data area (dbDataAreaAddr) |
| **A3** | ADBBase |
| **D0** | ADB command byte |

For mouse (address 3): handler at `$408074CE`.
For keyboard (address 2): handler at `$4080753A`.

The handler at `$134(A3)` was set during the original Talk R0 command setup.
The handler executes at **interrupt time** and must follow interrupt-level
restrictions.

---

## 5. SE/30 ROM Mouse Handler Disassembly

Full disassembly verified against the headless emulator using breakpoints and
register inspection.

```asm
; ═══════════════════════════════════════════════════════════════════
; ADB MOUSE DEVICE HANDLER — $408074CE .. $40807538  (106 bytes)
;
; Entry:
;   A3 = ADB globals base (from $0CF8)
;   ADBBase+$164 = byte 0 (button + Y delta)
;   ADBBase+$165 = byte 1 (button + X delta)
;   A2 = device data area pointer (UNUSED by this handler)
;
; Exit:
;   MTemp updated; button event posted if state changed
; ═══════════════════════════════════════════════════════════════════

; ─── Button State Processing ───

408074CE  7001      MOVEQ     #$01,D0           ; D0 = 1 (mouseDown)
408074D0  142B 0164 MOVE.B    $164(A3),D2       ; D2 = byte 0
408074D4  6A02      BPL.S     $408074D8         ; bit 7 clear → pressed
408074D6  7002      MOVEQ     #$02,D0           ; D0 = 2 (mouseUp)

408074D8  1238 0172 MOVE.B    $0172,D1          ; D1 = MBState
408074DC  B501      EOR.B     D2,D1             ; XOR: detect toggle
408074DE  6A28      BPL.S     $40807508         ; no toggle → skip to delta

; ─── Button Toggled → Post Event ───

408074E0  2238 016A MOVE.L    $016A,D1          ; D1 = Ticks
408074E4  92B8 016E SUB.L     $016E,D1          ; D1 -= MBTicks
408074E8  0C81 0001 CMPI.L    #$00000001,D1     ; debounce: skip if < 1 tick
408074EE  6D18      BLT.S     $40807508         ;   → skip to delta
408074F0  D3B8 0156 ADD.L     D1,$0156          ; DoubleTime += delta
408074F4  21F8 016A MOVE.L    $016A,$016E       ; MBTicks = Ticks
                    016E
408074FA  0202 0080 ANDI.B    #$80,D2           ; isolate button bit
408074FE  11C2 0172 MOVE.B    D2,$0172          ; MBState = new state
40807502  2040      MOVEA.L   D0,A0             ; A0 = event code
40807504  7000      MOVEQ     #$00,D0           ; D0 = 0 (message)
40807506  A02F      _PostEvent                   ; *** POST EVENT ***

; ─── Y Delta Processing ───

40807508  707F      MOVEQ     #$7F,D0           ; mask
4080750A  C02B 0164 AND.B     $164(A3),D0       ; D0 = Y delta (7-bit)
4080750E  D000      ADD.B     D0,D0             ; shift left: bit 6→7
40807510  E200      ASR.B     #1,D0             ; sign-extend bit 7→byte
40807512  4880      EXT.W     D0                ; byte → word
40807514  670A      BEQ.S     $40807520         ; zero → skip
40807516  D178 0828 ADD.W     D0,$0828          ; *** MTemp.v += delta ***
4080751A  11F8 08CF MOVE.B    $08CF,$08CE       ; CrsrNew = CrsrCouple
          08CE

; ─── X Delta Processing ───

40807520  707F      MOVEQ     #$7F,D0           ; mask
40807522  C02B 0165 AND.B     $165(A3),D0       ; D0 = X delta (7-bit)
40807526  D000      ADD.B     D0,D0             ; shift left
40807528  E200      ASR.B     #1,D0             ; sign-extend
4080752A  4880      EXT.W     D0                ; byte → word
4080752C  670A      BEQ.S     $40807538         ; zero → skip
4080752E  D178 082A ADD.W     D0,$082A          ; *** MTemp.h += delta ***
40807532  11F8 08CF MOVE.B    $08CF,$08CE       ; CrsrNew = CrsrCouple
          08CE

40807538  4E75      RTS
```

### Key Findings

1. **No private accumulator.** The handler never references A2 (data area
   pointer).  Deltas are added directly to MTemp.  MTemp IS the accumulator.

2. **No acceleration.** CrsrThresh (`$08EC`) is never referenced.  The
   `ADD.B D0,D0` / `ASR.B #1,D0` sequence is standard 7-bit to 8-bit sign
   extension: shift bit 6 into the sign position, then arithmetic shift right
   to propagate it.

3. **PostEvent fires before deltas.** `_PostEvent` at `$40807506` runs before
   the delta block at `$40807508`.  This means PostEvent reads Mouse (`$0830`)
   while it still reflects the pre-delta position -- so `event.where` is
   always correct even if phantom deltas later corrupt MTemp.

4. **Handler reads `$164(A3)` twice.** Once at `$408074D0` for button state,
   again at `$4080750A` for Y delta.  PostEvent executes between the two reads
   but in the same interrupt context, so no buffer modification risk.

5. **Debounce via MBTicks.** At `$408074E4`, the handler subtracts MBTicks
   from current Ticks.  If the difference is < 1 tick, the button event is
   skipped.  On the Mac Plus VIA path, this same mechanism is exploited by the
   "MBTicks hack" (see [Section 13](#13-mac-plus-mbticks-hack-and-quadrature-mouse)).

---

## 6. Low-Memory Globals Reference

### Mouse Position Globals

All Point-typed globals are 4 bytes: 16-bit vertical in the high word, 16-bit
horizontal in the low word (big-endian).

| Address | Name | Size | Role |
|---------|------|------|------|
| `$0828` | **MTemp** | 4 (Point) | Interrupt-level cursor position -- the "input" to CrsrVBLTask.  On the SE/30, the ROM mouse handler adds deltas directly here. |
| `$082C` | **RawMouse** | 4 (Point) | Raw/unprocessed mouse position (written by handler alongside MTemp on some ROMs; on SE/30, only MTemp is updated by the handler) |
| `$0830` | **Mouse** | 4 (Point) | Final processed position.  Written only by CrsrVBLTask after clipping to CrsrPin.  What `GetMouse` and `_PostEvent` read. |
| `$0834` | **CrsrPin** | 8 (Rect) | Cursor bounding rectangle.  Single-monitor: screen bounds.  Multi-monitor: gray region. |

### Cursor Control Globals

| Address | Name | Size | Role |
|---------|------|------|------|
| `$08CC` | CrsrVis | byte | Cursor visibility flag |
| `$08CD` | CrsrBusy | byte | Reentrance guard for cursor drawing |
| `$08CE` | **CrsrNew** | byte | Set by handler to signal VBL: cursor moved |
| `$08CF` | **CrsrCouple** | byte | Normally `$FF`; cursor coupled to mouse |
| `$08D0` | CrsrState | word | Hide/show nesting counter |
| `$08D2` | CrsrObscure | byte | Cursor obscured (typing hid it) |
| `$08D6` | MouseMask | long | AND mask for grid snapping |
| `$08DA` | MouseOffset | long | Offset added after masking |
| `$08EC` | **CrsrThresh** | word | Acceleration threshold -- **NOT used by SE/30 handler** |
| `$08EE` | **JCrsrTask** | long | VBL cursor task vector |

### Mouse Button Globals

| Address | Name | Size | Role |
|---------|------|------|------|
| `$0172` | **MBState** | byte | `$00` = button down, `$80` = button up (bit 7) |
| `$016E` | **MBTicks** | long | Tick when button last changed; debounce timer |
| `$0156` | DoubleTime | long | Double-click interval accumulator |

### ADB Globals (offsets from ADBBase at `$0CF8`)

| Offset | Name | Description |
|--------|------|-------------|
| `$130` | Response buffer ptr | Points to `ADBBase+$164` |
| `$134` | Handler pointer | Current device handler function |
| `$138` | Data area pointer | `dbDataAreaAddr` for current device |
| `$15C` | Command byte | Last ADB command |
| `$15D` | ADB flags | Bus-busy, queue-full, low-line |
| `$15F` | Pending-work flags | State machine mode bits |
| `$160` | Byte count | Bytes received in current transaction |
| `$163` | Expected length | Expected byte count |
| `$164` | Response byte 0 | **Shared ADB response buffer** (Y delta + button) |
| `$165` | Response byte 1 | **Shared ADB response buffer** (X delta + button) |

**ADBBase** is typically `$24B0` on a booted SE/30 with MacTest.

### ADB Device Table Entry (ADBDataBlock, 10 bytes)

Each device in the ADB device table is described by a 10-byte `ADBDataBlock`:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0 | 1 | devType | Device handler ID (1 or 2 for mouse) |
| +1 | 1 | origADBAddr | Original/default ADB address |
| +2 | 4 | dbServiceRtPtr | Pointer to device handler routine |
| +6 | 4 | dbDataAreaAddr | Pointer to handler's private data area |

A custom handler can be installed via `SetADBInfo` to fully intercept the
input path -- receiving raw ADB data, processing it, and controlling what
reaches MBState and the event queue.

### VIA Register Addresses

| Machine class | VIA1 base | VIA2 base |
|---------------|-----------|-----------|
| Compact Macs (SE, Classic) | `$EFE1FE` region | N/A |
| Mac II-class (II, IIx, IIcx, SE/30) | `$50F00000` | `$50F02000` |

Registers are spaced `$200` apart on Mac II-class (e.g., Shift Register at
VIA1+`$1400`, IFR at VIA1+`$1A00`).  Low-memory globals: VIA at `$01D4`
points to VIA1 base; VIA2Base at `$0CEC` points to VIA2.

---

## 7. CrsrVBLTask: The VBL Cursor Task

### How the VBL Fires

On **compact Macs** (SE, Classic), VBlank from built-in video connects to
VIA1 CA1 (IFR bit 1) at ~60.15 Hz.  On **NuBus Macs** (II, IIci, etc.), the
cursor task is a slot-based VBL task synchronized to the display's retrace
(VIA2 CA1, level-2 interrupt).  On the **SE/30**, the internal video slot E
fires via VIA2 CA1.

On **multi-monitor setups**, the cursor VBL fires on whichever display
currently contains the cursor.  When the cursor crosses monitors, the system
migrates the VBL task to the new display's slot.  The global **CrsrDevice**
(`$089C`) tracks which GDevice handle owns the cursor.

### Cursor Jump Vector Block

Cursor routines are accessed through jump vectors in low memory:

| Address | Vector | Called when |
|---------|--------|-----------|
| `$0800` | JHideCursor | HideCursor removes cursor, decrements CrsrState |
| `$0804` | JShowCursor | ShowCursor displays cursor, increments CrsrState |
| `$0808` | JShieldCursor | ShieldCursor hides if cursor overlaps a given rect |
| `$080C` | JScrnAddr | Computes framebuffer address from screen coordinates |
| `$0810` | JScrnSize | Returns screen dimensions |
| `$0814` | JInitCrsr | InitCursor resets to arrow, sets CrsrState to 0 |
| `$0818` | JSetCrsr | SetCursor changes current cursor image (writes TheCrsr at `$0844`) |
| `$081C` | JCrsrObscure | ObscureCursor hides cursor until next mouse movement |
| `$08EE` | **JCrsrTask** | **CrsrVBLTask -- the VBL cursor update routine** |

JCrsrTask is the only vector called automatically at interrupt time.  The
others are called by Toolbox routines or application code.  JShieldCursor is
called by QuickDraw before drawing to screen regions that might overlap the
cursor -- this prevents cursor "turds".

### CrsrVBLTask Sequence

Invoked through `JCrsrTask` (`$08EE`):

1. Check CrsrNew -- if zero, return (no update)
2. Check CrsrBusy -- if non-zero, defer (reentrance guard)
3. Set CrsrBusy = `$FF`
4. Read MTemp
5. Clip to CrsrPin bounding rect
6. Write Mouse (`$0830`) -- the official application-visible position
7. Apply MouseMask and MouseOffset (optional grid snapping)
8. Compute cursor image origin (subtract hotspot from Mouse)
9. Erase old cursor (restore save-behind buffer)
10. Save new background
11. Composite cursor image using mask+data algorithm
12. Update CrsrRect (`$083C`) and CrsrAddr (`$0888`)
13. Handle CrsrObscure (make cursor visible if it was obscured and mouse moved)
14. Clear CrsrNew
15. Clear CrsrBusy

**Mouse (`$0830`) is never written by the interrupt handler** -- only by
CrsrVBLTask.  This is the value that applications see via GetMouse and that
PostEvent reads for event.where.

The JCrsrTask vector can also be called directly by software for immediate
cursor updates (Elliot Nunn's QEMU USB tablet driver does this), but no
standard Apple ROM code does so.

---

## 8. Framebuffer Compositing

### B&W Cursor Drawing

The classic Mac cursor is a 16x16 pixel image defined by a Cursor record
(32 bytes image, 32 bytes mask, 4-byte hotspot).  The mask+data compositing
algorithm:

| Mask | Data | Screen result |
|------|------|---------------|
| 1 | 0 | White (forced) |
| 1 | 1 | Black (forced) |
| 0 | 0 | Unchanged (transparent) |
| 0 | 1 | Inverted (XOR) |

Erasure restores the save-behind buffer -- no XOR artifacts.

### Color Cursor Drawing

Color Macs use CCrsr records (crsrType `$8000` = B&W, `$8001` = color).
At depth >2 bpp, pre-expanded color pixel data from crsrXData replaces mask
pixels directly.  Pre-expansion at SetCCursor time ensures the VBL draw is a
fast block transfer.

### CrsrBusy Semaphore

CrsrBusy (`$08CD`) prevents reentrant corruption.  CrsrVBLTask sets it before
touching the framebuffer and clears it after.  Any other code (SetCursor,
HideCursor, spinning cursors) checks CrsrBusy first and defers if set.

---

## 9. The Phantom ADB Data Problem

### Symptom

After `set-mouse --global` writes MTemp to exact coordinates, subsequent ADB
auto-poll activity can corrupt MTemp by applying phantom deltas.  Typical
observed corruption: MTemp.v shifts by +60 pixels.

### Root Cause

The ADB response buffer at `ADBBase+$164`/`$165` is **shared between all ADB
devices**.  When the keyboard auto-poll delivers data, the ROM stores keyboard
bytes at `$164`/`$165`.  If the mouse handler subsequently runs (button event,
stale auto-poll response), it reads `$164` and interprets the keyboard data as
mouse deltas.

### Mechanism in Detail

1. Emulator's ADB transceiver alternates auto-poll between keyboard (address
   2) and mouse (address 3) via `last_poll_addr`.
2. Keyboard poll delivers key data to `ADBBase+$164`/`$165`.
3. Keyboard handler processes its data normally.
4. Buffer now contains keyboard data (e.g., `$3C $FF`).
5. Later, mouse data becomes pending (e.g., button press).
6. Emulator delivers mouse bytes, overwriting the buffer.
7. Mouse handler runs and processes correct data.
8. **However**, under certain timing conditions (SRQ scan, interleaved
   polling), the mouse handler can be invoked a second time with stale
   buffer contents, applying phantom deltas.

### Observed Phantom Data

| Buffer | Decoded | Effect on MTemp |
|--------|---------|-----------------|
| `$3C $FF` | dy=+60, dx=-1 | v += 60, h -= 1 |
| `$03 $FE` | dy=+3, dx=-2 | v += 3, h -= 2 |
| `$FF $FF` | dy=-1, dx=-1 | v -= 1, h -= 1 |

The `$3C` value (60 decimal) appears consistently and is likely a keyboard
scan code or ADB protocol byte left from boot-time initialization.

### Impact on Click Events

The phantom data does **not** affect PostEvent's `where` field, because
PostEvent fires before deltas are applied (see handler disassembly, section
5).  However, the corrupted MTemp IS subsequently copied to Mouse at the next
VBL tick.  Applications reading Mouse in a tracking loop (e.g., TrackControl
during button hold) will see the corrupted position.

### Measured Error

From position `(15,15)`, `set-mouse 95 295` (default ADB mode) produced
`(313, 96)` -- an error of `(+18, +1)` pixels.  This error was identical
with and without SCSI loopback, confirming it is an ADB timing issue.

---

## 10. The MTemp Guard

### Purpose

Prevents phantom ADB data from corrupting MTemp after `set-mouse --global`.

### Design

A periodic scheduler event fires every 1ms and checks whether MTemp has
drifted from the target position.  If drift detected, it restores MTemp,
RawMouse, and Mouse.

```c
#define MOUSE_GUARD_INTERVAL_NS (1 * 1000 * 1000) // 1 ms

static void mouse_guard_tick(void *source, uint64_t data) {
    if (!mouse_guard_active)
        return;

    int16_t cur_v = (int16_t)memory_read_uint16(0x0828);
    int16_t cur_h = (int16_t)memory_read_uint16(0x082A);

    if (cur_v != mouse_guard_v || cur_h != mouse_guard_h) {
        memory_write_uint16(0x0828, (uint16_t)mouse_guard_v);
        memory_write_uint16(0x082A, (uint16_t)mouse_guard_h);
        memory_write_uint16(0x082C, (uint16_t)mouse_guard_v);
        memory_write_uint16(0x082E, (uint16_t)mouse_guard_h);
        memory_write_uint16(0x0830, (uint16_t)mouse_guard_v);
        memory_write_uint16(0x0832, (uint16_t)mouse_guard_h);
    }

    scheduler_new_cpu_event(sched, &mouse_guard_tick,
                            NULL, 0, 0, MOUSE_GUARD_INTERVAL_NS);
}
```

### Guard Lifecycle

| Event | Guard state |
|-------|-------------|
| `set-mouse X Y --global` | **Activated** at (X, Y) |
| `set-mouse X Y --global` (again) | **Updated** to new (X, Y) |
| `set-mouse X Y` (default mode) | **Deactivated** |
| `set-mouse X Y --hw` | **Deactivated** |
| `mouse-button down` | Guard stays active (no deactivation) |
| `mouse-button up` | Guard stays active (no deactivation) |

The guard remains active until the next `set-mouse` call.  It is NOT
deactivated by mouse-button commands.  This ensures the cursor stays pinned
during TrackControl's tracking loop (which reads Mouse repeatedly while the
button is held).

### Timing Analysis

- Guard: every 1 ms
- Phantom corruption: every ~11 ms (ADB poll rate)
- VBL copies MTemp → Mouse: every ~16 ms

Worst case: phantom corrupts MTemp at t=0, guard restores at t<=1ms, VBL
copies restored MTemp at t<=17ms.  The guard corrects 16x faster than VBL
reads.

### Workaround Status

The MTemp guard is a **workaround**, not a root cause fix.  The underlying
problem -- stale keyboard data at `ADBBase+$164`/`$165` being read by the
mouse handler -- remains in the emulator's ADB state machine.  Possible
approaches for a proper fix:

- **Buffer pre-clear:** In `adb_autopoll_deferred()`, write zero-delta mouse
  bytes (`$80 $80`) to `ADBBase+$164`/`$165` via direct memory access before
  signaling the ROM.  This is the same approach used by ChromiVNC and MiniVNC.
- **SRQ scan audit:** Trace the ROM's SRQ scan path to understand exactly when
  and why the mouse handler is called with keyboard buffer data.  May reveal a
  state machine timing issue in the emulator.

### Implementation

Source: `src/core/debug/debug_mac.c`

- `mouse_guard_start(h, v)`: registers scheduler event, sets target, activates
- `mouse_guard_tick()`: checks and corrects MTemp; reschedules
- `mouse_guard_stop()`: sets `mouse_guard_active = false`

---

## 11. Emulator Debugger Commands

### set-mouse

```
set-mouse [--global|--hw] X Y
```

| Mode | Behavior |
|------|----------|
| `--global` | Writes MTemp, RawMouse, Mouse to (X, Y).  Sets CrsrNew = CrsrCouple.  **Activates MTemp guard.**  Recommended for test scripts. |
| `--hw` | Injects relative deltas (X, Y) through ADB hardware.  Deactivates guard. |
| (default) | Computes delta from current MTemp to (X, Y), injects via ADB.  Subject to ~6px phantom data error.  Deactivates guard. |

**Coordinate convention:** `set-mouse X Y` where X = horizontal (column),
Y = vertical (row).  Origin (0, 0) = top-left of screen.  The Mac Point struct
stores (v, h) = (Y, X), but the command uses the more natural (X, Y) order.

**Known MacTest SE/30 button coordinates:**
- Commencer button: `set-mouse 100 121 --global`
- Floppy dialog OK: `set-mouse 370 185 --global`

### mouse-button

```
mouse-button [--global|--hw] up|down
```

| Mode | Behavior |
|------|----------|
| `--global` | Writes MBState directly.  No event posted.  Only works for code that polls MBState (ModalDialog).  On Mac Plus, also sets MBTicks to future value (MBTicks hack). |
| `--hw` / (default) | Routes through ADB: `adb_mouse_event(button, 0, 0)`.  ROM handler writes MBState and calls `_PostEvent`.  Works for both ModalDialog and WaitNextEvent code. |

### post-event

```
post-event <what> <message>
```

Directly injects a Mac OS event into the Event Manager queue.  Finds a free
`EvQEl` slot in the pre-allocated pool and links it at the tail of
`EvQHdr` (`$014A`).  Event `where` field is filled from the current Mouse
global.

Usage example: `post-event 7 1` posts a `diskEvt` (event code 7) with
message 1 (drive number), used when the .Sony VBL task has stopped polling
CSTIN.

### mac-state

```
mac-state
```

Prints current Mouse, MTemp, MBState, CrsrNew, CrsrCouple, and Ticks.

Example output:
```
Mouse:   pos=(185,370)  button=UP  MBState=$80
Cursor:  MTemp=(185,370)  CrsrNew=$00  CrsrCouple=$FF
Ticks:   17077
```

Note: `mac-state` prints `pos=(v, h)` = `pos=(Y, X)`.

### parse_mode_flag

A helper in `debug_mac.c` that scans all argv positions for `--global`/`--hw`,
removing the flag and shifting remaining args.  This fixes a bug where the
original implementation only checked `argv[1]`, causing flags at the end
(`set-mouse 95 295 --global`) to be silently ignored.

---

## 12. Click Mechanism: PostEvent and Applications

### WaitNextEvent-based Code (Standard Event Loop)

1. `mouse-button down` → ADB event → ROM handler → `_PostEvent(mouseDown, 0)`
2. `_PostEvent` reads Mouse (`$0830`) → `event.where` = current position
3. Application calls `WaitNextEvent` → gets the event record
4. Application calls `FindWindow(event.where)` → identifies target window
5. Application calls `FindControl(event.where)` → identifies target control
6. Application calls `TrackControl(control, event.where)` → tracks mouse
7. TrackControl reads Mouse in a loop until button release
8. If Mouse is inside control at release → action fires

**The MTemp guard ensures Mouse always reflects the target position at step
7**, preventing phantom data from moving the cursor outside the button during
the hold period.

### ModalDialog-based Code

ModalDialog runs its own event loop using GetNextEvent internally.  It needs:
- A mouseDown event in the queue (from PostEvent via ADB path)
- Mouse inside the button's Rect during TrackControl

Same mechanism as above; the guard handles both.

### Why --global Button Alone Is Insufficient

`mouse-button --global` writes MBState but does **not** post a mouseDown
event.  WaitNextEvent-based code never sees a click.  MBState alone only
works for ModalDialog (which polls MBState directly in tight loops).

### PostEvent's `where` Field

`_PostEvent` (trap `$A02F`) reads the Mouse global (`$0830`) for the event's
`where` field.  Since the guard writes Mouse directly (alongside MTemp and
RawMouse), `where` is always correct when the guard is active.

### Event Queue Internals

The Event Manager queue starts at **EvQHdr** (`$014A`):

```
$014A + 0: QFlags   (2 bytes)
$014A + 2: QHead    (4 bytes, ptr to first EvQEl)
$014A + 6: QTail    (4 bytes, ptr to last EvQEl)
```

Each `EvQEl` is 22 bytes:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0 | 4 | qLink | Pointer to next element (or NULL) |
| +4 | 2 | qType | Entry type (5 = OS event) |
| +6 | 2 | evtQWhat | Event code (1=mouseDown, 2=mouseUp, 7=diskEvt) |
| +8 | 4 | evtQMessage | Event message (0 for mouse events) |
| +12 | 4 | evtQWhen | TickCount at event time |
| +16 | 4 | evtQWhere | Point (v.h) -- mouse position |
| +20 | 2 | evtQModifiers | Modifier key state |

The Event Manager pre-allocates a fixed pool (typically `evtBufCnt` entries,
default 20).  The `post-event` debugger command scans this pool for a free
slot (`qType == 0`) and links it at the tail of EvQHdr.

---

## 13. Mac Plus: MBTicks Hack and Quadrature Mouse

### The Mac Plus Button Problem

On the Mac Plus, the VIA interrupt handler **continuously overwrites MBState**
with the physical button state every VBL tick.  On ADB-era Macs (SE, II), the
handler only writes MBState when new ADB data arrives (~11ms), so direct
writes "stick" between polls.

### The MBTicks Hack

Discovered by Marcio Teixeira (MiniVNC) through ROM disassembly.  The VIA
handler implements a 3-tick debounce: when a button change is detected, it
reads MBTicks and waits until `Ticks - MBTicks >= 3` before committing.

By periodically writing MBTicks to a value in the future, the debounce timer
never expires, and the handler **never overwrites MBState**:

```c
/* Freeze VIA from overwriting MBState (critical on Mac Plus) */
*(long *)0x016E = TickCount() + 100;   /* MBTicks = far future */
*(char *)0x0172 = 0x00;                /* button down */
PostEvent(mouseDown, 0L);

/* Periodically refresh MBTicks during drag */
*(long *)0x016E = TickCount() + 100;

/* Release */
*(long *)0x016E = TickCount() + 100;
*(char *)0x0172 = 0x80;               /* button up */
PostEvent(mouseUp, 0L);
```

On ADB-era Macs, this hack is technically unnecessary but harmless.

### Mac Plus Mouse Positioning

On the Mac Plus (non-ADB), the mouse uses SCC DCD interrupts with quadrature
encoding through VIA port B.  The `set-mouse` default mode on the Plus writes
low-memory globals directly (identical to `--global`), since there is no ADB
subsystem to inject deltas through.  See [mouse.md](mouse.md) for full
hardware details.

---

## 14. How Other Emulators Inject Mouse Input

### Basilisk II / SheepShaver (cebix/macemu)

**Strategy: Patch ADBOp() trap, call ROM handler via Execute68k().**

Basilisk II disables auto-poll entirely by patching the `ADBOp()` trap.  For
mouse events, `ADBInterrupt()`:

1. Writes position to MTemp and RawMouse via `WriteMacInt16()`
2. Writes MBState via `WriteMacInt8()`
3. Sets CrsrNew = CrsrCouple
4. Constructs a Talk R0 packet at `ADBBase+$163` (button + zero deltas)
5. Calls the ROM handler via `Execute68k(handler_address)`

**Key insight:** The ROM handler overwrites MTemp from its zero-delta packet
immediately after Basilisk writes it.  But auto-poll is disabled and Basilisk
rewrites MTemp every cycle (~60Hz), so the momentary overwrite is corrected
within 16ms.  In a debugger's one-shot command with no continuous correction,
this would be fatal -- which is why the MTemp guard is necessary.

### Mini vMac

**Strategy: ADB hardware emulation (post-3.1.3).**

Early releases directly poked MTemp and MBState.  For pre-ADB Macs (Plus),
`MOUSEMDV.c` makes button state available via emulated VIA port A bit 3.
Every 1/60th second, `SixtiethSecondNotify()` fires and calls
`Mouse_Update()`, which writes coordinates to MTemp and RawMouse via
`put_vm_word()`, sets CrsrNew to `$FF`, and triggers VBL via
`VIA1_iCA1_PulseNtfy()`.

Starting with version 3.1.3, Mini vMac switched to full ADB protocol
emulation for SE and later.  `ADBEMDEV.c` encodes deltas into the device data
buffer, generates SRQ, and lets the ROM issue Talk R0 commands.  This
transition uncovered a timing bug: emulated ADB devices responded to commands
too quickly, requiring an added delay to match real hardware behavior.

### PCE/macplus

**Strategy: Full hardware quadrature emulation.**

For Mac 128K/512K/Plus: `mac_set_mouse()` accumulates deltas, toggles SCC DCD
lines to generate level-2 interrupts, and sets VIA port B bits 4-5 for
direction encoding.  The ROM's mouse ISR naturally computes position.

### MAME

**Strategy: HLE (High-Level Emulation) of ADB bus.**

`macadb_device` constructs Talk R0 responses with `seven_bit_diff()` for
deltas.  SRQ simulated via flag.  Had a notable X/Y swap bug (fixed PR
#10786, January 2023) and mouse-during-keypress stutter (issue #5778).

### Plus Too (FPGA)

Steve Chamberlin's FPGA Mac clone uses a hybrid approach: the FPGA address
decoder creates a "hole" in RAM at `$000828`, so when the ROM reads MTemp, it
gets PS/2 mouse position data from the FPGA hardware.  The mouse interrupt
never fires, yet the OS tracks cursor position correctly.

### Granny Smith Emulator Internals

**ADB transceiver** (`src/core/peripherals/adb.c`):

- **Auto-poll:** `adb_autopoll_deferred()` fires every ~11ms.  Checks
  `last_poll_addr`, alternates between keyboard (address 2) and mouse
  (address 3).
- **Mouse reply:** `prepare_mouse_reply()` clamps deltas to +-63 (7-bit
  range), encodes into `reply_buf[0..1]`.  Large deltas split across
  consecutive polls via `remain_dx`/`remain_dy`.
- **Byte delivery:** `adb_deliver_next_byte()` feeds reply bytes through
  `via_input_sr()`.  Dummy byte with bit3=LOW signals end-of-transfer.
- **Button acceleration:** `adb_mouse_event()` with button change asserts SRQ
  and reschedules auto-poll within `ADB_SHIFT_DELAY` (800us) instead of
  waiting 11ms.

**System wiring** (`src/core/system.c`):

- `system_mouse_update(button, dx, dy)` -> `adb_mouse_event()`
- `system_mouse_move(dx, dy)` -> `adb_mouse_move()`
- `system_mouse_move_adb(dx, dy)` -> ADB-only path for default `set-mouse`

**Debug commands** (`src/core/debug/debug_mac.c`):

- `set_mouse_global(x, y)`: writes MTemp, RawMouse, Mouse, CrsrNew
- `mouse_guard_start(h, v)`: registers scheduler event, activates guard
- `mouse_guard_tick()`: checks/corrects MTemp, reschedules every 1ms
- `mouse_guard_stop()`: deactivates guard

---

## 15. Dead Ends and Debunked Claims

### WRONG: "ROM handler maintains a private position accumulator"

*Source: `why-set-mouse-global-fails.md`, `handover-set-mouse-determinism.md`*

The handler never references A2 (data area pointer).  Deltas go directly to
MTemp.  The observed "overwrite" was caused by phantom ADB data, not a
separate accumulator.  Verified by disassembly: no instructions in
`$408074CE`-`$40807538` read or write through A2.

### WRONG: "CrsrThresh doubles deltas when |delta| > threshold"

*Source: `solution-precise-mouse-injection.md`, `mouse-workflow.md`*

The SE/30 handler never reads CrsrThresh (`$08EC`).  The `ADD.B D0,D0` /
`ASR.B #1,D0` sequence is 7-bit sign extension, not doubling.  The
"CrsrThresh trick" (inflating the threshold to disable acceleration) was
unnecessary.

**Note:** CrsrThresh-based acceleration MAY exist in other ROM versions
(Mac II, Mac Plus VBL task via MickeyBytes).  The Mac Plus's cursor task
at JCrsrTask does implement MickeyBytes-based acceleration.  But the SE/30's
interrupt-time handler does not reference CrsrThresh.

### WRONG: "PostEvent can't be called from the emulator"

*Source: `why-set-mouse-global-fails.md`*

The `post-event` debugger command successfully constructs event records and
links them into the EvQHdr queue via direct memory manipulation.

### Failed approaches (for button simulation)

| Approach | Why it fails |
|----------|-------------|
| Patching the `Button` trap | Drag routines (DragGrayRgn, Drag Manager) read MBState directly |
| jGNEFilter (GetNextEvent filter) | Drag loops never call GetNextEvent |
| Journaling mechanism | Broken under System 7 -- `journalRef` cleared within seconds |

These dead ends consumed months during MiniVNC's development before the
MBTicks solution was found.  On ADB-era Macs, direct MBState writes combined
with PostEvent (via ADB handler or `post-event`) are the correct approach.

---

## 16. End-to-End Timing

### Three Latency Components

| Component | Min | Avg | Max |
|-----------|-----|-----|-----|
| ADB polling wait | 0 ms | ~5.5 ms | ~11 ms |
| VBL wait (MTemp → Mouse) | 0 ms | ~8.3 ms | ~16.7 ms |
| Cursor draw (framebuffer ops) | ~0.7 ms | ~0.8 ms | ~0.9 ms |
| **Total** | **<1 ms** | **~14.6 ms** | **~28.6 ms** |

The ~60 Hz VBL rate is the true bottleneck for perceived cursor responsiveness.

### Interrupt Architecture

| Level | Mac II / SE / SE/30 | IIfx (OSS) | Quadra (A/UX map) |
|-------|---------------------|------------|-------------------|
| 1 | VIA1 (ADB SR, VBlank, timers) | ISM IOP (ADB) | Unused |
| 2 | VIA2 (NuBus, SCSI, ASC) | SCSI | VIA2 |
| 4 | SCC (serial) | SCC IOP | SCC |
| 6 | Power switch | VIA1 | VIA1 (elevated) |
| 7 | NMI (programmer's switch) | NMI | NMI |

On standard VIA-based machines, ADB interrupts arrive at **level 1** -- the
lowest maskable priority.  Any level 2+ interrupt preempts ADB processing.
On the IIfx and Quadras, VIA1 is elevated to level 6 for better interrupt
latency.

---

## 17. Quick Reference for Test Authors

### Place cursor at exact coordinates

```
set-mouse X Y --global
run 5000000        # allow VBL to update cursor image on screen
```

MTemp, RawMouse, and Mouse are set immediately.  The MTemp guard activates and
keeps all three pinned until the next `set-mouse` call.

### Click a button

```
set-mouse X Y --global
run 20000000       # settle + cursor redraw
mouse-button down  # posts mouseDown event via ADB
run 2000000        # app processes click
mouse-button up    # posts mouseUp event via ADB
run 20000000       # app responds
```

### Verify cursor position

```
mac-state
```

Example:
```
Mouse:   pos=(185,370)  button=UP  MBState=$80
Cursor:  MTemp=(185,370)  CrsrNew=$00  CrsrCouple=$FF
Ticks:   17077
```

Note: `mac-state` prints `pos=(v, h)` = `pos=(Y, X)`.

### Coordinate system

- Origin `(0, 0)` is the top-left pixel of the screen
- X increases rightward, Y increases downward
- SE/30 screen: 512 x 342 pixels.  Valid range: X in [0, 511], Y in [0, 341]
- `set-mouse` takes `(X, Y)` order; `mac-state` prints `(Y, X)` order

### Post a synthetic Mac OS event

```
post-event <what> <message>
```

Event codes: 1=mouseDown, 2=mouseUp, 3=keyDown, 7=diskEvt.  Useful when the
ROM's normal event-posting path is broken (e.g., .Sony VBL task stopped
polling).
