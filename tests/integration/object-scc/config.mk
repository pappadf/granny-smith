# Integration test: scc object class (M7a)
# Verifies the new `scc.*` paths against the same backing state as the
# legacy `scc loopback` command. SE/30 boots cleanly so the SCC is
# fully populated by the time the script runs.

TEST_NAME := Object-model SCC class
TEST_DESC := scc.loopback / scc.reset / scc.{a,b}.* expose the SCC peripheral

TEST_ROM := roms/Plus_v3.rom
