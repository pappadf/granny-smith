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
--   se-checkpoint   the SE's checkpoint round trip
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

-- Fill in a row's common fields.  The settle these rows' perf baselines were
-- taken with: a head start (`t.head`, 30 M by default), then a stability
-- wait up to `t.ceiling` (600 M by default) -- or, with `t.clock` (a menu-bar
-- clock never holds still), the Finder as the current application (CurApName,
-- $910) and a fixed stretch for its desktop to draw.  `t.adb` picks the ADB
-- About choreography over the quadrature one.  The About boxes are matched
-- whole (only the clock left out).
local function fill(t)
    t = mac.with({suite = "suite-compact", rtc = 3000000000, w = 512, h = 342, about_layout = "whole"}, t)
    local head, ceiling = t.head or 30000000, t.ceiling or 600000000
    if t.clock then
    t.settle = {"curapname", head = head, ceiling = ceiling, tail = 150000000}
    t.finder_ex = CLOCK
    else
    t.settle = {"stable", head = head, ceiling = ceiling}
    end
    t.quad = not t.adb
    if t.feature then t.feature = t.feature .. " about" end
    return t
end

-- Boot `t.model` with `t.rom` and `t.ram` KB off its media, reach the Finder
-- (`t.finder`), open the About box and match it (mac.about_row).  `t.keys`
-- are ADB raw key codes held from power-on until `t.keys_for` instructions
-- have run; `t.dismiss` is a startup dialog (its golden and the button's
-- {x, y}) clicked away before the Finder.  `t.finder` and `t.about` are the
-- row's goldens, every golden named literally so that scripts/check-goldens.py
-- can see it.
--
-- golden-collision-ok: se-608-fd-finder.png sefdhd-608-fd-finder.png classic-608-fd-finder.png — the same System 6.0.8 desktop off the same 800K disk; machine.id and rom.id (b2e362a8/b306e171/a49f9914) tell the rows apart, and their About goldens differ
-- golden-collision-ok: se-71-hd-finder.png sefdhd-71-hd-finder.png — the same System 7.1 desktop off the same SCSI disk; machine.id and rom.id (b2e362a8 vs b306e171) tell the rows apart, and their About goldens (4 MB vs 2.5 MB) differ
-- golden-collision-ok: se-71-hd-fsaccess.png sefdhd-71-hd-fsaccess.png — the same System 7.1 startup alert off the same SCSI disk; machine.id and rom.id (b2e362a8 vs b306e171) tell the rows apart
-- golden-collision-ok: se-753-hd-finder.png classic-753-hd-finder.png — the same System 7.5.3 desktop off the same SCSI disk; machine.id and rom.id (b2e362a8 vs a49f9914) tell the rows apart
-- golden-collision-ok: sefdhd-71-fd-finder.png classic-71-fd-finder.png — the same 7.1 Disk Tools desktop; machine.id and rom.id (b306e171 vs a49f9914) tell the rows apart
local function row(t)
    t = fill(t)
    mac.row(t.label, function() mac.about_row(t) end)
end

-- ---- Macintosh 128K ---------------------------------------------------------

row{label = "m128k-20-fd", finder = "goldens/m128k-20-fd-finder.png", about = "goldens/m128k-20-fd-about.png",
    model = "mac128k", rom = rom_128k, rom_id = "28ba61ce", ram = 128, fd = ssw_20,
    system = "2.0", feature = "fd-boot iwm 400k-gcr pwm-speed"}

row{label = "m128k-32-fd", finder = "goldens/m128k-32-fd-finder.png", about = "goldens/m128k-32-fd-about.png",
    model = "mac128k", rom = rom_128k, rom_id = "28ba61ce", ram = 128, fd = ssw_32,
    system = "3.2", feature = "fd-boot iwm 400k-gcr pwm-speed"}

-- ---- Macintosh 512K ---------------------------------------------------------

row{label = "m512k-20-fd", finder = "goldens/m512k-20-fd-finder.png", about = "goldens/m512k-20-fd-about.png",
    model = "mac512k", rom = rom_512k, rom_id = "28ba4e50", ram = 512, fd = sys_201,
    system = "2.0.1", feature = "fd-boot iwm 400k-gcr pwm-speed"}

row{label = "m512k-40-fd", finder = "goldens/m512k-40-fd-finder.png", about = "goldens/m512k-40-fd-about.png",
    model = "mac512k", rom = rom_512k, rom_id = "28ba4e50", ram = 512, fd = sys_40,
    system = "4.0", feature = "fd-boot iwm 400k-gcr pwm-speed"}

-- ---- Macintosh 512K enhanced ------------------------------------------------

row{label = "m512ke-32-fd", finder = "goldens/m512ke-32-fd-finder.png", about = "goldens/m512ke-32-fd-about.png",
    model = "mac512ke", rom = rom_plus, rom_id = "4d1f8172", ram = 512, fd = sys_32,
    system = "3.2", feature = "fd-boot iwm 800k-gcr no-scsi"}

row{label = "m512ke-42-fd", finder = "goldens/m512ke-42-fd-finder.png", about = "goldens/m512ke-42-fd-about.png",
    model = "mac512ke", rom = rom_plus, rom_id = "4d1f8172", ram = 512, fd = ssw_42,
    system = "4.2", feature = "fd-boot iwm 800k-gcr no-scsi"}

-- ---- Macintosh SE -----------------------------------------------------------

row{label = "se-608-fd", finder = "goldens/se-608-fd-finder.png", about = "goldens/se-608-fd-about.png",
    model = "se", rom = rom_se, rom_id = "b2e362a8", ram = 2048, fd = ssw_608, adb = true,
    system = "6.0.8", feature = "fd-boot iwm 800k-gcr adb"}

