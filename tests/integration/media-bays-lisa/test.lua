-- Converted from test.script by scripts/script-to-lua.py
assert(machine.hd.present, "hd= attached the ProFile")
assert(files.images.count == 1, "the ProFile's image is the machine's, like every drive's")
assert(files.images[0].kind == "profile", "and is a ProFile")
assert(gs.try(function() return machine.attach_cdrom(F"${vars.WORK_DIR}/pro.img") end, "none") == "none", "no CD bay on a Lisa")
machine.eject_media("profile")
assert(not machine.hd.present, "eject_media took the ProFile out")
assert(machine.storage[1].source == "", "storage names no image in the empty ProFile")
local b = machine.attach_hd(F"${vars.WORK_DIR}/pro.img")
assert(b.bus == "profile", "attach_hd on a Lisa uses the ProFile")
assert(machine.hd.present, "and it is attached")
assert(machine.storage[1].media.bus == "profile", "storage names the ProFile bay")
assert(machine.storage[1].source == F"${vars.WORK_DIR}/pro.img", "and the image in it")

-- An eject hands the image back: it is closed and its slot freed.  The
-- ProFile's used to stay open until the machine went away.  (The diskette's
-- stays open on purpose: the Lisa FDC's reinsert cache hands the same image
-- back when the same diskette goes in again.)
local n = files.images.count
machine.floppy.drive[0].insert(F"${vars.TEST_DATA}/systems/System_2_0_1.dsk")
assert(files.images.count == n + 1, "the diskette is tracked")
assert(files.images[n].writable, "inserted writable by default")
machine.floppy.drive[0].eject()
assert(files.images.count == n + 1, "the diskette stays for its reinsert")
local m = files.images.count
machine.eject_media("profile")
assert(files.images.count == m - 1, "the ejected ProFile's image is still open")

-- A blank ProFile is checkpointed and restored like any disk.  It used to
-- vanish: it had no file name to save, so the restore dropped it.
machine.hd.insert()
assert(machine.hd.present, "a blank ProFile")
assert(checkpoint.save(F"${vars.WORK_DIR}/blank.checkpoint"), "checkpoint save")
assert(checkpoint.load(F"${vars.WORK_DIR}/blank.checkpoint"), "checkpoint load")
assert(machine.hd.present, "the blank ProFile survived the restore")
assert(files.images[0].kind == "profile", "as a ProFile")

-- The ProFile's image is the one image node every drive has, and its export
-- (once machine.hd.save) is that node's: the consolidated 532-byte blocks.
assert(machine.hd.image.kind == "profile", "machine.hd.image is not the ProFile's")
assert(machine.hd.image.tag_bytes == 20, "a ProFile carries 20 tag bytes beside each sector")
assert(machine.hd.image.sectors == 9728, "the blank ProFile is not the 5 MB one")
assert(machine.hd.image.source == "", "a blank ProFile has no source")
assert(machine.hd.image.export(F"${vars.WORK_DIR}/saved-profile.image"), "hd.image.export")
assert(files.path_size(F"${vars.WORK_DIR}/saved-profile.image") == 9728 * 532, "the export is not 9728 532-byte blocks")
machine.hd.image.eject()
assert(not machine.hd.present, "hd.image.eject took the ProFile out")
assert(gs.try(function() return machine.hd.image.kind end, "gone") == "gone", "an empty ProFile has an image node")

-- The diskette's node likewise, present only while a disk is clamped.
machine.floppy.drive[0].insert(F"${vars.TEST_DATA}/systems/System_2_0_1.dsk")
assert(machine.floppy.drive[0].image.kind == "floppy", "drive[0].image is not a floppy")
assert(machine.floppy.drive[0].image.tag_bytes == 12, "a Lisa diskette carries 12 tag bytes beside each sector")
machine.floppy.drive[0].image.eject()
assert(not machine.floppy.drive[0].present, "image.eject left the diskette in")
assert(gs.try(function() return machine.floppy.drive[0].image.kind end, "gone") == "gone", "an empty drive has an image node")

-- A Lisa has no SCSI.  After a Mac with a controller was booted and replaced,
-- machine.scsi must not answer from that Mac's controller: a Lisa has no
-- machine.scsi at all (no pre-machine stand-in either).
machine.boot{model = "plus", rom = "../../data/roms/plus-v3-4d1f8172.rom"}
assert(gs.try(function() return machine.scsi.loopback end, "none") ~= "none", "the Plus has a live controller")
machine.boot{model = "lisa", rom = vars.ROM}
assert(gs.try(function() return machine.scsi.loopback end, "none") == "none", "a Lisa has no SCSI controller to answer")
assert(gs.try(function() return machine.scsi.device[0].type end, "none") == "none", "nor SCSI devices")
assert(gs.try(function() return machine.scsi end, "none") == "none", "a Lisa has no machine.scsi")
assert(#catalog.hd_models > 0, "the hard disk models answer from the catalog")
echo("media-bays-lisa-ok")
quit()
