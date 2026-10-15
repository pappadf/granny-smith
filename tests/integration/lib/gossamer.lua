-- Converted from gossamer.script by scripts/script-to-lua.py
local mac = dofile("mac.lua") -- NOTE: no direct mac.script include (this file is included by another); the library runs once per chunk
-- tests/integration/lib/gossamer.script — shared helpers for the beige
-- Power Macintosh G3 (Gossamer) rows.
--
-- Pull in with `include "../lib/gossamer.script"`.
--
-- REACHING OPEN FIRMWARE.  A beige G3 boots straight through Open Firmware
-- into Mac OS (`auto-boot?` defaults to true).  Apple's way in is to hold
-- Command-Option-O-F on the ADB keyboard from power-on, and that is what
-- the rows do: the keys go down before the first instruction and stay down
-- until the prompt appears.  Two pieces of the model carry it, both of them
-- real-hardware behaviour the firmware depends on:
--
--   * the keyboard re-reports every key still held after the ROM's ADB bus
--     reset, in the order the keys went down (adb.c, kbd_rereport_held) —
--     the firmware builds its key map only from key-downs it sees after its
--     own reset, and tests the combination on F;
--   * Cuda keeps an autopoll packet the host has not claimed yet at the head
--     of its output queue (cuda.c, cuda_present_parked_autopoll) — the
--     firmware turns autopoll on and then finishes its ADB probe before it
--     reads, and the [Command Option] packet has to survive that.
--
-- THE CONSOLE.  Open Firmware's console is the Rage Pro's screen and the
-- ADB keyboard.  The firmware's banner says how to move it ("For Open
-- Firmware serial I/O type: TTYA IO"), and the rows do exactly that, on
-- the keyboard.  From then on commands go in over the serial port, a line
-- at a time, paced by the line itself (gos_send).

-- Type command text on Open Firmware's console (ttya), a whole line at a
-- time: the serial line delivers it one character time apart, which is what
-- the firmware's input loop needs (it drains every available byte before it
-- echoes, so a burst overflowed its buffer -- the reason the rows once sent
-- seven characters and ran 5 M after each).  Returns when the firmware has
-- read it all.
function gos_send(s) -- global: included file
    mac.serial_type("a", s, 100000000)
end

-- Run until the firmware prints its prompt; return everything it said.
-- Long listings page at the console's height with `More [<space>,<cr>,q] ?`,
-- which a space answers.  The prompt is back within a few hundred thousand
-- instructions of a short command, so the slices start at 250 K and double
-- (to 16 M) while a long reply is still coming; `chunks` 20 M slices'
-- worth is the hang detector.
function gos_wait_prompt(chunks) -- global: included file
    local out = ""
    local spent = 0
    local slice = 250000
    while not gs.contains(out, "0 > ") and spent < chunks * 20000000 do
        scheduler.run(slice)
        spent = spent + slice
        local ch = machine.scc.a.sent()
        out = F"${out}${ch}"
        if gs.contains(ch, "More [") then
            machine.scc.a.receive(" ")
            slice = 250000
        elseif slice < 16000000 then
            slice = slice * 2
        end
    end
    return out
end

-- Complete the line and return the firmware's reply up to its next prompt.
function gos_go() -- global: included file
    machine.scc.a.receive("\r")
    local out = gos_wait_prompt(100)
    assert(gs.contains(out, "0 > "), "Open Firmware did not return to its prompt")
    return out
end

-- True while the CPU is executing Open Firmware (its image in RAM at
-- $FF800000 as the boot program relocates it) rather than the boot
-- program ($FFF0xxxx) or the 68k emulator ($FFExxxxx).
function gos_in_of() -- global: included file
    return machine.cpu.pc >= 0xFF800000 and machine.cpu.pc < 0xFFC00000
end

-- Boot `model` holding Command-Option-O-F, and leave the machine at the
-- Open Firmware prompt with the console on ttya.  Returns the prompt reply.
--
-- With the on-board Rage Pro, Open Firmware's console is the SCREEN, so
-- the prompt does not appear on the serial port: the row waits for the
-- boot to settle in Open Firmware (two consecutive samples inside it,
-- past the probe), then types `ttya io` — the command the banner offers
-- — on the keyboard, and checks the prompt over serial.
function gos_boot_prompt(model, ram_kb) -- global: included file
    machine.boot{model = model, ram = ram_kb, rom = vars.ROM}
    machine.adb.keyboard.down("Command")
    machine.adb.keyboard.down("Option")
    machine.adb.keyboard.down("o")
    machine.adb.keyboard.down("f")
    scheduler.run(400000000)
    local settled = 0
    local n = 0
    while settled < 2 and n < 20 do
        scheduler.run(100000000)
        if gos_in_of() then
            settled = settled + 1
        else
            settled = 0
        end
        n = n + 1
    end
    machine.adb.keyboard.up("f")
    machine.adb.keyboard.up("o")
    machine.adb.keyboard.up("Option")
    machine.adb.keyboard.up("Command")
    assert(gos_in_of(), F"${model}: Command-Option-O-F did not stop the boot in Open Firmware (pc ${machine.cpu.pc:08x})")
    scheduler.run(20000000)
    machine.adb.keyboard.type("ttya io\n")
    scheduler.run(200000000)
    local drained = machine.scc.a.sent()
    return gos_go()
end

-- --- Mac OS 9.2.1 install from its CD -------------------------------------------
--
-- From the CD's Finder desktop (the disc's window open, the blank disk the
-- first entry in Drive Setup's list): Utilities -> Drive Setup, initialize
-- the blank disk as one Mac OS partition, quit, then Mac OS Install through
-- its Continue screens and the license to Start, and the ~40 G-instruction
-- copy.  Leaves the installer's "installation process has finished" dialog
-- up and checks it against `golden` (menu-bar clock excluded).  Every
-- coordinate is the 640x480 desktop the Rage Pro comes up in.
function gos_install_921(golden) -- global: included file
    mac.double_click(318, 212) -- Utilities
    mac.run_ticks(240)
    mac.double_click(187, 180) -- Drive Setup
    mac.run_ticks(1500)
    mac.click(220, 143) -- the first drive: the blank disk
    mac.run_ticks(120)
    mac.click(414, 287) -- Initialize...
    mac.run_ticks(600)
    mac.click(427, 278) -- Initialize (confirm)
    mac.run_ticks(3000)
    machine.adb.keyboard.down("command")
    mac.run_ticks(6)
    machine.adb.keyboard.press("q") -- quit Drive Setup
    mac.run_ticks(6)
    machine.adb.keyboard.up("command")
    mac.run_ticks(240)
    mac.click(23, 58) -- close Utilities
    mac.run_ticks(120)
    mac.double_click(193, 152) -- Mac OS Install
    mac.run_ticks(1500)
    -- The screens up to Install Software: Continue (bottom right) four times,
    -- then the license dialog's Agree.
    mac.click(514, 383)
    mac.run_ticks(600)
    mac.click(514, 383)
    mac.run_ticks(900)
    mac.click(514, 383)
    mac.run_ticks(900)
    mac.click(514, 383)
    mac.run_ticks(600)
    mac.click(449, 306)
    mac.run_ticks(900)
    mac.click(514, 383) -- Start
    scheduler.run(40000000000)
    mac.check_ex(golden, 0, 440, 20, 640)
end
