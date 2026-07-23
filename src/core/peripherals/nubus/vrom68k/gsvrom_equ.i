| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| gsvrom_equ.i
| Clean-room equates for the GS generic declaration ROM.  Every value
| here is sourced from our own documentation — docs/core/peripherals/
| nubus_vrom.md, the three annotated whole-ROM disassemblies under
| local/gs-docs/, and the published *Designing Cards and Drivers for the
| Macintosh Family* text — NOT from Apple's AIncludes (proposal sec. 6.3).

| --- Format Block ------------------------------------------------------------
.equ TestPattern,     0x5A932BC7      | Apple declROM magic
.equ AppleFormat,     0x01            | the only defined format
.equ RomRevision,     0x01            | RevisionLevel 1..9

| --- sResource list entry IDs (any sResource) -------------------------------
.equ sRsrcType,       1               | offset -> 8-byte type record
.equ sRsrcName,       2               | offset -> cString
.equ sRsrcIcon,       3               | offset -> 32x32x1 icon
.equ sRsrcDrvrDir,    4               | offset -> sDriver directory
.equ sRsrcLoadRec,    5               | offset -> sLoadDriver sExec
.equ sRsrcBootRec,    6               | offset -> sBootRecord sExec
.equ sRsrcFlags,      7               | word: bit1 fOpenAtStart, bit2 f32BitMode
.equ sRsrcHWDevId,    8               | byte: per-hardware-device tag
.equ MinorBaseOS,     10              | offset -> long: std-slot base offset
.equ MinorLength,     11              | offset -> long: std-slot length
.equ MajorBaseOS,     12              | offset -> long: super-slot base
.equ MajorLength,     13              | offset -> long: super-slot length
.equ sGammaDir,       64              | offset -> gamma directory
.equ sRsrcVidNames,   65              | offset -> video-mode name directory

| sRsrcFlags bit values (word form combines both)
.equ fOpenAtStart,    2               | bit 1: open driver at startup
.equ f32BitMode,      4               | bit 2: 32-bit-mode base address

| --- Board sResource entry IDs ----------------------------------------------
.equ BoardId,         32              | word: DTS-assigned product id (required)
.equ PRAMInitData,    33              | offset -> sBlock, 6 modifiable PRAM bytes
.equ PrimaryInit,     34              | offset -> sExecBlock
.equ STimeOut,        35              | word: bus timeout constant
.equ VendorInfo,      36              | offset -> vendor string sub-list
.equ BoardFlags,      37              | word/inline: board feature flags
.equ SecondaryInit,   38              | offset -> sExecBlock

| VendorInfo sub-list IDs
.equ VendorId,        1               | vendor name cString
.equ SerialNum,       2
.equ RevLevel,        3
.equ PartNum,         4
.equ VendorDate,      5

| --- sRsrcType record values -------------------------------------------------
.equ CatBoard,        0x0001          | board sResource category
.equ CatDisplay,      0x0003          | display category
.equ TypBoard,        0x0000
.equ TypVideo,        0x0001
.equ DrSWBoard,       0x0000
.equ DrSWApple,       0x0001          | QuickDraw-compatible driver interface
.equ DrHWBoard,       0x0000

| --- sDriver directory IDs ---------------------------------------------------
.equ sMacOS68000,     1
.equ sMacOS68020,     2
.equ sMacOS68030,     3
.equ sMacOS68040,     4

| --- Functional video sResource: per-mode sub-list IDs -----------------------
.equ mVidParams,      1               | offset -> VPBlock sBlock
.equ mTable,          2               | offset -> fixed device ColorTable
.equ mPageCnt,        3               | word: page count
.equ mDevType,        4               | word: 0 indexed CLUT / 1 fixed / 2 direct
.equ FirstVidMode,    128             | mode-list IDs run 128, 129, ...
.equ SecondVidMode,   129
.equ ThirdVidMode,    130
.equ FourthVidMode,   131
.equ FifthVidMode,    132
.equ SixthVidMode,    133

| mDevType values
.equ clutType,        0               | indexed, settable CLUT
.equ fixedType,       1               | indexed, fixed CLUT
.equ directType,      2               | direct RGB

| VPBlock (mVidParams sBlock body) field offsets — after the long size
| field; see nubus_vrom.md sec. 6.2.
.equ vpBaseOffset,    0               | long: page-0 offset from FB base
.equ vpRowBytes,      4               | word
.equ vpBounds,        6               | 4 words: t/l/b/r
.equ vpVersion,       14              | word: 1
.equ vpPackType,      16              | word: 0
.equ vpPackSize,      18              | long: 0
.equ vpHRes,          22              | long: Fixed dpi
.equ vpVRes,          26              | long: Fixed dpi
.equ vpPixelType,     30              | word: 0 chunky indexed / 0x10 direct
.equ vpPixelSize,     32              | word: bpp
.equ vpCmpCount,      34              | word
.equ vpCmpSize,       36              | word
.equ vpPlaneBytes,    38              | long: 0

