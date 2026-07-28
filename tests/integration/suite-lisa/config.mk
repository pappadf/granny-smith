# Integration suite: Lisa 2 and Macintosh XL (proposal-integration-test-rework §7)
#
# One daemon run; rows re-instantiate via machine.boot and share the
# harness in ../lib/mac.script. Absorbs lisa-profile-boot,
# lisa-xenix-hdinit, lisa-xenix-boot, lisa-xenix-nofloppy, xl-boot and
# xl-no-media.
#
# This is pure consolidation — six directories become one daemon run with
# no new matrix cells, because the Lisa column is already complete (LOS
# 3.1, Xenix 3.0, MacWorks 3.0 are the only systems these machines run,
# and all three were covered). What it buys is one process instead of
# six, and the row harness: ROW= to reproduce a single flow, REGEN= to
# recapture, and per-row perf baselines.
#
# The row bodies live in rows.script and were transcribed by script, not
# by hand: the Xenix flows are long exact COPS scan-code sequences and
# retyping them would be the most likely way to break this suite.
#
# TEST_SETUP prepares both ProFile images (the rows attach them from
# WORK_DIR so the originals stay pristine) and seeds the LOS PRAM.
#
#   make test-suite-lisa
#   make test-suite-lisa TEST_VARS="ROW=xl-no-media"   one row only
#   make test-suite-lisa TEST_VARS="REGEN=1"           recapture goldens

TEST_NAME := Lisa 2 / Macintosh XL suite
TEST_DESC := LOS 3.1 ProFile boot, Xenix 3.0 hdinit + boot + no-floppy, MacWorks XL boot + no-media

TEST_ROM := roms/lisa2-revh-098917b2.rom
TEST_ARGS := model=lisa ram=2048

TEST_SETUP := cp "$(TEST_DATA)/Lisa/LisaOfficeSystem-3.1/LOS-3.1-ProFile.image" "$(WORK_DIR)/profile.image" && cp "$(TEST_DATA)/Lisa/Xenix-3.0/Xenix-3.0-ProFile.image" "$(WORK_DIR)/xenix-profile.image" && python3 suite-lisa/seed_pram.py "$(WORK_DIR)/profile.pram" --coldboot

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
