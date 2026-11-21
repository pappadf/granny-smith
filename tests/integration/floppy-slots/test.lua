-- Converted from test.script by scripts/script-to-lua.py
-- The q700 started with fd=a fd=b: a in drive 0, b nowhere.
assert(machine.floppy.drive[0].present, "fd= filled drive 0")
assert(machine.floppy.drive[0].disk.filename == F"${vars.WORK_DIR}/a.dsk", "drive 0 holds the first fd=")
assert(gs.try(function() return machine.floppy.drive[1].present end, "none") == "none", "no drive 1 on a Quadra 700")

-- The same on the PDM and TNT boards, whose drive 1 was a phantom "occupied".
local function one_drive(model, rom, ram_kb)
    machine.boot{model = model, ram = ram_kb, rom = F"${vars.TEST_DATA}/roms/${rom}"}
    assert(gs.try(function() return machine.floppy.drive[1].present end, "none") == "none", F"${model}: no drive 1")
    machine.floppy.drive[0].insert(F"${vars.WORK_DIR}/b.dsk")
    assert(machine.floppy.drive[0].present, F"${model}: drive 0 takes a disk")
    assert(machine.floppy.drive[0].insert(F"${vars.WORK_DIR}/a.dsk") == false, F"${model}: an occupied drive 0 refuses")
end
one_drive("pm6100", "pm6100-pm7100-pm8100-9feb69b3.rom", 16384)
one_drive("ans500", "ans500-ans700-962f6c13.rom", 32768)

-- A two-drive machine keeps its second drive.
machine.boot{model = "se30", rom = F"${vars.TEST_DATA}/roms/iix-iicx-se30-97221136.rom"}
assert(machine.floppy.drive[1].present == false, "SE/30 drive 1 exists and is empty")
machine.floppy.drive[1].insert(F"${vars.WORK_DIR}/b.dsk")
assert(machine.floppy.drive[1].present, "SE/30 drive 1 takes a disk")

echo("floppy-slots-ok")
quit()
