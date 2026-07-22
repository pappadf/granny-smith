; SPDX-License-Identifier: MIT
; Copyright (c) pappadf
;
; gsvrom_equ.i
; Clean-room equates for the GS generic declaration ROM.  Every value
; here is sourced from our own documentation — docs/core/peripherals/
; nubus_vrom.md, the three annotated whole-ROM disassemblies under
; local/gs-docs/, and the published *Designing Cards and Drivers for the
; Macintosh Family* text — NOT from Apple's AIncludes (proposal sec. 6.3).

; --- Format Block ------------------------------------------------------------
TestPattern     equ     $5A932BC7       ; Apple declROM magic
AppleFormat     equ     $01             ; the only defined format
RomRevision     equ     $01             ; RevisionLevel 1..9

; --- sResource list entry IDs (any sResource) -------------------------------
sRsrcType       equ     1               ; offset → 8-byte type record
sRsrcName       equ     2               ; offset → cString
sRsrcIcon       equ     3               ; offset → 32x32x1 icon
sRsrcDrvrDir    equ     4               ; offset → sDriver directory
sRsrcLoadRec    equ     5               ; offset → sLoadDriver sExec
sRsrcBootRec    equ     6               ; offset → sBootRecord sExec
sRsrcFlags      equ     7               ; word: bit1 fOpenAtStart, bit2 f32BitMode
sRsrcHWDevId    equ     8               ; byte: per-hardware-device tag
MinorBaseOS     equ     10              ; offset → long: std-slot base offset
MinorLength     equ     11              ; offset → long: std-slot length
MajorBaseOS     equ     12              ; offset → long: super-slot base
MajorLength     equ     13              ; offset → long: super-slot length
sGammaDir       equ     64              ; offset → gamma directory
sRsrcVidNames   equ     65              ; offset → video-mode name directory

; sRsrcFlags bit values (word form combines both)
fOpenAtStart    equ     2               ; bit 1: open driver at startup
f32BitMode      equ     4               ; bit 2: 32-bit-mode base address

; --- Board sResource entry IDs ----------------------------------------------
BoardId         equ     32              ; word: DTS-assigned product id (required)
PRAMInitData    equ     33              ; offset → sBlock, 6 modifiable PRAM bytes
PrimaryInit     equ     34              ; offset → sExecBlock
STimeOut        equ     35              ; word: bus timeout constant
VendorInfo      equ     36              ; offset → vendor string sub-list
BoardFlags      equ     37              ; word/inline: board feature flags
SecondaryInit   equ     38              ; offset → sExecBlock

; VendorInfo sub-list IDs
VendorId        equ     1               ; vendor name cString
SerialNum       equ     2
RevLevel        equ     3
PartNum         equ     4
VendorDate      equ     5

; --- sRsrcType record values -------------------------------------------------
CatBoard        equ     $0001           ; board sResource category
CatDisplay      equ     $0003           ; display category
TypBoard        equ     $0000
TypVideo        equ     $0001
DrSWBoard       equ     $0000
DrSWApple       equ     $0001           ; QuickDraw-compatible driver interface
DrHWBoard       equ     $0000

; --- sDriver directory IDs ---------------------------------------------------
sMacOS68000     equ     1
sMacOS68020     equ     2
sMacOS68030     equ     3
sMacOS68040     equ     4

; --- Functional video sResource: per-mode sub-list IDs -----------------------
mVidParams      equ     1               ; offset → VPBlock sBlock
mTable          equ     2               ; offset → fixed device ColorTable
mPageCnt        equ     3               ; word: page count
mDevType        equ     4               ; word: 0 indexed CLUT / 1 fixed / 2 direct
FirstVidMode    equ     128             ; mode-list IDs run 128, 129, ...
SecondVidMode   equ     129
ThirdVidMode    equ     130
FourthVidMode   equ     131
FifthVidMode    equ     132
SixthVidMode    equ     133

; mDevType values
clutType        equ     0               ; indexed, settable CLUT
fixedType       equ     1               ; indexed, fixed CLUT
directType      equ     2               ; direct RGB

