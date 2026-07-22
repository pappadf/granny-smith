# Integration test configuration: IIcx Video Modes — GS generic vROM
# Clone of iicx-video-modes running on the GENERIC JMFB sibling kind
# ("8_24", built-in GS declaration ROM — no vROM file consulted), with
# mode selection through the staged `video_mode=` boot argument instead
# of hand-poked PRAM (the C side seeds the same sPRAMRec bytes).  See
# proposal-generic-nubus-vrom.md sec. 9 stage 1.

TEST_NAME := IIcx Video Modes (GS generic vROM)
TEST_DESC := Cold-boot Welcome-at-N-bpp via staged video_mode on the generic 8_24 kind.

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iicx ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
