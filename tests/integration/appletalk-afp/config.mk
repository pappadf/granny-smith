# Integration test configuration: AppleTalk AFP shared-volume mount
#
# The guest-level AFP flow, ported from the legacy web UI's e2e
# (tests/e2e/specs/appletalk/appletalk.spec.ts, retired with the legacy
# UI). object-toplevel covers the appletalk.shares object surface; this
# test proves the emulated AppleShare server end-to-end against the real
# guest client: LToUP discovery (NBP lookup shows the server in the
# Chooser), the ASP session, AFP volume enumeration, and the mount —
# driven entirely through the guest UI (Chooser) with injected mouse
# input, matched pixel-exact at each protocol-visible stage.

TEST_NAME := AppleTalk AFP Mount
TEST_DESC := Chooser -> AppleShare -> discover host share -> connect as Guest -> mount volume on the desktop.

TEST_ROM := roms/Plus_v3.rom

TEST_SETUP := unzip -o -q $(TEST_DATA)/systems/hd1.zip -d $(TEST_TMPDIR)

TEST_ARGS := hd=$(TEST_TMPDIR)/hd1.img
