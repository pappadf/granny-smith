# Integration test: image HFS traversal + auto-mount + EBUSY + EROFS
# Exercises Phase 2 of proposal-image-vfs.md: implicit descent into an HFS
# disk image, recursive cp out of the image, and the hd-attach conflict rule.

TEST_NAME := Image HFS traverse
TEST_DESC := Traverse a System 6 HFS floppy via VFS descent; verify cp, EROFS, EBUSY

TEST_ROM := roms/SE30.rom
