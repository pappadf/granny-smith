-- Converted from test.script by scripts/script-to-lua.py
-- Integration test: the debugger on a running Lisa (see config.mk).
--
-- The Office System desktop runs processes in contexts 1-3 beside the
-- operating system's context 0, which is what makes the Lisa's MMU worth
-- inspecting: one logical address leads somewhere different in each.

machine.hd.insert(F"${vars.WORK_DIR}/profile.image")
assert(machine.hd.pram_init(2, false), "pram_init failed")
-- The desktop is complete by ~35M instructions (the suite-lisa row's golden).
scheduler.run(45000000)
assert(machine.cpu.mmu.start == false, "the MMU is still in START mode")

-- ---- MMU: an explicit context ---------------------------------------------
-- translate / walk / map answer for any context, and agree with the
-- descriptor of that context's segment.
local seg = 1
local la = seg * 0x20000
local distinct = 0
local prev = nil
for _, c in ipairs({0, 1, 2, 3}) do
    local t = machine.cpu.mmu.translate(la, {context = c})
    local d = machine.cpu.mmu.descriptor(seg, 1, c)[1]
    assert(t.valid == (d.type ~= "invalid"), F"ctx ${c}: translate and descriptor disagree on validity")
    if t.valid then
        assert(t.phys == d.phys, F"ctx ${c}: translate phys != the descriptor's page 0")
        assert(t.space == "ram", F"ctx ${c}: segment 1 is not RAM")
        local w = machine.cpu.mmu.walk(la, {context = c})
        assert(w.phys == t.phys, F"ctx ${c}: walk != translate")
        assert(w.steps[1].context == c, F"ctx ${c}: the walk read another context")
        -- The mapped run starting at the address is that context's too.
        local r = machine.cpu.mmu.map(la, la + 0x20000, {context = c})[1]
        assert(r.start == la and r.phys == t.phys, F"ctx ${c}: map disagrees")
        -- The segment's length in pages, decoded from its limit.
        assert(d.pages == 256 - d.limit, F"ctx ${c}: pages != 256 - limit for a growing segment")
        -- peek in the context reads the physical RAM translate names.
        assert(machine.cpu.mmu.peek(la, 4, "logical", c) == machine.cpu.mmu.peek(t.phys, 4, "physical"), F"ctx ${c}: logical and physical peeks differ")
        if prev ~= nil and prev ~= t.phys then
            distinct = distinct + 1
        end
        prev = t.phys
    end
end
-- Context 0 (the operating system) has no segment 1; the three user
-- contexts map it to three different programs.
assert(machine.cpu.mmu.translate(la, {context = 0}).valid == false, "context 0 maps segment 1")
assert(distinct == 2, "the user contexts do not map segment 1 to three different places")
-- Omitting the context keeps the old behaviour: the CPU's own choice.
-- Compared while a desktop process holds the latch: the operating system's
-- context 0 maps no segment 1, so there would be no address to compare.
-- The OS and the processes alternate many times a second; at a fixed 45M
-- instructions this used to land inside a level-1 interrupt storm (the
-- ProFile driver's Timer-2 race, fixed with the Lisa's E-synchronized VIA
-- cycles), which happened to leave context 2 latched.
local tries = 0
while machine.cpu.mmu.context == 0 and tries < 200 do
    scheduler.run(10000)
    tries = tries + 1
end
local cur = machine.cpu.mmu.context
assert(cur ~= 0, "no desktop process took the CPU within 2M instructions")
assert(machine.cpu.mmu.translate(la, false).phys == machine.cpu.mmu.translate(la, {context = cur}).phys, "user = the latch context")
assert(machine.cpu.mmu.translate(la, true).valid == machine.cpu.mmu.translate(la, {context = 0}).valid, "supervisor = context 0")
assert(gs.try(function() return machine.cpu.mmu.translate(la, {context = 4}) end, "refused") == "refused", "context 4 accepted")
assert(gs.try(function() return machine.cpu.mmu.peek(la, 4, "physical", 1) end, "refused") == "refused", "a physical peek took a context")

-- ---- Memory views: a context, and physical --------------------------------
local t1 = machine.cpu.mmu.translate(la, {context = 1})
local b1 = machine.memory.peek.bytes(la, 16, "logical", 1)
assert(b1 == machine.memory.peek.bytes(t1.phys, 16, "physical"), "peek.bytes: context 1 != physical")
assert(b1 ~= machine.memory.peek.bytes(la, 16, "logical", 2), "contexts 1 and 2 read the same bytes")
assert(gs.try(function() return machine.memory.peek.bytes(la, 16, "physical", 1) end, "refused") == "refused", "peek.bytes: physical took a context")
-- dump and disasm take the same argument (their text output is not checked
-- here, only that they accept it and refuse a bad one).
machine.memory.dump(la, 16, "logical", 1)
machine.memory.dump(t1.phys, 16, "physical")
debug.disasm(la, 2, {context = 1})
assert(gs.try(function() return debug.disasm(la, 2, 9) end, "refused") == "refused", "disasm accepted context 9")
-- The view is restored after each request: the plain read is the CPU's again.
assert(machine.memory.peek.l(la) == machine.cpu.mmu.peek(la, 4), "a context outlived its request")

-- ---- Physical memory logpoints and watchpoints ------------------------------
-- The screen lives at the top of main memory; the operating system reaches
-- it through a logical segment in context 0.  A physical logpoint used to
-- count nothing on the Lisa (only logical pages were checked).
local scr = machine.cpu.mmu.translate(0xFA0200, {context = 0})
assert(scr.valid and scr.space == "ram", "the screen segment is not RAM")
local lphys = debug.logpoints.add{addr = scr.phys, ["end"] = scr.phys + 0x7DFF, mode = "write", space = "physical", level = 9}
local llog = debug.logpoints.add{addr = 0xFA0200, ["end"] = 0xFA7FFF, mode = "write", level = 9}
machine.adb.mouse.move(30, 20)
scheduler.run(3000000)
assert(debug.logpoints[llog.id].hit_count > 0, "the pointer drew nothing on the screen")
assert(debug.logpoints[lphys.id].hit_count == debug.logpoints[llog.id].hit_count, "physical and logical logpoints disagree")
debug.logpoints.clear()
local wp = debug.watchpoints.add{addr = scr.phys, ["end"] = scr.phys + 0x7DFF, mode = "write", space = "physical"}
machine.adb.mouse.move(-30, -20)
scheduler.run(3000000)
assert(debug.watchpoints[wp.id].hit_count >= 1, "a physical watchpoint did not stop the machine")
debug.watchpoints.clear()

-- ---- No Macintosh names under the Lisa's own system -------------------------
-- A line-A word in LOS code is not a Macintosh trap: the disassembler shows
-- it raw, and debug.mac refuses -- unless asked for explicitly.
assert(debug.mac.mode == "auto", "debug.mac.mode does not start at auto")
assert(debug.mac.active == false, "the Macintosh views apply under LOS")
local la_word = debug.find.word(0xA022, 0x500000, 0x540000)[1]
assert(debug.frame(la_word, 1, 0).rows[1].mnem == "LINEA", "a line-A word disassembles as a Macintosh trap")
assert(gs.try(function() return debug.mac.atrap(0xA022) end, "refused") == "refused", "debug.mac.atrap answered on a Lisa")
assert(gs.try(function() return debug.mac.globals.read("Ticks") end, "refused") == "refused", "debug.mac.globals.read answered on a Lisa")
debug.mac.mode = "on"
assert(debug.mac.atrap(0xA022) == "_NewHandle", "debug.mac.mode = on does not force the names")
assert(debug.frame(la_word, 1, 0).rows[1].mnem == "_NewHandle", "the disassembler ignores debug.mac.mode")
debug.mac.mode = "auto"

-- ---- keyboard.type takes a whole line --------------------------------------
-- The transitions are paced in guest time, so a line longer than the COPS
-- FIFO is not refused (it was, past 24 bytes: 11 plain characters).
assert(machine.adb.keyboard.type("A1b2C3d4E5f6G7h8I9j0") == 20, "a 20-character line was refused")
scheduler.run(10000000)

-- ---- A breakpoint in a segment that is not present --------------------------
-- Open the Calculator: its process faults on its first code segment, the
-- operating system loads it and returns to the faulting instruction.  A
-- breakpoint there must fire when the instruction runs, not before the
-- fetch faults (as it did).
local function lclick(x, y)
    machine.adb.mouse.move(x, y, "global")
    scheduler.run(3000000)
    machine.adb.mouse.click(true)
    scheduler.run(800000)
    machine.adb.mouse.click(false)
    scheduler.run(800000)
end
local function menu_pick(mx, ix, iy)
    machine.adb.mouse.move(mx, 8, "global")
    scheduler.run(3000000)
    machine.adb.mouse.click(true)
    scheduler.run(3000000)
    machine.adb.mouse.move(ix, iy, "global")
    scheduler.run(3000000)
    machine.adb.mouse.click(false)
end
lclick(657, 340)
scheduler.run(5000000)
menu_pick(100, 140, 83)
scheduler.run(60000000)
lclick(309, 62)
scheduler.run(5000000)
menu_pick(100, 140, 83)
-- The operating system's bus-error handler, entered for the new process
-- (context 1); the fault address is in its exception frame.
debug.breakpoints.add(0x2E257E, "machine.cpu.mmu.context == 1")
scheduler.run(30000000)
assert(machine.cpu.pc == 0x2E257E, "the Calculator never faulted")
local fa = machine.memory.peek.l(machine.cpu.a7 + 34) & 0xFFFFFF
assert(machine.cpu.mmu.descriptor(fa >> 17, 1, 1)[1].type == "invalid", "the faulting segment is present")
debug.breakpoints.clear()
debug.breakpoints.add(fa, "machine.cpu.mmu.context == 1")
scheduler.run(60000000)
assert(machine.cpu.pc == fa, "the breakpoint did not fire")
assert(machine.cpu.mmu.translate(fa, false).valid, "the breakpoint fired before the segment was present")
debug.breakpoints.clear()

-- ---- Memory reads right after a checkpoint load -----------------------------
-- Stopped in user code (context 1): a save and load must leave the debugger
-- reading through the CPU's own context at once, not the supervisor's (it
-- read $FFFF at the PC until the next step).
assert((machine.cpu.sr & 0x2000) == 0, "not stopped in user mode")
local code_word = machine.memory.peek.w(machine.cpu.pc)
assert(code_word ~= 0xFFFF, "the code at the PC does not read")
assert(checkpoint.save(F"${vars.WORK_DIR}/user-stop.gscp"), "checkpoint.save failed")
assert(checkpoint.load(F"${vars.WORK_DIR}/user-stop.gscp"), "checkpoint.load failed")
assert(machine.memory.peek.w(machine.cpu.pc) == code_word, "the code at the PC reads differently after a load")
assert(debug.frame(machine.cpu.pc, 1, 0).rows[1].mnem ~= "DC.W", "the disassembly after a load is not code")
-- A memory logpoint added after a load fires, on RAM and on the MMU's
-- context latch alike.
local latch = debug.logpoints.add{addr = 0xFCE008, ["end"] = 0xFCE00F, mode = "write", level = 9}
scheduler.run(3000000)
assert(debug.logpoints[latch.id].hit_count > 0, "a latch logpoint added after a load never fired")
debug.logpoints.clear()

-- ---- The absolute mouse move after a checkpoint save and load ---------------
-- Reported to do nothing until one relative move; it must steer the cursor
-- (the OS's globals $CC00F0/$CC00F2, context 0) straight away.
local function cursor_at(x, y)
    local cx = machine.cpu.mmu.peek(0xCC00F0, 2, "logical", 0)
    local cy = machine.cpu.mmu.peek(0xCC00F2, 2, "logical", 0)
    return cx + 3 >= x and cx <= x + 3 and cy + 3 >= y and cy <= y + 3
end
assert(checkpoint.save(F"${vars.WORK_DIR}/mouse.gscp"), "checkpoint.save failed")
machine.adb.mouse.move(150, 40, "global")
scheduler.run(3000000)
assert(cursor_at(150, 40), "the global move after a save did not reach (150, 40)")
assert(checkpoint.load(F"${vars.WORK_DIR}/mouse.gscp"), "checkpoint.load failed")
machine.adb.mouse.move(500, 250, "global")
scheduler.run(3000000)
assert(cursor_at(500, 250), "the global move after a load did not reach (500, 250)")

-- ---- Soft power-off, and on again -------------------------------------------
-- The OS puts everything away and tells the COPS to switch the supply off.
-- The machine then stops (it used to loop on with a black screen); powering
-- it on again is a power cycle that keeps the ProFile and parameter memory.
assert(machine.power.state == "on", "the machine starts off")
machine.power.off()
local budget = 0
while machine.power.state == "on" and budget < 40 do
    scheduler.run(5000000)
    budget = budget + 1
end
assert(machine.power.state == "off", "the OS never switched the supply off")
local off_at = machine.cpu.instr_count
scheduler.run(5000000)
assert(machine.cpu.instr_count == off_at, "the CPU kept executing after power-off")
assert(gs.try(function() return machine.power.on() end, "x") ~= "x", "power.on failed")
assert(machine.power.state == "on", "power.on did not switch the machine on")
assert(gs.try(function() return machine.power.on() end, "refused") == "refused", "power.on accepted a machine already on")
-- The ROM's self-test and the OS come up again from the ProFile.
scheduler.run(120000000)
assert(machine.hd.present, "the ProFile went with the power")
assert(machine.cpu.mmu.start == false, "the OS did not come back after power-on")

echo("lisa-debugger: all checks passed")
quit()
