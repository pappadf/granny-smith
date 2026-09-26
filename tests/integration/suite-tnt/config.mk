# Integration suite: the TNT family's System 7.6 rows (Power Macintosh
# 7500/8500/9500)
#
# One daemon run; rows re-instantiate via machine.boot (the suite-pdm
# shape) and ../lib/mac.script provides the harness (row filter,
# keep-going, REGEN, condition-based waits).  Absorbs the former
# tnt-hd-boot, tnt-hd-boot-8500, tnt-hd-boot-9500 and
# tnt-pci-voodoo2-display directories; their assertions live on as rows
# (see test.script's row map).
#
# Each row attaches the published 7.6 image directly: every writable
# attach mints its own delta instance under the test's storage cache, so
# no row sees another row's writes and the base image is never touched.
#
#   make test-suite-tnt
#   make test-suite-tnt TEST_VARS="ROW=pm8500-76-hd"     one row only
#   make test-suite-tnt TEST_VARS="KEEP_GOING=1"         nightly mode
#   make test-suite-tnt TEST_VARS="REGEN=1"              recapture goldens

TEST_NAME := TNT suite (7500/8500/9500)
TEST_DESC := System 7.6 MESH-disk boots to the Finder with the startup chime on all three models (the 9500 on a PCI display card), plus the Voodoo2 pass-through switch

# 4 MB Power Macintosh 7500/8500/9500 ROM (stored checksum 0x96CD923D).
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_ARGS := model=pm7500 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
