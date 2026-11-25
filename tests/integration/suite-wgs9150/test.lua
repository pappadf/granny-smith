-- Workgroup Server 9150 suite — the 8100's board in a server tower.
--
-- What the model owes beyond the 8100 (Apple, "Workgroup Server 9150"
-- Developer Note, 1995; wgs9150.c): its own CPU identification, which the
-- ROM turns into Gestalt $27 (9150/80) or $39 (9150/120) — BoxFlag = Gestalt
-- - 6, so $21 / $33 — and a fourth NuBus slot at $A.
--
-- Row map:
--
--   wgs9150-753-hd     9150/80, System 7.5.3 from SCSI: Finder, About This
--                      Macintosh, BoxFlag $21.  (System 7.5 itself does not
--                      know the machine: "This startup disk will not work
--                      on this Macintosh model".)
--   wgs9150-120-76-hd  9150/120 on its own ROM, System 7.6: Finder, About
--                      This Computer, BoxFlag $33, and the CPU:bus ratio
--                      HWInit measured (3:1, a 40 MHz bus).
--   wgs9150-76-fd      9150/80 booting the System 7.6 "Install 1" floppy to
--                      the Installer.
--   wgs9150-24ac-slot-a  9150/80 with a Macintosh Display Card 24AC in slot
--                      $A — the connector the 8100 does not have: the Slot
--                      Manager enumerates it, its driver sets the mode and
--                      the OS paints it as the second screen.
--
-- Both System versions print the generic "Power Macintosh" in About; the
-- BoxFlag assertions carry the model identity.

local mac = dofile("../lib/mac.lua")
local ppc = dofile("../lib/ppc_rows.lua")
local boxflag, dispatch = ppc.boxflag, ppc.dispatch

local hd_753 = "../../data/systems/system_7_5_3_25mb_bare.img"
local hd_76 = "../../data/systems/system_7_6_170mb_24ac.img"
local fd_76 = "../../data/systems/SSW-7.6-1.4M/Install 1.img"
local rom_120 = "../../data/roms/wgs9150-120mhz-9b037f6f-d20052bd25d88bd6.rom"

local id_80 = "9c7c98f7-e9220fe5992ffdf2"
local id_120 = "9b037f6f-d20052bd25d88bd6"

-- Boot `model` from `hd` at SCSI id 0 to a stable Finder desktop.
local function boot_hd(model, rom, hd, slots)
    machine.boot{model = model, ram = 32768, rom = rom, slots = slots}
    machine.rtc.time = 0xE0000000
    machine.scsi.attach_hd(hd, 0)
    machine.rtc.pram.boot_device = 0
    scheduler.run(450000000)
    assert(debug.mac.globals.read("BootDrive") ~= 0, F"${model}: no startup device by 450 M")
    mac.wait_desktop(3000000000)
    assert(debug.mac.globals.read("DSErrCode") == 0, F"${model}: system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")
end

local function row_wgs9150_753_hd()
    local t0 = scheduler.host_user_ns
    boot_hd("wgs9150", vars.ROM, hd_753)
    assert(machine.id == "wgs9150", "machine.id != wgs9150")
    assert(machine.rom.id == id_80, F"rom.id ${machine.rom.id}")
    -- The machine-ID register: $A55A3014, the 9150's %100 code.
    assert(machine.memory.peek.b(0x5FFFFFFF) == 0x14, "machine-ID register low byte != $14")
    assert(boxflag() == 0x21, F"BoxFlag ${boxflag()}, want $21 (Gestalt $27)")
    mac.check_ex("goldens/wgs9150-7.5.3-finder.png", 0, 440, 20, 640)
    mac.about_box()
    mac.check_about_clock("goldens/wgs9150-7.5.3-about.png")
    mac.mips_floor("wgs9150-753-hd", t0, 20)
    mac.report_row("7.5.3", "ariel", "hd-boot 53c96 about")
    mac.report_perf("suite-wgs9150/wgs9150-753-hd", scheduler.instr_count)
end

local function row_wgs9150_120_76_hd()
    boot_hd("wgs9150_120", rom_120, hd_76)
    assert(machine.id == "wgs9150_120", "machine.id != wgs9150_120")
    assert(machine.rom.id == id_120, F"rom.id ${machine.rom.id}")
    assert(boxflag() == 0x33, F"BoxFlag ${boxflag()}, want $33 (Gestalt $39)")
    -- HWInit's clock measurement, from the kernel's ProcessorInfo record:
    -- 120 MHz on a 40 MHz bus.
    local pi = machine.cpu.mmu.peek(0x5FFFEFD8, 4)
    assert(machine.cpu.mmu.peek(pi + 4, 4) == 120000000, "measured CPU clock != 120 MHz")
    assert(machine.cpu.mmu.peek(pi + 8, 4) == 40000000, "measured bus clock != 40 MHz (ratio 3)")
    mac.check_ex("goldens/wgs9150_120-7.6-finder.png", 0, 440, 20, 640)
    mac.about_box()
    mac.check_about_clock("goldens/wgs9150_120-7.6-about.png")
    mac.report_row("7.6", "ariel", "hd-boot 53c96 about")
    mac.report_perf("suite-wgs9150/wgs9150-120-76-hd", scheduler.instr_count)
end

local function row_wgs9150_76_fd()
    ppc.boot_install1("wgs9150", 32768, vars.ROM, 0xE0000000, fd_76, "goldens/wgs9150-7.6-floppy-boot.png")
    assert(boxflag() == 0x21, F"BoxFlag ${boxflag()}, want $21")
    mac.report_row("7.6", "swim3", "fd-boot")
    mac.report_perf("suite-wgs9150/wgs9150-76-fd", scheduler.instr_count)
end

local function row_wgs9150_24ac_slot_a()
    boot_hd("wgs9150", vars.ROM, hd_76, "10=display_card_24ac,rom=substitute")
    local c = machine.nubus.slot[10].card
    assert(c.name == "Macintosh Display Card 24AC (substitute ROM)", "slot $A card != 24AC")
    assert(c.declrom.present, "slot $A declaration ROM not present")
    -- The OS ran the card's own driver (it powers on at 8 bpp) and painted
    -- the secondary-screen gray dither into its VRAM.
    assert(c.framebuffer.base == 0xFA000000, "slot $A framebuffer base != 0xFA000000")
    assert(c.framebuffer.width == 640, "slot $A framebuffer width != 640")
    assert(c.framebuffer.depth == 1, "slot $A framebuffer depth != 1 (driver never set the mode)")
    assert(machine.memory.peek.l(0xFA004000) == 0xAAAAAAAA, "slot $A fb: no gray dither at row 128")
    assert(machine.screen.width == 640, "built-in screen width != 640")
    mac.report_row("7.6", "24ac", "nubus slot A hd-boot")
    mac.report_perf("suite-wgs9150/wgs9150-24ac-slot-a", scheduler.instr_count)
end

-- ---- row dispatch -----------------------------------------------------------

dispatch("wgs9150-753-hd", row_wgs9150_753_hd, hd_753)
dispatch("wgs9150-120-76-hd", row_wgs9150_120_76_hd, hd_76)
dispatch("wgs9150-76-fd", row_wgs9150_76_fd, fd_76)
dispatch("wgs9150-24ac-slot-a", row_wgs9150_24ac_slot_a, hd_76)

mac.suite_done()
quit()
