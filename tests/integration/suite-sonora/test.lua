-- The Sonora suite (see config.mk): LC III, LC III+, LC 520, LC 550, Color
-- Classic II.
--
-- Each row: boot, settle at the Finder, match the Finder golden, open About
-- This Macintosh, match it (guest-memory figures excluded: mac.check_about_at
-- by screen size), and assert id, ROM id and BoxFlag (mac.about_row).  (Gestalt's
-- machine type is BoxFlag + 6 on every ROM-identified model.)

local mac = dofile("../lib/mac.lua")

local DATA = "../../data/"
local hd_71 = DATA .. "systems/system_7_1_20mb_24ac_cd_32bit.img"
local hd_753 = DATA .. "systems/system_7_5_3_25mb_bare.img"
local hd_76 = DATA .. "systems/system_7_6_170mb_24ac.img"
local fd_76 = DATA .. "systems/SSW-7.6-1.4M/Disk Tools 1.img"

local ROMS = {
    lciii = {path = DATA .. "roms/lciii-ecbbc41c.rom", id = "ecbbc41c"},
    lciii_early = {path = DATA .. "roms/lciii-early-ec904829.rom", id = "ec904829"},
    lc550 = {path = DATA .. "roms/lc550-cc2-mactv-ede66cbd.rom", id = "ede66cbd"},
}

