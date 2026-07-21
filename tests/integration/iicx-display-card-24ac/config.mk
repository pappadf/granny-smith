# Integration test configuration: IIcx + Apple Macintosh Display Card 24AC
#
# Three parts (see test.script):
#   Part A — the acceleration engine's register/aperture decode (fill,
#            solid fill, block copy, operand load/readback, STATUS/CONFIG),
#            driven directly through memory.poke/peek against hand-computed
#            expected VRAM.  No OS boot — deterministic and fast.
#   Part C — the engine-vs-fallback oracle (proposal §3.5): the engine's
#            pattern fill must equal the software (CPU) fill, toggled via the
#            object-model gate machine.nubus.slot[9].card.engine.enabled;
#            plus the object-model surface (slot[N].card.{framebuffer,
#            declrom,clut,mode,engine} + screen.source reference, §3.8).
#   Part B — a full Phase-1/2 boot to an 8-bpp COLOUR Finder desktop: the
#            vrom senses the 640x480 multisync monitor, the OS selects 8 bpp
#            (savedMode $82) and the desktop is matched pixel-exact.

TEST_NAME := IIcx Display Card 24AC
TEST_DESC := Display Card 24AC: engine decode + engine-vs-fallback oracle + object model + 8bpp colour desktop.

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# 8 MB RAM matches the iicx-video-modes budget so the boot-ROM slot scan /
# PrimaryInit timing transfers directly.  The display-card-24ac-d8daab87.vrom is
# found next to the ROM via the shared declrom loader's rom-dir search.
TEST_ARGS := model=iicx ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
