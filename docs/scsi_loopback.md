# Apple SCSI Loopback Test Card — Internal Function

This document describes the internal function of the Apple SCSI Loopback Test
Card (077-8219).

---

## 1. Physical Description

The Apple SCSI Loopback Test Card is a small DB-25 plug adapter used with
Apple diagnostic utilities (MacTest, Apple Service Diagnostic).  It plugs into
the external SCSI port and provides two functions:

1. **Standard SCSI termination** — 220 Ω pull-up / 330 Ω pull-down resistor
   network on every signal line, presenting proper bus termination.
2. **Control-to-data signal routing** — physical traces that wire specific
   control-signal pins on the DB-25 connector back to specific data-bus pins,
   allowing software to verify that every control line can be asserted and
   observed on the data bus.

The card does not contain any active components.  The pin-to-pin wiring is
purely passive, using traces on the PCB.

---

## 2. Loopback Card Wiring

The card connects control-signal output pins to data-bus input pins.  The SCSI
bus is active-low with terminators pulling all lines high; when the NCR 5380
asserts a control line (drives it low), the corresponding data-bus pin is also
pulled low through the card wiring, appearing as a `1` bit in the Current Data
Register (CDR).

### 2.1 ICR Control Signals → Data Bus

These signals are active in initiator mode.  They are driven by writing to the
Initiator Command Register (ICR):

| ICR bit | Signal | ICR mask | Data bus pin | CDR mask | Mnemonic        |
|---------|--------|----------|--------------|----------|-----------------|
| bit 1   | ATN    | `$02`    | DB6          | `$40`    | ATN → DB6       |
| bit 4   | ACK    | `$10`    | DB5          | `$20`    | ACK → DB5       |
| bit 3   | BSY    | `$08`    | DB2          | `$04`    | BSY → DB2       |
| bit 2   | SEL    | `$04`    | DB4          | `$10`    | SEL → DB4       |

### 2.2 TCR Control Signals → Data Bus

These signals are active in target mode (Mode Register bit 6 = TARGET set).
They are driven by writing to the Target Command Register (TCR):

| TCR bit | Signal | TCR mask | Data bus pin | CDR mask | Mnemonic        |
|---------|--------|----------|--------------|----------|-----------------|
| bit 0   | I/O    | `$01`    | DB7          | `$80`    | I/O → DB7       |
| bit 1   | C/D    | `$02`    | DB1          | `$02`    | C/D → DB1       |
| bit 2   | MSG    | `$04`    | DB3          | `$08`    | MSG → DB3       |
| bit 3   | REQ    | `$08`    | DB0          | `$01`    | REQ → DB0       |

### 2.3 Combined Pin Map

Viewed from the data bus side, the full mapping of which signal appears on each
data pin:

| Data bus pin | CDR bit | Source signal | Source register |
|--------------|---------|---------------|-----------------|
| DB0          | bit 0   | REQ           | TCR bit 3       |
| DB1          | bit 1   | C/D           | TCR bit 1       |
| DB2          | bit 2   | BSY           | ICR bit 3       |
| DB3          | bit 3   | MSG           | TCR bit 2       |
| DB4          | bit 4   | SEL           | ICR bit 2       |
| DB5          | bit 5   | ACK           | ICR bit 4       |
| DB6          | bit 6   | ATN           | ICR bit 1       |
| DB7          | bit 7   | I/O           | TCR bit 0       |

All 8 data bus pins are accounted for.  The ODR (Output Data Register) can also
drive the data bus directly when ICR bit 0 (DB) is asserted or when the chip is
in target mode.

---

## 3. CDR Pipeline Delay

The NCR 5380's bus drivers do not update instantaneously.  There is a
propagation delay of approximately two register-write cycles between when a
register is written and when its effect appears on the data bus (and thus in the
CDR).

### 3.1 Observed Behavior

When the CPU writes to an NCR 5380 register, the CDR does not immediately
reflect the new bus state.  Instead, the CDR continues to return the bus state
from *before* the second-to-last register write.  In other words, there is a
2-write pipeline delay.

