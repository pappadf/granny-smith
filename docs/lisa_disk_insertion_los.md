# How Lisa Office System (LOS 3.1) handles floppy disk insertion, detection, the mouse, and modal-alert clicks

Reverse-engineered from the LisaOS source (`local/lisa/Lisa_Source/`). This is the
"how it is *supposed* to work" reference for the disk-swap-during-install
investigation (the installer's "insert Office System N diskette" prompt). The
**Divergence & fix** section at the end of *this* file reconciles the documented
behaviour with our emulator.

All line numbers are exact for the `*.TEXT.unix.txt` copies under
`local/lisa/Lisa_Source/`.

---

## 1. The 6504A Sony FDC as the 68000 sees it

The 6504A floppy controller exposes ~1 KB of shared RAM in the 68000 I/O space, on
**odd bus bytes** (the driver uses `MOVEP`). The internal-Sony base is `$FCC000`
(`SOURCE-SONY.TEXT:602-605`: `hwbase := iospacemmu*$20000 + $0C000`, with
`IOSpace=$FC0000`). Byte offsets (from `SOURCE-SONYASM.TEXT:9-27`), and the 68000
address they appear at (`offset*2 + $FCC001`):

| Symbol     | RAM offset | 68000 addr | Meaning |
|------------|-----------|------------|---------|
| `DISKCMD`  | `$01` | `$FCC003`* | command/go-byte; **non-zero = command pending**; ROM zeroes it on accept |
| `DISKPARM` | `$03` | | RWTS sub-command: 0=READ 1=WRITE 2=UNCLAMP 3=FORMAT 4=VERIFY |
| `DISKDRIV` | `$05` | | drive id: `0`=TOP, `$80`=BOTTOM (`LOWER .EQU $80`) |
| `DISKERR`  | `$11` | | non-zero = error on the just-completed interrupt (+1800 base) |
| **`DISKIN`** | **`$41`** | **`$FCC083`** | **non-zero ⇒ a disk is currently present (LEVEL)** |
| **`DISKSTAT`** | **`$5F`** | **`$FCC0BF`** | **interrupt-source byte — see §1.1** |

*(addresses computed as `$FCC001 + offset*2`; e.g. `DISKSTAT` at offset `$5F` →
`$FCC001 + $BE = $FCC0BF`. NB our emulator and common docs cite the status byte as
`$00C05F` in the masked/aliased I/O view — same controller byte.)*

### 1.1 `DISKSTAT` bit layout (the interrupt-source byte)

The Sony driver copies `DISKSTAT` verbatim into the control-block field `int_stat`
(`SOURCE-SONYASM.TEXT:395`). `SOURCE-SONY.TEXT:84-90` declares it as a packed record;
**combined with the empirically-confirmed completion encoding (our working
completion path sets `0xC0` and the OS wakes the reader), the per-drive nibble is:
bit-N within the drive's 4-bit field is `{disk-in, button, RWTS-done, summary}`** —
i.e. the Lisa has TWO drive nibbles in `$C05F`:

| Bit | Mask | Drive-1 (lower) | Drive-2 (upper) | Meaning |
|-----|------|-----------------|-----------------|---------|
| 0 | `0x01` | `DSKIN1` | | disk-in-place, lower drive (LEVEL) |
| 1 | `0x02` | `BTN1`   | | eject button, lower drive |
| 2 | `0x04` | `RWTS1`  | | RWTS complete, lower drive |
| 3 | `0x08` | `DRV1`   | | summary: any of bits 0-2 set |
| 4 | `0x10` | | `DSKIN2` | **disk-in-place, upper drive (LEVEL)** |
| 5 | `0x20` | | `BTN2`   | eject button, upper drive |
| 6 | `0x40` | | `RWTS2`  | **RWTS complete, upper drive** |
| 7 | `0x80` | | `DRV2`   | summary: any of bits 4-6 set |

This bit layout matches our emulator's *completion* encoding
`ist = (DISKDRIV & 0x88) >> 1` (drive `$80` → bit6 `0x40` + summary bit7 → `0xC0`).

> **The Lisa 2 internal Sony drive is the UPPER drive** (drive id `$80`). Its status
> lives in the **upper nibble (bits 4-7)**: disk-in = bit4 (`DSKIN2`/`0x10`),
> RWTS-complete = bit6 (`RWTS2`/`0x40`), summary = bit7 (`DRV2`/`0x80`). (Naming note:
> the SONYASM `.EQU LOWER $80` and the "bot_*" record field names use confusing
> "lower/bottom" naming for what is physically the Lisa-2 drive; what matters is the
> *bit positions*, fixed above.)

### 1.2 The FDIR interrupt — VIA1, IPL level 1

The floppy/parallel-port completion interrupt arrives at **IPL level 1** via VIA1.
`DriverInit` installs the level-1 autovector at `$0064`
(`LIBHW-DRIVERS.TEXT:599-601`). The `Level1` handler (`:895-927`) reads VIA1's IFR
and dispatches the disk driver when the completion bit is set (`:900-911`):

```
@1  AND.B   VIA1+IER1,D0      ; mask IFR with the Interrupt Enable Register
    AND.B   #$22,D0           ; CA1 (bit1, FDC I/O completion) | Timer-2 (bit5)?
    BEQ.S   @2                ; neither -> skip
    MOVE.L  DiskRoutine,A0
    JSR     (A0)              ; ==> Sony driver DISK_INT
```

So the FDC attention/completion line is **VIA1 CA1 (IFR bit 1)**; the dispatch is
gated by `VIA1+IER1` (the interrupt *enable* register). **If CA1's enable bit in
`IER1` is clear, the handler will NOT dispatch the disk driver even though the line
is asserted.** (Mask `$22` = CA1 bit1 + Timer-2 bit5.)

### 1.3 Command / interrupt-acknowledge protocol

Commands: place a sub-command in `COMMAND` and a go-byte in `GO_CMD`; `START_RWTS`
strobes it into `DISKCMD`. Go-byte values (`SOURCE-SONY.TEXT:61-68`): `excmd=$81`
(RWTS), `seek=$83`, `clristat=$85`, `enabstat=$86`, `clrmask=$87`, `goaway=$89`.
On each interrupt `DISK_INT` (`SOURCE-SONY.TEXT:446-541`) (1) reads `DISKSTAT`→
`int_stat`, then (2) issues `CLRISTAT ($85)` with the just-read status to clear
exactly the set bits. `INITDISK` (`SOURCE-SONYASM.TEXT:420-427`) = `CLRISTAT` then
`ENBLDRV ($86)`.

---

## 2. Presence (LEVEL) vs insertion (EDGE) — two separate mechanisms

### 2.1 LEVEL: "a disk is present" — `ISDISKIN` / `DISKIN` ($41)

`ISDISKIN` (`SOURCE-SONYASM.TEXT:437-441`) is a pure level poll of `DISKIN` ($41):
```
ISDISKIN  CLR.L D0 / MOVE.B DISKIN(A2),D0 / MOVE D0,RESPONSE(A3) / RTS
```
It is used **only at drive init** (`hdinit`, `SOURCE-SONY.TEXT:629-636`): after
`INITDISK`, the driver calls `isdiskin`; if non-zero it sets `disk_present := gooddisk`.
The comment is explicit: "Call only during initialization when both disks are idle."
**It is NOT used to detect runtime insertions.**

### 2.2 EVENT: "a disk was just inserted" — `DSKIN`/`bot_in` bit + FDIR + `KEYPUSHED`

Runtime insertion is edge/event-driven. When a disk is clamped, the 6504 raises FDIR
(VIA1 CA1, IPL1) and sets the drive's disk-in bit in `DISKSTAT` (bit4 `DSKIN2` for
the Lisa-2 upper drive). `DISK_INT` (`SOURCE-SONY.TEXT:446-541`):

