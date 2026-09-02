# Integration test: the shipped Mac Glide driver runs its full hardware
# detection against the Voodoo2 model (proposal-pci-3dfx-voodoo2,
# milestone 3e).
#
# The guest is Mac OS 8.1 with 3dfx's own VOODOO2_DRV_1.0B5 extensions
# (byte-verified against the shipped archive) and MacSoft's `Quake 3Dfx`
# — a native Glide 2.x client.  The row boots to the Finder, launches
# Quake from the shell (Finder type-select + Cmd-O), asserts that the
# REAL 3DfxGlideLib2.x found the card by PCI ID and completed
# grSstQueryHardware (initEnable gates, byte swizzle, DAC + bring-up
# clock, the driver's render-based self-tests), and then keeps running
# INTO THE GAME: grSstWinOpen takes the monitor, the attract demo plays
# at 640x480 through the CMDFIFO setup path, and the golden
# quake-ingame.png is a hand-inspected, fully textured in-game frame.
#
# MEDIA-GATED: the guest image lives in gs-test-data
# (apps/quake_8_1_voodoo2.img.7z, fetched+extracted into
# tests/data/apps/); the row skips cleanly when the data is not
# fetched.

TEST_NAME := TNT Voodoo2 Glide
TEST_DESC := Mac OS 8.1 + the shipped 3dfx Glide driver: grSstQueryHardware completes against the model

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# Copied because the System writes to the volume; the copy only happens
# when the fetched image exists (the skip path needs no media).
TEST_SETUP := test ! -f ../../data/apps/quake_8_1_voodoo2.img || cp ../../data/apps/quake_8_1_voodoo2.img "$(WORK_DIR)/quake.img"

TEST_ARGS := model=pm7500 ram=65536

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
