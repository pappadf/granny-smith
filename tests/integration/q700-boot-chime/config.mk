# Integration test: Quadra 700 boot to chime + Start Manager idle
# (proposal-machine-quadra-700-900-950.md Phase C gate)
#
# Boots the shared 420DBFF3 ROM on the q700 profile and asserts the
# machine-free observables of the Phase C gate: model identify passes
# (the ROM's decoder-kind probe accepts the MCU register file and the
# VIA1 PA model sense reads Q700), RAM sizing lands MemTop on the
# configured total, the access-triggered overlay dropped, and the EASC
# path plays the boot chime (frame-count window; the chime is RTC-phase
# sensitive at the +/-1-frame level, so no sample-exact golden yet).

TEST_NAME := Quadra 700 boot chime + Start Manager idle
TEST_DESC := 420DBFF3 ROM on q700: identify, RAM sizing, overlay drop, chime

TEST_ROM := roms/q700-q900-420dbff3.rom
TEST_ARGS := model=q700 ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
