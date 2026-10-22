-- Converted from test.script by scripts/script-to-lua.py
-- Macintosh SE/30 suite (see config.mk for what it absorbs).
--
-- Row map (former directory → row; **bold** = new matrix coverage):
--
--   se30-nomedia     se30-boot: no bootable media → the blinking "?"
--                    floppy icon renders (1 MB, the minimum RAM option)
--   se30-chime       se30-boot-chime: sample-exact ASC wavetable chime
--                    through the producer path (frame-exact window)
--   se30-608-fd      the **direct-SWIM 800K GCR transport row**, moved
--                    out of boot-matrix (2 MB)
--   se30-71-fd       **SE/30 x 7.1**: se30-floppy-hd re-media'd to the
--                    1.4M Disk Tools floppy — genuine 7.1, and the
--                    1.44M MFM transport row (+ About box, 4 MB)
--   se30-701-gsvrom  se30-gsvrom: Finder on the runtime-generated
--                    generic GS vROM (7.0.1, 8 MB)
--   se30-75-hd       **SE/30 x 7.5**: prepared 7.5 image over SCSI —
--                    absorbs se30-scsi's HD-boot coverage (16 MB)
--   se30-75-hd-addr32  the same boot in 32-bit mode (MODE32): the frame
--                    buffer at the slot-$E base $FE000000
--   se30-71-hd-addr32  7.1 + 32-bit enabler in 32-bit mode: the vSyncEnA
--                    (VIA1 PB6) gate on the slot-$E video interrupt
--   se30-checkpoint  **GLUE-family checkpoint round-trip**
--
-- Each row also picks a distinct supported RAM configuration, so the
-- system-version and RAM-sizing axes fill together.

local mac = dofile("../lib/mac.lua")

local ssw_608 = "../../data/systems/SSW-6.08-800K/SSW 6.0.8-800k Disk1of4"
local disk_tools_71 = "../../data/systems/SSW-7.1-1.4M/Disk Tools.img"
local sys_701 = "../../data/systems/System_7_0_1.image"
local hd_75 = "../../data/systems/system_7_5_0_77mb_mode32_24ac.img"
local hd_71 = "../../data/systems/system_7_1_20mb_24ac_cd_32bit.img"

-- ---- se30-nomedia -----------------------------------------------------------
-- With no media the ROM finds no startup device and draws the floppy "?" icon
-- (the machine's PRAM skips the drive-discovery wait, so it gets there at
-- once). The budget is FIXED deliberately (not padding): the icon BLINKS, so
-- the golden is animation-phase-sensitive — a settle wait never converges and a
-- polling wait would only ever sample multiples of its step. The 335 M figure
-- is the budget the row has always had; it now lands in the icon's "?" phase.
local function row_se30_nomedia()
    machine.boot{model = "se30", ram = 1024, rom = vars.ROM}
    scheduler.run(335000000)
    assert(machine.id == "se30", "se30-nomedia: machine.id != se30")
    assert(machine.ram == 1024, "se30-nomedia: machine.ram != 1024")
    assert(machine.screen.width == 512, "se30-nomedia: width != 512")
    assert(machine.screen.height == 342, "se30-nomedia: height != 342")
    mac.check("goldens/se30-nomedia-512x342.png")
    mac.report_perf("suite-se30/se30-nomedia", scheduler.instr_count)
end

-- ---- se30-chime -------------------------------------------------------------
-- Sample-exact: the capture window is frame-exact by design (run_exact
-- semantics — the 12 M budget and the 15,359-frame length are both
-- load-bearing, so neither becomes a condition wait).
local function row_se30_chime()
    machine.boot{model = "se30", ram = 8192, rom = vars.ROM}
    machine.sound.capture.start()
    scheduler.run(12000000)
    machine.sound.capture.stop()
    assert(machine.sound.capture.frames == 15367, F"se30-chime: capture length ${machine.sound.capture.frames}, want 15367")
    machine.sound.match("goldens/se30-chime.wav")
    mac.report_perf("suite-se30/se30-chime", scheduler.instr_count)
end

-- ---- se30-608-fd ------------------------------------------------------------
-- The direct-SWIM transport row (800K GCR straight off the SWIM, no IOP
-- in the path) — moved here from boot-matrix, which keeps only its Plus
-- and IIcx rows. The Universal ROM's ~13 s drive-discovery wait is skipped
-- by the machine's own PRAM (no KeyMap poke any more).
local function row_se30_608_fd()
    machine.boot{model = "se30", ram = 2048, rom = vars.ROM}
    machine.floppy.drive[0].insert(ssw_608)
    local t0 = scheduler.host_user_ns
    scheduler.run(40000000)
    local spent = mac.wait_match("goldens/se30-6.0.8-512x342-finder.png", 400000000, 10000000)
    assert(machine.ram == 2048, "se30-608-fd: machine.ram != 2048")
    assert(machine.screen.width == 512, "se30-608-fd: width != 512")
    mac.assert_addr(false)
    mac.mips_floor("se30-608-fd", t0, 4)
    mac.report_row("6.0.8", "builtin", "fd-boot swim-direct 800k-gcr")
    mac.report_perf("suite-se30/se30-608-fd", scheduler.instr_count)
