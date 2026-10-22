-- Converted from test.script by scripts/script-to-lua.py
-- TNT family suite — System 7.6 from a MESH disk, and the PCI display rows.
--
-- Row map:
--
--   pm7500-76-hd     pm7500 (601) x 7.6 on the internal MESH bus: the
--                    complete boot chain to the Finder desktop.
--   pm8500-76-hd     pm8500 (604): the same chain on the machine the 604
--                    boot wall used to kill.
--   pm9500-76-mach64 pm9500, which has no onboard video: the same chain,
--                    every pixel drawn by an ATI Mach64 GX in PCI socket A1.
--   pm9500-mach64-depths  the same card with 4 MB and a 17" Multiple Scan:
--                    1024x768 picked from the Control Strip, then thousands
--                    and millions of colours, each drawing a Finder window.
--   tnt-voodoo2-display  pm8500 + a Voodoo2 in A1: the card takes the
--                    monitor from Control's live desktop and gives it back
--                    byte-identical.
--   pm7500-76-8bpp-restart  pm7500 with the display pinned to 256 colours
--                    in the Grand Central NVRAM, power-cycled: the second
--                    boot reaches the Finder at 640x480 8 bpp (#115).
--   pm7500-76-cd     pm7500 with a disc in the default configuration's
--                    CD-ROM drive (ID 3 on the 53C94 bus, machine.scsi2):
--                    System 7.6 mounts it beside the startup volume.
--
-- ---- the boot chain (tnt_hd_boot) -------------------------------------------
-- The boot-driver selection matches (XPRAM $77 "Default OS" = 1 through the
-- banked NVRAM — the bank-port 16-bit access law — against the DDM's
-- ddType), the DDM/partition-map/driver-partition reads run as complete
-- byte-perfect MESH transactions, the Apple_Driver43 loads, the boot mounts
-- the startup volume (BootDrive set), and System 7.6 starts all the way to
-- the Finder desktop.  The last wall on that chain — the loaded driver's
-- data phases all disconnecting — fell to the MESH SEQ_BUSFREE expect-free
-- law (mesh.c): BUSFREE with the target still REQing raises the
-- phase-mismatch exception instead of releasing, which is the very signal
-- the driver's [114]==4 probe path uses to continue into the data phase.
--
-- The image is 68k-install-flavored (its System lacks the 7500's gpch 68
-- patch resource) — empirically irrelevant: the boot reaches the Finder
-- regardless.  Each row attaches it directly; every writable attach mints
-- its own delta instance, so no row sees another row's writes.
--
-- The startup chime is asserted on all three models: they share one Grand
-- Central + AWACS sound path (tnt.c), so a model on which it went quiet has
-- lost something the other two still have.
--
-- Same discipline as the ROM ladder: clean bounded runs, no PC logpoints,
-- assertions read state afterward.

local mac = dofile("../lib/mac.lua")

local hd_76 = "../../data/systems/system_7_6_170mb_24ac.img"

-- Boot `model` (with PCI card `card` in the first socket, "" for none) from
-- the 7.6 image on MESH id 0, run `budget` instructions, and assert what
-- every TNT disk boot owes: the startup volume, a clean startup, and the
-- chime.
local function tnt_hd_boot(model, card, budget)
    machine.boot{model = model, ram = 32768, rom = vars.ROM, pci_card = card}
    machine.rtc.time = "2026-04-26T21:30:36"
    machine.scsi.attach_hd(hd_76, 0)

    -- Through OF, POST, the 68k boot, the gray desktop, the SCSI scan
    -- (complete MESH transactions against the disk), driver-partition load,
    -- the boot-volume mount, the Mac OS splash, and the full System 7.6
    -- startup to the Finder desktop.
    scheduler.run(budget)

    -- The boot-driver selection matched and the volume mounted: BootDrive
    -- carries the boot drive number instead of the scan's $FFFF.  ($23, not
    -- $03: the .Sony driver owns the floppy drive and its drive numbers now
    -- that the SWIM3 is wired, and the SCSI volume lands after it.)
    assert(debug.mac.globals.read("BootDrive") == 0x8023, F"${model}: BootDrive ${debug.mac.globals.read(\"BootDrive\"):04x}, not set (driver match / mount failed)")

    -- Startup completed without a system error: DSErrCode was explicitly
    -- cleared by the running system (the scan-era sentinel is $FFFF; the
    -- old data-phase wall bombed here with $66 dsOldSystem).
    assert(debug.mac.globals.read("DSErrCode") == 0, F"${model}: system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")

    -- The 68k startup chime played: the Sound Manager's DBDMA ring on
    -- channel 8 ran past the firmware beep's 51 928 frames and louder than
    -- its peak (2 921).  It needs the channel's completion interrupt to
    -- reach the 68k side — a LEVEL in Grand Central's Levels register, the
    -- only thing the NanoKernel's handler classifies from; modelled as a
    -- pulse, the ring parked after its first two buffers and the machine
    -- was silent after the beep (the chime, alert beeps, every game).
    echo(F"${model}: sound frames=${machine.sound.frames} peak=${machine.sound.peak}")
    assert(machine.sound.frames > 55000, F"${model}: the startup chime never played (frames ${machine.sound.frames})")
    assert(machine.sound.peak > 4000, F"${model}: the startup chime was silent (peak ${machine.sound.peak})")
