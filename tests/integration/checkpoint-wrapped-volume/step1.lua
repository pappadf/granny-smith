-- Converted from step1.script by scripts/script-to-lua.py
-- Attach a bare HFS volume as the hard disk (it is wrapped), let the machine
-- run, and save a consolidated checkpoint.
machine.scsi.attach_hd(F"${vars.TEST_RESULTS_DIR}/vol.dsk", 0)
assert(files.images[0].format == "raw")
scheduler.run(1000000)
echo(F"SIZE=${files.images[0].raw_size}")
assert(checkpoint.save(F"${vars.TEST_RESULTS_DIR}/state.gs"), "consolidated checkpoint saved")
quit()
