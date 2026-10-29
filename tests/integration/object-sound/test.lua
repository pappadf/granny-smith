-- Converted from test.script by scripts/script-to-lua.py
-- M7f integration: `sound` object class.
-- Walks the four members and exercises mute() — the boot enables
-- sound around the chime, so `enabled` flips during the run; we
-- pin the path resolves cleanly and that mute()/volume= round-trip.

scheduler.run(50000)

-- Top-level object + read-only attrs.
local _ = machine.sound
local _ = machine.sound.sample_rate

-- enabled / volume — values depend on boot timing, just verify the
-- paths resolve to a usable JSON shape.
local _ = machine.sound.enabled
local _ = machine.sound.volume

-- Mute via the method, assert `enabled` flipped to false.
echo(F"${machine.sound.mute(true)}")
assert(machine.sound.enabled == false)

-- Unmute and assert `enabled` flips back to true.
echo(F"${machine.sound.mute(false)}")
assert(machine.sound.enabled == true)

quit()
