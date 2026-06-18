# Integration test configuration: Apple Lisa 2 — Lisa Office System 3.1 boot
# Boots the Lisa 2 (rev-H boot ROM) from the LOS 3.1 install floppy (Disk 1) all
# the way to the Install/Repair/Restore menu — the same screen the LisaEm
# reference emulator reaches.

TEST_NAME := Apple Lisa 2 LOS 3.1 boot
TEST_DESC := Boots the Lisa 2 from the Lisa Office System 3.1 install floppy to the Install/Repair/Restore menu

# Interleaved rev-H Lisa 2 boot ROM (341-0175-H / 341-0176-H), 16 KB, checksum 0x098917B2.
TEST_ROM := roms/098917B2-LisaH.rom

# Lisa 2, 2 MB.  Booting the Office System exercises the whole stack: the boot
# ROM sizes/maps RAM and loads the OS off the Sony 400K floppy; the OS mounts the
# volume, demand-loads SYSTEM.SHELL and its ~25 code segments via the MC68000
# segment-MMU bus-error path, and the installer shell renders the menu.  RAM is
# based high at $80000 automatically for model=lisa (the way Lisa memory boards
# populate the space), so no env override is needed.
TEST_ARGS := model=lisa ram=2048 fd=$(TEST_DATA)/systems/LisaOfficeSystem-3.1/Install1.image
