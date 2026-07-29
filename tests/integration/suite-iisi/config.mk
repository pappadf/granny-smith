# Integration suite: Macintosh IIsi (proposal-integration-test-rework §7)
#
# One daemon run; rows re-instantiate via machine.boot and share the
# harness in ../lib/mac.script. Absorbs iisi-boot and iisi-boot-chime,
# and takes the re-hosted ADB keyboard test (§7's host re-assignment
# pass: on the IIsi, ADB runs through the Egret transceiver rather than
# the Universal ROM's VIA path the test used to exercise on the IIcx —
# a genuinely different transceiver, and the IIsi is builtin-video only
# so it is the natural home for a machine-agnostic test).
#
# The IIsi publishes only four RAM totals (5 / 9 / 17 / 65 MB — the
# 1+4, 1+8, 1+16 and 1+64 two-bank splits), so the RAM axis here is a
# clean spread: one row per option, with the 65 MB maximum on the cheap
# chime row where it costs nothing but still exercises RAM sizing.
#
#   make test-suite-iisi
#   make test-suite-iisi TEST_VARS="ROW=iisi-chime"   one row only
#   make test-suite-iisi TEST_VARS="REGEN=1"          recapture goldens

TEST_NAME := IIsi suite
TEST_DESC := Chime WAV, V8 7.0.1 boot, 6.0.8 HD row, ADB-via-Egret keyboard row, 7.5 HD boot

TEST_ROM := roms/iisi-36b7fb6c.rom
TEST_ARGS := model=iisi ram=17408

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
