; SPDX-License-Identifier: MIT
; Copyright (c) pappadf
;
; ops_mdcgc.s
; 8•24 GC ("Dolphin") personality: CPB equates + data/ops macros.
;
; The GC straddles two address spaces: the JMFB-compatible display block
; in STANDARD slot space ($Fs200000 — CLUT/VBL registers, same as the
; JMFB personality) and the accelerator/framebuffer in SUPER slot space
; ($s0000000 — DRAM, RISC heartbeat).  The visible framebuffer is card
; DRAM at super+$C011400 with a FIXED 1024-byte row pitch at every
; indexed depth (the HLE surfaces exactly that; display_card_824gc.c
; set_poweron_defaults).  Depth selection is NOT register-programmed:
; the card boots at the PRAM-seeded depth and runtime switches ride the
; GC-OS VidComm channel — so SetDepth only performs the bounded cardSync
; heartbeat wait the real driver does around mode programming (sec. 7.3).

; --- CPB equates -------------------------------------------------------------
GS_DRHW         equ     $001D           ; GC DrHW (Display_Video_Apple_MDCGC)
GS_FB_MINOR     equ     $A00            ; 24-bit boot family: std-slot VRAM base
GREG_FB_OFFSET  equ     $11400          ; framebuffer offset within card DRAM
GS_NMODES       equ     4               ; $80..$83 = 1/2/4/8 bpp boot depths
GS_FIRSTDIRECT  equ     8               ; direct modes arrive via VidComm only
GS_ROWLONGS_FIXED equ   256             ; fixed 1024-byte row pitch (256 longs)
GS_BOOT_SPID    equ     $80             ; 24-bit boot family (std-slot VRAM)
GS_DEFER_SPID   equ     $A0             ; 32-bit family (super-slot DRAM) that
                                        ; 32-Bit QuickDraw re-opens the driver
                                        ; on (or SecondaryInit swaps in)

; Super-slot framebuffer: card DRAM base + $11400 (GC824_FB_OFFSET).
GREG_FB_SUPER   equ     $C011400

GSVidNameStr    macro
                cString GSVidName,"Display_Video_Apple_MDCGC"
                endm
GSDrvrName      macro
                dc.b    26,".Display_Video_Apple_MDCGC"
                endm

; Standard-slot display registers (JMFB-compatible block).
GREG_CSR        equ     $200000         ; sense in bits 9-11
GREG_SWIC       equ     $20013C         ; SWICReg: bit1 = VBL mask
GREG_SWCLRVINT  equ     $200148         ; clear pending VBL IRQ
GREG_CLUTADDR   equ     $200200         ; palette index
GREG_CLUTDATA   equ     $200204         ; palette data (3 long writes)

; Super-slot registers (offsets from $s0000000).
GREG_SYNC_HB    equ     $4C00000        ; RISC video heartbeat: bit 31 toggles

