-- Converted from test.script by scripts/script-to-lua.py
-- The SHIPPED Mac Glide driver against the Voodoo2 model.
--
-- Everything in this row is the guest's own software doing the work:
-- CFM registers the four B5 extensions at boot, `Quake 3Dfx` imports
-- 3DfxGlideLib2.x, and the library walks the Name Registry, finds
-- pci121a,2, enables Memory Space itself, and runs 3dfx's own
-- initialisation — the sequence tnt-pci-voodoo2 replays by hand, here
-- executed by the binary 3dfx shipped.  The model has to satisfy the
-- driver's own SELF-TESTS to get this far: the un-dither calibration
-- demands unique 4x4 tile sums from the dither, the TMU census renders
-- config words through the texture path, and the memory sensing writes
-- sentinels that must alias exactly.
--
-- The row then keeps running into the game itself: Quake opens the
-- screen with grSstWinOpen, streams its whole render path through the
-- CMDFIFO (state groups, TSU setup vertices, packet-5 texture rows and
-- LFB spans, SGRAM-fill clears, fifo'd swaps), and the golden at the
-- bottom is a REAL in-game demo frame drawn by the 1997 driver.  Five
-- model gaps stood between "detection passes" and this frame — the
-- siProcess oscillator, the initEnable revision gate, the SGRAM fill,
-- the TSU's 16-bit float→12.4 truncation (the +786432 snap bias), and
-- packet-5 byte addressing — every one found by THIS software, none by
-- the hand-written tests (docs/internals/core/peripherals/pci/cards/voodoo2.md
-- tells the story).

local mac = dofile("../lib/mac.lua")
dofile("glide.lua")

-- The raster backend under test (voodoo2_raster.h): the build's default
-- (the worker thread) unless --var RASTER= says otherwise; the
-- tnt-voodoo2-glide-sw row plays the same launch on the normative
-- synchronous walker; RASTER=null is the no-raster variant
-- scripts/voodoo2/bench.sh can time (not a floor — see the script).
local RASTER = gs.try(function() return RASTER end, "thread")

img = F"${vars.WORK_DIR}/quake.img" -- global: used by an included file
if mac.have_media(img) then
    glide_to_launch(RASTER)

    glide_play("quake-ingame.png", RASTER, F"tnt-voodoo2-glide/${RASTER}")
else
    mac.skip_test("tnt-voodoo2-glide (tests/data/apps/quake_8_1_voodoo2.img not fetched — gs-test-data extended media)")
end
quit()