**Example:** If three writes occur in sequence:

| Write # | Action            | Bus state after write | CDR returns        |
|---------|-------------------|-----------------------|--------------------|
| 1       | ICR = `$02` (ATN) | ATN asserted → DB6    | `$00` (pre-write1) |
| 2       | ICR = `$06`       | ATN+SEL → DB6+DB4     | `$00` (pre-write1) |
| 3       | ICR = `$0C`       | BSY+SEL → DB2+DB4     | `$40` (post-write1)|

A CDR read after write 3 returns the bus state that existed after write 1 but
before write 2.

### 3.2 Implementation

The emulator models this as a 3-element circular buffer (`cdr_pipeline[3]`) with
an index (`cdr_idx`).  On every register write in loopback mode:

1. Capture the current bus state (via `compute_loopback_cdr()`) into
   `cdr_pipeline[cdr_idx]` — this records the bus state *before* the write
   takes effect.
2. Advance `cdr_idx = (cdr_idx + 1) % 3`.
3. Apply the register write.

On a CDR read, the emulator returns:

```c
cdr_pipeline[(cdr_idx + 1) % 3]
```

This is the oldest entry in the ring buffer — the bus state from 2 writes ago.

---

## 4. RST (Reset) Signal Behavior

The NCR 5380 RST line has special behavior that differs from other control
signals.

### 4.1 RST Assertion

Writing ICR with the RST bit ($80) set triggers an immediate chip reset: all
registers are cleared except the RST bit in ICR itself and the INT bit in BSR.
The INT bit is set because RST generates a non-maskable interrupt that survives
the reset.

While RST remains asserted, the chip stays in a continuous reset state.

### 4.2 RST Deassertion

When ICR is written with RST cleared (after previously being set), the chip
performs a *final* reset.  This clears ALL registers including the ICR output
latch itself.  The effect is:

1. ICR is cleared to `$00` — the RST bit is gone, and so are any other bits
   that might have been set in the same write.
2. All other registers are cleared.
3. BSR INT is set (the reset interrupt is latched).
4. The CDR pipeline is flushed (all entries set to `$00`).

This final-reset-on-deassertion behavior is critical for subtest 0, which
expects ICR to read `$00` after the RST sequence `$80, $84, $0C`.  The last
write (`$0C` = BSY+SEL, no RST) deasserts RST, triggering the final reset that
clears ICR.

### 4.3 scsi_reset Clears ODR and CDR

The chip reset procedure clears both ODR and CDR to zero.  If ODR retained a
stale value across reset, subsequent CDR reads in target mode or with DB
asserted would incorrectly include any leftover ODR bits.  Clearing ODR during
reset ensures a clean starting state.

---

## 5. Register Reflection Paths

In loopback mode, the status registers (CSR and BSR) reflect signals that the
NCR 5380 is driving onto the bus, since those same signals come back through the
terminated bus (and the loopback card wiring for the data bus).

### 5.1 CSR (Current SCSI Bus Status)

The CSR reflects bus-level control signals:

| CSR bit | Signal | Source                                  |
|---------|--------|-----------------------------------------|
| bit 7   | RST    | ICR RST (`$80`)                         |
| bit 6   | BSY    | ICR BSY (`$08`)                         |
| bit 5   | REQ    | TCR REQ (`$08`) — target mode only      |
| bit 4   | MSG    | TCR MSG (`$04`) — target mode only      |
| bit 3   | C/D    | TCR C/D (`$02`) — target mode only      |
| bit 2   | I/O    | TCR I/O (`$01`) — target mode only      |
| bit 1   | SEL    | ICR SEL (`$04`)                         |

### 5.2 BSR (Bus and Status Register)

The BSR reflects handshake signals and computed state:

| BSR bit | Signal    | Source                                         |
|---------|-----------|------------------------------------------------|
| bit 4   | INT       | Set by reset; cleared by reading RESET register|
| bit 3   | PM        | Phase Match — computed dynamically (see §5.3)  |
| bit 1   | ATN       | ICR ATN (`$02`)                                |
| bit 0   | ACK       | ICR ACK (`$10`)                                |

