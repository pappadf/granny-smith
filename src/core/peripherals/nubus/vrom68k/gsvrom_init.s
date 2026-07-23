| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| gsvrom_init.s
| Shared PrimaryInit framework (proposal sec. 3.1): locate personality ->
| read sense -> prune the sResource directory to the sensed monitor ->
| program the 1-bpp boot mode -> gray the screen -> default slot PRAM.
| Personality specifics come exclusively from the CPB data and ops
| emitted into this block (EmitCPB/EmitOps with the "PI" prefix) — the
| Slot Manager copies the whole sExecBlock to RAM before running it, so
| everything referenced here must be inside the block.
|
| Entry (per docs/core/peripherals/nubus_vrom.md sec. 8): A0 -> seBlock;
| only _SlotManager and _SwapMMUMode are guaranteed callable; return
| status in seStatus (0 = success).

	sExecBegin GSPInit,2            | CPU id 2 = 68020+

GSPrimaryInit:
	movem.l	d1-d7/a0-a6,-(sp)
	movea.l	a0,a2                   | A2 = seBlock
	moveq	#0,d5
	move.b	seSlot(a2),d5           | D5 = slot number
	| A4 = 0xFs000000 slot base (32-bit form)
	move.l	d5,d0
	swap	d0
	lsl.l	#8,d0                   | slot << 24
	or.l	#0xF0000000,d0
	movea.l	d0,a4
	| hardware lives above the 1 MB minor space — enter 32-bit mode
	moveq	#1,d0
	dc.w	_SwapMMUMode
	move.b	d0,d7                   | D7.b = saved addressing mode

	| read the monitor sense — the op returns the chosen functional
	| sResource spID directly (sense strategy is personality-private)
	bsr	PIReadSense
	move.b	d0,d6                   | D6.b = chosen spID (0 = none)

	| prune: delete every top-level video sResource except the sensed
	| monitor's (and the deferred 32-bit family, when the personality has
	| one — the boot-time Slot Manager follows the PRAM savedSRsrcID to
	| the boot family, and 32-Bit QuickDraw's slot upgrade re-opens the
	| driver on the 32-bit sister once it loads).  The set of top-level
	| spIDs is the one private convention the fragment still carries
	| (§3.4: "the sister ids the emulator seeds into PRAM"); it is NOT
	| mode geometry (that now lives only in the generated records) — so
	| the personality's SpidTab lists ids alone, no width/height.
	lea	PISpidTab(pc),a3
PIPruneLoop:
	move.w	(a3),d0
	beq.s	PIPruned                | table terminator
	cmp.b	d0,d6
	beq.s	PIPruneKeep             | the chosen monitor stays
	.if	GS_DEFER_SPID
	cmp.b	#GS_DEFER_SPID,d0
	beq.s	PIPruneKeep             | the 32-bit family stays for QD32
	.endif
	suba.w	#spBlockSize,sp
	movea.l	sp,a0
	move.b	d5,spSlot(a0)
	move.b	d0,spID(a0)
	clr.b	spExtDev(a0)
	moveq	#sDeleteSRTRec,d0
	dc.w	_SlotManager
	adda.w	#spBlockSize,sp
PIPruneKeep:
	addq.w	#2,a3
	bra.s	PIPruneLoop
