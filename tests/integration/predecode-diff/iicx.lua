-- Converted from iicx.script by scripts/script-to-lua.py
-- IIcx (68030 + PMMU), System 6 HD with Marathon: three checkpoints.
predecode.enabled = $PD
predecode.elide = $ELIDE
machine.boot{model = "iicx", ram = 8192, rom = vars.ROM}
machine.rtc.time = 3000000000
machine.scsi.attach_hd(vars.HD, 0)
scheduler.run(20000000)
echo(F"cp1 instr=${scheduler.instr_count} pc=${machine.cpu.pc:08x} sr=${machine.cpu.sr:04x}")
assert(checkpoint.save(F"${vars.OUT}-1.gs"), "save 1 failed")
scheduler.run(80000000)
echo(F"cp2 instr=${scheduler.instr_count} pc=${machine.cpu.pc:08x} sr=${machine.cpu.sr:04x}")
assert(checkpoint.save(F"${vars.OUT}-2.gs"), "save 2 failed")
scheduler.run(200000000)
echo(F"cp3 instr=${scheduler.instr_count} pc=${machine.cpu.pc:08x} sr=${machine.cpu.sr:04x}")
assert(checkpoint.save(F"${vars.OUT}-3.gs"), "save 3 failed")
echo(F"stats blocks=${predecode.blocks} allocs=${predecode.allocs} decodes=${predecode.decodes} inval=${predecode.invalidations} demotions=${predecode.demotions} suppressed=${predecode.suppressed_writes} elided=${predecode.elided}")
quit()
