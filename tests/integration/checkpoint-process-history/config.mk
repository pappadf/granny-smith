# Integration test: a checkpoint restores to the same state whatever the
# restoring process did first.  The I/O wait-state remainder is scheduler
# state, checkpointed with it; it used to be a process global that carried
# from machine to machine, so a restore depended on process history.

TEST_NAME := Checkpoint restore is independent of process history (IIcx)
TEST_DESC := Save mid-run; restore in a fresh process and after running a Quadra 700; all three runs end in the same state

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
