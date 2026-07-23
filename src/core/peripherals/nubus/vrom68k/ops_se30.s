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
.equ GS_FIRSTDIRECT,   8               | no direct modes
.equ GS_ROWLONGS_FIXED, 0              | tight stride (64 bytes at 512 px)
.equ GS_DEFER_SPID,    0               | no deferred family

.equ SE30_FB_OFFSET,   0xE08040        | framebuffer from the 0xFE000000 base

	.macro	GSDrvrName
	dc.b	25
	.ascii	".Display_Video_Apple_SE30"
	.endm

| --- CPB data (EmitCPB <pfx>) ------------------------------------------------
	.macro	EmitCPB pfx
\pfx&MonTab:
	dc.w	0x0080,512,342          | built-in 9" CRT (spID 0x80)
	dc.w	0
\pfx&LogBppTab:
	dc.b	0                       | 1 bpp
	dc.b	0
	.balign	2
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

| FbBase: out A1 = framebuffer base (VRAM + primary-buffer offset).
\pfx&FbBase:
	lea	SE30_FB_OFFSET(a4),a1
	rts

| BaseAddr: out D0.L = csBaseAddr = device base + vpBaseOffset.
\pfx&BaseAddr:
	move.l	pvBase(a5),d0
	add.l	#0x8040,d0
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
