# Integration test configuration: Macintosh IIci Floppy Boot
# Verifies that the Macintosh IIci boots from a System 7.1 floppy disk and
# lights its built-in RBV video to the Finder desktop.

TEST_NAME := IIci Floppy Boot
TEST_DESC := Boots the Macintosh IIci (RBV built-in video) from System 7.1 floppy and verifies the Finder desktop

# Dedicated 512 KB Macintosh IIci ("Aurora") ROM, checksum 0x368CADFE.
TEST_ROM := roms/368CADFE-IIci.rom

# Boot from the System 7.1 "Disk Tools" floppy.  RAM is pinned at 8 MB (the
# IIci profile default) so the boot path — and therefore the rendered
# desktop — is reproducible.  The framebuffer is driven by the RBV from the
# slot-$B aperture; ScrnBase resolves to $FBB08000.
TEST_ARGS := ram=8192 fd=$(TEST_DATA)/systems/System_7_1_0.dsk
