-- Converted from test.script by scripts/script-to-lua.py
-- The Plus ROM's VBL handler writes Ticks ($16A) sixty times a second, with or
-- without a disk, so a write logpoint there fires steadily.
scheduler.run(20000000)
debug.logpoints.add{addr = 0x16A, width = "l", mode = "write", message = "ticks"}
scheduler.run(2000000)
local before = debug.logpoints.entries[0].hit_count
assert(before > 0, F"the Ticks write logpoint fires before the checkpoint (${before} hits)")

-- A load rebuilds the machine, and the debug object with it: the previous
-- machine's breakpoints and logpoints do not survive (docs/internals/core/storage/
-- checkpointing.md).
checkpoint.save(F"${vars.WORK_DIR}/cp.gscp")
checkpoint.load(F"${vars.WORK_DIR}/cp.gscp")
assert(#debug.logpoints.entries == 0, "a load drops the previous machine's logpoints")

-- One added after the load must fire like the first did (#172).
debug.logpoints.add{addr = 0x16A, width = "l", mode = "write", message = "ticks"}
scheduler.run(2000000)
local after = debug.logpoints.entries[0].hit_count
assert(after > 0, F"a memory logpoint added after checkpoint.load fires (${after} hits)")

echo("checkpoint-logpoints-ok")
quit()
