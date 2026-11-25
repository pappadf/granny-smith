-- Compact Macintosh suite (see config.mk).
--
-- Row map:
--
--   m128k-20-fd     Macintosh 128K, System 2.0 (Finder 4.1) off the 400K disk
--   m128k-32-fd     Macintosh 128K, System 3.2 (Finder 5.3) off the 400K disk
--   m512k-20-fd     Macintosh 512K, System 2.0.1 off the 400K disk
--   m512k-40-fd     Macintosh 512K, System 4.0 off the 400K disk
--   m512ke-32-fd    Macintosh 512K enhanced, System 3.2 off an 800K disk
--   m512ke-42-fd    Macintosh 512K enhanced, System 4.2 off an 800K disk
--   se-608-fd       Macintosh SE, System 6.0.8 off the 800K disk
--   se-71-hd        Macintosh SE, System 7.1 off a SCSI hard disk
--   se-753-hd       Macintosh SE, System 7.5.3 off a SCSI hard disk
--   sefdhd-71-fd    Macintosh SE FDHD, System 7.1 off the 1.4 MB Disk Tools
--   sefdhd-608-fd   Macintosh SE FDHD, System 6.0.8 off the 800K disk
--   sefdhd-71-hd    Macintosh SE FDHD, System 7.1 off a SCSI hard disk
--   classic-608-fd  Macintosh Classic, System 6.0.8 off the 800K disk
--   classic-romdisk Macintosh Classic, the System 6.0.3 ROM disk (Cmd-Opt-X-O)
--   classic-71-fd   Macintosh Classic, System 7.1 off the 1.4 MB Disk Tools
--   classic-753-hd  Macintosh Classic, System 7.5.3 off a SCSI hard disk
--
-- Every row asserts the machine and ROM ids, reaches the Finder, opens the
-- first item of the Apple menu ("About the Finder" before System 7, "About
-- This Macintosh" under it, which names the model) and matches it.

local mac = dofile("../lib/mac.lua")

local roms = "../../data/roms/"
local sys = "../../data/systems/"

local rom_128k = roms .. "mac128k-28ba61ce.rom"
local rom_512k = roms .. "mac512k-28ba4e50.rom"
local rom_plus = roms .. "plus-v3-4d1f8172.rom"
local rom_se = roms .. "se-b2e362a8.rom"
local rom_sefdhd = roms .. "se-fdhd-b306e171.rom"
local rom_classic = roms .. "classic-a49f9914.rom"

local ssw_20 = sys .. "SSW-2.0-400K/System Disk.image"
local sys_201 = sys .. "System_2_0_1.dsk"
local ssw_32 = sys .. "SSW-3.2-400K/System Installation v1.0.image"
local sys_32 = sys .. "System_3_2_0.dsk"
local sys_40 = sys .. "System_4_0_0.dsk"
local ssw_42 = sys .. "SSW-4.2-800K/System Tools 1.image"
local ssw_608 = sys .. "SSW-6.08-800K/SSW 6.0.8-800k Disk1of4"
local disk_tools_71 = sys .. "SSW-7.1-1.4M/Disk Tools.img"
-- The universal 7.1 image (no 8*24 GC INIT, which is 68020 code)
local hd_71 = sys .. "system_7_1_20mb_24ac_cd_32bit.img"
local hd_753 = sys .. "system_7_5_3_25mb_bare.img"

-- System 7.5's menu-bar clock, excluded from the matches (top, left,
-- bottom, right) on a 512-wide screen
local CLOCK = {0, 360, 20, 450}

-- check() or, with the clock excluded, check_ex()
local function check(ref, clock)
    if clock then mac.check_ex(ref, CLOCK[1], CLOCK[2], CLOCK[3], CLOCK[4]) else mac.check(ref) end
end

-- Wait for the boot to come to rest.  A menu-bar clock never holds still,
-- so with `t.clock` the wait is for the Finder to be the current application
-- (CurApName, $910) and then a fixed stretch for its desktop to draw.
local function settle(t)
    local ceiling = t.ceiling or 600000000
    if not t.clock then
        mac.wait_stable(ceiling)
        return
    end
    mac.wait_until(function()
        return machine.memory.peek.l(0x910) == 0x0646696E and machine.memory.peek.w(0x914) == 0x6465
    end, 20000000, ceiling, "the Finder never became the current application")
    scheduler.run(150000000)
