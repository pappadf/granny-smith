-- Converted from test.script by scripts/script-to-lua.py
-- QuickDraw 3D Accelerator Card (pci_card "qd3d") on a Power Macintosh 8500.
--
-- The rig: a pm8500 (604, 64 MB) booting Mac OS 7.6 from
-- system_7_6_170mb_24ac_qd3d.dmg, which adds to the plain 7.6 image:
--
--   System Folder:Extensions   QuickDraw 3D 1.6 (release): QuickDraw™ 3D,
--                              RAVE, IR, Viewer, QD3DCustomElements, and the
--                              card's Apple QD3D HW Driver (ndrv, matches
--                              pci106b,9) and Apple QD3D HW Plug-In (the RAVE
--                              engine "Apple PCI Hardware").
--   QD3D Lab                   Read Me; Alternate Extensions (QD3D 1.5.4
--                              debug, 1.0.3 retail with the Accelerator
--                              layer, HW 1.6 debug, the installed 1.6 set);
--                              Debugging (MacsBug, not installed); RAVE Tools
--                              (Sneak3view, RaveEngineInfo, RAVEContextTest);
--                              Test Apps (3DMF Models, Gerbils! 1.0.2+ and
--                              1.0.3, HAVOC Preview, TextureEyes).
--
-- Row map:
--
--   qd3d-device      no disk: the card's PCI header, then two hand-built
--                    frames poked through its BAR -- a control program, two
--                    box-like objects and a descriptor list in RAM -- once
--                    plain (nearest surface wins) and once as the CSG
--                    difference A - B; the pixels the card DMAs back are
--                    checked one by one.
--   qd3d-boot        the card seated in slot A1: the image boots to the
--                    Finder, and Apple's driver has found the card, set its
--                    position register and built the object-cache free list.
--   qd3d-rave-info   RaveEngineInfo lists the card's engine, "Apple PCI
--                    Hardware", first, with 0x40000 texels of texture memory
--                    (the revision-2 figure).
--   qd3d-gerbils     Gerbils! 1.0.3 (Apple's acceptance test for the card)
--                    renders through the card: frames keep completing, no
--                    system error, and the title scene matches its golden.
--   qd3d-texture     TextureEyes with its QD3D Logo texture: a static,
--                    trilinearly filtered textured quad, golden-matched.
--   qd3d-viewer      the Teapot 3DMF model opened in SimpleText: QuickDraw 3D's
--                    Viewer draws ~4500 triangles in one static frame,
--                    golden-matched.
--   qd3d-thousands   Gerbils at thousands of colours: the card's 16-bpp
--                    (x1555) output path, and Control's 16-bpp DAC lookup.
--
-- The image rows share one boot: qd3d-boot checkpoints the open QD3D Lab
-- window.
-- Run on its own with make -C tests/integration test-tnt-qd3d.

local mac = dofile("../lib/mac.lua")

local hd = "../../data/systems/system_7_6_170mb_24ac_qd3d.dmg"
local lab = F"${mac.RESULTS_DIR}/qd3d-lab.gscp"

-- Low-memory CurApName ($910, a Pascal string) holds `name`.
local function current_app_is(name)
    return gs.contains(F"${machine.memory.read_cstring(0x911, #name)}", name)
end

-- Back to the open QD3D Lab window (the qd3d-boot row's checkpoint).
local function to_lab()
    assert(files.path_exists(lab), "no QD3D Lab checkpoint: run the qd3d-boot row first")
    machine.scsi.attach_hd(hd, 0)
    checkpoint.load(lab)
end

-- The card must keep completing frames: no stuck interrupt, no watchdog.
local function frames_flow(what)
    local f0 = machine.pci.slot[1].card.renderer.frames
    scheduler.run(100000000)
    assert(machine.pci.slot[1].card.renderer.frames > f0, F"${what}: the card stopped completing frames (${machine.pci.slot[1].card.renderer.frames})")
end

-- ---- qd3d-device -----------------------------------------------------------
-- Config-port note (the tnt-pci-voodoo2 idiom): Bandit's address/data ports
-- are little-endian and machine.memory.peek/poke are raw big-endian bus
-- views, so literals go through swap32.  Socket A1 is device 13.  The card's
-- own BAR is big-endian: its words go out as they are.
local function swap32(v)
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF)
end
local function cfg_wr(reg, val)
    machine.memory.poke.l(0xF2800000, swap32(0x00002000 | reg))
    machine.memory.poke.l(0xF2C00000, swap32(val))
    machine.memory.poke.l(0xF2800000, 0)
end
local function cfg_rd(reg)
    machine.memory.poke.l(0xF2800000, swap32(0x00002000 | reg))
    local v = swap32(machine.memory.peek.l(0xF2C00000))
    machine.memory.poke.l(0xF2800000, 0)
    return v
end
local function put(a, v)
    machine.memory.poke.l(a, v)
end

-- The hand-built frame, 16 x 4 pixels, one card: the control program at
-- $100000 (one 32-byte record per control block), the objects from $110000
-- (64 bytes each), the descriptor list at $200000 and the output at $300000.
local prog = 0x100000
local objs = 0x110000
local out = 0x300000

-- One control block: link, then seven command words.
local function put_cb(a, next_, c0, c1, c2, c3, c4, c5, c6)
    put(a, next_)
    put(a + 4, c0)
    put(a + 8, c1)
    put(a + 12, c2)
    put(a + 16, c3)
    put(a + 20, c4)
    put(a + 24, c5)
    put(a + 28, c6)
end

-- One flat rectangle x0..x1 (pixels) over all four rows, as two Gouraud
-- triangles at `a` and `a`+64: header, three 13.3 vertices, three depths,
-- three colours.  The second links to `next`.
local function put_tri(a, next_, hdr, p0, p1, p2, z, colour)
    put(a, next_)
    put(a + 4, hdr)
    put(a + 8, p0)
    put(a + 12, p1)
    put(a + 16, p2)
    put(a + 20, z)
    put(a + 24, z)
    put(a + 28, z)
    put(a + 32, colour)
    put(a + 36, colour)
    put(a + 40, colour)
end
local function put_rect(a, next_, hdr, x0, x1, z, colour)
    put_tri(a, a + 64, hdr, (x0 * 8) << 16, (x1 * 8) << 16, ((x1 * 8) << 16) | 32, z, colour)
    put_tri(a + 64, next_, hdr, (x0 * 8) << 16, ((x1 * 8) << 16) | 32, ((x0 * 8) << 16) | 32, z, colour)
end

-- The frame: box A spans x 0-12 (front face z 0.2 red, back face z 0.6
-- yellow), box B spans x 6-16 (front z 0.1 green, back z 0.3 blue).  `ha`
-- and `hb` are the two boxes' header bases (front faces; the back faces add
-- the back-facing bit).  All objects start on scanline 0, so they follow
-- the initial block.  The program then runs S = 4 + 1 scanlines.
local function build_frame(ha, hb)
    put_cb(prog, objs, 0x20000004, 0x0004FFFF, 0x20000030, 15, 0, 0, 0)
    put_rect(objs, objs + 128, ha, 0, 12, 0x3E4CCCCD, 0xFFFF0000)
    put_rect(objs + 128, objs + 256, ha | 0x04000000, 0, 12, 0x3F19999A, 0xFFFFFF00)
    put_rect(objs + 256, objs + 384, hb, 6, 16, 0x3DCCCCCD, 0xFF00FF00)
    put_rect(objs + 384, prog + 32, hb | 0x04000000, 6, 16, 0x3E99999A, 0xFF0000FF)
    local i = 0
    while i < 5 do
        local a = prog + 32 + i * 64
        local cur = (i * 8) | 4
        local nxt = 0xFFFF
        if i < 4 then
            nxt = ((i + 1) * 8) | 4
        end
        local link = 0
        if i < 4 then
            link = a + 64
        end
        put_cb(a, a + 32, 0x20000004, (nxt << 16) | cur, 0x20000028, 0, 0, 0, 0)
        put_cb(a + 32, link, 0, 0, 0, 0, 0, 0x2000002C, 0)
        i = i + 1
    end
    -- one WRITE of 64 bytes per scanline, then STOP; the output pre-filled
    local y = 0
    while y < 4 do
        put(0x200000 + y * 16, 0x00000040)
        put(0x200000 + y * 16 + 4, out + y * 64)
        put(0x200000 + y * 16 + 8, 0)
        put(0x200000 + y * 16 + 12, 0)
        local x = 0
        while x < 16 do
            put(out + y * 64 + x * 4, 0xEEEEEEEE)
            x = x + 1
        end
        y = y + 1
    end
    put(0x200040, 0x08000000)
