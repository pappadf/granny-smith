-- Converted from test.script by scripts/script-to-lua.py
assert(machine.hd.present, "hd= attached the ProFile")
assert(gs.try(function() return machine.attach_cdrom(F"${vars.WORK_DIR}/pro.img") end, "none") == "none", "no CD bay on a Lisa")
machine.eject_media("profile")
assert(not machine.hd.present, "eject_media took the ProFile out")
assert(machine.storage[2].filename == "", "storage names no image in the empty ProFile")
local b = machine.attach_hd(F"${vars.WORK_DIR}/pro.img")
assert(b.bus == "profile", "attach_hd on a Lisa uses the ProFile")
assert(machine.hd.present, "and it is attached")
assert(machine.storage[2].media.bus == "profile", "storage names the ProFile bay")
assert(machine.storage[2].filename == F"${vars.WORK_DIR}/pro.img", "and the image in it")

-- A Lisa has no SCSI.  After a Mac with a controller was booted and replaced,
-- machine.scsi must not answer from that Mac's controller: the machine-bound
-- members error, and only the pre-machine probe surface remains.
machine.boot{model = "plus", rom = "../../data/roms/plus-v3-4d1f8172.rom"}
assert(gs.try(function() return machine.scsi.loopback end, "none") ~= "none", "the Plus has a live controller")
machine.boot{model = "lisa", rom = vars.ROM}
assert(gs.try(function() return machine.scsi.loopback end, "none") == "none", "a Lisa has no SCSI controller to answer")
assert(gs.try(function() return machine.scsi.device[0].type end, "none") == "none", "nor SCSI devices")
assert(machine.scsi.hd_models ~= nil, "the probe surface still answers")
echo("media-bays-lisa-ok")
quit()
