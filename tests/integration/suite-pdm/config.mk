# Integration suite: the PDM family (Power Macintosh 6100/7100/8100)
# (proposal-powerpc-601-pdm.md, Phase G acceptance rows)
#
# One daemon run; rows re-instantiate via machine.boot (the boot-matrix
# pattern, suite-quadra shape) and ../lib/mac.script provides the harness
# (row filter, keep-going, REGEN, condition-based waits).
#
# Each row cold-boots System 7.5 from the prepared SCSI image to the
# Finder desktop and pixel-matches a desktop golden and an "About This
# Macintosh" golden (About proves the Gestalt machine identity end-to-end
# — the three desktops are otherwise pixel-identical).  The rows skip
# (with an echo) when the prepared image has not been fetched.
#
# Runtime note: each boot retires ~1.6 G instructions through the 601
# interpreter (~15 min/row at current native speed) — the 20 s
# WaitForPollDrive spin-up window is skipped by the documented Ticks
# poke, or each row would cost another ~1.2 G.
#
#   make test-suite-pdm
#   make test-suite-pdm TEST_VARS="ROW=6100-75-hd"     one row only
#   make test-suite-pdm TEST_VARS="KEEP_GOING=1"       nightly mode
#   make test-suite-pdm TEST_VARS="REGEN=1"            recapture goldens

TEST_NAME := PDM suite (6100/7100/8100)
TEST_DESC := System 7.5 SCSI boot to the Finder desktop + About box, all three models, plus a 24AC in a NuBus slot

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

TEST_ARGS := model=pm6100 ram=24576

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
