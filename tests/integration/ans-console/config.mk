# Integration test: the Cirrus 54M30 framebuffer as Open Firmware's console
# (proposal-apple-network-server-500-700 §5.8, Phase G)
#
# The Apple Network Server's on-board video is the repository's first REAL
# PCI framebuffer for an Old World Power Macintosh — a discrete SVGA part on
# Bandit 1, not a Control/Valkyrie framebuffer hung off the memory
# controller — and it is what the machine actually uses as its console: a
# Network Server ships with `output-device screen`, and Open Firmware's
# `(install-console)` falls back to `ttya` only when the screen fails to
# open.  So this row boots the machine as it shipped, with no `setenv`, and
# looks at what a monitor would show.
#
# THE MODE IS DERIVED, NOT CONFIGURED.  Nothing tells the emulator what
# resolution to present; Open Firmware programs the CRTC, the sequencer and
# the Cirrus extension registers, and the model reads a mode out of them —
# 640x480 at 8 bits per pixel, 640 bytes per scan line.  A wrong bit in that
# derivation is a wrong-shaped or garbled screen, which is exactly what a
# golden catches and nothing else does.
#
# 8 bpp is not a shortcut: Apple states the part "implements only a
# little-endian window into the packed-pixel frame buffer, hence Big Endian
# operating systems are limited to 8 bits per pixel", and at one byte per
# pixel byte order does not matter — so the existing PIXEL_8BPP path is
# correct rather than merely convenient.

TEST_NAME := ANS console
TEST_DESC := Boots the Network Server as it shipped and matches the Open Firmware console the 54M30 draws

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
