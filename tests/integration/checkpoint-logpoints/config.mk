# Integration test: memory logpoints fire after checkpoint.load (#172).
#
# checkpoint.load builds the new machine before destroying the old one.  The
# old debug object's teardown cleared the process-global memory-logpoint
# hook the new one had just installed, so no memory logpoint fired after a
# load -- not even one added afterwards -- while PC logpoints, which do not
# use the hook, kept working.  The hook is now released only by the debug
# object that installed it.

TEST_NAME := Checkpoint keeps memory logpoints working
TEST_DESC := a memory logpoint added after checkpoint.load fires; the load itself drops the previous machine's probes

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
