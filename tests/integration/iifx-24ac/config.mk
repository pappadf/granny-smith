# Integration test configuration: IIfx + Display Card 24AC (ROM-only).
#
# Guards the computed card-compatibility rule (nubus_card_fits_socket) on
# the OSS-family IIfx: no per-card whitelist exists, so the 24AC must seat
# in slot $9 when picked.  Before
# proposal-nubus-computed-card-compatibility.md stage 1 this pick was
# silently rejected (the IIfx whitelist only carried mdc_8_24) and the boot
# fell back to the JMFB, which this test's card-identity assert would catch.
#
# The screen assertion pins the ROM-only dummy-screen contract (see
# test.script); the real Slot-Manager/driver path on the IIfx is covered by
# the System 7 boot in iifx-824gc.

TEST_NAME := IIfx Display Card 24AC boot
TEST_DESC := Boot the IIfx with the 24AC selected (computed compatibility) and verify the card seats and the boot screen shows

# Dedicated IIfx ROM; the 24AC vROM (display-card-24ac.vrom) is found next
# to it by canonical catalog name.
TEST_ROM := roms/4147DD77-IIfx.rom

# The harness boots the IIfx once; the script re-boots with the 24AC
# selected (the ROM-only phase needs no disk).
TEST_ARGS := model=iifx ram=8192
