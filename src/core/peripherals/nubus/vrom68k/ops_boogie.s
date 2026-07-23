| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| ops_boogie.s
| Display Card 24AC ("Boogie") personality: CPB equates + data/ops macros.
| Register model per display_card_24ac.h — byte-wide lane-3 registers at
| full slot-relative offsets; extended monitor sense on SENSE_CLK; an
| active-low RAMDAC; depth in MODE_REG bits 7-5; no stride register.

| --- CPB equates -------------------------------------------------------------
.equ GS_DRHW,          0x002B          | 24AC DrHW (Display_Video_Apple_Boogie)
.equ GS_FB_MINOR,      0               | framebuffer at VRAM offset 0
.equ GS_NMODES,        5               | 0x80..0x84 = 1/4/8/16/32 bpp (no 2 bpp)
.equ GS_FIRSTDIRECT,   3               | codes 3 (16 bpp) and 4 (32 bpp) are direct
.equ GS_DEFER_SPID,    0               | no deferred 32-bit sResource family

	.macro	GSDrvrName
	dc.b	27
	.ascii	".Display_Video_Apple_Boogie"
	.endm

| Register offsets from the 0xFs000000 slot base (byte registers, lane 3).
.equ BREG_STATUS,      0xD00402        | R: depth code + busy toggle
.equ BREG_VIDCTL,      0xD00403        | R/W: bit 7 = VBL mask; low bits stay 2
.equ BREG_MODE,        0xD80001        | R/W: depth in bits 7-5
.equ BREG_SENSE,       0xD8000D        | monitor sense drive/read
.equ BREG_CLUT_ADDR,   0xC8001E        | runtime CLUT index (active-low)
.equ BREG_CLUT_DATA,   0xC8001A        | runtime CLUT data  (active-low)

| --- CPB data (EmitCPB <pfx>) ------------------------------------------------
| All three multisync monitors answer primary sense 6; the extended sense
| code distinguishes them.  spIDs reproduce the real ROM's sister scheme
| (0x6B/0x6C/0x6D) so emulator PRAM/mode staging matches.
	.macro	EmitCPB pfx
| Top-level video spIDs (the 0x6B/0x6C/0x6D sister scheme); geometry
| lives only in the generated records (§3.4).
\pfx&SpidTab:
	dc.w	0x006B,0x006C,0x006D
	dc.w	0                       | terminator
| Extended-sense code -> spID rows (byte pairs), 0-terminated.
\pfx&ExtMap:
	dc.b	0x03,0x6B
	dc.b	0x0B,0x6C
	dc.b	0x23,0x6D
	dc.b	0,0
| 50%-gray fill pattern per depth code (csMode - 0x80).
\pfx&PatTab:
	dc.l	0xAAAAAAAA              | 1 bpp checker
	dc.l	0xF0F0F0F0              | 4 bpp
	dc.l	0xFF00FF00              | 8 bpp
	dc.l	0x42104210              | 16 bpp: solid 50% gray (direct)
	dc.l	0x00808080              | 32 bpp: solid 50% gray (direct)
| MODE_REG depth ladder (bits 7-5), indexed by depth code.
\pfx&ModeTab:
	dc.b	0x00,0x20,0x40,0xA0,0xC0
	.balign	2
	.endm

| --- Ops (EmitOps <pfx>) -----------------------------------------------------
	.macro	EmitOps pfx

| HwInit: one-shot hardware bring-up quirks at PrimaryInit time.
\pfx&HwInit:
	rts

| FbBase: out A1 = framebuffer base address (in from A4 = slot base).
\pfx&FbBase:
	lea	GS_FB_MINOR(a4),a1
	rts

