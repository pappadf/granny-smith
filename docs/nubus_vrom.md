# NuBus Video Card Declaration ROM

This document describes, in detail, the on-card firmware ("declaration ROM",
sometimes called "configuration ROM" or just "vROM") used by a NuBus video
card on the classic Macintosh family. It covers the physical layout of the
ROM image in slot space, all of the data structures the Slot Manager
expects to find inside it, the executable blocks (`PrimaryInit`,
`SecondaryInit`, the on-card driver), and the hardware-related housekeeping
those blocks are expected to perform (VBL interrupt installation, slot PRAM
init, mode selection, gamma table delivery, etc.).

The authoritative reference for all of this is _Designing Cards and Drivers
for the Macintosh Family_ (3rd ed., 1992), chapters 7, 8, 9, and 11, plus
Appendix B. Slot Manager and Device Manager call descriptions come from the
matching chapters of _Inside Macintosh_. This document collapses those into
a single place from the perspective of "I need to lay out the bytes of a
NuBus video card vROM."

## 1. Where the ROM lives in the address map

### 1.1 Slot space

NuBus carves the top 1/16 of the 32-bit address space into 16 _standard
slot spaces_ of 16 MB each, addressed `$FsFF_FFFF .. $Fs00_0000` where `s`
is the slot's 4-bit ID (Macintosh NuBus uses `$9..$E`). The first 1 MB of
each standard slot space (`$Fs00_0000..$Fs0F_FFFF`) is the _minor slot
space_; it is the part visible in 24-bit address mode, where the CPU sees
it at `$s0_0000..$sF_FFFF`. The remaining 15 MB are only reachable when
the machine is in 32-bit mode. In addition, each slot owns one
256 MB _super slot space_ at `$s000_0000..$sFFF_FFFF`, again only
addressable in 32-bit mode.

The declaration ROM has to live at the top of the standard slot space.
Specifically, the Slot Manager probes the four bytes `$FsFF_FFFC ..
$FsFF_FFFF` looking for the `ByteLanes` byte (see §2.1), and the format
block extends down from there.

### 1.2 Byte lanes and the bus footprint

