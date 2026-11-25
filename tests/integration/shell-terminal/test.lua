-- The terminal's two forms, driven through shell.run (the line entry the
-- terminal and the daemon use).  A command line is a tree path, a command
-- word or $alias followed by argument-mode words or `= value`; anything
-- else is Lua.  Output goes to this script's output, which the checks below
-- do not read: each check asserts on the tree instead.

scheduler.run(50000)

-- === Command lines ==============================================================

-- A bare path reads; an assignment writes; the right-hand side is an
-- expression, or a bare word naming nothing (an enum label).
shell.run("machine.cpu.d0 = 0x1000 + 0x234")
assert(machine.cpu.d0 == 0x1234, "assignment evaluates its expression")
shell.run("machine.cpu.d1 = (2 * 8)")
assert(machine.cpu.d1 == 16, "a parenthesised right-hand side")
shell.run("scheduler.mode = turbo")
assert(scheduler.mode == "turbo", "a bare word is an enum label")
shell.run("machine.cpu.d0")
shell.run("machine.cpu.d4 = 0x44  # a trailing comment")
assert(machine.cpu.d4 == 0x44, "a command line's # comment is dropped")
shell.run('files.mkdir "' .. vars.WORK_DIR .. '/has # hash" -- and a Lua-style one')
assert(files.path_exists(vars.WORK_DIR .. "/has # hash"), "a # inside quotes is text")

-- $alias heads and arguments, (expr), "${...}" templates, named arguments.
shell.run("$d2 = 7")
assert(machine.cpu.d2 == 7, "an alias is a place")
shell.run("debug.breakpoints.add ($pc + 0x10)")
assert(#debug.breakpoints.entries == 1, "a (expr) argument")
assert(debug.breakpoints.entries[0].addr == machine.cpu.pc + 0x10, "the computed address")
shell.run("debug.breakpoints.add $pc space=physical")
assert(debug.breakpoints.entries[1].addr == machine.cpu.pc, "a $alias argument")
assert(debug.breakpoints.entries[1].space == "physical", "a named enum argument")
debug.breakpoints.clear()
x = 21
shell.run('files.mkdir "' .. vars.WORK_DIR .. '/n${x * 2}"')
assert(files.path_exists(vars.WORK_DIR .. "/n42"), "a \"...\" argument fills ${...} from the globals")
shell.run("files.mkdir '" .. vars.WORK_DIR .. "/raw${x}'")
assert(files.path_exists(vars.WORK_DIR .. "/raw${x}"), "a '...' argument is raw")
shell.run("files.mkdir " .. vars.WORK_DIR .. "/bare_word")
assert(files.path_exists(vars.WORK_DIR .. "/bare_word"), "a bare word is a string")

-- A template slot keeps its "..." text raw: the logpoint expands it later.
shell.run('debug.logpoints.add addr=0x16A message="t=${value:x} $pc"')
assert(debug.logpoints.entries[0].message == "t=${value:x} $pc", "a template argument is not expanded")
debug.logpoints.clear()

-- Command words run their method.
shell.run("run 1000")
shell.run("stop")

-- Errors: an unknown path, a method's own refusal, a bad argument.
assert(not gs.ok(function() shell.run("nosuch.member.anywhere") end), "an unknown path fails")
assert(not gs.ok(function() shell.run("ghostword") end), "an unknown word fails")
assert(not gs.ok(function() shell.run("machine.cpu.pc 5") end), "an attribute takes no arguments")
assert(not gs.ok(function() shell.run("debug.breakpoints.add space=physical 0x10") end),
    "a positional argument after a named one fails")

-- === Lua lines ==================================================================

shell.run("y = 6 * 7")
assert(y == 42, "a statement runs")
shell.run("machine.cpu.d3 = machine.cpu.d2 * 3")
assert(machine.cpu.d3 == 21, "an assignment whose value reads the tree")
shell.run("machine.memory.poke.b(0x1000, 0x5A)")
assert(machine.memory.peek.b(0x1000) == 0x5A, "a method followed by ( is a Lua call")
shell.run("z = $d2 + 1")
assert(z == 8, "$alias in a Lua line reads the alias")
shell.run("for i = 1, 3 do w = (w or 0) + i end")
assert(w == 6, "a block on one line")
assert(not gs.ok(function() shell.run("y = = 1") end), "a syntax error fails")
assert(not gs.ok(function() shell.run("error('boom')") end), "a raised error fails")

-- quit is a call when typed alone (not exercised: it would end the test);
-- a global function typed alone is called.
function bump() bumped = (bumped or 0) + 1 end
shell.run("bump")
assert(bumped == 1, "a bare global function is called")

-- === Continuation ===============================================================

assert(shell.needs_continuation("for i = 1, 3 do"), "an open block continues")
assert(shell.needs_continuation("function f()\n  return 1"), "an open function continues")
assert(shell.needs_continuation("s = [[long"), "an open long string continues")
assert(not shell.needs_continuation("for i = 1, 3 do end"), "a closed block does not")
assert(not shell.needs_continuation("files.ls /tmp"), "a command line never continues")
assert(not shell.needs_continuation("y = = 1"), "a syntax error is not a continuation")

-- === Files: require and dofile resolve against the running file ===================

local probe = require("lib.probe")
assert(probe.double(21) == 42, "require finds lib/probe.lua beside this file")
assert(probe.loaded_from == "lib/probe.lua", "the module's table")
assert(require("lib.probe") == probe, "require loads a module once")

-- === Templates ===================================================================

assert(shell.expand("pc=${machine.cpu.pc}") == "pc=" .. gs.fmt("machine.cpu.pc"), "a tree path keeps its text")
assert(shell.expand("${y + 1}") == "43", "an expression")
assert(shell.expand("${y:04x}") == "002a", "a format spec")
assert(shell.expand("$pc") == gs.fmt("machine.cpu.pc"), "an alias")

print("shell-terminal-ok")
quit()
