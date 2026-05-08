# Integration test configuration: IIx System 7.0.1 Floppy Boot
# Sister of iicx-floppy: same scenario but on the IIx profile
# (PA6=0, PB3=0, six-slot NuBus space).
#
# This is a "no-crash + booting" smoke test rather than a full
# Finder-desktop comparison — see iicx-floppy/config.mk for the
# rationale.  When the JMFB driver fleshes out colour modes,
# screen.match becomes possible.

TEST_NAME := IIx System 7.0.1 Floppy Boot
TEST_DESC := Boots IIx from the System 7.0.1 floppy and verifies no crash

TEST_ROM := roms/IIcx.rom

TEST_ARGS := model=iix ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
