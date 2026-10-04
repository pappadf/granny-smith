# Integration test: the configuration document's validation (V1-V9).
#
# machine.boot config= is validated in full before anything is built, and a
# rejection names the node at fault.  One case per rule, each a separate
# headless run (a rejected boot stops the script that issued it), each
# checked for its exact message; then the same documents' valid twins boot.

TEST_NAME := Configuration document validation
TEST_DESC := machine.boot config= rejects each V1-V9 violation with a message naming the node, and accepts a valid document

TEST_ROM := roms/iici-368cadfe.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
