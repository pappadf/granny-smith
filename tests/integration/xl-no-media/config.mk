# Integration test configuration: Macintosh XL — no startup device.
# Boots the Macintosh XL (Lisa 2 hardware, "3A" boot ROM) with NO floppy and NO
# ProFile attached.  With nothing to boot from, the boot ROM's startup scan finds
# the floppy drive empty (the Sony controller reports DRVERR, "no disk in drive")
# and settles on its "insert a diskette" prompt: a disk-drive icon with an insert
# arrow and the drive id (2 = the Lisa 2 / Mac XL internal upper Sony), offering
# the CONTINUE (Cmd-2) and STARTUP FROM... (Cmd-3) recovery buttons.

TEST_NAME := Macintosh XL no startup device
TEST_DESC := Boots the Macintosh XL with no floppy and no ProFile; the boot ROM reaches its "insert a diskette" prompt (empty-drive DRVERR), not an I/O-board fault

# Interleaved Macintosh XL "3A" boot ROM (341-0346-A / 341-0347-A), 16 KB,
# checksum 0x094C82F0.
TEST_ROM := Lisa/roms/094C82F0-MacXL.rom

# Macintosh XL, 1 MB (the macxl profile default, so the 608x431 framebuffer sits
# at $F8000 and the rendered prompt is reproducible).  No fd= and no profile=
# argument: the machine starts with no removable or fixed media at all.
TEST_ARGS := model=macxl ram=1024
