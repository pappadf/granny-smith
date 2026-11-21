-- Converted from test.script by scripts/script-to-lua.py
-- MacTest Pro (PPC) CD v11.0 (12/15/98) on a Power Macintosh 9500: every
-- test the Power Macintosh configuration offers must pass.
--
-- The CD goes into the 9500's own drive (machine.scsi2 ID 3): the ROM loads
-- the disc's own CD driver and boots its Mac OS 8.1 System Folder to the
-- Finder.  The Finder opens the disc, "Run MacTest Pro (PPC)", and the
-- "Power Macintosh" configuration, which launches MacTest Pro for Power
-- Macintosh 1.0.9; its main window shows Gestalt machine 67, the 9500
-- (main-window.png).  The 9500 has no built-in video: its display is the
-- ATI mach64 card in slot A1, which MacTest Pro tests too, and its main
-- window sits 27 pixels higher than on the 8500.  Two more PCI cards ride
-- along -- a Rage 128 in slot A2 and a Voodoo2 in A3 -- which this 1998
-- MacTest Pro does not test, but which must not upset the run.
--
-- All seven tests start selected: Expansion Card Tests (the ATI mach64 in
-- slot A1), Floppy Drive 1 Tests, Comprehensive Hardware Info, Logic Board
-- Components (Power Macintosh Tests Vol. 3), RAM Tests, Comprehensive
-- Software Info, and the MacTest Pro (PPC) system software test.  The
-- floppy test asks for "a known good disk" (insert-disk.png): a blank
-- 1.44 MB disk goes into the internal drive, and the test formats, writes,
-- verifies and seeks it.  The run must end with "All selected tests
-- passed." (passed.png).
--
-- What it leans on, by sub-test (each was a failure once):
--   Floppy / seek -- the SWIM3's DBDMA byte ring is emptied when the driver
--     stops the channel, or a write lays down the previous one's data.
--   Logic Board Components / Sound Codec Control Register -- a DBDMA
--     STORE_QUAD is one 32-bit bus access, and every command but STOP
--     writes its result word (the test polls xferStatus for it).
--   Logic Board Components / Sound Channel Program Play/Record -- the AWACS
--     input runs on DBDMA channel 9.
--   Logic Board Components / Color Look Up Register -- RaDACal misc
--     registers hold what is written ($22 included).
--   Logic Board Components / Keyboard Controller, Real Time Clock -- Cuda
--     RdTime round trips at the real part's pace (as on the 7100).
--
-- Window and icon positions come from the read-only disc's Finder data and
-- MacTest Pro's defaults, so they are the same every run; the clock is
-- pinned.

local mac = dofile("../lib/mac.lua")

local cd = "../../data/apps/mactest_pro_ppc_dec_1998.iso"

if mac.have_media(cd) then
    machine.boot{model = "pm9500", ram = 32768, slots = "1=mach64_gx;2=rage128;3=voodoo2", rom = vars.ROM}
    machine.rtc.time = "2026-10-01T12:00:00"
    assert(machine.scsi2.device[3].type == "cdrom", "no CD-ROM drive at machine.scsi2 ID 3")
    machine.attach_cdrom(cd)

    -- The disc's Finder: ~2 G instructions.
    mac.wait_desktop(5000000000)
    mac.check("finder.png")

    mac.double_click_fixed(598, 42) -- the "MacTest Pro (PPC)" disc
    mac.wait_match("cd-window.png", 1000000000, 50000000)
    mac.double_click_fixed(72, 196) -- "Run MacTest Pro (PPC)"
    mac.wait_match("run-folder.png", 1000000000, 50000000)
    mac.double_click_fixed(209, 112) -- the "Power Macintosh" configuration

    -- Loading MacTest Pro holds still for long stretches ("Creating Log
    -- Window", "Validating Test Modules and Help"), so a stability wait alone
    -- stops early: ~6 G instructions to the main window.
    mac.reach("main-window.png", 6000000000, 3000000000, 100000000)
    mac.check("main-window.png")

    mac.click_fixed(107, 101) -- Test (all seven already selected)

    -- The floppy test's disk request, then a blank HD disk for it.
    mac.reach("insert-disk.png", 1500000000, 1000000000, 50000000)
    mac.check("insert-disk.png")
    -- The prompt is up before the floppy test is listening for the disk:
    -- with the Expansion Card tests ahead of it, a disk inserted at once
    -- reached the Finder ("This disk is unreadable") instead.
    scheduler.run(6000000000)
    local blank = F"${vars.WORK_DIR}/blank-hd.dsk"
    assert(files.fd_create(blank, true), "could not create a blank 1.44 MB image")
    machine.floppy.drive[0].insert(blank, true)

    mac.run_until_match("passed.png", 40000000000, 500000000)
    mac.check("passed.png")
else
    mac.skip_test("pm9500-mactest-pro (tests/data/apps/mactest_pro_ppc_dec_1998.iso is not fetched -- scripts/fetch-test-data.sh)")
end

quit()
