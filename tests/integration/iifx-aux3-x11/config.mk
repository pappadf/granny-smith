# Integration test configuration: IIfx A/UX 3.0.1 — X11 session login
#
# A variant of iifx-aux3-boot.  Boots the IIfx to the same A/UX 3.0.1
# graphical `login:` window, but instead of logging straight in, it drives
# the CommandShell login chooser: Options -> "Change Session Type...",
# selects the "X11" session for user "root" via "This Session Only", then
# logs in.  A/UX starts a pure X11 session (x11start) instead of the A/UX
# Finder, so the captured desktop is the X11 root window with its xterm
# ("console" + two "localhost" login shells) rather than the Mac-style
# Finder desktop.
#
# This exercises the login window's Options menu, its modal
# "Change Session Type" dialog (text entry + list selection + the enabled
# session buttons), and the X11 startup path — none of which the plain
# iifx-aux3-boot login pin touches.  See test.script.

TEST_NAME := IIfx A/UX 3.0.1 X11 Session
TEST_DESC := Boot IIfx from the A/UX 3.0.1 HD image, choose an X11 session type at the login window, and log in as root to the X11 desktop.

# IIfx ROM (checksum 0x4147DD77).  JMFB declrom (mdc-8-24-revb-d1629664.vrom) is
# auto-discovered from the same directory as the ROM file.
TEST_ROM := roms/iifx-4147dd77.rom

# Copy the HD image into TEST_TMPDIR so its .delta/.journal are wiped with
# the tempdir on exit; the source image is left untouched between runs.
# Keep the TEST_SETUP line single-line — the Makefile extractor only honors
# $(TEST_DATA) and $(TEST_TMPDIR) substitutions.
TEST_SETUP := cp "$(TEST_DATA)/aux/aux_3.0.1/hd160-with-aux-301.img" "$(TEST_TMPDIR)/hd.img"

# 16 MB RAM matches iifx-aux3-boot (A/UX wants 16 MB).
TEST_ARGS := model=iifx ram=16384 hd=$(TEST_TMPDIR)/hd.img
