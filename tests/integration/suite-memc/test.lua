-- MEMC family suite: Centris 610/650, Quadra 610/650/800 ("Wombat" and
-- "WLCD").  Every row boots one model to the Finder and opens About This
-- Macintosh, which names the model: the identity the ROM derives from the
-- CPU-ID register ($A55A2BAD, shared) and the VIA1 straps (per model),
-- carried through BoxFlag and Gestalt to the window.
--
-- Row map:
--
--   q800-75-hd    Quadra 800, Quadra ROM, 7.5 from SCSI (16 MB)
--   q800-76-fd    Quadra 800, 7.6 Disk Tools from the SWIM II floppy
--   c650-71-hd    Centris 650, the original Centris ROM, 7.1 with System
--                 Enabler 040 from SCSI -- the system these machines shipped
--                 with (8 MB, the soldered RAM alone)
--   c610-71-hd    Centris 610 (68LC040, no FPU), Centris ROM, 7.1 + Enabler
--                 040 from SCSI
--   q800-71-hd    Quadra 800 on the original Centris ROM it launched with,
--                 7.1 + Enabler 040
--   q650-71-hd    Quadra 650, Quadra ROM, 7.1 + Enabler 040
--   q610-71-hd    Quadra 610, Quadra ROM, 7.1 + Enabler 040
--   c610-76-fd    Centris 610, Quadra ROM, 7.6 Disk Tools floppy
--   q610-76-hd    Quadra 610, 7.6 from SCSI (12 MB)
--   q650-753-hd   Quadra 650, 7.5.3 from SCSI (24 MB)
--   q800-checkpoint  family checkpoint round trip: boot -> save -> fresh
--                 boot -> load -> re-match, then the Apple menu answers
--
-- The 7.1 rows name the model in About This Macintosh (the enabler's name
-- table); the 7.5-7.6 media here say just "Macintosh" there, as on the
-- Quadra 700 (suite-quadra), so those rows carry the identity in BoxFlag.
-- The 7.1 image is the stock 7.1 disk with System Enabler 040 copied in
-- from the A/UX 3.0.1 installation boot disk (built in the emulator).
--
-- BoxFlags (the ROM's): Centris 650 24, Quadra 800 29, Quadra 650 30,
-- Centris 610 46, Quadra 610 47.  Both ROMs identify all five models.

local mac = dofile("../lib/mac.lua")

local rom_centris = "../../data/roms/c610-c650-f1a6f343.rom"
local rom_quadra = "../../data/roms/c610-c650-q610-q650-q800-f1acad13.rom"
local hd_71 = "../../data/systems/system_7_1_20mb_24ac_cd_32bit_enabler040.img"
local hd_75 = "../../data/systems/system_7_5_0_77mb_mode32_24ac.img"
local hd_753 = "../../data/systems/system_7_5_3_25mb_bare.img"
local hd_76 = "../../data/systems/system_7_6_170mb_24ac.img"
local fd_76 = "../../data/systems/SSW-7.6-1.4M/Disk Tools 1.img"

local rom_ids = {[rom_centris] = "f1a6f343", [rom_quadra] = "f1acad13"}

-- Every row: the clock pinned (the 7.5+ menu-bar clock), the Finder as the
-- current application and then a stability wait (the settle these rows'
-- perf baselines were taken with), a 640x480 desktop.  `clock`: the System
-- shows a menu-bar clock, so the goldens leave its corner out.
local ROW = {suite = "suite-memc", card = "memc", rtc = 3000000000, w = 640, h = 480, mips = 3,
    settle = {"desktop", ceiling = 2000000000, stable = 600000000}}

local function fill(t)
    t = mac.with(ROW, t)
    t.rom_id = rom_ids[t.rom]
    if t.clock then t.finder_ex = {0, 440, 20, 640} end
    return t
end

-- One boot-to-About row, run when its disk and the further media `...` (a
-- ROM) were fetched.
local function row(t, ...)
    t = fill(t)
    mac.media_row(t.label, function() mac.about_row(t) end, t.hd or t.fd, ...)
end

-- golden-collision-ok: c610-7.1-hd-finder.png c650-7.1-hd-finder.png q610-7.1-hd-finder.png q650-7.1-hd-finder.png q800-7.1-hd-finder.png — the same System 7.1 desktop on five boards; machine.id, BoxFlag (24/29/30/46/47) and each row's About golden, which names the model, tell them apart
-- golden-collision-ok: c610-7.6-fd-finder.png q800-7.6-fd-finder.png — the same Disk Tools desktop; machine.id and BoxFlag (46 vs 29) tell the rows apart
local q800_75_hd = {label = "q800-75-hd", model = "q800", rom = rom_quadra, ram = 16384, hd = hd_75, box = 29,
    system = "7.5", clock = true, finder = "goldens/q800-7.5-hd-finder.png", about = "goldens/q800-7.5-hd-about.png"}
row(q800_75_hd)
row{label = "q800-76-fd", model = "q800", rom = rom_quadra, ram = 16384, fd = fd_76, box = 29, system = "7.6", clock = true,
    finder = "goldens/q800-7.6-fd-finder.png", about = "goldens/q800-7.6-fd-about.png"}
row({label = "c650-71-hd", model = "c650", rom = rom_centris, ram = 8192, hd = hd_71, box = 24, system = "7.1",
    finder = "goldens/c650-7.1-hd-finder.png", about = "goldens/c650-7.1-hd-about.png"}, rom_centris)
row({label = "c610-71-hd", model = "c610", rom = rom_centris, ram = 8192, hd = hd_71, box = 46, system = "7.1",
    finder = "goldens/c610-7.1-hd-finder.png", about = "goldens/c610-7.1-hd-about.png"}, rom_centris)
row({label = "q800-71-hd", model = "q800", rom = rom_centris, ram = 8192, hd = hd_71, box = 29, system = "7.1",
    finder = "goldens/q800-7.1-hd-finder.png", about = "goldens/q800-7.1-hd-about.png"}, rom_centris)
row{label = "q650-71-hd", model = "q650", rom = rom_quadra, ram = 8192, hd = hd_71, box = 30, system = "7.1",
    finder = "goldens/q650-7.1-hd-finder.png", about = "goldens/q650-7.1-hd-about.png"}
row{label = "q610-71-hd", model = "q610", rom = rom_quadra, ram = 8192, hd = hd_71, box = 47, system = "7.1",
    finder = "goldens/q610-7.1-hd-finder.png", about = "goldens/q610-7.1-hd-about.png"}
row{label = "c610-76-fd", model = "c610", rom = rom_quadra, ram = 8192, fd = fd_76, box = 46, system = "7.6", clock = true,
    finder = "goldens/c610-7.6-fd-finder.png", about = "goldens/c610-7.6-fd-about.png"}
row{label = "q610-76-hd", model = "q610", rom = rom_quadra, ram = 12288, hd = hd_76, box = 47, system = "7.6", clock = true,
    finder = "goldens/q610-7.6-hd-finder.png", about = "goldens/q610-7.6-hd-about.png"}
row{label = "q650-753-hd", model = "q650", rom = rom_quadra, ram = 24576, hd = hd_753, box = 30, system = "7.5.3",
    clock = true, finder = "goldens/q650-7.5.3-hd-finder.png", about = "goldens/q650-7.5.3-hd-about.png"}

-- ---- q800-checkpoint ---------------------------------------------------------
-- The family's own checkpoint part (MEMC bank registers, IOSB, Kiwi, the
-- overlay) round-trips: save the booted Finder, wipe it with a fresh boot,
-- load it back and re-match the same golden.
mac.media_row("q800-checkpoint", function()
    mac.boot_finder(fill(mac.with(q800_75_hd, {label = "q800-checkpoint"})))
    mac.check_ex("goldens/q800-7.5-hd-finder.png", 0, 440, 20, 640)
    assert(checkpoint.save(F"${vars.WORK_DIR}/suite-memc.gsc"), "q800-checkpoint: save failed")
    machine.boot{model = "q800", ram = 16384, rom = rom_quadra}
    assert(checkpoint.load(F"${vars.WORK_DIR}/suite-memc.gsc"), "q800-checkpoint: load failed")
    scheduler.run(2000000)
    assert(mac.boxflag() == 29, "q800-checkpoint: BoxFlag lost across the restore")
    mac.check_ex("goldens/q800-7.5-hd-finder.png", 0, 440, 20, 640)
    -- Still running: the clock ticks and the Finder answers the Apple menu.
    local t0 = debug.mac.globals.read("Ticks")
    mac.run_ticks(30)
    assert(debug.mac.globals.read("Ticks") > t0, "q800-checkpoint: Ticks stalled after the restore")
    mac.about_box()
    mac.check_about_clock("goldens/q800-7.5-hd-about.png")
    mac.report_perf("suite-memc/q800-checkpoint", scheduler.instr_count)
end, hd_75)

mac.suite_done()
quit()
