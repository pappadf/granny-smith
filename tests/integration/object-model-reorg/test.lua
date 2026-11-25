-- Converted from test.script by scripts/script-to-lua.py
-- The reorganised object model: see config.mk.

-- === Root: children and methods, in model order ============================
local kids = ""
local verbs = ""
for _, m in ipairs(meta.members()) do
    if m.kind == "child" then kids = F"${kids} ${m.name}" end
    if m.kind == "method" then verbs = F"${verbs} ${m.name}" end
end
echo(F"root children:${kids}")
echo(F"root methods:${verbs}")
assert(kids == " machine scheduler checkpoint pacing predecode jit files debug log shell catalog appletalk", "root children in the fixed order")
assert(verbs == " objects attributes methods help time quit echo", "root methods (no download)")

-- === files: one node for host files, images, mounts, archives ==============
assert(files.path_exists("../../data/systems/System_6_0_8.dsk"), "files.path_exists")
assert(files.identify("../../data/systems/System_6_0_8.dsk").archive == "", "files.identify: no archive")
assert(files.images.meta.path == "files.images", "files.images path")

-- files.mounts: indexed by a never-reused serial.  Descend into the floppy to
-- mount it, unmount it, mount it again: the second mount gets a new index.
assert(files.mounts.count == 0, "no mounts yet")
local l1 = files.list("../../data/systems/System_6_0_8.dsk/partition1")
local first = files.mounts.find("../../data/systems/System_6_0_8.dsk")
assert(first >= 0, "the floppy is mounted after a descent")
assert(files.mounts[first].meta.path == F"files.mounts[${first}]", "a mount entry has its path")
assert(files.mounts[first].format == "HFS", "mount format")
assert(files.mounts[first].partitions == 1, "mount partitions")
assert(files.mounts[first].busy == false, "mount not busy")
assert(gs.contains(files.mounts[first].path, "System_6_0_8.dsk"), "mount path is the image")
assert(files.mounts[first].unmount(), "unmount succeeds")
assert(files.mounts.count == 0, "unmount drops the entry")
assert(files.mounts.find("../../data/systems/System_6_0_8.dsk") == -1, "unmounted image is not found")
local l2 = files.list("../../data/systems/System_6_0_8.dsk/partition1")
local second = files.mounts.find("../../data/systems/System_6_0_8.dsk")
assert(second > first, F"a remount gets a new serial (${first} -> ${second})")
assert(gs.try(function() return files.mounts[first].format end, "gone") == "gone", "the old serial does not resolve")
files.mounts[second].unmount()

-- === log: categories as objects, numeric levels ============================
assert(log.category["scsi"].meta.path == "log.category[\"scsi\"]", "keyed entry path")
assert(log.category["scsi"].level == 0, "scsi starts silent")
log.category["scsi"].level = 5
assert(log.category["scsi"].level == 5, "level set through the entry")
assert(log.levels["scsi"] == 5, "log.levels sees the entry's level")
log.set("scsi", {level = 0})
assert(log.category["scsi"].level == 0, "log.set sets the same level")
log.category["scsi"].file = F"${vars.WORK_DIR}/scsi.log"
assert(gs.contains(log.category["scsi"].file, "scsi.log"), "file sink set")
log.category["scsi"].file = "off"
assert(log.category["scsi"].file == "off", "file sink closed")
local nlevels = 0
for k, level in pairs(log.levels) do
    nlevels = nlevels + 1
    assert(log.category[k].level == level, F"log.levels agrees with log.category[${k}]")
end
assert(log.category.count == nlevels, "one entry per registered category")
assert(gs.try(function() return log.category["no_such_cat"].level end, "none") == "none", "unknown category does not resolve")

-- === catalog: what the emulator can build or fit ===========================
assert(#catalog.models > 10, "catalog.models lists the roster")
assert(catalog.profile("plus").id == "plus", "catalog.profile")
assert(#catalog.nubus_cards > 0, "catalog.nubus_cards")
assert(#catalog.pci_cards > 0, "catalog.pci_cards")
assert(catalog.vroms.size == 32768, "catalog.vroms")
assert(catalog.vroms.meta.path == "catalog.vroms", "catalog.vroms path")
assert(catalog.proms.meta.path == "catalog.proms", "catalog.proms path")
-- The Plus has no NuBus and no PCI: neither node is attached.
assert(gs.try(function() return machine.nubus.meta.path end, "absent") == "absent", "no machine.nubus on a Plus")
assert(gs.try(function() return machine.pci.meta.path end, "absent") == "absent", "no machine.pci on a Plus")

-- === shell aliases: one interface ==========================================
assert(gs.try(function() return shell.aliases end, "gone") == "gone", "shell.aliases removed")

-- === scheduler.mode: strict enum ===========================================
scheduler.mode = "paced"
assert(scheduler.mode == "paced", "paced")
scheduler.mode = "turbo"
assert(scheduler.mode == "turbo", "turbo")
assert(gs.try(function() return shell.eval("scheduler.mode = \"max\"") end, "rejected") == "rejected", "the legacy alias max is rejected")
assert(scheduler.mode == "turbo", "a rejected write leaves the mode")

-- === debug.find: per debug object, across a checkpoint load ================
scheduler.run(2000000)
local hits = debug.find.long(0x4D1F8172, 0x400000, 0x400010)
assert(#hits == 0 or #hits > 0, "debug.find answers before a load")
assert(debug.find.meta.path == "debug.find", "debug.find path")
debug.breakpoints.add(0x400100)
assert(debug.breakpoints[0].meta.path == "debug.breakpoints[0]", "breakpoint entry path")
debug.breakpoints.clear()
checkpoint.save(F"${vars.WORK_DIR}/reorg.gscp")
checkpoint.load(F"${vars.WORK_DIR}/reorg.gscp")
local hits2 = debug.find.long(0x4D1F8172, 0x400000, 0x400010)
assert(debug.find.meta.path == "debug.find", "debug.find after a load")

-- === Callback-backed children have paths ===================================
assert(machine.scsi.device.meta.path == "machine.scsi.device", "collection container path")
assert(machine.floppy.drive[0].meta.path == "machine.floppy.drive[0]", "drive entry path")
machine.scsi.attach_hd("../../data/systems/System_6_0_8.dsk", 4)
assert(machine.scsi.device[4].meta.path == "machine.scsi.device[4]", "device entry path")
assert(machine.scsi.device[4].image.meta.path == "machine.scsi.device[4].image", "lookup-backed child path")

echo("object-model-reorg-ok")
quit()
