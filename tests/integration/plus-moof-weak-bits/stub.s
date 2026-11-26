| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| Guest stub for the plus-moof-weak-bits row: the check a weak-bit copy
| protection makes.  It hooks the Sony driver's data-field read (JRdData,
| $022E) and _Reads one sector of the external drive's disk -- a sector
| whose data field is a weak area -- once per read in [first, last).  On
| each read the hook copies NIBS nibbles from where the driver left the
| head (just past the sector's address field), with no timing, as the
| protection does.  Each read is then scanned for records F6 AF xx yy 96
| F6, a record's number being (yy & 1) << 8 | xx; the distinct records of
| each read are counted, and so are those of all reads together.  The
| pass rule: the first read sees fewer than THRESH records, and all reads
| together at least THRESH -- the weak area must read differently from
| one revolution to the next.  Position independent; loaded anywhere in
| RAM.  Everything that outlives one run (counts, the union, the copied
| reads) is in a heap block the first run allocates (`heap`, 0 until
| then), which the next run is handed back.
|
| Reassemble (binutils-m68k-linux-gnu) and copy the bytes and the label
| offsets into test.lua:
|   m68k-linux-gnu-as -m68000 -o stub.o stub.s
|   m68k-linux-gnu-objcopy -O binary stub.o stub.bin
|   m68k-linux-gnu-nm stub.o
| (The labels stay local: a global one would leave its PC-relative
| displacements to the linker, and objcopy would write them as zero.)

        .set    NIBS, 693               | nibbles one read copies
        .set    READS, 15
        .set    THRESH, 61              | 70 % of the area's 88 records
        | The heap block.
        .set    H_COUNTS, 0             | READS words: distinct records per read
        .set    H_UNION, 32             | word: distinct records over all reads
        .set    H_PASS, 34              | word: 1 when the pass rule holds
        .set    H_SEENU, 64             | 512 bytes: seen in any read
        .set    H_SEENR, 576            | 512 bytes: seen in this read
        .set    H_SEC, 1088             | 512 bytes: the _Read buffer
        .set    H_BUFS, 1600            | READS x NIBS: the reads as copied
        .set    H_SIZE, H_BUFS + READS * NIBS

        .text
start:  movea.l heap(%pc),%a3
        move.l  %a3,%d0
        bne.s   1f
        move.l  #H_SIZE,%d0
        .short  0xA31E                  | _NewPtr,CLEAR
        move.l  %a0,%d0
        beq     done                    | no memory: heap stays 0
        movea.l %a0,%a3
        lea     heap(%pc),%a0
        move.l  %a3,(%a0)
1:      lea     orig(%pc),%a0
        move.l  0x22E.w,(%a0)           | save JRdData
        lea     hook(%pc),%a1
        move.l  %a1,0x22E.w             | install the hook
        move.w  first(%pc),%d7          | read index
next:   cmp.w   last(%pc),%d7
        bge     finish
        move.w  %d7,%d0
        mulu.w  #NIBS,%d0
        lea     H_BUFS(%a3),%a1
        adda.l  %d0,%a1
        lea     dest(%pc),%a0
        move.l  %a1,(%a0)               | where this read's nibbles go
        lea     armed(%pc),%a0
        move.w  #1,(%a0)
        lea     pb(%pc),%a0
        moveq   #19,%d0
        movea.l %a0,%a1
2:      clr.l   (%a1)+                  | a clean parameter block (80 bytes)
        dbf     %d0,2b
        move.w  #-5,24(%a0)             | ioRefNum: .Sony
        move.w  #2,22(%a0)              | ioVRefNum: drive 2 (external)
        lea     H_SEC(%a3),%a1
        move.l  %a1,32(%a0)             | ioBuffer
        move.l  #512,36(%a0)            | ioReqCount
        move.w  #1,44(%a0)              | ioPosMode: fsFromStart
        move.l  posoff(%pc),46(%a0)     | ioPosOffset
        .short  0xA002                  | _Read (fails: the sector has no data field)
        lea     armed(%pc),%a0
        clr.w   (%a0)
        bsr.s   count
        addq.w  #1,%d7
        bra.s   next
finish: move.l  orig(%pc),0x22E.w       | the driver's own JRdData again
        moveq   #0,%d0                  | the pass rule
        cmp.w   #THRESH,H_COUNTS(%a3)
        bge.s   3f
        cmp.w   #THRESH,H_UNION(%a3)
        blt.s   3f
        moveq   #1,%d0
3:      move.w  %d0,H_PASS(%a3)
done:   bra.s   done

| Count read %d7's distinct records into H_COUNTS, and the new ones into
| H_UNION.
count:  lea     H_SEENR(%a3),%a0
        move.w  #127,%d0
1:      clr.l   (%a0)+
        dbf     %d0,1b
        move.w  %d7,%d0
        mulu.w  #NIBS,%d0
        lea     H_BUFS(%a3),%a2
        adda.l  %d0,%a2
        moveq   #0,%d6                  | this read's records
        move.w  #NIBS-6,%d5
2:      cmp.b   #0xF6,(%a2)
        bne.s   4f
        cmp.b   #0xAF,1(%a2)
        bne.s   4f
        cmp.b   #0x96,4(%a2)
        bne.s   4f
        cmp.b   #0xF6,5(%a2)
        bne.s   4f
        move.b  3(%a2),%d1
        cmp.b   #0x96,%d1
        beq.s   5f
        cmp.b   #0x97,%d1
        bne.s   4f
5:      moveq   #1,%d0
        and.b   %d1,%d0
        lsl.w   #8,%d0
        move.b  2(%a2),%d0              | the record's number
        lea     H_SEENR(%a3),%a0
        tst.b   0(%a0,%d0.w)
        bne.s   6f
        st      0(%a0,%d0.w)
        addq.w  #1,%d6
6:      lea     H_SEENU(%a3),%a0
        tst.b   0(%a0,%d0.w)
        bne.s   4f
        st      0(%a0,%d0.w)
        addq.w  #1,H_UNION(%a3)
4:      addq.l  #1,%a2
        dbf     %d5,2b
        move.w  %d7,%d0
        add.w   %d0,%d0
        move.w  %d6,H_COUNTS(%a3,%d0.w)
        rts

| Entered by the Sony driver with A4 at the IWM's data register, the head
| just past an address field.  Copies on the first call of each read only.
hook:   movem.l %d0-%d7/%a0-%a6,-(%sp)
        lea     armed(%pc),%a0
        tst.w   (%a0)
        beq.s   chain
        clr.w   (%a0)
        movea.l dest(%pc),%a1
        move.w  #NIBS-1,%d3
1:      move.b  (%a4),%d1
        bpl.s   1b
        move.b  %d1,(%a1)+
        dbf     %d3,1b
chain:  movem.l (%sp)+,%d0-%d7/%a0-%a6
        move.l  orig(%pc),-(%sp)        | on to the driver's own routine
        rts

        .even
heap:   .long   0
first:  .word   0
last:   .word   0
posoff: .long   0
orig:   .long   0
dest:   .long   0
armed:  .word   0
pb:     .space  80
