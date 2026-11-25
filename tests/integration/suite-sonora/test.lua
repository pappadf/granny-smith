-- The Sonora suite (see config.mk): LC III, LC III+, LC 520, LC 550, Color
-- Classic II.
--
-- Each row: boot, settle at the Finder, match the Finder golden, open About
-- This Macintosh, match it (guest-memory figures excluded, as check_about
-- does on 640-wide screens), and assert id, ROM id and BoxFlag.  (Gestalt's
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

-- About-window exclusions on the Color Classic II's 512x384 screen: the
-- window sits 64 pixels left and 22 higher than on the 640-wide desktop
-- (7.5.3); 7.6 on the floppy places it 12 pixels higher still.
local ABOUT_EXCL_384 = {
    ["7.5.3"] = {122, 376, 136, 426, 140, 186, 154, 426},
    ["7.6"] = {108, 376, 124, 426, 126, 186, 142, 426},
}

-- Boot `model` on `rom` from `media` ("fd" or "hd") and settle at the Finder.
local function boot(model, rom, media_kind, media, ram)
    ram = ram or 8192
    machine.boot{model = model, ram = ram, rom = rom.path}
    machine.rtc.time = 3000000000
    if media_kind == "fd" then machine.floppy.drive[0].insert(media) else machine.scsi.attach_hd(media, 0) end
    scheduler.run(250000000)
    mac.wait_stable(1500000000)
    assert(machine.id == model, F"${model}: machine.id ${machine.id}")
    assert(machine.rom.id == rom.id, F"${model}: rom.id ${machine.rom.id}")
    assert(machine.ram == ram, F"${model}: machine.ram ${machine.ram}")
end

-- BoxFlag ($0CB3), read through the user-context window.
local function box_flag()
    return machine.cpu.mmu.peek(0xCB3, 1)
end

-- Match the About window (7.5+ draws a menu-bar clock, excluded too).
local function check_about(ref, system)
    if machine.screen.width == 640 then
        mac.check_about_clock(ref)
        return
    end
    if mac.REGEN_MODE == "1" then
        machine.screen.save(ref)
        return
    end
    local e = ABOUT_EXCL_384[system]
    machine.screen.match(ref, e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], 0, 360, 20, 450)
end

-- One Finder + About row.
local function about_row(label, model, rom, media_kind, media, system, box, w, h, d)
    boot(model, rom, media_kind, media)
    assert(machine.screen.width == w and machine.screen.height == h,
        F"${label}: screen ${machine.screen.width}x${machine.screen.height}")
    if mac.REGEN_MODE == "1" then echo(F"${label}: depth ${machine.screen.depth}") end
    assert(mac.REGEN_MODE == "1" or machine.screen.depth == d, F"${label}: depth ${machine.screen.depth}")
    local stem = F"goldens/${label}-${w}x${h}x${d}"
    mac.check_ex(stem .. "-finder.png", 0, w - 150, 20, w - 60)
    mac.about_box()
    check_about(stem .. "-about.png", system)
    assert(box_flag() == box, F"${label}: BoxFlag ${box_flag()}, want ${box}")
    mac.report_row(system, "builtin", media_kind .. "-boot about")
    mac.report_perf("suite-sonora/" .. label, scheduler.instr_count)
end

mac.row("lciii-753-hd", function() about_row("lciii-753-hd", "lciii", ROMS.lciii, "hd", hd_753, "7.5.3", 21, 640, 480, 8) end)
mac.row("lciii-76-fd", function() about_row("lciii-76-fd", "lciii", ROMS.lciii, "fd", fd_76, "7.6", 21, 640, 480, 8) end)
mac.row("lciii-early-76-hd", function()
    about_row("lciii-early-76-hd", "lciii", ROMS.lciii_early, "hd", hd_76, "7.6", 21, 640, 480, 8) end)
mac.row("lciiip-753-hd", function() about_row("lciiip-753-hd", "lciiip", ROMS.lciii, "hd", hd_753, "7.5.3", 56, 640, 480, 8) end)
mac.row("lc520-753-hd", function() about_row("lc520-753-hd", "lc520", ROMS.lc550, "hd", hd_753, "7.5.3", 50, 640, 480, 8) end)
mac.row("lc550-76-hd", function() about_row("lc550-76-hd", "lc550", ROMS.lc550, "hd", hd_76, "7.6", 74, 640, 480, 8) end)
mac.row("cc2-753-hd", function()
    about_row("cc2-753-hd", "colorclassicii", ROMS.lc550, "hd", hd_753, "7.5.3", 77, 512, 384, 8) end)
mac.row("cc2-76-fd", function()
    about_row("cc2-76-fd", "colorclassicii", ROMS.lc550, "fd", fd_76, "7.6", 77, 512, 384, 8) end)

-- System 7.1 refuses the LC III at startup without System Enabler 003, as on
-- the real machine: the ROM identified the board before the System looked.
mac.row("lciii-71-refused", function()
    machine.boot{model = "lciii", ram = 8192, rom = ROMS.lciii.path}
    machine.scsi.attach_hd(hd_71, 0)
    scheduler.run(100000000)
    mac.wait_stable(800000000)
    assert(machine.id == "lciii" and machine.rom.id == ROMS.lciii.id, "lciii-71-refused: machine")
    assert(box_flag() == 21, F"lciii-71-refused: BoxFlag ${box_flag()}")
    mac.check("goldens/lciii-71-refused-640x480.png")
    mac.report_perf("suite-sonora/lciii-71-refused", scheduler.instr_count)
end)

-- The LC III on its other monitor, the 12" RGB (sense 010, 512x384), at
-- 20 MB (a 16 MB SIMM stitched above the soldered 4 MB), then a checkpoint
-- round-trip: save, boot a different machine, load, re-match.  Covers the
-- Sonora RAM decode with a SIMM and the restored video mode.
mac.row("lciii-12in-checkpoint", function()
    machine.boot{model = "lciii", ram = 20480, rom = ROMS.lciii.path,
        config = "{\"displays\":{\"builtin\":{\"monitor\":\"12in_rgb\"}}}"}
    machine.rtc.time = 3000000000
    machine.scsi.attach_hd(hd_753, 0)
    scheduler.run(250000000)
    mac.wait_stable(1500000000)
    assert(machine.screen.width == 512 and machine.screen.height == 384, "lciii-12in-checkpoint: not 512x384")
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
