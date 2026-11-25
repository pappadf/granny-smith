-- osx-interactive — interactive use of Mac OS X 10.0.3 (Aqua) on the beige G3:
-- the work a user waits for, not a CPU-bound window.  Run by
-- scripts/bench/scenario.py (docs/guide/BENCHMARKS.md, "Interactive
-- scenarios"); not a metric-A/B row.
--
-- The disk is gs-test-data `systems/mac_os_x_10_0_3_2gb_g3_minimal_aqua.dmg`
-- with its sidecar configuration (Apple's Gossamer nvramrc patch, the ATA
-- boot-device, the 17" multiple-scan monitor), attached over a delta.
--
--   MODE=boot  boot to the Finder (10 G + 15 G scheduler slots, the second
--              stretch letting the desktop settle into its idle state) and
--              save CKPT
--   MODE=run   load CKPT and play the interactions twice (round 1 cold, round
--              2 warm: the translation tier compiles in round 1): open and
--              close the Finder's File menu, minimize the Finder window into
--              the Dock (the genie), restore it from the Dock.  Each is a
--              fixed stretch of scheduler slots, so every build and executor
--              runs the same guest instructions; each prints one @@SCEN line
--              with its host wall time and the guest instructions it retired.
--   MODE=live  the same on the running machine, already at the Finder, with
--              no checkpoint (the browser run: a checkpoint of this 256 MB
--              machine takes longer than the page's 3 s heartbeat watch)
--
-- The pointer: OS X keeps no Toolbox cursor globals, so "global" moves do not
-- work.  osx_goto() slams the pointer into the top-left corner and walks it
-- out in 2-count ADB steps, where OS X 10.0's acceleration is a flat 2.26
-- pixels per count (measured).
--
-- Guards: framebuffer checksums of the regions the interactions change.  The
-- File menu appears and goes away again; the window leaves its place for the
-- Dock and comes back pixel for pixel.  A scenario that stopped doing what it
-- claims fails its run rather than timing something else.

function osx_goto(x, y)
    local i = 0
    while i < 20 do
        machine.adb.mouse.move(-100, -100, "relative")
        scheduler.run(20000000)
        i = i + 1
    end
    local cx = x * 100 // 226
    local cy = y * 100 // 226
    while cx > 0 or cy > 0 do
        local sx = cx
        if cx >= 2 then
            sx = 2
        end
        local sy = cy
        if cy >= 2 then
            sy = 2
        end
        machine.adb.mouse.move(sx, sy, "relative")
        scheduler.run(20000000)
        cx = cx - sx
        cy = cy - sy
    end
    scheduler.run(50000000)
end

function osx_click()
    machine.adb.mouse.click(true)
    scheduler.run(30000000)
    machine.adb.mouse.click(false)
end

-- Run `slots` scheduler slots and report the host time and the guest
-- instructions they took
function scen_phase(name, slots)
    local w0 = scheduler.host_wall_ns
    local i0 = machine.cpu.instr_count
    scheduler.run(slots)
    local di = machine.cpu.instr_count - i0
    local ns = scheduler.host_wall_ns - w0
    echo(F"@@SCEN {\"phase\":\"${name}\",\"ns\":${ns},\"instr\":${di}}")
end

-- The regions the guards read: the File menu's drop-down, and the Finder
-- window below its title bar (the buttons there keep a rollover tint after
-- the restore)
function menu_sum()
    return machine.screen.checksum(24, 110, 300, 318)
end
function window_sum()
    return machine.screen.checksum(70, 68, 474, 610)
end

if vars.MODE == "boot" then
    -- CPI pinned at 2, the value the stretches and guards were placed with,
    -- so a change of the machine's default CPI does not move them (the
    -- checkpoint carries it into MODE=run)
    scheduler.cpi = 2
    machine.rtc.time = "2026-10-01T12:00:00"
    machine.attach_media("ata0", 0, "hd", F"${vars.TEST_DATA}/systems/mac_os_x_10_0_3_2gb_g3_minimal_aqua.dmg")
    scheduler.run(10000000000)
    scheduler.run(15000000000)
    assert(machine.screen.width == 1024, F"osx-interactive: the desktop is not at 1024 x 768 (${machine.screen.width})")
    checkpoint.save(vars.CKPT)
    echo(F"@@SCEN {\"phase\":\"boot\",\"instr\":${machine.cpu.instr_count}}")
else
    if vars.MODE == "run" then
        checkpoint.load(vars.CKPT)
    end
    -- The configuration under test (scenario.py's driver defines it: the
    -- executor, the translation tier's switches)
    gs.try(function() return scen_setup() end, nil)
    local menu0 = menu_sum()
    local win0 = window_sum()
    local r = 1
    while r <= 2 do
        osx_goto(128, 11)
        osx_click()
        scen_phase(F"menu-open${r}", 300000000)
        assert(menu_sum() ~= menu0, F"osx-interactive: the File menu did not open (round ${r})")
        osx_click()
        scen_phase(F"menu-close${r}", 300000000)
        assert(menu_sum() == menu0, F"osx-interactive: the File menu did not close (round ${r})")
        osx_goto(104, 58)
        osx_click()
        scen_phase(F"minimize${r}", 1500000000)
        assert(window_sum() ~= win0, F"osx-interactive: the window did not leave for the Dock (round ${r})")
        osx_goto(719, 728)
        osx_click()
        scen_phase(F"restore${r}", 1500000000)
            assert(window_sum() == win0, F"osx-interactive: the window did not come back from the Dock (round ${r})")
        r = r + 1
    end
end
quit()