; VPBlock (mVidParams sBlock body) field offsets — after the long size
; field; see nubus_vrom.md sec. 6.2.
vpBaseOffset    equ     0               ; long: page-0 offset from FB base
vpRowBytes      equ     4               ; word
vpBounds        equ     6               ; 4 words: t/l/b/r
vpVersion       equ     14              ; word: 1
vpPackType      equ     16              ; word: 0
vpPackSize      equ     18              ; long: 0
vpHRes          equ     22              ; long: Fixed dpi
vpVRes          equ     26              ; long: Fixed dpi
vpPixelType     equ     30              ; word: 0 chunky indexed / $10 direct
vpPixelSize     equ     32              ; word: bpp
vpCmpCount      equ     34              ; word
vpCmpSize       equ     36              ; word
vpPlaneBytes    equ     38              ; long: 0

; --- Slot Manager ------------------------------------------------------------
_SlotManager    equ     $A06E           ; selector in D0, A0 → spBlock

; spBlock field offsets (from the annotated disassemblies / published text)
spResult        equ     0               ; long
spsPointer      equ     4               ; long
spSize          equ     8               ; long
spOffsetData    equ     12              ; long
spIOFileName    equ     16              ; long
spsExecPBlk     equ     20              ; long
spParamData     equ     24              ; long
spMisc          equ     28              ; long
spReserved      equ     32              ; long
spIOReserved    equ     36              ; word
spRefNum        equ     38              ; word
spCategory      equ     40              ; word
spCType         equ     42              ; word
spDrvrSW        equ     44              ; word
spDrvrHW        equ     46              ; word
spTBMask        equ     48              ; byte
spSlot          equ     49              ; byte
spID            equ     50              ; byte
spExtDev        equ     51              ; byte
spHwDev         equ     52              ; byte
spByteLanes     equ     53              ; byte
spFlags         equ     54              ; byte
spKey           equ     55              ; byte
spBlockSize     equ     56              ; sizeof(spBlock)

; Slot Manager selectors (D0 values)
sReadByte       equ     $00
sReadWord       equ     $01
sReadLong       equ     $02
sGetcString     equ     $03
sGetBlock       equ     $05
sFindStruct     equ     $06
sReadStruct     equ     $07
sVersion        equ     $08
sSetSRsrcState  equ     $09
sInsertSRTRec   equ     $0A
sGetSRsrc       equ     $0B
sGetTypeSRsrc   equ     $0C
sReadInfo       equ     $10
sReadPRAMRec    equ     $11
sPutPRAMRec     equ     $12
sReadFHeader    equ     $13
sNextSRsrc      equ     $14
sNextTypeSRsrc  equ     $15
sRsrcInfo       equ     $16
sCkCardStat     equ     $18
sReadDrvrName   equ     $19
sFindDevBase    equ     $1B
sDeleteSRTRec   equ     $31

; seBlock (sExec parameter block, A0 on entry to PrimaryInit/SecondaryInit)
seSlot          equ     0               ; byte
sesRsrcId       equ     1               ; byte
seStatus        equ     2               ; word (result)
seFlags         equ     4               ; byte
seFiller0       equ     5
seFiller1       equ     6
seFiller2       equ     7
seResult        equ     8               ; long
seIOFileName    equ     12              ; long
seDevice        equ     16              ; byte
sePartition     equ     17
seOSType        equ     18
seReserved      equ     19
seRefNum        equ     20              ; byte
seNumDevices    equ     21
seBootState     equ     22              ; byte

; --- Traps used by the driver / init code ------------------------------------
_SIntInstall    equ     $A075
_SIntRemove     equ     $A076
_ReadXPRam      equ     $A051
_WriteXPRam     equ     $A052
_BlockMove      equ     $A02E
_NewPtrSysClear equ     $A71E           ; _NewPtr ,Sys,Clear
_NewHandleSysClear equ  $A722           ; _NewHandle ,Sys,Clear
_HLock          equ     $A029
_DisposePtr     equ     $A01F
_DisposeHandle  equ     $A023
_StripAddress   equ     $A055
_SwapMMUMode    equ     $A05D
_Gestalt        equ     $A1AD
_GetOSTrapAddress equ   $A346           ; D0 = trap number, returns A0
TrapNumGestalt  equ     $AD             ; _Gestalt's OS-trap number
TrapNumUnimpl   equ     $9F             ; _Unimplemented's trap number

