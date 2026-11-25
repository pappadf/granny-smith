-- djMEMC family suite: Centris 610/650, Quadra 610/650/800 ("Wombat" and
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
--
-- golden-collision-ok: the Finder desktops of rows booting the same System
-- are alike across models; each row asserts its own BoxFlag and machine id,
-- and the About goldens, which name the model, differ.

local mac = dofile("../lib/mac.lua")

local rom_centris = "../../data/roms/c610-c650-f1a6f343.rom"
local rom_quadra = "../../data/roms/c610-c650-q610-q650-q800-f1acad13.rom"
local hd_71 = "../../data/systems/system_7_1_20mb_24ac_cd_32bit_enabler040.img"
local hd_75 = "../../data/systems/system_7_5_0_77mb_mode32_24ac.img"
local hd_753 = "../../data/systems/system_7_5_3_25mb_bare.img"
local hd_76 = "../../data/systems/system_7_6_170mb_24ac.img"
local fd_76 = "../../data/systems/SSW-7.6-1.4M/Disk Tools 1.img"

local rom_ids = {[rom_centris] = "f1a6f343", [rom_quadra] = "f1acad13"}

-- Boot `model` from `media` (an "hd" or "fd" image) to the Finder and assert
-- the identity: machine id, RAM, ROM id and the BoxFlag.
local function boot(label, model, rom, ram, kind, media, boxflag)
    machine.boot{model = model, ram = ram, rom = rom}
    machine.rtc.time = 3000000000 -- the 7.5+ menu-bar clock, pinned
    if kind == "hd" then
        machine.scsi.attach_hd(media, 0)
    else
        machine.floppy.drive[0].insert(media)
    end
    mac.wait_desktop(2000000000)
    mac.wait_stable(600000000)
    assert(machine.id == model, F"${label}: machine.id ${machine.id}, want ${model}")
    assert(machine.ram == ram, F"${label}: machine.ram ${machine.ram}, want ${ram}")
    assert(machine.rom.id == rom_ids[rom], F"${label}: rom id ${machine.rom.id}, want ${rom_ids[rom]}")
    assert(debug.mac.globals.read("MMUFluff") == boxflag,
           F"${label}: BoxFlag ${debug.mac.globals.read(\"MMUFluff\")}, want ${boxflag}")
    assert(machine.screen.width == 640 and machine.screen.height == 480, F"${label}: not 640x480")
end

-- One boot-to-About row.  `clock`: the System shows a menu-bar clock, so
-- the goldens leave its corner out.
-- golden-collision-ok: c610-7.1-hd-finder.png c650-7.1-hd-finder.png q610-7.1-hd-finder.png q650-7.1-hd-finder.png q800-7.1-hd-finder.png — the same System 7.1 desktop on five boards; machine.id, BoxFlag (24/29/30/46/47) and each row's About golden, which names the model, tell them apart
-- golden-collision-ok: c610-7.6-fd-finder.png q800-7.6-fd-finder.png — the same Disk Tools desktop; machine.id and BoxFlag (46 vs 29) tell the rows apart
-- `finder` and `about` are the row's two goldens, named literally so that
-- scripts/check-goldens.py can see them.
local function about_row(label, model, rom, ram, kind, media, boxflag, system, clock, finder, about)
    local t0 = scheduler.host_user_ns
    boot(label, model, rom, ram, kind, media, boxflag)
    if clock then
        mac.check_ex(finder, 0, 440, 20, 640)
    else
        mac.check(finder)
    end
    mac.about_box()
    if clock then
        mac.check_about_clock(about)
    else
        mac.check_about(about)
    end
    mac.mips_floor(label, t0, 3)
    mac.report_row(system, "djmemc", F"${kind}-boot about")
    mac.report_perf(F"suite-djmemc/${label}", scheduler.instr_count)
end

local function row_q800_75_hd() about_row("q800-75-hd", "q800", rom_quadra, 16384, "hd", hd_75, 29, "7.5", true,
    "goldens/q800-7.5-hd-finder.png", "goldens/q800-7.5-hd-about.png") end
local function row_q800_76_fd() about_row("q800-76-fd", "q800", rom_quadra, 16384, "fd", fd_76, 29, "7.6", true,
    "goldens/q800-7.6-fd-finder.png", "goldens/q800-7.6-fd-about.png") end
local function row_c650_71_hd() about_row("c650-71-hd", "c650", rom_centris, 8192, "hd", hd_71, 24, "7.1", false,
    "goldens/c650-7.1-hd-finder.png", "goldens/c650-7.1-hd-about.png") end
local function row_q800_71_hd() about_row("q800-71-hd", "q800", rom_centris, 8192, "hd", hd_71, 29, "7.1", false,
    "goldens/q800-7.1-hd-finder.png", "goldens/q800-7.1-hd-about.png") end
local function row_q650_71_hd() about_row("q650-71-hd", "q650", rom_quadra, 8192, "hd", hd_71, 30, "7.1", false,
    "goldens/q650-7.1-hd-finder.png", "goldens/q650-7.1-hd-about.png") end
local function row_q610_71_hd() about_row("q610-71-hd", "q610", rom_quadra, 8192, "hd", hd_71, 47, "7.1", false,
    "goldens/q610-7.1-hd-finder.png", "goldens/q610-7.1-hd-about.png") end
local function row_c610_71_hd() about_row("c610-71-hd", "c610", rom_centris, 8192, "hd", hd_71, 46, "7.1", false,
    "goldens/c610-7.1-hd-finder.png", "goldens/c610-7.1-hd-about.png") end
local function row_c610_76_fd() about_row("c610-76-fd", "c610", rom_quadra, 8192, "fd", fd_76, 46, "7.6", true,
    "goldens/c610-7.6-fd-finder.png", "goldens/c610-7.6-fd-about.png") end
local function row_q610_76_hd() about_row("q610-76-hd", "q610", rom_quadra, 12288, "hd", hd_76, 47, "7.6", true,
    "goldens/q610-7.6-hd-finder.png", "goldens/q610-7.6-hd-about.png") end
local function row_q650_753_hd() about_row("q650-753-hd", "q650", rom_quadra, 24576, "hd", hd_753, 30, "7.5.3", true,
    "goldens/q650-7.5.3-hd-finder.png", "goldens/q650-7.5.3-hd-about.png") end

-- ---- q800-checkpoint ---------------------------------------------------------
-- The family's own checkpoint part (djMEMC bank registers, IOSB, Kiwi, the
-- overlay) round-trips: save the booted Finder, wipe it with a fresh boot,
-- load it back and re-match the same golden.
local function row_q800_checkpoint()
    boot("q800-checkpoint", "q800", rom_quadra, 16384, "hd", hd_75, 29)
    mac.check_ex("goldens/q800-7.5-hd-finder.png", 0, 440, 20, 640)
    assert(checkpoint.save(F"${vars.WORK_DIR}/suite-djmemc.gsc"), "q800-checkpoint: save failed")
    machine.boot{model = "q800", ram = 16384, rom = rom_quadra}
    assert(checkpoint.load(F"${vars.WORK_DIR}/suite-djmemc.gsc"), "q800-checkpoint: load failed")
    scheduler.run(2000000)
    assert(debug.mac.globals.read("MMUFluff") == 29, "q800-checkpoint: BoxFlag lost across the restore")
    mac.check_ex("goldens/q800-7.5-hd-finder.png", 0, 440, 20, 640)
    -- Still running: the clock ticks and the Finder answers the Apple menu.
    local t0 = debug.mac.globals.read("Ticks")
    mac.run_ticks(30)
    assert(debug.mac.globals.read("Ticks") > t0, "q800-checkpoint: Ticks stalled after the restore")
    mac.about_box()
    mac.check_about_clock("goldens/q800-7.5-hd-about.png")
    mac.report_perf("suite-djmemc/q800-checkpoint", scheduler.instr_count)
end

-- ---- row dispatch -----------------------------------------------------------

local function media_row(label, fn, ...)
    if not mac.row_on(label) then return end
    for _, m in ipairs({...}) do
        if not mac.have_media(m) then
            echo(F"skip: ${label} (${m} not fetched)")
            return
        end
    end
    mac.row(label, fn)
end

media_row("q800-75-hd", row_q800_75_hd, hd_75)
media_row("q800-76-fd", row_q800_76_fd, fd_76)
media_row("c650-71-hd", row_c650_71_hd, hd_71, rom_centris)
media_row("c610-71-hd", row_c610_71_hd, hd_71, rom_centris)
media_row("q800-71-hd", row_q800_71_hd, hd_71, rom_centris)
media_row("q650-71-hd", row_q650_71_hd, hd_71)
media_row("q610-71-hd", row_q610_71_hd, hd_71)
media_row("c610-76-fd", row_c610_76_fd, fd_76)
media_row("q610-76-hd", row_q610_76_hd, hd_76)
media_row("q650-753-hd", row_q650_753_hd, hd_753)
media_row("q800-checkpoint", row_q800_checkpoint, hd_75)

mac.suite_done()
quit()
