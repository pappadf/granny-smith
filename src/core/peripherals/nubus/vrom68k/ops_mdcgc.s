| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| ops_mdcgc.s
| 8*24 GC ("Dolphin") personality: CPB equates + data/ops macros.
|
| The GC straddles two address spaces: the JMFB-compatible display block
| in STANDARD slot space (0xFs200000 — CLUT/VBL registers, same as the
| JMFB personality) and the accelerator/framebuffer in SUPER slot space
| (0xs0000000 — DRAM, RISC heartbeat).  The visible framebuffer is card
| DRAM at super+0xC011400 with a FIXED 1024-byte row pitch at every
| indexed depth (the HLE surfaces exactly that; display_card_824gc.c
| set_poweron_defaults).  Depth selection is NOT register-programmed:
| the card boots at the PRAM-seeded depth and runtime switches ride the
| GC-OS VidComm channel — so SetDepth only performs the bounded cardSync
| heartbeat wait the real driver does around mode programming (sec. 7.3).

| --- CPB equates -------------------------------------------------------------
.equ GS_DRHW,          0x001D          | GC DrHW (Display_Video_Apple_MDCGC)
.equ GS_FB_MINOR,      0xA00           | 24-bit boot family: std-slot VRAM base
.equ GREG_FB_OFFSET,   0x11400         | framebuffer offset within card DRAM
.equ GS_NMODES,        4               | 0x80..0x83 = 1/2/4/8 bpp boot depths
.equ GS_FIRSTDIRECT,   8               | direct modes arrive via VidComm only
.equ GS_BOOT_SPID,     0x80            | 24-bit boot family (std-slot VRAM)
.equ GS_DEFER_SPID,    0xA0            | 32-bit family (super-slot DRAM) that
                                        | 32-Bit QuickDraw re-opens the driver
                                        | on (or SecondaryInit swaps in)

| Super-slot framebuffer: card DRAM base + 0x11400 (GC824_FB_OFFSET).
.equ GREG_FB_SUPER,    0xC011400

	.macro	GSDrvrName
	dc.b	26
	.ascii	".Display_Video_Apple_MDCGC"
	.endm

| Standard-slot display registers (JMFB-compatible block).
.equ GREG_CSR,         0x200000        | sense in bits 9-11
.equ GREG_SWIC,        0x20013C        | SWICReg: bit1 = VBL mask
.equ GREG_SWCLRVINT,   0x200148        | clear pending VBL IRQ
.equ GREG_CLUTADDR,    0x200200        | palette index
.equ GREG_CLUTDATA,    0x200204        | palette data (3 long writes)

| Super-slot registers (offsets from 0xs0000000).
.equ GREG_SYNC_HB,     0x4C00000       | RISC video heartbeat: bit 31 toggles

