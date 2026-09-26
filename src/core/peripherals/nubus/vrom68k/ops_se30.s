| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| ops_se30.s
| SE/30 built-in video personality: CPB equates + data/ops macros.
| The "card" is a bare framebuffer — no registers at all (fixed 512x342
| at 1 bpp, VRAM at slot 0xE standard space + 0xE00000, framebuffer at
| +0x8040) — so nearly every op is a no-op.  The device base is 0 with
| vpBaseOffset 0x8040, matching the real onboard vROM's records (ScrnBase
| = 0xFEE00000 + 0x8040).

| --- CPB equates -------------------------------------------------------------
.equ GS_DRHW,          0x0009          | SE/30 built-in video DrHW
.equ GS_FB_MINOR,      0               | device base 0; offset rides vpBaseOffset
.equ GS_NMODES,        1               | 1 bpp only
.equ GS_NPAGES,        2               | TWO framebuffers: the SE/30 has a
                                        | primary and an alternate raster,
                                        | selected by VIA1 PA6.  The hand-built
                                        | fallback ROM has always declared
                                        | mPageCnt 2 for this reason; the
                                        | generated ROM once under-declared it
                                        | as 1.  Declaring 2 obliges this
                                        | driver to SERVE page 1 --
                                        | Designing Cards and Drivers 3ed makes
                                        | cscSetMode the page-switch call and
                                        | GetBaseAddr answerable for a page that
                                        | is not displayed -- which is what the
                                        | SetPage / BaseAddr ops below do.
.equ GS_FIRSTDIRECT,   8               | no direct modes
.equ GS_DEFER_SPID,    0               | no deferred family

.equ SE30_FB_OFFSET,   0xE08040        | primary framebuffer, 0xFE000000 base
.equ SE30_FB_ALT_DELTA, -0x8000        | alternate raster sits 0x8000 BELOW the
                                        | primary (offsets 0x0040 vs 0x8040)

	.macro	GSDrvrName
	dc.b	25
	.ascii	".Display_Video_Apple_SE30"
	.endm

| --- CPB data (EmitCPB <pfx>) ------------------------------------------------
	.macro	EmitCPB pfx
| Top-level video spID (the built-in CRT); geometry lives only in the
| generated records.
\pfx&SpidTab:
	dc.w	0x0080
	dc.w	0                       | terminator
| 50%-gray fill pattern per depth code (csMode - 0x80).
\pfx&PatTab:
	dc.l	0xAAAAAAAA              | 1 bpp checker
	.endm

| --- Ops (EmitOps <pfx>) -----------------------------------------------------
	.macro	EmitOps pfx

| HwInit: select the PRIMARY video buffer by driving VIA1 PA6 high
| (DDR + output register + the shadow cells the real onboard vROM's
| PrimaryInit pokes) — without this the machine displays the empty
| alternate buffer.  VIA1 base from the low-memory global 0x1D4.
\pfx&HwInit:
	move.l	a0,-(sp)
	movea.l	0x1D4.w,a0
	bset	#6,0x600(a0)
	bset	#6,0x400(a0)
	bset	#6,0x1E00(a0)
	bset	#6,(a0)
	movea.l	(sp)+,a0
	rts

| FbBase: out A1 = base of the DISPLAYED framebuffer (gray fills and the
| like paint what the user is looking at, so this follows pvPage).
\pfx&FbBase:
	lea	SE30_FB_OFFSET(a4),a1
	tst.w	pvPage(a5)
	beq.s	9f
	lea	SE30_FB_ALT_DELTA(a1),a1
9:
	rts

| BaseAddr: in D2.W = page, out D0.L = csBaseAddr for THAT page (not
| necessarily the displayed one -- GetBaseAddr must answer for a page
| that is not switched in).
\pfx&BaseAddr:
	move.l	pvBase(a5),d0
	add.l	#0x8040,d0
	tst.w	d2
	beq.s	9f
	add.l	#SE30_FB_ALT_DELTA,d0
9:
	rts

| SetPage: switch the displayed raster.  VIA1 PA6 high = primary, low =
| alternate -- the same line HwInit drives at startup, including the
| shadow cells the real onboard vROM's PrimaryInit pokes.  VIA1 base
| from the low-memory global 0x1D4.
\pfx&SetPage:
	move.l	a0,-(sp)
	movea.l	0x1D4.w,a0
	bset	#6,0x600(a0)            | DDR: PA6 is an output either way
	tst.w	pvPage(a5)
	bne.s	8f
	bset	#6,0x400(a0)
	bset	#6,0x1E00(a0)
	bset	#6,(a0)
	bra.s	9f
8:
	bclr	#6,0x400(a0)
	bclr	#6,0x1E00(a0)
	bclr	#6,(a0)
9:
	movea.l	(sp)+,a0
	rts

| ReadSense: the built-in CRT is always connected.
\pfx&ReadSense:
	move.w	#0x80,d0
	rts

| SetDepth / ClutWrite / VBL ops: no hardware to program — the
| framebuffer is fixed 1-bpp and the VBL rides the machine's VIA, not a
| slot interrupt.
\pfx&SetDepth:
	rts
\pfx&ClutWrite:
	rts
\pfx&VblEnable:
	rts
\pfx&VblDisable:
	rts
\pfx&VblAck:
	rts
	.endm
