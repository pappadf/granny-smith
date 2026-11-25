-- Commands: bare words the terminal's command form runs as the method they
-- name (`ls /tmp` runs files.ls).  A script calls the method itself; the
-- words are the terminal's, so they are driven here through shell.run, the
-- line entry the terminal and the daemon use.

-- === Built-ins run their methods ==============================================
local dir = "/tmp/gs-shell-command-words"
if files.path_exists(dir) then
    files.rm(dir)
end
shell.run(F"mkdir ${dir}")
shell.run(F"cd ${dir}")
assert(files.pwd() == dir, F"cd sets the current directory (pwd: ${files.pwd()})")
shell.run("mkdir sub")
assert(files.path_exists(F"${dir}/sub"), "a relative path starts at the current directory")
shell.run("cd sub")
assert(files.pwd() == F"${dir}/sub", "cd into a relative directory")
shell.run("cd ..")
assert(files.pwd() == dir, "cd .. goes up")
shell.run("ls")
assert(not gs.ok(function() shell.run(F"cd ${dir}/nosuch") end), "cd refuses a missing directory")
shell.run("run 1000")
shell.run("stop")

-- === Declaring commands =======================================================
shell.command.add("ll", "files.list")
local found = false
for _, c in ipairs(shell.command.list()) do
    if c.name == "ll" then
        assert(c.target == "files.list", "ll's target")
        assert(c.builtin == false, "ll is a user command")
        found = true
    end
end
assert(found, "ll is listed")
shell.run("ll")
assert(not gs.ok(function() shell.command.add("pcv", "machine.cpu.pc") end), "a command needs a method")
assert(not gs.ok(function() shell.command.add("nope", "no.such.method") end), "a command needs a target that resolves")
assert(not gs.ok(function() shell.command.add("files", "files.ls") end), "a root path cannot be a command")
assert(not gs.ok(function() shell.command.add("ls", "files.list") end), "a built-in cannot be redeclared")
assert(not gs.ok(function() shell.command.add("local", "files.list") end), "a reserved word cannot be a command")
assert(not gs.ok(function() shell.command.add("function", "files.list") end), "function is reserved")
shell.command.remove("ll")
assert(not gs.ok(function() shell.run("ll") end), "a removed command is gone")

-- `command` is no keyword: a member may be called command.
assert(shell.command.meta.path == "shell.command", "shell.command resolves")

-- === help, completion, highlighting follow the target =========================
local u = shell.usage("ls")
assert(u.signature == "files.ls [path]", "ls's usage is files.ls's")
local c = shell.complete("l", 1, true)
local seen = false
for _, k in ipairs(c.candidates) do
    if k.text == "ls" then
        assert(k.kind == "method", "ls completes as a method")
        seen = true
    end
end
assert(seen, "ls is a completion")
local a = shell.complete("ls /", 4, true)
assert(a.context.method == "files.ls", "ls's arguments are files.ls's")
local h = shell.highlight("ls /tmp")
assert(h[1].class == "method", "ls highlights as a method")

-- === A global function shadows a command in the terminal ======================
-- A line whose first word is a Lua global is Lua: the global runs.
function ls()
    print("mine")
end
shell.run("ls")
ls = nil
shell.run("ls") -- the command again

files.rm(dir)
print("shell-command-words-ok")
quit()
