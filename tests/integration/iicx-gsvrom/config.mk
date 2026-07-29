# Integration suite: IIcx on the generic GS declaration ROM
# (proposal-integration-test-rework §7)
#
# One daemon run; rows re-instantiate via machine.boot and share the
# harness in ../lib/mac.script. Absorbs iicx-gsvrom-video-modes,
# iicx-gsvrom-24ac, iicx-gsvrom-824gc and iicx-gsvrom-custom-mode.
#
# Every row here runs a card on its RUNTIME-GENERATED declaration ROM
# rather than a real dump — the other half of §3.2's "card x vROM-source"
# plane, whose real-vROM half is iicx-video-modes. The rows all name
# their card and mode explicitly, so none can inherit video staging from
# the row above it.
#
#   make test-iicx-gsvrom
#   make test-iicx-gsvrom TEST_VARS="ROW=gsvrom-24ac"   one row only
#   make test-iicx-gsvrom TEST_VARS="REGEN=1"           recapture goldens

TEST_NAME := IIcx generic-vROM video suite
TEST_DESC := 10-cell JMFB sweep, 24AC 640x480 + 832x624, 8*24 GC bring-up, 800x600 custom mode — all on generated vROMs

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
