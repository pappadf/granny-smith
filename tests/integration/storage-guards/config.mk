# Integration test configuration: storage.rm / storage.mv guards
# Verifies the worker-mutation safety guards: protected paths (/, /opfs),
# move-into-own-subtree, and existing-destination refusal — plus that a
# legitimate move still works. Erroring object-model calls set a non-zero
# script exit code by design, so a custom runner drives the probes and
# matches the refusal output instead of relying on the exit code.

TEST_NAME := storage.rm/mv guards
TEST_DESC := Protected paths, self-move, and existing-destination refusals

TEST_ROM := roms/Plus_v3.rom
TEST_RUNNER := run.sh
