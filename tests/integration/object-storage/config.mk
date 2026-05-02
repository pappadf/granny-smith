# Integration test: storage object class (M8 slice 2)
# Boots Plus with one SCSI HD and one floppy so storage.images has at
# least two populated entries to walk.

TEST_NAME := Object-model storage class
TEST_DESC := storage.images indexed children — filename / path / type / writable

TEST_ROM := roms/Plus_v3.rom
TEST_SETUP := unzip -o -q $(TEST_DATA)/systems/hd1.zip -d $(TEST_TMPDIR)
TEST_ARGS := hd=$(TEST_TMPDIR)/hd1.img fd0=$(TEST_DATA)/systems/System_6_0_8.dsk
