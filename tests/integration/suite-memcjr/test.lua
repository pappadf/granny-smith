-- MEMCjr suite: LC 475 ("Primus"), LC 575 ("Optimus") and Quadra 605
-- ("Aladdin"), all on the October 1993 ROM $FF7439EE.  Every row boots one
-- model to the Finder and opens About This Macintosh: the identity the ROM
-- derives from the CPU-ID register ($A55A2221 / $A55A222E / $A55A2225),
-- carried through BoxFlag and Gestalt to the window.
--
-- Row map:
--
--   q605-71-hd    Quadra 605, 7.1 with System Enabler 065 from SCSI -- the
--                 system these machines shipped with (8 MB: 4 soldered +
--                 a 4 MB SIMM)
--   lc475-71-hd   LC 475, 7.1 + Enabler 065 from SCSI (4 MB, the soldered
--                 RAM alone)
--   lc575-71-hd   LC 575 (68LC040), 7.1 + Enabler 065 from SCSI (12 MB:
--                 an 8 MB double-sided SIMM)
--   q605-76-fd    Quadra 605, 7.6 Disk Tools from the SWIM II floppy
--   lc575-76-fd   LC 575, 7.6 Disk Tools floppy (36 MB, the maximum: a
--                 32 MB double-sided SIMM)
--   lc475-75-hd   LC 475, 7.5 from SCSI (20 MB: a 16 MB SIMM)
--   q605-753-hd   Quadra 605, 7.5.3 from SCSI (6 MB: a 2 MB SIMM)
--   q605-checkpoint  family checkpoint round trip on the MEMCjr side
--                 (bank latch, Cuda): boot -> save -> fresh boot -> load ->
--                 re-match, then the Apple menu answers
--
-- The 7.1 rows name the model in About This Macintosh (the enabler's name
-- table); the 7.5-7.6 media say just "Macintosh" there, as on every 68K
-- model, so those rows carry the identity in BoxFlag.  The 7.1 image is
-- the stock 7.1 disk with System Enabler 065 v1.2 copied in from the
-- MacTest Pro CD (built in the emulator).
--
-- BoxFlags (the ROM's): LC 475 83, LC 575 86, Quadra 605 88.
--
-- The Finder desktops of rows booting the same System are alike across
-- models; each row asserts its own BoxFlag and machine id, and the 7.1 About
-- goldens, which name the model, differ:
-- golden-collision-ok: lc475-7.1-hd-finder.png lc575-7.1-hd-finder.png q605-7.1-hd-finder.png — the same 7.1 desktop; BoxFlag, machine id and the About golden tell the models apart
-- golden-collision-ok: q605-7.6-fd-finder.png lc575-7.6-fd-finder.png — the same Disk Tools desktop; BoxFlag, machine id and RAM tell the rows apart

local mac = dofile("../lib/mac.lua")

local rom = "../../data/roms/lc475-lc575-q605-ff7439ee.rom"
local hd_71 = "../../data/systems/system_7_1_20mb_24ac_cd_32bit_enabler065.img"
local hd_75 = "../../data/systems/system_7_5_0_77mb_mode32_24ac.img"
local hd_753 = "../../data/systems/system_7_5_3_25mb_bare.img"
local fd_76 = "../../data/systems/SSW-7.6-1.4M/Disk Tools 1.img"

-- Every row: the October 1993 ROM, the clock pinned (the 7.5+ menu-bar
-- clock), the Finder as the current application and then a stability wait
-- (the settle these rows' perf baselines were taken with), a 640x480
-- desktop.  `clock`: the System shows a menu-bar clock, so the goldens leave
-- its corner out.
local ROW = {suite = "suite-memcjr", card = "memcjr", rom = rom, rom_id = "ff7439ee", rtc = 3000000000,
    w = 640, h = 480, mips = 3, settle = {"desktop", ceiling = 2000000000, stable = 600000000}}

local function fill(t)
    t = mac.with(ROW, t)
    if t.clock then t.finder_ex = {0, 440, 20, 640} end
    return t
end

-- One boot-to-About row, run when its disk was fetched.
local function row(t)
    t = fill(t)
    mac.media_row(t.label, function() mac.about_row(t) end, t.hd or t.fd)
end

local q605_71_hd = {label = "q605-71-hd", model = "q605", ram = 8192, hd = hd_71, box = 88, system = "7.1",
    finder = "goldens/q605-7.1-hd-finder.png", about = "goldens/q605-7.1-hd-about.png"}
row(q605_71_hd)
row{label = "lc475-71-hd", model = "lc475", ram = 4096, hd = hd_71, box = 83, system = "7.1",
    finder = "goldens/lc475-7.1-hd-finder.png", about = "goldens/lc475-7.1-hd-about.png"}
row{label = "lc575-71-hd", model = "lc575", ram = 12288, hd = hd_71, box = 86, system = "7.1",
    finder = "goldens/lc575-7.1-hd-finder.png", about = "goldens/lc575-7.1-hd-about.png"}
row{label = "q605-76-fd", model = "q605", ram = 8192, fd = fd_76, box = 88, system = "7.6", clock = true,
    finder = "goldens/q605-7.6-fd-finder.png", about = "goldens/q605-7.6-fd-about.png"}
row{label = "lc575-76-fd", model = "lc575", ram = 36864, fd = fd_76, box = 86, system = "7.6", clock = true,
    finder = "goldens/lc575-7.6-fd-finder.png", about = "goldens/lc575-7.6-fd-about.png"}
row{label = "lc475-75-hd", model = "lc475", ram = 20480, hd = hd_75, box = 83, system = "7.5", clock = true,
    finder = "goldens/lc475-7.5-hd-finder.png", about = "goldens/lc475-7.5-hd-about.png"}
row{label = "q605-753-hd", model = "q605", ram = 6144, hd = hd_753, box = 88, system = "7.5.3", clock = true,
    finder = "goldens/q605-7.5.3-hd-finder.png", about = "goldens/q605-7.5.3-hd-about.png"}

-- ---- q605-checkpoint ---------------------------------------------------------
-- The MEMCjr side of the family's checkpoint part (the bank latch values)
-- and the Cuda round-trip: save the booted Finder, wipe it with a fresh
-- boot, load it back and re-match the same golden.
mac.media_row("q605-checkpoint", function()
    mac.boot_finder(fill(mac.with(q605_71_hd, {label = "q605-checkpoint"})))
    mac.check("goldens/q605-7.1-hd-finder.png")
    assert(checkpoint.save(F"${vars.WORK_DIR}/suite-memcjr.gsc"), "q605-checkpoint: save failed")
    machine.boot{model = "q605", ram = 8192, rom = rom}
    assert(checkpoint.load(F"${vars.WORK_DIR}/suite-memcjr.gsc"), "q605-checkpoint: load failed")
    scheduler.run(2000000)
    assert(mac.boxflag() == 88, "q605-checkpoint: BoxFlag lost across the restore")
    mac.check("goldens/q605-7.1-hd-finder.png")
    -- Still running: the clock ticks and the Finder answers the Apple menu
    -- (the mouse goes through the restored Cuda).
    local t0 = debug.mac.globals.read("Ticks")
    mac.run_ticks(30)
    assert(debug.mac.globals.read("Ticks") > t0, "q605-checkpoint: Ticks stalled after the restore")
    mac.about_box()
    mac.check_about("goldens/q605-7.1-hd-about.png")
    mac.report_perf("suite-memcjr/q605-checkpoint", scheduler.instr_count)
end, hd_71)

mac.suite_done()
quit()