### 5.3 Phase Match

The Phase Match bit in BSR is `1` when the bus phase (from CSR bits 4:2 =
MSG/C\/D/I\/O) matches the expected phase (TCR bits 2:0).  In loopback mode
with the chip in target mode, the TCR drives the bus phase signals (I/O, C/D,
MSG) which are reflected back into CSR, so phase match is always `1` whenever
TCR is self-consistent.

### 5.4 RESET Register Read Side Effects

Reading the Reset Parity/Interrupt register (`$38`) has side effects: it clears
BSR bits 5 (parity error), 4 (INT/IRQ), and 2 (busy error).  This is the only
way to clear the INT bit that is latched by RST.

---

## 6. MacTest SCSI Subtests

The MacTest SE/30 ROM runs 12 SCSI subtests (numbered 0–11), organized into
three groups.  All subtests set a result in register D6: `$00000000` means pass,
any non-zero value encodes diagnostic information about the failure.

### 6.1 Test Architecture

The tests use an "engine" pattern at address `$26230`: a dispatch loop that
calls a sequence of subtest functions.  Each function returns D6 (result) and a
flag indicating pass/fail.  On the first failure, the engine exits and the test
reports the failing subtest number and D6 value.

The engine is entered 6 times with different subtest ranges:

| Engine call | Subtests | Group description              |
|-------------|----------|--------------------------------|
| 1           | 0        | RST verification               |
| 2           | 1        | ATN loopback                   |
| 3           | 2–8      | ACK, BSY, target mode, signals |
| 4           | 9        | Walking-1 data bus             |
| 5           | 10       | Walking-0 data bus             |
| 6           | 11       | ICR individual bit check       |

### 6.2 Expected Values Table

The ROM stores expected register values at address `$26244` (8 longwords, one
per subtest 0–7):

```
$26244: $00001800   ; subtest 0: ICR=$00, CSR=$00, BSR=$18, CDR=$00
$26248: $400C4208   ; subtest 1: CDR=$40, ICR=$0C, CSR=$42, BSR=$08
$2624C: $200C4208   ; subtest 2: CDR=$20, ICR=$0C, CSR=$42, BSR=$08
$26250: $040C4208   ; subtest 3: CDR=$04, ICR=$0C, CSR=$42, BSR=$08
$26254: $800C4640   ; subtest 4: CDR=$80, ICR=$0C, CSR=$46, MR=$40
$26258: $020C4A40   ; subtest 5: CDR=$02, ICR=$0C, CSR=$4A, MR=$40
$2625C: $080C5240   ; subtest 6: CDR=$08, ICR=$0C, CSR=$52, MR=$40
$26260: $010C6240   ; subtest 7: CDR=$01, ICR=$0C, CSR=$62, MR=$40
```

### 6.3 Subtest 0: RST Verification

**Purpose:** Verify that RST correctly resets the chip and that the interrupt
latch works.

**Sequence:**
1. Reset chip, MR = `$00`
2. ICR = `$80` — assert RST (chip reset, BSR INT set)
3. ICR = `$84` — assert RST + SEL (chip stays in reset)
4. ICR = `$0C` — deassert RST, set BSY + SEL
   - RST deassertion triggers final chip reset → ICR cleared to `$00`
5. Read ICR, CSR, BSR, CDR
6. Compare packed result against `$00001800`

**Expected:** ICR=`$00` (cleared by final reset), CSR=`$00` (no bus signals),
BSR=`$18` (INT + Phase Match), CDR=`$00` (pipeline flushed by reset).

**Key insight:** The CDR pipeline is flushed during reset, so despite register
writes occurring before the CDR read, the pipeline returns `$00`.  The BSR INT
bit is set because RST generates a non-maskable interrupt.  Phase Match is `1`
because both CSR phase bits and TCR are `$00` (trivially matching).

