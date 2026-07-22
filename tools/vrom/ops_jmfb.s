; SPDX-License-Identifier: MIT
; Copyright (c) pappadf
;
; ops_jmfb.s
; JMFB (8•24) personality: Card Parameter Block equates + data/ops macros.
;
; The PrimaryInit sExecBlock and the DRVR are each copied to RAM before
; execution, so any data or leaf routine they use must live INSIDE that
; block.  The CPB data and card ops are therefore emitted per-block via
; macros taking a label prefix (\1) — the personality stays one source,
; instantiated wherever a self-contained copy is needed (proposal sec. 3.1).

; --- CPB equates -------------------------------------------------------------
GS_DRHW         equ     $0019           ; JMFB DrHW (Display_Video_Apple_MDC)
GS_FB_MINOR     equ     $A00            ; framebuffer offset in standard slot space
GS_NMODES       equ     4               ; mode-list entries $80..$83 (1/2/4/8 bpp)
GS_FIRSTDIRECT  equ     8               ; no direct-RGB depth codes on this card
GS_ROWLONGS_FIXED equ   0               ; 0 = tight stride (width*bpp/32 longs/row)
GS_DEFER_SPID   equ     0               ; no deferred 32-bit sResource family

; Functional sResource name — Mac-side software matches on this exact
; string (proposal sec. 7.1); the DRVR name is "." + it.
GSVidNameStr    macro
                cString GSVidName,"Display_Video_Apple_MDC"
                endm
GSDrvrName      macro
                dc.b    24,".Display_Video_Apple_MDC"
                endm

; Register offsets from the $Fs000000 slot base (16-bit registers occupy
; the LOW half of a longword cell — write the value as a long, or as a
; word at offset+2; see jmfb.h and the HLE convention).
JREG_CSR        equ     $200000         ; control/status; sense in bits 9-11
JREG_VIDBASE    equ     $200008         ; framebuffer base, units of 32 bytes
JREG_ROWWORDS   equ     $20000C         ; stride, units of 4 bytes (indexed depths)
JREG_SWIC       equ     $20013C         ; Stopwatch SWICReg: bit1 = VBL mask
JREG_SWCLRVINT  equ     $200148         ; write anything: clear pending VBL IRQ
JREG_CLUTADDR   equ     $200200         ; RAMDAC palette index
JREG_CLUTDATA   equ     $200204         ; RAMDAC palette data (3 long writes: R,G,B)
JREG_PBCR       equ     $200208         ; pixel bus control: depth code in bits 3-4

; --- CPB data (EmitCPB <pfx>) ------------------------------------------------
; Monitor table rows: spID.w, width.w, height.w — spIDs reproduce the real
; ROM's ACTIVE Ax sister scheme so emulator PRAM seeding matches (sec. 3.3).
EmitCPB         macro
\1MonTab:
                dc.w    $00A6,640,480   ; 13" AppleColor  (sense $6)
                dc.w    $00A2,512,384   ; 12" RGB         (sense $2)
                dc.w    $00A1,640,870   ; 15" Portrait    (sense $1)
                dc.w    $00A7,1152,870  ; 21" RGB         (sense $0)
                dc.w    0               ; terminator
