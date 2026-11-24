-- Converted from test.script by scripts/script-to-lua.py
-- IIcx substitute-ROM video suite (see config.mk).
--
-- Row map (former directory → row):
--
--   gsvrom-sweep        iicx-gsvrom-video-modes: the 10-cell JMFB sweep
--                       (13" RGB 1/2/4/8 bpp, 12" RGB, 15" portrait,
--                       21" two-page) on the generated vROM
--   gsvrom-24ac         iicx-gsvrom-24ac: the 24AC-kind generated vROM at
--                       640x480x8 and 832x624x8
--   gsvrom-824gc        iicx-gsvrom-824gc: the 8*24 GC bring-up ladder on
--                       a generated vROM (no pixel match — blocked on the
--                       QD32 upgrade pass)
--   gsvrom-custom-mode  iicx-gsvrom-custom-mode: an 800x600x8 custom
--                       geometry the generated vROM can declare
--
-- Row bodies are in rows.script, transcribed by script from the absorbed
-- tests so the sweep's sense/PRAM seeding stays byte-exact.

local mac = dofile("../lib/mac.lua")
dofile("rows.lua")

mac.row("gsvrom-sweep", row_gsvrom_sweep)
mac.row("gsvrom-24ac", row_gsvrom_24ac)
mac.row("gsvrom-824gc", row_gsvrom_824gc)
mac.row("gsvrom-custom-mode", row_gsvrom_custom_mode)

mac.suite_done()
quit()