end

-- ---- pm7500-76-hd -----------------------------------------------------------
local function row_pm7500_76_hd()
    local t0 = scheduler.host_user_ns
    tnt_hd_boot("pm7500", "", 2500000000)
    -- The Finder desktop: menu bar, mounted "Macintosh HD" volume, Trash,
    -- and the Energy Saver first-boot dialog.  The menu-bar clock area is
    -- excluded — it tracks the pinned RTC plus guest uptime, so it shifts
    -- whenever model changes move boot timing.
    mac.check_ex("goldens/pm7500-7.6-640x480x32-finder.png", 0, 500, 20, 640)
    -- Phase 0 throughput floor of the predecoded-cores proposal (604
    -- interpreter through OF, the 68K emulator and the System 7.6 boot:
    -- measured 76 MIPS on the assert-enabled build, floor well below).
    mac.mips_floor("pm7500-76-hd", t0, 20)
    mac.report_row("7.6", "control", "hd-boot mesh chime")
    mac.report_perf("suite-tnt/pm7500-76-hd", scheduler.instr_count)
end

-- ---- pm8500-76-hd -----------------------------------------------------------
-- The same chain on the 604 machine that the boot wall used to kill: the 604
-- model's hardware-split path for misaligned page-crossing stores dropped
-- the update-form rA writeback, shearing the 68k emulator's A7 during the
-- boot-time Name-Registry import (pinned FE03 workspace at $3FF040,
-- clobbered return address, runaway into zeroed RAM).  This row holds the
-- fix: the same disk image boots to the Finder on the 604.
local function row_pm8500_76_hd()
    tnt_hd_boot("pm8500", "", 2500000000)
    mac.check_ex("goldens/pm8500-7.6-640x480x32-finder.png", 0, 500, 20, 640)
    mac.report_row("7.6", "control", "hd-boot mesh chime 604")
    mac.report_perf("suite-tnt/pm8500-76-hd", scheduler.instr_count)
end