end

-- ---- se30-71-fd -------------------------------------------------------------
-- NEW CELL SE/30 x 7.1 and the 1.44 M MFM transport row: se30-floppy-hd
-- re-media'd from 7.0.1 to the 1.4 M "Disk Tools" floppy, which is
-- genuinely 7.1 (the rule: a 7.1 row never uses an 800K "7.1" disk).
-- The About box confirms the System version and the 4 MB total.
local function row_se30_71_fd()
    machine.boot{model = "se30", ram = 4096, rom = vars.ROM}
    machine.rtc.time = 3000000000
    machine.floppy.drive[0].insert(disk_tools_71)
    scheduler.run(40000000)
    scheduler.run(600000000)
    -- wait_stable (not wait_match) because this row interacts afterwards:
    -- the settle point is identical in capture and verify runs, so the
    -- About choreography lands the same way in both.
    mac.wait_stable(2500000000)
    mac.check("goldens/se30-7.1-512x342-finder.png")
    assert(machine.ram == 4096, "se30-71-fd: machine.ram != 4096")
    mac.assert_addr(false)
    mac.about_box()
    mac.check("goldens/se30-7.1-512x342-about.png")
    mac.report_row("7.1", "builtin", "fd-boot swim-direct 1.44m-mfm about")
    mac.report_perf("suite-se30/se30-71-fd", scheduler.instr_count)
end

-- ---- se30-701-gsvrom --------------------------------------------------------
-- The built-in video's substitute ROM, the runtime-generated GS vROM, drives
-- it: the boot reaches the Finder, off the generated declaration ROM.  (The
-- other rows run Apple's SE/30 video ROM, which the test data offers.)
local function row_se30_701_gsvrom()
    machine.boot{model = "se30", ram = 8192, rom = vars.ROM, slots = "14=builtin_se30_video,rom=substitute"}
    machine.rtc.time = 3000000000
    machine.floppy.drive[0].insert(sys_701)
    scheduler.run(50000000)
    -- golden-collision-ok: se30-7.0.1-512x342-gsvrom-finder.png se30-7.1-512x342-finder.png
    -- - System_7_0_1.image and the 7.1 Disk Tools disk are both volumes named
    -- "Disk Tools", and at 512x342 mono the 7.0.1 and 7.1 Finders draw an
    -- identical desktop. The version is not what either golden proves: se30-71-fd
    -- pins 7.1 through its About-box golden, and this row's own claim is the
    -- substitute ROM, asserted below via the card's declaration ROM.
    local spent = mac.wait_match("goldens/se30-7.0.1-512x342-gsvrom-finder.png", 600000000, 10000000)
    assert(machine.id == "se30", "se30-701-gsvrom: machine.id != se30")
    assert(machine.ram == 8192, "se30-701-gsvrom: machine.ram != 8192")
    assert(machine.nubus.slot[14].card.declrom.path == "builtin:builtin_se30_video", "se30-701-gsvrom: not the generated vROM")
    mac.assert_addr(false)
    mac.report_row("7.0.1", "gsvrom", "fd-boot substitute-vrom")
    mac.report_perf("suite-se30/se30-701-gsvrom", scheduler.instr_count)
end

-- ---- se30-75-hd -------------------------------------------------------------
-- NEW CELL SE/30 x 7.5, and the SE/30's SCSI-HD boot path (se30-scsi
-- deleted — same path, fresher media). 16 MB exercises a RAM total the
-- SE/30 can only see in 32-bit mode; the prepared image carries MODE32,
-- but under default PRAM it still boots 24-bit (measured), so the
-- addressing assert records reality and MemTop is capped at 8 MB.
local function row_se30_75_hd()
    machine.boot{model = "se30", ram = 16384, rom = vars.ROM}
    machine.rtc.time = 3000000000
    machine.scsi.attach_hd(hd_75, 0)
    scheduler.run(300000000)
    scheduler.run(700000000)
    -- 7.5 puts a clock in the menu bar, so the golden excludes that
    -- corner (the RTC is pinned above, but the displayed minute still
    -- depends on where in guest time the frame lands).
    mac.wait_stable(2500000000)
    assert(machine.id == "se30", "se30-75-hd: machine.id != se30")
    assert(machine.ram == 16384, "se30-75-hd: machine.ram != 16384")
    mac.assert_addr(false)
    mac.check_ex("goldens/se30-7.5-512x342-finder.png", 0, 380, 20, 512)
    mac.report_row("7.5", "builtin", "hd-boot scsi")
    mac.report_perf("suite-se30/se30-75-hd", scheduler.instr_count)
end