PIPruned:
	tst.b	d6
	beq	PIDone                  | no monitor: slot goes quietly dark

	| geometry of the chosen monitor: its boot-mode (0x80) VPBlock read
	| from the generated records — the monitors[] table's single source
	| of truth (§3.4).  Allocation-free Slot Manager forms only: no
	| Memory Manager exists yet, so sGetBlock is off the table here.
	suba.w	#spBlockSize+20,sp
	movea.l	sp,a3                   | A3 = spBlock
	lea	spBlockSize(sp),a1      | A1 = VPBlock head buffer (20 bytes)
	move.b	d5,spSlot(a3)
	move.b	d6,spID(a3)
	clr.b	spExtDev(a3)
	movea.l	a3,a0
	moveq	#sRsrcInfo,d0
	dc.w	_SlotManager
	bne	PIVPFail
	move.b	#0x80,spID(a3)          | the boot-depth mode entry
	movea.l	a3,a0
	moveq	#sFindStruct,d0
	dc.w	_SlotManager
	bne	PIVPFail
	move.b	#1,spID(a3)             | mVidParams
	movea.l	a3,a0
	moveq	#sFindStruct,d0
	dc.w	_SlotManager
	bne	PIVPFail
	move.l	a1,spResult(a3)
	moveq	#20,d0
	move.l	d0,spSize(a3)
	movea.l	a3,a0
	moveq	#sReadStruct,d0
	dc.w	_SlotManager
	bne	PIVPFail
	move.w	8(a1),d2                | D2 = vpRowBytes (1-bpp boot mode)
	move.w	14(a1),d3               | D3 = height (vpBounds bottom)
	adda.w	#spBlockSize+20,sp

	| hardware bring-up quirks, then the 1-bpp boot mode
	bsr	PIHwInit
	moveq	#0,d0                   | depth code 0 = 1 bpp
	move.w	d2,d1                   | boot-mode row bytes for the op
	bsr	PISetDepth

	| gray the screen: alternating 0xAAAAAAAA / 0x55555555 rows at 1 bpp
	bsr	PIFbBase                | A1 = framebuffer base (op-provided)
	movea.l	a1,a0
	move.w	d2,d0
	lsr.w	#2,d0                   | longs per row = vpRowBytes / 4
	move.l	#0xAAAAAAAA,d1
	move.w	d3,d4
	subq.w	#1,d4
PIFillRow:
	move.w	d0,d3                   | height no longer needed — reuse
	subq.w	#1,d3
PIFillLong:
	move.l	d1,(a0)+
	dbf	d3,PIFillLong
	not.l	d1
	dbf	d4,PIFillRow

	| slot PRAM: default savedMode/sister when unset or for another
	| monitor.  (An emulator- or Monitors-seeded record that names the
	| chosen monitor is left untouched.)
	suba.w	#spBlockSize+8,sp
	movea.l	sp,a1                   | A1 = 8-byte sPRAMRec buffer
	lea	8(sp),a3                | A3 = spBlock
	move.l	a1,spResult(a3)
	move.b	d5,spSlot(a3)
	clr.b	spExtDev(a3)
	movea.l	a3,a0
	moveq	#sReadPRAMRec,d0
	dc.w	_SlotManager
	move.b	2(a1),d0                | savedMode: default only when it is
	cmp.b	#0x80,d0                | outside the mode-list range — a
	blo.s	PIFixMode               | VALID staged depth must survive a
	cmp.b	#0x80+GS_NMODES-1,d0    | sister-byte fix-up
	bls.s	PIModeOk
PIFixMode:
	move.b	#0x80,2(a1)             | boot depth: 1 bpp
PIModeOk:
	cmp.b	3(a1),d6                | sister must name the chosen monitor
	beq.s	PIPramOk
	move.b	d6,3(a1)                | savedSRsrcID
	move.b	d6,4(a1)                | savedRawSRsrcID
	move.l	a1,spsPointer(a3)
	movea.l	a3,a0
	moveq	#sPutPRAMRec,d0
	dc.w	_SlotManager
PIPramOk:
	adda.w	#spBlockSize+8,sp
	bra.s	PIDone

PIVPFail:
	adda.w	#spBlockSize+20,sp      | record walk failed — leave the slot
	                                | dark rather than paint blind

PIDone:
	move.b	d7,d0                   | restore addressing mode
	dc.w	_SwapMMUMode
	clr.w	seStatus(a2)            | success
	movem.l	(sp)+,d1-d7/a0-a6
	rts

	EmitCPB	PI
	EmitOps	PI

	sExecEnd GSPInit
