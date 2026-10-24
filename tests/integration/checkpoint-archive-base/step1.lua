-- Converted from step1.script by scripts/script-to-lua.py
-- Insert a floppy straight from the zip, save a consolidated checkpoint and
-- load it back.
local member = F"${vars.TEST_RESULTS_DIR}/floppies.zip/sys608.dsk"
machine.floppy.drive[0].insert(member)
scheduler.run(2000000)
assert(checkpoint.save(F"${vars.TEST_RESULTS_DIR}/state.checkpoint"), "consolidated checkpoint saved")
assert(checkpoint.load(F"${vars.TEST_RESULTS_DIR}/state.checkpoint"), "checkpoint loaded")
assert(files.path_compare(member, F"${vars.TEST_RESULTS_DIR}/sys608.dsk") == -1, "the archive member is untouched")
quit()
