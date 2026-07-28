# Media: system_6_0_8_20mb_8_24gc.img, not hd1.zip. Verified 2026-07-27:
# hd1 is the same System 6.0.8 at the same 21,411,840-byte ST225N geometry,
# and the GC image's System Folder is a strict superset (AppleShare
# included), so every geometry/catalog assert holds unchanged — and this
# removes the suite's last TEST_SETUP unzip (§6.1).
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

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := hd=$(TEST_TMPDIR)/hd.img

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
