# Integration suite: the AV family (Quadra 840AV / Centris 660AV)
# (proposal-quadra-av.md §5.1)
#
# One daemon run; rows re-instantiate via machine.boot with every staging
# argument named, and the shared library in ../lib/mac.script provides the
# harness (row filter, keep-going, REGEN, media gating).
#
# Both models share the 2 MB $5BF10FD1 ROM, so every row passes rom=
# explicitly rather than relying on the suite default — which is why
# TEST_ROM below names an already-fetched ROM the rows never boot.  The
# harness requires TEST_ROM to exist before it will start the daemon at
# all, so pointing it at the AV image would turn "the AV ROM has not
# reached gs-test-data yet" into a hard suite failure instead of the
# clean per-row skip the landable-before-data pattern calls for
# (proposal-quadra-av.md §5.2).  When roms/q840av-q660av-5bf10fd1.rom
# lands, this line can move to it; nothing else changes.
#
#   make test-suite-av
#   make test-suite-av TEST_VARS="ROW=q840av-identity"   one row only
#   make test-suite-av TEST_VARS="KEEP_GOING=1"          nightly mode
#   make test-suite-av TEST_VARS="REGEN=1"               recapture goldens

TEST_NAME := AV suite (Quadra 840AV / 660AV)
TEST_DESC := Cyclone/Tempest identity + CD boot to the Finder desktop

TEST_ROM := roms/q700-q900-420dbff3.rom
TEST_ARGS := model=q700 ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
