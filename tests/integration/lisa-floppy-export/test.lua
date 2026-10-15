-- Converted from test.script by scripts/script-to-lua.py
-- Integration test: Apple Lisa 2 — machine.floppy.drive.0.export.
--
-- The Lisa's Sony disk keeps writes in a delta and its 12-byte sector tags in
-- memory, so neither the source file nor files.export_raw reflects what the OS
-- wrote.  A .dc42 export writes both: a 400K Lisa diskette is 84 header bytes
-- + 800 x 512 data bytes + 800 x 12 tag bytes = 419284 bytes.  The header
-- keeps the source's own label, so an unchanged disk comes back byte for
-- byte.  The runner cd's into this directory, so WORK_DIR is absolute.

local src = "../../data/Lisa/Xenix-3.0/Xenix-3.0-1.dc42"
scheduler.run(10000000)
machine.floppy.drive[0].insert(src)
assert(machine.floppy.drive[0].present, "diskette not inserted")
assert(machine.floppy.drive[0].filename == src, "filename is the source path")

-- DiskCopy 4.2: identical to the source (data, tags, label, both checksums).
assert(machine.floppy.drive[0].export(F"${vars.WORK_DIR}/saved.dc42"), "export returned false")
assert(files.path_size(F"${vars.WORK_DIR}/saved.dc42") == 419284, "exported image has the wrong size")
assert(files.path_compare(src, F"${vars.WORK_DIR}/saved.dc42") == -1, "DiskCopy export differs from the unchanged source")

-- Raw: the data alone.
assert(machine.floppy.drive[0].export(F"${vars.WORK_DIR}/saved.img"), "raw export returned false")
assert(files.path_size(F"${vars.WORK_DIR}/saved.img") == 409600, "raw export has the wrong size")

-- An export never overwrites.
assert(gs.try(function() return machine.floppy.drive[0].export(F"${vars.WORK_DIR}/saved.dc42") end, false) == false, "export overwrote an existing file")

-- Round trip: the exported image mounts again and exports to the same bytes.
machine.floppy.drive[0].eject()
machine.floppy.drive[0].insert(F"${vars.WORK_DIR}/saved.dc42")
assert(machine.floppy.drive[0].present, "exported image did not mount")
assert(machine.floppy.drive[0].export(F"${vars.WORK_DIR}/saved2.dc42"), "second export returned false")
assert(files.path_compare(F"${vars.WORK_DIR}/saved.dc42", F"${vars.WORK_DIR}/saved2.dc42") == -1, "re-export differs")

-- A raw source gets a tag area and the Lisa's label, so it still exports as
-- a full DiskCopy 4.2 diskette.
machine.floppy.drive[0].eject()
machine.floppy.drive[0].insert(F"${vars.WORK_DIR}/saved.img")
assert(machine.floppy.drive[0].export(F"${vars.WORK_DIR}/fromraw.dc42"), "export of a raw source returned false")
assert(files.path_size(F"${vars.WORK_DIR}/fromraw.dc42") == 419284, "raw-sourced DiskCopy export has the wrong size")
quit()