### 6.4 Subtests 1–3: Initiator Control Signal Loopback

Each test asserts a single ICR control signal and verifies its loopback wiring
appears in CDR.  All three follow the same pattern:

**Sequence (example: subtest 1, ATN):**
1. Reset chip, MR = `$00`
2. ICR = `$02` — assert ATN
3. ICR = `$06` — assert ATN + SEL
4. ICR = `$0C` — assert BSY + SEL (deassert ATN)
5. Read CDR, ICR, CSR, BSR
6. Compare packed result against `$400C4208`

**How CDR gets `$40`:**  The CDR pipeline delay is critical here.  After step 4,
the CDR pipeline contains:

| Pipeline slot | Bus state captured before... | CDR value |
|---------------|------------------------------|-----------|
| Oldest (read) | Write 2 (ICR=`$06`)          | `$40` (ATN → DB6 from write 1) |
| Middle        | Write 3 (ICR=`$0C`)          | `$50` (ATN+SEL → DB6+DB4)      |
| Newest        | Write 4 (ICR=`$0C`)          | `$14` (BSY+SEL → DB2+DB4)      |

But the reset at boot flushes the pipeline first.  After reset, the pipeline is
`[$00, $00, $00]`.  We need to count all writes from the reset onward:

- The reset itself (from step 1) flushes the pipeline and resets `cdr_idx=0`.
- Write 1: ICR=`$02` — capture pre-write state into pipeline[0], advance to 1.
  Pre-write: `$00` (nothing asserted yet).  Pipeline: `[$00, $00, $00]`, idx=1.
- Write 2: ICR=`$06` — capture into pipeline[1].
  Pre-write: ATN asserted → CDR=`$40`.  Pipeline: `[$00, $40, $00]`, idx=2.
- Write 3: ICR=`$0C` — capture into pipeline[2].
  Pre-write: ATN+SEL → CDR=`$50`.  Pipeline: `[$00, $40, $50]`, idx=0.
- CDR read returns pipeline[(0+1)%3] = pipeline[1] = `$40`.

The same logic applies to subtests 2 and 3:

| Subtest | Signal | ICR writes          | CDR result | Explanation                |
|---------|--------|---------------------|------------|----------------------------|
| 1       | ATN    | `$02`, `$06`, `$0C` | `$40`      | ATN → DB6 via loopback     |
| 2       | ACK    | `$10`, `$14`, `$0C` | `$20`      | ACK → DB5 via loopback     |
| 3       | BSY    | `$08`, `$0C`, `$0C` | `$04`      | BSY → DB2 via loopback     |

**Other expected values:** ICR=`$0C` (last write), CSR=`$42` (BSY+SEL reflected),
BSR=`$08` (Phase Match, no INT — INT was cleared by reading RESET register during
initialization).

### 6.5 Subtests 4–7: Target Mode Loopback

These tests switch the chip to target mode (MR bit 6 = TARGET) and verify that
TCR control signals appear on the data bus through the loopback card wiring.

**Sequence (example: subtest 4, I/O):**
1. Reset chip
2. MR = `$40` — target mode
3. TCR = `$01` — assert I/O
4. ICR = `$04` — assert SEL
5. ICR = `$0C` — assert BSY + SEL
6. Read CDR, ICR, CSR, MR
7. Compare packed result against `$800C4640`

**How CDR gets `$80`:**  In target mode, TCR signals are wired to data pins.
TCR I/O (`$01`) maps to DB7 (`$80`) through the loopback card.  The pipeline
delay mechanism works the same as in initiator mode — the CDR read returns the
bus state from 2 writes prior, which is after the TCR write has taken effect.

| Subtest | TCR value | Signal | CDR result | Loopback wiring   |
|---------|-----------|--------|------------|-------------------|
| 4       | `$01`     | I/O    | `$80`      | I/O → DB7         |
| 5       | `$02`     | C/D    | `$02`      | C/D → DB1         |
| 6       | `$04`     | MSG    | `$08`      | MSG → DB3         |
| 7       | `$08`     | REQ    | `$01`      | REQ → DB0         |

