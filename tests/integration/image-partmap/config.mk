# Integration test: image partmap + probe
# Exercises the Phase 1 image command against an APM-formatted A/UX hard
# disk image.  Verifies the command runs to completion without triggering
# an assertion or non-zero exit.

TEST_NAME := Image partmap
TEST_DESC := Exercises `image partmap` and `image probe` against an APM-formatted A/UX disk image

# Universal ROM shared by SE/30, IIcx, IIx
TEST_ROM := roms/SE30.rom