| --- Slot Manager ------------------------------------------------------------
.equ _SlotManager,    0xA06E          | selector in D0, A0 -> spBlock

| spBlock field offsets (from the annotated disassemblies / published text)
.equ spResult,        0               | long
.equ spsPointer,      4               | long
.equ spSize,          8               | long
.equ spOffsetData,    12              | long
.equ spIOFileName,    16              | long
.equ spsExecPBlk,     20              | long
.equ spParamData,     24              | long
.equ spMisc,          28              | long
.equ spReserved,      32              | long
.equ spIOReserved,    36              | word
.equ spRefNum,        38              | word
.equ spCategory,      40              | word
.equ spCType,         42              | word
.equ spDrvrSW,        44              | word
.equ spDrvrHW,        46              | word
.equ spTBMask,        48              | byte
.equ spSlot,          49              | byte
.equ spID,            50              | byte
.equ spExtDev,        51              | byte
.equ spHwDev,         52              | byte
.equ spByteLanes,     53              | byte
.equ spFlags,         54              | byte
.equ spKey,           55              | byte
.equ spBlockSize,     56              | sizeof(spBlock)

| Slot Manager selectors (D0 values)
.equ sReadByte,       0x00
.equ sReadWord,       0x01
.equ sReadLong,       0x02
.equ sGetcString,     0x03
.equ sGetBlock,       0x05
.equ sFindStruct,     0x06
.equ sReadStruct,     0x07
.equ sVersion,        0x08
.equ sSetSRsrcState,  0x09
.equ sInsertSRTRec,   0x0A
.equ sGetSRsrc,       0x0B
.equ sGetTypeSRsrc,   0x0C
.equ sReadInfo,       0x10
.equ sReadPRAMRec,    0x11
.equ sPutPRAMRec,     0x12
.equ sReadFHeader,    0x13
.equ sNextSRsrc,      0x14
.equ sNextTypeSRsrc,  0x15
.equ sRsrcInfo,       0x16
.equ sCkCardStat,     0x18
.equ sReadDrvrName,   0x19
.equ sFindDevBase,    0x1B
.equ sDeleteSRTRec,   0x31

| seBlock (sExec parameter block, A0 on entry to PrimaryInit/SecondaryInit)
.equ seSlot,          0               | byte
.equ sesRsrcId,       1               | byte
.equ seStatus,        2               | word (result)
.equ seFlags,         4               | byte
.equ seFiller0,       5
.equ seFiller1,       6
.equ seFiller2,       7
.equ seResult,        8               | long
.equ seIOFileName,    12              | long
.equ seDevice,        16              | byte
.equ sePartition,     17
.equ seOSType,        18
.equ seReserved,      19
.equ seRefNum,        20              | byte
.equ seNumDevices,    21
.equ seBootState,     22              | byte

| --- Traps used by the driver / init code ------------------------------------
.equ _SIntInstall,    0xA075
.equ _SIntRemove,     0xA076
.equ _ReadXPRam,      0xA051
.equ _WriteXPRam,     0xA052
.equ _BlockMove,      0xA02E
.equ _NewPtrSysClear, 0xA71E          | _NewPtr ,Sys,Clear
.equ _NewHandleSysClear, 0xA722       | _NewHandle ,Sys,Clear
.equ _HLock,          0xA029
.equ _DisposePtr,     0xA01F
.equ _DisposeHandle,  0xA023
.equ _StripAddress,   0xA055
.equ _SwapMMUMode,    0xA05D
.equ _Gestalt,        0xA1AD
.equ _GetOSTrapAddress, 0xA346        | D0 = trap number, returns A0
.equ TrapNumGestalt,  0xAD            | _Gestalt's OS-trap number
.equ TrapNumUnimpl,   0x9F            | _Unimplemented's trap number

| Low-memory globals
.equ JIODone,         0x08FC          | jump vector: Device Manager IODone
.equ JVBLTask,        0x0D28          | jump vector: run slot VBL tasks (D0=slot)
.equ MMU32Bit,        0x0CB2          | current addressing-mode flag byte
.equ MainDevice,      0x08A8          | handle: the boot-screen GDevice
.equ ScrnBase,        0x0824          | long: boot-screen framebuffer base
.equ UTableBase,      0x011C          | pointer: unit table (DCE handles)

| GDevice / PixMap field offsets used by SecondaryInit's screen re-point
.equ gdRefNum,        0               | word: driving driver's refNum
.equ gdPMap,          22              | long: handle to the screen PixMap
.equ pmBaseAddr,      0               | long: PixMap base address

