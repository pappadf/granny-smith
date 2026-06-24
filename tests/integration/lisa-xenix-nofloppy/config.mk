# Integration test: Apple Lisa 2 — boot SCO Xenix 3.0 from the ProFile with NO
# floppy in the drive at any point.
#
# Companion to lisa-xenix-boot (which boots via the "Boot XProFile Patch"
# floppy).  This one boots the *same* installed ProFile with the floppy drive
# empty the whole time, driving only the boot ROM's on-screen device picker:
#
#   1. With the default parameter memory (BootVol = built-in floppy) and no
#      diskette, the rev-H ROM shows its "insert diskette / STARTUP FROM…" screen.
#   2. Apple-3 opens the ROM's STARTUP FROM device picker; Apple-3 again selects
#      the parallel-port ProFile (the ▣3 hard-disk icon).
#   3. The ProFile's own on-disk loader (written by `hdinit` at install) boots to
#      its "boot :" prompt, pre-filled with the default command `pf(0,0)xenix`;
#      Return accepts it and the kernel loads from the ProFile.
#   4. Xenix comes up to its multi-user startup prompt ("Type CONTROL-d to
#      proceed with normal startup, / or give root password …").
#
# This exercises the ProFile read path through the OS disk driver, which (unlike
# the boot ROM's level-polled PROREAD) waits on the BSY/CA1 *edge* for each
# block — so it depends on the controller signalling read-completion as a
# *deferred* BSY transition rather than synchronously at the command handshake.
# See docs/machines/lisa/profile.md and lisa_profile.c (pro_complete).
#
# Input (gitignored proprietary data, staged by a maintainer for CI): the
# installed Xenix ProFile image, copied to a writable scratch path so any
# fsck/mount writes never touch the staged input.

TEST_NAME := Apple Lisa 2 Xenix ProFile no-floppy boot
TEST_DESC := Boots the installed Xenix 3.0 ProFile to its multi-user prompt with no floppy, via the ROM STARTUP FROM picker

TEST_ROM := Lisa/roms/098917B2-LisaH.rom
TEST_ARGS := model=lisa ram=2048
TEST_SETUP := cp "$(TEST_DATA)/Lisa/Xenix-3.0/Xenix-3.0-ProFile.image" "$(WORK_DIR)/profile.image"
