-- Converted from test.script by scripts/script-to-lua.py
-- A Mac archive's member names are MacRoman; the VFS shows them as UTF-8 and
-- finds them as the Finder would.  names.sit (built at setup) holds a folder
-- "Dossier é" with a file "Café", and a floppy image "Système.dsk", each é
-- stored as the MacRoman byte 0x8E and the è as 0x8F.  utf8.zip names its member "Café" in
-- UTF-8 already: zip names are not MacRoman and are not converted.

-- The names, in archive order, as files.list (and the page) shows them.
local function names(dir)
    local s = ""
    for _, e in ipairs(files.list(dir)) do
        s = F"${s}|${e.name}"
    end
    return s
end

local sit = F"${vars.WORK_DIR}/names.sit"
assert(names(sit) == "|Dossier é|Système.dsk", F"the members, in UTF-8 (got ${names(sit)})")
assert(names(F"${sit}/Dossier é") == "|Café", "the folder's member, in UTF-8")

-- Reachable by its UTF-8 name, in either case, and not by a bare e.
files.cat(F"${sit}/Dossier é/Café")
assert(files.path_size(F"${sit}/Dossier é/Café") == 7, "the member by its UTF-8 name")
assert(files.path_size(F"${sit}/DOSSIER É/CAFÉ") == 7, "the member matched without case, as the Finder would")
assert(not gs.truthy(function() return files.path_exists(F"${sit}/Dossier e/Cafe") end), "diacriticals stay significant")

-- The disk image is a disk in turn, under its UTF-8 name.
assert(#files.list(F"${sit}/Système.dsk/partition1/System Folder") > 0, "the HFS volume of the member named in UTF-8")

-- Extracting writes the UTF-8 names to the host.
assert(files.archive.extract(sit, F"${vars.WORK_DIR}/out"), "extract the archive")
assert(files.path_compare(F"${vars.WORK_DIR}/out/Dossier é/Café", F"${sit}/Dossier é/Café") == -1, "the extracted file under its UTF-8 name")

-- files.archive.import takes the member by the same name, matched the same way.
assert(not gs.truthy(function() return files.archive.import(sit, F"${vars.WORK_DIR}/wrong.dmg", "SYSTÉME.DSK") end), "a different accent is a different name")
assert(files.archive.import(sit, F"${vars.WORK_DIR}/system.dmg", "SYSTÈME.DSK"), "the member by its UTF-8 name, without case")
assert(#files.list(F"${vars.WORK_DIR}/system.dmg/partition1/System Folder") > 0, "the imported image")

-- A zip's UTF-8 name is shown as stored and matched exactly.
local zip = F"${vars.WORK_DIR}/utf8.zip"
assert(names(zip) == "|Café", "the zip member's UTF-8 name, unchanged")
assert(files.path_size(F"${zip}/Café") == 6, "the zip member by its name")
assert(not gs.truthy(function() return files.path_exists(F"${zip}/CAFÉ") end), "zip names match exactly")

-- A checkpoint records a floppy read from the archive by its UTF-8 member
-- path, and a load finds it there again.
local fd = F"${sit}/Système.dsk"
machine.floppy.drive[0].insert(fd)
scheduler.run(2000000)
assert(machine.floppy.drive[0].image.source == fd, "the floppy names the member in UTF-8")
assert(checkpoint.save(F"${vars.WORK_DIR}/state.checkpoint"), "consolidated checkpoint saved")
assert(checkpoint.load(F"${vars.WORK_DIR}/state.checkpoint"), "checkpoint loaded")
assert(machine.floppy.drive[0].image.source == fd, "the restored floppy is the member")

quit()
