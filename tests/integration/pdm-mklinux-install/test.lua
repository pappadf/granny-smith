-- Converted from test.script by scripts/script-to-lua.py
local nav, untick
-- Integration test: install MkLinux DR3 onto the single Mac OS + MkLinux drive.
--
-- Half two of recreating `systems/mklinux_dr3_single_169mb.img`.
-- It starts from the drive
-- `pdm-mklinux-disk` publishes — partitioned, blessed, with the five Mac Files
-- already in its System Folder — and ends with that same drive carrying a
-- working MkLinux on /dev/sda6.
--
--   1  the Booter hands off to Mach_Kernel and the Red Hat-derived installer
--      comes up on the onboard Sonora/Ariel video
--   2  the installer's screens, keyboard-driven, to "Congratulations"
--   3  Mac OS again: point lilo.conf at the installed root and make MkLinux the
--      startup default
--   4  export, verify, publish
--
-- THE HAND-OFF is where one emulator defect stood: a BAT register write must
-- not steer instruction fetch until the processor context-synchronizes.
-- DR3's start.s writes `mtibatu 0` then `mtibatl 0` with nothing but a
-- trailing sync/isync, and the half-written pair relocates the kernel's own
-- text off the end of RAM.
--
-- EVERY SCREEN IS WAITED FOR, NOT BUDGETED FOR.  `screen()` runs in quanta
-- until the frame equals the golden and then lets the guest finish painting.
-- Both halves of that matter, and both were learned the hard way:
--
--   * A fixed budget between screens desynchronises the whole sequence the
--     moment one step is slower than measured, and REGEN then records the
--     stuck screen under the NEXT screen's name.  That is not hypothetical —
--     an earlier version of this test shipped `install-done.png` containing a
--     picture of the Partition Disks screen, and passed.
--   * A keystroke sent while newt is still repainting is dropped.  The pause
--     after each match is what makes the typing land.
--
-- KEYBOARD: DR3's ADB driver reads the arrow cluster at $3B-$3E and claims
-- $7B-$7E as the right-hand modifiers, so arrows are pressed as raw keycodes.
-- And on the Root Password screen focus starts on **Ok**, not on the field —
-- one Tab reaches it, and typing without that Tab goes nowhere at all.
--
-- MEDIA: the DR3 disc and the drive `pdm-mklinux-disk` publishes are not
-- redistributable and not part of the test data.  Both are read from
-- $GS_EXTRA_MEDIA_DIR/mklinux/media/, and this test SKIPS cleanly where they
-- are absent; the finished image is published back to the same directory.

local mac = dofile("../lib/mac.lua")

local mkl_root = F"${vars.EXTRA_MEDIA}/mklinux"
local dr3_iso = F"${mkl_root}/media/MkLinux-DR3/MkLinux-DR3.iso"
local drive = F"${mkl_root}/media/mklinux_single_macos_169mb.img"

-- --- helpers -----------------------------------------------------------------

-- Wait for the expected screen, then let it finish painting before typing.
-- Self-synchronising: a step that did not happen fails HERE, naming the screen
-- it was waiting for, instead of shifting every screen after it.
local function screen(golden, ceiling)
    mac.wait_match(golden, ceiling, 20000000)
    scheduler.run(100000000)
end

local function press(code)
    machine.adb.keyboard.press(code)
    scheduler.run(30000000)
end

-- Click `x,y` until the machine leaves the 68k address space.
--
-- The splash is only up for its ten-second countdown and when it appears
-- depends on how long the extensions parade takes, so the click has to hunt for
-- it rather than assume a moment.  What it watches for is the HAND-OFF, not a
-- screen: a 7.5 boot answers low memory and a machine running the Mach kernel
-- does not.  That is cheaper than a golden, needs no golden to exist yet (so
-- REGEN takes the same path as everything else), and — the part that matters —
-- it stops clicking the instant MkLinux takes over.
--
-- Clicking one time too many is not harmless: click() spends ~30 guest ticks,
-- and run_ticks polls the Mac OS Ticks global, which stops advancing once the
-- Booter has handed the machine over.  A click after the hand-off therefore
-- spins forever.  (Measured: a version of this loop that checked a golden
-- instead ran for ten minutes without finishing the boot.)
local function click_until_handoff(x, y)
    -- Nothing can happen before the extensions parade is done.
    scheduler.run(800000000)
    local n = 0
    while n < 12 do
        if not gs.ok(function() return machine.cpu.mmu.peek(0x17B, 1) end) then return n end
        mac.click(x, y)
        scheduler.run(120000000)
        n = n + 1
    end
    assert(false, "twelve clicks at the splash and the machine is still in the 68k address space — the Booter never handed off")
    return n
