# Integration test: config= names a pipe, not a file.
#
# The headless CLI reads config= from whatever path the shell hands it,
# including a process substitution (config=<(echo '{...}'), a /dev/fd/N
# pipe) and a named FIFO, by streaming it.  The host-file opener behind
# every other reader refuses such paths so they cannot hang it; config=
# once went through it too and failed with "cannot read config=/dev/fd/63".

TEST_NAME := config= from a pipe or FIFO
TEST_DESC := config=<(...) and config=<named FIFO> boot the document they carry

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
