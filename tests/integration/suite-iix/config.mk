# Integration suite: Macintosh IIx (proposal-integration-test-rework §7)
#
# One daemon run; rows re-instantiate via machine.boot and share the
# harness in ../lib/mac.script. Absorbs iix-boot, iix-floppy and
# iix-824gc, and takes the re-hosted external-floppy test (§7: the IIx
# has two floppy bays, so the external FD1 path belongs on the two-bay
# machine rather than the IIcx).
#
# §2.1 recorded that the IIx had "no pixel golden at all" — all three of
# its tests were checksum-only. This suite fixes that: the 7.0.1 row and
# the new portrait row both pixel-match, so the IIx finally has real
# pixel coverage, including the JMFB's 640x870 portrait geometry that
# nothing exercised under an OS before.
#
#   make test-suite-iix
#   make test-suite-iix TEST_VARS="ROW=iix-824gc-seat"  one row only
#   make test-suite-iix TEST_VARS="REGEN=1"             recapture goldens

TEST_NAME := IIx suite
TEST_DESC := 824GC card seat, JMFB 7.0.1 + portrait goldens (first IIx pixel tests), external-floppy row

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iix ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
