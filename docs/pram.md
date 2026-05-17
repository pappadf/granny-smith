# Macintosh Parameter RAM (PRAM): detection, initialization, and format

This document describes how the Macintosh ROM and System software use the
256 bytes of battery-backed PRAM exposed by the 0042 RTC chip — specifically:

1. How a "blank" or corrupted PRAM is **detected**.
2. How the ROM **initializes** PRAM to known defaults when detection fails.
3. The exact **format** in which per-display settings (resolution, pixel
   depth, color/grayscale) are persisted across reboots.

For the chip-level serial protocol used to read and write PRAM bytes, see
[rtc.md](rtc.md). This document is concerned with the **logical format**
that the ROM and Mac OS impose on top of those bytes.

The information below applies to the **SE/30** and **IIcx**, both of which
ship the same Universal ROM (checksum `0x97221136`) and therefore follow an
identical PRAM layout. Where machine-specific differences matter (mainly the
location of the built-in video device in the slot space) they are called
out explicitly.

> Throughout this document, **PRAM offset** means the 0..255 byte address
> visible on the chip's serial interface — i.e. the index into our
> `rtc_t::pram[256]` array in [rtc.c](../src/core/peripherals/rtc.c). The
> ROM/OS calls these "extended PRAM" or "XPRAM" addresses, since the
> original Macintosh 128K/512K/Plus only exposed 20 bytes through the
> non-extended commands.

## 1. Sources of truth

The format is not officially documented in a single place, but it can be
reconstructed precisely from these sources:

| Source                                                                               | What it covers                                                  |
| ------------------------------------------------------------------------------------ | --------------------------------------------------------------- |
| **System 7.1 OS source** — `OS/SysUtil.a` (`InitUtil`, `PRAMInit`, `PRAMInitTbl`)    | Validity check, default-value tables, and the cold-boot writer  |
| **System 7.1 OS source** — `OS/SlotMgr/SlotMgrInit.a` (`InitsPRAM`)                  | Per-slot PRAM initialization and `BoardID` matching             |
| **System 7.1 OS source** — `Interfaces/AIncludes/Slots.a`                            | `sPRAMRec` size, slot PRAM layout (`smPRAMTop`, `FirstPRAMSlot`)|
| **System 7.1 driver sources** — `DeclData/DeclVideo/{Tim,V8,DAFB,CSC,Sonora,RBV}/*Driver.a` | Per-driver `SetDefaultMode` / `GetDefaultMode` PRAM access. The pattern is identical across all of them; we use V8 below as a representative multi-mode example. |
| **Designing Cards and Drivers for the Macintosh Family, 3rd Edition (1992)**, ch. 8–9, pp. 5083, 5283, 5383 | Authoritative spec for slot PRAM and video-mode storage         |
| **SE/30 Universal ROM disassembly** — `INIT_PRAM` at `$40800478`, `SCSI_MGR_INIT` at `$40826636` | ROM-side validation hooks (`_InitUtil`, SCSI XPRAM word)        |
| **Inside Macintosh: Operating System Utilities** — Parameter RAM chapter            | Original 20-byte layout (Mac 128K..Plus); still used as the low region of XPRAM |

## 2. Two-region PRAM model

The 256-byte PRAM is partitioned into two logical regions, each protected by
its **own independent validity token**:

```
+----------------------------------------------------------------+
|  Offset      Region                                            |
+----------------------------------------------------------------+
|  $00..$13   "Old" / "low" 20-byte PRAM                         |
|              (Inside Macintosh I "Parameter RAM" — original    |
|              Mac 128K layout, still maintained for back-compat)|
|              Validity: byte at $00 must equal $A8              |
+----------------------------------------------------------------+
|  $14..$FF   "Extended PRAM" (XPRAM) — 236 bytes                |
|              Includes slot PRAM, Start Manager defaults,       |
|              system-config bytes                               |
|              Validity: 4 bytes at $0C..$0F must equal 'NuMc'   |
+----------------------------------------------------------------+
```

**Note the overlap.** The 'NuMc' XPRAM signature is stored at offsets
`$0C..$0F`, which are **inside** the 20-byte low region. This is intentional:
the ROM writes the `$A8` validity byte first via the "old" PRAM trap
(`_WriteParam`), then writes 'NuMc' at $0C..$0F via `_WriteXPRam`,
overwriting four of the low-PRAM bytes. The original 20-byte spec
designated those overlapping bytes as the AppleTalk node hint and serial
port settings; on machines with the new clock chip those four bytes
are repurposed as the XPRAM signature, and the AppleTalk/serial config
is moved into XPRAM proper.

Both validity tokens are **literal byte patterns**, not checksums. The ROM
makes no attempt to checksum or CRC the PRAM — bit rot or a partial write
will simply leave the validity byte/word intact and the rest may be junk.
This is by design; the per-slot PRAM has its own change-detection mechanism
described in §6.

## 3. Validity-byte detection on cold boot

The full detection-and-repair logic lives in `_InitUtil` (A-trap `$A03F`),
implemented in `OS/SysUtil.a` lines 996–1068. The ROM dispatches into it
from the post-PMMU init sequence at `$40800478`
([SE30-ROM-v2.asm:770](../local/gs-docs/asm/SE30-ROM-v2.asm)).

The relevant excerpt, with offsets annotated:

