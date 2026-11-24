-- Converted from test.script by scripts/script-to-lua.py
-- IIci: Mac OS 7.1 -> Penguin 19 -> Debian 3.1 "sarge" (Linux 2.2.25).
--
-- Nothing is clicked.  Mac OS opens `Startup Items:Boot Linux` (Penguin's saved
-- settings, "Auto Boot On Prefs File Open"), Penguin loads vmlinuz-2.2 with
-- `root=/dev/sdb2 ro`, and sysvinit brings up a console login on the 8*24.
-- The row logs in as root and runs `startx`: XFree86 4.3 fbdev at 8 bpp with
-- twm, an xterm and xclock (root's .xinitrc).
--
-- The RTC is pinned after machine.boot: Linux 2.2 takes its clock from it,
-- prints it on the console ("System Clock set. Local time: ...") and xclock
-- draws it, so both goldens depend on it.

local mac = dofile("../lib/mac.lua")

local D = "../../data/systems"

local function row_iici_sarge()
    machine.boot{model = "iici", ram = 32768, rom = vars.ROM, config = "{\"options\":{\"addressing\":\"32\"},\"cards\":[{\"slot\":\"nubus_c\",\"card\":\"mdc_8_24\"}],\"displays\":{\"builtin\":{\"monitor\":\"none\"},\"nubus_c\":{\"monitor\":\"13in_rgb\",\"mode\":\"640x480x8\"}}}"}
    machine.rtc.time = "2026-04-26T21:30:36"
    machine.scsi.attach_hd(tostring(D) .. "/system_7_1_20mb_24ac_cd_32bit_penguin_sarge.dmg", 0)
    machine.scsi.attach_hd(tostring(D) .. "/debian_3_1_512mb_iici.dmg", 1)
    assert(machine.id == "iici", "machine.id != iici")

    -- The login prompt: measured at 0.85 G instructions.  The ceiling is
    -- a hang detector; under REGEN the golden is captured at the ceiling.
    local spent = mac.run_until_match("sarge-login.png", 3000000000, 50000000)
    echo(F"iici-debian-sarge: login prompt after ${spent} instructions")
    mac.check("sarge-login.png")
    assert(machine.screen.depth == 8, F"iici-debian-sarge: ${machine.screen.depth} bpp, want 8")
    mac.report_row_foreign("Debian 3.1", "mdc_8_24", "linux-2.2 console login")

    -- Log in (root/debian) and start X.  Each line runs until the keys are
    -- taken and the console holds still.
    mac.type_settle("root\n", 10000000, 300000000)
    mac.type_settle("debian\n", 10000000, 1500000000)
    machine.adb.keyboard.type("startx\n")
    mac.wait_keys_taken(300000000)
    mac.run_until_match("sarge-x11.png", 4000000000, 50000000)
    mac.check("sarge-x11.png")
    mac.report_row_foreign("Debian 3.1", "mdc_8_24", "xfree86 fbdev twm")
    mac.report_perf("iici-debian-sarge/x11", scheduler.instr_count)
end

mac.row("iici-debian-sarge", row_iici_sarge)
mac.suite_done()
quit()
