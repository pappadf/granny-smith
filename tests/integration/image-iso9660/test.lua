-- Converted from test.script by scripts/script-to-lua.py
-- An ISO 9660 disc (built at setup: a primary tree and a Joliet tree) is a
-- bare volume: one synthetic partition holding the disc's files.
local iso = F"${vars.WORK_DIR}/disc.iso"
assert(files.probe(iso), "the disc is recognised as a disk")
local names = ""
for _, e in ipairs(files.list(F"${iso}/partition1")) do
    names = F"${names}|${e.name}"
end
assert(names == "|Disk Images|Read Me.txt", F"Joliet names, not 8.3 ones (got ${names})")

-- A file on the disc reads back byte for byte.
local readme = F"${iso}/partition1/Read Me.txt"
assert(files.path_size(readme) == files.path_size("config.mk"), "file size")
assert(files.path_compare(readme, "config.mk") == -1, "file contents")
assert(files.path_size(F"${iso}/partition1/read me.txt") > 0, "names are matched without case")

-- A floppy image on the disc is a disk in turn: descend into its HFS volume.
local fd = F"${iso}/partition1/Disk Images/System 6.0.8.dsk"
assert(files.path_size(fd) == 819200, "the floppy image on the disc")
assert(files.list(F"${fd}/partition1/System Folder"), "the HFS volume inside the floppy image on the disc")
assert(files.path_size(F"${fd}/partition1/System Folder/System/rsrc/_raw") > 0, "a resource fork two levels down")

-- A hybrid disc: the partition map's HFS volume and, beside it, the ISO side.
local hy = F"${vars.WORK_DIR}/hybrid.iso"
local roots = ""
for _, e in ipairs(files.list(hy)) do
    roots = F"${roots}|${e.name}"
end
assert(roots == "|partition1|partition2|iso9660", F"the map's partitions, then the ISO side (got ${roots})")
assert(files.path_compare(F"${hy}/partition2/Hello.txt", "config.mk") == -1, "the HFS side reads")
assert(files.path_compare(F"${hy}/ISO9660/Read Me.txt", "config.mk") == -1, "the ISO side reads (name matched without case)")
assert(not gs.truthy(function() return files.path_exists(F"${iso}/iso9660") end), "a disc that is only ISO has no iso9660 entry")
assert(not gs.truthy(function() return files.path_exists(F"${hy}/partition3") end), "the ISO side is not a partitionN")

-- Apple's "AA" entry is the file's Finder info, and a copy out keeps it.
assert(files.path_compare(F"${hy}/iso9660/Read Me.txt/finf", F"${vars.WORK_DIR}/readme.finf") == -1, "Finder info from AA")
assert(not gs.truthy(function() return files.path_exists(F"${iso}/partition1/Read Me.txt/finf") end), "no AA, no Finder info")
assert(files.cp(F"${hy}/iso9660/Read Me.txt", F"${vars.WORK_DIR}/Read Me.txt"), "copy the file out")
assert(files.path_size(F"${vars.WORK_DIR}/._Read Me.txt") > 0, "its AppleDouble sidecar carries the Finder info")

quit()
