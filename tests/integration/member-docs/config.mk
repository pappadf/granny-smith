# Integration test: member documentation lint.
# Boots every registered machine, runs shell.lint_members(), and fails on any
# documentation gap not listed in allow.txt.  The allow-list may only shrink.
TEST_NAME := Member docs
TEST_DESC := shell.lint_members reports no documentation gap outside allow.txt, on every model
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
