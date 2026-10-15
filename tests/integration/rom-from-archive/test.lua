-- Converted from test.script by scripts/script-to-lua.py
-- Integration test: boot from roms.zip/plus.rom (see config.mk).  The runner
-- cd's into this directory; WORK_DIR is absolute.

local zip = F"${vars.WORK_DIR}/roms.zip"
local member = F"${zip}/plus.rom"

-- The archive lists its members, and the ROM member reads back as the ROM.
local names = ""
for _, e in ipairs(files.list(zip)) do
    names = F"${names}|${e.name}"
end
assert(names == "|plus.rom|README.txt", F"the members, in archive order (got ${names})")
assert(files.cat(F"${zip}/README.txt"), "a member opens through files.cat")
assert(files.path_size(zip) < files.path_size(vars.ROM), "the member is deflated")
assert(files.path_size(member) == 131072, "the member's size")
assert(files.path_compare(member, vars.ROM) == -1, "the member decodes to the ROM")

-- Booting names the member: the ROM is identified from what it decodes to.
assert(machine.boot{model = "plus", rom = member}, "boot from the archive member")
assert(machine.rom.id == "4d1f8172", "the ROM read out of the archive")
assert(machine.rom.path == member, "the machine's ROM names the member")

-- And the machine runs it: the ROM's boot code draws the screen.
local before = machine.screen.checksum()
scheduler.run(20000000)
assert(machine.screen.checksum() ~= before, "the ROM from the archive runs")

quit()
