# STAYS ON THE IIcx. §7 re-hosted this to the IIci (the media is
# "MacTest-IIcx-IIci", so the IIci is the other machine it was written for) and
# the re-host was REVERTED after measurement: on the emulated IIci, MacTest
# fails immediately after Start with "SUSPECTED PROBLEM: Logic board", and every
# checkpoint from the floppy phase onward is that one modal dialog. That is a
# real defect, tracked as proposal-emulator-bug-fixes.md §9 and guarded by
# suite-iici's iici-mactest-diag milestone row; it is not something a golden
# recapture may paper over. The IIcx runs the full flow, so the coverage lives
# here.
#
# Integration test configuration: IIcx MacTest Boot
# Opens Options > Test Selections first, unchecks the ADB Communication
# (Keyboard + Mouse) sub-tests so they don't gate the floppy phase on
# emulated keyboard/mouse hardware we don't fully simulate, then runs
# the full IIcx MacTest floppy suite.  Also serves as regression for
# the Options-dialog interaction recorded in
# tests/integration/iicx-mactest-options.

TEST_NAME := IIcx MacTest Boot
TEST_DESC := Disables ADB Communication tests via Options > Test Selections, then runs the IIcx MacTest floppy suite

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/iix-iicx-se30-97221136.rom

# MacTest IIcx/IIci floppy disk image.
# Pin RAM at 4 MB to match the SE/30 baseline so MacTest's RAM-test reports
# don't drift across screenshots if the IIcx profile RAM default changes.
TEST_ARGS := model=iicx ram=4096 fd0=$(TEST_DATA)/apps/MacTest-IIcx-IIci.image

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
