-- plus-moof-track-content, quick-checkpoint step 2: the file now holds another
-- capture with the same size and time stamp, so only the capture's
-- fingerprint tells them apart.  A quick checkpoint depends on its base:
-- refused.
machine.register("00000000000a0c10", "20261126T000000Z")
assert(checkpoint.probe(), "the quick checkpoint is there")
assert(gs.try(function() return checkpoint.load() end, false) == false, "a changed capture is refused")
echo("changed-capture-refused")
quit()
