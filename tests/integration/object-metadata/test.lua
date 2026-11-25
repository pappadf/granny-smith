-- Converted from test.script by scripts/script-to-lua.py
-- The object-model metadata: see config.mk.  run.sh runs this and compares
-- the usage blocks it prints with the golden files in this directory.

-- === meta.members: keys by member kind (docs compared by presence) ========
local function member_of(ms, name)
    for _, m in ipairs(ms) do
        if m.name == name then return m end
    end
    return nil
end

-- Attribute: a type descriptor.
local d0 = member_of(machine.cpu.meta.members(), "d0")
assert(d0.kind == "attr", "d0 is an attribute")
assert(d0.type.kind == "uint", "d0 type kind")
assert(d0.type.presentation == "hex", "d0 presentation")
assert(d0.type.width == 0, "d0 width")
assert(d0.type.enum == nil, "d0 has no enum table")
assert(#d0.doc > 0, "d0 has a doc")

-- Method: args with their own type descriptors, and the result.
local add = member_of(debug.breakpoints.meta.members(), "add")
assert(add.kind == "method", "add is a method")
assert(#add.args == 3, "add has three arguments")
assert(add.args[1].name == "addr", "first argument")
assert(add.args[1].type.kind == "uint", "addr is a uint")
assert(add.args[1].optional == false, "addr is required")
assert(add.args[3].type.kind == "enum", "space is an enum")
assert(add.args[3].type.enum[2] == "physical", "space's values")
assert(add.args[3].optional == true, "space is optional")
assert(add.result.kind == "object", "add returns an object")

-- Named child with a node doc; a collection container with live entries.
local fl = member_of(machine.meta.members(), "floppy")
assert(fl.kind == "child", "floppy is a child")
assert(#fl.doc > 0, "floppy has a node doc")
assert(fl.collection == false, "floppy is not a collection")
local dr = member_of(machine.floppy.meta.members(), "drive")
assert(dr.collection == true, "drive is a collection container")
assert(#dr.indices == 1, "one drive (the external position ships empty)")
assert(dr.keys == nil, "drive is not keyed")
local cat = member_of(log.meta.members(), "category")
assert(cat.collection == true, "log.category is a collection container")
assert(cat.indices == nil, "log.category is not indexed")
assert(#cat.keys > 10, "log.category lists its keys")

-- Root children carry their domain.
local mach = member_of(meta.members(), "machine")
assert(mach.domain == "machine", "machine domain")
local at = member_of(meta.members(), "appletalk")
assert(at.domain == "network", "appletalk domain")
local sch = member_of(meta.members(), "scheduler")
assert(sch.domain == "emulator", "scheduler domain")

-- A lookup-backed child of a collection entry has the entry's path.  (Id 2:
-- the IIcx's id 3 is its CD bay, which takes only the CD-ROM drive.)
machine.scsi.attach_hd("../../data/systems/System_6_0_8.dsk", 2)
assert(machine.scsi.device[2].image.meta.path == "machine.scsi.device[2].image", "lookup child path")

-- Tiers: statistics and plumbing.
local _m6 = member_of(scheduler.meta.members(), "cycles")
assert(_m6.category == "advanced", "scheduler.cycles is advanced")
local _m7 = member_of(machine.cpu.meta.members(), "vbr")
assert(_m7.category == "advanced", "cpu.vbr is advanced")
local _m8 = member_of(shell.meta.members(), "complete")
assert(_m8.hidden == true, "shell.complete is hidden")

-- === shell.keywords ========================================================
assert(#shell.keywords >= 17, "every reserved word")
for _, k in ipairs(gs.list(shell.keywords)) do
    assert(#k.syntax > 0, F"keyword ${k.word} has a syntax")
end

-- === shell.needs_continuation ==============================================
assert(shell.needs_continuation("if x then"), "an open block continues")
assert(not shell.needs_continuation("print(1)"), "a statement does not")
assert(not shell.needs_continuation("files.ls /tmp"), "a command line never does")

-- === shell.complete detail and context =====================================
local c = shell.complete("machine.floppy.drive[0].ins", 27, true)
assert(c.candidates[1].text == "machine.floppy.drive[0].insert", "candidate text")
assert(c.candidates[1].kind == "method", "candidate kind")
assert(#c.candidates[1].doc > 0, "candidate doc")
assert(c.context.method == nil, "not in an argument position")
local p = shell.complete("machine.floppy.drive[0].insert foo", 34, true)
assert(p.context.method == "machine.floppy.drive[0].insert", "context method")
assert(p.context.arg_index == 0, "positional slot")
assert(p.context.arg_name == "path", "positional name")
local n = shell.complete("debug.breakpoints.add 0x10 space=p", 34, true)
assert(n.context.arg_index == 2, "a name= argument names its declared slot")
assert(n.context.arg_name == "space", "name= slot name")
local q = shell.complete("debug.breakpoints.add 0x10 space=physical 1", 43, true)
assert(q.context.arg_index == 1, "name= words do not count as positionals")
assert(gs.str(shell.complete("fi", 2).candidates) == "[files.]", "without detail the result is unchanged")

-- === shell.highlight on the live tree ======================================
-- `flopy` does not resolve: it and everything after it are unknown.
local h = shell.highlight("machine.flopy.drive")
assert(#h == 3, "three segment spans")
assert(h[1].class == "object", "machine resolves")
assert(h[2].class == "unknown", "flopy is unknown")
assert(h[2].start == 8, "flopy starts at byte 8")
assert(h[3].class == "unknown", "drive after it is unknown too")
local h2 = shell.highlight("machine.floppy.drive[0].insert /tmp/x.img writable=true")
assert(h2[7].class == "method", "insert resolves to a method")
assert(h2[8].class == "attribute", "a name= argument")
local h3 = shell.highlight("scheduler.mode = turbo")
assert(h3[(#h3 - 1) + 1].class == "enum", "an enum attribute's value")

-- === shell.usage: signature spans, and the golden texts =====================
local u = shell.usage("debug.breakpoints.add")
assert(u.signature == "debug.breakpoints.add <addr> [condition] [space: logical | physical]", "signature")
assert(u.arg_spans[1][1] == 22, "first span start")
assert(u.arg_spans[1][2] == 28, "first span end")
for _, path in ipairs({"debug.breakpoints.add", "machine.floppy.drive[0].insert", "scheduler.mode", "machine.cpu.d0", "machine.floppy"}) do
    echo(F"=== USAGE ${path}")
    echo(F"${shell.usage(path).text}")
end
echo("=== END")
assert(help("debug.breakpoints.add") == shell.usage("debug.breakpoints.add").text, "help prints the usage text")

echo("object-metadata-ok")
quit()
