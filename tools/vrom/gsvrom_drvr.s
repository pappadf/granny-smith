; SPDX-License-Identifier: MIT
; Copyright (c) pappadf
;
; gsvrom_drvr.s
; The shared slot video DRVR: Open/Close, Control/Status csCode dispatch,
; and the slot-VBL ISR (proposal sec. 3.1 — everything above the ops line
; never names a register address).  The Slot Manager copies this whole
; sBlock to the system heap, so the CPB data and card ops are emitted
; into it with the "DR" prefix.
;
; csCode ABI per docs/core/peripherals/nubus_vrom.md sec. 10 (confirmed
; against the GC ROM's driver): Control 0-9, Status 2-10.

; --- Driver private storage layout (dCtlStorage handle, locked) --------------
pvBase          equ     0               ; long: csBaseAddr (dCtlDevBase form)
pvSlotBase      equ     4               ; long: $Fs000000 (32-bit form)
pvMode          equ     8               ; word: current csMode ($80 + depth code)
pvFlags         equ     10              ; word: bit0 gray mode, bit1 VBL disabled
pvSlot          equ     12              ; word: slot number
pvWidth         equ     14              ; word: monitor width
pvHeight        equ     16              ; word: monitor height
pvMMUSave       equ     18              ; byte: saved addressing mode (Swap32)
pvSQEl          equ     20              ; 16-byte slot interrupt queue element
pvGamma         equ     36              ; 3×256 gamma LUT (R, G, B planes)
pvClut          equ     36+768          ; 3×256 raw CLUT copy (R, G, B planes)
pvGTbl          equ     36+768+768      ; GammaTbl block for GetGamma (12+256)
pvSize          equ     36+768+768+268+2

; --- The driver sBlock -------------------------------------------------------
        sBlockBegin GSDrvrBlk

GSDrvrHdr:
        dc.w    $4C00                   ; drvrFlags: dNeedLock|dStatEnable|dCtlEnable
        dc.w    0                       ; drvrDelay
        dc.w    0                       ; drvrEMask
        dc.w    0                       ; drvrMenu
        dc.w    DrvOpen-GSDrvrHdr
        dc.w    0                       ; no Prime routine
        dc.w    DrvControl-GSDrvrHdr
        dc.w    DrvStatus-GSDrvrHdr
        dc.w    DrvClose-GSDrvrHdr
        GSDrvrName                      ; personality Pascal name string
        even

; === Open ====================================================================
; A0 = IO parameter block, A1 = DCE.  Allocates the locked private block,
; caches slot geometry, installs the slot-VBL ISR, enables the card VBL.
DrvOpen:
        movem.l d1-d7/a0-a6,-(sp)
        movea.l a1,a3                   ; A3 = DCE
        tst.l   dCtlStorage(a3)
        bne     DrvOpenAgain            ; already open — refresh the base
        move.l  #pvSize,d0
        dc.w    _NewHandleSysClear      ; A0 = handle, D0 = MemErr
        tst.w   d0
        bne     DrvOpenFail
        move.l  a0,dCtlStorage(a3)
        dc.w    _HLock
        move.l  (a0),d0                 ; A5 = private block — STRIPPED, so it
        dc.w    _StripAddress           ; stays valid inside 32-bit Swap32
        movea.l d0,a5                   ; windows (master ptrs carry flag bits
                                        ; in the high byte on 24-bit systems)

        moveq   #0,d5
        move.b  dCtlSlot(a3),d5
        move.w  d5,pvSlot(a5)
        move.l  d5,d0
        swap    d0
        lsl.l   #8,d0
        or.l    #$F0000000,d0
        move.l  d0,pvSlotBase(a5)
        move.l  dCtlDevBase(a3),pvBase(a5)
        move.w  #$80,pvMode(a5)         ; power-on depth: 1 bpp

        ; monitor geometry: dCtlSlotId names the sResource that loaded us
        moveq   #0,d6
        move.b  dCtlSlotId(a3),d6
        lea     DRMonTab(pc),a0
DrvOpenFind:
        move.w  (a0),d0
        beq.s   DrvOpenDefMon           ; not found — first table row
        cmp.w   d0,d6
        beq.s   DrvOpenGotMon
        addq.w  #6,a0
        bra.s   DrvOpenFind
DrvOpenDefMon:
        lea     DRMonTab(pc),a0
DrvOpenGotMon:
        move.w  2(a0),pvWidth(a5)
        move.w  4(a0),pvHeight(a5)

        ; identity gamma LUT + GetGamma table + grayscale-ramp CLUT copy
        bsr     GammaIdentity
        lea     pvGTbl(a5),a0
        clr.w   (a0)+                   ; gVersion
        clr.w   (a0)+                   ; gType
        clr.w   (a0)+                   ; gFormulaSize
        move.w  #1,(a0)+                ; gChanCnt
        move.w  #256,(a0)+              ; gDataCnt
        move.w  #8,(a0)+                ; gDataWidth
        moveq   #0,d0
DrvOpenGTblLoop:
        move.b  d0,(a0)+
        addq.w  #1,d0
        cmp.w   #256,d0
        bne.s   DrvOpenGTblLoop
        lea     pvClut(a5),a0
        moveq   #0,d0
DrvOpenClutLoop:
        move.b  d0,(a0)
        move.b  d0,256(a0)
        move.b  d0,512(a0)
        addq.w  #1,a0
        addq.w  #1,d0
        cmp.w   #256,d0
        bne.s   DrvOpenClutLoop

        ; slot interrupt queue element + card VBL on
        lea     pvSQEl(a5),a0
        move.w  #sqHW,sqType(a0)
        clr.w   sqPrio(a0)
        lea     DrvVBL(pc),a1
        move.l  a1,sqAddr(a0)
        move.l  a5,sqParm(a0)
        move.w  d5,d0
        dc.w    _SIntInstall
        bsr     Swap32
        movea.l pvSlotBase(a5),a4
        bsr     DRVblEnable
        bsr     SwapBack
DrvOpenOk:
        movem.l (sp)+,d1-d7/a0-a6
        moveq   #0,d0
        rts
; Re-opened on a different sResource family (32-Bit QuickDraw's slot
; upgrade re-opens the driver on the 32-bit family): keep the existing
; state but track the new device base.
DrvOpenAgain:
        movea.l dCtlStorage(a3),a5
        move.l  (a5),d0
        dc.w    _StripAddress
        movea.l d0,a5
        move.l  dCtlDevBase(a3),pvBase(a5)
        bra.s   DrvOpenOk
DrvOpenFail:
        movem.l (sp)+,d1-d7/a0-a6
        moveq   #openErr,d0
        rts

; === Close ===================================================================
DrvClose:
        movem.l d1-d7/a0-a6,-(sp)
        movea.l a1,a3
        move.l  dCtlStorage(a3),d0
        beq.s   DrvCloseDone
        movea.l d0,a2                   ; A2 = storage handle
        move.l  (a2),d0
        dc.w    _StripAddress
        movea.l d0,a5
        bsr     Swap32
        movea.l pvSlotBase(a5),a4
        bsr     DRVblDisable
        bsr     SwapBack
        lea     pvSQEl(a5),a0
        move.w  pvSlot(a5),d0
        dc.w    _SIntRemove
        movea.l a2,a0
        dc.w    _DisposeHandle
        clr.l   dCtlStorage(a3)
DrvCloseDone:
        movem.l (sp)+,d1-d7/a0-a6
        moveq   #0,d0
        rts

; === Control =================================================================
; A0 = PB, A1 = DCE.  Handlers run with A0=PB, A2=csParam record, A5=private;
; result in D0.
DrvControl:
        movem.l d1-d7/a1-a6,-(sp)
        movea.l dCtlStorage(a1),a5
        move.l  (a5),d0
        dc.w    _StripAddress           ; flag-free private ptr (see DrvOpen)
        movea.l d0,a5
        move.w  csCode(a0),d0
        cmp.w   #9,d0
        bhi.s   CtlBad
        movea.l csParam(a0),a2
        add.w   d0,d0
        lea     CtlTab(pc),a1
        move.w  (a1,d0.w),d0
        jsr     (a1,d0.w)
        bra.s   DrvExit
CtlBad:
        moveq   #controlErr,d0
        bra.s   DrvExit

; Shared Control/Status exit: honour the immediate bit, else IODone.
DrvExit:
        move.w  ioTrap(a0),d1
        btst    #noQueueBit,d1
        movem.l (sp)+,d1-d7/a1-a6
        beq.s   DrvExitQueued
        rts
DrvExitQueued:
        movea.l JIODone,a0              ; IODone wants A1=DCE (restored), D0=result
        jmp     (a0)

CtlTab:
        dc.w    CtlReset-CtlTab         ; 0
        dc.w    CtlNoop-CtlTab          ; 1 KillIO
        dc.w    CtlSetMode-CtlTab       ; 2
        dc.w    CtlSetEntries-CtlTab    ; 3
        dc.w    CtlSetGamma-CtlTab      ; 4
        dc.w    CtlGrayScreen-CtlTab    ; 5
        dc.w    CtlSetGray-CtlTab       ; 6
        dc.w    CtlSetInterrupt-CtlTab  ; 7
        dc.w    CtlDirectSetEnt-CtlTab  ; 8
        dc.w    CtlSetDefault-CtlTab    ; 9

CtlNoop:
        moveq   #0,d0
        rts

; csCode 0 — Reset: default 1-bpp mode, page 0, gray screen.
CtlReset:
        move.w  #$80,pvMode(a5)
        bsr     ApplyMode
        bsr     GrayFill
        move.w  #$80,csMode(a2)
        clr.w   csPage(a2)
        bsr     DRBaseAddr
        move.l  d0,csBaseAddr(a2)
        moveq   #0,d0
        rts

; csCode 2 — SetMode: switch pixel depth (page 0 only).
CtlSetMode:
        move.w  csMode(a2),d2
        cmp.w   #$80,d2
        blo.s   CtlModeBad
        cmp.w   #$80+GS_NMODES-1,d2
        bhi.s   CtlModeBad
        tst.w   csPage(a2)
        bne.s   CtlModeBad
        move.w  d2,pvMode(a5)
        bsr     ApplyMode
        bsr     DRBaseAddr
        move.l  d0,csBaseAddr(a2)
        moveq   #0,d0
        rts
CtlModeBad:
        moveq   #controlErr,d0
        rts

; csCode 3 — SetEntries (indexed CLUT only on this personality).
; ColorSpec = {value.w, r.w, g.w, b.w}; csStart = -1 → per-entry value
; addressing.  Components go through the gamma LUT; a raw copy is kept
; for GetEntries.
CtlSetEntries:
        move.w  pvMode(a5),d0           ; indexed-CLUT depths only — direct
        sub.w   #$80,d0                 ; modes must use DirectSetEntries
        cmp.w   #GS_FIRSTDIRECT,d0
        bhs.s   CtlModeBad
CtlSetEntCommon:
        move.l  csTable(a2),d0
        beq.s   CtlModeBad
        dc.w    _StripAddress           ; table is read inside the 32-bit window
        movea.l d0,a1                   ; A1 = ColorSpec cursor
        move.w  csStart(a2),d0
        movea.w d0,a6                   ; A6 = csStart (sign preserved)
        move.w  csCount(a2),d4          ; count-1
        moveq   #0,d5                   ; sequential index
        move.l  a6,d0
        bmi.s   CtlSEStart
        move.w  d0,d5
CtlSEStart:
        bsr     Swap32
        movea.l pvSlotBase(a5),a4
CtlSEEnt:
        move.w  (a1)+,d6                ; value field
        move.l  a6,d0
        bmi.s   CtlSEByVal
        move.w  d5,d0
        bra.s   CtlSEHave
CtlSEByVal:
        move.w  d6,d0
CtlSEHave:
        and.w   #$FF,d0
        moveq   #0,d1
        move.b  (a1),d1                 ; R (high byte of 16-bit component)
        moveq   #0,d2
        move.b  2(a1),d2                ; G
        moveq   #0,d3
        move.b  4(a1),d3                ; B
        addq.w  #6,a1
        btst    #0,pvFlags+1(a5)
        beq.s   CtlSEColor
        move.w  d1,d6                   ; gray mode: luminance ≈ (r+g+b)*85/256
        add.w   d2,d6
        add.w   d3,d6
        mulu    #85,d6
        lsr.w   #8,d6
        move.b  d6,d1
        move.b  d6,d2
        move.b  d6,d3
CtlSEColor:
        lea     pvClut(a5),a2           ; raw copy for GetEntries
        move.b  d1,(a2,d0.w)
        move.b  d2,256(a2,d0.w)
        move.b  d3,512(a2,d0.w)
        lea     pvGamma(a5),a2
        move.b  (a2,d1.w),d1
        move.b  256(a2,d2.w),d2
        move.b  512(a2,d3.w),d3
        bsr     DRClutWrite
        addq.w  #1,d5
        dbf     d4,CtlSEEnt
        bsr     SwapBack
        moveq   #0,d0
        rts

; csCode 8 — DirectSetEntries: the direct-RGB counterpart of SetEntries
; (the gamma CLUT still exists behind direct modes).
CtlDirectSetEnt:
        move.w  pvMode(a5),d0
        sub.w   #$80,d0
        cmp.w   #GS_FIRSTDIRECT,d0
        blo     CtlModeBad              ; indexed modes use SetEntries
        bra     CtlSetEntCommon

; csCode 4 — SetGamma: NIL → identity ramp; else validate + copy the table.
CtlSetGamma:
        move.l  csGTable(a2),d0
        beq.s   CtlSGIdentity
        movea.l d0,a1
        cmp.w   #8,10(a1)               ; gDataWidth
        bne.s   CtlSGBad
        cmp.w   #256,8(a1)              ; gDataCnt
        bne.s   CtlSGBad
        move.w  6(a1),d2                ; gChanCnt
        moveq   #0,d1
        move.w  4(a1),d1                ; gFormulaSize
        lea     12(a1),a3
        adda.l  d1,a3                   ; A3 = gData
        lea     pvGamma(a5),a1
        cmp.w   #3,d2
        beq.s   CtlSGThree
        cmp.w   #1,d2
        bne.s   CtlSGBad
        move.w  #255,d1                 ; one channel: replicate to R/G/B
CtlSGOne:
        move.b  (a3)+,d0
        move.b  d0,(a1)
        move.b  d0,256(a1)
        move.b  d0,512(a1)
        addq.w  #1,a1
        dbf     d1,CtlSGOne
        bra.s   CtlSGStore
CtlSGThree:
        move.w  #767,d1
CtlSGCopy:
        move.b  (a3)+,(a1)+
        dbf     d1,CtlSGCopy
CtlSGStore:
        ; refresh the GetGamma copy (R plane, single channel)
        lea     pvGamma(a5),a1
        lea     pvGTbl+12(a5),a3
        move.w  #255,d1
CtlSGRefresh:
        move.b  (a1)+,(a3)+
        dbf     d1,CtlSGRefresh
        moveq   #0,d0
        rts
CtlSGIdentity:
        bsr     GammaIdentity
        lea     pvGTbl+12(a5),a3
        moveq   #0,d0
CtlSGIdRefresh:
        move.b  d0,(a3)+
        addq.w  #1,d0
        cmp.w   #256,d0
        bne.s   CtlSGIdRefresh
        moveq   #0,d0
        rts
CtlSGBad:
        moveq   #paramErr,d0
        rts

; csCode 5 — GrayScreen.
CtlGrayScreen:
        bsr     GrayFill
        moveq   #0,d0
        rts

; csCode 6 — SetGray: 0 = color, 1 = luminance-equivalent grays.
CtlSetGray:
        tst.b   (a2)
        beq.s   CtlSetGrayOff
        bset    #0,pvFlags+1(a5)
        bra.s   CtlSetGrayDone
CtlSetGrayOff:
        bclr    #0,pvFlags+1(a5)
CtlSetGrayDone:
        moveq   #0,d0
        rts

; csCode 7 — SetInterrupt: 0 = enable VBL, 1 = disable.
CtlSetInterrupt:
        move.b  (a2),d2                 ; read the flag BEFORE entering 32-bit
        bsr     Swap32                  ; mode — (a2) is a caller pointer
        movea.l pvSlotBase(a5),a4
        tst.b   d2
        bne.s   CtlSIDisable
        bsr     DRVblEnable
        bclr    #1,pvFlags+1(a5)
        bra.s   CtlSIDone
CtlSIDisable:
        bsr     DRVblDisable
        bset    #1,pvFlags+1(a5)
CtlSIDone:
        bsr     SwapBack
        moveq   #0,d0
        rts

; csCode 9 — SetDefaultMode: persist the spID into slot PRAM byte 2.
CtlSetDefault:
        move.b  (a2),d2
        move.l  a0,-(sp)
        suba.w  #spBlockSize+8,sp
        movea.l sp,a1                   ; 8-byte sPRAMRec buffer
        lea     8(sp),a3                ; spBlock
        move.l  a1,spResult(a3)
        move.w  pvSlot(a5),d0
        move.b  d0,spSlot(a3)
        clr.b   spExtDev(a3)
        movea.l a3,a0
        moveq   #sReadPRAMRec,d0
        dc.w    _SlotManager
        move.b  d2,2(a1)
        move.l  a1,spsPointer(a3)
        movea.l a3,a0
        moveq   #sPutPRAMRec,d0
        dc.w    _SlotManager
        adda.w  #spBlockSize+8,sp
        movea.l (sp)+,a0
        moveq   #0,d0
        rts

; === Status ==================================================================
DrvStatus:
        movem.l d1-d7/a1-a6,-(sp)
        movea.l dCtlStorage(a1),a5
        move.l  (a5),d0
        dc.w    _StripAddress
        movea.l d0,a5
        move.w  csCode(a0),d0
        cmp.w   #10,d0
        bhi.s   StBad
        cmp.w   #2,d0
        blo.s   StBad
        movea.l csParam(a0),a2
        sub.w   #2,d0
        add.w   d0,d0
        lea     StTab(pc),a1
        move.w  (a1,d0.w),d0
        jsr     (a1,d0.w)
        bra     DrvExit
StBad:
        moveq   #statusErr,d0
        bra     DrvExit

StTab:
        dc.w    StGetMode-StTab         ; 2
        dc.w    StGetEntries-StTab      ; 3
        dc.w    StGetPages-StTab        ; 4
        dc.w    StGetBase-StTab         ; 5
        dc.w    StGetGray-StTab         ; 6
        dc.w    StGetInterrupt-StTab    ; 7
        dc.w    StGetGamma-StTab        ; 8
        dc.w    StGetDefault-StTab      ; 9
        dc.w    StGetCurMode-StTab      ; 10

StGetMode:
        move.w  pvMode(a5),csMode(a2)
        clr.w   csPage(a2)
        bsr     DRBaseAddr
        move.l  d0,csBaseAddr(a2)
        moveq   #0,d0
        rts

; Status 3 — GetEntries: return the raw (pre-gamma) CLUT copy.
StGetEntries:
        move.l  csTable(a2),d0
        beq     CtlModeBad
        dc.w    _StripAddress
        movea.l d0,a1
        move.w  csStart(a2),d0
        movea.w d0,a6
        move.w  csCount(a2),d4
        moveq   #0,d5
        move.l  a6,d0
        bmi.s   StGEEnt
        move.w  d0,d5
StGEEnt:
        move.w  (a1),d6                 ; value field (kept in place)
        move.l  a6,d0
        bmi.s   StGEByVal
        move.w  d5,d0
        bra.s   StGEHave
StGEByVal:
        move.w  d6,d0
StGEHave:
        and.w   #$FF,d0
        addq.w  #2,a1
        lea     pvClut(a5),a2
        moveq   #0,d1
        move.b  (a2,d0.w),d1
        move.w  d1,d2
        lsl.w   #8,d2
        or.w    d1,d2
        move.w  d2,(a1)+                ; R as $rrrr
        moveq   #0,d1
        move.b  256(a2,d0.w),d1
        move.w  d1,d2
        lsl.w   #8,d2
        or.w    d1,d2
        move.w  d2,(a1)+
        moveq   #0,d1
        move.b  512(a2,d0.w),d1
        move.w  d1,d2
        lsl.w   #8,d2
        or.w    d1,d2
        move.w  d2,(a1)+
        addq.w  #1,d5
        dbf     d4,StGEEnt
        moveq   #0,d0
        rts

StGetPages:
        move.w  #1,csPage(a2)
        moveq   #0,d0
        rts

StGetBase:
        tst.w   csPage(a2)
        bne     CtlModeBad
        bsr     DRBaseAddr
        move.l  d0,csBaseAddr(a2)
        moveq   #0,d0
        rts

StGetGray:
        moveq   #0,d0
        btst    #0,pvFlags+1(a5)
        beq.s   StGetGrayOff
        moveq   #1,d0
StGetGrayOff:
        move.b  d0,(a2)
        moveq   #0,d0
        rts

StGetInterrupt:
        moveq   #0,d0
        btst    #1,pvFlags+1(a5)
        beq.s   StGetIntOn
        moveq   #1,d0
StGetIntOn:
        move.b  d0,(a2)
        moveq   #0,d0
        rts

StGetGamma:
        lea     pvGTbl(a5),a1
        move.l  a1,(a2)                 ; csGTable
        moveq   #0,d0
        rts

StGetDefault:
        move.l  a0,-(sp)
        suba.w  #spBlockSize+8,sp
        movea.l sp,a1
        lea     8(sp),a3
        move.l  a1,spResult(a3)
        move.w  pvSlot(a5),d0
        move.b  d0,spSlot(a3)
        clr.b   spExtDev(a3)
        movea.l a3,a0
        moveq   #sReadPRAMRec,d0
        dc.w    _SlotManager
        move.b  2(a1),d0
        adda.w  #spBlockSize+8,sp
        movea.l (sp)+,a0
        move.b  d0,(a2)
        moveq   #0,d0
        rts

; Status 10 — GetCurMode: the real GC driver answers noErr here.
StGetCurMode:
        moveq   #0,d0
        rts

; === Shared helpers ==========================================================

; Program the hardware for the current pvMode (depth + stride + base).
ApplyMode:
        bsr     Swap32
        movea.l pvSlotBase(a5),a4
        move.w  pvMode(a5),d0
        sub.w   #$80,d0                 ; depth code
        move.w  pvWidth(a5),d1          ; monitor width — the op derives
        bsr     DRSetDepth              ; whatever stride encoding it needs
        bsr     SwapBack
        rts

; 50%-gray dither fill of the whole screen at the current depth.
; Row geometry comes from the personality's log2(bpp) table; the fill
; pattern from its per-depth pattern table.
GrayFill:
        move.w  pvMode(a5),d0
        sub.w   #$80,d0
        lea     DRPatTab(pc),a1
        move.w  d0,d1
        lsl.w   #2,d1
        move.l  (a1,d1.w),d1            ; per-depth fill pattern
        moveq   #0,d5                   ; D5 = row-invert mask: indexed depths
        cmp.w   #GS_FIRSTDIRECT,d0      ; dither (invert each row); direct
        bhs.s   GrayNoDither            ; depths fill solid 50% gray
        moveq   #-1,d5
GrayNoDither:
        ifne    GS_ROWLONGS_FIXED
        move.w  #GS_ROWLONGS_FIXED,d2   ; fixed row pitch (e.g. GC: 1024 bytes)
        else
        lea     DRLogBppTab(pc),a1
        move.b  (a1,d0.w),d0            ; shift count = log2(bpp)
        move.w  pvWidth(a5),d2
        lsr.w   #5,d2
        lsl.w   d0,d2                   ; longs per row = (width/32) << log2(bpp)
        endif
        move.w  pvHeight(a5),d3
        bsr     Swap32
        movea.l pvSlotBase(a5),a4
        bsr     DRFbBase                ; A1 = framebuffer base (op-provided)
        subq.w  #1,d3
GrayRow:
        move.w  d2,d4
        subq.w  #1,d4
GrayLong:
        move.l  d1,(a1)+
        dbf     d4,GrayLong
        eor.l   d5,d1                   ; invert between rows (indexed only)
        dbf     d3,GrayRow
        bsr     SwapBack
        rts

; Rebuild the identity gamma LUT.
GammaIdentity:
        lea     pvGamma(a5),a1
        moveq   #0,d0
GammaIdLoop:
        move.b  d0,(a1)
        move.b  d0,256(a1)
        move.b  d0,512(a1)
        addq.w  #1,a1
        addq.w  #1,d0
        cmp.w   #256,d0
        bne.s   GammaIdLoop
        rts

; Enter 32-bit addressing (registers live above the 1 MB minor space);
; the previous mode is returned in D7.  It must stay in a REGISTER, not
; private storage: the VBL ISR swaps modes too, and an ISR firing inside
; a handler's Swap32..SwapBack window would clobber a shared memory slot
; and leave the machine in 32-bit mode on exit (every entry path saves
; D1-D7, so per-context D7 is interrupt-safe by construction).
Swap32:
        move.l  d0,-(sp)
        moveq   #1,d0
        dc.w    _SwapMMUMode
        move.b  d0,d7
        move.l  (sp)+,d0
        rts
SwapBack:
        move.l  d0,-(sp)
        move.b  d7,d0
        dc.w    _SwapMMUMode
        move.l  (sp)+,d0
        rts

; === Slot VBL ISR ============================================================
; A1 = SQParm = private block.  Runs the slot's VBL task queue, acks the
; card interrupt, returns D0=1 (serviced).
DrvVBL:
        movem.l d1-d7/a0-a6,-(sp)
        movea.l a1,a5
        move.w  pvSlot(a5),d0
        movea.l JVBLTask,a0
        jsr     (a0)
        bsr     Swap32
        movea.l pvSlotBase(a5),a4
        bsr     DRVblAck
        bsr     SwapBack
        movem.l (sp)+,d1-d7/a0-a6
        moveq   #1,d0
        rts

; === Personality data + ops (self-contained copy for the heap block) =========
        EmitCPB DR
        EmitOps DR

        sBlockEnd GSDrvrBlk
