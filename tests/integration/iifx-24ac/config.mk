# Integration test configuration: IIfx + Display Card 24AC.
#
# Guards the computed card-compatibility rule (nubus_card_fits_socket) on the
# OSS-family IIfx: no per-card whitelist exists, so the 24AC — validated on
# the IIcx — must seat and light the boot screen on the IIfx too.  Before
# proposal-nubus-computed-card-compatibility.md stage 1 this pick was
# silently rejected (the IIfx whitelist only carried mdc_8_24) and the boot
# fell back to the JMFB, which this test's card-identity assert would catch.
#
# (The 8•24 GC is the deliberate exception: its decl-ROM video driver does
# not come up on the IIfx yet — a pre-existing card-model gap the whitelist
# used to hide; see the proposal §8 and the iix-824gc test for the GLUE-side
# coverage.)

TEST_NAME := IIfx Display Card 24AC boot
TEST_DESC := Boot the IIfx with the 24AC selected (computed compatibility) and verify the card seats and paints

# Dedicated IIfx ROM; the 24AC vROM (display-card-24ac.vrom) is found next
# to it by canonical catalog name.
TEST_ROM := roms/4147DD77-IIfx.rom

# The harness boots the IIfx once; the script re-boots with the 24AC
# selected (video_card can't be passed as a startup arg).
TEST_ARGS := model=iifx ram=8192