-- System 7.1 names the model in About This Macintosh ("Macintosh SE");
-- 7.5.3 below names none of the compacts, the Plus included ("Macintosh").
row{label = "se-71-hd", finder = "goldens/se-71-hd-finder.png", about = "goldens/se-71-hd-about.png",
    model = "se", rom = rom_se, rom_id = "b2e362a8", ram = 4096, hd = hd_71, adb = true,
    dismiss = {"goldens/se-71-hd-fsaccess.png", 357, 245}, ceiling = 1200000000, system = "7.1",
    feature = "hd-boot scsi adb fs-access-dialog"}

row{label = "se-753-hd", finder = "goldens/se-753-hd-finder.png", about = "goldens/se-753-hd-about.png",
    model = "se", rom = rom_se, rom_id = "b2e362a8", ram = 4096, hd = hd_753, adb = true,
    clock = true, ceiling = 1200000000, system = "7.5.3", feature = "hd-boot scsi adb"}

-- ---- Macintosh SE FDHD ------------------------------------------------------

row{label = "sefdhd-71-fd", finder = "goldens/sefdhd-71-fd-finder.png", about = "goldens/sefdhd-71-fd-about.png",
    model = "sefdhd", rom = rom_sefdhd, rom_id = "b306e171", ram = 4096,
    fd = disk_tools_71, adb = true, ceiling = 1200000000, system = "7.1", feature = "fd-boot swim 1.44m-mfm adb"}

row{label = "sefdhd-608-fd", finder = "goldens/sefdhd-608-fd-finder.png", about = "goldens/sefdhd-608-fd-about.png",
    model = "sefdhd", rom = rom_sefdhd, rom_id = "b306e171", ram = 1024, fd = ssw_608,
    adb = true, system = "6.0.8", feature = "fd-boot swim 800k-gcr adb"}

-- The universal 7.1 image on a 68000: Apple Photo Access (68020 code) fails
-- to install and the System says so before the Finder, as on the Plus.
row{label = "sefdhd-71-hd", finder = "goldens/sefdhd-71-hd-finder.png", about = "goldens/sefdhd-71-hd-about.png",
    model = "sefdhd", rom = rom_sefdhd, rom_id = "b306e171", ram = 2560, hd = hd_71,
    adb = true, dismiss = {"goldens/sefdhd-71-hd-fsaccess.png", 357, 245}, ceiling = 1200000000, system = "7.1",
    feature = "hd-boot scsi adb fs-access-dialog"}

-- ---- Macintosh Classic ------------------------------------------------------

row{label = "classic-608-fd", finder = "goldens/classic-608-fd-finder.png", about = "goldens/classic-608-fd-about.png",
    model = "classic", rom = rom_classic, rom_id = "a49f9914", ram = 2560,
    fd = ssw_608, adb = true, system = "6.0.8", feature = "fd-boot swim 800k-gcr adb"}

-- Command-Option-X-O held at startup boots the System 6.0.3 disk in the
-- upper half of the Classic ROM.  Raw ADB codes: Command $37, Option $3A,
-- X $07, O $1F.
row{label = "classic-romdisk", finder = "goldens/classic-romdisk-finder.png", about = "goldens/classic-romdisk-about.png",
    model = "classic", rom = rom_classic, rom_id = "a49f9914", ram = 1024,
    keys = {0x37, 0x3A, 0x07, 0x1F}, keys_for = 6000000, adb = true, system = "6.0.3",
    feature = "rom-disk adb"}

row{label = "classic-71-fd", finder = "goldens/classic-71-fd-finder.png", about = "goldens/classic-71-fd-about.png",
    model = "classic", rom = rom_classic, rom_id = "a49f9914", ram = 4096,
    fd = disk_tools_71, adb = true, ceiling = 1200000000, system = "7.1", feature = "fd-boot swim 1.44m-mfm adb"}

row{label = "classic-753-hd", finder = "goldens/classic-753-hd-finder.png", about = "goldens/classic-753-hd-about.png",
    model = "classic", rom = rom_classic, rom_id = "a49f9914", ram = 4096, hd = hd_753,
    adb = true, clock = true, ceiling = 1200000000, system = "7.5.3", feature = "hd-boot scsi adb"}

-- ---- se-checkpoint ----------------------------------------------------------
-- The BBU boards' own checkpoint part (the access-dropped overlay) and the
-- ADB/SCSI-interrupt wiring round-trip: boot the SE to the 6.0.8 Finder, save,
-- boot a DIFFERENT machine, load, and require the restored SE to be the saved
-- one down to the pixel, and still running.
mac.row("se-checkpoint", function()
    mac.boot_finder(fill{label = "se-checkpoint", model = "se", rom = rom_se, rom_id = "b2e362a8", ram = 2048, fd = ssw_608})
    mac.check("goldens/se-608-fd-finder.png")
    assert(checkpoint.save(F"${vars.WORK_DIR}/suite-compact.gsc"), "se-checkpoint: save failed")
    machine.boot{model = "mac128k", ram = 128, rom = rom_128k}
    assert(checkpoint.load(F"${vars.WORK_DIR}/suite-compact.gsc"), "se-checkpoint: load failed")
    assert(machine.id == "se", "se-checkpoint: restored machine is not the SE")
    assert(machine.ram == 2048, "se-checkpoint: restored RAM size wrong")
    scheduler.run(5000000)
    mac.check("goldens/se-608-fd-finder.png")
    mac.about_box()
    mac.check("goldens/se-608-fd-about.png")
    mac.report_perf("suite-compact/se-checkpoint", scheduler.instr_count)
end)

mac.suite_done()
quit()