end

-- A renderer register through the packet port (opcode 1).
local function port_reg(bar, reg, value)
    put(bar, 0x20000000 | reg)
    put(bar, value)
end

-- Start the frame and check every output pixel against `want(x)` for
-- x 0-5, 6-11 and 12-15.  The pixels land, and the interrupt rises, when the
-- frame completes -- some microseconds after the go write, never inside it.
local function run_frame(bar, csg, w0, w1, w2)
    put(bar + 0x40, 0x1000)
    put(bar + 0x40, 0)
    port_reg(bar, 0x08, 0xFF404040)
    port_reg(bar, 0x24, csg)
    port_reg(bar, 0x00, 0x1000)
    put(bar + 0x4C, 0x200000)
    put(bar + 0x40, 0xC000)
    local f0 = machine.pci.slot[1].card.renderer.frames
    port_reg(bar, 0x20, prog)
    assert(machine.pci.slot[1].card.renderer.frames == f0 + 1, "the frame did not run")
    assert(machine.pci.slot[1].card.renderer.lines == 5, F"the program has 5 scanlines (4 + the drain line), not ${machine.pci.slot[1].card.renderer.lines}")
    assert(machine.pci.slot[1].card.renderer.objects == 8, F"8 triangles, not ${machine.pci.slot[1].card.renderer.objects}")
    assert(not machine.pci.slot[1].card.renderer.irq, "the interrupt is not raised synchronously with the go write")
    assert(machine.memory.peek.l(out) == 0xEEEEEEEE, "the pixels reach memory when the frame completes, not at the go write")
    scheduler.run(200000)
    assert(machine.pci.slot[1].card.renderer.irq, "the frame completed without raising its interrupt")
    put(bar + 0x40, 0xE000)
    assert(not machine.pci.slot[1].card.renderer.irq, "writing the acknowledge bit drops the interrupt")
    local y = 0
    while y < 4 do
        local x = 0
        while x < 16 do
            local want = w0
            if x >= 6 then
                want = w1
            end
            if x >= 12 then
                want = w2
            end
            local got = machine.memory.peek.l(out + y * 64 + x * 4)
            assert(got == want, F"pixel (${x},${y}) is ${got:08x}, not ${want:08x}")
            x = x + 1
        end
        y = y + 1
    end
