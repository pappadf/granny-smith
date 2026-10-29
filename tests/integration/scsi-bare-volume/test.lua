-- Converted from test.script by scripts/script-to-lua.py
local scsi_identify_ok
-- SCSI bare-volume wrapper
--
-- A bare HFS volume attached with scsi.attach_hd is presented behind a
-- synthesised Driver Descriptor Map, a three-entry Apple Partition Map and
-- the GSDisk driver (docs/core/storage/bare-volume-wrapper.md).  Nothing is
-- written into the file: the prefix lives in memory, guest writes land in the
-- image's delta as for any disk.
--
-- Rows:
--   wrap-layout     the device the guest sees, before anything boots
--   iici-753        IIci boots System 7.5.3 off the bare volume to the Finder
--   se30-753        the same volume on the SE/30 ROM
--   q700-753        ... and the Quadra 700 ROM
--   plus-701        the Plus ROM (no partition-map check, 68000 CPU) with the
--                   Plus-only minimal System 7.0.1 volume
--   iifx-753        the IIfx (OSS family; SCSI through its DMA controller)
--   q840av-71       the Quadra 840AV (AV family; the SCSI Manager 4.3 ROM) with
--                   a bare copy of the AV suite's System 7.1 volume
--   pm6100-753      the Power Mac 6100 (PDM family): the ROM runs the 68k
--                   driver under its emulator, as it does Apple's
--   pm7500-753      the Power Mac 7500 (TNT family)
--
-- One machine from every family that boots Mac OS from SCSI.  The Lisa/MacXL
-- (ProFile, no SCSI) and the Network Servers (no Mac OS) are out of scope.
-- System 7.5.3 on the 840AV stops with "illegal instruction" behind Apple's
-- own driver as well, so that row boots 7.1 instead.
--   two-volumes     two wrapped disks on one bus: two driver copies, two drives
--   plus-608-fd     the Plus boots System 6.0.8 from floppy with the bare 7.0.1
--                   volume on the bus: the driver's accRun announces it and it
--                   mounts beside the floppy
--   checkpoint      a checkpoint of a machine running off a wrapped disk
--                   restores to the same running desktop
--   driverless      a partitioned disk with no driver (Disk Copy / SheepShaver
--                   shape): its Apple_HFS partition is wrapped in place of the
--                   disk's own map, boots the IIci to the Finder, and survives
--                   a checkpoint round trip
--   trimmed         the 7.5.3 volume cut off after its last allocation block
--                   in use (as some archive.org images are): opened at the
--                   length its MDB claims, it boots the IIci to the Finder

local mac = dofile("../lib/mac.lua")

local vol753 = "../../data/systems/system_7_5_3_25mb_bare.img"
local vol701 = "../../data/systems/system_7_0_1_10mb_bare_plus.img"
local ROM_SE30 = F"${vars.TEST_DATA}/roms/iix-iicx-se30-97221136.rom"
local ROM_Q700 = F"${vars.TEST_DATA}/roms/q700-q900-420dbff3.rom"
local ROM_PLUS = F"${vars.TEST_DATA}/roms/plus-v3-4d1f8172.rom"
local ROM_IIFX = F"${vars.TEST_DATA}/roms/iifx-4147dd77.rom"
local ROM_AV = F"${vars.TEST_DATA}/roms/q840av-q660av-5bf10fd1.rom"
local ROM_PDM = F"${vars.TEST_DATA}/roms/pm6100-pm7100-pm8100-9feb69b3.rom"
local ROM_TNT = F"${vars.TEST_DATA}/roms/pm7500-pm8500-pm9500-96cd923d.rom"
-- The AV suite's Apple-partitioned System 7.1 disk, its HFS partition cut out
-- as a bare volume by config.mk's TEST_SETUP (scripts/apm-extract-hfs.py).
local volav71 = F"${vars.WORK_DIR}/av71_bare.img"
-- The 7.5.3 volume behind a driverless partition map (DDM naming no drivers,
-- Apple_HFS at block 64, a 16-block Apple_Free tail), built by config.mk's
-- TEST_SETUP (scripts/hfs-to-driverless-apm.py).
local voldl753 = F"${vars.WORK_DIR}/driverless753.img"
-- The 7.5.3 volume cut short of its MDB's claim, built by config.mk's
-- TEST_SETUP (scripts/hfs-trim.py).
local voltrim753 = F"${vars.WORK_DIR}/trimmed753.img"

-- Bytes the wrapper puts in front of a volume: DDM + 63-block map + 32-block
-- driver partition (IMAGE_WRAP_PREFIX_BLOCKS).
local PREFIX_BYTES = 96 * 512

-- The files.images entry whose file is `path` (by its base name).
local function image_index(path)
    local i = 0
    while i < files.images.count do
        if files.images[i].filename == path then return i end
        i = i + 1
    end
    error(F"no attached image ${path}")