; --- CPB data (EmitCPB <pfx>) ------------------------------------------------
; One monitor in stage 2: config 0 = 640×480 (the verified HLE config; the
; 16" config-1 variant runs through VidComm and is a stage-4 follow-up).
EmitCPB         macro
\1MonTab:
                dc.w    $0080,640,480   ; 13"/config 0 (sense $6, spID $80)
                dc.w    0
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
EmitOps         macro

; \1HwInit: one-shot hardware bring-up quirks at PrimaryInit time.
\1HwInit:
                rts

; \1Super: out D0.L = super-slot base ($s0000000) derived from A4.
\1Super:
                move.l  a4,d0
                lsl.l   #4,d0           ; $Fs000000 → $s0000000
                rts

; \1FbBase: out A1 = visible framebuffer (super-slot DRAM + $11400).
\1FbBase:
                move.l  d0,-(sp)
                bsr     \1Super
                movea.l d0,a1
                lea     GREG_FB_SUPER(a1),a1
                move.l  (sp)+,d0
                rts

; \1CardSync: BOUNDED wait for one full heartbeat cycle (bit 31 of
; super+$4C00000: 1→0→1).  The real declROM driver's cardSync is an
; UNBOUNDED loop — the documented System 6.0.8 hang class (sec. 7.3);
; this one gives up after 256 reads per edge.
\1CardSync:
                movem.l d0-d2/a0,-(sp)
                bsr     \1Super
                movea.l d0,a0
                move.w  #255,d2
\1SyncHigh:                             ; wait for bit 31 = 1
                move.l  GREG_SYNC_HB(a0),d1
                bmi.s   \1SyncFall
                dbf     d2,\1SyncHigh
                bra.s   \1SyncOut       ; give up — never hang the boot
\1SyncFall:
                move.w  #255,d2
\1SyncLow:                              ; wait for the falling edge
                move.l  GREG_SYNC_HB(a0),d1
                bpl.s   \1SyncRise
                dbf     d2,\1SyncLow
                bra.s   \1SyncOut
\1SyncRise:
                move.w  #255,d2
\1SyncBack:                             ; and the next rising edge
                move.l  GREG_SYNC_HB(a0),d1
                bmi.s   \1SyncOut
                dbf     d2,\1SyncBack
\1SyncOut:
                movem.l (sp)+,d0-d2/a0
                rts

; \1BaseAddr: out D0.L = csBaseAddr (A5 = private storage).  The GC's
; visible framebuffer lives in super-slot DRAM, which classic 24-bit
; QuickDraw cannot address — answer the 24-bit boot-family base until
; 32-Bit QuickDraw is loaded, the super-slot DRAM framebuffer after.
; (Known stage-2 gap, sec. 7.2: the OS's gDevice rebuild reads the
; DEVICE base + vpBaseOffset rather than asking the driver, so on
; System 6.0.8 the Finder still paints into std-slot VRAM; the
; accelerator bring-up itself is unaffected.)
\1BaseAddr:
                movem.l d1/a0-a1,-(sp)
                move.w  #TrapNumGestalt,d0
                dc.w    _GetOSTrapAddress
                move.l  a0,d1
                move.w  #TrapNumUnimpl,d0
                dc.w    _GetOSTrapAddress
                cmp.l   a0,d1
                beq.s   \1Base24        ; no Gestalt yet (ROM boot)
                move.l  #$71642020,d0   ; gestaltQuickdrawVersion 'qd  '
                dc.w    _Gestalt
                tst.w   d0
                bne.s   \1Base24
                move.l  a0,d0
                cmp.l   #$0200,d0
                blo.s   \1Base24        ; classic QuickDraw only
                moveq   #0,d0           ; 32-bit QD: super-slot DRAM base
                move.w  pvSlot(a5),d0
                swap    d0
                lsl.l   #8,d0
                lsl.l   #4,d0           ; slot << 28
                add.l   #$C000000+GREG_FB_OFFSET,d0
                bra.s   \1BaseDone
\1Base24:
                move.l  pvBase(a5),d0
\1BaseDone:
                movem.l (sp)+,d1/a0-a1
                rts

; \1ReadSense: out D0.W = functional spID.  Both GC monitors answer
; primary sense 6; stage 2 ships the single config-0 sResource, so any
; sensed monitor selects it (no-monitor still returns $80 — the GC is
; usable headless through its accelerator).
\1ReadSense:
                move.w  #GS_BOOT_SPID,d0
                rts

; \1SetDepth: depth is not register-programmed on the GC (the HLE boots
; at the PRAM-seeded depth; runtime switches ride VidComm).  Perform the
; bounded heartbeat wait the real driver does around mode changes.
\1SetDepth:
                bsr     \1CardSync
                rts

; \1ClutWrite: JMFB-block RAMDAC path (index + three long writes).
\1ClutWrite:
                movem.l d0-d4,-(sp)
                and.l   #$FF,d0
                move.l  d0,GREG_CLUTADDR(a4)
                moveq   #0,d4
                move.b  d1,d4
                move.l  d4,GREG_CLUTDATA(a4)
                moveq   #0,d4
                move.b  d2,d4
                move.l  d4,GREG_CLUTDATA(a4)
                moveq   #0,d4
                move.b  d3,d4
                move.l  d4,GREG_CLUTDATA(a4)
                movem.l (sp)+,d0-d4
                rts

; \1VblEnable / \1VblDisable / \1VblAck: Stopwatch block, same as JMFB.
\1VblEnable:
                move.l  d0,-(sp)
                moveq   #$5,d0
                move.l  d0,GREG_SWIC(a4)
                move.l  (sp)+,d0
                rts
\1VblDisable:
                move.l  d0,-(sp)
                moveq   #$7,d0
                move.l  d0,GREG_SWIC(a4)
                move.l  (sp)+,d0
                rts
\1VblAck:
                move.l  d0,-(sp)
                moveq   #1,d0
                move.l  d0,GREG_SWCLRVINT(a4)
                move.l  (sp)+,d0
                rts
                endm
