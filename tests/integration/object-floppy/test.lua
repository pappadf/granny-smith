-- M7e integration: `floppy` object class.
-- fd0 is loaded with a System 6.0.8 disk, fd1 is empty. The script
-- walks both drives, verifies present/disk reflect the load state,
-- exercises eject() and confirms present flips to false afterwards.

scheduler.run(50000)

-- Top-level floppy. Plus uses the IWM controller; sel is the active
-- drive selector.
local _ = machine.floppy
assert(machine.floppy.type == "iwm", "Plus floppy controller is iwm")
local _ = machine.floppy.sel

-- Drives collection.
local _ = machine.floppy.drive

-- Drive 0: populated by --fd0=. After 50K cycles the head is at
-- track 0, side 0, motor still spinning up. The inserted medium is a
-- first-class node now (drive[N].image); its delta stem is image.delta.
assert(machine.floppy.drive[0].index == 0)
assert(machine.floppy.drive[0].present == true)
assert(machine.floppy.drive[0].track == 0)
assert(machine.floppy.drive[0].side == 0)
local _ = machine.floppy.drive[0].motor_on
assert(machine.floppy.drive[0].present == true, "drive 0 holds a disk")
local _ = machine.floppy.drive[0].image.delta
-- The one image node (image_class.c): what every drive mounts.
assert(machine.floppy.drive[0].format == "gcr800", "drive 0 does not hold an 800K GCR disk")
assert(machine.floppy.drive[0].image.kind == "floppy")
assert(machine.floppy.drive[0].image.encoding == "raw")
assert(machine.floppy.drive[0].image.sectors == 1600)
assert(machine.floppy.drive[0].image.size == 819200)
assert(machine.floppy.drive[0].image.tag_bytes == 12, "a GCR floppy carries 12 tag bytes beside each sector")
assert(machine.floppy.drive[0].image.writable, "inserted writable by default")
assert(gs.contains(machine.floppy.drive[0].image.source, "System_6_0_8.dsk"))

-- Drive 1: empty — the image node is absent (no medium loaded).
assert(machine.floppy.drive[1].index == 1)
assert(machine.floppy.drive[1].present == false)
assert(machine.floppy.drive[1].format == "")
assert(gs.try(function() return machine.floppy.drive[1].image.kind end, "none") == "none", "an empty drive has an image node")

-- Eject drive 0 — present must flip to false and the image node disappears.
echo(F"${machine.floppy.drive[0].eject()}")
assert(machine.floppy.drive[0].present == false)

-- Ejecting an already-empty drive is an error (the method is honest
-- rather than silently no-op'ing — the harness shouldn't fail on
-- this since echo ${} prints the error and continues).

-- The other two controller types live where their machines do: `swim`
-- on the SE/30 (suite-030) and `swim3` on the PDM family, whose
-- pdm-floppy-* rows in suite-pdm walk this same tree on a booted 7100
-- (type, present, and the image node either side of an insert).
-- Both need their machine's ROM, which this unit-tier row does not have.

quit()
