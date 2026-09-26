# Integration test configuration: Shell v2 language
# Pins the v2 script-language semantics (docs/core/shell/shell.md).

TEST_NAME := Shell v2 Language
TEST_DESC := Control flow, functions, bindings, ranges, none/try, templates

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
