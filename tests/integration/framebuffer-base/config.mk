# Integration test: framebuffer.base has one meaning on every display source
#
# `framebuffer.base` is the guest physical address of the first displayed
# byte, 0 when the frame is not in guest-addressable memory -- on a NuBus
# card, a PCI card and a built-in chip alike (display_class.h).  Each source
# here is booted from its ROM with no disk until the ROM has set up the
# screen, and its base is pinned three ways: it equals ScrnBase (the address
# the guest itself draws through), the byte at it is the top-left pixel (a
# poke there changes exactly that byte's pixels and nothing next to them),
# and the next row starts a stride further on.  The sources: the Quadra
# 700's DAFB (machine.video), the SE/30's built-in video (a NuBus pseudo-card
# in slot $E) and the 7500's Control (machine.video and its PCI slot node).
# The Mach64 GX, a card whose base used to be a VRAM byte offset, is pinned
# the same way in tnt-pci-mach64.

TEST_NAME := Framebuffer base meaning
TEST_DESC := framebuffer.base is the guest physical address of the first displayed byte on DAFB, SE/30 video and Control

TEST_ROM := roms/q700-q900-420dbff3.rom

TEST_ARGS := model=q700

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
