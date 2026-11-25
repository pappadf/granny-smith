-- TNT 1997 suite — the Power Macintosh 7300, 7600, 8600 and 9600.
--
-- These are the 7500/8500/9500 boards with a 604e card on a 50 MHz bus
-- (Apple, "Power Macintosh 7300, 7600, 8600, and 9600 Computers" Developer
-- Note, 1996).  The 1997 ROMs refuse System 7.6 ("This startup disk will
-- not work on this Macintosh model"), so the disk rows boot Mac OS 8.1;
-- the 7600 also shipped (as the 7600/120 and /132) with the 1995 ROM, and
-- its 7.6 rows run that.
--
-- Row map:
--
--   pm7300-81-hd        pm7300, 1997 v1 ROM, Mac OS 8.1 from the MESH disk:
--                       Finder, About This Computer, BoxFlag $67.
--   pm7600-81-hd        pm7600, 1997 v1 ROM, the same.
--   pm8600-81-hd        pm8600 on the later $960FC647 ROM: BoxFlag $3F.
--   pm9600-81-mach64    pm9600 (no onboard video) on an ATI Mach64 GX in
--                       A1, 1997 v1 ROM: BoxFlag $3D.
--   pm9600-81-mach64-v2 the same on the $960FC647 ROM.
--   pm7600-76-hd        pm7600 on the 1995 v2 ROM, System 7.6 from disk:
--                       Finder and About This Computer.
--   pm7600-76-fd        pm7600 on the 1995 v2 ROM booting the System 7.6
--                       "Install 1" floppy to the Installer.
--
-- Identity: the developer note keeps the 7600/8600/9600 on their
-- predecessors' machine IDs, and the ROM's 68k identification is the 1995
-- routine byte for byte — so BoxFlag ($0CB3) is the 7500's $3E (Gestalt
-- 68), the 8500's $3F (69) and the 9500's $3D (67).  The 7300's documented
-- ID 109 is not the ROM's: Mac OS 8.1's startup patch rewrites BoxFlag $3E
-- to $67 (Gestalt 109) when BoxID bit 15 reads low, the 7300's strap (see
-- pm7300.c), so its row reads $67.
-- Neither Mac OS 8.1's "About This Computer" nor 7.6's names the model
-- (both print the generic "Power Macintosh" / "Mac OS computer"), so the
-- About goldens prove the boot reached a working Finder and the Apple menu,
-- and BoxFlag carries the model identity.

local mac = dofile("../lib/mac.lua")

local hd_81 = "../../data/apps/quake_8_1.img"
local hd_76 = "../../data/systems/system_7_6_170mb_24ac.img"
local fd_76 = "../../data/systems/SSW-7.6-1.4M/Install 1.img"

local rom_97 = "../../data/roms/pm7300-pm7600-pm8600-pm9600-960e4be9-949c2c56d07516b7.rom"
local rom_97v2 = "../../data/roms/pm8600-pm9600-v2-960fc647-013bb98cd4ad16c4.rom"
local rom_95v2 = "../../data/roms/pm7500-pm8500-pm9500-v2-9630c68b-4db4a42fea3b53b3.rom"

local id_97 = "960e4be9-949c2c56d07516b7"
local id_97v2 = "960fc647-013bb98cd4ad16c4"
local id_95v2 = "9630c68b-4db4a42fea3b53b3"

-- Every row: the 1997 RTC date, a disk at MESH id 0, the startup disk and
-- no system error, the menu-bar clock left out of the Finder match.
local ROW = {suite = "suite-tnt-1997", rtc = "2026-04-26T21:30:36", boot_drive = 0x8023, dserr = true,
    finder_ex = {0, 440, 20, 640}}