1. read `DISKSTAT`→`int_stat`, read `DISKERR`, sum checksum errors.
2. `CLRISTAT` to clear.
3. **insertion handling (`:471-479`):**
   ```
   if int_stat.<disk-in> then
   begin
     bbtabl1_bad := false;
     disk_present := gooddisk;        { <-- THE level-state cache the read path checks }
     if chan < 0 then KEYPUSHED (11)  { internal Sony inserted: pseudo-key 11 }
     else            KEYPUSHED (12+slot);
   end;
   ```
4. completion handling (`:481-482`): `if (not int_stat.<rwts-done>) or (cur_num_requests = 0) then EXIT`. So a *pure insertion* interrupt (disk-in set, RWTS-done clear, no pending request) exits after posting the pseudo-key — it does NOT mount anything.

### 2.3 Insertion → pseudo-key → window `diskEvent` → **the Filer (not the alert)**

`KEYPUSHED(11)` (internal Sony inserted) propagates: OS driver `KEYPUSHED`
(`SOURCE-DRIVERASM.TEXT:141`) → HW `KeyPushed` (`libhw-KEYBD.TEXT:747-761`, keycodes
documented `:718-739`, `0B`=Internal Sony Inserted) → synthesizes down+up of that
keycode via `Key` → enqueues a keyboard event. The window manager `CheckWindow`
(`LIBWM-WINDOWS.TEXT:318-334`) converts disk-insert keycodes into a window-manager
**`diskEvent`** (`what=14`, `why=whyDisk=302`) and **`MakeActive(filerFolder)` +
`SendEvent(event, filerProcess)` — i.e. it is delivered ONLY to the Filer process,
and it makes the Filer the active folder.**