end

local function row_qd3d_device()
    machine.boot{model = "pm8500", ram = 65536, rom = vars.ROM, pci_card = "qd3d"}
    -- Let the ROM bring up the memory controller first: before it has, the
    -- bus master's view of RAM is not the CPU's.
    scheduler.run(50000000)
    assert(machine.pci.slot[1].card.name == "QuickDraw 3D Accelerator", "the card is seated in A1")
    -- The header Apple's driver matches: pci106b,9, revision 2, one 4 KB
    -- memory BAR, INTA, no expansion ROM.
    assert(cfg_rd(0x00) == 0x0009106B, "vendor $106B / device $0009 -- the node name pci106b,9")
    assert((cfg_rd(0x08) & 0xFF) == 0x02, "revision 2: the 0x40000-texel, 16-bpp-capable ASIC")
    assert((cfg_rd(0x3C) & 0x0000FF00) == 0x00000100, "interrupt pin 1 = INTA")
    cfg_wr(0x10, 0xFFFFFFFF)
    assert(cfg_rd(0x10) == 0xFFFFF000, "BAR0: 4 KB, memory, not prefetchable")
    cfg_wr(0x14, 0xFFFFFFFF)
    assert(cfg_rd(0x14) == 0, "no BAR1")
    cfg_wr(0x30, 0xFFFFFFFF)
    assert(cfg_rd(0x30) == 0, "no expansion ROM")
    -- Memory space and bus mastering on, the way the driver sets them.
    cfg_wr(0x10, 0x81000000)
    cfg_wr(0x04, 0x0016)
    assert((cfg_rd(0x04) & 0x16) == 0x16, "memory space, bus master, memory write and invalidate latch")
    local bar = machine.pci.slot[1].card.config.bar[0].base
    -- Both frames on every raster backend, swapped in place: the worker
    -- thread (the default), the inline walker (normative) and null, which
    -- draws no objects -- every pixel the background.
    assert(machine.pci.slot[1].card.renderer.raster == "thread", "the renderer runs on its worker thread by default")
    for _, backend in ipairs({"thread", "sw", "null"}) do
        machine.pci.slot[1].card.renderer.raster = backend
        assert(machine.pci.slot[1].card.renderer.raster == backend, F"the ${backend} backend is in use")
        local bg = 0x00404040
        local drawn = backend ~= "null"
        -- Plain: the nearest surface wins -- A's red front, then B's green
        -- front, which is nearer than A's wherever the two overlap.
        build_frame(0x68200000, 0x68200000)
        if drawn then
            run_frame(bar, 0, 0x00FF0000, 0x0000FF00, 0x0000FF00)
        else
            run_frame(bar, 0, bg, bg, bg)
        end
        -- CSG A - B (kQ3CSGEquationAandnotB, 0x22222222): A is operand 0, B
        -- operand 1.  Where only A is, its front face shows; where B carves
        -- A, the surface is B's back face, where the ray leaves B still
        -- inside A; where only B is, nothing of the result is there.
        build_frame(0x69000000, 0x69000001)
        if drawn then
            run_frame(bar, 0x22222222, 0x00FF0000, 0x000000FF, bg)
        else
            run_frame(bar, 0x22222222, bg, bg, bg)
        end
    end
    machine.pci.slot[1].card.renderer.raster = "thread"
