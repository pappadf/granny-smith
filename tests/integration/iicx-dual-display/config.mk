# Integration test configuration: IIcx dual display (stage 2 multi-card).
#
# Two video cards on one machine: the default 8•24 (JMFB) in socket $9 plus
# an 8•24 GC staged into socket $A via the per-slot config surface
# (machine.nubus.slot[10].card_id — proposal-nubus-computed-card-compatibility.md
# §5.6).  Before stage 2 the slot table froze every non-video slot as EMPTY
# and the single pending pick could only populate one slot.

TEST_NAME := IIcx dual display
TEST_DESC := Boot the IIcx with two video cards (JMFB in $9 + 8•24 GC staged into $A) and pin the dual-card contract

TEST_ROM := roms/IIcx.rom

# The harness boots the IIcx once; the script stages socket $A and re-boots.
TEST_ARGS := model=iicx ram=8192
