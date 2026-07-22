; SPDX-License-Identifier: MIT
; Copyright (c) pappadf
;
; gsvrom.s
; GS generic NuBus declaration ROM — top-level source.  One source tree,
; four personalities selected at assembly time (-DPERSONALITY_JMFB /
; _BOOGIE / _MDCGC / _SE30); the shared core is byte-identical across
; images and only the Card Parameter Block, ops table, and declarative
; records differ (proposal-generic-nubus-vrom.md sec. 3).
;
; Layout: assembled bottom-up as a flat binary; crc.py stamps Length/CRC/
; ByteLanes so the Format Block ends at the top of the chip (and thus at
; the top of the slot's standard space once the loader tail-places it).
;
; Stage status: the JMFB personality carries the full structure (video
; sResources + PrimaryInit + DRVR); the other three are board-sResource-
; only skeletons until their stages land (proposal sec. 9).

        include "gsvrom_equ.i"
        include "gsvrom_macros.i"

; --- Personality selection ---------------------------------------------------
        ifd     PERSONALITY_JMFB
GS_BOARDID      equ     $0027           ; reuse the real 8•24 BoardId (sec. 5.2)
GS_FULL         equ     1
        include "ops_jmfb.s"
        endif
        ifd     PERSONALITY_BOOGIE
GS_BOARDID      equ     $05FA           ; Display Card 24AC BoardId
GS_FULL         equ     1
        include "ops_boogie.s"
        endif
        ifd     PERSONALITY_MDCGC
GS_BOARDID      equ     $002C           ; 8•24 GC BoardId (GraphAccel matches $2C)
GS_FULL         equ     1
        include "ops_mdcgc.s"
        endif
        ifd     PERSONALITY_SE30
GS_BOARDID      equ     $000C           ; SE/30 onboard-video BoardId
GS_FULL         equ     1
        include "ops_se30.s"
        endif

; =============================================================================
; sResource directory
; =============================================================================
RomStart:
Directory:
        OSLstEntry 1,BoardSRsrc         ; board sResource (ID 1)
        ifd     PERSONALITY_JMFB
        OSLstEntry $A1,VSA1             ; 15" Portrait
        OSLstEntry $A2,VSA2             ; 12" RGB
        OSLstEntry $A6,VSA6             ; 13" AppleColor
        OSLstEntry $A7,VSA7             ; 21" RGB
        endif
        ifd     PERSONALITY_BOOGIE
        OSLstEntry $6B,VS6B             ; 640×480 multisync
        OSLstEntry $6C,VS6C             ; 800×600
        OSLstEntry $6D,VS6D             ; 832×624
        endif
        ifd     PERSONALITY_MDCGC
        OSLstEntry $80,VSB0GC           ; 24-bit boot family (std-slot VRAM)
        OSLstEntry $A0,VSA0GC           ; 32-bit family (super-slot DRAM)
        endif
        ifd     PERSONALITY_SE30
        OSLstEntry $80,VS80SE           ; built-in 9" CRT, 512×342×1
        endif
        EndOfList

; =============================================================================
; Board sResource
; =============================================================================
BoardSRsrc:
        OSLstEntry sRsrcType,BoardType
        OSLstEntry sRsrcName,BoardName
        DatLstEntry BoardId,GS_BOARDID
        ifd     GS_FULL
        OSLstEntry PRAMInitData,GSPramInit
        OSLstEntry PrimaryInit,GSPInit
        endif
        OSLstEntry VendorInfo,BoardVendor
        ifd     PERSONALITY_MDCGC
        OSLstEntry SecondaryInit,GSSInit
        endif
        EndOfList

BoardType:
        dc.w    CatBoard,TypBoard,DrSWBoard,DrHWBoard

        ifd     PERSONALITY_JMFB
        cString BoardName,"GS Generic Display (8*24)"
        endif
        ifd     PERSONALITY_BOOGIE
        cString BoardName,"GS Generic Display (24AC)"
        endif
        ifd     PERSONALITY_MDCGC
        cString BoardName,"GS Generic Display (8*24 GC)"
        endif
        ifd     PERSONALITY_SE30
        cString BoardName,"GS Generic Display (SE/30)"
        endif

BoardVendor:
        OSLstEntry VendorId,VendorIdStr
        OSLstEntry RevLevel,RevLevelStr
        OSLstEntry PartNum,PartNumStr
        EndOfList

        cString VendorIdStr,"granny-smith"
        ifd     PERSONALITY_MDCGC
        ; The GC accelerator's host driver version-gates on the RevLevel
        ; cString (sec. 5.2/7.1): byte-authentic value where software
        ; matches — "MDC 8•24 GC 1.1" with the MacRoman bullet ($A5).
RevLevelStr:
        dc.b    "MDC 8",$A5,"24 GC 1.1",0
        even
        else
        cString RevLevelStr,"1.0"
        endif
        cString PartNumStr,"gsvrom"

        ifd     GS_FULL
; Slot PRAM defaults when the Slot Manager first sees this board (or the
; BoardId changed): savedMode = $80 (1 bpp); PrimaryInit fills in the
; sister ids once the monitor sense is known.
GSPramInit:
        dc.l    12                      ; sBlock size (incl. self)
        dc.b    0,0,$80,0               ; -,-, b1 = savedMode, b2
        dc.b    0,0,0,0                 ; b3..b6

; =============================================================================
; Functional video sResources (shared records + one sResource per monitor)
; =============================================================================
GSVidType:
        dc.w    CatDisplay,TypVideo,DrSWApple,GS_DRHW
        GSVidNameStr

GSMinorBase:
        dc.l    GS_FB_MINOR             ; framebuffer offset in std slot space

GSDrvrDir:
        OSLstEntry sMacOS68020,GSDrvrBlk
        EndOfList

        ifd     PERSONALITY_JMFB
        VideoSRsrc4 A1,640,870          ; 15" Portrait  (spID $A1)
        VideoSRsrc4 A2,512,384          ; 12" RGB       (spID $A2)
        VideoSRsrc4 A6,640,480          ; 13" AppleColor (spID $A6)
        VideoSRsrc4 A7,1152,870         ; 21" RGB       (spID $A7)
        endif
        ifd     PERSONALITY_BOOGIE
        VideoSRsrc5D 6B,640,480         ; multisync 640×480 (spID $6B)
        VideoSRsrc5D 6C,800,600         ; 800×600            (spID $6C)
        VideoSRsrc5D 6D,832,624         ; 832×624            (spID $6D)
        endif
        ifd     PERSONALITY_MDCGC
; Two families: the $80 BOOT family is 24-bit-addressed (std-slot VRAM
; at $A00 — the ROM's Welcome paints there, off the surfaced display)
; and the $A0 family is 32-bit-addressed with the framebuffer in
; super-slot DRAM (+$11400, fixed 1024-byte pitch — the surface the HLE
; displays and the accelerator renders into).  Known stage-2 gap (see
; sec. 7.2 and the BaseAddr op): the visible-Finder re-point on 6.0.8
; needs the real card's full quadrant state machine; the accelerator
; bring-up (attach → boot → arm → gc-on) works against this layout.
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
        dc.l    1024*480                ; window the boot family occupies
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
        dc.l    $C000000+GREG_FB_OFFSET ; DRAM framebuffer in super slot space
GSMajorLen:
        dc.l    $200000-GREG_FB_OFFSET  ; DRAM size minus the framebuffer offset
VSGCM1: OSLstEntry mVidParams,VSGCP1
        DatLstEntry mPageCnt,1
        DatLstEntry mDevType,clutType
        EndOfList
VSGCM2: OSLstEntry mVidParams,VSGCP2
        DatLstEntry mPageCnt,1
        DatLstEntry mDevType,clutType
        EndOfList
VSGCM3: OSLstEntry mVidParams,VSGCP3
        DatLstEntry mPageCnt,1
        DatLstEntry mDevType,clutType
        EndOfList
VSGCM4: OSLstEntry mVidParams,VSGCP4
        DatLstEntry mPageCnt,1
        DatLstEntry mDevType,clutType
        EndOfList
        VPBlkR  VSGCP1,640,480,1,1024
        VPBlkR  VSGCP2,640,480,2,1024
        VPBlkR  VSGCP3,640,480,4,1024
        VPBlkR  VSGCP4,640,480,8,1024
        endif
        ifd     PERSONALITY_SE30
; One fixed mode: 512×342 at 1 bpp, rowbytes 64, framebuffer at device
; base + $8040 (the primary buffer, exactly as the real onboard vROM
; declares it).
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
        dc.l    $8040+64*342            ; through the end of the framebuffer
VS80SEM1:
        OSLstEntry mVidParams,VS80SEP1
        DatLstEntry mPageCnt,1
        DatLstEntry mDevType,clutType
        EndOfList
        VPBlkO  VS80SEP1,512,342,1,64,$8040
        endif

; =============================================================================
; Executable blocks
; =============================================================================
        include "gsvrom_init.s"         ; PrimaryInit sExecBlock
        include "gsvrom_drvr.s"         ; the DRVR sBlock

        endif                           ; GS_FULL

; =============================================================================
; Format Block (20 bytes; Length/CRC/ByteLanes stamped by crc.py)
; =============================================================================
FormatBlock:
        dc.l    (Directory-*)&$00FFFFFF ; DirectoryOffset (signed 24-bit,
                                        ; self-relative; top byte zero)
        dc.l    0                       ; Length      — stamped by crc.py
        dc.l    0                       ; CRC         — stamped by crc.py
        dc.b    RomRevision             ; RevisionLevel
        dc.b    AppleFormat             ; Format
        dc.l    TestPattern             ; TestPattern
        dc.b    0                       ; Reserved
        dc.b    0                       ; ByteLanes   — stamped by crc.py
RomEnd:
