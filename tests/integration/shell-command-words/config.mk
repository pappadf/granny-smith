# Integration test: commands -- bare words that run a method.
#
# The built-ins (ls, cd, pwd, run, …) run their methods; `command NAME = PATH`
# declares one, refusing a target that is not a method and a name that is a
# root path; a tree path wins over a command; help, completion and
# highlighting follow the method a command runs.

TEST_NAME := Shell commands (bare words)
TEST_DESC := built-in and declared commands: dispatch, cd/pwd, errors, help, completion, highlighting

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
