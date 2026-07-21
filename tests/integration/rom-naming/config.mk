# Integration test: canonical ROM/vROM filename conformance (proposal-test-rom-naming.md §4.3).
#
# The enforcement that makes the reorganization stick.  Enumerates every file
# in tests/data/roms and, via machine.(v)rom.identify, asserts:
#   1. the file is RECOGNISED (no unknown blobs may live in the directory);
#   2. its basename EQUALS the canonical name the tooling grammar
#      (scripts/rom_naming.py) derives from its content id (no drift);
#   3. no two files share a checksum/CRC (kills duplicate blobs forever).
#
# Uses a run.sh runner because the object-model shell has no directory
# enumeration — the runner lists the dir in bash and drives one headless
# identify pass over all files.  TEST_ROM is just the harness's boot ROM.

TEST_NAME := ROM/vROM canonical filename conformance
TEST_DESC := every tests/data/roms file is recognised, canonically named, and checksum-unique

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh
