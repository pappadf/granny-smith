-- Converted from test.script by scripts/script-to-lua.py
-- MacTest Pro (680x0) CD (2/96) on a Macintosh Quadra 700: every test the
-- configuration offers must pass.
--
-- The CD alone, at SCSI id 3: the ROM boots the disc's System 7.5 System
-- Folder to the Finder, whose "MacTest Pro" window opens with the disc
-- (finder.png).  The Finder opens "Run MacTest Pro (sys 7)" (run-folder.png)
-- and its "Quadra/Centris" configuration, which launches MacTest Pro.  Its
-- main window must name the machine "Macintosh Quadra 700", ID 22, with
-- nine tests available and selected (main-window.png).
--
-- The nine: Expansion Card Tests for the display cards (8*24 in slot $D and
-- 24AC in slot $E), then File Checker (MacTest Pro), Floppy Drive 1,
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
--   One floppy drive -- the second drive select answers "no drive" (/DRVIN
--     high); a phantom "Floppy Drive 2" test used to be offered and fail.
--   Logic Board Components / NuBus Controller -- YANCC's write-buffer error
--     interrupt: error bit 7 with enable bit 10 requests level 7, taken once
--     per rise; the test counts 25.
--   Logic Board Components / Ethernet ROM -- the station-address PROM in
--     Apple's 08:00:07 OUI, repeating so that both 8-byte halves XOR to $FF.
--
-- Window and icon positions come from the read-only disc's Finder data and
-- MacTest Pro's defaults, so they are the same every run; the clock is
-- pinned.

local mac = dofile("../lib/mac.lua")

local cd = "../../data/apps/mactest_pro_68k_feb_1996.iso"

if mac.have_media(cd) then
    machine.boot{model = "q700", slots = "13=mdc_8_24;14=display_card_24ac", rom = vars.ROM}
    machine.rtc.time = "2026-10-01T12:00:00"
    assert(machine.scsi.attach_cdrom(cd, 3), "attach the CD at SCSI id 3")

    mac.wait_desktop(3000000000)
    mac.wait_stable(1000000000)
    mac.check("finder.png")

    mac.double_click_fixed(67, 190) -- "Run MacTest Pro (sys 7)"
    mac.wait_stable(1000000000)
    mac.check("run-folder.png")
    mac.double_click_fixed(140, 157) -- the "Quadra/Centris" configuration

    -- Loading MacTest Pro holds still for long stretches, so a stability
    -- wait alone can stop early: ~1 G instructions to the main window.
    mac.reach("main-window.png", 3000000000, 2000000000, 100000000)
    mac.check("main-window.png")

    mac.click_fixed(107, 101) -- Test (all nine are selected by default)

    -- The floppy test's disk request, then a blank HD disk for it.
    mac.reach("insert-disk.png", 1500000000, 1000000000, 50000000)
    mac.check("insert-disk.png")
    local blank = F"${vars.WORK_DIR}/blank-hd.dsk"
    assert(files.fd_create(blank, true), "could not create a blank 1.44 MB image")
    machine.floppy.drive[0].insert(blank, true)

    mac.run_until_match("passed.png", 20000000000, 500000000)
    mac.check("passed.png")
else
    mac.skip_test("q700-mactest-pro (tests/data/apps/mactest_pro_68k_feb_1996.iso is not fetched -- scripts/fetch-test-data.sh)")
end

quit()
