-- Converted from test.script by scripts/script-to-lua.py
-- The ATI Rage 128 GL (Xclaim VR 128 ROM, 113-57406-108) in socket A1 of a
-- Power Macintosh 9500.
--
-- THE ACCEPTANCE TARGET IS THE CARD'S OWN FCODE.  At probe it maps the I/O
-- BAR at $14 and reaches the whole register file through MM_INDEX/MM_DATA,
-- initialises the clocks and the memory controller, senses the monitor
-- (DDC, then the VSYNC loopback that tells an Apple-sense cable from a VGA
-- one, then the Apple sense lines), and publishes a node whose every
-- property is derived from those answers.  `.properties` over Open
-- Firmware's serial console reads the node back.
--
-- Config-port note (the Mach64 row's): the Bandit config ports are
-- little-endian and machine.memory.peek/poke are big-endian bus views, so
-- every literal below is the byte-swapped form of the value software sees.
-- Socket A1 is device 13: IDSEL bit 13 -> LE $00002000 -> $00200000.

-- --- The card kind and the ROM catalogue ------------------------------------
assert(gs.contains(F"${catalog.pci_cards}", "rage128"), "the card must be registered")
assert(catalog.proms.identify(F"${vars.TEST_DATA}/roms/rage128-xclaimvr128-108-d0c84d42.prom").card_id == "rage128", "the Xclaim ROM provides it")

-- --- The config header, before a single instruction runs --------------------
machine.boot{model = "pm9500", ram = 32768, rom = vars.ROM, pci_card = "rage128"}
assert(machine.pci.slot[1].card.name == "ATI Rage 128 GL", "the card is seated in A1")
assert(machine.pci.slot[1].irq == 23, "A1's INTA reaches Grand Central 23")
assert(machine.pci.slot[1].card.regs.vram_size == 0x1000000, "16 MB, the Rage Orion / Xclaim VR 128")
assert(machine.pci.slot[1].card.monitor.id == "vga", "the default monitor: VGA with DDC")

machine.memory.poke.l(0xF2800000, 0x00200000)
assert(machine.memory.peek.l(0xF2C00000) == 0x02104552, "vendor $1002 / device $5245 ('RE')")
machine.memory.poke.l(0xF2800000, 0x04200000)
assert(machine.memory.peek.l(0xF2C00000) == 0x8000B002, "status $02B0 (CAP_LIST, 66 MHz, fast B2B, medium DEVSEL); AD_STEPPING")
machine.memory.poke.l(0xF2800000, 0x08200000)
assert(machine.memory.peek.l(0xF2C00000) == 0x00000003, "class $030000, revision 0")
machine.memory.poke.l(0xF2800000, 0x34200000)
assert(machine.memory.peek.l(0xF2C00000) == 0x50000000, "the capability list starts at $50")
machine.memory.poke.l(0xF2800000, 0x50200000)
assert(machine.memory.peek.l(0xF2C00000) == 0x025C1000, "AGP 1.0 capability, next $5C")
machine.memory.poke.l(0xF2800000, 0x5C200000)
assert(machine.memory.peek.l(0xF2C00000) == 0x01000102, "power management, the last capability")
-- BAR sizing: the universal probe.
machine.memory.poke.l(0xF2800000, 0x10200000)
machine.memory.poke.l(0xF2C00000, 0xFFFFFFFF)
assert(machine.memory.peek.l(0xF2C00000) == 0x080000FC, "BAR0: 64 MB, prefetchable")
machine.memory.poke.l(0xF2800000, 0x14200000)
machine.memory.poke.l(0xF2C00000, 0xFFFFFFFF)
assert(machine.memory.peek.l(0xF2C00000) == 0x01FFFFFF, "BAR1: 256 bytes of I/O")
machine.memory.poke.l(0xF2800000, 0x18200000)
machine.memory.poke.l(0xF2C00000, 0xFFFFFFFF)
assert(machine.memory.peek.l(0xF2C00000) == 0x00C0FFFF, "BAR2: 16 KB")
machine.memory.poke.l(0xF2800000, 0x1C200000)
machine.memory.poke.l(0xF2C00000, 0xFFFFFFFF)
assert(machine.memory.peek.l(0xF2C00000) == 0x00000000, "no fourth BAR")
machine.memory.poke.l(0xF2800000, 0x30200000)
machine.memory.poke.l(0xF2C00000, 0xFFFFFFFF)
assert(machine.memory.peek.l(0xF2C00000) == 0x0100FEFF, "the ROM BAR: 128 KB")
machine.memory.poke.l(0xF2C00000, 0x00000000)
machine.memory.poke.l(0xF2800000, 0x00000000)

-- --- Open Firmware builds the node from the card's FCode --------------------
-- Command-Option-O-F held from power-on stops the boot at OF 1.0.5's prompt,
-- whose console is the serial port.  Commands go in a few characters at a
-- time (Open Firmware's console drops a long burst).
local function of_send(s)
    machine.scc.a.receive(s)
    scheduler.run(20000000)
end
local function of_go()
    machine.scc.a.receive("\r")
    local out = ""
    local n = 0
    while not gs.truthy(function() return gs.contains(out, "0 > ") end) and n < 60 do
        scheduler.run(40000000)
        local ch = machine.scc.a.sent()
        out = F"${out}${ch}"
        if gs.contains(ch, "More [") then
            machine.scc.a.receive(" ")
        end
        n = n + 1
    end
    assert(gs.contains(out, "0 > "), "Open Firmware did not return to its prompt")
    return out
end
-- Boot with slot entry `entry` to the prompt and return the card's properties.
local function card_properties(entry)
    machine.boot{model = "pm9500", ram = 32768, rom = vars.ROM, slots = entry}
    machine.adb.keyboard.down("Command")
    machine.adb.keyboard.down("Option")
    machine.adb.keyboard.down("o")
    machine.adb.keyboard.down("f")
    scheduler.run(800000000)
    machine.adb.keyboard.up("f")
    machine.adb.keyboard.up("o")
    machine.adb.keyboard.up("Option")
    machine.adb.keyboard.up("Command")
    scheduler.run(200000000)
    local banner = machine.scc.a.sent()
    assert(gs.contains(banner, "Open Firmware, 1.0.5"), "Command-Option-O-F must stop the boot in Open Firmware")
    of_send("dev /ban")
    of_send("dit/@D")
    local cd = of_go()
    assert(not gs.truthy(function() return gs.contains(cd, "nknown") end), "the card's node must exist at /bandit/@D")
    of_send(".propert")
    of_send("ies")
    return of_go()
end

local p = card_properties("1=rage128")
assert(gs.contains(p, "name                    ATY,Rage128v"), "the Xclaim FCode names the node ATY,Rage128v")
assert(gs.contains(p, "model                   ATY,Rage128"), "model ATY,Rage128")
-- ATY,Rom# "113-57406-108", ATY,Card# "109-57400-00", ATY,Fcode "1.69": as bytes.
assert(gs.contains(p, "3131332D 35373430 362D3130 3800"), "ATY,Rom# 113-57406-108")
assert(gs.contains(p, "3130392D 35373430 302D3030 00"), "ATY,Card# 109-57400-00")
assert(gs.contains(p, "312E3639 00"), "ATY,Fcode 1.69")
assert(gs.contains(p, "AAPL,interrupts         00000017"), "A1's interrupt")
assert(gs.contains(p, "AAPL,slot-name          A1"), "socket A1")
assert(gs.contains(p, "devsel-speed            00000001"), "medium DEVSEL, from status $02B0")
assert(gs.contains(p, "device_type             display"), "a display")
-- reg: config space, the ROM (128 KB), BAR0 with the prefetchable bit the
-- FCode reads back from config $10 ($42...), and BAR2 (16 KB).  No I/O BAR.
assert(gs.contains(p, "02006830 00000000 00000000  00000000 00020000"), "reg: the 128 KB ROM")
assert(gs.contains(p, "42006810 00000000 00000000  00000000 04000000"), "reg: BAR0, 64 MB, prefetchable")
assert(gs.contains(p, "02006818 00000000 00000000  00000000 00004000"), "reg: BAR2, 16 KB")
-- Open Firmware placed BAR0 64 MB-aligned inside Bandit 1's window.
assert(gs.contains(p, "C2006810 00000000 84000000  00000000 04000000"), "assigned-addresses: BAR0 at $84000000")
assert(machine.pci.slot[1].card.config.bar[0].base == 0x84000000, "...and the card decodes it there")
assert(machine.pci.slot[1].card.config.bar[1].base ~= 0, "the I/O BAR the FCode probes through was assigned")
-- Sense: no VSYNC loopback on a VGA cable, so the FCode takes the VGA path —
-- code $717 in ATY,Flags 31:16 — and the DDC monitor's EDID is read and
-- verified (flag bits 0, 1, 3), the DAC load sensed (bit 4).
assert(gs.contains(p, "ATY,Flags               0717001B"), "VGA, DDC answered, EDID valid, load sensed")
assert(gs.contains(p, "EDID                    00FFFFFF FFFFFF00"), "the EDID property, header first")
-- VGA mode 5: 640 x 480, its linebytes rounded up to 256 by the FCode.
assert(gs.contains(p, "width                   00000280"), "640 pixels")
assert(gs.contains(p, "height                  000001E0"), "480 lines")
assert(gs.contains(p, "depth                   00000008"), "8 bpp")
assert(gs.contains(p, "linebytes               00000300"), "linebytes 768")
assert(gs.contains(p, "driver,AAPL,MacOS,PowerPC"), "the ndrv is published")
assert(gs.contains(p, "0001844A bytes total"), "...all 99,402 bytes of it")

-- A VGA monitor that does not answer DDC: no EDID, the same VGA mode.
local p = card_properties("1=rage128,monitor=vga_noddc")
assert(not gs.truthy(function() return gs.contains(p, "EDID ") end), "no DDC, no EDID property")
assert(gs.contains(p, "ATY,Flags               07170012"), "VGA, no EDID")

-- An Apple 13" RGB on an Apple cable: the VSYNC loopback follows the
-- polarity the FCode drives, so it trusts the sense lines and reads $62B.
local p = card_properties("1=rage128,monitor=13in_rgb")
assert(gs.contains(p, "ATY,Flags               062B8012"), "Apple sense path ($8000), code $62B")
assert(gs.contains(p, "width                   00000280"), "640 x 480 at 67 Hz for the 13-inch")

-- A 21" Macintosh Color Display: code $000 -> 1152 x 870.
local p = card_properties("1=rage128,monitor=21in_rgb")
assert(gs.contains(p, "ATY,Flags               00008010"), "Apple sense, code $000")
assert(gs.contains(p, "width                   00000480"), "1152 pixels")
assert(gs.contains(p, "height                  00000366"), "870 lines")

-- The Nexus 128's ROM on a 32 MB card: its own node name.
local p = card_properties(F"1=rage128,memory=32m,rom=${vars.TEST_DATA}/roms/rage128-nexus128-103-35b433aa.prom")
assert(gs.contains(p, "name                    ATY,Rage128n"), "the Nexus FCode names its node ATY,Rage128n")
assert(gs.contains(p, "312E3638 2000"), "ATY,Fcode 1.68")
assert(machine.pci.slot[1].card.regs.vram_size == 0x2000000, "32 MB")

-- --- The same card in a beige Power Macintosh G3 ---------------------------
-- Open Firmware 2.4 behind Grackle runs the same FCode and must build the
-- same node.  Its console is the on-board Rage Pro's screen, so `ttya io`
-- goes in on the keyboard (lib/gossamer.script's recipe) — but only once
-- this card's probe is over: the FCode spends over a second of guest time
-- in its DDC and sense waits, and keystrokes typed before it returns are
-- lost.  The probe's end is visible in the card's own command register: the
-- FCode turns I/O and memory decode on for its probe and off again after.
local ROM_G3 = F"${vars.TEST_DATA}/roms/pmg3dt-pmg3mt-78f57389.rom"
machine.boot{model = "pmg3dt", ram = 65536, rom = ROM_G3, pci_card = "rage128"}
machine.adb.keyboard.down("Command")
machine.adb.keyboard.down("Option")
machine.adb.keyboard.down("o")
machine.adb.keyboard.down("f")
local probed = false
local n = 0
while n < 30 and not (probed and (machine.pci.slot[1].card.config.command & 3) == 0) do
    scheduler.run(100000000)
    if (machine.pci.slot[1].card.config.command & 1) ~= 0 then
        probed = true
    end
    n = n + 1
end
assert(probed, "OF 2.4 must run the card's probe (it enables the I/O BAR for it)")
scheduler.run(300000000)
machine.adb.keyboard.up("f")
machine.adb.keyboard.up("o")
machine.adb.keyboard.up("Option")
machine.adb.keyboard.up("Command")
machine.adb.keyboard.type("ttya io\n")
scheduler.run(300000000)
local drained = machine.scc.a.sent()
of_send("dev /pci")
of_send("/@d")
local cd = of_go()
assert(not gs.truthy(function() return gs.contains(cd, "nknown") end), "the card's node must exist at /pci/@d")
of_send(".propert")
of_send("ies")
local p = of_go()
assert(gs.contains(p, "name                    ATY,Rage128v"), "OF 2.4 builds the same node")
assert(gs.contains(p, "ATY,Flags               0717001B"), "...with the same sense and DDC answers")
assert(gs.contains(p, "C2006810 00000000 84000000  00000000 04000000"), "BAR0 64 MB-aligned in Grackle's window")
assert(gs.contains(p, "AAPL,slot-name          A1"), "socket A1")

-- --- The ndrv draws the Mac OS desktop --------------------------------------
-- No disk: the ROM's Mac OS starts on the card's ndrv, which takes the
-- console over from the FCode, and waits on the grey desktop for a startup
-- disk.  Every pixel on screen is the card's.
machine.boot{model = "pm9500", ram = 32768, rom = vars.ROM, pci_card = "rage128"}
scheduler.run(600000000)
assert(gs.try(function() return machine.pci.slot[7].card.name end, "(empty)") == "(empty)", "the fake Control stands down for a display card")
assert(machine.screen.width == 640, "640 pixels across")
assert(machine.screen.height == 480, "480 lines")
assert(machine.screen.depth == 16, "the ndrv runs the desktop in thousands of colours")
assert(machine.pci.slot[1].card.framebuffer.depth == 16, "machine.screen.source resolves to the card's framebuffer")
assert((machine.pci.slot[1].card.regs.crtc_gen_cntl & 0x03000000) == 0x03000000, "CRTC_EN and CRTC_EXT_DISP_EN")
-- The ndrv sets linear aperture 1 to 16 bpp swapping for QuickDraw.
assert((machine.pci.slot[1].card.regs.config_cntl & 0xC) == 0x4, "aperture 1: big-endian, 16 bpp swapping")
-- The grey desktop is a one-pixel dither: horizontally adjacent pixels
-- differ, which a blank or black screen cannot fake.
assert(machine.screen.checksum(200, 200, 201, 201) ~= machine.screen.checksum(200, 201, 201, 202), "the desktop dither is drawn")

-- --- The apertures and their swappers ----------------------------------------
-- Both register apertures decode one little-endian file; the second copy is
-- BAR2 + 8 KB and CONFIG_REG_1_BASE says so (its field's low bit is
-- hardwired to one).  machine.memory.peek is a big-endian bus view, so a
-- little-endian register reads byte-reversed through aperture 0.
local REGS = machine.pci.slot[1].card.config.bar[2].base
local APER = machine.pci.slot[1].card.config.bar[0].base
assert(machine.pci.slot[1].card.regs.read(0x10C) == REGS + 0x2000, "CONFIG_REG_1_BASE = BAR2 + 8 KB")
assert(machine.pci.slot[1].card.regs.read(0x104) == APER + 0x2000000, "CONFIG_APER_1_BASE = BAR0 + 32 MB")
assert(machine.pci.slot[1].card.regs.read(0x108) == 0x2000000, "CONFIG_APER_SIZE: 32 MB per linear aperture")
assert(machine.pci.slot[1].card.regs.read(0x110) == 0x2000, "CONFIG_REG_APER_SIZE: 8 KB per register aperture")
assert(machine.pci.slot[1].card.regs.read(0xF8) == 0x1000000, "CONFIG_MEMSIZE: 16 MB")
assert(machine.pci.slot[1].card.regs.read(0xF00) == 0x52451002, "the config-space mirror at $F00")
-- BIOS_0_SCRATCH ($10) through register aperture 0: little-endian.
machine.memory.poke.l(REGS + 0x10, 0x78563412)
assert(machine.pci.slot[1].card.regs.read(0x10) == 0x12345678, "aperture 0 is little-endian")
assert(machine.memory.peek.l(REGS + 0x2000 + 0x10) == 0x78563412, "aperture 1 is too, until APER_REG_ENDIAN")
local CC = machine.pci.slot[1].card.regs.config_cntl
machine.memory.poke.l(REGS + 0xE0, 0x10000000 | (CC << 24)) -- CONFIG_CNTL |= APER_REG_ENDIAN (LE)
assert(machine.memory.peek.l(REGS + 0x2000 + 0x10) == 0x12345678, "APER_REG_ENDIAN: aperture 1 reads in host order")
assert(machine.memory.peek.l(REGS + 0x10) == 0x78563412, "...and aperture 0 is untouched")
-- A byte write is "add 1, 2 or 3 to the address" and touches one lane.
machine.memory.poke.b(REGS + 0x11, 0xAB)
assert(machine.pci.slot[1].card.regs.read(0x10) == 0x1234AB78, "a byte write lands in its lane")
-- Indirect access: MM_INDEX/MM_DATA through the I/O face reach the same file.
machine.memory.poke.l(REGS + 0x0, 0x10000000) -- MM_INDEX := $10 (LE)
assert(machine.memory.peek.l(REGS + 0x4) == 0x78AB3412, "MM_DATA reads BIOS_0_SCRATCH")
-- The linear apertures: aperture 1 is set by the ndrv to 16 bpp swapping,
-- so a halfword stored there lands byte-reversed in VRAM, as the little-
-- endian pixel the CRTC scans; aperture 0 sees VRAM straight.
local SPOT = 0x00F00000
machine.memory.poke.w(APER + 0x2000000 + SPOT, 0x7C00) -- pure red, 1-5-5-5
assert(machine.memory.peek.b(APER + SPOT) == 0x00, "VRAM byte 0: the pixel's low byte")
assert(machine.memory.peek.b(APER + SPOT + 1) == 0x7C, "VRAM byte 1: the pixel's high byte")
assert(machine.memory.peek.w(APER + 0x2000000 + SPOT) == 0x7C00, "the swapped aperture reads the pixel back in host order")
-- Past the 16 MB of memory the aperture is empty: reads float high.
assert(machine.memory.peek.l(APER + 0x1000000) == 0xFFFFFFFF, "nothing past CONFIG_MEMSIZE")

echo("tnt-pci-rage128: header, FCode node, sense/DDC, the ndrv desktop and the apertures all hold")
quit()
