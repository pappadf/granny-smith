# Integration test configuration: IIcx custom resolution — GS generic vROM
# Boots the generic 8_24 (JMFB) kind with a custom_mode= resolution that
# fits the minor slot window (proposal-nubus-runtime-vrom §3.6 / stage E).
# The declaration ROM is generated at card_init with a video sResource at
# the requested geometry; the card boots its default 13" monitor on it.

TEST_NAME := IIcx Custom Resolution (GS generic vROM)
TEST_DESC := Cold-boot the generic 8_24 kind at a custom 800x600x8 resolution.

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iicx ram=8192
