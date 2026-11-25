| SPDX-License-Identifier: MIT
| Copyright (c) pappadf
|
| Guest stub for the plus-moof-protection row: hook the Sony driver's
| data-field read (JRdData, $022E), _Read one block, and scan the nibbles
| the head delivers from where the driver left it -- what a disk's copy
| protection does.  Position independent; loaded anywhere in RAM.
|
| Reassemble (binutils-m68k-linux-gnu) and copy the bytes and the label
| offsets into test.lua:
|   m68k-linux-gnu-as -m68000 -o stub.o stub.s
|   m68k-linux-gnu-objcopy -O binary stub.o stub.bin
|   m68k-linux-gnu-nm stub.o
| (The labels stay local: a global one would leave its PC-relative
| displacements to the linker, and objcopy would write them as zero.)
|
| mode 1: find a $DE immediately followed by D5 AA AD (a signature in front
|         of a data mark).
| mode 2: find B5 96 FA B5, then DE AA, then count the $FF nibbles up to the
|         next one, and keep that one.

        .text
start:  lea     orig(%pc),%a0
        move.l  0x22E.w,(%a0)           | save JRdData
        lea     hook(%pc),%a1
        move.l  %a1,0x22E.w             | install the hook
        lea     pb(%pc),%a0
        moveq   #19,%d0
        movea.l %a0,%a1
1:      clr.l   (%a1)+                  | a clean parameter block (80 bytes)
        dbf     %d0,1b
        move.w  #-5,24(%a0)             | ioRefNum: .Sony
        move.w  #1,22(%a0)              | ioVRefNum: drive 1
        lea     buf(%pc),%a1
        move.l  %a1,32(%a0)             | ioBuffer
        move.l  #512,36(%a0)            | ioReqCount
        move.w  #1,44(%a0)              | ioPosMode: fsFromStart
        move.l  posoff(%pc),46(%a0)     | ioPosOffset
        .short  0xA002                  | _Read
        lea     rdres(%pc),%a1
        move.w  %d0,(%a1)
        move.l  orig(%pc),0x22E.w       | the driver's own JRdData again
done:   bra.s   done

| Entered by the Sony driver with A4 at the IWM's data register, the head
| just past an address field.  Scans on the first call only.
hook:   movem.l %d0-%d7/%a0-%a6,-(%sp)
        lea     ran(%pc),%a0
        tst.w   (%a0)
        bne.s   chain
        move.w  #1,(%a0)
        move.w  #0x3000,%d3             | nibbles to scan: over a revolution
        moveq   #0,%d4                  | the last four nibbles
        move.w  mode(%pc),%d5
2:      move.b  (%a4),%d1
        bpl.s   2b
        lsl.l   #8,%d4
        move.b  %d1,%d4
        cmp.w   #1,%d5
        bne.s   3f
        cmp.l   #0xDED5AAAD,%d4         | mode 1: the signature
        beq.s   sig
        bra.s   4f
3:      cmp.l   #0xB596FAB5,%d4         | mode 2: the marker
        beq.s   mark
4:      dbf     %d3,2b
        bra.s   chain
sig:    lea     found(%pc),%a0
        move.w  #1,(%a0)
        bra.s   chain
mark:   lea     found(%pc),%a0
        move.w  #1,(%a0)
        moveq   #0,%d4
        moveq   #1,%d3                  | DE AA
5:      move.b  (%a4),%d1
        bpl.s   5b
        lsl.w   #8,%d4
        move.b  %d1,%d4
        dbf     %d3,5b
        lea     mid(%pc),%a0
        move.w  %d4,(%a0)
        moveq   #0,%d2                  | $FF nibbles counted
6:      move.b  (%a4),%d1
        bpl.s   6b
        cmp.b   #0xFF,%d1
        bne.s   7f
        addq.w  #1,%d2
        cmp.w   #1000,%d2
        bne.s   6b
7:      lea     count(%pc),%a0
        move.w  %d2,(%a0)
        lea     next(%pc),%a0
        move.b  %d1,1(%a0)
chain:  movem.l (%sp)+,%d0-%d7/%a0-%a6
        move.l  orig(%pc),-(%sp)        | on to the driver's own routine
        rts

        .even
orig:   .long   0
mode:   .word   0
ran:    .word   0
found:  .word   0
mid:    .word   0
count:  .word   0
next:   .word   0
rdres:  .word   0
posoff: .long   0
pb:     .space  80
buf:    .space  512
