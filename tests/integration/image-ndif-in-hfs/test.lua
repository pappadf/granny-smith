-- Converted from test.script by scripts/script-to-lua.py
-- Integration test: attach toast.img/partition1/Disk.ndif as a CD (see
-- config.mk).  The runner cd's into this directory; WORK_DIR is absolute.

local toast = F"${vars.WORK_DIR}/toast.img"
local ndif = F"${toast}/partition1/Disk.ndif"
local fd = F"${vars.TEST_DATA}/systems/System_6_0_8.dsk"

-- The outer volume holds the NDIF file: its chunks in the data fork, the
-- chunk map in the resource fork.
assert(files.identify(toast).opens, "the HFS image is recognised")
assert(files.path_size(ndif) > 0, "the NDIF's data fork")
assert(files.path_size(F"${ndif}/rsrc/_raw") > 0, "the chunk map's resource fork")

-- Attached as a CD, the drive sees the decoded disk, read-only.
machine.scsi.attach_cdrom(ndif, 3)
assert(files.images[0].kind == "cd", "attached as a CD-ROM")
assert(files.images[0].encoding == "ndif", "the NDIF wrapper is decoded")
assert(files.images[0].size == 819200, "the decoded disk is the floppy's size")
assert(not files.images[0].writable, "a CD is read-only")

-- The decoded disk is the floppy, byte for byte, and its volume lists.
assert(files.export_raw(ndif, F"${vars.WORK_DIR}/decoded.img"), "export the decoded disk")
assert(files.path_compare(F"${vars.WORK_DIR}/decoded.img", fd) == -1, "NDIF decodes to the original floppy")
assert(#files.list(F"${ndif}/partition1/System Folder") > 0, "the HFS volume inside the NDIF inside the HFS volume")

-- A checkpoint restore reopens the disc as the kind its drive presents.  It
-- used to re-guess the kind from the size, so this floppy-sized disc came back
-- as an 800K floppy (and lit the floppy light).
assert(checkpoint.save(F"${vars.WORK_DIR}/cd.checkpoint"), "checkpoint save")
assert(checkpoint.load(F"${vars.WORK_DIR}/cd.checkpoint"), "checkpoint load")
assert(files.images[0].kind == "cd", "restored as a CD-ROM, not re-guessed from its size")
assert(not files.images[0].writable, "still read-only after the restore")

quit()
