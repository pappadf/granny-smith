# Integration test: the removed machine-, bus- and card-specific special cases
# stay removed.
#
# The machine configuration moved every such case into the formal
# descriptors (the profile's tree, the card kinds, the boot document).  This
# lists the names each one went by -- functions, globals, fields, ids -- and
# fails if any comes back.

TEST_NAME := Removal inventory
TEST_DESC := Special cases the configuration tree replaced stay absent from src/ and app/web2/src/

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
