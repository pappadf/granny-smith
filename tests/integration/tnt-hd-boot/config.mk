# Integration test: TNT boot from a MESH disk — Phase E's frontier row
#
# Boots the pm7500 with the 7.6 image on the internal (MESH) SCSI bus and
# asserts the Phase-E part-2 high-water: the boot-driver selection
# matches (XPRAM $77 through the banked NVRAM — the bank-port 16-bit
# access law), the DDM/partition-map/driver-partition reads run as
# complete byte-perfect MESH transactions, the Apple_Driver43 loads, and
# the boot mounts the startup volume (BootDrive set) into the "Mac OS"
# splash screen.  The known frontier BEYOND this row: the loaded disk
# driver's data-phase arming (its softc "transfer armed" state stays
# empty, every data phase disconnects, and the System load dies as
# dsBadPatch shown as dsOldSystem) — the splash-with-bomb golden pins
# exactly how far the boot gets; when the arming question falls, this
# row's goldens move forward to the Finder (T13).
#
# The image is 68k-install-flavored (it lacks the 7500's gpch 68 patch
# resource, carrying only the 8500's 69) — irrelevant up to this row's
# markers, and the boot fails LATER on dsBadPatch either way; replace
# with a universal install when gs-test-data gains one.

TEST_NAME := TNT MESH disk boot
TEST_DESC := pm7500 + 7.6 disk on the MESH bus: driver match, driver load, BootDrive, the Mac OS splash

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# Copied into WORK_DIR because the System writes to the volume during boot.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_6_170mb_24ac.img" "$(WORK_DIR)/hd.img"

TEST_ARGS := model=pm7500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