-- ---- se30-75-hd-addr32 ------------------------------------------------------
-- The 7.5 boot with the configuration's addressing option at 32-bit (MODE32
-- on the image).  In 32-bit mode the Slot Manager puts the frame buffer at the
-- slot-$E base -- the video sResource's MinorBaseOS is 0, so ScrnBase is
-- $FE008040, where in 24-bit mode it is $FEE08040.  The VRAM used to decode
-- only at $FEE00000, so the machine booted to a Finder no one could see: a
-- gray screen to the end.
local function row_se30_75_hd_addr32()
    machine.boot{model = "se30", ram = 16384, rom = vars.ROM, config = "{\"options\":{\"addressing\":\"32\"}}"}
    machine.rtc.time = 3000000000
    machine.scsi.attach_hd(hd_75, 0)
    scheduler.run(1000000000)
    mac.wait_stable(2500000000)
    assert(machine.ram == 16384, "se30-75-hd-addr32: machine.ram != 16384")
    assert(debug.mac.globals.read("ScrnBase") == 0xFE008040, "se30-75-hd-addr32: ScrnBase is not the 32-bit slot-$E base")
    mac.assert_addr(true)
    mac.check_ex("goldens/se30-7.5-512x342-finder.png", 0, 380, 20, 512)
    mac.report_row("7.5", "builtin", "hd-boot scsi addr32")
    mac.report_perf("suite-se30/se30-75-hd-addr32", scheduler.instr_count)
end

-- ---- se30-71-hd-addr32 -----------------------------------------------------
-- 7.1 with the 32-bit enabler, 32-bit addressing on.  The ROM boots 24-bit;
-- the enabler closes the video driver (which leaves VIA1 PB6, vSyncEnA, high)
-- and restarts the machine in 32-bit mode.  The slot-$E vSync request used to
-- be raised every VBL whatever PB6 said, so one landing in that window found
-- no handler in SlotQDT: SysError 51 and a Sad Mac 0000000F/00000033.  The
-- window is timing-sensitive -- any logpoint or breakpoint single-steps the
-- run and dodges it -- so this row deliberately runs without one.
local function row_se30_71_hd_addr32()
    machine.boot{model = "se30", ram = 8192, rom = vars.ROM, config = "{\"options\":{\"addressing\":\"32\"}}"}
    machine.rtc.time = 3000000000
    machine.scsi.attach_hd(hd_71, 0)
    scheduler.run(600000000)
    mac.wait_stable(2500000000)
    assert(machine.ram == 8192, "se30-71-hd-addr32: machine.ram != 8192")
    mac.assert_addr(true)
    mac.check("goldens/se30-7.1-512x342-hd-addr32-finder.png")
    mac.report_row("7.1", "builtin", "hd-boot scsi addr32 enabler")
    mac.report_perf("suite-se30/se30-71-hd-addr32", scheduler.instr_count)
end

-- ---- se30-checkpoint --------------------------------------------------------
-- The GLUE device set's save/restore round-trip, in process — boot, save, wipe
-- with a fresh boot, load, re-match.
local function row_se30_checkpoint()
    machine.boot{model = "se30", ram = 2048, rom = vars.ROM}
    machine.floppy.drive[0].insert(ssw_608)
    scheduler.run(40000000)
    mac.wait_match("goldens/se30-6.0.8-512x342-finder.png", 400000000, 10000000)
    assert(checkpoint.save(F"${vars.WORK_DIR}/suite-se30.gsc"), "se30-checkpoint: save failed")
    machine.boot{model = "se30", ram = 1024, rom = vars.ROM}
    assert(checkpoint.load(F"${vars.WORK_DIR}/suite-se30.gsc"), "se30-checkpoint: load failed")
    scheduler.run(5000000)
    assert(machine.ram == 2048, "se30-checkpoint: restored RAM size wrong")
    mac.check("goldens/se30-6.0.8-512x342-finder.png")
    mac.report_perf("suite-se30/se30-checkpoint", scheduler.instr_count)
end

-- ---- row dispatch -----------------------------------------------------------

mac.row("se30-nomedia", row_se30_nomedia)
mac.row("se30-chime", row_se30_chime)
mac.row("se30-608-fd", row_se30_608_fd)
mac.row("se30-71-fd", row_se30_71_fd)
mac.row("se30-701-gsvrom", row_se30_701_gsvrom)
if mac.row_on("se30-75-hd") then
    if mac.have_media(hd_75) then
        mac.row("se30-75-hd", row_se30_75_hd)
    else
        echo("skip: se30-75-hd (7.5 prepared image not fetched)")
    end
end
if mac.row_on("se30-75-hd-addr32") then
    if mac.have_media(hd_75) then
        mac.row("se30-75-hd-addr32", row_se30_75_hd_addr32)
    else
        echo("skip: se30-75-hd-addr32 (7.5 prepared image not fetched)")
    end
end
if mac.row_on("se30-71-hd-addr32") then
    if mac.have_media(hd_71) then
        mac.row("se30-71-hd-addr32", row_se30_71_hd_addr32)
    else
        echo("skip: se30-71-hd-addr32 (7.1 prepared image not fetched)")
    end
end
mac.row("se30-checkpoint", row_se30_checkpoint)

mac.suite_done()
quit()
