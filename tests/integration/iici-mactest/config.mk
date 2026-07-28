# RE-HOSTED IIcx -> IIci (§7): the media is "MacTest-IIcx-IIci", so the
# IIci is the other target it was written for, and running it here means
# the diagnostic exercises the MDU/RBV board instead of a second GLUE
# machine. Video comes from the built-in RBV rather than a NuBus card.
# Integration test configuration: IIci MacTest Boot
# Opens Options > Test Selections first, unchecks the ADB Communication
# (Keyboard + Mouse) sub-tests so they don't gate the floppy phase on
# emulated keyboard/mouse hardware we don't fully simulate, then runs
# the full IIci MacTest floppy suite.  Also serves as regression for
# the Options-dialog interaction recorded in
# tests/integration/iicx-mactest-options.

TEST_NAME := IIci MacTest Boot
TEST_DESC := Disables ADB Communication tests via Options > Test Selections, then runs the IIci MacTest floppy suite

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/iici-368cadfe.rom

# MacTest IIcx/IIci floppy disk image.
# Pin RAM at 4 MB to match the SE/30 baseline so MacTest's RAM-test reports
# don't drift across screenshots if the IIci profile RAM default changes.
TEST_ARGS := model=iici ram=8192 fd0=$(TEST_DATA)/apps/MacTest-IIcx-IIci.image

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