NuBus is 32 bits wide and supports cards that physically occupy only a
subset of the four 8-bit byte lanes (lane 0 = `/AD7../AD0`, lane 1 =
`/AD15../AD8`, lane 2 = `/AD23../AD16`, lane 3 = `/AD31../AD24`). A vROM
chip is almost always wired to a single lane — typically lane 3 — for
cost reasons. When that happens, each byte of the chip image appears in
the bus space at a stride of 4: chip byte `i` is visible at `bus_addr =
$Fs00_0000 + base + i*4 + lane`. As a consequence, a 32 KB ROM chip
covers 128 KB of bus space; the other three lanes return undefined data
(or whatever the card's NuBus interface drives onto them).

Every offset stored inside the ROM (directory offset, sResource offset,
sub-list offsets, etc.) is a signed offset _measured in valid byte-lane
bytes_, not in raw bus bytes. The Slot Manager scales by the byte-lane
multiplier internally when computing real bus addresses. From the ROM
author's point of view, you can treat the ROM as a flat byte sequence
and ignore the bus expansion.

### 1.3 Where to put each section in the chip

The ROM image is built bottom-up by the assembler, but the Slot Manager
reads it top-down. Concretely:

- The 20-byte **format block** occupies the highest bytes of the chip.
- The **CRC field** in the format block covers all bytes from
  `top_of_chip - Length` through `top_of_chip` (the CRC bytes
  themselves are treated as zero during the calculation).
- The **sResource directory** sits at the low end of the covered region;
  its address is given by a self-relative offset stored in the format
  block.
- All other sResources, sub-directories, executable blocks (`SExecBlock`)
  and data blocks (`sBlock`) live in between, addressed by signed
  offsets from their referring entries.

## 2. The format block

The format block is the entry point for the entire ROM. It is always
20 bytes long and is laid out so that the Slot Manager can identify it
without knowing the byte-lane configuration in advance.

```
high address
+---------------------------------+
|  ByteLanes      (1 byte)        |  <- $FsFF_FFFC..$FsFF_FFFF
|  Reserved       (1 byte = $00)  |
|  TestPattern    (4 bytes)       |  = $5A93_2BC7
|  Format         (1 byte)        |  = $01 (Apple format)
|  RevisionLevel  (1 byte)        |  1..9
|  CRC            (4 bytes)       |
|  Length         (4 bytes)       |
|  DirectoryOffset (4 bytes)      |
+---------------------------------+
low address
```

The fields are listed here from the highest address downward (i.e. the
order the Slot Manager reads them), but the assembler emits them in the
reverse order so that `ByteLanes` ends up last.

### 2.1 `ByteLanes`

Top byte of the format block, sitting at the highest address of the
standard slot space. The low nibble has a bit set for each byte lane the
ROM is wired to. The high nibble must be the one's complement of the low
nibble. The position of the _first_ set bit (lowest-numbered enabled
lane) determines which of the four addresses `$FsFF_FFFC .. $FsFF_FFFF`
holds the `ByteLanes` byte.

| Lanes used | `ByteLanes` | Address     |
| ---------- | ----------- | ----------- |
| 0          | `$E1`       | `$FsFF_FFFC` |
| 1          | `$D2`       | `$FsFF_FFFD` |
| 0,1        | `$C3`       | `$FsFF_FFFD` |
| 2          | `$B4`       | `$FsFF_FFFE` |
| 0,2        | `$A5`       | `$FsFF_FFFE` |
| 1,2        | `$96`       | `$FsFF_FFFE` |
| 0,1,2      | `$87`       | `$FsFF_FFFE` |
| 3          | `$78`       | `$FsFF_FFFF` |
| 0,3        | `$69`       | `$FsFF_FFFF` |
| 1,3        | `$5A`       | `$FsFF_FFFF` |
| 0,1,3      | `$4B`       | `$FsFF_FFFF` |
| 2,3        | `$3C`       | `$FsFF_FFFF` |
| 0,2,3      | `$2D`       | `$FsFF_FFFF` |
| 1,2,3      | `$1E`       | `$FsFF_FFFF` |
| 0,1,2,3    | `$0F`       | `$FsFF_FFFF` |

Any other value is rejected and the slot is treated as empty.

The Slot Manager scans the four candidate addresses, validates the
high/low nibble complement rule, and only then trusts the byte. From
that point on it knows the stride and can read the rest of the format
block at the correct offsets.

### 2.2 `Reserved`

One byte, must be `$00`.

### 2.3 `TestPattern`

Four bytes, must be `$5A93_2BC7`. This is the Apple magic. It also acts
as an endianness/lane-arrangement sanity check after `ByteLanes` has been
decoded.

### 2.4 `Format`

One byte. `$01` selects the Apple format described in this document. No
other formats are defined.

### 2.5 `RevisionLevel`

One byte in the range 1..9. Above 9 is a fatal Slot Manager error.

### 2.6 `CRC`

A 4-byte rotate-left-and-add checksum over `Length` bytes counted in
valid byte-lane bytes only (i.e. the actual content of the chip), with
the 4 CRC bytes themselves treated as zero during the computation.
Algorithm:

```
sum := 0
for byte in chip[low .. high-1]:           // length bytes
    sum := rol.l(sum, 1)
    if byte address is inside the CRC field:
        byte := 0
    sum := (sum + byte) & 0xFFFFFFFF
verify sum == stored CRC
```

The Slot Manager rejects the ROM if the CRC does not match.

### 2.7 `Length`

A 4-byte unsigned count of how many valid-byte-lane bytes the CRC covers
and, equivalently, how far below the format block the lowest meaningful
byte of the ROM sits. The bottom of the covered region is

```
low_addr = top_of_chip - Length
```

### 2.8 `DirectoryOffset`

A signed 24-bit value (zero-extended to 32 bits, but only the low 24
bits matter), interpreted as a self-relative offset from the
`DirectoryOffset` field itself to the start of the sResource directory.
The offset counts valid-byte-lane bytes; the Slot Manager scales it
internally by the byte-lane multiplier to produce the real bus address.

For typical video cards this offset is negative, since the directory is
near the bottom of the chip and the format block is at the top.

## 3. The sResource directory

The sResource directory is the index that maps an 8-bit ID number onto
each top-level sResource list. It looks like this:

```
+---------+--------+
|   ID    | Offset |     <- 4 bytes per entry, ID in top byte
| (byte)  | (24-bit signed)
+---------+--------+
|   ID    | Offset |
+---------+--------+
   ...
+---------+--------+
|  $FF    | 0       |   <- end-of-list marker; entry is $FF00_0000
+---------+--------+
```

Each non-terminating entry is a 32-bit word whose top byte is the
sResource ID and whose low 24 bits are a signed self-relative offset
pointing to that sResource's sub-list. ID values:

- `0..127` — reserved for Apple. Only `BoardSResource` (ID `$01`) is
  currently defined.
- `128..254` — assigned by the card designer to functional sResources.
  On a video card these are video sResources, one per
  "monitor × pixel-depth" combination the card supports.
- `255` — the end-of-list marker. The terminating entry must be
  `$FF00_0000` exactly.

Entries within the directory must be in **ascending ID order**; the Slot
Manager does a linear scan, breaking out when it hits the requested ID
or a higher one.

Some video cards (e.g. ones that offer both 24-bit and 32-bit-addressing
sResources for the same configuration) include 30+ entries in this
directory. The directory's only job is to map IDs to sub-list addresses,
so the same `mVidParams` block can be — and often is — pointed at by
multiple directory entries.

## 4. sResource sub-lists

Every entry in the directory points to an sResource sub-list with the
same shape as the directory itself: a list of 4-byte `(ID, offset/data)`
entries terminated by `$FF00_0000`. The difference is that the entries
in an sResource sub-list are not always pure offsets. There are three
forms:

```
offset-form (used by OSLstEntry):
  bits 31-24 : ID
  bits 23-0  : signed 24-bit self-relative offset to long data, cString,
               sBlock, sub-list, or SExecBlock

word-form (used by DatLstEntry for small numbers):
  bits 31-24 : ID
  bits 23-16 : $00
  bits 15-0  : 16-bit data value

byte-form:
  bits 31-24 : ID
  bits 23-8  : $0000
  bits  7-0  : 8-bit data value
```

The two MPW macros that build these entries are `OSLstEntry` ("offset
list entry") and `DatLstEntry` ("data list entry"). Whether a given ID
is offset-form or data-form is fixed by Apple's definition of that ID.

Entries inside a sub-list, like entries in the directory, must be in
ascending ID order, terminated by `$FF00_0000`.

### 4.1 Apple-defined sResource IDs (general)

The following IDs are recognised by the Slot Manager in any sResource
(both board and functional):

| ID  | Name           | Form     | Meaning                                          |
| --- | -------------- | -------- | ------------------------------------------------ |
| 1   | `sRsrcType`    | offset   | Points to the 8-byte type record (§4.2). Required. |
| 2   | `sRsrcName`    | offset   | Points to a NUL-terminated C string ≤254 chars. Required. |
| 3   | `sRsrcIcon`    | offset   | Points to a 128-byte 32×32×1 'ICON' image. |
| 4   | `sRsrcDrvrDir` | offset   | Points to an sDriver directory (§7). |
| 5   | `sRsrcLoadRec` | offset   | Points to an sLoadDriver record (SExecBlock format) used to fetch the driver from elsewhere when the driver doesn't live in the ROM. |
| 6   | `sRsrcBootRec` | offset   | Points to an sBootRecord (SExecBlock format) used when booting from this device. |
| 7   | `sRsrcFlags`   | word     | Two flag bits: bit 1 `fOpenAtStart`, bit 2 `f32BitMode`. |
| 8   | `sRsrcHWDevId` | byte     | Unique-per-hardware-device tag inside the card. |
| 10  | `MinorBaseOS`  | offset   | Offset to a long whose value is the device's base address offset within standard slot space (`$Fss0_0000` in 24-bit mode, `$Fs00_0000` in 32-bit mode). |
| 11  | `MinorLength`  | offset   | Offset to a long: number of bytes of standard slot space the device occupies. |
| 12  | `MajorBaseOS`  | offset   | Offset to a long: base in super slot space (relative to `$s000_0000`). |
| 13  | `MajorLength`  | offset   | Offset to a long: super-slot-space length. |
| 15  | `sRsrcCicn`    | offset   | Variable-length 'cicn' color icon, wrapped in an sBlock. |
| 16  | `sRsrcIcl8`    | offset   | 32×32×8 color icon, indexed into the 8-bit system CLUT. |
| 17  | `sRsrcIcl4`    | offset   | 32×32×4 color icon, indexed into the 4-bit system CLUT. |
| 108 | `sMemory`      | offset   | Secondary-level resource list describing card RAM/ROM/device address ranges for intelligent NuBus-master cards. Not used by ordinary video cards. |

The `sRsrcFlags` bits control how the Slot Manager builds the base
address that ends up in the driver's device control entry (DCE):

- `fOpenAtStart` (bit 1): when set, the Slot Manager opens the driver at
  system startup. Default-if-absent is `2` (set), which gives the
  24-bit-mode base address.
- `f32BitMode` (bit 2): when set, the device wants the 32-bit-mode
  base form `$Fs00_0000` rather than the 24-bit-mode form `$Fss0_0000`.
  Required by anything that uses more than the first 1 MB of standard
  slot space, including most non-trivial video cards.

### 4.2 `sRsrcType` — the 8-byte type record

`sRsrcType` always points to an 8-byte record laid out as four 16-bit
fields. The structure is hierarchical: the meaning of each field is
narrowed by the field that precedes it.

```
+-----------+--------+
| Category  | cType  |    32 bits: high 16 = Category, low 16 = cType
+-----------+--------+
|   DrSW    |  DrHW  |    32 bits: high 16 = DrSW,    low 16 = DrHW
+-----------+--------+
```

- `Category` (bit 30..16 of the first word; bit 31 is reserved): the
  broadest classification — display, network, terminal, serial,
  parallel, etc. `CatDisplay` = `$0003`. `CatBoard` = `$0001`.
- `cType` (bits 15..0 of the first word): a subclass within the
  category. Under `CatDisplay` you have `TypVideo` = `$0001`, `TypLCD` =
  `$0002`, etc.
- `DrSW` (high 16 bits of the second word): the _driver-software_
  interface — i.e. the set of control/status calls the driver
  understands. Under `CatDisplay`/`TypVideo`, `DrSWApple` = `$0001` is
  the QuickDraw-compatible interface described in chapter 9 (see §11).
- `DrHW` (low 16 bits of the second word): identifies a specific
  hardware product. Each unique card design (e.g. `DrHWTFB` = `$0001`
  for the original Macintosh II Video Card) gets its own `DrHW` value.

For a video card's _functional_ sResource a typical type record is
`$0003_0001 $0001_xxxx` where `xxxx` is the DTS-assigned `DrHW`.

The same type-record layout, with all four fields fixed at known
values, identifies the board sResource (§5.1):

```
Category=$0001 (CatBoard)
cType   =$0000 (TypBoard)
DrSW    =$0000 (DrSWBoard)
DrHW    =$0000 (DrHWBoard)
```

### 4.3 `sRsrcName`

`sRsrcName` points to a NUL-terminated C-string of length ≤254 bytes.
Convention: build the name by stripping the per-field prefixes from the
type record and joining them with underscores. The Apple QuickDraw
driver `_Open` routine reads this string to decide whether to load the
in-ROM driver or a newer one off disk:

```
sRsrcType  = CatDisplay / TypVideo / DrSWApple / DrHWTFB
sRsrcName  = "Display_Video_Apple_TFB"
```

The string is prefixed with `.` and converted to a Pascal `Str255` to
become the driver name as it appears in the unit table.

## 5. The board sResource

Every NuBus card must have exactly one board sResource. It identifies
the card itself (not any particular function the card performs) and is
the place where the Slot Manager finds the entry points needed to bring
the card up at startup.

### 5.1 Required entries

In addition to the standard `sRsrcType` (with the fixed-value
`CatBoard`/`TypBoard`/`DrSWBoard`/`DrHWBoard` record above) and
`sRsrcName`, the board sResource recognises these IDs:

| ID  | Name            | Form     | Description |
| --- | --------------- | -------- | --- |
| 32  | `BoardId`       | word     | 16-bit unique product ID, assigned by Apple DTS. **Required**: an absent or invalid `BoardId` marks the whole slot invalid. |
| 33  | `PRAMInitData`  | offset   | Offset to an sBlock containing default values for the slot's 6 modifiable PRAM bytes (§5.2). |
| 34  | `PrimaryInit`   | offset   | Offset to an SExecBlock containing the primary initialization code (§8). |
| 35  | `STimeOut`      | word     | TimeOut constant for cards that can lock out the CPU. Only honoured on the original Mac II / IIx / IIcx. |
| 36  | `VendorInfo`    | offset   | Offset to a sub-list of vendor strings (§5.3). |
| 38  | `SecondaryInit` | offset   | Offset to an SExecBlock containing the secondary initialization code (§9). |

`BoardId` is the single piece of data the Slot Manager treats as
load-bearing: without it the slot is considered empty.

### 5.2 Slot PRAM and the `PRAMInitData` sBlock

Each NuBus slot has 8 bytes of dedicated PRAM. The first 2 bytes are
the slot's `BoardID` (read-only as far as drivers are concerned and
maintained by the Slot Manager). The remaining 6 bytes are available
for whatever the card wants — except that on video cards byte 2
(`VendorUse1`) is reserved by the system to hold the `spID` of the
sResource describing the screen's _last_ pixel depth (so the system can
restore it at startup). The Monitors control panel writes this byte;
the driver's `Open` routine reads it back and configures the hardware
accordingly.