; Low-memory globals
JIODone         equ     $08FC           ; jump vector: Device Manager IODone
JVBLTask        equ     $0D28           ; jump vector: run slot VBL tasks (D0=slot)
MMU32Bit        equ     $0CB2           ; current addressing-mode flag byte
MainDevice      equ     $08A8           ; handle: the boot-screen GDevice
ScrnBase        equ     $0824           ; long: boot-screen framebuffer base
UTableBase      equ     $011C           ; pointer: unit table (DCE handles)

; GDevice / PixMap field offsets used by SecondaryInit's screen re-point
gdRefNum        equ     0               ; word: driving driver's refNum
gdPMap          equ     22              ; long: handle to the screen PixMap
pmBaseAddr      equ     0               ; long: PixMap base address

; --- Device Manager / DRVR ---------------------------------------------------
; DCE (AuxDCE) field offsets — dCtlSlot at $28 confirmed against the real
; drivers' `MOVE.B $28(A1),D0` slot-base derivation.
dCtlDriver      equ     0               ; long
dCtlFlags       equ     4               ; word
dCtlQHdr        equ     6               ; 10 bytes (qFlags.w, qHead.l, qTail.l)
dCtlPosition    equ     16              ; long
dCtlStorage     equ     20              ; long (handle to private storage)
dCtlRefNum      equ     24              ; word
dCtlCurTicks    equ     26              ; long
dCtlWindow      equ     30              ; long
dCtlDelay       equ     34              ; word
dCtlEMask       equ     36              ; word
dCtlMenu        equ     38              ; word
dCtlSlot        equ     40              ; byte ($28)
dCtlSlotId      equ     41              ; byte
dCtlDevBase     equ     42              ; long: slot device base address
dCtlOwner       equ     46              ; long
dCtlExtDev      equ     50              ; byte

; DRVR flags word bits
dNeedLockMask   equ     $4000
dStatEnableMask equ     $2000
dCtlEnableMask  equ     $0800
dWritEnableMask equ     $0200
dReadEnableMask equ     $0100

; IO parameter block offsets used by Control/Status
ioTrap          equ     6               ; word
ioCmdAddr       equ     8               ; long
ioCompletion    equ     12              ; long
ioResult        equ     16              ; word
ioVRefNum       equ     22              ; word
ioCRefNum       equ     24              ; word
csCode          equ     26              ; word
csParam         equ     28              ; start of cs parameter area
noQueueBit      equ     9               ; ioTrap bit: immediate (don't IODone)

; Video driver Control csCodes
cscReset            equ 0
cscKillIO           equ 1
cscSetMode          equ 2
cscSetEntries       equ 3
cscSetGamma         equ 4
cscGrayScreen       equ 5
cscSetGray          equ 6
cscSetInterrupt     equ 7
cscDirectSetEntries equ 8
cscSetDefaultMode   equ 9

; Video driver Status csCodes
cscGetMode          equ 2
cscGetEntries       equ 3
cscGetPageCnt       equ 4
cscGetPageBase      equ 5
cscGetGray          equ 6
cscGetInterrupt     equ 7
cscGetGamma         equ 8
cscGetDefaultMode   equ 9

; VDPageInfo (csParam points at one of these for mode calls)
csMode          equ     0               ; word
csData          equ     2               ; long
csPage          equ     6               ; word
csBaseAddr      equ     8               ; long

; VDEntryRecord
csTable         equ     0               ; long: ColorSpec *
csStart         equ     4               ; word: first entry / -1
csCount         equ     6               ; word: entries-1

; VDGammaRecord
csGTable        equ     0               ; long: GammaTbl *

; Result codes
noErr           equ     0
controlErr      equ     -17
statusErr       equ     -18
openErr         equ     -23
paramErr        equ     -50

; Slot IRQ queue element (SQElem)
sqLink          equ     0
sqType          equ     4
sqPrio          equ     6
sqAddr          equ     8
sqParm          equ     12
sqSize          equ     16
sqHW            equ     6               ; queue type id value ("slot queue")
