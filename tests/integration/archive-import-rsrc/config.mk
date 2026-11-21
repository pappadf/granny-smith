# Integration test: files.archive.import sees through a wrapper whose
# archive is in its resource fork.  A MacBinary file with an empty data fork
# and a StuffIt archive as its resource fork (a .sea.bin) imports the
# archive's member, as `peeler list` shows it, not the wrapper's empty file.
TEST_NAME := Archive import through a resource-fork wrapper
TEST_DESC := files.archive.import of a .sea.bin takes the member of the StuffIt archive in its resource fork
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