end

-- Boot `t.model` with `t.rom` and `t.ram` KB off its media, reach the Finder
-- (`t.name`'s golden), open the About box and match it.  `t.adb` picks the
-- ADB choreography over the quadrature one; `t.keys` are ADB raw key codes
-- held from power-on until `t.keys_for` instructions have run; `t.dismiss`
-- is a startup dialog (its golden suffix and the button's {x, y}) clicked
-- away before the Finder; `t.clock` drops the menu-bar clock from the
-- matches.
local function about_row(label, t)
    machine.boot{model = t.model, ram = t.ram, rom = t.rom}
    assert(machine.id == t.model, F"${label}: machine.id ${machine.id} != ${t.model}")
    assert(machine.rom.id == t.rom_id, F"${label}: rom.id ${machine.rom.id} != ${t.rom_id}")
    assert(machine.ram == t.ram, F"${label}: machine.ram != ${t.ram}")
    machine.rtc.time = 3000000000
    if t.fd then machine.floppy.drive[0].insert(t.fd) end
    if t.hd then machine.scsi.attach_hd(t.hd, 0) end
    if t.keys then
        for _, k in ipairs(t.keys) do machine.adb.keyboard.down(k) end
        scheduler.run(t.keys_for)
        for _, k in ipairs(t.keys) do machine.adb.keyboard.up(k) end
    end
    scheduler.run(t.head or 30000000)
    settle(t)
    if t.dismiss then
        check(F"goldens/${label}-${t.dismiss[1]}.png", t.clock)
        -- The alert comes up while the extensions load, before the cursor
        -- task and Ticks are reliably running: fixed budgets, not tick waits
        machine.adb.mouse.move(t.dismiss[2], t.dismiss[3], "global")
        scheduler.run(5000000)
        machine.adb.mouse.click(true)
        scheduler.run(2000000)
        machine.adb.mouse.click(false)
        scheduler.run(10000000)
        settle(t)
    end
    assert(debug.mac.os, F"${label}: the Finder is up but debug.mac.os is false")
    assert(machine.screen.width == 512 and machine.screen.height == 342, F"${label}: screen not 512x342")
    check(F"goldens/${label}-finder.png", t.clock)
    if t.adb then mac.about_box() else mac.about_box_quad() end
    check(F"goldens/${label}-about.png", t.clock)
    mac.report_row(t.system, "builtin", t.feature .. " about")
    mac.report_perf("suite-compact/" .. label, scheduler.instr_count)
end

-- ---- Macintosh 128K ---------------------------------------------------------

mac.row("m128k-20-fd", function()
    about_row("m128k-20-fd", {model = "mac128k", rom = rom_128k, rom_id = "28ba61ce", ram = 128, fd = ssw_20,
        system = "2.0", feature = "fd-boot iwm 400k-gcr pwm-speed"})
end)

mac.row("m128k-32-fd", function()
    about_row("m128k-32-fd", {model = "mac128k", rom = rom_128k, rom_id = "28ba61ce", ram = 128, fd = ssw_32,
        system = "3.2", feature = "fd-boot iwm 400k-gcr pwm-speed"})
end)

-- ---- Macintosh 512K ---------------------------------------------------------

mac.row("m512k-20-fd", function()
    about_row("m512k-20-fd", {model = "mac512k", rom = rom_512k, rom_id = "28ba4e50", ram = 512, fd = sys_201,
        system = "2.0.1", feature = "fd-boot iwm 400k-gcr pwm-speed"})
end)

mac.row("m512k-40-fd", function()
    about_row("m512k-40-fd", {model = "mac512k", rom = rom_512k, rom_id = "28ba4e50", ram = 512, fd = sys_40,
        system = "4.0", feature = "fd-boot iwm 400k-gcr pwm-speed"})
end)

-- ---- Macintosh 512K enhanced ------------------------------------------------

mac.row("m512ke-32-fd", function()
    about_row("m512ke-32-fd", {model = "mac512ke", rom = rom_plus, rom_id = "4d1f8172", ram = 512, fd = sys_32,
        system = "3.2", feature = "fd-boot iwm 800k-gcr no-scsi"})
end)

mac.row("m512ke-42-fd", function()
    about_row("m512ke-42-fd", {model = "mac512ke", rom = rom_plus, rom_id = "4d1f8172", ram = 512, fd = ssw_42,
        system = "4.2", feature = "fd-boot iwm 800k-gcr no-scsi"})
end)

-- ---- Macintosh SE -----------------------------------------------------------

mac.row("se-608-fd", function()
    about_row("se-608-fd", {model = "se", rom = rom_se, rom_id = "b2e362a8", ram = 2048, fd = ssw_608, adb = true,
        system = "6.0.8", feature = "fd-boot iwm 800k-gcr adb"})
end)

-- System 7.1 names the model in About This Macintosh ("Macintosh SE");
-- 7.5.3 below names none of the compacts, the Plus included ("Macintosh").
mac.row("se-71-hd", function()
    about_row("se-71-hd", {model = "se", rom = rom_se, rom_id = "b2e362a8", ram = 4096, hd = hd_71, adb = true,
        dismiss = {"fsaccess", 357, 245}, ceiling = 1200000000, system = "7.1",
        feature = "hd-boot scsi adb fs-access-dialog"})
end)

mac.row("se-753-hd", function()
    about_row("se-753-hd", {model = "se", rom = rom_se, rom_id = "b2e362a8", ram = 4096, hd = hd_753, adb = true,
        clock = true, ceiling = 1200000000, system = "7.5.3", feature = "hd-boot scsi adb"})
end)

-- ---- Macintosh SE FDHD ------------------------------------------------------

mac.row("sefdhd-71-fd", function()
    about_row("sefdhd-71-fd", {model = "sefdhd", rom = rom_sefdhd, rom_id = "b306e171", ram = 4096,
        fd = disk_tools_71, adb = true, ceiling = 1200000000, system = "7.1", feature = "fd-boot swim 1.44m-mfm adb"})
end)

mac.row("sefdhd-608-fd", function()
    about_row("sefdhd-608-fd", {model = "sefdhd", rom = rom_sefdhd, rom_id = "b306e171", ram = 1024, fd = ssw_608,
        adb = true, system = "6.0.8", feature = "fd-boot swim 800k-gcr adb"})
end)

-- The universal 7.1 image on a 68000: Apple Photo Access (68020 code) fails
-- to install and the System says so before the Finder, as on the Plus.
mac.row("sefdhd-71-hd", function()
    about_row("sefdhd-71-hd", {model = "sefdhd", rom = rom_sefdhd, rom_id = "b306e171", ram = 2560, hd = hd_71,
        adb = true, dismiss = {"fsaccess", 357, 245}, ceiling = 1200000000, system = "7.1",
        feature = "hd-boot scsi adb fs-access-dialog"})
end)

-- ---- Macintosh Classic ------------------------------------------------------

mac.row("classic-608-fd", function()
    about_row("classic-608-fd", {model = "classic", rom = rom_classic, rom_id = "a49f9914", ram = 2560,
        fd = ssw_608, adb = true, system = "6.0.8", feature = "fd-boot swim 800k-gcr adb"})
end)

-- Command-Option-X-O held at startup boots the System 6.0.3 disk in the
-- upper half of the Classic ROM.  Raw ADB codes: Command $37, Option $3A,
-- X $07, O $1F.
mac.row("classic-romdisk", function()
    about_row("classic-romdisk", {model = "classic", rom = rom_classic, rom_id = "a49f9914", ram = 1024,
        keys = {0x37, 0x3A, 0x07, 0x1F}, keys_for = 6000000, adb = true, system = "6.0.3",
        feature = "rom-disk adb"})
end)

mac.row("classic-71-fd", function()
    about_row("classic-71-fd", {model = "classic", rom = rom_classic, rom_id = "a49f9914", ram = 4096,
        fd = disk_tools_71, adb = true, ceiling = 1200000000, system = "7.1", feature = "fd-boot swim 1.44m-mfm adb"})
end)

mac.row("classic-753-hd", function()
    about_row("classic-753-hd", {model = "classic", rom = rom_classic, rom_id = "a49f9914", ram = 4096, hd = hd_753,
        adb = true, clock = true, ceiling = 1200000000, system = "7.5.3", feature = "hd-boot scsi adb"})
end)

mac.suite_done()
quit()