```asm
InitUtil    CLR.W   -(SP)               ; assume PRAM was OK
            BSR     InitCPHardware      ; clock-chip-specific init
            LEA     SysParam,A1         ; A1 = $0308 (low-mem mirror of $00..$13)
            BSR     ReadPram            ; read all 20 low PRAM bytes
            BSR     ReadTime
            MOVEQ   #0,D0
            CMP.B   #$A8,(A1)           ; *** STEP 1: validity byte at $00 ***
            BEQ.S   CkNewPram           ; valid → skip low-PRAM rewrite
            MOVE.W  #PRInitErr,(SP)     ; remember "had to reset"
            ; rewrite all 20 bytes from PRAMInit table
            MOVEQ   #4,D1
            LEA     PramInit,A0
@1          MOVE.L  (A0)+,(A1)+
            DBRA    D1,@1
            BSR     WritePram           ; flush back to clock chip
CkNewPram   BTST    #6,HWCfgFlags       ; new clock chip present?
            BEQ.S   ClkXit              ; no → done (Mac Plus etc.)
            LEA     GetParam,A0
            MOVE.L  #$4000C,D0          ; offset $0C, count 4
            _ReadXPRam
            MOVE.L  #'NuMc',D1          ; $4E754D63
            CMP.L   (A0),D1             ; *** STEP 2: XPRAM signature at $0C..$0F ***
            BEQ.S   ClkXit              ; valid → skip XPRAM rewrite
            MOVE.W  #PRInitErr,(SP)
            MOVE.L  D1,(A0)             ; write 'NuMc' validator
            MOVE.L  #$4000C,D0
            _WriteXPRam
            ; zero XPRAM[$20..$24..$28..$2C..$30..$34..$38..$3C] in 4-byte stripes
            ; (the actual loop walks D1=32..28 with ADDQ.B #4 — wraps at 256)
            ; copy 20 bytes of PRAMInitTbl into XPRAM[$76..$89]
            MOVE.L  #$00140076,D0
            _WriteXPRam
            ; write Memory-Manager flags default ($80) at $8A
            LEA     MMFlags,A0
            MOVE.B  #MMFlagsDefault,(A0)
            MOVE.L  #MMPRAMloc,D0
            _WriteXPRam
ClkXit      MOVE.W  (SP)+,D0
            BNE.S   @1                  ; non-zero → "had to reset"
            BSET    #5,HWCfgFlags       ; bit 5 = "PRAM was already valid"
@1          EXT.L   D0
            RTS
```

Two points worth stressing:

1. **The two checks are independent.** The low-PRAM rewrite happens iff
   `[$00] != $A8`; the XPRAM rewrite happens iff `[$0C..$0F] != 'NuMc'`.
   Either can trigger without the other.
2. **`HWCfgFlags` bit 5** is set by the ROM at boot to record "PRAM was
   already valid when we found it." Drivers and the Start Manager check
   this bit to decide whether to trust other PRAM-derived state (e.g.
   whether the saved video mode is reliable or should fall back to
   defaults).

Beyond `_InitUtil`, the SE/30 ROM also performs a third, narrower
validation on the SCSI configuration word at XPRAM `$02..$03`, run from
`scsi_mgr_init` at ROM `$40826636`
([SE30-ROM-v2.asm:10499](../local/gs-docs/asm/SE30-ROM-v2.asm)):

```asm
MOVE.W (A7),D0          ; D0 = XPRAM[$02..$03] read by _ReadXPRam
ANDI.W #$7878,D0        ; mask reserved bits
CMPI.W #$4848,D0        ; expected pattern
BEQ.S  valid
MOVE.W #$4F48,(A7)      ; default: host ID 7, no-reset bit set
_WriteXPRam
```

This is a **field-level** sanity check rather than a region-level
validator — it protects the SCSI host-ID byte against a partial write,
even when the surrounding PRAM passes the `$A8` / 'NuMc' tests.

### Empirical confirmation

We verified the validity-byte mechanism against our emulator. Starting
the SE/30 from a cold-zeroed PRAM (`memset(rtc->pram, 0, 256)` in
[rtc.c:344](../src/core/peripherals/rtc.c)) and running the ROM through
`_InitUtil`:

```
$ rtc.pram_read 0x00       → 0xA8        ; old-PRAM validity byte
$ rtc.pram_read 0x0c..0x0f → 4E 75 4D 63 ; 'NuMc' XPRAM signature
$ rtc.pram_read 0x76..0x7b → 00 01 FF FF FF DF
$ rtc.pram_read 0x8a       → 0x80        ; MMFlags default
```

The three values match `PRAMInit[$00]`, `'NuMc'`, the first six bytes of
`PRAMInitTbl`, and `MMFlagsDefault` exactly.

## 4. Default values written when validity fails

### 4.1 Low PRAM defaults (`PRAMInit`, 20 bytes, written into `$00..$13`)

From [SysUtil.a:1073-1116](../local/gs-docs/src/sys71src-main/OS/SysUtil.a):

| Offset | Bytes      | Field                           | Default value                       |
| ------ | ---------- | ------------------------------- | ----------------------------------- |
| $00    | 1          | Old-PRAM validity byte          | `$A8`                               |
| $01    | 1          | AppleTalk node hint, modem port | `$00`                               |
| $02    | 1          | AppleTalk node hint, printer port | `$00`                             |
| $03    | 1          | Serial port use flags           | `(0<<4) | (0<<0)` = neither port in use |
| $04..$05 | 2 (word) | Port A config                   | 2 stop, no parity, 8 data, 9600 baud |
| $06..$07 | 2 (word) | Port B config                   | 2 stop, no parity, 8 data, 9600 baud |
| $08..$0B | 4 (long) | Alarm time                      | `$00000000` (midnight, 1 Jan 1904)   |
| $0C..$0D | 2 (word) | Application font number − 1     | `Geneva − 1` = 2                     |
| $0E    | 1          | Auto-key threshold / rate       | high nibble = 24/4, low = 6/2        |
| $0F    | 1          | Printer connection              | `$00` (printer port)                 |
| $10    | 1          | Alarm enable / mouse track / volume | bit 7 = 0, bits 5..3 = 3, bits 2..0 = 3 |
| $11    | 1          | Double-click / caret blink ticks | high nibble = 32/4, low = 32/4      |
| $12    | 1          | Disk-cache size in 32 KB blocks | `3` (= 96 KB)                        |
| $13    | 1          | Misc bits (color desktop, mouse scaling, cache active, preferred boot, menu blink) | `(1<<7)|(1<<6)|(0<<5)|(0<<4)|(3<<2)` |

Note that `$0C..$0F` from this table are immediately **overwritten** by
the 'NuMc' XPRAM signature — that is normal and is how the ROM marks the
XPRAM region valid in the same physical bytes.

### 4.2 Start Manager defaults (`PRAMInitTbl`, 20 bytes, written into `$76..$89`)

