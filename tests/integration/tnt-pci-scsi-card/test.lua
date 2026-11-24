-- Converted from test.script by scripts/script-to-lua.py
-- A Power Macintosh 8500 with two PC cards in its PCI sockets: a Symbios
-- 53C825A SCSI host adapter in A1 and a Cirrus Logic 54M30 in B1.
--
-- WHY.  Both parts entered this repository soldered to the Apple Network
-- Server's board, where Windows NT drives them with its own symc810.sys and
-- cirrus.sys.  A PCI device is a PCI device: in another machine's socket
-- each is a PC card with no FCode PROM, which Open Firmware and the Mac OS
-- leave alone and an operating system with its own driver uses.  That is
-- the configuration Windows NT on an 8500 needs (powermac-nt-hal): NT has
-- no driver for MESH, the 53C94 or Control.
--
-- THE SCSI CARD BRINGS ITS OWN BUS.  A card in a socket is not wired to the
-- board's buses, so the machine builds one for it after the slot walk:
-- storage bus "pci_1" (the slot's id), object machine.pci_1, a checkpoint
-- part of its own (machine_card_scsi_buses).  The configuration names
-- devices on it like any bus, and refuses them when the slot holds no card
-- that brings one.
--
-- WHAT IS ASSERTED.
--   1  both cards seated, the card bus up, its drives attached
--   2  System 7.6 boots from MESH to a clean Finder desktop, BYTE-IDENTICAL
--      to the plain 8500's (suite-tnt's golden): two ROM-less cards change
--      nothing on Control's screen, and the Mac OS took no fault probing them
--   3  PCI configuration space at A1's IDSEL answers the 53C825A's id
--   4  a checkpoint restores both cards, the card bus and its drives, and
--      the same desktop
--   5  a configuration with a device on pci_1 and no card there is refused
--   6  Open Firmware, stopped at its prompt on ttya (the 8500 takes nvram.
--      settings), built its generic nodes for both: /bandit/pci1000,3@D and
--      /bandit/pci1013,a0@E -- "Open Firmware queries PCI cards that contain
--      no FCode and creates basic node entries" (Designing PCI Cards and
--      Drivers, 1999, p.91).  The device tree is what an ARC firmware on
--      Open Firmware hands Windows NT.

local mac = dofile("../lib/mac.lua")

local hd_76 = "../../data/systems/system_7_6_170mb_24ac.img"
local finder = "../suite-tnt/goldens/pm8500-7.6-640x480x32-finder.png"

-- Bandit 1's configuration cycle (CONFIG_ADDR/DATA at $F2800000/$F2C00000,
-- little-endian through the byte-lane swap): IDSEL bit 13 is socket A1.
local function swap32(v)
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF)
end
local function cfg_rd(reg)
    machine.memory.poke.l(0xF2800000, swap32(0x00002000 | reg))
    local v = swap32(machine.memory.peek.l(0xF2C00000))
    machine.memory.poke.l(0xF2800000, 0)
    return v
end

-- 1
machine.boot{model = "pm8500", ram = 32768, rom = vars.ROM, slots = "1=sym53c825;2=cirrus_54m30"}
machine.rtc.time = "2026-04-26T21:30:36"
assert(machine.pci.slot[1].card.name == "53C825A (slot 1)", "the SCSI card is not in A1")
assert(machine.pci.slot[2].card.name == "Cirrus 54M30", "the 54M30 is not in B1")
machine.scsi.attach_hd(hd_76, 0)
-- The card's bus takes drives like any other: by its own object, and by
-- storage position through the machine.  Mac OS has no driver for the card,
-- so neither is ever seen by the guest here; NT's symc810.sys would.
assert(machine.pci_1.attach_hd(hd_76, 0), "the card bus did not take a hard disk")
local bay = machine.attach_media("pci_1", 3, "cd", hd_76)
assert(bay.bus == "pci_1" and bay.id == 3, F"attach_media answered ${bay}")
assert(machine.pci_1.device[0].type == "hd", "no hard disk at ID 0 on the card bus")
assert(machine.pci_1.device[3].type == "cdrom", "no CD-ROM at ID 3 on the card bus")
echo("1: both cards seated; pci_1 carries a hard disk at ID 0 and a CD-ROM at ID 3")

