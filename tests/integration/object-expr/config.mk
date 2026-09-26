# Integration test: $(...) expressions in the legacy shell
# Exercises the new expression substitution and predicate `assert`.

TEST_NAME := Object-model expressions (Plus)
TEST_DESC := $(...) substitution and predicate assert in the legacy shell

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
