| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| gsdisk_drvr.s
| The Granny Smith SCSI disk driver: the 68k boot driver the bare-volume
| wrapper (image_wrap.c) places in the driver partition of a synthesised
| Apple Partition Map, so a naked HFS volume boots and mounts as a SCSI
| hard disk.  See docs/core/storage/bare-volume-wrapper.md.
|
| A fresh implementation against the published contracts (Inside
| Macintosh: Devices — Device Manager and SCSI Manager chapters) and the
| ROM boot code's observable behaviour; no Apple driver code.  68000
| instructions only, so one binary serves every 68k ROM.
|
| How the ROM runs it (SCSILoad, identical in contract on the Plus, SE/30,
| IIci and Quadra ROMs): read block 0 ('ER'), take the first descriptor of
| ddType 1, load ddSize blocks from ddBlock into a _NewPtrSys block, and
| JSR to its first byte with D5 = the SCSI ID and A0 -> a copy of block 1.
| The IIci-and-later loaders also require an "Apple_Driver*" partition
| named "Maci…" starting at ddBlock whose pmBootCksum matches.
|
| What Install does: create our DCE (unit 32+id, refNum ~(32+id)), walk
| the partition map, and give every Apple_HFS partition a drive queue
| element.  The Start Manager then finds the boot volume by walking the
| drive queue and _Read-ing each drive's boot blocks through us — exactly
| as for any other SCSI disk.  The other volumes are announced later, from
| periodic time (accRun): a disk-inserted event posted during SCSILoad does
| not survive startup, so each drive still without a mounted volume gets
| its event once the system is running, and periodic time then switches
| itself off.

| --- Low-memory globals -------------------------------------------------------
.equ JIODone,         0x08FC          | jump vector: Device Manager IODone
.equ UTableBase,      0x011C          | long: unit table base
.equ DrvQHdr,         0x0308          | drive queue header (qFlags.w, qHead.l, qTail.l)
.equ VCBQHdr,         0x0356          | volume queue header (qFlags.w, qHead.l, qTail.l)
.equ vcbDrvNum,       72              | VCB: drive number (0 once offline)

| --- Traps ---------------------------------------------------------------------
.equ _NewPtrSysClear, 0xA71E          | _NewPtr ,Sys,Clear
.equ _DisposePtr,     0xA01F
.equ _HLock,          0xA029
.equ _BlockMove,      0xA02E          | A0 src, A1 dst, D0 count
.equ _PostEvent,      0xA02F          | A0 event code, D0 message
.equ _DrvrInstall,    0xA03D          | A0 driver, D0 refNum -> blank DCE
.equ _AddDrive,       0xA04E          | A0 qEl, D0 = drive<<16 | refNum
.equ _SCSIDispatch,   0xA815          | selector word on the stack

| SCSIDispatch selectors (stack-based, Pascal convention)
.equ scsiGet,         1
.equ scsiSelect,      2
.equ scsiCmd,         3
.equ scsiComplete,    4
.equ scsiRead,        5
.equ scsiWrite,       6

| Transfer Instruction Block opcodes
.equ scInc,           1
.equ scStop,          7

| --- Structures ------------------------------------------------------------------
| DCE
.equ dCtlDriver,      0
.equ dCtlFlags,       4
.equ dCtlPosition,    16
.equ dCtlStorage,     20
.equ dCtlRefNum,      24
.equ dCtlDelay,       34
.equ dNeedTimeBit,    5               | dCtlFlags high byte: periodic accRun

| IO / control parameter block
.equ ioTrap,          6
.equ ioResult,        16
.equ ioVRefNum,       22              | drive number
.equ ioBuffer,        32
.equ ioReqCount,      36
.equ ioActCount,      40
.equ csCode,          26
.equ csParam,         28
.equ noQueueBit,      9               | ioTrap bit: immediate (no IODone)

| Drive queue element (the four flag bytes sit just before it)
.equ qLink,           0
.equ qType,           4
.equ dQDrive,         6
.equ dQRefNum,        8
.equ dQFSID,          10
.equ dQDrvSz,         12
.equ dQDrvSz2,        14

