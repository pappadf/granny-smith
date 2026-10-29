-- Converted from test.script by scripts/script-to-lua.py
local bus_clock_lcd -- declared up front: read by functions defined before them
-- The Apple Network Server's Open Firmware device tree, matched against
-- Apple's published Listing 6-1.
--
-- The tree is an OUTPUT, not a build target.  Every node below exists
-- because Open Firmware probed a register we model and decided the device
-- was there; a missing node means a missing or wrong register, and an extra
-- one means we are pretending to be hardware this machine does not have.

local mac = dofile("../lib/mac.lua")
dofile("../lib/ans.lua")

-- Console plumbing (chunked input, the prompt wait, and the two-cold-boot
-- serial-console setup) lives in the library — see its header for why a
-- Network Server's console is the screen until it is told otherwise.
local function send(s) ans_send(s) end
local function go() return ans_go() end

local function tree(model, ram_kb, bus_clock, l2_label)
    -- Two cold boots: the machine's shipping console is the 54M30, and the
    -- row wants the serial port.  See lib/ans.script.
    local banner = ans_boot_serial(model, ram_kb)
    assert(gs.contains(banner, "OpenFirmware1.1.22"), F"S6: ${model} Open Firmware did not announce itself")
    assert(gs.contains(banner, "0 > "), F"S6: ${model} no Open Firmware prompt")
    -- The firmware's own default boot path, from NVRAM: the ANS boots AIX
    -- off a fast/wide disk, and says so even with no disk attached.
    assert(gs.contains(banner, "/bandit/53c825@11/sd@"), F"S6: ${model} the default boot path is not a fast/wide disk")
    assert(gs.contains(banner, ":aix"), F"S6: ${model} the default boot path has no :aix suffix")

    -- NOT asserted here: the verbose cold-boot narration this firmware
    -- prints on its FIRST boot over a blank store — `TTYa initialized.`,
    -- `SECURITY ShinerESB 1st-825 2nd-825 gc 53c94 mace escc …`, `_cold-init`,
    -- `install-console`.  That output is real, and it is the most direct
    -- readout of `?esb` there is, but it appears only once: the Grand
    -- Central NVRAM part is NON-VOLATILE and its contents survive a machine
    -- rebuild, so every boot after the first one goes straight to its boot
    -- attempt.  This row cold-boots twice by construction (the console
    -- setup), so asserting on it would depend on boot order — exactly the
    -- kind of order-dependence R10 warns about.  `?esb` is read below from
    -- the firmware's own vocabulary instead, which does not care.

    -- --- S5: the flag, read back from the firmware's own vocabulary ---------
    send("?esb .")
    local esb = go()
    assert(gs.contains(esb, "-1"), F"S5: ${model} ?esb is not true")

    -- --- S7: the tree matches Listing 6-1 -----------------------------------
    send("dev / l")
    send("s")
    local ls = go()
    -- The two fast/wide controllers at the unit addresses Apple prints in
    -- hex (@11 / @12) against the decimal IDSELs (17 / 18) of its interrupt
    -- table — confusing the two puts every device at the wrong config
    -- address, so this is the assertion that catches it.
    assert(gs.contains(ls, "/53c825\\x00@11"), F"S7: ${model} 53c825@11 missing")
    assert(gs.contains(ls, "/53c825\\x00@12"), F"S7: ${model} 53c825@12 missing")
    assert(gs.contains(ls, "/gc\\x00@10"), F"S7: ${model} gc@10 missing")
    assert(gs.contains(ls, "/54m30\\x00@F"), F"S7: ${model} 54m30@F missing")
    assert(gs.contains(ls, "/hammerhead\\x00@F8000000"), F"S7: ${model} hammerhead missing")
    assert(gs.contains(ls, "/bandit\\x00@F2000000"), F"S7: ${model} bandit 1 missing")
    assert(gs.contains(ls, "/bandit\\x00@F4000000"), F"S7: ${model} bandit 2 missing")
    -- MESH IS GONE — delta #4, and a deletion is still a fact to assert.
    assert(not gs.truthy(function() return gs.contains(ls, "/mesh") end), F"S7: ${model} the Network Servers have no MESH")
    -- Grand Central's subtree is EXACTLY the 9500's, and its six children
    -- are the ones AIX's own `devices.pci.gc` fileset enumerates by name:
    -- `53c94 mace ch-a ch-b swim3 via-cuda`.
    assert(gs.contains(ls, "/53c94\\x00@10000"), F"S7: ${model} 53c94 missing")
    assert(gs.contains(ls, "/mace\\x00@11000"), F"S7: ${model} mace missing")
    assert(gs.contains(ls, "/escc\\x00@13000"), F"S7: ${model} escc missing")
    assert(gs.contains(ls, "/ch-a\\x00@13020"), F"S7: ${model} escc ch-a missing")
    assert(gs.contains(ls, "/ch-b\\x00@13000"), F"S7: ${model} escc ch-b missing")
    assert(gs.contains(ls, "/awacs\\x00@14000"), F"S7: ${model} awacs missing")
    assert(gs.contains(ls, "/swim3\\x00@15000"), F"S7: ${model} swim3 missing")
    assert(gs.contains(ls, "/via-cuda\\x00@16000"), F"S7: ${model} via-cuda missing")
    assert(gs.contains(ls, "/keyboard\\x00@0,0"), F"S7: ${model} adb keyboard missing")
    assert(gs.contains(ls, "/mouse\\x00@1,0"), F"S7: ${model} adb mouse missing")
    assert(gs.contains(ls, "/nvram\\x00@1D000"), F"S7: ${model} nvram missing")
    -- The LCD, which Apple's abridged listing omits and the ROM publishes.
    assert(gs.contains(ls, "/lcd\\x00@1C000"), F"S7: ${model} the front-panel LCD node is missing")

    -- --- The root node: the machine's own identity ---------------------------
    send("dev /")
    local cd = go()
    send(".propert")
    send("ies")
    local root = go()
    assert(gs.contains(root, "AAPL,ShinerESB"), F"S7: ${model} root compatible is not AAPL,ShinerESB")
    assert(gs.contains(root, "MacRISC"), F"S7: ${model} root compatible is missing MacRISC")
    -- Open Firmware publishes the BUS clock as the root clock-frequency,
    -- which is the board descriptor's `bus_hz` coming back out of the ROM.
    assert(gs.contains(root, bus_clock), F"S7: ${model} root clock-frequency != ${bus_clock}")

    -- --- The 53C825A node: the AIX binding contract --------------------------
    send("dev /ban")
    send("dit/53c8")
    send("25@11")
    local cd2 = go()
    send(".propert")
    send("ies")
    local scsi = go()
    -- `name` is what AIX matches against its ODM PdDv database, and
    -- `compatible` is the `devid` its `devices.pci.pci1000+3` fileset
    -- carries.  Neither is ours to choose — Open Firmware derives them from
    -- what it probed — which is exactly why they are worth asserting.
    assert(gs.contains(scsi, "name                    53c825"), F"S7: ${model} node name is not 53c825")
    assert(gs.contains(scsi, "model                   NCR,825A"), F"S7: ${model} model is not NCR,825A")
    assert(gs.contains(scsi, "compatible              pci1000,3"), F"S7: ${model} compatible is not pci1000,3")
    assert(gs.contains(scsi, "device_type             scsi"), F"S7: ${model} device_type is not scsi")
    assert(gs.contains(scsi, "revision-id             00000014"), F"S7: ${model} revision-id is not $14")
    -- AAPL,interrupts — the property AIX reads to build its ODM entry, NOT
    -- the PCI pin (which is 1) and NOT the raw EXT number.  The rule was
    -- first deduced as "the Grand Central interrupt-controller bit
    -- number", i.e. 20 + EXT index, from a single published data point
    -- (0x1D for a slot-6 card, which is EXT9 = GC 29).  This is the
    -- firmware's own answer for a device on EXT2: 20 + 2 = 22 = 0x16.
    assert(gs.contains(scsi, "AAPL,interrupts         00000016"), F"S7: ${model} AAPL,interrupts != $16 (the 20+EXT rule)")
    assert(gs.contains(scsi, "AAPL,connector          scsi-int"), F"S7: ${model} AAPL,connector")

    -- --- The 54M30 node: identity, and the interrupt it does NOT have -------
    send("dev /ban")
    send("dit/54m3")
    send("0@F")
    local cd3 = go()
    send(".propert")
    send("ies")
    local video = go()
    assert(gs.contains(video, "name                    54m30"), F"S7: ${model} node name is not 54m30")
    assert(gs.contains(video, "compatible              pci1013,a0"), F"S7: ${model} compatible is not pci1013,a0")
    assert(gs.contains(video, "device_type             display"), F"S7: ${model} device_type is not display")
    -- Apple states plainly that the part has no interrupt line, and the
    -- tree agrees: no `interrupts` property at all, where the 53C825A above
    -- carries one.  Allocating it an EXT position would corrupt the map.
    assert(not gs.truthy(function() return gs.contains(video, "AAPL,interrupts") end), F"S7: ${model} the 54M30 must carry no routed interrupt")

    -- --- The device aliases: the boot paths a boot command will type ---------
    send("devalias")
    local aliases = go()
    -- `pci1` and `pci2` are the two Bandits, which is the form the six
    -- per-slot RAID boot commands in Apple's own manual take.
    assert(gs.contains(aliases, "pci1                /bandit@F2000000"), F"S7: ${model} pci1 alias")
    assert(gs.contains(aliases, "pci2                /bandit@F4000000"), F"S7: ${model} pci2 alias")
    assert(gs.contains(aliases, "lcd                 /bandit/gc/lcd"), F"S7: ${model} lcd alias")
    assert(gs.contains(aliases, "screen              /bandit/54m30@F"), F"S7: ${model} screen alias")
    -- Bus @11 carries bays 0-3 and bus @12 carries 4-6 — the backplane's
    -- own split, and the reason `machine.scsi` needs a bus index here.
    assert(gs.contains(aliases, "disk0               /bandit/53c825@11/sd@0,0"), F"S7: ${model} disk0 alias")
    assert(gs.contains(aliases, "disk3               /bandit/53c825@11/sd@3,0"), F"S7: ${model} disk3 alias")
    assert(gs.contains(aliases, "disk4               /bandit/53c825@12/sd@4,0"), F"S7: ${model} disk4 alias")
    assert(gs.contains(aliases, "disk6               /bandit/53c825@12/sd@6,0"), F"S7: ${model} disk6 alias")

    -- --- The LCD settled on its banner --------------------------------------
    assert(machine.lcd.line(2) == bus_clock_lcd, F"S4: ${model} CPU banner: got [${machine.lcd.line(2)}] want [${bus_clock_lcd}]")
    assert(machine.lcd.line(3) == l2_label, F"S4: ${model} L2 banner: got [${machine.lcd.line(3)}] want [${l2_label}]")

    mac.report_perf(F"ans-device-tree/${model}", scheduler.instr_count)
    echo(F"ans-device-tree ${model}: Listing 6-1 matched; ?esb set; AAPL,interrupts confirms the 20+EXT rule")
end

-- clock-frequency is printed in hex: 44 MHz = $029F6300, 50 MHz = $02FAF080.
bus_clock_lcd = "066MHz 604, 44MHzBus"
tree("ans500", 32768, "029F6300", "0512KB Level 2 Cache")
bus_clock_lcd = "075MHz 604, 50MHzBus"
tree("ans700", 49152, "02FAF080", "1024KB Level 2 Cache")
quit()
