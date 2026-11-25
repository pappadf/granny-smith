-- AppleShare from the AV Quadras over EtherTalk (see config.mk).
-- Coordinates are global, on the 640x480 desktop.

local mac = dofile("../lib/mac.lua")

local av_rom = "../../data/roms/q840av-q660av-5bf10fd1.rom"
local av_hd = "../../data/systems/system_7_1_77mb_av.img"

-- Click at global (x, y) once the cursor is there.
local function gclick(x, y)
    machine.adb.mouse.move(x, y, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    mac.wait_mouse_taken(100000000)
end

-- Pull down the Apple menu and release on the item at global y.
local function apple_menu(y)
    machine.adb.mouse.move(300, 300, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.move(20, 8, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    scheduler.run(8000000)
    machine.adb.mouse.move(80, y, "global")
    scheduler.run(6000000)
    machine.adb.mouse.click(false)
end

-- Boot `model` from the 7.1 AV disk, switch the Network control panel from
-- LocalTalk to "EtherTalk Built In" -- which opens .ENET, whose three MACE
-- loopback self-tests (INTLPB, MENDECLPB, EXTLPB: a 79-byte frame out of
-- PSC channel 2, back 83 bytes through channel 1's chain buffers) must pass
-- or the panel falls back to LocalTalk -- and list the emulated server in
-- the Chooser's AppleShare panel, found by an NBP lookup over Ethernet.
local function av_ethertalk_chooser(model)
    machine.boot{model = model, ram = 16384, rom = av_rom}
    machine.rtc.time = 3000000000
    machine.scsi.attach_hd(av_hd, 0)
    mac.wait_desktop(1500000000)
    assert(machine.id == model, "booted the wrong model")
    -- The share outlives machine.boot: publish it once.
    if appletalk.afp.volumes.count == 0 then
        assert(appletalk.afp.volumes.add("Shared", F"${vars.WORK_DIR}/share").path == F"${vars.WORK_DIR}/share", "share not registered")
    end
    assert(appletalk.stats.elap_rx == 0, "EtherTalk traffic before EtherTalk was chosen")

    -- Control Panels -> Network -> EtherTalk Built In -> OK.
    apple_menu(150)
    scheduler.run(300000000)
    mac.double_click(167, 82)
    scheduler.run(300000000)
    gclick(208, 108)
    scheduler.run(100000000)
    gclick(322, 189)
    mac.run_until_match("network-ethertalk.png", 600000000, 10000000)
    mac.check("network-ethertalk.png")
    assert(appletalk.stats.elap_rx > 0, "the guest's EtherTalk sent nothing to the network")
    gclick(114, 51) -- close the Network panel
    scheduler.run(100000000)

    -- Chooser -> AppleShare: the server comes back over Ethernet.
    apple_menu(132)
    scheduler.run(150000000)
    gclick(148, 115)
    mac.run_until_match("server-listed.png", 400000000, 5000000)
    mac.check("server-listed.png")
    assert(appletalk.stats.elap_tx > 0, "the server answered nothing over Ethernet")
end

if mac.have_media(av_hd) then
    -- --- Quadra 840AV: list, Guest, mount, and copy 410 KB off the volume ---
    av_ethertalk_chooser("q840av")
    gclick(380, 111) -- Shared Folders
    scheduler.run(10000000)
    gclick(428, 302) -- OK
    scheduler.run(150000000)
    gclick(453, 258) -- Guest -> OK
    mac.run_until_match("volume-list.png", 600000000, 10000000)
    mac.check("volume-list.png")
    assert(appletalk.afp.sessions.count == 1, "the 840AV opened no ASP session")
    gclick(400, 258) -- mount "Shared"
    scheduler.run(300000000)
    gclick(98, 67) -- close the Chooser
    scheduler.run(300000000)
    mac.double_click(600, 95) -- open the mounted volume
    scheduler.run(400000000)
    assert(appletalk.afp.volumes["Shared"].cnid_count == 2, "the payload was not listed")

    -- Drag payload.image to the desktop; the Finder offers to copy it to
    -- the startup disk.  Every byte comes over AFP/ASP/ATP/DDP in ELAP
    -- frames: PSC channel 1's chain buffers carry the host's responses in.
    local payload_size = files.path_size(F"${vars.WORK_DIR}/share/payload.image")
    local bytes0 = appletalk.afp.stats.bytes_read
    machine.adb.mouse.move(38, 90, "global")
    scheduler.run(20000000)
    machine.adb.mouse.click(true)
    scheduler.run(5000000)
    machine.adb.mouse.move(120, 250, "global")
    scheduler.run(5000000)
    machine.adb.mouse.move(250, 380, "global")
    scheduler.run(5000000)
    machine.adb.mouse.move(300, 400, "global")
    scheduler.run(5000000)
    machine.adb.mouse.click(false)
    scheduler.run(100000000)
    gclick(462, 187) -- "copy to the startup disk?" -> OK
    local spent = 0
    while appletalk.afp.stats.bytes_read - bytes0 < payload_size do
        assert(spent < 4000000000, F"the copy stalled: ${appletalk.afp.stats.bytes_read - bytes0} of ${payload_size} bytes read")
        scheduler.run(20000000)
        spent = spent + 20000000
    end
    -- The Finder closes the fork when the copy is done.  Only the copy is
    -- asserted, not a golden: where the copied icon lands is the Finder's
    -- business (pdm-afp-copy-motion makes the same choice).
    while appletalk.afp.stats.open_forks ~= 0 do
        assert(spent < 4600000000, "the copy never closed its fork")
        scheduler.run(20000000)
        spent = spent + 20000000
    end
    scheduler.run(100000000)
    assert(appletalk.afp.stats.bytes_read - bytes0 == payload_size, F"read ${appletalk.afp.stats.bytes_read - bytes0} of ${payload_size} bytes")
    assert(appletalk.afp.stats.open_forks == 0, "the copy left a fork open")
    assert(debug.mac.globals.read("DSErrCode") == 0, "system error during the copy")
    echo(F"av-ethertalk-afp: ${payload_size} bytes copied over EtherTalk (${appletalk.stats.elap_rx} frames in, ${appletalk.stats.elap_tx} out)")

    -- --- Centris/Quadra 660AV: the same switch, the same lookup ------------
    av_ethertalk_chooser("q660av")
else
    mac.skip_test(F"needs ${av_hd}")
end

quit()
