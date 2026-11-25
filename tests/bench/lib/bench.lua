-- tests/bench/lib/bench.lua — shared helpers for the CPU benchmark rows
-- (docs/guide/BENCHMARKS.md).
--
-- A row script (tests/bench/<row>.lua) defines two functions:
--
--   bench_boot()    boot from nothing to the row's state, deterministically
--                   (RTC pinned after machine.boot, scripted input only), and
--                   assert that the state was reached;
--   bench_guard()   assert, at the end of a measured window, that the machine
--                   is still in that state (the right application, the MMU on,
--                   the 68K emulator running...).  Called through try(), so a
--                   failure marks the measurement invalid, not the run red.
--
-- The harness (scripts/bench/run.py) writes a small driver script per run
-- that loads ../integration/lib/mac.lua (as the global `mac`), this file and
-- the row, then either boots and saves a checkpoint or loads one and measures
-- a window.
-- The slow-path ceiling and the "not stuck" check are applied by the
-- harness to the numbers bench_window() prints, so they are tuned in one
-- place (tests/bench/rows.json).

-- Run `n` scheduler instructions and print what the window cost: guest
-- instructions retired, memory slow-path accesses, host CPU time of the
-- process, wall time, and where the PC ended.  One @@WIN line, read by run.py.
function bench_window(n)
    local i0 = scheduler.instr_count
    local s0 = machine.memory.slowpath_count
    local t0 = scheduler.host_user_ns
    local w0 = scheduler.host_wall_ns
    scheduler.run(n)
    local t1 = scheduler.host_user_ns
    local w1 = scheduler.host_wall_ns
    local di = scheduler.instr_count - i0
    local ds = machine.memory.slowpath_count - s0
    echo(F"@@WIN {\"planned\":${n},\"guest_instr\":${di},\"slowpath\":${ds},\"host_ns\":${t1 - t0},\"wall_ns\":${w1 - w0},\"pc\":\"${machine.cpu.pc:08x}\"}")
end

-- The row's state guard, reported rather than asserted: one @@GUARD line.
function bench_check()
    if gs.try(function() return bench_guard() end, "FAILED") == "FAILED" then
        echo("@@GUARD {\"ok\":false}")
    else
        echo("@@GUARD {\"ok\":true}")
    end
end

-- True when the current application's name (CurApName, $910, a Pascal
-- string) starts with the 4-byte word `w0` followed by the 2-byte `w1` --
-- the length byte and the first five characters, as wait_desktop() reads it.
function bench_app_is(w0, w1)
    return gs.try(function() return machine.cpu.mmu.peek(0x910, 4) end, 0) == w0 and gs.try(function() return machine.cpu.mmu.peek(0x914, 2) end, 0) == w1
end

-- The Finder is the current application and has an application heap.
function bench_assert_finder(label)
    assert(bench_app_is(0x0646696E, 0x6465), F"${label}: the Finder is not the current application")
    assert(debug.mac.globals.read("ApplZone") ~= debug.mac.globals.read("SysZone"), F"${label}: no application heap")
end

-- A PowerPC machine is running 68K code under the ROM's emulator: the
-- nanokernel runs the emulator in user mode (MSR[PR]).
function bench_assert_68k_emulator(label)
    assert((machine.cpu.msr & 0x4000) ~= 0, F"${label}: not in the 68K emulator (MSR=${machine.cpu.msr:08x})")
end
