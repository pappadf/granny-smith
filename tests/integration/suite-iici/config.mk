# Integration suite: Macintosh IIci (proposal-integration-test-rework §7)
#
# One daemon run; rows re-instantiate via machine.boot and share the
# harness in ../lib/mac.script. Absorbs iici-boot and iici-boot-chime,
# and adds the IIci's first HD-boot coverage: 7.5, plus the **second 7.6
# consumer**, which §7 moved here from the IIcx precisely because the
# IIci is a clean-ROM 32-bit machine — 7.6 selects 32-bit addressing
# itself, so the cell is legal by construction and open question q.4
# dissolves.
#
# The RBV depth sweep §3.2 wants (2/4/8 bpp) is deliberately absent: the
# built-in video is stuck at 1 bpp because it exposes no declaration ROM,
# so the guest has no depth mode list to choose from (measured
# 2026-07-28 — slot[11].card.declrom.present is false, and even a VALID
# slot-PRAM record with BoardID $001F and depth $83 comes back rewritten
# to $80). Tracked as an emulator defect in
# proposal-emulator-bug-fixes.md §5, with the depth rows to be written
# once it is fixed — through the Monitors control panel, the pattern
# suite-quadra's Thousands row proves.
#
#   make test-suite-iici
#   make test-suite-iici TEST_VARS="ROW=iici-chime"   one row only
#   make test-suite-iici TEST_VARS="REGEN=1"          recapture goldens

TEST_NAME := IIci suite
TEST_DESC := Chime WAV, RBV 7.0.1 boot + About, 7.5 HD, 7.6 HD (32-bit), MDU checkpoint round-trip

TEST_ROM := roms/iici-368cadfe.rom
TEST_ARGS := model=iici ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
