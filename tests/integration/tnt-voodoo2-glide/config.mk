# Integration test: the shipped Mac Glide driver runs its full hardware
# detection against the Voodoo2 model (proposal-pci-3dfx-voodoo2,
# milestone 3e).
#
# The guest is Mac OS 8.1 with 3dfx's own VOODOO2_DRV_1.0B5 extensions
# (byte-verified against the shipped archive) and MacSoft's `Quake 3Dfx`
# — a native Glide 2.x client.  The row boots to the Finder, launches
# Quake from the shell (Finder type-select + Cmd-O), and asserts that
# the REAL 3DfxGlideLib2.x found the card by PCI ID and completed
# grSstQueryHardware: the initEnable gates opened by the guest, the PPC
# byte-swizzle path enabled, the DAC detected and the bring-up clock
# set, and the driver's own render-based self-tests passed (dither
# calibration, TMU config and per-TMU memory sensing — hundreds of
# triangles through the rasteriser, verified by the driver itself).
#
# MEDIA-GATED: the guest image is machine-local (local/, not fetched
# test data); the row skips cleanly when it is absent.

TEST_NAME := TNT Voodoo2 Glide
TEST_DESC := Mac OS 8.1 + the shipped 3dfx Glide driver: grSstQueryHardware completes against the model

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# Copied because the System writes to the volume; the copy only happens
# when the machine-local image exists (the skip path needs no media).
TEST_SETUP := test ! -f ../../local/macos81voodoo2-quake-installed.img || cp ../../local/macos81voodoo2-quake-installed.img "$(WORK_DIR)/quake.img"

TEST_ARGS := model=pm7500 ram=65536

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
