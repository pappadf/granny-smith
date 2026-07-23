| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| ops_jmfb.s
| JMFB (8*24) personality: Card Parameter Block equates + data/ops macros.
|
| The PrimaryInit sExecBlock and the DRVR are each copied to RAM before
| execution, so any data or leaf routine they use must live INSIDE that
| block.  The CPB data and card ops are therefore emitted per-block via
| macros taking a label prefix (pfx) — the personality stays one source,
| instantiated wherever a self-contained copy is needed (proposal sec. 3.1).

| --- CPB equates -------------------------------------------------------------
.equ GS_DRHW,          0x0019          | JMFB DrHW (Display_Video_Apple_MDC)
.equ GS_FB_MINOR,      0xA00           | framebuffer offset in standard slot space
.equ GS_NMODES,        4               | mode-list entries 0x80..0x83 (1/2/4/8 bpp)
.equ GS_FIRSTDIRECT,   8               | no direct-RGB depth codes on this card
.equ GS_DEFER_SPID,    0               | no deferred 32-bit sResource family

| DRVR name — "." + the functional sResource name the builder generates
| ("Display_Video_Apple_MDC"; Mac-side software matches on the exact
| string, proposal sec. 7.1).
	.macro	GSDrvrName
	dc.b	24
	.ascii	".Display_Video_Apple_MDC"
	.endm

| Register offsets from the 0xFs000000 slot base (16-bit registers occupy
| the LOW half of a longword cell — write the value as a long, or as a
| word at offset+2; see jmfb.h and the HLE convention).
.equ JREG_CSR,         0x200000        | control/status; sense in bits 9-11
.equ JREG_VIDBASE,     0x200008        | framebuffer base, units of 32 bytes
.equ JREG_ROWWORDS,    0x20000C        | stride, units of 4 bytes (indexed depths)
.equ JREG_SWIC,        0x20013C        | Stopwatch SWICReg: bit1 = VBL mask
.equ JREG_SWCLRVINT,   0x200148       | write anything: clear pending VBL IRQ
.equ JREG_CLUTADDR,    0x200200        | RAMDAC palette index
.equ JREG_CLUTDATA,    0x200204        | RAMDAC palette data (3 long writes: R,G,B)
.equ JREG_PBCR,        0x200208        | pixel bus control: depth code in bits 3-4

| --- CPB data (EmitCPB <pfx>) ------------------------------------------------
| Monitor table rows: spID.w, width.w, height.w — spIDs reproduce the real
| ROM's ACTIVE Ax sister scheme so emulator PRAM seeding matches (sec. 3.3).
	.macro	EmitCPB pfx
| Top-level video spIDs (the ACTIVE Ax sister scheme) — the prune's
| kill-list; geometry lives only in the generated records (§3.4).
\pfx&SpidTab:
	dc.w	0x00A6,0x00A2,0x00A1,0x00A7
	dc.w	0                       | terminator
| Raw 3-bit sense -> functional sResource spID (0 = no usable monitor).
| Mapping mirrors jmfb.c::monitor_for_sense (JMFBPrimaryInit.a's table).
\pfx&SenseMap:
	dc.b	0xA7                    | 000 RGB Workstation (Kong 21")
	dc.b	0xA1                    | 001 B&W Portrait
	dc.b	0xA2                    | 010 12" RGB (Rubik)
	dc.b	0xA7                    | 011 B&W Workstation (Kong dims)
	dc.b	0xA6                    | 100 NTSC — approximate as 13"
	dc.b	0xA1                    | 101 RGB Portrait
	dc.b	0xA6                    | 110 Standard RGB 13"
	dc.b	0                       | 111 no connect / extended sense
| 50%-gray fill pattern per depth code (csMode - 0x80).
\pfx&PatTab:
	dc.l	0xAAAAAAAA              | 1 bpp checker
	dc.l	0xCCCCCCCC              | 2 bpp
	dc.l	0xF0F0F0F0              | 4 bpp
	dc.l	0xFF00FF00              | 8 bpp
	.endm

| --- Ops (EmitOps <pfx>) -----------------------------------------------------
| Register conventions shared by every personality's ops:
|   A4 = 0xFs000000 slot base, valid in 32-bit addressing mode only.
|   Each op preserves all registers except those documented as outputs.
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

| ReadSense: out D0.W = chosen functional sResource spID (0 = no usable
| monitor).  The whole sense strategy is personality-private (proposal
| sec. 3.1): the JMFB reads the 3-bit sense field in CSR bits 9-11 and
| maps it through the CPB sense table.
\pfx&ReadSense:
	move.l	JREG_CSR(a4),d0
	lsr.l	#8,d0
	lsr.l	#1,d0                   | sense lives in bits 9-11
	and.w	#7,d0
	lea	\pfx&SenseMap(pc),a0
	moveq	#0,d1
	move.b	(a0,d0.w),d1
	move.w	d1,d0
	rts

| SetDepth: in D0.W = depth code 0..3 (1<<code bpp), D1.W = the target
| mode's vpRowBytes (from the record).  Programs depth + stride + base.
\pfx&SetDepth:
	movem.l	d0-d2,-(sp)
	move.w	d0,d2
	lsl.w	#3,d2
	or.w	#0x80,d2                | PBCR = 0x80 | code<<3 (matches the
	                                | real driver's observable values)
	lsr.w	#2,d1                   | RowWords = vpRowBytes / 4
	moveq	#0,d0
	move.w	d2,d0
	move.l	d0,JREG_PBCR(a4)
	moveq	#0,d0
	move.w	d1,d0
	move.l	d0,JREG_ROWWORDS(a4)
	move.l	#GS_FB_MINOR/32,d0
	move.l	d0,JREG_VIDBASE(a4)     | base fixed at slot+0xA00
	movem.l	(sp)+,d0-d2
	rts

| ClutWrite: in D0.W = palette index, D1.B/D2.B/D3.B = R/G/B.
| Index write resets the RAMDAC phase; three long writes carry one
| component each in the low byte, then the index auto-increments.
\pfx&ClutWrite:
	movem.l	d0-d4,-(sp)
	and.l	#0xFF,d0
	move.l	d0,JREG_CLUTADDR(a4)
	moveq	#0,d4
	move.b	d1,d4
	move.l	d4,JREG_CLUTDATA(a4)
	moveq	#0,d4
	move.b	d2,d4
	move.l	d4,JREG_CLUTDATA(a4)
	moveq	#0,d4
	move.b	d3,d4
	move.l	d4,JREG_CLUTDATA(a4)
	movem.l	(sp)+,d0-d4
	rts

| VblEnable / VblDisable: SWICReg bit 1 is an active-high VBL mask.
| The real driver writes 5 / 7; the HLE keys on bit 1 only.
\pfx&VblEnable:
	move.l	d0,-(sp)
	moveq	#0x5,d0
	move.l	d0,JREG_SWIC(a4)
	move.l	(sp)+,d0
	rts
\pfx&VblDisable:
	move.l	d0,-(sp)
	moveq	#0x7,d0
	move.l	d0,JREG_SWIC(a4)
	move.l	(sp)+,d0
	rts

| VblAck: any write clears the pending VBL interrupt.
\pfx&VblAck:
	move.l	d0,-(sp)
	moveq	#1,d0
	move.l	d0,JREG_SWCLRVINT(a4)
	move.l	(sp)+,d0
	rts
	.endm
