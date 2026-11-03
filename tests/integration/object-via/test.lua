-- Converted from test.script by scripts/script-to-lua.py
-- M7c integration: `via1` / `via2` object classes.
-- SE/30 has both. The script just walks the tree and reads each
-- attribute — the exact register values depend on boot timing, so
-- we don't pin them, but each path must resolve to a numeric/JSON
-- value with no error JSON.

scheduler.run(100000)

-- via1 attributes.
local _ = machine.via1
local _ = machine.via1.ifr
local _ = machine.via1.ier
local _ = machine.via1.acr
local _ = machine.via1.pcr
local _ = machine.via1.sr
local _ = machine.via1.freq_factor

-- via1 port children.
local _ = machine.via1.port_a
local _ = machine.via1.port_a.output
local _ = machine.via1.port_a.input
local _ = machine.via1.port_a.direction
local _ = machine.via1.port_b.output
local _ = machine.via1.port_b.input
local _ = machine.via1.port_b.direction

-- via2 — only present on SE/30 / IIcx, which is what this test boots.
local _ = machine.via2
local _ = machine.via2.ifr
local _ = machine.via2.ier
local _ = machine.via2.acr
local _ = machine.via2.port_a.output
local _ = machine.via2.port_b.direction

-- Sanity: Universal-ROM machines (this test boots IIcx) clock both
-- VIAs with a divisor of 20. Plus would be 10. Pin the value so a
-- regression in either machine's clock plumbing fails fast.
assert(machine.via1.freq_factor == 20, "IIcx via1 freq_factor regressed")
assert(machine.via2.freq_factor == 20, "IIcx via2 freq_factor regressed")

quit()