-- 2 (suite-tnt's tnt_hd_boot budget and assertions)
scheduler.run(2500000000)
assert(debug.mac.globals.read("BootDrive") == 0x8023, F"BootDrive ${debug.mac.globals.read(\"BootDrive\"):04x}, not set (driver match / mount failed)")
assert(debug.mac.globals.read("DSErrCode") == 0, F"system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")
machine.screen.match(finder, 0, 500, 20, 640)
echo("2: System 7.6 at the Finder from MESH, the desktop identical to the plain 8500's")

-- 3
assert(cfg_rd(0) == 0x00031000, F"A1 config ID ${cfg_rd(0):08x}, not the 53C825A (device 3, vendor 1000)")
echo("3: A1's configuration space answers 1000:0003")

-- 4
local ck = F"${vars.WORK_DIR}/tnt-pci-scsi-card.gscp"
checkpoint.save(ck)
checkpoint.load(ck)
assert(machine.pci.slot[1].card.name == "53C825A (slot 1)", "the restored machine lost the SCSI card")
assert(machine.pci.slot[2].card.name == "Cirrus 54M30", "the restored machine lost the 54M30")
assert(machine.pci_1.device[0].type == "hd" and machine.pci_1.device[3].type == "cdrom", "the restored card bus lost its drives")
machine.screen.match(finder, 0, 500, 20, 640)
echo("4: a checkpoint restores both cards, the card bus with its drives, and the desktop")
mac.report_perf("tnt-pci-scsi-card/finder", scheduler.instr_count)

-- 5
assert(not gs.truthy(function() return machine.boot{model = "pm8500", rom = vars.ROM, config = "{\"model\":\"pm8500\",\"storage\":[{\"bus\":\"pci_1\",\"unit\":0,\"type\":\"hd\"}]}"} end), "a device on pci_1 with no card there must be refused")
echo("5: a device on pci_1 without a card there is refused")

-- 6
assert(machine.boot{model = "pm8500", rom = vars.ROM, config = "{\"model\":\"pm8500\",\"cards\":[{\"slot\":\"pci_1\",\"card\":\"sym53c825\"},{\"slot\":\"pci_2\",\"card\":\"cirrus_54m30\"}],\"nvram\":{\"auto-boot?\":\"false\",\"input-device\":\"ttya\",\"output-device\":\"ttya\"}}"}, "the 8500 with both cards and its console on ttya")
local of = ""
local spins = 0
while not gs.truthy(function() return gs.contains(of, "0 > ") end) and spins < 200 do
    scheduler.run(25000000)
    of = F"${of}${machine.scc.a.sent()}"
    spins = spins + 1
end
assert(gs.contains(of, "0 > "), "Open Firmware never reached its prompt on ttya")
machine.scc.a.receive("dev / ls\r")
local tree = ""
spins = 0
while spins < 80 do
    scheduler.run(25000000)
    tree = F"${tree}${machine.scc.a.sent()}"
    spins = spins + 1
end
assert(gs.contains(tree, "/pci1000,3"), F"no Open Firmware node for the 53C825A card: ${tree}")
assert(gs.contains(tree, "/pci1013,a0"), "no Open Firmware node for the 54M30 card")
echo("6: Open Firmware built /bandit/pci1000,3 and /bandit/pci1013,a0 for the two cards")

echo("tnt-pci-scsi-card: a 53C825A SCSI card with its own bus and a Cirrus 54M30 in an 8500's sockets; System 7.6 boots past them to the unchanged desktop")
quit()