| --- Device Manager / DRVR ---------------------------------------------------
| DCE (AuxDCE) field offsets — dCtlSlot at 0x28 confirmed against the real
| drivers' `MOVE.B $28(A1),D0` slot-base derivation.
.equ dCtlDriver,      0               | long
.equ dCtlFlags,       4               | word
.equ dCtlQHdr,        6               | 10 bytes (qFlags.w, qHead.l, qTail.l)
.equ dCtlPosition,    16              | long
.equ dCtlStorage,     20              | long (handle to private storage)
.equ dCtlRefNum,      24              | word
.equ dCtlCurTicks,    26              | long
.equ dCtlWindow,      30              | long
.equ dCtlDelay,       34              | word
.equ dCtlEMask,       36              | word
.equ dCtlMenu,        38              | word
.equ dCtlSlot,        40              | byte (0x28)
.equ dCtlSlotId,      41              | byte
.equ dCtlDevBase,     42              | long: slot device base address
.equ dCtlOwner,       46              | long
.equ dCtlExtDev,      50              | byte

| DRVR flags word bits
.equ dNeedLockMask,   0x4000
.equ dStatEnableMask, 0x2000
.equ dCtlEnableMask,  0x0800
.equ dWritEnableMask, 0x0200
.equ dReadEnableMask, 0x0100

| IO parameter block offsets used by Control/Status
.equ ioTrap,          6               | word
.equ ioCmdAddr,       8               | long
.equ ioCompletion,    12              | long
.equ ioResult,        16              | word
.equ ioVRefNum,       22              | word
.equ ioCRefNum,       24              | word
.equ csCode,          26              | word
.equ csParam,         28              | start of cs parameter area
.equ noQueueBit,      9               | ioTrap bit: immediate (don't IODone)

| Video driver Control csCodes
.equ cscReset,            0
.equ cscKillIO,           1
.equ cscSetMode,          2
.equ cscSetEntries,       3
.equ cscSetGamma,         4
.equ cscGrayScreen,       5
.equ cscSetGray,          6
.equ cscSetInterrupt,     7
.equ cscDirectSetEntries, 8
.equ cscSetDefaultMode,   9

| Video driver Status csCodes
.equ cscGetMode,          2
.equ cscGetEntries,       3
.equ cscGetPageCnt,       4
.equ cscGetPageBase,      5
.equ cscGetGray,          6
.equ cscGetInterrupt,     7
.equ cscGetGamma,         8
.equ cscGetDefaultMode,   9

| VDPageInfo (csParam points at one of these for mode calls)
.equ csMode,          0               | word
.equ csData,          2               | long
.equ csPage,          6               | word
.equ csBaseAddr,      8               | long

| VDEntryRecord
.equ csTable,         0               | long: ColorSpec *
.equ csStart,         4               | word: first entry / -1
.equ csCount,         6               | word: entries-1

| VDGammaRecord
.equ csGTable,        0               | long: GammaTbl *

| Result codes
.equ noErr,           0
.equ controlErr,      -17
.equ statusErr,       -18
.equ openErr,         -23
.equ paramErr,        -50

| Slot IRQ queue element (SQElem)
.equ sqLink,          0
.equ sqType,          4
.equ sqPrio,          6
.equ sqAddr,          8
.equ sqParm,          12
.equ sqSize,          16
.equ sqHW,            6               | queue type id value ("slot queue")

| --- Driver private storage layout (dCtlStorage handle, locked) --------------
| Shared between the DRVR framework (gsvrom_drvr.s) and the personality
| ops (which read pvBase/pvSlot in their BaseAddr op) — defined here so
| every EmitOps instantiation sees the values up front.
.equ pvBase,          0               | long: csBaseAddr (dCtlDevBase form)
.equ pvSlotBase,      4               | long: 0xFs000000 (32-bit form)
.equ pvMode,          8               | word: current csMode (0x80 + depth code)
.equ pvFlags,         10              | word: bit0 gray mode, bit1 VBL disabled
.equ pvSlot,          12              | word: slot number
.equ pvWidth,         14              | word: monitor width  (from the record)
.equ pvHeight,        16              | word: monitor height (from the record)
.equ pvRowBytes,      18              | word: current-mode vpRowBytes (record)
.equ pvSQEl,          20              | 16-byte slot interrupt queue element
.equ pvGamma,         36              | 3x256 gamma LUT (R, G, B planes)
.equ pvClut,          36+768          | 3x256 raw CLUT copy (R, G, B planes)
.equ pvGTbl,          36+768+768      | GammaTbl block for GetGamma (12+256)
.equ pvSpID,          36+768+768+268  | word: dCtlSlotId (the sResource that
                                        | loaded us; DrvReadVP queries it)
.equ pvSize,          36+768+768+268+2
