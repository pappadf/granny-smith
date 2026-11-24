-- Converted from test.script by scripts/script-to-lua.py
-- AppleShare from the beige G3: Chooser -> AppleShare over LocalTalk.
-- See config.mk.  Coordinates are global, on the 640x480 desktop.

local mac = dofile("../lib/mac.lua")

local hd = "../../data/systems/mac_os_9_2_1_516mb_g3.img"

if mac.have_media(hd) then
    machine.boot{model = "pmg3dt", ram = 65536, rom = vars.ROM}
    machine.rtc.time = "2026-10-01T12:00:00"
    machine.scsi.attach_hd(hd, 0)
    assert(appletalk.afp.volumes.add("Shared", F"${vars.WORK_DIR}/share").path == F"${vars.WORK_DIR}/share", "share not registered")
    -- The desktop with the Setup Assistant open (suite-gossamer's g3dt-hd).
    scheduler.run(24000000000)

    -- Apple menu -> Chooser
    machine.adb.mouse.move(300, 300, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.move(20, 8, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    scheduler.run(60000000)
    machine.adb.mouse.move(75, 86, "global")
    scheduler.run(20000000)
    machine.adb.mouse.click(false)
    scheduler.run(1200000000)

    -- AppleShare: the NBP lookup goes out by DBDMA, the reply comes back in
    machine.adb.mouse.move(148, 110, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    mac.run_until_match("server-listed.png", 6000000000, 100000000)
    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/server-listed.png") end, nil)
    mac.check("server-listed.png")
    assert(appletalk.stats.ddp_in > 0, "no DDP frame came from the guest")
    assert(appletalk.stats.llap_tx > 0, "the server answered nothing")

    -- Shared Folders -> OK -> Guest -> Connect: ASP over the same path, and
    -- the volume list
    machine.adb.mouse.move(383, 111, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    scheduler.run(40000000)
    machine.adb.mouse.move(510, 301, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    scheduler.run(3000000000)
    machine.adb.mouse.move(461, 310, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    mac.run_until_match("volume-list.png", 6000000000, 100000000)
    gs.try(function() return machine.screen.save(F"${mac.RESULTS_DIR}/volume-list.png") end, nil)
    mac.check("volume-list.png")
    assert(appletalk.afp.sessions.count == 1, "the G3 opened no ASP session")
else
    mac.skip_test(F"needs ${hd}")
end

quit()