| --- CPB data (EmitCPB <pfx>) ------------------------------------------------
| One monitor in stage 2: config 0 = 640x480 (the verified HLE config; the
| 16" config-1 variant runs through VidComm and is a stage-4 follow-up).
	.macro	EmitCPB pfx
| Top-level video spID: the 24-bit boot family (the 0xA0 32-bit sister
| is kept by GS_DEFER_SPID, not listed here); geometry lives only in
| the generated records (§3.4).
\pfx&SpidTab:
	dc.w	0x0080
	dc.w	0                       | terminator
| 50%-gray fill pattern per depth code (csMode - 0x80).
\pfx&PatTab:
	dc.l	0xAAAAAAAA              | 1 bpp checker
	dc.l	0xCCCCCCCC              | 2 bpp
	dc.l	0xF0F0F0F0              | 4 bpp
	dc.l	0xFF00FF00              | 8 bpp
	.endm

| --- Ops (EmitOps <pfx>) -----------------------------------------------------
	.macro	EmitOps pfx

| HwInit: one-shot hardware bring-up quirks at PrimaryInit time.
\pfx&HwInit:
	rts

| Super: out D0.L = super-slot base (0xs0000000) derived from A4.
\pfx&Super:
	move.l	a4,d0
	lsl.l	#4,d0                   | 0xFs000000 -> 0xs0000000
	rts

| FbBase: out A1 = visible framebuffer (super-slot DRAM + 0x11400).
\pfx&FbBase:
	move.l	d0,-(sp)
	bsr	\pfx&Super
	movea.l	d0,a1
	lea	GREG_FB_SUPER(a1),a1
	move.l	(sp)+,d0
	rts

| CardSync: BOUNDED wait for one full heartbeat cycle (bit 31 of
| super+0x4C00000: 1->0->1).  The real declROM driver's cardSync is an
| UNBOUNDED loop — the documented System 6.0.8 hang class (sec. 7.3);
| this one gives up after 256 reads per edge.
\pfx&CardSync:
	movem.l	d0-d2/a0,-(sp)
	bsr	\pfx&Super
	movea.l	d0,a0
	move.w	#255,d2
\pfx&SyncHigh:                          | wait for bit 31 = 1
	move.l	GREG_SYNC_HB(a0),d1
	bmi.s	\pfx&SyncFall
	dbf	d2,\pfx&SyncHigh
	bra.s	\pfx&SyncOut            | give up — never hang the boot
\pfx&SyncFall:
	move.w	#255,d2
\pfx&SyncLow:                           | wait for the falling edge
	move.l	GREG_SYNC_HB(a0),d1
	bpl.s	\pfx&SyncRise
	dbf	d2,\pfx&SyncLow
	bra.s	\pfx&SyncOut
\pfx&SyncRise:
	move.w	#255,d2
\pfx&SyncBack:                          | and the next rising edge
	move.l	GREG_SYNC_HB(a0),d1
	bmi.s	\pfx&SyncOut
	dbf	d2,\pfx&SyncBack
\pfx&SyncOut:
	movem.l	(sp)+,d0-d2/a0
	rts

| BaseAddr: out D0.L = csBaseAddr (A5 = private storage).  The GC's
| visible framebuffer lives in super-slot DRAM, which classic 24-bit
| QuickDraw cannot address — answer the 24-bit boot-family base until
| 32-Bit QuickDraw is loaded, the super-slot DRAM framebuffer after.
| (Known stage-2 gap, sec. 7.2: the OS's gDevice rebuild reads the
| DEVICE base + vpBaseOffset rather than asking the driver, so on
| System 6.0.8 the Finder still paints into std-slot VRAM; the
| accelerator bring-up itself is unaffected.)
\pfx&BaseAddr:
	movem.l	d1/a0-a1,-(sp)
	move.w	#TrapNumGestalt,d0
	dc.w	_GetOSTrapAddress
	move.l	a0,d1
	move.w	#TrapNumUnimpl,d0
	dc.w	_GetOSTrapAddress
	cmp.l	a0,d1
	beq.s	\pfx&Base24             | no Gestalt yet (ROM boot)
	move.l	#0x71642020,d0          | gestaltQuickdrawVersion 'qd  '
	dc.w	_Gestalt
	tst.w	d0
	bne.s	\pfx&Base24
	move.l	a0,d0
	cmp.l	#0x0200,d0
	blo.s	\pfx&Base24             | classic QuickDraw only
	moveq	#0,d0                   | 32-bit QD: super-slot DRAM base
	move.w	pvSlot(a5),d0
	swap	d0
	lsl.l	#8,d0
	lsl.l	#4,d0                   | slot << 28
	add.l	#0xC000000+GREG_FB_OFFSET,d0
	bra.s	\pfx&BaseDone
\pfx&Base24:
	move.l	pvBase(a5),d0
\pfx&BaseDone:
	movem.l	(sp)+,d1/a0-a1
	rts

| ReadSense: out D0.W = functional spID.  Both GC monitors answer
| primary sense 6; stage 2 ships the single config-0 sResource, so any
| sensed monitor selects it (no-monitor still returns 0x80 — the GC is
| usable headless through its accelerator).
\pfx&ReadSense:
	move.w	#GS_BOOT_SPID,d0
	rts

| SetDepth: depth is not register-programmed on the GC (the HLE boots
| at the PRAM-seeded depth; runtime switches ride VidComm).  Perform the
| bounded heartbeat wait the real driver does around mode changes.
\pfx&SetDepth:
	bsr	\pfx&CardSync
	rts

| ClutWrite: JMFB-block RAMDAC path (index + three long writes).
\pfx&ClutWrite:
	movem.l	d0-d4,-(sp)
	and.l	#0xFF,d0
	move.l	d0,GREG_CLUTADDR(a4)
	moveq	#0,d4
	move.b	d1,d4
	move.l	d4,GREG_CLUTDATA(a4)
	moveq	#0,d4
	move.b	d2,d4
	move.l	d4,GREG_CLUTDATA(a4)
	moveq	#0,d4
	move.b	d3,d4
	move.l	d4,GREG_CLUTDATA(a4)
	movem.l	(sp)+,d0-d4
	rts

| VblEnable / VblDisable / VblAck: Stopwatch block, same as JMFB.
\pfx&VblEnable:
	move.l	d0,-(sp)
	moveq	#0x5,d0
	move.l	d0,GREG_SWIC(a4)
	move.l	(sp)+,d0
	rts
\pfx&VblDisable:
	move.l	d0,-(sp)
	moveq	#0x7,d0
	move.l	d0,GREG_SWIC(a4)
	move.l	(sp)+,d0
	rts
\pfx&VblAck:
	move.l	d0,-(sp)
	moveq	#1,d0
	move.l	d0,GREG_SWCLRVINT(a4)
	move.l	(sp)+,d0
	rts
	.endm
