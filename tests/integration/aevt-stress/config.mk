# Integration test configuration: Apple event stress run.
#
# The companion to aevt-finder.  Where that test proves the channel works,
# this one leans on it: three back-to-back browses, every object-specifier
# form the Finder understands, list- and record-valued replies, both error
# paths, two applications addressed alternately, a burst of overlapping
# sends, and a final check that nothing leaked.  Around forty events, each
# taking and returning its own PPC session.
#
# System 7.5 on a IIci for the same reason as aevt-finder: the Scriptable
# Finder, and a machine with AppleTalk.  Program linking is switched on at
# runtime through the control panels, since image writes go to a per-run
# delta and a prepared variant could not persist.

TEST_NAME := Apple event stress
TEST_DESC := Boot System 7.5, enable program linking, then drive dozens of Apple events against the Finder and Find File — every specifier form, both error paths, overlapping sends — and assert nothing leaked.

TEST_ROM := roms/iici-368cadfe.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_5_0_77mb_mode32_24ac.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := model=iici ram=8192 hd=$(TEST_TMPDIR)/hd.img

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
