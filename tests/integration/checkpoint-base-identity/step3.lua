-- Converted from step3.script by scripts/script-to-lua.py
-- The base was replaced with other bytes of the same size: refused.
machine.register("fedcba9876543210", "20261001T000000Z")
assert(checkpoint.probe(), "the quick checkpoint is there")
assert(gs.try(function() return checkpoint.load() end, false) == false, "a replaced base is refused")
echo("replaced-base-refused")
quit()
