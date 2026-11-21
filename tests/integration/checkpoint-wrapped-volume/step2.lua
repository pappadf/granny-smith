-- Converted from step2.script by scripts/script-to-lua.py
-- Restore the consolidated checkpoint and report the disk's size as the
-- guest sees it (the wrapper included).
assert(checkpoint.load(F"${vars.TEST_RESULTS_DIR}/state.gs"), "checkpoint restored")
echo(F"SIZE=${files.images[0].raw_size}")
quit()
