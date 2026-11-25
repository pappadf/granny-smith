-- tests/integration/lib/ppc_rows.lua — the row helpers the PowerPC Mac OS
-- suites (suite-pm7200, suite-tnt-1997, suite-wgs9150) shared by copy.
--
-- Pull in with `local ppc = dofile("../lib/ppc_rows.lua")`.  The rows still
-- run through ../lib/mac.lua (one instance per script state, so the row
-- counters these helpers feed are the suite's own).

local mac = dofile("mac.lua")

local P = {}

-- BoxFlag ($0CB3), read through the user-context window once the 68k
-- emulator is in user mode (MSR[PR]).
function P.boxflag()
    while (machine.cpu.msr & 0x4000) == 0 do scheduler.run(1000) end
    return machine.cpu.mmu.peek(0xCB3, 1)
end

-- Run row `label` (fn) when the row filter selects it and its `media` was
-- fetched; say so when it was not.
function P.dispatch(label, fn, media)
    if mac.row_on(label) then
        if mac.have_media(media) then
            mac.row(label, fn)
        else
            echo(F"skip: ${label} (${media} not fetched)")
        end
    end
end

-- Mac OS 8.1's About This Computer, matched with the menu-bar clock, the
-- "Largest Unused Block" figure and the Mac OS row's figure and bar
-- excluded: they move with boot-time heap use (the check_about rationale).
function P.check_about_81(ref)
    if mac.REGEN_MODE == "1" then
        machine.screen.save(ref)
    else
        machine.screen.match(ref, 0, 440, 20, 640, 182, 90, 197, 330, 202, 250, 219, 530)
    end
end

-- The System 7.6 "Install 1" floppy through the SWIM3: build `model` with
-- `ram` KB on `rom`, set the clock to `rtc`, insert the floppy `fd` and run
-- through boot blocks, System and extensions to the Installer, matched
-- against `golden` (the menu bar excluded).  The caller asserts identity.
function P.boot_install1(model, ram, rom, rtc, fd, golden)
    machine.boot{model = model, ram = ram, rom = rom}
    machine.rtc.time = rtc
    machine.floppy.drive[0].insert(fd)
    assert(machine.floppy.drive[0].present, "boot floppy not in FD0")
    mac.run_until_match_ex(golden, 0, 440, 20, 640, 7600000000, 250000000)
    mac.check_ex(golden, 0, 440, 20, 640)
end

return P
