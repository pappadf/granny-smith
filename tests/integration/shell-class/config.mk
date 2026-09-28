# Integration test: Shell class on the object root
# Verifies that the `shell.*` surface — attributes (prompt / running /
# aliases / vars) and methods (run / complete / expand / script_run /
# alias_set / alias_unset / interrupt) — is reachable through gs_eval
# and behaves as docs/core/shell/shell.md describes.

TEST_NAME := Shell class surface
TEST_DESC := shell.{run,complete,expand,alias_set,alias_unset,vars,aliases,prompt,running} via gs_eval

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
