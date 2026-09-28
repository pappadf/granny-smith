# Integration test: every test-data ROM is one the core knows.
#
# Enumerates every file in tests/data/roms and, via machine.(v|p)rom.identify,
# asserts:
#   1. the file is RECOGNISED by its surface (no unknown blobs in the directory);
#   2. every CPU ROM is INTACT (its own stored checksum verifies — this is also
#      the permanent check on reconstructed images) and SUPPORTED (its row in
#      the core's ROM table names at least one emulated model).
# Test-data filenames are free-form labels: nothing here parses them.
#
# Uses a run.sh runner because the object-model shell has no directory
# enumeration — the runner lists the dir in bash and drives one headless
# identify pass over all files.  TEST_ROM is just the harness's boot ROM.

TEST_NAME := ROM/vROM/PROM test-data catalog
TEST_DESC := every tests/data/roms file is recognised; every CPU ROM is intact and supported

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
