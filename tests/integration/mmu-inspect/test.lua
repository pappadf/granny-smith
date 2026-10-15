-- Converted from test.script by scripts/script-to-lua.py
-- machine.cpu.mmu's inspection methods, the same on every MMU kind:
--   translate(addr)  {phys, valid, via, access}
--   walk(addr)       translate's map plus `steps`, one per register, table
--                    level or page-table group the MMU consulted
--   map()            the runs that translate linearly
--   descriptor(...)  raw descriptors decoded
-- Each machine is checked for agreement between the four on its live tables.

-- walk answers what translate answers, and its last step resolved it.
local function check_walk(addr)
    local t = machine.cpu.mmu.translate(addr)
    local w = machine.cpu.mmu.walk(addr)
    assert(w.valid == t.valid, "walk and translate agree on validity")
    assert(w.via == t.via, "walk and translate agree on via")
    if t.valid then
        assert(w.phys == t.phys, "walk and translate agree on phys")
        assert(w.access == t.access, "walk and translate agree on access")
        if #w.steps > 0 then
            assert(w.steps[#w.steps].outcome == "hit", "a valid walk ends on a hit")
        end
    elseif #w.steps > 0 then
        assert(w.steps[#w.steps].outcome == "fault", "an invalid walk ends on a fault")
    end
end

-- The PC is mapped, and every run of the map translates linearly: its first
-- and last byte land where the run says.
local function check_map(pc)
    local m = machine.cpu.mmu.map{limit = 64}
    assert(#m > 0, "something is mapped")
    local hit = false
    for _, r in ipairs(m) do
        local first = machine.cpu.mmu.translate(r.start)
        assert(first.valid, "a run's start translates")
        assert(first.phys == r.phys, "a run's start lands on its phys")
        local last = machine.cpu.mmu.translate(r.start + r.size - 1)
        assert(last.phys == ((r.phys + r.size - 1) & 0xffffffff), "a run is linear to its last byte")
    end
    local around = machine.cpu.mmu.map(pc & 0xfffff000, (pc & 0xfffff000) + 0x1000)
    assert(#around == 1, "the PC's page is one run")
    assert(around[1].phys == (machine.cpu.mmu.translate(pc & 0xfffff000).phys), "the PC's run agrees with translate")
end

-- The descriptor the PC's walk read last decodes to the same word.
local function check_descriptor(format)
    local w = machine.cpu.mmu.walk(machine.cpu.pc)
    local s = w.steps[#w.steps]
    local d = machine.cpu.mmu.descriptor(s.addr, 1, format)
    assert(d[1].desc == s.desc, "descriptor() reads the word the walk read")
    assert(d[1].type == "page", "the PC's leaf is a page descriptor")
end

-- 68030 PMMU.
machine.boot{model = "se30", rom = "../../data/roms/iix-iicx-se30-97221136.rom"}
scheduler.run(30000000)
assert(machine.cpu.mmu.enabled, "the ROM turned the PMMU on")
check_walk(machine.cpu.pc)
check_walk(0x50000000)
local w030 = machine.cpu.mmu.walk(machine.cpu.pc)
assert(w030.steps[2].step == "tt", "the 030 checks TT first")
assert(w030.steps[3].step == "root", "then the root pointer")
assert(w030.steps[4].step == "level" and w030.steps[4].name == "A", "then level A")
check_map(machine.cpu.pc)
-- The root pointer's DT says whether the tables hold short or long descriptors.
local fmt030 = "short"
if (machine.cpu.mmu.crp_hi & 3) == 3 then
    fmt030 = "long"
end
check_descriptor(fmt030)
assert(machine.cpu.mmu.translate(machine.cpu.pc, {fetch = true}).valid, "fetch-side translation")

-- 68040.
machine.boot{model = "q700", rom = "../../data/roms/q700-q900-420dbff3.rom"}
scheduler.run(40000000)
assert(machine.cpu.mmu.enabled, "the ROM turned the 040 MMU on")
check_walk(machine.cpu.pc)
check_walk(0x00000000)
local w040 = machine.cpu.mmu.walk(machine.cpu.pc)
assert(w040.steps[4].name == "root" and w040.steps[5].name == "pointer", "root, pointer, page levels")
check_map(machine.cpu.pc)
check_descriptor("page")

-- PowerPC 601: segment first, then the BATs.
machine.boot{model = "pm7100", rom = "../../data/roms/pm6100-pm7100-pm8100-9feb69b3.rom"}
scheduler.run(60000000)
check_walk(machine.cpu.pc)
check_walk(0x00000000)
check_walk(0x68000000)
assert(machine.cpu.mmu.walk(machine.cpu.pc).steps[2].step == "segment", "the 601 consults the segment register first")
check_map(machine.cpu.pc)

-- PowerPC 604: the BATs first, then the segment and the hashed page table.
machine.boot{model = "pm8500", rom = "../../data/roms/pm7500-pm8500-pm9500-96cd923d.rom"}
scheduler.run(60000000)
check_walk(machine.cpu.pc)
check_walk(0x00000000)
local w604 = machine.cpu.mmu.walk(machine.cpu.pc)
assert(w604.steps[2].step == "bat", "the 604 consults the BATs first")
assert(w604.via == "page" and w604.steps[#w604.steps].step == "pteg", "the PC is in the page table")
check_map(machine.cpu.pc)
check_descriptor("pte")
-- A page-table entry names the address it maps, and that address translates
-- back to the entry's page.
local leaf = w604.steps[#w604.steps]
local pte = machine.cpu.mmu.descriptor(leaf.addr + leaf.slot * 8, 1)[1]
assert(pte.ea == (machine.cpu.pc & 0xfffff000), "the entry's ea is the PC's page")
assert(machine.cpu.mmu.translate(pte.ea).phys == pte.phys, "and translates to its page")

-- Lisa: one step, the segment descriptor.
machine.boot{model = "lisa", rom = "../../data/roms/lisa2-revh-098917b2.rom"}
scheduler.run(3000000)
check_walk(machine.cpu.pc)
local wl = machine.cpu.mmu.walk(machine.cpu.pc)
assert(#wl.steps == 1 and wl.steps[2].step == "segment", "one segment step")
assert(wl.space == "rom", "the boot ROM's space")
local seg = machine.cpu.mmu.descriptor(wl.steps[2].index, 1, 0)[1]
assert(seg.slr == wl.steps[2].slr and seg.sor == wl.steps[2].sor, "descriptor() reads the walk's SOR/SLR")
check_map(machine.cpu.pc)

-- No MMU, no node.
machine.boot{model = "plus", rom = "../../data/roms/plus-v3-4d1f8172.rom"}
assert(not gs.ok(function() return machine.cpu.mmu.walk(0) end), "a Plus has no MMU")

echo("mmu-inspect-ok")
quit()
