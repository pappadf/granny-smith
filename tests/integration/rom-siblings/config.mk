# Integration test: a script's machine.boot rom=<other directory> offers that
# directory's card ROMs (#187).
#
# Headless offers the *.vrom / *.prom files beside the ROM it was started
# with.  A later machine.boot from a script did not repeat the walk for the
# new ROM's directory, so a ROM booted from elsewhere, with its card ROMs
# next to it, found no offers: an explicitly picked card that needs a
# declaration ROM failed the strict check.  machine_boot_apply now asks the
# platform to offer the siblings of every ROM it boots.

TEST_NAME := Card ROMs beside a script-booted ROM
TEST_DESC := machine.boot rom=<dir without the CLI ROM> resolves the 8•24 GC declaration ROM kept beside it

# The runner requires one; run.sh starts headless on a copy elsewhere.
TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
