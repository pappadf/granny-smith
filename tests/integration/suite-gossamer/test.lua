-- Converted from test.script by scripts/script-to-lua.py
-- Beige G3 family suite — Mac OS 9.2.1 from a MESH disk.
--
-- Row map:
--
--   g3dt-hd   pmg3dt (266 MHz 750, 512 KB L2) x 9.2.1 on the MESH bus: the
--             boot chain to the desktop and the Mac OS Setup Assistant.
--   g3dt-floppy  the g3dt-hd desktop, a 1.44 MB disk inserted in the
--             SuperDrive: SWIM3 MFM reads through DBDMA channel 1 under
--             Mac OS 9's floppy driver, and File Exchange mounts the FAT12
--             volume on the desktop.
--   g3mt-hd   pmg3mt (300 MHz 750, 1 MB L2): the same disk, the same chain.
--
-- The disk is gs-test-data `systems/mac_os_9_2_1_516mb_g3.img`, installed
-- from the retail CD by `g3-install-scsi` and never booted; every writable
-- attach mints its own delta, so each row's first boot is the image's first.
--
-- The chain: Open Firmware probes MESH and finds the Apple_Driver43 disk,
-- the 68k Start Manager loads its driver, the 9.2.1 System runs its
-- extensions — Open Transport, the ATI Rage Pro accelerator, QuickTime —
-- with virtual memory on (the first boot creates VM Storage and pages
-- against it), the Finder comes up and the Setup Assistant opens.

local mac = dofile("../lib/mac.lua")

local hd = "../../data/systems/mac_os_9_2_1_516mb_g3.img"

local function g3_hd_boot(row, model)
    machine.boot{model = model, ram = 65536, rom = vars.ROM}
    machine.rtc.time = "2026-10-01T12:00:00"
    machine.scsi.attach_hd(hd, 0)
    -- 6 G in, mid-boot, the g3dt row saves the fixture gossamer-checkpoint
    -- restores in a fresh process (the state it would reach itself).
    scheduler.run(6000000000)
    if row == "g3dt-hd" then fixture_save("g3dt-921-midboot") end
    -- Up to 6 G more: the row's frame (the desktop with the Setup Assistant
    -- open, outside the menu clock) and the startup sound, sampled every
    -- 250 M.  REGEN runs all 12 G, where the goldens were captured.
    if mac.REGEN_MODE == "1" then
        scheduler.run(6000000000)
    else
        local spent = 6000000000
        while spent < 12000000000 do
            scheduler.run(250000000)
            spent = spent + 250000000
            if machine.sound.frames > 110000 and machine.screen.matches(F"goldens/${row}.png", 0, 440, 20, 640) then break end
        end
    end

    -- The disk is the startup volume, and startup finished without a
    -- system error.
    assert(debug.mac.globals.read("BootDrive") == 0x8023, F"${row}: BootDrive ${debug.mac.globals.read(\"BootDrive\"):04x}, not the SCSI volume")
    assert(debug.mac.globals.read("DSErrCode") == 0, F"${row}: system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")

    -- The startup sound played past the ROM's boot beep (101,080 frames).
    echo(F"${row}: sound frames=${machine.sound.frames} peak=${machine.sound.peak}")
    assert(machine.sound.frames > 110000, F"${row}: no startup sound after the boot beep (frames ${machine.sound.frames})")

    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/${row}.png") end, nil)
    check_ex(F"goldens/${row}.png", 0, 440, 20, 640)
    report_perf(F"suite-gossamer/${row}", scheduler.instr_count)
end

g3_hd_boot("g3dt-hd", "pmg3dt")

-- --- g3dt-floppy -------------------------------------------------------------
assert(machine.floppy.drive[0].insert("../../data/systems/ans_nt_boot_floppy_daea7d6.img", false), "g3dt-floppy: insert the 1.44 MB disk")
run_until_match_ex("goldens/g3dt-floppy.png", 0, 440, 20, 640, 3000000000, 250000000)
gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/g3dt-floppy.png") end, nil)
check_ex("goldens/g3dt-floppy.png", 0, 440, 20, 640)
report_perf("suite-gossamer/g3dt-floppy", scheduler.instr_count)

g3_hd_boot("g3mt-hd", "pmg3mt")
echo("suite-gossamer: both models boot Mac OS 9.2.1 to the desktop")
quit()