From [SysUtil.a:1119-1136](../local/gs-docs/src/sys71src-main/OS/SysUtil.a):

| Offset    | Bytes | Field                                 | Default value           |
| --------- | ----- | ------------------------------------- | ----------------------- |
| $76       | 1     | Reserved                              | `$00`                   |
| $77       | 1     | Default OS                            | `$01`                   |
| $78..$7B  | 4     | Default boot device                   | `$FFFFFFDF`             |
| $7C..$7D  | 2     | Sound alert ID                        | `$0000`                 |
| $7E       | 1     | Hierarchical menu display flag        | `$00`                   |
| $7F       | 1     | Hierarchical menu drag flag           | `$00`                   |
| **$80..$81** | **2** | **Default video device (slot/sRsrc)** | `$0000`             |
| $82..$87  | 6     | Default highlight color (RGB16×3)     | `$00 × 6` (black)       |
| $88..$89  | 2     | Reserved                              | `$0000`                 |

The remaining XPRAM region is **stripe-zeroed** by the same routine: the
loop at `SysUtil.a:1033-1042` walks an 8-byte address window from $20
upward in 4-byte writes, ensuring there is no stale junk left over from a
previously-installed card configuration. The slot PRAM region (`$46..$75`,
see §5) is included in this zero pass — slot PRAM is then re-populated by
the Slot Manager later during boot, when each card's `BoardID` is matched
against PRAM.

### 4.3 Memory Manager flags (1 byte at `$8A`)

Written explicitly after the table copy, because it is the only XPRAM
default that is non-zero outside the `$76..$89` range:

```asm
LEA     MMFlags,A0
MOVE.B  #MMFlagsDefault,(A0)   ; default = $80
MOVE.L  #MMPRAMloc,D0          ; offset $8A, length 1
_WriteXPRam
```

`MMFlagsDefault = $80` — the high bit enables 32-bit-clean Memory Manager
behavior on machines that support it (it is patched off by older System
versions if needed).

## 5. Full PRAM memory map after a successful boot

Combining the low-PRAM defaults, the XPRAM signature, the StartMgr block,
the slot PRAM area (§6), and the SCSI XPRAM bytes:

```
Offset    Bytes  Region / Field
-------   -----  -------------------------------------------------------------
$00       1      Old-PRAM validity byte ($A8)
$01..$02  2      AppleTalk node hints (modem, printer)
$03       1      Serial port use flags
$04..$07  4      Serial Port A and B config words
$08..$0B  4      Alarm time (Mac-epoch seconds)
$0C..$0F  4      XPRAM validity signature 'NuMc' ($4E754D63)
                 (overlays bytes that the original 20-byte map called
                 application-font / auto-key / printer / misc)
$10..$13  4      Misc low-PRAM bits (alarm, mouse, volume, blink, cache, ...)
$14..$1F  12     Reserved / extended low-PRAM continuation
$02..$03  2      *** Note: SCSI XPRAM word lives at offsets $02..$03 in
                     XPRAM coordinates per Designing Cards & Drivers; on the
                     SE30 ROM this is read with offset=$0002, count=2
                     and validated separately. See §3 last block. ***
$20..$45  38     Reserved / general XPRAM
$46..$75  48     Slot PRAM area (six 8-byte sPRAMRec slots) — see §6
$76..$89  20     Start Manager / Sound / Menu / Default-video / Hilite block
$8A       1      Memory Manager flags ($80)
$8B..$FF  117    Reserved / cdev-private / app-defined
```

The slot PRAM region exactly fills `$46..$75` (48 bytes = 6 slots × 8 bytes),
abutting the StartMgr block at `$76`. The 12 bytes between $14 and $1F and
the 38 bytes between $20 and $45 are scratch space used by various OS
components (network drivers, AppleTalk extended state, etc.) and are not
part of the validated layout.

## 6. Per-slot video PRAM — resolution and pixel depth

This is where the per-display state lives. **All** persistent video mode
information (resolution, pixel depth, which card holds the boot screen) is
written through the **Slot Manager**, not the bare RTC PRAM traps, and
follows the layout defined in *Designing Cards and Drivers*, ch. 8 §
"sPRAMInit" and ch. 9 § "Saving the Default Mode".

### 6.1 Slot PRAM region

From [Slots.a:100-105](../local/gs-docs/src/sys71src-main/Interfaces/AIncludes/Slots.a):

```asm
sizeSPRAMRec    EQU     8       ; size of an sPRAM record
smPRAMTop       EQU     $46     ; PRAM offset of the start of slot PRAM
FirstPRAMSlot   EQU     $9      ; lowest slot that has its own PRAM record
sFirstSlot      EQU     $9      ; lowest slot supported
sLastSlot       EQU     $E      ; highest slot supported
```

Six 8-byte records are allocated contiguously from PRAM offset `$46`,
covering slots `$9..$E`:

| PRAM offset | Slot | Notes                                                        |
| ----------- | ---- | ------------------------------------------------------------ |
| `$46..$4D`  | `$9` | First slot. On SE/30: built-in 1-bit video pseudo-slot. On IIcx: physical NuBus slot $9. |
| `$4E..$55`  | `$A` | NuBus slot $A (also PDS on SE/30)                            |
| `$56..$5D`  | `$B` | NuBus slot $B                                                |
| `$5E..$65`  | `$C` | NuBus slot $C                                                |
| `$66..$6D`  | `$D` | NuBus slot $D                                                |
| `$6E..$75`  | `$E` | NuBus slot $E                                                |

#### SE/30 specifics

The SE/30 has **no programmable video device**: the built-in 9″ display is
a fixed 512×342 1-bit framebuffer scanned out of main RAM by the GLUE
ASIC. There is no CLUT, no depth control, and no real video driver in
the System file. To make the Slot Manager and `_InitGraf` happy, however,
the SE/30 ROM (and our `tests/data/roms/SE30.vrom` declaration ROM)
exposes the built-in display as a **pseudo-slot at slot $9** with a
single mode-list entry (`spID = $80`, 1 bpp, 512×342). The 8-byte
sPRAMRec at `$46..$4D` is therefore real and is maintained by the Slot
Manager exactly like any other slot's PRAM, but on a stock SE/30 only
two of the six user bytes ever vary in practice:

