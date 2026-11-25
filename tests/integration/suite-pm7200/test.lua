-- Power Macintosh 7200 suite — the Catalyst board.
--
-- The 7500's Bandit and Grand Central, but Platinum (memory controller and
-- on-board video in one part) where the 7500 has Hammerhead, the DACula
-- RAMDAC, 1 MB of VRAM, and the Curio's 53C94 as the only SCSI bus — no
-- MESH, no Chaos/Control (Apple, "Power Macintosh 7200 Computer" Developer
-- Note, 1995).  It runs the 7500/8500/9500 ROM, which tells the board apart
-- by Platinum's part identifier.
--
-- Row map:
--
--   pm7200-753-hd   v1 ROM, System 7.5.3 (the 7200 shipped with 7.5.2) from
--                   the 53C94 disk: Finder, About This Macintosh.
--   pm7200-76-hd    v2 ROM, System 7.6 from the 53C94 disk: Finder, About
--                   This Computer.
--   pm7200-81-hd    v1 ROM, Mac OS 8.1 from the 53C94 disk, 64 MB in two
--                   DIMMs' worth of banks: Finder, About This Computer.
--   pm7200-76-fd    v2 ROM, the System 7.6 "Install 1" floppy through the
--                   SWIM3 to the Installer.
--
-- Identity: BoxFlag ($0CB3) reads $66, the Catalyst ProductInfo (Gestalt
-- 108, the developer note's machine ID); these Systems' About windows print
-- the generic "Power Macintosh" / "Mac OS computer", so BoxFlag, machine.id
-- and rom.id carry the model identity and the About goldens prove the boot
-- reached a working Finder and the Apple menu.  Every row also checks the
-- Platinum part identifier and that the 1 MB of VRAM held the driver to a
-- depth it can scan out (16 bpp at 640x480).

local mac = dofile("../lib/mac.lua")

local hd_753 = "../../data/systems/system_7_5_3_25mb_bare.img"
local hd_76 = "../../data/systems/system_7_6_170mb_24ac.img"
local hd_81 = "../../data/apps/quake_8_1.img"
local fd_76 = "../../data/systems/SSW-7.6-1.4M/Install 1.img"

local rom_v2 = "../../data/roms/pm7500-pm8500-pm9500-v2-9630c68b-4db4a42fea3b53b3.rom"

local id_v1 = "96cd923d-c241cd82bf90797a"
local id_v2 = "9630c68b-4db4a42fea3b53b3"

-- BoxFlag, read through the user-context window once the 68k emulator is
-- in user mode.
local function boxflag()
    while (machine.cpu.msr & 0x4000) == 0 do scheduler.run(1000) end
    return machine.cpu.mmu.peek(0xCB3, 1)
end

-- The board facts every row asserts: the model, the ROM revision, the
-- Catalyst identity, Platinum at $F8000000 and the scan-out it set.
local function check_board(rom_id)
    assert(machine.id == "pm7200", F"machine.id ${machine.id}")
    assert(machine.rom.id == rom_id, F"rom.id ${machine.rom.id}, want ${rom_id}")
    assert(boxflag() == 0x66, F"BoxFlag ${boxflag()}, want $66 (Gestalt 108)")
    assert((machine.memory.peek.l(0xF8000000) >> 16) == 0x3001, "Platinum part identifier != $3001xxxx")
    assert(machine.video.framebuffer.width == 640, "screen width != 640")
    assert(machine.video.framebuffer.depth == 16, F"depth ${machine.video.framebuffer.depth}, want 16 (1 MB VRAM)")
end

-- Boot from the 53C94 disk at ID 0 to a stable Finder.
local function boot_hd(rom, hd, ram)
    machine.boot{model = "pm7200", ram = ram, rom = rom}
    machine.rtc.time = "2026-04-26T21:30:36"
    machine.scsi.attach_hd(hd, 0)
    mac.wait_desktop(6000000000)
    assert(debug.mac.globals.read("BootDrive") == 0x8023, F"BootDrive ${debug.mac.globals.read(\"BootDrive\"):04x}")
    assert(debug.mac.globals.read("DSErrCode") == 0, F"system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")
end

local function row_pm7200_753_hd()
    local t0 = scheduler.host_user_ns
    boot_hd(vars.ROM, hd_753, 16384)
    check_board(id_v1)
    mac.check_ex("goldens/pm7200-7.5.3-finder.png", 0, 440, 20, 640)
    mac.about_box()
    mac.check_about_clock("goldens/pm7200-7.5.3-about.png")
    mac.mips_floor("pm7200-753-hd", t0, 20)
    mac.report_row("7.5.3", "platinum", "hd-boot 53c94 about")
    mac.report_perf("suite-pm7200/pm7200-753-hd", scheduler.instr_count)
end

local function row_pm7200_76_hd()
    boot_hd(rom_v2, hd_76, 32768)
    check_board(id_v2)
    -- Close the Energy Saver first-boot message, then About This Computer.
    machine.adb.keyboard.press("Return")
    mac.run_ticks(60)
    mac.check_ex("goldens/pm7200-7.6-finder.png", 0, 440, 20, 640)
    mac.about_box()
    mac.check_about_clock("goldens/pm7200-7.6-about.png")
    mac.report_row("7.6", "platinum", "hd-boot 53c94 about")
    mac.report_perf("suite-pm7200/pm7200-76-hd", scheduler.instr_count)
end

-- Mac OS 8.1's About This Computer, matched with the menu-bar clock, the
-- "Largest Unused Block" figure and the Mac OS row's figure and bar
-- excluded (the suite-tnt-1997 rectangles).
local function check_about_81(ref)
    if mac.REGEN_MODE == "1" then
        machine.screen.save(ref)
    else
        machine.screen.match(ref, 0, 440, 20, 640, 182, 90, 197, 330, 202, 250, 219, 530)
    end
end

local function row_pm7200_81_hd()
    boot_hd(vars.ROM, hd_81, 65536)
    check_board(id_v1)
    mac.check_ex("goldens/pm7200-8.1-finder.png", 0, 440, 20, 640)
    mac.about_box()
    check_about_81("goldens/pm7200-8.1-about.png")
    mac.report_row("8.1", "platinum", "hd-boot 53c94 about")
    mac.report_perf("suite-pm7200/pm7200-81-hd", scheduler.instr_count)
end

-- The System 7.6 "Install 1" floppy through the SWIM3: boot blocks,
-- System, extensions, then the Installer.
local function row_pm7200_76_fd()
    machine.boot{model = "pm7200", ram = 16384, rom = rom_v2}
    machine.rtc.time = "2026-04-26T21:30:36"
    machine.floppy.drive[0].insert(fd_76)
    assert(machine.floppy.drive[0].present, "boot floppy not in FD0")
    mac.run_until_match_ex("goldens/pm7200-7.6-floppy-boot.png", 0, 440, 20, 640, 7600000000, 250000000)
    mac.check_ex("goldens/pm7200-7.6-floppy-boot.png", 0, 440, 20, 640)
    check_board(id_v2)
    mac.report_row("7.6", "swim3", "fd-boot")
    mac.report_perf("suite-pm7200/pm7200-76-fd", scheduler.instr_count)
end

-- ---- row dispatch -----------------------------------------------------------

local function dispatch(label, fn, media)
    if mac.row_on(label) then
        if mac.have_media(media) then
            mac.row(label, fn)
        else
            echo(F"skip: ${label} (${media} not fetched)")
        end
    end
end

dispatch("pm7200-753-hd", row_pm7200_753_hd, hd_753)
dispatch("pm7200-76-hd", row_pm7200_76_hd, hd_76)
dispatch("pm7200-81-hd", row_pm7200_81_hd, hd_81)
dispatch("pm7200-76-fd", row_pm7200_76_fd, fd_76)

mac.suite_done()
quit()
