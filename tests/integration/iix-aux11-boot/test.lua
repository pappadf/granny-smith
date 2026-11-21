-- Converted from test.script by scripts/script-to-lua.py
-- IIx: A/UX 1.1 from an installed HD80SC.
--
-- The disk's Mac side starts sash (the Standalone Shell) as its Startup
-- program.  `launch` boots /unix from the A/UX Root partition; the kernel
-- asks whether to check the root file system (no), comes up in single-user
-- mode at a root prompt, and `init 2` takes it to multi-user, where root logs
-- in on the console (no password) and runs `uname -a`.
--
-- A/UX keeps its own clock (it says "time invalid" and counts on from the
-- time on the disk), so the dates on the console come from the disk and the
-- instruction count, not from the RTC; the RTC is pinned anyway so the Mac
-- side is reproducible.
--
-- Console waits: A/UX idles in STOP at each prompt, so a step of
-- scheduler.run retires few instructions there; type_settle() waits for the
-- typed keys to be taken and the console to hold still.

local mac = dofile("../lib/mac.lua")

local D = "../../data/systems"

local function row_iix_aux11()
    machine.boot{model = "iix", ram = 8192, rom = vars.ROM, slots = "9=mac_ii_video_card"}
    machine.rtc.time = "1989-03-01T10:00:00"
    machine.scsi.attach_hd(tostring(D) .. "/aux_1_1_80mb_iix.dmg", 0)
    assert(machine.id == "iix", "machine.id != iix")
    assert(machine.nubus.slot[9].card.declrom.crc == 0x729755F8, "slot 9 is not running Apple's 342-0008 ROM")

    -- sash's Standalone Shell window: measured at under 0.3 G instructions.
    mac.run_until_match("sash.png", 1500000000, 50000000)
    mac.check("sash.png")

    -- The kernel banner and the root file system question: about 0.1 G
    -- instructions of disk reads, then A/UX waits at the prompt.
    machine.adb.keyboard.type("launch\n")
    mac.wait_keys_taken(300000000)
    mac.run_until_match("kernel.png", 3000000000, 50000000)
    mac.check("kernel.png")
    mac.report_row_foreign("A/UX 1.1", "mac_ii_video_card", "hd-boot kernel")

    -- No fsck; single-user asks for the terminal type (mac2), then root's
    -- prompt; `init 2` asks about the other file systems (no), and the
    -- console login comes up.
    mac.type_settle("n\n", 50000000, 3000000000)
    mac.type_settle("\n", 50000000, 3000000000)
    mac.type_settle("init 2\n", 50000000, 3000000000)
    mac.type_settle("n\n", 50000000, 3000000000)

    -- Log in as root (no password), take the default terminal type, uname.
    mac.type_settle("root\n", 50000000, 3000000000)
    mac.type_settle("\n", 50000000, 3000000000)
    mac.type_settle("uname -a\n", 50000000, 3000000000)
    mac.check("uname.png")
    mac.report_row_foreign("A/UX 1.1", "mac_ii_video_card", "multi-user root login uname")
    mac.report_perf("iix-aux11-boot/uname", scheduler.instr_count)
end

mac.row("iix-aux11-boot", row_iix_aux11)
mac.suite_done()
quit()
