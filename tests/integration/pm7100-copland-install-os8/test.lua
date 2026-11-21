-- Converted from test.script by scripts/script-to-lua.py
-- Integration test: Power Macintosh 7100 — install Mac OS 8 D11E4 (Copland).
--
-- Phase 4-5 of the Copland install.  See config.mk for the phase map.
--
-- MEDIA: not redistributable and not part of the test data.  Everything below
-- `copland_root` is read from $GS_EXTRA_MEDIA_DIR/copland/, and the test skips
-- cleanly without it.
--
-- The bus:
--
--   SCSI 0   system_7_5_0_77mb_mode32_24ac.img    the 7.5 host we boot from
--   SCSI 1   copland_target_75_230mb.img          the target: formatted by
--                                                 Drive Setup 2.0d5c2 and
--                                                 carrying System 7.5.0
--   SCSI 3   the Mac OS 8 DDK 0.4 CD              `Install Mac OS` lives here

local mac = dofile("../lib/mac.lua")

local copland_root = F"${vars.EXTRA_MEDIA}/copland"
local media = F"${copland_root}/media"
local ddk_cd = F"${copland_root}/images/copland_d11e4_jun_1996_mac_os_8_ddk_0.4_original_cd_image.dmg"
local target = F"${media}/copland_target_75_230mb.img"
local host_hd = "../../data/systems/system_7_5_0_77mb_mode32_24ac.img"

