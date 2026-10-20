# Integration test configuration: the Lisa boot ROM's video self-test at
# every memory size, at 1x.
#
# VIDCHK waits ~20 ms of instruction loop for a vertical retrace and
# reports CPU error 42 when none comes (S1018-54).  At the Lisa's old 4
# cycles per instruction the loop lasted under half a frame, so whether it
# passed depended on where in the frame it started -- 512 KB failed every
# time, 1 MB passed by 32 cycles.  The row runs the Lisa 2 and Macintosh
# XL ROMs to the end of the test at 512 KB, 1 MB and 2 MB, at 1x.  An
# accelerated CPU shortens the loop and can still fail it, as a faster
# real Lisa would, so a Lisa is accelerated only after its boot ROM.

TEST_NAME := Lisa boot ROM video self-test (VIDCHK)
TEST_DESC := VIDCHK passes on the Lisa 2 and Macintosh XL at 512 KB, 1 MB and 2 MB, at 1x

TEST_ROM := roms/lisa2-revh-098917b2.rom
TEST_ARGS := model=lisa ram=1024

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
