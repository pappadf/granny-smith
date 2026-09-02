# Integration test: perf-pdm — the PowerPC throughput row
# (proposal-predecoded-interpreter-cores.md, Phase 0).
#
# The 6100 ROM boot to the SCSI-scan wall: 200 M instructions of the 601
# interpreter with no media attached, so the row measures the core (with
# the ROM's 68K emulator as the workload) and nothing else.  It reports
# the deterministic instruction spend (@@PERF) and asserts the coarse
# host-throughput floor the predecoded cores raised.

TEST_NAME := PDM throughput (6100 ROM boot)
TEST_DESC := 200M-instruction 601 ROM boot: MIPS floor + instruction-spend baseline

TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
