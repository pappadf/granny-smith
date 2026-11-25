-- Quadra 950: Mac OS 7.1 -> Penguin 19 -> Debian 12-era m68k (Linux 6.1.27).
--
-- Nothing is clicked.  Mac OS opens `Startup Items:Boot Linux`, Penguin loads
-- vmlinux-6.1 and initrd-6.1 with `root=/dev/sdb2 console=ttyS0,9600
-- console=tty0`, systemd starts a root autologin on the modem port and LightDM
-- logs `debian` in to XFCE (Xorg fbdev at depth 15).
--
-- The serial console is the assertion channel: the guest is asked to print
-- markers built by shell expansion (`SYS-$(...)`), so the echo of the typed
-- line never contains the expected reply.  The screen is checked once at the
-- end, with the panel clock masked.

local mac = dofile("../lib/mac.lua")

local D = "../../data/systems"

-- Type `line` on the modem port and run until the shell prompts again;
-- returns everything the guest printed on the way (the echo included).
local function ask(line, ceiling)
    mac.serial_type("a", F"${line}\r", 200000000)
    return mac.run_until_sent("a", "root@quadra:~#", 20000000, ceiling)
end

local function row_q950_bookworm()
    machine.boot{model = "q950", ram = 262144, rom = vars.ROM, config = "{\"options\":{\"addressing\":\"32\"},\"displays\":{\"builtin\":{\"monitor\":\"13in_rgb\",\"mode\":\"640x480x16\"}}}"}
    machine.rtc.time = "2026-04-26T21:30:36"
    machine.scsi.attach_hd(tostring(D) .. "/system_7_1_20mb_24ac_cd_32bit_penguin_bookworm.dmg", 0)
    machine.scsi.attach_hd(tostring(D) .. "/debian_12_2gb_q950_xfce.dmg", 1)
    assert(machine.id == "q950", "machine.id != q950")

    -- Root's shell on ttyS0: measured at 7.3 G instructions.
    local out = mac.run_until_sent("a", "root@quadra:~#", 20000000, 30000000000)
    assert(gs.contains(out, "root@quadra:~#"), "q950-debian-bookworm: no root shell on the serial console")
    assert(gs.contains(out, "Linux version 6.1"), "q950-debian-bookworm: the 6.1 kernel's banner is missing")
    echo(F"q950-debian-bookworm: serial root shell after ${scheduler.instr_count} instructions")

    -- systemd reached its target with nothing failed.
    local sys = ask("echo SYS-$(systemctl is-system-running --wait)", 20000000000)
    assert(gs.contains(sys, "SYS-running"), F"q950-debian-bookworm: systemd is not 'running': ${sys}")

    -- LightDM's autologin brought up the XFCE session, xfdesktop included
    -- (the process GLib's g_once bug killed).
    local xfd = ask("for i in $(seq 300); do pgrep -x xfdesktop >/dev/null && break; sleep 1; done; echo XFD-$(pgrep -cx xfdesktop)", 30000000000)
    assert(gs.contains(xfd, "XFD-1"), F"q950-debian-bookworm: xfdesktop is not running: ${xfd}")

    -- xfce4-terminal on the session's display stays up (it died in
    -- libfontconfig when FMOVEM -(SP) faulted on a new stack page).
    local term = ask("su debian -c 'DISPLAY=:0 XAUTHORITY=/home/debian/.Xauthority setsid xfce4-terminal >/dev/null 2>&1 &'; sleep 60; echo TERM-$(pgrep -cx xfce4-terminal)", 20000000000)
    assert(gs.contains(term, "TERM-1"), F"q950-debian-bookworm: xfce4-terminal is not running: ${term}")

    -- The desktop: panel, dock, the File System and Home icons, and the
    -- terminal window.  X draws it well after the session's processes are up
    -- (the screen is black until about 16 G instructions; measured), so this
    -- waits for the golden.  The panel clock (top right) is masked: it moves
    -- every guest minute, and it does not follow the RTC (it reads 2023).
    mac.run_until_match_ex("xfce-desktop.png", 0, 500, 28, 600, 20000000000, 200000000)
    mac.check_ex("xfce-desktop.png", 0, 500, 28, 600)
    assert(machine.screen.depth == 16, F"q950-debian-bookworm: ${machine.screen.depth} bpp, want 16")
    mac.report_row_foreign("Debian 12", "dafb", "linux-6.1 systemd xfce")
    mac.report_perf("q950-debian-bookworm/desktop", scheduler.instr_count)
end

mac.row("q950-debian-bookworm", row_q950_bookworm)
mac.suite_done()
quit()
