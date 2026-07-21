# Integration test: machine.profile() capability probe
# Verifies the derived `capabilities` block (cpu/mmu/fpu/nubus) and the
# per-card `video_slots` block that the frontend now probes instead of
# guessing from the model's display name (proposal §4.4 / §6.1).

TEST_NAME := Machine Capability Probe
TEST_DESC := machine.profile().capabilities mmu.kind per model + VROM-by-card

# Any ROM works — machine.profile() is a static registry lookup and does
# not boot the machine; Plus is the smallest.
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh
