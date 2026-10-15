-- Converted from test.script by scripts/script-to-lua.py
-- Mac OS X 10.0.3 -> Finder on a beige Power Macintosh G3, from ATA.
--
-- The disk is gs-test-data `systems/mac_os_x_10_0_3_2gb_g3_minimal.dmg`: a
-- 2 GB HFS+ volume with no Apple driver partition, so the emulator wraps it
-- with the GSDisk driver and the volume is partition 3 (`ide@20000/@0:3`,
-- the configuration's boot-device).  It is attached writable over a delta,
-- so the checked-in image is never written.
--
-- MEDIA-GATED on that disk: the row skips when the fetched test data
-- predates it (tests/data-version pins an earlier gs-test-data commit).
--
-- The chain: Open Firmware runs Apple's nvramrc patch and `0 bootr`, loads
-- BootX from the HFS+ startup file, BootX loads mach_kernel and the kexts
-- from /System/Library/Extensions (10.0 has no Extensions.mkext); the
-- kernel finds the root on HeathrowATA, the Rage Pro's ROM ndrv senses the
-- 17" multiple-scan display (Apple's extended code $0B) and SystemStarter
-- brings up NetInfo; loginwindow logs `mini` in automatically and the
-- Finder opens the volume's window at 1024 x 768, the mode the disk
-- remembers for that display.
--
-- The disk carries no menu-bar clock (it lived in /Applications), and the
-- clock is pinned anyway.

local mac = dofile("../lib/mac.lua")

local hd = "../../data/systems/mac_os_x_10_0_3_2gb_g3_minimal.dmg"

if have_media(hd) then
    machine.rtc.time = "2026-10-01T12:00:00"
    machine.attach_media("ata0", 0, "hd", hd)
    assert(machine.ata.devices == "hd - - -", "the disk is not ATA bus 0's master")
    assert(machine.pci.slot[4].card.monitor.id == "17in_multi", "the configuration's 17\" monitor is not strapped")

    -- Open Firmware, BootX, the kernel and its kexts, SystemStarter,
    -- loginwindow and the Finder: one 10 G run, then polls every 500 M up to
    -- 30 G.  The head start is one run in both modes because the boot is not
    -- indifferent to being sliced: polled every 500 M from power-on, the
    -- Finder's window comes up inactive.  Stay well inside 45 G, where the
    -- idle machine's Energy Saver puts the display to sleep.  The screen is
    -- Open Firmware's 640 x 480 until the ndrv sets the 17"'s 1024 x 768, and
    -- matching a golden of another size is an error, not a mismatch, so the
    -- match waits for the mode.
    scheduler.run(10000000000)
    if mac.REGEN_MODE == "1" then
        wait_stable(10000000000)
    else
        local spent = 10000000000
        local up = false
        if machine.screen.width == 1024 then up = machine.screen.matches("finder.png") end
        while spent < 30000000000 and not up do
            scheduler.run(500000000)
            spent = spent + 500000000
            if machine.screen.width == 1024 then up = machine.screen.matches("finder.png") end
        end
        echo(F"the Finder after ${spent} of scheduler time")
    end
    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/finder.png") end, nil)

    -- The ndrv drives the Rage Pro at 1024 x 768 and the Finder is up.
    assert(machine.pci.slot[4].card.monitor.sensed_extended == 0x0B, F"the ndrv sensed extended code ${machine.pci.slot[4].card.monitor.sensed_extended:02x}, not the 17\"'s $0B")
    check("finder.png")
    report_perf("g3-osx-10-0-finder/finder", scheduler.instr_count)
    echo("g3-osx-10-0-finder: Mac OS X 10.0.3 at the Finder")
else
    -- The perf gate reads this row's own skip line; skip_test marks the test.
    echo("skip: finder (the disk is not in this checkout's test data)")
    skip_test("g3-osx-10-0-finder (systems/mac_os_x_10_0_3_2gb_g3_minimal.dmg is not in the fetched test data -- see this row's header)")
end
quit()
