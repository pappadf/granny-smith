-- q840av-finder — the 68040 core and its MMU: a Quadra 840AV at the System
-- 7.1 Finder desktop (the AV suite's 77 MB image).

function bench_boot()
    machine.boot{model = "q840av", ram = 16384, rom = F"${vars.TEST_DATA}/roms/q840av-q660av-5bf10fd1.rom"}
    machine.rtc.time = 3000000000
    machine.scsi.attach_hd(F"${vars.TEST_DATA}/systems/system_7_1_77mb_av.img", 0)
    mac.wait_desktop(1500000000)
    bench_guard()
end

function bench_guard()
    bench_assert_finder("q840av-finder")
end
