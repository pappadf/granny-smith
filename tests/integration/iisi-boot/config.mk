# Integration test configuration: Macintosh IIsi Floppy Boot
# Verifies that the Macintosh IIsi ("Erickson") boots from a System 7.0.1 HD
# floppy and lights its built-in V8 video — whose frame buffer lives at the
# BOTTOM of Bank A (physical 0), with system RAM relocated into Bank B at
# physical $04000000 — to the Finder desktop.  This is the regression test for
# the "video frame buffer at physical 0 / two physical RAM banks" fix; before
# it, the screen fill clobbered the vector table and the machine reset-looped.

TEST_NAME := IIsi Floppy Boot
TEST_DESC := Boots the Macintosh IIsi (Egret + V8 built-in video at physical 0) from System 7.0.1 floppy and verifies the Finder desktop

# Dedicated 512 KB Macintosh IIsi ("Erickson") ROM, checksum 0x36B7FB6C.
TEST_ROM := roms/iisi-36b7fb6c.rom

# RAM pinned at 17 MB = 1 MB soldered Bank A ($00000000, holds the video frame
# buffer at its bottom) + 16 MB Bank B ($04000000, system "low memory"), the
# IIsi profile default, so the boot path — and the rendered desktop — is
# reproducible.  Boot from the same System 7.0.1 1.44 MB floppy as iici-boot.
TEST_ARGS := ram=17408 fd=$(TEST_DATA)/systems/System_7_0_1.image
