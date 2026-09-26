# Integration test: media go to the model's bays, from the command line and
# from the object model alike (M1-M3).
#
# Where a hard disk or a CD goes is in the profile -- the buses with their
# bays, the `boot` flag, has_cdrom/cdrom_id -- but headless put hd=N at SCSI
# id N on the first bus whatever the model, while the web frontend used the
# boot bay.  On a Network Server the boot bay is id 2 and the CD bay id 0, so
# the two front ends put the same disk in different places.  Both now go
# through profile_hd_bays / profile_cdrom_bay, which machine.profile exports
# as hd_bays / hd_default / cdrom and machine.attach_hd / attach_cdrom use.

TEST_NAME := Media bays
TEST_DESC := hd=/cdrom= and machine.attach_hd/attach_cdrom/eject_media place media in the profile's bays (ans500, plus)

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_SETUP := truncate -s 20M "$(WORK_DIR)/hd.img" && truncate -s 20M "$(WORK_DIR)/hd2.img" && truncate -s 4M "$(WORK_DIR)/cd.iso"
TEST_ARGS := model=ans500 ram=32768 hd=$(WORK_DIR)/hd.img cdrom=$(WORK_DIR)/cd.iso

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