end

-- The facts every boot row asserts once the Finder is up: the Start
-- Manager booted a drive, a System file is open, and the Finder launched.
local function assert_desktop(label)
    assert(debug.mac.globals.read("BootDrive") ~= 0, F"${label}: BootDrive is 0 — nothing booted")
    assert(debug.mac.globals.read("SysMap") ~= 0, F"${label}: SysMap unset — no System file opened")
    assert(debug.mac.globals.read("ApplZone") ~= debug.mac.globals.read("SysZone"), F"${label}: the Finder never launched")
end

-- ---- wrap-layout -------------------------------------------------------------
-- What the guest is offered: a hard disk the size of the volume plus the
-- prefix, while the file on the host stays exactly as it was.  The same file
-- attached as a CD-ROM is NOT wrapped (the CD driver reads bare HFS itself).
local function row_wrap_layout()
    machine.boot{model = "iici", ram = 8192, rom = vars.ROM}
    local file_size = files.path_size(vol753)
    assert(scsi_identify_ok(), "wrap-layout: identify_hd refused the bare volume")
    assert(machine.scsi.attach_hd(vol753, 0), "wrap-layout: attach_hd failed")
    assert(machine.scsi.device[0].type == "hd", "wrap-layout: SCSI 0 is not a hard disk")
    local i = image_index(vol753)
    assert(files.images[i].raw_size == file_size + PREFIX_BYTES, "wrap-layout: disk is " .. tostring(files.images[i].raw_size) .. " bytes, want " .. tostring(file_size) .. " + " .. tostring(PREFIX_BYTES))
    assert(files.path_size(vol753) == file_size, "wrap-layout: the volume file changed size")
    assert(machine.scsi.attach_cdrom(vol753, 3), "wrap-layout: attach_cdrom failed")
    assert(machine.scsi.device[3].type == "cdrom", "wrap-layout: SCSI 3 is not a CD-ROM")
end

function scsi_identify_ok()
    return machine.scsi.identify_hd(vol753)
end

