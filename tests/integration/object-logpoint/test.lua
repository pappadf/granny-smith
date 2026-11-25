-- Integration: logpoint messages use ${...} interpolation.
--
-- The VBL handler bumps Ticks ($00016A.l) once per VBL — set a write
-- logpoint there with a message that exercises:
--   ${machine.cpu.pc}              — register read at fire time (object path)
--   ${$value:08x}          — per-fire bound name, format spec
--   ${$addr:08x}           — per-fire bound name
--   ${$size}               — default formatter
-- `message=` is a template slot (OBJ_ARG_TEMPLATE; docs/internals/core/shell/shell.md):
-- the string body is stored raw and evaluated per fire with $value/$addr/$size
-- bound.
-- After running long enough for at least one VBL, the harness
-- captures the formatted line and the run continues to `quit`.

log.set("memory", 5)
log.set("logpoint", 5)

debug.logpoints.add{addr = 0x16A, width = "l", mode = "write", level = 5, message = "Ticks bumped pc=${machine.cpu.pc} addr=${$addr:08x} val=${$value:08x} size=${$size}"}

-- Bad input is an error, never an entry: a category outside the manifest
-- (it used to trip the manifest assert and halt) and an inverted range.
assert(not gs.ok(function() return debug.logpoints.add{addr = 0x400, category = "nosuchcat"} end), "an unknown category was accepted")
assert(not gs.ok(function() return debug.logpoints.add{addr = 0x400, ["end"] = 0x3FF} end), "end below addr was accepted")

-- Call form keeps a whole "..." message as its template, as argument mode
-- does: it is filled in at each fire, not once now (it used to be
-- interpolated here, so every hit printed the same stale values).
local lpc = debug.logpoints.add{addr = 0x16A, width = "l", mode = "write", level = 5, message = "call form val=${$value:08x}"}
assert(lpc.message == 'call form val=${$value:08x}', "the call-form message was interpolated at add time")
-- A message built by an expression is an expression: evaluated now.
local lpe = debug.logpoints.add{addr = 0x16A, width = "l", mode = "write", level = 5, message = "built " .. F"${1 + 1}"}
assert(lpe.message == "built 2", "an expression message was not evaluated")

scheduler.run(5000000)

echo(F"${#debug.logpoints.entries} logpoints")
quit()