- byte 2 (depth): always `$80` (the only depth this card advertises)
- byte 3 (mode):  always `$80` (the only resolution)

Bit-rot or a card-swap will still trigger the BoardID re-init in §6.5,
which simply rewrites both back to `$80`.

A SE/30 PDS card with a real video controller (e.g. SuperMac Spectrum/24
PDQ) lives in slot $A or higher and exercises the full PRAM mechanism
described below.

#### IIcx specifics

The IIcx has **no built-in video at all**. All video on a IIcx comes
from an Apple-supplied or third-party NuBus card installed in one of
slots `$9..$E`. In our integration test
[tests/integration/iicx-floppy/config.mk](../tests/integration/iicx-floppy/config.mk),
the card we install is the **Apple Macintosh Display Card 8•24** (Apple
codename "JMFB", declaration ROM `Apple-341-0868.vrom`,
[src/core/peripherals/nubus/cards/jmfb.c](../src/core/peripherals/nubus/cards/jmfb.c)).
That card is multi-mode and multi-depth (1/2/4/8/16/24 bpp, multiple
resolutions selected by the connected monitor's sense lines), so its
sPRAMRec exercises both byte 2 (depth) and byte 3 (resolution) — this
is the case the V8/DAFB/CSC/Sonora driver excerpt in §6.3 illustrates.

Both machines run the same Slot Manager code path
([SlotMgrInit.a:1057-1149](../local/gs-docs/src/sys71src-main/OS/SlotMgr/SlotMgrInit.a))
and the same `_InitUtil` low-PRAM / XPRAM validators (§3) — the PRAM
mechanism does not vary between the two. Only the *contents* of the
slot $9 sPRAMRec differ in their typical range of values.

### 6.2 sPRAMRec layout (8 bytes per slot)

From *Designing Cards and Drivers*, p. 5083, and confirmed by the
RBV / Tim / V8 / DAFB / CSC / Sonora video drivers:

```
+------+------+----------+----------+----------+----------+----------+----------+
| byte |  0   |    1     |    2     |    3     |    4     |    5     |    6     | 7
| name |  BoardID (word) | VendorUse1| VendorUse2| VendorUse3..6 (driver-private) |
+------+------+----------+----------+----------+----------+----------+----------+
        \_______ ______/  \____ ___/ \____ ___/ \________________ ________________/
                V              V          V                      V
        Slot Manager       For video  For video        Driver-private 4 bytes:
        BoardID match      cards: spID cards: spID    typically gamma directory
        (cannot be         of the     of the active    selection, sense-line
        modified by app)   pixel-     mode (=video      latch state, luminance
                           depth      timing or         flag, etc. Layout is
                           sResource  resolution        per-driver. NOT
                           (1/2/4/8/  family)           validated by the OS.
                           16/32 bpp)
```

| Byte | Width | Name        | Authoritative writer                  | Meaning                                                      |
| ---- | ----- | ----------- | ------------------------------------- | ------------------------------------------------------------ |
| 0..1 | word  | `BoardID`   | Slot Manager (`InitsPRAM`)            | Copy of the card's board sResource `BoardId` field. Used as a "card has been swapped / PRAM is for a different card" detector. The Slot Manager — never an application — is the only thing that writes this. |
| 2    | byte  | `VendorUse1`| **System (Monitors cdev)** — reserved by the OS for video sResources | spID of the **pixel-depth** sResource currently in effect on this card. Standard mode-list values: `$80` = 1 bpp, `$81` = 2 bpp, `$82` = 4 bpp, `$83` = 8 bpp, `$84` = 16 bpp direct, `$85` = 24/32 bpp direct. |
| 3    | byte  | `VendorUse2`| Video driver (via control selector 9, `SetDefaultMode`) | spID of the **mode-list** sResource currently in effect — this picks the timing / resolution on multi-resolution cards. `$80` is always the default mode for the card; `$81`+ are alternates (different resolutions, different sense-line monitors, etc.). |
| 4..7 | 4     | `VendorUse3..6` | Video driver — driver-private  | Layout is per-card. Common uses: gamma table directory entry, sense-line latch state, page-mode persistence, accelerated-cursor state. **Not** color/grayscale (see §6.4). |

The reservation of byte 2 for the system is explicit in the *Designing
Cards & Drivers* spec (p. 5083, emphasis from the original):

> "Each NuBus slot has 8 bytes of dedicated PRAM. The first 2 bytes
> cannot be modified and always contain the card's BoardID. Normally,
> the other 6 bytes are reserved for the use of the device; but with
> video devices, the **VendorUse1 field (byte 2) of the slot's PRAM is
> reserved by the system to hold the spID of the slot resource
> describing the last screen pixel depth that this card was set to**.
> The InitGDevice call passes this value to the driver's SetMode
> routine to set the proper hardware pixel depth and uses this value to
> determine the default color table for this depth. The Monitors
> Control Panel device sets byte 2."

### 6.3 How a video driver reads and writes the mode bytes

The exact sequence is identical across every in-tree multi-mode video
driver (V8, DAFB, CSC, Sonora, RBV) — `_sReadPRAMRec` to fetch the
8-byte record, mutate byte 2 or byte 3, `_SPutPRAMRec` to write it
back. We use the **V8 driver** as the canonical example because it is
the simplest multi-mode driver pattern and its implementation maps
directly onto how the Apple Display Card 8•24 (JMFB, the IIcx's video
card in our integration test) and any third-party
SE/30 PDS video card behave.

[V8Driver.a:1042-1075](../local/gs-docs/src/sys71src-main/DeclData/DeclVideo/V8/V8Driver.a),
implementing control selector 9 (`SetDefaultMode`):

```asm
V8SetDefaultMode
            WITH    spBlock
            SUBA    #spBlockSize,SP             ; allocate spBlock on stack
            MOVE.L  SP,A0
            MOVE.B  dCtlSlot(A1),spSlot(A0)     ; slot number from the DCE
            CLR.B   spExtDev(A0)
            SUBA    #SizesPRAMRec,SP            ; allocate 8-byte sPRAMRec
            MOVE.L  SP,spResult(A0)             ; spResult points to it
            _sReadPRAMRec                       ; read the 8 bytes
            MOVE.B  csMode(A2),3(SP)            ; *** byte 3 = new mode spID ***
            MOVE.L  SP,spsPointer(A0)
            _SPutPRAMRec                        ; write the 8 bytes back
            ADDA    #SizesPRAMRec+spBlockSize,SP
            BRA     V8CtlGood
```

`V8GetDefaultMode` (status selector 9) is the symmetric read at
[V8Driver.a:1422-1445](../local/gs-docs/src/sys71src-main/DeclData/DeclVideo/V8/V8Driver.a):

```asm
V8GetDefaultMode
            ; ... allocate spBlock + sPRAMRec ...
            _sReadPRAMRec
            MOVE.B  3(SP),csMode(A2)            ; return byte 3 to caller
```

The Monitors cdev calls these selectors. The corresponding update of byte
2 (pixel depth) goes through the OS-level `SetEntries` path: when Monitors
changes pixel depth, it calls the driver's `SetMode` (control selector 2)
to switch the hardware, which writes byte 2 of the slot's sPRAMRec via the
Slot Manager `_SPutPRAMRec` trap as part of confirming the new mode.

> Single-mode drivers (the SE/30 built-in pseudo-slot, the original
> Apple Macintosh II Video Card / TFB / Tim) deliberately *don't*
> implement selector 9 — see
> [TimDriver.a:486-494](../local/gs-docs/src/sys71src-main/DeclData/DeclVideo/Tim/TimDriver.a),
> which dispatches `SetDefaultMode` to a "control bad" stub on the grounds
> that "TIM only has one video sRsrc and it's already set." Their
> `GetDefaultMode` simply returns the slot ID as the mode (see
> [TimDriver.a:785-795](../local/gs-docs/src/sys71src-main/DeclData/DeclVideo/Tim/TimDriver.a)).
> The slot's sPRAMRec still exists and still has byte 2 set to the
> single supported depth, but byte 3 is never touched by the driver
> itself — only by the Slot Manager during BoardID-mismatch re-init.

### 6.4 Color vs. grayscale is **not** stored in PRAM

This surprised us during research, so it is worth being explicit:

The `SetGray` driver call (control selector 6, `csMode = 0` color /
`csMode = 1` luminance-mapped grayscale) flips an in-RAM driver flag —
typically a single bit in the driver's private `GFlags` storage — and
nothing else. The pattern is identical across every multi-mode driver;
this excerpt happens to be from the IIci's RBV driver (used here only
because it has the most readable `GFlags` example — neither SE/30 nor
IIcx uses RBV), [RBVDriver.a:816-844](../local/gs-docs/src/sys71src-main/DeclData/DeclVideo/RBV/RBVDriver.a):

