# Integration test: predecoded cores — differential checkpoint equality
# (proposal-predecoded-interpreter-cores.md §9.2).
#
# The definition of "same guest timeline": boot each machine, run to fixed
# instruction counts chosen to land mid-boot, checkpoint, and compare the
# guest state block by block between the switch core (predecode.enabled=0)
# and the predecoded core at every elision level.  scripts/cmp-checkpoints.py
# masks the two things that are not guest state (host pointers written
# verbatim into a few structs; the host-side storage bookkeeping).
#
# Rows: plus (68000, System 6.0.8 floppy), iicx (68030 + PMMU, System 6 HD
# with Marathon), pdm (PowerPC 601, the 6100 ROM boot to the SCSI-scan wall).

TEST_NAME := Predecode differential checkpoints
TEST_DESC := Switch core vs predecoded core: byte-identical guest state at fixed instruction counts

# The runner boots each machine with its own ROM; this one is the Plus.
TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
