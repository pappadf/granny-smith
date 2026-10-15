-- Converted from q7.script by scripts/script-to-lua.py
local mac = dofile("../lib/mac.lua")
local hd_75 = "../../data/systems/system_7_5_0_77mb_mode32_24ac.img"
local hd_76 = "../../data/systems/system_7_6_170mb_24ac.img"

echo("--- A: 7.5 from default PRAM (expect 24-bit)")
machine.boot{model = "iici", ram = 16384, video_card = "builtin_rbv_video", rom = vars.ROM}
machine.rtc.time = 3000000000
machine.scsi.attach_hd(hd_75, 0)
scheduler.run(300000000)
wait_stable(1500000000)
echo(F"A MMU32bit=${debug.mac.globals.read(\"MMU32bit\")}")
echo(F"A PRAM=${machine.rtc.pram.snapshot()}")

echo("--- B: 7.6, which selects 32-bit itself")
machine.boot{model = "iici", ram = 32768, video_card = "builtin_rbv_video", rom = vars.ROM}
machine.scsi.attach_hd(hd_76, 0)
scheduler.run(400000000)
wait_stable(1800000000)
echo(F"B MMU32bit=${debug.mac.globals.read(\"MMU32bit\")}")
echo(F"B PRAM=${machine.rtc.pram.snapshot()}")

echo("--- C: 7.5 again, PRAM inherited from the 7.6 boot")
machine.boot{model = "iici", ram = 16384, video_card = "builtin_rbv_video", rom = vars.ROM}
machine.scsi.attach_hd(hd_75, 0)
scheduler.run(300000000)
wait_stable(1500000000)
echo(F"C MMU32bit=${debug.mac.globals.read(\"MMU32bit\")}")
echo(F"C PRAM=${machine.rtc.pram.snapshot()}")
quit()
