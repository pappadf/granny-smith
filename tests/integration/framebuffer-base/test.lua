-- See config.mk.  framebuffer.base is the guest physical address of the
-- first displayed byte, on every display source.

-- The base of the source under test: machine.video's, or the SE/30's slot.
local function fb_base(src)
    if src == "video" then
        return machine.video.framebuffer.base
    end
    return machine.nubus.slot[14].card.framebuffer.base
end

-- Run until the source scans out of guest memory and the ROM has published
-- that screen: ScrnBase (the low-memory global the guest draws through) is
-- the source's base plus `other` -- 0 unless the guest reaches the same byte
-- through another of its addresses.  Before that $824 is the boot ROM
-- overlay or unset; fails past `ceiling` instructions.
local function wait_screen(src, other, ceiling)
    local spent = 0
    while fb_base(src) == 0 or machine.memory.peek.l(0x824) ~= fb_base(src) + other do
        assert(spent < ceiling, F"ScrnBase ${machine.memory.peek.l(0x824):08x} never became base ${fb_base(src):08x} + ${other:x}")
        scheduler.run(5000000)
        spent = spent + 5000000
    end
end

-- machine.screen.checksum's hash of `n` guest PHYSICAL bytes from `addr`
-- (c = c * 31 + byte, the hash it takes over the bytes behind a region).
local function phys_hash(addr, n)
    local c = 0
    for i = 0, n - 1 do
        c = c * 31 + machine.cpu.mmu.peek(addr + i, 1, "physical")
    end
    return c
end

-- The memory at `base` is what the screen scans first: the first four bytes
-- there are the leading pixels of row 0, and the four a stride further on
-- lead row 1.  Read through the physical map, so it holds whatever the CPU's
-- translation happens to be.
local function pin_scanned_bytes(label, base, stride, depth)
    local px = 32 // depth
    assert(machine.screen.checksum(0, 0, 1, px) == phys_hash(base, 4), F"${label}: memory at base ${base:08x} is not the first scanned bytes")
    assert(machine.screen.checksum(1, 0, 2, px) == phys_hash(base + stride, 4), F"${label}: memory at base + stride is not row 1")
end

-- On a PowerPC machine the debug poke is physical, so the claim can be made
-- the other way round too: replacing the byte at `base` changes the first
-- pixel(s) and nothing beside or below them.
local function pin_by_poke(label, base, stride, depth)
    local ppb = 1
    if depth < 8 then
        ppb = 8 // depth
    end
    local row0 = machine.screen.checksum(0, 0, 1, ppb)
    local right = machine.screen.checksum(0, ppb, 1, 2 * ppb)
    local row1 = machine.screen.checksum(1, 0, 2, ppb)
    local v = machine.memory.peek.b(base)
    machine.memory.poke.b(base, 0xFF - v)
    assert(machine.screen.checksum(0, 0, 1, ppb) ~= row0, F"${label}: poking base does not change the first pixel(s)")
    assert(machine.screen.checksum(0, ppb, 1, 2 * ppb) == right, F"${label}: poking base changes the pixels beside them")
    assert(machine.screen.checksum(1, 0, 2, ppb) == row1, F"${label}: poking base changes row 1")
    machine.memory.poke.b(base, v)
    assert(machine.screen.checksum(0, 0, 1, ppb) == row0, F"${label}: restoring base does not restore the first pixel(s)")
end

-- --- Quadra 700: DAFB, a built-in chip with its own VRAM at $F9000000 -------
-- Its base used to be a byte offset into that VRAM.
wait_screen("video", 0, 400000000)
local B = machine.video.framebuffer.base
assert(B >= 0xF9000000 and B < 0xF9200000, F"DAFB: base ${B:08x} is not in the VRAM window")
pin_scanned_bytes("DAFB", B, machine.video.framebuffer.stride, machine.video.framebuffer.depth)

-- --- SE/30: the built-in video, a NuBus pseudo-card in slot $E ----------------
-- Its base used to be the slot base, $FE000000, which holds no pixel: the
-- primary screen starts $8040 into the VRAM, which decodes there (where the
-- Slot Manager puts the 32-bit ScrnBase) and again at $FEE00000 (where it
-- forms ScrnBase in 24-bit mode, as this ROM boots).
machine.boot{model = "se30", rom = F"${vars.TEST_DATA}/roms/iix-iicx-se30-97221136.rom"}
wait_screen("slot", 0xE00000, 400000000)
local S = machine.nubus.slot[14].card.framebuffer.base
assert(S == 0xFE008040, F"SE/30: base ${S:08x} is not the primary buffer at slot $E + $8040")
assert(machine.screen.source.base == S, "SE/30: screen.source is not the same node")
pin_scanned_bytes("SE/30", S, machine.nubus.slot[14].card.framebuffer.stride, machine.nubus.slot[14].card.framebuffer.depth)

-- --- 7500: Control, a built-in PCI device -------------------------------------
-- Its base used to be a byte offset into the 4 MB store.  The generic PCI
-- layer gives it a framebuffer node of its own (slot 4), the one
-- machine.screen.source resolves to, reading the same base as machine.video.
machine.boot{model = "pm7500", ram = 32768, rom = F"${vars.TEST_DATA}/roms/pm7500-pm8500-pm9500-96cd923d.rom"}
wait_screen("video", 0, 1000000000)
local C = machine.video.framebuffer.base
assert(C == machine.pci.slot[4].card.config.bar[2].base + 0x800000 + 0x210, F"Control: base ${C:08x} is not the VRAM BAR's usable half + the scan start")
assert(machine.pci.slot[4].card.framebuffer.base == C, "Control: the PCI node and machine.video disagree")
assert(machine.screen.source.base == C, "Control: screen.source is not Control's node")
pin_scanned_bytes("Control", C, machine.video.framebuffer.stride, machine.video.framebuffer.depth)
pin_by_poke("Control", C, machine.video.framebuffer.stride, machine.video.framebuffer.depth)

quit()
