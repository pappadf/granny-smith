; SPDX-License-Identifier: MIT
; Copyright (c) pappadf
;
; gsvrom_init.s
; Shared PrimaryInit framework (proposal sec. 3.1): locate personality →
; read sense → prune the sResource directory to the sensed monitor →
; program the 1-bpp boot mode → gray the screen → default slot PRAM.
; Personality specifics come exclusively from the CPB data and ops
; emitted into this block (EmitCPB/EmitOps with the "PI" prefix) — the
; Slot Manager copies the whole sExecBlock to RAM before running it, so
; everything referenced here must be inside the block.
;
; Entry (per docs/core/peripherals/nubus_vrom.md sec. 8): A0 → seBlock;
; only _SlotManager and _SwapMMUMode are guaranteed callable; return
; status in seStatus (0 = success).

        sExecBegin GSPInit,2            ; CPU id 2 = 68020+

GSPrimaryInit:
        movem.l d1-d7/a0-a6,-(sp)
        movea.l a0,a2                   ; A2 = seBlock
        moveq   #0,d5
        move.b  seSlot(a2),d5           ; D5 = slot number
        ; A4 = $Fs000000 slot base (32-bit form)
        move.l  d5,d0
        swap    d0
        lsl.l   #8,d0                   ; slot << 24
        or.l    #$F0000000,d0
        movea.l d0,a4
        ; hardware lives above the 1 MB minor space — enter 32-bit mode
        moveq   #1,d0
        dc.w    _SwapMMUMode
        move.b  d0,d7                   ; D7.b = saved addressing mode

        ; read the monitor sense — the op returns the chosen functional
        ; sResource spID directly (sense strategy is personality-private)
        bsr     PIReadSense
        move.b  d0,d6                   ; D6.b = chosen spID (0 = none)

        ; prune: delete every monitor sResource except the sensed one
        lea     PIMonTab(pc),a3
PIPruneLoop:
        move.w  (a3),d0
        beq.s   PIPruned                ; table terminator
        cmp.b   d0,d6
        beq.s   PIPruneKeep             ; the chosen monitor stays
        suba.w  #spBlockSize,sp
        movea.l sp,a0
        move.b  d5,spSlot(a0)
        move.b  d0,spID(a0)
        clr.b   spExtDev(a0)
        moveq   #sDeleteSRTRec,d0
        dc.w    _SlotManager
        adda.w  #spBlockSize,sp
PIPruneKeep:
        addq.w  #6,a3
        bra.s   PIPruneLoop
PIPruned:
        ; (The 32-bit family GS_DEFER_SPID — when the personality has one —
        ; is left in the SRT untouched: the boot-time Slot Manager follows
        ; the PRAM savedSRsrcID to the boot family, and 32-Bit QuickDraw's
        ; slot-device upgrade pass re-opens the driver on the 32-bit sister
        ; (boot spID + $20) once it loads.)
        tst.b   d6
        beq     PIDone                  ; no monitor: slot goes quietly dark

        ; geometry of the chosen monitor
        lea     PIMonTab(pc),a3
PIFindMon:
        move.w  (a3),d0
        beq     PIDone
        cmp.b   d0,d6
        beq.s   PIFoundMon
        addq.w  #6,a3
        bra.s   PIFindMon
PIFoundMon:
        move.w  2(a3),d2                ; D2 = width
        move.w  4(a3),d3                ; D3 = height

        ; hardware bring-up quirks, then the 1-bpp boot mode
        bsr     PIHwInit
        moveq   #0,d0                   ; depth code 0 = 1 bpp
        move.w  d2,d1                   ; monitor width
        bsr     PISetDepth

        ; gray the screen: alternating $AAAAAAAA / $55555555 rows at 1 bpp
        bsr     PIFbBase                ; A1 = framebuffer base (op-provided)
        movea.l a1,a0
        ifne    GS_ROWLONGS_FIXED
        move.w  #GS_ROWLONGS_FIXED,d0   ; fixed row pitch (e.g. GC: 1024 bytes)
        else
        move.w  d2,d0
        lsr.w   #5,d0                   ; longs per row = width/32 at 1 bpp
        endif
        move.l  #$AAAAAAAA,d1
        move.w  d3,d4
        subq.w  #1,d4
PIFillRow:
        move.w  d0,d3                   ; height no longer needed — reuse
        subq.w  #1,d3
PIFillLong:
        move.l  d1,(a0)+
        dbf     d3,PIFillLong
        not.l   d1
        dbf     d4,PIFillRow

        ; slot PRAM: default savedMode/sister when unset or for another
        ; monitor.  (An emulator- or Monitors-seeded record that names the
        ; chosen monitor is left untouched.)
        suba.w  #spBlockSize+8,sp
        movea.l sp,a1                   ; A1 = 8-byte sPRAMRec buffer
        lea     8(sp),a3                ; A3 = spBlock
        move.l  a1,spResult(a3)
        move.b  d5,spSlot(a3)
        clr.b   spExtDev(a3)
        movea.l a3,a0
        moveq   #sReadPRAMRec,d0
        dc.w    _SlotManager
        move.b  2(a1),d0                ; savedMode: default only when it is
        cmp.b   #$80,d0                 ; outside the mode-list range — a
        blo.s   PIFixMode               ; VALID staged depth must survive a
        cmp.b   #$80+GS_NMODES-1,d0     ; sister-byte fix-up
        bls.s   PIModeOk
