-- Converted from q7.script by scripts/script-to-lua.py
local mac = dofile("../lib/mac.lua")
local sys_701 = "../../data/systems/System_7_0_1.image"
local function probe(label, v)
    machine.boot{model = "iifx", ram = 16384, video_card = "mdc_8_24", rom = vars.ROM}
    machine.rtc.pram.validate()
    machine.rtc.pram.poke(0x8a, v)
    machine.floppy.drive[0].insert(sys_701)
    scheduler.run(200000000)
    mac.wait_stable(900000000)
    echo(F"RESULT $8A=${label}: MMU32bit=${debug.mac.globals.read(\"MMU32bit\")}")
end
probe("01", gs.bytes(0x01, 1))
probe("04", gs.bytes(0x04, 1))
quit()