local function boot_host()
    -- Clock AFTER boot, ascending base (see the format test's note): this is
    -- the last phase, so 0xE0100000 — above everything the format and 7.5
    -- phases stamped, so files those phases created and this one modifies keep
    -- crDat < mdDat (the condition Copland's ValidCatalogEntry enforces).
    machine.boot{model = "pm7100", ram = 24576, rom = vars.ROM}
    machine.rtc.time = 0xE0100000
    machine.scsi.attach_hd(host_hd, 0)
    machine.scsi.attach_hd(target, 1)
    machine.scsi.attach_cdrom(ddk_cd, 3)

    -- suite-pdm's boot_pdm_75() recipe: chip init and bus scan (no spin-up
    -- wait: the machine's PRAM disables it), then boot blocks to the Finder.
    scheduler.run(360000000)
    scheduler.run(1200000000)
    mac.wait_stable(900000000)
    assert(machine.cpu.mmu.peek(0xAF0, 2) == 0, F"system error ${machine.cpu.mmu.peek(0xAF0, 2)} during boot")
    mac.check_ex("host-desktop.png", 0, 440, 20, 640)
end

local function open_installer()
    -- The CD window auto-opens; `Mac OS 8 Runtime SW` then the D11E4 folder.
    mac.double_click(152, 193)
    mac.wait_stable(600000000)
    mac.check_ex("cd-runtime-sw.png", 0, 440, 20, 640)

    mac.double_click(275, 170)
    mac.wait_stable(600000000)
    mac.check_ex("cd-d11e4-folder.png", 0, 440, 20, 640)

    -- `Install Mac OS` is a 'kajr' installation SCRIPT, not an application —
    -- double-clicking it launches the `Installer` sitting beside it in the same
    -- folder, with this script's rules loaded.
    mac.double_click(87, 137)
    scheduler.run(800000000)
    mac.wait_stable(3000000000)
    -- It comes up on the volume it booted from, `Macintosh HD` — 77 MB — and
    -- rule 205's text fires verbatim: "must choose a disk with a capacity of at
    -- least 230 megabytes".  Two Switch Disk clicks from here, the same trap and
    -- the same fix as the 7.5 install before it, except that this Installer
    -- refuses rather than silently doing the wrong thing.
    mac.check_ex("installer-too-small.png", 0, 440, 20, 640)

    -- First click lands on the CD, which is locked and says so.
    mac.click(307, 355)
    mac.wait_stable(1000000000)
    mac.check_ex("installer-locked-cd.png", 0, 440, 20, 640)
    -- The intervening move is required, not cosmetic: a same-spot warp emits no
    -- ADB motion, so a second click at the same pixel has no fresh position
    -- event to bind to and is dropped (../lib/mac.script, click()).
    machine.adb.mouse.move(420, 290, "global")
    mac.run_ticks(10)
    mac.click(307, 355)
    mac.wait_stable(1000000000)
    -- `untitled`, and every rule passes: the panel switches to "Click Install to
    -- install the Mac OS 8 Developer Release: Compatibility Edition" and the
    -- Install button goes live.  That sentence is rule 206's package set, and
    -- reaching it means rules 200 (a NuBus PDM), 202 (16 MB), 204 (230 MB),
    -- **207 (the SecondaryLoader partition)** and 210 (System 7.5 on the target)
    -- all passed — which is the whole reason the two tests before this one
    -- exist.  Rule 207 is the strictest: it wants the loader partition on the
    -- disk itself, not just files on a volume.
    mac.check_ex("installer-ready.png", 0, 440, 20, 640)
end

-- Phase 4 — the install itself.
local function install_os8()
    mac.click(471, 359)
    -- Measured: the install runs between 4 and 5 G instructions from the click,
    -- off the CD, with no disk swaps and no dialog to dismiss on the way.  A
    -- fixed budget past the end and then a settle, rather than a bare stability
    -- wait: the "Reading from:" progress panel holds still for long stretches
    -- while it copies, and a stability wait returns on one of those frames.
    scheduler.run(6000000000)
    mac.wait_stable(4000000000)
    -- Always leave the frame behind, so a golden mismatch is diagnosable from
    -- the run's artifacts rather than needing a re-run.
    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/tail.png") end, nil)
    -- The Installer's own verdict.  Unlike 7.5's, it reports directly, with no
    -- advisory in between and no request to re-feed any media.
    mac.check_ex("installed.png", 0, 440, 20, 640)

    -- Quit rather than Continue: it leaves the Installer, which closes the
    -- target volume rather than leaving it open for another pass.
    mac.click(452, 303)
    mac.wait_stable(2000000000)
    mac.check_ex("installer-quit.png", 0, 440, 20, 640)
    mac.report_perf("pm7100-copland-install-os8/install", scheduler.instr_count)
end

-- Phase 5 — prove Copland landed, and publish the volume for the boot test.
local function export_volume()
    machine.scsi.device[1].image.export(F"${vars.WORK_DIR}/copland-installed.img")
    local root = files.list(F"${vars.WORK_DIR}/copland-installed.img/partition6")
    echo(F"target root: ${root}")
    local mos = files.list(F"${vars.WORK_DIR}/copland-installed.img/partition6/Mac OS Folder")
    echo(F"Mac OS Folder (${#mos} items): ${mos}")
    local sysf = files.list(F"${vars.WORK_DIR}/copland-installed.img/partition6/System Folder")
    echo(F"System Folder (${#sysf} items): ${sysf}")

    -- Copland installs BESIDE the 7.5 System, it does not replace it: the
    -- machine boots 7.5 normally and Copland only on a Caps Lock latch.  So
    -- both folders must be here — one missing means the wrong thing happened,
    -- in either direction.
    assert(files.path_exists(F"${vars.WORK_DIR}/copland-installed.img/partition6/Mac OS Folder"), "no Mac OS Folder — Copland did not land on the target volume")
    assert(files.path_exists(F"${vars.WORK_DIR}/copland-installed.img/partition6/System Folder/System"), "the 7.5 System is gone — Copland should install alongside it, not over it")
    assert(files.path_exists(F"${vars.WORK_DIR}/copland-installed.img/partition6/System 7.5 --Read Me"), "the 7.5 install this started from is not intact")

    -- Name what has to be there rather than counting entries — a count is both
    -- weaker evidence and more brittle (the lesson pm7100-copland-install paid
    -- for).  `Mac OS Loader` is the one that matters most: it is the file the
    -- Apple_MacOSPrep secondary loader partition chains to, so a volume without
    -- it cannot start Copland however complete the rest looks.  `System
    -- Libraries` and `Hardware Support` are the microkernel's own trees, and
    -- `Mac OS 8 Applications` / `Apple Extras` come from the other two packages
    -- rule 206 adds, so between them they cover all three.
    assert(files.path_exists(F"${vars.WORK_DIR}/copland-installed.img/partition6/Mac OS Folder/Mac OS Loader"), "no Mac OS Loader — the secondary loader partition would have nothing to chain to")
    assert(files.path_exists(F"${vars.WORK_DIR}/copland-installed.img/partition6/Mac OS Folder/System"), "no Mac OS 8 System file")
    assert(files.path_exists(F"${vars.WORK_DIR}/copland-installed.img/partition6/Mac OS Folder/System Libraries"), "no System Libraries — the microkernel tree is missing")
    assert(files.path_exists(F"${vars.WORK_DIR}/copland-installed.img/partition6/Mac OS Folder/Hardware Support"), "no Hardware Support — the microkernel tree is incomplete")
    assert(files.path_exists(F"${vars.WORK_DIR}/copland-installed.img/partition6/Mac OS 8 Applications"), "no Mac OS 8 Applications — a package the installer adds did not run")
    assert(files.path_exists(F"${vars.WORK_DIR}/copland-installed.img/partition6/Apple Extras"), "no Apple Extras — a package the installer adds did not run")

    -- The loader partition Drive Setup 2.0d5c2 wrote must still be there: it is
    -- what rule 207 checked on the way in, and what the ROM's secondary loader
    -- will need on the way out.
    files.partmap(F"${vars.WORK_DIR}/copland-installed.img")
    assert(#files.list(F"${vars.WORK_DIR}/copland-installed.img") == 7, "the install disturbed the partition map")

    files.cp(F"${vars.WORK_DIR}/copland-installed.img", tostring(media) .. "/copland_d11e4_installed_230mb.img")
    assert(files.path_exists(tostring(media) .. "/copland_d11e4_installed_230mb.img"), "the installed volume was not published to media/")
end

if mac.have_media(target) and mac.have_media(ddk_cd) and mac.have_media(host_hd) then
    boot_host()
    open_installer()
    install_os8()
    export_volume()
    mac.report_row("7.5+D11E4", "ariel", "copland-install-phase4 os8-ddk-cd-install")
else
    mac.skip_test("pm7100-copland-install-os8 (Copland media not present — set GS_EXTRA_MEDIA_DIR)")
end

quit()
