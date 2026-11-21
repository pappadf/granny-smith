-- Converted from av.script by scripts/script-to-lua.py
-- Quadra 840AV (68040 + 040 MMU, DSP3210): System 7.1 boot from the AV
-- image, three checkpoints.  The row that found the ROM-overlay alias: the
-- ROM runs at zero first and at $4080xxxx after, through the same host pages.
predecode.enabled = $PD
predecode.elide = $ELIDE
machine.boot{model = "q840av", ram = 16384, rom = vars.ROM}
-- Pinned after the boot: only the PDM family adopts a pre-boot seed.
machine.rtc.time = 3000000000
machine.scsi.attach_hd(vars.HD, 0)
scheduler.run(50000000)
echo(F"cp1 instr=${scheduler.instr_count} pc=${machine.cpu.pc:08x} sr=${machine.cpu.sr:04x}")
assert(checkpoint.save(F"${vars.OUT}-1.gs"), "save 1 failed")
scheduler.run(250000000)
echo(F"cp2 instr=${scheduler.instr_count} pc=${machine.cpu.pc:08x} sr=${machine.cpu.sr:04x}")
assert(checkpoint.save(F"${vars.OUT}-2.gs"), "save 2 failed")
scheduler.run(300000000)
echo(F"cp3 instr=${scheduler.instr_count} pc=${machine.cpu.pc:08x} sr=${machine.cpu.sr:04x}")
assert(checkpoint.save(F"${vars.OUT}-3.gs"), "save 3 failed")
echo(F"stats blocks=${predecode.blocks} allocs=${predecode.allocs} decodes=${predecode.decodes} inval=${predecode.invalidations} demotions=${predecode.demotions} suppressed=${predecode.suppressed_writes} realiases=${predecode.realiases} generic=${predecode.generic_steps}")
quit()