PIFixMode:
        move.b  #$80,2(a1)              ; boot depth: 1 bpp
PIModeOk:
        cmp.b   3(a1),d6                ; sister must name the chosen monitor
        beq.s   PIPramOk
        move.b  d6,3(a1)                ; savedSRsrcID
        move.b  d6,4(a1)                ; savedRawSRsrcID
        move.l  a1,spsPointer(a3)
        movea.l a3,a0
        moveq   #sPutPRAMRec,d0
        dc.w    _SlotManager
PIPramOk:
        adda.w  #spBlockSize+8,sp

PIDone:
        move.b  d7,d0                   ; restore addressing mode
        dc.w    _SwapMMUMode
        clr.w   seStatus(a2)            ; success
        movem.l (sp)+,d1-d7/a0-a6
        rts

        EmitCPB PI
        EmitOps PI

        sExecEnd GSPInit

        ifne    GS_DEFER_SPID
; =============================================================================
; SecondaryInit — family swap (nubus_vrom.md sec. 9).  Runs under Slot
; Manager v1+ with the Toolbox alive.  When 32-bit QuickDraw is present,
; swap the 24-bit boot family (GS_BOOT_SPID) for the 32-bit one
; (GS_DEFER_SPID) whose framebuffer lives in super-slot DRAM, and — if
; this card drives the boot screen — re-point the live GDevice PixMap,
; the DCE dCtlDevBase, the driver's cached base, and ScrnBase.
; =============================================================================
        sExecBegin GSSInit,2

GSSecondaryInit:
        movem.l d1-d7/a0-a6,-(sp)
        movea.l a0,a2                   ; A2 = seBlock
        moveq   #0,d5
        move.b  seSlot(a2),d5

        ; 32-bit QuickDraw present?  (gestaltQuickdrawVersion >= $0200)
        move.l  #$71642020,d0           ; 'qd  '
        dc.w    _Gestalt
        tst.w   d0
        bne     SIDone                  ; no Gestalt answer — stay 24-bit
        move.l  a0,d0                   ; response
        cmp.l   #$0200,d0
        blo     SIDone                  ; classic QD only — stay 24-bit

        ; swap the SRT families: delete the boot one, insert the 32-bit one
        suba.w  #spBlockSize,sp
        movea.l sp,a3
        move.b  d5,spSlot(a3)
        move.b  #GS_BOOT_SPID,spID(a3)
        clr.b   spExtDev(a3)
        movea.l a3,a0
        moveq   #sDeleteSRTRec,d0
        dc.w    _SlotManager
        move.b  d5,spSlot(a3)
        move.b  #GS_DEFER_SPID,spID(a3)
        clr.b   spExtDev(a3)
        movea.l a3,a0
        moveq   #sInsertSRTRec,d0
        dc.w    _SlotManager

        ; our driver's refNum (negative iff open) via sRsrcInfo
        move.b  d5,spSlot(a3)
        move.b  #GS_DEFER_SPID,spID(a3)
        clr.b   spExtDev(a3)
        movea.l a3,a0
        moveq   #sRsrcInfo,d0
        dc.w    _SlotManager
        move.w  spRefNum(a3),d6
        adda.w  #spBlockSize,sp

        ; D4 = new framebuffer base: super slot base + DRAM offset
        move.l  d5,d0
        swap    d0
        lsl.l   #8,d0                   ; slot << 24
        lsl.l   #4,d0                   ; slot << 28
        add.l   #GREG_FB_SUPER,d0
        move.l  d0,d4

        ; patch the DCE + the driver's cached base
        tst.w   d6
        bge.s   SIScreen                ; not open — nothing to patch
        move.w  d6,d0
        not.w   d0                      ; unit = -refNum-1
        lsl.w   #2,d0
        movea.l UTableBase,a0
        movea.l (a0,d0.w),a0            ; DCE handle
        move.l  (a0),d0
        dc.w    _StripAddress
        movea.l d0,a1                   ; DCE
        move.l  d4,dCtlDevBase(a1)
        move.l  dCtlStorage(a1),d0
        beq.s   SIScreen
        movea.l d0,a0
        move.l  (a0),d0
        dc.w    _StripAddress
        movea.l d0,a0
        move.l  d4,(a0)                 ; driver private pvBase (offset 0)

SIScreen:
        ; boot screen?  MainDevice's gdRefNum names its driving driver.
        movea.l MainDevice,a0
        move.l  a0,d0
        beq.s   SIDone
        move.l  (a0),d0
        dc.w    _StripAddress
        movea.l d0,a1                   ; GDevice
        cmp.w   gdRefNum(a1),d6
        bne.s   SIDone
        movea.l gdPMap(a1),a0           ; PixMap handle
        move.l  (a0),d0
        dc.w    _StripAddress
        movea.l d0,a1
        move.l  d4,pmBaseAddr(a1)
        move.l  d4,ScrnBase

SIDone:
        clr.w   seStatus(a2)            ; success
        movem.l (sp)+,d1-d7/a0-a6
        rts

        sExecEnd GSSInit
        endif