-- ---- boot rows ------------------------------------------------------------------
-- Match the Finder desktop in `golden`, excluding the right-hand end of the
-- menu bar (System 7.5's clock) on whatever width the screen has.
local function check_desktop(golden)
    local w = machine.screen.width
    mac.check_ex(golden, 0, w - 240, 20, w)
end

-- Boot `vol` at SCSI 0 on (model, rom, ram) and match the Finder desktop.
-- SCSI 0 is named the startup device in PRAM, as the web UI does (the PDM
-- ROM waits for a default device otherwise).  `warm` instructions run before
-- the settle wait, so a long quiet stretch mid-boot (a PowerPC ROM, the
-- extensions parade) cannot pass for the desktop.
local function boot_desktop(label, model, rom, ram, vol, golden, warm)
    machine.boot{model = model, ram = ram, rom = rom}
    -- A fixed clock: the RTC otherwise starts at the host's wall time, and a
    -- menu-bar clock turning over during the settle wait costs a random
    -- extra 60M instructions (perf gate) on some runs and not others.
    machine.rtc.time = "2026-04-26T21:30:00"
    assert(machine.scsi.attach_hd(vol, 0), F"${label}: attach_hd failed")
    machine.rtc.pram.boot_device = 0
    scheduler.run(warm)
    mac.wait_stable(1600000000)
    assert_desktop(label)
    local i = image_index(vol)
    -- The volume was read through the driver; the Finder also writes to it
    -- (desktop database, volume info) — through the driver into the delta.
    assert(files.images[i].reads > 0, F"${label}: nothing read from the disk")
    assert(files.images[i].writes > 0, F"${label}: nothing written to the disk")
    check_desktop(golden)
    mac.report_perf(F"scsi-bare-volume/${label}", scheduler.instr_count)
end

local function row_iici_753()
    boot_desktop("iici-753", "iici", vars.ROM, 8192, vol753, "goldens/iici-7.5.3-640x480x1-bare-finder.png", 150000000)
end

local function row_se30_753()
    boot_desktop("se30-753", "se30", ROM_SE30, 8192, vol753, "goldens/se30-7.5.3-512x342x1-bare-finder.png", 150000000)
end

-- golden-collision-ok: goldens/iici-7.5.3-640x480x1-bare-finder.png goldens/q700-7.5.3-640x480x1-bare-finder.png - the same System on the same 640x480x1 screen draws the same desktop; each row boots its own model from its own ROM, and assert_desktop proves that machine reached the Finder
local function row_q700_753()
    boot_desktop("q700-753", "q700", ROM_Q700, 8192, vol753, "goldens/q700-7.5.3-640x480x1-bare-finder.png", 150000000)
end

local function row_plus_701()
    boot_desktop("plus-701", "plus", ROM_PLUS, 4096, vol701, "goldens/plus-7.0.1-512x342x1-bare-finder.png", 150000000)
end

local function row_iifx_753()
    boot_desktop("iifx-753", "iifx", ROM_IIFX, 8192, vol753, "goldens/iifx-7.5.3-640x480x1-bare-finder.png", 400000000)
end

local function row_q840av_71()
    boot_desktop("q840av-71", "q840av", ROM_AV, 16384, volav71, "goldens/q840av-7.1-640x480x8-bare-finder.png", 600000000)
end

local function row_pm6100_753()
    boot_desktop("pm6100-753", "pm6100", ROM_PDM, 24576, vol753, "goldens/pm6100-7.5.3-640x480x8-bare-finder.png", 1500000000)
end

local function row_pm7500_753()
    boot_desktop("pm7500-753", "pm7500", ROM_TNT, 16384, vol753, "goldens/pm7500-7.5.3-640x480x8-bare-finder.png", 1500000000)
end

-- ---- two-volumes ------------------------------------------------------------------
-- Two wrapped disks: the ROM loads a driver copy from each (one per SCSI ID),
-- each copy installs its own DCE and drive.  One boots; the other is mounted
-- by the disk-inserted event its driver posts from accRun once the system is
-- up (an event posted during SCSILoad would not survive startup) — the golden
-- shows both volume icons.
local function row_two_volumes()
    machine.boot{model = "iici", ram = 8192, rom = vars.ROM}
    assert(machine.scsi.attach_hd(vol753, 0), "two-volumes: attach_hd 0 failed")
    assert(machine.scsi.attach_hd(vol701, 1), "two-volumes: attach_hd 1 failed")
    scheduler.run(150000000)
    mac.wait_stable(1600000000)
    assert_desktop("two-volumes")
    assert(files.images[image_index(vol701)].reads > 0, "two-volumes: the second volume was never read")
    check_desktop("goldens/iici-7.5.3-640x480x1-bare-two-volumes.png")
end

-- ---- plus-608-fd -------------------------------------------------------------------
-- The Plus boots the 6.0.8 floppy; the wrapped volume is not the startup disk,
-- so its driver posts the disk-inserted event from accRun once the system is
-- up, and the Finder mounts it.  That Control call's exit used to store the
-- result through A0 after the announce loop had left the event code (diskEvt,
-- 7) there: a word write to $17, an address error on the 68000 -- "Sorry, a
-- system error occurred. ID = 02" on every such boot.
local function row_plus_608_fd()
    machine.boot{model = "plus", ram = 4096, rom = ROM_PLUS}
    machine.rtc.time = "2026-04-26T21:30:00" -- a fixed clock, as boot_desktop
    machine.floppy.drive[0].insert(F"${vars.WORK_DIR}/fd608.dsk")
    assert(machine.scsi.attach_hd(vol701, 0), "plus-608-fd: attach_hd failed")
    scheduler.run(150000000)
    mac.wait_stable(1600000000)
    assert_desktop("plus-608-fd")
    assert(files.images[image_index(vol701)].reads > 0, "plus-608-fd: the volume was never read")
    check_desktop("goldens/plus-6.0.8-512x342x1-fd-bare-701-mounted.png")
end

-- ---- checkpoint -------------------------------------------------------------------
-- The prefix is not in the file, so a restore has to re-wrap the image
-- (IMAGE_CKPT_WRAPPED); a restored machine that lost it would read the bare
-- volume at the wrong offset and crash.
local function row_checkpoint()
    machine.boot{model = "iici", ram = 8192, rom = vars.ROM}
    assert(machine.scsi.attach_hd(vol753, 0), "checkpoint: attach_hd failed")
    scheduler.run(150000000)
    mac.wait_stable(1600000000)
    assert(checkpoint.save(F"${vars.WORK_DIR}/bare-volume.checkpoint"), "checkpoint: save failed")
    assert(checkpoint.load(F"${vars.WORK_DIR}/bare-volume.checkpoint"), "checkpoint: load failed")
    local i = image_index(vol753)
    assert(files.images[i].raw_size == files.path_size(vol753) + PREFIX_BYTES, "checkpoint: restored disk is not wrapped")
    -- Keep running off the restored disk: the Finder must still be there.
    scheduler.run(200000000)
    assert_desktop("checkpoint")
    check_desktop("goldens/iici-7.5.3-640x480x1-bare-finder.png")
end

-- ---- driverless -------------------------------------------------------------------
-- The guest sees the prefix and the HFS partition only — the disk's own map
-- and its free tail are hidden — and the file is untouched.  A checkpoint
-- re-wraps it at the partition's offset: a restore that lost the offset would
-- read the map where the volume should be.
local function row_driverless()
    machine.boot{model = "iici", ram = 8192, rom = vars.ROM}
    machine.rtc.time = "2026-04-26T21:30:00" -- a fixed clock, as boot_desktop
    local file_size = files.path_size(voldl753)
    assert(machine.scsi.attach_hd(voldl753, 0), "driverless: attach_hd failed")
    local i = image_index(voldl753)
    assert(files.images[i].raw_size == files.path_size(vol753) + PREFIX_BYTES, "driverless: disk is " .. tostring(files.images[i].raw_size) .. " bytes, want the HFS partition + " .. tostring(PREFIX_BYTES))
    scheduler.run(150000000)
    mac.wait_stable(1600000000)
    assert_desktop("driverless")
    assert(files.images[i].writes > 0, "driverless: nothing written to the disk")
    check_desktop("goldens/iici-7.5.3-640x480x1-bare-finder.png")
    mac.report_perf("scsi-bare-volume/driverless", scheduler.instr_count)
    assert(checkpoint.save(F"${vars.WORK_DIR}/driverless.checkpoint"), "driverless: checkpoint save failed")
    assert(checkpoint.load(F"${vars.WORK_DIR}/driverless.checkpoint"), "driverless: checkpoint load failed")
    local j = image_index(voldl753)
    assert(files.images[j].raw_size == files.path_size(vol753) + PREFIX_BYTES, "driverless: restored disk is not wrapped")
    scheduler.run(200000000)
    assert_desktop("driverless (restored)")
    check_desktop("goldens/iici-7.5.3-640x480x1-bare-finder.png")
    assert(files.path_size(voldl753) == file_size, "driverless: the disk file changed size")
end

-- ---- trimmed ----------------------------------------------------------------------
-- The disk is as long as the volume says, not the file: the missing tail reads
-- as zeros with the alternate MDB in its place, and the file is untouched.
local function row_trimmed()
    machine.boot{model = "iici", ram = 8192, rom = vars.ROM}
    machine.rtc.time = "2026-04-26T21:30:00" -- a fixed clock, as boot_desktop
    local file_size = files.path_size(voltrim753)
    assert(file_size < files.path_size(vol753), "trimmed: the volume was not trimmed")
    assert(machine.scsi.attach_hd(voltrim753, 0), "trimmed: attach_hd failed")
    local i = image_index(voltrim753)
    assert(files.images[i].raw_size == files.path_size(vol753) + PREFIX_BYTES, "trimmed: disk is " .. tostring(files.images[i].raw_size) .. " bytes, want the whole volume + " .. tostring(PREFIX_BYTES))
    scheduler.run(150000000)
    mac.wait_stable(1600000000)
    assert_desktop("trimmed")
    assert(files.images[i].writes > 0, "trimmed: nothing written to the disk")
    check_desktop("goldens/iici-7.5.3-640x480x1-bare-finder.png")
    assert(files.path_size(voltrim753) == file_size, "trimmed: the disk file changed size")
end

-- ---- row dispatch ------------------------------------------------------------------

if mac.have_media(vol753) and mac.have_media(vol701) then
    mac.row("wrap-layout", row_wrap_layout)
    mac.row("iici-753", row_iici_753)
    mac.row("se30-753", row_se30_753)
    mac.row("q700-753", row_q700_753)
    mac.row("plus-701", row_plus_701)
    mac.row("iifx-753", row_iifx_753)
    if mac.row_on("q840av-71") then
        if mac.have_media(volav71) then
            mac.row("q840av-71", row_q840av_71)
        else
            echo(F"skip: q840av-71 (no bare AV volume at ${volav71})")
        end
    end
    mac.row("pm6100-753", row_pm6100_753)
    mac.row("pm7500-753", row_pm7500_753)
    mac.row("two-volumes", row_two_volumes)
    if mac.row_on("plus-608-fd") then
        if mac.have_media(F"${vars.WORK_DIR}/fd608.dsk") then
            mac.row("plus-608-fd", row_plus_608_fd)
        else
            echo(F"skip: plus-608-fd (no 6.0.8 floppy at ${vars.WORK_DIR}/fd608.dsk)")
        end
    end
    mac.row("checkpoint", row_checkpoint)
    if mac.row_on("trimmed") then
        if mac.have_media(voltrim753) then
            mac.row("trimmed", row_trimmed)
        else
            echo(F"skip: trimmed (no trimmed volume at ${voltrim753})")
        end
    end
    if mac.row_on("driverless") then
        if mac.have_media(voldl753) then
            mac.row("driverless", row_driverless)
        else
            echo(F"skip: driverless (no driverless disk at ${voldl753})")
        end
    end
else
    mac.skip_test(F"scsi-bare-volume (no bare volumes at ${vol753} / ${vol701})")
end

mac.suite_done()
quit()