| BaseAddr: out D0.L = the csBaseAddr the driver reports (A5 = private
| storage; runs in the CALLER's addressing mode — no hardware access).
\pfx&BaseAddr:
	move.l	pvBase(a5),d0
	rts

| ReadSense: out D0.W = functional sResource spID (0 = none).
| Primary probe (all lines released) must answer code 6 (multisync);
| then the three-round extended probe assembles the 6-bit code the
| ExtMap rows translate.  Read-back bit CLEAR => ext bit 1 (active-low).
\pfx&ReadSense:
	movem.l	d1-d3/a0,-(sp)
	clr.b	BREG_SENSE(a4)          | release all lines (primary probe)
	move.b	BREG_SENSE(a4),d0
	not.b	d0
	lsr.b	#5,d0
	and.w	#7,d0
	cmp.w	#6,d0
	bne	\pfx&SenseNone          | not a multisync monitor: no display
	moveq	#0,d1                   | D1 = assembled ext code
	move.b	#0x80,BREG_SENSE(a4)    | drive line A
	move.b	BREG_SENSE(a4),d0
	btst	#6,d0
	bne.s	\pfx&SenseA1
	bset	#5,d1
\pfx&SenseA1:
	btst	#5,d0
	bne.s	\pfx&SenseA2
	bset	#4,d1
\pfx&SenseA2:
	move.b	#0x40,BREG_SENSE(a4)    | drive line B
	move.b	BREG_SENSE(a4),d0
	btst	#7,d0
	bne.s	\pfx&SenseB1
	bset	#3,d1
\pfx&SenseB1:
	btst	#5,d0
	bne.s	\pfx&SenseB2
	bset	#2,d1
\pfx&SenseB2:
	move.b	#0x20,BREG_SENSE(a4)    | drive line C
	move.b	BREG_SENSE(a4),d0
	btst	#7,d0
	bne.s	\pfx&SenseC1
	bset	#1,d1
\pfx&SenseC1:
	btst	#6,d0
	bne.s	\pfx&SenseC2
	bset	#0,d1
\pfx&SenseC2:
	clr.b	BREG_SENSE(a4)          | release the lines again
	lea	\pfx&ExtMap(pc),a0
\pfx&SenseLook:
	move.b	(a0)+,d2                | ext code of this row
	beq.s	\pfx&SenseNone          | table end: unknown monitor
	move.b	(a0)+,d3                | spID
	cmp.b	d1,d2
	bne.s	\pfx&SenseLook
	moveq	#0,d0
	move.b	d3,d0
	movem.l	(sp)+,d1-d3/a0
	rts
\pfx&SenseNone:
	moveq	#0,d0
	movem.l	(sp)+,d1-d3/a0
	rts

| SetDepth: in D0.W = depth code 0..4, D1.W = width (unused — the card
| derives its stride from the mode).  Programs MODE_REG bits 7-5.
\pfx&SetDepth:
	movem.l	d0/a0,-(sp)
	lea	\pfx&ModeTab(pc),a0
	move.b	(a0,d0.w),d0
	move.b	d0,BREG_MODE(a4)
	movem.l	(sp)+,d0/a0
	rts

| ClutWrite: in D0.W = index, D1/D2/D3.B = R/G/B.  The 24AC RAMDAC is
| ACTIVE-LOW: the genuine driver NOT.Bs the index and every component,
| and the HLE undoes exactly that (display_card_24ac.c clut_set_index).
\pfx&ClutWrite:
	movem.l	d0-d3,-(sp)
	not.b	d0
	move.b	d0,BREG_CLUT_ADDR(a4)
	not.b	d1
	move.b	d1,BREG_CLUT_DATA(a4)
	not.b	d2
	move.b	d2,BREG_CLUT_DATA(a4)
	not.b	d3
	move.b	d3,BREG_CLUT_DATA(a4)
	movem.l	(sp)+,d0-d3
	rts

| VblEnable / VblDisable / VblAck: VIDCTL bit 7 is the VBL mask;
| the ISR acks by pulsing it high then low (edge-triggered re-arm).
| The low bits stay 2 — the driver-open gate value.
\pfx&VblEnable:
	move.b	#0x02,BREG_VIDCTL(a4)
	rts
\pfx&VblDisable:
	move.b	#0x82,BREG_VIDCTL(a4)
	rts
\pfx&VblAck:
	move.b	#0x82,BREG_VIDCTL(a4)
	move.b	#0x02,BREG_VIDCTL(a4)
	rts
	.endm
