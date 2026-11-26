-- plus-moof-track-content, quick-checkpoint step 1: a writable capture in
-- drive 0, a quick checkpoint of it.
machine.register("00000000000a0c10", "20261126T000000Z")
machine.floppy.drive[0].insert(vars.QUICK, true)
assert(machine.floppy.drive[0].image.capture, "the capture is kept")
scheduler.run(1000000)
assert(checkpoint.snapshot("capture"), "quick checkpoint saved")
quit()
