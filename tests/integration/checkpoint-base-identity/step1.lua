-- Converted from step1.script by scripts/script-to-lua.py
-- Save a quick checkpoint of a machine with a writable floppy.
machine.register("fedcba9876543210", "20261001T000000Z")
machine.floppy.drive[0].insert(F"${vars.TEST_RESULTS_DIR}/a.dsk", true)
scheduler.run(1000000)
assert(checkpoint.snapshot("base"), "quick checkpoint saved")
quit()
