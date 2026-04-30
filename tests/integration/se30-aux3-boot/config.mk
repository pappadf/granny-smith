# Integration test configuration: SE/30 A/UX 3.0.1 HD Boot
# Verifies that the SE/30 boots A/UX 3.0.1 from a pre-installed HD image.

TEST_NAME := SE/30 A/UX 3.0.1 HD Boot
TEST_DESC := Boot SE/30 with 16 MB RAM from a pre-installed A/UX 3.0.1 HD image

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/SE30.rom

# Copy the HD image into TEST_TMPDIR so its .delta/.journal are wiped with
# the tempdir on exit; the source image is left untouched between runs.
# Keep the TEST_SETUP line single-line — the Makefile extractor only honors
# $(TEST_DATA) and $(TEST_TMPDIR) substitutions.
TEST_SETUP := cp "$(TEST_DATA)/aux/aux_3.0.1/hd160-with-aux-301.img" "$(TEST_TMPDIR)/hd.img"

# 16 MB RAM required for A/UX boot.
TEST_ARGS := ram=16384 hd=$(TEST_TMPDIR)/hd.img
