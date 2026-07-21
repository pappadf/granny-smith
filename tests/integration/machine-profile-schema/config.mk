# Integration test: machine.profile() schema snapshot (proposal §6.1)
# Pins the SHAPE (keys + value types, value-independent) of every model's
# machine.profile() JSON against a committed golden, so adding/removing/
# retyping a field fails loudly. Complements machine-capabilities, which
# asserts specific capability *values*.

TEST_NAME := Machine Profile Schema Snapshot
TEST_DESC := machine.profile() JSON shape pinned per model (added/removed/retyped field => fail)

# Any ROM works — machine.profile() is a static registry lookup, no boot.
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh
