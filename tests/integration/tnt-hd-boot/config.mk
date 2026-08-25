# Integration test: TNT boot from a MESH disk — ladder rung T13
#
# Boots the pm7500 with the 7.6 image on the internal (MESH) SCSI bus
# through the complete chain: the boot-driver selection matches (XPRAM
# $77 through the banked NVRAM — the bank-port 16-bit access law), the
# DDM/partition-map/driver-partition reads run as complete byte-perfect
# MESH transactions, the Apple_Driver43 loads, the boot mounts the
# startup volume (BootDrive set), and System 7.6 starts all the way to
# the Finder desktop.  The final wall — the loaded driver's data phases
# all disconnecting ("the arming wall") — fell to the MESH SEQ_BUSFREE
# expect-free law (mesh.c): BUSFREE with the target still REQing raises
# the phase-mismatch exception instead of releasing, which is the very
# signal the driver's [114]==4 probe path uses to continue into the
# data phase.
#
# The image is 68k-install-flavored (its System lacks the 7500's gpch
# 68 patch resource) — empirically irrelevant: the boot reaches the
# Finder regardless.

TEST_NAME := TNT MESH disk boot
TEST_DESC := pm7500 + 7.6 disk on the MESH bus: driver match, driver load, BootDrive, System 7.6 to the Finder (T13)

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# Copied into WORK_DIR because the System writes to the volume during boot.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_6_170mb_24ac.img" "$(WORK_DIR)/hd.img"

TEST_ARGS := model=pm7500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