`PRAMInitData` is an sBlock of the form:

```
+----+----+----+----+
| 00 | Physical Block Size  |   (4 bytes; size includes itself)
+----+----+----+----+
| 00 | 00 | b1 | b2 |
+----+----+----+----+
| b3 | b4 | b5 | b6 |
+----+----+----+----+
```

`b1..b6` are loaded into the slot's 6 modifiable PRAM bytes when the
Slot Manager either (a) detects this card for the first time, or (b)
sees that the `BoardId` in PRAM disagrees with the `BoardId` in the
sResource — i.e. a different card was just plugged into the slot.

If `PRAMInitData` is omitted, the 6 bytes are initialised to zero.

### 5.3 `VendorInfo`

A standard sub-list (ascending IDs, `$FF00_0000` terminator) whose
entries point to C strings. None of these IDs are assigned by Apple —
they are just a convention for collecting informational strings.

| ID  | Name        | Meaning |
| --- | ----------- | --- |
| 1   | `VendorID`  | Vendor name (e.g. `"Apple Computer, Inc."`). |
| 2   | `SerialNum` | Individual unit serial number. |
| 3   | `RevLevel`  | Design revision level. |
| 4   | `PartNum`   | Part number. |
| 5   | `Date`      | Last revision date. |

### 5.4 `sRsrcVidNames` on the board sResource