-- ---- pm9500-76-mach64 -------------------------------------------------------
-- The 9500 "requires a display card in a PCI slot" — it has no built-in video
-- — so this row stages one and asserts the whole chain end to end: the card
-- enumerates, Open Firmware assigns it, the ndrv in its own expansion ROM
-- drives it, and System 7.6 reaches the Finder on it.  Its CRTC supplies the
-- display descriptor, its RGB514 the palette, and its draw engine paints the
-- desktop.  It needs a longer budget than the Control machines.
--
-- The golden was captured once and inspected by hand (TESTING.md: goldens are
-- reviewed, never REGEN'd to green).  What that inspection established,
-- recorded here so nobody has to derive it again:
--
-- The desktop dither is structurally IDENTICAL to the pm8500 row's — same
-- checkerboard, same phase, same pixel positions — but the two goldens do not
-- share a single desktop pixel VALUE: this row renders the pair as
-- (135,135,135) / (193,193,193), the 8500 renders it as (96,96,96) /
-- (160,160,160).  That is expected, and it is worth knowing why before anyone
-- "fixes" it:
--
--   * The pixel INDICES are $81 and $F8.  In Apple's standard 8-bit CLUT those
--     are the neutral cube entry (102,102,102) and the fourth gray ramp entry
--     (170,170,170) — so both machines are drawing the same two logical
--     colours, and the difference is entirely in the CLUT.
--   * A Mac video driver writes its CLUT through a gamma table, and the ndrv
--     in this card's own expansion ROM carries a different one from
--     Control's.  135/193 is 102/170 under roughly a 1.4 gamma.
--   * The RGB514 is an 8-bit DAC and this model keeps all eight bits verbatim
--     (mach64_refresh_clut is a straight byte copy).  The 8500's numbers are
--     its linear values with the low nibble dropped — 102 -> 96, 170 -> 160 —
--     so the Control path is the lossy one, not this.
--
-- In other words the two rows disagreeing about desktop grey is a fact about
-- two different DACs and two different gamma tables, not a defect in either.
local function row_pm9500_76_mach64()
    tnt_hd_boot("pm9500", "mach64_gx", 4000000000)

    -- The Control/Chaos entry is a BUILTIN_FALLBACK: with a display card in a
    -- socket it stands down, and the machine is the single-display machine
    -- the hardware is.
    assert(gs.try(function() return machine.pci.slot[7].card.name end, "(empty)") == "(empty)", "no onboard video when a display card is seated")

    -- The screen is the CARD's, derived from the CRTC registers the card's
    -- own driver programmed — not a machine framebuffer.
    assert(machine.screen.width == 640, "640 pixels across")
    assert(machine.screen.height == 480, "480 lines")
    assert(machine.screen.depth == 8, "8 bpp")
    assert(machine.pci.slot[1].card.framebuffer.depth == 8, "machine.screen.source resolves to the card's framebuffer")

    -- Menu bar, "Macintosh HD", Trash, the control strip and the Energy Saver
    -- first-boot dialog — every pixel of it drawn through the Mach64's draw
    -- engine.
    mac.check_ex("goldens/pm9500-7.6-640x480x8-finder.png", 0, 500, 20, 640)
    mac.report_row("7.6", "mach64_gx", "hd-boot mesh chime pci-display")
    mac.report_perf("suite-tnt/pm9500-76-mach64", scheduler.instr_count)
end

-- ---- pm9500-mach64-depths ---------------------------------------------------
-- The deep modes of a 4 MB card (S1018-21).  Two faults hid here, and each
-- one broke a depth this row now draws a Finder window in:
--
--   * Thousands.  The driver keeps the draw engine's state in context blocks
--     in VRAM and writes them through the big-endian aperture, which swaps
--     within each pixel of MEM_PIX_WIDTH.  Read back without undoing that
--     swap, every block loaded at 16 bpp was garbage ($08C80000 as
--     $C8080000) and windows were erased to the desktop pattern.
--   * Millions.  The aperture aliased past the installed 4 MB, so the
--     driver's 6 MB write/read-back probe passed and it put the blocks at
--     "the top of 6 MB" -- $1FFxxx, inside the 3 MB frame of 1024 x 768 at
--     32 bpp.  The first desktop fill overwrote them.
--
-- The resolution and depth are chosen the way a user does it, from the
-- Control Strip's two monitor modules, so the driver's own mode switch runs.
local function mach64_strip(x, y, item_x, item_y)
    machine.adb.mouse.move(x, y, "global")
    mac.wait_cursor(20)
    machine.adb.mouse.click(true)
    mac.run_ticks(30)
    machine.adb.mouse.move(item_x, item_y, "global")
    mac.run_ticks(10)
    machine.adb.mouse.click(false)
    mac.run_ticks(300)
end

local function row_pm9500_mach64_depths()
    machine.boot{model = "pm9500", ram = 32768, rom = vars.ROM, pci_card = "mach64_gx", pci_option = "vram=4m,monitor=17in_multi"}
    machine.rtc.time = "2026-04-26T21:30:36"
    machine.scsi.attach_hd(hd_76, 0)
    scheduler.run(4000000000)
    assert(debug.mac.globals.read("DSErrCode") == 0, F"system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")
    assert(machine.screen.width == 832, "the 17-inch Multiple Scan starts at 832 x 624")

    -- Close the Energy Saver message; then the resolution module (third
    -- item, "1024 x 768, 75Hz").
    machine.adb.keyboard.press("Return")
    mac.run_ticks(30)
    mach64_strip(130, 611, 180, 594)
    assert(machine.screen.width == 1024 and machine.screen.height == 768, "1024 x 768 was not taken")

    -- The depth module: "Thousands of Colors", then open the startup disk.
    mach64_strip(100, 755, 150, 726)
    assert(machine.screen.depth == 16, "thousands is 16 bpp")
    mac.double_click(984, 52)
    mac.run_ticks(300)
    mac.check_ex("goldens/pm9500-mach64-1024x768x16-window.png", 0, 860, 20, 940)

    -- "Millions of Colors": the window redraws over a 3 MB frame.
    mach64_strip(100, 755, 150, 739)
    assert(machine.screen.depth == 32, "millions is 32 bpp")
    mac.check_ex("goldens/pm9500-mach64-1024x768x32-window.png", 0, 860, 20, 940)
    mac.report_row("7.6", "mach64_gx", "1024x768 thousands millions")
end

-- ---- tnt-voodoo2-display ----------------------------------------------------
-- The Voodoo2 pass-through switch against a live System 7.6 desktop
-- (docs/internals/core/peripherals/pci/cards/voodoo2.md).  A pm8500 boots to the Finder
-- on its soldered Control video with a Voodoo2 seated in socket A1 — the
-- machine a real owner had.  The card then takes the monitor (fbiInit0[0],
-- video timing programmed, unblanked, outputs driven), shows its own
-- rendering, and releases it — and the desktop must come back BYTE-IDENTICAL,
-- the strongest available statement that the takeover was non-destructive.
--
-- The mechanism under test is deliberately NOT new code: tnt_display()
-- resolves the screen through pci_primary_display_card(), which calls every
-- card's ops->display() fresh each frame.  The Voodoo2 answers NULL while
-- passing through and its descriptor while driving, so the monitor follows
-- fbiInit0[0] at frame granularity with no change to the generic layer.
--
-- The desktop is the pm8500 row's golden, deliberately: a ROM-less card in a
-- socket changes nothing on Control's screen, so the identity is itself an
-- assertion.  A checkpoint taken while the card is driving must restore to
-- the identical framebuffer checksum.

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
-- The card's register BAR, set by the row once the card is enumerated.
local V2_BAR = 0
local function vreg_wr(off, val)
    machine.memory.poke.l(V2_BAR + off, swap32(val))
end
local function vreg_rd(off)
    return swap32(machine.memory.peek.l(V2_BAR + off))
end

local function row_tnt_voodoo2_display()
    -- System 7.6 to the Finder — with the ROM-less card seated the whole
    -- way.  Mac OS finds no driver for pci121a,2 and must not care (a
    -- missing/ignored Voodoo2 causes no boot problem).
    tnt_hd_boot("pm8500", "voodoo2", 2500000000)

    -- --- The idle card yields: Control draws the desktop. -----------------
    assert(not machine.pci.slot[1].card.video.drives_monitor, "the card powers up passing through")
    assert(machine.screen.depth == 32, "the desktop is Control's millions-of-colours mode")
    assert(machine.screen.width == 640, "Control's 640x480")
    mac.check_ex("goldens/pm8500-7.6-640x480x32-finder.png", 0, 500, 20, 640)
    mac.report_row("7.6", "control+voodoo2", "hd-boot pci-passthrough take-release checkpoint")

    -- --- The card takes the monitor. --------------------------------------
    -- Exactly what the Glide library does: enable Memory Space itself, open
    -- the init gate, program the video, unblank, drive the outputs, and set
    -- the pass-through bit.  (The guest keeps running underneath — the
    -- takeover is asynchronous to it, like the real cable switch.)
    cfg_wr(0x04, cfg_rd(0x04) | 0x0002)
    cfg_wr(0x40, 0x00000003)
    V2_BAR = machine.pci.slot[1].card.config.bar[0].base
    -- 640x480 timing, front-buffer layout, LFB reads on.
    vreg_wr(0x220, (704 << 16) | 96)
    vreg_wr(0x224, (523 << 16) | 2)
    vreg_wr(0x208, (25 << 16) | 38)
    vreg_wr(0x20C, (480 << 16) | 639)
    vreg_wr(0x218, (vreg_rd(0x218) & 0xFFF007FF) | (150 << 11) | 0x400000)
    vreg_wr(0x214, (vreg_rd(0x214) & 0xFEFFFF0F) | 0xA0)
    vreg_wr(0x214, vreg_rd(0x214) | 0x8)
    -- The card renders before it takes the screen: clear to a deep blue and
    -- draw one gouraud triangle — unmistakably not a Macintosh desktop.
    vreg_wr(0x118, (0 << 16) | 640)
    vreg_wr(0x11C, (0 << 16) | 480)
    vreg_wr(0x148, 0x00000060)
    vreg_wr(0x130, 0x00000000)
    vreg_wr(0x110, 0x00000601)
    vreg_wr(0x124, 0x00000000)
    vreg_wr(0x110, 0x00000600)
    vreg_wr(0x104, 0x00000000)
    vreg_wr(0x008, 0x00000C80)
    vreg_wr(0x00C, 0x00000640)
    vreg_wr(0x010, 0x00000C80)
    vreg_wr(0x014, 0x00001400)
    vreg_wr(0x018, 0x00001E00)
    vreg_wr(0x01C, 0x00001400)
    vreg_wr(0x020, 0x000FF000)
    vreg_wr(0x024, 0x00000000)
    vreg_wr(0x028, 0x00000000)
    vreg_wr(0x040, 0x00000000)
    vreg_wr(0x044, 0x00000600)
    vreg_wr(0x048, 0x00000000)
    vreg_wr(0x060, 0x00000000)
    vreg_wr(0x064, 0x00000000)
    vreg_wr(0x068, 0x00000C00)
    vreg_wr(0x080, 0x80000000)
    -- NOW the switch: video out of reset, unblanked, outputs driven,
    -- pass-through taken — the pass-through predicate turns true in one place.
    vreg_wr(0x214, (vreg_rd(0x214) & 0xFFFFEEFF) | 0x1E000)
    vreg_wr(0x210, vreg_rd(0x210) | 0x1)
    assert(machine.pci.slot[1].card.video.drives_monitor, "the predicate reads true once everything is driven")
    -- Let a frame pass so every consumer re-resolves the display chain.
    scheduler.run(20000000)
    assert(machine.screen.depth == 32, "the screen is the Voodoo2's DAC output: 5-6-5 expanded to 8-8-8 through the gamma CLUT")
    assert(machine.screen.width == 640, "..at the card's programmed videoDimensions")
    assert(machine.screen.height == 480, "..both axes")
    assert(machine.screen.source.displayed_buffer == 0, "screen.source resolves to the CARD's framebuffer node")
    -- The card's own rendering.
    mac.check("goldens/pm8500-voodoo2-640x480x32-frame.png")

    -- --- A checkpoint taken while the card is driving restores exactly. ---
    local csum = machine.screen.checksum(0, 0, 480, 640)
    assert(checkpoint.save(F"${vars.WORK_DIR}/voodoo2-mid-frame.checkpoint"), "checkpoint save")
    assert(checkpoint.load(F"${vars.WORK_DIR}/voodoo2-mid-frame.checkpoint"), "checkpoint load")
    assert(machine.screen.depth == 32, "restored with the card still driving")
    assert(machine.screen.checksum(0, 0, 480, 640) == csum, "the framebuffer checksum survives the round trip")

    -- --- Release: the desktop comes back byte-identically. ----------------
    -- The shell's peek/poke bindings survive the restore; the card hands the
    -- monitor back by clearing the one bit.
    machine.memory.poke.l(0xF2800000, swap32(0x00002004))
    machine.memory.poke.l(0xF2C00000, swap32(0x00000002))
    machine.memory.poke.l(0xF2800000, 0)
    vreg_wr(0x210, vreg_rd(0x210) & 0xFFFFFFFE)
    assert(not machine.pci.slot[1].card.video.drives_monitor, "released")
    scheduler.run(20000000)
    assert(machine.screen.depth == 32, "Control is the screen again")
    -- THE strongest statement in the row: the same golden as before the
    -- takeover, byte for byte — the takeover was non-destructive.
    mac.check_ex("goldens/pm8500-7.6-640x480x32-finder.png", 0, 500, 20, 640)
    mac.report_perf("suite-tnt/tnt-voodoo2-display", scheduler.instr_count)
end

-- ---- pm7500-76-8bpp-restart -------------------------------------------------
-- The 256-colour restart (#115) — which was never about the colours.  A
-- boot on a store that already carries POST's "RobG" log (every boot but
-- the first on a formatted store) has POST test BoxID bit 8, and with the
-- factory-test strap modelled clear it parked in the ROM's Serial Test
-- Manager before Open Firmware ran: no device tree, no BARs, a black
-- screen.  Choosing 256 Colours was simply what made the reporter restart
-- (docs/internals/machines/tnt/tnt.md, "Machine identity").  This row is the
-- regression hold for that law AND the depth lever the issue asked for:
-- the Monitors depth lives neither on the disk nor in the Cuda PRAM but in
-- Grand Central's NVRAM, in the Control driver's persistent `gprf` Name
-- Registry record (the offsets below were found by diffing `machine.nvram`
-- before and after one click on 256 Colours).  The row boots once so Open
-- Firmware formats the store and Mac OS writes the record, pins the depth
-- byte through `machine.nvram` — the lever the 68k rows have in
-- `machine.rtc.pram.poke` — and power-cycles.  The store follows the power
-- switch (machine.restart never tears the machine down, so the part is simply
-- untouched; machine.boot would hand a new machine a blank one), so the second boot is exactly the one a real owner
-- gets after choosing 256 Colours and restarting: POST takes the warm-log
-- path, Open Firmware assigns Control's BARs, the ndrv programs the 8 bpp
-- mode, and System 7.6 draws its desktop through the CLUT.
local GPRF_TAG = 0x1409 -- the 4-byte property name "gprf"
local GPRF_DEPTH = 0x1410 -- depth index inside the record: 0 = 256 colours, 2 = millions
local function row_pm7500_76_8bpp_restart()
    tnt_hd_boot("pm7500", "", 2500000000)
    assert(machine.screen.depth == 32, "the first boot is the default millions-of-colours desktop")

    -- The record the ndrv saved on this first boot, where this ROM's Name
    -- Registry put it — a layout shift fails here, not as a silent no-op.
    assert(machine.nvram.peek(GPRF_TAG) == 0x67 and machine.nvram.peek(GPRF_TAG + 1) == 0x70 and machine.nvram.peek(GPRF_TAG + 2) == 0x72 and machine.nvram.peek(GPRF_TAG + 3) == 0x66, "no 'gprf' record at NVRAM $1409 — the Name Registry's NVRAM layout moved")
    assert(machine.nvram.peek(GPRF_DEPTH) == 2, "the saved depth index is not the default millions (2)")

    -- Choose 256 Colours the way the control panel's click lands in the
    -- store, and press the power switch.
    machine.nvram.poke(GPRF_DEPTH, gs.bytes(0x00, 1))
    machine.restart()
    assert(machine.nvram.peek(GPRF_DEPTH) == 0, "the store did not survive the power-cycle")
    scheduler.run(2500000000)

    assert(debug.mac.globals.read("BootDrive") == 0x8023, F"second boot: BootDrive ${debug.mac.globals.read(\"BootDrive\"):04x}, not set")
    assert(debug.mac.globals.read("DSErrCode") == 0, F"second boot: system error ${debug.mac.globals.read(\"DSErrCode\"):04x}")
    -- Control came up: the BARs were assigned (the raster is not the blank
    -- buffer) and the ndrv's 8 bpp mode is what the screen shows.
    assert(machine.screen.width == 640, "640 pixels across")
    assert(machine.screen.height == 480, "480 lines")
    assert(machine.screen.depth == 8, "8 bpp — the pinned 256 colours")
    assert(machine.screen.format == "8bpp_clut", "indexed through the RaDACal CLUT")
    -- The golden carries the "not shut down properly" alert over the 8 bpp
    -- desktop: the power switch was thrown on a running desktop, so the
    -- Finder says so — deterministic, and itself proof the first boot was
    -- a complete one.  Menu-bar clock excluded as in the other rows.
    mac.check_ex("goldens/pm7500-7.6-640x480x8-finder.png", 0, 500, 20, 640)
    mac.report_row("7.6", "control", "hd-boot nvram-depth restart 8bpp")
    mac.report_perf("suite-tnt/pm7500-76-8bpp-restart", scheduler.instr_count)
end

-- ---- pm7500-76-cd -----------------------------------------------------------
-- The factory CD-ROM drive, on the bus the real machine has it on: the
-- 53C94's (machine.scsi2), not MESH.  The default configuration carries it,
-- so `cd=` and a CD drop have a drive to go into.  The disc is a bare HFS
-- volume (no partition map), which the Apple CD-ROM driver mounts as is.
-- Getting there takes the SCSI Manager's old-API path: a DMA select ($C1)
-- that waits for the 53C94's DREQ, which the HAL reads as DBDMA channel 0's
-- device status bit s5 -- before that was wired the Finder never came up.
local cd_disc = "../../data/systems/SSW-7.6-1.4M/Disk Tools 1.img"
local function row_pm7500_76_cd()
    machine.boot{model = "pm7500", ram = 32768, rom = vars.ROM}
    machine.rtc.time = "2026-04-26T21:30:36"
    assert(machine.scsi2.device[3].type == "cdrom", "the default configuration has no CD-ROM drive at machine.scsi2 ID 3")
    machine.scsi.attach_hd(hd_76, 0)
    machine.attach_cdrom(cd_disc)
    scheduler.run(2500000000)

    assert(debug.mac.globals.read("BootDrive") == 0x8023, F"pm7500-cd: BootDrive ${debug.mac.globals.read(\"BootDrive\"):04x}, not set")
    assert(debug.mac.globals.read("DSErrCode") == 0, F"pm7500-cd: system error ${debug.mac.globals.read(\"DSErrCode\"):04x} during startup")

    -- Two volumes on the VCB queue (VCBQHdr.qHead at $358), the second the
    -- disc: its vcbVN (a Pascal string at +$2C) is "Disk Tools 1" (read_cstring
    -- answers it quoted).
    local vcb = machine.memory.peek.l(0x358)
    assert(vcb ~= 0, "no volume mounted")
    local disc = machine.memory.peek.l(vcb)
    assert(disc ~= 0, "the disc did not mount (one volume on the VCB queue)")
    assert(machine.memory.peek.l(disc) == 0, "more than two volumes mounted")
    assert(machine.memory.peek.b(disc + 0x2C) == 12 and machine.memory.read_cstring(disc + 0x2D, 12) == "\"Disk Tools 1\"", "the second volume is not the disc")
    mac.report_row("7.6", "control", "hd-boot cd-mount 53c94")
    mac.report_perf("suite-tnt/pm7500-76-cd", scheduler.instr_count)
end

-- ---- row dispatch -----------------------------------------------------------

if mac.row_on("pm7500-76-hd") then
    if mac.have_media(hd_76) then
        mac.row("pm7500-76-hd", row_pm7500_76_hd)
    else
        echo("skip: pm7500-76-hd (7.6 image not fetched)")
    end
end
if mac.row_on("pm8500-76-hd") then
    if mac.have_media(hd_76) then
        mac.row("pm8500-76-hd", row_pm8500_76_hd)
    else
        echo("skip: pm8500-76-hd (7.6 image not fetched)")
    end
end
if mac.row_on("pm9500-76-mach64") then
    if mac.have_media(hd_76) then
        mac.row("pm9500-76-mach64", row_pm9500_76_mach64)
    else
        echo("skip: pm9500-76-mach64 (7.6 image not fetched)")
    end
end
if mac.row_on("pm9500-mach64-depths") then
    if mac.have_media(hd_76) then
        mac.row("pm9500-mach64-depths", row_pm9500_mach64_depths)
    else
        echo("skip: pm9500-mach64-depths (7.6 image not fetched)")
    end
end
if mac.row_on("tnt-voodoo2-display") then
    if mac.have_media(hd_76) then
        mac.row("tnt-voodoo2-display", row_tnt_voodoo2_display)
    else
        echo("skip: tnt-voodoo2-display (7.6 image not fetched)")
    end
end
if mac.row_on("pm7500-76-8bpp-restart") then
    if mac.have_media(hd_76) then
        mac.row("pm7500-76-8bpp-restart", row_pm7500_76_8bpp_restart)
    else
        echo("skip: pm7500-76-8bpp-restart (7.6 image not fetched)")
    end
end
if mac.row_on("pm7500-76-cd") then
    if mac.have_media(hd_76) and mac.have_media(cd_disc) then
        mac.row("pm7500-76-cd", row_pm7500_76_cd)
    else
        echo("skip: pm7500-76-cd (7.6 image or Disk Tools disc not fetched)")
    end
end

mac.suite_done()
quit()
