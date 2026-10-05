# Integration test: debugger reads never perturb the guest (#159).
#
# A device register whose read has a side effect -- a VBL toggle, an
# interrupt flag cleared on read -- answers an inspection through its
# peek_* (memory_interface_t): the value the read would return, and nothing
# changed.  These rows read such registers through machine.memory.peek and
# check the device state the read would have moved.

TEST_NAME := Debugger reads are side-effect-free
TEST_DESC := memory.peek of self-advancing / clear-on-read registers leaves them as they were

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
