# Integration suite: the AV family (Quadra 840AV / Centris 660AV)
# (proposal-quadra-av.md §5.1)
#
# One daemon run; rows re-instantiate via machine.boot with every staging
# argument named, and the shared library in ../lib/mac.script provides the
# harness (row filter, keep-going, REGEN, media gating).
#
# Both models share the 2 MB $5BF10FD1 ROM, so every row passes rom=
# explicitly rather than relying on the suite default; TEST_ROM only has to
# name a ROM that exists, because the harness requires it before it will
# start the daemon at all.  It now names the AV ROM itself, which landed in
# gs-test-data at revision 2937e56.
#
#   make test-suite-av
#   make test-suite-av TEST_VARS="ROW=q840av-identity"   one row only
#   make test-suite-av TEST_VARS="KEEP_GOING=1"          nightly mode
#   make test-suite-av TEST_VARS="REGEN=1"               recapture goldens

TEST_NAME := AV suite (Quadra 840AV / 660AV)
TEST_DESC := Cyclone/Tempest identity + 7.1 HD boot to the Finder desktop

TEST_ROM := roms/q840av-q660av-5bf10fd1.rom
TEST_ARGS := model=q840av ram=16384

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
