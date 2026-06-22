# Integration test: core layering check (proposal §4.3)
# Asserts the platform-agnostic core never includes a machine-implementation
# header — only the public core/machine_profile.h.  Pure static check; the
# ROM is required by the harness but unused.
TEST_NAME := Core Layering Check
TEST_DESC := src/core/ must not #include any src/machines/ implementation header
TEST_ROM := roms/Plus_v3.rom
TEST_RUNNER := run.sh
