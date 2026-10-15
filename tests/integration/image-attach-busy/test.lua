-- Converted from test.script by scripts/script-to-lua.py
-- Integration test: the image VFS refuses a file the emulator holds writable.
-- The runner cd's into tests/integration/; WORK_DIR is absolute.

local dmg = F"${vars.WORK_DIR}/fixture.dmg"
local fd = F"${vars.WORK_DIR}/floppy.dsk"
local ro = "../../data/systems/System_6_0_8.dsk"
local raw = F"${vars.WORK_DIR}/hd.img"

-- Readable through the VFS while nothing holds them.
assert(files.list(F"${dmg}/partition1"), "the .dmg is not readable before attach")
assert(files.list(F"${fd}/partition1"), "the floppy is not readable before insert")

-- A hard disk attached writable: the mount made above now refuses.  The
-- image the SCSI disk opened is the decoded scratch copy of the .dmg, so a
-- key taken from the opened image would have protected the wrong file.
machine.scsi.attach_hd(dmg, 2)
assert(not gs.truthy(function() return files.list(F"${dmg}/partition1") end), "a writable hard disk is still served by the VFS")

-- A floppy inserted writable refuses the same way.  The floppy path never
-- notified anything, so the VFS served the base under the guest's writes.
machine.floppy.drive[0].insert(fd, true)
assert(not gs.truthy(function() return files.list(F"${fd}/partition1") end), "a writable floppy is still served by the VFS")

-- Attached before the VFS ever mounted it.  The notification this replaced
-- was sent at attach time and found no mount to mark, so the first mount made
-- afterwards served the file as if nothing held it.
machine.scsi.attach_hd(raw, 4)
assert(not gs.truthy(function() return files.list(F"${raw}/partition1") end), "a disk attached before its first mount is served by the VFS")

-- machine.restart tears nothing down, so the disk never stops being held.
machine.restart()
assert(not gs.truthy(function() return files.list(F"${dmg}/partition1") end), "a .dmg is served by the VFS after machine.restart")

-- A read-only insert leaves nothing the VFS cannot see: still served.
machine.floppy.drive[1].insert(ro)
assert(files.list(F"${ro}/partition1"), "a read-only floppy is refused by the VFS")

quit()
