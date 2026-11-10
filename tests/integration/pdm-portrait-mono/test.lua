-- Converted from test.script by scripts/script-to-lua.py
-- PDM built-in video on the 15" Portrait (see config.mk).
machine.boot{model = "pm6100", monitor = "portrait", rom = vars.ROM}
scheduler.run(400000000)
assert(machine.screen.width == 640, F"portrait: 640 pixels across (got ${machine.screen.width})")
assert(machine.screen.height == 870, F"portrait: 870 lines (got ${machine.screen.height})")
assert(machine.screen.depth == 8, F"portrait: the ROM's 8 bpp (got ${machine.screen.depth})")
-- Leave out the floppy icon (it blinks) and the cursor.
machine.screen.match("gray-screen.png", 405, 290, 465, 350, 0, 0, 40, 40)
quit()