Video cards that support _video mode families_ (e.g. "Full Color
Display" vs "Black & White Only" on the same physical screen) include
an extra entry `sRsrcVidNames` (ID `65`) in the board sResource. It
points to a sub-directory of `(spID, offset-to-name-block)` entries,
one per video sResource the card declares. Each name block is an sBlock
containing:

```
DC.L blockSize                   ; physical block size (includes self)
DC.W localizationResID           ; 2-byte localization tag
DC.B "Name of this video mode"   ; cString, ≤35 chars
ALIGN 2
```

The Monitors control panel reads these to populate its mode-family
selector.

## 6. Functional sResources (video)

A video card declares one functional sResource per "screen mode entry"
it wants to expose to the system. "Screen mode entry" here means a
specific combination of (monitor type, pixel depth, addressing mode).
Cards typically declare a separate sResource for each of:

- each connected display the card can detect (RS-170, Apple high-res
  RGB, two-page display, portrait display, etc., or just one for
  fixed-display cards),
- each pixel depth supported by that display (1, 2, 4, 8 bpp for
  indexed-CLUT modes; 16 bpp / 32 bpp for direct modes),
- each addressing mode (one variant for 24-bit, one for 32-bit, since
  the `f32BitMode` flag and `MinorBaseOS`/`MajorBaseOS` differ),
- video mode "families" (a 1-bpp-only fixed-CLUT variant for B&W, etc.)

That can mean 30 to 50 functional sResources on a flexible card. Many
of them share the same underlying `mVidParams` block — only the entries
in the per-sResource sub-list differ.

A functional video sResource sub-list typically looks like:

```
sRsrcType       -> 8-byte type record (CatDisplay/TypVideo/DrSW/DrHW)
sRsrcName       -> cString "Display_Video_<vendor>_<product>"
sRsrcDrvrDir    -> sDriver directory                                  [§7]
sRsrcFlags      = $0006   ; f32BitMode + fOpenAtStart (if 32-bit)
sRsrcHWDevId    = 1       ; this card's device
MinorBaseOS     -> long: frame-buffer offset from $Fs00_0000
MinorLength     -> long: frame-buffer length in standard slot space
sGammaDir       -> gamma directory (per-monitor)                      [§6.3]
FirstVidMode    -> mode-1 sub-list (depth-specific params)            [§6.1]
SecondVidMode   -> mode-2 sub-list
...
EndOfList       = 0
```

The "`FirstVidMode`/`SecondVidMode`/..." IDs are video mode list slots
beginning at ID 128 and increasing by 1 for each mode. The Slot
Manager's mode-list scan walks IDs `128, 129, 130, ...` and stops at
the first `$FF00_0000` terminator.

### 6.1 The per-mode mode list

Each `FirstVidMode` / `SecondVidMode` / ... entry points to a small
sub-list that describes one pixel depth:

| ID  | Name         | Form     | Meaning |
| --- | ------------ | -------- | --- |
| 1   | `mVidParams` | offset   | Offset to the video parameter sBlock (§6.2). |
| 2   | `mTable`     | offset   | Offset to a fixed device CLUT for fixed devices. Format is the standard `ColorTable` / `cTabHandle` block: ctSeed, ctFlags, ctSize, then size+1 (value,r,g,b) entries. |
| 3   | `mPageCnt`   | word     | Number of video pages available in this mode (counting number, not zero-based). |
| 4   | `mDevType`   | word     | Device type: 0 = indexed CLUT, 1 = indexed fixed, 2 = direct. |

So a typical 4-bpp indexed mode block looks like:

```
OSLstEntry  mVidParams, _FBVParams
DatLstEntry mPageCnt,    Pages4
DatLstEntry mDevType,    CLUTType        ; 0
DatLstEntry EndOfList,   0
```

### 6.2 The video parameter sBlock (`mVidParams`)

The `mVidParams` offset points to an sBlock whose body is a fully
populated copy of a QuickDraw `PixMap` record (minus the runtime
fields). The structure exactly mirrors `PixMap` because the Slot
Manager copies these values into the gDevice's pmHandle at startup.

| Offset | Size  | Field          | Description |
| ------ | ----- | -------------- | --- |
| 0      | long  | _block size_   | sBlock size, includes the size field itself. |
| 4      | long  | `vpBaseOffset` | Offset from the frame-buffer base to page 0. Usually 0. |
| 8      | word  | `vpRowBytes`   | Row stride in bytes. High bit must be clear. |
| 10     | 4×word| `vpBounds`     | top/left/bottom/right rectangle for the device. |
| 18     | word  | `vpVersion`    | Always 1. |
| 20     | word  | `vpPackType`   | Reserved (0). |
| 22     | long  | `vpPackSize`   | Reserved (0). |
| 26     | long  | `vpHRes`       | Horizontal resolution in Fixed pixels/inch. |
| 30     | long  | `vpVRes`       | Vertical resolution in Fixed pixels/inch. |
| 34     | word  | `vpPixelType`  | `$0` = ChunkyIndexed, `$10` = ChunkyDirect. |
| 36     | word  | `vpPixelSize`  | Bits per pixel, rounded up to a power of 2. |
| 38     | word  | `vpCmpCount`   | Components per pixel (1 for indexed, 3 for direct). |
| 40     | word  | `vpCmpSize`    | Bits per component (1/2/4/8 for indexed; 5 or 8 for direct). |
| 42     | long  | `vpPlaneBytes` | Reserved (0). |

For 16-bpp direct ("hi-color") `vpPixelType=ChunkyDirect,
vpPixelSize=16, vpCmpCount=3, vpCmpSize=5`. For 32-bpp direct
("true-color") use `vpPixelSize=32, vpCmpSize=8`.

### 6.3 Gamma directory and gamma tables

Each functional video sResource may contain an optional
`sGammaDir` entry (ID `64`) pointing to a gamma directory — a standard
sub-list of `(spID, offset)` entries, terminated by `$FF00_0000`. IDs
start at 128 (the default gamma for this video mode) and increase by 1
for each additional table the card carries.

Each entry's offset points to a gamma table sBlock laid out as:

```
DC.L    blockSize                ; physical block size, includes self
DC.W    monitorLocalizationID    ; 2 bytes identifying the monitor
DC.B    'Monitor Display Name',0 ; cString, ≤35 chars, ALIGN 2
DC.W    gVersion                 ; 0 in all current ROMs
DC.W    gType                    ; the drHW value the table applies to
DC.W    gFormulaSize             ; bytes of gFormulaData; usually 0
DC.W    gChanCnt                 ; 1 (R=G=B) or 3 (R,G,B)
DC.W    gDataCnt                 ; values per channel (typically 256)
DC.W    gDataWidth               ; bits per value (typically 8)
DC.B    gFormulaData [gFormulaSize]
DC.B    gData [gChanCnt * gDataCnt * gDataWidth/8]
```

The gamma table is consumed by the Monitors control panel and by the
video driver itself (via the `SetGamma` control call described in §11),
to compensate for the non-linear luminance response of the connected
display. Each card design typically ships one default gamma per
monitor it can identify, plus an "Uncorrected" linear ramp that the
driver synthesises on demand.

## 7. Drivers in the ROM

For a video card, at least a minimal driver _must_ live in the
declaration ROM, because video has to be up well before the file system
is available (the system needs to show the "Welcome to Macintosh"
screen, MacsBug error messages, etc.). Two delivery mechanisms exist:

- `sRsrcDrvrDir` (ID 4) — the driver code sits in the ROM. The Slot
  Manager copies it into the system heap at startup and patches the
  unit table to point at the heap copy. This is the path video cards
  use.
- `sRsrcLoadRec` (ID 5) — the ROM only carries a small SExecBlock that
  the Slot Manager runs to _go fetch_ the driver from somewhere else
  (an attached disk, the network, etc.). Used by intelligent cards that
  carry too much driver code to fit on the card.

Exactly one of these two entries should be present.

### 7.1 The `sDriver` directory

`sRsrcDrvrDir` points to a sub-list whose IDs select the target
operating system / CPU:

| ID  | Name           | Description |
| --- | -------------- | --- |
| 1   | `sMacOS68000`  | Driver runs on a 68000 system. |
| 2   | `sMacOS68020`  | Driver runs on a 68020 system. |
| 3   | `sMacOS68030`  | Driver runs on a 68030 system. |
| 4   | `sMacOS68040`  | Driver runs on a 68040 system. |

Each entry's offset points to a driver sBlock:

```
DC.L    blockSize     ; size of the driver, including this field
;       driver code follows immediately
```

The "code" starts with a standard Macintosh DRVR resource header
(driver flags, delay/EMASK/menu words, Open/Prime/Control/Status/Close
offsets, optional driver name as a Pascal string). The Slot Manager
copies the entire block onto the system heap, patches the relocatable
references (driver-relative branch tables), and installs the result in
the unit table.

Cards that need to support both classic Mac OS and A/UX include an
additional sub-directory under a non-standard ID, with the A/UX driver
in its own sBlock.

## 8. PrimaryInit — the one-shot card bring-up

`PrimaryInit` is the first piece of card code that runs. The Slot
Manager invokes it during the slot-scanning phase of `StartBoot`, after
it has validated the format block and built the slot resource table for
that slot, but _before_ system extensions or any driver `Open` call.

### 8.1 SExecBlock layout

`PrimaryInit` (like `SecondaryInit`, `sLoadDriver`, and `sBootRecord`)
is an SExecBlock — a length-prefixed code block with a small header:

```
+----+----+----+----+
|        size       |   physical block size, includes self
+----+----+----+----+
| 02 | 00 | CPU | 0 |   revision=$02, reserved=$00, CPUID, code-offset-hi
+----+----+----+----+
|         ...       |   code follows
```

`CPUID` is `01`/`02`/`03`/`04` for 68000/68020/68030/68040 respectively.
The Slot Manager picks an SExecBlock compatible with the host CPU when
multiple variants are present.

### 8.2 Execution environment

When the Slot Manager jumps to `PrimaryInit`:

- Only Slot Manager calls are guaranteed to work — no Memory Manager,
  no Resource Manager, no Toolbox.
- The code must be ≤16 KB (target 2 KB) and finish in ≤200 ms.
- All interrupts must be left disabled on exit; in particular, **VBL
  interrupts on the card must be disabled**. The frame buffer should
  be cleared to 50% gray or the dithered gray pattern so the rest of
  the boot has a clean canvas.
- `A0` points to an `seBlock` parameter block. The block tells you the
  slot number (`seSlot`) and sResource ID (`seSResource`), and you
  return your status in `seStatus`.

### 8.3 What PrimaryInit must do

The expected sequence for a video card is:

1. Initialise the on-card hardware: program the Frame Buffer
   Controller, select the right pixel clock based on the monitor sense
   lines, size the installed VRAM, etc.
2. Determine the connected display (if any) by reading the monitor
   sense lines on the video connector. If no display is connected, the
   conventional response is to delete _all_ video sResources from the
   slot resource table with `_sDeleteSRTRec` and return a successful
   status, so the card silently disappears from the system.
3. Walk the slot resource table (via `_sNextsRsrc` / `_sGetsRsrc`) and
   delete every video sResource that does not apply to the detected
   display _or_ the detected VRAM size. Two pairs of sResources are
   typically present for every configuration — one for 24-bit
   addressing and one for 32-bit — and `PrimaryInit` decides which to
   keep:
   - Call `_sVersion` to find out whether the new Slot Manager is in
     ROM (i.e. supports inserted-at-runtime sResources).
   - If `_sVersion` returns an error, the old Slot Manager is in ROM;
     delete every 32-bit-addressed sResource with `_sDeleteSRTRec`.
   - Else, check whether 32-bit QuickDraw is in ROM
     (`_GetTrapAddress` on the unnamed `$AB03` trap and compare with
     `Unimplemented`). If it is, delete the 24-bit sResources; if it
     isn't, delete the 32-bit sResources unless the card can only
     operate in 32-bit mode, in which case return `seStatus = $8001`.
4. Read the slot's PRAM. If `VendorUse1` already names a sResource
   compatible with the current display and VRAM, leave it; otherwise
   update PRAM to a sane default so the next boot picks the right
   depth.
5. Return status in `seStatus`:
   - `0` or positive — non-fatal completion; the card is active.
   - `$8001` — special: defer using the card as a video device until
     after `SecondaryInit`. The Slot Manager will not use this card
     for the "Welcome to Macintosh" screen or as the MacsBug screen.
   - Negative — fatal; the slot is marked invalid.

The final state in `seStatus` is stored in
`sInfoRecord.siInitStatusV`.

## 9. SecondaryInit — patch-aware second pass

`SecondaryInit` is run by version 1+ of the Slot Manager (Mac IIci, IIfx,
and any later machine), but only on slots whose `PrimaryInit` returned
a non-fatal status — including the `$8001` "defer" status. The runtime
environment is much friendlier than `PrimaryInit`'s:

- No size or time limits.
- Interrupts are enabled.
- The full Toolbox / Memory Manager / Resource Manager are available.
- System patches have been installed, so it is now safe to check for
  trap addresses that may have come from system files (e.g. 32-bit
  QuickDraw patched in via `INIT` resources).

`SecondaryInit` exists primarily so a card with 32-bit-only features
(e.g. direct-color modes) can recover when the host machine has
32-bit QuickDraw patched in rather than in ROM. The expected sequence
for a video card is:

1. Call `_sVersion`. If `spResult == 2`, the new Slot Manager is in
   ROM, everything was correctly configured in `PrimaryInit`, and
   `SecondaryInit` should just return success.
2. If `spResult == 1`, the new Slot Manager was patched in:
   - Check for 32-bit QuickDraw with `_GetTrapAddress` on `$AB03`. If
     missing and the card cannot operate in 24-bit mode, return
     failure.
   - Otherwise remove the 24-bit sResource lists with `_sDeleteSRTRec`,
     then add the 32-bit lists with `_sInsertSRTRec`.
3. If this card is the startup screen (`spRefNum != 0`), update
   `gDevice^^.gdPMap^^.pmBaseAddr` to the 32-bit base address (the
   driver may also need to update its private cached base address).
4. For cards with video mode families, use `_SetSRsrcState` /
   `_sInsertSRTRec` to enable the right family member.
5. Return `seStatus = 0` on success.

The Slot Manager's pass over `SecondaryInit` records can also re-open
the driver if its `fOpenAtStart` bit was clear or its `PrimaryInit`
deferred opening, so the card finally becomes a usable video device.

## 10. The video driver (resident at runtime)

Once `PrimaryInit` (and possibly `SecondaryInit`) have stabilised the
sResource table and chosen a default mode, the Slot Manager loads the
selected driver — by copying the right `sDriverDir` sBlock onto the
heap and adding a `dCtlEntry` to the unit table — and the Start Manager
calls `_Open` on it.

The driver itself is a regular Mac OS device driver (DRVR resource
header followed by `Open`/`Prime`/`Control`/`Status`/`Close` routines),
but with two video-specific responsibilities.

### 10.1 `Open` responsibilities

When the Start Manager calls `_Open` on the video driver, the driver
must:

1. Allocate any private storage it needs and store a handle to it in
   `dCtlStorage`. **All driver state must live here**, not in the
   driver code segment, because the driver code may be relocated by
   the Memory Manager.
2. Read the slot's PRAM (via `_ReadXPRam` or by looking through the
   `dCtlSlot`/`dCtlSlotId` fields) to find the previously selected
   pixel depth (stored in `VendorUse1`).
