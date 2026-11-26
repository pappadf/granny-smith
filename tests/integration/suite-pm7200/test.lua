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
--                   the 53C94 disk: Finder, a checkpoint round trip, About
--                   This Macintosh on the restored machine.
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

-- The board facts every row asserts: the model, the ROM revision, the
-- Catalyst identity, Platinum at $F8000000 and the scan-out it set.
local function check_board(rom_id)
    assert(machine.id == "pm7200", F"machine.id ${machine.id}")
    assert(machine.rom.id == rom_id, F"rom.id ${machine.rom.id}, want ${rom_id}")
    assert(mac.boxflag() == 0x66, F"BoxFlag ${mac.boxflag()}, want $66 (Gestalt 108)")
    assert((machine.memory.peek.l(0xF8000000) >> 16) == 0x3001, "Platinum part identifier != $3001xxxx")
    assert(machine.video.framebuffer.width == 640, "screen width != 640")
    assert(machine.video.framebuffer.depth == 16, F"depth ${machine.video.framebuffer.depth}, want 16 (1 MB VRAM)")
end

-- Pending scheduler events of one source.event (scheduler.events).
local function pending(source, event)
    local n = 0
    for _, e in ipairs(scheduler.events) do
        if e.source == source and e.event == event then n = n + 1 end
    end
    return n
end

-- Every disk row: the 53C94 disk at ID 0, the Finder as the current
-- application and then a stability wait (wait_desktop: the settle these
-- rows' perf baselines were taken with), the startup disk and no system
-- error, the board facts, and the menu-bar clock left out of the matches.
local ROW = {suite = "suite-pm7200", model = "pm7200", card = "platinum", feature = "hd-boot 53c94 about",
    rtc = "2026-04-26T21:30:36", settle = {"desktop", ceiling = 6000000000}, boot_drive = 0x8023, dserr = true,
    box = 0x66, finder_ex = {0, 440, 20, 640}, clock = true}

local function row(t)
    t = mac.with(ROW, t)
    t.check = function() check_board(t.rom_id) end
    mac.media_row(t.label, function() mac.about_row(t) end, t.hd)
end

-- A checkpoint round trip on the desktop: Platinum's registers, the DRAM
-- bank decode, the half-bank VRAM view and the VRAM come back, and the About
-- box opens on the restored machine.  The retrace clock is one
-- self-re-arming event chain, before the round trip and after it: the
-- restored queue brings the saved event back, and the rebuild must not arm
-- a second (two save/load cycles would make it four).
row{label = "pm7200-753-hd", rom = vars.ROM, rom_id = id_v1, ram = 16384, hd = hd_753, system = "7.5.3", mips = 20,
    finder = "goldens/pm7200-7.5.3-finder.png", about = "goldens/pm7200-7.5.3-about.png",
    after_finder = function()
        assert(pending("platinum", "vbl") == 1, F"${pending(\"platinum\", \"vbl\")} platinum.vbl events pending, want 1")
        for _ = 1, 2 do
            assert(checkpoint.save(F"${vars.WORK_DIR}/pm7200.checkpoint"), "checkpoint save")
            assert(checkpoint.load(F"${vars.WORK_DIR}/pm7200.checkpoint"), "checkpoint load")
            local n = pending("platinum", "vbl")
            assert(n == 1, F"${n} platinum.vbl events pending after the checkpoint round trip, want 1")
        end
        mac.check_ex("goldens/pm7200-7.5.3-finder.png", 0, 440, 20, 640)
    end}

-- Close the Energy Saver first-boot message, then About This Computer.
row{label = "pm7200-76-hd", rom = rom_v2, rom_id = id_v2, ram = 32768, hd = hd_76, system = "7.6",
    finder = "goldens/pm7200-7.6-finder.png", about = "goldens/pm7200-7.6-about.png",
    before_finder = function()
        machine.adb.keyboard.press("Return")
        mac.run_ticks(60)
    end}

-- Mac OS 8.1, 64 MB in two DIMMs' worth of banks.
row{label = "pm7200-81-hd", rom = vars.ROM, rom_id = id_v1, ram = 65536, hd = hd_81, system = "8.1",
    clock = false, about_layout = "Mac OS 8.1",
    finder = "goldens/pm7200-8.1-finder.png", about = "goldens/pm7200-8.1-about.png"}

-- The System 7.6 "Install 1" floppy through the SWIM3: boot blocks,
-- System, extensions, then the Installer.
mac.media_row("pm7200-76-fd", function()
    mac.boot_install1("pm7200", 16384, rom_v2, "2026-04-26T21:30:36", fd_76, "goldens/pm7200-7.6-floppy-boot.png")
    check_board(id_v2)
    mac.report_row("7.6", "swim3", "fd-boot")
    mac.report_perf("suite-pm7200/pm7200-76-fd", scheduler.instr_count)
end, fd_76)

mac.suite_done()
quit()
