-- Converted from test.script by scripts/script-to-lua.py
-- The printer's lifetime, read through the classic LaserWriter driver's
-- PatchPrep upload.  See config.mk for the legs.
--
-- Each print is the appletalk-print row's: File -> Print Directory... of the
-- Macintosh HD window, from System 6.0.8 on a Plus.  A new machine starts
-- with fresh PRAM, where System 6 keeps AppleTalk's active flag, so the
-- machine.boot leg goes through the Chooser again, as a user would.

-- --- the steps ----------------------------------------------------------------

local mac = dofile("../lib/mac.lua")

local function click(x, y, settle)
    machine.adb.mouse.move(x, y)
    scheduler.run(2000000)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    wait_mouse_taken(100000000)
    scheduler.run(settle)
end

-- Apple menu -> Chooser -> LaserWriter -> "requires AppleTalk" OK -> the
-- printer -> close.
local function choose_printer()
    machine.adb.mouse.move(25, 10)
    scheduler.run(2000000)
    machine.adb.mouse.click(true)
    scheduler.run(4000000)
    machine.adb.mouse.move(25, 123)
    scheduler.run(4000000)
    machine.adb.mouse.click(false)
    scheduler.run(60000000)
    click(165, 135, 20000000)
    click(187, 206, 60000000)
    click(300, 88, 20000000)
    click(72, 47, 40000000)
end

-- Double-click the Macintosh HD icon to open its window.
local function open_disk()
    machine.adb.mouse.move(470, 52)
    scheduler.run(2000000)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    wait_mouse_taken(100000000)
    scheduler.run(80000)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    wait_mouse_taken(100000000)
    scheduler.run(40000000)
end

-- File -> Print Directory... -> Print, then run until the document lands
-- and the connection has closed.
local function print_directory()
    local want = appletalk.printer.documents + 1
    machine.adb.mouse.move(53, 10)
    scheduler.run(2000000)
    machine.adb.mouse.click(true)
    scheduler.run(4000000)
    machine.adb.mouse.move(100, 203)
    scheduler.run(4000000)
    machine.adb.mouse.click(false)
    scheduler.run(60000000)
    click(452, 45, 2000000)
    local n = 0
    while appletalk.printer.documents < want and n < 300 do
        scheduler.run(2000000)
        n = n + 1
    end
    scheduler.run(30000000)
    echo(F"printer: documents=${appletalk.printer.documents} interpreter_jobs=${appletalk.printer.interpreter_jobs} permanent=${appletalk.printer.interpreter_permanent_jobs} outcome='${appletalk.printer.last_outcome}'")
    assert(appletalk.printer.documents == want, "the print produced no document")
    assert(appletalk.printer.last_outcome == "ok", "the job did not finish cleanly")
    assert(appletalk.printer.status == "status: idle", "the printer is not idle after the job")
end

-- --- 1. The first print uploads PatchPrep ---------------------------------------

machine.rtc.time = "2026-04-26T21:30:36"
scheduler.run(30000000)
appletalk.printer.enabled = true

if appletalk.printer.interpreter == false then
    echo("SKIP: this headless binary has no PostScript interpreter (build with PLATEN=1)")
    quit()
end

assert(appletalk.printer.interpreter_jobs == 0, "the printer served jobs before any print")
choose_printer()
open_disk()
print_directory()
assert(appletalk.printer.interpreter_permanent_jobs == 1, "the first print did not download PatchPrep (exitserver)")
local first = appletalk.printer.interpreter_jobs
assert(first > 1, "the first print ran fewer jobs than the query and the document")

-- --- 2. The second print finds it resident ----------------------------------------

print_directory()
assert(appletalk.printer.interpreter_permanent_jobs == 1, "the second print downloaded PatchPrep again: the printer forgot it")
local second = appletalk.printer.interpreter_jobs - first
assert(second < first, "the second print did not skip the upload")

-- --- 3. machine.restart keeps the printer -------------------------------------------

local before = appletalk.printer.interpreter_jobs
machine.restart()
machine.rtc.time = "2026-04-26T22:00:00"
scheduler.run(30000000)
appletalk.printer.enabled = true
scheduler.run(60000000)
assert(appletalk.printer.interpreter_jobs == before, "machine.restart replaced the printer")
open_disk()
print_directory()
assert(appletalk.printer.interpreter_permanent_jobs == 1, "the print after machine.restart downloaded PatchPrep again")
assert(appletalk.printer.interpreter_jobs - before == second, "the print after machine.restart did not skip the upload")

-- --- 4. appletalk.printer.restart() is a new printer ------------------------------

assert(appletalk.printer.restart(), "the printer did not restart")
assert(appletalk.printer.interpreter_jobs == 0, "the restarted printer kept its job count")
assert(appletalk.printer.interpreter_permanent_jobs == 0, "the restarted printer kept its downloads")
print_directory()
assert(appletalk.printer.interpreter_permanent_jobs == 1, "the restarted printer did not need PatchPrep again")
assert(appletalk.printer.interpreter_jobs == first, "the print on the restarted printer ran a different job sequence")

-- --- A checkpoint load is a new printer ----------------------------------------
--
-- The printer is not part of the machine, so it is never in a checkpoint; a
-- load changes the machine on the cable, and the printer restarts with it --
-- no job runs on from one machine into the next.  Its name and enabled state
-- are the host's and stay.  No print needed: the counters say it.

assert(appletalk.printer.interpreter_jobs > 0, "nothing to lose before the load")
checkpoint.save(F"${vars.WORK_DIR}/persist.gscp")
checkpoint.load(F"${vars.WORK_DIR}/persist.gscp")
assert(appletalk.printer.interpreter_jobs == 0, "the load kept the printer's job count")
assert(appletalk.printer.interpreter_permanent_jobs == 0, "the load kept the printer's downloads")
assert(appletalk.printer.enabled, "the load disabled the printer")

-- --- 5. machine.boot is a new machine, and a new printer ------------------------

-- AppleTalk inactive, as at startup (config.mk): the Chooser asks again.
machine.boot{model = "plus", rom = vars.ROM, config = "{\"options\":{\"appletalk\":\"inactive\"}}"}
machine.scsi.attach_hd(vars.HD, 0)
machine.rtc.time = "2026-04-26T23:00:00"
scheduler.run(30000000)
appletalk.printer.enabled = true
scheduler.run(20000000)
assert(appletalk.printer.interpreter_jobs == 0, "machine.boot kept the printer")
choose_printer()
open_disk()
print_directory()
assert(appletalk.printer.interpreter_permanent_jobs == 1, "the new machine's print did not download PatchPrep to the new printer")
assert(appletalk.printer.interpreter_jobs == first, "the new machine's print ran a different job sequence")

echo("done")
quit()