```asm
SetGray     BTST    #IsMono,GFlags(A3)       ; mono-only monitor?
            BEQ.S   @1
            MOVE.B  #1,csMode(A2)            ; force on for mono devices
@1          MOVEQ   #0,D1                    ; bit-field offset for GrayFlag
            BSR.S   SetIntCom                ; sets the bit in GFlags(A3)
            BRA     CtlGood
```

The V8 driver, the JMFB / Apple Display Card 8•24 driver running on
the IIcx in our integration tests, and every other multi-mode driver
all do the same thing — it's the universal Mac video-driver convention.

The persistence of color-vs-grayscale across reboots is instead handled
by the **'scrn' resource in the System file**, which the Start Manager
reads at `_InitGraf` time. *Designing Cards & Drivers* p. 5536:

> "An `_InitGraf` call then causes the 'scrn' screen configuration
> resource to be read from the System file. This resource includes
> information about the size and orientation of the different monitors
> configured into the system, including their last video mode (pixel
> size), color table, and gamma table."

Practical consequence for this emulator: erasing PRAM (clearing $A8 / 'NuMc')
will reset resolution and pixel depth, but the Mac OS's *color/grayscale*
preference and the explicit color-table contents will survive in 'scrn' as
long as the System file does. Conversely, replacing the System file or
booting a different system will reset color/grayscale even if PRAM is
intact.

### 6.5 Card-change detection (the per-slot validity mechanism)

The slot PRAM has no `$A8`-style validity byte. Instead, the Slot Manager
treats the **first word (BoardID) as the validity token**: it must equal
the value advertised by the card currently in the slot.

[SlotMgrInit.a:1057-1149](../local/gs-docs/src/sys71src-main/OS/SlotMgr/SlotMgrInit.a)
implements `InitsPRAM`, called once per slot during boot:

```
1.  Read the 8-byte sPRAMRec from PRAM.
2.  Read the BoardId field from the slot's actual board sResource
    (in the card's declaration ROM).
3.  If BoardID-in-PRAM == BoardID-in-ROM → leave PRAM alone.
    Saved depth (byte 2) and mode (byte 3) are now considered valid
    and will be honored by the StartMgr / driver.
4.  Otherwise (slot is empty, card has changed, or PRAM is blank):
    a.  If the card declares a PRAMInitData sResource:
            read 6 bytes of card-specific defaults from the ROM;
            prepend the new BoardID; write all 8 bytes back.
    b.  Otherwise:
            write [new_BoardID, 0, 0, 0, 0, 0, 0, 0] —
            i.e. force depth/mode back to "card default".
5.  Update the sInfoRecord state to statePRAMInit ($02).
```

This is what *Designing Cards & Drivers* p. 4896 means by "connecting to
a different monitor or changing the amount of frame buffer RAM on a card
may cause PRAM to become invalid. If you physically move the card to a
different slot, this will cause the PRAM to become invalid as well": each
of those changes alters the card's effective BoardID (e.g. by changing
the sense-line response or the card's chip-IDs that feed BoardID), and
the per-slot match in step 3 then fails.

## 7. The "default video device" pointer