| Partition map entry
.equ pmSig,           0
.equ pmMapBlkCnt,     4
.equ pmPyPartStart,   8
.equ pmPartBlkCnt,    12
.equ pmParType,       48

| Result codes
.equ noErr,           0
.equ controlErr,      -17
.equ statusErr,       -18
.equ closErr,         -24
.equ ioErr,           -36
.equ paramErr,        -50
.equ nsDrvErr,        -56
.equ diskEvt,         7

| --- Private storage (one _NewPtrSysClear block per loaded copy) ----------------
.equ MaxDrives,       4               | Apple_HFS partitions we mount
.equ AnnounceDelay,   30              | ticks between accRun announcements
.equ MaxXferBlocks,   64              | blocks per SCSI command (32 KB)

| One drive: 4 flag bytes, the queue element, then its partition extent.
.equ dvFlags,         0               | writeProt, diskInPlace, installed, sides
.equ dvQEl,           4               | drive queue element (16 bytes)
.equ dvStart,         20              | long: first physical block
.equ dvBlocks,        24              | long: partition length in blocks
.equ dvSize,          28

.equ psId,            0               | word: our SCSI ID
.equ psRefNum,        2               | word: our driver refNum
.equ psNDrives,       4               | word: drives in use (low byte);
                                      | high byte: drives announced (bit/drive)
.equ psStat,          6               | word: SCSIComplete status
.equ psMsg,           8               | word: SCSIComplete message
.equ psCdb,           10              | 10-byte CDB
.equ psTib,           20              | 2 TIB instructions x 10 bytes
.equ psDrives,        40              | MaxDrives x dvSize
.equ psBounce,        psDrives+MaxDrives*dvSize  | one 512-byte block
.equ psSize,          psBounce+512

	.text

| === Entry: the ROM JSRs to the first byte ======================================
Entry:
	bra	Install

| === The DRVR header (dCtlDriver points here) ====================================
DrvrHdr:
	dc.w	0x6F00                  | dNeedLock|dNeedTime|dStatEnable|dCtlEnable|dWritEnable|dReadEnable
	dc.w	AnnounceDelay           | drvrDelay
	dc.w	0                       | drvrEMask
	dc.w	0                       | drvrMenu
	dc.w	DrvOpen-DrvrHdr
	dc.w	DrvPrime-DrvrHdr
	dc.w	DrvControl-DrvrHdr
	dc.w	DrvStatus-DrvrHdr
	dc.w	DrvClose-DrvrHdr
	.byte	7
	.ascii	".GSDisk"
	.balign	2
| Build stamp: image_wrap.c writes the emulator build id over the zeros
| (then checksums), so a bug report's disk names the driver that ran.
	.ascii	"GSDisk build "
DrvrStamp:
	.fill	24,1,0
	.balign	2

| === Install =========================================================================
| D5 = SCSI ID, A0 -> block 1.  Everything is preserved for the ROM loop.
Install:
	movem.l	d0-d7/a0-a6,-(sp)
	move.l	#psSize,d0
	dc.w	_NewPtrSysClear
	tst.w	d0
	bne	InstDone
	movea.l	a0,a2                   | A2 = private storage
	moveq	#7,d0
	and.w	d5,d0
	move.w	d0,psId(a2)
	add.w	#32,d0                  | unit 32+id ...
	not.w	d0                      | ... refNum ~unit
	move.w	d0,psRefNum(a2)

	lea	DrvrHdr(pc),a0
	dc.w	_DrvrInstall            | blank DCE handle in the unit table
	tst.w	d0
	bne	InstFail

	| DCE handle: UTableBase[unit].  _DrvrInstall only clears the entry and
	| marks it RAM-based; driver pointer, flags and storage are ours to set.
	move.w	psRefNum(a2),d0
	not.w	d0
	lsl.w	#2,d0
	movea.l	UTableBase,a0
	movea.l	0(a0,d0.w),a0
	dc.w	_HLock
	movea.l	(a0),a1                 | A1 = DCE (no _StripAddress: the Plus
	                                | ROM lacks it, and a 24-bit master
	                                | pointer's flag byte is ignored by
	                                | the 24-bit bus anyway)
	lea	DrvrHdr(pc),a0
	move.l	a0,dCtlDriver(a1)
	move.w	#0x6F20,dCtlFlags(a1)   | header flags | dOpened, pointer-based
	move.w	#AnnounceDelay,dCtlDelay(a1)
	move.l	a2,dCtlStorage(a1)

	bsr	ScanMap                 | fill psDrives from the partition map
	bsr	AddDrives               | put them in the drive queue
	bra.s	InstDone