-- Every row: 8 MB, the clock pinned, a fixed head start then a stability
-- wait (the settle these rows' perf baselines were taken with).  Every System
-- here draws a menu-bar clock, left out of the matches.
local ROW = {suite = "suite-sonora", ram = 8192, rtc = 3000000000, clock = true,
    settle = {"stable", head = 250000000, ceiling = 1500000000}}

-- `rom` is a ROMS entry.
local function fill(t)
    t = mac.with(ROW, t)
    t.rom, t.rom_id = t.rom.path, t.rom.id
    if t.w then t.finder_ex = {0, t.w - 150, 20, t.w - 60} end
    return t
end

local function row(t)
    t = fill(t)
    mac.row(t.label, function() mac.about_row(t) end)
end

-- golden-collision-ok: lc520-753-hd-640x480x8-finder.png lciii-753-hd-640x480x8-finder.png lciiip-753-hd-640x480x8-finder.png — the same System 7.5.3 desktop at 640x480x8; machine.id, rom.id and BoxFlag (50/21/56) tell the rows apart
-- golden-collision-ok: lciii-753-hd-640x480x8-about.png lciiip-753-hd-640x480x8-about.png — 7.5.3 names both "Macintosh" with the same RAM; machine.id and BoxFlag (21 vs 56) tell them apart
-- golden-collision-ok: lc550-76-hd-640x480x8-finder.png lciii-early-76-hd-640x480x8-finder.png — the same System 7.6 desktop; machine.id, rom.id (ede66cbd vs ec904829) and BoxFlag (74 vs 21) tell them apart
row{label = "lciii-753-hd", model = "lciii", rom = ROMS.lciii, hd = hd_753, system = "7.5.3", box = 21, w = 640, h = 480, d = 8,
    finder = "goldens/lciii-753-hd-640x480x8-finder.png", about = "goldens/lciii-753-hd-640x480x8-about.png"}
row{label = "lciii-76-fd", model = "lciii", rom = ROMS.lciii, fd = fd_76, system = "7.6", box = 21, w = 640, h = 480, d = 8,
    finder = "goldens/lciii-76-fd-640x480x8-finder.png", about = "goldens/lciii-76-fd-640x480x8-about.png"}
row{label = "lciii-early-76-hd", model = "lciii", rom = ROMS.lciii_early, hd = hd_76, system = "7.6", box = 21,
    w = 640, h = 480, d = 8,
    finder = "goldens/lciii-early-76-hd-640x480x8-finder.png", about = "goldens/lciii-early-76-hd-640x480x8-about.png"}
row{label = "lciiip-753-hd", model = "lciiip", rom = ROMS.lciii, hd = hd_753, system = "7.5.3", box = 56, w = 640, h = 480, d = 8,
    finder = "goldens/lciiip-753-hd-640x480x8-finder.png", about = "goldens/lciiip-753-hd-640x480x8-about.png"}
row{label = "lc520-753-hd", model = "lc520", rom = ROMS.lc550, hd = hd_753, system = "7.5.3", box = 50, w = 640, h = 480, d = 8,
    finder = "goldens/lc520-753-hd-640x480x8-finder.png", about = "goldens/lc520-753-hd-640x480x8-about.png"}
row{label = "lc550-76-hd", model = "lc550", rom = ROMS.lc550, hd = hd_76, system = "7.6", box = 74, w = 640, h = 480, d = 8,
    finder = "goldens/lc550-76-hd-640x480x8-finder.png", about = "goldens/lc550-76-hd-640x480x8-about.png"}
-- On the Color Classic II's 512x384 screen the About window sits 64 pixels
-- left and 22 higher than on the 640-wide desktop (7.5.3); 7.6 on the
-- floppy places it 12 pixels higher still.
row{label = "cc2-753-hd", model = "colorclassicii", rom = ROMS.lc550, hd = hd_753, system = "7.5.3", box = 77,
    w = 512, h = 384, d = 8,
    finder = "goldens/cc2-753-hd-512x384x8-finder.png", about = "goldens/cc2-753-hd-512x384x8-about.png"}
row{label = "cc2-76-fd", model = "colorclassicii", rom = ROMS.lc550, fd = fd_76, system = "7.6", box = 77,
    w = 512, h = 384, d = 8, about_layout = "512x384 Disk Tools 7.6",
    finder = "goldens/cc2-76-fd-512x384x8-finder.png", about = "goldens/cc2-76-fd-512x384x8-about.png"}

-- System 7.1 refuses the LC III at startup without System Enabler 003, as on
-- the real machine: the ROM identified the board before the System looked.
mac.row("lciii-71-refused", function()
    mac.boot_finder(fill{label = "lciii-71-refused", model = "lciii", rom = ROMS.lciii, hd = hd_71, rtc = false, box = 21,
        settle = {"stable", head = 100000000, ceiling = 800000000}})
    mac.check("goldens/lciii-71-refused-640x480.png")
    mac.report_perf("suite-sonora/lciii-71-refused", scheduler.instr_count)
end)

-- The LC III on its other monitor, the 12" RGB (sense 010, 512x384), at
-- 20 MB (a 16 MB SIMM stitched above the soldered 4 MB), then a checkpoint
-- round-trip: save, boot a different machine, load, re-match.  Covers the
-- Sonora RAM decode with a SIMM and the restored video mode.
mac.row("lciii-12in-checkpoint", function()
    mac.boot_finder(fill{label = "lciii-12in-checkpoint", model = "lciii", rom = ROMS.lciii, ram = 20480, hd = hd_753,
        w = 512, h = 384, boot = {config = "{\"displays\":{\"builtin\":{\"monitor\":\"12in_rgb\"}}}"}})
    assert(machine.cpu.mmu.peek(0x1EF8, 4) == 20480 * 1024, "lciii-12in-checkpoint: PhysMemTop")
    mac.check_ex("goldens/lciii-12in-753-512x384x8-finder.png", 0, 362, 20, 452)
    mac.report_row("7.5.3", "builtin", "hd-boot 12in-rgb")
    assert(checkpoint.save(F"${vars.WORK_DIR}/suite-sonora.gsc"), "lciii-12in-checkpoint: save failed")
    machine.boot{model = "lc550", ram = 8192, rom = ROMS.lc550.path}
    assert(checkpoint.load(F"${vars.WORK_DIR}/suite-sonora.gsc"), "lciii-12in-checkpoint: load failed")
    scheduler.run(5000000)
    mac.wait_stable(400000000)
    assert(machine.id == "lciii" and machine.ram == 20480, "lciii-12in-checkpoint: restored machine wrong")
    mac.check_ex("goldens/lciii-12in-753-512x384x8-finder.png", 0, 362, 20, 452)
    mac.report_perf("suite-sonora/lciii-12in-checkpoint", scheduler.instr_count)
end)

mac.suite_done()
quit()