3. Install its interrupt handler into the slot interrupt queue (see
   §10.3).
4. Enable VBL interrupts on the card.
5. Set up the initial gamma table from `sGammaDir` (the system will
   override later when `InitGraf` reads the `scrn` resource).

The driver's `Open` routine does _not_ set the video mode — the Start
Manager explicitly issues a `SetMode` control call later, based on the
`scrn` resource and the value in PRAM.

### 10.2 `Close` responsibilities

`Close` is the mirror of `Open`. It must:

1. Disable VBL interrupts on the card.
2. Remove the slot interrupt queue element with `_SIntRemove`.
3. Dispose of `dCtlStorage`.
4. Turn off the video signal — for CRTs, drop sync; for LCDs, drop the
   backlight — so the screen doesn't persist a stale image across a
   reboot.

### 10.3 The slot interrupt queue

A video driver gets called on every VBL via the slot interrupt queue.
The Device Manager maintains a queue of `SQElem` records per slot:

```
SQLink   EQU 0     ; link to next element        (long, pointer)
SQType   EQU 4     ; queue type ID (validity)    (word)
SQPrio   EQU 6     ; priority (unsigned byte)    (low byte of word)
SQAddr   EQU 8     ; interrupt service routine   (long, pointer)
SQParm   EQU 12    ; A1 value to pass to ISR     (long)
SQSize   EQU 16    ; size of an element
```

