-- Converted from test.script by scripts/script-to-lua.py
-- The SCRIPTS engine, driven by Open Firmware's own probe words.
--
-- What a successful probe proves is a whole chain, and none of it can be
-- faked by a register model: the firmware builds a six-instruction SCRIPT in
-- RAM, hands DSP its physical address, and the engine fetches it over the
-- bus, selects the target, moves a six-byte INQUIRY command block out,
-- moves the reply in, takes STATUS and MESSAGE IN, clears ACK, and
-- interrupts.  The text the firmware then prints is the INQUIRY data that
-- came back off the emulated bus.

local mac = dofile("../lib/mac.lua")
dofile("../lib/ans.lua")

local function probe(model, ram_kb)
    local boot = ans_boot_serial(model, ram_kb)

    -- A disk on fast/wide channel 0 and a CD-ROM on channel 1 — one device
    -- per controller, so a probe that answered on the wrong bus would show
    -- up as the wrong device rather than as silence.  The disk goes in bay 1:
    -- bay 0 is the CD bay, which takes only the CD-ROM drive.
    machine.scsi.attach_hd(F"${vars.WORK_DIR}/ansdisk.img", 1)
    -- Any ISO answers INQUIRY as a removable read-only device, so this one
    -- is borrowed from the shared corpus rather than manufactured: what the
    -- row is proving is the bus the answer came back on, not the contents.
    machine.scsi2.attach_cdrom("../../data/aux/aux_3.0.1/APPLE_AUX_3-0-1_RETAIL.iso", 4)

    -- --- S8: channel 0 --------------------------------------------------------
    ans_send("probe-scsi1")
    local p1 = ans_go()
    assert(gs.contains(p1, "Target 1"), F"S8: ${model} probe-scsi1 found no target 1")
    assert(gs.contains(p1, "Unit 0"), F"S8: ${model} probe-scsi1 reported no LUN 0")
    assert(gs.contains(p1, "Disk"), F"S8: ${model} probe-scsi1 did not identify a disk")
    -- A target answers on LUN 0 and NOWHERE ELSE.  Before the shared bus
    -- model declined an unimplemented LUN the way SCSI-2 requires — INQUIRY
    -- data with peripheral qualifier 011b, device type 1Fh, status GOOD —
    -- this listed the same drive as Unit 0 through Unit 7, and AIX would
    -- have configured eight of it.
    assert(not gs.contains(p1, "Unit 1"), F"S8: ${model} a single-LUN target must not answer LUN 1")

    -- --- S8: channel 1, which is a different bus entirely --------------------
    ans_send("probe-scsi2")
    local p2 = ans_go()
    assert(gs.contains(p2, "Target 4"), F"S8: ${model} probe-scsi2 found no target 4")
    assert(gs.contains(p2, "Removable Read Only device") or gs.contains(p2, "CD"), F"S8: ${model} probe-scsi2 did not identify the CD-ROM")
    -- The disk is on channel 0 ONLY: the two controllers are separate buses,
    -- and a model that shared one would show it here.
    assert(not gs.contains(p2, "Target 1"), F"S8: ${model} channel 1 must not see channel 0's disk")

    -- --- The device aliases resolve to real media ----------------------------
    -- `disk0` is `/bandit/53c825@11/sd@0,0` and `disk4` is
    -- `/bandit/53c825@12/sd@4,0` — the backplane's own split, which is why
    -- the bays are numbered across two controllers.
    ans_send("dev disk0 device-end")
    local d0 = ans_go()
    assert(not gs.contains(d0, "not found"), F"S8: ${model} the disk0 alias does not resolve")

    mac.report_perf(F"ans-scsi/${model}", scheduler.instr_count)
    echo(F"ans-scsi ${model}: SCRIPTS engine ran INQUIRY on both fast/wide channels")
end

-- One disk, created fresh in the work directory so the row owns what it
-- writes to.
assert(files.hd_create(F"${vars.WORK_DIR}/ansdisk.img", "500M"), "could not create the test disk")

probe("ans500", 32768)
probe("ans700", 49152)
quit()