end

-- ---- qd3d-boot --------------------------------------------------------------
-- Boot with the card in slot A1, dismiss the first-boot Energy Saver dialog,
-- open Macintosh HD (Cmd-O on the selected volume) and QD3D Lab.
local function row_qd3d_boot()
    machine.boot{model = "pm8500", ram = 65536, rom = vars.ROM, pci_card = "qd3d"}
    machine.rtc.time = "2026-04-26T21:30:36"
    machine.scsi.attach_hd(hd, 0)
    mac.wait_desktop(4000000000)
    assert(debug.mac.globals.read("BootDrive") == 0x8023, F"BootDrive ${debug.mac.globals.read(\"BootDrive\"):04x}, not set (mount failed)")
    assert(debug.mac.globals.read("DSErrCode") == 0, F"system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")
    -- The driver found the card: one card, index 0 ($F0 | 0 << 2 | 0), and
    -- the object cache's free list starts at $2000.
    assert(machine.pci.slot[1].card.renderer.position == 0xF0, F"the driver never set the card's position register (${machine.pci.slot[1].card.renderer.position:x})")
    assert(machine.pci.slot[1].card.renderer.read(0x34) == 0x2000, "the driver never built the object-cache free list")
    machine.screen.save(F"${mac.RESULTS_DIR}/qd3d-boot.png")
    machine.adb.keyboard.press("Return")
    scheduler.run(60000000)
    machine.adb.keyboard.down("Command")
    machine.adb.keyboard.press("o")
    machine.adb.keyboard.up("Command")
    mac.wait_stable(400000000)
    mac.double_click(358, 95)
    mac.wait_stable(400000000)
    checkpoint.save(lab)
end