Installation in the driver's `Open` routine:

```asm
LEA       MySQEl, A0
LEA       PollRoutine, A1
MOVE.L    A1, SQAddr(A0)
MOVE.W    #Prio, SQPrio(A0)
MOVE.L    A1Parm, SQParm(A0)      ; usually dCtlStorage handle
MOVE.W    Slot, D0
_SIntInstall
```

`SQPrio` is unsigned — higher value runs first. Priorities 200..255 are
reserved for Apple. Removal is `_SIntRemove`.

The ISR itself is called with `A1 = SQParm`. It must preserve every
register except `A1` and `D0`, must keep the CPU at priority ≥2, and
must return with `RTS`. It sets `D0` non-zero if it serviced the
interrupt and zero if not. The Device Manager polls each ISR in the
slot's queue, in `SQPrio` order, until one returns non-zero, then
acks the VIA2 interrupt flag and returns from the original level-2
interrupt with `RTE`.

The ISR is responsible for clearing the card's own interrupt source
(usually by reading a hardware "interrupt acked" register on the card).
NuBus `/NMRQ` must stay asserted until the ISR clears the underlying
condition, otherwise the Slot Manager will see no edge.

### 10.4 Control calls (`csCode`)

The video driver Control calls are how Color QuickDraw, the Color
Manager, and the Monitors control panel interact with the card. The
driver decodes `csCode` from the parameter block and dispatches:

| `csCode` | Name              | `csParam`      | Notes |
| -------- | ----------------- | -------------- | --- |
| 0        | `Reset`           | `VDPgInfoPtr`  | **Required.** Return card to startup state: default depth (1 bpp preferred), page 0, default colors. Reinit driver private state. |
| 1        | `KillIO`          | none           | **Required.** Cancel pending I/O. For synchronous video drivers, `NoErr`. |
| 2        | `SetMode`         | `VDPgInfoPtr`  | **Required.** Set the pixel depth (`csMode`) and select a video page (`csPage`). Return the frame buffer base in `csBaseAddr`. All depths share the same base address. Indexed devices should pre-load the CLUT with 50% gray to hide the flash during the depth change. The Monitors cdev stores the chosen mode in PRAM. |
| 3        | `SetEntries`      | `VDEntRecPtr`  | Set `csCount`+1 CLUT entries starting at `csStart`. `csStart = -1` means use the `value` field of each color spec to address the entry. Has no effect on direct devices (return an error). |
| 4        | `SetGamma`        | `VDGamRecPtr`  | Install a gamma correction table. `NIL` means build a linear ramp. |
| 5        | `GrayScreen`      | `VDPgInfoPtr`  | Fill the named page with a 50% gray dither pattern, and on direct devices regenerate a linear-ramp CLUT through gamma. |
| 6        | `SetGray`         | `VDFlagPtr`    | Switch `SetEntries` between color (`0`) and luminance-equivalent grayscale (`1`). |
| 7        | `SetInterrupt`    | `VDFlagPtr`    | Enable (`0`) or disable (`1`) the card's VBL interrupt. |
| 8        | `DirectSetEntries`| `VDEntRecPtr`  | Like `SetEntries` but for direct devices only. Caller must restore the linear ramp afterwards. |
| 9        | `SetDefaultMode`  | `VDDefModePtr` | Store the given video sResource `spID` in slot PRAM as the boot-time default. Monitors uses this when the user picks a new mode family. |
| ≥128     | _vendor-specific_ | -              | Reserved for the driver's own private extensions. |

### 10.5 Status calls (`csCode`)

| `csCode` | Name              | `csParam`      | Notes |
| -------- | ----------------- | -------------- | --- |
| 0, 1     | unused            |                | Return `StatBad`. |
| 2        | `GetMode`         | `VDPgInfoPtr`  | **Required.** Return current `csMode`, `csPage`, `csBaseAddr`. |
| 3        | `GetEntries`      | `VDEntRecPtr`  | **Required.** Return CLUT entries; with gamma applied. |
| 4        | `GetPages`        | `VDPgInfoPtr`  | **Required.** Return total page count for the queried mode. |
| 5        | `GetBaseAddr`     | `VDPgInfoPtr`  | **Required.** Return frame buffer base for a given page. |
| 6        | `GetGray`         | `VDFlagPtr`    | **Required.** Current gray-mode flag. |
| 7        | `GetInterrupt`    | `VDFlagPtr`    | VBL enabled / disabled. |
| 8        | `GetGamma`        | `VDGamRecPtr`  | Pointer to the current gamma table. |
| 9        | `GetDefaultMode`  | `VDDefModePtr` | `spID` of the boot-time default mode. |

