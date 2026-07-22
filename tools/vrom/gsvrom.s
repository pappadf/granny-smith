| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| gsvrom.s
| GS generic NuBus declaration ROM — top-level source.  One source tree,
| four personalities selected at assembly time (--defsym PERSONALITY_JMFB
| / _BOOGIE / _MDCGC / _SE30 =1); the shared core is byte-identical across
| images and only the Card Parameter Block, ops table, and declarative
| records differ (proposal-generic-nubus-vrom.md sec. 3).
|
| Layout: assembled bottom-up as a flat binary; crc.py stamps Length/CRC/
| ByteLanes so the Format Block ends at the top of the chip (and thus at
| the top of the slot's standard space once the loader tail-places it).
|
| Assembler: GNU as (m68k binutils) in .altmacro mode; see gsvrom_macros.i
| for the port notes (runtime-vrom proposal sec. 2.1).

	.altmacro
	.include "gsvrom_equ.i"
	.include "gsvrom_macros.i"

| --- Personality selection ---------------------------------------------------
	.ifdef	PERSONALITY_JMFB
.equ GS_BOARDID, 0x0027                 | reuse the real 8*24 BoardId (sec. 5.2)
.equ GS_FULL, 1
	.include "ops_jmfb.s"
	.endif
	.ifdef	PERSONALITY_BOOGIE
.equ GS_BOARDID, 0x05FA                 | Display Card 24AC BoardId
.equ GS_FULL, 1
	.include "ops_boogie.s"
	.endif
	.ifdef	PERSONALITY_MDCGC
.equ GS_BOARDID, 0x002C                 | 8*24 GC BoardId (GraphAccel matches $2C)
.equ GS_FULL, 1
	.include "ops_mdcgc.s"
	.endif
	.ifdef	PERSONALITY_SE30
.equ GS_BOARDID, 0x000C                 | SE/30 onboard-video BoardId
.equ GS_FULL, 1
	.include "ops_se30.s"
	.endif

| =============================================================================
| sResource directory
| =============================================================================
RomStart:
Directory:
	OSLstEntry 1,BoardSRsrc         | board sResource (ID 1)
	.ifdef	PERSONALITY_JMFB
	OSLstEntry 0xA1,VSA1            | 15" Portrait
	OSLstEntry 0xA2,VSA2            | 12" RGB
	OSLstEntry 0xA6,VSA6            | 13" AppleColor
	OSLstEntry 0xA7,VSA7            | 21" RGB
	.endif
	.ifdef	PERSONALITY_BOOGIE
	OSLstEntry 0x6B,VS6B            | 640x480 multisync
	OSLstEntry 0x6C,VS6C            | 800x600
	OSLstEntry 0x6D,VS6D            | 832x624
	.endif
	.ifdef	PERSONALITY_MDCGC
	OSLstEntry 0x80,VSB0GC          | 24-bit boot family (std-slot VRAM)
	OSLstEntry 0xA0,VSA0GC          | 32-bit family (super-slot DRAM)
	.endif
	.ifdef	PERSONALITY_SE30
	OSLstEntry 0x80,VS80SE          | built-in 9" CRT, 512x342x1
	.endif
	EndOfList

| =============================================================================
| Board sResource
| =============================================================================
BoardSRsrc:
	OSLstEntry sRsrcType,BoardType
	OSLstEntry sRsrcName,BoardName
	DatLstEntry BoardId,GS_BOARDID
	.ifdef	GS_FULL
	OSLstEntry PRAMInitData,GSPramInit
	OSLstEntry PrimaryInit,GSPInit
	.endif
	OSLstEntry VendorInfo,BoardVendor
	.ifdef	PERSONALITY_MDCGC
	OSLstEntry SecondaryInit,GSSInit
	.endif
	EndOfList

BoardType:
	dc.w	CatBoard,TypBoard,DrSWBoard,DrHWBoard

	.ifdef	PERSONALITY_JMFB
BoardName:
	.asciz	"GS Generic Display (8*24)"
	.balign	2
	.endif
	.ifdef	PERSONALITY_BOOGIE
BoardName:
	.asciz	"GS Generic Display (24AC)"
	.balign	2
	.endif
	.ifdef	PERSONALITY_MDCGC
BoardName:
	.asciz	"GS Generic Display (8*24 GC)"
	.balign	2
	.endif
	.ifdef	PERSONALITY_SE30
BoardName:
	.asciz	"GS Generic Display (SE/30)"
	.balign	2
	.endif

BoardVendor:
	OSLstEntry VendorId,VendorIdStr
	OSLstEntry RevLevel,RevLevelStr
	OSLstEntry PartNum,PartNumStr
	EndOfList

VendorIdStr:
	.asciz	"granny-smith"
	.balign	2
	.ifdef	PERSONALITY_MDCGC
| The GC accelerator's host driver version-gates on the RevLevel
| cString (sec. 5.2/7.1): byte-authentic value where software
| matches — "MDC 8<bullet>24 GC 1.1" with the MacRoman bullet (0xA5).
RevLevelStr:
	.ascii	"MDC 8"
	dc.b	0xA5
	.asciz	"24 GC 1.1"
	.balign	2
	.else
RevLevelStr:
	.asciz	"1.0"
	.balign	2
	.endif
PartNumStr:
	.asciz	"gsvrom"
	.balign	2

	.ifdef	GS_FULL
| Slot PRAM defaults when the Slot Manager first sees this board (or the
| BoardId changed): savedMode = 0x80 (1 bpp); PrimaryInit fills in the
| sister ids once the monitor sense is known.
GSPramInit:
	dc.l	12                      | sBlock size (incl. self)
	dc.b	0,0,0x80,0              | -,-, b1 = savedMode, b2
	dc.b	0,0,0,0                 | b3..b6

| =============================================================================
| Functional video sResources (shared records + one sResource per monitor)
| =============================================================================
GSVidType:
	dc.w	CatDisplay,TypVideo,DrSWApple,GS_DRHW
	GSVidNameStr

GSMinorBase:
	dc.l	GS_FB_MINOR             | framebuffer offset in std slot space

GSDrvrDir:
	OSLstEntry sMacOS68020,GSDrvrBlk
	EndOfList

	.ifdef	PERSONALITY_JMFB
	VideoSRsrc4 A1,640,870          | 15" Portrait  (spID 0xA1)
	VideoSRsrc4 A2,512,384          | 12" RGB       (spID 0xA2)
	VideoSRsrc4 A6,640,480          | 13" AppleColor (spID 0xA6)
	VideoSRsrc4 A7,1152,870         | 21" RGB       (spID 0xA7)
	.endif
	.ifdef	PERSONALITY_BOOGIE
	VideoSRsrc5D 6B,640,480         | multisync 640x480 (spID 0x6B)
	VideoSRsrc5D 6C,800,600         | 800x600            (spID 0x6C)
	VideoSRsrc5D 6D,832,624         | 832x624            (spID 0x6D)
	.endif
	.ifdef	PERSONALITY_MDCGC
| Two families: the 0x80 BOOT family is 24-bit-addressed (std-slot VRAM
| at 0xA00 — the ROM's Welcome paints there, off the surfaced display)
| and the 0xA0 family is 32-bit-addressed with the framebuffer in
| super-slot DRAM (+0x11400, fixed 1024-byte pitch — the surface the HLE
| displays and the accelerator renders into).  Known stage-2 gap (see
| sec. 7.2 and the BaseAddr op): the visible-Finder re-point on 6.0.8
| needs the real card's full quadrant state machine; the accelerator
| bring-up (attach -> boot -> arm -> gc-on) works against this layout.
VSB0GC:
	OSLstEntry sRsrcType,GSVidType
	OSLstEntry sRsrcName,GSVidName
	OSLstEntry sRsrcDrvrDir,GSDrvrDir
	DatLstEntry sRsrcFlags,fOpenAtStart
	DatLstEntry sRsrcHWDevId,1
	OSLstEntry MinorBaseOS,GSMinorBase
	OSLstEntry MinorLength,GSMinorLen
	OSLstEntry FirstVidMode,VSGCM1
	OSLstEntry SecondVidMode,VSGCM2
	OSLstEntry ThirdVidMode,VSGCM3
	OSLstEntry FourthVidMode,VSGCM4
	EndOfList
GSMinorLen:
	dc.l	1024*480                | window the boot family occupies
VSA0GC:
	OSLstEntry sRsrcType,GSVidType
	OSLstEntry sRsrcName,GSVidName
	OSLstEntry sRsrcDrvrDir,GSDrvrDir
	DatLstEntry sRsrcFlags,fOpenAtStart+f32BitMode
	DatLstEntry sRsrcHWDevId,1
	OSLstEntry MajorBaseOS,GSMajorBase
	OSLstEntry MajorLength,GSMajorLen
	OSLstEntry FirstVidMode,VSGCM1
	OSLstEntry SecondVidMode,VSGCM2
	OSLstEntry ThirdVidMode,VSGCM3
	OSLstEntry FourthVidMode,VSGCM4
	EndOfList
GSMajorBase:
	dc.l	0xC000000+GREG_FB_OFFSET | DRAM framebuffer in super slot space
GSMajorLen:
	dc.l	0x200000-GREG_FB_OFFSET | DRAM size minus the framebuffer offset
VSGCM1:	OSLstEntry mVidParams,VSGCP1
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
VSGCM2:	OSLstEntry mVidParams,VSGCP2
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
VSGCM3:	OSLstEntry mVidParams,VSGCP3
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
VSGCM4:	OSLstEntry mVidParams,VSGCP4
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
	VPBlkR	VSGCP1,640,480,1,1024
	VPBlkR	VSGCP2,640,480,2,1024
	VPBlkR	VSGCP3,640,480,4,1024
	VPBlkR	VSGCP4,640,480,8,1024
	.endif
	.ifdef	PERSONALITY_SE30
| One fixed mode: 512x342 at 1 bpp, rowbytes 64, framebuffer at device
| base + 0x8040 (the primary buffer, exactly as the real onboard vROM
| declares it).
VS80SE:
	OSLstEntry sRsrcType,GSVidType
	OSLstEntry sRsrcName,GSVidName
	OSLstEntry sRsrcDrvrDir,GSDrvrDir
	DatLstEntry sRsrcFlags,fOpenAtStart
	DatLstEntry sRsrcHWDevId,1
	OSLstEntry MinorBaseOS,GSMinorBase
	OSLstEntry MinorLength,GSMinorLenSE
	OSLstEntry FirstVidMode,VS80SEM1
	EndOfList
GSMinorLenSE:
	dc.l	0x8040+64*342           | through the end of the framebuffer
VS80SEM1:
	OSLstEntry mVidParams,VS80SEP1
	DatLstEntry mPageCnt,1
	DatLstEntry mDevType,clutType
	EndOfList
	VPBlkO	VS80SEP1,512,342,1,64,0x8040
	.endif

| =============================================================================
| Executable blocks
| =============================================================================
	.include "gsvrom_init.s"        | PrimaryInit sExecBlock
	.include "gsvrom_drvr.s"        | the DRVR sBlock

	.endif                          | GS_FULL

| =============================================================================
| Format Block (20 bytes; Length/CRC/ByteLanes stamped by crc.py)
| =============================================================================
FormatBlock:
	dc.l	(Directory-.)&0x00FFFFFF | DirectoryOffset (signed 24-bit,
	                                | self-relative; top byte zero)
	dc.l	0                       | Length      — stamped by crc.py
	dc.l	0                       | CRC         — stamped by crc.py
	dc.b	RomRevision             | RevisionLevel
	dc.b	AppleFormat             | Format
	dc.l	TestPattern             | TestPattern
	dc.b	0                       | Reserved
	dc.b	0                       | ByteLanes   — stamped by crc.py
RomEnd:
