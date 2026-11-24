-- Converted from test.script by scripts/script-to-lua.py
-- OpenGL on an ATI Rage 128 GL under Mac OS 9.2.1.
--
-- The boot of g3-rage128-macos921, then the OpenGL SDK CD: the Finder
-- opens Sample Apps:GLUT:book and runs two samples, whose sources are on
-- the same CD (Source:Sample Code:GLUT:book).
--   * checker: a 64 x 64 checkerboard on a flat quad and on a quad seen in
--     perspective.  ATI's driver sends S and T pre-multiplied by W
--     (SETUP_CNTL TEXTURE_ST_FORMAT); read as affine, the board came out
--     scaled by 1/w.
--   * mipmap: a plane receding to the horizon with six hand-made mip
--     levels, each its own colour — the bands show the level selection.
--   * fog: five lit spheres at increasing depth in exponential fog, Z
--     buffered.
-- Each frame matches a golden; the counts show the 3D engine drew it.
-- The samples are static: an animated one (texbind) lands on a different
-- frame from run to run, so it is not pinned.
--
-- The mouse is the ADB mouse in its default (delta) mode, stepped to its
-- target: under 9.2.1 the "global" warp of lib/mac.script's click() is
-- not reliable.

local mac = dofile("../lib/mac.lua")

local hd = "../../data/systems/mac_os_9_2_1_516mb_g3.img"
local cd = "../../data/apps/OpenGL_SDK_1.2.img_.bin"

local function gl_goto(x, y)
    local k = 0
    while k < 8 do
        machine.adb.mouse.move(x, y)
        scheduler.run(120000000)
        k = k + 1
    end
end
local function gl_click(x, y)
    gl_goto(x, y)
    machine.adb.mouse.click(true)
    scheduler.run(100000000)
    machine.adb.mouse.click(false)
    scheduler.run(600000000)
end
local function gl_cmdkey(k)
    machine.adb.keyboard.down("Command")
    scheduler.run(60000000)
    machine.adb.keyboard.down(k)
    scheduler.run(60000000)
    machine.adb.keyboard.up(k)
    machine.adb.keyboard.up("Command")
    scheduler.run(2000000000)
end
-- Select a Finder item by typing its name, then File > Open.
local function gl_open(name, run)
    machine.adb.keyboard.type(name)
    scheduler.run(1000000000)
    gl_cmdkey("o")
    scheduler.run(run)
end

-- Each sample is quit before the next opens from the book window.
local function gl_sample(name)
    gl_cmdkey("q")
    scheduler.run(4000000000)
    local a = machine.pci.slot[1].card.regs.prims3d
    gl_open(name, 16000000000)
    local b = machine.pci.slot[1].card.regs.prims3d
    echo(F"g3-rage128-opengl: ${name} drew ${b - a} primitives")
    assert(b > a, F"${name} draws through the 3D engine")
    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/${name}.png") end, nil)
    mac.check_ex(F"goldens/${name}.png", 0, 440, 20, 640)
end

-- The row itself: the boot, the Finder walk and the three samples.
local function gl_run()
    machine.boot{model = "pmg3dt", ram = 65536, rom = vars.ROM, pci_card = "rage128", monitor = "none"}
    machine.rtc.time = "2026-10-01T12:00:00"
    machine.scsi.attach_hd(hd, 0)
    scheduler.run(24000000000)
    assert(debug.mac.globals.read("DSErrCode") == 0, F"system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")

    machine.scsi.attach_cdrom(cd, 3)
    scheduler.run(8000000000)
    -- Quit the Setup Assistant (first boot) and confirm.
    gl_cmdkey("q")
    scheduler.run(2000000000)
    gl_click(461, 173)
    scheduler.run(4000000000)
    -- The CD's icon on the desktop, then Sample Apps (the window's first
    -- folder), GLUT, book.
    gl_click(592, 105)
    gl_cmdkey("o")
    scheduler.run(8000000000)
    gl_click(85, 97)
    gl_cmdkey("o")
    scheduler.run(6000000000)
    gl_open("GLUT", 6000000000)
    gl_open("book", 8000000000)

    local p0 = machine.pci.slot[1].card.regs.prims3d
    gl_open("checker", 16000000000)
    local p1 = machine.pci.slot[1].card.regs.prims3d
    echo(F"g3-rage128-opengl: checker drew ${p1 - p0} primitives")
    assert(p1 > p0, "checker draws through the 3D engine")
    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/checker.png") end, nil)
    mac.check_ex("goldens/checker.png", 0, 440, 20, 640)

    gl_sample("mipmap")
    gl_sample("fog")

    echo("g3-rage128-opengl: ATI's OpenGL renderer draws through the Rage 128's 3D engine")
end

-- The SDK CD is private test data.  Without it the row skips -- by not
-- running, rather than by an early `quit`: headless stops a script's job
-- asynchronously, so a statement after `quit` can start and be cancelled.
if mac.have_media(cd) then
    gl_run()
else
    echo(F"skip: ${cd} not present")
end
quit()
