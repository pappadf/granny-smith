-- Converted from test.script by scripts/script-to-lua.py
-- Integration test: ejecting an ATA hard disk (see config.mk), through
-- machine.eject_media and through the unit's image node.

local n = files.images.count
assert(machine.ata.attach_hd(F"${vars.WORK_DIR}/ata.img", 0), "attach an ATA hard disk as the primary bus's master")
assert(files.images.count == n + 1, "its image is tracked")
assert(machine.ata.device[0].type == "hd", F"unit 0 is ${machine.ata.device[0].type}, not a hard disk")
assert(machine.ata.device[1].type == "none", F"unit 1 is ${machine.ata.device[1].type}, not empty")
assert(machine.ata.device[0].present, "unit 0 holds no image")
assert(machine.ata.device[0].image.kind == "hd", F"unit 0's image is a ${machine.ata.device[0].image.kind}")
assert(machine.ata.device[0].image.source == F"${vars.WORK_DIR}/ata.img", "unit 0's image came from elsewhere")
machine.eject_media("ata", 0)
assert(machine.ata.device[0].type == "none", F"the disk is still on the bus: unit 0 is ${machine.ata.device[0].type}")
assert(not machine.ata.device[0].present, "unit 0 still holds an image")
assert(gs.try(function() return machine.ata.device[0].image.kind end, "gone") == "gone", "the image node outlived the eject")
assert(files.images.count == n, "the ejected disk's image is still open")
assert(gs.try(function() return machine.eject_media("ata", 0) end, "empty") == "empty", "nothing left to eject at unit 0")

-- The image node's own eject takes it off the bus the same way.
assert(machine.ata.attach_hd(F"${vars.WORK_DIR}/ata.img", 0), "re-attach the disk")
machine.ata.device[0].image.eject()
assert(machine.ata.device[0].type == "none", "image.eject left the disk on the bus")
assert(files.images.count == n, "image.eject left the image open")
echo("gossamer-ata-eject-ok")
quit()
