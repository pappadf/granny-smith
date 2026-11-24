-- Converted from test.script by scripts/script-to-lua.py
-- Cold boot of an installed AIX 4.1.5.  See config.mk for what the media is
-- and why it is gated.
--
-- S13, ON THE MONITOR.  A Network Server with AIX on the disk in bay 2
-- and the key in Normal boots it with no help: POST, Open Firmware's default
-- boot device `disk2:aix`, the kernel, cfgmgr over both fast/wide channels
-- (LCD 890 / 868), and `Console login:` — on the 54M30 graphics console,
-- which this installation made its system console (`chcons /dev/lft0`).
-- The row then logs in as root from the emulated ADB keyboard and matches
-- the shell it gets.  Both screens are pixel goldens drawn by the guest.
--
-- What the two matches prove, in order: the installed system came up
-- multi-user with its LFT console on the display; the keyboard adapter was
-- defined by cfgcuda from Cuda's device list, the keyboard attached to the
-- LFT, and a typed line went through Cuda autopoll, the kernel's ADB
-- manager, kbddd and the LFT into a login.
--
-- The image's Installation Assistant has already been completed, so no
-- first-boot prompts stand between multi-user and the login.  The banner's
-- "Last login" line is the previous login stored in the image, not the
-- clock, so it is stable.
--
-- BUDGET.  A Normal-key cold boot is slow by the machine's design — POST
-- runs its full memory test, and cfgmgr pays one selection time-out plus a
-- bus reset for every absent id on two wide buses.  The row boots with
-- 64 MB so POST has less to test, samples in 100 M-cycle strides polled
-- against the login golden (capped at 200 G), and waits for the root
-- shell's golden rather than a fixed budget after the login.

local mac = dofile("../lib/mac.lua")
dofile("../lib/ans.lua")

-- The golden lives in gs-test-data (systems/aix_4_1_5_lp240s_234mb.img.7z,
-- extracted in place by fetch-test-data.sh), like the MkLinux disks.
local aix_disk = "../../data/systems/aix_4_1_5_lp240s_234mb.img"

local function installed_boot()
    machine.boot{model = "ans500", ram = 65536, rom = vars.ROM}
    machine.board.keyswitch = "normal"
    assert(machine.board.keyswitch == "normal", "the front keyswitch did not take the Normal position")

    -- Bay 2 — Open Firmware's `disk2` alias, its default boot device.  The
    -- golden image is never written: changes go to a scratch delta.
    machine.scsi.attach_hd(aix_disk, 2)
    assert(machine.scsi.device[2].type == "hd", "the installed AIX disk is not in bay 2")
    assert(gs.contains(machine.scsi.device[2].product, "LP240S"), "the installed disk is not the LP240S the image was made on")

    local saw_scsi2 = 0
    local saw_integrated = 0
    local saw_login = 0
    for i = 0, 1999 do
        scheduler.run(100000000)
        if gs.contains(machine.lcd.text, "890") then
            saw_scsi2 = 1
        end
        if gs.contains(machine.lcd.text, "868") then
            saw_integrated = 1
        end
        -- The 54M30 has no framebuffer until Open Firmware opens it (a few
        -- G cycles in); the screen node cannot be asked before that.  The
        -- frame's top four text rows carry the subsystem PIDs (`0513-059
        -- The syslogd Subsystem has been started. Subsystem PID is 3320.`)
        -- and one of them differs from boot to boot, so the compare
        -- excludes them.
        if i >= 100 then
            if machine.screen.matches("console-login.png", 0, 0, 64, 640) then
                saw_login = 1
                break
            end
        end
    end

    -- --- S13: the installed system came up, on the monitor -----------------
    assert(saw_scsi2 == 1, "S13: the LCD never showed 890 (cfgmgr configuring the SCSI-2 adapter)")
    assert(saw_integrated == 1, "S13: the LCD never showed 868 (the integrated SCSI adapter)")
    if saw_login == 0 then
        -- Leave the frame the row did see in the work directory, for the diff.
        machine.screen.save(F"${vars.WORK_DIR}/console-login-actual.png")
    end
    assert(saw_login == 1, "S13: the graphics console never showed 'Console login:' (see console-login-actual.png in the work directory)")
    mac.check_ex("console-login.png", 0, 0, 64, 640)

    -- --- The ADB keyboard: log in as root (the install set no password) ----
    machine.adb.keyboard.type("root\n")
    mac.wait_match("root-shell.png", 3000000000, 100000000)

    -- And the disk is still the one it booted from.
    assert(machine.scsi.device[2].type == "hd", "S13: the boot disk went away under AIX")
    mac.report_perf("ans-aix-installed-boot/root-shell", scheduler.instr_count)

    echo("ans-aix-installed-boot: an installed AIX 4.1.5 cold-boots through Open Firmware's disk2:aix to 'Console login:' on the graphics console, and root logs in from the ADB keyboard (rung S13)")
end

if mac.have_media(aix_disk) then
    installed_boot()
else
    mac.skip_test("ans-aix-installed-boot (the installed AIX 4.1.5 disk image is not present — run scripts/fetch-test-data.sh)")
end

quit()
