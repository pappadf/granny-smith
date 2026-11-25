-- pm7500-of — the 604 core: a Power Macintosh 7500 ROM boot through Open
-- Firmware and POST.  The checkpoint is 160 M instructions in, where the
-- 68K emulator has started; past ~220 M the media-free ROM settles into a
-- timebase wait, so this row's windows stay short (rows.json).

function bench_boot()
    machine.boot{model = "pm7500", ram = 32768, rom = F"${vars.TEST_DATA}/roms/pm7500-pm8500-pm9500-96cd923d.rom"}
    machine.rtc.time = 3000000000
    scheduler.run(160000000)
    bench_guard()
end

function bench_guard()
    bench_assert_68k_emulator("pm7500-of")
end
