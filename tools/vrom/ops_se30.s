; SPDX-License-Identifier: MIT
; Copyright (c) pappadf
;
; ops_se30.s
; SE/30 built-in video personality: CPB equates + data/ops macros.
; The "card" is a bare framebuffer — no registers at all (fixed 512×342
; at 1 bpp, VRAM at slot $E standard space + $E00000, framebuffer at
; +$8040) — so nearly every op is a no-op.  The device base is 0 with
; vpBaseOffset $8040, matching the real onboard vROM's records (ScrnBase
; = $FEE00000 + $8040).

; --- CPB equates -------------------------------------------------------------
GS_DRHW         equ     $0009           ; SE/30 built-in video DrHW
GS_FB_MINOR     equ     0               ; device base 0; offset rides vpBaseOffset
GS_NMODES       equ     1               ; 1 bpp only
GS_FIRSTDIRECT  equ     8               ; no direct modes
GS_ROWLONGS_FIXED equ   0               ; tight stride (64 bytes at 512 px)
GS_DEFER_SPID   equ     0               ; no deferred family

SE30_FB_OFFSET  equ     $E08040         ; framebuffer from the $FE000000 base

GSVidNameStr    macro
                cString GSVidName,"Display_Video_Apple_SE30"
                endm
GSDrvrName      macro
                dc.b    25,".Display_Video_Apple_SE30"
                endm

; --- CPB data (EmitCPB <pfx>) ------------------------------------------------
EmitCPB         macro
\1MonTab:
                dc.w    $0080,512,342   ; built-in 9" CRT (spID $80)
                dc.w    0
\1LogBppTab:
                dc.b    0               ; 1 bpp
                dc.b    0
                even
\1PatTab:
                dc.l    $AAAAAAAA       ; 1 bpp checker
                endm

; --- Ops (EmitOps <pfx>) -----------------------------------------------------
EmitOps         macro

; \1HwInit: select the PRIMARY video buffer by driving VIA1 PA6 high
; (DDR + output register + the shadow cells the real onboard vROM's
; PrimaryInit pokes) — without this the machine displays the empty
; alternate buffer.  VIA1 base from the low-memory global $1D4.
\1HwInit:
                move.l  a0,-(sp)
                movea.l $1D4.w,a0
                bset    #6,$600(a0)
                bset    #6,$400(a0)
                bset    #6,$1E00(a0)
                bset    #6,(a0)
                movea.l (sp)+,a0
                rts

; \1FbBase: out A1 = framebuffer base (VRAM + primary-buffer offset).
\1FbBase:
                lea     SE30_FB_OFFSET(a4),a1
                rts

; \1BaseAddr: out D0.L = csBaseAddr = device base + vpBaseOffset.
\1BaseAddr:
                move.l  pvBase(a5),d0
                add.l   #$8040,d0
                rts

; \1ReadSense: the built-in CRT is always connected.
\1ReadSense:
                move.w  #$80,d0
                rts

; \1SetDepth / \1ClutWrite / VBL ops: no hardware to program — the
; framebuffer is fixed 1-bpp and the VBL rides the machine's VIA, not a
; slot interrupt.
\1SetDepth:
                rts
\1ClutWrite:
                rts
\1VblEnable:
                rts
\1VblDisable:
                rts
\1VblAck:
                rts
                endm
