# Integration test: object-model metadata (object-model proposal, phase 2).
#
# meta.members exports a type descriptor for attributes and arguments, the
# effective (inherited) task, node docs, collection shape and root domains;
# shell.usage / help render one usage text from it; shell.complete with
# detail says what each candidate is and which method argument the cursor is
# in.  The usage texts are compared with golden files.
TEST_NAME := Object model metadata
TEST_DESC := meta.members type/task/doc/collection keys, shell.usage goldens, shell.complete detail and context
TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
