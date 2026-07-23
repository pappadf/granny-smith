| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| gsvrom_sinit.s
| SecondaryInit — family swap (nubus_vrom.md sec. 9).  Runs under Slot
| Manager v1+ with the Toolbox alive.  When 32-bit QuickDraw is present,
| swap the 24-bit boot family (GS_BOOT_SPID) for the 32-bit one
| (GS_DEFER_SPID) whose framebuffer lives in super-slot DRAM, and — if
| this card drives the boot screen — re-point the live GDevice PixMap,
| the DCE dCtlDevBase, the driver's cached base, and ScrnBase.
|
| Only assembled for personalities with a deferred family (the GC);
| split out of gsvrom_init.s so it builds as its own fragment
| (frag_<p>_sinit.bin) the declrom builder splices independently.

	sExecBegin GSSInit,2

GSSecondaryInit:
	movem.l	d1-d7/a0-a6,-(sp)
	movea.l	a0,a2                   | A2 = seBlock
	moveq	#0,d5
	move.b	seSlot(a2),d5

	| 32-bit QuickDraw present?  (gestaltQuickdrawVersion >= 0x0200)
	move.l	#0x71642020,d0          | 'qd  '
	dc.w	_Gestalt
	tst.w	d0
	bne	SIDone                  | no Gestalt answer — stay 24-bit
	move.l	a0,d0                   | response
	cmp.l	#0x0200,d0
	blo	SIDone                  | classic QD only — stay 24-bit

	| swap the SRT families: delete the boot one, insert the 32-bit one
	suba.w	#spBlockSize,sp
	movea.l	sp,a3
	move.b	d5,spSlot(a3)
	move.b	#GS_BOOT_SPID,spID(a3)
	clr.b	spExtDev(a3)
	movea.l	a3,a0
	moveq	#sDeleteSRTRec,d0
	dc.w	_SlotManager
	move.b	d5,spSlot(a3)
	move.b	#GS_DEFER_SPID,spID(a3)
	clr.b	spExtDev(a3)
	movea.l	a3,a0
	moveq	#sInsertSRTRec,d0
	dc.w	_SlotManager

	| our driver's refNum (negative iff open) via sRsrcInfo
	move.b	d5,spSlot(a3)
	move.b	#GS_DEFER_SPID,spID(a3)
	clr.b	spExtDev(a3)
	movea.l	a3,a0
	moveq	#sRsrcInfo,d0
	dc.w	_SlotManager
	move.w	spRefNum(a3),d6
	adda.w	#spBlockSize,sp

	| D4 = new framebuffer base: super slot base + DRAM offset
	move.l	d5,d0
	swap	d0
	lsl.l	#8,d0                   | slot << 24
	lsl.l	#4,d0                   | slot << 28
	add.l	#GREG_FB_SUPER,d0
	move.l	d0,d4

	| patch the DCE + the driver's cached base
	tst.w	d6
	bge.s	SIScreen                | not open — nothing to patch
	move.w	d6,d0
	not.w	d0                      | unit = -refNum-1
	lsl.w	#2,d0
	movea.l	UTableBase,a0
	movea.l	(a0,d0.w),a0            | DCE handle
	move.l	(a0),d0
	dc.w	_StripAddress
	movea.l	d0,a1                   | DCE
	move.l	d4,dCtlDevBase(a1)
	move.l	dCtlStorage(a1),d0
	beq.s	SIScreen
	movea.l	d0,a0
	move.l	(a0),d0
	dc.w	_StripAddress
	movea.l	d0,a0
	move.l	d4,(a0)                 | driver private pvBase (offset 0)

SIScreen:
	| boot screen?  MainDevice's gdRefNum names its driving driver.
	movea.l	MainDevice,a0
	move.l	a0,d0
	beq.s	SIDone
	move.l	(a0),d0
	dc.w	_StripAddress
	movea.l	d0,a1                   | GDevice
	cmp.w	gdRefNum(a1),d6
	bne.s	SIDone
	movea.l	gdPMap(a1),a0           | PixMap handle
	move.l	(a0),d0
	dc.w	_StripAddress
	movea.l	d0,a1
	move.l	d4,pmBaseAddr(a1)
	move.l	d4,ScrnBase

SIDone:
	clr.w	seStatus(a2)            | success
	movem.l	(sp)+,d1-d7/a0-a6
	rts

	sExecEnd GSSInit
