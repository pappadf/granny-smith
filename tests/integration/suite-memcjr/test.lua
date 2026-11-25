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

-- Boot `model` from `media` (an "hd" or "fd" image) to the Finder and assert
-- the identity: machine id, RAM, ROM id and the BoxFlag.
local function boot(label, model, ram, kind, media, boxflag)
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
    assert(machine.rom.id == "ff7439ee", F"${label}: rom id ${machine.rom.id}, want ff7439ee")
    assert(debug.mac.globals.read("MMUFluff") == boxflag,
           F"${label}: BoxFlag ${debug.mac.globals.read(\"MMUFluff\")}, want ${boxflag}")
    assert(machine.screen.width == 640 and machine.screen.height == 480, F"${label}: not 640x480")
end

-- One boot-to-About row: `finder` and `about` are its two goldens.
-- `clock`: the System shows a menu-bar clock, so the goldens leave its
-- corner out.
local function about_row(label, model, ram, kind, media, boxflag, system, clock, finder, about)
    local t0 = scheduler.host_user_ns
    boot(label, model, ram, kind, media, boxflag)
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
    mac.report_row(system, "memcjr", F"${kind}-boot about")
    mac.report_perf(F"suite-memcjr/${label}", scheduler.instr_count)
end

local function row_q605_71_hd()
    about_row("q605-71-hd", "q605", 8192, "hd", hd_71, 88, "7.1", false,
              "goldens/q605-7.1-hd-finder.png", "goldens/q605-7.1-hd-about.png")
end
local function row_lc475_71_hd()
    about_row("lc475-71-hd", "lc475", 4096, "hd", hd_71, 83, "7.1", false,
              "goldens/lc475-7.1-hd-finder.png", "goldens/lc475-7.1-hd-about.png")
end
local function row_lc575_71_hd()
    about_row("lc575-71-hd", "lc575", 12288, "hd", hd_71, 86, "7.1", false,
              "goldens/lc575-7.1-hd-finder.png", "goldens/lc575-7.1-hd-about.png")
end
local function row_q605_76_fd()
    about_row("q605-76-fd", "q605", 8192, "fd", fd_76, 88, "7.6", true,
              "goldens/q605-7.6-fd-finder.png", "goldens/q605-7.6-fd-about.png")
end
local function row_lc575_76_fd()
    about_row("lc575-76-fd", "lc575", 36864, "fd", fd_76, 86, "7.6", true,
              "goldens/lc575-7.6-fd-finder.png", "goldens/lc575-7.6-fd-about.png")
end
local function row_lc475_75_hd()
    about_row("lc475-75-hd", "lc475", 20480, "hd", hd_75, 83, "7.5", true,
              "goldens/lc475-7.5-hd-finder.png", "goldens/lc475-7.5-hd-about.png")
end
local function row_q605_753_hd()
    about_row("q605-753-hd", "q605", 6144, "hd", hd_753, 88, "7.5.3", true,
              "goldens/q605-7.5.3-hd-finder.png", "goldens/q605-7.5.3-hd-about.png")
end

-- ---- q605-checkpoint ---------------------------------------------------------
-- The MEMCjr side of the family's checkpoint part (the bank latch values)
-- and the Cuda round-trip: save the booted Finder, wipe it with a fresh
-- boot, load it back and re-match the same golden.
local function row_q605_checkpoint()
    boot("q605-checkpoint", "q605", 8192, "hd", hd_71, 88)
    mac.check("goldens/q605-7.1-hd-finder.png")
    assert(checkpoint.save(F"${vars.WORK_DIR}/suite-memcjr.gsc"), "q605-checkpoint: save failed")
    machine.boot{model = "q605", ram = 8192, rom = rom}
    assert(checkpoint.load(F"${vars.WORK_DIR}/suite-memcjr.gsc"), "q605-checkpoint: load failed")
    scheduler.run(2000000)
    assert(debug.mac.globals.read("MMUFluff") == 88, "q605-checkpoint: BoxFlag lost across the restore")
    mac.check("goldens/q605-7.1-hd-finder.png")
    -- Still running: the clock ticks and the Finder answers the Apple menu
    -- (the mouse goes through the restored Cuda).
    local t0 = debug.mac.globals.read("Ticks")
    mac.run_ticks(30)
    assert(debug.mac.globals.read("Ticks") > t0, "q605-checkpoint: Ticks stalled after the restore")
    mac.about_box()
    mac.check_about("goldens/q605-7.1-hd-about.png")
    mac.report_perf("suite-memcjr/q605-checkpoint", scheduler.instr_count)
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

media_row("q605-71-hd", row_q605_71_hd, hd_71)
media_row("lc475-71-hd", row_lc475_71_hd, hd_71)
media_row("lc575-71-hd", row_lc575_71_hd, hd_71)
media_row("q605-76-fd", row_q605_76_fd, fd_76)
media_row("lc575-76-fd", row_lc575_76_fd, fd_76)
media_row("lc475-75-hd", row_lc475_75_hd, hd_75)
media_row("q605-753-hd", row_q605_753_hd, hd_753)
media_row("q605-checkpoint", row_q605_checkpoint, hd_71)

mac.suite_done()
quit()
