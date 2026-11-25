-- The V8 family suite (see config.mk): LC II, Classic II, Color Classic.
--
-- Each row: boot, settle at the Finder, match the Finder golden, open About
-- This Macintosh, match it (guest-memory figures excluded, as check_about
-- does on 640-wide screens), and assert id, ROM id and BoxFlag.

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

-- About-window exclusions per screen width: "Largest Unused Block" and the
-- System Software row's figure and bar move with boot-time heap use.  The
-- 640-wide desktop is check_about's; the compact screens centre the window
-- 64 pixels further left and (512x342) higher.
local ABOUT_EXCL = {
    [342] = {100, 376, 114, 426, 118, 186, 132, 426},
    [384] = {122, 376, 136, 426, 140, 186, 154, 426},
}

-- Boot `model` from `media` ("fd" or "hd" path) and settle at the Finder.
local function boot(model, media_kind, media)
    local rom = ROMS[model]
    machine.boot{model = model, ram = 10240, rom = rom.path}
    machine.rtc.time = 3000000000
    if media_kind == "fd" then machine.floppy.drive[0].insert(media) else machine.scsi.attach_hd(media, 0) end
    scheduler.run(250000000)
    mac.wait_stable(1500000000)
    assert(machine.id == model, F"${model}: machine.id ${machine.id}")
    assert(machine.rom.id == rom.id, F"${model}: rom.id ${machine.rom.id}")
    assert(machine.ram == 10240, F"${model}: machine.ram ${machine.ram}")
end

-- BoxFlag ($0CB3), read through the user-context window.
local function box_flag()
    return machine.cpu.mmu.peek(0xCB3, 1)
end

-- Match the About window: 640-wide via check_about[_clock]; compact screens
-- with the per-height exclusions (and the 7.5+ menu-bar clock).
local function check_about(ref, clock)
    local h = machine.screen.height
    if h == 480 then
        if clock then mac.check_about_clock(ref) else mac.check_about(ref) end
        return
    end
    if mac.REGEN_MODE == "1" then
        machine.screen.save(ref)
        return
    end
    local e = ABOUT_EXCL[h]
    if clock then
        machine.screen.match(ref, e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], 0, 360, 20, 450)
    else
        machine.screen.match(ref, e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8])
    end
end

-- One Finder + About row.
local function about_row(label, model, media_kind, media, system, box, w, h, d, finder, about, clock)
    boot(model, media_kind, media)
    assert(machine.screen.width == w and machine.screen.height == h,
        F"${label}: screen ${machine.screen.width}x${machine.screen.height}")
    assert(machine.screen.depth == d, F"${label}: depth ${machine.screen.depth}")
    if clock then mac.check_ex(finder, 0, w - 150, 20, w - 60) else mac.check(finder) end
    mac.about_box()
    check_about(about, clock)
    assert(box_flag() == box, F"${label}: BoxFlag ${box_flag()}, want ${box}")
    mac.report_row(system, "builtin", media_kind .. "-boot about")
    mac.report_perf("suite-v8/" .. label, scheduler.instr_count)
end

mac.row("lcii-701-fd", function() about_row("lcii-701-fd", "lcii", "fd", sys_701, "7.0.1", 13, 640, 480, 1,
    "goldens/lcii-701-fd-640x480x1-finder.png", "goldens/lcii-701-fd-640x480x1-about.png") end)
mac.row("lcii-71-hd", function() about_row("lcii-71-hd", "lcii", "hd", hd_71, "7.1", 31, 640, 480, 1,
    "goldens/lcii-71-hd-640x480x1-finder.png", "goldens/lcii-71-hd-640x480x1-about.png") end)
mac.row("lcii-753-hd", function() about_row("lcii-753-hd", "lcii", "hd", hd_753, "7.5.3", 31, 640, 480, 4,
    "goldens/lcii-753-hd-640x480x4-finder.png", "goldens/lcii-753-hd-640x480x4-about.png", true) end)
mac.row("clii-701-fd", function() about_row("clii-701-fd", "classicii", "fd", sys_701, "7.0.1", 17, 512, 342, 1,
    "goldens/clii-701-fd-512x342x1-finder.png", "goldens/clii-701-fd-512x342x1-about.png") end)
mac.row("clii-71-hd", function() about_row("clii-71-hd", "classicii", "hd", hd_71, "7.1", 17, 512, 342, 1,
    "goldens/clii-71-hd-512x342x1-finder.png", "goldens/clii-71-hd-512x342x1-about.png") end)
mac.row("ccl-753-hd", function() about_row("ccl-753-hd", "colorclassic", "hd", hd_753, "7.5.3", 43, 512, 384, 8,
    "goldens/ccl-753-hd-512x384x8-finder.png", "goldens/ccl-753-hd-512x384x8-about.png", true) end)
mac.row("ccl-76-hd", function() about_row("ccl-76-hd", "colorclassic", "hd", hd_76, "7.6", 43, 512, 384, 8,
    "goldens/ccl-76-hd-512x384x8-finder.png", "goldens/ccl-76-hd-512x384x8-about.png", true) end)

-- The Color Classic refuses System 7.0.1 with the startup alert, as the real
-- machine does (it needs 7.1 and its System Enabler): the ROM identified the
-- board as a Color Classic before the System looked.
mac.row("ccl-701-fd", function()
    machine.boot{model = "colorclassic", ram = 10240, rom = ROMS.colorclassic.path}
    machine.floppy.drive[0].insert(sys_701)
    scheduler.run(100000000)
    mac.wait_stable(600000000)
    assert(machine.id == "colorclassic", "ccl-701-fd: machine.id")
    assert(machine.rom.id == ROMS.colorclassic.id, "ccl-701-fd: rom.id")
    mac.check("goldens/ccl-701-fd-512x384-refused.png")
    mac.report_perf("suite-v8/ccl-701-fd", scheduler.instr_count)
end)

mac.suite_done()
quit()