**CSR values:** Target mode TCR signals are reflected in CSR (I/O, C/D, MSG,
REQ), plus ICR signals (BSY, SEL).  The CSR values differ per subtest because
different TCR bits produce different CSR phase signals.

### 6.6 Subtest 8: Combined Signal Test

Subtest 8 follows the same structure as subtests 1–3 but tests a combined set
of control signals (likely ACK and ATN together).  It is the final test in the
FN3 engine call (subtests 2–8).

### 6.7 Subtests 9–10: Walking Bit Data Bus Tests

These tests verify that all 8 data bus lines can be independently driven using
the ODR.

**Subtest 9 — Walking-1:**
```
reset chip, MR = $00
D1 = $80 (MSB first)
loop:
    ODR = D1
    ICR = $01    ; assert DB (drive data bus from ODR)
    ICR = $02    ; assert ATN (clear DB)
    ICR = $12    ; assert ACK + ATN
    D2 = CDR     ; read data bus back
    if D1 != D2: FAIL with D6 = (expected << 8) | actual
    ROR.W #1, D1 ; rotate: $80 → $40 → $20 → ... → $01
    loop while carry clear
D6 = 0           ; PASS
```

The ICR sequence `$01, $02, $12` creates the necessary pipeline delay: the CDR
read after the third write returns the bus state from before the second write —
which is when ICR=`$01` (DB asserted) and the ODR value is being driven onto the
data bus.

The loopback card wiring also contributes bits to the CDR value (ATN→DB6,
ACK→DB5, etc.), but the ODR value is carefully chosen (walking single bit) so
that these extra bits don't conflict with the expected pattern.  Actually, after
the pipeline delay, the CDR returns the state when only DB was asserted
(ICR=`$01`), so no control signal bits are present.

**Subtest 10 — Walking-0:** Identical to subtest 9 but with `D1 = $7F` (bit 7
clear, all others set), rotated with `ROR.B`.  Together, the walking-1 and
walking-0 tests verify every data bus line can be independently driven both high
and low.

### 6.8 Subtest 11: ICR Individual Bit Verification

Tests each ICR control bit individually by writing a single bit, reading ICR
back to verify it was stored, then reading the appropriate status register to
verify the signal appeared on the SCSI bus.

| Step | ICR write | Verify ICR | Status reg | Mask   | Expected | Signal          |
|------|-----------|------------|------------|--------|----------|-----------------|
| 1    | `$80`     | `$80`      | CSR        | `$80`  | `$80`    | RST → CSR RST   |
| 2    | `$10`     | `$10`      | BSR        | `$01`  | `$01`    | ACK → BSR ACK   |
| 3    | `$08`     | `$08`      | CSR        | `$40`  | `$40`    | BSY → CSR BSY   |
| 4    | `$04`     | `$04`      | CSR        | `$02`  | `$02`    | SEL → CSR SEL   |
| 5    | `$02`     | `$02`      | BSR        | `$40`  | `$40`    | ATN → BSR DRQ?  |

**Note on step 5:** The ATN test reads BSR and masks with `$40` (DRQ bit), not
`$02` (ATN bit).  This may indicate that ATN assertion triggers a DRQ condition
on the Z53C80, or that the disassembled mask value needs re-verification.

---

## 7. Emulator Implementation Summary

### 7.1 Loopback Mode Activation

Loopback mode is enabled via the shell command `hd loopback on` and disabled
with `hd loopback off`.  The command `hd loopback` (with no argument) queries
the current state.

Loopback must be enabled *after* the SCSI Manager completes its device scan
during boot.  Enabling it at boot time causes the SCSI Manager's arbitration
sequence to fail because loopback mode bypasses the bus state machine.  In
integration tests, this is done via a breakpoint at the MacTest SCSI test entry
point.

### 7.2 Write Path

In loopback mode, every register write follows this procedure:

