# Integration test: re.disasm_code + per-CODE disassembly output of
# re.dump (PR 3 of the resource-fork-as-VFS-tree proposal).  Drives the
# System 6.0.8 Finder, takes a deterministic snapshot of CODE 1's first
# few hundred bytes of output, and compares to a checked-in golden file.
# The snapshot is line-stable as long as cpu_disasm and the annotator
# don't change format; future format tweaks update the golden together
# with this test.

TEST_NAME := re.disasm_code (System 6 Finder)
TEST_DESC := Snapshot CODE 1 + jump-table disasm and compare to checked-in golden
TEST_ROM := roms/Plus_v3.rom
TEST_RUNNER := run.sh
