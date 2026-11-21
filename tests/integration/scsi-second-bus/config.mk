# Integration test: the second SCSI bus is a bus like the first, and /RESET
# reaches it and the 53C96 chips.
#
# The 7500/8500/9500 (the 53C94's CD-ROM and external bus), the Network
# Servers (fast/wide channel 1) and the Quadra 900/950 (the external 53C96)
# each have a `machine.scsi2`.  It used to live in the family state, where
# the shared media path and the shared /RESET could not see it:
#   - machine.attach_media("scsi2", ...) on a Quadra 900 was refused (only
#     TNT carried wrappers for it);
#   - no warm reset cleared the second bus, so a CD the OS had locked with
#     PREVENT ALLOW MEDIUM REMOVAL stayed locked across a restart;
#   - and the Quadra 700/900/950 and 660AV/840AV never reset their 53C96
#     chips at /RESET (PDM and TNT did), so a chip's configuration survived.
# The PREVENT is issued the way a driver would, through the chip's own
# registers (destination ID, the CDB in the FIFO, Select without ATN).

TEST_NAME := Second SCSI bus
TEST_DESC := machine.scsi2 takes runtime media on every model that has one, /RESET clears it (a PREVENT lock goes), and /RESET resets the 53C96 chips (q700, q900, q840av, pm8500)

TEST_ROM := roms/q700-q900-420dbff3.rom
TEST_ARGS := model=q700 ram=8192

TEST_SETUP := truncate -s 20M "$(WORK_DIR)/hd.img" && truncate -s 4M "$(WORK_DIR)/cd.iso"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
