# Integration test: tools/dump --disasm-code mode.  Drives the System 6.0.8
# Finder, takes a deterministic snapshot of CODE 0 (jump table) + CODE 1's
# first 20 lines of output, and compares to checked-in goldens.  The
# snapshot is line-stable as long as cpu_disasm and the annotator don't
# change format; future format tweaks update the golden together with
# this test.

TEST_NAME := dump --disasm-code (System 6 Finder)
TEST_DESC := Snapshot CODE 1 + jump-table disasm and compare to checked-in golden
TEST_ROM := roms/Plus_v3.rom
TEST_RUNNER := run.sh
