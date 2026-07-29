# RE-HOSTED IIfx -> IIci (§7's A/UX redistribution, MILESTONE class): this
# becomes RBV-under-A/UX, a video path nothing exercises. §7 picks the IIci
# over the IIsi because A/UX requires an FPU and the IIci has a standard
# 68882. Expected to surface real gaps, so it is run-not-fatal until it
# passes; promotion is a reviewed edit.
# Integration test configuration: IIci A/UX 3.0.1 HD Boot at 8 bpp (via RBV)
#
# 8-bpp sibling of iifx-aux3-boot (which runs the JMFB's default 1 bpp).
# Exists because 8-bit colour is the configuration that exposed two distinct
# IIfx bugs, previously pinned only by the legacy web-UI e2e
# (tests/e2e/specs/iifx-aux3-boot/iifx-aux3-login.spec.ts, retired with the
# legacy UI):
#   1. jmfb.c PRAM seeding — selecting a video mode makes the JMFB factory
#      seed slot-PRAM AND stamp the boot-ROM PRAM validity tokens; a
#      regression there (the token stamp suppressing the ROM's default
#      startup-device PRAM init) left D3=0 at SCSILoad → no boot driver →
#      Mac-OS no-boot floppy.  See notes/iifx-debug/117.
#   2. CPU instruction-fetch fault handling — 8bpp's larger framebuffer
#      raises memory pressure, so A/UX exec'ing /etc/init demand-pages
#      init's text page from disk; f_trap once routed that PMMU
#      instruction-fetch fault through the non-retry exception_bus_error,
#      whose same-PC double-fault→HALT heuristic falsely fired → HALT →
#      GLU reset → ROM POST hang.  Fixed via exception_bus_error_retry.
#
# The video mode is seeded the production way (machine.nubus.video_mode
# before machine.boot — exactly what the web2 New Machine dialog does), so
# no hd= in TEST_ARGS: the script re-boots with the seed and attaches the
# HD itself.  See test.script.

TEST_NAME := IIci A/UX 3.0.1 HD Boot at 8 bpp (reaches graphical login)
TEST_DESC := Boot IIci (16 MB, built-in RBV at 8 bpp) from the A/UX 3.0.1 HD image; expect the graphical login window, pixel-exact.

# IIci ROM (checksum 0x368CADFE).  Video is the machine's built-in RBV, not a
# NuBus card, so no declrom is discovered or needed — that is the point of the
# re-host.
TEST_ROM := roms/iici-368cadfe.rom

TEST_ARGS := model=iici ram=16384

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
