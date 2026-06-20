# Integration test configuration: Macintosh XL — MacWorks XL boot
# Boots the Macintosh XL (Lisa 2 hardware, "3A" boot ROM) under Apple's original
# MacWorks XL.  MacWorks XL is a two-disk boot: the loader disk brings up the Mac
# environment, ejects itself, and waits for a system disk — which the test script
# inserts, just as a user would (no special swap magic).  The system disk carries
# the MacWorks XL system software (MFS-era, System <= 3.2) and its Finder.

TEST_NAME := Macintosh XL MacWorks Boot
TEST_DESC := Boots the Macintosh XL via MacWorks XL (loader disk, then scripted system-disk insertion) to the Mac Finder desktop

# Interleaved Macintosh XL "3A" boot ROM (341-0346-A / 341-0347-A), 16 KB,
# checksum 0x094C82F0.
TEST_ROM := Lisa/roms/094C82F0-MacXL.rom

# Boot from the MacWorks XL loader disk.  RAM is pinned at 1 MB (the macxl
# profile default) so the 608x431 framebuffer sits at $F8000 and the rendered
# desktop is reproducible.  The system disk is inserted at runtime by the script.
TEST_ARGS := model=macxl ram=1024 fd=$(TEST_DATA)/Lisa/MacWorksXL/MacWorksXL_3.0_May1985.dc42