PRAM `$80..$81` (set in §4.2) is the global pointer that tells the Start
Manager which slot's video card holds the **boot screen** — the one that
draws the happy-Mac icon and runs `_InitGraf`. It is independent from the
per-slot mode bytes: byte 2/3 of slot N's sPRAMRec say "what mode card N
should boot in"; `$80..$81` says "which card N is the boot card."

When `$80..$81` is the default `$0000`, the Start Manager picks the first
video sResource it finds during slot scan. The Monitors cdev's "Identify
Monitors" / "Drag the menu bar" UI is what writes a non-zero value here,
encoding the chosen slot and sResource ID (high byte = slot, low byte =
sResource ID, per the Start Manager source).

> The Start Manager then validates that choice on every boot — if the
> card identified by `$80..$81` is missing or fails its BoardID check,
> the StartMgr falls back to scanning slots in numerical order. This is
> the second layer of the "PRAM became invalid because the card moved"
> behavior.

## 8. Summary — what this means for the emulator

Our [rtc.c:344](../src/core/peripherals/rtc.c) `memset(rtc, 0, sizeof(rtc_t))`
on cold boot is the right starting state: byte $00 is `$00 ≠ $A8`, bytes
$0C..$0F are zero `≠ 'NuMc'`, every slot's BoardID is zero (which never
matches a real card), so the ROM is guaranteed to take both the
"low-PRAM rewrite" path and the "XPRAM rewrite" path on first boot, and
the Slot Manager is guaranteed to take the "card has changed" path for
every slot. Subsequent boots reload the same image and find everything
valid, and the saved video mode persists — exactly matching real
hardware behavior.

If you want to **seed PRAM** to a specific saved configuration (for fast
boot tests, or to skip the Monitors-cdev round trip in an integration
test), the minimum write set is:

```python
# 1. Validity tokens
pram[0x00]            = 0xA8
pram[0x0C:0x10]       = b'NuMc'

# 2. Start Manager defaults at $76..$89 (see §4.2 table)
pram[0x76:0x8A]       = bytes([0x00, 0x01,
                               0xFF, 0xFF, 0xFF, 0xDF,
                               0x00, 0x00,
                               0x00, 0x00,
                               0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00])
pram[0x8A]            = 0x80   # MMFlagsDefault

# 3. SCSI XPRAM word at $02..$03 — host ID 7, no-reset bit set
pram[0x02:0x04]       = b'\x4F\x48'

# 4. Per-slot video — slot $9
#    On SE/30: built-in 1-bit pseudo-slot (only valid depth/mode is $80).
#    On IIcx:  first NuBus card; for the Apple Display Card 8•24 (JMFB)
#              the depth byte should match a depth advertised by
#              Apple-341-0868.vrom (e.g. $80=1bpp, $81=2bpp, $82=4bpp,
#              $83=8bpp) and the mode byte should match a mode-list
#              entry advertised by the same vrom.
slot9 = 0x46
pram[slot9 + 0:2]     = card_board_id_word    # must match the card!
pram[slot9 + 2]       = 0x80                  # depth spID
pram[slot9 + 3]       = 0x80                  # default mode-list spID
pram[slot9 + 4:8]     = b'\x00\x00\x00\x00'

# 5. Default boot video — slot $9, sRsrc whatever-the-first-video-is
pram[0x80:0x82]       = b'\x09\x80'           # high byte=slot, low byte=sRsrc id
```

Skipping step 1 means the ROM will rewrite *everything* on next boot,
including any per-slot mode you carefully prepared. Skipping step 4's
BoardID match means the Slot Manager will reset that slot's mode bytes
to the card defaults during boot.

## 9. Empirical findings — IIcx + Apple Display Card 8•24 (JMFB)

The PRAM model above is the System 7.1 / *Designing Cards & Drivers*
spec. Probing our IIcx + JMFB boot live (commit `170be46` adds an
`rtc.pram` whole-PRAM setter that makes this practical) revealed
several places where this Universal ROM (`$97221136`) and this video
driver diverge from that spec. Anyone planning to drive boot-time
video mode through PRAM needs to know about all four.

### 9.1 The 'NuMc' XPRAM signature is *not* present after a clean boot

The doc text in §3 cites `_InitUtil` from System 7.1's
`OS/SysUtil.a` — that source guards the XPRAM rewrite on a 4-byte
'NuMc' literal at `$0C..$0F`. After a known-good cold boot in our
emulator (IIcx, 8 MB, System 7.0.1 floppy, 250 M instructions to
Finder), `pram[$0C..$0F]` reads `00 02 63 00`, **not** `4E 75 4D 63`.
The bytes that are written look like the original 20-byte low-PRAM
defaults' application-font / autokey / printer fields rather than the
new clock chip's XPRAM signature.

Two non-exclusive explanations are consistent with this observation:

* The Universal ROM's `_InitUtil` predates the 'NuMc' validator
  documented for System 7.1 — the Mac II/IIcx/SE-30 family ROM
  was released earlier and the validator was added on later models
  with the new clock chip (the `BTST #6,HWCfgFlags` gate the spec
  describes).
* `HWCfgFlags` bit 6 is not set on the IIcx hardware-config word, so
  the gated XPRAM-rewrite path simply never runs and the bytes at
  `$0C..$0F` retain whatever the low-PRAM `PRAMInit` table writes.

The practical consequence: **do not seed `pram[$0C..$0F] = 'NuMc'`
when the test machine is an IIcx or SE/30**. Doing so doesn't break
boot (we verified end-to-end), but it bakes a signature into PRAM that
real hardware running this ROM never produces, which weakens any
"PRAM image matches expected baseline" assertion. Stick to seeding
the validity byte at `$00` (`$A8`) only, and let the ROM's own
`PRAMInit` populate `$01..$13` from its built-in defaults.

### 9.2 The PRAMInit defaults for `$01..$13` differ from the System 7.1 table

After the cold-boot path, our PRAM reads:

```
$00..$13:  a8 80 01 00 cc 0a cc 0a 00 00 00 00 00 02 63 00 03 88 00 6c
```

Compare against §4.1's PRAMInit table (System 7.1):

