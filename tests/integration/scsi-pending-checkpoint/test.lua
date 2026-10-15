-- Converted from test.script by scripts/script-to-lua.py
-- A checkpoint saved with the SCSI selection timeout pending (see config.mk).

machine.boot{model = "q700", ram = 8192, rom = vars.ROM}

local n = 0
local hit = 0
while n < 3000 do
scheduler.run(50000)
n = n + 1
if gs.contains(F"${scheduler.events}", "\"select_timeout\"") then
hit = 1
break
end
end
assert(hit == 1, "the selection timeout was never pending")

assert(checkpoint.save(F"${vars.WORK_DIR}/scsi-pending.checkpoint"), "checkpoint save")
assert(checkpoint.load(F"${vars.WORK_DIR}/scsi-pending.checkpoint"), "checkpoint load")
assert(gs.contains(F"${scheduler.events}", "\"select_timeout\""), "the restored queue lost the selection timeout")

-- The restored machine runs on through the timeout and the rest of the probe.
scheduler.run(20000000)
echo("scsi-pending-checkpoint ok")
quit()
