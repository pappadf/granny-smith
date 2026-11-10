-- Converted from test.script by scripts/script-to-lua.py
local mac = dofile("../lib/mac.lua")

-- Run until the screen is `w` x `h`, in 40 M polls, up to the 720 M the ROM
-- ladder needs for the native driver's mode set.
local function until_screen(w, h)
    local spent = 0
    while spent < 720000000 do
        scheduler.run(40000000)
        spent = spent + 40000000
        if machine.screen.width == w and machine.screen.height == h then return spent end
    end
    return spent
end

-- The port and its monitors are published through the profile: four
-- monitors ("no monitor" is not one -- an unplugged port is the connected
-- display being another device).
local bv = catalog.profile("pm7500").displays.builtin
assert(#bv.monitors == 4, "the Control port lists four monitors")
assert(#catalog.profile("pm8500").displays.builtin.monitors == 4, "so does the 8500's")

-- Open Firmware reads the sense lines and mode-sets Control for the monitor
-- it finds; the ROM ladder puts the native driver's mode set inside 720 M
-- instructions.
machine.boot{model = "pm7500", ram = 32768, monitor = "twopage", rom = vars.ROM}
until_screen(1152, 870)
assert(machine.screen.width == 1152, F"twopage: 1152 pixels across (got ${machine.screen.width})")
assert(machine.screen.height == 870, F"twopage: 870 lines (got ${machine.screen.height})")

-- A power cycle keeps the strap: the monitor stays plugged in.  (A fixed
-- budget: the screen is already 1152 x 870 before the restart, so a poll
-- would prove nothing.)
machine.restart()
scheduler.run(720000000)
assert(machine.screen.width == 1152, "after restart: still 1152 across")
assert(machine.screen.height == 870, "after restart: still 870 lines")

machine.boot{model = "pm7500", ram = 32768, monitor = "portrait", rom = vars.ROM}
until_screen(640, 870)
assert(machine.screen.width == 640, F"portrait: 640 pixels across (got ${machine.screen.width})")
assert(machine.screen.height == 870, F"portrait: 870 lines (got ${machine.screen.height})")

-- The default is still the 13"/14" strap.
machine.boot{model = "pm7500", ram = 32768, rom = vars.ROM}
until_screen(640, 480)
assert(machine.screen.width == 640, "default: 640 pixels across")
assert(machine.screen.height == 480, "default: 480 lines")

echo("tnt-monitor-ok")
quit()
