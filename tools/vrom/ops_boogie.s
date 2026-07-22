; SPDX-License-Identifier: MIT
; Copyright (c) pappadf
;
; ops_boogie.s
; Display Card 24AC ("Boogie") personality: CPB equates + data/ops macros.
; Register model per display_card_24ac.h — byte-wide lane-3 registers at
; full slot-relative offsets; extended monitor sense on SENSE_CLK; an
; active-low RAMDAC; depth in MODE_REG bits 7-5; no stride register.

; --- CPB equates -------------------------------------------------------------
GS_DRHW         equ     $002B           ; 24AC DrHW (Display_Video_Apple_Boogie)
GS_FB_MINOR     equ     0               ; framebuffer at VRAM offset 0
GS_NMODES       equ     5               ; $80..$84 = 1/4/8/16/32 bpp (no 2 bpp)
GS_FIRSTDIRECT  equ     3               ; codes 3 (16 bpp) and 4 (32 bpp) are direct
GS_ROWLONGS_FIXED equ   0               ; 0 = tight stride (width*bpp/32 longs/row)
GS_DEFER_SPID   equ     0               ; no deferred 32-bit sResource family

GSVidNameStr    macro
                cString GSVidName,"Display_Video_Apple_Boogie"
                endm
GSDrvrName      macro
                dc.b    27,".Display_Video_Apple_Boogie"
                endm

; Register offsets from the $Fs000000 slot base (byte registers, lane 3).
BREG_STATUS     equ     $D00402         ; R: depth code + busy toggle
BREG_VIDCTL     equ     $D00403         ; R/W: bit 7 = VBL mask; low bits stay 2
BREG_MODE       equ     $D80001         ; R/W: depth in bits 7-5
BREG_SENSE      equ     $D8000D         ; monitor sense drive/read
BREG_CLUT_ADDR  equ     $C8001E         ; runtime CLUT index (active-low)
BREG_CLUT_DATA  equ     $C8001A         ; runtime CLUT data  (active-low)

; --- CPB data (EmitCPB <pfx>) ------------------------------------------------
; All three multisync monitors answer primary sense 6; the extended sense
; code distinguishes them.  spIDs reproduce the real ROM's sister scheme
; ($6B/$6C/$6D) so emulator PRAM/mode staging matches.
EmitCPB         macro
\1MonTab:
                dc.w    $006B,640,480   ; 640×480@67  (ext sense $03)
                dc.w    $006C,800,600   ; 800×600@60  (ext sense $0B)
                dc.w    $006D,832,624   ; 832×624@75  (ext sense $23)
                dc.w    0
; Extended-sense code → spID rows (byte pairs), 0-terminated.
\1ExtMap:
                dc.b    $03,$6B
                dc.b    $0B,$6C
                dc.b    $23,$6D
                dc.b    0,0
; Depth-code tables, indexed by (csMode - $80).
\1LogBppTab:
                dc.b    0,2,3,4,5       ; 1/4/8/16/32 bpp
                dc.b    0
                even
\1PatTab:
                dc.l    $AAAAAAAA       ; 1 bpp checker
                dc.l    $F0F0F0F0       ; 4 bpp
                dc.l    $FF00FF00       ; 8 bpp
                dc.l    $42104210       ; 16 bpp: solid 50% gray (direct)
                dc.l    $00808080       ; 32 bpp: solid 50% gray (direct)
; MODE_REG depth ladder (bits 7-5), indexed by depth code.
\1ModeTab:
                dc.b    $00,$20,$40,$A0,$C0
                even
                endm

; --- Ops (EmitOps <pfx>) -----------------------------------------------------
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

; \1ReadSense: out D0.W = functional sResource spID (0 = none).
; Primary probe (all lines released) must answer code 6 (multisync);
; then the three-round extended probe assembles the 6-bit code the
; ExtMap rows translate.  Read-back bit CLEAR ⇒ ext bit 1 (active-low).
\1ReadSense:
                movem.l d1-d3/a0,-(sp)
                clr.b   BREG_SENSE(a4)  ; release all lines (primary probe)
                move.b  BREG_SENSE(a4),d0
                not.b   d0
                lsr.b   #5,d0
                and.w   #7,d0
                cmp.w   #6,d0
                bne     \1SenseNone     ; not a multisync monitor: no display
                moveq   #0,d1           ; D1 = assembled ext code
                move.b  #$80,BREG_SENSE(a4) ; drive line A
                move.b  BREG_SENSE(a4),d0
                btst    #6,d0
                bne.s   \1SenseA1
                bset    #5,d1
\1SenseA1:      btst    #5,d0
                bne.s   \1SenseA2
                bset    #4,d1
\1SenseA2:      move.b  #$40,BREG_SENSE(a4) ; drive line B
                move.b  BREG_SENSE(a4),d0
                btst    #7,d0
                bne.s   \1SenseB1
                bset    #3,d1
\1SenseB1:      btst    #5,d0
                bne.s   \1SenseB2
                bset    #2,d1
\1SenseB2:      move.b  #$20,BREG_SENSE(a4) ; drive line C
                move.b  BREG_SENSE(a4),d0
                btst    #7,d0
                bne.s   \1SenseC1
                bset    #1,d1
\1SenseC1:      btst    #6,d0
                bne.s   \1SenseC2
                bset    #0,d1
\1SenseC2:      clr.b   BREG_SENSE(a4)  ; release the lines again
                lea     \1ExtMap(pc),a0
\1SenseLook:
                move.b  (a0)+,d2        ; ext code of this row
                beq.s   \1SenseNone     ; table end: unknown monitor
                move.b  (a0)+,d3        ; spID
                cmp.b   d1,d2
                bne.s   \1SenseLook
                moveq   #0,d0
                move.b  d3,d0
                movem.l (sp)+,d1-d3/a0
                rts
\1SenseNone:
                moveq   #0,d0
                movem.l (sp)+,d1-d3/a0
                rts

; \1SetDepth: in D0.W = depth code 0..4, D1.W = width (unused — the card
; derives its stride from the mode).  Programs MODE_REG bits 7-5.
\1SetDepth:
                movem.l d0/a0,-(sp)
                lea     \1ModeTab(pc),a0
                move.b  (a0,d0.w),d0
                move.b  d0,BREG_MODE(a4)
                movem.l (sp)+,d0/a0
                rts

; \1ClutWrite: in D0.W = index, D1/D2/D3.B = R/G/B.  The 24AC RAMDAC is
; ACTIVE-LOW: the genuine driver NOT.Bs the index and every component,
; and the HLE undoes exactly that (display_card_24ac.c clut_set_index).
\1ClutWrite:
                movem.l d0-d3,-(sp)
                not.b   d0
                move.b  d0,BREG_CLUT_ADDR(a4)
                not.b   d1
                move.b  d1,BREG_CLUT_DATA(a4)
                not.b   d2
                move.b  d2,BREG_CLUT_DATA(a4)
                not.b   d3
                move.b  d3,BREG_CLUT_DATA(a4)
                movem.l (sp)+,d0-d3
                rts

; \1VblEnable / \1VblDisable / \1VblAck: VIDCTL bit 7 is the VBL mask;
; the ISR acks by pulsing it high then low (edge-triggered re-arm).
; The low bits stay 2 — the driver-open gate value.
\1VblEnable:
                move.b  #$02,BREG_VIDCTL(a4)
                rts
\1VblDisable:
                move.b  #$82,BREG_VIDCTL(a4)
                rts
\1VblAck:
                move.b  #$82,BREG_VIDCTL(a4)
                move.b  #$02,BREG_VIDCTL(a4)
                rts
                endm
