-- Converted from test.script by scripts/script-to-lua.py
-- Per-device monitor sense (see config.mk).

-- The guest's screens: GDevices on DeviceList ($8A8), each a handle whose
-- record links the next at gdNextGD (+$1E); the main one is MainDevice
-- ($8A4), whose PixMap (gdPMap, +$16) gives the base address it scans.
local function pk(a) return machine.cpu.mmu.peek(a, 4) end
local function screens()
    local n = 0
    local h = pk(0x8A8)
    while h ~= 0 and n < 8 do
        n = n + 1
        h = pk(pk(h) + 0x1E)
    end
    return n
end
-- ...and the NuBus slot whose space that base is in ($Fs000000).
local function main_slot()
    return (pk(pk(pk(pk(pk(0x8A4)) + 0x16))) >> 24) & 0xF
end

-- Boot `config` on the IIci, from 7.0.1, to the Finder.
local function boot(config)
    machine.boot{model = "iici", ram = 8192, rom = vars.ROM, config = config}
    machine.rtc.time = 3000000000
    machine.floppy.drive[0].insert("../../data/systems/System_7_0_1.image")
end

local card = "{\"cards\":[{\"slot\":\"nubus_c\",\"card\":\"mdc_8_24\"}]"

-- --- The card connected, the built-in port unplugged. -----------------------
boot(F"${card},\"displays\":{\"builtin\":{\"monitor\":\"none\"},\"nubus_c\":{\"monitor\":\"13in_rgb\"}}}")
assert(machine.nubus.slot[11].card.monitor == "none", "the built-in port has a monitor")
assert(machine.nubus.slot[11].card.sense == 7, "the built-in port's sense lines are not open")
assert(machine.nubus.slot[12].card.monitor == "13in_rgb", "the card's monitor")
assert(machine.nubus.slot[12].card.sense == 6, "the card's sense code")
scheduler.run(300000000)
assert(screens() == 1, F"card connected: the System sees ${screens()} screens")
assert(main_slot() == 12, F"card connected: the main screen is not the card's (slot ${main_slot()})")
assert(machine.screen.width == 640, "card connected: no picture from the card")

-- --- The built-in video connected, the card's connector unplugged. ----------
boot(F"${card}}")
assert(machine.nubus.slot[11].card.monitor == "13in_rgb", "the built-in port's monitor")
assert(machine.nubus.slot[12].card.monitor == "none", "the card has a monitor (one monitor per configuration)")
assert(machine.nubus.slot[12].card.sense == 7, "the card's sense lines are not open")
scheduler.run(300000000)
assert(screens() == 1, F"built-in connected: the System sees ${screens()} screens")
assert(main_slot() == 11, F"built-in connected: the main screen is not the built-in video's (slot ${main_slot()})")

-- --- The legacy arguments: the card keeps its own default monitor. ----------
machine.boot{model = "iici", ram = 8192, rom = vars.ROM, video_card = "mdc_8_24"}
machine.rtc.time = 3000000000
machine.floppy.drive[0].insert("../../data/systems/System_7_0_1.image")
assert(machine.nubus.slot[12].card.monitor == "13in_rgb", "the legacy card's default monitor")
scheduler.run(300000000)
assert(screens() == 2, F"legacy arguments: the System sees ${screens()} screens, not both")
quit()
