-- Converted from test.script by scripts/script-to-lua.py
-- The second SCSI bus and /RESET -- see config.mk.

-- 53C9x register offsets on $10 centres (the same chip on every board here).
local R_FIFO = 0x20
local R_COMMAND = 0x30
local R_DESTID = 0x40
local R_CONFIG1 = 0x80
local R_CONFIG2 = 0xB0

-- One PREVENT ALLOW MEDIUM REMOVAL (prevent) to target `id` through the chip
-- at `chip`, run to bus free: flush the FIFO, the CDB into it, Select without
-- ATN, then Initiator Command Complete and Message Accepted.  Answers the
-- status byte.
local function prevent_once(chip, id)
    machine.memory.poke.b(chip + R_COMMAND, 0x01)
    machine.memory.poke.b(chip + R_DESTID, id)
    machine.memory.poke.b(chip + R_FIFO, 0x1E)
    machine.memory.poke.b(chip + R_FIFO, 0x00)
    machine.memory.poke.b(chip + R_FIFO, 0x00)
    machine.memory.poke.b(chip + R_FIFO, 0x00)
    machine.memory.poke.b(chip + R_FIFO, 0x01)
    machine.memory.poke.b(chip + R_FIFO, 0x00)
    machine.memory.poke.b(chip + R_COMMAND, 0x41)
    machine.memory.poke.b(chip + R_COMMAND, 0x11)
    local status = machine.memory.peek.b(chip + R_FIFO)
    machine.memory.poke.b(chip + R_COMMAND, 0x12)
    return status
end

-- Lock the drive: the first command after an insertion reports its unit
-- attention (CHECK CONDITION), the second is the one that takes.
local function prevent(model, chip, id)
    prevent_once(chip, id)
    assert(prevent_once(chip, id) == 0, F"${model}: PREVENT to ID ${id} did not complete GOOD")
    assert(gs.try(function() return machine.eject_media("scsi2", id) end, "locked") == "locked", F"${model}: the PREVENT did not lock the drive")
end

-- A 53C96's configuration is cleared by /RESET (CONFIG1 keeps its bus ID).
local function chip_reset(model, chip)
    machine.memory.poke.b(chip + R_CONFIG1, 0x47)
    machine.memory.poke.b(chip + R_CONFIG2, 0x18)
    assert(machine.memory.peek.b(chip + R_CONFIG1) == 0x47, F"${model}: CONFIG1 at ${chip:08x} did not take the write")
    machine.reset()
    assert(machine.memory.peek.b(chip + R_CONFIG1) == 0x07, F"${model}: /RESET left the 53C96 at ${chip:08x} configured")
    assert(machine.memory.peek.b(chip + R_CONFIG2) == 0x00, F"${model}: /RESET left CONFIG2 at ${chip:08x} set")
end

-- A CD on machine.scsi2 at `id`, locked through the chip at `chip`, is
-- released by /RESET.
local function second_bus_reset(model, chip, id)
    assert(machine.scsi2.device[id].type == "cdrom", F"${model}: no CD-ROM drive at machine.scsi2 ID ${id}")
    machine.scsi2.device[id].insert(F"${vars.WORK_DIR}/cd.iso")
    prevent(model, chip, id)
    machine.reset()
    machine.eject_media("scsi2", id)
    assert(machine.scsi2.device[id].medium_present == false, F"${model}: /RESET did not release the lock on machine.scsi2")
end

-- --- Quadra 700: the one 53C96 at $50F0F000.
chip_reset("q700", 0x50F0F000)

-- --- Quadra 900: media at run time on the external bus, both chips reset,
-- and the external bus released by /RESET.
machine.boot{model = "q900", ram = 8192, rom = F"${vars.TEST_DATA}/roms/q700-q900-420dbff3.rom"}
local b = machine.attach_media("scsi2", 4, "hd", F"${vars.WORK_DIR}/hd.img")
assert(b.bus == "scsi2" and b.id == 4, "q900: attach_media answers scsi2 ID 4")
assert(machine.scsi2.device[4].type == "hd", "q900: the disk is on machine.scsi2")
machine.eject_media("scsi2", 4)
assert(gs.try(function() return machine.eject_media("scsi2", 4) end, "empty") == "empty", "q900: eject_media took the disk back")
chip_reset("q900", 0x50F0F000)
chip_reset("q900 external", 0x50F0F400)
machine.attach_media("scsi2", 3, "cd", F"${vars.WORK_DIR}/cd.iso")
assert(machine.scsi2.device[3].type == "cdrom", "q900: a CD-ROM drive at machine.scsi2 ID 3")
prevent("q900", 0x50F0F400, 3)
machine.reset()
machine.eject_media("scsi2", 3)
assert(machine.scsi2.device[3].medium_present == false, "q900: /RESET did not release the lock on machine.scsi2")

-- --- Quadra 840AV: the one 53C96 at $50F18000.
machine.boot{model = "q840av", ram = 8192, rom = F"${vars.TEST_DATA}/roms/q840av-q660av-5bf10fd1.rom"}
chip_reset("q840av", 0x50F18000)

-- --- Power Macintosh 8500: the 53C94 at Grand Central +$10000 drives
-- machine.scsi2, the CD-ROM drive at ID 3.
machine.boot{model = "pm8500", ram = 32768, rom = F"${vars.TEST_DATA}/roms/pm7500-pm8500-pm9500-96cd923d.rom"}
chip_reset("pm8500", 0xF3010000)
second_bus_reset("pm8500", 0xF3010000, 3)

echo("scsi-second-bus-ok")
quit()
