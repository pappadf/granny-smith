# Integration test configuration: Macintosh IIci Floppy Boot
# Verifies that the Macintosh IIci boots from a 1.44MB HD MFM floppy disk
# (System 7.0.1) and lights its built-in RBV video to the Finder desktop.

TEST_NAME := IIci Floppy Boot
TEST_DESC := Boots the Macintosh IIci (RBV built-in video) from System 7.0.1 HD floppy and verifies the Finder desktop

# Dedicated 512 KB Macintosh IIci ("Aurora") ROM, checksum 0x368CADFE.
TEST_ROM := roms/368CADFE-IIci.rom

# Boot from the 1.44MB HD MFM floppy (System 7.0.1), the same image used by
# se30-floppy-hd and iifx-boot.  RAM is pinned at 8 MB (the IIci profile
# default) so the boot path — and therefore the rendered desktop — is
# reproducible.  The framebuffer is driven by the RBV from the slot-$B
# aperture; ScrnBase resolves to $FBB08000.
TEST_ARGS := ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