-- ---- qd3d-rave-info ---------------------------------------------------------
-- QD3D Lab:RAVE Tools:RaveEngineInfo writes its report to a
-- "RaveEngineInfo.out" window; drag its scroll thumb to the top, where the
-- engine list and the first engine's gestalt answers are.
local function row_qd3d_rave_info()
    to_lab()
    mac.double_click(231, 95)
    mac.wait_stable(400000000)
    mac.double_click(167, 95)
    scheduler.run(300000000)
    assert(current_app_is("RaveEngineInfo"), "RaveEngineInfo is not the current application")
    machine.adb.mouse.move(564, 310, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    mac.run_ticks(10)
    machine.adb.mouse.move(564, 92, "global")
    mac.run_ticks(20)
    machine.adb.mouse.click(false)
    mac.run_ticks(30)
    assert(debug.mac.globals.read("DSErrCode") == 0, F"system error ${debug.mac.globals.read(\"DSErrCode\"):04x} in RaveEngineInfo")
    mac.check_ex("goldens/qd3d-rave-info.png", 0, 500, 20, 640)
end

-- ---- qd3d-gerbils -----------------------------------------------------------
-- QD3D Lab:Test Apps:Gerbils! 1.0.3:Gerbils!.  Its title scene (a textured
-- cube turning over a star field) is drawn by the card straight into
-- Control's VRAM through the output DMA.
local function row_qd3d_gerbils()
    to_lab()
    mac.double_click(39, 147)
    mac.wait_stable(400000000)
    mac.double_click(295, 95)
    mac.wait_stable(400000000)
    mac.double_click(39, 92)
    scheduler.run(600000000)
    assert(current_app_is("Gerbils!"), "Gerbils! is not the current application")
    assert(debug.mac.globals.read("DSErrCode") == 0, F"system error ${debug.mac.globals.read(\"DSErrCode\"):04x} in Gerbils!")
    assert(machine.pci.slot[1].card.renderer.frames > 100, F"Gerbils! did not render through the card (${machine.pci.slot[1].card.renderer.frames} frames)")
    assert(machine.pci.slot[1].card.renderer.objects > 100, F"the card's last frame held ${machine.pci.slot[1].card.renderer.objects} objects, not the title scene")
    mac.check_ex("goldens/qd3d-gerbils.png", 0, 500, 20, 640)
    frames_flow("Gerbils!")
end

-- ---- qd3d-texture -----------------------------------------------------------
-- QD3D Lab:Test Apps:TextureEyes:TextureEyes, then Texture > Picture (Cmd-P)
-- applies the QD3D Logo to its square.  The scene is static, so the golden
-- pins the card's texture path: upload, page and level addressing, the
-- trilinear filter and the 5-bit-per-channel texel store.
local function row_qd3d_texture()
    to_lab()
    mac.double_click(39, 147)
    mac.wait_stable(400000000)
    mac.double_click(167, 147)
    mac.wait_stable(400000000)
    mac.double_click(39, 92)
    scheduler.run(600000000)
    assert(current_app_is("TextureEyes"), "TextureEyes is not the current application")
    machine.adb.keyboard.down("Command")
    machine.adb.keyboard.press("p")
    machine.adb.keyboard.up("Command")
    scheduler.run(600000000)
    assert(debug.mac.globals.read("DSErrCode") == 0, F"system error ${debug.mac.globals.read(\"DSErrCode\"):04x} in TextureEyes")
    assert(machine.pci.slot[1].card.renderer.read(0x80) ~= 0, "no texture map was loaded into the card")
    mac.check_ex("goldens/qd3d-texture.png", 0, 500, 20, 640)
end

-- ---- qd3d-viewer ------------------------------------------------------------
-- QD3D Lab:Test Apps:3DMF Models:Teapot opens in SimpleText, whose QuickDraw
-- 3D Viewer renders the model once and then holds still.
local function row_qd3d_viewer()
    to_lab()
    mac.double_click(39, 147)
    mac.wait_stable(400000000)
    mac.double_click(39, 95)
    mac.wait_stable(400000000)
    mac.double_click(39, 147)
    scheduler.run(900000000)
    assert(current_app_is("SimpleText"), "SimpleText is not the current application")
    assert(debug.mac.globals.read("DSErrCode") == 0, F"system error ${debug.mac.globals.read(\"DSErrCode\"):04x} in the Viewer")
    assert(machine.pci.slot[1].card.renderer.objects > 4000, F"the card's last frame held ${machine.pci.slot[1].card.renderer.objects} objects, not the teapot")
    mac.check_ex("goldens/qd3d-viewer.png", 0, 500, 20, 640)
end

-- ---- qd3d-thousands ---------------------------------------------------------
-- Switch the display to thousands of colours from the Control Strip's
-- monitor-depth module, then run Gerbils: the plug-in sets DMACONTROL's
-- 16-bpp bits and the card streams x1555 pixels.
local function row_qd3d_thousands()
    to_lab()
    machine.adb.mouse.move(100, 467, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    mac.run_ticks(30)
    machine.adb.mouse.move(150, 359, "global")
    mac.run_ticks(20)
    machine.adb.mouse.click(false)
    scheduler.run(400000000)
    assert(machine.screen.depth == 16, F"thousands of colours is 16 bpp (${machine.screen.depth})")
    mac.double_click(39, 147)
    mac.wait_stable(400000000)
    mac.double_click(295, 95)
    mac.wait_stable(400000000)
    mac.double_click(39, 92)
    scheduler.run(600000000)
    assert(current_app_is("Gerbils!"), "Gerbils! is not the current application")
    assert((machine.pci.slot[1].card.renderer.dma_control & 0x0C00) == 0x0C00, F"the card is not producing 16-bpp output (DMACONTROL ${machine.pci.slot[1].card.renderer.dma_control:04x})")
    mac.check_ex("goldens/qd3d-thousands.png", 0, 500, 20, 640)
    frames_flow("Gerbils! at thousands")
end

-- ---- row dispatch -----------------------------------------------------------

mac.row("qd3d-device", row_qd3d_device)
if not gs.truthy(function() return mac.have_media(hd) end) then
    echo("skip: the image rows (system_7_6_170mb_24ac_qd3d.dmg not fetched)")
else
    mac.row("qd3d-boot", row_qd3d_boot)
    mac.row("qd3d-rave-info", row_qd3d_rave_info)
    mac.row("qd3d-gerbils", row_qd3d_gerbils)
    mac.row("qd3d-texture", row_qd3d_texture)
    mac.row("qd3d-viewer", row_qd3d_viewer)
    mac.row("qd3d-thousands", row_qd3d_thousands)
end
mac.suite_done()

quit()