-- Boot `model` on `rom` from the Mac OS 8.1 disk to a stable Finder
-- (wait_desktop: the settle these rows' perf baselines were taken with), then
-- assert identity and open About This Computer.  `card` is the PCI display
-- card ("" for the onboard Control).
local ROW_81 = mac.with(ROW, {ram = 65536, hd = hd_81, settle = {"desktop", ceiling = 6000000000},
    system = "8.1", feature = "hd-boot mesh about", about_layout = "Mac OS 8.1", mips = 20})

local function row_81(t)
    t = mac.with(ROW_81, t)
    t.boot = {pci_card = t.card}
    if t.card == "" then t.card = "control" end
    mac.media_row(t.label, function() mac.about_row(t) end, t.hd)
end

-- golden-collision-ok: pm7300-8.1-finder.png pm7600-8.1-finder.png pm8600-8.1-finder.png pm7300-8.1-about.png pm7600-8.1-about.png pm8600-8.1-about.png — the same Mac OS 8.1 desktop and About window on the same Control raster; the rows' BoxFlag and machine.id assertions carry the model identity, which this About window does not print.
row_81{label = "pm7300-81-hd", model = "pm7300", rom = rom_97, rom_id = id_97, card = "", box = 0x67,
    finder = "goldens/pm7300-8.1-finder.png", about = "goldens/pm7300-8.1-about.png"}
row_81{label = "pm7600-81-hd", model = "pm7600", rom = rom_97, rom_id = id_97, card = "", box = 0x3E,
    finder = "goldens/pm7600-8.1-finder.png", about = "goldens/pm7600-8.1-about.png"}
row_81{label = "pm8600-81-hd", model = "pm8600", rom = rom_97v2, rom_id = id_97v2, card = "", box = 0x3F,
    finder = "goldens/pm8600-8.1-finder.png", about = "goldens/pm8600-8.1-about.png"}
-- The 9600 has no onboard video: the Mach64 GX in A1 draws everything, and
-- the Control stand-in retires (pm9500.c).
row_81{label = "pm9600-81-mach64", model = "pm9600", rom = rom_97, rom_id = id_97, card = "mach64_gx", box = 0x3D,
    finder = "goldens/pm9600-8.1-finder.png", about = "goldens/pm9600-8.1-about.png",
    check = function()
        assert(gs.try(function() return machine.pci.slot[7].card.name end, "(empty)") == "(empty)",
            "no onboard video when a display card is seated")
    end}
row_81{label = "pm9600-81-mach64-v2", model = "pm9600", rom = rom_97v2, rom_id = id_97v2, card = "mach64_gx", box = 0x3D,
    finder = "goldens/pm9600-8.1-finder.png", about = "goldens/pm9600-8.1-about.png"}

-- The 7600 on the 1995 ROM it first shipped with, System 7.6 from disk: a
-- fixed 4 G boot (its baseline), then close the Energy Saver first-boot
-- message and open About This Computer.
local pm7600_76_hd = mac.with(ROW, {label = "pm7600-76-hd", model = "pm7600", ram = 32768, rom = rom_95v2,
    rom_id = id_95v2, hd = hd_76, settle = {"run", head = 4000000000}, box = 0x3E, clock = true,
    system = "7.6", card = "control", feature = "hd-boot mesh about 1995-rom",
    finder = "goldens/pm7600-7.6-finder.png", about = "goldens/pm7600-7.6-about.png",
    before_finder = function()
        machine.adb.keyboard.press("Return")
        mac.run_ticks(60)
    end})
mac.media_row("pm7600-76-hd", function() mac.about_row(pm7600_76_hd) end, hd_76)

-- The 7600 booting the System 7.6 "Install 1" floppy through the SWIM3:
-- boot blocks, System, extensions, then the Installer (the floppy shape of
-- suite-pdm's pdm-floppy-boot; Disk Tools 2, which would open a Finder, is
-- not in the fixture set).
mac.media_row("pm7600-76-fd", function()
    mac.boot_install1("pm7600", 32768, rom_95v2, "2026-04-26T21:30:36", fd_76, "goldens/pm7600-7.6-floppy-boot.png")
    assert(mac.boxflag() == 0x3E, F"pm7600: BoxFlag ${mac.boxflag()}, want $3E")
    mac.report_row("7.6", "control", "fd-boot swim3")
    mac.report_perf("suite-tnt-1997/pm7600-76-fd", scheduler.instr_count)
end, fd_76)

mac.suite_done()
quit()