| Offset | Spec default | Observed | Notes                           |
| ------ | ------------ | -------- | ------------------------------- |
| `$00`  | `$A8`        | `$A8`    | Validity byte — matches         |
| `$01`  | `$00`        | `$80`    | Differs                         |
| `$02`  | `$00`        | `$01`    | Differs (and see §9.4 — SCSI overlay) |
| `$03`  | `$00`        | `$00`    | Matches                         |
| `$04..$07` | port-A/B configs | `cc 0a cc 0a` | Matches IM-VI baudrate equate, just not the System 7.1 default |
| `$08..$0B` | `$00 × 4`  | `$00 × 4` | Alarm time — matches            |
| `$0C..$0F` | `'NuMc'` (per spec) | `00 02 63 00` | See §9.1                |
| `$10..$13` | various      | `03 88 00 6c` | Differs                         |

The difference is consistent with this ROM carrying its own
`PRAMInit` table (one per ROM family) rather than the System
7.1 OS-side one. The Mac OS later overwrites some bytes from
the System file's preference resources, but the boot-ROM phase
predates that.

For seeding purposes: don't try to match the spec's exact 20-byte
PRAMInit content. Either let the ROM populate `$01..$13` itself
(only seed `$00 = $A8`), or capture a real cold-boot dump from this
ROM and replay it byte-for-byte.

### 9.3 The JMFB BoardID is `$0027`, not the `$0001` typical of
   *Designing Cards & Drivers*' generic example

Confirmed empirically via `rtc.pram_read 0x46/0x47` after PrimaryInit:
slot 9's PRAM `BoardID` field is `$00 $27`. Tests that seed slot
9's record in expectation of preserving the saved depth must write
`pram[$46..$47] = $00 $27`, otherwise `InitsPRAM` sees BoardID
mismatch and rewrites the whole slot record from the card's
`PRAMInitData` (zeroing the carefully-prepared depth byte at `$48`).

### 9.4 SCSI XPRAM word: also model-specific

The spec describes a SCSI XPRAM word at `$02..$03` validated by
`scsi_mgr_init` against `$4848` (host ID 7, no-reset). The IIcx
ROM's `scsi_mgr_init` at `$40826636` reads from XPRAM offset `$02`
length `2` and applies the same `ANDI #$7878 / CMPI #$4848` test
— but the *low byte* of `$02..$03` overlaps the system-defined
"AppleTalk node hint, printer port" field of the original 20-byte
PRAM (see §4.1). On a machine where AppleTalk has ever been
configured, the printer-port hint will be non-zero and may flip the
SCSI validator into "rewrite" mode on every boot. We saw `$02 = $01`
in our cold-boot dump — that's `(0<<4) | (1<<0)` for the AppleTalk
node-hint format, not a SCSI-mgr-style flag byte. The SCSI mgr
recovers by writing `$4F48` over the top, but it makes the field a
poor candidate for "set this to mark PRAM as freshly seeded" tests.

### 9.5 PRIMARY INIT FORCIBLY RESETS THE DEPTH BYTE — saved depth
   does *not* survive a cold boot of an IIcx without Mac OS running