end

local function menu_pick(title_x, title_y, item_x, item_y)
    machine.adb.mouse.move(100, 300, "global")
    mac.run_ticks(20)
    machine.adb.mouse.move(title_x, title_y, "global")
    mac.run_ticks(30)
    machine.adb.mouse.click(true)
    mac.run_ticks(40)
    machine.adb.mouse.move(item_x, item_y, "global")
    mac.run_ticks(30)
    machine.adb.mouse.click(false)
    mac.run_ticks(60)
end

local function cmd_key(code)
    machine.adb.keyboard.down("command")
    mac.run_ticks(6)
    machine.adb.keyboard.press(code)
    mac.run_ticks(6)
    machine.adb.keyboard.up("command")
    mac.run_ticks(20)
end

-- --- phase 1: into the installer ---------------------------------------------

local function phase1_hand_off()
    -- The drive arrives with the map pdm-mklinux-disk wrote and nothing else.
    -- Read BEFORE the attach — the VFS will not open an image the running
    -- machine holds.
    local parts = files.list(drive)
    assert(#parts == 8, F"the published drive has ${#parts} partitions, want 8")
    -- Phase 4 compares the file it edits against the disc's pristine copy, and
    -- by then the disc is in the drive.
    files.cp(tostring(dr3_iso) .. "/partition1/Mac Files/lilo.conf", F"${vars.WORK_DIR}/lilo-pristine.conf")

    machine.boot{model = "pm7100", ram = 40960, rom = vars.ROM}
    machine.rtc.time = 0xE1100000
    machine.scsi.attach_hd(drive, 0)
    machine.scsi.attach_cdrom(dr3_iso, 3)
    scheduler.run(360000000)

    -- CLICK UNTIL IT TAKES.  The splash is only up for its ten-second
    -- countdown, and when it appears depends on how long the extensions parade
    -- takes — which changed the moment this drive gained the MkLinux Booter.
    -- A fixed budget put the click on the "Welcome to Macintosh" progress bar
    -- (measured), the splash then timed out into Mac OS, and every screen after
    -- it was a Finder desktop.  So aim at `Boot MkLinux` repeatedly: on the
    -- boot screen and on an empty desktop the click does nothing, and on the
    -- splash it does the one thing wanted.
    local clicks = click_until_handoff(139, 177)
    echo(F"the Booter handed off after ${clicks} click(s) at the splash")

    -- THE HAND-OFF, ASSERTED BEFORE ANY MORE GOLDENS ARE TAKEN.  Under REGEN a
    -- desynchronised run records whatever is on screen under the next screen's
    -- name, twenty times over, and passes.  This cannot be faked: a 7.5 boot
    -- answers low memory and a machine running the Mach kernel does not, so if
    -- the click never landed the run stops here instead of shipping a picture
    -- of the Finder as `install-done.png`.
    screen("installer-welcome.png", 4000000000)

    -- The baseline for "did the install reach the disk".  It is taken HERE, not
    -- against the published drive: Mac OS mounts its own HFS partition on the
    -- way through and writes volume metadata, so a comparison against the
    -- published image is satisfied by Mac OS alone (measured: first difference
    -- at 361479, inside partition 5).  From this instant on, only MkLinux runs.
    machine.scsi.device[0].image.export(F"${vars.WORK_DIR}/before-install.img")
    mac.report_perf("pdm-mklinux-install/phase1", scheduler.instr_count)
end

-- --- phase 2: the installer --------------------------------------------------

local function phase2_installer()
    press("Return")
    screen("keyboard-type.png", 2000000000)
    press("Return") -- `us` is preselected
    screen("installation-method.png", 2000000000)
    press("Return") -- Local CDROM
    -- A screen that is easy to miss and cost this test a day: choosing the CD
    -- raises a Note before the path question, so a sequence built by counting
    -- Returns lands one screen late from here on.
    screen("insert-cd-note.png", 4000000000)
    press("Return")
    screen("installation-path.png", 6000000000)
    press("Return") -- Install, not Upgrade
    -- One drive, and MkLinux's own SCSI driver names it.  Nothing is edited
    -- here — DR3's installer cannot partition from this screen at all, and
    -- does not need to.
    screen("partition-disks.png", 3000000000)

    press("Return") -- Done
    -- One candidate, /dev/sda6, and the partition named `swap` correctly
    -- withheld — the installer keys swap off the NAME.
    screen("select-root.png", 3000000000)
    press("Return")
    screen("mount-points.png", 3000000000)
    press("Tab")
    press("Return")
    -- /dev/sda7 arrives already ticked.
    screen("active-swap.png", 3000000000)
    press("Tab")
    press("Return")
    screen("format-partitions.png", 4000000000)

    -- The root filesystem has never held MkLinux, so it must be formatted; the
    -- box starts clear and Space ticks it.
    press("space")
    press("Tab")
    press("Return")
    screen("components.png", 4000000000)

    -- "ABSOLUTE MINIMUM" is DR3's own name for unchecking every option on this
    -- page, and there is no button for it — the eight checked by default are
    -- rows 2, 3, 4, 6, 10, 11, 13, 14 of twenty-eight.  0x3D is DR3's down
    -- arrow.  58 MB installed instead of 188, which is what keeps the finished
    -- image at 89 MiB of non-zero bytes.
    untick(1) -- X Window System
    untick(1) -- Mail/WWW/News Tools
    untick(1) -- Mac Utilities
    untick(2) -- File Managers
    untick(4) -- X multimedia support
    untick(1) -- Console Multimedia
    untick(2) -- Networked Workstation
    untick(1) -- Dialup Workstation
    nav(0x3E, 13) -- back to the top so the golden is taken from a fixed row
    mac.check("components-minimum.png")

    press("Tab") -- -> "Select individual packages"
    press("Tab") -- -> Ok
    press("Return")
    screen("install-log.png", 3000000000)

    -- THE LONG ONE: 58 MB of packages.  A generous ceiling, and the screen it
    -- waits for is Configure Mouse with `ADB Mouse` already on the right line —
    -- the README's instruction and the installer's own default agreeing.
    press("Return")
    screen("configure-mouse.png", 12000000000)
    mac.report_perf("pdm-mklinux-install/packages", scheduler.instr_count)

    press("Tab") -- -> "Emulate 3 Buttons?"
    press("Tab") -- -> Ok
    press("Return")
    -- Network Configuration has no golden of its own: it carries nothing this
    -- test wants to assert, and a golden is a file somebody has to inspect
    -- forever after.  Driven blind — Tab moves Yes -> No — and the timezone
    -- screen below is the sync point.  If the count is ever wrong the wait
    -- there fails loudly, which is the only outcome that matters.
    scheduler.run(600000000)
    press("Tab")
    press("Return")
    -- "Hardware clock set to GMT" is what makes MkLinux read the Mac OS clock
    -- correctly; focus starts on it, so Space ticks it directly.
    screen("timezone.png", 3000000000)
    press("space")
    press("Tab")
    press("Tab")
    press("Return")
    -- Services (defaults are fine) and Configure Printer (No) — both blind for
    -- the same reason as Network Configuration; root-password is the sync point.
    scheduler.run(600000000)
    press("Tab")
    press("Return")
    scheduler.run(600000000)
    press("Tab")
    press("Return")
    screen("root-password.png", 4000000000)

    -- FOCUS STARTS ON Ok, NOT ON THE FIELD.  One Tab reaches Password; typing
    -- without it goes nowhere and the form comes back "Password Mismatch".
    press("Tab")
    machine.adb.keyboard.type("mklinux")
    scheduler.run(80000000)
    press("Tab")
    machine.adb.keyboard.type("mklinux")
    scheduler.run(80000000)
    press("Tab")
    press("Return")
    -- THE GOLDEN THAT RECORDS THE ANSWER: "Set rootdev equal to /dev/sda6".
    screen("info-rootdev.png", 3000000000)
    press("Return")
    screen("install-done.png", 3000000000)

    -- Return here shuts the system down: termination signals, kill signals,
    -- `unmounting filesystems`, then `MACH Reboot`.  The unmount is what makes
    -- the artifact trustworthy, so the phase runs it and asserts what happens
    -- next rather than avoiding it.
    --
    -- KNOWN GAP, asserted rather than papered over: the restart does not
    -- happen.  powermac_reboot() sends Cuda pseudo-command $11 (RESET_SYSTEM)
    -- and panics if it ever returns; granny-smith's Cuda accepts $11 but only
    -- clears its own autopoll/tick state, so the kernel comes back and prints
    -- `panic: system should have restarted`.  Power-cycling from inside a
    -- device callback needs a deferred-restart seam the emulator does not have
    -- yet.  Until it does, this golden is the truthful record of what the
    -- machine does, and the phase power-cycles it itself.
    press("Return")
    screen("mach-reboot-panic.png", 4000000000)

    -- THE INSTALL HAS TO HAVE REACHED THE DISK, and nothing above proves it:
    -- every screen is the guest's own drawing, and a golden of
    -- "Congratulations" says only that the installer believed itself.  The root
    -- partition starts at block 82624, byte 42303488 — the first difference
    -- from the drive we booted must be at or past it, because everything before
    -- it is Mac OS's and the installer has no business there.
    machine.scsi.device[0].image.export(F"${vars.WORK_DIR}/after-install.img")
    local wrote = files.path_compare(F"${vars.WORK_DIR}/before-install.img", F"${vars.WORK_DIR}/after-install.img")
    echo(F"the drive first differs from its pre-install state at ${wrote}")
    assert(wrote >= 42303488, F"the install first changed byte ${wrote}, which is before the root partition at 42303488 — it did not install where it said it would")
end

-- Move `n` rows in the component list.
function nav(code, n)
    for i = 0, n - 1 do
        machine.adb.keyboard.press(code)
        scheduler.run(15000000)
    end
end

-- Move down `steps` rows and untick that component.
function untick(steps)
    nav(0x3D, steps)
    machine.adb.keyboard.press("space")
    scheduler.run(40000000)
end

-- --- phase 3: point lilo.conf at the installed root --------------------------

local function phase3_lilo()
    -- The power cycle DR3 asked for and could not have.  machine.restart tears
    -- nothing down, so the media stay attached as the same open handles and
    -- every write above survives it by construction.
    machine.restart()
    scheduler.run(360000000)
    scheduler.run(900000000)
    mac.click(503, 177) -- Boot MacOS
    scheduler.run(1500000000)
    mac.wait_stable(2000000000)
    mac.check_ex("macos-after-install.png", 0, 440, 20, 640)

    -- Apple menu -> Control Panels -> MkLinux, in one press-move-release.
    -- Deliberately NOT through a Finder window: what those look like is a
    -- property of the volume's history, and the Apple menu's submenu is
    -- alphabetical and independent of every window on screen.
    machine.adb.mouse.move(100, 300, "global")
    mac.run_ticks(20)
    machine.adb.mouse.move(20, 9, "global")
    mac.run_ticks(30)
    machine.adb.mouse.click(true)
    mac.run_ticks(50)
    machine.adb.mouse.move(100, 132, "global") -- Control Panels (submenu opens)
    mac.run_ticks(60)
    machine.adb.mouse.move(250, 443, "global") -- MkLinux
    mac.run_ticks(40)
    machine.adb.mouse.click(false)
    scheduler.run(900000000)
    mac.wait_stable(2000000000)
    mac.check_ex("control-panel.png", 0, 440, 20, 640)

    -- "Custom..." launches SimpleText on lilo.conf — the Booter's own
    -- documented way of editing it.  A FIXED budget and no settle: measured
    -- once with a settle and it worked, measured again after a full install in
    -- front of it and the settle burned its whole ceiling.
    mac.click(274, 181)
    scheduler.run(2000000000)
    mac.check_ex("lilo-conf-open.png", 0, 440, 20, 640)

    -- The last line reads `rootdev=/dev/scd0`.  Clicking past its end puts the
    -- caret at the end of the line; four Deletes take `scd0` off and `sda6`
    -- goes on, which is what the Info screen told us to write.
    mac.click(300, 435)
    mac.run_ticks(20)
    press("delete")
    press("delete")
    press("delete")
    press("delete")
    machine.adb.keyboard.type("sda6")
    mac.run_ticks(20)
    mac.check_ex("lilo-conf-edited.png", 0, 440, 20, 640)

    cmd_key(0x01) -- save
    scheduler.run(300000000)
    cmd_key(0x0C) -- quit SimpleText
    scheduler.run(1200000000)

    mac.click(54, 170) -- Startup Selection: MkLinux, so the boot test runs hands-free
    mac.run_ticks(30)
    mac.check_ex("startup-mklinux.png", 0, 440, 20, 640)
    cmd_key(0x0D) -- close the control panel
    scheduler.run(600000000)
end

-- --- phase 4: flush, verify, publish -----------------------------------------

local function phase4_publish()
    -- Shut Down before reading anything: the control panel's writes to
    -- lilo.conf and MkLinux.prefs are in the guest's cache until the volume is
    -- flushed.
    menu_pick(233, 9, 250, 140)
    scheduler.run(600000000)
    machine.scsi.device[0].image.export(F"${vars.WORK_DIR}/mklinux-single.img")

    -- The map is untouched — the installer formatted INSIDE the partitions.
    files.partmap(F"${vars.WORK_DIR}/mklinux-single.img")
    local parts = files.list(F"${vars.WORK_DIR}/mklinux-single.img")
    assert(#parts == 8, F"the installed drive has ${#parts} partitions, want 8")

    -- The two files the boot test depends on, read back from the exported
    -- volume rather than trusted from a golden of the editor.  Byte-compared
    -- against the disc's pristine copy rather than searched: `files.cat` prints a
    -- file, it does not return one, and the shell has no substring operator.
    -- `files.path_compare` answers -1 for identical and otherwise the FIRST
    -- differing offset — which pins not just that the file changed but where.
    local p5 = F"${vars.WORK_DIR}/mklinux-single.img/partition5/System Folder/Preferences"
    files.cp(F"${p5}/lilo.conf", F"${vars.WORK_DIR}/lilo-edited.conf")
    files.cp(F"${p5}/MkLinux.prefs", F"${vars.WORK_DIR}/prefs-edited")
    files.cat(F"${vars.WORK_DIR}/lilo-edited.conf")
    files.cat(F"${vars.WORK_DIR}/prefs-edited")

    -- `scd0` -> `sda6` is length-preserving, and offset 1045 is the third
    -- character of the device name on the file's last real line — the only byte
    -- in 1110 the edit was allowed to touch first.
    assert(files.path_size(F"${vars.WORK_DIR}/lilo-edited.conf") == 1110, F"lilo.conf is ${files.path_size(F\"${vars.WORK_DIR}/lilo-edited.conf\")} bytes, want 1110 (the rootdev edit is length-preserving)")
    local lilo_diff = files.path_compare(F"${vars.WORK_DIR}/lilo-pristine.conf", F"${vars.WORK_DIR}/lilo-edited.conf")
    assert(lilo_diff == 1045, F"lilo.conf first differs from the disc's copy at ${lilo_diff}, want 1045 (inside `rootdev=/dev/s..`)")
    -- `bootos=MacOS` -> `bootos=MkLinux` is two bytes longer.
    assert(files.path_size(F"${vars.WORK_DIR}/prefs-edited") == 101, F"MkLinux.prefs is ${files.path_size(F\"${vars.WORK_DIR}/prefs-edited\")} bytes, want 101 (99 + `MacOS`->`MkLinux`)")

    files.cp(F"${vars.WORK_DIR}/mklinux-single.img", tostring(mkl_root) .. "/media/mklinux_dr3_single_169mb.img")
    assert(files.path_exists(tostring(mkl_root) .. "/media/mklinux_dr3_single_169mb.img"), "the finished image was not published")
end

if mac.have_media(dr3_iso) and mac.have_media(drive) then
    phase1_hand_off()
    phase2_installer()
    phase3_lilo()
    phase4_publish()
    mac.report_row("DR3", "ariel", "mklinux-install mach-handoff redhat-installer lilo-conf")
else
    mac.skip_test("pdm-mklinux-install (the DR3 disc or the built drive not present — set GS_EXTRA_MEDIA_DIR)")
end

quit()
