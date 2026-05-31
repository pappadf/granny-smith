# Integration test configuration: IIfx MacTest Boot (8-bit colour)
# Boots the Macintosh IIfx from the MacTest 1.0 floppy with the JMFB display
# configured for 13" RGB at 8 bpp, and verifies that MacTest's hardware-
# identification panel correctly probes the IIfx logic board (System type
# "Macintosh IIfx", 68030 CPU, 68882 FPU, 512 KB ROM, 16 MB RAM) in 256-colour
# mode.  Also a regression guard for the 24-bit master-pointer addressing fix
# in src/core/memory/memory.c (dispatch_device_at_logical) — see test.script.
# Complements tests/integration/iifx-boot (which boots IIfx to the System 7.0.1
# Finder) and mirrors tests/integration/iicx-mactest (the IIcx/IIci MacTest
# suite); the 8 bpp setup follows tests/integration/iicx-video-modes.
#
# The video depth is selected inside test.script (nubus.video_mode before the
# inline machine.boot), so TEST_ARGS only needs to wire up the ROM, RAM, and
# floppy for the harness.

TEST_NAME := IIfx MacTest Boot (8bpp)
TEST_DESC := Boots Macintosh IIfx from the MacTest floppy at 8-bit colour and verifies the hardware-identification panel

TEST_ROM := roms/4147DD77-IIfx.rom

# MacTest 1.0 1.44MB HD MFM floppy image.
# RAM is pinned at 16 MB to match the iifx-boot baseline: ram=8192 currently
# regresses the IIfx boot (stalls in POST at $40843F96 while probing the SCC
# IOP); ram=16384 boots cleanly.  MacTest reports "RAM size: 16 MB" at this
# setting, which the main.png baseline below depends on.
TEST_ARGS := model=iifx ram=16384 fd=$(TEST_DATA)/apps/MacTest-IIfx.image
