# Integration test configuration: the terminal's two forms.
# Pins how a terminal or daemon line runs (docs/internals/core/lua/lua.md,
# "The terminal"): command lines and Lua chunks, as shell.run reads them.

TEST_NAME := Terminal lines
TEST_DESC := Command lines (argument mode, assignment, $alias, templates), Lua lines, errors, continuation

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
