# Integration suite: the compact Macintosh models beside the Plus
#
# The Macintosh 128K, 512K and 512K enhanced (PAL boards, quadrature mouse,
# floppy only), and the SE, SE FDHD and Classic (BBU boards, ADB, SCSI).
# Every row boots to the Finder, opens the About box from the Apple menu and
# matches it against a golden, and asserts the machine and ROM ids.  The
# Plus keeps its own suite (suite-plus).
#
#   make test-suite-compact
#   make test-suite-compact TEST_VARS="ROW=se-608-fd"    one row only
#   make test-suite-compact TEST_VARS="REGEN=1"          recapture goldens

TEST_NAME := Compact Macintosh suite
TEST_DESC := 128K/512K/512Ke floppy rows, SE/SE FDHD/Classic floppy and SCSI rows, Classic ROM disk, About boxes

# Each row names its own ROM; this one starts the daemon.
TEST_ROM := roms/se-b2e362a8.rom
TEST_ARGS := model=se ram=4096

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
