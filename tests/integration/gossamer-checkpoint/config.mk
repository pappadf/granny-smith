# Integration test: beige G3 checkpoint save/restore.  Step 1 boots the
# Mac OS 9.2.1 disk halfway (extensions loading: MESH, DBDMA, the Rage Pro
# accelerator, Screamer and VM all carry live state) and saves; step 2
# restores in a fresh process, finishes the boot and must reach the same
# pixel-exact desktop the straight suite-gossamer g3dt-hd run pins.

TEST_NAME := Gossamer checkpoint save/restore
TEST_DESC := Beige G3 save-state: mid-boot save, cross-process restore, desktop pixel-match

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/mac_os_9_2_1_516mb_g3.img" "$(TEST_TMPDIR)/hd.img"

TEST_RUNNER := run.sh

TEST_TIMEOUT := 3600

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
# Under `make test`, step 1 is suite-gossamer's g3dt row: it saves the same
# 6 G mid-boot state, and this test restores that file instead of booting
# there itself.  Run alone, it does both steps.
TEST_NEEDS := g3dt-921-midboot

TEST_TIER := matrix