> **Implication:** A disk insertion (a) sets `disk_present := gooddisk` in the Sony
> driver, and (b) posts a `diskEvent` addressed to the Filer (activating the Filer).
> It does **not** post anything to the installer or its alert.

---

## 3. THE INSTALLER "insert disk N" FLOW — the central question

**ANSWER: the user MUST click the Continue button. Disk insertion does NOT
auto-dismiss the alert.** (Hypothesis (b).)

### 3.1 Control flow (`APIN-OFFICE.TEXT:2131-2147`)

```
REPEAT
   IF (NOT Dismount (bootPos, EjectIt, NotBoot)) THEN AbortInstall;   { eject prev disk }
   IntToStr (diskCount, tempString); ArgAlert (1, tempString);
   IF (AskAlert (installAlert, 117) = 1) THEN NoMore := TRUE;          { Cancel }
   IF (NoMore = FALSE) THEN
      BEGIN
      WaitAlert (installAlert, 108);     { 'Installing system software' }
      Mount(err, volName, passWord, sDevName);    { <-- only after AskAlert RETURNS }
      ...
```
`AskAlert(117)` BLOCKS; `Mount` runs only after it returns. Alert 117 is a two-button
ask alert `^?Cancel^!Continue` (`APIN-OFFICEALERT.TEXT:181-187`); the next alert's
comment is the smoking gun: *"User inserted diskette in response to alert 117."*
Mapping (`askButnOffset=6`, `AskAlert := Alert(...) - 6`): **Cancel → 1**, **Continue → 2**.

### 3.2 Why insertion can't dismiss the alert (`LIBAM-ALERTMGR`)

`AskAlert`→`Alert(...,askProc)`→`AlertBox`→`UserDismissal` (`:2289-2368`). The ONLY
exit-with-a-button path is `what=buttonDown` + `ButnPushed` returns true (`:2351-2354`).
`AlertEvent` (`:690-747`) only returns an event for `alertFolder` (`act<>private3`) or
for the active/dialog/menu folder when `act<=keyDown(3)`. A `diskEvent` has `what=14`
(`>keyDown`) and is addressed to `filerFolder`, so `AlertEvent` skips it
(`:737-738`) — it is never consumed by the alert. The unit's change-log confirms the
intent (`:38-39`): *"Push private3 event ... to keep Filer from seeing DiskEvent."*

> So inserting the disk is **necessary** (sets `disk_present := gooddisk` so the
> later `Mount`'s `dskio` doesn't return `nodiskpres (614)`) but **NOT sufficient** —
> a real Continue button click is required to make `UserDismissal` return 2.

### 3.3 Mount + ValidDiskette + error codes