This is the headline finding. The §6.2 spec says byte 2 of slot
PRAM (`VendorUse1` / `savedMode` in the JMFB driver's terminology)
holds the saved pixel-depth spID and that "the InitGDevice call
passes this value to the driver's SetMode routine to set the
proper hardware pixel depth". The JMFB *driver*'s `SetDefaultMode`
(control selector 9) does write byte 2, and `GetDefaultMode` does
read it back. But on the **boot path**, before the driver is ever
opened, JMFBPrimaryInit unconditionally resets byte 2 if its own
sense/clock checks fail — and for the typical "fresh machine, never
configured before" state those checks always fail.

[JMFBPrimaryInit.a, lines 305–435](../local/gs-docs/src/sys71src-main/Drivers/Video/JMFBPrimaryInit.a):

```asm
SUBA   #sizesPRAMRec,SP
MOVE.L SP,spResult(A0)
_sReadPRAMRec
MOVE.B savedSRsrcID(SP),D1   ; D1 = byte 3 of slot record
SF     D2                    ; D2 = invalidate flag, default false
...
BSR    pGetClockType
CMP.B  savedClockType(SP),D0 ; if PRAM clock-type byte differs from real chip:
BNE.s  @setInvalidate        ;   set invalidate flag
...
BFEXTU D1{29:3},D0           ; low 3 bits of byte 3 = saved monitor sense
CMP.B  D4,D0                 ; if saved sense differs from current sense:
BNE.s  @setInvalidate        ;   set invalidate flag
...
CheckInval
TST.B  D2                    ; invalidate?
BEQ.s  @done
MOVE.B D4,savedSRsrcID(SP)   ; *** rewrite byte 3 from current sense
MOVE.B D4,savedRawSRsrcID(SP); *** and byte 4
MOVE.B #FirstVidMode,savedMode(SP) ; *** AND FORCE BYTE 2 TO $80 (1 bpp)
MOVE.L SP,spsPointer(A0)
_SPutPRAMRec                  ; write back to PRAM
```

We verified this empirically. Capturing the slot-9 record after a
cold boot and replaying it across 6 fresh `machine.boot` cycles with
byte 2 mutated to `$80, $81, $82, $83, $84, $85` in turn:

| Seeded `$48` | Post-boot `$48` | Post-boot CLUTPBCR | Screen depth observed |
| ------------ | --------------- | ------------------ | --------------------- |
| `$80`        | `$80`           | `$0`               | 1 bpp                 |
| `$81`        | `$80`           | `$0`               | 1 bpp                 |
| `$82`        | `$80`           | `$0`               | 1 bpp                 |
| `$83`        | `$80`           | `$0`               | 1 bpp                 |
| `$84`        | `$80`           | `$0`               | 1 bpp                 |
| `$85`        | `$80`           | `$0`               | 1 bpp                 |

In every case PrimaryInit's `_SPutPRAMRec` from the `CheckInval`
path overwrote our seed back to `$80` *before* the driver opened
and *before* `_InitGDevice` had a chance to honour any saved depth.
The screen ran 1 bpp regardless.

### 9.6 What this implies for an integration test

The chain "set PRAM → cold boot → boot in saved depth" requires
*Mac OS* (specifically, the Monitors control panel, plus `_InitGraf`
running over a populated 'scrn' resource — see §6.4) to wire the
saved spID through to the driver. None of that is present at the
"reach the floppy/? icon" stage that our existing integration tests
use as an anchor. An IIcx booted from a cold ROM, regardless of
PRAM, will reach the boot-disk-prompt screen in 1 bpp — that is
real-hardware-correct behavior, not an emulator bug.

To cover the JMFB renderer at every depth in an integration test,
the realistic options (in increasing order of fidelity):

1. **Direct register programming** — write CLUTPBCR / JMFBRowWords
   from the test script via `memory.poke.l` against the slot $9
   register block.  Exercises the renderer at every depth but
   bypasses both PRAM and the OS's mode-switch flow; the captured
   framebuffer is "the boot ROM's 1bpp byte stream reinterpreted
   as N-bpp pixels," which is visually noise rather than the kind
   of image you'd ever see on real hardware.
2. **Driver `_Control(cscSetMode)` trap** — once the driver is open
   (i.e. after System 7 has reached `_InitGraf`), build a
   `cscSetMode` parameter block in RAM and trap into the driver.
   The driver writes JMFB hardware registers and pre-greys VRAM but
   does NOT update the GDevice's `gdPMap` or invalidate windows, so
   QuickDraw and Finder don't redraw — captured screenshots are
   uniform gray.
3. **Pascal Toolbox `SetDepth` trap (Recommended)** — call
   `SetDepth(MainDevice, depth, 0, 0)` (Palettes.h:
   `{0x303C, 0x0A13, 0xAAA2}`).  Internally this orchestrates
   `_Control(cscSetMode)` + the gdPMap pixelSize/rowBytes update
   that QuickDraw needs + the secondary update calls Mac OS
   requires; pair it with `_PaintBehind(NIL, GrayRgn)` to flush
   pending update events and the Window Manager redraws the
   desktop in the new format.  Captured screenshots are *real*
   Finder-at-N-bpp.  Implemented in our shell as
   [`screen.set_video_mode N`](../src/core/debug/debug.c) — injects
   a 36-byte 68k stub at scratch RAM ($00400000) that pushes
   SetDepth args, traps `_PaletteDispatch`, then traps
   `_PaintBehind`, then halts at a known PC sentinel.
4. **Full Monitors-cdev round trip** — boot to Finder, drive the
   Monitors control panel via simulated mouse events to change
   depth, watch the driver call `_SPutPRAMRec`, then reboot and
   verify the saved depth is honoured.  This exercises PRAM
   end-to-end but is slow (each iteration takes a full Finder boot
   plus a control-panel UI run) and brittle (depends on the exact
   pixel layout of the System 7.0.1 Monitors cdev's UI).  The only
   path that produces a "this saved value really survived a cold
   boot" assertion.

Our `iicx-video-modes` integration test uses **option 3**.  Boots
the IIcx with the System 7.0.1 floppy, reaches Finder, then calls
`screen.set_video_mode {1,2,4,8}` and asserts on the resulting
JMFB register state plus the rendered framebuffer's checksum.  The
boot-icon resolution sweep at non-default monitor senses still
uses cold-boot-per-resolution because no Mac OS is needed there —
sense lines drive the JMFB's mode-list selection at PrimaryInit,
which happens before `_InitGraf`.

### 9.7 Implementation gaps surfaced along the way

* **`savedClockType` (slot-PRAM byte 6)** is the flag that decides
  whether `JMFBPrimaryInit`'s `CheckInval` path resets the slot
  record on each boot.  The IIcx ROM's `pGetClockType` returns
  `$00` (Endeavor) on our default JMFB, and a fresh PRAM cell is
  also `$00`, so the comparison succeeds *by coincidence* — the
  `BNE.s @setInvalidate` doesn't fire on clock mismatch.  But the
  same code path's sense-line check (`BFEXTU D1{29:3} ≠ D4`)
  always fires when the saved low 3 bits don't match the current
  monitor sense, and that's why a hand-seeded
  `savedSRsrcID = $83` (8 bpp + sense `011`) gets rewritten back to
  `$80` + the actual sense.  Test scripts that want to prevent
  this would have to seed the low 3 bits of byte 3 with the
  current monitor's sense (`$6` for 13" RGB → byte 3 = `$A6`)
  *and* keep byte 6 matching `pGetClockType`'s return, *and* match
  BoardID — at which point you've recreated the cold-boot baseline
  PRAM exactly, with byte 2 being the only meaningful knob.
* **JMFB 24 bpp encoding (was a gap, now closed).** When the doc
  was first written, calling `SetDepth(32)` brought up the JMFB's
  "millions of colours" mode but Finder crashed with a system-error
  bomb because two pieces of decoding were wrong:
    1. `recompute_stride` derived `width = stride / 3` for the
       PIXEL_32BPP_XRGB path, giving 853 instead of 640 for the JMFB
       driver's `RowWords = 240` — the card *stores* 4 bytes/pixel
       (Mac OS sets PixMap.pixelSize = 32 and writes XRGB; the
       RAMDAC bypass mode discards the X byte during scan, so "24
       bpp" describes the visible colour depth, not storage).  Fix:
       `width = stride / 4`.
    2. `JMFBVideoBase` was always decoded as `value × 32` to get the
       byte offset, but the JMFB driver's `TFBM30Parms` encodes the
       24bpp framebuffer base as `(defmBaseOffset × 3/4) >> 5 >> 1`.
       Inverting: byte_offset = `value × 32 × 8 / 3` for 24 bpp.
       Without this, `display.bits` pointed ~1700 bytes before the
       framebuffer Mac OS painted into and each row showed content
       from a slightly-wrong VRAM region.
  Both fixes landed in commit `64263ef`; `iicx-video-modes` now
  covers depth = 32 (= 24 bpp packed) end-to-end with a Finder
  baseline at `finder-13in_rgb-640x480-24bpp.png`.
