-- iici-aux — the 68030 with its PMMU busy: a IIci booting A/UX 3.0.1, in the
-- kernel's start-up with translation on.  (At the login prompt A/UX idles in
-- STOP, which measures the scheduler, not the CPU; the window is earlier.)

function bench_boot()
    machine.boot{model = "iici", ram = 16384, rom = F"${vars.TEST_DATA}/roms/iici-368cadfe.rom"}
    machine.rtc.pram.validate()
    machine.rtc.pram.poke(0x56, gs.bytes(0x001f838600000000, 8))
    machine.scsi.attach_hd(F"${vars.TEST_DATA}/aux/aux_3.0.1/hd160-with-aux-301.img", 0)
    machine.rtc.time = "2026-04-26T21:30:36"
    -- Past the last STOP-idle gap of the early kernel (~176 M instructions,
    -- waiting on the disk): from 186 M the kernel computes with translation on
    -- for 40 M instructions (0.2-1 % slow-path accesses).  Counted in retired
    -- instructions, which a STOP does not advance.
    scheduler.run(180000000)
    while scheduler.instr_count < 186000000 do
        scheduler.run(1000000)
    end
    bench_guard()
end

-- The PMMU is translating.
function bench_guard()
    assert(machine.cpu.mmu.enabled, "iici-aux: the PMMU is off")
end