; Raw 3-bit sense → functional sResource spID (0 = no usable monitor).
; Mapping mirrors jmfb.c::monitor_for_sense (JMFBPrimaryInit.a's table).
\1SenseMap:
                dc.b    $A7             ; 000 RGB Workstation (Kong 21")
                dc.b    $A1             ; 001 B&W Portrait
                dc.b    $A2             ; 010 12" RGB (Rubik)
                dc.b    $A7             ; 011 B&W Workstation (Kong dims)
                dc.b    $A6             ; 100 NTSC — approximate as 13"
                dc.b    $A1             ; 101 RGB Portrait
                dc.b    $A6             ; 110 Standard RGB 13"
                dc.b    0               ; 111 no connect / extended sense
; Depth-code tables, indexed by (csMode - $80): log2(bpp) shift counts
; and the 50%-gray fill pattern per depth.
\1LogBppTab:
                dc.b    0,1,2,3         ; 1/2/4/8 bpp
                even
\1PatTab:
                dc.l    $AAAAAAAA       ; 1 bpp checker
                dc.l    $CCCCCCCC       ; 2 bpp
                dc.l    $F0F0F0F0       ; 4 bpp
                dc.l    $FF00FF00       ; 8 bpp
                endm

; --- Ops (EmitOps <pfx>) -----------------------------------------------------
; Register conventions shared by every personality's ops:
;   A4 = $Fs000000 slot base, valid in 32-bit addressing mode only.
;   Each op preserves all registers except those documented as outputs.
EmitOps         macro

; \1HwInit: one-shot hardware bring-up quirks at PrimaryInit time.
\1HwInit:
                rts

; \1FbBase: out A1 = framebuffer base address (in from A4 = slot base).
\1FbBase:
                lea     GS_FB_MINOR(a4),a1
                rts

; \1BaseAddr: out D0.L = the csBaseAddr the driver reports (A5 = private
; storage; runs in the CALLER's addressing mode — no hardware access).
\1BaseAddr:
                move.l  pvBase(a5),d0
                rts

; \1ReadSense: out D0.W = chosen functional sResource spID (0 = no usable
; monitor).  The whole sense strategy is personality-private (proposal
; sec. 3.1): the JMFB reads the 3-bit sense field in CSR bits 9-11 and
; maps it through the CPB sense table.
\1ReadSense:
                move.l  JREG_CSR(a4),d0
                lsr.l   #8,d0
                lsr.l   #1,d0           ; sense lives in bits 9-11
                and.w   #7,d0
                lea     \1SenseMap(pc),a0
                moveq   #0,d1
                move.b  (a0,d0.w),d1
                move.w  d1,d0
                rts

; \1SetDepth: in D0.W = depth code 0..3 (1<<code bpp), D1.W = monitor
; width.  Programs depth + stride + framebuffer base.
\1SetDepth:
                movem.l d0-d2,-(sp)
                move.w  d0,d2
                lsl.w   #3,d2
                or.w    #$80,d2         ; PBCR = $80 | code<<3 (matches the
                                        ; real driver's observable values)
                lsr.w   #5,d1
                lsl.w   d0,d1           ; RowWords = (width/32) << log2(bpp)
                moveq   #0,d0
                move.w  d2,d0
                move.l  d0,JREG_PBCR(a4)
                moveq   #0,d0
                move.w  d1,d0
                move.l  d0,JREG_ROWWORDS(a4)
                move.l  #GS_FB_MINOR/32,d0
                move.l  d0,JREG_VIDBASE(a4) ; base fixed at slot+$A00
                movem.l (sp)+,d0-d2
                rts

; \1ClutWrite: in D0.W = palette index, D1.B/D2.B/D3.B = R/G/B.
; Index write resets the RAMDAC phase; three long writes carry one
; component each in the low byte, then the index auto-increments.
\1ClutWrite:
                movem.l d0-d4,-(sp)
                and.l   #$FF,d0
                move.l  d0,JREG_CLUTADDR(a4)
                moveq   #0,d4
                move.b  d1,d4
                move.l  d4,JREG_CLUTDATA(a4)
                moveq   #0,d4
                move.b  d2,d4
                move.l  d4,JREG_CLUTDATA(a4)
                moveq   #0,d4
                move.b  d3,d4
                move.l  d4,JREG_CLUTDATA(a4)
                movem.l (sp)+,d0-d4
                rts

; \1VblEnable / \1VblDisable: SWICReg bit 1 is an active-high VBL mask.
; The real driver writes $5 / $7; the HLE keys on bit 1 only.
\1VblEnable:
                move.l  d0,-(sp)
                moveq   #$5,d0
                move.l  d0,JREG_SWIC(a4)
                move.l  (sp)+,d0
                rts
\1VblDisable:
                move.l  d0,-(sp)
                moveq   #$7,d0
                move.l  d0,JREG_SWIC(a4)
                move.l  (sp)+,d0
                rts

; \1VblAck: any write clears the pending VBL interrupt.
\1VblAck:
                move.l  d0,-(sp)
                moveq   #1,d0
                move.l  d0,JREG_SWCLRVINT(a4)
                move.l  (sp)+,d0
                rts
                endm
