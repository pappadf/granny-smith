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
TEST_ROM := roms/IIcx.rom

# MacTest IIcx/IIci floppy disk image.
# Pin RAM at 4 MB to match the SE/30 baseline so MacTest's RAM-test reports
# don't drift across screenshots if the IIcx profile RAM default changes.
TEST_ARGS := model=iicx ram=4096 fd0=$(TEST_DATA)/apps/MacTest-IIcx-IIci.image
