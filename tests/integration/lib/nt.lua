-- Converted from nt.script by scripts/script-to-lua.py
local mac = dofile("mac.lua") -- NOTE: no direct mac.script include (this file is included by another); the library runs once per chunk
-- tests/integration/lib/nt.script — shared helpers for the Windows NT rows on
-- the Apple Network Server (ans-nt-install, ans-nt-installed-boot).
--
-- Pull in after mac.script and ans.script:
--
--     include "../lib/mac.script"
--     include "../lib/ans.script"
--     include "../lib/nt.script"
--
-- NT on this machine comes from the powermac-nt-hal boot floppy
-- (github.com/pappadf/powermac-nt-hal): three Open Firmware scripts on it
-- (setup.of, boot.of, bootdisk.of), each started with two typed lines,
--
--     load fd:,\<script>
--     load-base loadsize eval
--
-- on the serial console.  Until NT's kernel runs, SETUPLDR and the veneer
-- draw on ttya; from the kernel on NT draws only on the 54M30 and reads
-- only the ADB keyboard.  These helpers are that split.

-- --- Keys ---------------------------------------------------------------------

-- One ADB key, down and up in separate auto-polls.  Not keyboard.press()
-- (both transitions in one Talk R0 packet) and not keyboard.type() (4 ms
-- spacing, inside one 11 ms poll): NT's ADB path acts on the first
-- transition of a packet and drops the rest.  Codes are the Apple Extended
-- Keyboard's on the wire — the arrows in particular are 0x3B-0x3E, not the
-- 0x7B-0x7E virtual codes the Mac OS uses, which NT does not map.
function nt_key(code) -- global: included file
    machine.adb.keyboard.down(code)
    scheduler.run(3000000)
    machine.adb.keyboard.up(code)
    scheduler.run(3000000)
end

-- `n` presses of one key, with the guest given time to act on each.
function nt_keys(code, n) -- global: included file
    local k = 0
    while k < n do
        nt_key(code)
        scheduler.run(60000000)
        k = k + 1
    end
end

K_RETURN = "0x24" -- global: included file
K_ESCAPE = "0x35" -- global: included file
K_PGDN = "0x79" -- global: included file
K_F8 = "0x64" -- global: included file
K_UP = "0x3E" -- global: included file
K_DOWN = "0x3D" -- global: included file

-- --- Open Firmware on ttya ---------------------------------------------------

-- Run until the prompt comes back, returning everything printed on the way.
-- Drain first: a stale "0 > " from the previous command would end the wait
-- before the command had even run.
function nt_prompt(ceiling) -- global: included file
    local out = mac.run_until_sent("a", "0 > ", 250000, ceiling)
    assert(gs.contains(out, "0 > "), F"Open Firmware never came back to its prompt: ${out}")
    return out
end

-- Type `load fd:,\<file>` and then `load-base loadsize eval` (the file name
-- comes in two pieces, `chunk1` and `chunk2`, from when the console was fed
-- seven characters at a time).  Returns what the eval printed within `run`
-- instructions; the caller waits for whatever the script starts.
function nt_load_eval(chunk1, chunk2, run) -- global: included file
    local junk = machine.scc.a.sent()
    ans_send(F"load fd:,\\${chunk1}${chunk2}")
    machine.scc.a.receive("\r")
    local loaded = nt_prompt(600000000)
    assert(gs.contains(loaded, " ok"), F"load fd:,\\${chunk1}${chunk2} failed: ${loaded}")
    ans_send("load-base loadsize eval")
    machine.scc.a.receive("\r")
    local out = ""
    local spent = 0
    while spent < run do
        scheduler.run(20000000)
        spent = spent + 20000000
        out = F"${out}${machine.scc.a.sent()}"
    end
    return out
end

-- --- SETUPLDR, which draws on ttya ------------------------------------------
--
-- machine.scc.a.sent() returns the bytes ESCAPED (ESC as \x1B, a backslash
-- doubled), so the markers below avoid backslashes rather than guess at
-- how many to write.

-- Send one key over the serial console and run until the next screen says
-- `want`.  A little more time after the text appears lets the screen finish
-- drawing and SETUPLDR get back to its keyboard poll.
function nt_ldr(key, want, ceiling, rung) -- global: included file
    local junk = machine.scc.a.sent()
    if key ~= "" then
        machine.scc.a.receive(key)
    end
    local out = ""
    local spent = 0
    while not gs.truthy(function() return gs.contains(out, want) end) do
        assert(spent < ceiling, F"${rung}: SETUPLDR never showed '${want}'")
        scheduler.run(20000000)
        spent = spent + 20000000
        out = F"${out}${machine.scc.a.sent()}"
    end
    scheduler.run(100000000)
    return (F"${out}${machine.scc.a.sent()}")
