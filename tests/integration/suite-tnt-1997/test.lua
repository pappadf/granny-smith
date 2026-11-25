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
local ppc = dofile("../lib/ppc_rows.lua")
local boxflag, dispatch, check_about_81 = ppc.boxflag, ppc.dispatch, ppc.check_about_81

local hd_81 = "../../data/apps/quake_8_1.img"
local hd_76 = "../../data/systems/system_7_6_170mb_24ac.img"
local fd_76 = "../../data/systems/SSW-7.6-1.4M/Install 1.img"

local rom_97 = "../../data/roms/pm7300-pm7600-pm8600-pm9600-960e4be9-949c2c56d07516b7.rom"
local rom_97v2 = "../../data/roms/pm8600-pm9600-v2-960fc647-013bb98cd4ad16c4.rom"
local rom_95v2 = "../../data/roms/pm7500-pm8500-pm9500-v2-9630c68b-4db4a42fea3b53b3.rom"

local id_97 = "960e4be9-949c2c56d07516b7"
local id_97v2 = "960fc647-013bb98cd4ad16c4"
local id_95v2 = "9630c68b-4db4a42fea3b53b3"

-- Boot `model` on `rom` from the Mac OS 8.1 disk at MESH id 0 to a stable
-- Finder, then assert identity and open About This Computer.
local function tnt97_81_row(label, model, rom, rom_id, card, want_boxflag, finder, about)
    local t0 = scheduler.host_user_ns
    machine.boot{model = model, ram = 65536, rom = rom, pci_card = card}
    machine.rtc.time = "2026-04-26T21:30:36"
    machine.scsi.attach_hd(hd_81, 0)
    mac.wait_desktop(6000000000)
    assert(machine.id == model, F"${model}: machine.id ${machine.id}")
    assert(machine.rom.id == rom_id, F"${model}: rom.id ${machine.rom.id}, want ${rom_id}")
    assert(debug.mac.globals.read("BootDrive") == 0x8023, F"${model}: BootDrive ${debug.mac.globals.read(\"BootDrive\"):04x}")
    assert(debug.mac.globals.read("DSErrCode") == 0, F"${model}: system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")
    assert(boxflag() == want_boxflag, F"${model}: BoxFlag ${boxflag()}, want ${want_boxflag}")
    mac.check_ex(finder, 0, 440, 20, 640)
    mac.about_box()
    check_about_81(about)
    mac.report_row("8.1", card == "" and "control" or card, "hd-boot mesh about")
    mac.report_perf(F"suite-tnt-1997/${label}", scheduler.instr_count)
    mac.mips_floor(label, t0, 20)
end

-- golden-collision-ok: pm7300-8.1-finder.png pm7600-8.1-finder.png pm8600-8.1-finder.png pm7300-8.1-about.png pm7600-8.1-about.png pm8600-8.1-about.png — the same Mac OS 8.1 desktop and About window on the same Control raster; the rows' BoxFlag and machine.id assertions carry the model identity, which this About window does not print.
local function row_pm7300_81_hd()
    tnt97_81_row("pm7300-81-hd", "pm7300", rom_97, id_97, "", 0x67, "goldens/pm7300-8.1-finder.png", "goldens/pm7300-8.1-about.png")
end
local function row_pm7600_81_hd()
    tnt97_81_row("pm7600-81-hd", "pm7600", rom_97, id_97, "", 0x3E, "goldens/pm7600-8.1-finder.png", "goldens/pm7600-8.1-about.png")
end
local function row_pm8600_81_hd()
    tnt97_81_row("pm8600-81-hd", "pm8600", rom_97v2, id_97v2, "", 0x3F, "goldens/pm8600-8.1-finder.png", "goldens/pm8600-8.1-about.png")
end
-- The 9600 has no onboard video: the Mach64 GX in A1 draws everything, and
-- the Control stand-in retires (pm9500.c).
local function row_pm9600_81_mach64()
    tnt97_81_row("pm9600-81-mach64", "pm9600", rom_97, id_97, "mach64_gx", 0x3D, "goldens/pm9600-8.1-finder.png", "goldens/pm9600-8.1-about.png")
    assert(gs.try(function() return machine.pci.slot[7].card.name end, "(empty)") == "(empty)", "no onboard video when a display card is seated")
end
local function row_pm9600_81_mach64_v2()
    tnt97_81_row("pm9600-81-mach64-v2", "pm9600", rom_97v2, id_97v2, "mach64_gx", 0x3D, "goldens/pm9600-8.1-finder.png", "goldens/pm9600-8.1-about.png")
end

-- The 7600 on the 1995 ROM it first shipped with, System 7.6 from disk.
local function row_pm7600_76_hd()
    machine.boot{model = "pm7600", ram = 32768, rom = rom_95v2}
    machine.rtc.time = "2026-04-26T21:30:36"
    machine.scsi.attach_hd(hd_76, 0)
    scheduler.run(4000000000)
    assert(machine.rom.id == id_95v2, F"pm7600: rom.id ${machine.rom.id}")
    assert(debug.mac.globals.read("BootDrive") == 0x8023, F"pm7600: BootDrive ${debug.mac.globals.read(\"BootDrive\"):04x}")
    assert(debug.mac.globals.read("DSErrCode") == 0, F"pm7600: system error ${debug.mac.globals.read(\"DSErrCode\"):04x}")
    assert(boxflag() == 0x3E, F"pm7600: BoxFlag ${boxflag()}, want $3E")
    -- Close the Energy Saver first-boot message, then About This Computer.
    machine.adb.keyboard.press("Return")
    mac.run_ticks(60)
    mac.check_ex("goldens/pm7600-7.6-finder.png", 0, 440, 20, 640)
    mac.about_box()
    mac.check_about_clock("goldens/pm7600-7.6-about.png")
    mac.report_row("7.6", "control", "hd-boot mesh about 1995-rom")
    mac.report_perf("suite-tnt-1997/pm7600-76-hd", scheduler.instr_count)
end

-- The 7600 booting the System 7.6 "Install 1" floppy through the SWIM3:
-- boot blocks, System, extensions, then the Installer (the floppy shape of
-- suite-pdm's pdm-floppy-boot; Disk Tools 2, which would open a Finder, is
-- not in the fixture set).
local function row_pm7600_76_fd()
    ppc.boot_install1("pm7600", 32768, rom_95v2, "2026-04-26T21:30:36", fd_76, "goldens/pm7600-7.6-floppy-boot.png")
    assert(boxflag() == 0x3E, F"pm7600: BoxFlag ${boxflag()}, want $3E")
    mac.report_row("7.6", "control", "fd-boot swim3")
    mac.report_perf("suite-tnt-1997/pm7600-76-fd", scheduler.instr_count)
end

-- ---- row dispatch -----------------------------------------------------------

dispatch("pm7300-81-hd", row_pm7300_81_hd, hd_81)
dispatch("pm7600-81-hd", row_pm7600_81_hd, hd_81)
dispatch("pm8600-81-hd", row_pm8600_81_hd, hd_81)
dispatch("pm9600-81-mach64", row_pm9600_81_mach64, hd_81)
dispatch("pm9600-81-mach64-v2", row_pm9600_81_mach64_v2, hd_81)
dispatch("pm7600-76-hd", row_pm7600_76_hd, hd_76)
dispatch("pm7600-76-fd", row_pm7600_76_fd, fd_76)

mac.suite_done()
quit()
