# Integration test configuration: SE/30 — GS generic vROM
# The SE/30 profile now DEFAULTS to the generic sibling kind ("se30",
# built-in GS declaration ROM): boot to Finder with no onboard-video
# vROM consulted (proposal-generic-nubus-vrom.md sec. 7.4 / stage 3).

TEST_NAME := SE/30 Video (GS generic vROM)
TEST_DESC := Boot the SE/30 to Finder on the default generic video kind.

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=se30 ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
