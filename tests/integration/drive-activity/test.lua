-- Converted from test.script by scripts/script-to-lua.py
-- The floppy is attached but nothing has read it yet.
assert(files.images[0].type == "fd_ds", "fd0 is the System 6.0.8 disk")
assert(files.images[0].format == "raw", "fd0 carries no wrapper layer")
assert(files.images[0].reads == 0, "no I/O before the machine runs")
assert(files.images[0].writes == 0)

-- The Plus boots from the floppy: its reads are counted.
scheduler.run(30000000)
assert(files.images[0].reads > 0, "the boot read the floppy")

-- Ejected, the image is closed at once: it leaves the tracked images, so its
-- counters no longer feed the light.
echo(F"${machine.floppy.drive[0].eject()}")
assert(gs.try(function() return files.images[0].reads end, "gone") == "gone", "an ejected disk is still tracked")
scheduler.run(5000000)

quit()
