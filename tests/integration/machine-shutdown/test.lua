-- Converted from test.script by scripts/script-to-lua.py
-- machine.shutdown — switch the machine off for good.
--
-- The contract under test:
--   * shutdown stops the machine and tears it down: machine.created is false,
--     the hardware nodes and the machine's attributes are gone, and the
--     machine is off the AppleTalk cable (node_id 0);
--   * with no machine there is nothing to shut down: a second shutdown fails;
--   * the next machine.boot builds a new machine, its drives empty.

assert(machine.created, "the harness booted a Plus")
assert(files.fd_create(F"${vars.WORK_DIR}/scratch.fd", false), "blank floppy created")
machine.floppy.drive[0].insert(F"${vars.WORK_DIR}/scratch.fd", true)
assert(machine.floppy.drive[0].disk.present, "floppy inserted")
scheduler.run(2000000)
assert(appletalk.node_id ~= 0, "the running machine is on the AppleTalk cable")

-- --- Shut down: no machine is left. -----------------------------------------
assert(machine.shutdown(), "machine.shutdown")
assert(machine.created == false, "after shutdown: machine.created is false")
assert(not gs.ok(function() return machine.id end), "after shutdown: no machine.id")
assert(not gs.ok(function() return machine.cpu.pc end), "after shutdown: no CPU")
assert(not gs.ok(function() return machine.floppy.drive[0].disk.present end), "after shutdown: no floppy drive")
assert(appletalk.node_id == 0, "after shutdown: off the AppleTalk cable")
assert(not gs.truthy(function() return machine.shutdown() end), "a second shutdown has no machine to shut down")

-- --- The next machine starts clean. ------------------------------------------
assert(machine.boot("plus", 4096, vars.ROM), "machine.boot after shutdown")
assert(machine.created, "a machine again")
assert(machine.id == "plus", "machine.id after the new boot")
assert(gs.try(function() return machine.floppy.drive[0].disk.present end, false) == false, "the new machine's drive is empty")
scheduler.run(2000000)
assert(machine.cpu.pc ~= 0, "the new machine runs")

quit()
