# Integration test: scsi object class (M7d)
# Boots Plus with a SCSI HD attached so devices.N is non-empty.

TEST_NAME := Object-model SCSI class
TEST_DESC := scsi.bus.phase + scsi.devices indexed children + scsi.loopback

TEST_ROM := roms/Plus_v3.rom
TEST_SETUP := unzip -o -q $(TEST_DATA)/systems/hd1.zip -d $(TEST_TMPDIR)
TEST_ARGS := hd=$(TEST_TMPDIR)/hd1.img
