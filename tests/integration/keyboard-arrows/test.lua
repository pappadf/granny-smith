-- Converted from test.script by scripts/script-to-lua.py
-- Run to the ROM's boot-disk wait, where the keyboard driver keeps KeyMap.
scheduler.run(60000000)
assert(machine.memory.peek.l(0x174) == 0 and machine.memory.peek.l(0x178) == 0 and machine.memory.peek.l(0x17C) == 0 and machine.memory.peek.l(0x180) == 0, "KeyMap idle")

-- KeyMap bit for virtual key code k: byte $174 + k/8, bit k%8.
local function arrow(name, byte_addr, mask)
    machine.adb.keyboard.down(name)
    scheduler.run(4000000)
    assert(machine.memory.peek.b(byte_addr) & mask, F"${name}: its arrow bit is not set")
    -- Right Shift / Option / Control are virtual $3C-$3E: byte $17B bits 4-6.
    assert((machine.memory.peek.b(0x17B) & 0x70) == 0, F"${name}: pressed a right-hand modifier")
    machine.adb.keyboard.up(name)
    scheduler.run(4000000)
    assert(machine.memory.peek.l(0x180) == 0, F"${name}: still held after up")
end
-- Left $7B, Right $7C, Down $7D, Up $7E: byte $183, bits 3-6.
arrow("left", 0x183, 0x08)
arrow("right", 0x183, 0x10)
arrow("down", 0x183, 0x20)
arrow("up", 0x183, 0x40)

-- The Lisa's keymap takes the same raw codes.
machine.boot{model = "lisa", rom = F"${vars.TEST_DATA}/roms/lisa2-revh-098917b2.rom"}
-- The Lisa accepts the left arrow (a refused key is an error)
machine.adb.keyboard.press("left")

echo("keyboard-arrows-ok")
quit()
