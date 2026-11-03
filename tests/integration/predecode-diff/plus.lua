-- Converted from plus.script by scripts/script-to-lua.py
-- Plus, System 6.0.8 floppy: three checkpoints through the boot.
predecode.enabled = $PD
predecode.elide = $ELIDE
machine.boot{model = "plus", ram = 2048, rom = vars.ROM}
machine.rtc.time = 3000000000
machine.floppy.drive[0].insert(vars.FD)
scheduler.run(10000000)
echo(F"cp1 instr=${scheduler.instr_count} pc=${machine.cpu.pc:08x} sr=${machine.cpu.sr:04x}")
assert(checkpoint.save(F"${vars.OUT}-1.gs"), "save 1 failed")
scheduler.run(40000000)
echo(F"cp2 instr=${scheduler.instr_count} pc=${machine.cpu.pc:08x} sr=${machine.cpu.sr:04x}")
assert(checkpoint.save(F"${vars.OUT}-2.gs"), "save 2 failed")
scheduler.run(150000000)
echo(F"cp3 instr=${scheduler.instr_count} pc=${machine.cpu.pc:08x} sr=${machine.cpu.sr:04x}")
assert(checkpoint.save(F"${vars.OUT}-3.gs"), "save 3 failed")
echo(F"stats blocks=${predecode.blocks} allocs=${predecode.allocs} decodes=${predecode.decodes} inval=${predecode.invalidations} demotions=${predecode.demotions} suppressed=${predecode.suppressed_writes} elided=${predecode.elided}")
quit()