1. **Capture pre-write bus state** into the CDR pipeline ring buffer via
   `compute_loopback_cdr()`.
2. **Advance the pipeline index** (`cdr_idx`).
3. **Store the register value** — ICR, MR, TCR, ODR, etc. are stored normally.
4. **ICR-specific:** RST assertion triggers `scsi_reset()`.  RST deassertion
   triggers `scsi_reset()` followed by clearing ICR to `$00`.
5. **MR-specific:** In loopback mode, MR writes skip arbitration and DMA logic.

### 7.3 Read Path

| Register | Loopback behavior                                                |
|----------|------------------------------------------------------------------|
| CDR      | Returns `cdr_pipeline[(cdr_idx + 1) % 3]` — 2-write delay       |
| ICR      | Returns stored ICR value                                         |
| MR       | Returns stored MR value                                          |
| TCR      | Returns stored TCR value                                         |
| CSR      | Dynamically computed from ICR (BSY/SEL/RST) and TCR in target mode (I/O/C\/D/MSG/REQ) |
| BSR      | Dynamically computed: ICR ACK→BSR ACK, ICR ATN→BSR ATN, plus Phase Match |
| RESET    | Returns `$FF`; side-effect clears BSR bits 5, 4, 2              |

### 7.4 Key Data Structures

```c
struct scsi {
    ...
    bool loopback;           // loopback mode active
    uint8_t cdr_pipeline[3]; // 3-element ring buffer for CDR delay
    int cdr_idx;             // current write position in ring buffer
    ...
};
```

### 7.5 `compute_loopback_cdr()`

Computes the current bus state that would appear in CDR based on:

- **ODR contribution:** if ICR DB bit is set or chip is in target mode, the ODR
  value is OR'd into the result.
- **ICR contribution:** ATN→`$40`, ACK→`$20`, BSY→`$04`, SEL→`$10` — per the
  loopback card wiring table.
- **TCR contribution (target mode only):** I/O→`$80`, C/D→`$02`, MSG→`$08`,
  REQ→`$01` — per the loopback card wiring table.

### 7.6 `scsi_reset()`

Clears all registers to zero (CSR, BSR, MR, TCR, ODR, CDR), flushes the CDR
pipeline, resets `cdr_idx` to 0, clears the DMA buffer, and sets BSR INT (reset
interrupt).  Updates DRQ and IRQ output lines.

---

## 8. Corrections to Earlier Analysis

Several conclusions in earlier analysis documents were proven incorrect during
debugging:

### 8.1 "The loopback card is just a passive terminator"

**Wrong.**  The card contains physical wiring that routes control-signal pins to
data-bus pins.  This wiring is the mechanism that produces non-zero CDR values
when control signals are asserted — it is not bus crosstalk, capacitance, or a
latching artifact.

### 8.2 "CDR reads non-zero without DB assertion — possibly residual data on the bus"

**Wrong.**  The non-zero CDR values (e.g., `$40` when ATN is asserted) are
caused by the loopback card's pin-to-pin wiring.  ATN is physically connected to
DB6 on the card, so asserting ATN always makes DB6 appear in CDR regardless of
whether the DB driver is enabled.

### 8.3 "CDR values don't match any obvious bit manipulation pattern"

**Explained.**  The CDR values follow the loopback card wiring table exactly
(§2).  What appeared to be inconsistent transformations are simply the physical
pin assignments on the DB-25 connector — there is no algorithmic transformation,
just a wiring table.

### 8.4 CDR timing confusion

Earlier analysis was confused by CDR values that didn't match the *current*
register state.  This was caused by the 2-write CDR pipeline delay (§3), which
means CDR returns bus state from 2 register writes ago, not the current state.
Once the pipeline delay is accounted for, all CDR values match predictions
exactly.

### 8.5 "ICR retains BSY+SEL after RST deassertion"

**Wrong.**  RST deassertion triggers a final chip reset that clears ICR to `$00`.
The emulator was initially not modeling this, causing subtest 0 to fail because
ICR read back as `$0C` instead of `$00`.
