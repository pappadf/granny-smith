-- Converted from test.script by scripts/script-to-lua.py
-- MacTest Pro (680x0) CD (2/96) on a Macintosh IIci: every test the
-- configuration offers must pass.
--
-- The CD alone, at SCSI id 3: the ROM boots the disc's System 7.5 System
-- Folder to the Finder, whose "MacTest Pro" window opens with the disc
-- (finder.png).  The Finder opens "Run MacTest Pro (sys 7)" (run-folder.png)
-- and its "Mac II Family" configuration, which launches MacTest Pro.  Its
-- main window must name the machine "Macintosh IIci", ID 11, with nine
-- tests available and selected (main-window.png).
--
-- The nine: Expansion Card Tests for the display cards (24AC in slot $C,
-- 8*24 GC in slot $D), then File Checker (MacTest Pro), Floppy Drive 1,
-- Comprehensive Hardware Info, Logic Board Components, RAM Tests, Video RAM
-- Tests and Comprehensive Software Info.  The floppy test runs early and
-- asks for "a known good disk (all data on the disk will be erased)"
-- (insert-disk.png): a blank 1.44 MB disk goes into the internal drive.  The
-- run must end with "All selected tests passed." (passed.png).
--
-- What it leans on (each was a failure once):
--   Expansion Card / 24AC -- the card's monitor-sense register answers a
--     longword read at its odd address $D8000D byte by byte, as the bus
--     splits it; read whole, the lines said "no monitor" and the card was
--     not offered.
--   Expansion Card / 8*24GC -- writing a sense register drives its line,
--     as MacTest's extended-sense probe expects; with only the $2C/$30/$34
--     drives honoured it read "no monitor" and skipped the card.
--   One floppy drive -- the second drive select answers "no drive" (/DRVIN
--     high); a phantom "Floppy Drive 2" test used to be offered and fail.
--   No cache card -- the cache connector's $52000000-$52FFFFFF window
--     bus-errors when empty; mirroring the I/O there made MacTest find a
--     cache card and fail its test.
--   Logic Board Components / SCSI Controller -- the 5380 register walk
--     toggles SEL/BSY and MR.ARBITRATE without selecting: the bus must come
--     back free, or the CD the app runs from is lost.
--
-- Window and icon positions come from the read-only disc's Finder data and
-- MacTest Pro's defaults, so they are the same every run; the clock is
-- pinned.

local mac = dofile("../lib/mac.lua")

local cd = "../../data/apps/mactest_pro_68k_feb_1996.iso"

if mac.have_media(cd) then
    machine.boot{model = "iici", ram = 8192, slots = "12=display_card_24ac;13=824gc", rom = vars.ROM}
    machine.rtc.time = "2026-10-01T12:00:00"
    assert(machine.scsi.attach_cdrom(cd, 3), "attach the CD at SCSI id 3")

    mac.wait_desktop(3000000000)
    mac.wait_stable(1000000000)
    mac.check("finder.png")

    mac.double_click_fixed(67, 190) -- "Run MacTest Pro (sys 7)"
    mac.wait_stable(1000000000)
    mac.check("run-folder.png")
    mac.double_click_fixed(140, 105) -- the "Mac II Family" configuration

    -- Loading MacTest Pro holds still for long stretches, so a stability
    -- wait alone can stop early: ~1 G instructions to the main window.
    mac.reach("main-window.png", 3000000000, 2000000000, 100000000)
    mac.check("main-window.png")

    mac.click_fixed(107, 103) -- Test (all nine are selected by default)

    -- The floppy test's disk request, then a blank HD disk for it.
    mac.reach("insert-disk.png", 1500000000, 1000000000, 50000000)
    mac.check("insert-disk.png")
    local blank = F"${vars.WORK_DIR}/blank-hd.dsk"
    assert(files.fd_create(blank, true), "could not create a blank 1.44 MB image")
    machine.floppy.drive[0].insert(blank, true)

    mac.run_until_match("passed.png", 20000000000, 500000000)
    mac.check("passed.png")
else
    mac.skip_test("iici-mactest-pro (tests/data/apps/mactest_pro_68k_feb_1996.iso is not fetched -- scripts/fetch-test-data.sh)")
end

quit()
