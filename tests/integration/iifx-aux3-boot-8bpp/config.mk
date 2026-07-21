# Integration test configuration: IIfx A/UX 3.0.1 HD Boot at 8 bpp
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

TEST_NAME := IIfx A/UX 3.0.1 HD Boot at 8 bpp (reaches graphical login)
TEST_DESC := Boot IIfx (16 MB, JMFB at 13" RGB 8 bpp) from the A/UX 3.0.1 HD image; expect the graphical login window, pixel-exact.

# IIfx ROM (checksum 0x4147DD77).  JMFB declrom (mdc-8-24-revb-d1629664.vrom) is
# auto-discovered from the same directory as the ROM file.
TEST_ROM := roms/iifx-4147dd77.rom

TEST_ARGS := model=iifx ram=16384
