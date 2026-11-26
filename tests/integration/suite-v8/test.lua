-- The V8 family suite (see config.mk): LC II, Classic II, Color Classic.
--
-- Each row: boot, settle at the Finder, match the Finder golden, open About
-- This Macintosh, match it (guest-memory figures excluded: mac.check_about_at
-- by screen size), and assert id, ROM id and BoxFlag (mac.about_row).

local mac = dofile("../lib/mac.lua")

local DATA = "../../data/"
local sys_701 = DATA .. "systems/System_7_0_1.image"
local hd_71 = DATA .. "systems/system_7_1_20mb_24ac_cd_32bit.img"
local hd_753 = DATA .. "systems/system_7_5_3_25mb_bare.img"
local hd_76 = DATA .. "systems/system_7_6_170mb_24ac.img"

local ROMS = {
    lcii = {path = DATA .. "roms/lcii-35c28f5f.rom", id = "35c28f5f"},
    classicii = {path = DATA .. "roms/classicii-3193670e.rom", id = "3193670e"},
    colorclassic = {path = DATA .. "roms/colorclassic-ecd99dc0.rom", id = "ecd99dc0"},
}

-- Every row: 10 MB, the clock pinned, a fixed head start then a stability
-- wait (the settle these rows' perf baselines were taken with).
local ROW = {suite = "suite-v8", ram = 10240, rtc = 3000000000,
    settle = {"stable", head = 250000000, ceiling = 1500000000}}

-- Fill in the ROM and, with `clock` (System 7.5+'s menu-bar clock), the
-- Finder match's clock exclusion.
local function fill(t)
    t = mac.with(ROW, t)
    t.rom, t.rom_id = ROMS[t.model].path, ROMS[t.model].id
    if t.clock then t.finder_ex = {0, t.w - 150, 20, t.w - 60} end
    return t
end

-- One Finder + About row.
local function row(t)
    t = fill(t)
    mac.row(t.label, function() mac.about_row(t) end)
end

row{label = "lcii-701-fd", model = "lcii", fd = sys_701, system = "7.0.1", box = 13, w = 640, h = 480, d = 1,
    finder = "goldens/lcii-701-fd-640x480x1-finder.png", about = "goldens/lcii-701-fd-640x480x1-about.png"}
row{label = "lcii-71-hd", model = "lcii", hd = hd_71, system = "7.1", box = 31, w = 640, h = 480, d = 1,
    finder = "goldens/lcii-71-hd-640x480x1-finder.png", about = "goldens/lcii-71-hd-640x480x1-about.png"}
row{label = "lcii-753-hd", model = "lcii", hd = hd_753, system = "7.5.3", box = 31, w = 640, h = 480, d = 4, clock = true,
    finder = "goldens/lcii-753-hd-640x480x4-finder.png", about = "goldens/lcii-753-hd-640x480x4-about.png"}
row{label = "clii-701-fd", model = "classicii", fd = sys_701, system = "7.0.1", box = 17, w = 512, h = 342, d = 1,
    finder = "goldens/clii-701-fd-512x342x1-finder.png", about = "goldens/clii-701-fd-512x342x1-about.png"}
row{label = "clii-71-hd", model = "classicii", hd = hd_71, system = "7.1", box = 17, w = 512, h = 342, d = 1,
    finder = "goldens/clii-71-hd-512x342x1-finder.png", about = "goldens/clii-71-hd-512x342x1-about.png"}
row{label = "ccl-753-hd", model = "colorclassic", hd = hd_753, system = "7.5.3", box = 43, w = 512, h = 384, d = 8,
    clock = true,
    finder = "goldens/ccl-753-hd-512x384x8-finder.png", about = "goldens/ccl-753-hd-512x384x8-about.png"}
row{label = "ccl-76-hd", model = "colorclassic", hd = hd_76, system = "7.6", box = 43, w = 512, h = 384, d = 8,
    clock = true,
    finder = "goldens/ccl-76-hd-512x384x8-finder.png", about = "goldens/ccl-76-hd-512x384x8-about.png"}

-- The Color Classic refuses System 7.0.1 with the startup alert, as the real
-- machine does (it needs 7.1 and its System Enabler): the ROM identified the
-- board as a Color Classic before the System looked.
mac.row("ccl-701-fd", function()
    mac.boot_finder(fill{label = "ccl-701-fd", model = "colorclassic", fd = sys_701, rtc = false,
        settle = {"stable", head = 100000000, ceiling = 600000000}})
    mac.check("goldens/ccl-701-fd-512x384-refused.png")
    mac.report_perf("suite-v8/ccl-701-fd", scheduler.instr_count)
end)

-- The LC II on its other monitor, the 12" RGB (sense 010, 512x384), at 6 MB
-- (2 MB of SIMMs below the soldered 4 MB), then a checkpoint round-trip:
-- save, boot a different machine, load, re-match.  Covers the V8 RAM decode
-- with SIMMs and the video's restored strap.
mac.row("lcii-12in-checkpoint", function()
    mac.boot_finder(fill{label = "lcii-12in-checkpoint", model = "lcii", ram = 6144, fd = sys_701, w = 512, h = 384,
        boot = {config = "{\"displays\":{\"builtin\":{\"monitor\":\"12in_rgb\"}}}"}})
    assert(((machine.rbv.monp >> 3) & 7) == 2, "lcii-12in-checkpoint: MonP does not latch sense 2")
    mac.check("goldens/lcii-12in-701-512x384x1-finder.png")
    mac.report_row("7.0.1", "builtin", "fd-boot 12in-rgb")
    assert(checkpoint.save(F"${vars.WORK_DIR}/suite-v8.gsc"), "lcii-12in-checkpoint: save failed")
    machine.boot{model = "classicii", ram = 2048, rom = ROMS.classicii.path}
    assert(checkpoint.load(F"${vars.WORK_DIR}/suite-v8.gsc"), "lcii-12in-checkpoint: load failed")
    scheduler.run(5000000)
    mac.wait_stable(400000000)
    assert(machine.id == "lcii" and machine.ram == 6144, "lcii-12in-checkpoint: restored machine wrong")
    mac.check("goldens/lcii-12in-701-512x384x1-finder.png")
    mac.report_perf("suite-v8/lcii-12in-checkpoint", scheduler.instr_count)
end)

mac.suite_done()
quit()