`Mount`→`fs_mount`→`real_mount` reads the disk's MDDF (volume name).
`ValidDiskette` (`APIN-OFFICE.TEXT:1958-1988`) checks the volume name is
`'Office System N x.y'` (`auxName='Office System'` @1, disk-# @15, `version='3.0'` @17).
Mount error CASE (`:2149-2194`): `0/-1173..-1175/1052`→proceed; `1053`(boot disk
reinserted)→eject+fail; **`614`(nodiskpres)/`1062`→`Mounted:=FALSE` silently re-loop**;
`606/866/1054/1197`(bad disk)→eject+StopAlert139; `1820`(write-protect)→StopAlert202.
**`614` is the silent-reprompt path that matches our symptom when `disk_present` is
not `gooddisk`.**

---

## 4. The mouse position, the mouse button, and the alert click (the click side)

### 4.1 `$CC00F0/$CC00F2` = `MousX`/`MousY` (NOT the cursor)

Computing the `LIBHW-DRIVERS.TEXT` global layout (base `Globals=$CC0000`, equates
`:191-301`):

| Global | Addr | Meaning |
|--------|------|---------|
| **`MousX`** | **`$CC00F0`** | **mouse X (0..719) — what `GetMouse` returns** |
| **`MousY`** | **`$CC00F2`** | **mouse Y (0..363)** |
| `MousDx/Dy` | `$CC00F4/F5` | last raw COPS delta |
| `MousScaling` | `$CC00F6` | 0 = off (LOS default: 1:1, no X/Y asymmetry) |
| `CrsrX` | `$CC020E` | on-screen cursor X (a VBL-lagged copy of MousX) |
| `CrsrY` | `$CC0210` | on-screen cursor Y |
| `CrsrTracking` | `$CC0212` | 1 = cursor follows mouse (default ON) |

The COPS level-2 ISR `COPS` (`LIBHW-DRIVERS.TEXT:1074`) decodes a 3-byte mouse report
(`$00` lead, then `dx`, then `dy`) and calls `MouseMovement`
(`libhw-MOUSE.TEXT:235-306`), which integrates the deltas (1:1 when `MousScaling=0`),
clamps to `[0,719]×[0,363]`, and writes `MousX`/`MousY` (`:303-304`). `VertRetrace`
(`LIBHW-DRIVERS.TEXT:936-962`) copies `MousX/MousY`→`CrsrX/CrsrY` each VBL when
`CrsrTracking` is on. **So `MousX/MousY` (`$CC00F0/F2`) is updated immediately at COPS
time; the visible cursor lags by ≤1 frame. There is no lag on the global that
matters for clicks.**

### 4.2 `GetMouse` reads `MousX`/`MousY`

Both `GetMouse` definitions read the same global:
- WM `GetMouse` (`LIBWM-EVENTS.TEXT:866-895`): `MouseLocation(pt.h,pt.v)` (reads
  `MousX/MousY`) then `GlobalToLocal(pt)` (line 894) — returns the mouse in the
  current GrafPort's LOCAL coordinates.
- QD `GetMouse` (`libqd-QDSUPPORT.TEXT:29-33`): identical.

**Neither reads `CrsrX/CrsrY`.** ⇒ Placing the mouse so `MousX/MousY` (`$CC00F0/F2`)
sits on a button GUARANTEES `GetMouse` returns that point. (This validates our warp's
choice of `$CC00F0/F2`.)

### 4.3 Button state and `StillDown`

The mouse button is keycode `$06`, stored as a bit in `KeyBitmap` (set on the `0x86`
down keycode, cleared on `0x06` up; `Key`, `libhw-KEYBD.TEXT:970-1143`). `Button`
(`LIBWM-EVENTS.TEXT:370-398`) = `KeyIsDown(6)` (live). `StillDown` (`:1350-1387`) =
`Button AND (no button transition pending in the event queue)` — it calls
`CheckEvents(FALSE)` to drain the COPS buffer first.

The button keycode ALSO enqueues a keyboard EVENT (down and up); the event's
`mouseX/mouseY` are sampled from `MouseLocation` (= `MousX/MousY`) **at the instant
the keycode is processed** (`libhw-KEYBD.TEXT:1076-1078`).

### 4.4 The alert click path and how the buttonDown event is routed

`CheckEvents` (`LIBWM-EVENTS.TEXT:510-530`) builds the `EventRecord` with
`where := (keyState.mouseX, keyState.mouseY)` (the press-time `MousX/MousY`, GLOBAL).
`CheckWindow`→`HitTest` (`LIBWM-WINDOWS.TEXT:528-569`): clicks with `where.v < 16` go
to the menu bar; otherwise it picks the first VISIBLE window whose `strucRgn`
contains `where`; `HitContent` sets `event.who := alertFolder` for an alert and
`SendEvent`s to that process. `AlertEvent` then returns it (with
`GlobalToLocal(where)`, `:713`) and `UserDismissal` calls
`ButnPushed(topButn,botButn,pushed,where)`. `ButnPushed` (`:942-963`):

```
repeat
   pushedButn := noButn;
   for d := first to last do with butns[d] do
      if fShown then PushButn(d, PtInRect(pt, hitBox));   { hitBox = drawn rect, ShowButn:2023 }
   GetMouse(pt);                          { re-read MousX/MousY, GlobalToLocal }
until not StillDown;                       { loop while held }
ButnPushed := (pushedButn <> noButn);
```

### 4.5 Every way a correctly-positioned, shown button can be SILENTLY dropped

1. `where.v < 16` → routed to the menu bar (not the alert).
2. `(MousX,MousY)` at press-time NOT inside the alert window's `strucRgn` → `who ≠
   alertFolder` → `UserDismissal` (insist) just beeps and ignores it.
3. **Stale press-time `where`**: the buttonDown event's `where` is `MousX/MousY` at
   the instant the COPS button-DOWN byte is processed. If the emulator delivers the
   `0x86` keycode before `MousX/MousY` has been updated to the on-button location
   (wrong byte ordering), the event carries the OLD position → wrong `who`.
4. Button not `fShown` (e.g. the hidden button in a one-button alert).
5. **`StillDown` never goes false** (button-up keycode `0x06` never delivered) →
   `ButnPushed` loops forever / never confirms.
6. The down event lost and only the up arrives → `UserDismissal` ignores `buttonUp`.
7. **Wrong active process**: `CheckEvents` deletes buttonDown/Up/keyDown if `NOT
   ImActive` — if the alert's process is not the active process when the event is
   drained, the click is discarded. *(NB: a disk insertion `MakeActive(filerFolder)`s
   the Filer — §2.3 — which can steal "active" from the installer.)*

---

## 5. Divergence from our emulator & the fix plan

Cross-referencing the documented hardware behaviour with our
`src/core/peripherals/lisa_fdc.c` / `src/machines/lisa.c`:

- **Mouse/cursor: CORRECT.** `$CC00F0/F2` is `MousX/MousY`; our COPS-delta warp
  converges it and `GetMouse` reads it. The Cancel-button click works, proving the
  click path is sound.

- **Disk-in DRVSTAT bits: WRONG (primary suspected bug).** Our `lisa_fdc_insert`
  sets `DRVSTAT |= COMPLETE1|OR1 = 0x0c` (lower-drive bits 2,3). The Lisa-2 internal
  drive is the **upper** drive (`$80`); its disk-in is **bit4 `DSKIN2` (0x10) +
  summary bit7 (0x80) = `0x90`** (the upper-drive nibble from §1.1).
  Our *completion* path is already upper-drive-correct (`(drive&0x88)>>1 → 0xC0`);
  only insert is wrong, and it uses the COMPLETE bit instead of the disk-in bit. The
  OS's `DISK_INT` insertion branch (§2.2) looks for the disk-in bit; with `0x0c` it
  never sets `disk_present := gooddisk`, so a later `Mount` returns `614` and the
  installer silently re-prompts. **Fix: set `0x90` on insert.**

- **FDIR servicing on insert.** Empirically (instrumented `DISKSTAT` reads) the OS
  does NOT read `$C05F` after our insert — i.e. the insert's FDIR is not being
  serviced as a level-1 disk interrupt during the alert wait, whereas read
  *completions* are. The FDC attention line (FDIR) is a level the OS services via
  IPL 1, so an insertion must re-assert it. We must
  ensure our insert's FDIR actually reaches the OS's `Level1`/`DISK_INT` (VIA1 CA1
  IFR bit1 with its IER enabled) so `DISK_INT` runs, reads `0x90`, and sets
  `disk_present`. This is the second half of the fix to verify after correcting the
  bits.

- **`DISKIN` ($41) byte.** The controller reports it non-zero when a disk is
  present; we set `0x01` (`ISDISKIN` only tests non-zero), so this is fine.

**Net:** the install almost certainly does NOT need a "magically dismiss the alert on
insert" change (the OS genuinely wants a Continue click, and our click path works).
It needs (1) the correct upper-drive disk-in `DRVSTAT` encoding (`0x90`) on insert so
`Mount` finds the disk, and (2) the insert FDIR to be serviced so `DISK_INT` sets
`disk_present`. The sequence the test must perform is therefore: **insert disk →
(FDIR serviced → `disk_present:=gooddisk`) → click Continue → `Mount` reads disk N →
`ValidDiskette` → copy.**
