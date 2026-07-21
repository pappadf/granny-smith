# Integration test configuration: IIx + Display Card 8•24 GC.
#
# Guards the computed card-compatibility rule (nubus_card_fits_socket): the
# IIx declares no per-card whitelist, so the 8•24 GC — validated on the IIcx
# — must seat and light the boot screen on the IIx too.  Before
# proposal-nubus-computed-card-compatibility.md stage 1 this pick was
# silently rejected (the IIx whitelist only carried mdc_8_24) and the boot
# fell back to the JMFB, which this test's card-identity assert would catch.

TEST_NAME := IIx Display Card 8•24 GC boot
TEST_DESC := Boot the IIx with the 8•24 GC selected (computed compatibility) and verify the card seats and paints

# Universal IIx/IIcx/SE/30 ROM; the 8•24 GC vROM (824gc-v1.1-revb-d722b053.vrom) is
# found next to it by canonical catalog name.
TEST_ROM := roms/iix-iicx-se30-97221136.rom

# The harness boots the IIx once; the script re-boots with the 8•24 GC
# selected (video_card can't be passed as a startup arg).
TEST_ARGS := model=iix ram=8192
