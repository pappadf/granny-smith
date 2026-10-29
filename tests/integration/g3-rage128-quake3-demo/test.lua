-- Converted from test.script by scripts/script-to-lua.py
-- Quake III Arena's demo `four` on the G3 with a Rage 128 GL, timed.
--
-- The G3 of g3-rage128-macos921 with 256 MB, booting gs-test-data's
-- stripped 9.2.1 disk (systems/mac_os_9_2_1_516mb_g3_rage128_demos.dmg),
-- and the Quake III 1.30 CD cut down to that demo
-- (apps/quake3_1_30_demo_four_cd.iso, HFS volume "Q3A").  The Finder opens
-- the CD and starts Quake3; the CD key screen takes no key (Tab to ACCEPT,
-- then Return: Quake reads the mouse as deltas, so a click cannot be
-- aimed); the console runs `demo four`: 63 s of an eight-player FFA on
-- q3dm6 that ends when THEINDIGO reaches the fraglimit, after which Quake
-- disconnects and returns to its main menu.
--
-- The row times the demo from the Return that enters the command to the
-- point it ends.  Before it, `set nextdemo "echo gsdemoend"`: when a demo
-- finishes, Quake disconnects and runs the `nextdemo` cvar
-- (CL_DemoCompleted, CL_NextDemo), so `gsdemoend` is printed on its console
-- at that moment, whatever draws the screen.  Quake's console text is a
-- ring of 16-bit cells (colour byte, character) in guest RAM, found by the
-- command's own echo; the run goes on in slices until the console holds
-- " hit the fraglimit" (the demo's last event) and then, in finer slices,
-- until the `gsdemoend` line -- the one not followed by the quote that ends
-- the echoed `set` command.  Printed: host
-- wall-clock and CPU time, guest time (cycles over the clock), instructions,
-- the frames Quake drew (the card's page flips) and the frame rate in guest
-- and in host time, the 3D primitives.  Asserted: the demo ends, every
-- file it loads is on the CD (no "could not find" on the console), and it
-- drew.

local mac = dofile("../lib/mac.lua")

local hd = "../../data/systems/mac_os_9_2_1_516mb_g3_rage128_demos.dmg"
local cd = "../../data/apps/quake3_1_30_demo_four_cd.iso"

-- Console cells: colour 7 (white) for the echo and the engine's lines,
-- colour 3 (yellow) for warnings
-- The row's name and CPU engine (run.sh of g3-rage128-quake3-demo-jit
-- sets both)
local ROW = (vars.ROW_NAME or "g3-rage128-quake3-demo")
local ENGINE = (vars.CPU_ENGINE or "default")

local PAT_ECHO = "07 5d 07 5c 07 64 07 65 07 6d 07 6f 07 20 07 66 07 6f 07 75 07 72"
local PAT_FRAGLIMIT = "07 20 07 68 07 69 07 74 07 20 07 74 07 68 07 65 07 20 07 66 07 72 07 61 07 67 07 6c 07 69 07 6d 07 69 07 74"
-- "gsdemoend": the echo of the nextdemo cvar at the demo's end
local PAT_END = "07 67 07 73 07 64 07 65 07 6d 07 6f 07 65 07 6e 07 64"
local PAT_MISSING = "03 63 03 6f 03 75 03 6c 03 64 03 20 03 6e 03 6f 03 74 03 20 03 66 03 69 03 6e 03 64"

local function q3_cmdkey(k)
    machine.adb.keyboard.down("Command")
    scheduler.run(60000000)
    machine.adb.keyboard.down(k)
    scheduler.run(60000000)
    machine.adb.keyboard.up(k)
    machine.adb.keyboard.up("Command")
    scheduler.run(2000000000)
end
-- Select a Finder item by typing its name, then File > Open
local function q3_open(name, run)
    machine.adb.keyboard.type(name)
    scheduler.run(1500000000)
    q3_cmdkey("o")
    scheduler.run(run)
end
-- Has the console printed `gsdemoend` itself, not just echoed the typed
-- `set nextdemo "echo gsdemoend"` (where a quote follows the word)?
local function q3_ended(lo, hi)
    for _, a in ipairs(debug.find.bytes(PAT_END, lo, hi)) do
        if (machine.memory.peek.w(a + 18) & 0xff) ~= 0x22 then
            return true
        end
    end
    return false
end
-- Thousandths as d.ddd
local function milli(x)
    return (F"${x // 1000}.${x % 1000:03d}")
end

local function q3_run()
    local doc = "{\"cards\":[{\"slot\":\"pci_1\",\"card\":\"rage128\",\"options\":{\"memory\":\"32m\"}}],\"displays\":{\"pci_1\":{\"monitor\":\"13in_rgb\"}}}"
    machine.boot{model = "pmg3dt", ram = 262144, rom = vars.ROM, config = doc}
    -- ENGINE=jit (g3-rage128-quake3-demo-jit, the wasm build under Node):
    -- the browser's "JIT + kernel" -- the predecoded loop, and hot code,
    -- supervisor code too, translated to WebAssembly
    if ENGINE == "jit" then
        predecode.enabled = 1
        jit.supervisor = 1
        jit.enabled = 1
        assert(jit.enabled == 1, "this build has no translation tier (ENGINE=jit needs the wasm build)")
    end
    echo(tostring(ROW) .. ": engine " .. tostring(ENGINE) .. ", predecode " .. tostring(predecode.enabled) .. ", jit " .. tostring(jit.enabled))
    machine.rtc.time = "2026-10-01T12:00:00"
    machine.scsi.attach_hd(hd, 0)
    machine.scsi.attach_cdrom(cd, 3)
    scheduler.run(30000000000)
    -- A system error is a positive DSErrCode; start-up leaves negative
    -- status values there in passing (as the machine watch reads it)
    local ds = debug.mac.globals.read("DSErrCode")
    assert(ds == 0 or ds >= 0x8000, F"system error ${ds:04x} during startup")

    -- The CD (the desktop's "Q3A"), its Quake3 folder, the Quake3 application
    q3_open("Q3A", 6000000000)
    q3_open("Quake3", 6000000000)
    q3_open("Quake3", 32000000000)
    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/cdkey.png") end, nil)
    -- The CD key screen: no key needed, ACCEPT by keyboard
    machine.adb.keyboard.press("tab")
    scheduler.run(600000000)
    machine.adb.keyboard.down("return")
    scheduler.run(200000000)
    machine.adb.keyboard.up("return")
    scheduler.run(4000000000)
    -- The held Return goes on into Single Player: back to the main menu
    machine.adb.keyboard.press("escape")
    scheduler.run(2000000000)
    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/menu.png") end, nil)

    -- The console; the end marker; the command
    machine.adb.keyboard.down(0x32)
    scheduler.run(60000000)
    machine.adb.keyboard.up(0x32)
    scheduler.run(1500000000)
    machine.adb.keyboard.type("set nextdemo \"echo gsdemoend\"")
    scheduler.run(1000000000)
    machine.adb.keyboard.press("return")
    scheduler.run(1000000000)
    machine.adb.keyboard.type("demo four")
    scheduler.run(500000000)

    local hz = scheduler.frequency
    local i0 = scheduler.instr_count
    local c0 = scheduler.cycles
    local w0 = scheduler.host_wall_ns
    local u0 = scheduler.host_user_ns
    local f0 = machine.pci.slot[1].card.regs.flips
    local p0 = machine.pci.slot[1].card.regs.prims3d
    local j0 = jit.instructions
    -- The status lines over exactly the demo: the card's says where its
    -- host time went (3D, 2D, scanout)
    log.status_every = 0
    local _ = log.status
    machine.adb.keyboard.press("return")
    scheduler.run(20000000)
    local echo = debug.find.bytes(PAT_ECHO, 0, 0x10000000)
    assert(#echo == 1, F"the console's echo of the command (found ${#echo} times)")
    local lo = echo[1] - 0x10000
    local hi = echo[1] + 0x10000
    echo(tostring(ROW) .. ": the console is around " .. ("%08x"):format(echo[1]))

    -- Until the demo's last event (coarse), then until `gsdemoend` (fine)
    local ceiling = 60000000000
    local spent = 20000000
    while spent < ceiling and #debug.find.bytes(PAT_FRAGLIMIT, lo, hi) == 0 do
        scheduler.run(100000000)
        spent = spent + 100000000
    end
    assert(spent < ceiling, "the demo never reached its fraglimit")
    while spent < ceiling and q3_ended(lo, hi) == false do
        scheduler.run(2000000)
        spent = spent + 2000000
    end
    assert(spent < ceiling, "Quake never ran nextdemo (gsdemoend) after the demo")

    local di = scheduler.instr_count - i0
    local gms = (scheduler.cycles - c0) * 1000 // hz
    local wms = (scheduler.host_wall_ns - w0) // 1000000
    local ums = (scheduler.host_user_ns - u0) // 1000000
    local frames = machine.pci.slot[1].card.regs.flips - f0
    local prims = machine.pci.slot[1].card.regs.prims3d - p0
    local dj = jit.instructions - j0
    local _ = log.status
    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/end.png") end, nil)
    echo(tostring(ROW) .. ": demo four, from Return to its end (within 2 M instructions):")
    echo(tostring(ROW) .. ":   host wall-clock " .. tostring(milli(wms)) .. " s, host CPU " .. tostring(milli(ums)) .. " s")
    echo(tostring(ROW) .. ":   guest " .. tostring(milli(gms)) .. " s (" .. tostring(scheduler.cycles - c0) .. " cycles at " .. tostring(hz) .. " Hz)")
    echo(tostring(ROW) .. ":   " .. tostring(di) .. " instructions: " .. tostring(di // 1000 // gms) .. " MIPS guest, " .. tostring(di // 1000 // wms) .. " MIPS host")
    echo(tostring(ROW) .. ":   speed " .. tostring(milli(gms * 1000 // wms)) .. "x real time")
    echo(tostring(ROW) .. ":   " .. tostring(frames) .. " frames (page flips): " .. tostring(milli(frames * 1000000 // gms)) .. " fps guest, " .. tostring(milli(frames * 1000000 // wms)) .. " fps host")
    echo(tostring(ROW) .. ":   " .. tostring(prims) .. " 3D primitives, raster " .. tostring(machine.pci.slot[1].card.regs.raster))
    echo(tostring(ROW) .. ":   engine " .. tostring(ENGINE) .. ": " .. tostring(dj * 100 // di) .. "% of the instructions ran translated")

    local missing = #debug.find.bytes(PAT_MISSING, lo, hi)
    assert(missing == 0, F"${missing} files the demo loads are not on the CD (could not find)")
    assert(frames > 500, F"Quake drew ${frames} frames")
    assert(prims > 100000, F"the 3D engine drew ${prims} primitives")
    echo(tostring(ROW) .. ": Quake III plays demo four to its end")
end

-- Both disc images are private test data.  Without them the row skips, by
-- not running (see g3-rage128-opengl).
if mac.have_media(cd) and mac.have_media(hd) then
    q3_run()
else
    echo(F"skip: ${cd} or ${hd} not present")
end
quit()
