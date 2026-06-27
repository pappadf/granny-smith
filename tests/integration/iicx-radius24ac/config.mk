# Integration test configuration: IIcx + Radius "Apple Macintosh 24AC" card
#
# Two independent parts (see test.script):
#   Part A — the acceleration engine's register/aperture decode (fill,
#            solid fill, block copy, operand load/readback, STATUS/CONFIG),
#            driven directly through memory.poke/peek against hand-computed
#            expected VRAM.  No OS boot — deterministic and fast.
#   Part B — a Phase-1 boot smoke: the Slot Manager finds the card and its
#            vrom PrimaryInit configures it (CRTC/PLL/RAMDAC/depth) at
#            640×480.  (Full color-desktop screenshot matching awaits the
#            standard-video-driver GDevice/monitor-sense RE — see the
#            proposal §6 open questions and notes/.)

TEST_NAME := IIcx Radius 24AC
TEST_DESC := Radius 24AC card: acceleration-engine decode + Phase-1 boot/PrimaryInit smoke.

TEST_ROM := roms/IIcx.rom

# 8 MB RAM matches the iicx-video-modes budget so the boot-ROM slot scan /
# PrimaryInit timing transfers directly.  The display-card-24ac.vrom is
# found next to the ROM via the shared declrom loader's rom-dir search.
TEST_ARGS := model=iicx ram=8192 fd=$(TEST_DATA)/systems/System_7_0_1.image
