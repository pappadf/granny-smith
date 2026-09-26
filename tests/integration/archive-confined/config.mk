# Integration test: archive extraction stays inside its output directory.
# A StuffIt 5 archive whose one file is named "../escape" -- a legal Mac
# name, '/' being an ordinary character on HFS -- must extract as
# "..:escape" inside the output directory, not as "escape" beside it.
TEST_NAME := Archive extraction is confined
TEST_DESC := archive.extract of an entry named "../escape" lands inside the output directory
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
