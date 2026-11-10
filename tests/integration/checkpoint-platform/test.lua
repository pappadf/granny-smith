-- Converted from test.script by scripts/script-to-lua.py
-- A registered machine checkpoints under <checkpoint dir>/<id>-<created>,
-- and a writable image's delta lives there too.  The setup planted a delta
-- and journal from a "previous session" in that directory: files no
-- checkpoint refers to and no image holds open.
machine.register("0123456789abcdef", "20260926T000000Z")
local cpdir = F"${vars.WORK_DIR}/cp/0123456789abcdef-20260926T000000Z"
local function count(needle)
    local n = 0
    for _, e in ipairs(files.list(cpdir)) do
        if gs.contains(e.name, needle) then
            n = n + 1
        end
    end
    return n
end
machine.floppy.drive[0].insert(F"${vars.WORK_DIR}/media/System_3_2_0.dsk", true)
scheduler.run(1000000)
assert(count(".delta") == 2, "the inserted floppy's delta joined the stale one")
assert(checkpoint.snapshot("test"), "quick checkpoint saved")
assert(checkpoint.probe(), "probe finds it")

-- clear takes the stale delta and journal with the checkpoint, and keeps the
-- live image's (#149).
checkpoint.clear()
assert(not gs.truthy(function() return checkpoint.probe() end), "clear removed it")
assert(count("stale") == 0, "clear removed the stale delta and journal")
assert(count(".delta") == 1, "clear kept the live image's delta")

-- find_media picks the floppy image out of a directory, and copies it.
assert(files.find_media(F"${vars.WORK_DIR}/media", F"${vars.WORK_DIR}/found.dsk"), "a floppy image was found")
assert(gs.try(function() return files.find_media(F"${vars.WORK_DIR}/cp") end, "none") == "none", "no media in the checkpoint dir")

-- Browser-only: a download.
assert(gs.try(function() return files.download(F"${vars.WORK_DIR}/found.dsk") end, "unsupported") == "unsupported", "download says it is not supported")

echo("checkpoint-platform-ok")
quit()