InstFail:
	movea.l	a2,a0
	dc.w	_DisposePtr
InstDone:
	movem.l	(sp)+,d0-d7/a0-a6
	rts

| --- ScanMap: record every Apple_HFS partition (A2 = storage) --------------------
ScanMap:
	movem.l	d0-d7/a0-a1/a3,-(sp)
	moveq	#1,d0                   | block 1: first map entry
	bsr	ReadBounce
	bne	ScanDone
	lea	psBounce(a2),a3
	cmp.w	#0x504D,pmSig(a3)       | 'PM'
	bne	ScanDone
	move.l	pmMapBlkCnt(a3),d6
	cmp.l	#63,d6
	bls.s	ScanCountOk
	moveq	#63,d6
ScanCountOk:
	moveq	#1,d7                   | D7 = map block
ScanLoop:
	cmp.l	d6,d7
	bhi	ScanDone
	move.l	d7,d0
	bsr	ReadBounce
	bne	ScanNext
	cmp.w	#0x504D,pmSig(a3)
	bne	ScanNext
	lea	pmParType(a3),a0
	lea	HfsType(pc),a1
	moveq	#9,d0                   | "Apple_HFS" — exact, then NUL
ScanCmp:
	cmpm.b	(a0)+,(a1)+
	bne.s	ScanNext
	subq.w	#1,d0
	bne.s	ScanCmp
	tst.b	(a0)
	bne.s	ScanNext
	moveq	#0,d0
	move.b	psNDrives+1(a2),d0
	cmp.w	#MaxDrives,d0
	bcc.s	ScanDone
	mulu	#dvSize,d0
	lea	psDrives(a2,d0.w),a0
	move.l	pmPyPartStart(a3),dvStart(a0)
	move.l	pmPartBlkCnt(a3),dvBlocks(a0)
	addq.b	#1,psNDrives+1(a2)
ScanNext:
	addq.l	#1,d7
	bra.s	ScanLoop
ScanDone:
	movem.l	(sp)+,d0-d7/a0-a1/a3
	rts

HfsType:
	.ascii	"Apple_HFS"
	.balign	2

| --- AddDrives: put each partition in the drive queue (A2 = storage) ------------
AddDrives:
	movem.l	d0-d7/a0-a1/a3,-(sp)
	moveq	#0,d7                   | D7 = drive index
