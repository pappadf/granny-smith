# Integration test: object-model class registration (08-core-infra G2)
# Boots every registered machine and fails if object_validate_class rejected
# any class descriptor that reached the tree.
TEST_NAME := Object Class Registry
TEST_DESC := every class every machine registers passes object_validate_class
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
