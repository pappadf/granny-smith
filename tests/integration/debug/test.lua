-- Integration test: Debug tooling — `find` family, breakpoint entries,
-- the probes, disasm and step (the first test written in Lua).
--
-- `find.{str|bytes|word|long}` return a list of match addresses (a 1-based
-- Lua sequence); failures raise.  This script asserts on the typed results
-- and echoes stable markers that run.sh greps.

-- === find: introspection ================================================

-- methods("debug.find") lists the four sub-command methods this script
-- exercises.
echo(table.concat(methods("debug.find"), ", "))

-- === debug.find.str: ASCII search =============================================

-- "Apple" appears in the Plus ROM (which is mirrored at 0x400000).
-- Restrict the scan to the primary mirror so the expected hit count is
-- stable.
local hits = debug.find.str("Apple", 0x400000, 0x440000)
assert(#hits > 0, "debug.find.str should locate 'Apple' in the ROM mirror")
echo(F"str-first=${hits[1]:08x}")
echo(F"str-count=${#hits}")

-- Bounded scan (start + count window).
local windowed = debug.find.str("Apple", 0x400000, 0x400000 + 0x40000 - 1)
assert(#windowed == #hits, "windowed scan should see the same hits")

-- Empty pattern is a typed error, not a silent no-op.
assert(not gs.ok(function() return debug.find.str("", 0x400000, 0x440000) end), "empty pattern must error")

-- === debug.find.bytes: hex-byte-sequence search ===============================

-- Search for NOP (4E 71) in the mapped address range.
local nops = debug.find.bytes("4E 71", 0x400000, 0x440000)
assert(#nops > 0, "debug.find.bytes should hit 68K NOPs")
echo(F"nop-count=${#nops}")

-- Non-hex token is rejected.
assert(not gs.ok(function() return debug.find.bytes("ZZ") end), "non-hex byte token must error")

-- === debug.find.word / debug.find.long: big-endian value search =====================

-- "Apple" begins with bytes 41 70 ('A','p'), so word 0x4170 hits where
-- "Apple" starts — same location the str test already asserts.
-- Probe a window pinned at the known str hit so the first word/long
-- match is exactly that address (0x4170 also occurs elsewhere in ROM).
local w = debug.find.word(0x4170, hits[1], hits[1] + 3)
assert(w[1] == hits[1], "debug.find.word should hit at the str offset")
echo(F"word-first=${w[1]:08x}")

local l = debug.find.long(0x4170706C, hits[1], hits[1] + 7)
assert(l[1] == hits[1], "debug.find.long should hit at the str offset")
echo(F"long-first=${l[1]:08x}")

-- Values exceeding 16 bits must be rejected — not silently truncated.
assert(not gs.ok(function() return debug.find.word(0x10000, 0x400000, 0x440000) end), "16-bit overflow must error")

-- Backwards range is rejected.
assert(not gs.ok(function() return debug.find.str("Apple", 0x500000, 0x400000) end), "backwards range must error")

-- === format specs in F"${expr:fmt}" =======================================
--
-- These exercises pin down the supported specs (decimal, hex,
-- width-padded hex, force-string) so we don't regress them.

echo(F"${0xab:x}")
echo(F"${0xab:08x}")
echo(F"[${0xab:8X}]")
echo(F"${0xab:d}")
echo(F"[${42:5d}]")
echo(F"${'hello':s}")
echo(F"fmt-marker[${0xab:08x}]")

-- === indexed-child access ===============================================
--
-- Reading an empty indexed slot raises (and aborts the script when not
-- caught); reading the index-less collection yields the entries list.

debug.breakpoints.clear()
assert(not gs.ok(function() return debug.breakpoints[42] end), "empty slot read must error")
assert(#debug.breakpoints.entries == 0, "cleared collection reads as empty list")

-- === breakpoint entries: dedupe, enumerate, enable, remove ==============
--
-- What web2's Breakpoints panel relies on.  Ids are stable and never
-- reused, so a list is walked with meta.indices, never with count; a second
-- add at one address returns the existing entry; a disabled entry stays
-- listed but does not fire.

debug.breakpoints.clear()
debug.breakpoints.add(0x400100)
debug.breakpoints.add(0x400100)
assert(debug.breakpoints.count == 1, "a duplicate add must not stack")
debug.breakpoints.add(0x400200)
assert(debug.breakpoints.count == 2, "two addresses, two entries")
local first = debug.breakpoints.meta.indices("entries")[1]
assert(debug.breakpoints.entries[first].enabled == true, "enabled by default")
debug.breakpoints.entries[first].remove()
assert(debug.breakpoints.count == 1, "remove by id")
assert(#debug.breakpoints.meta.indices("entries") == 1, "indices follow the removal")

-- A disabled breakpoint on a PC the ROM keeps passing does not fire;
-- re-enabled, it stops the machine there.
debug.breakpoints.clear()
scheduler.run(200000)
local loop_pc = machine.cpu.pc
debug.breakpoints.add(loop_pc)
local bp = debug.breakpoints.meta.indices("entries")[1]
debug.breakpoints.entries[bp].enabled = false
scheduler.run(200000)
assert(debug.breakpoints.entries[bp].hit_count == 0, "a disabled breakpoint must not fire")
debug.breakpoints.entries[bp].enabled = true
scheduler.run(200000)
assert(debug.breakpoints.entries[bp].hit_count == 1, "re-enabled, it fires")
assert(machine.cpu.pc == loop_pc, "and stops the machine at its address")
debug.breakpoints.clear()
echo("bp-contract-ok")

-- === probes on an interrupt handler's first instruction ===============
--
-- The probes ran after each instruction, at the next PC, while a pending
-- interrupt was taken at the sprint's entry: the handler's first instruction
-- executed before any probe saw its address, so a breakpoint or PC logpoint
-- there almost never fired (#173).  The Plus ROM's level-1 autovector ($64)
-- is its VIA handler, entered at least once per VBL; Ticks counts the VBLs.
-- The ROM's RAM test runs for the first ~20 M instructions; the vector and
-- Ticks are live once it idles at the disk icon.
debug.logpoints.clear()
scheduler.run(25000000)
local lvl1 = machine.memory.peek.l(0x64)
assert(lvl1 >= 0x400000 and lvl1 < 0x420000, F"the level-1 vector points into the ROM (${lvl1:08x})")
local t0 = debug.mac.globals.read("Ticks")
debug.logpoints.add{addr = lvl1, message = "lvl1"}
scheduler.run(1000000)
local vbls = debug.mac.globals.read("Ticks") - t0
assert(vbls > 10, F"VBLs during the run (${vbls})")
local lp = debug.logpoints.meta.indices("entries")[1]
local lp_hits = debug.logpoints.entries[lp].hit_count
assert(lp_hits >= vbls, F"the handler's first instruction is probed on every entry (${lp_hits} hits, ${vbls} VBLs)")
debug.logpoints.clear()
debug.breakpoints.add(lvl1)
scheduler.run(1000000)
assert(machine.cpu.pc == lvl1, "a breakpoint on the handler's first instruction stops there")
debug.breakpoints.clear()
echo("irq-entry-probe-ok")

-- === debug.disasm with explicit address =================================
--
-- `debug.disasm` accepts an optional starting address. The two-arg form
-- `disasm(addr, count)` lets you disassemble at an arbitrary address
-- without first moving PC. The address below is in the Plus ROM that
-- this test boots against.

debug.disasm(0x400090, 3)

-- === debug.step runs through the frame loop =============================
--
-- N stepped instructions must be N run ones: debug.step arms the same budget
-- scheduler.run does, so VBL pulses while stepping.  Before, stepping ran the
-- guest with the VBL line dead and the two diverged within a few frames.
-- 600000 Plus instructions span several
-- VBLs.  (Last in the script: it boots the machine again -- both runs must
-- start from the same constructed machine, instruction counter included, which
-- a machine.restart (a power cycle of the SAME machine) does not give.)

machine.boot{model = "plus", rom = "../../data/roms/plus-v3-4d1f8172.rom"}
machine.rtc.time = 3000000000
scheduler.run(400000)
debug.step(600000)
local step_pc = machine.cpu.pc
local step_n = machine.cpu.instr_count
machine.boot{model = "plus", rom = "../../data/roms/plus-v3-4d1f8172.rom"}
machine.rtc.time = 3000000000
scheduler.run(400000)
scheduler.run(600000)
assert(machine.cpu.instr_count == step_n, "same instruction count")
assert(machine.cpu.pc == step_pc, "debug.step N must end where scheduler.run N does")
echo("step-matches-run-ok")

-- A log file that cannot be opened reports fopen's own reason.  shell.run
-- prints the error and fails without ending this script.
local log_probe = gs.try(function() return shell.run("log.set cpu file=/nonexistent-gs-dir/cpu.log") end, false)
echo("log-file-probe-done")

quit()