end

-- --- NT, which draws only on the 54M30 ----------------------------------------

-- Run until the screen is `golden`, in 20 M quanta, up to `ceiling`.
--
-- Verify mode polls screen.matches() and fails with the usual diff
-- artifacts at the ceiling.  REGEN mode waits for the picture to hold still
-- and captures it.  Two things about NT's screens make mac.script's
-- wait_match unsuitable for that: its checksum covers the whole
-- framebuffer, whose off-screen stride padding NT's display driver uses as
-- scratch (so it churns under a still picture), and a focused edit control
-- blinks its caret, so a page waiting for input alternates between two
-- pictures for ever.  This one checksums the visible region and counts
-- either of the last two pictures as still.
function nt_screen(golden, ceiling) -- global: included file
    if mac.REGEN_MODE == "1" then
        local seen = 0
        local seen2 = 0
        local still = 0
        local spent = 0
        while still < 15 do
            assert(spent < ceiling, F"nt_screen(REGEN): ${golden}: the screen never settled")
            scheduler.run(20000000)
            spent = spent + 20000000
            local now = machine.screen.checksum(0, 0, machine.screen.height, machine.screen.width)
            if now == seen or now == seen2 then
                still = still + 1
            else
                seen2 = seen
                seen = now
                still = 0
            end
        end
        machine.screen.save(golden)
        return spent
    end
    local spent = 0
    while spent < ceiling do
        scheduler.run(20000000)
        spent = spent + 20000000
        if machine.screen.matches(golden) then return spent end
    end
    machine.screen.match(golden)
    return spent
end

-- --- Power-on and setup.of ------------------------------------------------------

-- N1: a machine that has never been booted, on the given ROM (the 2.26NT
-- ROM: the production 1.1.22 cannot run NT), with 64 MB -- setup.of puts
-- load-base at 62 MB and boot.of / bootdisk.of stage the veneer between 59
-- and 63 MB, so the 32 MB default cannot work.  Leaves the machine at `0 >`
-- on ttya and returns what the firmware printed.
function nt_power_on(rom) -- global: included file
    machine.boot{model = "ans500", ram = 65536, rom = rom}
    local fw = ""
    local spins = 0
    while not gs.truthy(function() return gs.contains(fw, "0 > ") end) and not gs.truthy(function() return gs.contains(fw, "install-console>") end) and spins < 24 do
        scheduler.run(250000000)
        fw = F"${fw}${machine.scc.a.sent()}"
        spins = spins + 1
    end
    local settle = 0
    while not gs.truthy(function() return gs.contains(fw, "0 > ") end) and settle < 5 do
        scheduler.run(250000000)
        fw = F"${fw}${machine.scc.a.sent()}"
        settle = settle + 1
    end
    -- The console on ttya.  From a virgin store 2.26NT, like the production
    -- ROM, comes up on the screen; Apple's documented terminal setup on the
    -- ADB keyboard and a power cycle move it, as ans_boot_serial does.
    if not gs.truthy(function() return gs.contains(fw, "0 > ") end) then
        mac.type_settle("setenv output-device ttya\n", 5000000, 400000000)
        mac.type_settle("setenv input-device ttya\n", 5000000, 400000000)
        local ignored = machine.scc.a.sent()
        machine.restart()
        fw = F"${fw}${nt_prompt(3000000000)}"
    end
    assert(gs.contains(fw, "OpenFirmware2.26"), "N1: this is not the 2.26NT firmware")
    assert(gs.contains(fw, "0 > "), "N1: the firmware never reached its prompt")

    -- A fixed clock, set only now: the power cycle above builds a new RTC,
    -- seeded from the host.  The HAL reads the time from Cuda, NT shows it
    -- (the taskbar clock is in a golden) and stamps every file it writes
    -- with it, so the host's clock would make both depend on when the row
    -- ran.  After the date the gs-test-data disk was installed on, so
    -- nothing on it is from the future.
    machine.rtc.time = "2026-09-28T09:00:00"
    return fw
end

-- N2: setup.of, which the floppy's README says to run once per machine.
-- The store is virgin on every row (nt_power_on clears it), so every row
-- runs it.  It resets the machine itself; returns once the prompt is back.
function nt_setup_of() -- global: included file
    local le = nt_load_eval("setup", ".of", 200000000)
    assert(gs.contains(le, "setting little-endian mode"), F"N2: setup.of did not run: ${le}")
    local back = nt_prompt(6000000000)
    assert(gs.contains(back, "OpenFirmware2.26"), "N2: the machine did not come back from setup.of's reset")
    echo("N2: little-endian configuration set; the firmware is back")
end
