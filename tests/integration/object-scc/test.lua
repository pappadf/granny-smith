-- Converted from test.script by scripts/script-to-lua.py
-- Integration: `scc` object class.
-- The new `scc.*` paths and the legacy `scc loopback` command must
-- share the same backing state — flipping one and reading via the
-- other is the strongest test that the object surface is a real
-- view, not a separate bag of fields.

scheduler.run(50000)

-- Defaults.
local _ = machine.scc
local _ = machine.scc.loopback
local _ = machine.scc.pclk_hz
local _ = machine.scc.rtxc_hz

-- Channel children both resolve and report their own index.
local _ = machine.scc.a
local _ = machine.scc.b
local _ = machine.scc.a.index
local _ = machine.scc.b.index
local _ = machine.scc.a.dcd
local _ = machine.scc.b.dcd
local _ = machine.scc.a.tx_empty
local _ = machine.scc.b.rx_pending

-- Flip via the typed setter, assert via the path predicate. The
-- round-trip is the real test — anything that decouples the attribute
-- from the underlying scc state shows up here as an assertion failure.
machine.scc.loopback = true
assert(machine.scc.loopback == true)
machine.scc.loopback = false
assert(machine.scc.loopback == false)

-- Reset method runs to completion (no return value to assert beyond
-- the absence of an error in the JSON output).
echo(F"${machine.scc.reset()}")

-- Host-side transmit capture — the mirror of `receive`.  Write bytes straight
-- into channel A's data register (the Plus decodes the ESCC write base at
-- $BFFFF8, so aData is +6) and drain them back out.
-- The escape rule is asserted on a non-printable byte, because the whole point
-- of the surface is to make a guest's serial console readable as TEXT: a
-- capture that silently swallowed $01 would look like clean output.
assert(machine.scc.a.sent_pending == 0, "channel A capture is not empty before the write")
machine.memory.poke.b(0xBFFFFE, 0x48)
machine.memory.poke.b(0xBFFFFE, 0x69)
machine.memory.poke.b(0xBFFFFE, 0x0A)
machine.memory.poke.b(0xBFFFFE, 0x01)
assert(machine.scc.a.sent_pending == 4, F"capture holds ${machine.scc.a.sent_pending} byte(s), want 4")
assert(machine.scc.b.sent_pending == 0, "channel B captured channel A's bytes")
local drained = machine.scc.a.sent()
assert(drained == "Hi\n\\x01", F"sent() returned [${drained}], want [Hi\\n\\x01]")
-- A drain is a drain: what came out is gone, and nothing was lost on the way.
assert(machine.scc.a.sent_pending == 0, "sent() did not clear the capture")
assert(machine.scc.a.sent() == "", "a second drain returned text")
assert(machine.scc.a.sent_dropped == 0, "the capture dropped bytes")

quit()