Parameter blocks for the calls above use the data types in §10.6.

### 10.6 Parameter records

```pascal
VDPgInfo = record
    csMode:     INTEGER;   { mode within device         }
    csData:     LONGINT;   { data supplied by driver    }
    csPage:     INTEGER;   { page to switch in          }
    csBaseAddr: Ptr;       { base address of page       }
end;

VDEntryRecord = record
    csTable:    Ptr;       { ColorSpec[csCount+1]       }
    csStart:    INTEGER;   { first entry / -1           }
    csCount:    INTEGER;   { entries-1                  }
end;

VDGammaRecord = record
    csGTable:   Ptr;       { GammaTbl                   }
end;

VDFlagRec = record
    flag:       SignedByte;
end;

VDDefModeRec = record
    spID:       SignedByte;
end;
```

The Status convention is that results are returned in `csParamBlock`
(not in the standard I/O parameter block), so callers must use the
low-level `PBStatus` call for any field beyond `csCode`.

## 11. End-to-end startup sequence

To put the pieces together, this is the full sequence of events from
power-on to the first pixel:

1. Slot Manager loops over slots `$9..$E`. For each:
   1. Probes `$FsFF_FFFC..$FsFF_FFFF` for a valid `ByteLanes` byte.
   2. Validates `TestPattern`, `Format`, `RevisionLevel`.
   3. Verifies the CRC over `Length` bytes.
   4. Follows `DirectoryOffset` to the sResource directory.
   5. Walks the directory, copying every sResource's metadata into a
      slot-resource-table entry in main RAM.
2. For each populated slot whose board sResource has a `PRAMInitData`
   block whose `BoardId` differs from the PRAM bytes (i.e. a new card
   or a wiped PRAM), overwrite the 6 modifiable PRAM bytes with the
   block's contents.
3. For each populated slot whose board sResource has a `PrimaryInit`
   entry, run that SExecBlock with `A0 → seBlock` describing the slot.
   The card hardware comes up; sResources for the wrong display,
   memory size, or addressing mode are removed; `seStatus` is stored
   in `sInfoRecord.siInitStatusV`.
4. The Start Manager scans the slot resource tables for
   `CatDisplay / TypVideo / DrSWApple` matches and chooses the startup
   screen.
5. Slot Manager `_HOpen`s the chosen video sResource's driver. For an
   `sRsrcDrvrDir`-style driver, the right `sMacOSnnnnn` sBlock is
   copied to the system heap, a `dCtlEntry` is built, and the
   driver's `Open` routine runs (allocates state, installs VBL ISR via
   `_SIntInstall`, enables VBL on the card).
6. `InitGDevice` builds a `gDevice` from the active mode's `mVidParams`
   block and the driver's reported base address.
7. The Start Manager issues `SetEntries` (default CLUT for the depth),
   `SetGamma` (from the `scrn` resource or the default `gama` 0), and
   draws the "Welcome to Macintosh" splash.
8. After system patches have been loaded but _before_ extensions run,
   Slot Manager v1+ walks the slots again and runs `SecondaryInit` on
   every slot whose `PrimaryInit` succeeded (including
   `seStatus = $8001` cases). Card-specific cleanup (re-publish 32-bit
   sResources, swap `gdPMap.pmBaseAddr`, re-open the driver if
   deferred) happens here.
9. Normal system run-time begins: VBL ticks fire the slot's interrupt
   queue, Color QuickDraw and the Color Manager drive the card via
   Control/Status calls, and the Monitors control panel mutates the
   PRAM defaults via `SetDefaultMode`.

## 12. Summary of structural invariants

A few rules cut across every section above; collecting them in one
place:

- All multi-byte fields are stored **big-endian** in chip-byte order.
  The NuBus interface logic does byte-lane swapping on the bus, but
  the slot manager reads chip bytes one lane at a time, so as far as
  the ROM is concerned the layout is straightforward 68k big-endian.
- Every offset inside the ROM is **self-relative** (computed from the
  address of the offset field itself), **signed**, **24-bit**, and
  measured in **valid byte-lane bytes only**. The Slot Manager scales
  by the byte-lane multiplier internally.
- Every directory or sub-list ends with the **4-byte sentinel**
  `$FF00_0000`. The high byte `$FF` is the EndOfList ID; the low 24
  bits are always zero.
- Entries inside a directory or sub-list must be in **ascending ID
  order**; the Slot Manager scans linearly and gives up at the first
  ID greater than the requested one.
- IDs `0..127` are reserved for Apple; only ID `1` (`BoardSResource`)
  is currently defined in that range. IDs `128..254` are for the
  card designer. ID `255` is the terminator.
- Every `sBlock` and `SExecBlock` starts with its own size in bytes
  (size includes the size field itself), so the Slot Manager can copy
  the block into RAM without having to chase the block's internal
  structure.
- The format block occupies the four-byte cell that ends at
  `$FsFF_FFFF`; everything else hangs off offsets contained within
  it.
- The CRC is computed across exactly `Length` valid-byte-lane bytes,
  starting at `top_of_chip - Length`, with the CRC field itself
  treated as zero during the computation.
- `BoardId` is the one piece of data that can single-handedly
  invalidate the slot — if it is missing or wrong, the entire slot
  is marked empty regardless of what else the ROM contains.
