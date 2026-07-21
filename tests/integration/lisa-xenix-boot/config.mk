# Integration test: Apple Lisa 2 — boot SCO Xenix 3.0 from the installed ProFile.
#
# Boots the Xenix system that the lisa-xenix-install test installs onto the
# ProFile.  With the installed ProFile attached and the "Boot XProFile Patch"
# floppy in the drive, the loader's "boot :" prompt is answered with
# "pf(0,0)xenix" to load the kernel from the hard disk; the installed system then
# comes up to its multi-user startup prompt:
#
#   rootdev 0 0 swapdev 0 0
#   Root 3872k Swap 992k
#   Type CONTROL-d to proceed with normal startup,
#   (or give root password for system maintenance):
#
# (XENIX Installation Guide for the Apple Lisa 2, May 1984, §1.5.4 step 12 /
# §1.5.5.)  The floppy supplies only the bootstrap loader; the kernel and the
# whole OS are read from the installed ProFile (pf(0,0)xenix).
#
# Input (gitignored proprietary data, staged by a maintainer for CI): the
# installed Xenix ProFile image, produced by running lisa-xenix-install (which
# ends with a `profile.save` of its cleanly-shut-down image to
# "$(WORK_DIR)/xenix-installed.image") and copying that result to
# tests/data/Lisa/Xenix-3.0/Xenix-3.0-ProFile.image.

TEST_NAME := Apple Lisa 2 boot Xenix from the ProFile
TEST_DESC := Boots the installed Xenix 3.0 ProFile to its multi-user startup prompt

TEST_ROM := roms/lisa2-revh-098917b2.rom
TEST_ARGS := model=lisa ram=2048 fd=$(TEST_DATA)/Lisa/Xenix-3.0/Xenix-3.0-Boot-XProFile.dc42

# Copy the installed image to a writable scratch path so the staged input is never
# modified by the boot (fsck / mount may write).
TEST_SETUP := cp "$(TEST_DATA)/Lisa/Xenix-3.0/Xenix-3.0-ProFile.image" "$(WORK_DIR)/profile.image"
