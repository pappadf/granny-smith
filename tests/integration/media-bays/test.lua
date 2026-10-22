-- Converted from test.script by scripts/script-to-lua.py
-- hd= went to the default configuration's hard disk (front bay 2, id 2),
-- cdrom= to its CD-ROM drive (front bay 0, id 0).
assert(machine.scsi.device[2].type == "hd", "hd= in the default hard disk's place")
assert(machine.scsi.device[0].type == "cdrom", "cdrom= in the default CD-ROM drive")

-- The profile's default configuration says the same.
local d = catalog.default_config("ans500")
assert(d.storage[1].bus == "scsi" and d.storage[1].unit == 0 and d.storage[1].type == "cd", "default CD-ROM drive")
assert(d.storage[2].bus == "scsi" and d.storage[2].unit == 2 and d.storage[2].type == "hd", "default hard disk")
assert(d.startup.bus == "scsi" and d.startup.unit == 2, "the startup device is the default hard disk")

-- machine.storage names what is in each device and the bay eject_media
-- takes it from -- what the web app rebuilds its mounts from after a
-- checkpoint restore, which attached nothing through it.
assert(machine.storage[2].filename == F"${vars.WORK_DIR}/hd.img", "storage names the hard disk's image")
assert(machine.storage[2].media.bus == "scsi" and machine.storage[2].media.id == 2, "and its bay")
assert(machine.storage[1].filename == F"${vars.WORK_DIR}/cd.iso", "storage names the disc")
assert(catalog.profile("lisa").storage[1].id == "profile", "the Lisa's disk is on the ProFile port")
assert(#catalog.profile("lisa").storage == 1, "no phantom SCSI bus on the Lisa")
assert(catalog.default_config("plus").storage[2].type == "cd", "the Plus has an external CD-ROM drive")

-- attach_media names a position as the configuration does: the second
-- fast/wide channel's front bay 4.
local b = machine.attach_media("scsi2", 4, "hd", F"${vars.WORK_DIR}/hd2.img")
assert(b.bus == "scsi2", "attach_media answers the bus")
assert(b.id == 4, "attach_media answers the id")
assert(machine.scsi2.device[4].type == "hd", "the disk is on machine.scsi2")

-- An occupied position, a unit the bus does not have, a second CD: refused,
-- not overwritten.
assert(gs.try(function() return machine.attach_hd(F"${vars.WORK_DIR}/hd2.img") end, "refused") == "refused", "occupied hard disk refused")
assert(gs.try(function() return machine.attach_hd(F"${vars.WORK_DIR}/hd2.img", 1) end, "refused") == "refused", "a second default hard disk does not exist")
assert(gs.try(function() return machine.attach_media("scsi2", 3, "hd", F"${vars.WORK_DIR}/hd2.img") end, "refused") == "refused", "unit 3 is not on bus 1")
assert(gs.try(function() return machine.attach_media("scsi", 7, "hd", F"${vars.WORK_DIR}/hd2.img") end, "refused") == "refused", "the initiator's ID")
assert(gs.try(function() return machine.attach_media("bogus", 1, "hd", F"${vars.WORK_DIR}/hd2.img") end, "refused") == "refused", "no such bus")
assert(gs.try(function() return machine.attach_cdrom(F"${vars.WORK_DIR}/cd.iso") end, "refused") == "refused", "occupied CD-ROM drive refused")

-- eject_media takes back what attach answered.
machine.eject_media("scsi2", 4)
assert(gs.try(function() return machine.eject_media("scsi2", 4) end, "empty") == "empty", "nothing left to eject")
assert(gs.try(function() return machine.eject_media("bogus", 0) end, "bad") == "bad", "unknown bus refused")

-- The Plus's external CD-ROM drive is at ID 3, its hard disk at ID 0.
machine.boot{model = "plus", rom = F"${vars.TEST_DATA}/roms/plus-v3-4d1f8172.rom"}
local c = machine.attach_cdrom(F"${vars.WORK_DIR}/cd.iso")
assert(c.id == 3, "the Plus's CD-ROM drive at id 3")
local h = machine.attach_hd(F"${vars.WORK_DIR}/hd.img")
assert(h.id == 0, "a Plus's first disk at id 0")

-- A configuration without a CD-ROM drive has none to insert into.
machine.boot{model = "plus", rom = F"${vars.TEST_DATA}/roms/plus-v3-4d1f8172.rom", config = "{\"storage\":[{\"bus\":\"scsi\",\"unit\":0,\"type\":\"hd\"}]}"}
assert(gs.try(function() return machine.scsi.device[3].type end, "none") ~= "cdrom", "no CD-ROM drive when the configuration has none")

-- Only a CD-ROM goes in a CD-ROM drive, even an empty one: a disk at the
-- IIcx's CD-ROM drive would save a checkpoint no restore of it can rebuild.
machine.boot{model = "iicx", rom = F"${vars.TEST_DATA}/roms/iix-iicx-se30-97221136.rom"}
assert(machine.scsi.device[3].type == "cdrom", "the IIcx's CD-ROM drive is id 3")
assert(machine.scsi.attach_hd(F"${vars.WORK_DIR}/hd2.img", 3) == false, "a disk in the CD-ROM drive refused")
assert(machine.scsi.device[3].type == "cdrom", "the CD-ROM drive keeps its drive")

-- The configuration places CD-ROM drives anywhere a bus takes one: two here.
machine.boot{model = "iicx", rom = F"${vars.TEST_DATA}/roms/iix-iicx-se30-97221136.rom", config = "{\"storage\":[{\"bus\":\"scsi\",\"unit\":0,\"type\":\"hd\"},{\"bus\":\"scsi\",\"unit\":4,\"type\":\"cd\"},{\"bus\":\"scsi\",\"unit\":5,\"type\":\"cd\"}]}"}
assert(machine.scsi.device[4].type == "cdrom", "a CD-ROM drive at id 4")
assert(machine.scsi.device[5].type == "cdrom", "and one at id 5")
assert(gs.try(function() return machine.scsi.device[3].type end, "none") ~= "cdrom", "and none at the default id 3")

-- A bus id of the configuration is not a bay bus: the G3's second ATA bus
-- (ata1) is ATA unit 2, and storage says so, with the image in it.
machine.boot{model = "pmg3dt", rom = F"${vars.TEST_DATA}/roms/pmg3dt-pmg3mt-78f57389.rom", config = "{\"storage\":[{\"bus\":\"ata0\",\"unit\":0,\"type\":\"hd\"},{\"bus\":\"ata1\",\"unit\":0,\"type\":\"hd\"}]}"}
machine.attach_media("ata1", 0, "hd", F"${vars.WORK_DIR}/hd2.img")
assert(machine.storage[2].media.bus == "ata" and machine.storage[2].media.id == 2, "ata1 is ATA unit 2")
assert(machine.storage[2].filename == F"${vars.WORK_DIR}/hd2.img", "storage names the ATA disk's image")
assert(machine.storage[1].filename == "", "an empty device names no image")

-- An ATA bus takes an ATAPI CD-ROM drive too: the secondary bus's master
-- (ata1 unit 0, ATA unit 2), where the Mac OS X 10.0 install CD boots from.
machine.boot{model = "pmg3dt", rom = F"${vars.TEST_DATA}/roms/pmg3dt-pmg3mt-78f57389.rom", config = "{\"storage\":[{\"bus\":\"ata0\",\"unit\":0,\"type\":\"hd\"},{\"bus\":\"ata1\",\"unit\":0,\"type\":\"cd\"}]}"}
assert(machine.storage[1].type == "cd", "the configuration's ATA CD-ROM drive is in storage")
machine.attach_media("ata1", 0, "cd", F"${vars.WORK_DIR}/hd2.img")
assert(machine.storage[1].media.bus == "ata" and machine.storage[1].media.id == 2, "the ATA CD-ROM drive is ATA unit 2")
assert(machine.storage[1].filename == F"${vars.WORK_DIR}/hd2.img", "storage names the disc's image")

echo("media-bays-ok")
quit()