AddLoop:
	moveq	#0,d0
	move.b	psNDrives+1(a2),d0
	cmp.w	d0,d7
	bcc	AddDone
	move.w	d7,d0
	mulu	#dvSize,d0
	lea	psDrives(a2,d0.w),a3    | A3 = drive record
	clr.b	dvFlags+0(a3)           | not write-protected
	move.b	#8,dvFlags+1(a3)        | disk in place, non-ejectable
	move.b	#1,dvFlags+2(a3)        | installed
	clr.b	dvFlags+3(a3)
	move.w	#1,dvQEl+qType(a3)      | size is dQDrvSz2:dQDrvSz
	move.l	dvBlocks(a3),d0
	move.w	d0,dvQEl+dQDrvSz(a3)
	swap	d0
	move.w	d0,dvQEl+dQDrvSz2(a3)
	clr.w	dvQEl+dQFSID(a3)

	| Drive number: one past the highest already queued, and at least 5
	| (1-4 are the floppy drives' by convention).
	moveq	#4,d1
	movea.l	DrvQHdr+2,a0            | qHead
AddScan:
	move.l	a0,d0
	beq.s	AddScanned
	cmp.w	dQDrive(a0),d1
	bcc.s	AddScanNext
	move.w	dQDrive(a0),d1
AddScanNext:
	movea.l	qLink(a0),a0
	bra.s	AddScan
AddScanned:
	addq.w	#1,d1
	move.w	d1,dvQEl+dQDrive(a3)
	move.w	psRefNum(a2),dvQEl+dQRefNum(a3)
	move.w	d1,d0
	swap	d0
	move.w	psRefNum(a2),d0         | D0 = drive<<16 | refNum
	lea	dvQEl(a3),a0
	dc.w	_AddDrive
	addq.w	#1,d7
	bra	AddLoop
AddDone:
	movem.l	(sp)+,d0-d7/a0-a1/a3
	rts

| === Open / Close ==================================================================
DrvOpen:
	moveq	#noErr,d0
	rts
DrvClose:
	moveq	#closErr,d0             | a boot driver never goes away
	rts

| === Prime ===========================================================================
| A0 = IO pb, A1 = DCE.  Byte position in dCtlPosition (the Device Manager
| has already applied ioPosMode); ioVRefNum is the drive.  Synchronous:
| the transfer completes before IODone.
DrvPrime:
	movem.l	d1-d7/a0-a6,-(sp)
	movea.l	a0,a4                   | A4 = pb
	movea.l	a1,a5                   | A5 = DCE
	movea.l	dCtlStorage(a5),a2
	clr.l	ioActCount(a4)
	bsr	FindDrive               | A3 = drive record
	bne	PrimeExit
	move.l	dCtlPosition(a5),d4     | D4 = byte position
	move.l	ioReqCount(a4),d5       | D5 = bytes left
	movea.l	ioBuffer(a4),a6         | A6 = user buffer
	| bounds: position + count must lie inside the partition
	move.l	dvBlocks(a3),d0
	moveq	#9,d1
	lsl.l	d1,d0                   | partition bytes (volumes < 4 GB)
	cmp.l	d0,d4
	bhi	PrimeRange
	sub.l	d4,d0
	cmp.l	d0,d5
	bhi	PrimeRange
	moveq	#0,d6                   | D6 = 0 read / 1 write
	cmp.b	#3,ioTrap+1(a4)         | aWrCmd
	bne.s	PrimeLoop
	moveq	#1,d6
PrimeLoop:
	tst.l	d5
	beq	PrimeOk
	move.l	d4,d0
	and.l	#511,d0
	bne.s	PrimePartial            | not block-aligned
	cmp.l	#512,d5
	bcs.s	PrimePartial            | less than a block left
	| Whole blocks straight to/from the caller's buffer.
	move.l	d5,d1
	moveq	#9,d0
	lsr.l	d0,d1                   | blocks available
	cmp.l	#MaxXferBlocks,d1
	bls.s	PrimeWhole
	moveq	#MaxXferBlocks,d1
PrimeWhole:
	move.l	d4,d0
	moveq	#9,d2
	lsr.l	d2,d0
	add.l	dvStart(a3),d0          | physical block
	movea.l	a6,a0
	move.l	d6,d2
	bsr	ScsiXfer
	bne	PrimeIoErr
	moveq	#9,d0
	lsl.l	d0,d1                   | bytes moved
	add.l	d1,d4
	sub.l	d1,d5
	adda.l	d1,a6
	add.l	d1,ioActCount(a4)
	bra.s	PrimeLoop
PrimePartial:
	| Through the bounce block: D3 = offset in block, D7 = bytes this pass.
	move.l	d4,d3
	and.l	#511,d3
	move.l	#512,d7
	sub.l	d3,d7
	cmp.l	d5,d7
	bls.s	PrimePartLen
	move.l	d5,d7
PrimePartLen:
	move.l	d4,d0
	moveq	#9,d2
	lsr.l	d2,d0
	add.l	dvStart(a3),d0
	move.l	d0,-(sp)                | physical block, kept for the write-back
	bsr	ReadBounce
	bne.s	PrimePartErr
	lea	psBounce(a2),a0
	adda.l	d3,a0
	tst.l	d6
	bne.s	PrimePartWrite
	movea.l	a6,a1                   | read: bounce -> user
	move.l	d7,d0
	dc.w	_BlockMove
	bra.s	PrimePartNext
PrimePartWrite:
	movea.l	a0,a1                   | write: user -> bounce -> disk
	movea.l	a6,a0
	move.l	d7,d0
	dc.w	_BlockMove
	move.l	(sp),d0
	lea	psBounce(a2),a0
	moveq	#1,d1
	moveq	#1,d2
	bsr	ScsiXfer
	bne.s	PrimePartErr
PrimePartNext:
	addq.l	#4,sp
	add.l	d7,d4
	sub.l	d7,d5
	adda.l	d7,a6
	add.l	d7,ioActCount(a4)
	bra	PrimeLoop
PrimePartErr:
	addq.l	#4,sp
PrimeIoErr:
	moveq	#ioErr,d0
	bra.s	PrimeExit
PrimeRange:
	moveq	#paramErr,d0
	bra.s	PrimeExit
PrimeOk:
	moveq	#noErr,d0
PrimeExit:
	move.l	d4,dCtlPosition(a5)     | advance past what was moved
	movea.l	a4,a0
	movea.l	a5,a1
	bra	DrvExit

| === Control =========================================================================
DrvControl:
	movem.l	d1-d7/a0-a6,-(sp)
	move.w	csCode(a0),d1
	moveq	#noErr,d0
	cmp.w	#1,d1                   | killCode
	beq.s	CtlDone
	cmp.w	#5,d1                   | verify: the medium is always good
	beq.s	CtlDone
	cmp.w	#6,d1                   | format: preformatted, nothing to do
	beq.s	CtlDone
	cmp.w	#7,d1                   | eject: a hard disk stays put
	beq.s	CtlDone
	cmp.w	#65,d1                  | accRun: announce unmounted drives
	beq.s	CtlAccRun
	cmp.w	#21,d1                  | physical drive icon
	beq.s	CtlIcon
	cmp.w	#22,d1                  | media icon
	beq.s	CtlIcon
	moveq	#controlErr,d0
CtlDone:
	bra	DrvExit
CtlIcon:
	lea	DriveIcon(pc),a1
	move.l	a1,csParam(a0)          | csParam -> icon + mask
	bra.s	CtlDone
CtlAccRun:
	bsr	Announce
	moveq	#noErr,d0
	bra.s	CtlDone

| --- Announce: A1 = DCE.  Post a disk-inserted event for each of our drives
| that has none yet and no mounted volume; once every drive is announced,
| stop asking for periodic time.
Announce:
	movea.l	dCtlStorage(a1),a2
	moveq	#0,d7                   | D7 = drive index
	moveq	#0,d6
	move.b	psNDrives+1(a2),d6      | D6 = drives
	lea	psDrives(a2),a3
AnnLoop:
	cmp.w	d6,d7
	bcc.s	AnnAll
	btst	d7,psNDrives(a2)        | announced already?
	bne.s	AnnNext
	| Mounted already (the boot volume, or mounted by someone else)?
	move.w	dvQEl+dQDrive(a3),d1
	movea.l	VCBQHdr+2,a0
AnnVcb:
	move.l	a0,d0
	beq.s	AnnPost
	cmp.w	vcbDrvNum(a0),d1
	beq.s	AnnMark
	movea.l	(a0),a0                 | qLink
	bra.s	AnnVcb
AnnPost:
	moveq	#0,d0
	move.w	d1,d0                   | message: the drive number
	movea.w	#diskEvt,a0
	dc.w	_PostEvent
	tst.w	d0
	bne.s	AnnNext                 | queue full: try again next time
AnnMark:
	bset	d7,psNDrives(a2)
AnnNext:
	lea	dvSize(a3),a3
	addq.w	#1,d7
	bra.s	AnnLoop
AnnAll:
	moveq	#0,d7
AnnCheck:
	cmp.w	d6,d7
	bcc.s	AnnDone
	btst	d7,psNDrives(a2)
	beq.s	AnnOut                  | still one to go
	addq.w	#1,d7
	bra.s	AnnCheck
AnnDone:
	bclr	#dNeedTimeBit,dCtlFlags(a1)
AnnOut:
	rts

| === Status ==========================================================================
DrvStatus:
	movem.l	d1-d7/a0-a6,-(sp)
	movea.l	a0,a4
	movea.l	dCtlStorage(a1),a2
	cmp.w	#8,csCode(a4)           | drive status
	bne.s	StsBad
	bsr	FindDrive
	bne.s	StsOut
	| DrvSts: track, then the 4 flag bytes and the queue element as-is.
	lea	csParam(a4),a0
	clr.w	(a0)+
	lea	dvFlags(a3),a1
	moveq	#20-1,d1
StsCopy:
	move.b	(a1)+,(a0)+
	dbra	d1,StsCopy
	moveq	#noErr,d0
	bra.s	StsOut
StsBad:
	moveq	#statusErr,d0
StsOut:
	movea.l	a4,a0
	bra	DrvExit

| Shared Prime/Control/Status exit: honour the immediate bit, else IODone.
| Entered with the registers saved by the routine's movem, D0 = result.
DrvExit:
	move.w	d0,ioResult(a0)
	move.w	ioTrap(a0),d1
	btst	#noQueueBit,d1
	movem.l	(sp)+,d1-d7/a0-a6
	beq.s	DrvExitQueued
	rts
DrvExitQueued:
	move.l	JIODone,-(sp)           | IODone wants A1 = DCE, D0 = result
	rts

| --- FindDrive: A4 = pb, A2 = storage -> A3 = drive record, Z set; else
| D0 = nsDrvErr and Z clear.
FindDrive:
	move.w	ioVRefNum(a4),d1
	moveq	#0,d2
	lea	psDrives(a2),a3
	moveq	#0,d3
	move.b	psNDrives+1(a2),d3
FindLoop:
	cmp.w	d3,d2
	bcc.s	FindNone
	cmp.w	dvQEl+dQDrive(a3),d1
	beq.s	FindHit
	lea	dvSize(a3),a3
	addq.w	#1,d2
	bra.s	FindLoop
FindHit:
	moveq	#0,d0
	rts
FindNone:
	moveq	#nsDrvErr,d0
	rts

| --- ReadBounce: D0 = physical block -> psBounce.  Z set on success. ------------
ReadBounce:
	movem.l	d1-d2/a0,-(sp)
	lea	psBounce(a2),a0
	moveq	#1,d1
	moveq	#0,d2
	bsr	ScsiXfer
	movem.l	(sp)+,d1-d2/a0          | movem leaves the CCR alone
	rts

| === SCSI transfer ==================================================================
| D0 = physical block, D1 = block count (1..MaxXferBlocks), D2 = 0 read /
| 1 write, A0 = buffer, A2 = storage.  READ(10)/WRITE(10) through the
| SCSI Manager's polled calls, retried on a busy bus or a bad status
| (the first command after a reset reports a unit attention).
| Returns D0 = 0 and Z set on success.
ScsiXfer:
	movem.l	d1-d7/a0-a1,-(sp)
	move.l	d0,d3                   | D3 = block
	move.l	d1,d4                   | D4 = count
	move.l	d2,d5                   | D5 = direction
	movea.l	a0,a1                   | A1 = buffer
	moveq	#8-1,d7                 | D7 = attempts
XferTry:
	| CDB: READ(10) 0x28 / WRITE(10) 0x2A
	lea	psCdb(a2),a0
	move.b	#0x28,(a0)
	tst.l	d5
	beq.s	XferCdbOp
	move.b	#0x2A,(a0)
XferCdbOp:
	clr.b	1(a0)
	move.l	d3,d0
	move.b	d0,5(a0)                | LBA, big-endian, byte by byte (the
	lsr.l	#8,d0                   | CDB is not word aligned for a long)
	move.b	d0,4(a0)
	lsr.l	#8,d0
	move.b	d0,3(a0)
	lsr.l	#8,d0
	move.b	d0,2(a0)
	clr.b	6(a0)
	move.w	d4,d0
	move.b	d0,8(a0)
	lsr.w	#8,d0
	move.b	d0,7(a0)
	clr.b	9(a0)
	| TIB: scInc buffer, count; scStop
	lea	psTib(a2),a0
	move.w	#scInc,(a0)+
	move.l	a1,(a0)+
	move.l	d4,d0
	moveq	#9,d1
	lsl.l	d1,d0
	move.l	d0,(a0)+
	move.w	#scStop,(a0)+
	clr.l	(a0)+
	clr.l	(a0)

	clr.w	-(sp)                   | SCSIGet
	move.w	#scsiGet,-(sp)
	dc.w	_SCSIDispatch
	tst.w	(sp)+
	bne	XferRetry
	clr.w	-(sp)                   | SCSISelect(id)
	move.w	psId(a2),-(sp)
	move.w	#scsiSelect,-(sp)
	dc.w	_SCSIDispatch
	tst.w	(sp)+
	bne	XferRetry
	clr.w	-(sp)                   | SCSICmd(cdb, 10)
	pea	psCdb(a2)
	move.w	#10,-(sp)
	move.w	#scsiCmd,-(sp)
	dc.w	_SCSIDispatch
	move.w	(sp)+,d6                | D6 = first error in the sequence
	bne.s	XferComplete
	clr.w	-(sp)                   | SCSIRead / SCSIWrite(tib)
	pea	psTib(a2)
	move.w	#scsiRead,d0
	tst.l	d5
	beq.s	XferSel
	move.w	#scsiWrite,d0
XferSel:
	move.w	d0,-(sp)
	dc.w	_SCSIDispatch
	move.w	(sp)+,d6
XferComplete:
	clr.w	-(sp)                   | SCSIComplete(&stat, &msg, 5 s)
	pea	psStat(a2)
	pea	psMsg(a2)
	move.l	#300,-(sp)
	move.w	#scsiComplete,-(sp)
	dc.w	_SCSIDispatch
	move.w	(sp)+,d0
	bne.s	XferRetry
	tst.w	d6
	bne.s	XferRetry
	move.w	psStat(a2),d0
	and.w	#0x1E,d0                | status byte: good = 0
	bne.s	XferRetry
	moveq	#0,d0
	bra.s	XferOut
XferRetry:
	dbra	d7,XferTry
	moveq	#ioErr,d0
XferOut:
	movem.l	(sp)+,d1-d7/a0-a1
	tst.w	d0
	rts

	.balign	2
DriveIcon:
| 32x32 drive icon + mask (ICN# layout), drawn for this driver.
| icon
	dc.l	0x00000000,0x00000000,0x00000000,0x00000000
	dc.l	0x00000000,0x00000000,0x00000000,0x00000000
	dc.l	0x00000000,0x7FFFFFFE,0x40000002,0x40000002
	dc.l	0x40000002,0x40000002,0x40000002,0x40000002
	dc.l	0x40000002,0x40000002,0x7FFFFFFE,0x40000002
	dc.l	0x4F000002,0x40000002,0x7FFFFFFE,0x3FFFFFFC
	dc.l	0x00000000,0x00000000,0x00000000,0x00000000
	dc.l	0x00000000,0x00000000,0x00000000,0x00000000
| mask
	dc.l	0x00000000,0x00000000,0x00000000,0x00000000
	dc.l	0x00000000,0x00000000,0x00000000,0x00000000
	dc.l	0x00000000,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE
	dc.l	0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE
	dc.l	0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE
	dc.l	0x7FFFFFFE,0x7FFFFFFE,0x7FFFFFFE,0x3FFFFFFC
	dc.l	0x00000000,0x00000000,0x00000000,0x00000000
	dc.l	0x00000000,0x00000000,0x00000000,0x00000000

DrvrEnd:
