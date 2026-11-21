-- Converted from test.script by scripts/script-to-lua.py
-- A System 2.0.1 floppy: MFS, one flat directory of nine files.
local d = "../../data/systems/System_2_0_1.dsk"
assert(files.probe(d), "the MFS floppy is recognised as a disk")
assert(machine.floppy.identify(d) == "400K", "it is a 400K floppy")
local names = ""
for _, e in ipairs(files.list(F"${d}/partition1")) do
    names = F"${names}|${e.name}"
end
assert(names == "|Desktop|Clipboard File|Imagewriter|System|SysVersion|Finder|Fonts|Disk Copy|Font Mover", F"the directory, in order (got ${names})")

-- Both forks, at the sizes the directory records.
local sys = F"${d}/partition1/System"
assert(files.path_size(sys) == 5120, "System data fork")
assert(files.path_size(F"${sys}/rsrc/_raw") == 44560, "System resource fork")
assert(files.path_size(F"${d}/partition1/Finder/rsrc/_raw") == 46080, "Finder resource fork")
assert(files.path_size(F"${d}/partition1/system") == 5120, "names are matched without case")

-- Copying the volume out keeps each resource fork in an AppleDouble sidecar.
assert(files.cp(F"${d}/partition1", F"${vars.WORK_DIR}/out", true), "copy the volume out")
assert(files.path_size(F"${vars.WORK_DIR}/out/System") == 5120, "System copied")
assert(files.path_size(F"${vars.WORK_DIR}/out/._System") > 44560, "System's sidecar holds its resource fork")
assert(files.path_size(F"${vars.WORK_DIR}/out/._Finder") > 46080, "Finder's sidecar holds its resource fork")

quit()
