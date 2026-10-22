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

-- A checkpoint taken with the floppy in, for the restore below.
assert(checkpoint.save(F"${vars.WORK_DIR}/in.checkpoint"), "checkpoint save with the floppy in")

-- Ejected, the floppy's file is released at once: the VFS serves it again and
-- a file inside it copies out, resource fork and all.  The image used to stay
-- open until the machine went away ("cannot read resource fork ... Resource
-- busy").
machine.floppy.drive[0].eject()
assert(files.list(F"${fd}/partition1"), "an ejected floppy is still refused by the VFS")
files.cp(F"${fd}/partition1/System Folder/Finder", F"${vars.WORK_DIR}/Finder")
assert(files.path_exists(F"${vars.WORK_DIR}/._Finder"), "the Finder copied out of the ejected floppy without its resource fork")

-- Deleted and replaced by another file under the same name, it inserts and
-- ejects again like a new disk.
files.rm(fd)
assert(not gs.truthy(function() return files.path_exists(fd) end), "the ejected floppy could not be removed")
files.cp(ro, fd)
machine.floppy.drive[0].insert(fd, true)
assert(not gs.truthy(function() return files.list(F"${fd}/partition1") end), "the re-inserted floppy is served by the VFS")
machine.floppy.drive[0].eject()
assert(files.list(F"${fd}/partition1"), "the floppy ejected a second time is still refused")

-- A checkpoint taken while the disk was in restores after the eject, disk and
-- all; one taken after it restores with the drive empty.
assert(checkpoint.save(F"${vars.WORK_DIR}/out.checkpoint"), "checkpoint save with the drive empty")
assert(checkpoint.load(F"${vars.WORK_DIR}/in.checkpoint"), "the checkpoint taken with the floppy in does not restore")
assert(machine.floppy.drive[0].present, "the restored machine lost its floppy")
assert(machine.floppy.drive[0].disk.filename == fd, "the restored drive holds another disk")
assert(checkpoint.load(F"${vars.WORK_DIR}/out.checkpoint"), "the checkpoint taken with the drive empty does not restore")
assert(not machine.floppy.drive[0].present, "the drive ejected before the save holds a disk after the restore")
machine.floppy.drive[0].insert(fd, true)

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
