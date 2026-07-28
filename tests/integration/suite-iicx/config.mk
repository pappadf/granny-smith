# Integration suite: Macintosh IIcx (proposal-integration-test-rework §7)
#
# The IIcx's share after §7's host-redistribution pass is deliberately
# small: its ADB keyboard test moved to the IIsi (Egret), its external-
# floppy test to the IIx (two bays), and its 24AC suite to the IIci (24AC
# beside a live RBV). What stays IIcx-hosted lives in its own directories
# because each is about a card rather than the machine: iicx-video-modes
# (the 16-cell real-vROM JMFB sweep), the iicx-gsvrom-* family, the
# iicx-824gc-* accel/decline oracles, iicx-display-card-24ac and
# iicx-dual-display.
#
# This suite therefore holds the machine-level rows: the chime golden
# now, and the boot rows folded in as the remaining §7 consolidations
# land.
#
#   make test-suite-iicx
#   make test-suite-iicx TEST_VARS="REGEN=1"   recapture goldens

TEST_NAME := IIcx suite
TEST_DESC := Machine-level IIcx rows: sample-exact boot chime

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
