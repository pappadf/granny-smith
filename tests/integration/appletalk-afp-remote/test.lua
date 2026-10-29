-- Converted from test.script by scripts/script-to-lua.py
-- AppleTalk AFP remote server — a second file server with a remote volume.
--
-- Boots System 6, adds a file server beside "Shared Folders"
-- (appletalk.remote.add_server, the call the page makes for archive.org) and
-- gives it a read-only volume whose content is a host folder
-- served through the dir:// provider.  That provider is the core's own, but
-- the path is the page's: the volume starts as an empty cache, every folder
-- is listed the first time the guest enters it, a file's data is fetched when
-- it is opened, and the commands that need them wait (ASP holds the reply)
-- until they are there.  Then, through the guest UI:
--
--   1. The Chooser lists both servers -- chooser-two-servers.png.
--   2. "Remote Test" → Guest → its volume list -- remote-volume-list.png.
--   3. Mount "Remote" and open it: the root is listed on demand, the volume
--      is locked (read-only), and the BinHex'd StuffIt archive holding a disk
--      image shows as the folder "MacTest_Disk" -- remote-window.png.
--   4. Drag "Read Me" to the startup disk: the Finder's copy opens its data
--      fork, which makes the server fetch it.
--   5. Open "MacTest_Disk": the server fetches the archive, unpacks it --
--      BinHex, then StuffIt, then the disk image inside -- and the window
--      shows the disk's files with their own icons -- remote-archive-folder.png.
--
-- Budgets are instruction-exact and the wall clock is pinned.  The windows'
-- info bars carry the host's free space, so they are left out of the matches.

local mac = dofile("../lib/mac.lua")
dofile("../lib/afp.lua")

machine.rtc.time = "2026-04-26T21:30:36"

-- Boot to the Finder desktop.
scheduler.run(30000000)

-- The second server: its own Chooser entry on its own ASP socket.
assert(appletalk.remote.add_server("Remote Test"), "server not added")
assert(appletalk.remote.servers[1] == "Remote Test", "server not listed")
assert(appletalk.nbp["Remote Test"].type == "AFPServer", "server not advertised over NBP")
assert(appletalk.nbp["Remote Test"].socket ~= appletalk.nbp["Shared Folders"].socket, "both servers on one socket")
assert(gs.try(function() return appletalk.remote.add_server("Remote Test") end, false) == false, "a duplicate server name was accepted")

assert(appletalk.remote.add_volume("Remote Test", "Remote", F"dir://${vars.WORK_DIR}/source", F"${vars.WORK_DIR}/cache"), "volume not added")
assert(appletalk.remote.volumes[0].uri == F"dir://${vars.WORK_DIR}/source", "volume not listed")
assert(appletalk.remote.issued == 0, "a request went out before the guest asked for anything")

-- Apple menu → Chooser → AppleShare → "AppleShare requires AppleTalk" → OK:
-- both servers are listed.
machine.adb.mouse.move(25, 10)
scheduler.run(2000000)
machine.adb.mouse.click(true)
scheduler.run(4000000)
machine.adb.mouse.move(25, 123)
scheduler.run(4000000)
machine.adb.mouse.click(false)
scheduler.run(60000000)
afp_click(95, 90, 20000000)
afp_click(190, 206, 60000000)
mac.check("chooser-two-servers.png")

-- Select "Remote Test" (the second line), OK, Guest → OK: its volume list.
afp_click(280, 101, 6000000)
afp_click(333, 178, 40000000)
afp_click(392, 259, 40000000)
mac.check("remote-volume-list.png")

-- Mount "Remote" and close the Chooser.
afp_mount_and_close()
assert(appletalk.afp.sessions.count == 1, "expected one ASP session")
assert(appletalk.afp.sessions[0].server_socket == appletalk.nbp["Remote Test"].socket, "the session is not with the second server")

-- Double-click the volume: its root is listed now, on demand.
local function afp_open(x, y)
    machine.adb.mouse.move(x, y)
    scheduler.run(2000000)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    mac.wait_mouse_taken(100000000)
    scheduler.run(80000)
    machine.adb.mouse.click(true)
    machine.adb.mouse.click(false)
    mac.wait_mouse_taken(100000000)
    scheduler.run(60000000)
end
afp_open(468, 108)
mac.check_ex("remote-window.png", 64, 140, 78, 420)
assert(appletalk.remote.issued == 1, "the root listing was not the one request")
assert(files.path_size(F"${vars.WORK_DIR}/cache/Read Me") == 0, "a file was fetched before it was opened")

-- Drag "Read Me" onto the startup disk: the copy reads its data fork, which
-- the server fetches first.
machine.adb.mouse.move(110, 105)
scheduler.run(2000000)
machine.adb.mouse.click(true)
scheduler.run(4000000)
machine.adb.mouse.move(200, 90)
scheduler.run(2000000)
machine.adb.mouse.move(400, 60)
scheduler.run(2000000)
machine.adb.mouse.move(470, 55)
scheduler.run(4000000)
machine.adb.mouse.click(false)
mac.wait_mouse_taken(100000000)
scheduler.run(80000000)
assert(appletalk.remote.issued == 2, "opening the file did not fetch it")
assert(files.path_size(F"${vars.WORK_DIR}/cache/Read Me") == 18, "the fetched file is not in the cache")

-- Open the archive's folder: fetched, unpacked through three layers, shown.
afp_open(237, 110)
mac.check_ex2("remote-archive-folder.png", 64, 140, 78, 420, 104, 140, 118, 420)
assert(appletalk.remote.issued == 3, "opening the container did not fetch it")
assert(appletalk.remote.failed == 0, "a request failed")
assert(files.path_exists(F"${vars.WORK_DIR}/cache/MacTest_Disk/System"), "the disk image inside the archive was not opened")
assert(files.path_exists(F"${vars.WORK_DIR}/cache/MacTest_Disk/DeskTop") == false, "the disk's desktop file was copied out")
quit()
