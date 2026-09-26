# Integration test: watchpoints stop the machine on a data access (#180).
#
# A watchpoint is a stopping memory logpoint: the same access hook and page
# refcounts as debug.logpoints, its own collection.  The Plus ROM's VBL
# handler writes Ticks ($16A) sixty times a second, so a write watchpoint
# there stops the machine within a frame.

TEST_NAME := Watchpoints
TEST_DESC := debug.watchpoints stops the machine after a write to Ticks, is listed apart from logpoints, honours enabled and remove

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
