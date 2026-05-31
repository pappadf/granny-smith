# Integration test configuration: IIfx A/UX 3.0.1 HD Boot
# Mirrors se30-aux3-boot but exercises the IIfx-specific boot path
# (PMMU + FMC + OSS + SCC/SWIM IOPs) against the same A/UX 3 HD image.
#
# REGRESSION NOTE (2026-05-20): On the SE/30 the same HD image boots
# through "Welcome to A/UX. Loading..." into the A/UX kernel, lands at
# the A/UX login screen, and after `root + return + return` reaches the
# Mac-on-A/UX desktop (see se30-aux3-boot).  On the IIfx the boot stops
# at the Mac OS Finder of the MacPartition volume — the A/UX Startup
# application is never launched and `g_last_user_crp` stays zero.
# The user remembers this working on the (now-deleted) iifx branch,
# i.e. before the code-review fix-pass landed.  This test pins the
# current broken state so any future change that re-enables A/UX boot
# on IIfx — or any further regression — is caught.

TEST_NAME := IIfx A/UX 3.0.1 HD Boot (regression: stops at Mac Finder)
TEST_DESC := Boot IIfx with 16 MB RAM from the A/UX 3.0.1 HD image; expect a stable Mac OS Finder (the A/UX Startup never runs — currently regressed).

# IIfx ROM (checksum 0x4147DD77).  JMFB declrom (Apple-341-0868.vrom) is
# auto-discovered from the same directory as the ROM file; the content is
# identical to tests/data/roms/341-0868.vrom.
TEST_ROM := roms/4147DD77-IIfx.rom

# Copy the HD image into TEST_TMPDIR so its .delta/.journal are wiped with
# the tempdir on exit; the source image is left untouched between runs.
# Keep the TEST_SETUP line single-line — the Makefile extractor only honors
# $(TEST_DATA) and $(TEST_TMPDIR) substitutions.
TEST_SETUP := cp "$(TEST_DATA)/aux/aux_3.0.1/hd160-with-aux-301.img" "$(TEST_TMPDIR)/hd.img"

# 16 MB RAM matches the SE/30 A/UX test (A/UX itself wants 16 MB; even
# though the kernel isn't running here, keep the budget identical so
# any future A/UX startup runs against the same footprint).
TEST_ARGS := model=iifx ram=16384 hd=$(TEST_TMPDIR)/hd.img
